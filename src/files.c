/* Wire: FILE_BEGIN [count u32][total u64], then per item FILE [is_dir u8][size u64][relative path, UTF-8]
 * followed by FILE_DATA chunks for a file's bytes, then FILE_END. Items arrive parents first.
 * The receiver writes into Desktop\Nexus Received\<date time>\ and puts the top-level items on
 * the clipboard, so Ctrl+V in Explorer copies them where wanted. */

#include "link.h" /* winsock2.h before windows.h */
#include <shellapi.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "files.h"

#define CHUNK 32768

static HWND ui;
static BOOL (*link_out)(int to, const unsigned char *, int);

static void note(const wchar_t *fmt, ...)
{
    wchar_t *s = malloc(256 * sizeof(wchar_t));
    va_list a;
    va_start(a, fmt);
    _vsnwprintf(s, 256, fmt, a);
    va_end(a);
    s[255] = 0;
    if (!PostMessageW(ui, WM_FILES_NOTE, 0, (LPARAM)s)) free(s);
}

static void size_text(wchar_t out[32], LONGLONG b)
{
    if (b < 1024 * 1024) _snwprintf(out, 32, L"%d KB", (int)((b + 1023) / 1024));
    else _snwprintf(out, 32, L"%d.%d MB", (int)(b / 1048576), (int)(b % 1048576 * 10 / 1048576));
    out[31] = 0;
}

/* ---- sending ---- */

typedef struct {
    wchar_t **paths;
    int n;
    wchar_t peer[64];
    int to, count;
    LONGLONG total, sent;
    int last_pct;
    BOOL ok;
} Batch;

static volatile LONG sending;

/* Visits path and, for a folder, everything under it, parents first. rel starts at rel_from. */
typedef void (*Visit)(Batch *, const wchar_t *path, int rel_from, BOOL dir, LONGLONG size);

static void walk(Batch *b, const wchar_t *path, int rel_from, Visit visit)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!b->ok || !GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return;
    BOOL dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    visit(b, path, rel_from, dir, (LONGLONG)a.nFileSizeHigh << 32 | a.nFileSizeLow);
    if (!dir || (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return; /* never follow junctions */
    wchar_t pattern[MAX_PATH];
    if (_snwprintf(pattern, MAX_PATH, L"%ls\\*", path) < 0) return;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
        wchar_t child[MAX_PATH];
        if (_snwprintf(child, MAX_PATH, L"%ls\\%ls", path, f.cFileName) > 0) walk(b, child, rel_from, visit);
    } while (FindNextFileW(h, &f));
    FindClose(h);
}

static void count(Batch *b, const wchar_t *path, int rel_from, BOOL dir, LONGLONG size)
{
    (void)path, (void)rel_from;
    b->count++;
    if (!dir) b->total += size;
}

static void put64(unsigned char *o, LONGLONG v)
{
    for (int i = 0; i < 8; i++) o[i] = (unsigned char)(v >> (8 * i));
}

