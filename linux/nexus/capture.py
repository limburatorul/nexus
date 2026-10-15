"""The main PC's side on Wayland: the InputCapture portal (Beamer's capture_portal) holds this PC's
pointer and keys once the pointer meets the edge facing the controlled PC, and Nexus sends them
there. Crossing is immediate, as on Windows. While input is away, a double tap of Right Ctrl or
Ctrl+Alt+Pause brings it home.
"""

import struct
import time

import capture_portal
import desktop_portal
from core.return_edge import ReturnEdge

from . import keymap, link, wayland

WM = {0x201: (1, 1), 0x202: (1, 0), 0x204: (2, 1), 0x205: (2, 0), 0x207: (3, 1), 0x208: (3, 0)}
WM_XBUTTONDOWN, WM_XBUTTONUP, WM_MOUSEWHEEL, WM_MOUSEHWHEEL = 0x20B, 0x20C, 0x20A, 0x20E
R_CTRL, L_CTRL, L_ALT, R_ALT, PAUSE = 97, 29, 56, 100, 119
TAP_SECONDS = 0.4


class Capture:
    def __init__(self, controller):
        """controller: .cfg, .status, .remote (read and set here), .send(msg), .crossed (a Qt signal
        for the clipboard, which belongs to the Qt thread)."""
        self.c = controller
        self.hooks = capture_portal.Hooks(self.on_key, self.on_mouse, self.on_motion)
        self.hooks.grab_while(lambda: self.c.remote)
        self.hooks.barriers_from(self.models)
        self.hooks.on_home = lambda: self.home(None, tell=True)
        self._models = {}
        self._back = None  # where the pointer was when it crossed
        self._down = set()
        self._last_tap = 0.0
        self._tap_clean = False

    def start(self) -> None:
        self.hooks.start()

    def models(self):
        """The edge the controlled PC is on, while one is connected and input is here."""
        from .net import CONNECTED
        if self.c.status != CONNECTED or self.c.remote or self.c.cfg.role != "main":
            return []
        edge = self.c.cfg.edge
        if edge not in self._models:
            self._models[edge] = ReturnEdge(edge, resistance_px=0)
        return [self._models[edge]]

    def push(self) -> bool:
        """Tests: push the pointer through the edge with injected motion, as Beamer's shortcut does."""
        return self.hooks.push_through(self.models(), lambda: None, lambda: None)

    # ---- the capture thread's callbacks ----

    def _cross(self) -> None:
        b = wayland.bounds()
        x, y = desktop_portal.cursor_position()
        edge = ("left", "right", "top", "bottom").index(self.c.cfg.edge)
        self._back = (x, y)
        self.c.remote = True
        self.c.send(bytes([link.ENTER, edge, 0]) + struct.pack("<H", wayland.along(b, x, y, edge)))
        self.c.crossed.emit()

    def on_motion(self, dx: int, dy: int) -> None:
        if not self.c.remote:
            if self.hooks.holding:  # the desktop caught the pointer at our edge: over it goes
                self._cross()
            return
        dx, dy = max(-32768, min(32767, dx)), max(-32768, min(32767, dy))
        self.c.send(bytes([link.MOVE]) + struct.pack("<hh", dx, dy))

    def on_mouse(self, message: int, x: int, y: int, data: int) -> None:
        if not self.c.remote:
            return
        if message in WM:
            button, down = WM[message]
        elif message in (WM_XBUTTONDOWN, WM_XBUTTONUP):
            button, down = (4 if data >> 16 == 1 else 5), message == WM_XBUTTONDOWN
        elif message in (WM_MOUSEWHEEL, WM_MOUSEHWHEEL):
            delta = (data >> 16) & 0xFFFF
            self.c.send(bytes([link.WHEEL]) + struct.pack("<H", delta) + bytes([message == WM_MOUSEHWHEEL]))
            return
        else:
            return
        self.c.send(bytes([link.BUTTON, button, int(down)]))

    def on_key(self, name, down: bool, code: int, us) -> None:
        if not self.c.remote:
            return
        repeat = down and code in self._down
        (self._down.add if down else self._down.discard)(code)
        if down and not repeat:
            if code == R_CTRL:
                now = time.monotonic()
                if self._tap_clean and now - self._last_tap < TAP_SECONDS:
                    self._tap_clean = False
                    return self.home(None, tell=True)
                self._tap_clean, self._last_tap = True, now
            else:
                self._tap_clean = False
            if code == PAUSE and self._down & {L_CTRL, R_CTRL} and self._down & {L_ALT, R_ALT}:
                return self.home(None, tell=True)
        win = keymap.to_windows(code)
        if win:
            vk, scan, ext = win
            self.c.send(bytes([link.KEY, vk, scan, (0 if down else 1) | (2 if ext else 0)]))

    def home(self, frac, tell: bool) -> None:
        """Input comes back: at `frac` along our edge when the controlled PC handed it back, else
        where it left. `tell`: the controlled PC does not know yet."""
        if not self.c.remote:
            return
        if tell:
            self.c.send(bytes([link.LEAVE, 0, 0, 0]))
        edge = ("left", "right", "top", "bottom").index(self.c.cfg.edge)
        point = wayland.on_edge(wayland.bounds(), edge, frac) if frac is not None else self._back
        if point:
            desktop_portal.set_cursor_position(*point)  # where the capture lets go
        self._down.clear()
        self.c.remote = False
        self.c.changed.emit()
