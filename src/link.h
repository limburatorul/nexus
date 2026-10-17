/* The encrypted link between the main PC and a controlled PC (wire format in SPEC.md). */
#ifndef LINK_H
#define LINK_H

#include <winsock2.h>
#include <windows.h>

#define LINK_PORT 24800
#define LINK_VERSION 1
#define LINK_MAX 65536 /* largest plaintext message */

enum { MSG_HELLO = 1, MSG_PING, MSG_PONG, MSG_ENTER, MSG_LEAVE, MSG_MOVE, MSG_BUTTON, MSG_WHEEL, MSG_KEY,
       MSG_CLIPBOARD /* UTF-8 text */, MSG_AUDIO, MSG_AUDIO_STATE,
       MSG_FILE_BEGIN, MSG_FILE, MSG_FILE_DATA, MSG_FILE_END,
       MSG_UPDATE_BEGIN, MSG_UPDATE_DATA, MSG_UPDATE_END,
       MSG_TEST /* test mode only */,
       /* 0.5 and later; an older peer never gets them (see SPEC.md) */
       MSG_KEX /* handshake: ephemeral X25519 public key */, MSG_LOCKED /* controlled PC: its desktop is locked */,
       MSG_CLIP_BEGIN /* [format u8][size u32] */, MSG_CLIP_DATA, MSG_CLIP_END };
enum { CLIP_TEXT = 1 /* UTF-8 */, CLIP_DIB /* CF_DIB: BITMAPINFOHEADER, colours, bits */ };
#define CLIP_TEXT_MAX (256 * 1024)
#define CLIP_IMAGE_MAX (8 * 1024 * 1024)

/* link_handshake results */
enum { LINK_OK = 0, LINK_ERR_NET, LINK_ERR_PEER, LINK_ERR_VERSION, LINK_ERR_ROLE, LINK_ERR_PASSWORD };
/* link_recv errors (a length otherwise) */
#define LINK_RECV_NET (-1)
#define LINK_RECV_FORGED (-2)

typedef struct {
    SOCKET s;
    unsigned char ktx[32], krx[32];
    unsigned long long ntx, nrx; /* frame counters: the nonce, never sent, so replays fail to open */
    int forward_secret;          /* the keys also come from an ephemeral X25519 exchange (both ends 0.5+) */
} Link;

/* Whether a Nexus version (from HELLO) has 0.5's link: forward secrecy, chunked clipboard, LOCKED. */
int link_v5(const char *version);

/* Stretches the shared password into the 32-byte key kept in the config instead of the password. */
void link_key_from_password(const char *utf8, unsigned char key[32]);
/* HELLO carries the name, a NUL and the Nexus version; a peer without the version reports "". */
int link_handshake(Link *l, SOCKET s, int is_main, const unsigned char key[32], const char *name, const char *version,
                   char peer[64], char peer_version[16]);
int link_send(Link *l, const void *msg, int len);

/* The clipboard in pieces (0.5): CLIP_BEGIN [format][size u32], CLIP_DATA until size, CLIP_END after
 * the last format. clip_out sends one format through send(to, ...); clip_in collects them. */
#define LINK_DEAD_MS 2000 /* nothing heard this long while input is across: the link is gone */
BOOL clip_out(int format, const void *data, unsigned int size, BOOL (*send)(int to, const unsigned char *msg, int len),
              int to);
typedef struct { unsigned char *data[3]; unsigned int size[3], got[3]; int format; } ClipIn;
/* TRUE at CLIP_END: each data[f] with got[f] == size[f] is complete; clip_in_free afterwards. */
BOOL clip_in(ClipIn *c, const unsigned char *msg, int len);
void clip_in_free(ClipIn *c);
/* Whether a CF_DIB from the other PC is one Windows can read: header, colours and pixels all there. */
BOOL clip_dib_ok(const unsigned char *dib, unsigned int size);
int link_recv(Link *l, unsigned char *out, int cap);

#endif
