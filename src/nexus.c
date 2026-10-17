/* Nexus: one keyboard, mouse and sound for PCs from Windows XP to 11.
 * The settings window in Beamer's Vernier look, the tray icon, the config and the network thread.
 * Everything is plain Win32/GDI so the same exe runs on XP SP3 and on Windows 11. */

#include "link.h" /* winsock2.h has to come before windows.h */
#include <shellapi.h>
#include <shlobj.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "audio.h"
#include "files.h"
#include "update.h"
#include "input.h"
#include "log.h"
#include "resource.h"
#include <iphlpapi.h>
#include "version.h"

#define WIDEN2(s) L##s
#define WIDEN(s) WIDEN2(s)
#define SITE_URL L"https://github.com/limburatorul/nexus"

#define WM_TRAY (WM_APP + 1)
#define WM_NEXUS_QUIT (WM_APP + 2) /* sent by the installer before it overwrites the exe */
#define WM_NEXUS_SHOW (WM_APP + 3) /* sent by a second instance */
#define WM_NET_STATUS (WM_APP + 4)   /* wParam: NS_*, lParam: which PC */
#define WM_NET_MSG (WM_APP + 5)      /* wParam: length, lParam: malloc'd PC number + message, freed here */
#define WM_AUDIO_STATE (WM_APP + 6)  /* wParam: AUDIO_* of this controlled PC */
#define WM_TEST (WM_APP + 7)         /* wParam: command, lParam: reply buffer (test mode) */
#define TEST_LINK_PORT 24810         /* a test profile beside the real one, see "/test" */
#define TEST_CONTROL_PORT 24811
#define DISCOVERY_PORT 24802
#define TEST_DISCOVERY_PORT 24812
#define WM_FOUND (WM_APP + 8)        /* lParam: malloc'd Found, freed here */
#define MAX_FOUND 4
#define MAX_PEERS 4 /* controlled PCs a main PC works with */
#define THIS_PC MAX_PEERS /* this PC, where a PC number goes */
#define MAP_W 5 /* the map: this PC in the middle of a 5 x 3 grid */
#define MAP_H 3
#define TIMER_PING 1
#define TIMER_BEAT 2 /* 250 ms: the heartbeat while input is across, the lock check */
#define WM_SCROLL_TO (WM_APP + 9) /* a control got the focus: scroll it into view */

enum { ID_TRAY_OPEN = 1, ID_TRAY_QUIT, ID_TRAY_SEND, ID_TRAY_PAUSE, ID_TRAY_LOGS, ID_TRAY_ABOUT, ID_TRAY_WAKE0,
       ID_TRAY_WAKE3 = ID_TRAY_WAKE0 + MAX_PEERS - 1,
       ID_NAV0 = 100, ID_QUIT = 200, ID_SITE, ID_SETUP, ID_ROLE0, ID_ROLE1,
       ID_PASSWORD, ID_PASSWORD_LABEL, ID_UPDATE, ID_FOUND0, ID_FOUND3 = ID_FOUND0 + MAX_FOUND - 1,
       ID_ADDR0, ID_ADDR3 = ID_ADDR0 + MAX_PEERS - 1, ID_ADDR_LABEL0, ID_ADDR_LABEL3 = ID_ADDR_LABEL0 + MAX_PEERS - 1,
       ID_TILE0, ID_TILE3 = ID_TILE0 + MAX_PEERS - 1,
       ID_RES0, ID_RES3 = ID_RES0 + 3, /* edge resistance: touch, light, normal, firm */
       ID_PAUSE, ID_FULLSCREEN, ID_HIDE_POINTER, ID_LOGS, ID_SWITCH_KEY, ID_STYLE_TAP, ID_STYLE_HOLD,
       ID_JUMP0, ID_JUMP3 = ID_JUMP0 + MAX_PEERS - 1, ID_WAKE0, ID_WAKE3 = ID_WAKE0 + MAX_PEERS - 1 };
enum { K_NAV, K_OUTLINE, K_LINK, K_SEGMENT, K_TILE, K_TOGGLE };
static const int RESISTANCE[] = {0, 60, DEFAULT_RESISTANCE, 240}; /* px, for ID_RES0.. */
enum { ROLE_MAIN, ROLE_CONTROLLED };
typedef struct { char ip[16]; wchar_t name[32]; int role; DWORD seen; } Found;
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
    {L"Overview", L"Overview", L"The PCs this one works with, where input is right now, and how the links are doing.",
     L"Machines"},
    {L"Crossing", L"Crossing",
     L"Where the PCs stand around this screen. Push the pointer through an edge to cross to the PC beside it, "
     L"and on from there to the next one.",
     L"Map"},
    {L"Keyboard", L"Keyboard", L"Switch without the mouse, and a way home that always works.", L"Shortcuts"},
    {L"Sound", L"Sound", L"The controlled PC's sound, played on this PC's speakers.", L"Sound"},
    {L"Files", L"Files", L"Copy files on one PC, switch, and paste them on another.", L"Files"},
    {L"Connection", L"Connection", L"This PC's role, the password all the PCs share, and where to find the others.",
     L"This PC"},
};
#define NPAGES ((int)(sizeof PAGES / sizeof PAGES[0]))

enum { NS_SETUP, NS_WAITING, NS_CONNECTING, NS_UNREACHABLE, NS_PASSWORD, NS_VERSION, NS_ROLE, NS_PEER, NS_PORT,
       NS_CONNECTED };
static const wchar_t *STATUS_WORD[] = {L"Not set up",     L"Waiting",          L"Connecting", L"Not reachable",
                                       L"Wrong password", L"Version mismatch", L"Same role",  L"Not Nexus",
                                       L"Port in use",    L"Connected"};
static const wchar_t *STATUS_NOTE[] = {
    L"Choose this PC's role and the shared password on the Connection page.",
    L"Waiting for the main PC to connect.",
    L"Trying to reach the controlled PC.",
    L"The controlled PC does not answer. Check that Nexus runs there and that the address is right.",
    L"The two PCs have different passwords. Type the same one on both.",
    L"The other PC runs a different Nexus. Install the same version on both.",
    L"Both PCs have the same role. Make one the main PC and the other the controlled PC.",
    L"Something other than Nexus answered on port 24800.",
    L"Another program uses port 24800, so the main PC cannot connect to this one.",
    L""};
static const wchar_t *EDGE_NAMES[] = {L"left", L"right", L"top", L"bottom"};
static const int DIR_X[] = {-1, 1, 0, 0}, DIR_Y[] = {0, 0, -1, 1}; /* a step through each edge */

/* A controlled PC: where it is (config), its link (network threads) and what the UI shows of it. */
typedef struct {
    wchar_t addr[128]; /* empty: a free slot */
    int x, y;          /* its cell on the map, this PC at 0,0 */
    SOCKET conn;       /* under g.cs */
    CRITICAL_SECTION send_cs; /* one sender at a time, without holding up the other PCs */
    Link link;
    BOOL link_up;      /* link and link_up under send_cs */
    HANDLE wake;
    wchar_t name[64];  /* from the handshake, under g.cs */
    char version[16];  /* "" from a Nexus too old to say */
    BOOL secret;       /* forward secret (both ends 0.5+) */
    volatile DWORD heard; /* when its network thread last read a frame */
    int status, rtt, audio_state; /* UI thread */
    BYTE mac[6];       /* for Wake-on-LAN, learnt after a connection; all zero: unknown */
    WORD jump;         /* the chord that goes straight there: modifiers << 8 | key */
} Peer;

typedef struct { int kind; RECT r; const wchar_t *s; HFONT f; int color; HWND edit; } Label;
enum { L_TEXT, L_LINE, L_DOT, L_WELL, L_KEYCAP, L_CELL, L_HOME }; /* L_CELL, L_HOME: the map */

static struct {
    HINSTANCE inst;
    HWND found_btn[MAX_FOUND];
    HWND wnd, hover, nav[NPAGES], quit, site, update, setup, role[2], pw, pw_label;
    HWND addr[MAX_PEERS], addr_label[MAX_PEERS], tile[MAX_PEERS];
    HWND res[4], pause, fullscreen, hide_pointer, logs, switch_key, style[2], jump[MAX_PEERS], wake[MAX_PEERS];
    int scroll, content_h; /* the page column scrolls: how far, and how tall the page is */
    int recording;         /* the Keyboard page waits for a key: 0, 1 the switch key, 2 + n PC n's jump key */
    HICON icon_here, icon_away, icon_paused;
    ClipIn clip_in;        /* the clipboard in pieces from the PC giving up input (0.5) */
    RECT map_rc; /* the map on the Crossing page */
    int drag;    /* the tile being dragged, -1 for none */
    POINT grab;  /* where in that tile it was grabbed */
    WNDPROC button_proc;
    NOTIFYICONDATAW tray;
    UINT taskbar_created;
    const Palette *p;
    HBRUSH well_brush;
    int dpi, page, body_h;
    HFONT title, body, nav_font, nav_sel, status_font, eyebrow, button, small, mono;
    RECT desc_rc, module_rc;
    Label labels[64];
    int nlabels;
    wchar_t ini[MAX_PATH], addresses[128], line[256], note[256];

    /* config: written by the UI thread, read by the network threads under cs */
    struct {
        int role;
        unsigned char key[32];
        BOOL has_key;
        int resistance;               /* main PC: px of push before an edge gives */
        BOOL fullscreen_hold, paused; /* main PC */
        BOOL hide_pointer;            /* controlled PC: blank its pointer while input is elsewhere */
        BYTE switch_vk;               /* main PC */
        BOOL switch_hold;
    } cfg;
    Peer peer[MAX_PEERS]; /* a controlled PC uses peer[0] for its main PC */

    /* network */
    CRITICAL_SECTION cs;
    SOCKET listener;
    int gen;
    char my_name[64];
    wchar_t my_name_w[64];
    BOOL test; /* started with /test: own profile and ports, no hooks, driven over TEST_CONTROL_PORT */
    int port, discovery_port;
    SOCKET discovery;
    Found found[MAX_FOUND]; /* other Nexus PCs answering on this network */
    unsigned long local_ip[4], broadcast[4];
    const wchar_t *app_class;

    /* UI view of the links */
    BOOL remote; /* main: input is on a controlled PC */
    int target;  /* main: the PC with input, -1 while it is here */
    int last;    /* main: the PC input went to last, for the shortcut and for sound */
    wchar_t files_note[256]; /* the latest transfer, for the Files page */
    char update_version[16]; /* a newer Nexus downloaded and ready to install */
    wchar_t update_note[256];
    BOOL installing; /* the update goes to the controlled PC first, then this one installs */
} g;

static int S(int px) { return MulDiv(px, g.dpi, 96); }
static void redraw(const RECT *r);
static void test_command(const char *cmd, char *out);
static void move_peer(int pc, int x, int y);
static void go_home(void);
static int cell_w, cell_h; /* a map cell, set by layout() */

static const int SIDEBAR = 280, FOOTER = 96, NAV_H = 52, STATUS_H = 132;

/* ---- config ---- */

static const wchar_t *PEER_KEY[MAX_PEERS] = {L"peer", L"peer2", L"peer3", L"peer4"};
static const wchar_t *PLACE_KEY[MAX_PEERS] = {L"place", L"place2", L"place3", L"place4"};
static const wchar_t *MAC_KEY[MAX_PEERS] = {L"mac", L"mac2", L"mac3", L"mac4"};
static const wchar_t *JUMP_KEY[MAX_PEERS] = {L"jump", L"jump2", L"jump3", L"jump4"};

/* ---- the map: controlled PCs on a grid around this one ---- */

static BOOL on_map(int x, int y) { return x >= -MAP_W / 2 && x <= MAP_W / 2 && y >= -MAP_H / 2 && y <= MAP_H / 2; }

/* Who stands in a cell: a PC number, THIS_PC, or -1. */
static int peer_at(int x, int y)
{
    if (!x && !y) return THIS_PC;
    for (int i = 0; i < MAX_PEERS; i++)
        if (g.peer[i].addr[0] && g.peer[i].x == x && g.peer[i].y == y) return i;
    return -1;
}

/* Who is past an edge of a PC (THIS_PC for this one). */
static int neighbour(int pc, int edge)
{
    int x = pc == THIS_PC ? 0 : g.peer[pc].x, y = pc == THIS_PC ? 0 : g.peer[pc].y;
    return peer_at(x + DIR_X[edge], y + DIR_Y[edge]);
}

/* A cell for a newly added PC: beside this one first. */
static void free_cell(int pc)
{
    static const int order[][2] = {{1, 0}, {-1, 0}, {0, -1}, {0, 1}, {2, 0}, {-2, 0}, {1, -1}, {1, 1},
                                   {-1, -1}, {-1, 1}, {2, -1}, {2, 1}, {-2, -1}, {-2, 1}};
    for (int i = 0; i < (int)(sizeof order / sizeof order[0]); i++) {
        int who = peer_at(order[i][0], order[i][1]);
        if (who < 0 || who == pc) {
            g.peer[pc].x = order[i][0], g.peer[pc].y = order[i][1];
            return;
        }
    }
}

static void config_load(void)
{
    wchar_t v[128];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, g.ini);
    wcscat(g.ini, L"\\Nexus");
    CreateDirectoryW(g.ini, NULL);
    wcscat(g.ini, g.test ? L"\\test.ini" : L"\\nexus.ini");

    GetPrivateProfileStringW(L"nexus", L"role", L"main", v, 128, g.ini);
    g.cfg.role = lstrcmpiW(v, L"controlled") == 0 ? ROLE_CONTROLLED : ROLE_MAIN;
    /* Before 0.3 there was one controlled PC, on the side named by "edge". */
    GetPrivateProfileStringW(L"nexus", L"edge", L"right", v, 128, g.ini);
    int edge = EDGE_RIGHT;
    for (int i = 0; i < 4; i++)
        if (lstrcmpiW(v, EDGE_NAMES[i]) == 0) edge = i;
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g.peer[i];
        GetPrivateProfileStringW(L"nexus", PEER_KEY[i], L"", p->addr, 128, g.ini);
        GetPrivateProfileStringW(L"nexus", PLACE_KEY[i], L"", v, 128, g.ini);
        wchar_t *comma;
        p->x = (int)wcstol(v, &comma, 10); /* "x,y"; 0,0 is this PC's cell, so a bad place gets a free one */
        p->y = *comma == L',' ? (int)wcstol(comma + 1, NULL, 10) : 0;
        if (!v[0] && i == 0) p->x = DIR_X[edge], p->y = DIR_Y[edge];
        if (p->addr[0] && (!on_map(p->x, p->y) || peer_at(p->x, p->y) != i)) free_cell(i);
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g.peer[i];
        unsigned int b[6];
        GetPrivateProfileStringW(L"nexus", MAC_KEY[i], L"", v, 128, g.ini);
        if (swscanf(v, L"%2x-%2x-%2x-%2x-%2x-%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6)
            for (int k = 0; k < 6; k++) p->mac[k] = (BYTE)b[k];
        p->jump = (WORD)GetPrivateProfileIntW(L"nexus", JUMP_KEY[i], 0, g.ini);
    }
    g.cfg.resistance = GetPrivateProfileIntW(L"nexus", L"resistance", DEFAULT_RESISTANCE, g.ini);
    g.cfg.resistance = max(0, min(1020, g.cfg.resistance));
    g.cfg.fullscreen_hold = GetPrivateProfileIntW(L"nexus", L"fullscreen_hold", 0, g.ini) != 0;
    g.cfg.hide_pointer = GetPrivateProfileIntW(L"nexus", L"hide_pointer", 1, g.ini) != 0;
    g.cfg.switch_vk = (BYTE)GetPrivateProfileIntW(L"nexus", L"switch_key", VK_RCONTROL, g.ini);
    if (!g.cfg.switch_vk) g.cfg.switch_vk = VK_RCONTROL;
    GetPrivateProfileStringW(L"nexus", L"switch_style", L"tap", v, 128, g.ini);
    g.cfg.switch_hold = !lstrcmpiW(v, L"hold");
    GetPrivateProfileStringW(L"nexus", L"key", L"", v, 128, g.ini);
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
    wchar_t hex[65] = L"", place[16];
    for (int i = 0; g.cfg.has_key && i < 32; i++) _snwprintf(hex + 2 * i, 3, L"%02x", g.cfg.key[i]);
    WritePrivateProfileStringW(L"nexus", L"role", g.cfg.role == ROLE_CONTROLLED ? L"controlled" : L"main", g.ini);
    WritePrivateProfileStringW(L"nexus", L"edge", NULL, g.ini);
    for (int i = 0; i < MAX_PEERS; i++) {
        _snwprintf(place, 16, L"%d,%d", g.peer[i].x, g.peer[i].y);
        place[15] = 0;
        WritePrivateProfileStringW(L"nexus", PEER_KEY[i], g.peer[i].addr[0] ? g.peer[i].addr : NULL, g.ini);
        WritePrivateProfileStringW(L"nexus", PLACE_KEY[i], g.peer[i].addr[0] ? place : NULL, g.ini);
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        const BYTE *b = g.peer[i].mac;
        wchar_t mac[24], jump[8];
        _snwprintf(mac, 24, L"%02x-%02x-%02x-%02x-%02x-%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
        _snwprintf(jump, 8, L"%u", g.peer[i].jump);
        mac[23] = jump[7] = 0;
        BOOL known = g.peer[i].addr[0] && (b[0] | b[1] | b[2] | b[3] | b[4] | b[5]);
        WritePrivateProfileStringW(L"nexus", MAC_KEY[i], known ? mac : NULL, g.ini);
        WritePrivateProfileStringW(L"nexus", JUMP_KEY[i], g.peer[i].jump ? jump : NULL, g.ini);
    }
    wchar_t n[16];
    _snwprintf(n, 16, L"%d", g.cfg.resistance);
    WritePrivateProfileStringW(L"nexus", L"resistance", n, g.ini);
    WritePrivateProfileStringW(L"nexus", L"fullscreen_hold", g.cfg.fullscreen_hold ? L"1" : L"0", g.ini);
    WritePrivateProfileStringW(L"nexus", L"hide_pointer", g.cfg.hide_pointer ? L"1" : L"0", g.ini);
    _snwprintf(n, 16, L"%d", g.cfg.switch_vk);
    WritePrivateProfileStringW(L"nexus", L"switch_key", n, g.ini);
    WritePrivateProfileStringW(L"nexus", L"switch_style", g.cfg.switch_hold ? L"hold" : L"tap", g.ini);
    /* The stretched key, never the password itself. */
    WritePrivateProfileStringW(L"nexus", L"key", hex, g.ini);
}

/* What the hook thread follows, from the config. */
static void sync_keys(void)
{
    InputConfig c = {0};
    c.resistance = g.cfg.resistance, c.fullscreen_hold = g.cfg.fullscreen_hold, c.paused = g.cfg.paused;
    c.switch_vk = g.cfg.switch_vk, c.hold = g.cfg.switch_hold;
    for (int i = 0; i < MAX_PEERS; i++) c.jump[i] = g.peer[i].addr[0] ? g.peer[i].jump : 0;
    input_main_config(&c);
}

static int peer_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PEERS; i++) n += g.peer[i].addr[0] != 0;
    return n;
}

