/* Plays the main PC against a running Bridger set up as the controlled PC with password "test-pw"
 * on this machine: sends clipboard text, crosses in at the left edge, moves right, copies text
 * "there", pushes back out and expects that text, then LEAVE.
 * Moves this PC's pointer briefly and borrows its clipboard text (both put back); sends no keys. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/link.h"

static Link l;

static int next(unsigned char *buf) /* the next message that is not a ping */
{
    for (;;) {
        int n = link_recv(&l, buf, LINK_MAX);
        if (n <= 0) return n;
        if (buf[0] == MSG_PING) { buf[0] = MSG_PONG; link_send(&l, buf, n); continue; }
        if (buf[0] != MSG_PONG) return n;
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

int main(void)
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
