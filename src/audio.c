/* Sound travels the link as MSG_AUDIO: [rate u32 LE][16-bit stereo frames], about 20 ms each.
 * Capture: WASAPI loopback of the default output (Vista and later), else the sound card's
 * "Stereo Mix" recording input through waveIn (Windows XP). Playback: waveOut, started once
 * 60 ms are queued and fed no further ahead than 250 ms, so latency stays bounded. */

#include "link.h" /* winsock2.h before windows.h */
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "audio.h"

#define CHUNK_MS 20
#define PREBUFFER_MS 60
#define MAX_AHEAD_MS 250

static const GUID CLSID_Enumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_Enumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID IID_Client = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID IID_Capture = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const GUID SUBTYPE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

/* ---- capture (controlled PC) ---- */

static struct {
    HANDLE thread;
    volatile LONG stop;
    void (*send)(const unsigned char *, int);
    unsigned char pkt[5 + 4 * 192000 * CHUNK_MS / 1000];
    int frames, rate;
    BOOL heard; /* a chunk of pure silence is not sent */
} cap;

static void flush(void)
{
    if (cap.frames && cap.heard) {
        cap.pkt[0] = MSG_AUDIO;
        for (int i = 0; i < 4; i++) cap.pkt[1 + i] = (unsigned char)(cap.rate >> (8 * i));
        cap.send(cap.pkt, 5 + 4 * cap.frames);
    }
    cap.frames = 0;
    cap.heard = FALSE;
}

static void push(short l, short r, int rate)
{
    if (rate != cap.rate) flush(), cap.rate = rate;
    unsigned char *o = cap.pkt + 5 + 4 * cap.frames;
    o[0] = (unsigned char)l, o[1] = (unsigned char)(l >> 8), o[2] = (unsigned char)r, o[3] = (unsigned char)(r >> 8);
    cap.heard |= l || r;
    if (++cap.frames >= rate * CHUNK_MS / 1000) flush();
}

static void state(int s)
{
    unsigned char m[2] = {MSG_AUDIO_STATE, (unsigned char)s};
    cap.send(m, 2);
}

static short to16(float f) { return (short)(f >= 1.0f ? 32767 : f <= -1.0f ? -32767 : f * 32767.0f); }

/* Returns FALSE when there is no loopback to be had (XP has no WASAPI). */
static BOOL loopback(void)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    IAudioCaptureClient *cc = NULL;
    WAVEFORMATEX *wf = NULL;
    BOOL ok = FALSE;
    if (FAILED(CoCreateInstance(&CLSID_Enumerator, NULL, CLSCTX_ALL, &IID_Enumerator, (void **)&en))) return FALSE;
    if (SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) &&
        SUCCEEDED(dev->lpVtbl->Activate(dev, &IID_Client, CLSCTX_ALL, NULL, (void **)&ac)) &&
        SUCCEEDED(ac->lpVtbl->GetMixFormat(ac, &wf)) &&
        SUCCEEDED(ac->lpVtbl->Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, wf,
                                         NULL)) &&
        SUCCEEDED(ac->lpVtbl->GetService(ac, &IID_Capture, (void **)&cc))) {
        BOOL is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                        (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                         IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)wf)->SubFormat, &SUBTYPE_FLOAT));
        int ch = wf->nChannels, rate = (int)wf->nSamplesPerSec;
        /* The shared-mode mix is 32-bit float in practice; 16-bit PCM is the only other kind handled. */
        if (((is_float && wf->wBitsPerSample == 32) || (!is_float && wf->wBitsPerSample == 16)) && rate <= 192000 &&
            SUCCEEDED(ac->lpVtbl->Start(ac))) {
            ok = TRUE;
            state(AUDIO_LOOPBACK);
            HRESULT hr = S_OK;
            while (!cap.stop && SUCCEEDED(hr)) {
                Sleep(10);
                UINT32 n;
                BYTE *data;
                DWORD flags;
                while ((hr = cc->lpVtbl->GetNextPacketSize(cc, &n)) == S_OK && n &&
                       SUCCEEDED(hr = cc->lpVtbl->GetBuffer(cc, &data, &n, &flags, NULL, NULL))) {
                    for (UINT32 i = 0; i < n; i++) {
                        short l = 0, r = 0;
                        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && is_float) {
                            const float *f = (const float *)data + i * ch;
                            l = to16(f[0]), r = to16(ch > 1 ? f[1] : f[0]); /* front left and right */
                        } else if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                            const short *s = (const short *)data + i * ch;
                            l = s[0], r = ch > 1 ? s[1] : s[0];
                        }
                        push(l, r, rate);
                    }
                    cc->lpVtbl->ReleaseBuffer(cc, n);
                }
            } /* a failure here is usually the output device changing: the caller starts over */
            ac->lpVtbl->Stop(ac);
        }
    }
    if (cc) cc->lpVtbl->Release(cc);
    if (ac) ac->lpVtbl->Release(ac);
    if (dev) dev->lpVtbl->Release(dev);
    en->lpVtbl->Release(en);
    CoTaskMemFree(wf);
    return ok;
}

