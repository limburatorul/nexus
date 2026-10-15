/* Releases are GitHub releases tagged vX.Y.Z with Nexus-Setup-X.Y.Z.exe attached. The main PC reads
 * the latest one over HTTPS (WinHTTP), downloads a newer installer into %APPDATA%\Nexus\updates
 * and keeps it, so a controlled PC that connects later with an older Nexus can be sent the same
 * file: UPDATE_BEGIN [size u32][version], UPDATE_DATA chunks, UPDATE_END. The controlled PC only
 * runs a newer version, and only a complete file that starts like a Windows program. */

#include "link.h" /* winsock2.h before windows.h */
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "update.h"
#include "version.h"

#define WIDEN2(s) L##s
#define WIDEN(s) WIDEN2(s)
#define LATEST L"https://api.github.com/repos/limburatorul/nexus/releases/latest"
#define ASSETS "https://github.com/limburatorul/nexus/releases/download/"
#define MAX_INSTALLER (20 * 1024 * 1024)
#define CHUNK 32768

static HWND ui;

int version_cmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (x != y) return x < y ? -1 : 1;
        a = *ea == '.' ? ea + 1 : ea;
        b = *eb == '.' ? eb + 1 : eb;
    }
    return 0;
}

static BOOL valid_version(const char *v)
{
    int dots = 0, n = 0;
    for (; v[n]; n++)
        if (v[n] == '.') dots++;
        else if (v[n] < '0' || v[n] > '9') return FALSE;
    return dots == 2 && n >= 5 && n <= 15;
}

BOOL update_installer(const char *version, wchar_t path[MAX_PATH])
{
    wchar_t dir[MAX_PATH];
    if (!valid_version(version) || SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir) != S_OK) return FALSE;
    _snwprintf(path, MAX_PATH, L"%ls\\Nexus\\updates\\Nexus-Setup-%hs.exe", dir, version);
    path[MAX_PATH - 1] = 0;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

/* ---- GitHub ---- */

/* The body of an HTTPS GET, at most max bytes, NUL-terminated; NULL on any failure. */
static char *https_get(const wchar_t *url, DWORD max, DWORD *len)
{
    wchar_t host[256], path[2048];
    URL_COMPONENTS u;
    memset(&u, 0, sizeof u);
    u.dwStructSize = sizeof u;
    u.lpszHostName = host, u.dwHostNameLength = 256;
    u.lpszUrlPath = path, u.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &u) || u.nScheme != INTERNET_SCHEME_HTTPS) return NULL;
    HINTERNET s = WinHttpOpen(L"Nexus/" WIDEN(NEXUS_VERSION), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return NULL;
    DWORD tls12 = 0x800; /* WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2: GitHub's minimum; Windows 7 needs KB3140245 */
    WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls12, sizeof tls12);
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)
                    : NULL;
    char *body = NULL;
    DWORD status = 0, size = sizeof status, n = 0, got = 0;
    BOOL ok = r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json", (DWORD)-1, NULL, 0, 0, 0) &&
              WinHttpReceiveResponse(r, NULL) &&
              WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size,
                                  NULL) &&
              status == 200 && (body = malloc(max + 1));
    while (ok && (ok = WinHttpReadData(r, body + n, max - n, &got)) && got)
        if ((n += got) == max) ok = FALSE; /* bigger than anything we expect */
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    if (!ok) {
        free(body);
        return NULL;
    }
    body[n] = 0;
    *len = n;
    return body;
}

/* The string value after "key": at or after from, without escapes (none of ours have any). */
static const char *json_string(const char *from, const char *key, char *out, int cap)
{
    const char *k = strstr(from, key), *p;
    if (!k) return NULL;
    for (p = k + strlen(key); *p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t'; p++) {}
    if (*p != '"') return NULL;
    int i = 0;
    for (p++; *p && *p != '"' && *p != '\\' && i < cap - 1; p++) out[i++] = *p;
    out[i] = 0;
    return *p == '"' ? p : NULL;
}

static void check(void)
{
    DWORD n;
    char *json = https_get(LATEST, 1 << 20, &n), tag[32], url[512], want[64];
    if (!json) return;
    const char *v = json_string(json, "\"tag_name\"", tag, sizeof tag) ? tag + (tag[0] == 'v') : "";
    wchar_t path[MAX_PATH];
    if (valid_version(v) && version_cmp(v, NEXUS_VERSION) > 0) {
        _snprintf(want, sizeof want, "/Nexus-Setup-%s.exe", v);
        want[63] = 0;
        BOOL have = update_installer(v, path);
        for (const char *p = json; !have && (p = json_string(p, "\"browser_download_url\"", url, sizeof url));)
            if (!strncmp(url, ASSETS, strlen(ASSETS)) && strstr(url, want) &&
                !strcmp(url + strlen(url) - strlen(want), want)) {
                wchar_t wurl[512], part[MAX_PATH], dir[MAX_PATH];
                MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 512);
                DWORD size;
                char *exe = https_get(wurl, MAX_INSTALLER, &size);
                if (!exe) break;
                if (size > 2 && exe[0] == 'M' && exe[1] == 'Z') { /* a Windows program, not an error page */
                    lstrcpynW(dir, path, MAX_PATH);
                    *wcsrchr(dir, L'\\') = 0;
                    CreateDirectoryW(dir, NULL);
                    _snwprintf(part, MAX_PATH, L"%ls.part", path);
                    HANDLE f = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                    DWORD wrote = 0;
                    BOOL written = f != INVALID_HANDLE_VALUE && WriteFile(f, exe, size, &wrote, NULL) && wrote == size;
                    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
                    have = written && MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING);
                    if (!have) DeleteFileW(part);
                }
                free(exe);
            }
        if (have) {
            char *ready = _strdup(v);
            if (!PostMessageW(ui, WM_UPDATE_READY, 0, (LPARAM)ready)) free(ready);
        }
    }
    free(json);
}

