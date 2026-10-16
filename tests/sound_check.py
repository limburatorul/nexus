"""Plays the main PC against a controlled Nexus test instance and listens to the sound it sends.

    python tests/sound_check.py <controlled IP> [seconds]

Play a tone on the controlled PC meanwhile; this prints the sound state, how much audio arrived, its loudness
and its pitch (from zero crossings), so a 440 Hz tone over there should read close to 440 Hz here.
"""

import socket
import struct
import sys
import time

from driver import key, link

host, seconds = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 6
s = socket.create_connection((host, 24810), timeout=10)
l = link.Link(s, True, key(), "sound-check", "0.3.0")
states, rate, samples, end = [], 0, [], time.time() + seconds
while time.time() < end:
    m = l.recv()
    if m[0] == link.PING:
        l.send(bytes([link.PONG]) + m[1:])
    elif m[0] == link.AUDIO_STATE:
        states.append(m[1])
    elif m[0] == link.AUDIO:
        rate = struct.unpack("<I", m[1:5])[0]
        pcm = m[5:]
        samples.extend(struct.unpack("<%dh" % (len(pcm) // 2), pcm)[::2])  # left channel
got = len(samples) / rate if rate else 0
peak = max((abs(v) for v in samples), default=0)
crossings = sum(1 for a, b in zip(samples, samples[1:]) if a < 0 <= b)
print("states %s, rate %d, %.1f s of sound in %.0f s, peak %d, pitch ~%.0f Hz"
      % (states, rate, got, seconds, peak, crossings / got if got else 0))
