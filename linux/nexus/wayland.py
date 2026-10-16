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


def span(screens, b, edge):
    """The stretch (lo, hi, inclusive) of a desktop edge that screens touch. With a 4K and a 1080p screen
    side by side, 60% of the way down the 1080p one is 60% of that screen, not of the desktop's height."""
    left, top, right, bottom = b
    touches = {LEFT: lambda s: s.x == left, RIGHT: lambda s: s.x + s.width - 1 == right,
               TOP: lambda s: s.y == top, BOTTOM: lambda s: s.y + s.height - 1 == bottom}[edge]
    at = [s for s in screens if touches(s)]
    if edge in (LEFT, RIGHT):
        return (min(s.y for s in at), max(s.y + s.height - 1 for s in at)) if at else (top, bottom)
    return (min(s.x for s in at), max(s.x + s.width - 1 for s in at)) if at else (left, right)


def along(b, x, y, edge) -> int:
    """Position along the screens at an edge, 0..65535, as the Windows side counts it: 60% of the way
    down here arrives 60% of the way down there, whatever the two screens' sizes."""
    lo, hi = span(desktop_portal.monitors(), b, edge)
    return max(0, min(65535, ((y if edge in (LEFT, RIGHT) else x) - lo) * 65535 // max(1, hi - lo)))


def on_edge(b, edge, frac):
    """A point just inside an edge, at a position along the screens there."""
    left, top, right, bottom = b
    lo, hi = span(desktop_portal.monitors(), b, edge)
    at = lo + frac * (hi - lo) // 65535
    return {LEFT: (left + 2, at), RIGHT: (right - 2, at), TOP: (at, top + 2), BOTTOM: (at, bottom - 2)}[edge]


class Injector:
    """The controlled PC's side: what the main PC sends, played here."""

    def __init__(self):
        self.remote = inject_portal.remote()
        self.active = False
        self.exits = 1 << LEFT  # edges of this screen that lead to another PC
        self.tell_edge = False  # the main PC names the edge each LEAVE went through (0.3 and later)

    def start(self) -> None:
        self.remote.start()

    def release_all(self) -> None:
        self.remote.submit(lambda s: s.release_held(), release=True)

    def replay(self, msg: bytes):
        """Plays one message; returns a LEAVE to send back when the pointer goes home, else None."""
        kind = msg[0]
        if kind == link.ENTER and len(msg) in (5, 6) and msg[1] <= BOTTOM:
            way_in = msg[1] ^ 1  # moving right, the pointer comes in at this PC's left
            # A main PC before 0.3 knows one PC only: the way in is the way back.
            self.exits = msg[5] & 15 if len(msg) == 6 else 1 << way_in
            self.tell_edge = len(msg) == 6
            if not msg[2]:
                x, y = on_edge(bounds(), way_in, msg[3] | msg[4] << 8)
                self.remote.submit(lambda s: s.place(x, y))
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
            out = (x <= b[0] and dx < 0, x >= b[2] and dx > 0, y <= b[1] and dy < 0, y >= b[3] and dy > 0)
            for e in range(4):
                if out[e] and self.exits & 1 << e:  # through an edge to another PC: input goes there
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
    """Text and copied files, through the same portal session."""

    def __init__(self):
        self._synced = None
        self._forced = None  # a test's own offer, to travel once as if another app had copied it

    def changed(self):
        """("text", str) or ("files", [paths]) another app copied since the last call, else None."""
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
        text, _png = clipboard_portal._contents(offered)
        return ("text", text) if text else None

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
