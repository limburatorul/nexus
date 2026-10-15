/* Plays the controlled PC for a running Nexus set up as the main PC with password "test-pw" and
 * address 127.0.0.1: accepts it, answers pings for a few seconds and prints what arrives.
 * "fake_controlled 5 tone" first streams 2 s of a quiet 440 Hz tone and checks, with the default
 * speakers' peak meter, that Nexus plays it (other sound playing at the time also counts). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/link.h"
#include <mmdeviceapi.h>

/* MinGW declares IAudioMeterInformation without its C vtable; the first four slots are enough here. */
typedef struct Meter Meter;
typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(Meter *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(Meter *);
    ULONG(STDMETHODCALLTYPE *Release)(Meter *);
    HRESULT(STDMETHODCALLTYPE *GetPeakValue)(Meter *, float *);
} MeterVtbl;
struct Meter { const MeterVtbl *lpVtbl; };

static const GUID CLSID_Enumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_Enumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID IID_Meter = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

static Meter *speaker_meter(void)
{
    IMMDeviceEnumerator *en;
    IMMDevice *dev;
    Meter *m = NULL;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    assert(SUCCEEDED(CoCreateInstance(&CLSID_Enumerator, NULL, CLSCTX_ALL, &IID_Enumerator, (void **)&en)));
    assert(SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)));
    assert(SUCCEEDED(dev->lpVtbl->Activate(dev, &IID_Meter, CLSCTX_ALL, NULL, (void **)&m)));
    return m;
}

static void stream_tone(Link *l)
{
    Meter *meter = speaker_meter();
    unsigned char st[2] = {MSG_AUDIO_STATE, 1}, pkt[5 + 960 * 4] = {MSG_AUDIO, 0x80, 0xBB, 0, 0}; /* 48000 Hz */
    assert(link_send(l, st, 2));
    float peak = 0;
    for (int chunk = 0, i = 0; chunk < 100; chunk++) { /* 100 x 20 ms */
        for (int f = 0; f < 960; f++, i++) {
            short v = (short)(3000 * sin(i * 2 * 3.14159265 * 440 / 48000));
            unsigned char *o = pkt + 5 + 4 * f;
            o[0] = o[2] = (unsigned char)v, o[1] = o[3] = (unsigned char)(v >> 8);
        }
        assert(link_send(l, pkt, sizeof pkt));
        Sleep(20);
        float p;
        if (chunk > 25 && SUCCEEDED(meter->lpVtbl->GetPeakValue(meter, &p)) && p > peak) peak = p;
    }
    printf("speaker peak while streaming: %.3f\n", peak);
    assert(peak > 0.02f);
}

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
    char peer[64], peer_version[16];
    link_key_from_password("test-pw", key);
    int r = link_handshake(&l, s, 0, key, "fake-xp", "0.0.1", peer, peer_version);
    printf("handshake %d, peer %s\n", r, peer);
    assert(r == LINK_OK);
    if (argc > 2 && strcmp(argv[2], "tone") == 0) stream_tone(&l);
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