static BOOL configured(void) { return g.cfg.has_key && (g.cfg.role == ROLE_CONTROLLED || peer_count()); }

static BOOL up(int pc) { return pc == THIS_PC || (pc >= 0 && g.peer[pc].status == NS_CONNECTED); }

static BOOL has_mac(int pc)
{
    const BYTE *b = g.peer[pc].mac;
    return (b[0] | b[1] | b[2] | b[3] | b[4] | b[5]) != 0;
}

/* The edges of a PC (THIS_PC for this one) that lead to a PC that is there. */
static int exits(int pc)
{
    int mask = 0;
    for (int e = 0; e < 4; e++) {
        int n = neighbour(pc, e);
        if (n >= 0 && up(n)) mask |= 1 << e;
    }
    return mask;
}

static int first_connected(void)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (g.peer[i].addr[0] && up(i)) return i;
    return -1;
}

/* The PC whose sound plays here: the one input went to last, else the first one there. */
static int sound_from(void) { return g.last >= 0 && up(g.last) ? g.last : first_connected(); }

static const wchar_t *name_of(int pc)
{
    if (pc < 0 || pc >= MAX_PEERS) return g.cfg.role == ROLE_MAIN ? L"the controlled PC" : L"the main PC";
    return g.peer[pc].name[0] ? g.peer[pc].name : g.peer[pc].addr[0] ? g.peer[pc].addr : L"the main PC";
}

/* ---- network threads: one per controlled PC on the main PC, one for the main PC on a controlled one ---- */

static void net_status(int pc, int st) { PostMessageW(g.wnd, WM_NET_STATUS, st, pc); }

static BOOL net_send(int pc, const unsigned char *msg, int len)
{
    if (pc < 0 || pc >= MAX_PEERS) return FALSE;
    Peer *p = &g.peer[pc];
    EnterCriticalSection(&p->send_cs);
    BOOL ok = p->link_up && link_send(&p->link, msg, len);
    LeaveCriticalSection(&p->send_cs);
    return ok;
}

/* Drops a PC's connection (-1: all of them) and makes its thread re-read the config. */
static void net_restart(int pc)
{
    EnterCriticalSection(&g.cs);
    g.gen++;
    for (int i = 0; i < MAX_PEERS; i++)
        if (pc < 0 || pc == i) {
            if (g.peer[i].conn != INVALID_SOCKET) shutdown(g.peer[i].conn, SD_BOTH);
            SetEvent(g.peer[i].wake);
        }
    if (pc <= 0 && g.listener != INVALID_SOCKET) closesocket(g.listener), g.listener = INVALID_SOCKET;
    LeaveCriticalSection(&g.cs);
}

static SOCKET dial(const char *host)
{
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(g.port);
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
        a.sin_port = htons(g.port);
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 2) == 0) g.listener = s;
        else closesocket(s);
    }
    SOCKET s = g.listener;
    LeaveCriticalSection(&g.cs);
    return s;
}

/* Runs one connection: handshake, then every message goes to the UI thread, pings answered here. */
static int session(int pc, SOCKET s, int role, const unsigned char key[32])
{
    Peer *p = &g.peer[pc];
    BOOL on = TRUE;
    DWORD timeout = 6000; /* both ends ping every second; silence this long means the link is gone */
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&on, sizeof on);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof timeout);
    EnterCriticalSection(&g.cs);
    p->conn = s;
    LeaveCriticalSection(&g.cs);

    Link l;
    char peer[64], peer_version[16];
    int r = link_handshake(&l, s, role == ROLE_MAIN, key, g.my_name, NEXUS_VERSION, peer, peer_version);
    if (r == LINK_OK) {
        EnterCriticalSection(&g.cs);
        MultiByteToWideChar(CP_UTF8, 0, peer, -1, p->name, 64);
        lstrcpynA(p->version, peer_version, 16);
        p->secret = l.forward_secret;
        p->heard = GetTickCount();
        LeaveCriticalSection(&g.cs);
        log_line("link %d: connected to %s (Nexus %s)%s", pc, peer, peer_version[0] ? peer_version : "?",
                 l.forward_secret ? ", forward secret" : "");
        EnterCriticalSection(&p->send_cs);
        p->link = l;
        p->link_up = TRUE;
        LeaveCriticalSection(&p->send_cs);
        if (role == ROLE_MAIN) {
            struct sockaddr_in a;
            int al = sizeof a;
            ULONG mac[2] = {0, 0}, ml = 6;
            if (!getpeername(s, (struct sockaddr *)&a, &al) && SendARP(a.sin_addr.s_addr, 0, mac, &ml) == NO_ERROR && ml == 6) {
                EnterCriticalSection(&g.cs);
                memcpy(p->mac, mac, 6);
                LeaveCriticalSection(&g.cs);
            }
        }
        net_status(pc, NS_CONNECTED);
        unsigned char *buf = malloc(1 + LINK_MAX);
        int n;
        while ((n = link_recv(&l, buf + 1, LINK_MAX)) > 0) {
            unsigned char *m = buf + 1;
            p->heard = GetTickCount();
            if (m[0] == MSG_PING && n == 5) {
                m[0] = MSG_PONG;
                net_send(pc, m, 5);
            } else if (m[0] >= MSG_UPDATE_BEGIN && m[0] <= MSG_UPDATE_END) {
                if (role == ROLE_CONTROLLED) update_receive(m, n);
            } else if (m[0] >= MSG_FILE_BEGIN && m[0] <= MSG_FILE_END) {
                files_receive(m, n); /* written to disk here, so a big copy never queues up in memory */
            } else {
                unsigned char *copy = malloc(1 + n);
                copy[0] = (unsigned char)pc;
                memcpy(copy + 1, m, n);
                PostMessageW(g.wnd, WM_NET_MSG, n, (LPARAM)copy);
            }
        }
        free(buf);
        log_line("link %d: closed (%s)", pc, n == LINK_RECV_FORGED ? "a frame did not open" : "connection ended");
        /* ponytail: one incoming transfer at a time for all links; files only travel with input,
         * which is in one place, so two PCs never send at once */
        files_abort();
        update_abort();
    }
    EnterCriticalSection(&p->send_cs);
    p->link_up = FALSE;
    LeaveCriticalSection(&p->send_cs);
    EnterCriticalSection(&g.cs);
    p->conn = INVALID_SOCKET;
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


/* ---- discovery: who else runs Nexus on this network ----
 * UDP: "NXUS?" asks, every Nexus answers "NXUS!" + its role + its version + NUL + its name. Nothing
 * here is secret or trusted: an answer only fills in an address, connecting still needs the password. */

static DWORD WINAPI discovery_thread(void *unused)
{
    (void)unused;
    unsigned char buf[160], reply[128];
    struct sockaddr_in from;
    for (;;) {
        int len = sizeof from;
        int n = recvfrom(g.discovery, (char *)buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &len);
        if (n <= 0) { /* an ICMP "port unreachable" for an earlier probe shows up here: not fatal */
            Sleep(50);
            continue;
        }
        BOOL mine = FALSE;
        for (int i = 0; i < 4; i++) mine |= g.local_ip[i] && g.local_ip[i] == from.sin_addr.s_addr;
        if (n == 5 && !memcmp(buf, "NXUS?", 5)) {
            int v = (int)strlen(NEXUS_VERSION), m = (int)strlen(g.my_name);
            memcpy(reply, "NXUS!", 5);
            reply[5] = (unsigned char)g.cfg.role;
            memcpy(reply + 6, NEXUS_VERSION, v);
            reply[6 + v] = 0;
            memcpy(reply + 7 + v, g.my_name, m);
            sendto(g.discovery, (const char *)reply, 7 + v + m, 0, (struct sockaddr *)&from, len);
        } else if (!mine && n > 8 && !memcmp(buf, "NXUS!", 5) && buf[5] <= ROLE_CONTROLLED) {
            buf[n] = 0;
            const char *name = memchr(buf + 6, 0, n - 6);
            Found *f = calloc(1, sizeof *f);
            if (!name || !f || !strcmp(name + 1, g.my_name)) { /* our own answer, from one of our addresses */
                free(f);
                continue;
            }
            lstrcpynA(f->ip, inet_ntoa(from.sin_addr), 16);
            MultiByteToWideChar(CP_UTF8, 0, name + 1, -1, f->name, 31);
            f->role = buf[5];
            f->seen = GetTickCount();
            if (!PostMessageW(g.wnd, WM_FOUND, 0, (LPARAM)f)) free(f);
        }
    }
    return 0;
}

static void discovery_start(void)
{
    struct sockaddr_in a = {0};
    BOOL on = TRUE;
    a.sin_family = AF_INET;
    a.sin_port = htons(g.discovery_port);
    g.discovery = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(g.discovery, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof on);
    if (bind(g.discovery, (struct sockaddr *)&a, sizeof a) != 0) { /* another program has the port */
        closesocket(g.discovery);
        g.discovery = INVALID_SOCKET;
        return;
    }
    CloseHandle(CreateThread(NULL, 0, discovery_thread, NULL, 0, NULL));
}

/* Main PC: ask every few seconds. Windows sends 255.255.255.255 out of one adapter only, so each
 * local network gets its own broadcast too. */
static void discovery_probe(void)
{
    struct sockaddr_in to = {0};
    if (g.discovery == INVALID_SOCKET) return;
    to.sin_family = AF_INET;
    to.sin_port = htons(g.discovery_port);
    to.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(g.discovery, "NXUS?", 5, 0, (struct sockaddr *)&to, sizeof to);
    for (int i = 0; i < 4 && g.broadcast[i]; i++) {
        to.sin_addr.s_addr = g.broadcast[i];
        sendto(g.discovery, "NXUS?", 5, 0, (struct sockaddr *)&to, sizeof to);
    }
}

/* An answer: kept by address, the oldest making room. Returns whether the list on screen changes. */
static BOOL discovery_found(const Found *f)
{
    int slot = 0;
    for (int i = 0; i < MAX_FOUND; i++) {
        if (!strcmp(g.found[i].ip, f->ip)) {
            BOOL changed = g.found[i].role != f->role || lstrcmpW(g.found[i].name, f->name);
            g.found[i] = *f;
            return changed;
        }
        if (g.found[i].seen < g.found[slot].seen) slot = i;
    }
    g.found[slot] = *f;
    return TRUE;
}

static BOOL discovery_expire(void) /* PCs not heard from for 10 s are gone */
{
    BOOL changed = FALSE;
    for (int i = 0; i < MAX_FOUND; i++)
        if (g.found[i].ip[0] && GetTickCount() - g.found[i].seen > 10000) memset(&g.found[i], 0, sizeof g.found[i]), changed = TRUE;
    return changed;
}

static DWORD WINAPI net_thread(void *arg)
{
    int pc = (int)(INT_PTR)arg;
    Peer *p = &g.peer[pc];
    int gen = -1, sticky = 0; /* the last failure stays on screen until the next success or a config change */
    for (;;) {
        EnterCriticalSection(&g.cs);
        int role = g.cfg.role;
        /* A controlled PC has one main PC: this thread's job only for the first slot. */
        BOOL ready = g.cfg.has_key && (role == ROLE_MAIN ? p->addr[0] != 0 : pc == 0);
        unsigned char key[32];
        char host[128];
        memcpy(key, g.cfg.key, 32);
        WideCharToMultiByte(CP_ACP, 0, p->addr, -1, host, sizeof host, NULL, NULL);
        if (gen != g.gen) gen = g.gen, sticky = 0;
        LeaveCriticalSection(&g.cs);

        if (!ready) {
            net_status(pc, NS_SETUP);
            WaitForSingleObject(p->wake, INFINITE);
            continue;
        }
        SOCKET s;
        if (role == ROLE_MAIN) {
            if (pc == 0) {
                EnterCriticalSection(&g.cs);
                if (g.listener != INVALID_SOCKET) closesocket(g.listener), g.listener = INVALID_SOCKET;
                LeaveCriticalSection(&g.cs);
            }
            net_status(pc, sticky ? sticky : NS_CONNECTING);
            s = dial(host);
            if (s == INVALID_SOCKET) {
                net_status(pc, sticky = NS_UNREACHABLE);
                WaitForSingleObject(p->wake, 3000);
                continue;
            }
        } else {
            SOCKET ls = listen_socket();
            if (ls == INVALID_SOCKET) {
                net_status(pc, NS_PORT);
                WaitForSingleObject(p->wake, 3000);
                continue;
            }
            net_status(pc, sticky ? sticky : NS_WAITING);
            s = accept(ls, NULL, NULL);
            if (s == INVALID_SOCKET) continue; /* the listener was closed for a config change */
        }
        sticky = session(pc, s, role, key);
        if (sticky) net_status(pc, sticky);
        if (sticky || role == ROLE_MAIN) WaitForSingleObject(p->wake, sticky ? 3000 : 500);
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
    g.eyebrow = mkfont(L"Hanken Grotesk SemiBold", 16, FW_NORMAL); /* a card's heading */
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
    redraw(NULL);
}

static COLORREF color(int c)
{
    const COLORREF all[] = {g.p->ink, g.p->ink2, g.p->ink3, g.p->signal, g.p->amber, g.p->fault, g.p->off};
    return all[c];
}

/* ---- status text ---- */

/* The state shown for this PC as a whole: connected while any controlled PC is, else the first one's. */
static int overall(void)
{
    if (g.cfg.role == ROLE_CONTROLLED) return g.peer[0].status;
    if (!configured()) return NS_SETUP;
    if (first_connected() >= 0) return NS_CONNECTED;
    for (int i = 0; i < MAX_PEERS; i++)
        if (g.peer[i].addr[0]) return g.peer[i].status;
    return NS_SETUP;
}

/* The PC the sidebar talks about: where input is, else the one used last, else the first one there. */
static int shown_peer(void)
{
    if (g.cfg.role == ROLE_CONTROLLED) return 0;
    if (g.target >= 0) return g.target;
    if (g.last >= 0 && up(g.last)) return g.last;
    return first_connected();
}

static int status_color_of(int st)
{
    switch (st) {
    case NS_CONNECTED: return C_SIGNAL;
    case NS_SETUP: case NS_WAITING: case NS_CONNECTING: return C_OFF;
    case NS_UNREACHABLE: return C_AMBER;
    default: return C_FAULT;
    }
}

static int status_color(void) { return status_color_of(overall()); }

/* g.line: the short status word for the sidebar; g.note: the sentence for the Overview card. */
static void status_text(void)
{
    int st = overall();
    const wchar_t *name = name_of(shown_peer());
    if (st != NS_CONNECTED) {
        lstrcpynW(g.line, STATUS_WORD[st], 256);
        lstrcpynW(g.note, STATUS_NOTE[st], 256);
    } else if (g.remote && g.target >= 0) {
        _snwprintf(g.line, 256, L"Input on %ls", name);
        _snwprintf(g.note, 256, L"Input is on %ls. Push back through its edge, use the switch key (Keyboard page), "
                                L"or press Ctrl+Alt+Pause to return.", name);
    } else if (g.cfg.role == ROLE_CONTROLLED && input_controlled_active()) {
        _snwprintf(g.line, 256, L"Controlled by %ls", name);
        _snwprintf(g.note, 256, L"%ls is driving this PC.", name);
    } else {
        lstrcpynW(g.line, L"Connected", 256);
        if (g.cfg.role == ROLE_MAIN)
            lstrcpynW(g.note, L"Push the pointer through an edge that leads to another PC (the Crossing page "
                              L"shows which), or use the switch key on the Keyboard page.", 256);
        else _snwprintf(g.note, 256, L"Connected to %ls, the main PC.", name);
    }
    if (g.cfg.role == ROLE_MAIN && g.cfg.paused && !g.remote) wcscat(g.line, L", crossing paused");
    g.line[255] = g.note[255] = 0;
    _snwprintf(g.tray.szTip, 64, L"Nexus: %ls", g.line);
    g.tray.szTip[63] = 0;
    /* The icon says where input is: a blue dot while it is on another PC, amber while crossing is paused. */
    BOOL away = g.cfg.role == ROLE_MAIN ? g.remote : st == NS_CONNECTED && !input_controlled_active();
    g.tray.hIcon = away ? g.icon_away : g.cfg.role == ROLE_MAIN && g.cfg.paused ? g.icon_paused : g.icon_here;
    g.tray.uFlags = NIF_TIP | NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g.tray);
}

