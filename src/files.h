/* Files copied in Explorer follow input to the other PC and land on its clipboard for Ctrl+V. */
#ifndef FILES_H
#define FILES_H

#include <windows.h>

#define FILES_MAX_BYTES (1024LL * 1024 * 1024) /* a bigger copy stays put: likely a slip, and slow */

/* Posted to the UI window. lParam is malloc'd and freed by the receiver of the message. */
#define WM_FILES_NOTE (WM_APP + 20) /* lParam: wchar_t text for the Files page */
#define WM_FILES_DONE (WM_APP + 21) /* lParam: received top-level paths, NUL-separated, double-NUL ended */

/* send: puts one message on the link to PC `to` and says whether that link is still up (any thread). */
void files_init(HWND ui, BOOL (*send)(int to, const unsigned char *msg, int len));
/* Sends the files of a CF_HDROP to PC `to` on a background thread; FALSE while a transfer is still running. */
BOOL files_send(HDROP drop, const wchar_t *peer, int to);
/* Network thread: one FILE_* message; files_abort when the link drops mid-way. */
void files_receive(const unsigned char *msg, int len);
void files_abort(void);

#endif
