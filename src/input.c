/* Main PC: low-level hooks swallow local input while it is on the other PC and post it to the UI
 * thread, which sends it. The pointer is parked mid-screen under an invisible window with no
 * cursor, so it disappears and every hook position is a delta from the park point.
 * Controlled PC: SendInput replays what arrives, and the return edge hands input back.
 *
 * An edge gives only after a push (resistance), never in its corners, and not right after input
 * arrived through it (grace). The push is measured with Raw Input, because the hook reports a
 * pointer Windows has already stopped at the edge. Port of Beamer core/return_edge.py and
 * win_app/capture_win.py + sender.py (GPL-3, Toby George Read Kalkman). */

#include "link.h" /* winsock2.h before windows.h */
#include "input.h"
#include <stdio.h>

#define HT_SET (WM_APP + 1)    /* wParam: some PC connected, lParam: edges that lead to one */
#define HT_LEAVE (WM_APP + 2)  /* wParam: 0, or 1 + position along the edge (0..65535); lParam: edge, -1: where it left */
#define HT_CONFIG (WM_APP + 3) /* lParam: malloc'd InputConfig, freed here */
#define HT_RECORD (WM_APP + 4) /* wParam: 1 a key, 2 a chord */
#define HT_SHOW (WM_APP + 5)   /* the local mouse moved while the pointer was hidden */
#define HT_CROSS (WM_APP + 6)  /* wParam: 1 + the PC, 0 the one used last: as the switch key, from the tray */
#define TAP_MS 400
#define INJECTED_MARK 0x5355584E /* "NXUS" in SendInput's dwExtraInfo: Raw Input's only way to know our own moves */

/* ---- the screens' edges: pure, for both roles and tests/edge_test.c ---- */

#define CORNER_PX 8                      /* a push this near an end of a wall does not count */
#define LANDING_CLEAR_PX (CORNER_PX + 2) /* an arrival lands at least this far from the ends */
#define GRACE_PX 4                       /* after arriving: this much travel back in, or */
#define GRACE_MS 300                     /* this long, before the edge counts pushes again */
#define DECAY_PX_PER_S (DEFAULT_RESISTANCE * 1000 / 400) /* the push fades in 0.4 s when the hand stops */

static const int STEP_X[] = {-1, 1, 0, 0}, STEP_Y[] = {0, 0, -1, 1}; /* a pixel through each edge */

typedef struct { RECT r[16]; int n; DWORD at; } Screens;

static BOOL on_screens(const Screens *s, int x, int y)
{
    for (int i = 0; i < s->n; i++)
        if (x >= s->r[i].left && x < s->r[i].right && y >= s->r[i].top && y < s->r[i].bottom) return TRUE;
    return FALSE;
}

/* On a screen, with no screen one pixel past edge e: the pointer stops here. Not the whole desktop's
 * rectangle: beside a 4K screen with a 1080p one to its right, tops aligned, the 4K's right edge below
 * the 1080p one is a wall too. */
static BOOL wall_at(const Screens *s, POINT p, int e)
{
    return on_screens(s, p.x, p.y) && !on_screens(s, p.x + STEP_X[e], p.y + STEP_Y[e]);
}

/* Near an end of a wall: a window's Close button, a taskbar's Start button. */
static BOOL in_corner(const Screens *s, POINT p, int e)
{
    BOOL vertical = e == EDGE_LEFT || e == EDGE_RIGHT;
    for (int d = -1; d <= 1; d += 2) {
        POINT q = {p.x + (vertical ? 0 : d * CORNER_PX), p.y + (vertical ? d * CORNER_PX : 0)};
        if (!wall_at(s, q, e)) return TRUE;
    }
    return FALSE;
}

static BOOL covers(const RECT *q, BOOL vertical, int line, int t)
{
    return vertical ? q->left <= line && line < q->right && q->top <= t && t < q->bottom
                    : q->top <= line && line < q->bottom && q->left <= t && t < q->right;
}

/* The stretch [lo, hi) of screen i's side e with no screen past it; lo >= hi when there is none. */
static void exposed(const Screens *s, int i, int e, int *lo, int *hi)
{
    const RECT *r = &s->r[i];
    BOOL vertical = e == EDGE_LEFT || e == EDGE_RIGHT;
    int line = e == EDGE_LEFT ? r->left - 1 : e == EDGE_RIGHT ? r->right : e == EDGE_TOP ? r->top - 1 : r->bottom;
    *lo = vertical ? r->top : r->left, *hi = vertical ? r->bottom : r->right;
    for (BOOL moved = TRUE; moved && *lo < *hi;) {
        moved = FALSE;
        for (int j = 0; j < s->n; j++)
            if (covers(&s->r[j], vertical, line, *lo)) *lo = vertical ? s->r[j].bottom : s->r[j].right, moved = TRUE;
    }
    for (BOOL moved = TRUE; moved && *lo < *hi;) {
        moved = FALSE;
        for (int j = 0; j < s->n; j++)
            if (covers(&s->r[j], vertical, line, *hi - 1)) *hi = vertical ? s->r[j].top : s->r[j].left, moved = TRUE;
    }
}

