"""Wayland: replaying input and reaching the clipboard through the desktop's RemoteDesktop portal
session (Beamer's inject_portal, in linux/beamer). The first start asks the user once; the
desktop's restore token keeps the answer.
"""

import os
import struct
import sys
from urllib.parse import quote, unquote, urlparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "beamer"))
import clipboard_portal  # noqa: E402
import desktop_portal  # noqa: E402
import inject_portal  # noqa: E402

from . import keymap, link  # noqa: E402

LEFT, RIGHT, TOP, BOTTOM = range(4)
BUTTONS = {1: 0x110, 2: 0x111, 3: 0x112, 4: 0x113, 5: 0x114}  # left, right, middle, back, forward


def bounds():
    """The virtual desktop: (left, top, right, bottom), right and bottom inclusive."""
    screens = desktop_portal.monitors()
    if not screens:
        return 0, 0, 1919, 1079
    return (min(s.x for s in screens), min(s.y for s in screens),
            max(s.x + s.width - 1 for s in screens), max(s.y + s.height - 1 for s in screens))


# ---- walls, corners and the push against them: the same rules as src/input.c (pure, tests/edge_test.py).
# Port of Beamer core/return_edge.py (GPL-3, Toby George Read Kalkman).

STEP = ((-1, 0), (1, 0), (0, -1), (0, 1))  # a pixel through each edge
CORNER_PX = 8  # a push this near an end of a wall does not count
LANDING_CLEAR_PX = CORNER_PX + 2  # an arrival lands at least this far from the ends
GRACE_PX, GRACE_SECONDS = 4, 0.3  # after arriving, the edge holds until this much travel back in, or this long
DEFAULT_RESISTANCE = 120
DECAY_PX_PER_SECOND = DEFAULT_RESISTANCE / 0.4
PASS, HOLD, CROSS = range(3)


def on_screens(screens, x, y) -> bool:
    return any(s.x <= x < s.x + s.width and s.y <= y < s.y + s.height for s in screens)


def wall_at(screens, x, y, edge) -> bool:
    """On a screen with no screen one pixel past `edge`: the pointer stops here. Not the desktop's
    rectangle: beside a 4K screen with a 1080p one to its right, tops aligned, the 4K's right edge
    below the 1080p one is a wall too."""
    dx, dy = STEP[edge]
    return on_screens(screens, x, y) and not on_screens(screens, x + dx, y + dy)


def in_corner(screens, x, y, edge) -> bool:
    """Near an end of a wall: a window's Close button, a panel's corner."""
    vertical = edge in (LEFT, RIGHT)
    return any(not wall_at(screens, x + (0 if vertical else d), y + (d if vertical else 0), edge)
               for d in (-CORNER_PX, CORNER_PX))


def exposed(screens, s, edge):
    """The stretch [lo, hi) of screen s's side `edge` with no screen past it; lo >= hi for none."""
    vertical = edge in (LEFT, RIGHT)
    line = {LEFT: s.x - 1, RIGHT: s.x + s.width, TOP: s.y - 1, BOTTOM: s.y + s.height}[edge]

    def covering(t):
        return next((q for q in screens if (q.x <= line < q.x + q.width and q.y <= t < q.y + q.height if vertical
                                            else q.y <= line < q.y + q.height and q.x <= t < q.x + q.width)), None)
    lo, hi = (s.y, s.y + s.height) if vertical else (s.x, s.x + s.width)
    while lo < hi and covering(lo):
        q = covering(lo)
        lo = q.y + q.height if vertical else q.x + q.width
    while lo < hi and covering(hi - 1):
        q = covering(hi - 1)
        hi = q.y if vertical else q.x
    return lo, hi


def outer(s, b, edge) -> bool:
    """Whether screen s reaches the desktop's outer edge on side `edge`."""
    left, top, right, bottom = b
    return {LEFT: s.x == left, RIGHT: s.x + s.width - 1 == right, TOP: s.y == top,
            BOTTOM: s.y + s.height - 1 == bottom}[edge]


