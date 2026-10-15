/* Plays the main PC against a running Bridger set up as the controlled PC with password "test-pw"
 * on this machine: sends clipboard text, crosses in at the left edge, moves right, copies text
 * "there", pushes back out and expects that text, then LEAVE.
 * Moves this PC's pointer briefly and borrows its clipboard text (both put back); sends no keys.
 * "fake_main audio" instead checks that sound playing here comes back over the link.
 * "fake_main files" sends files both ways; it writes under Desktop\Bridger Received and cleans up. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "../src/link.h"
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>

static Link l;
static int want_audio; /* otherwise sound messages are skipped like pings */

static int next(unsigned char *buf) /* the next message that is not a ping */
{
    for (;;) {
        int n = link_recv(&l, buf, LINK_MAX);
        if (n <= 0) return n;
        if (buf[0] == MSG_PING) { buf[0] = MSG_PONG; link_send(&l, buf, n); continue; }
        if (buf[0] == MSG_PONG || (!want_audio && (buf[0] == MSG_AUDIO || buf[0] == MSG_AUDIO_STATE))) continue;
        return n;
    }
}

static wchar_t *clip_get(void)
{
    static wchar_t text[256];
    text[0] = 0;
    if (!OpenClipboard(NULL)) return text;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = h ? GlobalLock(h) : NULL;
    if (w) lstrcpynW(text, w, 256), GlobalUnlock(h);
    CloseClipboard();
    return text;
}

static void clip_put(const wchar_t *s)
{
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (lstrlenW(s) + 1) * sizeof(wchar_t));
    lstrcpyW(GlobalLock(h), s);
    GlobalUnlock(h);
    assert(OpenClipboard(NULL));
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
}

static void move(int dx, int dy)
{
    unsigned char m[5] = {MSG_MOVE, (unsigned char)dx, (unsigned char)(dx >> 8), (unsigned char)dy, (unsigned char)(dy >> 8)};
    assert(link_send(&l, m, 5));
    Sleep(100);
}

/* "fake_main audio": plays a quiet 440 Hz tone here and expects Bridger to send it back as MSG_AUDIO. */
static int audio_check(void)
{
    static short tone[44100 * 2 * 3 / 2]; /* 1.5 s stereo */
    for (int i = 0; i < 44100 * 3 / 2; i++) tone[2 * i] = tone[2 * i + 1] = (short)(3000 * sin(i * 2 * 3.14159265 * 440 / 44100));
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};
    HWAVEOUT h;
    WAVEHDR hdr = {(LPSTR)tone, sizeof tone, 0, 0, 0, 0, NULL, 0};
    assert(waveOutOpen(&h, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR);
    waveOutPrepareHeader(h, &hdr, sizeof hdr);
    waveOutWrite(h, &hdr, sizeof hdr);

    want_audio = 1;
    DWORD t = 500;
    setsockopt(l.s, SOL_SOCKET, SO_RCVTIMEO, (char *)&t, sizeof t);
    unsigned char buf[LINK_MAX];
    int packets = 0, loudest = 0, rate = 0, st = -1;
    for (DWORD end = GetTickCount() + 2500; GetTickCount() < end;) {
        int n = next(buf);
        if (n <= 0) continue; /* timeout: keep listening until the end */
        if (buf[0] == MSG_AUDIO_STATE && n == 2) st = buf[1];
        if (buf[0] != MSG_AUDIO) continue;
        packets++;
        rate = buf[1] | buf[2] << 8 | buf[3] << 16 | buf[4] << 24;
        for (int i = 5; i + 1 < n; i += 2) {
            int v = abs((short)(buf[i] | buf[i + 1] << 8));
            if (v > loudest) loudest = v;
        }
    }
    waveOutReset(h);
    waveOutUnprepareHeader(h, &hdr, sizeof hdr);
    waveOutClose(h);
    printf("audio state %d, %d packets at %d Hz, loudest sample %d\n", st, packets, rate, loudest);
    assert(st == 1 /* AUDIO_LOOPBACK */ || st == 2 /* AUDIO_STEREO_MIX */);
    assert(packets > 20 && rate >= 8000 && loudest > 300);
    puts("fake_main audio: ok");
    return 0;
}

static void file_msg(int is_dir, LONGLONG size, const char *path)
{
    unsigned char m[300] = {MSG_FILE, (unsigned char)is_dir};
    for (int i = 0; i < 8; i++) m[2 + i] = (unsigned char)(size >> (8 * i));
    int n = (int)strlen(path);
    memcpy(m + 10, path, n);
    assert(link_send(&l, m, 10 + n));
}

