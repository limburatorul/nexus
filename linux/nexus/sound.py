"""Sound, as on Windows: the controlled PC sends what its speakers play in 20 ms MSG_AUDIO chunks
([rate u32][16-bit stereo frames]); the main PC plays them. PulseAudio's parec/pacat do the work,
and work on PipeWire too (pipewire-pulse).
"""

import fcntl
import os
import shutil
import struct
import subprocess
import threading

from . import link

RATE = 48000
CHUNK = RATE * 4 * 20 // 1000  # 20 ms of 16-bit stereo
LOOPBACK, STEREO_MIX, NONE = 1, 2, 3  # MSG_AUDIO_STATE values (Windows calls PipeWire's monitor a loopback)


class Capture:
    def __init__(self, send, on_state):
        self.send, self.on_state = send, on_state
        self._proc = None
        self._stop = threading.Event()

    def start(self) -> None:
        self._stop.clear()
        threading.Thread(target=self._run, daemon=True, name="nexus-capture").start()

    def stop(self) -> None:
        self._stop.set()
        proc, self._proc = self._proc, None
        if proc:
            proc.terminate()

    def _run(self) -> None:
        while not self._stop.is_set():
            if not shutil.which("parec"):
                self._state(NONE)
                return
            self._proc = proc = subprocess.Popen(
                ["parec", "--device=@DEFAULT_MONITOR@", "--format=s16le", "--rate=%d" % RATE, "--channels=2",
                 "--latency-msec=20", "--raw"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            self._state(LOOPBACK)
            header = bytes([link.AUDIO]) + struct.pack("<I", RATE)
            while not self._stop.is_set():
                chunk = proc.stdout.read(CHUNK)
                if not chunk:
                    break
                if chunk.count(0) != len(chunk):  # pure silence is not sent
                    self.send(header + chunk)
            proc.wait()
            self._stop.wait(1)  # the sound server restarted, say: look again shortly

    def _state(self, state: int) -> None:
        self.send(bytes([link.AUDIO_STATE, state]))
        self.on_state(state)


class Player:
    def __init__(self):
        self._proc = None
        self._rate = 0

    def play(self, msg: bytes) -> None:
        if len(msg) < 9 or (len(msg) - 5) % 4:
            return
        (rate,) = struct.unpack("<I", msg[1:5])
        if not 8000 <= rate <= 192000:
            return
        if rate != self._rate or not self._proc or self._proc.poll() is not None:
            self.stop()
            if not shutil.which("pacat"):
                return
            self._proc = subprocess.Popen(
                ["pacat", "--playback", "--raw", "--format=s16le", "--rate=%d" % rate, "--channels=2",
                 "--latency-msec=60"], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL)
            fd = self._proc.stdin.fileno()
            fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)
            self._rate = rate
        try:
            os.write(self._proc.stdin.fileno(), msg[5:])
        except BlockingIOError:
            pass  # the player is behind: dropping a chunk keeps the delay from growing
        except OSError:
            self.stop()

    def stop(self) -> None:
        proc, self._proc, self._rate = self._proc, None, 0
        if proc:
            proc.kill()
