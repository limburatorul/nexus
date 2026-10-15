/* Keyboard and mouse: captured on the main PC, replayed on the controlled one. */
#ifndef INPUT_H
#define INPUT_H

#include <windows.h>

enum { EDGE_LEFT, EDGE_RIGHT, EDGE_TOP, EDGE_BOTTOM };

/* Posted to the UI window by the hook thread. */
#define WM_INPUT_SEND (WM_APP + 10)  /* wParam: message type, lParam: packed fields, see input.c */
#define WM_INPUT_STATE (WM_APP + 11) /* wParam: TRUE while input is on the other PC */

/* Main PC: hooks run on their own thread so a busy UI never stalls the system's input. */
void input_main_start(HWND ui);
void input_main_set(BOOL connected, int edge); /* call on connect, disconnect and edge change */
void input_main_leave(int frac);               /* the controlled PC handed input back; -1: no position */
int input_pack(UINT type, LPARAM packed, unsigned char *out); /* WM_INPUT_SEND -> wire message */
void input_debug(char *out, int cap); /* test mode: what the hooks have seen */

/* Controlled PC: replays a wire message; returns a LEAVE message to send back in out, or 0. */
int input_replay(const unsigned char *msg, int len, unsigned char out[8]);
void input_release_all(void);
BOOL input_controlled_active(void);

#endif