/* A notification from the tray icon (a balloon on XP and 7, a toast on 10 and 11); it goes in the log too. */
static void notify(const wchar_t *title, const wchar_t *text)
{
    char t[300];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, t, sizeof t, NULL, NULL);
    log_line("notice: %s", t);
    g.tray.uFlags = NIF_INFO;
    lstrcpynW(g.tray.szInfoTitle, title, 64);
    lstrcpynW(g.tray.szInfo, text, 256);
    g.tray.dwInfoFlags = NIIF_INFO;
    g.tray.uTimeout = 10000;
    Shell_NotifyIconW(NIM_MODIFY, &g.tray);
}

/* The app icon with a coloured dot in its corner (NO_FILL: as it is). */
static HICON badge_icon(COLORREF c)
{
    int size = GetSystemMetrics(SM_CXSMICON);
    HICON base = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, 0);
    ICONINFO ii;
    if (c == NO_FILL || !base || !GetIconInfo(base, &ii)) return base;
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
    DWORD *px = malloc((size_t)size * size * 4);
    HDC dc = GetDC(NULL);
    if (px && GetDIBits(dc, ii.hbmColor, 0, size, px, &bi, DIB_RGB_COLORS)) {
        double r = size * 0.28, cx = size - r - 0.5, cy = size - r - 0.5;
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                double d = sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
                double ring = max(0.0, min(1.0, r + 1.5 - d)), dot = max(0.0, min(1.0, r + 0.5 - d));
                if (ring <= 0) continue;
                DWORD *p = &px[y * size + x];
                /* a dark ring keeps the dot apart from the icon, then the dot over it (straight alpha) */
                double ab = (*p >> 24) / 255.0, a1 = ring + ab * (1 - ring), a = dot + a1 * (1 - dot);
                int ch[3];
                for (int k = 0; k < 3; k++) {
                    double base_c = (*p >> (8 * k)) & 0xFF, dot_c = k == 0 ? GetBValue(c) : k == 1 ? GetGValue(c) : GetRValue(c);
                    double c1 = (0x0e * ring + base_c * ab * (1 - ring)) / a1;
                    ch[k] = max(0, min(255, (int)((dot_c * dot + c1 * a1 * (1 - dot)) / a + 0.5)));
                }
                *p = (DWORD)(a * 255 + 0.5) << 24 | ch[2] << 16 | ch[1] << 8 | ch[0];
            }
        SetDIBits(dc, ii.hbmColor, 0, size, px, &bi, DIB_RGB_COLORS);
        HICON made = CreateIconIndirect(&ii);
        if (made) DestroyIcon(base), base = made;
    }
    ReleaseDC(NULL, dc);
    free(px);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    return base;
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
        g.local_ip[i] = a.s_addr;
        g.broadcast[i] = a.s_addr | htonl(0xFF); /* ponytail: assumes /24, which home networks are */
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

/* Smooth shapes: GDI+ (part of Windows since XP) draws edges anti-aliased; text stays with GDI.
 * The few flat-API functions used here, declared as gdiplus.dll exports them. */
typedef struct { UINT32 version; void *debug; BOOL no_thread, no_codecs; } GpStartup;
int WINAPI GdiplusStartup(ULONG_PTR *token, const GpStartup *in, void *out);
int WINAPI GdipCreateFromHDC(HDC dc, void **gr);
int WINAPI GdipDeleteGraphics(void *gr);
int WINAPI GdipSetSmoothingMode(void *gr, int mode);
int WINAPI GdipCreateSolidFill(DWORD argb, void **brush);
int WINAPI GdipDeleteBrush(void *brush);
int WINAPI GdipCreatePen1(DWORD argb, float width, int unit, void **pen);
int WINAPI GdipSetPenStartCap(void *pen, int cap);
int WINAPI GdipSetPenEndCap(void *pen, int cap);
int WINAPI GdipSetPenLineJoin(void *pen, int join);
int WINAPI GdipDeletePen(void *pen);
int WINAPI GdipCreatePath(int fill_mode, void **path);
int WINAPI GdipDeletePath(void *path);
int WINAPI GdipAddPathArc(void *path, float x, float y, float w, float h, float start, float sweep);
int WINAPI GdipClosePathFigure(void *path);
int WINAPI GdipFillPath(void *gr, void *brush, void *path);
int WINAPI GdipDrawPath(void *gr, void *pen, void *path);
int WINAPI GdipFillEllipse(void *gr, void *brush, float x, float y, float w, float h);
int WINAPI GdipDrawEllipse(void *gr, void *pen, float x, float y, float w, float h);
int WINAPI GdipDrawArc(void *gr, void *pen, float x, float y, float w, float h, float start, float sweep);
int WINAPI GdipDrawLine(void *gr, void *pen, float x1, float y1, float x2, float y2);
enum { GP_ANTIALIAS = 4, GP_PIXEL = 2, GP_ROUND = 2 };

static DWORD argb(COLORREF c, int alpha) { return (DWORD)alpha << 24 | GetRValue(c) << 16 | GetGValue(c) << 8 | GetBValue(c); }

static void *smooth(HDC dc)
{
    void *gr = NULL;
    GdipCreateFromHDC(dc, &gr);
    GdipSetSmoothingMode(gr, GP_ANTIALIAS);
    return gr;
}