static RECT bounds(const Screens *s)
{
    RECT v = {0, 0, 1, 1};
    for (int i = 0; i < s->n; i++) {
        if (!i) v = s->r[0];
        UnionRect(&v, &v, &s->r[i]);
    }
    return v;
}

/* Whether screen i reaches the desktop's outer edge on side e. */
static BOOL outer(const Screens *s, int i, int e)
{
    RECT v = bounds(s);
    const RECT *r = &s->r[i];
    return e == EDGE_LEFT ? r->left == v.left : e == EDGE_RIGHT ? r->right == v.right : e == EDGE_TOP ? r->top == v.top
                                                                                                    : r->bottom == v.bottom;
}

/* The stretch lo..hi of side e that the outermost screens cover, so screens of different sizes line up
 * proportionally: with a 4K and a 1080p screen side by side, 60% of the way down the 1080p one is 60% of
 * that screen, not of the desktop's full height. Arrivals land here.
 * ponytail: screens stacked along an edge with a gap between them map across the gap too */
static void span_of(const Screens *s, int e, int *lo, int *hi)
{
    BOOL vertical = e == EDGE_LEFT || e == EDGE_RIGHT;
    RECT v = bounds(s);
    *lo = vertical ? v.bottom : v.right, *hi = vertical ? v.top : v.left;
    for (int i = 0; i < s->n; i++) {
        if (!outer(s, i, e)) continue;
        *lo = min(*lo, vertical ? s->r[i].top : s->r[i].left);
        *hi = max(*hi, vertical ? s->r[i].bottom : s->r[i].right);
    }
    if (*lo >= *hi) *lo = vertical ? v.top : v.left, *hi = vertical ? v.bottom : v.right;
}

/* Position along side e, 0..65535, of a point leaving through it: along the outermost screens, as
 * arrivals count it, or, leaving through a wall further in (the 4K's right edge below a 1080p screen),
 * along that screen's own stretch of wall. */
static int along(const Screens *s, POINT p, int e)
{
    int lo, hi;
    span_of(s, e, &lo, &hi);
    for (int i = 0; i < s->n; i++) {
        const RECT *r = &s->r[i];
        if (p.x < r->left || p.x >= r->right || p.y < r->top || p.y >= r->bottom || outer(s, i, e)) continue;
        int a, b;
        exposed(s, i, e, &a, &b);
        if (a < b) lo = a, hi = b;
    }
    int at = e == EDGE_LEFT || e == EDGE_RIGHT ? p.y : p.x;
    return max(0, min(65535, MulDiv(at - lo, 65535, hi - lo - 1)));
}

/* A point just inside the outermost wall on side e, frac along it, clear of the corners. */
static POINT on_edge(const Screens *s, int e, int frac)
{
    BOOL vertical = e == EDGE_LEFT || e == EDGE_RIGHT;
    int lo, hi, best = -1, best_d = 0, a = 0, b = 0;
    span_of(s, e, &lo, &hi);
    int at = lo + MulDiv(frac, hi - lo - 1, 65535);
    for (int i = 0; i < s->n; i++) { /* the outermost screen whose wall holds that point, else the nearest */
        int ea, eb;
        exposed(s, i, e, &ea, &eb);
        if (ea >= eb || !outer(s, i, e)) continue;
        int d = at < ea ? ea - at : at >= eb ? at - eb + 1 : 0;
        if (best < 0 || d < best_d) best = i, best_d = d, a = ea, b = eb;
    }
    POINT p;
    if (best < 0) { /* no screens known */
        RECT v = bounds(s);
        p.x = (v.left + v.right) / 2, p.y = (v.top + v.bottom) / 2;
        return p;
    }
    const RECT *r = &s->r[best];
    if (b - a > 2 * LANDING_CLEAR_PX) a += LANDING_CLEAR_PX, b -= LANDING_CLEAR_PX;
    at = max(a, min(b - 1, at));
    switch (e) {
    case EDGE_LEFT: p.x = r->left + 2, p.y = at; break;
    case EDGE_RIGHT: p.x = r->right - 3, p.y = at; break;
    case EDGE_TOP: p.x = at, p.y = r->top + 2; break;
    default: p.x = at, p.y = r->bottom - 3;
    }
    (void)vertical;
    return p;
}

/* The pressure against one edge: outward travel at the wall builds it, travel back in and time
 * take it away, and at `resistance` px it gives. */
enum { PRESS_PASS, PRESS_HOLD, PRESS_CROSS };
typedef struct { int travel, inward; DWORD last, arrival; BOOL arriving; } Press;

static int outward(int e, int dx, int dy) { return e == EDGE_LEFT ? -dx : e == EDGE_RIGHT ? dx : e == EDGE_TOP ? -dy : dy; }

