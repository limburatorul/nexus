/* Main PC: low-level hooks swallow local input while it is on the other PC and post it to the UI
 * thread, which sends it. The pointer is parked mid-screen under an invisible window with no
 * cursor, so it disappears and every hook position is a delta from the park point.
 * Controlled PC: SendInput replays what arrives, and the return edge hands input back. */

#include "link.h" /* winsock2.h before windows.h */
#include "input.h"
#include <stdio.h>

#define HT_SET (WM_APP + 1)   /* wParam: some PC connected, lParam: edges that lead to one */
#define HT_LEAVE (WM_APP + 2) /* wParam: 0, or 1 + position along the edge (0..65535); lParam: edge, -1: where it left */
#define TAP_MS 400

static RECT vscreen(void)
{
    RECT v;
    v.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    v.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    v.right = v.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    v.bottom = v.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return v;
}

/* The stretch lo..hi of a desktop edge that screens touch. With a 4K and a 1080p screen side by side,
 * 60% of the way down the 1080p one is 60% of that screen, not of the desktop's full height.
 * ponytail: screens stacked along an edge with a gap between them map across the gap too */
static void span_of(const RECT *screen, int n, RECT v, int edge, int *lo, int *hi)
{
    BOOL vertical = edge == EDGE_LEFT || edge == EDGE_RIGHT;
    *lo = vertical ? v.bottom : v.right, *hi = vertical ? v.top : v.left;
    for (int i = 0; i < n; i++) {
        const RECT *r = &screen[i];
        BOOL touches = edge == EDGE_LEFT ? r->left == v.left : edge == EDGE_RIGHT ? r->right == v.right
                     : edge == EDGE_TOP ? r->top == v.top : r->bottom == v.bottom;
        if (!touches) continue;
        *lo = min(*lo, vertical ? r->top : r->left);
        *hi = max(*hi, vertical ? r->bottom : r->right);
    }
    if (*lo >= *hi) *lo = vertical ? v.top : v.left, *hi = vertical ? v.bottom : v.right;
}

typedef struct { RECT r[16]; int n; } Screens;

static BOOL CALLBACK add_screen(HMONITOR m, HDC dc, LPRECT r, LPARAM s)
{
    (void)m, (void)dc;
    Screens *all = (Screens *)s;
    if (all->n < 16) all->r[all->n++] = *r;
    return TRUE;
}

static void edge_span(RECT v, int edge, int *lo, int *hi)
{
    Screens s = {0};
    EnumDisplayMonitors(NULL, NULL, add_screen, (LPARAM)&s);
    span_of(s.r, s.n, v, edge, lo, hi);
}

/* Position along an edge, 0..65535, of the screens at that edge, so screens of different sizes line up
 * proportionally: 60% of the way down here arrives 60% of the way down there. */
static int along(RECT v, POINT p, int edge)
{
    int lo, hi;
    edge_span(v, edge, &lo, &hi);
    int at = edge == EDGE_LEFT || edge == EDGE_RIGHT ? p.y : p.x;
    return max(0, min(65535, MulDiv(at - lo, 65535, hi - lo - 1)));
}

/* A point just inside an edge, at a position along the screens there. */
static POINT on_edge(RECT v, int edge, int frac)
{
    POINT p;
    int lo, hi;
    edge_span(v, edge, &lo, &hi);
    int at = lo + MulDiv(frac, hi - lo - 1, 65535);
    switch (edge) {
    case EDGE_LEFT: p.x = v.left + 2, p.y = at; break;
    case EDGE_RIGHT: p.x = v.right - 3, p.y = at; break;
    case EDGE_TOP: p.x = at, p.y = v.top + 2; break;
    default: p.x = at, p.y = v.bottom - 3;
    }
    return p;
}

/* ---- main PC ---- */

static struct {
    HWND ui, hider;
    DWORD thread;
    BOOL connected, remote, tap_clean;
    int edges, buttons; /* edges: bit per edge of this screen that leads to a connected PC */
    BYTE down[256], passdown[256];
    POINT park, back;
    DWORD last_tap;
    DWORD mouse_events, key_events, last_vk, last_flags; /* for the test mode's "hooks" */
} m;