static void *rounded(float x, float y, float w, float h, float radius)
{
    void *path = NULL;
    float d = max(1.0f, 2 * radius);
    GdipCreatePath(0, &path);
    GdipAddPathArc(path, x, y, d, d, 180, 90);
    GdipAddPathArc(path, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(path, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(path, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(path);
    return path;
}

/* The outline runs just inside r, as GDI's RoundRect draws it. */
static void round_rect(HDC dc, const RECT *r, int radius, COLORREF fill_c, COLORREF line_c, int width)
{
    void *gr = smooth(dc), *brush = NULL, *pen = NULL;
    float half = width / 2.0f;
    void *path = rounded(r->left + half, r->top + half, r->right - r->left - 2 * half, r->bottom - r->top - 2 * half,
                         radius - half);
    if (fill_c != NO_FILL) GdipCreateSolidFill(argb(fill_c, 255), &brush), GdipFillPath(gr, brush, path);
    if (fill_c != line_c) GdipCreatePen1(argb(line_c, 255), (float)width, GP_PIXEL, &pen), GdipDrawPath(gr, pen, path);
    if (brush) GdipDeleteBrush(brush);
    if (pen) GdipDeletePen(pen);
    GdipDeletePath(path);
    GdipDeleteGraphics(gr);
}

/* A dot; alpha below 255 for the soft halo around a lit one. */
static void dot_alpha(HDC dc, int x, int y, int size, COLORREF c, int alpha)
{
    void *gr = smooth(dc), *brush = NULL;
    GdipCreateSolidFill(argb(c, alpha), &brush);
    GdipFillEllipse(gr, brush, (float)x, (float)y, (float)size, (float)size);
    GdipDeleteBrush(brush);
    GdipDeleteGraphics(gr);
}

static void dot(HDC dc, int x, int y, int size, COLORREF c) { dot_alpha(dc, x, y, size, c, 255); }

/* A status light: the dot, and a halo when it is lit (connected). */
static void status_light(HDC dc, int x, int y, int size, int c)
{
    if (c == C_SIGNAL) dot_alpha(dc, x - size / 2, y - size / 2, 2 * size, color(c), 48);
    dot(dc, x, y, size, color(c));
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

static void draw_icon(HDC dc, int page, int x, int y, COLORREF c)
{
    /* Line icons for the sidebar in a 24-unit box, round-capped like Lucide's. */
    float k = g.dpi / 96.0f;
#define X(a) (x + (a) * k)
#define Y(b) (y + (b) * k)
#define LINE(a, b, c2, d) GdipDrawLine(gr, pen, X(a), Y(b), X(c2), Y(d))
#define BOX(a, b, w, h, r) \
    do { \
        void *box = rounded(X(a), Y(b), (w) * k, (h) * k, (r) * k); \
        GdipDrawPath(gr, pen, box); \
        GdipDeletePath(box); \
    } while (0)
    void *gr = smooth(dc), *pen = NULL;
    GdipCreatePen1(argb(c, 255), 1.75f * k, GP_PIXEL, &pen);
    GdipSetPenStartCap(pen, GP_ROUND);
    GdipSetPenEndCap(pen, GP_ROUND);
    GdipSetPenLineJoin(pen, GP_ROUND);
    switch (page) {
    case PAGE_OVERVIEW: /* a screen and a second one */
        BOX(2, 4, 14, 11, 2);
        LINE(9, 15, 9, 19), LINE(6, 19, 12, 19);
        BOX(14, 10, 8, 10, 2);
        break;
    case PAGE_CROSSING: /* arrows both ways */
        LINE(3, 8, 21, 8), LINE(21, 8, 17, 4), LINE(21, 8, 17, 12);
        LINE(21, 16, 3, 16), LINE(3, 16, 7, 12), LINE(3, 16, 7, 20);
        break;
    case PAGE_KEYBOARD:
        BOX(2, 5, 20, 14, 2.5f);
        {
            void *keys = NULL;
            GdipCreateSolidFill(argb(c, 255), &keys);
            for (int i = 0; i < 5; i++) GdipFillEllipse(gr, keys, X(5 + 3.5f * i), Y(8.5f), 2 * k, 2 * k);
            GdipDeleteBrush(keys);
        }
        LINE(8, 15, 16, 15);
        break;
    case PAGE_SOUND: /* speaker */
        LINE(3, 9, 7, 9), LINE(7, 9, 12, 4), LINE(12, 4, 12, 20), LINE(12, 20, 7, 15), LINE(7, 15, 3, 15),
            LINE(3, 15, 3, 9);
        GdipDrawArc(gr, pen, X(11), Y(8), 8 * k, 8 * k, -60, 120);
        GdipDrawArc(gr, pen, X(10), Y(4), 14 * k, 16 * k, -65, 130);
        break;
    case PAGE_FILES: /* folder */
        LINE(2, 7, 2, 18), LINE(2, 18, 22, 18), LINE(22, 18, 22, 9), LINE(22, 9, 12, 9), LINE(12, 9, 10, 6),
            LINE(10, 6, 3, 6), LINE(3, 6, 2, 7);
        break;
    default: /* link */
        GdipDrawEllipse(gr, pen, X(2), Y(6), 12 * k, 12 * k);
        GdipDrawEllipse(gr, pen, X(10), Y(6), 12 * k, 12 * k);
    }
#undef X
#undef Y
#undef LINE
#undef BOX
    GdipDeletePen(pen);
    GdipDeleteGraphics(gr);
}

/* ---- owner-drawn controls: real BUTTON/STATIC controls, so Tab, Space and screen readers work ---- */

static LRESULT CALLBACK button_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    int id = GetDlgCtrlID(w);
    if (id >= ID_TILE0 && id <= ID_TILE3) { /* a PC on the map: dragged to another cell */
        int pc = id - ID_TILE0;
        if (m == WM_LBUTTONDOWN) {
            SetFocus(w);
            SetCapture(w);
            g.drag = pc;
            g.grab.x = (short)LOWORD(lp), g.grab.y = (short)HIWORD(lp);
            return 0;
        }
        if (m == WM_MOUSEMOVE && g.drag == pc) {
            POINT pt;
            RECT r;
            GetCursorPos(&pt);
            ScreenToClient(g.wnd, &pt);
            GetWindowRect(w, &r);
            MoveWindow(w, pt.x - g.grab.x, pt.y - g.grab.y, r.right - r.left, r.bottom - r.top, TRUE);
            return 0;
        }
        if ((m == WM_LBUTTONUP || m == WM_CAPTURECHANGED) && g.drag == pc) {
            g.drag = -1;
            if (m == WM_LBUTTONUP) ReleaseCapture();
            RECT r;
            GetWindowRect(w, &r);
            MapWindowPoints(NULL, g.wnd, (POINT *)&r, 2);
            int cx = (r.left + r.right) / 2 - g.map_rc.left, cy = (r.top + r.bottom) / 2 - g.map_rc.top;
            /* Dropped off the map, or capture lost: back to its cell. */
            if (m == WM_LBUTTONUP && cx >= 0 && cy >= 0 && cx < MAP_W * cell_w && cy < MAP_H * cell_h)
                move_peer(pc, cx / cell_w - MAP_W / 2, cy / cell_h - MAP_H / 2);
            else move_peer(pc, g.peer[pc].x, g.peer[pc].y);
            return 0;
        }
    }
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
    if (id >= ID_RES0 && id <= ID_RES3) return RESISTANCE[id - ID_RES0] == g.cfg.resistance;
    if (id == ID_STYLE_TAP || id == ID_STYLE_HOLD) return (id == ID_STYLE_HOLD) == g.cfg.switch_hold;
    if (id == ID_PAUSE) return g.cfg.paused;
    if (id == ID_FULLSCREEN) return g.cfg.fullscreen_hold;
    if (id == ID_HIDE_POINTER) return g.cfg.hide_pointer;
    return FALSE;
}

static void draw_control(const DRAWITEMSTRUCT *d)
{
    const Palette *p = g.p;
    RECT r = d->rcItem;
    int w = r.right, h = r.bottom;
    wchar_t label[64];
    GetWindowTextW(d->hwndItem, label, 64);
    wchar_t *state = wcschr(label, L'\t'); /* "Pause crossing\ton": the state is for screen readers, not drawn */
    if (state) *state = 0;

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
        BOOL in_module = kind == K_SEGMENT || kind == K_TILE || kind == K_TOGGLE || d->CtlID == ID_SETUP ||
                         (d->CtlID >= ID_FOUND0 && d->CtlID <= ID_FOUND3) || (d->CtlID >= ID_JUMP0 && d->CtlID <= ID_WAKE3) ||
                         d->CtlID == ID_SWITCH_KEY;
        COLORREF bg = in_module ? p->panel : p->ground;
        fill(dc, &r, bg);
        if (kind == K_NAV) {
            int page = (int)d->CtlID - ID_NAV0;
            BOOL sel = page == g.page;
            if (sel || hover) round_rect(dc, &r, S(8), sel ? p->well : p->panel, sel ? p->well : p->panel, 1);
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
            if (sel) round_rect(dc, &b, S(8), p->ink, p->ink, S(1));
            else round_rect(dc, &b, S(8), hover ? p->well : bg, hover ? p->ink2 : p->edge, S(1));
            text(dc, label, r, g.button, sel ? p->ground : p->ink, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        } else if (kind == K_TOGGLE) { /* a switch, then its label; the button's text names it for screen readers */
            BOOL on = is_selected((int)d->CtlID);
            RECT track = {S(2), (h - S(20)) / 2, S(38), (h + S(20)) / 2};
            round_rect(dc, &track, S(10), on ? p->signal : hover ? p->well : bg, on ? p->signal : hover ? p->ink2 : p->edge,
                       S(1));
            dot(dc, on ? S(22) : S(6), (h - S(12)) / 2, S(12), on ? p->ground : p->ink2);
            RECT t = {S(50), 0, w, h};
            text(dc, label, t, g.body, p->ink, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        } else if (kind == K_TILE) {
            const Peer *pp = &g.peer[d->CtlID - ID_TILE0];
            RECT b = {0, 0, w, h}, t;
            round_rect(dc, &b, S(10), hover ? p->rule : p->well, hover ? p->ink2 : p->edge, S(1));
            status_light(dc, S(13), S(16), S(8), status_color_of(pp->status));
            SetRect(&t, S(26), S(8), w - S(8), S(30));
            text(dc, label, t, g.button, p->ink, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            SetRect(&t, S(12), S(30), w - S(8), S(50));
            /* Under the name, the address; with no name yet, the address is the name and the state goes here. */
            text(dc, pp->name[0] ? pp->addr : STATUS_WORD[pp->status], t, g.small, p->ink3,
                 DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        } else {
            COLORREF c = d->CtlID == ID_UPDATE ? p->signal : hover ? p->ink : p->ink3;
            text(dc, label, r, d->CtlID == ID_UPDATE ? g.button : g.small, c, DT_SINGLELINE | DT_VCENTER);
        }
        if (focus) {
            RECT f = {S(1), S(1), w - S(1), h - S(1)};
            round_rect(dc, &f, S(9), NO_FILL, p->signal, S(2));
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

static int view_bottom; /* the footer's top: page controls below it are cut off there */

/* A control on the page: shown, and cut to the part above the footer while the page scrolls. */
static void place(HWND c, int x, int y, int w, int h)
{
    MoveWindow(c, x, y, w, h, TRUE);
    if (y + h <= 0 || y >= view_bottom) {
        ShowWindow(c, SW_HIDE);
        return;
    }
    SetWindowRgn(c, y < 0 || y + h > view_bottom ? CreateRectRgn(0, max(0, -y), w, min(h, view_bottom - y)) : NULL, TRUE);
    ShowWindow(c, SW_SHOW);
}

/* "Ctrl + Alt + F1", "Right Ctrl": a recorded key as the keyboard names it. */
static void key_name(WORD chord, wchar_t *out, int cap)
{
    static const struct { BYTE vk; const wchar_t *name; } sided[] = {
        {VK_LCONTROL, L"Left Ctrl"}, {VK_RCONTROL, L"Right Ctrl"}, {VK_LMENU, L"Left Alt"}, {VK_RMENU, L"Right Alt"},
        {VK_LSHIFT, L"Left Shift"}, {VK_RSHIFT, L"Right Shift"}, {VK_LWIN, L"Left Windows"}, {VK_RWIN, L"Right Windows"},
        {VK_PAUSE, L"Pause"}, {VK_APPS, L"Menu"}};
    static const BYTE extended[] = {VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_LEFT, VK_RIGHT, VK_UP,
                                    VK_DOWN, VK_NUMLOCK, VK_DIVIDE, VK_SNAPSHOT};
    BYTE vk = LOBYTE(chord), mods = HIBYTE(chord);
    wchar_t name[64] = L"";
    for (int i = 0; i < (int)(sizeof sided / sizeof sided[0]); i++)
        if (sided[i].vk == vk) lstrcpynW(name, sided[i].name, 64);
    if (!name[0]) {
        LONG code = (LONG)MapVirtualKeyW(vk, 0) << 16;
        for (int i = 0; i < (int)sizeof extended; i++)
            if (extended[i] == vk) code |= 1 << 24;
        if (!GetKeyNameTextW(code, name, 64)) _snwprintf(name, 64, L"Key %02X", vk);
    }
    name[63] = 0;
    _snwprintf(out, cap, L"%ls%ls%ls%ls%ls", mods & CHORD_CTRL ? L"Ctrl + " : L"", mods & CHORD_ALT ? L"Alt + " : L"",
               mods & CHORD_SHIFT ? L"Shift + " : L"", mods & CHORD_WIN ? L"Win + " : L"", name);
    out[cap - 1] = 0;
}

/* A toggle's or segment's text: its name, a tab, and its state for screen readers (only the name is drawn). */
static void state_text(HWND c, const wchar_t *name, BOOL on, BOOL segment)
{
    wchar_t t[96];
    _snwprintf(t, 96, L"%ls\t%ls", name, segment ? (on ? L"selected" : L"") : on ? L"on" : L"off");
    t[95] = 0;
    SetWindowTextW(c, t);
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

/* "through this PC's right edge and XP's top edge": the edges that lead to a PC on the map. */
static void ways_in(int pc, wchar_t *out, int cap)
{
    int at = 0;
    out[0] = 0;
    for (int e = 0; e < 4; e++) {
        int n = neighbour(pc, e);
        if (n < 0) continue;
        const wchar_t *who = n == THIS_PC ? L"this PC" : name_of(n);
        at += _snwprintf(out + at, cap - at, L"%ls%ls's %ls edge", at ? L" and " : L"", who, EDGE_NAMES[e ^ 1]);
        if (at < 0 || at >= cap) break;
    }
    out[cap - 1] = 0;
}

static void layout_page(void)
{
    static wchar_t detail[MAX_PEERS + 1][192], address_line[192], ledger[MAX_PEERS][256];
    static wchar_t keycaps[MAX_PEERS + 1][64], jump_text[MAX_PEERS][96], wake_text[MAX_PEERS][64];
    HWND page_controls[] = {g.setup, g.role[0], g.role[1], g.pw, g.pw_label, g.found_btn[0], g.found_btn[1],
                            g.found_btn[2], g.found_btn[3], g.res[0], g.res[1], g.res[2], g.res[3], g.pause,
                            g.fullscreen, g.hide_pointer, g.switch_key, g.style[0], g.style[1]};
    for (int i = 0; i < (int)(sizeof page_controls / sizeof page_controls[0]); i++) ShowWindow(page_controls[i], SW_HIDE);
    for (int i = 0; i < MAX_PEERS; i++) {
        ShowWindow(g.addr[i], SW_HIDE), ShowWindow(g.addr_label[i], SW_HIDE);
        ShowWindow(g.jump[i], SW_HIDE), ShowWindow(g.wake[i], SW_HIDE);
        if (g.drag != i) ShowWindow(g.tile[i], SW_HIDE);
    }

    RECT c;
    GetClientRect(g.wnd, &c);
    int top = S(STATUS_H) + S(16);
    for (int i = 0; i < NPAGES; i++)
        MoveWindow(g.nav[i], S(16), top + i * S(NAV_H + 4), S(SIDEBAR - 32), S(NAV_H), TRUE);
    int fy = c.bottom - S(FOOTER);
    view_bottom = fy;
    MoveWindow(g.site, S(28), fy - S(44), S(SIDEBAR - 56), S(28), TRUE);
    MoveWindow(g.logs, S(28), fy - S(100), S(SIDEBAR - 56), S(28), TRUE);
    if (g.update_version[0] && g.cfg.role == ROLE_MAIN && !g.installing) {
        wchar_t label[48];
        _snwprintf(label, 48, L"Install Nexus %hs", g.update_version);
        label[47] = 0;
        SetWindowTextW(g.update, label);
        place(g.update, S(28), fy - S(136), S(SIDEBAR - 56), S(28));
    } else {
        ShowWindow(g.update, SW_HIDE);
    }
    MoveWindow(g.quit, c.right - S(32) - S(190), fy + (S(FOOTER) - S(48)) / 2, S(190), S(48), TRUE);

    /* Page column: Beamer's 40px margin, at most 960 wide. */
    int x0 = S(SIDEBAR) + S(40), width = c.right - x0 - S(40);
    if (width > S(960)) width = S(960);
    lay_dc = GetDC(g.wnd);
    g.nlabels = 0;
    int dh = text_height(lay_dc, PAGES[g.page].desc, width, g.body);
    SetRect(&g.desc_rc, x0, S(92) - g.scroll, x0 + width, S(92) - g.scroll + dh);

    int my = g.desc_rc.bottom + S(28), mx = x0 + S(32), mw = width - S(64), y = my + S(28);
    add_label(L_LINE, mx, y, mw, S(24), PAGES[g.page].eyebrow, g.eyebrow, C_INK, NULL);
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
        for (int i = 0; i < MAX_PEERS; i++) {
            const Peer *p = &g.peer[i];
            if (main_pc ? !p->addr[0] : i > 0) continue;
            add_label(L_DOT, mx, y + S(7), S(10), S(10), NULL, NULL, status_color_of(p->status), NULL);
            add_label(L_LINE, mx + S(22), y, mw - S(22), S(24),
                      p->status == NS_CONNECTED || main_pc ? name_of(i) : L"Main PC", g.status_font, C_INK, NULL);
            y += S(28);
            const wchar_t *sealed = p->status != NS_CONNECTED ? L"" : p->secret ? L"  \x00b7  forward secret"
                                                                                 : L"  \x00b7  password key only (older Nexus)";
            if (main_pc) _snwprintf(detail[i], 192, L"Controlled PC  \x00b7  %ls  \x00b7  %ls%ls", p->addr, STATUS_WORD[p->status], sealed);
            else _snwprintf(detail[i], 192, L"Main PC  \x00b7  connects to this PC on port %d%ls", g.port, sealed);
            detail[i][191] = 0;
            add_label(L_LINE, mx, y, mw, S(22), detail[i], g.body, C_INK3, NULL);
            y += S(30);
            if (main_pc && p->status != NS_CONNECTED && has_mac(i)) { /* asleep, maybe: Wake-on-LAN */
                _snwprintf(wake_text[i], 64, L"Wake %ls", name_of(i));
                wake_text[i][63] = 0;
                SetWindowTextW(g.wake[i], wake_text[i]);
                place(g.wake[i], mx, y, min(mw, S(300)), S(28));
                y += S(30);
            }
            y += S(6);
        }
        y = add_text(mx, y, mw, g.note, g.body, C_INK2);
        if (g.update_note[0]) y = add_text(mx, y + S(12), mw, g.update_note, g.body, C_INK);
        break;
    case PAGE_CROSSING:
        if (!main_pc) {
            y = add_text(mx, y, mw,
                         L"The main PC arranges the PCs and sets how hard an edge resists. Push the pointer through "
                         L"an edge of this screen that has a PC beside it on the main PC's map to go there.",
                         g.body, C_INK2) + S(20);
            state_text(g.hide_pointer, L"Hide this PC's pointer while input is on another PC", g.cfg.hide_pointer, FALSE);
            place(g.hide_pointer, mx, y, mw, S(32));
            y = add_text(mx, y + S(36), mw, L"Moving this PC's own mouse shows it again.", g.small, C_INK3);
            break;
        }
        if (!peer_count()) {
            y = add_text(mx, y, mw, L"Add a controlled PC on the Connection page; it then shows up here.", g.body,
                         C_INK2);
            break;
        }
        /* The map: this PC in the middle cell, the others where they were dragged. */
        cell_w = min(S(150), mw / MAP_W), cell_h = cell_w * 3 / 5;
        SetRect(&g.map_rc, mx, y, mx + MAP_W * cell_w, y + MAP_H * cell_h);
        for (int cy = 0; cy < MAP_H; cy++)
            for (int cx = 0; cx < MAP_W; cx++) {
                int gx = cx - MAP_W / 2, gy = cy - MAP_H / 2, who = peer_at(gx, gy);
                int l = g.map_rc.left + cx * cell_w + S(4), t = g.map_rc.top + cy * cell_h + S(4);
                if (who == THIS_PC)
                    add_label(L_HOME, l, t, cell_w - S(8), cell_h - S(8), L"This PC", g.button, C_INK, NULL);
                else if (who < 0)
                    add_label(L_CELL, l, t, cell_w - S(8), cell_h - S(8), NULL, NULL, 0, NULL);
                else if (g.drag != who) {
                    SetWindowTextW(g.tile[who], name_of(who));
                    place(g.tile[who], l, t, cell_w - S(8), cell_h - S(8));
                }
            }
        y = g.map_rc.bottom + S(20);
        for (int i = 0; i < MAX_PEERS; i++) {
            if (!g.peer[i].addr[0]) continue;
            wchar_t ways[192];
            ways_in(i, ways, 192);
            if (ways[0]) _snwprintf(ledger[i], 256, L"%ls: through %ls.", name_of(i), ways);
            else _snwprintf(ledger[i], 256, L"%ls: no edge leads there. Move it beside a PC, or give it a jump key "
                                            L"on the Keyboard page.", name_of(i));
            ledger[i][255] = 0;
            y = add_text(mx, y, mw, ledger[i], g.body, ways[0] ? C_INK2 : C_AMBER) + S(6);
        }
        y = add_text(mx, y + S(10), mw,
                     L"Drag a PC to another cell, or select it and use the arrow keys. A PC crosses to the one "
                     L"beside it and back the same way. The pointer arrives at the same height, crossing waits while "
                     L"a mouse button is held, and what you copied comes along.",
                     g.small, C_INK3) + S(24);
        /* How hard an edge resists: Beamer's pressure, in px of push, its corners never giving. */
        y = add_text(mx, y, mw, L"Edge resistance", g.button, C_INK) + S(8);
        {
            static const wchar_t *names[] = {L"Touch", L"Light", L"Normal", L"Firm"};
            for (int i = 0; i < 4; i++) {
                state_text(g.res[i], names[i], is_selected(ID_RES0 + i), TRUE);
                place(g.res[i], mx + i * S(118), y, S(110), S(40));
            }
        }
        y = add_text(mx, y + S(48), mw,
                     L"An edge gives once the pointer has pushed against it a little, so a window's Close button or a "
                     L"menu at the edge stays reachable. The last few pixels at each corner never cross. Touch crosses "
                     L"at once, as Nexus did before 0.5.",
                     g.small, C_INK3) + S(16);
        state_text(g.pause, L"Pause crossing: input stays on this PC", g.cfg.paused, FALSE);
        place(g.pause, mx, y, mw, S(32));
        y += S(36);
        state_text(g.fullscreen, L"Hold the edges while a full-screen app or game has focus", g.cfg.fullscreen_hold, FALSE);
        place(g.fullscreen, mx, y, mw, S(32));
        y = add_text(mx, y + S(36), mw, L"The keys on the Keyboard page still switch while the edges hold.", g.small,
                     C_INK3);
        break;
    case PAGE_KEYBOARD:
        if (!main_pc) {
            add_label(L_KEYCAP, mx, y, S(190), S(40), L"Set on the main PC", g.mono, C_INK, NULL);
            add_text(mx + S(210), y + S(10), mw - S(210), L"Switch input between the PCs.", g.body, C_INK2);
            y += S(56);
        } else {
            key_name(g.cfg.switch_vk, keycaps[MAX_PEERS], 64);
            if (!g.cfg.switch_hold) wcscat(keycaps[MAX_PEERS], L"  \x00d7 2");
            add_label(L_KEYCAP, mx, y, S(190), S(40), g.recording == 1 ? L"Press a key\x2026" : keycaps[MAX_PEERS],
                      g.mono, C_INK, NULL);
            add_text(mx + S(210), y + S(10), mw - S(330),
                     g.cfg.switch_hold ? L"Hold it: input is on the PC used last while the key is down."
                                       : L"Double-tap it: input goes to the PC used last, and back.",
                     g.body, C_INK2);
            place(g.switch_key, mx + mw - S(110), y, S(110), S(40));
            y += S(52);
            state_text(g.style[0], L"Double-tap", !g.cfg.switch_hold, TRUE);
            state_text(g.style[1], L"Hold", g.cfg.switch_hold, TRUE);
            place(g.style[0], mx, y, S(140), S(40));
            place(g.style[1], mx + S(148), y, S(140), S(40));
            y += S(56);
            int any = 0;
            for (int i = 0; i < MAX_PEERS; i++) {
                if (!g.peer[i].addr[0]) continue;
                if (!any++) y = add_text(mx, y, mw, L"Jump keys", g.button, C_INK) + S(8);
                if (g.recording == 2 + i) lstrcpynW(keycaps[i], L"Press the keys\x2026", 64);
                else if (g.peer[i].jump) key_name(g.peer[i].jump, keycaps[i], 64);
                else lstrcpynW(keycaps[i], L"None", 64);
                add_label(L_KEYCAP, mx, y, S(190), S(40), keycaps[i], g.mono, C_INK, NULL);
                _snwprintf(jump_text[i], 96, L"Straight to %ls, wherever input is.", name_of(i));
                jump_text[i][95] = 0;
                add_text(mx + S(210), y + S(10), mw - S(330), jump_text[i], g.body, C_INK2);
                place(g.jump[i], mx + mw - S(110), y, S(110), S(40));
                y += S(52);
            }
            if (any)
                y = add_text(mx, y, mw,
                             L"A jump key is Ctrl, Alt or Win with another key. Press Esc while one is being set to "
                             L"take it away. A PC that is asleep is woken up when it has been seen once.",
                             g.small, C_INK3) + S(16);
        }
        add_label(L_KEYCAP, mx, y, S(190), S(40), L"Ctrl + Alt + Pause", g.mono, C_INK, NULL);
        add_text(mx + S(210), y + S(10), mw - S(210), L"Bring input back to the main PC at once.", g.body, C_INK2);
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
                     main_pc ? L"The main PC has the keyboard and mouse, and plays the other PCs' sound."
                             : L"The controlled PC is driven by the main PC's keyboard and mouse.",
                     g.small, C_INK3) + S(18);
        {
            int fw = min(mw, S(380));
            y = add_field_row(mx, y, fw, g.pw_label, g.pw,
                              g.cfg.has_key ? L"Set. Type a new one and press Enter to change it."
                                            : L"Type the same password on every PC and press Enter.");
            if (main_pc) {
                /* Two columns of addresses: the PCs added so far and one empty field for the next. */
                int cw = min((mw - S(16)) / 2, S(300)), shown = 0, empty = 0;
                for (int i = 0; i < MAX_PEERS; i++) {
                    if (!g.peer[i].addr[0] && empty++) continue;
                    int fx = mx + (shown % 2) * (cw + S(16)), fyy = y + (shown / 2) * S(76);
                    place(g.addr_label[i], fx, fyy, cw, S(22));
                    add_label(L_WELL, fx, fyy + S(26), cw, S(40), NULL, NULL, 0, g.addr[i]);
                    place(g.addr[i], fx + S(12), fyy + S(26) + (S(40) - g.body_h) / 2, cw - S(24), g.body_h);
                    shown++;
                }
                y += (shown + 1) / 2 * S(76);
                y = add_text(mx, y, mw,
                             L"Each controlled PC shows its address on its own Connection page. Clear an address "
                             L"to remove that PC.",
                             g.small, C_INK3) + S(10);
                int listed = 0;
                for (int i = 0; i < MAX_FOUND; i++) {
                    BOOL added = FALSE;
                    wchar_t ip[16];
                    MultiByteToWideChar(CP_ACP, 0, g.found[i].ip, -1, ip, 16);
                    for (int k = 0; k < MAX_PEERS; k++) added |= g.found[i].ip[0] && !lstrcmpiW(ip, g.peer[k].addr);
                    if (g.found[i].ip[0] && g.found[i].role == ROLE_CONTROLLED && !added) {
                        static wchar_t labels[MAX_FOUND][64];
                        if (!listed++) y = add_text(mx, y, fw, L"Found on this network, click to add:", g.small, C_INK3) + S(4);
                        _snwprintf(labels[i], 64, L"%ls  \x00b7  %hs", g.found[i].name, g.found[i].ip);
                        labels[i][63] = 0;
                        SetWindowTextW(g.found_btn[i], labels[i]);
                        place(g.found_btn[i], mx, y, fw, S(28));
                        y += S(30);
                    }
                }
                if (listed) y += S(16);
            }
        }
        _snwprintf(address_line, 192, L"This PC's address: %ls  \x00b7  port %d", g.addresses, g.port);
        address_line[191] = 0;
        y = add_text(mx, y, mw, address_line, g.body, C_INK2);
        break;
    case PAGE_FILES:
        y = add_text(mx, y, mw,
                     L"Copy files or folders in Explorer, cross to another PC and press Ctrl+V. They travel when "
                     L"you cross and land in a dated folder under Nexus Received on its desktop, whatever their "
                     L"size.",
                     g.body, C_INK2);
        if (g.files_note[0]) y = add_text(mx, y + S(16), mw, g.files_note, g.body, C_INK);
        break;
    case PAGE_SOUND: {
        static wchar_t sound[400];
        int from = main_pc ? sound_from() : 0;
        const wchar_t *name = name_of(from);
        int state = from >= 0 ? g.peer[from].audio_state : AUDIO_UNKNOWN;
        if (overall() != NS_CONNECTED || from < 0)
            _snwprintf(sound, 400, main_pc ? L"Connect a controlled PC first; its sound then plays here."
                                           : L"Once the main PC connects, this PC's sound plays on its speakers.");
        else if (state == AUDIO_LOOPBACK || state == AUDIO_STEREO_MIX)
            _snwprintf(sound, 400,
                       main_pc ? L"%ls's sound plays on this PC's speakers%ls. Its own speakers keep playing too; "
                                 L"turn them down there if you only want to hear it here.%ls"
                               : L"This PC's sound plays on %ls's speakers%ls.%ls",
                       name, state == AUDIO_STEREO_MIX ? L", recorded from its sound card's Stereo Mix" : L"",
                       main_pc && peer_count() > 1 ? L" With several PCs, the sound comes from the one input "
                                                     L"went to last." : L"");
        else if (state == AUDIO_NONE)
            _snwprintf(sound, 400,
                       L"%ls cannot record what its speakers play. On Windows XP, turn on the Stereo Mix (or What U "
                       L"Hear) recording device in the Sounds and Audio Devices control panel; Nexus looks again "
                       L"every few seconds.",
                       main_pc ? name : L"This PC");
        else
            _snwprintf(sound, 400, L"Waiting for %ls to report its sound.", main_pc ? name : L"this PC");
        sound[399] = 0;
        y = add_text(mx, y, mw, sound, g.body, C_INK2);
        break;
    }
    default:
        y = add_text(mx, y, mw, L"Coming in a later version.", g.body, C_INK2);
    }
    SetRect(&g.module_rc, x0, my, x0 + width, y + S(28));
    ReleaseDC(g.wnd, lay_dc);
    /* The page scrolls when it is taller than the space above the footer. */
    g.content_h = y + S(28) + S(40) + g.scroll;
    SCROLLINFO si = {sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, g.content_h - 1, (UINT)max(1, fy), g.scroll, 0};
    SetScrollInfo(g.wnd, SB_VERT, &si, TRUE);
}

/* Places everything; showing or hiding the scroll bar changes the width, so then once more. */
static void layout(void)
{
    static BOOL busy; /* SetScrollInfo can send WM_SIZE from inside */
    if (busy) return;
    busy = TRUE;
    for (int i = 0; i < 3; i++) {
        RECT before, after;
        GetClientRect(g.wnd, &before);
        int most = max(0, g.content_h - (before.bottom - S(FOOTER)));
        if (g.scroll > most) g.scroll = most;
        layout_page();
        GetClientRect(g.wnd, &after);
        if (after.right == before.right && g.scroll <= max(0, g.content_h - (after.bottom - S(FOOTER)))) break;
    }
    busy = FALSE;
}

static void scroll_to(int y)
{
    RECT c;
    GetClientRect(g.wnd, &c);
    y = max(0, min(y, g.content_h - (c.bottom - S(FOOTER))));
    if (y == g.scroll) return;
    g.scroll = y;
    layout();
    redraw(NULL);
}

/* Tab moved the focus to a control the page has scrolled away. */
static void scroll_into_view(HWND w)
{
    RECT r, c;
    if (!w || GetParent(w) != g.wnd) return;
    GetWindowRect(w, &r);
    MapWindowPoints(NULL, g.wnd, (POINT *)&r, 2);
    GetClientRect(g.wnd, &c);
    if (r.left < S(SIDEBAR)) return; /* the sidebar does not scroll */
    int fy = c.bottom - S(FOOTER);
    if (r.bottom > fy - S(16)) scroll_to(g.scroll + r.bottom - fy + S(16));
    else if (r.top < S(16)) scroll_to(g.scroll + r.top - S(16));
}

/* The window's picture is drawn once into a cached bitmap and only redrawn when its content changes
 * (redraw()). A window dragged across it on Windows XP, which has no compositor, then costs a copy of
 * the uncovered strip instead of a full redraw (measured 100-500 ms in an XP virtual machine). */
static struct { HBITMAP bmp; int w, h; BOOL dirty, all; RECT part; } cache;

/* r: the part whose content changed (the once-a-second response time); NULL: everything. */
static void redraw(const RECT *r)
{
    if (r) UnionRect(&cache.part, &cache.part, r);
    else cache.all = TRUE;
    cache.dirty = TRUE;
    InvalidateRect(g.wnd, r, FALSE);
}

static void render(HDC dc, RECT c)
{
    const Palette *p = g.p;
    fill(dc, &c, p->ground);

    int fy = c.bottom - S(FOOTER);
    RECT r;
    SetRect(&r, S(SIDEBAR), 0, S(SIDEBAR) + 1, fy), fill(dc, &r, p->rule);
    SetRect(&r, 0, S(STATUS_H), S(SIDEBAR), S(STATUS_H) + 1), fill(dc, &r, p->rule);

    /* Status block: where input is and how the link is. */
    status_light(dc, S(28), S(37), S(12), status_color());
    SetRect(&r, S(52), S(30), S(SIDEBAR - 16), S(56));
    text(dc, g.line, r, g.status_font, p->ink, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SetRect(&r, S(52), S(56), S(SIDEBAR - 16), S(80));
    BOOL main_pc = g.cfg.role == ROLE_MAIN;
    int shown = shown_peer(), connected = 0;
    for (int i = 0; main_pc && i < MAX_PEERS; i++) connected += g.peer[i].addr[0] && up(i);
    for (int i = 0; main_pc && shown < 0 && i < MAX_PEERS; i++)
        if (g.peer[i].addr[0]) shown = i;
    wchar_t who[64];
    if (connected > 1 && !g.remote) _snwprintf(who, 64, L"%d PCs connected", connected);
    else if (shown >= 0 && (main_pc || overall() == NS_CONNECTED)) lstrcpynW(who, name_of(shown), 64);
    else lstrcpynW(who, main_pc ? L"Main PC" : L"Controlled PC", 64);
    who[63] = 0;
    text(dc, who, r, g.body, p->ink2, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    wchar_t rtt[32] = L"Response: \x2014";
    if (shown >= 0 && up(shown) && g.peer[shown].rtt >= 0) _snwprintf(rtt, 32, L"Response: %d ms", g.peer[shown].rtt);
    SetRect(&r, S(28), S(92), S(SIDEBAR - 16), S(116));
    text(dc, rtt, r, g.body, p->ink2, DT_SINGLELINE | DT_VCENTER);

    SetRect(&r, S(28), fy - S(72), S(SIDEBAR - 16), fy - S(46));
    text(dc, L"Nexus " WIDEN(NEXUS_VERSION), r, g.small, p->ink3, DT_SINGLELINE | DT_VCENTER);

    /* Page */
    SetRect(&r, g.desc_rc.left, S(40) - g.scroll, g.desc_rc.right, S(80) - g.scroll);
    text(dc, PAGES[g.page].title, r, g.title, p->ink, DT_SINGLELINE | DT_VCENTER);
    text(dc, PAGES[g.page].desc, g.desc_rc, g.body, p->ink2, DT_WORDBREAK);
    round_rect(dc, &g.module_rc, S(12), p->panel, p->rule, S(1));
    for (int i = 0; i < g.nlabels; i++) {
        const Label *l = &g.labels[i];
        switch (l->kind) {
        case L_TEXT: text(dc, l->s, l->r, l->f, color(l->color), DT_WORDBREAK); break;
        case L_LINE:
            text(dc, l->s, l->r, l->f, color(l->color), DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            break;
        case L_DOT: status_light(dc, l->r.left, l->r.top, l->r.right - l->r.left, l->color); break;
        case L_WELL:
            round_rect(dc, &l->r, S(8), p->well, GetFocus() == l->edit ? p->signal : p->edge, S(1));
            break;
        case L_KEYCAP:
            round_rect(dc, &l->r, S(8), p->well, p->edge, S(1));
            text(dc, l->s, l->r, l->f, color(l->color), DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            break;
        case L_CELL: round_rect(dc, &l->r, S(10), NO_FILL, p->rule, S(1)); break;
        case L_HOME: {
            RECT t = l->r;
            round_rect(dc, &l->r, S(10), p->well, p->signal, S(2));
            t.top += S(8), t.bottom = t.top + S(22), t.left += S(12), t.right -= S(8);
            text(dc, l->s, t, l->f, p->ink, DT_SINGLELINE | DT_VCENTER);
            OffsetRect(&t, 0, S(22));
            text(dc, g.my_name_w, t, g.small, p->ink3, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            break;
        }
        }
    }

    /* Footer last: the page scrolls under it. */
    SetRect(&r, 0, fy, c.right, c.bottom), fill(dc, &r, p->ground);
    SetRect(&r, 0, fy, c.right, fy + 1), fill(dc, &r, p->rule);
    SetRect(&r, S(28), fy + S(18), c.right - S(260), c.bottom - S(18));
    text(dc, L"Changes apply as you make them. Closing this window keeps Nexus running in the tray.", r,
         g.body, p->ink2, DT_WORDBREAK);
}

static void paint(HDC win, const RECT *dirty_part)
{
    RECT c;
    GetClientRect(g.wnd, &c);
    if (!cache.bmp || cache.w != c.right || cache.h != c.bottom) {
        if (cache.bmp) DeleteObject(cache.bmp);
        cache.bmp = CreateCompatibleBitmap(win, c.right, c.bottom);
        cache.w = c.right, cache.h = c.bottom, cache.dirty = cache.all = TRUE;
    }
    HDC dc = CreateCompatibleDC(win);
    HGDIOBJ old = SelectObject(dc, cache.bmp);
    if (cache.dirty) {
        if (!cache.all) { /* clipped, so GDI skips drawing everything outside the changed part */
            HRGN part = CreateRectRgnIndirect(&cache.part);
            SelectClipRgn(dc, part);
            DeleteObject(part);
        }
        render(dc, c);
        SelectClipRgn(dc, NULL);
        SetRectEmpty(&cache.part);
        cache.dirty = cache.all = FALSE;
    }
    BitBlt(win, dirty_part->left, dirty_part->top, dirty_part->right - dirty_part->left,
           dirty_part->bottom - dirty_part->top, dc, dirty_part->left, dirty_part->top, SRCCOPY);
    SelectObject(dc, old);
    DeleteDC(dc);
}

static void refresh(void)
{
    status_text();
    layout();
    redraw(NULL);
    for (int i = 0; i < NPAGES; i++) InvalidateRect(g.nav[i], NULL, FALSE);
    for (int i = 0; i < MAX_PEERS; i++) InvalidateRect(g.tile[i], NULL, FALSE);
    for (int i = 0; i < 2; i++) InvalidateRect(g.role[i], NULL, FALSE);
}

static void set_page(int page)
{
    if (page < 0 || page >= NPAGES) return;
    g.page = page;
    g.scroll = 0;
    if (g.recording) g.recording = 0, input_main_record(0);
    if (page == PAGE_CONNECTION) this_pc_addresses();
    refresh();
}

/* ---- applying settings ---- */

static void sync_input(void)
{
    BOOL main_pc = g.cfg.role == ROLE_MAIN;
    input_main_set(main_pc && first_connected() >= 0, main_pc ? exits(THIS_PC) : 0);
}

/* A controlled PC's address; empty removes it. A new one gets a free cell beside this PC. */
static void set_address(int pc, const wchar_t *addr)
{
    Peer *p = &g.peer[pc];
    if (lstrcmpiW(addr, p->addr) == 0) return;
    BOOL added = !p->addr[0];
    EnterCriticalSection(&g.cs);
    lstrcpynW(p->addr, addr, 128);
    LeaveCriticalSection(&g.cs);
    if (added) free_cell(pc);
    p->name[0] = 0;
    SetWindowTextW(g.addr[pc], p->addr);
    config_save();
    net_restart(pc);
    sync_input();
    refresh();
}

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
        config_save();
        net_restart(-1);
        refresh();
        return;
    }
    for (int pc = 0; pc < MAX_PEERS; pc++) {
        if (edit != g.addr[pc]) continue;
        /* Only what a host name or address can hold. */
        wchar_t *s = v, *e;
        while (*s == L' ') s++;
        for (e = s; *e && *e != L' '; e++)
            if (!iswalnum(*e) && *e != L'.' && *e != L'-') return;
        *e = 0;
        set_address(pc, s);
    }
}

/* Drags and arrow keys on the map end here; a PC already in that cell swaps places. */
static void move_peer(int pc, int x, int y)
{
    int other = peer_at(x, y);
    if (on_map(x, y) && other != THIS_PC && other != pc) {
        if (other >= 0) g.peer[other].x = g.peer[pc].x, g.peer[other].y = g.peer[pc].y;
        g.peer[pc].x = x, g.peer[pc].y = y;
        config_save();
        sync_input();
    }
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
    net_restart(-1);
    sync_input();
    refresh();
}

static void save_and_sync(void)
{
    config_save();
    sync_keys();
    refresh();
}

static void open_log_folder(void)
{
    wchar_t dir[MAX_PATH];
    lstrcpynW(dir, g.ini, MAX_PATH);
    *wcsrchr(dir, L'\\') = 0;
    ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

/* Wake-on-LAN: the magic packet (6 x FF, then the MAC 16 times) to every local network's broadcast.
 * The MAC comes from SendARP after a connection, never from parsing "arp -a". Port of Beamer core/wol.py. */
static void wake(int pc)
{
    unsigned char pkt[102];
    struct sockaddr_in to = {0};
    BOOL on = TRUE;
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) memcpy(pkt + 6 + 6 * i, g.peer[pc].mac, 6);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof on);
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(s, (const char *)pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to);
    for (int i = 0; i < 4 && g.broadcast[i]; i++) {
        to.sin_addr.s_addr = g.broadcast[i];
        sendto(s, (const char *)pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to);
    }
    closesocket(s);
    wchar_t t[160];
    _snwprintf(t, 160, L"Waking %ls. It connects by itself once it is up.", name_of(pc));
    t[159] = 0;
    notify(L"Wake-on-LAN", t);
}

/* A jump key or the tray named a PC that is not there: wake it if it can be, else say so. */
static void wake_or_tell(int pc)
{
    if (has_mac(pc)) return wake(pc);
    wchar_t t[160];
    _snwprintf(t, 160, L"%ls is not connected.", name_of(pc));
    t[159] = 0;
    notify(L"Nexus", t);
}

static void about(void)
{
    MessageBoxW(g.wnd,
                L"Nexus " WIDEN(NEXUS_VERSION) L"\n\nOne keyboard, mouse and sound for PCs from Windows XP to 11 and "
                L"Linux.\n\n" SITE_URL L"\n\nGPL-3.0-or-later. The look and much of the crossing logic come from Beamer "
                L"by Toby George Read Kalkman (GPL-3.0); the fonts are Hanken Grotesk and B612 Mono (SIL OFL).",
                L"About Nexus", MB_OK | MB_ICONINFORMATION);
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
    g.icon_here = badge_icon(NO_FILL);
    g.icon_away = badge_icon(DARK.signal);
    g.icon_paused = badge_icon(DARK.amber);
    g.tray.hIcon = g.icon_here;
    lstrcpynW(g.tray.szTip, L"Nexus", 64);
    Shell_NotifyIconW(NIM_ADD, &g.tray);
}

static void tray_menu(void)
{
    POINT pt;
    HMENU m = CreatePopupMenu();
    wchar_t head[128], wake_label[MAX_PEERS][80];
    _snwprintf(head, 128, L"Nexus " WIDEN(NEXUS_VERSION) L"  \x00b7  %ls", g.line);
    head[127] = 0;
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, head);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Open Nexus");
    if (g.cfg.role == ROLE_MAIN) {
        AppendMenuW(m, MF_STRING | (first_connected() >= 0 ? 0 : MF_GRAYED), ID_TRAY_SEND,
                    g.remote ? L"Bring input back" : L"Send input across");
        AppendMenuW(m, MF_STRING | (g.cfg.paused ? MF_CHECKED : 0), ID_TRAY_PAUSE, L"Pause crossing");
        for (int i = 0; i < MAX_PEERS; i++)
            if (g.peer[i].addr[0] && !up(i) && has_mac(i)) {
                _snwprintf(wake_label[i], 80, L"Wake %ls", name_of(i));
                wake_label[i][79] = 0;
                AppendMenuW(m, MF_STRING, ID_TRAY_WAKE0 + i, wake_label[i]);
            }
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_TRAY_LOGS, L"Open log folder");
    AppendMenuW(m, MF_STRING, ID_TRAY_ABOUT, L"About Nexus");
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
    log_line("quit");
    input_release_all();
    input_hide_pointer(FALSE);
    WritePrivateProfileStringW(L"nexus", L"pointer_hidden", NULL, g.ini);
    Shell_NotifyIconW(NIM_DELETE, &g.tray);
    DestroyWindow(g.wnd);
}

/* ---- clipboard: the PC giving up input sends its text, so a copy there pastes here ---- */

static DWORD clip_seq; /* the clipboard as Nexus last sent or set it: only a newer copy travels */

/* Another program often holds the clipboard for a moment right after a copy (Windows 10/11's
 * clipboard history does), so one refusal is not an answer. */
static BOOL open_clipboard(void)
{
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(g.wnd)) return TRUE;
        Sleep(20);
    }
    return FALSE;
}

/* Text and a picture go together and the app that pastes picks (0.5 peers: text up to 256 KB, a CF_DIB
 * picture up to 8 MB, in pieces; over a limit that part stays behind, never cut short). An older peer
 * gets text up to 64 KB in one CLIPBOARD message, as before. */
static void clipboard_send(int pc)
{
    if (GetClipboardSequenceNumber() == clip_seq || !open_clipboard()) return;
    BOOL v5 = link_v5(g.peer[pc].version);
    HANDLE h = GetClipboardData(CF_UNICODETEXT), drop = GetClipboardData(CF_HDROP);
    HANDLE dib = v5 && !drop ? GetClipboardData(CF_DIB) : NULL;
    const wchar_t *w = h && !drop ? GlobalLock(h) : NULL;
    char *text = NULL;
    unsigned char *picture = NULL;
    int text_n = 0;
    unsigned int picture_n = 0;
    /* Files copied in Explorer go instead of text; a transfer still running keeps the new copy back. */
    if (drop && !files_send((HDROP)drop, name_of(pc), pc))
        lstrcpynW(g.files_note, L"Still sending the last files; copy the new ones again once they are across.", 256);
    if (w) {
        text_n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL) - 1; /* without the NUL */
        int limit = v5 ? CLIP_TEXT_MAX : LINK_MAX - 1;
        if (text_n > 0 && text_n <= limit && (text = malloc(text_n + 1)))
            WideCharToMultiByte(CP_UTF8, 0, w, -1, text, text_n + 1, NULL, NULL);
        else if (text_n > limit) log_line("clipboard: %d bytes of text stay here (over %d)", text_n, limit);
        GlobalUnlock(h);
    }
    const void *d = dib ? GlobalLock(dib) : NULL;
    if (d) {
        SIZE_T n = GlobalSize(dib);
        if (n && n <= CLIP_IMAGE_MAX && (picture = malloc(n))) memcpy(picture, d, n), picture_n = (unsigned int)n;
        else if (n > CLIP_IMAGE_MAX) log_line("clipboard: a %lu-byte picture stays here (over 8 MB)", (unsigned long)n);
        GlobalUnlock(dib);
    }
    CloseClipboard(); /* copied out first, so other apps are not kept waiting while it travels */
    clip_seq = GetClipboardSequenceNumber();
    if (v5 && (text || picture)) {
        if (text) clip_out(CLIP_TEXT, text, text_n, net_send, pc);
        if (picture) clip_out(CLIP_DIB, picture, picture_n, net_send, pc);
        unsigned char end = MSG_CLIP_END;
        net_send(pc, &end, 1);
        log_line("clipboard: sent %d bytes of text and a %u-byte picture to link %d", text ? text_n : 0, picture_n, pc);
    } else if (text) {
        unsigned char *msg = malloc(text_n + 1);
        if (msg) {
            msg[0] = MSG_CLIPBOARD;
            memcpy(msg + 1, text, text_n);
            net_send(pc, msg, text_n + 1);
            free(msg);
        }
    }
    free(text);
    free(picture);
}

/* A complete set of clipboard pieces (0.5): every complete part goes on the clipboard at once. */
static void clipboard_take(ClipIn *in)
{
    BOOL has_text = in->data[CLIP_TEXT] && in->got[CLIP_TEXT] == in->size[CLIP_TEXT];
    BOOL has_dib = in->data[CLIP_DIB] && in->got[CLIP_DIB] == in->size[CLIP_DIB] &&
                   clip_dib_ok(in->data[CLIP_DIB], in->size[CLIP_DIB]);
    HGLOBAL t = NULL, p = NULL;
    if (has_text) {
        int n = (int)in->size[CLIP_TEXT];
        int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)in->data[CLIP_TEXT], n, NULL, 0);
        if (wn > 0 && (t = GlobalAlloc(GMEM_MOVEABLE, (wn + 1) * sizeof(wchar_t)))) { /* not valid UTF-8: dropped */
            wchar_t *w = GlobalLock(t);
            MultiByteToWideChar(CP_UTF8, 0, (const char *)in->data[CLIP_TEXT], n, w, wn);
            w[wn] = 0;
            GlobalUnlock(t);
        }
    }
    if (has_dib && (p = GlobalAlloc(GMEM_MOVEABLE, in->size[CLIP_DIB]))) {
        memcpy(GlobalLock(p), in->data[CLIP_DIB], in->size[CLIP_DIB]);
        GlobalUnlock(p);
    }
    if (!t && !p) return;
    if (!open_clipboard()) {
        if (t) GlobalFree(t);
        if (p) GlobalFree(p);
        return;
    }
    EmptyClipboard();
    if (t && !SetClipboardData(CF_UNICODETEXT, t)) GlobalFree(t);
    if (p && !SetClipboardData(CF_DIB, p)) GlobalFree(p);
    CloseClipboard();
    clip_seq = GetClipboardSequenceNumber();
    log_line("clipboard: received %u bytes of text and a %u-byte picture", t ? in->size[CLIP_TEXT] : 0,
             p ? in->size[CLIP_DIB] : 0);
}

/* Takes h whatever happens: it goes on the clipboard or is freed. */
static void clipboard_put(UINT format, HGLOBAL h)
{
    if (!open_clipboard()) {
        GlobalFree(h);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(format, h)) GlobalFree(h);
    CloseClipboard();
    clip_seq = GetClipboardSequenceNumber();
}

/* Received files go on the clipboard as Explorer puts copied files there, so Ctrl+V pastes them. */
static void clipboard_set_files(const wchar_t *paths)
{
    size_t n = 0;
    while (paths[n]) n += wcslen(paths + n) + 1;
    HGLOBAL h = GlobalAlloc(GHND, sizeof(DROPFILES) + (n + 1) * sizeof(wchar_t));
    if (!h) return;
    DROPFILES *d = GlobalLock(h);
    d->pFiles = sizeof(DROPFILES);
    d->fWide = TRUE;
    memcpy(d + 1, paths, (n + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    clipboard_put(CF_HDROP, h);
}

static void clipboard_set(const unsigned char *s, int n)
{
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)s, n, NULL, 0);
    if (wn <= 0) return; /* not valid UTF-8: not from Nexus */
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (wn + 1) * sizeof(wchar_t));
    if (!h) return;
    wchar_t *w = GlobalLock(h);
    MultiByteToWideChar(CP_UTF8, 0, (const char *)s, n, w, wn);
    w[wn] = 0;
    GlobalUnlock(h);
    clipboard_put(CF_UNICODETEXT, h);
}

/* The capture thread's messages go out as they are; its state also shows on this PC's Sound page. */
static void audio_out(const unsigned char *msg, int len)
{
    if (msg[0] == MSG_AUDIO_STATE) PostMessageW(g.wnd, WM_AUDIO_STATE, msg[1], 0);
    net_send(0, msg, len);
}

static BOOL is_main_pc(void) { return g.cfg.role == ROLE_MAIN; }

/* Main PC, on connecting: a controlled PC with an older Nexus gets this PC's own installer. */
static void update_peer(int pc)
{
    wchar_t path[MAX_PATH];
    const Peer *p = &g.peer[pc];
    if (!p->version[0])
        _snwprintf(g.update_note, 256, L"%ls runs a Nexus too old to be updated from here. Install "
                                       L"Nexus " WIDEN(NEXUS_VERSION) L" there by hand once.", name_of(pc));
    else if (version_cmp(p->version, NEXUS_VERSION) < 0 && update_installer(NEXUS_VERSION, path) &&
             update_push(NEXUS_VERSION, net_send, pc))
        _snwprintf(g.update_note, 256, L"Updating %ls to Nexus " WIDEN(NEXUS_VERSION) L"\x2026", name_of(pc));
    /* ponytail: one push at a time; a PC that finds it busy gets the update on its next connection */
    g.update_note[255] = 0;
}

/* The sidebar button: a controlled PC that is behind first, then this PC (its installer restarts Nexus,
 * and the other PCs get the new version when they reconnect). */
static void install_update(void)
{
    wchar_t path[MAX_PATH];
    if (!update_installer(g.update_version, path)) return;
    g.installing = TRUE;
    for (int i = 0; i < MAX_PEERS; i++) {
        const Peer *p = &g.peer[i];
        if (p->addr[0] && up(i) && p->version[0] && version_cmp(p->version, g.update_version) < 0 &&
            update_push(g.update_version, net_send, i)) {
            _snwprintf(g.update_note, 256, L"Sending Nexus %hs to %ls first\x2026", g.update_version, name_of(i));
            g.update_note[255] = 0;
            refresh();
            return;
        }
    }
    update_run(path);
    refresh();
}

/* ---- crossing between the PCs on the map (main PC) ---- */

/* The edge of this PC that faces pc, when it stands beside it; else -1. */
static int edge_toward(int pc)
{
    for (int e = 0; e < 4; e++)
        if (neighbour(THIS_PC, e) == pc) return e;
    return -1;
}

/* Input goes to pc, moving in direction edge, at frac along the edge it comes in by (keep: the pointer
 * stays where it is over there, for the shortcut). */
static void enter_peer(int pc, int edge, BOOL keep, int frac)
{
    unsigned char m[7] = {MSG_ENTER, (BYTE)edge, (BYTE)keep, (BYTE)frac, (BYTE)(frac >> 8), (BYTE)exits(pc),
                          (BYTE)(g.cfg.resistance / 4)};
    clipboard_send(pc);
    /* Before 0.3 a controlled PC knew one way back only, and takes no sixth byte; before 0.5 no resistance. */
    net_send(pc, m, link_v5(g.peer[pc].version) ? 7 : version_cmp(g.peer[pc].version, "0.3.0") >= 0 ? 6 : 5);
    g.target = g.last = pc;
    log_line("input to link %d", pc);
}

/* The hook thread says input left this PC: through an edge, by the switch key (keep), or by PC jump's
 * jump key (-1: none). A jump key while input is already across goes from that PC straight to jump. */
static void cross_from_here(int edge, BOOL keep, int frac, int jump)
{
    if (jump >= 0 && g.target >= 0) {
        if (!up(jump)) wake_or_tell(jump);
        else if (jump != g.target) {
            go_home();
            enter_peer(jump, edge_toward(jump) >= 0 ? edge_toward(jump) : EDGE_RIGHT, TRUE, 0);
        }
        refresh();
        return;
    }
    int to = jump >= 0 ? jump : keep ? (g.last >= 0 && up(g.last) ? g.last : first_connected()) : neighbour(THIS_PC, edge);
    if (to < 0 || to == THIS_PC || !up(to)) { /* it went away meanwhile, or it is asleep */
        if (jump >= 0) wake_or_tell(jump);
        input_main_leave(-1, -1);
        return;
    }
    if (keep) edge = edge_toward(to) >= 0 ? edge_toward(to) : EDGE_RIGHT;
    enter_peer(to, edge, keep, frac);
    refresh();
}

/* The PC with input let it go through an edge (edge -1: the way it came, from a Nexus before 0.3):
 * on to the PC past that edge, or home. */
static void cross_on(int from, int edge, int frac)
{
    if (edge < 0 && edge_toward(from) >= 0) edge = edge_toward(from) ^ 1;
    int next = edge < 0 ? THIS_PC : neighbour(from, edge);
    if (next >= 0 && next != THIS_PC && up(next)) {
        clip_seq = 0; /* what was copied over there goes on, not only what was copied here */
        enter_peer(next, edge, FALSE, frac < 0 ? 32767 : frac);
    } else {
        g.target = -1;
        input_main_leave(next == THIS_PC ? edge ^ 1 : -1, frac);
        if (g.test) g.remote = FALSE; /* no hook thread to report it */
    }
    refresh();
}

static void go_home(void)
{
    unsigned char leave[4] = {MSG_LEAVE, 0, 0, 0};
    net_send(g.target, leave, 4);
    g.target = -1;
}

static void ping(int pc)
{
    DWORD t = GetTickCount();
    unsigned char m[5] = {MSG_PING, (BYTE)t, (BYTE)(t >> 8), (BYTE)(t >> 16), (BYTE)(t >> 24)};
    net_send(pc, m, 5);
}

/* Every 250 ms. While input is across, a link that has been silent for 2 s is gone: input comes home at
 * once instead of after the 6 s socket timeout (port of Beamer's ACK heartbeat, core/receiver.py). A
 * controlled PC pings too while it has input, so a big transfer the other way never makes it look dead.
 * It also checks for a locked desktop (port of Beamer win_app/desktop_win.py) and keeps its pointer
 * hidden while input is elsewhere. */
static void beat(void)
{
    static int locked, hidden;
    DWORD now = GetTickCount();
    if (g.cfg.role == ROLE_MAIN && g.target >= 0) {
        int pc = g.target;
        Peer *p = &g.peer[pc];
        ping(pc);
        if (now - p->heard > LINK_DEAD_MS) {
            wchar_t t[200];
            log_line("link %d: silent for %lu ms while it had input; input comes home", pc, (unsigned long)(now - p->heard));
            g.target = -1;
            input_main_leave(-1, -1);
            if (g.test) g.remote = FALSE;
            EnterCriticalSection(&g.cs);
            if (p->conn != INVALID_SOCKET) shutdown(p->conn, SD_BOTH); /* its thread reconnects */
            LeaveCriticalSection(&g.cs);
            _snwprintf(t, 200, L"%ls stopped answering, so input is back on this PC.", name_of(pc));
            t[199] = 0;
            notify(L"Input came home", t);
            refresh();
        }
    }
    BOOL connected = g.cfg.role == ROLE_CONTROLLED && g.peer[0].status == NS_CONNECTED;
    if (connected && input_controlled_active()) {
        ping(0);
        BOOL stop = now - g.peer[0].heard > LINK_DEAD_MS;
        locked = input_desktop_unlocked() ? 0 : locked + 1;
        if (locked >= 2) { /* twice in a row, 250 ms apart: not a flicker */
            log_line("this PC's desktop is locked: input goes back to the main PC");
            unsigned char leave[4] = {MSG_LEAVE, 0, 0, 0}, said = MSG_LOCKED;
            if (link_v5(g.peer[0].version)) net_send(0, &said, 1);
            else net_send(0, leave, 4); /* an older main PC: the way it came */
            stop = TRUE, locked = 0;
        }
        if (stop) input_controlled_stop(), refresh();
    }
    int hide = connected && g.cfg.hide_pointer && !input_controlled_active();
    if (hide != hidden) {
        hidden = hide;
        input_hide_pointer(hide);
        /* A mark in the config, so a crash that left it hidden is undone on the next start. */
        WritePrivateProfileStringW(L"nexus", L"pointer_hidden", hide ? L"1" : NULL, g.ini);
    }
}

static void on_net_msg(int pc, unsigned char *msg, int len)
{
    Peer *p = &g.peer[pc];
    if (msg[0] == MSG_CLIPBOARD && len > 1) {
        clipboard_set(msg + 1, len - 1);
    } else if (msg[0] >= MSG_CLIP_BEGIN && msg[0] <= MSG_CLIP_END) {
        if (clip_in(&g.clip_in, msg, len)) {
            clipboard_take(&g.clip_in);
            clip_in_free(&g.clip_in);
        }
    } else if (msg[0] == MSG_LOCKED && len == 1 && g.cfg.role == ROLE_MAIN && pc == g.target) {
        wchar_t t[200];
        g.target = -1;
        input_main_leave(-1, -1);
        if (g.test) g.remote = FALSE;
        _snwprintf(t, 200, L"Windows on %ls is locked, so input is back here. Unlock it at that PC.", name_of(pc));
        t[199] = 0;
        notify(L"Input came home", t);
        refresh();
    } else if (msg[0] == MSG_PONG && len == 5) {
        DWORD sent = msg[1] | msg[2] << 8 | msg[3] << 16 | (DWORD)msg[4] << 24;
        p->rtt = (int)(GetTickCount() - sent);
        RECT r = {0, 0, S(SIDEBAR), S(STATUS_H)};
        redraw(&r);
    } else if (g.cfg.role == ROLE_MAIN) {
        if (msg[0] == MSG_LEAVE && (len == 4 || len == 5) && pc == g.target)
            cross_on(pc, len == 5 && msg[4] <= EDGE_BOTTOM ? msg[4] : -1, msg[1] ? msg[2] | msg[3] << 8 : -1);
        else if (msg[0] == MSG_AUDIO && pc == sound_from())
            audio_play(msg, len);
        else if (msg[0] == MSG_AUDIO_STATE && len == 2 && msg[1] <= AUDIO_NONE && msg[1] != p->audio_state)
            p->audio_state = msg[1], refresh();
    } else {
        unsigned char out[8];
        BOOL was = input_controlled_active();
        int n = input_replay(msg, len, out);
        if (was && !input_controlled_active()) clipboard_send(0); /* ahead of LEAVE, so it is there on arrival */
        if (n) net_send(0, out, n);
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
        g.pw = add_field(L"Shared password", ID_PASSWORD_LABEL, ID_PASSWORD, ES_PASSWORD, &g.pw_label);
        for (int i = 0; i < MAX_PEERS; i++) {
            static const wchar_t *labels[] = {L"Controlled PC 1", L"Controlled PC 2", L"Controlled PC 3",
                                              L"Controlled PC 4"};
            g.addr[i] = add_field(labels[i], ID_ADDR_LABEL0 + i, ID_ADDR0 + i, 0, &g.addr_label[i]);
            SetWindowTextW(g.addr[i], g.peer[i].addr);
            g.tile[i] = add_button(L"", ID_TILE0 + i, K_TILE);
        }
        g.site = add_button(L"Nexus on GitHub", ID_SITE, K_LINK);
        for (int i = 0; i < MAX_FOUND; i++) g.found_btn[i] = add_button(L"", ID_FOUND0 + i, K_LINK);
        g.update = add_button(L"", ID_UPDATE, K_LINK);
        g.quit = add_button(L"Quit Nexus", ID_QUIT, K_OUTLINE);
        for (int i = 0; i < 4; i++) g.res[i] = add_button(L"", ID_RES0 + i, K_SEGMENT);
        g.pause = add_button(L"", ID_PAUSE, K_TOGGLE);
        g.fullscreen = add_button(L"", ID_FULLSCREEN, K_TOGGLE);
        g.hide_pointer = add_button(L"", ID_HIDE_POINTER, K_TOGGLE);
        g.logs = add_button(L"Open log folder", ID_LOGS, K_LINK);
        g.switch_key = add_button(L"Change", ID_SWITCH_KEY, K_OUTLINE);
        g.style[0] = add_button(L"", ID_STYLE_TAP, K_SEGMENT);
        g.style[1] = add_button(L"", ID_STYLE_HOLD, K_SEGMENT);
        for (int i = 0; i < MAX_PEERS; i++) {
            g.jump[i] = add_button(L"Set", ID_JUMP0 + i, K_OUTLINE);
            g.wake[i] = add_button(L"", ID_WAKE0 + i, K_LINK);
        }
        apply_theme();
        tray_add();
        status_text(); /* the first status the network reports may equal the initial one */
        SetTimer(w, TIMER_PING, 1000, NULL);
        SetTimer(w, TIMER_BEAT, 250, NULL);
        return 0;
    case WM_MOUSEWHEEL:
        scroll_to(g.scroll - (short)HIWORD(wp) * S(48) / WHEEL_DELTA);
        return 0;
    case WM_VSCROLL: {
        RECT c;
        GetClientRect(w, &c);
        int page = c.bottom - S(FOOTER), to = g.scroll;
        switch (LOWORD(wp)) {
        case SB_LINEUP: to -= S(48); break;
        case SB_LINEDOWN: to += S(48); break;
        case SB_PAGEUP: to -= page; break;
        case SB_PAGEDOWN: to += page; break;
        case SB_TOP: to = 0; break;
        case SB_BOTTOM: to = g.content_h; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: {
            SCROLLINFO si = {sizeof si, SIF_TRACKPOS, 0, 0, 0, 0, 0};
            GetScrollInfo(w, SB_VERT, &si);
            to = si.nTrackPos;
            break;
        }
        }
        scroll_to(to);
        return 0;
    }
    case WM_INPUT_KEY: { /* the key the Keyboard page asked for; 0: Esc */
        int what = g.recording;
        g.recording = 0;
        if (what == 1 && wp) g.cfg.switch_vk = (BYTE)wp;
        if (what >= 2) {
            for (int i = 0; i < MAX_PEERS && wp; i++)
                if (g.peer[i].jump == (WORD)wp) g.peer[i].jump = 0; /* one chord, one PC */
            g.peer[what - 2].jump = (WORD)wp;
        }
        save_and_sync();
        return 0;
    }
    case WM_SIZE:
        layout();
        redraw(NULL);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(760);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(640);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        paint(BeginPaint(w, &ps), &ps.rcPaint);
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
        else if (id >= ID_RES0 && id <= ID_RES3) g.cfg.resistance = RESISTANCE[id - ID_RES0], save_and_sync();
        else if (id == ID_PAUSE || id == ID_TRAY_PAUSE) {
            g.cfg.paused = !g.cfg.paused; /* for this session only */
            log_line("crossing %s", g.cfg.paused ? "paused" : "resumed");
            sync_keys(), refresh();
        } else if (id == ID_FULLSCREEN) g.cfg.fullscreen_hold = !g.cfg.fullscreen_hold, save_and_sync();
        else if (id == ID_HIDE_POINTER) {
            g.cfg.hide_pointer = !g.cfg.hide_pointer;
            save_and_sync(); /* the heartbeat timer hides or shows it */
        } else if (id == ID_STYLE_TAP || id == ID_STYLE_HOLD) g.cfg.switch_hold = id == ID_STYLE_HOLD, save_and_sync();
        else if (id == ID_SWITCH_KEY || (id >= ID_JUMP0 && id <= ID_JUMP3)) {
            g.recording = id == ID_SWITCH_KEY ? 1 : 2 + id - ID_JUMP0;
            input_main_record(id == ID_SWITCH_KEY ? 1 : 2);
            refresh();
        } else if (id >= ID_WAKE0 && id <= ID_WAKE3) wake(id - ID_WAKE0);
        else if (id >= ID_TRAY_WAKE0 && id <= ID_TRAY_WAKE3) wake(id - ID_TRAY_WAKE0);
        else if (id == ID_LOGS || id == ID_TRAY_LOGS) open_log_folder();
        else if (id == ID_TRAY_ABOUT) about();
        else if (id == ID_TRAY_SEND) input_main_cross(-1);
        else if (id == ID_ROLE0 || id == ID_ROLE1) set_role(id - ID_ROLE0);
        else if ((id == ID_PASSWORD || (id >= ID_ADDR0 && id <= ID_ADDR3)) &&
                 (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
            if (code == EN_KILLFOCUS) apply_field((HWND)lp);
            redraw(&g.module_rc);
        } else if (id == ID_QUIT || id == ID_TRAY_QUIT) quit();
        else if (id == ID_TRAY_OPEN) show_window();
        else if (id == ID_SITE) ShellExecuteW(NULL, L"open", SITE_URL, NULL, NULL, SW_SHOWNORMAL);
        else if (id == ID_UPDATE) install_update();
        else if (id >= ID_FOUND0 && id <= ID_FOUND3 && g.found[id - ID_FOUND0].ip[0]) {
            wchar_t ip[16];
            MultiByteToWideChar(CP_ACP, 0, g.found[id - ID_FOUND0].ip, -1, ip, 16);
            for (int i = 0; i < MAX_PEERS; i++)
                if (!g.peer[i].addr[0]) { /* the first free slot; with all four taken nothing happens */
                    set_address(i, ip);
                    break;
                }
        }
        return 0;
    }
    case WM_FOUND:
        if (discovery_found((const Found *)lp) && g.page == PAGE_CONNECTION) refresh();
        free((void *)lp);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_BEAT) return beat(), 0;
        if (discovery_expire() && g.page == PAGE_CONNECTION) refresh();
        if (g.cfg.role == ROLE_MAIN && GetTickCount() / 1000 % 3 == 0) discovery_probe();
        for (int i = 0; i < MAX_PEERS; i++) {
            DWORD t = GetTickCount();
            unsigned char ping[5] = {MSG_PING, (BYTE)t, (BYTE)(t >> 8), (BYTE)(t >> 16), (BYTE)(t >> 24)};
            if (g.peer[i].status == NS_CONNECTED) net_send(i, ping, 5);
        }
        return 0;
    case WM_NET_STATUS: {
        Peer *p = &g.peer[lp];
        if ((int)wp == p->status) return 0;
        log_line("link %d: %ls", (int)lp, STATUS_WORD[wp]);
        if (wp != NS_CONNECTED) {
            BOOL sound = (int)lp == sound_from();
            p->rtt = -1, p->audio_state = AUDIO_UNKNOWN;
            if (g.cfg.role == ROLE_CONTROLLED) {
                input_controlled_stop();
                audio_capture_stop();
            } else if ((int)lp == g.target) { /* input was over there: it comes home */
                g.target = -1;
                input_main_leave(-1, -1);
                if (g.test) g.remote = FALSE;
            }
            if (sound) audio_play_stop();
        } else if (g.cfg.role == ROLE_CONTROLLED) {
            audio_capture_start(audio_out);
        } else {
            update_peer((int)lp);
            config_save(); /* the MAC SendARP may have learnt, for Wake-on-LAN */
        }
        p->status = (int)wp;
        sync_input();
        refresh();
        return 0;
    }
    case WM_NET_MSG: {
        unsigned char *copy = (unsigned char *)lp;
        on_net_msg(copy[0], copy + 1, (int)wp);
        free(copy);
        return 0;
    }
    case WM_INPUT_SEND:
        if (wp == MSG_ENTER) {
            cross_from_here((int)(lp & 0xF), (int)(lp >> 4) & 1, (int)(lp >> 8) & 0xFFFF, (int)(lp >> 24 & 0xF) - 1);
        } else if (wp == MSG_LEAVE) {
            go_home();
        } else {
            unsigned char msg[8];
            int n = input_pack((UINT)wp, lp, msg);
            if (n) net_send(g.target, msg, n);
        }
        return 0;
    case WM_UPDATE_READY:
        lstrcpynA(g.update_version, (const char *)lp, 16);
        free((void *)lp);
        refresh();
        return 0;
    case WM_UPDATE_PUSHED: {
        const wchar_t *name = name_of((int)lp);
        if (wp) _snwprintf(g.update_note, 256, L"%ls got the update and installs it now. On Windows Vista or later, "
                                               L"confirm the Windows prompt on its own screen.", name);
        else _snwprintf(g.update_note, 256, L"Sending the update to %ls did not finish; it is tried again on the "
                                            L"next connection.", name);
        g.update_note[255] = 0;
        if (g.installing) {
            wchar_t path[MAX_PATH];
            if (update_installer(g.update_version, path)) update_run(path);
            else g.installing = FALSE;
        }
        refresh();
        return 0;
    }
    case WM_UPDATE_RUN:
        update_run((const wchar_t *)lp);
        free((void *)lp);
        return 0;
    case WM_FILES_NOTE:
        lstrcpynW(g.files_note, (const wchar_t *)lp, 256);
        free((void *)lp);
        refresh();
        return 0;
    case WM_FILES_DONE:
        clipboard_set_files((const wchar_t *)lp);
        free((void *)lp);
        return 0;
    case WM_TEST:
        test_command((const char *)wp, (char *)lp);
        return 0;
    case WM_AUDIO_STATE:
        g.peer[0].audio_state = (int)wp;
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
    case WM_CLOSE: /* closing keeps Nexus in the tray, as Beamer does */
        ShowWindow(w, SW_HIDE);
        return 0;
    case WM_NEXUS_SHOW:
        show_window();
        return 0;
    case WM_NEXUS_QUIT:
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


/* ---- test mode ("/test"): a script drives this instance over TEST_CONTROL_PORT ----
 * The driver connects with the same password as the link (it plays the main role in the
 * handshake), sends MSG_TEST + a command and gets MSG_TEST + the answer. Commands run on the UI
 * thread, so they touch the same state as clicks. tests/driver.py lists them. */

static void put_files(const char *list) /* "a|b|c", as Explorer's Copy would put them */
{
    wchar_t paths[2048];
    int n = MultiByteToWideChar(CP_UTF8, 0, list, -1, paths, 2047);
    if (n <= 0) return;
    for (int i = 0; i < n; i++)
        if (paths[i] == L'|') paths[i] = 0;
    paths[n] = 0; /* double NUL */
    clipboard_set_files(paths);
}

static unsigned long fnv(const wchar_t *path, long *size)
{
    unsigned long h = 2166136261UL;
    unsigned char buf[8192];
    DWORD got;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    *size = -1;
    if (f == INVALID_HANDLE_VALUE) return 0;
    *size = 0;
    while (ReadFile(f, buf, sizeof buf, &got, NULL) && got)
        for (DWORD i = 0; i < got; i++) h = (h ^ buf[i]) * 16777619UL, ++*size;
    CloseHandle(f);
    return h;
}

static void test_command(const char *cmd, char *out)
{
    const int cap = 4096;
    char arg[2048] = "";
    wchar_t warg[1024] = L"";
    const char *sp = strchr(cmd, ' ');
    if (sp) {
        lstrcpynA(arg, sp + 1, sizeof arg);
        MultiByteToWideChar(CP_UTF8, 0, arg, -1, warg, 1024);
    }
    lstrcpynA(out, "ok", cap);
    if (!strncmp(cmd, "status", 6)) { /* about the PC with input, else the one used last or the first one */
        char peer[64], note[512];
        int pc = shown_peer() >= 0 ? shown_peer() : 0;
        WideCharToMultiByte(CP_UTF8, 0, g.peer[pc].name, -1, peer, sizeof peer, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, g.files_note, -1, note, sizeof note, NULL, NULL);
        _snprintf(out, cap, "role=%s status=%d peer=%s peer_version=%s remote=%d active=%d audio=%d version=%s secret=%d "
                  "files=%s",
                  g.cfg.role == ROLE_MAIN ? "main" : "controlled", overall(), peer, g.peer[pc].version, g.remote,
                  input_controlled_active(), g.peer[pc].audio_state, NEXUS_VERSION, g.peer[pc].secret, note);
    } else if (!strncmp(cmd, "clip-image ", 11)) { /* "clip-image W H": a test picture, as if the user copied it */
        int w = atoi(arg), h = atoi(strchr(arg, ' ') ? strchr(arg, ' ') + 1 : "0"), stride = (w * 3 + 3) & ~3;
        HGLOBAL mem = w > 0 && h > 0 && w <= 4096 && h <= 4096 ? GlobalAlloc(GHND, 40 + (SIZE_T)stride * h) : NULL;
        if (!mem) return (void)lstrcpynA(out, "usage: clip-image W H", cap);
        BITMAPINFOHEADER *bh = GlobalLock(mem);
        bh->biSize = 40, bh->biWidth = w, bh->biHeight = h, bh->biPlanes = 1, bh->biBitCount = 24;
        BYTE *bits = (BYTE *)(bh + 1);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                BYTE *px = bits + (SIZE_T)(h - 1 - y) * stride + x * 3; /* bottom-up rows */
                px[0] = (BYTE)(x * 7 + y), px[1] = (BYTE)(y * 5), px[2] = (BYTE)(x ^ y);
            }
        GlobalUnlock(mem);
        clipboard_put(CF_DIB, mem);
        clip_seq = 0;
    } else if (!strncmp(cmd, "clip-big ", 9)) { /* "clip-big N": N characters of text, too big for one message */
        int n = atoi(arg);
        HGLOBAL mem = n > 0 && n <= 1 << 20 ? GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(wchar_t)) : NULL;
        if (!mem) return (void)lstrcpynA(out, "usage: clip-big N", cap);
        wchar_t *w = GlobalLock(mem);
        for (int i = 0; i < n; i++) w[i] = (wchar_t)(i % 61 == 60 ? L'\n' : L'a' + i % 26);
        w[n] = 0;
        GlobalUnlock(mem);
        clipboard_put(CF_UNICODETEXT, mem);
        clip_seq = 0;
    } else if (!strncmp(cmd, "clip-hash", 9)) { /* "length fnv" of the clipboard's text as UTF-8 */
        lstrcpynA(out, "none", cap);
        if (open_clipboard()) {
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            const wchar_t *w = h ? GlobalLock(h) : NULL;
            if (w) {
                int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL) - 1;
                char *u = malloc(n + 1);
                unsigned long hash = 2166136261UL;
                if (u) {
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, n + 1, NULL, NULL);
                    for (int i = 0; i < n; i++) hash = (hash ^ (BYTE)u[i]) * 16777619UL;
                    _snprintf(out, cap, "%d %08lx", n, hash);
                    free(u);
                }
                GlobalUnlock(h);
            }
            CloseClipboard();
        }
    } else if (!strncmp(cmd, "clip-image-get", 14)) { /* "W H fnv" of the picture's pixels, top row first, as RGB */
        lstrcpynA(out, "none", cap);
        if (open_clipboard()) {
            HANDLE dib = GetClipboardData(CF_DIB);
            const BITMAPINFOHEADER *bh = dib ? GlobalLock(dib) : NULL;
            if (bh && (bh->biBitCount == 24 || bh->biBitCount == 32) && bh->biCompression == BI_RGB) {
                int w = bh->biWidth, h = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight, bpp = bh->biBitCount / 8;
                int stride = (w * bpp + 3) & ~3;
                const BYTE *bits = (const BYTE *)bh + bh->biSize + bh->biClrUsed * 4;
                unsigned long hash = 2166136261UL;
                for (int y = 0; y < h; y++) {
                    const BYTE *row = bits + (SIZE_T)(bh->biHeight < 0 ? y : h - 1 - y) * stride;
                    for (int x = 0; x < w; x++)
                        for (int k = 2; k >= 0; k--) hash = (hash ^ row[x * bpp + k]) * 16777619UL;
                }
                _snprintf(out, cap, "%d %d %08lx", w, h, hash);
            }
            if (bh) GlobalUnlock(dib);
            CloseClipboard();
        }
    } else if (!strncmp(cmd, "peers", 5)) { /* pc x,y status name, one per line */
        int at = 0;
        out[0] = 0;
        for (int i = 0; i < MAX_PEERS; i++) {
            char name[64];
            WideCharToMultiByte(CP_UTF8, 0, name_of(i), -1, name, sizeof name, NULL, NULL);
            if (g.peer[i].addr[0])
                at += _snprintf(out + at, cap - at, "%d %d,%d %d %s%s\n", i, g.peer[i].x, g.peer[i].y, g.peer[i].status,
                                name, i == g.target ? " *" : "");
        }
    } else if (!strncmp(cmd, "cross", 5) && g.cfg.role == ROLE_MAIN) { /* "cross [pc]": as the shortcut does */
        int pc = sp ? atoi(arg) : first_connected();
        if (pc < 0 || pc >= MAX_PEERS || !up(pc)) return (void)lstrcpynA(out, "not connected", cap);
        if (g.target >= 0) go_home();
        enter_peer(pc, edge_toward(pc) >= 0 ? edge_toward(pc) : EDGE_RIGHT, TRUE, 0);
        g.remote = TRUE, refresh();
    } else if (!strncmp(cmd, "home", 4) && g.cfg.role == ROLE_MAIN) {
        go_home();
        g.remote = FALSE, refresh();
    } else if (!strncmp(cmd, "move ", 5)) {
        char *end;
        int dx = (int)strtol(arg, &end, 10), dy = (int)strtol(end, NULL, 10);
        unsigned char m[5] = {MSG_MOVE, (BYTE)dx, (BYTE)(dx >> 8), (BYTE)dy, (BYTE)(dy >> 8)};
        net_send(g.target, m, 5);
    } else if (!strncmp(cmd, "cursor", 6)) {
        POINT p;
        GetCursorPos(&p);
        _snprintf(out, cap, "%ld %ld", p.x, p.y);
    } else if (!strncmp(cmd, "clip-set ", 9)) {
        clipboard_set((const unsigned char *)arg, (int)strlen(arg));
        clip_seq = 0; /* as if the user copied it: it travels on the next switch */
    } else if (!strncmp(cmd, "files ", 6)) {
        put_files(arg);
        clip_seq = 0;
    } else if (!strncmp(cmd, "clip-get", 8)) {
        lstrcpynA(out, "empty", cap);
        if (open_clipboard()) {
            HDROP drop = GetClipboardData(CF_HDROP);
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            if (drop) {
                int n = (int)DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0), at = 6;
                lstrcpynA(out, "files:", cap);
                for (int i = 0; i < n && at < cap - 600; i++) {
                    wchar_t f[MAX_PATH];
                    DragQueryFileW(drop, i, f, MAX_PATH);
                    if (i) out[at++] = '|';
                    at += WideCharToMultiByte(CP_UTF8, 0, f, -1, out + at, cap - at, NULL, NULL) - 1;
                }
            } else if (h) {
                const wchar_t *w = GlobalLock(h);
                lstrcpynA(out, "text:", cap);
                if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out + 5, cap - 5, NULL, NULL), GlobalUnlock(h);
            }
            CloseClipboard();
        }
    } else if (!strncmp(cmd, "write ", 6)) { /* write <path>|<text> */
        wchar_t *bar = wcschr(warg, L'|');
        char *text = strchr(arg, '|');
        DWORD n;
        if (!bar || !text) return (void)lstrcpynA(out, "usage: write path|text", cap);
        *bar = 0;
        HANDLE f = CreateFileW(warg, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) return (void)lstrcpynA(out, "cannot write", cap);
        WriteFile(f, text + 1, (DWORD)strlen(text + 1), &n, NULL);
        CloseHandle(f);
    } else if (!strncmp(cmd, "fnv ", 4)) {
        long size;
        unsigned long h = fnv(warg, &size);
        _snprintf(out, cap, "%ld %08lx", size, h);
    } else if (!strncmp(cmd, "hooks", 5)) {
        input_debug(out, cap);
    } else if (!strncmp(cmd, "found", 5)) {
        int at = 0;
        out[0] = 0;
        for (int i = 0; i < MAX_FOUND && at < cap - 100; i++)
            if (g.found[i].ip[0]) {
                char name[64];
                WideCharToMultiByte(CP_UTF8, 0, g.found[i].name, -1, name, sizeof name, NULL, NULL);
                at += _snprintf(out + at, cap - at, "%s%s %s %s", at ? "|" : "", g.found[i].ip,
                                g.found[i].role == ROLE_MAIN ? "main" : "controlled", name);
            }
    } else if (!strncmp(cmd, "temp", 4)) {
        wchar_t t[MAX_PATH];
        GetTempPathW(MAX_PATH, t);
        WideCharToMultiByte(CP_UTF8, 0, t, -1, out, cap, NULL, NULL);
    } else if (!strncmp(cmd, "quit", 4)) {
        PostMessageW(g.wnd, WM_NEXUS_QUIT, 0, 0);
    } else {
        lstrcpynA(out, "unknown or not now", cap);
    }
    out[cap - 1] = 0;
}

