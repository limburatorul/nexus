/* Bridger: one keyboard, mouse and sound for PCs from Windows XP to 11.
 * The settings window in Beamer's Vernier look, the tray icon, the config and the network thread.
 * Everything is plain Win32/GDI so the same exe runs on XP SP3 and on Windows 11. */

#include "link.h" /* winsock2.h has to come before windows.h */
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "audio.h"
#include "input.h"
#include "resource.h"
#include "version.h"

#define WIDEN2(s) L##s
#define WIDEN(s) WIDEN2(s)
#define APP_CLASS L"BridgerWnd"
#define SITE_URL L"https://github.com/limburatorul/bridger"

#define WM_TRAY (WM_APP + 1)
#define WM_BRIDGER_QUIT (WM_APP + 2) /* sent by the installer before it overwrites the exe */
#define WM_BRIDGER_SHOW (WM_APP + 3) /* sent by a second instance */
#define WM_NET_STATUS (WM_APP + 4)   /* wParam: NS_* */
#define WM_NET_MSG (WM_APP + 5)      /* wParam: length, lParam: malloc'd message, freed here */
#define WM_AUDIO_STATE (WM_APP + 6)  /* wParam: AUDIO_* of this controlled PC */
#define TIMER_PING 1

enum { ID_TRAY_OPEN = 1, ID_TRAY_QUIT, ID_NAV0 = 100, ID_QUIT = 200, ID_SITE, ID_SETUP, ID_ROLE0, ID_ROLE1,
       ID_EDGE0, ID_EDGE3 = ID_EDGE0 + 3, ID_PASSWORD, ID_PEER, ID_PASSWORD_LABEL, ID_PEER_LABEL };
enum { K_NAV, K_OUTLINE, K_LINK, K_SEGMENT };
enum { ROLE_MAIN, ROLE_CONTROLLED };
enum { PAGE_OVERVIEW, PAGE_CROSSING, PAGE_KEYBOARD, PAGE_SOUND, PAGE_FILES, PAGE_CONNECTION };
#define NO_FILL ((COLORREF)0xFFFFFFFF)

/* Vernier palette, copied from Beamer's tokens.py (GPL-3). */
typedef struct { COLORREF ground, panel, well, rule, edge, ink, ink2, ink3, signal, amber, fault, off; } Palette;
static const Palette DARK = {
    RGB(0x0e, 0x0f, 0x0d), RGB(0x16, 0x17, 0x14), RGB(0x1f, 0x20, 0x1c), RGB(0x2a, 0x2b, 0x26),
    RGB(0x6e, 0x6f, 0x66), RGB(0xef, 0xee, 0xe6), RGB(0xb3, 0xb2, 0xa7), RGB(0x8f, 0x8e, 0x84),
    RGB(0x5f, 0xd4, 0xf4), RGB(0xf2, 0xb5, 0x44), RGB(0xff, 0x7a, 0x66), RGB(0x4a, 0x4b, 0x44)};
static const Palette LIGHT = {
    RGB(0xf1, 0xf0, 0xea), RGB(0xfb, 0xfa, 0xf6), RGB(0xe5, 0xe4, 0xdc), RGB(0xdc, 0xdb, 0xd3),
    RGB(0x7f, 0x7e, 0x74), RGB(0x1b, 0x1c, 0x18), RGB(0x46, 0x46, 0x3f), RGB(0x66, 0x65, 0x5c),
    RGB(0x00, 0x6a, 0x88), RGB(0x8f, 0x5c, 0x00), RGB(0xb5, 0x35, 0x22), RGB(0xae, 0xad, 0xa3)};
enum { C_INK, C_INK2, C_INK3, C_SIGNAL, C_AMBER, C_FAULT, C_OFF };

static const struct { const wchar_t *name, *title, *desc, *eyebrow; } PAGES[] = {
    {L"Overview", L"OVERVIEW", L"The PC this one works with, where input is right now, and how the link is doing.",
     L"MACHINE"},
    {L"Crossing", L"CROSSING",
     L"Which edge of this screen leads to the controlled PC. Push the pointer through it to cross, and back "
     L"through the facing edge over there to return.",
     L"EDGE"},
    {L"Keyboard", L"KEYBOARD", L"Switch without the mouse, and a way home that always works.", L"SHORTCUTS"},
    {L"Sound", L"SOUND", L"The controlled PC's sound, played on this PC's speakers.", L"SOUND"},
    {L"Files", L"FILES", L"Copy files on one PC, switch, and paste them on the other.", L"FILES"},
    {L"Connection", L"CONNECTION", L"This PC's role, the password both PCs share, and where to find the other one.",
     L"THIS PC"},
};
#define NPAGES ((int)(sizeof PAGES / sizeof PAGES[0]))

enum { NS_SETUP, NS_WAITING, NS_CONNECTING, NS_UNREACHABLE, NS_PASSWORD, NS_VERSION, NS_ROLE, NS_PEER, NS_PORT,
       NS_CONNECTED };
static const wchar_t *STATUS_WORD[] = {L"Not set up",     L"Waiting",          L"Connecting", L"Not reachable",
                                       L"Wrong password", L"Version mismatch", L"Same role",  L"Not Bridger",
                                       L"Port in use",    L"Connected"};
static const wchar_t *STATUS_NOTE[] = {
    L"Choose this PC's role and the shared password on the Connection page.",
    L"Waiting for the main PC to connect.",
    L"Trying to reach the controlled PC.",
    L"The controlled PC does not answer. Check that Bridger runs there and that the address is right.",
    L"The two PCs have different passwords. Type the same one on both.",
    L"The other PC runs a different Bridger. Install the same version on both.",
    L"Both PCs have the same role. Make one the main PC and the other the controlled PC.",
    L"Something other than Bridger answered on port 24800.",
    L"Another program uses port 24800, so the main PC cannot connect to this one.",
    L""};
static const wchar_t *EDGE_NAMES[] = {L"left", L"right", L"top", L"bottom"};

typedef struct { int kind; RECT r; const wchar_t *s; HFONT f; int color; HWND edit; } Label;
enum { L_TEXT, L_LINE, L_DOT, L_WELL, L_KEYCAP };

static struct {
    HINSTANCE inst;
    HWND wnd, hover, nav[NPAGES], quit, site, setup, role[2], edge[4], pw, peer, pw_label, peer_label;
    WNDPROC button_proc;
    NOTIFYICONDATAW tray;
    UINT taskbar_created;
    const Palette *p;
    HBRUSH well_brush;
    int dpi, page, body_h;
    HFONT title, body, nav_font, nav_sel, status_font, eyebrow, button, small, mono;
    RECT desc_rc, module_rc;
    Label labels[32];
    int nlabels;
    wchar_t ini[MAX_PATH], addresses[128], line[256], note[256];

    /* config: written by the UI thread, read by the network thread under cs */
    struct { int role, edge; wchar_t peer[128]; unsigned char key[32]; BOOL has_key; } cfg;

    /* network */
    CRITICAL_SECTION cs;
    HANDLE wake;
    SOCKET conn, listener;
    Link link;
    BOOL link_up;
    int gen;
    char my_name[64];
    wchar_t peer_name[64];