/* The recording input that carries what the speakers play, by the names drivers give it. */
static int stereo_mix_device(void)
{
    static const wchar_t *names[] = {L"stereo mix", L"what u hear", L"wave out mix", L"loopback", L"mix"};
    UINT count = waveInGetNumDevs();
    for (int k = 0; k < (int)(sizeof names / sizeof names[0]); k++)
        for (UINT i = 0; i < count; i++) {
            WAVEINCAPSW c;
            if (waveInGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
            CharLowerW(c.szPname);
            if (wcsstr(c.szPname, names[k])) return (int)i;
        }
    return -1;
}

static BOOL stereo_mix(void)
{
    int dev = stereo_mix_device();
    if (dev < 0) return FALSE;
    static short buf[4][44100 * 2 * CHUNK_MS / 1000];
    WAVEHDR hdr[4] = {{0}};
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    HWAVEIN h;
    if (waveInOpen(&h, (UINT)dev, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(ev);
        return FALSE;
    }
    for (int i = 0; i < 4; i++) {
        hdr[i].lpData = (LPSTR)buf[i];
        hdr[i].dwBufferLength = sizeof buf[i];
        waveInPrepareHeader(h, &hdr[i], sizeof hdr[i]);
        waveInAddBuffer(h, &hdr[i], sizeof hdr[i]);
    }
    waveInStart(h);
    state(AUDIO_STEREO_MIX);
    while (!cap.stop) {
        WaitForSingleObject(ev, 100);
        for (int i = 0; i < 4; i++)
            if (hdr[i].dwFlags & WHDR_DONE) {
                const short *s = (const short *)hdr[i].lpData;
                for (DWORD j = 0; j < hdr[i].dwBytesRecorded / 4; j++) push(s[2 * j], s[2 * j + 1], 44100);
                waveInAddBuffer(h, &hdr[i], sizeof hdr[i]);
            }
    }
    waveInReset(h);
    for (int i = 0; i < 4; i++) waveInUnprepareHeader(h, &hdr[i], sizeof hdr[i]);
    waveInClose(h);
    CloseHandle(ev);
    return TRUE;
}

static DWORD WINAPI capture_thread(void *unused)
{
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (!cap.stop) {
        BOOL ran = loopback() || stereo_mix();
        if (!ran) state(AUDIO_NONE);
        /* Look again in a while: a device may be plugged in or Stereo Mix switched on. */
        for (int i = 0; i < (ran ? 10 : 50) && !cap.stop; i++) Sleep(100);
    }
    CoUninitialize();
    return 0;
}

void audio_capture_start(void (*send)(const unsigned char *, int))
{
    if (cap.thread) return;
    cap.stop = 0;
    cap.send = send;
    cap.frames = 0;
    cap.thread = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
}

void audio_capture_stop(void)
{
    if (!cap.thread) return;
    InterlockedExchange(&cap.stop, 1);
    WaitForSingleObject(cap.thread, 3000);
    CloseHandle(cap.thread);
    cap.thread = NULL;
}

/* ---- playback (main PC, UI thread) ---- */

#define SLOTS 32

static struct {
    HWAVEOUT h;
    int rate;
    BOOL paused;
    WAVEHDR hdr[SLOTS];
    char *buf[SLOTS];
} pl;

void audio_play_stop(void)
{
    if (!pl.h) return;
    waveOutReset(pl.h);
    for (int i = 0; i < SLOTS; i++)
        if (pl.hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(pl.h, &pl.hdr[i], sizeof pl.hdr[i]);
    waveOutClose(pl.h);
    pl.h = NULL;
    pl.rate = 0;
}

void audio_play(const unsigned char *m, int len)
{
    if (len < 9 || (len - 5) % 4) return;
    int rate = (int)(m[1] | m[2] << 8 | m[3] << 16 | (DWORD)m[4] << 24);
    if (rate < 8000 || rate > 192000) return;
    if (rate != pl.rate) {
        audio_play_stop();
        WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, (DWORD)rate, (DWORD)rate * 4, 4, 16, 0};
        if (waveOutOpen(&pl.h, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            pl.h = NULL;
            return;
        }
        pl.rate = rate;
        waveOutPause(pl.h);
        pl.paused = TRUE;
    }
    int per_ms = rate * 4 / 1000, queued = 0, slot = -1;
    for (int i = 0; i < SLOTS; i++) {
        WAVEHDR *hd = &pl.hdr[i];
        if ((hd->dwFlags & WHDR_PREPARED) && !(hd->dwFlags & WHDR_DONE)) queued += (int)hd->dwBufferLength;
        else if (slot < 0) slot = i;
    }
    /* ponytail: the two PCs' sound clocks drift; dropping a chunk past 250 ms is the whole correction */
    if (queued > MAX_AHEAD_MS * per_ms || slot < 0) return;
    if (!queued && !pl.paused) waveOutPause(pl.h), pl.paused = TRUE; /* ran dry: build a cushion again */

    WAVEHDR *hd = &pl.hdr[slot];
    if (hd->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(pl.h, hd, sizeof *hd);
    if (!pl.buf[slot] && !(pl.buf[slot] = malloc(LINK_MAX))) return;
    memcpy(pl.buf[slot], m + 5, len - 5);
    memset(hd, 0, sizeof *hd);
    hd->lpData = pl.buf[slot];
    hd->dwBufferLength = (DWORD)(len - 5);
    waveOutPrepareHeader(pl.h, hd, sizeof *hd);
    waveOutWrite(pl.h, hd, sizeof *hd);
    if (pl.paused && queued + len - 5 >= PREBUFFER_MS * per_ms) waveOutRestart(pl.h), pl.paused = FALSE;
}
