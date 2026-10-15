"""Drives two Nexus test instances ("nexus /test") end to end: text, pointer and files, both ways.

    python tests/driver.py key                     # once: a test password, key in build/test/key.hex
    python tests/driver.py ini main <controlled IP> # writes build/test/test.ini for a main PC
    python tests/driver.py ini controlled           # ... for a controlled PC
    python tests/driver.py run <main IP> <controlled IP>

Each test instance reads %APPDATA%\\Nexus\\test.ini (~/.config/nexus/test.ini on Linux), links on port
24810 and takes commands on 24811, authenticated with the same key, so it runs beside a normal Nexus
and never touches this PC's mouse and keyboard.
"""

import os
import secrets
import socket
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "linux"))
from nexus import link  # noqa: E402

TEST_DIR = os.path.join(os.path.dirname(__file__), "..", "build", "test")
CONTROL_PORT = 24811


def key() -> bytes:
    return bytes.fromhex(open(os.path.join(TEST_DIR, "key.hex")).read().strip())


class Machine:
    def __init__(self, host: str):
        sock = socket.create_connection((host, CONTROL_PORT), timeout=10)
        self.link = link.Link(sock, True, key(), "driver", "test")
        self.name = self.link.peer

    def __call__(self, command: str) -> str:
        self.link.send(bytes([link.TEST]) + command.encode("utf-8"))
        while True:
            msg = self.link.recv()
            if msg[0] == link.TEST:
                return msg[1:].decode("utf-8", "replace")

    def status(self) -> dict:
        fields = self("status").split(" ", 8)
        return dict(f.split("=", 1) for f in fields)


def until(what, check, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        got = check()
        if got:
            return got
        time.sleep(0.3)
    raise AssertionError("timed out waiting for " + what)


def run(main_host: str, controlled_host: str) -> None:
    main, ctrl = Machine(main_host), Machine(controlled_host)
    print("main %s, controlled %s" % (main.name, ctrl.name))
    until("both connected", lambda: main.status()["status"] == "9" and ctrl.status()["status"] == "9", 30)
    tag = secrets.token_hex(3)

    text = "nexus %s șț é" % tag
    main("clip-set " + text)
    main("cross")
    until("text on the controlled PC", lambda: ctrl("clip-get") == "text:" + text)
    print("ok  text main -> controlled")

    before = ctrl("cursor")
    main("move 40 0"), main("move 40 0")
    until("the pointer to move there", lambda: ctrl("cursor") != before, 5)
    print("ok  pointer moves on the controlled PC (%s -> %s)" % (before, ctrl("cursor")))

    back = "back %s" % tag
    ctrl("clip-set " + back)
    main("home")
    until("text back on the main PC", lambda: main("clip-get") == "text:" + back)
    print("ok  text controlled -> main")

    for sender, receiver, going in ((main, ctrl, "cross"), (ctrl, main, "home")):
        path = sender("temp") + "nexus-%s-%s.txt" % (sender.name, tag)
        content = "file from %s, %s\n" % (sender.name, tag) * 200
        assert sender("write %s|%s" % (path, content)) == "ok"
        sent = sender("fnv " + path)
        sender("files " + path)
        if going == "home":
            main("cross")  # input has to be over there before it can come home
            time.sleep(1)
        main(going)
        def arrived():  # the receiver's own earlier copy may still be on its clipboard
            clip = receiver("clip-get")
            return clip if clip.startswith("files:") and "Nexus Received" in clip and tag in clip else None
        got = until("files on %s" % receiver.name, arrived, 20)
        received = got[len("files:"):].split("|")[0]
        assert receiver("fnv " + received) == sent, (receiver("fnv " + received), sent)
        print("ok  file %s -> %s (%s)" % (sender.name, receiver.name, received))
    print("all passed")


def main_cli() -> None:
    os.makedirs(TEST_DIR, exist_ok=True)
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "key":
        password = secrets.token_urlsafe(18)
        with open(os.path.join(TEST_DIR, "key.hex"), "w") as f:
            f.write(link.key_from_password(password).hex())
        print("test key written")
    elif cmd == "ini":
        role, peer = sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else "")
        with open(os.path.join(TEST_DIR, "test.ini"), "w", newline="\r\n") as f:
            f.write("[nexus]\nrole=%s\nedge=right\npeer=%s\nkey=%s\n" % (role, peer, key().hex()))
        print("build/test/test.ini: %s %s" % (role, peer))
    elif cmd == "run":
        run(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)


if __name__ == "__main__":
    main_cli()
