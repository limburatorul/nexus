/* Runs the real link over loopback: right password, wrong password, same role, a forged frame. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/link.h"

typedef struct { SOCKET s; int is_main, result; unsigned char key[32]; char peer[64], peer_version[16]; Link l; } Side;

static DWORD WINAPI run(void *p)
{
    Side *d = p;
    d->result = link_handshake(&d->l, d->s, d->is_main, d->key, d->is_main ? "main-pc" : "xp-box",
                               d->is_main ? "0.3.1" : "0.2.0", d->peer, d->peer_version);
    return 0;
}

static void pair(SOCKET *a, SOCKET *b)
{
    struct sockaddr_in at = {0};
    int len = sizeof at;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(ls, (struct sockaddr *)&at, sizeof at) == 0 && listen(ls, 1) == 0);
    getsockname(ls, (struct sockaddr *)&at, &len);
    *a = socket(AF_INET, SOCK_STREAM, 0);
    assert(connect(*a, (struct sockaddr *)&at, sizeof at) == 0);
    *b = accept(ls, NULL, NULL);
    closesocket(ls);
}

/* Handshakes both ends at once and returns them for further checks. */
static void handshake(Side *m, Side *c, const char *pw_main, const char *pw_ctrl, int ctrl_is_main)
{
    memset(m, 0, sizeof *m), memset(c, 0, sizeof *c);
    pair(&m->s, &c->s);
    m->is_main = 1, c->is_main = ctrl_is_main;
    link_key_from_password(pw_main, m->key);
    link_key_from_password(pw_ctrl, c->key);
    HANDLE t = CreateThread(NULL, 0, run, m, 0, NULL);
    run(c);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

int main(void)
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    Side m, c;
    static unsigned char big[LINK_MAX], got[LINK_MAX];

    handshake(&m, &c, "correct horse", "correct horse", 0);
    assert(m.result == LINK_OK && c.result == LINK_OK);
    assert(strcmp(m.peer, "xp-box") == 0 && strcmp(c.peer, "main-pc") == 0);
    assert(strcmp(m.peer_version, "0.2.0") == 0 && strcmp(c.peer_version, "0.3.1") == 0);
    for (int i = 0; i < LINK_MAX; i++) big[i] = (unsigned char)(i * 7);
    assert(link_send(&m.l, "\x06\x01\x00\xff\xff", 5));
    assert(link_recv(&c.l, got, sizeof got) == 5 && memcmp(got, "\x06\x01\x00\xff\xff", 5) == 0);
    assert(link_send(&c.l, big, LINK_MAX));
    assert(link_recv(&m.l, got, sizeof got) == LINK_MAX && memcmp(got, big, LINK_MAX) == 0);
    assert(!link_send(&m.l, big, LINK_MAX + 1));

    /* A frame altered on the way (one byte of ciphertext flipped) must not open. */
    unsigned char frame[4 + 16 + 3] = {19, 0, 0, 0};
    {
        SOCKET real = m.l.s;
        SOCKET a, b;
        pair(&a, &b);
        m.l.s = a;
        assert(link_send(&m.l, "abc", 3));
        for (int got_n = 0, r; got_n < (int)sizeof frame; got_n += r)
            assert((r = recv(b, (char *)frame + got_n, sizeof frame - got_n, 0)) > 0);
        frame[sizeof frame - 1] ^= 1;
        send(real, (const char *)frame, sizeof frame, 0);
        assert(link_recv(&c.l, got, sizeof got) == LINK_RECV_FORGED);
        closesocket(a), closesocket(b);
    }
    closesocket(m.s), closesocket(c.s);

    handshake(&m, &c, "correct horse", "wrong horse", 0);
    assert(m.result == LINK_ERR_PASSWORD && c.result == LINK_ERR_PASSWORD);
    closesocket(m.s), closesocket(c.s);

    handshake(&m, &c, "pw", "pw", 1);
    assert(m.result == LINK_ERR_ROLE && c.result == LINK_ERR_ROLE);
    closesocket(m.s), closesocket(c.s);

    puts("link_test: ok");
    return 0;
}
