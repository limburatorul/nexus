/* Updates: the main PC fetches new releases from GitHub; a controlled PC gets them over the link
 * (Windows XP cannot reach GitHub itself, it has no TLS 1.2). */
#ifndef UPDATE_H
#define UPDATE_H

#include <windows.h>

/* Posted to the UI window; a pointer in lParam is malloc'd and freed by whoever handles it. */
#define WM_UPDATE_READY (WM_APP + 30)  /* lParam: char* version whose installer is downloaded */
#define WM_UPDATE_PUSHED (WM_APP + 31) /* wParam: TRUE when the installer reached the other PC, lParam: which */
#define WM_UPDATE_RUN (WM_APP + 32)    /* lParam: wchar_t* installer received from the main PC */

int version_cmp(const char *a, const char *b); /* <0, 0, >0 like strcmp, for "1.2.3" */

/* Main PC: checks GitHub a minute after start and every 6 hours while is_main() says so. */
void update_start(HWND ui, BOOL (*is_main)(void));
/* The downloaded installer for version, if there is one. */
BOOL update_installer(const char *version, wchar_t path[MAX_PATH]);
/* Sends that installer to PC `to` on a background thread; WM_UPDATE_PUSHED reports the end. */
BOOL update_push(const char *version, BOOL (*send)(int to, const unsigned char *msg, int len), int to);
/* Runs an installer silently; it closes this Nexus, installs and starts the new one. */
void update_run(const wchar_t *installer);

/* Controlled PC, network thread: one MSG_UPDATE_* message; update_abort when the link drops. */
void update_receive(const unsigned char *msg, int len);
void update_abort(void);

#endif
