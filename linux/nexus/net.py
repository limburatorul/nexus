"""The connection, as src/nexus.c's network thread: a controlled PC listens, the main PC dials; the
handshake, then every message goes to on_message, with pings answered here and sent each second.

Callbacks run on the network thread; the Qt app moves them to its own thread.
"""

import logging
import socket
import threading
import time

from . import link

LOG = logging.getLogger("nexus")

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
        self._chans = {}  # kind -> (socket, Link): a 0.7 PC's own files and clipboard connections
        self._session_lock = threading.Lock()  # controlled PC: one input link at a time
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
        shown = -1  # controlled PC: "Waiting" shown for this config; a link's thread says the rest
        while True:
            with self._lock:
                cfg = self.config
                if gen != self._gen:
                    gen, sticky = self._gen, 0
            addr = cfg.peers[self.index] if cfg.role == "main" else ""
            if not cfg.key or (addr == "" if cfg.role == "main" else self.index):
                self._set_status(SETUP)
                shown = -1
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
                sticky = self._session(sock, cfg)
                if sticky:
                    self._set_status(sticky)
                self._wait(3 if sticky else 0.5)
                continue
            listener = self._listen_socket()
            if not listener:
                self._set_status(PORT)
                shown = -1
                self._wait(3)
                continue
            if not self.link and shown != gen:
                self._set_status(WAITING)
                shown = gen
            try:
                sock, _ = listener.accept()
            except OSError:
                continue  # closed for a config change
            # Each connection on its own thread: the input link, and a 0.7 main PC's files and clipboard.
            threading.Thread(target=self._accepted, args=(sock, cfg), daemon=True).start()

    def _handshake(self, sock, cfg):
        """(the Link, 0), or (None, the status to show) with the socket closed."""
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(6)  # both ends ping every second; silence this long means the link is gone
        try:
            return link.Link(sock, cfg.role == "main", cfg.key, self.name, self.version), 0
        except link.WrongPassword:
            sock.close()
            return None, PASSWORD
        except link.LinkError as e:
            sock.close()
            text = str(e)
            return None, VERSION if "version" in text else ROLE if "role" in text else PEER if "Nexus" in text else (
                UNREACHABLE if cfg.role == "main" else 0)
        except OSError:
            sock.close()
            return None, UNREACHABLE if cfg.role == "main" else 0

    def _session(self, sock, cfg) -> int:
        """Main PC: one connection to the controlled PC, from the handshake to its end."""
        with self._lock:
            self._sock = sock
        ln, err = self._handshake(sock, cfg)
        if ln:
            self._serve(sock, ln, cfg)
        else:
            with self._lock:
                self._sock = None
        return err

    def _accepted(self, sock, cfg) -> None:
        """Controlled PC: a connection from the main PC, its input link or (0.7) one for files or the clipboard."""
        ln, err = self._handshake(sock, cfg)
        kind = link.CHAN_INPUT
        if ln and link.v7(self.version) and link.v7(ln.peer_version):
            try:
                msg = ln.recv()
            except Exception:
                msg = b""
            kind = msg[1] if len(msg) == 2 and msg[0] == link.CHANNEL and msg[1] <= link.CHAN_CLIP else None
        for _ in range(20):  # the input link's own thread is a step behind
            if kind in (None, link.CHAN_INPUT) or self.link:
                break
            time.sleep(0.05)
        if not ln or kind is None or (kind != link.CHAN_INPUT and not self.link):
            sock.close()
            if err and not self.link:
                self._set_status(err)
            return
        if kind != link.CHAN_INPUT:
            self._chan(kind, sock, ln)
            return
        with self._lock:  # the main PC connecting again: its old link may not have timed out here yet
            if self._sock:
                try:
                    self._sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
        with self._session_lock:
            with self._lock:
                self._sock = sock
            self._serve(sock, ln, cfg)
            self._set_status(WAITING)

    def _serve(self, sock, ln, cfg) -> None:
        """The input link after its handshake. A main PC opens a 0.7 PC's files and clipboard connections."""
        # An older PC gets file copies on this link too: uncapped, megabytes of a copy queue ahead of input.
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
        own = cfg.role == "main" and link.v7(self.version) and link.v7(ln.peer_version)
        if own:
            ln.send(bytes([link.CHANNEL, link.CHAN_INPUT]))
        self.peer, self.peer_version, self.secret, self.link = ln.peer, ln.peer_version, ln.forward_secret, ln
        self.heard = time.monotonic()
        if cfg.role == "main":
            try:
                addr = sock.getpeername()[0]
                self.mac = arp_mac(addr) or self.mac
                for kind in (link.CHAN_FILES, link.CHAN_CLIP) if own else ():
                    threading.Thread(target=self._chan_dial, args=(kind, addr, cfg), daemon=True).start()
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
                    self.files.receive(msg)  # an older PC's: written to disk here, as on Windows
                else:
                    self.on_message(msg)
        except Exception:  # closed, timed out, or a forged frame: the link is over either way
            pass
        finally:
            self.link = None
            with self._lock:  # its files and clipboard connections go with it
                chans = [s for s, _ in self._chans.values()]
                self._sock = None
            for s in chans:
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            if self.files:
                self.files.abort()
            sock.close()

    def _chan_dial(self, kind: int, addr: str, cfg) -> None:
        """Main PC: opens one of a 0.7 PC's own connections, beside its input link."""
        sock = None
        try:
            sock = socket.create_connection((addr, self.port), timeout=3)
            sock.settimeout(6)
            ln = link.Link(sock, True, cfg.key, self.name, self.version)
            ln.send(bytes([link.CHANNEL, kind]))
        except Exception:
            LOG.info("link %d: no own connection for %s, so it shares the input link", self.index,
                     "files" if kind == link.CHAN_FILES else "the clipboard")
            if sock:
                sock.close()
            return
        self._chan(kind, sock, ln)

    def _chan(self, kind: int, sock, ln) -> None:
        """A files or clipboard connection after its handshake, until it or the input link ends. Files are
        written to disk on this thread; the clipboard goes to on_message as the input link's does."""
        sock.settimeout(None)  # silent between copies; it closes with the input link instead
        with self._lock:
            old = self._chans.get(kind)
            self._chans[kind] = (sock, ln)
        if old:  # a newer one replaces it
            try:
                old[0].shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        try:
            while True:
                msg = ln.recv()
                if kind == link.CHAN_FILES and self.files and link.FILE_BEGIN <= msg[0] <= link.FILE_END:
                    self.files.receive(msg)
                elif kind == link.CHAN_CLIP and (msg[0] == link.CLIPBOARD or link.CLIP_BEGIN <= msg[0] <= link.CLIP_END):
                    self.on_message(msg)
        except Exception:
            pass
        finally:
            with self._lock:
                if self._chans.get(kind, (None,))[0] is sock:
                    del self._chans[kind]
            if kind == link.CHAN_FILES and self.files:
                self.files.abort()
            sock.close()

    def send_chan(self, kind: int, msg: bytes) -> bool:
        """Files or the clipboard: on that kind's own connection (0.7), else on the input link as before."""
        chan = self._chans.get(kind)
        if not chan:
            return self.send(msg)
        try:
            chan[1].send(msg)
            return True
        except OSError:
            return False
