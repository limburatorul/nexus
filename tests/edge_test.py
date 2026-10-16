"""The Linux twin of edge_test.c: a 4K screen with a 1080p one to its right, tops aligned.
Runs where the Linux version's dependencies are (python tests/edge_test.py from the repository)."""

import os
import sys
from collections import namedtuple

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "linux"))
from nexus.wayland import BOTTOM, LEFT, RIGHT, TOP, span  # noqa: E402

Screen = namedtuple("Screen", "x y width height")
screens = [Screen(0, 0, 3840, 2160), Screen(3840, 0, 1920, 1080)]
b = (0, 0, 5759, 2159)
assert span(screens, b, RIGHT) == (0, 1079)  # only the 1080p screen: 60% down it is y 647, not 1295
assert span(screens, b, LEFT) == (0, 2159)
assert span(screens, b, TOP) == (0, 5759)
assert span(screens, b, BOTTOM) == (0, 3839)
assert span([], b, RIGHT) == (0, 2159)
print("edge_test.py: ok")
