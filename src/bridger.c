/* Bridger: one keyboard, mouse and sound for PCs from Windows XP to 11.
 * M0: the settings window in Beamer's Vernier look, the tray icon and a single instance.
 * Everything is plain Win32/GDI so the same exe runs on XP SP3 and on Windows 11. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include "resource.h"
#include "version.h"

#define WIDEN2(s) L##s
#define WIDEN(s) WIDEN2(s)
#define APP_CLASS L"BridgerWnd"
#define SITE_URL L"https://github.com/limburatorul/bridger"

#define WM_TRAY (WM_APP + 1)
#define WM_BRIDGER_QUIT (WM_APP + 2) /* sent by the installer before it overwrites the exe */
#define WM_BRIDGER_SHOW (WM_APP + 3) /* sent by a second instance */

enum { ID_TRAY_OPEN = 1, ID_TRAY_QUIT, ID_NAV0 = 100, ID_QUIT = 200, ID_SITE };
enum { K_NAV, K_OUTLINE, K_LINK };
#define NO_FILL ((COLORREF)0xFFFFFFFF)

/* Vernier palette, copied from Beamer's tokens.py (GPL-3). */
typedef struct { COLORREF ground, panel, well, rule, edge, ink, ink2, ink3, signal, off; } Palette;
static const Palette DARK = {
    RGB(0x0e, 0x0f, 0x0d), RGB(0x16, 0x17, 0x14), RGB(0x1f, 0x20, 0x1c), RGB(0x2a, 0x2b, 0x26),
    RGB(0x6e, 0x6f, 0x66), RGB(0xef, 0xee, 0xe6), RGB(0xb3, 0xb2, 0xa7), RGB(0x8f, 0x8e, 0x84),
    RGB(0x5f, 0xd4, 0xf4), RGB(0x4a, 0x4b, 0x44)};
static const Palette LIGHT = {
    RGB(0xf1, 0xf0, 0xea), RGB(0xfb, 0xfa, 0xf6), RGB(0xe5, 0xe4, 0xdc), RGB(0xdc, 0xdb, 0xd3),
    RGB(0x7f, 0x7e, 0x74), RGB(0x1b, 0x1c, 0x18), RGB(0x46, 0x46, 0x3f), RGB(0x66, 0x65, 0x5c),
    RGB(0x00, 0x6a, 0x88), RGB(0xae, 0xad, 0xa3)};

static const struct { const wchar_t *name, *title, *desc, *eyebrow, *body; } PAGES[] = {
    {L"Prezentare", L"PREZENTARE",
     L"Calculatoarele împerecheate cu acest PC, unde e controlul acum și ce folosești zilnic. "
     L"Împerechează aici o mașină mai întâi.",
     L"MAȘINI", L"Încă nu e nicio mașină împerecheată."},
    {L"Trecere", L"TRECERE", L"Prin ce margine trece cursorul spre fiecare mașină și cum se întoarce.",
     L"MARGINI", L"Disponibil după împerechere."},
    {L"Tastatură", L"TASTATURĂ",
     L"Comutare fără mouse: dublu-tap pe Right Ctrl. Scăpare de urgență: Ctrl+Alt+Pause.",
     L"COMUTARE", L"Tastele se pot schimba după împerechere."},
    {L"Sunet", L"SUNET", L"Sunetul mașinii controlate, redat pe boxele acestui PC.", L"SUNET",
     L"Încă nu e nicio sursă de sunet."},
    {L"Fișiere", L"FIȘIERE", L"Copiază fișiere pe un PC, comută și dă Ctrl+V pe celălalt.", L"FIȘIERE",
     L"Niciun transfer încă."},
    {L"Conexiune", L"CONEXIUNE", L"Parola comună, portul și adresa acestui PC.", L"CONEXIUNE",
     L"Nicio conexiune configurată."},
};
#define NPAGES ((int)(sizeof PAGES / sizeof PAGES[0]))

static struct {
    HINSTANCE inst;
    HWND wnd, hover, nav[NPAGES], quit, site;
    WNDPROC button_proc;
    NOTIFYICONDATAW tray;
    UINT taskbar_created;
    const Palette *p;
    int dpi, page;
    HFONT title, body, nav_font, nav_sel, status, eyebrow, button, small;
    RECT desc_rc, module_rc;
} g;

static int S(int px) { return MulDiv(px, g.dpi, 96); }

