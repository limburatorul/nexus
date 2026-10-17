"""Drives two Nexus test instances ("nexus /test") end to end: text, pointer and files, both ways.

    python tests/driver.py key                     # once: a test password, key in build/test/key.hex
    python tests/driver.py ini main <controlled IP> # writes build/test/test.ini for a main PC
    python tests/driver.py ini controlled           # ... for a controlled PC
    python tests/driver.py run <main IP> <controlled IP> [its number on the main PC, from 0, or push]
    python tests/driver.py edge <main IP> <controlled IP> [its number]  # resistance and corners there
    python tests/driver.py beat <main IP> <controlled IP> <controlled VM name>  # freezes the VM for a moment

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
        head, _, files = self("status").partition(" files=")  # the last field, and it has spaces
        fields = dict(f.split("=", 1) for f in head.split())
        fields["files"] = files
        return fields


def until(what, check, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        got = check()
        if got:
            return got
        time.sleep(0.3)
    raise AssertionError("timed out waiting for " + what)


def run(main_host: str, controlled_host: str, pc: str = "") -> None:
    """pc: which of the main PC's controlled PCs this is (peer = 0, the default: the first connected)."""
    main, ctrl = Machine(main_host), Machine(controlled_host)
    # "push": a Linux main PC pushes the pointer through its edge, since the desktop captures input only there
    cross = "push" if pc == "push" else ("cross " + pc).strip()
    print("main %s, controlled %s" % (main.name, ctrl.name))
    until("both connected", lambda: main.status()["status"] == "9" and ctrl.status()["status"] == "9", 30)
    tag = secrets.token_hex(3)

    text = "nexus %s șț é" % tag
    main("clip-set " + text)
    time.sleep(1)  # Linux: the desktop tells the portal about a copy a moment later
    main(cross)
    until("text on the controlled PC", lambda: ctrl("clip-get") == "text:" + text)
    print("ok  text main -> controlled")

    before = ctrl("cursor")
    # Towards the middle: at an edge with a PC past it, 0.5's edge would hold an 80 px push outwards.
    x, y = (int(v) for v in before.split())
    step = "move %d %d" % (40 if x < 400 else -40, 40 if y < 300 else -40)
    main(step), main(step)
    until("the pointer to move there", lambda: ctrl("cursor") != before, 5)
    print("ok  pointer moves on the controlled PC (%s -> %s)" % (before, ctrl("cursor")))

    back = "back %s" % tag
    ctrl("clip-set " + back)
    main("home")
    until("text back on the main PC", lambda: main("clip-get") == "text:" + back)
    print("ok  text controlled -> main")

    for sender, receiver, going in ((main, ctrl, cross), (ctrl, main, "home")):
        path = sender("temp") + "nexus-%s-%s.txt" % (sender.name, tag)
        content = "file from %s, %s\n" % (sender.name, tag) * 200
        assert sender("write %s|%s" % (path, content)) == "ok"
        sent = sender("fnv " + path)
        sender("files " + path)
        if going == "home":
            main(cross)  # input has to be over there before it can come home
            time.sleep(1)
        main(going)
        def arrived():  # the receiver's own earlier copy may still be on its clipboard
            clip = receiver("clip-get")
            return clip if clip.startswith("files:") and "Nexus Received" in clip and tag in clip else None
        got = until("files on %s" % receiver.name, arrived, 20)
        received = got[len("files:"):].split("|")[0]
        assert receiver("fnv " + received) == sent, (receiver("fnv " + received), sent)
        print("ok  file %s -> %s (%s)" % (sender.name, receiver.name, received))
    if all(m.status()["version"] >= "0.5" for m in (main, ctrl)):
        run5(main, ctrl, cross)
    print("all passed")


def run5(main, ctrl, cross="cross") -> None:
    """0.5: forward secrecy, text over 64 KB and a picture, both ways."""
    assert main.status()["secret"] == "1" and ctrl.status()["secret"] == "1", (main("status"), ctrl("status"))
    print("ok  the link is forward secret on both ends")
    main("home")
    time.sleep(1)
    for what, make, read, label in (("clip-big 200000", "clip-big 200000", "clip-hash", "200 KB of text"),
                                    ("clip-image 640 480", "clip-image 640 480", "clip-image-get", "a 640x480 picture")):
        for sender, receiver, going in ((main, ctrl, cross), (ctrl, main, "home")):
            if going == "home":
                main(cross)  # input has to be over there before it can come home
                time.sleep(1)
            assert sender(make) == "ok", what
            want = sender(read)
            assert want != "none", (what, want)
            time.sleep(1)
            main(going)
            until("%s on %s" % (label, receiver.name), lambda: receiver(read) == want, 20)
            print("ok  %s %s -> %s (%s)" % (label, sender.name, receiver.name, want))
            if going != "home":
                main("home")
                time.sleep(1)


def edge(main_host: str, controlled_host: str, pc: str = "") -> None:
    """0.5's edge on a controlled PC with another PC past its right edge (multi.py's B): a short push and a
    push in the corner hold, a full one gives. The main PC's resistance is the default, 120 px."""
    main, ctrl = Machine(main_host), Machine(controlled_host)
    until("connected", lambda: main.status()["status"] == "9", 30)
    main(("cross " + pc).strip())
    until("input over there", lambda: "active=1" in ctrl("status"), 5)
    here = lambda: "active=1" in ctrl("status")  # noqa: E731
    main("move 0 -3000"), main("move 3000 0")  # into the top right corner
    time.sleep(0.5)  # past the arrival grace
    for _ in range(5):
        main("move 200 0")
    time.sleep(0.3)
    assert here(), "the corner crossed"
    print("ok  1000 px of push in the corner: no crossing")
    main("move 0 300")  # down the wall, out of the corner
    time.sleep(1)  # the push so far fades
    main("move 60 0")
    time.sleep(0.3)
    assert here(), "60 px crossed"
    print("ok  60 px of push against the edge: no crossing")
    time.sleep(1)
    main("move 60 0"), main("move 70 0")
    until("input gone on", lambda: not here(), 3)
    print("ok  130 px of push in one go: crossed")


def beat(main_host: str, controlled_host: str, vm: str, pc: str = "") -> None:
    """0.5's dead-link detection: input on a controlled VM, the VM frozen (VBoxManage pause), input must be
    home within ~2 s instead of 6, and the link back once the VM runs again."""
    import subprocess
    vbox = os.path.join(os.environ.get("VBOX_MSI_INSTALL_PATH", r"C:\Program Files\Oracle\VirtualBox"), "VBoxManage.exe")
    main, ctrl = Machine(main_host), Machine(controlled_host)
    until("connected", lambda: main.status()["status"] == "9", 30)
    main(("cross " + pc).strip())
    until("input over there", lambda: "active=1" in ctrl("status"), 5)
    subprocess.run([vbox, "controlvm", vm, "pause"], check=True)
    start = time.time()
    try:
        until("input home", lambda: main.status()["remote"] == "0", 10)
        took = time.time() - start
    finally:
        subprocess.run([vbox, "controlvm", vm, "resume"], check=True)
    assert took < 3.5, took
    print("ok  frozen controlled PC: input home after %.1f s" % took)
    until("connected again", lambda: main.status()["status"] == "9", 30)
    print("ok  link back after the VM resumed")


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
        run(*sys.argv[2:5])
    elif cmd == "beat":
        beat(*sys.argv[2:6])
    elif cmd == "edge":
        edge(*sys.argv[2:5])
    else:
        print(__doc__)


if __name__ == "__main__":
    main_cli()
