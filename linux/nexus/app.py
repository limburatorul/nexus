"""Nexus for Linux: the controller (what src/nexus.c does on Windows) and its window, in Beamer's
Vernier look. Run with: python -m nexus [--test] [--tray]
"""

import os
import socket
import struct
import sys
import tempfile
import threading
import time

from PySide6.QtCore import QObject, QRectF, QSize, Qt, QTimer, Signal, Slot
from PySide6.QtGui import QAction, QColor, QFont, QFontDatabase, QGuiApplication, QIcon, QPainter, QPen, QPixmap
from PySide6.QtWidgets import (QApplication, QFrame, QHBoxLayout, QLabel, QLineEdit, QMenu, QPushButton,
                               QScrollArea, QSizePolicy, QSystemTrayIcon, QVBoxLayout, QWidget)

from . import VERSION, link, net, sound
from .config import EDGES, Config
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
    ("Overview", "The PC this one works with, where input is right now, and how the link is doing.", "MACHINE"),
    ("Crossing", "Which edge of this screen leads to the controlled PC. Push the pointer through it to cross, "
                 "and back through the facing edge over there to return.", "EDGE"),
    ("Keyboard", "Switch without the mouse, and a way home that always works.", "SHORTCUTS"),
    ("Sound", "The controlled PC's sound, played on this PC's speakers.", "SOUND"),
    ("Files", "Copy files on one PC, switch, and paste them on the other.", "FILES"),
    ("Connection", "This PC's role, the password both PCs share, and where to find the other one.", "THIS PC"),
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