static BOOL (*main_pc)(void);

static DWORD WINAPI check_thread(void *unused)
{
    (void)unused;
    Sleep(60 * 1000); /* let the network come up after sign-in */
    for (;;) {
        if (main_pc()) check();
        Sleep(6 * 60 * 60 * 1000);
    }
    return 0;
}

void update_start(HWND w, BOOL (*is_main)(void))
{
    ui = w;
    main_pc = is_main;
    HANDLE t = CreateThread(NULL, 0, check_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

void update_run(const wchar_t *installer) { ShellExecuteW(NULL, L"open", installer, L"/S", NULL, SW_SHOWNORMAL); }

/* ---- main PC -> controlled PC ---- */

static struct {
    char version[16];
    BOOL (*send)(const unsigned char *, int);
} push;
static volatile LONG pushing;

static DWORD WINAPI push_thread(void *unused)
{
    (void)unused;
    static unsigned char m[1 + CHUNK];
    wchar_t path[MAX_PATH];
    BOOL ok = FALSE;
    HANDLE f = update_installer(push.version, path)
                   ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL)
                   : INVALID_HANDLE_VALUE;
    DWORD size = f != INVALID_HANDLE_VALUE ? GetFileSize(f, NULL) : 0;
    if (size && size <= MAX_INSTALLER) {
        int v = (int)strlen(push.version);
        m[0] = MSG_UPDATE_BEGIN;
        for (int i = 0; i < 4; i++) m[1 + i] = (unsigned char)(size >> (8 * i));
        memcpy(m + 5, push.version, v);
        ok = push.send(m, 5 + v);
        DWORD got;
        while (ok && ReadFile(f, m + 1, CHUNK, &got, NULL) && got) {
            m[0] = MSG_UPDATE_DATA;
            ok = push.send(m, 1 + (int)got);
        }
        m[0] = MSG_UPDATE_END;
        ok = ok && push.send(m, 1);
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    PostMessageW(ui, WM_UPDATE_PUSHED, ok, 0);
    InterlockedExchange(&pushing, 0);
    return 0;
}

BOOL update_push(const char *version, BOOL (*send)(const unsigned char *, int))
{
    if (InterlockedCompareExchange(&pushing, 1, 0)) return FALSE;
    lstrcpynA(push.version, version, 16);
    push.send = send;
    HANDLE t = CreateThread(NULL, 0, push_thread, NULL, 0, NULL);
    if (!t) InterlockedExchange(&pushing, 0);
    else CloseHandle(t);
    return t != NULL;
}

/* ---- controlled PC: receiving ---- */

static struct {
    HANDLE f;
    DWORD size, got;
    wchar_t path[MAX_PATH];
} rx = {.f = INVALID_HANDLE_VALUE};

void update_abort(void)
{
    if (rx.f == INVALID_HANDLE_VALUE) return;
    CloseHandle(rx.f);
    rx.f = INVALID_HANDLE_VALUE;
    DeleteFileW(rx.path);
}

void update_receive(const unsigned char *m, int len)
{
    if (m[0] == MSG_UPDATE_BEGIN && len > 5 && len < 5 + 16) {
        char version[16];
        update_abort();
        memcpy(version, m + 5, len - 5);
        version[len - 5] = 0;
        rx.size = m[1] | m[2] << 8 | m[3] << 16 | (DWORD)m[4] << 24;
        rx.got = 0;
        wchar_t dir[MAX_PATH];
        /* Only ever forward: an older or equal version is never installed over this one. */
        if (!valid_version(version) || version_cmp(version, NEXUS_VERSION) <= 0 || !rx.size ||
            rx.size > MAX_INSTALLER || !GetTempPathW(MAX_PATH, dir))
            return;
        _snwprintf(rx.path, MAX_PATH, L"%lsNexus-Setup-%hs.exe", dir, version);
        rx.path[MAX_PATH - 1] = 0;
        rx.f = CreateFileW(rx.path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    } else if (rx.f == INVALID_HANDLE_VALUE) {
        return; /* nothing being received, or it was refused */
    } else if (m[0] == MSG_UPDATE_DATA && len > 1) {
        DWORD wrote;
        if (rx.got + (DWORD)(len - 1) > rx.size || !WriteFile(rx.f, m + 1, len - 1, &wrote, NULL)) return update_abort();
        rx.got += len - 1;
    } else if (m[0] == MSG_UPDATE_END && len == 1) {
        char mz[2] = {0};
        DWORD n = 0;
        CloseHandle(rx.f);
        rx.f = INVALID_HANDLE_VALUE;
        HANDLE f = CreateFileW(rx.path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) ReadFile(f, mz, 2, &n, NULL), CloseHandle(f);
        if (rx.got != rx.size || n != 2 || mz[0] != 'M' || mz[1] != 'Z') {
            DeleteFileW(rx.path);
            return;
        }
        wchar_t *run = _wcsdup(rx.path); /* started from the UI thread */
        if (!PostMessageW(ui, WM_UPDATE_RUN, 0, (LPARAM)run)) free(run);
    }
}
