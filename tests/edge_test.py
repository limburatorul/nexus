"""The Linux twin of edge_test.c: staggered screens, corners, resistance, the fade and the arrival grace.
The layout is a 4K screen with a 1080p one to its right, tops aligned. Runs where the Linux version's
modules import (python tests/edge_test.py from the repository)."""

import os
import sys
from collections import namedtuple

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "linux"))
from nexus.wayland import (BOTTOM, CROSS, HOLD, LANDING_CLEAR_PX, LEFT, PASS, RIGHT, TOP, Press,  # noqa: E402
                           along_on, in_corner, landing, span, wall_at)

Screen = namedtuple("Screen", "x y width height")
screens = [Screen(0, 0, 3840, 2160), Screen(3840, 0, 1920, 1080)]
b = (0, 0, 5759, 2159)

# Staggered: the 4K's right edge below the 1080p screen is a wall too, not only the desktop's.
assert wall_at(screens, 5759, 500, RIGHT) and wall_at(screens, 3839, 1500, RIGHT)
assert not wall_at(screens, 3839, 500, RIGHT) and not wall_at(screens, 5759, 500, LEFT)
assert wall_at(screens, 4000, 1079, BOTTOM) and not wall_at(screens, 4000, 1080, BOTTOM)
# Arrivals spread over the outermost screens, as in 0.4: only the 1080p one on the right, so 60% down it is
# y 647, not 1295, and leaving 60% down it says 60%. Leaving through the 4K's wall below it counts along that
# stretch of wall: halfway down it is 50%.
assert span(screens, b, RIGHT) == (0, 1079)
assert along_on(screens, b, 5759, 647, RIGHT) * 100 // 65535 == 59
assert along_on(screens, b, 3839, 1620, RIGHT) * 100 // 65535 == 50
assert span(screens, b, LEFT) == (0, 2159)
assert span(screens, b, TOP) == (0, 5759)
assert span(screens, b, BOTTOM) == (0, 3839)
assert span([], b, RIGHT) == (0, 2159)

# Arrivals land just inside the outermost wall, clear of its ends.
assert landing(screens, b, RIGHT, 0) == (5757, LANDING_CLEAR_PX)
assert landing(screens, b, RIGHT, 65535) == (5757, 1079 - LANDING_CLEAR_PX)
x, y = landing(screens, b, BOTTOM, 49151)  # 75% along the bottom: on the 4K
assert y == 2157 and 2800 < x < 2900

# Corners: near either end of a wall a push does not count, mid-wall it does.
assert in_corner(screens, 5759, 3, RIGHT) and in_corner(screens, 5759, 1075, RIGHT)
assert not in_corner(screens, 5759, 500, RIGHT)
assert in_corner(screens, 3839, 1083, RIGHT) and not in_corner(screens, 3839, 1500, RIGHT)

now = [100.0]


def at(t):
    now[0] = 100.0 + t


def press():
    return Press(lambda: now[0])


# Resistance: 120 px of push in quick steps gives; less does not.
at(0)
p = press()
p.feed(True, 50, 120)
at(0.01)
assert p.feed(True, 50, 120) == HOLD
at(0.02)
assert p.feed(True, 50, 120) == CROSS
# A slow lean fades away: 40 px every 200 ms never adds up to 120.
p = press()
for i in range(20):
    at(1 + i * 0.2)
    assert p.feed(True, 40, 120) == HOLD
# Moving back in takes the push away; off the wall a push counts for nothing; no resistance: a touch.
at(5)
p = press()
p.feed(True, 100, 120)
assert p.feed(False, -90, 120) == PASS and p.feed(True, 100, 120) == HOLD
p = press()
assert p.feed(False, 500, 120) == PASS and p.feed(True, 1, 0) == CROSS
# Arrival grace: the edge holds right after arriving, until 0.3 s pass or the pointer moves 4 px back in.
at(10)
p.arrive()
at(10.1)
assert p.feed(True, 500, 0) == HOLD
at(10.4)
assert p.feed(True, 1, 0) == CROSS
at(11)
p.arrive()
assert p.feed(False, -4, 0) == PASS and p.feed(True, 1, 0) == CROSS
print("edge_test.py: ok")