static const int SIDEBAR = 280, FOOTER = 96, NAV_H = 52, STATUS_H = 132;

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
    g.status = mkfont(L"Hanken Grotesk SemiBold", 15, FW_NORMAL);
    g.eyebrow = mkfont(L"Hanken Grotesk SemiBold", 11, FW_NORMAL);
    g.button = mkfont(L"Hanken Grotesk SemiBold", 14, FW_NORMAL);
    g.small = mkfont(L"Hanken Grotesk", 13, FW_NORMAL);
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
    case 0: /* screen and a second screen */
        Rectangle(dc, P(2, 4), P(17, 15));
        MoveToEx(dc, P(9, 15), NULL), LineTo(dc, P(9, 19));
        MoveToEx(dc, P(5, 19), NULL), LineTo(dc, P(13, 19));
        Rectangle(dc, P(14, 10), P(23, 20));
        break;
    case 1: /* arrows both ways */
        MoveToEx(dc, P(3, 8), NULL), LineTo(dc, P(21, 8)), LineTo(dc, P(17, 4));
        MoveToEx(dc, P(21, 16), NULL), LineTo(dc, P(3, 16)), LineTo(dc, P(7, 20));
        break;
    case 2: /* keyboard */
        RoundRect(dc, P(1, 5), P(23, 19), S(4), S(4));
        for (int i = 0; i < 5; i++) Rectangle(dc, P(5 + 3 * i, 9), P(6 + 3 * i, 10));
        MoveToEx(dc, P(7, 15), NULL), LineTo(dc, P(17, 15));
        break;
    case 3: /* speaker */
        MoveToEx(dc, P(3, 9), NULL), LineTo(dc, P(7, 9)), LineTo(dc, P(12, 4)), LineTo(dc, P(12, 20)),
            LineTo(dc, P(7, 15)), LineTo(dc, P(3, 15)), LineTo(dc, P(3, 9));
        Arc(dc, P(10, 7), P(20, 17), P(16, 18), P(16, 6));
        Arc(dc, P(11, 3), P(24, 21), P(18, 22), P(18, 2));
        break;
    case 4: /* folder */
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

/* ---- owner-drawn buttons: real BUTTON controls, so Tab, Space and screen readers work ---- */

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