static void send_item(Batch *b, const wchar_t *path, int rel_from, BOOL dir, LONGLONG size)
{
    static unsigned char m[1 + CHUNK]; /* the one sending thread at a time */
    HANDLE f = INVALID_HANDLE_VALUE;
    if (!dir && (f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
                                 NULL)) == INVALID_HANDLE_VALUE)
        return; /* locked or gone: skipped */
    m[0] = MSG_FILE;
    m[1] = (unsigned char)dir;
    put64(m + 2, dir ? 0 : size);
    int n = WideCharToMultiByte(CP_UTF8, 0, path + rel_from, -1, (char *)m + 10, 1024, NULL, NULL);
    b->ok = n > 1 && link_out(b->to, m, 10 + n - 1);
    for (LONGLONG left = dir ? 0 : size; b->ok && left > 0;) {
        DWORD got = 0;
        if (!ReadFile(f, m + 1, (DWORD)(left < CHUNK ? left : CHUNK), &got, NULL) || !got) {
            b->ok = FALSE; /* the file shrank or failed: the receiver keeps what came */
            break;
        }
        m[0] = MSG_FILE_DATA;
        b->ok = link_out(b->to, m, 1 + (int)got);
        left -= got, b->sent += got;
        int pct = b->total ? (int)(b->sent * 100 / b->total) : 100;
        if (pct / 10 != b->last_pct / 10) note(L"Sending to %ls\x2026 %d%%", b->peer, pct), b->last_pct = pct;
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

static DWORD WINAPI send_thread(void *p)
{
    Batch *b = p;
    b->ok = TRUE;
    for (int i = 0; i < b->n; i++) walk(b, b->paths[i], 0, count);
    wchar_t size[32];
    size_text(size, b->total);
    if (b->total > FILES_MAX_BYTES) {
        note(L"The copied files are %ls, over the 1 GB Nexus carries at once, so they stayed on this PC.", size);
    } else {
        unsigned char begin[13] = {MSG_FILE_BEGIN};
        for (int i = 0; i < 4; i++) begin[1 + i] = (unsigned char)(b->count >> (8 * i));
        put64(begin + 5, b->total);
        note(L"Sending %d item%ls (%ls) to %ls\x2026", b->count, b->count == 1 ? L"" : L"s", size, b->peer);
        b->ok = link_out(b->to, begin, 13);
        for (int i = 0; i < b->n && b->ok; i++) {
            const wchar_t *slash = wcsrchr(b->paths[i], L'\\');
            walk(b, b->paths[i], slash ? (int)(slash - b->paths[i]) + 1 : 0, send_item);
        }
        unsigned char end = MSG_FILE_END;
        if (b->ok) b->ok = link_out(b->to, &end, 1);
        if (b->ok) note(L"Sent %d item%ls (%ls) to %ls.", b->count, b->count == 1 ? L"" : L"s", size, b->peer);
        else note(L"Sending to %ls stopped part way; copy the files again to retry.", b->peer);
    }
    for (int i = 0; i < b->n; i++) free(b->paths[i]);
    free(b->paths);
    free(b);
    InterlockedExchange(&sending, 0);
    return 0;
}

BOOL files_send(HDROP drop, const wchar_t *peer, int to)
{
    if (InterlockedCompareExchange(&sending, 1, 0)) return FALSE;
    Batch *b = calloc(1, sizeof *b);
    b->n = (int)DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    b->paths = calloc(b->n ? b->n : 1, sizeof *b->paths);
    for (int i = 0; i < b->n; i++) {
        b->paths[i] = malloc(MAX_PATH * sizeof(wchar_t));
        DragQueryFileW(drop, i, b->paths[i], MAX_PATH);
    }
    lstrcpynW(b->peer, peer, 64);
    b->to = to;
    b->last_pct = -10;
    HANDLE t = CreateThread(NULL, 0, send_thread, b, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&sending, 0); /* ponytail: the batch leaks if no thread could start */
    return t != NULL;
}

/* ---- receiving (network thread) ---- */

static struct {
    BOOL active;
    wchar_t dir[MAX_PATH];
    HANDLE file;
    LONGLONG left, total, got;
    int count, last_pct;
    wchar_t *tops; /* top-level paths, NUL-separated */
    int tops_len;
} rx;

/* A relative path we are willing to create: plain names only, nothing absolute, no "..". */
static BOOL safe_relative(const wchar_t *p)
{
    const wchar_t *seg = p;
    if (!*p) return FALSE;
    for (const wchar_t *c = p;; c++) {
        if (*c == L'\\' || !*c) {
            int n = (int)(c - seg);
            if (!n || (n == 1 && seg[0] == L'.') || (n == 2 && seg[0] == L'.' && seg[1] == L'.')) return FALSE;
            if (!*c) return TRUE;
            seg = c + 1;
        } else if (*c < 32 || wcschr(L"/:*?\"<>|", *c)) {
            return FALSE;
        }
    }
}

static void close_file(void)
{
    if (rx.file != INVALID_HANDLE_VALUE) CloseHandle(rx.file);
    rx.file = INVALID_HANDLE_VALUE;
}

void files_abort(void)
{
    if (!rx.active) return;
    close_file();
    free(rx.tops);
    rx.tops = NULL;
    rx.active = FALSE;
    if (RemoveDirectoryW(rx.dir)) note(L"A transfer was refused or stopped before anything arrived."); /* empty */
    else note(L"The transfer stopped part way. What arrived is in %ls.", rx.dir);
}

static LONGLONG get64(const unsigned char *p)
{
    LONGLONG v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static void begin(const unsigned char *m, int len)
{
    if (len != 13) return;
    files_abort();
    rx.count = (int)(m[1] | m[2] << 8 | m[3] << 16 | (DWORD)m[4] << 24);
    rx.total = get64(m + 5);
    if (rx.total < 0 || rx.total > FILES_MAX_BYTES) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t desk[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desk) != S_OK) return;
    _snwprintf(rx.dir, MAX_PATH, L"%ls\\Nexus Received", desk);
    CreateDirectoryW(rx.dir, NULL);
    _snwprintf(rx.dir, MAX_PATH, L"%ls\\Nexus Received\\%04d-%02d-%02d %02d-%02d-%02d", desk, t.wYear, t.wMonth,
               t.wDay, t.wHour, t.wMinute, t.wSecond);
    rx.dir[MAX_PATH - 1] = 0;
    for (int i = 2; !CreateDirectoryW(rx.dir, NULL); i++) { /* two batches within one second */
        if (GetLastError() != ERROR_ALREADY_EXISTS || i > 9) return;
        size_t n = wcslen(rx.dir);
        if (i > 2) n -= 2;
        _snwprintf(rx.dir + n, MAX_PATH - n, L" %d", i);
    }
    rx.active = TRUE;
    rx.file = INVALID_HANDLE_VALUE;
    rx.left = rx.got = 0;
    rx.last_pct = -10;
    rx.tops = calloc(1, sizeof(wchar_t));
    rx.tops_len = 0;
    wchar_t size[32];
    size_text(size, rx.total);
    note(L"Receiving %d item%ls (%ls)\x2026", rx.count, rx.count == 1 ? L"" : L"s", size);
}

static void item(const unsigned char *m, int len)
{
    wchar_t rel[MAX_PATH], path[MAX_PATH];
    if (len < 11 || rx.left) return files_abort(); /* a new item before the last one was complete */
    close_file();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)m + 10, len - 10, rel, MAX_PATH - 1);
    if (n <= 0) return files_abort();
    rel[n] = 0;
    LONGLONG size = get64(m + 2);
    if (!safe_relative(rel) || size < 0 || rx.got + size > rx.total ||
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", rx.dir, rel) < 0 || wcslen(rx.dir) + 1 + n >= MAX_PATH - 1)
        return files_abort();
    if (!wcschr(rel, L'\\')) { /* top level: goes on the clipboard */
        wchar_t *t = realloc(rx.tops, (rx.tops_len + wcslen(path) + 2) * sizeof(wchar_t));
        if (!t) return files_abort();
        rx.tops = t;
        wcscpy(rx.tops + rx.tops_len, path);
        rx.tops_len += (int)wcslen(path) + 1;
        rx.tops[rx.tops_len] = 0;
    }
    if (m[1]) {
        CreateDirectoryW(path, NULL);
        return;
    }
    rx.file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (rx.file == INVALID_HANDLE_VALUE) return files_abort();
    rx.left = size;
    if (!size) close_file();
}