static void press_arrive(Press *p, DWORD now) { p->arriving = TRUE, p->arrival = now, p->inward = 0, p->travel = 0; }

static int press_feed(Press *p, BOOL at_wall, int out, DWORD now, int resistance)
{
    DWORD elapsed = min(now - p->last, 10000);
    p->last = now;
    /* A brisk push outruns the fade; a slow lean on the edge to reach a menu never adds up. */
    p->travel = max(0, p->travel - (int)(elapsed * DECAY_PX_PER_S / 1000));
    if (p->arriving && out < 0) p->inward -= out;
    BOOL grace = p->arriving && p->inward < GRACE_PX && now - p->arrival < GRACE_MS;
    if (!grace) p->arriving = FALSE;
    if (out <= 0 || !at_wall) {
        if (out < 0) p->travel = max(0, p->travel + out);
        return PRESS_PASS;
    }
    if (grace) return PRESS_HOLD;
    p->travel += out;
    if (p->travel < resistance) return PRESS_HOLD;
    p->travel = 0;
    return PRESS_CROSS;
}

static BOOL CALLBACK add_screen(HMONITOR mon, HDC dc, LPRECT r, LPARAM s)
{
    (void)mon, (void)dc;
    Screens *all = (Screens *)s;
    if (all->n < 16) all->r[all->n++] = *r;
    return TRUE;
}

/* The screens, read again at most once a second (each caller keeps its own, on its own thread). */
static const Screens *screens(Screens *s)
{
    if (s->n && GetTickCount() - s->at < 1000) return s;
    s->n = 0;
    EnumDisplayMonitors(NULL, NULL, add_screen, (LPARAM)s);
    s->at = GetTickCount();
    return s;
}

/* Per-monitor DPI awareness for the calling thread (Windows 10 1607+), so screens, the pointer and
 * SendInput agree in real pixels when the monitors are scaled differently. Older Windows has one
 * DPI for all monitors, and nothing to do. */
typedef HANDLE(WINAPI *SetDpiContext)(HANDLE);
static SetDpiContext set_dpi_context;

static HANDLE per_monitor_dpi(void)
{
    static BOOL looked;
    if (!looked) {
        set_dpi_context = (SetDpiContext)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                                "SetThreadDpiAwarenessContext");
        looked = TRUE;
    }
    if (!set_dpi_context) return NULL;
    HANDLE old = set_dpi_context((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */);
    return old ? old : set_dpi_context((HANDLE)-3 /* PER_MONITOR_AWARE, before 1703 */);
}

static void restore_dpi(HANDLE old)
{
    if (old && set_dpi_context) set_dpi_context(old);
}

/* ---- the pointer of a PC whose input is elsewhere (port of Beamer win_app/desktop_win.py) ---- */

static volatile LONG pointer_hidden;

void input_restore_pointer(void) { SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0); }

void input_hide_pointer(BOOL hide)
{
    static const DWORD ids[] = {32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644, 32645, 32646, 32648, 32649,
                                32650, 32651}; /* every OCR_* a pointer can show as */
    if (!hide) {
        if (InterlockedExchange(&pointer_hidden, 0)) input_restore_pointer(); /* the user's own scheme and size */
        return;
    }
    if (InterlockedExchange(&pointer_hidden, 1)) return;
    BYTE and_mask[128], xor_mask[128];
    memset(and_mask, 0xFF, sizeof and_mask);
    memset(xor_mask, 0, sizeof xor_mask);
    for (int i = 0; i < (int)(sizeof ids / sizeof ids[0]); i++) {
        /* SetSystemCursor takes the cursor over and destroys it, so each one gets its own. */
        HCURSOR blank = CreateCursor(GetModuleHandleW(NULL), 0, 0, 32, 32, and_mask, xor_mask);
        if (blank && !SetSystemCursor(blank, ids[i])) DestroyCursor(blank);
    }
}

BOOL input_desktop_unlocked(void)
{
    /* Not even allowed to open it is an answer too: the secure desktop has input. */
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    wchar_t name[64] = L"";
    DWORD n = 0;
    if (!d) return FALSE;
    BOOL normal = GetUserObjectInformationW(d, UOI_NAME, name, sizeof name, &n) && !lstrcmpiW(name, L"Default");
    CloseDesktop(d);
    return normal;
}

/* ---- main PC ---- */

static struct {
    HWND ui, hider;
    DWORD thread;
    BOOL connected, remote, tap_clean, raw_ok, abs_pointer, holding, switch_eaten;
    int edges, buttons, recording; /* edges: bit per edge of this screen that leads to a connected PC */
    InputConfig cfg;
    BYTE down[256], passdown[256];
    POINT park, back;
    DWORD last_tap;
    Press press[4];
    Screens screens;
    DWORD mouse_events, key_events, raw_events, last_vk, last_flags; /* for the test mode's "hooks" */
} m;

static void post(UINT type, DWORD packed) { PostMessageW(m.ui, WM_INPUT_SEND, type, (LPARAM)packed); }

