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
       MSG_TEST /* test mode only */ };

/* link_handshake results */
enum { LINK_OK = 0, LINK_ERR_NET, LINK_ERR_PEER, LINK_ERR_VERSION, LINK_ERR_ROLE, LINK_ERR_PASSWORD };
/* link_recv errors (a length otherwise) */
#define LINK_RECV_NET (-1)
#define LINK_RECV_FORGED (-2)

typedef struct {
    SOCKET s;
    unsigned char ktx[32], krx[32];
    unsigned long long ntx, nrx; /* frame counters: the nonce, never sent, so replays fail to open */
} Link;

/* Stretches the shared password into the 32-byte key kept in the config instead of the password. */
void link_key_from_password(const char *utf8, unsigned char key[32]);
/* HELLO carries the name, a NUL and the Nexus version; a peer without the version reports "". */
int link_handshake(Link *l, SOCKET s, int is_main, const unsigned char key[32], const char *name, const char *version,
                   char peer[64], char peer_version[16]);
int link_send(Link *l, const void *msg, int len);
int link_recv(Link *l, unsigned char *out, int cap);

#endif