    /* UI view of the link */
    int status, rtt;
    BOOL remote; /* main: input is on the controlled PC */
    int audio_state; /* AUDIO_*: how the controlled PC captures its sound */
} g;

static int S(int px) { return MulDiv(px, g.dpi, 96); }

static const int SIDEBAR = 280, FOOTER = 96, NAV_H = 52, STATUS_H = 132;

/* ---- config ---- */

static void config_load(void)
{
    wchar_t v[128];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, g.ini);
    wcscat(g.ini, L"\\Bridger");
    CreateDirectoryW(g.ini, NULL);
    wcscat(g.ini, L"\\bridger.ini");

    GetPrivateProfileStringW(L"bridger", L"role", L"main", v, 128, g.ini);
    g.cfg.role = lstrcmpiW(v, L"controlled") == 0 ? ROLE_CONTROLLED : ROLE_MAIN;
    GetPrivateProfileStringW(L"bridger", L"edge", L"right", v, 128, g.ini);
    g.cfg.edge = EDGE_RIGHT;
    for (int i = 0; i < 4; i++)
        if (lstrcmpiW(v, EDGE_NAMES[i]) == 0) g.cfg.edge = i;
    GetPrivateProfileStringW(L"bridger", L"peer", L"", g.cfg.peer, 128, g.ini);
    GetPrivateProfileStringW(L"bridger", L"key", L"", v, 128, g.ini);
    g.cfg.has_key = wcslen(v) == 64;
    for (int i = 0; g.cfg.has_key && i < 64; i++) {
        wchar_t ch = v[i];
        int d = ch >= L'0' && ch <= L'9' ? ch - L'0' : ch >= L'a' && ch <= L'f' ? ch - L'a' + 10 : -1;
        if (d < 0) g.cfg.has_key = FALSE;
        else g.cfg.key[i / 2] = (unsigned char)(i % 2 ? g.cfg.key[i / 2] << 4 | d : d);
    }
}

static void config_save(void)
{
    wchar_t hex[65] = L"";
    for (int i = 0; g.cfg.has_key && i < 32; i++) _snwprintf(hex + 2 * i, 3, L"%02x", g.cfg.key[i]);
    WritePrivateProfileStringW(L"bridger", L"role", g.cfg.role == ROLE_CONTROLLED ? L"controlled" : L"main", g.ini);
    WritePrivateProfileStringW(L"bridger", L"edge", EDGE_NAMES[g.cfg.edge], g.ini);
    WritePrivateProfileStringW(L"bridger", L"peer", g.cfg.peer, g.ini);
    /* The stretched key, never the password itself. */
    WritePrivateProfileStringW(L"bridger", L"key", hex, g.ini);
}

static BOOL configured(void) { return g.cfg.has_key && (g.cfg.role == ROLE_CONTROLLED || g.cfg.peer[0]); }

/* ---- network thread ---- */

static void net_status(int st) { PostMessageW(g.wnd, WM_NET_STATUS, st, 0); }

static void net_send(const unsigned char *msg, int len)
{
    EnterCriticalSection(&g.cs);
    if (g.link_up) link_send(&g.link, msg, len);
    LeaveCriticalSection(&g.cs);
}

/* Drops the current connection and makes the network thread re-read the config. */
static void net_restart(void)
{
    EnterCriticalSection(&g.cs);
    g.gen++;
    if (g.conn != INVALID_SOCKET) shutdown(g.conn, SD_BOTH);
    if (g.listener != INVALID_SOCKET) closesocket(g.listener), g.listener = INVALID_SOCKET;
    LeaveCriticalSection(&g.cs);
    SetEvent(g.wake);
}

static SOCKET dial(const char *host)
{
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(LINK_PORT);
    a.sin_addr.s_addr = inet_addr(host);
    if (a.sin_addr.s_addr == INADDR_NONE) {
        struct hostent *h = gethostbyname(host);
        if (!h) return INVALID_SOCKET;
        memcpy(&a.sin_addr, h->h_addr_list[0], 4);
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb); /* connect with a 3 s limit instead of Windows' 21 s */
    connect(s, (struct sockaddr *)&a, sizeof a);
    fd_set w, e;
    FD_ZERO(&w), FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    struct timeval tv = {3, 0};
    int err = 1, len = sizeof err;
    if (select(0, NULL, &w, &e, &tv) == 1 && FD_ISSET(s, &w)) getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    if (err) closesocket(s), s = INVALID_SOCKET;
    return s;
}

static SOCKET listen_socket(void)
{
    EnterCriticalSection(&g.cs);
    if (g.listener == INVALID_SOCKET) {
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_port = htons(LINK_PORT);
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 2) == 0) g.listener = s;
        else closesocket(s);
    }
    SOCKET s = g.listener;
    LeaveCriticalSection(&g.cs);
    return s;
}

/* Runs one connection: handshake, then every message goes to the UI thread, pings answered here. */
static int session(SOCKET s, int role, const unsigned char key[32])
{
    BOOL on = TRUE;
    DWORD timeout = 6000; /* both ends ping every second; silence this long means the link is gone */
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&on, sizeof on);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof timeout);
    EnterCriticalSection(&g.cs);
    g.conn = s;
    LeaveCriticalSection(&g.cs);

    Link l;
    char peer[64];
    int r = link_handshake(&l, s, role == ROLE_MAIN, key, g.my_name, peer);
    if (r == LINK_OK) {
        EnterCriticalSection(&g.cs);
        g.link = l;
        g.link_up = TRUE;
        MultiByteToWideChar(CP_UTF8, 0, peer, -1, g.peer_name, 64);
        LeaveCriticalSection(&g.cs);
        net_status(NS_CONNECTED);
        unsigned char *buf = malloc(LINK_MAX);
        int n;
        while ((n = link_recv(&l, buf, LINK_MAX)) > 0) {
            if (buf[0] == MSG_PING && n == 5) {
                buf[0] = MSG_PONG;
                net_send(buf, 5);
            } else {
                unsigned char *copy = malloc(n);
                memcpy(copy, buf, n);
                PostMessageW(g.wnd, WM_NET_MSG, n, (LPARAM)copy);
            }
        }
        free(buf);
    }
    EnterCriticalSection(&g.cs);
    g.link_up = FALSE;
    g.conn = INVALID_SOCKET;
    closesocket(s);
    LeaveCriticalSection(&g.cs);
    switch (r) {
    case LINK_OK: return 0;
    case LINK_ERR_PEER: return NS_PEER;
    case LINK_ERR_VERSION: return NS_VERSION;
    case LINK_ERR_ROLE: return NS_ROLE;
    case LINK_ERR_PASSWORD: return NS_PASSWORD;
    default: return role == ROLE_MAIN ? NS_UNREACHABLE : 0;
    }
}

