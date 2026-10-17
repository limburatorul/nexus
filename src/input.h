/* Keyboard and mouse: captured on the main PC, replayed on the controlled one. */
#ifndef INPUT_H
#define INPUT_H

#include <windows.h>

enum { EDGE_LEFT, EDGE_RIGHT, EDGE_TOP, EDGE_BOTTOM };
enum { CHORD_CTRL = 1, CHORD_ALT = 2, CHORD_SHIFT = 4, CHORD_WIN = 8 }; /* a chord's modifiers, << 8 above its key */

/* Posted to the UI window by the hook thread. */
#define WM_INPUT_SEND (WM_APP + 10)  /* wParam: message type, lParam: packed fields, see input.c */
#define WM_INPUT_STATE (WM_APP + 11) /* wParam: TRUE while input is on the other PC */
#define WM_INPUT_KEY (WM_APP + 12)   /* a recorded key: wParam modifiers << 8 | virtual key, 0 when cancelled */

#define DEFAULT_RESISTANCE 120 /* px of push before an edge gives, Beamer's default */

/* What the main PC's hooks follow; the UI thread hands over a whole new one with input_main_config. */
typedef struct {
    int resistance;               /* px of push against an edge before it gives; 0: it gives at a touch */
    BOOL fullscreen_hold, paused; /* edges hold while a full-screen app has focus / always */
    BYTE switch_vk;               /* the switch key (left and right told apart) */
    BOOL hold;                    /* TRUE: input is across while that key is held; FALSE: double-tap it */
    WORD jump[4];                 /* per controlled PC: modifiers << 8 | key that goes straight there, 0: none */
} InputConfig;

/* Main PC: hooks run on their own thread so a busy UI never stalls the system's input. */
void input_main_start(HWND ui);
void input_main_set(BOOL connected, int edges); /* edges: bit per edge of this screen leading to a connected PC */
void input_main_config(const InputConfig *cfg);
void input_main_cross(int pc);                  /* input goes to pc (-1: the one used last), or comes home */
void input_main_leave(int edge, int frac);      /* input comes back at frac along edge; -1: where it left */
void input_main_record(int what);               /* the next key is reported, not used: 1 a key, 2 a chord, 0 stop */
int input_pack(UINT type, LPARAM packed, unsigned char *out); /* WM_INPUT_SEND -> wire message */
void input_debug(char *out, int cap); /* test mode: what the hooks have seen */

/* Controlled PC: replays a wire message; returns the length of a LEAVE to send back in leave, or 0. */
int input_replay(const unsigned char *msg, int len, unsigned char leave[8]);
void input_release_all(void);
BOOL input_controlled_active(void);
void input_controlled_stop(void); /* input goes home without a LEAVE from the main PC (locked, link lost) */

/* The pointer of a PC whose input is elsewhere: blanked system-wide, or back in the user's own scheme.
 * The local mouse moving brings it back too. */
void input_hide_pointer(BOOL hide);
void input_restore_pointer(void); /* after a crash: SPI_SETCURSORS, safe to call any time */

/* FALSE while this PC's input desktop is not the normal one: the lock screen, UAC, Ctrl+Alt+Del. */
BOOL input_desktop_unlocked(void);

#endif
