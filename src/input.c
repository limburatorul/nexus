/* Main PC: low-level hooks swallow local input while it is on the other PC and post it to the UI
 * thread, which sends it. The pointer is parked mid-screen under an invisible window with no
 * cursor, so it disappears and every hook position is a delta from the park point.
 * Controlled PC: SendInput replays what arrives, and the return edge hands input back. */

#include "link.h" /* winsock2.h before windows.h */
#include "input.h"

#define HT_SET (WM_APP + 1)   /* wParam: connected, lParam: edge */
#define HT_LEAVE (WM_APP + 2) /* wParam: 0, or 1 + position along the edge (0..65535) */
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

/* Position along an edge, 0..65535, so screens of different sizes line up proportionally. */
static int along(RECT v, POINT p, int edge)
{
    if (edge == EDGE_LEFT || edge == EDGE_RIGHT) return MulDiv(p.y - v.top, 65535, v.bottom - v.top - 1);
    return MulDiv(p.x - v.left, 65535, v.right - v.left - 1);
}

/* A point just inside an edge, at a position along it. */
static POINT on_edge(RECT v, int edge, int frac)
{
    POINT p;
    int x = v.left + MulDiv(frac, v.right - v.left - 1, 65535), y = v.top + MulDiv(frac, v.bottom - v.top - 1, 65535);
    switch (edge) {
    case EDGE_LEFT: p.x = v.left + 2, p.y = y; break;
    case EDGE_RIGHT: p.x = v.right - 3, p.y = y; break;
    case EDGE_TOP: p.x = x, p.y = v.top + 2; break;
    default: p.x = x, p.y = v.bottom - 3;
    }
    return p;
}

/* ---- main PC ---- */

static struct {
    HWND ui, hider;
    DWORD thread;
    BOOL connected, remote, tap_clean;
    int edge, buttons;
    BYTE down[256], passdown[256];
    POINT park, back;
    DWORD last_tap;
} m;

static void post(UINT type, DWORD packed) { PostMessageW(m.ui, WM_INPUT_SEND, type, (LPARAM)packed); }

static void enter(POINT from, BOOL keep)
{
    m.remote = TRUE;
    m.back = from;
    m.tap_clean = FALSE;
    post(MSG_ENTER, m.edge | (DWORD)keep << 4 | (DWORD)(keep ? 0 : along(vscreen(), from, m.edge)) << 8);
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

static BOOL at_edge(POINT p)
{
    RECT v = vscreen();
    switch (m.edge) {
    case EDGE_LEFT: return p.x <= v.left;
    case EDGE_RIGHT: return p.x >= v.right - 1;
    case EDGE_TOP: return p.y <= v.top;
    default: return p.y >= v.bottom - 1;
    }
}

static LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp)
{
    MSLLHOOKSTRUCT *e = (MSLLHOOKSTRUCT *)lp;
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
        else if (wp == WM_MOUSEMOVE && m.connected && !m.buttons && at_edge(e->pt)) {
            enter(e->pt, FALSE);
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
                    enter(p, TRUE);
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
    wc.lpszClassName = L"BridgerHider"; /* no class cursor: the pointer vanishes over it */
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
            m.edge = (int)msg.lParam;
            if (!m.connected && m.remote) leave_local(m.back);
        } else if (msg.message == HT_LEAVE) {
            if (m.remote) leave_local(msg.wParam ? on_edge(vscreen(), m.edge, (int)msg.wParam - 1) : m.back);
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

void input_main_set(BOOL connected, int edge)
{
    if (m.thread) PostThreadMessageW(m.thread, HT_SET, connected, edge);
}

void input_main_leave(int frac) { PostThreadMessageW(m.thread, HT_LEAVE, frac < 0 ? 0 : frac + 1, 0); }

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
    BOOL active;
    int ret_edge, buttons;
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

int input_replay(const unsigned char *msg, int len, unsigned char out[8])
{
    RECT v = vscreen();
    if (msg[0] == MSG_ENTER && len == 5 && msg[1] <= EDGE_BOTTOM) {
        c.ret_edge = msg[1] ^ 1; /* the main PC's right edge leads in at this PC's left, and so on */
        if (msg[2]) {
            GetCursorPos(&c.pos);
        } else {
            c.pos = on_edge(v, c.ret_edge, msg[3] | msg[4] << 8);
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
        BOOL out_edge = (c.ret_edge == EDGE_LEFT && c.pos.x <= v.left && dx < 0) ||
                        (c.ret_edge == EDGE_RIGHT && c.pos.x >= v.right - 1 && dx > 0) ||
                        (c.ret_edge == EDGE_TOP && c.pos.y <= v.top && dy < 0) ||
                        (c.ret_edge == EDGE_BOTTOM && c.pos.y >= v.bottom - 1 && dy > 0);
        if (out_edge) { /* pushed through the return edge: input goes home at the same height */
            int f = along(v, c.pos, c.ret_edge);
            input_release_all();
            c.active = FALSE;
            out[0] = MSG_LEAVE, out[1] = 1, out[2] = (BYTE)f, out[3] = (BYTE)(f >> 8);
            return 4;
        }
        c.pos.x = max(v.left, min(v.right - 1, c.pos.x + dx));
        c.pos.y = max(v.top, min(v.bottom - 1, c.pos.y + dy));
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
