/* Plays the controlled PC for a running Bridger set up as the main PC with password "test-pw" and
 * address 127.0.0.1: accepts it, answers pings for a few seconds and prints what arrives. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/link.h"

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 5;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(LINK_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    assert(bind(ls, (struct sockaddr *)&a, sizeof a) == 0 && listen(ls, 1) == 0);
    SOCKET s = accept(ls, NULL, NULL);
    DWORD t = 1500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&t, sizeof t);
    Link l;
    unsigned char key[32], buf[LINK_MAX];
    char peer[64];
    link_key_from_password("test-pw", key);
    int r = link_handshake(&l, s, 0, key, "fake-xp", peer);
    printf("handshake %d, peer %s\n", r, peer);
    assert(r == LINK_OK);
    int pings = 0;
    for (DWORD end = GetTickCount() + seconds * 1000; GetTickCount() < end;) {
        int n = link_recv(&l, buf, LINK_MAX);
        if (n <= 0) break;
        if (buf[0] == MSG_PING) buf[0] = MSG_PONG, link_send(&l, buf, n), pings++;
        else printf("message %d, %d bytes\n", buf[0], n);
    }
    printf("answered %d pings\n", pings);
    assert(pings >= seconds - 1);
    puts("fake_controlled: ok");
    return 0;
}