static void draw_button(const DRAWITEMSTRUCT *d)
{
    const Palette *p = g.p;
    int kind = (int)GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA);
    BOOL hover = d->hwndItem == g.hover || (d->itemState & ODS_SELECTED);
    BOOL focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
    RECT r = d->rcItem;
    int w = r.right, h = r.bottom;
    wchar_t label[64];
    GetWindowTextW(d->hwndItem, label, 64);

    /* Draw off-screen so hover changes do not flicker. */
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ old = SelectObject(dc, bmp);
    fill(dc, &r, p->ground);

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
    } else if (kind == K_OUTLINE) {
        RECT b = {0, 0, w, h};
        round_rect(dc, &b, S(5), hover ? p->well : p->ground, hover ? p->ink2 : p->edge, S(1));
        text(dc, label, r, g.button, p->ink, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    } else {
        text(dc, label, r, g.small, hover ? p->ink : p->ink3, DT_SINGLELINE | DT_VCENTER);
    }
    if (focus) {
        RECT f = {S(1), S(1), w - S(1), h - S(1)};
        round_rect(dc, &f, S(6), NO_FILL, p->signal, S(2));
    }
    BitBlt(d->hDC, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* ---- layout and painting of the window itself ---- */

static void layout(void)
{
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
    HDC dc = GetDC(g.wnd);
    int dh = text_height(dc, PAGES[g.page].desc, width, g.body);
    SetRect(&g.desc_rc, x0, S(40) + S(52), x0 + width, S(40) + S(52) + dh);
    int my = g.desc_rc.bottom + S(28);
    int bh = text_height(dc, PAGES[g.page].body, width - S(64), g.body);
    SetRect(&g.module_rc, x0, my, x0 + width, my + S(28) + S(24) + S(16) + bh + S(28));
    ReleaseDC(g.wnd, dc);
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
    SetRect(&r, 0, fy, c.right, fy + 1), fill(dc, &r, p->rule);
    SetRect(&r, 0, S(STATUS_H), S(SIDEBAR), S(STATUS_H) + 1), fill(dc, &r, p->rule);

    /* Status block: where input is. M0 has no link yet. */
    HBRUSH dot = CreateSolidBrush(p->off);
    HGDIOBJ ob = SelectObject(dc, dot), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, S(28), S(40), S(28) + S(12), S(40) + S(12));
    SelectObject(dc, ob), SelectObject(dc, op), DeleteObject(dot);
    SetRect(&r, S(52), S(30), S(SIDEBAR - 16), S(56));
    text(dc, L"Neconectat", r, g.status, p->ink, DT_SINGLELINE | DT_VCENTER);
    SetRect(&r, S(52), S(56), S(SIDEBAR - 16), S(80));
    text(dc, L"Nicio mașină împerecheată", r, g.body, p->ink2, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SetRect(&r, S(28), S(92), S(SIDEBAR - 16), S(116));
    text(dc, L"Răspuns: —", r, g.body, p->ink2, DT_SINGLELINE | DT_VCENTER);

    SetRect(&r, S(28), fy - S(72), S(SIDEBAR - 16), fy - S(46));
    text(dc, L"Bridger " WIDEN(BRIDGER_VERSION), r, g.small, p->ink3, DT_SINGLELINE | DT_VCENTER);
    SetRect(&r, S(28), fy + S(18), c.right - S(260), c.bottom - S(18));
    text(dc, L"Modificările se aplică imediat. Închiderea ferestrei lasă Bridger să ruleze în tray.", r,
         g.body, p->ink2, DT_WORDBREAK);

    /* Page */
    SetRect(&r, g.desc_rc.left, S(40), g.desc_rc.right, S(40) + S(40));
    text(dc, PAGES[g.page].title, r, g.title, p->ink, DT_SINGLELINE | DT_VCENTER);
    text(dc, PAGES[g.page].desc, g.desc_rc, g.body, p->ink2, DT_WORDBREAK);
    fill(dc, &g.module_rc, p->panel);
    SetRect(&r, g.module_rc.left + S(32), g.module_rc.top + S(28), g.module_rc.right - S(32),
            g.module_rc.top + S(28) + S(24));
    SetTextCharacterExtra(dc, S(11) * 14 / 100); /* Beamer's eyebrow tracking, 0.14 em */
    text(dc, PAGES[g.page].eyebrow, r, g.eyebrow, p->ink3, DT_SINGLELINE | DT_VCENTER);
    SetTextCharacterExtra(dc, 0);
    SetRect(&r, r.left, r.bottom + S(16), r.right, g.module_rc.bottom);
    text(dc, PAGES[g.page].body, r, g.body, p->ink2, DT_WORDBREAK);

    BitBlt(win, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static void set_page(int page)
{
    if (page < 0 || page >= NPAGES) return;
    g.page = page;
    layout();
    InvalidateRect(g.wnd, NULL, FALSE);
    for (int i = 0; i < NPAGES; i++) InvalidateRect(g.nav[i], NULL, FALSE);
}

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
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Deschide Bridger");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_TRAY_QUIT, L"Ieșire");
    SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);
    GetCursorPos(&pt);
    SetForegroundWindow(g.wnd); /* so the menu closes when clicking elsewhere */
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.wnd, NULL);
    PostMessageW(g.wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void quit(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g.tray);
    DestroyWindow(g.wnd);
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        g.wnd = w;
        for (int i = 0; i < NPAGES; i++) g.nav[i] = add_button(PAGES[i].name, ID_NAV0 + i, K_NAV);
        g.site = add_button(L"Pagina Bridger pe GitHub", ID_SITE, K_LINK);
        g.quit = add_button(L"Ieșire din Bridger", ID_QUIT, K_OUTLINE);
        apply_theme();
        tray_add();
        return 0;
    case WM_SIZE:
        layout();
        InvalidateRect(w, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(720);
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
        draw_button((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= ID_NAV0 && id < ID_NAV0 + NPAGES) set_page(id - ID_NAV0);
        else if (id == ID_QUIT || id == ID_TRAY_QUIT) quit();
        else if (id == ID_TRAY_OPEN) show_window();
        else if (id == ID_SITE) ShellExecuteW(NULL, L"open", SITE_URL, NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) show_window();
        else if (lp == WM_RBUTTONUP) tray_menu();
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpW((const wchar_t *)lp, L"ImmersiveColorSet") == 0) apply_theme();
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

    /* WS_EX_CONTROLPARENT + IsDialogMessage give Tab/Shift+Tab between the buttons. */
    CreateWindowExW(WS_EX_CONTROLPARENT, APP_CLASS, L"Bridger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    CW_USEDEFAULT, CW_USEDEFAULT, S(1000), S(700), NULL, NULL, inst, NULL);
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
        if (IsDialogMessageW(g.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