def span(screens, b, edge):
    """The stretch (lo, hi, inclusive) of side `edge` that the outermost screens cover, so screens of
    different sizes line up proportionally: with a 4K and a 1080p screen side by side, 60% of the way down
    the 1080p one is 60% of that screen, not of the desktop's height. Arrivals land here."""
    left, top, right, bottom = b
    at = [s for s in screens if outer(s, b, edge)]
    if edge in (LEFT, RIGHT):
        return (min(s.y for s in at), max(s.y + s.height - 1 for s in at)) if at else (top, bottom)
    return (min(s.x for s in at), max(s.x + s.width - 1 for s in at)) if at else (left, right)


def along_on(screens, b, x, y, edge) -> int:
    """Position along side `edge`, 0..65535, of a point leaving through it, as the Windows side counts it:
    along the outermost screens, or, through a wall further in, along that screen's own stretch of wall."""
    lo, hi = span(screens, b, edge)
    for s in screens:
        if s.x <= x < s.x + s.width and s.y <= y < s.y + s.height and not outer(s, b, edge):
            a, z = exposed(screens, s, edge)
            if a < z:
                lo, hi = a, z - 1
    return max(0, min(65535, ((y if edge in (LEFT, RIGHT) else x) - lo) * 65535 // max(1, hi - lo)))


def along(b, x, y, edge) -> int:
    return along_on(desktop_portal.monitors(), b, x, y, edge)


def landing(screens, b, edge, frac):
    """A point just inside the outermost wall on side `edge`, frac along it, clear of the corners."""
    lo, hi = span(screens, b, edge)
    at = lo + frac * (hi - lo) // 65535
    best = None
    for s in screens:
        a, z = exposed(screens, s, edge)
        if a < z and outer(s, b, edge):
            d = a - at if at < a else at - z + 1 if at >= z else 0
            if best is None or d < best[0]:
                best = (d, s, a, z)
    if best is None:
        left, top, right, bottom = b
        return (left + right) // 2, (top + bottom) // 2
    _d, s, a, z = best
    if z - a > 2 * LANDING_CLEAR_PX:
        a, z = a + LANDING_CLEAR_PX, z - LANDING_CLEAR_PX
    at = max(a, min(z - 1, at))
    return {LEFT: (s.x + 2, at), RIGHT: (s.x + s.width - 3, at), TOP: (at, s.y + 2), BOTTOM: (at, s.y + s.height - 3)}[edge]


def on_edge(b, edge, frac):
    return landing(desktop_portal.monitors(), b, edge, frac)


def outward(edge, dx, dy) -> float:
    return (-dx, dx, -dy, dy)[edge]


class Press:
    """The pressure against one edge: outward travel at the wall builds it, travel back in and time take
    it away, and at `resistance` px it gives. Right after input arrives the edge holds (grace)."""

    def __init__(self, clock=None):
        import time
        self.clock = clock or time.monotonic
        self.travel = self.inward = 0.0
        self.last = self.clock()
        self.arrival = None

    def arrive(self) -> None:
        self.arrival, self.inward, self.travel = self.clock(), 0.0, 0.0

    def feed(self, at_wall: bool, out: float, resistance: int) -> int:
        now = self.clock()
        self.travel = max(0.0, self.travel - (now - self.last) * DECAY_PX_PER_SECOND)
        self.last = now
        if self.arrival is not None and out < 0:
            self.inward -= out
        grace = self.arrival is not None and self.inward < GRACE_PX and now - self.arrival < GRACE_SECONDS
        if not grace:
            self.arrival = None
        if out <= 0 or not at_wall:
            if out < 0:
                self.travel = max(0.0, self.travel + out)
            return PASS
        if grace:
            return HOLD
        self.travel += out
        if self.travel < resistance:
            return HOLD
        self.travel = 0.0
        return CROSS


def screen_locked() -> bool:
    """Whether the desktop's lock screen is up, asked over the session bus (KDE, then GNOME)."""
    global _bus
    try:
        from jeepney import DBusAddress, new_method_call
        from jeepney.io.blocking import open_dbus_connection
        if _bus is None:
            _bus = open_dbus_connection(bus="SESSION")
        for name, path in (("org.freedesktop.ScreenSaver", "/ScreenSaver"), ("org.gnome.ScreenSaver", "/org/gnome/ScreenSaver")):
            try:
                reply = _bus.send_and_get_reply(new_method_call(DBusAddress(path, bus_name=name, interface=name),
                                                                "GetActive"), timeout=1)
                if reply.body:
                    return bool(reply.body[0])
            except Exception:
                continue
    except Exception:
        _bus = None
    return False


_bus = None


class Injector:
    """The controlled PC's side: what the main PC sends, played here."""

    def __init__(self):
        self.remote = inject_portal.remote()
        self.active = False
        self.exits = 1 << LEFT  # edges of this screen that lead to another PC
        self.tell_edge = False  # the main PC names the edge each LEAVE went through (0.3 and later)
        self.resistance = 0  # the main PC's, from ENTER (0.5+); 0 from an older one: a touch
        self.press = [Press() for _ in range(4)]

    def start(self) -> None:
        self.remote.start()

    def release_all(self) -> None:
        self.remote.submit(lambda s: s.release_held(), release=True)

    def replay(self, msg: bytes):
        """Plays one message; returns a LEAVE to send back when the pointer goes home, else None."""
        kind = msg[0]
        if kind == link.ENTER and len(msg) in (5, 6, 7) and msg[1] <= BOTTOM:
            way_in = msg[1] ^ 1  # moving right, the pointer comes in at this PC's left
            # A main PC before 0.3 knows one PC only: the way in is the way back.
            self.exits = msg[5] & 15 if len(msg) >= 6 else 1 << way_in
            self.tell_edge = len(msg) >= 6
            self.resistance = msg[6] * 4 if len(msg) == 7 else 0
            if not msg[2]:
                x, y = on_edge(bounds(), way_in, msg[3] | msg[4] << 8)
                self.remote.submit(lambda s: s.place(x, y))
            for p in self.press:
                p.arrive()
            self.active = True
        elif kind == link.LEAVE and len(msg) == 4:
            self.release_all()
            self.active = False
        elif not self.active:
            return None
        elif kind == link.MOVE and len(msg) == 5:
            dx, dy = struct.unpack("<hh", msg[1:])
            b = bounds()
            x, y = desktop_portal.cursor_position()
            screens = desktop_portal.monitors()
            for e in range(4):
                if not self.exits & 1 << e:
                    continue
                wall = wall_at(screens, x, y, e) and not in_corner(screens, x, y, e)
                if self.press[e].feed(wall, outward(e, dx, dy), self.resistance) == CROSS:
                    # pushed through an edge to another PC: input goes there
                    frac = along(b, x, y, e)
                    self.release_all()
                    self.active = False
                    return bytes([link.LEAVE, 1]) + struct.pack("<H", frac) + (bytes([e]) if self.tell_edge else b"")
            self.remote.submit(lambda s: s.move_by(dx, dy))
        elif kind == link.BUTTON and len(msg) == 3 and msg[1] in BUTTONS:
            code, down = BUTTONS[msg[1]], bool(msg[2])
            self.remote.submit(lambda s: s.button(code, down), release=not down)
        elif kind == link.WHEEL and len(msg) == 4:
            (delta,) = struct.unpack("<h", msg[1:3])
            notches = delta / 120.0
            # Windows: up and right are positive. Beamer's scroll: y away from the hand, x to the left.
            dy, dx = (0.0, -notches) if msg[3] else (notches, 0.0)
            self.remote.submit(lambda s: s.scroll(dy, dx, "line"))
        elif kind == link.KEY and len(msg) == 4:
            code = keymap.to_evdev(msg[1], msg[2], bool(msg[3] & 2))
            if code is not None:
                down = not msg[3] & 1
                self.remote.submit(lambda s: s.keys([(code, down)]), release=not down)
        return None


class Clipboard:
    """Text, a picture (PNG) and copied files, through the same portal session."""

    def __init__(self):
        self._synced = None
        self._forced = None  # a test's own offer, to travel once as if another app had copied it

    def changed(self):
        """("clip", (text, png)) with either None, or ("files", [paths]), another app copied since the last
        call, else None."""
        forced, self._forced = self._forced, None
        if forced:
            return forced
        try:
            offered, ours, changes = clipboard_portal._offered()
        except Exception:
            return None
        if ours or changes == self._synced:
            return None
        self._synced = changes
        if "text/uri-list" in offered:
            data = clipboard_portal._read("text/uri-list")
            paths = [unquote(urlparse(u).path) for u in (data or b"").decode("utf-8", "replace").split()
                     if u.startswith("file://")]
            if paths:
                return "files", paths
        text, png = clipboard_portal._contents(offered)
        return ("clip", (text, png)) if text or png else None

    def get_png(self):
        """The picture on the clipboard now, as PNG (for tests)."""
        offered, ours, _changes = clipboard_portal._offered()
        if ours:
            return inject_portal.remote().ask(lambda s: dict(s.offer), default={}).get("image/png")
        return clipboard_portal._read("image/png") if "image/png" in offered else None

    def get(self):
        """What is on the clipboard now, whoever put it there (for tests)."""
        offered, ours, _changes = clipboard_portal._offered()
        if ours:  # our own offer: the portal lists only other apps' types
            offer = inject_portal.remote().ask(lambda s: dict(s.offer), default={})
            if "text/uri-list" in offer:
                return "files", [unquote(urlparse(u).path) for u in offer["text/uri-list"].decode().split()]
            text = offer.get("text/plain;charset=utf-8")
            return ("text", text.decode("utf-8")) if text else None
        if "text/uri-list" in offered:
            data = clipboard_portal._read("text/uri-list") or b""
            return "files", [unquote(urlparse(u).path) for u in data.decode().split() if u.startswith("file://")]
        text, _png = clipboard_portal._contents(offered)
        return ("text", text) if text else None

    def _offer(self, offer) -> bool:
        remote = inject_portal.remote()
        ok = bool(remote.ask(lambda s: s.set_selection(offer), default=False))
        if ok:
            self._synced = remote.ask(lambda s: s.changes, default=None)  # our own write never travels back
        return ok

    def set_text(self, text: str) -> bool:
        data = text.replace("\r\n", "\n").encode("utf-8")
        return self._offer({"text/plain;charset=utf-8": data, "text/plain": data, "UTF8_STRING": data})

    def set_clip(self, text, png) -> bool:
        """Text and a picture together; the app that pastes picks."""
        offer = {}
        if text is not None:
            data = text.replace("\r\n", "\n").encode("utf-8")
            offer.update({"text/plain;charset=utf-8": data, "text/plain": data, "UTF8_STRING": data})
        if png:
            offer["image/png"] = png
        return bool(offer) and self._offer(offer)

    def set_files(self, paths) -> bool:
        uris = ["file://" + quote(p) for p in paths]
        return self._offer({
            "text/uri-list": ("\r\n".join(uris) + "\r\n").encode(),
            "x-special/gnome-copied-files": ("copy\n" + "\n".join(uris)).encode(),
            "text/plain;charset=utf-8": "\n".join(paths).encode("utf-8"),
        })

    def copy_as_user(self, kind: str, value) -> None:
        """Tests: text goes through Klipper, so it arrives as another app's copy would; anything
        else is offered here and sent once anyway."""
        import shutil
        import subprocess
        if kind == "text":
            tool = shutil.which("qdbus6") or shutil.which("qdbus")
            if tool and subprocess.run([tool, "org.kde.klipper", "/klipper", "setClipboardContents", value],
                                       capture_output=True).returncode == 0:
                return
            self.set_text(value)
        else:
            self.set_files(value)
        self._forced = (kind, value)
