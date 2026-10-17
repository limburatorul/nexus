"""Nexus for Linux: the controller (what src/nexus.c does on Windows) and its window, in Beamer's
Vernier look. Run with: python -m nexus [--test] [--tray]
"""

import logging
import logging.handlers
import os
import socket
import struct
import sys
import tempfile
import threading
import time

from PySide6.QtCore import QBuffer, QIODevice, QObject, QRect, QRectF, QSize, Qt, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import (QAction, QColor, QDesktopServices, QFont, QFontDatabase, QGuiApplication, QIcon, QImage,
                           QPainter, QPen, QPixmap)
from PySide6.QtWidgets import (QApplication, QFrame, QGridLayout, QHBoxLayout, QLabel, QLineEdit, QMenu, QPushButton,
                               QScrollArea, QSizePolicy, QSystemTrayIcon, QVBoxLayout, QWidget)

from . import VERSION, keymap, link, net, sound
from .config import EDGES, MAP_H, MAP_W, MAX_PEERS, THIS_PC, Config, config_dir
from .discovery import PORT as DISCOVERY_PORT, TEST_PORT as TEST_DISCOVERY_PORT, Discovery
from .files import Files

HERE = os.path.dirname(os.path.abspath(__file__))
RES = next((p for p in (os.path.join(HERE, "res"), os.path.join(HERE, "..", "..", "res")) if os.path.isdir(p)), HERE)
SITE = "https://github.com/limburatorul/nexus"

# Vernier palette, from Beamer's tokens.py (GPL-3), the same values as the Windows app.
DARK = dict(ground="#0e0f0d", panel="#161714", well="#1f201c", rule="#2a2b26", edge="#6e6f66", ink="#efeee6",
            ink2="#b3b2a7", ink3="#8f8e84", signal="#5fd4f4", amber="#f2b544", fault="#ff7a66", off="#4a4b44")
LIGHT = dict(ground="#f1f0ea", panel="#fbfaf6", well="#e5e4dc", rule="#dcdbd3", edge="#7f7e74", ink="#1b1c18",
             ink2="#46463f", ink3="#66655c", signal="#006a88", amber="#8f5c00", fault="#b53522", off="#aeada3")

STATUS_WORD = ["Not set up", "Waiting", "Connecting", "Not reachable", "Wrong password", "Version mismatch",
               "Same role", "Not Nexus", "Port in use", "Connected"]
STATUS_NOTE = [
    "Choose this PC's role and the shared password on the Connection page.",
    "Waiting for the main PC to connect.",
    "Trying to reach the controlled PC.",
    "The controlled PC does not answer. Check that Nexus runs there and that the address is right.",
    "The two PCs have different passwords. Type the same one on both.",
    "The other PC runs a different Nexus. Install the same version on both.",
    "Both PCs have the same role. Make one the main PC and the other the controlled PC.",
    "Something other than Nexus answered on port 24800.",
    "Another program uses port 24800, so the main PC cannot connect to this one.",
    ""]
PAGES = [
    ("Overview", "The PCs this one works with, where input is right now, and how the links are doing.", "Machines"),
    ("Crossing", "Where the PCs stand around this screen. Push the pointer through an edge to cross to the PC "
                 "beside it, and on from there to the next one.", "Map"),
    ("Keyboard", "Switch without the mouse, and a way home that always works.", "Shortcuts"),
    ("Sound", "The controlled PC's sound, played on this PC's speakers.", "Sound"),
    ("Files", "Copy files on one PC, switch, and paste them on another.", "Files"),
    ("Connection", "This PC's role, the password all the PCs share, and where to find the others.", "This PC"),
]


def fnv(path: str) -> str:
    h, size = 2166136261, 0
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                for b in chunk:
                    h = ((h ^ b) * 16777619) & 0xFFFFFFFF
                size += len(chunk)
    except OSError:
        return "-1 00000000"
    return "%d %08x" % (size, h)


LOG = logging.getLogger("nexus")


def dib_to_png(dib: bytes):
    """A Windows CF_DIB as PNG, through Qt's BMP reader (a file header in front); None when Qt cannot read it."""
    if len(dib) < 40:
        return None
    size, bits, compression, used = struct.unpack_from("<I10xHI12xI", dib)
    colours = used or (1 << bits if bits <= 8 else 0)
    offset = 14 + size + colours * 4 + (12 if compression == 3 and size == 40 else 0)
    image = QImage.fromData(b"BM" + struct.pack("<IHHI", 14 + len(dib), 0, 0, offset) + dib, "BMP")
    if image.isNull():
        return None
    buf = QBuffer()
    buf.open(QIODevice.WriteOnly)
    image.save(buf, "PNG")
    return bytes(buf.data())


def png_to_dib(png: bytes) -> bytes:
    """A PNG as a Windows CF_DIB: Qt writes a BMP, and the DIB is that without its 14-byte file header."""
    image = QImage.fromData(png, "PNG")
    if image.isNull():
        return b""
    buf = QBuffer()
    buf.open(QIODevice.WriteOnly)
    image.convertToFormat(QImage.Format_RGB32).save(buf, "BMP")
    return bytes(buf.data())[14:]


def wayland_locked() -> bool:
    from . import wayland
    return wayland.screen_locked()


VK_NAMES = {0xA2: "Left Ctrl", 0xA3: "Right Ctrl", 0xA4: "Left Alt", 0xA5: "Right Alt", 0xA0: "Left Shift",
            0xA1: "Right Shift", 0x5B: "Left Win", 0x5C: "Right Win", 0x13: "Pause", 0x14: "Caps Lock",
            0x91: "Scroll Lock", 0x2D: "Insert", 0x2E: "Delete", 0x24: "Home", 0x23: "End", 0x21: "Page Up",
            0x22: "Page Down", 0x5D: "Menu", 0x09: "Tab", 0xC0: "`"}


def key_name(chord: int) -> str:
    """"Ctrl + Alt + F1", "Right Ctrl": a recorded key (modifiers << 8 | Windows virtual key)."""
    vk, mods = chord & 0xFF, chord >> 8
    name = VK_NAMES.get(vk) or (chr(vk) if 0x30 <= vk <= 0x5A else "F%d" % (vk - 0x6F) if 0x70 <= vk <= 0x87
                                else "Key %02X" % vk)
    return "".join(m + " + " for bit, m in ((1, "Ctrl"), (2, "Alt"), (4, "Shift"), (8, "Win")) if mods & bit) + name


def at_least(version: str, want) -> bool:
    try:
        return tuple(int(v) for v in version.split(".")) >= want
    except ValueError:
        return False


