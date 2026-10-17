/* nexus.log in %APPDATA%\Nexus (test.log for /test), rotated at 1 MB, and crash.log for a crash. */
#ifndef LOG_H
#define LOG_H

#include <windows.h>

/* dir: the folder; on_crash runs inside the crash handler (it must not need the heap). */
void log_start(const wchar_t *dir, BOOL test, void (*on_crash)(void));
void log_line(const char *fmt, ...); /* any thread; a line with the time in front */

#endif