static void post(UINT type, DWORD packed) { PostMessageW(m.ui, WM_INPUT_SEND, type, (LPARAM)packed); }

/* The UI thread picks the PC: the one past that edge, or for the shortcut (keep) the one used last. */
static void enter(POINT from, BOOL keep, int edge)
{
    m.remote = TRUE;
    m.back = from;
    m.tap_clean = FALSE;
    post(MSG_ENTER, edge | (DWORD)keep << 4 | (DWORD)(keep ? 0 : along(vscreen(), from, edge)) << 8);
    m.park.x = GetSystemMetrics(SM_CXSCREEN) / 2;
    m.park.y = GetSystemMetrics(SM_CYSCREEN) / 2;
    SetWindowPos(m.hider, HWND_TOPMOST, m.park.x - 32, m.park.y - 32, 64, 64, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetCursorPos(m.park.x, m.park.y);
    PostMessageW(m.ui, WM_INPUT_STATE, TRUE, 0);
}

static void leave_local(POINT to)
{
    m.remote = FALSE;
    ShowWindow(m.hider, SW_HIDE);
    SetCursorPos(to.x, to.y);
    PostMessageW(m.ui, WM_INPUT_STATE, FALSE, 0);
}

/* Which edge leading to a connected PC the pointer touches, or -1. */
static int at_edge(POINT p)
{
    RECT v = vscreen();
    BOOL on[4] = {p.x <= v.left, p.x >= v.right - 1, p.y <= v.top, p.y >= v.bottom - 1};
    for (int e = 0; e < 4; e++)
        if (on[e] && m.edges & 1 << e) return e;
    return -1;
}

static LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp)
{
    MSLLHOOKSTRUCT *e = (MSLLHOOKSTRUCT *)lp;
    m.mouse_events++;
    if (code < 0 || (e->flags & LLMHF_INJECTED)) return CallNextHookEx(NULL, code, wp, lp);
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
        if (btn) m.buttons = down ? m.buttons | 1 << btn : m.buttons & ~(1 << btn);
        /* Never cross mid-drag: the button would stay down here and arrive up-only there. */
        else if (wp == WM_MOUSEMOVE && !m.buttons && at_edge(e->pt) >= 0) {
            enter(e->pt, FALSE, at_edge(e->pt));
            return 1;
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

static LRESULT CALLBACK key_hook(int code, WPARAM wp, LPARAM lp)
{
    KBDLLHOOKSTRUCT *e = (KBDLLHOOKSTRUCT *)lp;
    m.key_events++, m.last_vk = e->vkCode, m.last_flags = e->flags;
    if (code < 0 || (e->flags & LLKHF_INJECTED)) return CallNextHookEx(NULL, code, wp, lp);
    BYTE vk = (BYTE)e->vkCode;
    BOOL up = (e->flags & LLKHF_UP) != 0, repeat = !up && m.down[vk];
    m.down[vk] = !up;

    if (!up && !repeat) {
        /* Double-tap Right Ctrl switches, with no other key between the taps. */
        if (vk == VK_RCONTROL) {
            if (m.tap_clean && e->time - m.last_tap < TAP_MS && m.connected) {
                m.tap_clean = FALSE;
                if (m.remote) {
                    post(MSG_LEAVE, 0);
                    leave_local(m.back);
                } else if (!m.buttons) {
                    POINT p;
                    GetCursorPos(&p);
                    enter(p, TRUE, 0);
                }
                return 1;
            }
            m.tap_clean = TRUE;
            m.last_tap = e->time;
        } else {
            m.tap_clean = FALSE;
        }
        /* Ctrl+Alt+Pause always brings input home (Ctrl turns Pause into Cancel). */
        BOOL ctrl = m.down[VK_LCONTROL] || m.down[VK_RCONTROL], alt = m.down[VK_LMENU] || m.down[VK_RMENU];
        if (m.remote && ctrl && alt && (vk == VK_PAUSE || vk == VK_CANCEL)) {
            post(MSG_LEAVE, 0);
            leave_local(m.back);
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

static DWORD WINAPI hook_thread(void *ready)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"NexusHider"; /* no class cursor: the pointer vanishes over it */
    RegisterClassW(&wc);
    m.hider = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, wc.lpszClassName,
                              L"", WS_POPUP, 0, 0, 64, 64, NULL, NULL, inst, NULL);
    SetLayeredWindowAttributes(m.hider, 0, 1, LWA_ALPHA); /* alpha 1: invisible, still under the pointer */
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
            if (m.remote) leave_local(msg.wParam && edge >= 0 ? on_edge(vscreen(), edge, (int)msg.wParam - 1) : m.back);
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
    CloseHandle(CreateThread(NULL, 0, hook_thread, ready, 0, &m.thread));
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
}

void input_main_set(BOOL connected, int edges)
{
    if (m.thread) PostThreadMessageW(m.thread, HT_SET, connected, edges);
}

void input_main_leave(int edge, int frac)
{
    if (m.thread) PostThreadMessageW(m.thread, HT_LEAVE, frac < 0 ? 0 : frac + 1, edge);
}

void input_debug(char *out, int cap)
{
    _snprintf(out, cap, "hooks=%d mouse=%lu keys=%lu last_vk=%02lx flags=%02lx remote=%d connected=%d",
              m.thread != 0, m.mouse_events, m.key_events, m.last_vk, m.last_flags, m.remote, m.connected);
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
    POINT pos; /* tracked here: GetCursorPos lags behind SendInput */
    BYTE keys[256], scan[256], ext[256];
} c;

static void mouse(DWORD flags, DWORD data)
{
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    SendInput(1, &in, sizeof in);
}

static void key(BYTE vk, BYTE scan, BOOL ext, BOOL up)
{
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk; /* by virtual key, so Pause, NumLock and media keys arrive as themselves */
    in.ki.wScan = scan;
    in.ki.dwFlags = (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
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

int input_replay(const unsigned char *msg, int len, unsigned char leave[8])
{
    RECT v = vscreen();
    if (msg[0] == MSG_ENTER && (len == 5 || len == 6) && msg[1] <= EDGE_BOTTOM) {
        int in = msg[1] ^ 1; /* moving right, the pointer comes in at this PC's left, and so on */
        /* A main PC before 0.3 knows one PC only: the way in is the way back. */
        c.exits = len == 6 ? msg[5] & 15 : 1 << in;
        c.tell_edge = len == 6;
        if (msg[2]) {
            GetCursorPos(&c.pos);
        } else {
            c.pos = on_edge(v, in, msg[3] | msg[4] << 8);
            SetCursorPos(c.pos.x, c.pos.y);
        }
        c.active = TRUE;
        return 0;
    }
    if (msg[0] == MSG_LEAVE && len == 4) {
        input_release_all();
        c.active = FALSE;
        return 0;
    }
    if (!c.active) return 0;

    if (msg[0] == MSG_MOVE && len == 5) {
        int dx = (short)(msg[1] | msg[2] << 8), dy = (short)(msg[3] | msg[4] << 8);
        BOOL out[4] = {c.pos.x <= v.left && dx < 0, c.pos.x >= v.right - 1 && dx > 0, c.pos.y <= v.top && dy < 0,
                       c.pos.y >= v.bottom - 1 && dy > 0};
        for (int e = 0; e < 4; e++)
            if (out[e] && c.exits & 1 << e) { /* pushed through an edge to another PC: input goes there */
                int f = along(v, c.pos, e);
                input_release_all();
                c.active = FALSE;
                leave[0] = MSG_LEAVE, leave[1] = 1, leave[2] = (BYTE)f, leave[3] = (BYTE)(f >> 8), leave[4] = (BYTE)e;
                return c.tell_edge ? 5 : 4;
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
        LONGLONG w = v.right - v.left - 1, h = v.bottom - v.top - 1;
        in.mi.dx = (LONG)min(65535, ((LONGLONG)(c.pos.x - v.left) * 2 + 1) * 65535 / (2 * w));
        in.mi.dy = (LONG)min(65535, ((LONGLONG)(c.pos.y - v.top) * 2 + 1) * 65535 / (2 * h));
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
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
