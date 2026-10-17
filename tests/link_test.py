"""The Linux twin of link_test.c: the link over loopback with 0.5's forward-secret handshake, a 0.4 peer
beside it, a wrong password, and the clipboard's pieces. Needs PyNaCl (python tests/link_test.py)."""

import os
import socket
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "linux"))
from nexus import link  # noqa: E402


def pair(pw_main, pw_ctrl, v_main, v_ctrl):
    a, b = socket.socketpair()
    out = {}

    def side(name, sock, is_main, pw, version):
        try:
            out[name] = link.Link(sock, is_main, link.key_from_password(pw), name, version)
        except link.LinkError as e:
            out[name] = e

    t = threading.Thread(target=side, args=("main", a, True, pw_main, v_main))
    t.start()
    side("ctrl", b, False, pw_ctrl, v_ctrl)
    t.join()
    return out["main"], out["ctrl"]


m, c = pair("pw", "pw", "0.5.0", "0.5.0")
assert m.forward_secret and c.forward_secret and c.peer_version == "0.5.0"
m.send(b"\x02ping")
assert c.recv() == b"\x02ping"
c.send(b"\x03pong")
assert m.recv() == b"\x03pong"
first = m._ktx
m2, _c2 = pair("pw", "pw", "0.5.0", "0.5.0")
assert m2._ktx != first  # new ephemeral keys every time

m, c = pair("pw", "pw", "0.5.0", "0.4.0")  # a 0.4 peer: no KEX, and it still works
assert not m.forward_secret and not c.forward_secret
m.send(b"\x06\x01\x00\x01\x00")
assert c.recv() == b"\x06\x01\x00\x01\x00"

m, c = pair("pw", "other", "0.5.0", "0.5.0")
assert isinstance(m, link.WrongPassword) and isinstance(c, link.WrongPassword)

assert link.v5("0.5.0") and link.v5("1.0.0") and link.v5("0.10.2") and not link.v5("0.4.9") and not link.v5("test")

image = bytes((i * 13) & 0xFF for i in range(300000))
got = link.ClipIn()
msgs = list(link.clip_out(link.CLIP_DIB, image)) + list(link.clip_out(link.CLIP_TEXT, "hëllo".encode()))
assert len(msgs) == 1 + 5 + 2 and all(len(x) <= link.MAX for x in msgs)
assert all(got.feed(x) is None for x in msgs)
done = got.feed(bytes([link.CLIP_END]))
assert done == {link.CLIP_DIB: image, link.CLIP_TEXT: "hëllo".encode()}
got.feed(bytes([link.CLIP_BEGIN, link.CLIP_TEXT]) + (256 * 1024 + 1).to_bytes(4, "little"))  # over the limit
got.feed(bytes([link.CLIP_DATA]) + b"xyz")
assert got.feed(bytes([link.CLIP_END])) == {}
got.feed(bytes([link.CLIP_BEGIN, link.CLIP_TEXT]) + (2).to_bytes(4, "little"))  # says 2, sends 3
got.feed(bytes([link.CLIP_DATA]) + b"xyz")
assert got.feed(bytes([link.CLIP_END])) == {}
print("link_test.py: ok")
