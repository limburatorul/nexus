/* Runs the real link over loopback: right password, wrong password, same role, a forged frame,
 * 0.5's forward-secret handshake (and a 0.4 peer beside it), and the clipboard's pieces. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/link.h"

typedef struct {
    SOCKET s;
    int is_main, result;
    unsigned char key[32];
    char peer[64], peer_version[16];
    const char *version;
    Link l;
} Side;

static const char *main_version = "0.3.1", *ctrl_version = "0.2.0";

static DWORD WINAPI run(void *p)
{
    Side *d = p;
    d->result = link_handshake(&d->l, d->s, d->is_main, d->key, d->is_main ? "main-pc" : "xp-box", d->version,
                               d->peer, d->peer_version);
    return 0;
}

/* clip_out's messages go straight into clip_in, as the link would carry them. */
static ClipIn got_clip;
static int clip_messages, clip_ends;
static BOOL to_clip_in(int to, const unsigned char *m, int n)
{
    (void)to;
    clip_messages++;
    clip_ends += clip_in(&got_clip, m, n);
    return TRUE;
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
    m->version = main_version, c->version = ctrl_version;
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

    /* 0.5 on both ends: the keys also come from an X25519 exchange, and frames still flow both ways. */
    main_version = ctrl_version = "0.5.0";
    handshake(&m, &c, "correct horse", "correct horse", 0);
    assert(m.result == LINK_OK && c.result == LINK_OK && m.l.forward_secret && c.l.forward_secret);
    assert(strcmp(c.peer_version, "0.5.0") == 0);
    assert(memcmp(m.l.ktx, c.l.krx, 32) == 0 && memcmp(m.l.krx, c.l.ktx, 32) == 0);
    unsigned char first_keys[32];
    memcpy(first_keys, m.l.ktx, 32);
    assert(link_send(&m.l, "\x02ping", 5) && link_recv(&c.l, got, sizeof got) == 5);
    assert(link_send(&c.l, "\x03pong", 5) && link_recv(&m.l, got, sizeof got) == 5);
    closesocket(m.s), closesocket(c.s);
    handshake(&m, &c, "correct horse", "correct horse", 0); /* same password, new ephemeral keys */
    assert(m.result == LINK_OK && memcmp(first_keys, m.l.ktx, 32) != 0);
    closesocket(m.s), closesocket(c.s);
    handshake(&m, &c, "correct horse", "wrong horse", 0);
    assert(m.result == LINK_ERR_PASSWORD && c.result == LINK_ERR_PASSWORD);
    closesocket(m.s), closesocket(c.s);

    /* A 0.4 peer on either end: the 0.4 handshake, no KEX, and it still works. */
    ctrl_version = "0.4.0";
    handshake(&m, &c, "pw", "pw", 0);
    assert(m.result == LINK_OK && c.result == LINK_OK && !m.l.forward_secret && !c.l.forward_secret);
    assert(link_send(&m.l, "\x06\x01\x00\x01\x00", 5) && link_recv(&c.l, got, sizeof got) == 5);
    closesocket(m.s), closesocket(c.s);
    main_version = "0.4.0", ctrl_version = "0.5.0";
    handshake(&m, &c, "pw", "pw", 0);
    assert(m.result == LINK_OK && c.result == LINK_OK && !m.l.forward_secret);
    closesocket(m.s), closesocket(c.s);
    assert(link_v5("0.5.0") && link_v5("0.10.1") && link_v5("1.0.0") && !link_v5("0.4.9") && !link_v5(""));

    /* The clipboard in pieces: 300 KB of image and some text arrive whole; over the limit stays behind. */
    static unsigned char image[300000];
    for (int i = 0; i < (int)sizeof image; i++) image[i] = (unsigned char)(i * 13);
    assert(clip_out(CLIP_DIB, image, sizeof image, to_clip_in, 0));
    assert(clip_out(CLIP_TEXT, "hello", 5, to_clip_in, 0));
    assert(clip_messages == 1 + 5 + 2 && !clip_ends); /* 300000 bytes = 5 pieces of up to 65535 */
    to_clip_in(0, (const unsigned char *)"\x19", 1); /* CLIP_END */
    assert(clip_ends == 1 && got_clip.got[CLIP_DIB] == sizeof image && got_clip.size[CLIP_DIB] == sizeof image);
    assert(memcmp(got_clip.data[CLIP_DIB], image, sizeof image) == 0);
    assert(got_clip.got[CLIP_TEXT] == 5 && memcmp(got_clip.data[CLIP_TEXT], "hello", 5) == 0);
    clip_in_free(&got_clip);
    unsigned char too_big[6] = {MSG_CLIP_BEGIN, CLIP_TEXT, 1, 0, 4, 0}; /* 256 KB + 1 */
    to_clip_in(0, too_big, 6);
    to_clip_in(0, (const unsigned char *)"\x18xyz", 4);
    assert(!got_clip.data[CLIP_TEXT] && !got_clip.got[CLIP_TEXT]);
    unsigned char short_one[6] = {MSG_CLIP_BEGIN, CLIP_TEXT, 2, 0, 0, 0}; /* says 2, sends 3: dropped */
    to_clip_in(0, short_one, 6);
    to_clip_in(0, (const unsigned char *)"\x18xyz", 4);
    assert(!got_clip.data[CLIP_TEXT]);
    clip_in_free(&got_clip);

    /* A picture from the other PC goes on the clipboard only when Windows can read all of it. */
    static unsigned char dib[40 + 12 * 10];
    BITMAPINFOHEADER *bh = (BITMAPINFOHEADER *)dib;
    bh->biSize = 40, bh->biWidth = 4, bh->biHeight = -10, bh->biPlanes = 1, bh->biBitCount = 24;
    assert(clip_dib_ok(dib, sizeof dib));      /* 4 px of 3 bytes: 12 a row, 10 rows */
    assert(!clip_dib_ok(dib, sizeof dib - 1)); /* one byte short */
    assert(!clip_dib_ok(dib, 39));
    bh->biBitCount = 8; /* 256 colours more than there is room for */
    assert(!clip_dib_ok(dib, sizeof dib));
    bh->biBitCount = 24, bh->biCompression = BI_JPEG;
    assert(!clip_dib_ok(dib, sizeof dib));
    bh->biCompression = BI_RGB, bh->biWidth = 0;
    assert(!clip_dib_ok(dib, sizeof dib));

    puts("link_test: ok");
    return 0;
}
