/* The controlled PC's sound, played on the main PC. */
#ifndef AUDIO_H
#define AUDIO_H

enum { AUDIO_UNKNOWN, AUDIO_LOOPBACK, AUDIO_STEREO_MIX, AUDIO_NONE }; /* MSG_AUDIO_STATE values */

/* Controlled PC: captures on its own thread and hands each message (MSG_AUDIO, MSG_AUDIO_STATE) to send. */
void audio_capture_start(void (*send)(const unsigned char *msg, int len));
void audio_capture_stop(void);

/* Main PC, UI thread: plays one MSG_AUDIO message. */
void audio_play(const unsigned char *msg, int len);
void audio_play_stop(void);

#endif
