/* Handshake: each side sends "BRDG", the link version, its role and a fresh 24-byte nonce in clear.
 * SHA-512(key || main nonce || controlled nonce) gives one 32-byte key per direction. Every frame is
 * then u32 length (LE) + XSalsa20-Poly1305 ciphertext, its nonce the direction's frame counter.
 * The first frame each way is HELLO with the PC's name: if it does not open, the passwords differ. */

#include <string.h>
#include "link.h"
#include <ntsecapi.h> /* RtlGenRandom: advapi32's SystemFunction036, Windows XP and later */
#include "tweetnacl.h"


static const unsigned char MAGIC[4] = {'B', 'R', 'D', 'G'};

/* TweetNaCl's source of randomness. Without it no nonce is safe, so there is no carrying on. */
void randombytes(unsigned char *p, unsigned long long n)
{
    if (!RtlGenRandom(p, (ULONG)n)) FatalAppExitW(0, L"Bridger: no system random number generator.");
}

void link_key_from_password(const char *pw, unsigned char key[32])
{
    unsigned char buf[8 + 256], h[64];
    size_t n = strlen(pw);
    if (n > 256) n = 256;
    memcpy(buf, "bridger1", 8);
    memcpy(buf + 8, pw, n);
    crypto_hash(h, buf, 8 + n);
    /* ponytail: plain iterated SHA-512; a real KDF (scrypt) if offline guessing of weak passwords matters */
    for (int i = 0; i < 20000; i++) crypto_hash(h, h, 64);
    memcpy(key, h, 32);
}

static int send_all(SOCKET s, const unsigned char *p, int n)
{
    while (n > 0) {
        int r = send(s, (const char *)p, n, 0);
        if (r <= 0) return 0;
        p += r, n -= r;
    }
    return 1;
}

static int recv_all(SOCKET s, unsigned char *p, int n)
{
    while (n > 0) {
        int r = recv(s, (char *)p, n, 0);
        if (r <= 0) return 0;
        p += r, n -= r;
    }
    return 1;
}

static void counter_nonce(unsigned char n[24], unsigned long long c)
{
    memset(n, 0, 24);
    for (int i = 0; i < 8; i++) n[i] = (unsigned char)(c >> (8 * i));
}

int link_send(Link *l, const void *msg, int len)
{
    unsigned char m[32 + LINK_MAX], c[32 + LINK_MAX]; /* 128 KB of stack; threads have 1 MB */
    unsigned char n[24];
    if (len < 1 || len > LINK_MAX) return 0;
    memset(m, 0, 32);
    memcpy(m + 32, msg, len);
    counter_nonce(n, l->ntx++);
    crypto_secretbox(c, m, len + 32, n, l->ktx);
    /* The box starts with 16 zero bytes; the length goes in the last 4 so one send() carries it all. */
    unsigned int clen = len + 16;
    for (int i = 0; i < 4; i++) c[12 + i] = (unsigned char)(clen >> (8 * i));
    return send_all(l->s, c + 12, clen + 4);
}

int link_recv(Link *l, unsigned char *out, int cap)
{
    unsigned char c[32 + LINK_MAX], m[32 + LINK_MAX];
    unsigned char hdr[4], n[24];
    if (!recv_all(l->s, hdr, 4)) return LINK_RECV_NET;
    unsigned int clen = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (unsigned int)hdr[3] << 24;
    if (clen < 17 || clen > LINK_MAX + 16) return LINK_RECV_NET; /* not a frame we could have sent */
    memset(c, 0, 16);
    if (!recv_all(l->s, c + 16, (int)clen)) return LINK_RECV_NET;
    counter_nonce(n, l->nrx++);
    if (crypto_secretbox_open(m, c, clen + 16, n, l->krx) != 0) return LINK_RECV_FORGED;
    int len = (int)clen - 16;
    if (len > cap) return LINK_RECV_NET;
    memcpy(out, m + 32, len);
    return len;
}

int link_handshake(Link *l, SOCKET s, int is_main, const unsigned char key[32], const char *name, char peer[64])
{
    unsigned char mine[30], theirs[30], buf[80], h[64], msg[64];
    memcpy(mine, MAGIC, 4);
    mine[4] = LINK_VERSION;
    mine[5] = is_main ? 0 : 1;
    randombytes(mine + 6, 24);
    if (!send_all(s, mine, 30) || !recv_all(s, theirs, 30)) return LINK_ERR_NET;
    if (memcmp(theirs, MAGIC, 4) != 0) return LINK_ERR_PEER;
    if (theirs[4] != LINK_VERSION) return LINK_ERR_VERSION;
    if (theirs[5] == mine[5]) return LINK_ERR_ROLE;

    memcpy(buf, key, 32);
    memcpy(buf + 32, is_main ? mine + 6 : theirs + 6, 24);
    memcpy(buf + 56, is_main ? theirs + 6 : mine + 6, 24);
    crypto_hash(h, buf, 80);
    memcpy(l->ktx, is_main ? h : h + 32, 32);
    memcpy(l->krx, is_main ? h + 32 : h, 32);
    l->s = s;
    l->ntx = l->nrx = 0;

    int n = (int)strlen(name);
    if (n > 63) n = 63;
    msg[0] = MSG_HELLO;
    memcpy(msg + 1, name, n);
    if (!link_send(l, msg, n + 1)) return LINK_ERR_NET;
    int r = link_recv(l, msg, sizeof msg);
    if (r == LINK_RECV_FORGED) return LINK_ERR_PASSWORD;
    if (r < 1) return LINK_ERR_NET;
    if (msg[0] != MSG_HELLO) return LINK_ERR_PEER;
    memcpy(peer, msg + 1, r - 1);
    peer[r - 1] = 0;
    return LINK_OK;
}