class Controller(QObject):
    """Everything Nexus does, on the Qt thread; the network, sound and portal threads report here.

    A main PC links to up to four controlled PCs on its map and routes input between them: it goes to
    the PC past the edge it crossed, and on from there to that PC's neighbour (`target` has it now,
    `last` had it last). A controlled PC uses session 0 only."""

    changed = Signal()
    crossed = Signal(int, bool)  # input went to a PC, maybe from the capture thread: the clipboard follows
    notice = Signal(str, str)  # title, text: a tray notification
    jumped = Signal(int)  # a jump key named a PC (capture thread)
    _status = Signal(int, int)
    _message = Signal(int, bytes)
    _note = Signal(str)
    _files_done = Signal(list)
    _found = Signal()
    _audio = Signal(int)
    _command = Signal(object)

    def __init__(self, test: bool):
        super().__init__()
        self.test = test
        self.cfg = Config.load(test)
        self.port = 24810 if test else link.PORT
        self.name = socket.gethostname()
        self.remote, self.target, self.last, self.audio_state, self.files_note = False, -1, -1, 0, ""
        self.paused = False  # crossing paused from the tray or the Crossing page (this session only)
        self.clip_in = link.ClipIn()
        self._locked = 0  # lock checks in a row that found this controlled PC locked
        self.state = [net.SETUP] * MAX_PEERS  # each session's status, as this thread last heard it
        self.sessions, self.files = [], []
        for i in range(MAX_PEERS):
            f = Files(None, self._note.emit, self._files_done.emit)
            s = net.Session(self.cfg, self.name, VERSION, self.port, lambda st, i=i: self._status.emit(i, st),
                            lambda m, i=i: self._message.emit(i, m), files=f, index=i)
            f.send = s.send
            self.sessions.append(s)
            self.files.append(f)
        self.sound_capture = sound.Capture(self._send, self._audio.emit)
        self.capture = None  # the main PC's input capture, made when it is the main PC
        self.player = sound.Player()
        self.discovery = Discovery(TEST_DISCOVERY_PORT if test else DISCOVERY_PORT, self.name, VERSION,
                                   lambda: 0 if self.cfg.role == "main" else 1, self._found.emit)
        from . import wayland  # the portal session asks the desktop once; nothing here before the window
        self.injector, self.clipboard = wayland.Injector(), wayland.Clipboard()
        self._status.connect(self.on_status)
        self._message.connect(self.on_message)
        self._note.connect(self.on_note)
        self._files_done.connect(self.clipboard.set_files)
        self._found.connect(self.changed)
        self._audio.connect(self.on_audio)
        self._command.connect(self._run_command, Qt.BlockingQueuedConnection)
        self.crossed.connect(self.send_clipboard)
        self.jumped.connect(self.jump)
        self.send = self._send
        self.timer = QTimer(self, interval=1000, timeout=self.tick)
        self.beat_timer = QTimer(self, interval=250, timeout=self.beat)

    def start(self) -> None:
        self.injector.start()  # also the clipboard's way in, on either role
        self.start_capture()
        for s in self.sessions:
            s.start()
        self.discovery.start()
        self.timer.start()
        self.beat_timer.start()
        if self.test:
            from .control import Control
            Control(lambda: self.cfg.key, self.name, VERSION, self.command).start()

    def start_capture(self) -> None:
        """On a main PC: the desktop asks once for leave to capture input at the edge."""
        if self.cfg.role == "main" and self.capture is None:
            from .capture import Capture
            self.capture = Capture(self)
            self.capture.start()

    def _send(self, msg: bytes) -> bool:
        """To the PC with input (a controlled PC: to the main PC)."""
        pc = 0 if self.cfg.role == "controlled" else self.target
        return pc >= 0 and self.sessions[pc].send(msg)

    # ---- the PCs ----

    def up(self, pc: int) -> bool:
        return pc == THIS_PC or 0 <= pc < MAX_PEERS and self.state[pc] == net.CONNECTED

    def exits(self, pc: int) -> int:
        """The edges of a PC (THIS_PC for this one) that lead to a PC that is there."""
        return sum(1 << e for e in range(4) if self.up(self.cfg.neighbour(pc, e)))

    def first_connected(self) -> int:
        return next((i for i in range(MAX_PEERS) if self.cfg.peers[i] and self.up(i)), -1)

    def sound_from(self) -> int:
        """The PC whose sound plays here: the one input went to last, else the first one there."""
        return self.last if self.last >= 0 and self.up(self.last) else self.first_connected()

    def shown(self) -> int:
        """The PC the sidebar talks about: where input is, else the one used last, else the first one there."""
        if self.cfg.role == "controlled":
            return 0
        return self.target if self.target >= 0 else self.sound_from()

    @property
    def status(self) -> int:
        """This PC's state as a whole: connected while any controlled PC is, else the first one's."""
        if self.cfg.role == "controlled":
            return self.state[0]
        if not self.cfg.ready:
            return net.SETUP
        if self.first_connected() >= 0:
            return net.CONNECTED
        return next((self.state[i] for i in range(MAX_PEERS) if self.cfg.peers[i]), net.SETUP)

    def name_of(self, pc: int) -> str:
        if not 0 <= pc < MAX_PEERS:
            return "the controlled PC" if self.cfg.role == "main" else "the main PC"
        return self.sessions[pc].peer or self.cfg.peers[pc] or "the main PC"

    def edge_toward(self, pc: int) -> int:
        """The edge of this PC that faces pc, when it stands beside it; else -1."""
        return next((e for e in range(4) if self.cfg.neighbour(THIS_PC, e) == pc), -1)

    # ---- what the threads report ----

    @Slot(int, int)
    def on_status(self, pc: int, status: int) -> None:
        s = self.sessions[pc]
        if status != net.CONNECTED:
            sounding = pc == self.sound_from()
            if self.cfg.role == "controlled":
                self.injector.release_all()
                self.injector.active = False
                self.sound_capture.stop()
            elif pc == self.target:  # input was over there: it comes home
                if self.capture:
                    self.capture.home(None, None)
                self.target, self.remote = -1, False
            if sounding:
                self.player.stop()
            s.rtt, s.audio_state = None, 0
        elif self.cfg.role == "controlled":
            self.sound_capture.start()
        elif s.mac and s.mac != self.cfg.macs[pc]:  # learnt for Wake-on-LAN
            self.cfg.macs[pc] = s.mac
            self.cfg.save()
        if status != self.state[pc]:
            LOG.info("link %d: %s%s", pc, STATUS_WORD[status],
                     (", forward secret" if s.secret else ", password key only") if status == net.CONNECTED else "")
        self.state[pc] = status
        self.changed.emit()

    @Slot(int, bytes)
    def on_message(self, pc: int, msg: bytes) -> None:
        kind = msg[0]
        if kind == link.CLIPBOARD and len(msg) > 1:
            self.clipboard.set_text(msg[1:].decode("utf-8", "replace"))
        elif link.CLIP_BEGIN <= kind <= link.CLIP_END:
            done = self.clip_in.feed(msg)
            if done:
                text = done.get(link.CLIP_TEXT)
                png = dib_to_png(done[link.CLIP_DIB]) if link.CLIP_DIB in done else None
                self.clipboard.set_clip(text.decode("utf-8", "replace") if text else None, png)
                LOG.info("clipboard: received %d bytes of text and a %d-byte picture", len(text or b""), len(png or b""))
        elif kind == link.LOCKED and len(msg) == 1 and self.cfg.role == "main" and pc == self.target:
            if self.capture:
                self.capture.home(None, None)
            self.target, self.remote = -1, False
            self.notice.emit("Input came home", "%s is locked, so input is back here. Unlock it at that PC."
                             % self.name_of(pc))
            self.changed.emit()
        elif kind == link.PONG and len(msg) == 5:
            self.sessions[pc].rtt = ((int(time.monotonic() * 1000) & 0xFFFFFFFF) -
                                     struct.unpack("<I", msg[1:])[0]) & 0xFFFFFFFF
            self.changed.emit()
        elif self.cfg.role == "main":
            if kind == link.LEAVE and len(msg) in (4, 5) and pc == self.target:
                self.cross_on(pc, msg[4] if len(msg) == 5 and msg[4] <= 3 else None,
                              msg[2] | msg[3] << 8 if msg[1] else None)
            elif kind == link.AUDIO and pc == self.sound_from():
                self.player.play(msg)
            elif kind == link.AUDIO_STATE and len(msg) == 2:
                self.sessions[pc].audio_state = msg[1]
                self.changed.emit()
        elif pc == 0:
            was = self.injector.active
            out = self.injector.replay(msg)
            if was and not self.injector.active:
                self.send_clipboard(0)  # ahead of LEAVE, so it is there on arrival
            if out:
                self._send(out)
            if was != self.injector.active:
                self.changed.emit()

    @Slot(str)
    def on_note(self, text: str) -> None:
        self.files_note = text
        self.changed.emit()

    @Slot(int)
    def on_audio(self, state: int) -> None:
        self.audio_state = state
        self.changed.emit()

    def tick(self) -> None:
        if self.cfg.role == "main" and int(time.time()) % 3 == 0:
            self.discovery.probe()
        if self.discovery.expire():
            self.changed.emit()

    def beat(self) -> None:
        """Every 250 ms. While input is across, a link silent for 2 s is gone and input comes home at once
        (port of Beamer's ACK heartbeat, core/receiver.py); a controlled PC with input pings too, and
        hands input back when its screen is locked."""
        now = time.monotonic()
        for i, s in enumerate(self.sessions):
            s.beat = i == (self.target if self.cfg.role == "main" else 0 if self.injector.active else -1)
        if self.cfg.role == "main" and self.target >= 0:
            pc, s = self.target, self.sessions[self.target]
            if now - s.heard > link.DEAD_SECONDS:
                LOG.info("link %d: silent for %.1f s while it had input; input comes home", pc, now - s.heard)
                if self.capture:
                    self.capture.home(None, None)
                self.target, self.remote = -1, False
                s.drop()
                self.notice.emit("Input came home", "%s stopped answering, so input is back on this PC."
                                 % self.name_of(pc))
                self.changed.emit()
        elif self.cfg.role == "controlled" and self.injector.active and self.state[0] == net.CONNECTED:
            s, stop = self.sessions[0], False
            if now - s.heard > link.DEAD_SECONDS:
                stop = True
            self._locked = self._locked + 1 if wayland_locked() else 0
            if self._locked >= 2:  # twice in a row, not a flicker
                LOG.info("this PC's screen is locked: input goes back to the main PC")
                s.send(bytes([link.LOCKED]) if link.v5(s.peer_version) else bytes([link.LEAVE, 0, 0, 0]))
                stop, self._locked = True, 0
            if stop:
                self.injector.release_all()
                self.injector.active = False
                self.changed.emit()

    @Slot(int)
    def jump(self, pc: int) -> None:
        """A jump key: input goes straight to pc, or it is woken when it is asleep."""
        if self.up(pc):
            if pc != self.target:
                self.cross(pc)
        else:
            self.wake(pc)

    def wake(self, pc: int) -> None:
        """Wake-on-LAN: the magic packet to the broadcast address. The MAC comes from the kernel's
        neighbour table after a connection (net.arp_mac). Port of Beamer core/wol.py."""
        mac = self.cfg.macs[pc] or self.sessions[pc].mac
        if not mac:
            self.notice.emit("Nexus", "%s is not connected." % self.name_of(pc))
            return
        packet = b"\xff" * 6 + bytes.fromhex(mac.replace("-", "")) * 16
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            try:
                s.sendto(packet, ("255.255.255.255", 9))
            except OSError:
                pass
        self.notice.emit("Wake-on-LAN", "Waking %s. It connects by itself once it is up." % self.name_of(pc))

    def set_paused(self, paused: bool) -> None:
        self.paused = paused
        LOG.info("crossing %s", "paused" if paused else "resumed")
        self.changed.emit()

    # ---- crossing between the PCs on the map (main PC) ----

    def enter_peer(self, pc: int, edge: int, keep: bool, frac: int, relay: bool = False) -> None:
        """Input goes to pc, moving in direction edge, at frac along the edge it comes in by (keep: the
        pointer stays where it is over there). From any thread; the clipboard follows on the Qt thread,
        and with `relay` what came from the PC before goes on too."""
        s = self.sessions[pc]
        msg = bytes([link.ENTER, edge, int(keep)]) + struct.pack("<H", frac) + bytes([self.exits(pc),
                                                                                       self.cfg.resistance // 4])
        # before 0.3: one way back only; before 0.5: no resistance
        s.send(msg if link.v5(s.peer_version) else msg[:6] if at_least(s.peer_version, (0, 3, 0)) else msg[:5])
        LOG.info("input to link %d", pc)
        self.target = self.last = pc
        self.crossed.emit(pc, relay)
        self.changed.emit()

    def cross_from_here(self, edge: int, frac: int) -> bool:
        """The capture thread: the pointer went through this PC's edge."""
        to = self.cfg.neighbour(THIS_PC, edge)
        if to == THIS_PC or not self.up(to):  # it went away meanwhile
            return False
        self.enter_peer(to, edge, False, frac)
        return True

    def cross_on(self, frm: int, edge, frac) -> None:
        """The PC with input let it go through an edge (None: the way it came, from a Nexus before 0.3):
        on to the PC past that edge, or home."""
        if edge is None and self.edge_toward(frm) >= 0:
            edge = self.edge_toward(frm) ^ 1
        nxt = THIS_PC if edge is None else self.cfg.neighbour(frm, edge)
        if nxt != THIS_PC and self.up(nxt):
            self.enter_peer(nxt, edge, False, 32767 if frac is None else frac, relay=True)
            return
        if self.capture:
            self.capture.home(edge ^ 1 if nxt == THIS_PC and edge is not None else None, frac)
        self.target, self.remote = -1, False
        self.changed.emit()

    @Slot(int, bool)
    def send_clipboard(self, pc: int, relay: bool = False) -> None:
        got = self.clipboard.get() if relay else self.clipboard.changed()
        if not got:
            return
        kind, value = got
        if kind == "text":  # get(), for a relay: text only
            kind, value = "clip", (value, None)
        s = self.sessions[pc]
        if kind == "files":
            if not self.files[pc].send_paths(value, self.name_of(pc)):
                self.on_note("Still sending the last files; copy the new ones again once they are across.")
            return
        text, png = value
        data = text.encode("utf-8") if text else b""
        if not link.v5(s.peer_version):  # an older peer: text up to 64 KB, as before
            if data and len(data) < link.MAX:
                s.send(bytes([link.CLIPBOARD]) + data)
            return
        # Text and picture together; over a limit that part stays behind, never cut short.
        dib = png_to_dib(png) if png else b""
        sent = False
        for fmt, part in ((link.CLIP_TEXT, data), (link.CLIP_DIB, dib)):
            if part and len(part) <= link.CLIP_MAX[fmt]:
                for msg in link.clip_out(fmt, part):
                    s.send(msg)
                sent = True
            elif part:
                LOG.info("clipboard: %d bytes of format %d stay here (over the limit)", len(part), fmt)
        if sent:
            s.send(bytes([link.CLIP_END]))

    def cross(self, pc: int = -1) -> bool:
        """Main PC: input goes to pc, else the PC used last (the shortcut and tests end here)."""
        pc = pc if pc >= 0 else self.sound_from()
        if self.cfg.role != "main" or not 0 <= pc < MAX_PEERS or not self.up(pc):
            return False
        if self.target >= 0:
            self.home()
        self.send_clipboard(pc)
        edge = self.edge_toward(pc)
        self.enter_peer(pc, edge if edge >= 0 else 1, True, 0)
        self.remote = True
        self.changed.emit()
        return True

    def home(self) -> None:
        if self.cfg.role == "main" and self.remote:
            self._send(bytes([link.LEAVE, 0, 0, 0]))
            self.target, self.remote = -1, False
            self.changed.emit()

    def save_setting(self, **values) -> None:
        """A setting that needs no new connection (resistance, keys)."""
        for k, v in values.items():
            setattr(self.cfg, k, v)
        self.cfg.save()
        self.changed.emit()

    def set_config(self, **values) -> None:
        for k, v in values.items():
            setattr(self.cfg, k, v)
        self.cfg.save()
        for s in self.sessions:
            s.restart()
        self.start_capture()
        self.changed.emit()

    def set_address(self, pc: int, addr: str) -> None:
        """A controlled PC's address; empty removes it. A new one gets a free cell beside this PC."""
        if addr == self.cfg.peers[pc]:
            return
        added = not self.cfg.peers[pc]
        self.cfg.peers[pc] = addr
        if added:
            self.cfg.free_cell(pc)
        self.cfg.save()
        self.sessions[pc].peer = ""
        self.sessions[pc].restart()
        self.changed.emit()

    def move_peer(self, pc: int, x: int, y: int) -> None:
        if self.cfg.move(pc, x, y):
            self.cfg.save()
        self.changed.emit()

    # ---- test mode ----

    def command(self, cmd: str) -> str:
        """From the control thread: runs on the Qt thread, as a click would."""
        holder = [cmd, "no answer"]
        self._command.emit(holder)
        return holder[1]

    @Slot(object)
    def _run_command(self, holder) -> None:
        try:
            holder[1] = self._answer(holder[0])
        except Exception as e:  # a test must see the failure, not hang
            holder[1] = "error: %r" % e

    def _answer(self, cmd: str) -> str:
        verb, _, arg = cmd.partition(" ")
        if verb == "status":  # about the PC with input, else the one used last or the first one
            pc = max(self.shown(), 0)
            s = self.sessions[pc]
            return ("role=%s status=%d peer=%s peer_version=%s remote=%d active=%d audio=%d version=%s secret=%d "
                    "files=%s") % (
                self.cfg.role, self.status, s.peer, s.peer_version, self.remote, self.injector.active,
                s.audio_state if self.cfg.role == "main" else self.audio_state, VERSION, s.secret, self.files_note)
        if verb == "clip-image":  # "clip-image W H": the same test picture as on Windows, as if copied
            w, h = (int(v) for v in arg.split())
            image = QImage(w, h, QImage.Format_RGB32)
            for y in range(h):
                for x in range(w):
                    image.setPixel(x, y, 0xFF000000 | ((x ^ y) & 0xFF) << 16 | (y * 5 & 0xFF) << 8 | ((x * 7 + y) & 0xFF))
            buf = QBuffer()
            buf.open(QIODevice.WriteOnly)
            image.save(buf, "PNG")
            png = bytes(buf.data())
            self.clipboard.set_clip(None, png)
            self.clipboard._forced = ("clip", (None, png))
            return "ok"
        if verb == "clip-image-get":  # "W H fnv" of the picture's pixels, top row first, as RGB
            png = self.clipboard.get_png()
            image = QImage.fromData(png, "PNG") if png else QImage()
            if image.isNull():
                return "none"
            h = 2166136261
            for y in range(image.height()):
                for x in range(image.width()):
                    p = image.pixel(x, y)
                    for v in (p >> 16 & 0xFF, p >> 8 & 0xFF, p & 0xFF):
                        h = ((h ^ v) * 16777619) & 0xFFFFFFFF
            return "%d %d %08x" % (image.width(), image.height(), h)
        if verb == "clip-big":  # "clip-big N": N characters, too many for one message
            text = "".join("\n" if i % 61 == 60 else chr(97 + i % 26) for i in range(int(arg)))
            self.clipboard.set_text(text)
            self.clipboard._forced = ("text", text)
            return "ok"
        if verb == "clip-hash":  # "length fnv" of the clipboard's text as UTF-8
            got = self.clipboard.get()
            if not got or got[0] != "text":
                return "none"
            data, h = got[1].encode("utf-8"), 2166136261
            for v in data:
                h = ((h ^ v) * 16777619) & 0xFFFFFFFF
            return "%d %08x" % (len(data), h)
        if verb == "peers":  # pc x,y status name, one per line
            return "".join("%d %d,%d %d %s%s\n" % (i, *self.cfg.places[i], self.state[i], self.name_of(i),
                                                   " *" if i == self.target else "")
                           for i in range(MAX_PEERS) if self.cfg.peers[i])
        if verb == "cross":
            if not self.cross(int(arg) if arg else -1):
                return "not connected"
        elif verb == "home":
            self.home()
        elif verb == "move":
            dx, dy = (int(v) for v in arg.split())
            self._send(bytes([link.MOVE]) + struct.pack("<hh", dx, dy))
        elif verb == "cursor":
            import desktop_portal
            return "%d %d" % desktop_portal.cursor_position()
        elif verb == "clip-set":
            self.clipboard.copy_as_user("text", arg)
        elif verb == "files":
            self.clipboard.copy_as_user("files", arg.split("|"))
        elif verb == "clip-get":
            got = self.clipboard.get()
            if not got:
                return "empty"
            return "files:" + "|".join(got[1]) if got[0] == "files" else "text:" + got[1]
        elif verb == "write":
            path, _, text = arg.partition("|")
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(text)
        elif verb == "fnv":
            return fnv(arg)
        elif verb == "temp":
            return tempfile.gettempdir() + "/"
        elif verb == "found":
            return "|".join("%s %s %s" % (ip, "main" if r == 0 else "controlled", n)
                            for ip, (n, r, _s) in self.discovery.found.items())
        elif verb == "push":
            return "pushed" if self.capture and self.capture.push() else "no capture to push through"
        elif verb == "portal":
            r = self.injector.remote
            return "ready=%s clipboard=%s problem=%s offered=%s ours=%s changes=%s" % (
                r.ready.is_set(), r.clipboard_enabled, r.problem, r.offered, r.ours, r.changes)
        elif verb == "quit":
            QTimer.singleShot(0, QApplication.quit)
        else:
            return "unknown or not now"
        return "ok"


# ---- the window ----

def page_icon(page: int, color: str, size: int = 24) -> QIcon:
    """The Windows app's line icons, drawn the same way in a 24-unit box, round-capped."""
    pix = QPixmap(size * 2, size * 2)
    pix.setDevicePixelRatio(2)
    pix.fill(Qt.transparent)
    p = QPainter(pix)
    p.setRenderHint(QPainter.Antialiasing)
    p.setPen(QPen(QColor(color), 1.75, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
    line = lambda *pts: p.drawPolyline([QPointF(*q) for q in pts])  # noqa: E731
    if page == 0:
        p.drawRoundedRect(QRectF(2, 4, 14, 11), 2, 2), line((9, 15), (9, 19)), line((6, 19), (12, 19))
        p.drawRoundedRect(QRectF(14, 10, 8, 10), 2, 2)
    elif page == 1:
        line((3, 8), (21, 8)), line((17, 4), (21, 8), (17, 12))
        line((21, 16), (3, 16)), line((7, 12), (3, 16), (7, 20))
    elif page == 2:
        p.drawRoundedRect(QRectF(2, 5, 20, 14), 2.5, 2.5)
        p.setBrush(QColor(color))
        for i in range(5):
            p.drawEllipse(QRectF(5 + 3.5 * i, 8.5, 2, 2))
        line((8, 15), (16, 15))
    elif page == 3:
        line((3, 9), (7, 9), (12, 4), (12, 20), (7, 15), (3, 15), (3, 9))
        p.drawArc(QRectF(11, 8, 8, 8), -60 * 16, 120 * 16)
        p.drawArc(QRectF(10, 4, 14, 16), -65 * 16, 130 * 16)
    elif page == 4:
        line((3, 6), (10, 6), (12, 9), (22, 9), (22, 18), (2, 18), (2, 7), (3, 6))
    else:
        p.drawEllipse(QRectF(2, 6, 12, 12)), p.drawEllipse(QRectF(10, 6, 12, 12))
    p.end()
    return QIcon(pix)


class Light(QWidget):
    """A status light: the dot, and a soft halo around it while it is lit (connected)."""

    def __init__(self, size: int):
        super().__init__()
        self.size, self.color, self.lit = size, QColor("gray"), False
        self.setFixedSize(size * 2, size * 2)

    def set(self, color: str, lit: bool) -> None:
        self.color, self.lit = QColor(color), lit
        self.update()

    def paintEvent(self, _e) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(Qt.NoPen)
        if self.lit:
            halo = QColor(self.color)
            halo.setAlpha(48)
            p.setBrush(halo)
            p.drawEllipse(QRectF(0, 0, 2 * self.size, 2 * self.size))
        p.setBrush(self.color)
        p.drawEllipse(QRectF(self.size / 2, self.size / 2, self.size, self.size))
        p.end()


from PySide6.QtCore import QPointF  # noqa: E402  (used by page_icon)


class Tile(QPushButton):
    """A controlled PC on the map: dragged to another cell, or moved with the arrow keys."""

    ARROWS = {Qt.Key_Left: (-1, 0), Qt.Key_Right: (1, 0), Qt.Key_Up: (0, -1), Qt.Key_Down: (0, 1)}

    def __init__(self, view, pc: int):
        super().__init__(view.c.name_of(pc), view)
        self.view, self.pc, self._grab = view, pc, None
        x, y = view.c.cfg.places[pc]
        self.setAccessibleName("%s, on the map at %d, %d from this PC" % (view.c.name_of(pc), x, y))
        self.setAccessibleDescription("Arrow keys move it to the next cell.")
        self.setGeometry(view.cell_rect(x, y))

    def _move_to(self, x: int, y: int) -> None:
        self.setGeometry(self.view.cell_rect(*self.view.c.cfg.places[self.pc]))
        # Later: the move rebuilds the page, and with it this button.
        QTimer.singleShot(0, lambda: self.view.c.move_peer(self.pc, x, y))

    def mousePressEvent(self, e) -> None:
        if e.button() == Qt.LeftButton:
            self.setFocus()
            self.raise_()
            self._grab = e.position().toPoint()

    def mouseMoveEvent(self, e) -> None:
        if self._grab is not None:
            self.move(self.mapToParent(e.position().toPoint()) - self._grab)

    def mouseReleaseEvent(self, e) -> None:
        if self._grab is None or e.button() != Qt.LeftButton:
            return
        self._grab = None
        mid, w, h = self.geometry().center(), self.view.cell_w, self.view.cell_h
        if self.view.rect().contains(mid):  # dropped off the map: back to its cell
            self._move_to(mid.x() // w - MAP_W // 2, mid.y() // h - MAP_H // 2)
        else:
            self._move_to(*self.view.c.cfg.places[self.pc])

    def paintEvent(self, _e) -> None:
        pal, c = self.view.win.p, self.view.c
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        r = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        hover = self.underMouse() or self._grab is not None
        p.setPen(QPen(QColor(pal["signal"] if self.hasFocus() else pal["ink2"] if hover else pal["edge"]),
                      2 if self.hasFocus() else 1))
        p.setBrush(QColor(pal["rule"] if hover else pal["well"]))
        p.drawRoundedRect(r, 10, 10)
        state = c.state[self.pc]
        color = QColor(self.view.win._status_color(state))
        if state == net.CONNECTED:
            halo = QColor(color)
            halo.setAlpha(48)
            p.setPen(Qt.NoPen), p.setBrush(halo), p.drawEllipse(QRectF(9, 12, 16, 16))
        p.setPen(Qt.NoPen), p.setBrush(color), p.drawEllipse(QRectF(13, 16, 8, 8))
        f = p.font()
        f.setWeight(QFont.DemiBold)
        p.setFont(f)
        p.setPen(QColor(pal["ink"]))
        name = p.fontMetrics().elidedText(c.name_of(self.pc), Qt.ElideRight, self.width() - 36)
        p.drawText(QRectF(28, 8, self.width() - 32, 22), Qt.AlignLeft | Qt.AlignVCenter, name)
        f.setWeight(QFont.Normal)
        f.setPixelSize(13)
        p.setFont(f)
        p.setPen(QColor(pal["ink3"]))
        known = c.sessions[self.pc].peer  # with no name yet, the address is the name and the state goes here
        sub = c.cfg.peers[self.pc] if known else STATUS_WORD[state]
        p.drawText(QRectF(12, 30, self.width() - 20, 20), Qt.AlignLeft | Qt.AlignVCenter,
                   p.fontMetrics().elidedText(sub, Qt.ElideRight, self.width() - 20))
        p.end()

    def keyPressEvent(self, e) -> None:
        step = self.ARROWS.get(e.key())
        if not step:
            return super().keyPressEvent(e)
        x, y = self.view.c.cfg.places[self.pc]
        self.view.win.focus_tile = self.pc
        self._move_to(x + step[0], y + step[1])


class MapView(QWidget):
    """The Crossing page's map: this PC in the middle cell of a 5 x 3 grid, the others where they were put."""

    def __init__(self, win):
        super().__init__()
        self.win, self.c = win, win.c
        self.cell_w = max(70, min(150, (win.content.width() - 144) // MAP_W))
        self.cell_h = self.cell_w * 3 // 5
        self.setFixedSize(MAP_W * self.cell_w, MAP_H * self.cell_h)
        for i in range(MAX_PEERS):
            if self.c.cfg.peers[i]:
                t = Tile(self, i)
                if win.focus_tile == i:
                    QTimer.singleShot(0, t.setFocus)
        win.focus_tile = None

    def cell_rect(self, x: int, y: int) -> QRect:
        return QRect((x + MAP_W // 2) * self.cell_w + 4, (y + MAP_H // 2) * self.cell_h + 4,
                     self.cell_w - 8, self.cell_h - 8)

    def paintEvent(self, _e) -> None:
        p, pal = QPainter(self), self.win.p
        p.setRenderHint(QPainter.Antialiasing)
        for cy in range(MAP_H):
            for cx in range(MAP_W):
                x, y = cx - MAP_W // 2, cy - MAP_H // 2
                r = QRectF(self.cell_rect(x, y))
                if (x, y) == (0, 0):
                    p.setPen(QPen(QColor(pal["signal"]), 2))
                    p.setBrush(QColor(pal["well"]))
                    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 10, 10)
                    f = p.font()
                    f.setWeight(QFont.DemiBold)
                    p.setFont(f)
                    p.setPen(QColor(pal["ink"]))
                    p.drawText(r.adjusted(12, 8, -8, 0), Qt.AlignLeft | Qt.AlignTop, "This PC")
                    f.setWeight(QFont.Normal)
                    f.setPixelSize(13)
                    p.setFont(f)
                    p.setPen(QColor(pal["ink3"]))
                    p.drawText(r.adjusted(12, 30, -8, 0), Qt.AlignLeft | Qt.AlignTop, socket.gethostname())
                else:
                    p.setPen(QPen(QColor(pal["rule"]), 1))
                    p.setBrush(Qt.NoBrush)
                    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10)
        p.end()


class Window(QWidget):
    def __init__(self, c: Controller):
        super().__init__()
        self.c = c
        self.page = 0
        self.focus_tile = None  # the map tile the arrow keys moved, focused again once rebuilt
        self.recording = 0  # the Keyboard page waits for a key: 1 the switch key, 2 + n PC n's jump key
        self.setObjectName("root")
        self.setWindowTitle("Nexus (test)" if c.test else "Nexus")
        self.setWindowIcon(QIcon(os.path.join(RES, "nexus.ico")))
        self.setMinimumSize(760, 640)
        self.resize(1000, 800)
        light = QGuiApplication.styleHints().colorScheme() == Qt.ColorScheme.Light
        self.p = LIGHT if light else DARK
        self._build()
        c.changed.connect(self.refresh)
        self.refresh()

    def _style(self) -> str:
        p = self.p
        return f"""
        QWidget {{ color: {p['ink']}; font-family: 'Hanken Grotesk'; font-size: 14px; }}
        #root, #content {{ background: {p['ground']}; }}
        #sidebar {{ border-right: 1px solid {p['rule']}; }}
        #status {{ border-bottom: 1px solid {p['rule']}; }}
        #footer {{ border-top: 1px solid {p['rule']}; }}
        QLabel {{ background: transparent; }}
        QLabel#title {{ font-size: 28px; font-weight: 700; }}
        QLabel#desc, QLabel#body {{ color: {p['ink2']}; }}
        QLabel#small {{ color: {p['ink3']}; font-size: 13px; }}
        QLabel#eyebrow {{ color: {p['ink']}; font-size: 16px; font-weight: 600; }}
        QLabel#line {{ font-weight: 600; font-size: 15px; }}
        QLabel#keycap {{ background: {p['well']}; border: 1px solid {p['edge']}; border-radius: 8px;
                         font-family: 'B612 Mono'; font-size: 13px; padding: 10px 18px; }}
        #module {{ background: {p['panel']}; border: 1px solid {p['rule']}; border-radius: 12px; }}
        QPushButton {{ background: transparent; border: 1px solid {p['edge']}; border-radius: 8px; padding: 10px 18px;
                       font-weight: 600; color: {p['ink']}; }}
        QPushButton:hover {{ background: {p['well']}; border-color: {p['ink2']}; }}
        QPushButton:checked {{ background: {p['ink']}; color: {p['ground']}; border-color: {p['ink']}; }}
        QPushButton:focus {{ outline: none; border: 2px solid {p['signal']}; }}
        QPushButton#nav {{ border: none; text-align: left; padding: 14px 16px; font-weight: 400; font-size: 15px;
                           color: {p['ink2']}; border-left: 3px solid transparent; border-radius: 8px; }}
        QPushButton#nav:hover {{ background: {p['panel']}; color: {p['ink']}; }}
        QPushButton#nav:checked {{ background: {p['well']}; color: {p['ink']}; font-weight: 600;
                                   border-left: 3px solid {p['signal']}; }}
        QPushButton#link {{ border: none; text-align: left; padding: 4px 0; font-weight: 400; color: {p['ink3']};
                            font-size: 13px; }}
        QPushButton#link:hover {{ color: {p['ink']}; background: transparent; }}
        QLineEdit {{ background: {p['well']}; border: 1px solid {p['edge']}; border-radius: 8px; padding: 10px 12px; }}
        QLineEdit:focus {{ border-color: {p['signal']}; }}
        QScrollArea {{ border: none; }}
        """

    def _label(self, text="", name="body", wrap=True) -> QLabel:
        lab = QLabel(text)
        lab.setObjectName(name)
        lab.setWordWrap(wrap)
        lab.setTextInteractionFlags(Qt.TextSelectableByMouse)
        return lab

    def _build(self) -> None:
        self.setStyleSheet(self._style())
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)
        top = QHBoxLayout()
        top.setSpacing(0)
        outer.addLayout(top, 1)

        side = QFrame(objectName="sidebar")
        side.setFixedWidth(280)
        sl = QVBoxLayout(side)
        sl.setContentsMargins(0, 0, 0, 0)
        status = QFrame(objectName="status")
        stl = QVBoxLayout(status)
        stl.setContentsMargins(28, 28, 16, 18)
        row = QHBoxLayout()
        self.dot = Light(12)
        self.line = self._label(name="line", wrap=False)
        row.addWidget(self.dot)
        row.addSpacing(6)
        row.addWidget(self.line, 1)
        stl.addLayout(row)
        self.who = self._label(wrap=False)
        self.who.setContentsMargins(24, 0, 0, 0)
        stl.addWidget(self.who)
        stl.addSpacing(10)
        self.response = self._label(wrap=False)
        stl.addWidget(self.response)
        sl.addWidget(status)
        nav = QVBoxLayout()
        nav.setContentsMargins(16, 16, 16, 16)
        nav.setSpacing(4)
        self.navs = []
        for i, (name, _d, _e) in enumerate(PAGES):
            b = QPushButton(name, objectName="nav", checkable=True)
            b.setIconSize(QSize(24, 24))
            b.clicked.connect(lambda _=False, i=i: self.show_page(i))
            nav.addWidget(b)
            self.navs.append(b)
        sl.addLayout(nav)
        sl.addStretch(1)
        foot = QVBoxLayout()
        foot.setContentsMargins(28, 0, 16, 16)
        foot.addWidget(self._label("Nexus " + VERSION, "small"))
        site = QPushButton("Nexus on GitHub", objectName="link")
        site.clicked.connect(lambda: __import__("webbrowser").open(SITE))
        foot.addWidget(site)
        sl.addLayout(foot)
        top.addWidget(side)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        self.content = QWidget(objectName="content")
        scroll.setWidget(self.content)
        top.addWidget(scroll, 1)

        footer = QFrame(objectName="footer")
        fl = QHBoxLayout(footer)
        fl.setContentsMargins(28, 18, 32, 18)
        fl.addWidget(self._label("Changes apply as you make them. Closing this window keeps Nexus running in "
                                 "the tray."), 1)
        quit_btn = QPushButton("Quit Nexus")
        quit_btn.setMinimumSize(190, 48)
        quit_btn.clicked.connect(QApplication.quit)
        fl.addWidget(quit_btn)
        outer.addWidget(footer)

    def show_page(self, i: int) -> None:
        self.page = i
        self._shape = None
        self.refresh()

    def _status_color(self, s=None) -> str:
        s = self.c.status if s is None else s
        return self.p["signal"] if s == net.CONNECTED else self.p["off"] if s <= net.CONNECTING else (
            self.p["amber"] if s == net.UNREACHABLE else self.p["fault"])

    def refresh(self) -> None:
        c, p = self.c, self.p
        pc = c.shown()
        peer = c.name_of(pc)
        if c.status != net.CONNECTED:
            line, note = STATUS_WORD[c.status], STATUS_NOTE[c.status]
        elif c.remote:
            line = "Input on %s" % c.name_of(c.target)
            note = "Input is on %s. Push the pointer through an edge that leads on, use the switch key, or " \
                   "press Ctrl+Alt+Pause to return." % c.name_of(c.target)
        elif c.cfg.role == "controlled" and c.injector.active:
            line, note = "Controlled by %s" % peer, "%s is driving this PC." % peer
        elif c.cfg.role == "main":
            line = "Connected"
            n = sum(c.up(i) for i in range(MAX_PEERS))
            note = ("Connected to %s." % peer if n == 1 else "Connected to %d PCs." % n) + \
                " Push the pointer through an edge that has a PC beside it on the Crossing page to cross."
        else:
            line, note = "Connected", "Connected to %s, the main PC." % peer
        self.dot.set(self._status_color(), c.status == net.CONNECTED)
        self.line.setText(line)
        if c.cfg.role == "main":
            who = peer if pc >= 0 else next((a for a in c.cfg.peers if a), "")
        else:
            who = peer if c.status == net.CONNECTED else ""
        self.who.setText(who or ("Main PC" if c.cfg.role == "main" else "Controlled PC"))
        rtt = c.sessions[pc].rtt if pc >= 0 else None
        self.response.setText("Response: %s" % ("%d ms" % rtt if rtt is not None and c.up(pc) else "—"))
        for i, b in enumerate(self.navs):
            b.setChecked(i == self.page)
            b.setIcon(page_icon(i, p["ink"] if i == self.page else p["ink2"]))
        # Rebuilt only when what it shows changes: not for each response time, never under the typing hand.
        shape = (self.page, note, c.cfg.role, tuple(c.cfg.peers), tuple(c.cfg.places), bool(c.cfg.key),
                 c.cfg.resistance, c.cfg.switch_key, c.cfg.switch_style, tuple(c.cfg.jumps), tuple(c.cfg.macs),
                 c.paused, self.recording, tuple(s.secret for s in c.sessions),
                 tuple(c.state), tuple(s.peer for s in c.sessions), tuple(s.audio_state for s in c.sessions),
                 c.audio_state, c.sound_from(), c.files_note,
                 tuple(sorted((ip, v[:2]) for ip, v in c.discovery.found.items())))
        focus = QApplication.focusWidget()
        if shape != getattr(self, "_shape", None) and not isinstance(focus, QLineEdit):
            self._shape = shape
            self._page(note)

    def _page(self, note: str) -> None:
        old = self.content.layout()
        if old:
            QWidget().setLayout(old)  # hands the old layout and its widgets to a throwaway parent
        c, main = self.c, self.c.cfg.role == "main"
        lay = QVBoxLayout(self.content)
        lay.setContentsMargins(40, 40, 40, 28)
        lay.setSpacing(0)
        name, desc, eyebrow = PAGES[self.page]
        lay.addWidget(self._label(name, "title", wrap=False))
        lay.addSpacing(14)
        lay.addWidget(self._label(desc, "desc"))
        lay.addSpacing(28)
        module = QFrame(objectName="module")
        m = QVBoxLayout(module)
        m.setContentsMargins(32, 28, 32, 28)
        m.setSpacing(12)
        m.addWidget(self._label(eyebrow, "eyebrow"))
        m.addSpacing(4)
        if self.page == 0:
            if not c.cfg.ready:
                m.addWidget(self._label("Nothing is set up yet. Choose this PC's role and the shared password first."))
                b = QPushButton("Set up the connection")
                b.clicked.connect(lambda: self.show_page(5))
                m.addWidget(b, 0, Qt.AlignLeft)
            else:
                for i in range(MAX_PEERS):
                    if main and not c.cfg.peers[i] or not main and i:
                        continue
                    row = QHBoxLayout()
                    dot = Light(10)
                    dot.set(self._status_color(c.state[i]), c.state[i] == net.CONNECTED)
                    row.addWidget(dot)
                    row.addSpacing(6)
                    row.addWidget(self._label(c.name_of(i) if main or c.up(i) else "Main PC", "line"), 1)
                    m.addLayout(row)
                    sealed = "" if not c.up(i) else "  ·  forward secret" if c.sessions[i].secret else \
                        "  ·  password key only (older Nexus)"
                    m.addWidget(self._label(("Controlled PC  ·  %s  ·  %s" % (c.cfg.peers[i], STATUS_WORD[c.state[i]])
                                             if main else "Main PC  ·  connects to this PC on port %d" % c.port) + sealed,
                                            "small"))
                    if main and not c.up(i) and (c.cfg.macs[i] or c.sessions[i].mac):
                        wake = QPushButton("Wake %s" % c.name_of(i), objectName="link")
                        wake.clicked.connect(lambda _=False, i=i: c.wake(i))
                        m.addWidget(wake, 0, Qt.AlignLeft)
                    m.addSpacing(8)
                m.addWidget(self._label(note))
        elif self.page == 1:
            if not main:
                m.addWidget(self._label("The main PC arranges the PCs. Push the pointer through an edge of this "
                                        "screen that has a PC beside it on the main PC's map to go there."))
            elif not any(c.cfg.peers):
                m.addWidget(self._label("Add a controlled PC on the Connection page; it then shows up here."))
            else:
                m.addWidget(MapView(self), 0, Qt.AlignLeft)
                m.addSpacing(8)
                for i in range(MAX_PEERS):
                    if not c.cfg.peers[i]:
                        continue
                    ways = self._ways_in(i)
                    lab = self._label("%s: through %s." % (c.name_of(i), ways) if ways else
                                      "%s: no edge leads there. Move it beside a PC, or give it a jump key on the "
                                      "Keyboard page." % c.name_of(i))
                    if not ways:
                        lab.setStyleSheet("color: %s;" % self.p["amber"])
                    m.addWidget(lab)
                m.addSpacing(8)
                m.addWidget(self._label("Drag a PC to another cell, or select it and use the arrow keys. A PC "
                                        "crosses to the one beside it and back the same way. The pointer arrives at "
                                        "the same height, and what you copied comes along.", "small"))
                m.addSpacing(16)
                m.addWidget(self._label("Edge resistance", "line"))
                row = QHBoxLayout()
                for label, px in (("Touch", 0), ("Light", 60), ("Normal", 120), ("Firm", 240)):
                    b = QPushButton(label, checkable=True, checked=c.cfg.resistance == px)
                    b.setAccessibleName("Edge resistance: %s" % label)
                    b.clicked.connect(lambda _=False, px=px: c.save_setting(resistance=px))
                    row.addWidget(b)
                row.addStretch(1)
                m.addLayout(row)
                m.addWidget(self._label("An edge gives once the pointer has pushed against it a little, so a "
                                        "window's Close button or a panel at the edge stays reachable. The last few "
                                        "pixels at each corner never cross. Touch crosses at once, as before 0.5.",
                                        "small"))
                pause = QPushButton("Pause crossing: input stays on this PC", checkable=True, checked=c.paused)
                pause.clicked.connect(lambda on: c.set_paused(on))
                m.addWidget(pause, 0, Qt.AlignLeft)
        elif self.page == 2:
            switch = key_name(c.cfg.switch_key) + ("" if c.cfg.switch_style == "hold" else "  × 2")
            rows = [(switch if main else "Set on the main PC",
                     ("Hold it, let go: input comes back to this PC." if c.cfg.switch_style == "hold" else
                      "Double-tap it: input comes back to this PC.") if main else "Switch input between the PCs."),
                    ("Ctrl + Alt + Pause", "Bring input back to this PC at once.")]
            if main:
                rows += [("Press a key…" if self.recording == 2 + i else key_name(c.cfg.jumps[i]) if c.cfg.jumps[i]
                          else "None", "Straight to %s, wherever input is." % c.name_of(i))
                         for i in range(MAX_PEERS) if c.cfg.peers[i]]
                if self.recording == 1:
                    rows[0] = ("Press a key…", rows[0][1])
            records = [1, None] + [2 + i for i in range(MAX_PEERS) if c.cfg.peers[i]]  # what each row's button sets
            for n, (keys, what) in enumerate(rows):
                row = QHBoxLayout()
                cap = self._label(keys, "keycap", wrap=False)
                cap.setFixedWidth(190)
                cap.setAlignment(Qt.AlignCenter)
                row.addWidget(cap)
                row.addSpacing(20)
                row.addWidget(self._label(what), 1)
                if main and records[n]:
                    b = QPushButton("Change" if n == 0 else "Set")
                    b.setAccessibleName("Change the switch key" if n == 0 else "Set the jump key: " + what)
                    b.clicked.connect(lambda _=False, r=records[n]: self.record(r))
                    row.addWidget(b)
                m.addLayout(row)
                if main and n == 0:
                    styles = QHBoxLayout()
                    for style, label in (("tap", "Double-tap"), ("hold", "Hold")):
                        b = QPushButton(label, checkable=True, checked=c.cfg.switch_style == style)
                        b.clicked.connect(lambda _=False, s=style: c.save_setting(switch_style=s))
                        styles.addWidget(b)
                    styles.addStretch(1)
                    m.addLayout(styles)
            m.addWidget(self._label("These work on this PC's keyboard while input is on another PC; to cross, push "
                                    "the pointer through an edge (Wayland lets no app see keys before that). A jump "
                                    "key is Ctrl, Alt or Win with another key; Esc while setting one takes it away."
                                    if main else "These are pressed on the main PC's keyboard.", "small"))
        elif self.page == 3:
            m.addWidget(self._label(self._sound_text()))
        elif self.page == 4:
            m.addWidget(self._label("Copy files or folders in your file manager, cross to the other PC and press "
                                    "Ctrl+V. They travel when you cross and land in a dated folder under Nexus "
                                    "Received on its desktop, whatever their size."))
            if c.files_note:
                m.addWidget(self._label(c.files_note, "line"))
        else:
            self._connection(m, main)
        lay.addWidget(module)
        lay.addStretch(1)

    def _ways_in(self, pc: int) -> str:
        """"this PC's right edge and XP's top edge": the edges that lead to pc on the map."""
        c, ways = self.c, []
        for e in range(4):
            n = c.cfg.neighbour(pc, e)
            if n >= 0:
                ways.append("%s's %s edge" % ("this PC" if n == THIS_PC else c.name_of(n), EDGES[e ^ 1]))
        return " and ".join(ways)

    def _sound_text(self) -> str:
        c, main = self.c, self.c.cfg.role == "main"
        pc = c.sound_from() if main else 0
        name = c.name_of(pc)
        state = c.sessions[pc].audio_state if main and pc >= 0 else c.audio_state
        if c.status != net.CONNECTED:
            return ("Connect a controlled PC first; its sound then plays here." if main else
                    "Once the main PC connects, this PC's sound plays on its speakers.")
        if state in (sound.LOOPBACK, sound.STEREO_MIX):
            return ("%s's sound plays on this PC's speakers: the sound of the PC used last. Its own speakers keep "
                    "playing too; turn them down there if you only want to hear it here." % name) if main else \
                "This PC's sound plays on %s's speakers." % name
        if state == sound.NONE:
            return "%s cannot record what its speakers play." % (name if main else "This PC")
        return "Waiting for %s to report its sound." % (name if main else "this PC")

    def _connection(self, m, main: bool) -> None:
        c = self.c
        row = QHBoxLayout()
        for role, label in (("main", "Main PC"), ("controlled", "Controlled PC")):
            b = QPushButton(label, checkable=True, checked=c.cfg.role == role)
            b.setMinimumWidth(170)
            b.clicked.connect(lambda _=False, r=role: c.set_config(role=r))
            row.addWidget(b)
        row.addStretch(1)
        m.addLayout(row)
        m.addWidget(self._label("The main PC has the keyboard and mouse, and plays the other PCs' sound." if main
                                else "The controlled PC is driven by the main PC's keyboard and mouse.", "small"))
        m.addSpacing(16)
        m.addWidget(self._label("Shared password", "line"))
        pw = QLineEdit(echoMode=QLineEdit.Password, maximumWidth=380)
        pw.setAccessibleName("Shared password")

        def set_password():
            if pw.text():
                from .link import key_from_password
                c.set_config(key=key_from_password(pw.text()))
        pw.returnPressed.connect(set_password)
        m.addWidget(pw)
        m.addWidget(self._label("Set. Type a new one and press Enter to change it." if c.cfg.key else
                                "Type the same password on all PCs and press Enter.", "small"))
        if main:
            m.addSpacing(16)
            grid = QGridLayout()
            grid.setHorizontalSpacing(16)
            for i in range(MAX_PEERS):
                cell = QVBoxLayout()
                cell.addWidget(self._label("Controlled PC %d" % (i + 1), "line"))
                peer = QLineEdit(c.cfg.peers[i], maximumWidth=380)
                peer.setAccessibleName("Controlled PC %d's address" % (i + 1))

                def set_address(peer=peer, i=i):
                    addr = peer.text().strip()
                    if all(ch.isalnum() or ch in ".-" for ch in addr):  # only what a host name or address holds
                        c.set_address(i, addr)
                peer.returnPressed.connect(set_address)
                cell.addWidget(peer)
                grid.addLayout(cell, i // 2, i % 2)
            m.addLayout(grid)
            m.addWidget(self._label("Each controlled PC shows its address on its own Connection page. Clear an "
                                    "address to remove that PC.", "small"))
            found = [(ip, n) for ip, (n, r, _s) in c.discovery.found.items() if r == 1 and ip not in c.cfg.peers]
            free = next((i for i in range(MAX_PEERS) if not c.cfg.peers[i]), -1)
            if found and free >= 0:
                m.addWidget(self._label("Found on this network:", "small"))
                for ip, n in found:
                    b = QPushButton("%s  ·  %s" % (n, ip), objectName="link")
                    b.clicked.connect(lambda _=False, ip=ip: c.set_address(free, ip))
                    m.addWidget(b, 0, Qt.AlignLeft)
        m.addSpacing(16)
        m.addWidget(self._label("This PC's address: %s  ·  port %d" % (local_addresses(), c.port)))

    def record(self, what: int) -> None:
        """The next key pressed in this window sets the switch key (1) or PC n's jump key (2 + n)."""
        self.recording = what
        self.setFocus()
        self.grabKeyboard()
        self.refresh()

    def keyPressEvent(self, e) -> None:
        if not self.recording or e.isAutoRepeat():
            return super().keyPressEvent(e)
        what, c = self.recording, self.c
        win = keymap.to_windows(e.nativeScanCode() - 8)  # an XKB keycode is the evdev code + 8
        vk = win[0] if win else 0
        if e.key() == Qt.Key_Escape:
            if what >= 2:
                jumps = list(c.cfg.jumps)
                jumps[what - 2] = 0
                c.save_setting(jumps=jumps)
        elif what == 1:
            if not vk or e.key() in (Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space):
                return  # not those: they would switch while typing
            c.save_setting(switch_key=vk)
        else:
            mods = e.modifiers()
            chord = ((1 if mods & Qt.ControlModifier else 0) | (2 if mods & Qt.AltModifier else 0) |
                     (4 if mods & Qt.ShiftModifier else 0) | (8 if mods & Qt.MetaModifier else 0))
            if not vk or e.key() in (Qt.Key_Control, Qt.Key_Alt, Qt.Key_Shift, Qt.Key_Meta) or not chord & 11:
                return  # wait for Ctrl, Alt or Win with another key
            jumps = [0 if j == chord << 8 | vk else j for j in c.cfg.jumps]  # one chord, one PC
            jumps[what - 2] = chord << 8 | vk
            c.save_setting(jumps=jumps)
        self.recording = 0
        self.releaseKeyboard()
        self.refresh()

    def closeEvent(self, event) -> None:  # closing keeps Nexus in the tray
        event.ignore()
        self.hide()


def local_addresses() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))  # no packet is sent: this only picks the outgoing address
        return s.getsockname()[0]
    except OSError:
        return "unknown"


def tray_icon(badge) -> QIcon:
    """The app icon with a coloured dot in its corner while input is elsewhere (None: as it is)."""
    base = QIcon(os.path.join(RES, "nexus.ico"))
    if not badge:
        return base
    pix = base.pixmap(64, 64)
    p = QPainter(pix)
    p.setRenderHint(QPainter.Antialiasing)
    p.setPen(QPen(QColor("#0e0f0d"), 4))
    p.setBrush(QColor(badge))
    p.drawEllipse(QRectF(34, 34, 28, 28))
    p.end()
    return QIcon(pix)


def start_log(test: bool) -> str:
    """nexus.log (test.log) beside the config, 1 MB and one older file, and crash.log for an exception
    nothing caught, on any thread. Port of the idea in Beamer core/crash_log.py. Returns the folder."""
    import threading as _threading
    import traceback
    folder = config_dir()
    os.makedirs(folder, exist_ok=True)
    fmt = "%(asctime)s %(name)s %(levelname)s %(message)s"
    handler = logging.handlers.RotatingFileHandler(os.path.join(folder, "test.log" if test else "nexus.log"),
                                                   maxBytes=1024 * 1024, backupCount=1, encoding="utf-8")
    handler.setFormatter(logging.Formatter(fmt))
    logging.basicConfig(level=logging.INFO, format=fmt)
    logging.getLogger().addHandler(handler)

    def crashed(kind, value, tb, thread=""):
        text = "".join(traceback.format_exception(kind, value, tb))
        LOG.error("crash%s: %s", thread, text)
        with open(os.path.join(folder, "crash.log"), "a", encoding="utf-8") as f:
            f.write("%s%s\n%s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), thread, text))
    sys.excepthook = crashed
    _threading.excepthook = lambda a: crashed(a.exc_type, a.exc_value, a.exc_traceback,
                                              " in thread %s" % (a.thread.name if a.thread else "?"))
    return folder


def main() -> None:
    test = "--test" in sys.argv
    log_folder = start_log(test)
    LOG.info("Nexus %s starts", VERSION)
    if test:  # kill -USR1 prints every thread's stack to the log, for hangs
        import faulthandler
        import signal
        faulthandler.register(signal.SIGUSR1)
    app = QApplication(sys.argv)
    app.setApplicationName("Nexus")
    app.setDesktopFileName("nexus")
    app.setQuitOnLastWindowClosed(False)
    for f in ("HankenGrotesk-Regular.ttf", "HankenGrotesk-SemiBold.ttf", "HankenGrotesk-Bold.ttf",
              "B612Mono-Regular.ttf"):
        QFontDatabase.addApplicationFont(os.path.join(RES, "fonts", f))
    sys.path.insert(0, os.path.join(HERE, "..", "beamer"))
    import desktop_portal
    desktop_portal.refresh_qt_screens()
    c = Controller(test)
    win = Window(c)
    icons = {None: tray_icon(None), "away": tray_icon(DARK["signal"]), "paused": tray_icon(DARK["amber"])}
    tray = QSystemTrayIcon(icons[None], app)
    menu = QMenu()

    def about():
        from PySide6.QtWidgets import QMessageBox
        QMessageBox.about(win, "About Nexus",
                          "Nexus %s\n\nOne keyboard, mouse and sound for PCs from Windows XP to 11 and Linux.\n\n%s\n\n"
                          "GPL-3.0-or-later. The look and much of the crossing logic come from Beamer by Toby George "
                          "Read Kalkman (GPL-3.0); the fonts are Hanken Grotesk and B612 Mono (SIL OFL)." % (VERSION, SITE))

    def fill_menu():
        """The menu says what is true now: built again each time it opens, and when the state changes."""
        menu.clear()
        head = menu.addAction("Nexus %s" % VERSION)
        head.setEnabled(False)
        menu.addSeparator()
        menu.addAction(QAction("Open Nexus", menu, triggered=win.showNormal))
        if c.cfg.role == "main":
            send = QAction("Bring input back" if c.remote else "Send input across", menu,
                           triggered=lambda: c.home() if c.remote else c.capture and c.capture.push(
                               lambda: c.notice.emit("Nexus", "This desktop hands input to Nexus only at an edge: "
                                                     "push the pointer through the edge beside the other PC.")))
            send.setEnabled(c.first_connected() >= 0)
            menu.addAction(send)
            menu.addAction(QAction("Pause crossing", menu, checkable=True, checked=c.paused,
                                   triggered=lambda on: c.set_paused(on)))
            for i in range(MAX_PEERS):
                if c.cfg.peers[i] and not c.up(i) and (c.cfg.macs[i] or c.sessions[i].mac):
                    menu.addAction(QAction("Wake %s" % c.name_of(i), menu, triggered=lambda _=False, i=i: c.wake(i)))
        menu.addSeparator()
        menu.addAction(QAction("Open log folder", menu,
                               triggered=lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(log_folder))))
        menu.addAction(QAction("About Nexus", menu, triggered=about))
        menu.addSeparator()
        menu.addAction(QAction("Quit", menu, triggered=app.quit))

    def state():
        away = c.remote if c.cfg.role == "main" else c.status == net.CONNECTED and not c.injector.active
        tray.setIcon(icons["away" if away else "paused" if c.cfg.role == "main" and c.paused else None])
        tray.setToolTip("Nexus: %s" % win.line.text())
        fill_menu()
    tray.setContextMenu(menu)
    c.changed.connect(state)
    c.notice.connect(lambda title, text: (LOG.info("notice: %s", text), tray.showMessage(title, text)))
    tray.activated.connect(lambda reason: win.showNormal() if reason == QSystemTrayIcon.Trigger else None)
    state()
    tray.show()
    c.start()
    if "--tray" not in sys.argv:
        win.show()
    sys.exit(app.exec())