static DWORD WINAPI net_thread(void *unused)
{
    (void)unused;
    int gen = -1, sticky = 0; /* the last failure stays on screen until the next success or a config change */
    for (;;) {
        EnterCriticalSection(&g.cs);
        int role = g.cfg.role, ready = configured();
        unsigned char key[32];
        char host[128];
        memcpy(key, g.cfg.key, 32);
        WideCharToMultiByte(CP_ACP, 0, g.cfg.peer, -1, host, sizeof host, NULL, NULL);
        if (gen != g.gen) gen = g.gen, sticky = 0;
        LeaveCriticalSection(&g.cs);

        if (!ready) {
            net_status(NS_SETUP);
            WaitForSingleObject(g.wake, INFINITE);
            continue;
        }
        SOCKET s;
        if (role == ROLE_MAIN) {
            EnterCriticalSection(&g.cs);
            if (g.listener != INVALID_SOCKET) closesocket(g.listener), g.listener = INVALID_SOCKET;
            LeaveCriticalSection(&g.cs);
            net_status(sticky ? sticky : NS_CONNECTING);
            s = dial(host);
            if (s == INVALID_SOCKET) {
                net_status(sticky = NS_UNREACHABLE);
                WaitForSingleObject(g.wake, 3000);
                continue;
            }
        } else {
            SOCKET ls = listen_socket();
            if (ls == INVALID_SOCKET) {
                net_status(NS_PORT);
                WaitForSingleObject(g.wake, 3000);
                continue;
            }
            net_status(sticky ? sticky : NS_WAITING);
            s = accept(ls, NULL, NULL);
            if (s == INVALID_SOCKET) continue; /* the listener was closed for a config change */
        }
        sticky = session(s, role, key);
        if (sticky) net_status(sticky);
        if (sticky || role == ROLE_MAIN) WaitForSingleObject(g.wake, sticky ? 3000 : 500);
    }
    return 0;
}

/* ---- fonts and theme ---- */

static HFONT mkfont(const wchar_t *face, int px, int weight)
{
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void load_fonts(void)
{
    static const int ids[] = {IDR_FONT_REGULAR, IDR_FONT_SEMIBOLD, IDR_FONT_BOLD, IDR_FONT_MONO};
    for (int i = 0; i < 4; i++) {
        HRSRC r = FindResourceW(g.inst, MAKEINTRESOURCEW(ids[i]), (LPCWSTR)RT_RCDATA);
        DWORD n = 0;
        AddFontMemResourceEx(LockResource(LoadResource(g.inst, r)), SizeofResource(g.inst, r), 0, &n);
    }
    /* The static SemiBold cut registers as its own legacy family, as Google's static fonts do. */
    g.title = mkfont(L"Hanken Grotesk", 28, FW_BOLD);
    g.body = mkfont(L"Hanken Grotesk", 14, FW_NORMAL);
    g.nav_font = mkfont(L"Hanken Grotesk", 15, FW_NORMAL);
    g.nav_sel = mkfont(L"Hanken Grotesk SemiBold", 15, FW_NORMAL);
    g.status_font = mkfont(L"Hanken Grotesk SemiBold", 15, FW_NORMAL);
    g.eyebrow = mkfont(L"Hanken Grotesk SemiBold", 11, FW_NORMAL);
    g.button = mkfont(L"Hanken Grotesk SemiBold", 14, FW_NORMAL);
    g.small = mkfont(L"Hanken Grotesk", 13, FW_NORMAL);
    g.mono = mkfont(L"B612 Mono", 13, FW_NORMAL);
    HDC dc = GetDC(NULL);
    TEXTMETRICW tm;
    SelectObject(dc, g.body);
    GetTextMetricsW(dc, &tm);
    g.body_h = tm.tmHeight;
    ReleaseDC(NULL, dc);
}

/* Follows Windows 10/11's app theme; older Windows has none, so Beamer's dark look. */
static BOOL system_is_light(void)
{
    DWORD v = 0, n = sizeof v;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &k) != ERROR_SUCCESS)
        return FALSE;
    LONG r = RegQueryValueExW(k, L"AppsUseLightTheme", NULL, NULL, (BYTE *)&v, &n);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && v != 0;
}

static void apply_theme(void)
{
    g.p = system_is_light() ? &LIGHT : &DARK;
    if (g.well_brush) DeleteObject(g.well_brush);
    g.well_brush = CreateSolidBrush(g.p->well);
    /* Dark title bar on Windows 10 2004+; dwmapi is absent on XP, hence the dynamic load. */
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
        SetAttr set = (SetAttr)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
        BOOL dark = g.p == &DARK;
        if (set) set(g.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
        FreeLibrary(dwm);
    }
    InvalidateRect(g.wnd, NULL, TRUE);
}

static COLORREF color(int c)
{
    const COLORREF all[] = {g.p->ink, g.p->ink2, g.p->ink3, g.p->signal, g.p->amber, g.p->fault, g.p->off};
    return all[c];
}

/* ---- status text ---- */

static int status_color(void)
{
    switch (g.status) {
    case NS_CONNECTED: return C_SIGNAL;
    case NS_SETUP: case NS_WAITING: case NS_CONNECTING: return C_OFF;
    case NS_UNREACHABLE: return C_AMBER;
    default: return C_FAULT;
    }
}

/* g.line: the short status word for the sidebar; g.note: the sentence for the Overview card. */
static void status_text(void)
{
    const wchar_t *name = g.peer_name[0] ? g.peer_name : L"the other PC";
    if (g.status != NS_CONNECTED) {
        lstrcpynW(g.line, STATUS_WORD[g.status], 256);
        lstrcpynW(g.note, STATUS_NOTE[g.status], 256);
    } else if (g.remote) {
        _snwprintf(g.line, 256, L"Input on %ls", name);
        _snwprintf(g.note, 256, L"Input is on %ls. Push back through its edge, double-tap Right Ctrl, "
                                L"or press Ctrl+Alt+Pause to return.", name);
    } else if (g.cfg.role == ROLE_CONTROLLED && input_controlled_active()) {
        _snwprintf(g.line, 256, L"Controlled by %ls", name);
        _snwprintf(g.note, 256, L"%ls is driving this PC.", name);
    } else {
        lstrcpynW(g.line, L"Connected", 256);
        _snwprintf(g.note, 256, g.cfg.role == ROLE_MAIN ? L"Connected to %ls. Push the pointer through the %ls edge "
                                                          L"or double-tap Right Ctrl to cross."
                                                        : L"Connected to %ls, the main PC.",
                   name, EDGE_NAMES[g.cfg.edge]);
    }
    g.line[255] = g.note[255] = 0;
    _snwprintf(g.tray.szTip, 64, L"Bridger: %ls", g.line);
    g.tray.szTip[63] = 0;
    g.tray.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g.tray);
}

static void this_pc_addresses(void)
{
    char host[256];
    struct hostent *h;
    wcscpy(g.addresses, L"unknown");
    if (gethostname(host, sizeof host) != 0 || !(h = gethostbyname(host))) return;
    g.addresses[0] = 0;
    for (int i = 0; h->h_addr_list[i] && i < 4; i++) {
        struct in_addr a;
        memcpy(&a, h->h_addr_list[i], 4);
        wchar_t one[24];
        _snwprintf(one, 24, i ? L", %hs" : L"%hs", inet_ntoa(a));
        one[23] = 0;
        wcscat(g.addresses, one);
    }
}

