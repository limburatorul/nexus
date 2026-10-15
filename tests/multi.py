"""Drives a main PC with three controlled PCs on its map: B right of this PC, C right of B, D left of it.

    python tests/multi.py <main IP> <B IP> <C IP> <D IP>

The main PC's test.ini lists B, C and D as peer, peer2 and peer3 at places 1,0, 2,0 and -1,0. The pointer
crosses into B, on through B's right edge into C, back through C's left edge into B, home through B's
left edge, and then into D; text copied on C comes back along the chain.
"""

import secrets
import sys
import time

from driver import Machine, until


def push(main, ctrl, dx, steps=60):
    """Moves the pointer on the PC with input until it leaves that PC (or steps run out)."""
    for _ in range(steps):
        main("move %d 0" % dx)
        if ctrl("status").split()[5] == "active=0":
            return True
        time.sleep(0.02)
    return False


def target(main):
    for line in main("peers").splitlines():
        if line.endswith(" *"):
            return int(line.split()[0])
    return -1


def run(main_host, b_host, c_host, d_host):
    main, b, c, d = Machine(main_host), Machine(b_host), Machine(c_host), Machine(d_host)
    until("all connected", lambda: main("peers").count(" 9 ") == 3, 30)
    tag = secrets.token_hex(3)

    main("cross 0")
    until("input on B", lambda: "active=1" in b("status"), 5)
    print("ok  this PC -> B")

    assert push(main, b, 400), "B never let go through its right edge"
    until("input on C", lambda: "active=1" in c("status"), 5)
    assert target(main) == 1, main("peers")
    print("ok  B -> C through B's right edge")

    text = "chain %s" % tag
    c("clip-set " + text)
    time.sleep(1)
    assert push(main, c, -400), "C never let go through its left edge"
    until("input back on B", lambda: "active=1" in b("status"), 5)
    until("C's text on B", lambda: b("clip-get") == "text:" + text, 10)
    print("ok  C -> B through C's left edge, with C's copied text")

    assert push(main, b, -400), "B never let go through its left edge"
    until("input home", lambda: "remote=0" in main("status"), 5)
    assert main("clip-get") == "text:" + text, main("clip-get")
    print("ok  B -> this PC through B's left edge, text here too")

    main("cross 2")
    until("input on D", lambda: "active=1" in d("status"), 5)
    assert push(main, d, 400), "D never let go through its right edge"
    until("input home from D", lambda: "remote=0" in main("status"), 5)
    print("ok  this PC -> D and back through D's right edge")
    print("all passed")


if __name__ == "__main__":
    run(*sys.argv[1:5])
