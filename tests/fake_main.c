/* Plays the main PC against a running Bridger set up as the controlled PC with password "test-pw"
 * on this machine: crosses in at the left edge, moves right, pushes back out and expects LEAVE.
 * Moves this PC's pointer briefly (and puts it back); sends no keys. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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

    unsigned char enter[5] = {MSG_ENTER, 1 /* main's right edge */, 0, 0x00, 0x80 /* half way down */};
    assert(link_send(&l, enter, 5));
    Sleep(200);
    GetCursorPos(&p);
    printf("after enter: %ld,%ld\n", p.x, p.y);
    assert(p.x == left + 2);

    move(100, 0), move(100, 0), move(100, 0);
    GetCursorPos(&p);
    printf("after 3 moves right: %ld,%ld\n", p.x, p.y);
    assert(abs(p.x - (left + 302)) <= 1); /* absolute input lands on a 1.5 px grid at 150% scaling */

    move(-1000, 0); /* to the left edge */
    move(-10, 0);   /* through it: input goes back to the main PC */
    int n = next(buf);
    printf("got message %d (len %d), position %d\n", buf[0], n, buf[2] | buf[3] << 8);
    assert(n == 4 && buf[0] == MSG_LEAVE && buf[1] == 1);
    int frac = buf[2] | buf[3] << 8;
    assert(frac > 32768 - 200 && frac < 32768 + 200);

    move(50, 0); /* no longer in control: must not move the pointer */
    GetCursorPos(&p);
    assert(abs(p.x - left) <= 1);
    SetCursorPos(before.x, before.y);
    closesocket(s);
    puts("fake_main: ok");
    return 0;
}