class Controller(QObject):
    """Everything Nexus does, on the Qt thread; the network, sound and portal threads report here."""

    changed = Signal()
    crossed = Signal()  # from the capture thread: input went over, the clipboard follows
    _status = Signal(int)
    _message = Signal(bytes)
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
        self.status, self.rtt, self.remote, self.audio_state, self.files_note = net.SETUP, None, False, 0, ""
        self.files = Files(self._send, self._note.emit, self._files_done.emit)
        self.session = net.Session(self.cfg, self.name, VERSION, self.port, self._status.emit, self._message.emit,
                                   files=self.files)
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
        self.send = self._send
        self.timer = QTimer(self, interval=1000, timeout=self.tick)

    def start(self) -> None:
        self.injector.start()  # also the clipboard's way in, on either role
        self.start_capture()
        self.session.start()
        self.discovery.start()
        self.timer.start()
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
        return self.session.send(msg)

    # ---- what the threads report ----

    @Slot(int)
    def on_status(self, status: int) -> None:
        if status != net.CONNECTED:
            if self.capture:
                self.capture.home(None, tell=False)
            self.rtt, self.remote, self.audio_state = None, False, 0
            self.injector.release_all()
            self.injector.active = False
            self.sound_capture.stop()
            self.player.stop()
        elif self.cfg.role == "controlled":
            self.sound_capture.start()
        self.status = status
        self.changed.emit()

    @Slot(bytes)
    def on_message(self, msg: bytes) -> None:
        kind = msg[0]
        if kind == link.CLIPBOARD and len(msg) > 1:
            self.clipboard.set_text(msg[1:].decode("utf-8", "replace"))
        elif kind == link.PONG and len(msg) == 5:
            self.rtt = ((int(time.monotonic() * 1000) & 0xFFFFFFFF) - struct.unpack("<I", msg[1:])[0]) & 0xFFFFFFFF
            self.changed.emit()
        elif self.cfg.role == "main":
            if kind == link.LEAVE and len(msg) == 4:
                if self.capture:
                    self.capture.home(msg[2] | msg[3] << 8 if msg[1] else None, tell=False)
                self.remote = False
                self.changed.emit()
            elif kind == link.AUDIO:
                self.player.play(msg)
            elif kind == link.AUDIO_STATE and len(msg) == 2:
                self.audio_state = msg[1]
                self.changed.emit()
        else:
            was = self.injector.active
            out = self.injector.replay(msg)
            if was and not self.injector.active:
                self.send_clipboard()  # ahead of LEAVE, so it is there on arrival
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

    # ---- actions ----

    def send_clipboard(self) -> None:
        got = self.clipboard.changed()
        if not got:
            return
        kind, value = got
        if kind == "files":
            if not self.files.send_paths(value, self.session.peer or "the other PC"):
                self.on_note("Still sending the last files; copy the new ones again once they are across.")
        else:
            self._send(bytes([link.CLIPBOARD]) + value.encode("utf-8")[:link.MAX - 1])

    def cross(self) -> None:
        """Main PC: input goes over (the shortcut and the edge both end here)."""
        if self.cfg.role == "main" and self.status == net.CONNECTED and not self.remote:
            self.send_clipboard()
            self._send(bytes([link.ENTER, EDGES.index(self.cfg.edge), 1, 0, 0]))
            self.remote = True
            self.changed.emit()

    def home(self) -> None:
        if self.cfg.role == "main" and self.remote:
            self._send(bytes([link.LEAVE, 0, 0, 0]))
            self.remote = False
            self.changed.emit()

    def set_config(self, **values) -> None:
        for k, v in values.items():
            setattr(self.cfg, k, v)
        self.cfg.save()
        self.session.restart()
        self.start_capture()
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
        if verb == "status":
            return "role=%s status=%d peer=%s peer_version=%s remote=%d active=%d audio=%d version=%s files=%s" % (
                self.cfg.role, self.status, self.session.peer, self.session.peer_version, self.remote,
                self.injector.active, self.audio_state, VERSION, self.files_note)
        if verb == "cross":
            self.cross()
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
    """The Windows app's line icons, drawn the same way in a 24-unit box."""
    pix = QPixmap(size * 2, size * 2)
    pix.setDevicePixelRatio(2)
    pix.fill(Qt.transparent)
    p = QPainter(pix)
    p.setRenderHint(QPainter.Antialiasing)
    p.setPen(QPen(QColor(color), 1.6))
    if page == 0:
        p.drawRect(2, 4, 15, 11), p.drawLine(9, 15, 9, 19), p.drawLine(5, 19, 13, 19), p.drawRect(14, 10, 9, 10)
    elif page == 1:
        p.drawPolyline([QPointF(*q) for q in ((3, 8), (21, 8), (17, 4))])
        p.drawPolyline([QPointF(*q) for q in ((21, 16), (3, 16), (7, 20))])
    elif page == 2:
        p.drawRoundedRect(QRectF(1, 5, 22, 14), 2, 2)
        for i in range(5):
            p.drawPoint(5 + 3 * i, 9)
        p.drawLine(7, 15, 17, 15)
    elif page == 3:
        p.drawPolygon([QPointF(*q) for q in ((3, 9), (7, 9), (12, 4), (12, 20), (7, 15), (3, 15))])
        p.drawArc(QRectF(10, 7, 10, 10), -60 * 16, 120 * 16)
        p.drawArc(QRectF(11, 3, 13, 18), -60 * 16, 120 * 16)
    elif page == 4:
        p.drawPolygon([QPointF(*q) for q in ((2, 6), (9, 6), (11, 8), (22, 8), (22, 19), (2, 19))])
    else:
        p.drawEllipse(QRectF(2, 6, 12, 12)), p.drawEllipse(QRectF(10, 6, 12, 12))
    p.end()
    return QIcon(pix)


from PySide6.QtCore import QPointF  # noqa: E402  (used by page_icon)