/* ---- drawing helpers ---- */

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
}

static void round_rect(HDC dc, const RECT *r, int radius, COLORREF fill_c, COLORREF line_c, int width)
{
    HPEN pen = CreatePen(PS_SOLID, width, line_c);
    HBRUSH br = fill_c == NO_FILL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill_c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, 2 * radius, 2 * radius);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    if (fill_c != NO_FILL) DeleteObject(br);
}

static void dot(HDC dc, int x, int y, int size, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x, y, x + size, y + size);
    SelectObject(dc, ob), SelectObject(dc, op), DeleteObject(b);
}

static int text(HDC dc, const wchar_t *s, RECT r, HFONT f, COLORREF c, UINT flags)
{
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    return DrawTextW(dc, s, -1, &r, flags | DT_NOPREFIX);
}

static int text_height(HDC dc, const wchar_t *s, int width, HFONT f)
{
    RECT r = {0, 0, width, 0};
    SelectObject(dc, f);
    DrawTextW(dc, s, -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    return r.bottom;
}

/* Line icons for the sidebar, drawn in a 24-unit box. */
static void draw_icon(HDC dc, int page, int x, int y, COLORREF c)
{
#define P(a, b) x + S(a), y + S(b)
    HPEN pen = CreatePen(PS_SOLID, S(2) > 1 ? S(2) - (S(2) > 2) : 1, c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    switch (page) {
    case PAGE_OVERVIEW: /* screen and a second screen */
        Rectangle(dc, P(2, 4), P(17, 15));
        MoveToEx(dc, P(9, 15), NULL), LineTo(dc, P(9, 19));
        MoveToEx(dc, P(5, 19), NULL), LineTo(dc, P(13, 19));
        Rectangle(dc, P(14, 10), P(23, 20));
        break;
    case PAGE_CROSSING: /* arrows both ways */
        MoveToEx(dc, P(3, 8), NULL), LineTo(dc, P(21, 8)), LineTo(dc, P(17, 4));
        MoveToEx(dc, P(21, 16), NULL), LineTo(dc, P(3, 16)), LineTo(dc, P(7, 20));
        break;
    case PAGE_KEYBOARD:
        RoundRect(dc, P(1, 5), P(23, 19), S(4), S(4));
        for (int i = 0; i < 5; i++) Rectangle(dc, P(5 + 3 * i, 9), P(6 + 3 * i, 10));
        MoveToEx(dc, P(7, 15), NULL), LineTo(dc, P(17, 15));
        break;
    case PAGE_SOUND: /* speaker */
        MoveToEx(dc, P(3, 9), NULL), LineTo(dc, P(7, 9)), LineTo(dc, P(12, 4)), LineTo(dc, P(12, 20)),
            LineTo(dc, P(7, 15)), LineTo(dc, P(3, 15)), LineTo(dc, P(3, 9));
        Arc(dc, P(10, 7), P(20, 17), P(16, 18), P(16, 6));
        Arc(dc, P(11, 3), P(24, 21), P(18, 22), P(18, 2));
        break;
    case PAGE_FILES: /* folder */
        MoveToEx(dc, P(2, 6), NULL), LineTo(dc, P(9, 6)), LineTo(dc, P(11, 8)), LineTo(dc, P(22, 8)),
            LineTo(dc, P(22, 19)), LineTo(dc, P(2, 19)), LineTo(dc, P(2, 6));
        break;
    default: /* link */
        Ellipse(dc, P(2, 6), P(14, 18));
        Ellipse(dc, P(10, 6), P(22, 18));
    }
#undef P
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

/* ---- owner-drawn controls: real BUTTON/STATIC controls, so Tab, Space and screen readers work ---- */

static LRESULT CALLBACK button_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_MOUSEMOVE && g.hover != w) {
        HWND old = g.hover;
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, w, 0};
        g.hover = w;
        TrackMouseEvent(&t);
        if (old) InvalidateRect(old, NULL, FALSE);
        InvalidateRect(w, NULL, FALSE);
    } else if (m == WM_MOUSELEAVE) {
        g.hover = NULL;
        InvalidateRect(w, NULL, FALSE);
    } else if (m == WM_SETCURSOR) {
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    } else if (m == WM_ERASEBKGND) {
        return 1;
    }
    return CallWindowProcW(g.button_proc, w, m, wp, lp);
}

static HWND add_button(const wchar_t *label, int id, int kind)
{
    HWND b = CreateWindowW(L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, g.wnd,
                           (HMENU)(INT_PTR)id, g.inst, NULL);
    SetWindowLongPtrW(b, GWLP_USERDATA, kind);
    g.button_proc = (WNDPROC)SetWindowLongPtrW(b, GWLP_WNDPROC, (LONG_PTR)button_proc);
    return b;
}

/* A field: an owner-drawn STATIC as its label (screen readers name the edit after it) and the edit. */
static HWND add_field(const wchar_t *label, int label_id, int id, DWORD style, HWND *label_wnd)
{
    *label_wnd = CreateWindowW(L"STATIC", label, WS_CHILD | SS_OWNERDRAW, 0, 0, 0, 0, g.wnd,
                               (HMENU)(INT_PTR)label_id, g.inst, NULL);
    HWND e = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | style, 0, 0, 0, 0, g.wnd,
                           (HMENU)(INT_PTR)id, g.inst, NULL);
    SendMessageW(e, WM_SETFONT, (WPARAM)g.body, 0);
    SendMessageW(e, EM_LIMITTEXT, 120, 0);
    return e;
}

static BOOL is_selected(int id)
{
    if (id == ID_ROLE0 || id == ID_ROLE1) return id - ID_ROLE0 == g.cfg.role;
    if (id >= ID_EDGE0 && id <= ID_EDGE3) return id - ID_EDGE0 == g.cfg.edge;
    return FALSE;
}

