/* What happened, for bug reports from PCs we cannot sit at: nexus.log (1 MB, then nexus.log.1),
 * and crash.log with the exception and where it hit. Port of the idea in Beamer
 * win_app/kvm_bridge_win.py and core/crash_log.py (GPL-3, Toby George Read Kalkman). */

#include "log.h"
#include <stdarg.h>
#include <stdio.h>

#define LOG_MAX (1024 * 1024)

static CRITICAL_SECTION cs;
static wchar_t path[MAX_PATH], old_path[MAX_PATH], crash_path[MAX_PATH];
static BOOL started;
static void (*crash_hook)(void);

static void append(const wchar_t *file, const char *s, int n)
{
    HANDLE f = CreateFileW(file, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    DWORD done;
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, s, n, &done, NULL);
    CloseHandle(f);
}

static int stamp(char *out, int cap)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    return _snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                     t.wSecond, t.wMilliseconds);
}

void log_line(const char *fmt, ...)
{
    char line[600];
    va_list a;
    if (!started) return;
    int n = stamp(line, sizeof line);
    va_start(a, fmt);
    int m = _vsnprintf(line + n, sizeof line - n - 2, fmt, a);
    va_end(a);
    n = m < 0 ? (int)sizeof line - 3 : n + m;
    line[n++] = '\r', line[n++] = '\n';
    EnterCriticalSection(&cs);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &info) && info.nFileSizeLow > LOG_MAX)
        MoveFileExW(path, old_path, MOVEFILE_REPLACE_EXISTING); /* the one before stays as nexus.log.1 */
    append(path, line, n);
    LeaveCriticalSection(&cs);
}

static LONG WINAPI crashed(EXCEPTION_POINTERS *e)
{
    char s[400];
    HMODULE mod = NULL;
    void *at = e->ExceptionRecord->ExceptionAddress;
    wchar_t name[MAX_PATH] = L"?";
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at,
                       &mod);
    if (mod) GetModuleFileNameW(mod, name, MAX_PATH);
    int n = stamp(s, sizeof s);
    n += _snprintf(s + n, sizeof s - n - 2, "crash: exception %08lx at %p (%ls + %lx), thread %lu\r\n",
                   e->ExceptionRecord->ExceptionCode, at, name, (unsigned long)((char *)at - (char *)mod),
                   GetCurrentThreadId());
    append(crash_path, s, n);
    append(path, s, n);
    if (crash_hook) crash_hook(); /* the pointer comes back even when Nexus does not */
    return EXCEPTION_CONTINUE_SEARCH; /* Windows' own report as well */
}

void log_start(const wchar_t *dir, BOOL test, void (*on_crash)(void))
{
    InitializeCriticalSection(&cs);
    _snwprintf(path, MAX_PATH, L"%ls\\%ls.log", dir, test ? L"test" : L"nexus");
    _snwprintf(old_path, MAX_PATH, L"%ls.1", path);
    _snwprintf(crash_path, MAX_PATH, L"%ls\\crash.log", dir);
    path[MAX_PATH - 1] = old_path[MAX_PATH - 1] = crash_path[MAX_PATH - 1] = 0;
    crash_hook = on_crash;
    started = TRUE;
    SetUnhandledExceptionFilter(crashed);
}