class Window(QWidget):
    def __init__(self, c: Controller):
        super().__init__()
        self.c = c
        self.page = 0
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
        QWidget {{ background: {p['ground']}; color: {p['ink']}; font-family: 'Hanken Grotesk'; font-size: 14px; }}
        #sidebar {{ border-right: 1px solid {p['rule']}; }}
        #status {{ border-bottom: 1px solid {p['rule']}; }}
        #footer {{ border-top: 1px solid {p['rule']}; }}
        QLabel {{ background: transparent; }}
        QLabel#title {{ font-size: 28px; font-weight: 700; }}
        QLabel#desc, QLabel#body {{ color: {p['ink2']}; }}
        QLabel#small {{ color: {p['ink3']}; font-size: 13px; }}
        QLabel#eyebrow {{ color: {p['ink3']}; font-size: 11px; font-weight: 600; letter-spacing: 1.5px; }}
        QLabel#line {{ font-weight: 600; font-size: 15px; }}
        QLabel#keycap {{ background: {p['well']}; border: 1px solid {p['edge']}; border-radius: 6px;
                         font-family: 'B612 Mono'; font-size: 13px; padding: 10px 18px; }}
        #module {{ background: {p['panel']}; }}
        #module QWidget {{ background: {p['panel']}; }}
        QPushButton {{ background: transparent; border: 1px solid {p['edge']}; border-radius: 5px; padding: 10px 18px;
                       font-weight: 600; color: {p['ink']}; }}
        QPushButton:hover {{ background: {p['well']}; border-color: {p['ink2']}; }}
        QPushButton:checked {{ background: {p['ink']}; color: {p['ground']}; border-color: {p['ink']}; }}
        QPushButton:focus {{ outline: none; border: 2px solid {p['signal']}; }}
        QPushButton#nav {{ border: none; text-align: left; padding: 14px 16px; font-weight: 400; font-size: 15px;
                           color: {p['ink2']}; border-left: 3px solid transparent; border-radius: 6px; }}
        QPushButton#nav:hover {{ background: {p['panel']}; color: {p['ink']}; }}
        QPushButton#nav:checked {{ background: {p['well']}; color: {p['ink']}; font-weight: 600;
                                   border-left: 3px solid {p['signal']}; }}
        QPushButton#link {{ border: none; text-align: left; padding: 4px 0; font-weight: 400; color: {p['ink3']};
                            font-size: 13px; }}
        QPushButton#link:hover {{ color: {p['ink']}; background: transparent; }}
        QLineEdit {{ background: {p['well']}; border: 1px solid {p['edge']}; border-radius: 4px; padding: 10px 12px; }}
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
        self.dot = QLabel()
        self.dot.setFixedSize(12, 12)
        self.line = self._label(name="line", wrap=False)
        row.addWidget(self.dot)
        row.addSpacing(12)
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
        self.content = QWidget()
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

    def _status_color(self) -> str:
        s = self.c.status
        return self.p["signal"] if s == net.CONNECTED else self.p["off"] if s <= net.CONNECTING else (
            self.p["amber"] if s == net.UNREACHABLE else self.p["fault"])

    def refresh(self) -> None:
        c, p = self.c, self.p
        peer = c.session.peer or "the other PC"
        if c.status != net.CONNECTED:
            line, note = STATUS_WORD[c.status], STATUS_NOTE[c.status]
        elif c.remote:
            line = "Input on %s" % peer
            note = "Input is on %s. Push back through its edge, double-tap Right Ctrl, or press Ctrl+Alt+Pause " \
                   "to return." % peer
        elif c.cfg.role == "controlled" and c.injector.active:
            line, note = "Controlled by %s" % peer, "%s is driving this PC." % peer
        else:
            line = "Connected"
            note = ("Connected to %s. Push the pointer through the %s edge or double-tap Right Ctrl to cross."
                    % (peer, c.cfg.edge)) if c.cfg.role == "main" else "Connected to %s, the main PC." % peer
        self.dot.setStyleSheet("background: %s; border-radius: 6px;" % self._status_color())
        self.line.setText(line)
        who = c.session.peer if c.status == net.CONNECTED else (c.cfg.peer if c.cfg.role == "main" else "")
        self.who.setText(who or ("Main PC" if c.cfg.role == "main" else "Controlled PC"))
        self.response.setText("Response: %s" % ("%d ms" % c.rtt if c.rtt is not None and c.status == net.CONNECTED
                                                 else "—"))
        for i, b in enumerate(self.navs):
            b.setChecked(i == self.page)
            b.setIcon(page_icon(i, p["ink"] if i == self.page else p["ink2"]))
        # Rebuilt only when what it shows changes: not for each response time, never under the typing hand.
        shape = (self.page, note, c.cfg.role, c.cfg.edge, c.cfg.peer, bool(c.cfg.key), c.status, c.audio_state,
                 c.files_note, tuple(sorted((ip, v[:2]) for ip, v in c.discovery.found.items())))
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
        lay.addWidget(self._label(name.upper(), "title", wrap=False))
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
                m.addWidget(self._label(c.session.peer if c.status == net.CONNECTED and c.session.peer else
                                        c.cfg.peer if main else "Main PC", "line"))
                m.addWidget(self._label(("Controlled PC  ·  %s  ·  port %d" % (c.cfg.peer, c.port)) if main
                                        else "Main PC  ·  connects to this PC on port %d" % c.port, "small"))
                m.addWidget(self._label(note))
        elif self.page == 1:
            if main:
                m.addWidget(self._label("The controlled PC sits on this side of this screen:"))
                row = QHBoxLayout()
                for e in EDGES:
                    b = QPushButton(e.capitalize(), checkable=True, checked=c.cfg.edge == e)
                    b.setMinimumWidth(110)
                    b.clicked.connect(lambda _=False, e=e: c.set_config(edge=e))
                    row.addWidget(b)
                row.addStretch(1)
                m.addLayout(row)
                m.addWidget(self._label("The pointer arrives at the same height on the other screen. Copied text "
                                        "comes along when you cross.", "small"))
            else:
                m.addWidget(self._label("The edge is chosen on the main PC. Push the pointer back through the edge "
                                        "of this screen that faces it to return input there."))
        elif self.page == 2:
            for keys, what in (("Right Ctrl  × 2", "Switch input to the other PC and back."),
                               ("Ctrl + Alt + Pause", "Bring input back to this PC at once.")):
                row = QHBoxLayout()
                cap = self._label(keys, "keycap", wrap=False)
                cap.setFixedWidth(190)
                cap.setAlignment(Qt.AlignCenter)
                row.addWidget(cap)
                row.addSpacing(20)
                row.addWidget(self._label(what), 1)
                m.addLayout(row)
            m.addWidget(self._label("Both work on this PC's keyboard, wherever input is." if main else
                                    "These are pressed on the main PC's keyboard.", "small"))
        elif self.page == 3:
            m.addWidget(self._label(self._sound_text()))
        elif self.page == 4:
            m.addWidget(self._label("Copy files or folders in your file manager, cross to the other PC and press "
                                    "Ctrl+V. They travel when you cross and land in a dated folder under Nexus "
                                    "Received on its desktop, up to 1 GB at a time."))
            if c.files_note:
                m.addWidget(self._label(c.files_note, "line"))
        else:
            self._connection(m, main)
        lay.addWidget(module)
        lay.addStretch(1)

    def _sound_text(self) -> str:
        c, main = self.c, self.c.cfg.role == "main"
        name = c.session.peer or "the other PC"
        if c.status != net.CONNECTED:
            return ("Connect a controlled PC first; its sound then plays here." if main else
                    "Once the main PC connects, this PC's sound plays on its speakers.")
        if c.audio_state in (sound.LOOPBACK, sound.STEREO_MIX):
            return ("%s's sound plays on this PC's speakers. Its own speakers keep playing too; turn them down "
                    "there if you only want to hear it here." % name) if main else \
                "This PC's sound plays on %s's speakers." % name
        if c.audio_state == sound.NONE:
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
        m.addWidget(self._label("The main PC has the keyboard and mouse, and plays the other PC's sound." if main
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
                                "Type the same password on both PCs and press Enter.", "small"))
        if main:
            m.addSpacing(16)
            m.addWidget(self._label("Controlled PC's address", "line"))
            peer = QLineEdit(c.cfg.peer, maximumWidth=380)
            peer.setAccessibleName("Controlled PC's address")
            peer.returnPressed.connect(lambda: c.set_config(peer=peer.text().strip()))
            m.addWidget(peer)
            m.addWidget(self._label("The controlled PC shows its address on its own Connection page.", "small"))
            found = [(ip, n) for ip, (n, r, _s) in c.discovery.found.items() if r == 1]
            if found:
                m.addWidget(self._label("Found on this network:", "small"))
                for ip, n in found:
                    b = QPushButton("%s  ·  %s" % (n, ip), objectName="link")
                    b.clicked.connect(lambda _=False, ip=ip: c.set_config(peer=ip))
                    m.addWidget(b, 0, Qt.AlignLeft)
        m.addSpacing(16)
        m.addWidget(self._label("This PC's address: %s  ·  port %d" % (local_addresses(), c.port)))

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


def main() -> None:
    import logging
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    test = "--test" in sys.argv
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
    tray = QSystemTrayIcon(QIcon(os.path.join(RES, "nexus.ico")), app)
    menu = QMenu()
    menu.addAction(QAction("Open Nexus", menu, triggered=win.showNormal))
    menu.addSeparator()
    menu.addAction(QAction("Quit", menu, triggered=app.quit))
    tray.setContextMenu(menu)
    tray.activated.connect(lambda reason: win.showNormal() if reason == QSystemTrayIcon.Trigger else None)
    tray.show()
    c.start()
    if "--tray" not in sys.argv:
        win.show()
    sys.exit(app.exec())