static void data(const unsigned char *m, int len)
{
    DWORD wrote;
    if (rx.file == INVALID_HANDLE_VALUE || len - 1 > rx.left) return files_abort();
    if (!WriteFile(rx.file, m + 1, len - 1, &wrote, NULL) || wrote != (DWORD)(len - 1)) {
        note(L"Writing a received file failed. Is the disk full?");
        return files_abort();
    }
    rx.left -= len - 1, rx.got += len - 1;
    if (!rx.left) close_file();
    int pct = rx.total ? (int)(rx.got * 100 / rx.total) : 100;
    if (pct / 10 != rx.last_pct / 10) note(L"Receiving\x2026 %d%%", pct), rx.last_pct = pct;
}

static void end(void)
{
    close_file();
    rx.active = FALSE;
    if (rx.tops_len && PostMessageW(ui, WM_FILES_DONE, 0, (LPARAM)rx.tops)) rx.tops = NULL;
    free(rx.tops);
    rx.tops = NULL;
    note(L"Received %d item%ls into %ls. They are on the clipboard: paste them with Ctrl+V.", rx.count,
         rx.count == 1 ? L"" : L"s", rx.dir);
}

void files_receive(const unsigned char *m, int len)
{
    if (m[0] == MSG_FILE_BEGIN) begin(m, len);
    else if (!rx.active) return; /* the rest of a batch that was refused or aborted */
    else if (m[0] == MSG_FILE) item(m, len);
    else if (m[0] == MSG_FILE_DATA) data(m, len);
    else if (m[0] == MSG_FILE_END && len == 1) end();
}

void files_init(HWND w, BOOL (*send)(int, const unsigned char *, int))
{
    ui = w;
    link_out = send;
    rx.file = INVALID_HANDLE_VALUE;
}