static void draw_control(const DRAWITEMSTRUCT *d)
{
    const Palette *p = g.p;
    RECT r = d->rcItem;
    int w = r.right, h = r.bottom;
    wchar_t label[64];
    GetWindowTextW(d->hwndItem, label, 64);

    /* Draw off-screen so hover changes do not flicker. */
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ old = SelectObject(dc, bmp);

    if (d->CtlType == ODT_STATIC) { /* field label inside the module */
        fill(dc, &r, p->panel);
        text(dc, label, r, g.button, p->ink2, DT_SINGLELINE | DT_VCENTER);
    } else {
        int kind = (int)GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA);
        BOOL hover = d->hwndItem == g.hover || (d->itemState & ODS_SELECTED);
        BOOL focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
        BOOL in_module = kind == K_SEGMENT || d->CtlID == ID_SETUP;
        COLORREF bg = in_module ? p->panel : p->ground;
        fill(dc, &r, bg);
        if (kind == K_NAV) {
            int page = (int)d->CtlID - ID_NAV0;
            BOOL sel = page == g.page;
            if (sel || hover) round_rect(dc, &r, S(6), sel ? p->well : p->panel, sel ? p->well : p->panel, 1);
            if (sel) {
                RECT bar = {0, S(10), S(3), h - S(10)};
                fill(dc, &bar, p->signal);
            }
            draw_icon(dc, page, S(20), (h - S(24)) / 2, sel ? p->ink : p->ink2);
            RECT t = {S(60), 0, w, h};
            text(dc, label, t, sel ? g.nav_sel : g.nav_font, sel || hover ? p->ink : p->ink2,
                 DT_SINGLELINE | DT_VCENTER);
        } else if (kind == K_OUTLINE || kind == K_SEGMENT) {
            /* A chosen segment is filled like Beamer's primary button; the rest are outlined. */
            BOOL sel = kind == K_SEGMENT && is_selected((int)d->CtlID);
            RECT b = {0, 0, w, h};
            if (sel) round_rect(dc, &b, S(5), p->ink, p->ink, S(1));
            else round_rect(dc, &b, S(5), hover ? p->well : bg, hover ? p->ink2 : p->edge, S(1));
            text(dc, label, r, g.button, sel ? p->ground : p->ink, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        } else {
            text(dc, label, r, g.small, hover ? p->ink : p->ink3, DT_SINGLELINE | DT_VCENTER);
        }
        if (focus) {
            RECT f = {S(1), S(1), w - S(1), h - S(1)};
            round_rect(dc, &f, S(6), NO_FILL, p->signal, S(2));
        }
    }
    BitBlt(d->hDC, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* ---- layout: places this page's controls and records what paint() draws ---- */

static HDC lay_dc;

static void add_label(int kind, int x, int y, int w, int h, const wchar_t *s, HFONT f, int c, HWND edit)
{
    if (g.nlabels == (int)(sizeof g.labels / sizeof g.labels[0])) return;
    Label *l = &g.labels[g.nlabels++];
    l->kind = kind, l->s = s, l->f = f, l->color = c, l->edit = edit;
    SetRect(&l->r, x, y, x + w, y + h);
}

static int add_text(int x, int y, int w, const wchar_t *s, HFONT f, int c)
{
    int h = text_height(lay_dc, s, w, f);
    add_label(L_TEXT, x, y, w, h, s, f, c, NULL);
    return y + h;
}

static void place(HWND c, int x, int y, int w, int h)
{
    MoveWindow(c, x, y, w, h, TRUE);
    ShowWindow(c, SW_SHOW);
}

static int add_field_row(int x, int y, int w, HWND label, HWND edit, const wchar_t *hint)
{
    place(label, x, y, w, S(22));
    y += S(26);
    add_label(L_WELL, x, y, w, S(40), NULL, NULL, 0, edit);
    place(edit, x + S(12), y + (S(40) - g.body_h) / 2, w - S(24), g.body_h);
    y += S(40) + S(8);
    return add_text(x, y, w, hint, g.small, C_INK3) + S(20);
}

static void layout(void)
{
    static wchar_t detail[192], address_line[192];
    HWND page_controls[] = {g.setup, g.role[0], g.role[1], g.edge[0], g.edge[1], g.edge[2], g.edge[3],
                            g.pw, g.peer, g.pw_label, g.peer_label};
    for (int i = 0; i < (int)(sizeof page_controls / sizeof page_controls[0]); i++) ShowWindow(page_controls[i], SW_HIDE);

    RECT c;
    GetClientRect(g.wnd, &c);
    int top = S(STATUS_H) + S(16);
    for (int i = 0; i < NPAGES; i++)
        MoveWindow(g.nav[i], S(16), top + i * S(NAV_H + 4), S(SIDEBAR - 32), S(NAV_H), TRUE);
    int fy = c.bottom - S(FOOTER);
    MoveWindow(g.site, S(28), fy - S(44), S(SIDEBAR - 56), S(28), TRUE);
    MoveWindow(g.quit, c.right - S(32) - S(190), fy + (S(FOOTER) - S(48)) / 2, S(190), S(48), TRUE);

    /* Page column: Beamer's 40px margin, at most 960 wide. */
    int x0 = S(SIDEBAR) + S(40), width = c.right - x0 - S(40);
    if (width > S(960)) width = S(960);
    lay_dc = GetDC(g.wnd);
    g.nlabels = 0;
    int dh = text_height(lay_dc, PAGES[g.page].desc, width, g.body);
    SetRect(&g.desc_rc, x0, S(92), x0 + width, S(92) + dh);

    int my = g.desc_rc.bottom + S(28), mx = x0 + S(32), mw = width - S(64), y = my + S(28);
    add_label(L_LINE, mx, y, mw, S(24), PAGES[g.page].eyebrow, g.eyebrow, C_INK3, NULL);
    y += S(24) + S(16);
    BOOL main_pc = g.cfg.role == ROLE_MAIN;

    switch (g.page) {
    case PAGE_OVERVIEW:
        if (!configured()) {
            y = add_text(mx, y, mw, L"Nothing is set up yet. Choose this PC's role and the shared password first.",
                         g.body, C_INK2) + S(16);
            place(g.setup, mx, y, S(220), S(44));
            y += S(44);
            break;
        }
        add_label(L_DOT, mx, y + S(7), S(10), S(10), NULL, NULL, status_color(), NULL);
        add_label(L_LINE, mx + S(22), y, mw - S(22), S(24),
                  g.status == NS_CONNECTED && g.peer_name[0] ? g.peer_name : main_pc ? g.cfg.peer : L"Main PC",
                  g.status_font, C_INK, NULL);
        y += S(30);
        if (main_pc) _snwprintf(detail, 192, L"Controlled PC  ·  %ls  ·  port %d", g.cfg.peer, LINK_PORT);
        else _snwprintf(detail, 192, L"Main PC  ·  connects to this PC on port %d", LINK_PORT);
        detail[191] = 0;
        add_label(L_LINE, mx, y, mw, S(22), detail, g.body, C_INK3, NULL);
        y += S(30);
        y = add_text(mx, y, mw, g.note, g.body, C_INK2);
        break;
    case PAGE_CROSSING:
        if (!main_pc) {
            y = add_text(mx, y, mw,
                         L"The edge is chosen on the main PC. Push the pointer back through the edge of this "
                         L"screen that faces it to return input there.",
                         g.body, C_INK2);
            break;
        }
        y = add_text(mx, y, mw, L"The controlled PC sits on this side of this screen:", g.body, C_INK2) + S(14);
        for (int i = 0; i < 4; i++) place(g.edge[i], mx + i * S(118), y, S(110), S(40));
        y += S(40) + S(16);
        y = add_text(mx, y, mw,
                     L"The pointer arrives at the same height on the other screen. Crossing waits while a mouse "
                     L"button is held, so a drag never gets split between the two PCs. Copied text comes "
                     L"along when you cross.",
                     g.small, C_INK3);
        break;
    case PAGE_KEYBOARD:
        add_label(L_KEYCAP, mx, y, S(190), S(40), L"Right Ctrl  \x00d7 2", g.mono, C_INK, NULL);
        add_text(mx + S(210), y + S(10), mw - S(210), L"Switch input to the other PC and back.", g.body, C_INK2);
        y += S(56);
        add_label(L_KEYCAP, mx, y, S(190), S(40), L"Ctrl + Alt + Pause", g.mono, C_INK, NULL);
        add_text(mx + S(210), y + S(10), mw - S(210), L"Bring input back to this PC at once.", g.body, C_INK2);
        y += S(56);
        y = add_text(mx, y, mw,
                     main_pc ? L"Both work on this PC's keyboard, wherever input is."
                             : L"These are pressed on the main PC's keyboard.",
                     g.small, C_INK3);
        break;
    case PAGE_CONNECTION:
        place(g.role[0], mx, y, S(170), S(40));
        place(g.role[1], mx + S(178), y, S(170), S(40));
        y += S(40) + S(12);
        y = add_text(mx, y, mw,
                     main_pc ? L"The main PC has the keyboard and mouse, and plays the other PC's sound."
                             : L"The controlled PC is driven by the main PC's keyboard and mouse.",
                     g.small, C_INK3) + S(28);
        {
            int fw = min(mw, S(380));
            y = add_field_row(mx, y, fw, g.pw_label, g.pw,
                              g.cfg.has_key ? L"Set. Type a new one and press Enter to change it."
                                            : L"Type the same password on both PCs and press Enter.");
            if (main_pc)
                y = add_field_row(mx, y, fw, g.peer_label, g.peer,
                                  L"The controlled PC shows its address on its own Connection page.");
        }
        _snwprintf(address_line, 192, L"This PC's address: %ls  ·  port %d", g.addresses, LINK_PORT);
        address_line[191] = 0;
        y = add_text(mx, y, mw, address_line, g.body, C_INK2);
        break;
    case PAGE_SOUND: {
        static wchar_t sound[320];
        const wchar_t *name = g.peer_name[0] ? g.peer_name : L"the other PC";
        if (g.status != NS_CONNECTED)
            _snwprintf(sound, 320, main_pc ? L"Connect a controlled PC first; its sound then plays here."
                                           : L"Once the main PC connects, this PC's sound plays on its speakers.");
        else if (g.audio_state == AUDIO_LOOPBACK || g.audio_state == AUDIO_STEREO_MIX)
            _snwprintf(sound, 320,
                       main_pc ? L"%ls's sound plays on this PC's speakers%ls. Its own speakers keep playing too; "
                                 L"turn them down there if you only want to hear it here."
                               : L"This PC's sound plays on %ls's speakers%ls.",
                       name, g.audio_state == AUDIO_STEREO_MIX ? L", recorded from its sound card's Stereo Mix" : L"");
        else if (g.audio_state == AUDIO_NONE)
            _snwprintf(sound, 320,
                       L"%ls cannot record what its speakers play. On Windows XP, turn on the Stereo Mix (or What U "
                       L"Hear) recording device in the Sounds and Audio Devices control panel; Bridger looks again "
                       L"every few seconds.",
                       main_pc ? name : L"This PC");
        else
            _snwprintf(sound, 320, L"Waiting for %ls to report its sound.", main_pc ? name : L"this PC");
        sound[319] = 0;
        y = add_text(mx, y, mw, sound, g.body, C_INK2);
        break;
    }
    default:
        y = add_text(mx, y, mw, L"Coming in a later version.", g.body, C_INK2);
    }
    SetRect(&g.module_rc, x0, my, x0 + width, y + S(28));
    ReleaseDC(g.wnd, lay_dc);
}

static void paint(HDC win)
{
    const Palette *p = g.p;
    RECT c;
    GetClientRect(g.wnd, &c);
    HDC dc = CreateCompatibleDC(win);
    HBITMAP bmp = CreateCompatibleBitmap(win, c.right, c.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    fill(dc, &c, p->ground);

    int fy = c.bottom - S(FOOTER);
    RECT r;
    SetRect(&r, S(SIDEBAR), 0, S(SIDEBAR) + 1, fy), fill(dc, &r, p->rule);
    SetRect(&r, 0, S(STATUS_H), S(SIDEBAR), S(STATUS_H) + 1), fill(dc, &r, p->rule);

    /* Status block: where input is and how the link is. */
    dot(dc, S(28), S(40), S(12), color(status_color()));
    SetRect(&r, S(52), S(30), S(SIDEBAR - 16), S(56));
    text(dc, g.line, r, g.status_font, p->ink, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SetRect(&r, S(52), S(56), S(SIDEBAR - 16), S(80));
    const wchar_t *who = g.status == NS_CONNECTED ? g.peer_name : g.cfg.role == ROLE_MAIN ? g.cfg.peer : L"";
    text(dc, who[0] ? who : g.cfg.role == ROLE_MAIN ? L"Main PC" : L"Controlled PC", r, g.body, p->ink2,
         DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    wchar_t rtt[32] = L"Response: \x2014";
    if (g.status == NS_CONNECTED && g.rtt >= 0) _snwprintf(rtt, 32, L"Response: %d ms", g.rtt);
    SetRect(&r, S(28), S(92), S(SIDEBAR - 16), S(116));
    text(dc, rtt, r, g.body, p->ink2, DT_SINGLELINE | DT_VCENTER);

    SetRect(&r, S(28), fy - S(72), S(SIDEBAR - 16), fy - S(46));
    text(dc, L"Bridger " WIDEN(BRIDGER_VERSION), r, g.small, p->ink3, DT_SINGLELINE | DT_VCENTER);

    /* Page */
    SetRect(&r, g.desc_rc.left, S(40), g.desc_rc.right, S(40) + S(40));
    text(dc, PAGES[g.page].title, r, g.title, p->ink, DT_SINGLELINE | DT_VCENTER);
    text(dc, PAGES[g.page].desc, g.desc_rc, g.body, p->ink2, DT_WORDBREAK);
    fill(dc, &g.module_rc, p->panel);
    for (int i = 0; i < g.nlabels; i++) {
        const Label *l = &g.labels[i];
        switch (l->kind) {
        case L_TEXT: text(dc, l->s, l->r, l->f, color(l->color), DT_WORDBREAK); break;
        case L_LINE:
            if (l->f == g.eyebrow) SetTextCharacterExtra(dc, S(11) * 14 / 100); /* Beamer's 0.14 em tracking */
            text(dc, l->s, l->r, l->f, color(l->color), DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            SetTextCharacterExtra(dc, 0);
            break;
        case L_DOT: dot(dc, l->r.left, l->r.top, l->r.right - l->r.left, color(l->color)); break;
        case L_WELL:
            round_rect(dc, &l->r, S(4), p->well, GetFocus() == l->edit ? p->signal : p->edge, S(1));
            break;
        case L_KEYCAP:
            round_rect(dc, &l->r, S(6), p->well, p->edge, S(1));
            text(dc, l->s, l->r, l->f, color(l->color), DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            break;
        }
    }

    /* Footer last: a page taller than the window slides under it. */
    /* ponytail: no scrolling; add it if pages outgrow a 1024x768 screen */
    SetRect(&r, 0, fy, c.right, c.bottom), fill(dc, &r, p->ground);
    SetRect(&r, 0, fy, c.right, fy + 1), fill(dc, &r, p->rule);
    SetRect(&r, S(28), fy + S(18), c.right - S(260), c.bottom - S(18));
    text(dc, L"Changes apply as you make them. Closing this window keeps Bridger running in the tray.", r,
         g.body, p->ink2, DT_WORDBREAK);

    BitBlt(win, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static void refresh(void)
{
    status_text();
    layout();
    InvalidateRect(g.wnd, NULL, FALSE);
    for (int i = 0; i < NPAGES; i++) InvalidateRect(g.nav[i], NULL, FALSE);
    for (int i = 0; i < 4; i++) InvalidateRect(g.edge[i], NULL, FALSE);
    for (int i = 0; i < 2; i++) InvalidateRect(g.role[i], NULL, FALSE);
}

static void set_page(int page)
{
    if (page < 0 || page >= NPAGES) return;
    g.page = page;
    if (page == PAGE_CONNECTION) this_pc_addresses();
    refresh();
}

/* ---- applying settings ---- */

static void sync_input(void) { input_main_set(g.status == NS_CONNECTED && g.cfg.role == ROLE_MAIN, g.cfg.edge); }

static void apply_field(HWND edit)
{
    wchar_t v[128];
    GetWindowTextW(edit, v, 128);
    if (edit == g.pw) {
        if (!v[0]) return;
        char utf8[400];
        WideCharToMultiByte(CP_UTF8, 0, v, -1, utf8, sizeof utf8, NULL, NULL);
        SetCursor(LoadCursor(NULL, IDC_WAIT));
        EnterCriticalSection(&g.cs);
        link_key_from_password(utf8, g.cfg.key);
        g.cfg.has_key = TRUE;
        LeaveCriticalSection(&g.cs);
        SecureZeroMemory(utf8, sizeof utf8);
        SecureZeroMemory(v, sizeof v);
        SetWindowTextW(edit, L"");
    } else {
        /* Only what a host name or address can hold. */
        wchar_t *s = v, *e;
        while (*s == L' ') s++;
        for (e = s; *e && *e != L' '; e++)
            if (!iswalnum(*e) && *e != L'.' && *e != L'-') return;
        *e = 0;
        if (lstrcmpW(s, g.cfg.peer) == 0) return;
        EnterCriticalSection(&g.cs);
        lstrcpynW(g.cfg.peer, s, 128);
        LeaveCriticalSection(&g.cs);
        SetWindowTextW(edit, g.cfg.peer);
    }
    config_save();
    net_restart();
    refresh();
}

static void set_role(int role)
{
    if (role == g.cfg.role) return;
    EnterCriticalSection(&g.cs);
    g.cfg.role = role;
    LeaveCriticalSection(&g.cs);
    input_release_all();
    config_save();
    net_restart();
    sync_input();
    refresh();
}

/* ---- tray and window ---- */

static void show_window(void)
{
    ShowWindow(g.wnd, IsIconic(g.wnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.wnd);
}

static void tray_add(void)
{
    g.tray.cbSize = NOTIFYICONDATAW_V2_SIZE; /* the size XP understands */
    g.tray.hWnd = g.wnd;
    g.tray.uID = 1;
    g.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.tray.uCallbackMessage = WM_TRAY;
    g.tray.hIcon = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON), 0);
    lstrcpynW(g.tray.szTip, L"Bridger", 64);
    Shell_NotifyIconW(NIM_ADD, &g.tray);
}

static void tray_menu(void)
{
    POINT pt;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Open Bridger");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_TRAY_QUIT, L"Quit");
    SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);
    GetCursorPos(&pt);
    SetForegroundWindow(g.wnd); /* so the menu closes when clicking elsewhere */
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.wnd, NULL);
    PostMessageW(g.wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void quit(void)
{
    input_release_all();
    Shell_NotifyIconW(NIM_DELETE, &g.tray);
    DestroyWindow(g.wnd);
}

/* ---- clipboard: the PC giving up input sends its text, so a copy there pastes here ---- */

static DWORD clip_seq; /* the clipboard as Bridger last sent or set it: only a newer copy travels */

static void clipboard_send(void)
{
    if (GetClipboardSequenceNumber() == clip_seq || !OpenClipboard(g.wnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = h ? GlobalLock(h) : NULL;
    if (w) {
        static unsigned char msg[LINK_MAX]; /* UI thread only */
        /* ponytail: text over 64 KB stays behind; chunk it if that ever matters */
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, (char *)msg + 1, LINK_MAX - 1, NULL, NULL);
        GlobalUnlock(h);
        msg[0] = MSG_CLIPBOARD;
        if (n > 1) net_send(msg, n); /* n counts the NUL, which the type byte replaces */
    }
    CloseClipboard();
    clip_seq = GetClipboardSequenceNumber();
}

static void clipboard_set(const unsigned char *s, int n)
{
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)s, n, NULL, 0);
    if (wn <= 0) return; /* not valid UTF-8: not from Bridger */
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (wn + 1) * sizeof(wchar_t));
    if (!h) return;
    wchar_t *w = GlobalLock(h);
    MultiByteToWideChar(CP_UTF8, 0, (const char *)s, n, w, wn);
    w[wn] = 0;
    GlobalUnlock(h);
    BOOL open = FALSE;
    for (int i = 0; i < 10 && !(open = OpenClipboard(g.wnd)); i++) Sleep(20); /* another app may hold it a moment */
    if (!open) {
        GlobalFree(h);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    CloseClipboard();
    clip_seq = GetClipboardSequenceNumber();
}

/* The capture thread's messages go out as they are; its state also shows on this PC's Sound page. */
static void audio_out(const unsigned char *msg, int len)
{
    if (msg[0] == MSG_AUDIO_STATE) PostMessageW(g.wnd, WM_AUDIO_STATE, msg[1], 0);
    net_send(msg, len);
}

static void on_net_msg(unsigned char *msg, int len)
{
    if (msg[0] == MSG_CLIPBOARD && len > 1) {
        clipboard_set(msg + 1, len - 1);
    } else if (msg[0] == MSG_PONG && len == 5) {
        DWORD sent = msg[1] | msg[2] << 8 | msg[3] << 16 | (DWORD)msg[4] << 24;
        g.rtt = (int)(GetTickCount() - sent);
        RECT r = {0, 0, S(SIDEBAR), S(STATUS_H)};
        InvalidateRect(g.wnd, &r, FALSE);
    } else if (g.cfg.role == ROLE_MAIN) {
        if (msg[0] == MSG_LEAVE && len == 4) input_main_leave(msg[1] ? msg[2] | msg[3] << 8 : -1);
        else if (msg[0] == MSG_AUDIO) audio_play(msg, len);
        else if (msg[0] == MSG_AUDIO_STATE && len == 2 && msg[1] <= AUDIO_NONE && msg[1] != g.audio_state)
            g.audio_state = msg[1], refresh();
    } else {
        unsigned char out[8];
        BOOL was = input_controlled_active();
        int n = input_replay(msg, len, out);
        if (was && !input_controlled_active()) clipboard_send(); /* ahead of LEAVE, so it is there on arrival */
        if (n) net_send(out, n);
        if (was != input_controlled_active()) refresh();
    }
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        g.wnd = w;
        for (int i = 0; i < NPAGES; i++) g.nav[i] = add_button(PAGES[i].name, ID_NAV0 + i, K_NAV);
        g.setup = add_button(L"Set up the connection", ID_SETUP, K_OUTLINE);
        g.role[0] = add_button(L"Main PC", ID_ROLE0, K_SEGMENT);
        g.role[1] = add_button(L"Controlled PC", ID_ROLE1, K_SEGMENT);
        for (int i = 0; i < 4; i++) {
            static const wchar_t *names[] = {L"Left", L"Right", L"Top", L"Bottom"};
            g.edge[i] = add_button(names[i], ID_EDGE0 + i, K_SEGMENT);
        }
        g.pw = add_field(L"Shared password", ID_PASSWORD_LABEL, ID_PASSWORD, ES_PASSWORD, &g.pw_label);
        g.peer = add_field(L"Controlled PC's address", ID_PEER_LABEL, ID_PEER, 0, &g.peer_label);
        SetWindowTextW(g.peer, g.cfg.peer);
        g.site = add_button(L"Bridger on GitHub", ID_SITE, K_LINK);
        g.quit = add_button(L"Quit Bridger", ID_QUIT, K_OUTLINE);
        apply_theme();
        tray_add();
        SetTimer(w, TIMER_PING, 1000, NULL);
        return 0;
    case WM_SIZE:
        layout();
        InvalidateRect(w, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(760);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(640);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        paint(BeginPaint(w, &ps));
        EndPaint(w, &ps);
        return 0;
    }
    case WM_DRAWITEM:
        draw_control((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, g.p->ink);
        SetBkColor((HDC)wp, g.p->well);
        return (LRESULT)g.well_brush;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= ID_NAV0 && id < ID_NAV0 + NPAGES) set_page(id - ID_NAV0);
        else if (id == ID_SETUP) set_page(PAGE_CONNECTION);
        else if (id == ID_ROLE0 || id == ID_ROLE1) set_role(id - ID_ROLE0);
        else if (id >= ID_EDGE0 && id <= ID_EDGE3) {
            g.cfg.edge = id - ID_EDGE0;
            config_save();
            sync_input();
            refresh();
        } else if ((id == ID_PASSWORD || id == ID_PEER) && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
            if (code == EN_KILLFOCUS) apply_field((HWND)lp);
            InvalidateRect(w, &g.module_rc, FALSE);
        } else if (id == ID_QUIT || id == ID_TRAY_QUIT) quit();
        else if (id == ID_TRAY_OPEN) show_window();
        else if (id == ID_SITE) ShellExecuteW(NULL, L"open", SITE_URL, NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }
    case WM_TIMER:
        if (g.status == NS_CONNECTED) {
            DWORD t = GetTickCount();
            unsigned char ping[5] = {MSG_PING, (BYTE)t, (BYTE)(t >> 8), (BYTE)(t >> 16), (BYTE)(t >> 24)};
            net_send(ping, 5);
        }
        return 0;
    case WM_NET_STATUS:
        if ((int)wp != g.status) {
            if (wp != NS_CONNECTED) {
                g.rtt = -1, g.remote = FALSE, g.audio_state = AUDIO_UNKNOWN;
                input_release_all();
                audio_capture_stop();
                audio_play_stop();
            } else if (g.cfg.role == ROLE_CONTROLLED) {
                audio_capture_start(audio_out);
            }
            g.status = (int)wp;
            sync_input();
            refresh();
        }
        return 0;
    case WM_NET_MSG:
        on_net_msg((unsigned char *)lp, (int)wp);
        free((void *)lp);
        return 0;
    case WM_INPUT_SEND: {
        unsigned char msg[8];
        int n = input_pack((UINT)wp, lp, msg);
        if (wp == MSG_ENTER) clipboard_send();
        if (n) net_send(msg, n);
        return 0;
    }
    case WM_AUDIO_STATE:
        g.audio_state = (int)wp;
        refresh();
        return 0;
    case WM_INPUT_STATE:
        g.remote = (BOOL)wp;
        refresh();
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) show_window();
        else if (lp == WM_RBUTTONUP) tray_menu();
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpW((const wchar_t *)lp, L"ImmersiveColorSet") == 0) apply_theme(), refresh();
        return 0;
    case WM_CLOSE: /* closing keeps Bridger in the tray, as Beamer does */
        ShowWindow(w, SW_HIDE);
        return 0;
    case WM_BRIDGER_SHOW:
        show_window();
        return 0;
    case WM_BRIDGER_QUIT:
        quit();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    if (m == g.taskbar_created && m) { /* Explorer restarted: the tray icon is gone */
        g.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        Shell_NotifyIconW(NIM_ADD, &g.tray);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    CreateMutexW(NULL, FALSE, L"Local\\Bridger");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(APP_CLASS, NULL);
        if (other) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, WM_BRIDGER_SHOW, 0, 0);
        }
        return 0;
    }

    g.inst = inst;
    /* Sharp text at 125%/150%; Vista added this, so XP takes the bitmap-scaled path it never had. */
    FARPROC dpi_aware = GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware");
    if (dpi_aware) dpi_aware();
    HDC screen = GetDC(NULL);
    g.dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);
    load_fonts();
    g.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    InitializeCriticalSection(&g.cs);
    g.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g.conn = g.listener = INVALID_SOCKET;
    g.rtt = -1;
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD name_len = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(name, &name_len);
    WideCharToMultiByte(CP_UTF8, 0, name, -1, g.my_name, sizeof g.my_name, NULL, NULL);
    config_load();
    this_pc_addresses();

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = APP_CLASS;
    RegisterClassExW(&wc);

    /* WS_EX_CONTROLPARENT + IsDialogMessage give Tab/Shift+Tab between the controls. */
    CreateWindowExW(WS_EX_CONTROLPARENT, APP_CLASS, L"Bridger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    CW_USEDEFAULT, CW_USEDEFAULT, S(1000), S(800), NULL, NULL, inst, NULL);
    input_main_start(g.wnd);
    sync_input();
    CloseHandle(CreateThread(NULL, 0, net_thread, NULL, 0, NULL));
    /* Started at sign-in with /tray: stay in the tray until asked. */
    if (!wcsstr(cmd, L"/tray")) ShowWindow(g.wnd, show);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        int nav = -1;
        for (int i = 0; i < NPAGES; i++)
            if (msg.hwnd == g.nav[i]) nav = i;
        if (nav >= 0 && msg.message == WM_KEYDOWN && (msg.wParam == VK_UP || msg.wParam == VK_DOWN)) {
            int next = nav + (msg.wParam == VK_DOWN ? 1 : -1); /* arrows move between pages, like a list */
            if (next >= 0 && next < NPAGES) set_page(next), SetFocus(g.nav[next]);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && (msg.hwnd == g.pw || msg.hwnd == g.peer)) {
            apply_field(msg.hwnd);
            continue;
        }
        if (IsDialogMessageW(g.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
