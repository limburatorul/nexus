"""The connection, as src/nexus.c's network thread: a controlled PC listens, the main PC dials; the
handshake, then every message goes to on_message, with pings answered here and sent each second.

Callbacks run on the network thread; the Qt app moves them to its own thread.
"""

import socket
import threading
import time

from . import link

def arp_mac(ip: str) -> str:
    """The MAC the kernel's neighbour table has for ip ("aa-bb-.."), else "": for Wake-on-LAN. Read from
    /proc/net/arp, a fixed kernel format, never from "arp -a" text, which changes with the language."""
    try:
        with open("/proc/net/arp") as f:
            for line in f.readlines()[1:]:
                cols = line.split()
                if len(cols) >= 4 and cols[0] == ip and cols[3] != "00:00:00:00:00:00":
                    return cols[3].replace(":", "-").lower()
    except OSError:
        pass
    return ""


# The same numbers as the Windows NS_* states, so test output reads the same on both.
SETUP, WAITING, CONNECTING, UNREACHABLE, PASSWORD, VERSION, ROLE, PEER, PORT, CONNECTED = range(10)


class Session:
    def __init__(self, config, name: str, version: str, port: int, on_status, on_message, files=None, index=0):
        """index: which of the main PC's controlled PCs (peer..peer4); a controlled PC uses 0 only."""
        self.config, self.name, self.version, self.port, self.index = config, name, version, port, index
        self.on_status, self.on_message, self.files = on_status, on_message, files
        self.link = None
        self.peer = ""
        self.peer_version = ""
        self.rtt, self.audio_state = None, 0  # the Qt thread's, about this peer
        self.secret = False  # forward secret (both ends 0.5+)
        self.mac = ""  # learnt after a connection, for Wake-on-LAN
        self.heard = 0.0  # when the last frame came in (time.monotonic)
        self.beat = False  # set while input is across: ping every 250 ms instead of every second
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._gen = 0
        self._listener = None
        self._sock = None
        self.status = SETUP

    def start(self) -> None:
        threading.Thread(target=self._run, daemon=True, name="nexus-net%d" % self.index).start()
        threading.Thread(target=self._ping, daemon=True, name="nexus-ping%d" % self.index).start()

    def restart(self) -> None:
        """Drop the connection and read the config again (after the user changed it)."""
        with self._lock:
            self._gen += 1
            if self._sock:
                try:
                    self._sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            if self._listener:
                self._listener.close()
                self._listener = None
        self._wake.set()

    def send(self, msg: bytes) -> bool:
        link_now = self.link
        if not link_now:
            return False
        try:
            link_now.send(msg)
            return True
        except OSError:
            return False

    # ---- internals ----

    def _set_status(self, status: int) -> None:
        if status != self.status:
            self.status = status
            self.on_status(status)

    def _ping(self) -> None:
        tick = 0
        while True:
            time.sleep(0.25)
            tick += 1
            if self.link and (self.beat or tick % 4 == 0):
                self.send(bytes([link.PING]) + (int(time.monotonic() * 1000) & 0xFFFFFFFF).to_bytes(4, "little"))

    def drop(self) -> None:
        """The link went silent: close it, and this thread connects again."""
        with self._lock:
            if self._sock:
                try:
                    self._sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def _wait(self, seconds: float) -> None:
        self._wake.wait(seconds)
        self._wake.clear()

    def _listen_socket(self):
        with self._lock:
            if not self._listener:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try:
                    s.bind(("", self.port))
                    s.listen(2)
                    self._listener = s
                except OSError:
                    s.close()
            return self._listener

    def _run(self) -> None:
        gen, sticky = -1, 0  # the last failure stays on screen until a success or a config change
        while True:
            with self._lock:
                cfg = self.config
                if gen != self._gen:
                    gen, sticky = self._gen, 0
            addr = cfg.peers[self.index] if cfg.role == "main" else ""
            if not cfg.key or (addr == "" if cfg.role == "main" else self.index):
                self._set_status(SETUP)
                self._wait(3600)
                continue
            if cfg.role == "main":
                with self._lock:
                    if self._listener:
                        self._listener.close()
                        self._listener = None
                self._set_status(sticky or CONNECTING)
                try:
                    sock = socket.create_connection((addr, self.port), timeout=3)
                except OSError:
                    sticky = UNREACHABLE
                    self._set_status(sticky)
                    self._wait(3)
                    continue
            else:
                listener = self._listen_socket()
                if not listener:
                    self._set_status(PORT)
                    self._wait(3)
                    continue
                self._set_status(sticky or WAITING)
                try:
                    sock, _ = listener.accept()
                except OSError:
                    continue  # closed for a config change
            sticky = self._session(sock, cfg)
            if sticky:
                self._set_status(sticky)
            if sticky or cfg.role == "main":
                self._wait(3 if sticky else 0.5)

    def _session(self, sock, cfg) -> int:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(6)  # both ends ping every second; silence this long means the link is gone
        with self._lock:
            self._sock = sock
        try:
            ln = link.Link(sock, cfg.role == "main", cfg.key, self.name, self.version)
        except link.WrongPassword:
            sock.close()
            return PASSWORD
        except link.LinkError as e:
            sock.close()
            text = str(e)
            return VERSION if "version" in text else ROLE if "role" in text else PEER if "Nexus" in text else (
                UNREACHABLE if cfg.role == "main" else 0)
        except OSError:
            sock.close()
            return UNREACHABLE if cfg.role == "main" else 0
        self.peer, self.peer_version, self.secret, self.link = ln.peer, ln.peer_version, ln.forward_secret, ln
        self.heard = time.monotonic()
        if cfg.role == "main":
            try:
                self.mac = arp_mac(sock.getpeername()[0]) or self.mac
            except OSError:
                pass
        self._set_status(CONNECTED)
        try:
            while True:
                msg = ln.recv()
                self.heard = time.monotonic()
                kind = msg[0]
                if kind == link.PING and len(msg) == 5:
                    self.send(bytes([link.PONG]) + msg[1:])
                elif self.files and link.FILE_BEGIN <= kind <= link.FILE_END:
                    self.files.receive(msg)  # written to disk here, as on Windows
                else:
                    self.on_message(msg)
        except Exception:  # closed, timed out, or a forged frame: the link is over either way
            pass
        finally:
            self.link = None
            if self.files:
                self.files.abort()
            with self._lock:
                self._sock = None
            sock.close()
        return 0