static DWORD WINAPI test_thread(void *unused)
{
    (void)unused;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(TEST_CONTROL_PORT);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 1) != 0) return 0;
    static unsigned char in[LINK_MAX + 1], out[1 + 4096];
    for (;;) {
        SOCKET s = accept(ls, NULL, NULL);
        if (s == INVALID_SOCKET) continue;
        unsigned char key[32];
        char peer[64], peer_version[16];
        Link l;
        EnterCriticalSection(&g.cs);
        memcpy(key, g.cfg.key, 32);
        LeaveCriticalSection(&g.cs);
        DWORD timeout = 30000; /* a driver that vanished must not keep the next one out */
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof timeout);
        int n;
        if (link_handshake(&l, s, 0, key, g.my_name, NEXUS_VERSION, peer, peer_version) == LINK_OK)
            while ((n = link_recv(&l, in, LINK_MAX)) > 0)
                if (in[0] == MSG_TEST) {
                    in[n] = 0;
                    out[0] = MSG_TEST;
                    SendMessageW(g.wnd, WM_TEST, (WPARAM)(in + 1), (LPARAM)(out + 1));
                    if (!link_send(&l, out, 1 + (int)strlen((char *)out + 1))) break;
                }
        closesocket(s);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    g.test = wcsstr(cmd, L"/test") != NULL;
    g.port = g.test ? TEST_LINK_PORT : LINK_PORT;
    g.app_class = g.test ? L"NexusTestWnd" : L"NexusWnd";
    CreateMutexW(NULL, FALSE, g.test ? L"Local\\NexusTest" : L"Local\\Nexus");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(g.app_class, NULL);
        if (other) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, WM_NEXUS_SHOW, 0, 0);
        }
        return 0;
    }

    g.inst = inst;
    GpStartup gp_in = {1, NULL, FALSE, FALSE};
    ULONG_PTR gp_token;
    GdiplusStartup(&gp_token, &gp_in, NULL);
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
    g.listener = INVALID_SOCKET;
    g.target = g.last = g.drag = -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        g.peer[i].wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        g.peer[i].conn = INVALID_SOCKET;
        g.peer[i].rtt = -1;
        InitializeCriticalSection(&g.peer[i].send_cs);
    }
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD name_len = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(name, &name_len);
    WideCharToMultiByte(CP_UTF8, 0, name, -1, g.my_name, sizeof g.my_name, NULL, NULL);
    lstrcpynW(g.my_name_w, name, 64);
    config_load();
    {
        wchar_t dir[MAX_PATH];
        lstrcpynW(dir, g.ini, MAX_PATH);
        *wcsrchr(dir, L'\\') = 0;
        log_start(dir, g.test, input_restore_pointer);
    }
    OSVERSIONINFOW os = {sizeof os, 0, 0, 0, 0, {0}};
    GetVersionExW(&os);
    log_line("Nexus %s starts on Windows %lu.%lu.%lu as the %s PC%s", NEXUS_VERSION, os.dwMajorVersion,
             os.dwMinorVersion, os.dwBuildNumber, g.cfg.role == ROLE_MAIN ? "main" : "controlled", g.test ? " (test)" : "");
    if (GetPrivateProfileIntW(L"nexus", L"pointer_hidden", 0, g.ini)) { /* hidden when Nexus last stopped */
        input_restore_pointer();
        WritePrivateProfileStringW(L"nexus", L"pointer_hidden", NULL, g.ini);
        log_line("the pointer was left hidden; it is back");
    }
    /* Input first: a busy PC must not make the pointer stutter. Windows 11's power throttling (EcoQoS)
     * would slow Nexus down while its window is hidden; Windows 8+ lets it opt out. */
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    {
        typedef BOOL(WINAPI * SetInfo)(HANDLE, int, void *, DWORD);
        SetInfo set_info = (SetInfo)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
        struct { ULONG version, control, state; } throttle = {1, 1 /* EXECUTION_SPEED */, 0 /* off */};
        if (set_info) set_info(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &throttle, sizeof throttle);
    }
    this_pc_addresses();

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = g.app_class;
    RegisterClassExW(&wc);

    /* WS_EX_CONTROLPARENT + IsDialogMessage give Tab/Shift+Tab between the controls. */
    CreateWindowExW(WS_EX_CONTROLPARENT, g.app_class, g.test ? L"Nexus (test)" : L"Nexus", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL,
                    CW_USEDEFAULT, CW_USEDEFAULT, S(1000), S(800), NULL, NULL, inst, NULL);
    /* A test instance leaves this PC's mouse and keys alone, unless a test VM asks for real hooks. */
    if (!g.test || wcsstr(cmd, L"/hooks")) input_main_start(g.wnd);
    files_init(g.wnd, net_send);
    if (g.test) CloseHandle(CreateThread(NULL, 0, test_thread, NULL, 0, NULL));
    else update_start(g.wnd, is_main_pc);
    sync_input();
    sync_keys();
    for (int i = 0; i < MAX_PEERS; i++) CloseHandle(CreateThread(NULL, 0, net_thread, (void *)(INT_PTR)i, 0, NULL));
    g.discovery_port = g.test ? TEST_DISCOVERY_PORT : DISCOVERY_PORT;
    discovery_start();
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
        int tile = -1, field = msg.hwnd == g.pw;
        for (int i = 0; i < MAX_PEERS; i++) {
            if (msg.hwnd == g.tile[i]) tile = i;
            field |= msg.hwnd == g.addr[i];
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && field) {
            apply_field(msg.hwnd);
            continue;
        }
        if (tile >= 0 && msg.message == WM_KEYDOWN && msg.wParam >= VK_LEFT && msg.wParam <= VK_DOWN) {
            static const int dx[] = {-1, 0, 1, 0}, dy[] = {0, -1, 0, 1}; /* VK_LEFT, UP, RIGHT, DOWN */
            int k = (int)msg.wParam - VK_LEFT;
            move_peer(tile, g.peer[tile].x + dx[k], g.peer[tile].y + dy[k]);
            SetFocus(g.tile[tile]);
            continue;
        }
        if (IsDialogMessageW(g.wnd, &msg)) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) scroll_into_view(GetFocus());
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