/* The UI thread picks the PC: the one past that edge, pc when a jump key named it, or for the
 * shortcut (keep) the one used last. */
static void enter(POINT from, BOOL keep, int edge, int pc)
{
    m.remote = TRUE;
    m.back = from;
    m.tap_clean = FALSE;
    post(MSG_ENTER, edge | (DWORD)keep << 4 | (DWORD)(keep ? 0 : along(screens(&m.screens), from, edge)) << 8 |
                        (DWORD)(pc + 1) << 24);
    MONITORINFO mi = {0};
    mi.cbSize = sizeof mi;
    POINT origin = {0, 0};
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    m.park.x = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
    m.park.y = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
    SetWindowPos(m.hider, HWND_TOPMOST, m.park.x - 32, m.park.y - 32, 64, 64, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetCursorPos(m.park.x, m.park.y);
    PostMessageW(m.ui, WM_INPUT_STATE, TRUE, 0);
}

static void leave_local(POINT to)
{
    m.remote = m.holding = FALSE;
    ShowWindow(m.hider, SW_HIDE);
    SetCursorPos(to.x, to.y);
    for (int e = 0; e < 4; e++) press_arrive(&m.press[e], GetTickCount());
    PostMessageW(m.ui, WM_INPUT_STATE, FALSE, 0);
}

/* A release the hook never saw (let go over the secure desktop or an elevated window) would hold
 * every edge shut for good, so at an edge Windows is asked again. Port of Beamer sender.py. */
static void recheck_buttons(void)
{
    static const int vk[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (int i = 0; i < 5; i++)
        if (GetAsyncKeyState(vk[i]) < 0) return;
    m.buttons = 0;
}

/* A full-screen app (a game) has focus: Windows says so on Vista+, and on XP its window covers its
 * monitor exactly. A maximised window stops at the taskbar and does not count. Port of Beamer
 * win_app/desktop_win.py. */
static BOOL fullscreen_app(void)
{
    typedef HRESULT(WINAPI * Quns)(int *);
    static Quns quns;
    static BOOL looked;
    HWND w = GetForegroundWindow();
    wchar_t cls[64] = L"";
    DWORD pid = 0;
    if (!w) return FALSE;
    GetWindowThreadProcessId(w, &pid);
    GetClassNameW(w, cls, 64);
    if (pid == GetCurrentProcessId() || !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") ||
        !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd"))
        return FALSE;
    if (!looked) quns = (Quns)(void *)GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHQueryUserNotificationState"),
                 looked = TRUE;
    int state = 0;
    if (quns && quns(&state) == S_OK && (state == 3 || state == 4)) return TRUE; /* D3D full screen, presentation */
    RECT r;
    MONITORINFO mi = {0};
    mi.cbSize = sizeof mi;
    if (!GetWindowRect(w, &r) || !GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return FALSE;
    return EqualRect(&r, &mi.rcMonitor) && (!quns || state == 2 /* QUNS_BUSY */);
}

/* Whether the edges may give now: not paused, and not held for a full-screen app. */
static BOOL edges_open(void) { return !m.cfg.paused && !(m.cfg.fullscreen_hold && fullscreen_app()); }

/* The hand moved the mouse by dx, dy (Raw Input, before Windows stops the pointer at the edge). */
static void push(int dx, int dy)
{
    if (m.remote || !m.connected || !m.edges) return;
    POINT p;
    GetCursorPos(&p);
    const Screens *s = screens(&m.screens);
    DWORD now = GetTickCount();
    for (int e = 0; e < 4; e++) {
        if (!(m.edges & 1 << e)) continue;
        int out = outward(e, dx, dy);
        BOOL wall = wall_at(s, p, e) && !in_corner(s, p, e);
        if (wall && out > 0 && m.buttons) recheck_buttons();
        if (m.buttons) { /* a drag that reaches the edge is a drag, not a crossing */
            m.press[e].travel = 0;
            continue;
        }
        if (wall && out > 0 && !edges_open()) continue;
        if (press_feed(&m.press[e], wall, out, now, m.cfg.resistance) == PRESS_CROSS) {
            enter(p, FALSE, e, -1);
            return;
        }
    }
}

/* Without Raw Input's counts (a tablet, a remote desktop, a virtual machine's absolute pointer) there
 * is no push to measure: the edge gives at a touch, as before 0.5. */
static void touch(POINT p)
{
    const Screens *s = screens(&m.screens);
    for (int e = 0; e < 4; e++) {
        if (!(m.edges & 1 << e) || !wall_at(s, p, e) || in_corner(s, p, e)) continue;
        if (m.buttons) recheck_buttons();
        if (m.buttons || (m.press[e].arriving && GetTickCount() - m.press[e].arrival < GRACE_MS) || !edges_open())
            return;
        enter(p, FALSE, e, -1);
        return;
    }
}

static LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp)
{
    MSLLHOOKSTRUCT *e = (MSLLHOOKSTRUCT *)lp;
    m.mouse_events++;
    if (code < 0 || (e->flags & LLMHF_INJECTED)) return CallNextHookEx(NULL, code, wp, lp);
    if (pointer_hidden && wp == WM_MOUSEMOVE) PostThreadMessageW(m.thread, HT_SHOW, 0, 0); /* this PC's own mouse */
    int btn = 0, down = 0;
    switch (wp) {
    case WM_LBUTTONDOWN: down = 1; /* fall through */
    case WM_LBUTTONUP: btn = 1; break;
    case WM_RBUTTONDOWN: down = 1; /* fall through */
    case WM_RBUTTONUP: btn = 2; break;
    case WM_MBUTTONDOWN: down = 1; /* fall through */
    case WM_MBUTTONUP: btn = 3; break;
    case WM_XBUTTONDOWN: down = 1; /* fall through */
    case WM_XBUTTONUP: btn = HIWORD(e->mouseData) == XBUTTON1 ? 4 : 5; break;
    }
    if (!m.remote) {
        /* Never cross mid-drag: the button would stay down here and arrive up-only there. */
        if (btn) m.buttons = down ? m.buttons | 1 << btn : m.buttons & ~(1 << btn);
        else if (wp == WM_MOUSEMOVE && m.connected && (!m.raw_ok || m.abs_pointer || !m.raw_events)) {
            touch(e->pt);
            if (m.remote) return 1;
        }
        return CallNextHookEx(NULL, code, wp, lp);
    }
    if (wp == WM_MOUSEMOVE) {
        int dx = e->pt.x - m.park.x, dy = e->pt.y - m.park.y;
        if (dx || dy) post(MSG_MOVE, (DWORD)(dx & 0xFFFF) | (DWORD)(dy & 0xFFFF) << 16);
    } else if (btn) {
        post(MSG_BUTTON, btn | down << 8);
    } else if (wp == WM_MOUSEWHEEL || wp == 0x020E /* WM_MOUSEHWHEEL, Vista+ */) {
        post(MSG_WHEEL, HIWORD(e->mouseData) | (DWORD)(wp == 0x020E) << 16);
    }
    return 1;
}

static BOOL is_modifier(BYTE vk)
{
    return (vk >= VK_SHIFT && vk <= VK_MENU) || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN;
}

static int chord_mods(void)
{
    return (m.down[VK_LCONTROL] || m.down[VK_RCONTROL] ? CHORD_CTRL : 0) | (m.down[VK_LMENU] || m.down[VK_RMENU] ? CHORD_ALT : 0) |
           (m.down[VK_LSHIFT] || m.down[VK_RSHIFT] ? CHORD_SHIFT : 0) | (m.down[VK_LWIN] || m.down[VK_RWIN] ? CHORD_WIN : 0);
}

static void go_home(void)
{
    post(MSG_LEAVE, 0);
    leave_local(m.back);
}

/* The switch key and the jump keys, pressed now (not a repeat). TRUE: used up here. */
static BOOL shortcut(BYTE vk)
{
    if (vk == m.cfg.switch_vk && m.connected) {
        if (m.cfg.hold) { /* input is across while the key is held */
            m.switch_eaten = TRUE;
            if (m.remote) go_home();
            else if (!m.buttons) {
                POINT p;
                GetCursorPos(&p);
                enter(p, TRUE, 0, -1);
                m.holding = TRUE;
            }
            return TRUE;
        }
        /* Double-tap, with no other key between the taps. */
        if (m.tap_clean && GetTickCount() - m.last_tap < TAP_MS) {
            m.tap_clean = FALSE;
            if (m.remote) go_home();
            else if (!m.buttons) {
                POINT p;
                GetCursorPos(&p);
                enter(p, TRUE, 0, -1);
            }
            return TRUE;
        }
        m.tap_clean = TRUE;
        m.last_tap = GetTickCount();
        return FALSE;
    }
    m.tap_clean = FALSE;
    int mods = chord_mods();
    for (int i = 0; i < 4 && m.connected; i++) {
        if (!m.cfg.jump[i] || LOBYTE(m.cfg.jump[i]) != vk || HIBYTE(m.cfg.jump[i]) != mods) continue;
        if (m.remote) post(MSG_ENTER, 1 << 4 | (DWORD)(i + 1) << 24); /* from one PC straight to another */
        else if (!m.buttons) {
            POINT p;
            GetCursorPos(&p);
            enter(p, TRUE, 0, i);
        }
        return TRUE;
    }
    return FALSE;
}

static LRESULT CALLBACK key_hook(int code, WPARAM wp, LPARAM lp)
{
    KBDLLHOOKSTRUCT *e = (KBDLLHOOKSTRUCT *)lp;
    m.key_events++, m.last_vk = e->vkCode, m.last_flags = e->flags;
    if (code < 0 || (e->flags & LLKHF_INJECTED)) return CallNextHookEx(NULL, code, wp, lp);
    BYTE vk = (BYTE)e->vkCode;
    BOOL up = (e->flags & LLKHF_UP) != 0, repeat = !up && m.down[vk];
    m.down[vk] = !up;

    if (m.recording && !up && !repeat) { /* the Keyboard page asked for the next key */
        int chord = m.recording == 2, mods = chord_mods();
        if (vk == VK_ESCAPE) {
            m.recording = 0;
            PostMessageW(m.ui, WM_INPUT_KEY, 0, 0);
        } else if (!chord && vk != VK_RETURN && vk != VK_SPACE) { /* those would switch while typing */
            m.recording = 0;
            PostMessageW(m.ui, WM_INPUT_KEY, vk, 0);
        } else if (chord && !is_modifier(vk) && mods & (CHORD_CTRL | CHORD_ALT | CHORD_WIN)) {
            m.recording = 0;
            PostMessageW(m.ui, WM_INPUT_KEY, mods << 8 | vk, 0);
        } else if (chord && is_modifier(vk)) {
            return CallNextHookEx(NULL, code, wp, lp);
        }
        return 1;
    }
    if (vk == m.cfg.switch_vk && m.cfg.hold && (repeat || up) && m.switch_eaten) {
        if (up) {
            m.switch_eaten = FALSE;
            if (m.holding && m.remote) go_home();
            m.holding = FALSE;
        }
        return 1;
    }
    if (!up && !repeat) {
        if (shortcut(vk)) return 1;
        /* Ctrl+Alt+Pause always brings input home (Ctrl turns Pause into Cancel). */
        BOOL ctrl = m.down[VK_LCONTROL] || m.down[VK_RCONTROL], alt = m.down[VK_LMENU] || m.down[VK_RMENU];
        if (m.remote && ctrl && alt && (vk == VK_PAUSE || vk == VK_CANCEL)) {
            go_home();
            return 1;
        }
    }
    if (!m.remote) {
        m.passdown[vk] = !up;
        return CallNextHookEx(NULL, code, wp, lp);
    }
    /* A key held here before crossing is released here too, or this PC would think it stuck. */
    if (up && m.passdown[vk]) {
        m.passdown[vk] = 0;
        return CallNextHookEx(NULL, code, wp, lp);
    }
    post(MSG_KEY, vk | (e->scanCode & 0xFF) << 8 | (DWORD)(up | ((e->flags & LLKHF_EXTENDED) ? 2 : 0)) << 16);
    return 1;
}

static void raw_input(HRAWINPUT h)
{
    RAWINPUT ri;
    UINT size = sizeof ri;
    if (GetRawInputData(h, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1 || ri.header.dwType != RIM_TYPEMOUSE)
        return;
    m.raw_events++;
    if (ri.data.mouse.ulExtraInformation == INJECTED_MARK) return; /* Nexus's own SendInput */
    m.abs_pointer = (ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
    if (!m.abs_pointer && (ri.data.mouse.lLastX || ri.data.mouse.lLastY)) push(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
}

static LRESULT CALLBACK hider_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INPUT) raw_input((HRAWINPUT)lp);
    else if (msg == WM_DISPLAYCHANGE) m.screens.n = 0;
    return DefWindowProcW(w, msg, wp, lp);
}

static DWORD WINAPI hook_thread(void *ready)
{
    /* Windows silently drops a hook that keeps it waiting past LowLevelHooksTimeout. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    per_monitor_dpi(); /* before the window: it takes the thread's awareness */
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = hider_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"NexusHider"; /* no class cursor: the pointer vanishes over it */
    RegisterClassW(&wc);
    m.hider = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, wc.lpszClassName,
                              L"", WS_POPUP, 0, 0, 64, 64, NULL, NULL, inst, NULL);
    SetLayeredWindowAttributes(m.hider, 0, 1, LWA_ALPHA); /* alpha 1: invisible, still under the pointer */
    RAWINPUTDEVICE mouse_dev = {0x01, 0x02, RIDEV_INPUTSINK, m.hider}; /* generic desktop, mouse */
    m.raw_ok = RegisterRawInputDevices(&mouse_dev, 1, sizeof mouse_dev);
    SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, inst, 0);
    SetWindowsHookExW(WH_KEYBOARD_LL, key_hook, inst, 0);

    MSG msg;
    PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE); /* create the queue before saying ready */
    SetEvent((HANDLE)ready);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == HT_SET) {
            m.connected = (BOOL)msg.wParam;
            m.edges = m.connected ? (int)msg.lParam : 0;
            if (!m.connected && m.remote) leave_local(m.back);
        } else if (msg.message == HT_LEAVE) {
            int edge = (int)msg.lParam;
            if (m.remote) leave_local(msg.wParam && edge >= 0 ? on_edge(screens(&m.screens), edge, (int)msg.wParam - 1) : m.back);
        } else if (msg.message == HT_CONFIG) {
            m.cfg = *(InputConfig *)msg.lParam;
            free((void *)msg.lParam);
            m.tap_clean = m.holding = m.switch_eaten = FALSE;
        } else if (msg.message == HT_RECORD) {
            m.recording = (int)msg.wParam;
        } else if (msg.message == HT_CROSS) {
            POINT p;
            GetCursorPos(&p);
            if (m.remote) go_home();
            else if (m.connected && !m.buttons) enter(p, TRUE, 0, (int)msg.wParam - 1);
        } else if (msg.message == HT_SHOW) {
            input_hide_pointer(FALSE);
        } else {
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

void input_main_start(HWND ui)
{
    if (m.thread) return;
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    m.ui = ui;
    m.cfg.resistance = DEFAULT_RESISTANCE, m.cfg.switch_vk = VK_RCONTROL;
    CloseHandle(CreateThread(NULL, 0, hook_thread, ready, 0, &m.thread));
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
}

void input_main_set(BOOL connected, int edges)
{
    if (m.thread) PostThreadMessageW(m.thread, HT_SET, connected, edges);
}

void input_main_config(const InputConfig *cfg)
{
    InputConfig *copy = malloc(sizeof *copy);
    if (!m.thread || !copy) return free(copy);
    *copy = *cfg;
    if (!PostThreadMessageW(m.thread, HT_CONFIG, 0, (LPARAM)copy)) free(copy);
}

void input_main_record(int what)
{
    if (m.thread) PostThreadMessageW(m.thread, HT_RECORD, what, 0);
}

void input_main_cross(int pc)
{
    if (m.thread) PostThreadMessageW(m.thread, HT_CROSS, pc + 1, 0);
}

void input_main_leave(int edge, int frac)
{
    if (m.thread) PostThreadMessageW(m.thread, HT_LEAVE, frac < 0 ? 0 : frac + 1, edge);
}

void input_debug(char *out, int cap)
{
    _snprintf(out, cap, "hooks=%d mouse=%lu keys=%lu raw=%lu raw_ok=%d abs=%d last_vk=%02lx flags=%02lx remote=%d "
              "connected=%d resistance=%d pointer_hidden=%ld",
              m.thread != 0, m.mouse_events, m.key_events, m.raw_events, m.raw_ok, m.abs_pointer, m.last_vk,
              m.last_flags, m.remote, m.connected, m.cfg.resistance, pointer_hidden);
}

int input_pack(UINT type, LPARAM packed, unsigned char *o)
{
    DWORD p = (DWORD)packed;
    o[0] = (unsigned char)type;
    switch (type) {
    case MSG_ENTER: o[1] = p & 0xF, o[2] = (p >> 4) & 1, o[3] = (BYTE)(p >> 8), o[4] = (BYTE)(p >> 16); return 5;
    case MSG_LEAVE: o[1] = 0, o[2] = 0, o[3] = 0; return 4;
    case MSG_MOVE: o[1] = (BYTE)p, o[2] = (BYTE)(p >> 8), o[3] = (BYTE)(p >> 16), o[4] = (BYTE)(p >> 24); return 5;
    case MSG_BUTTON: o[1] = (BYTE)p, o[2] = (BYTE)(p >> 8); return 3;
    case MSG_WHEEL: o[1] = (BYTE)p, o[2] = (BYTE)(p >> 8), o[3] = (BYTE)(p >> 16); return 4;
    case MSG_KEY: o[1] = (BYTE)p, o[2] = (BYTE)(p >> 8), o[3] = (BYTE)(p >> 16); return 4;
    }
    return 0;
}

/* ---- controlled PC ---- */

static struct {
    BOOL active, tell_edge; /* tell_edge: the main PC names the edge each LEAVE went through */
    int exits, buttons;     /* exits: bit per edge of this screen that leads to another PC */
    int resistance;         /* the main PC's, from ENTER (0.5+); 0 from an older one: a touch */
    POINT pos; /* tracked here: GetCursorPos lags behind SendInput */
    BYTE keys[256], scan[256], ext[256];
    Press press[4];
    Screens screens;
} c;

static void mouse(DWORD flags, DWORD data)
{
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    in.mi.dwExtraInfo = INJECTED_MARK;
    SendInput(1, &in, sizeof in);
}

static void key(BYTE vk, BYTE scan, BOOL ext, BOOL up)
{
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk; /* by virtual key, so Pause, NumLock and media keys arrive as themselves */
    in.ki.wScan = scan;
    in.ki.dwFlags = (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
    in.ki.dwExtraInfo = INJECTED_MARK;
    SendInput(1, &in, sizeof in);
}

static const DWORD BUTTON_DOWN[] = {0, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN,
                                    MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN};
static const DWORD BUTTON_UP[] = {0, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP,
                                  MOUSEEVENTF_XUP, MOUSEEVENTF_XUP};

static void button(int b, BOOL down)
{
    mouse(down ? BUTTON_DOWN[b] : BUTTON_UP[b], b == 4 ? XBUTTON1 : b == 5 ? XBUTTON2 : 0);
    c.buttons = down ? c.buttons | 1 << b : c.buttons & ~(1 << b);
}

void input_release_all(void)
{
    for (int vk = 1; vk < 256; vk++)
        if (c.keys[vk]) key((BYTE)vk, c.scan[vk], c.ext[vk], TRUE), c.keys[vk] = 0;
    for (int b = 1; b <= 5; b++)
        if (c.buttons & 1 << b) button(b, FALSE);
}

BOOL input_controlled_active(void) { return c.active; }

void input_controlled_stop(void)
{
    input_release_all();
    c.active = FALSE;
}

static int replay(const unsigned char *msg, int len, unsigned char leave[8])
{
    const Screens *s = screens(&c.screens);
    RECT v = bounds(s);
    if (msg[0] == MSG_ENTER && len >= 5 && len <= 7 && msg[1] <= EDGE_BOTTOM) {
        int in = msg[1] ^ 1; /* moving right, the pointer comes in at this PC's left, and so on */
        /* A main PC before 0.3 knows one PC only: the way in is the way back. */
        c.exits = len >= 6 ? msg[5] & 15 : 1 << in;
        c.tell_edge = len >= 6;
        c.resistance = len == 7 ? msg[6] * 4 : 0;
        c.screens.n = 0, s = screens(&c.screens);
        if (msg[2]) {
            GetCursorPos(&c.pos);
        } else {
            c.pos = on_edge(s, in, msg[3] | msg[4] << 8);
            SetCursorPos(c.pos.x, c.pos.y);
        }
        for (int e = 0; e < 4; e++) press_arrive(&c.press[e], GetTickCount());
        c.active = TRUE;
        return 0;
    }
    if (msg[0] == MSG_LEAVE && len == 4) {
        input_controlled_stop();
        return 0;
    }
    if (!c.active) return 0;

    if (msg[0] == MSG_MOVE && len == 5) {
        int dx = (short)(msg[1] | msg[2] << 8), dy = (short)(msg[3] | msg[4] << 8);
        DWORD now = GetTickCount();
        for (int e = 0; e < 4; e++) {
            if (!(c.exits & 1 << e)) continue;
            BOOL wall = wall_at(s, c.pos, e) && !in_corner(s, c.pos, e);
            if (press_feed(&c.press[e], wall, outward(e, dx, dy), now, c.resistance) == PRESS_CROSS) {
                int f = along(s, c.pos, e); /* pushed through an edge to another PC: input goes there */
                input_controlled_stop();
                leave[0] = MSG_LEAVE, leave[1] = 1, leave[2] = (BYTE)f, leave[3] = (BYTE)(f >> 8), leave[4] = (BYTE)e;
                return c.tell_edge ? 5 : 4;
            }
        }
        c.pos.x += dx, c.pos.y += dy;
        /* Kept on a screen, as Windows keeps the pointer: beside a smaller screen there is no desktop. */
        MONITORINFO mi = {0};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromPoint(c.pos, MONITOR_DEFAULTTONEAREST), &mi);
        c.pos.x = max(mi.rcMonitor.left, min(mi.rcMonitor.right - 1, c.pos.x));
        c.pos.y = max(mi.rcMonitor.top, min(mi.rcMonitor.bottom - 1, c.pos.y));
        INPUT in = {0};
        in.type = INPUT_MOUSE;
        /* Aim at the pixel's centre: Windows rounds the 0..65535 position down when mapping it back. */
        LONGLONG w = max(1, v.right - v.left - 1), h = max(1, v.bottom - v.top - 1);
        in.mi.dx = (LONG)min(65535, ((LONGLONG)(c.pos.x - v.left) * 2 + 1) * 65535 / (2 * w));
        in.mi.dy = (LONG)min(65535, ((LONGLONG)(c.pos.y - v.top) * 2 + 1) * 65535 / (2 * h));
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in.mi.dwExtraInfo = INJECTED_MARK;
        SendInput(1, &in, sizeof in);
    } else if (msg[0] == MSG_BUTTON && len == 3 && msg[1] >= 1 && msg[1] <= 5) {
        button(msg[1], msg[2] != 0);
    } else if (msg[0] == MSG_WHEEL && len == 4) {
        mouse(msg[3] ? 0x1000 /* MOUSEEVENTF_HWHEEL, Vista+ */ : MOUSEEVENTF_WHEEL,
              (DWORD)(short)(msg[1] | msg[2] << 8));
    } else if (msg[0] == MSG_KEY && len == 4 && msg[1]) {
        BYTE vk = msg[1];
        BOOL up = msg[3] & 1;
        key(vk, msg[2], (msg[3] & 2) != 0, up);
        c.keys[vk] = !up, c.scan[vk] = msg[2], c.ext[vk] = (msg[3] & 2) != 0;
    }
    return 0;
}

int input_replay(const unsigned char *msg, int len, unsigned char leave[8])
{
    HANDLE dpi = per_monitor_dpi(); /* the UI thread itself stays system-aware, for its window */
    int n = replay(msg, len, leave);
    restore_dpi(dpi);
    return n;
}