static void file_begin(int count, LONGLONG total)
{
    unsigned char m[13] = {MSG_FILE_BEGIN, (unsigned char)count};
    for (int i = 0; i < 8; i++) m[5 + i] = (unsigned char)(total >> (8 * i));
    assert(link_send(&l, m, 13));
}

static int clip_files(wchar_t out[2][MAX_PATH]) /* paths on the clipboard as CF_HDROP */
{
    int n = 0;
    assert(OpenClipboard(NULL));
    HDROP d = GetClipboardData(CF_HDROP);
    if (d) {
        n = (int)DragQueryFileW(d, 0xFFFFFFFF, NULL, 0);
        for (int i = 0; i < n && i < 2; i++) DragQueryFileW(d, i, out[i], MAX_PATH);
    }
    CloseClipboard();
    return n;
}

static void remove_tree(const wchar_t *path) /* only what this test created */
{
    wchar_t from[MAX_PATH + 1] = {0};
    lstrcpynW(from, path, MAX_PATH);
    SHFILEOPSTRUCTW op = {NULL, FO_DELETE, from, NULL, FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI, 0, NULL, NULL};
    SHFileOperationW(&op);
}

/* "fake_main files": files both ways, and a path that tries to leave the received folder. */
static int files_check(void)
{
    static unsigned char data[1 + 25000] = {MSG_FILE_DATA};
    wchar_t got[2][MAX_PATH], parent[MAX_PATH], desk[MAX_PATH], probe[MAX_PATH];
    unsigned char buf[LINK_MAX];

    /* main -> controlled: a folder holding a small file, and a 100 000-byte file */
    file_begin(3, 100005);
    file_msg(1, 0, "bt");
    file_msg(0, 5, "bt\\a.txt");
    data[1] = 'h', data[2] = 'e', data[3] = 'l', data[4] = 'l', data[5] = 'o';
    assert(link_send(&l, data, 6));
    file_msg(0, 100000, "b.bin");
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 25000; i++) data[1 + i] = (unsigned char)((k * 25000 + i) * 31);
        assert(link_send(&l, data, 1 + 25000));
    }
    unsigned char end = MSG_FILE_END;
    assert(link_send(&l, &end, 1));
    Sleep(500);
    assert(clip_files(got) == 2);
    printf("received: %ls\n          %ls\n", got[0], got[1]);
    assert(wcsstr(got[0], L"\\Bridger Received\\") && wcsstr(got[0], L"\\bt") && wcsstr(got[1], L"\\b.bin"));
    lstrcpynW(parent, got[0], MAX_PATH);
    *wcsrchr(parent, L'\\') = 0;
    _snwprintf(probe, MAX_PATH, L"%ls\\a.txt", got[0]);
    HANDLE f = CreateFileW(probe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char small[8] = {0};
    DWORD n = 0;
    assert(f != INVALID_HANDLE_VALUE && ReadFile(f, small, 8, &n, NULL) && n == 5 && !memcmp(small, "hello", 5));
    CloseHandle(f);
    f = CreateFileW(got[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    assert(f != INVALID_HANDLE_VALUE && GetFileSize(f, NULL) == 100000);
    static unsigned char big[100000];
    assert(ReadFile(f, big, sizeof big, &n, NULL) && n == sizeof big);
    for (int i = 0; i < 100000; i++) assert(big[i] == (unsigned char)(i * 31));
    CloseHandle(f);

    /* a path climbing out of the received folder is refused, and nothing is written */
    file_begin(1, 4);
    file_msg(0, 4, "..\\escape.txt");
    memcpy(data + 1, "evil", 4);
    assert(link_send(&l, data, 5));
    assert(link_send(&l, &end, 1));
    Sleep(300);
    SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desk);
    _snwprintf(probe, MAX_PATH, L"%ls\\Bridger Received\\escape.txt", desk);
    assert(GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES);
    assert(clip_files(got) == 2); /* the refused batch did not replace the clipboard */
    puts("unsafe path refused");

    /* controlled -> main: a file copied "there" travels when input leaves it */
    wchar_t src[MAX_PATH];
    GetTempPathW(MAX_PATH, src);
    wcscat(src, L"bridger-send.txt");
    f = CreateFileW(src, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(f, "from controlled", 15, &n, NULL);
    CloseHandle(f);
    HGLOBAL h = GlobalAlloc(GHND, sizeof(DROPFILES) + (wcslen(src) + 2) * sizeof(wchar_t));
    DROPFILES *d = GlobalLock(h);
    d->pFiles = sizeof(DROPFILES), d->fWide = TRUE;
    wcscpy((wchar_t *)(d + 1), src);
    GlobalUnlock(h);
    assert(OpenClipboard(NULL));
    EmptyClipboard();
    SetClipboardData(CF_HDROP, h);
    CloseClipboard();
    unsigned char enter[5] = {MSG_ENTER, 1, 1 /* keep the pointer where it is */, 0, 0};
    unsigned char leave[4] = {MSG_LEAVE, 0, 0, 0};
    assert(link_send(&l, enter, 5) && link_send(&l, leave, 4));
    int kinds[4] = {MSG_FILE_BEGIN, MSG_FILE, MSG_FILE_DATA, MSG_FILE_END};
    for (int k = 0; k < 4; k++) {
        int len = next(buf);
        assert(len > 0 && buf[0] == kinds[k]);
        if (buf[0] == MSG_FILE) assert(len == 10 + 16 && !memcmp(buf + 10, "bridger-send.txt", 16) && buf[2] == 15);
        if (buf[0] == MSG_FILE_DATA) assert(len == 16 && !memcmp(buf + 1, "from controlled", 15));
    }
    puts("file sent back on leaving");

    remove_tree(parent);
    DeleteFileW(src);
    _snwprintf(probe, MAX_PATH, L"%ls\\Bridger Received", desk);
    RemoveDirectoryW(probe); /* only if this test left it empty */
    puts("fake_main files: ok");
    return 0;
}

int main(int argc, char **argv)
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    POINT before, p;
    GetCursorPos(&before);
    wchar_t saved[256];
    lstrcpynW(saved, clip_get(), 256);
    int left = GetSystemMetrics(SM_XVIRTUALSCREEN);

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(LINK_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    assert(connect(s, (struct sockaddr *)&a, sizeof a) == 0);
    unsigned char key[32], buf[LINK_MAX];
    char peer[64];
    link_key_from_password("test-pw", key);
    int r = link_handshake(&l, s, 1, key, "fake-main", peer);
    printf("handshake %d, peer %s\n", r, peer);
    assert(r == LINK_OK);
    if (argc > 1 && strcmp(argv[1], "audio") == 0) return audio_check();
    if (argc > 1 && strcmp(argv[1], "files") == 0) {
        wchar_t saved_text[256];
        lstrcpynW(saved_text, clip_get(), 256);
        int r2 = files_check();
        clip_put(saved_text);
        return r2;
    }

    /* MSG_CLIPBOARD, then UTF-8 text ending in s-comma (U+0219) */
    const char hello[] = {MSG_CLIPBOARD, 'h', 'e', 'l', 'l', 'o', ' ', 'f', 'r', 'o', 'm', ' ', 'm', 'a', 'i', 'n', ' ',
                          (char)0xC8, (char)0x99};
    assert(link_send(&l, hello, sizeof hello));
    unsigned char enter[5] = {MSG_ENTER, 1 /* main's right edge */, 0, 0x00, 0x80 /* half way down */};
    assert(link_send(&l, enter, 5));
    Sleep(200);
    GetCursorPos(&p);
    printf("after enter: %ld,%ld\n", p.x, p.y);
    assert(p.x == left + 2);
    assert(lstrcmpW(clip_get(), L"hello from main \x0219") == 0);

    move(100, 0), move(100, 0), move(100, 0);
    GetCursorPos(&p);
    printf("after 3 moves right: %ld,%ld\n", p.x, p.y);
    assert(abs(p.x - (left + 302)) <= 1); /* absolute input lands on a 1.5 px grid at 150% scaling */

    clip_put(L"copied on controlled");
    move(-1000, 0); /* to the left edge */
    move(-10, 0);   /* through it: input goes back to the main PC, the new copy first */
    int n = next(buf);
    printf("got message %d: \"%.*s\"\n", buf[0], n - 1, buf + 1);
    assert(buf[0] == MSG_CLIPBOARD && n == 21 && memcmp(buf + 1, "copied on controlled", 20) == 0);
    n = next(buf);
    printf("got message %d (len %d), position %d\n", buf[0], n, buf[2] | buf[3] << 8);
    assert(n == 4 && buf[0] == MSG_LEAVE && buf[1] == 1);
    int frac = buf[2] | buf[3] << 8;
    assert(frac > 32768 - 200 && frac < 32768 + 200);

    move(50, 0); /* no longer in control: must not move the pointer */
    GetCursorPos(&p);
    assert(abs(p.x - left) <= 1);
    SetCursorPos(before.x, before.y);
    clip_put(saved);
    closesocket(s);
    puts("fake_main: ok");
    return 0;
}
