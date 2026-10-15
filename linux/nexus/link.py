"""The Nexus link in Python: the same wire as src/link.c (SPEC.md), so Linux and Windows talk.

A connection opens with 30 clear bytes each way: b"NXUS", the link version, the role (0 main,
1 controlled) and a fresh 24-byte nonce. SHA-512(key || main nonce || controlled nonce) gives one
32-byte key per direction; every frame is then a little-endian u32 length and an XSalsa20-Poly1305
box whose nonce is that direction's frame counter. The first frame each way is HELLO: the PC's
name, a NUL, its Nexus version. A HELLO that does not open means the passwords differ.
"""

import hashlib
import os
import socket
import struct
import threading

from nacl.bindings import crypto_secretbox, crypto_secretbox_open
from nacl.exceptions import CryptoError

PORT = 24800
LINK_VERSION = 1
MAX = 65536  # largest plaintext message
MAGIC = b"NXUS"

(HELLO, PING, PONG, ENTER, LEAVE, MOVE, BUTTON, WHEEL, KEY, CLIPBOARD, AUDIO, AUDIO_STATE,
 FILE_BEGIN, FILE, FILE_DATA, FILE_END, UPDATE_BEGIN, UPDATE_DATA, UPDATE_END, TEST) = range(1, 21)


class LinkError(Exception):
    """The other end is gone, or is not a Nexus we can talk to."""


class WrongPassword(LinkError):
    pass


def key_from_password(password: str) -> bytes:
    """The stretched key kept in the config instead of the password (as link_key_from_password)."""
    h = hashlib.sha512(b"nexus1" + password.encode("utf-8")[:256]).digest()
    for _ in range(20000):
        h = hashlib.sha512(h).digest()
    return h[:32]


def _recv_all(sock: socket.socket, n: int) -> bytes:
    data = b""
    while len(data) < n:
        part = sock.recv(n - len(data))
        if not part:
            raise LinkError("connection closed")
        data += part
    return data


def _nonce(counter: int) -> bytes:
    return struct.pack("<Q", counter) + bytes(16)


class Link:
    """One handshaken connection. send() may be called from several threads; recv() from one."""

    def __init__(self, sock: socket.socket, is_main: bool, key: bytes, name: str, version: str):
        self.sock = sock
        self._send_lock = threading.Lock()
        mine = MAGIC + bytes([LINK_VERSION, 0 if is_main else 1]) + os.urandom(24)
        sock.sendall(mine)
        theirs = _recv_all(sock, 30)
        if theirs[:4] != MAGIC:
            raise LinkError("not Nexus")
        if theirs[4] != LINK_VERSION:
            raise LinkError("link version %d" % theirs[4])
        if theirs[5] == mine[5]:
            raise LinkError("both ends have the same role")
        main_nonce, controlled_nonce = (mine[6:], theirs[6:]) if is_main else (theirs[6:], mine[6:])
        h = hashlib.sha512(key + main_nonce + controlled_nonce).digest()
        self._ktx, self._krx = (h[:32], h[32:]) if is_main else (h[32:], h[:32])
        self._ntx = self._nrx = 0

        self.send(bytes([HELLO]) + name.encode("utf-8")[:63] + b"\0" + version.encode()[:15])
        try:
            hello = self.recv()
        except CryptoError:
            raise WrongPassword("the two PCs have different passwords") from None
        if hello[:1] != bytes([HELLO]):
            raise LinkError("no HELLO")
        name_part, _, version_part = hello[1:].partition(b"\0")
        self.peer = name_part.decode("utf-8", "replace")
        self.peer_version = version_part.decode("ascii", "replace")

    def send(self, msg: bytes) -> None:
        if not 1 <= len(msg) <= MAX:
            raise ValueError("message size %d" % len(msg))
        with self._send_lock:
            box = crypto_secretbox(msg, _nonce(self._ntx), self._ktx)
            self._ntx += 1
            self.sock.sendall(struct.pack("<I", len(box)) + box)

    def recv(self) -> bytes:
        """The next message; raises CryptoError for a frame that was altered or replayed."""
        (n,) = struct.unpack("<I", _recv_all(self.sock, 4))
        if not 17 <= n <= MAX + 16:
            raise LinkError("bad frame length %d" % n)
        box = _recv_all(self.sock, n)
        msg = crypto_secretbox_open(box, _nonce(self._nrx), self._krx)
        self._nrx += 1
        return msg
