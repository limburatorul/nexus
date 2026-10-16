"""Copied files follow input, as src/files.c: FILE_BEGIN [count u32][total u64], then per item FILE
[is_dir u8][size u64][relative path, UTF-8, "\\" between parts] and FILE_DATA chunks, then FILE_END.
Received items land in <Desktop>/Nexus Received/<date time>/ and go on the clipboard.
"""

import os
import shutil
import struct
import threading
import time

from . import link

CHUNK = 32768


def desktop_dir() -> str:
    """XDG_DESKTOP_DIR from user-dirs.dirs, else ~/Desktop."""
    home = os.path.expanduser("~")
    path = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.join(home, ".config"), "user-dirs.dirs")
    try:
        for line in open(path, encoding="utf-8"):
            if line.startswith("XDG_DESKTOP_DIR="):
                return line.split("=", 1)[1].strip().strip('"').replace("$HOME", home)
    except OSError:
        pass
    return os.path.join(home, "Desktop")


def size_text(n: int) -> str:
    if n < 1048576:
        return "%d KB" % ((n + 1023) // 1024)
    return "%.1f MB" % (n / 1048576) if n < 1 << 30 else "%.1f GB" % (n / (1 << 30))


def safe_parts(rel: str):
    """The parts of a received relative path, or None if it could leave the received folder."""
    parts = rel.split("\\")
    for p in parts:
        if p in ("", ".", "..") or "/" in p or "\0" in p:
            return None
    return parts


class Files:
    def __init__(self, send, on_note, on_done):
        """send(msg) -> bool puts one message on the link; on_note(text) and on_done(paths) report."""
        self.send, self.on_note, self.on_done = send, on_note, on_done
        self._sending = threading.Lock()
        self._rx = None

    # ---- sending ----

    def send_paths(self, paths, peer: str) -> bool:
        """Sends files and folders on a background thread; False while a transfer still runs."""
        if not self._sending.acquire(blocking=False):
            return False
        threading.Thread(target=self._send_thread, args=(list(paths), peer), daemon=True).start()
        return True

    def _items(self, paths):
        """(path, relative path with "\\", is_dir) for each item, parents first; links are not followed."""
        for top in paths:
            top = top.rstrip("/")
            base = os.path.dirname(top)
            if os.path.isdir(top) and not os.path.islink(top):
                for root, dirs, names in os.walk(top, followlinks=False):
                    yield root, os.path.relpath(root, base).replace(os.sep, "\\"), True
                    for name in names:
                        full = os.path.join(root, name)
                        if os.path.isfile(full):
                            yield full, os.path.relpath(full, base).replace(os.sep, "\\"), False
            elif os.path.isfile(top):
                yield top, os.path.basename(top), False

    def _send_thread(self, paths, peer):
        try:
            items = list(self._items(paths))
            total = sum(os.path.getsize(p) for p, _, d in items if not d)
            count = len(items)
            plural = "" if count == 1 else "s"
            self.on_note("Sending %d item%s (%s) to %s…" % (count, plural, size_text(total), peer))
            ok = self.send(bytes([link.FILE_BEGIN]) + struct.pack("<IQ", count, total))
            sent = 0
            for path, rel, is_dir in items:
                if not ok:
                    break
                try:
                    f = None if is_dir else open(path, "rb")
                except OSError:
                    continue  # unreadable: skipped
                size = 0 if is_dir else os.fstat(f.fileno()).st_size
                ok = self.send(bytes([link.FILE, int(is_dir)]) + struct.pack("<Q", size) + rel.encode("utf-8"))
                while ok and f:
                    chunk = f.read(CHUNK)
                    if not chunk:
                        break
                    ok = self.send(bytes([link.FILE_DATA]) + chunk)
                    sent += len(chunk)
                if f:
                    f.close()
            ok = ok and self.send(bytes([link.FILE_END]))
            if ok:
                self.on_note("Sent %d item%s (%s) to %s." % (count, plural, size_text(total), peer))
            else:
                self.on_note("Sending to %s stopped part way; copy the files again to retry." % peer)
        finally:
            self._sending.release()

    # ---- receiving (network thread) ----

    def abort(self) -> None:
        rx, self._rx = self._rx, None
        if not rx:
            return
        if rx["file"]:
            rx["file"].close()
        try:
            os.rmdir(rx["dir"])  # only when nothing arrived
            self.on_note("A transfer was refused or stopped before anything arrived.")
        except OSError:
            self.on_note("The transfer stopped part way. What arrived is in %s." % rx["dir"])

    def receive(self, msg: bytes) -> None:
        kind = msg[0]
        if kind == link.FILE_BEGIN:
            self._begin(msg)
        elif not self._rx:
            return  # the rest of a batch that was refused or aborted
        elif kind == link.FILE:
            self._item(msg)
        elif kind == link.FILE_DATA:
            self._data(msg[1:])
        elif kind == link.FILE_END and len(msg) == 1:
            self._end()

    def _begin(self, msg):
        if len(msg) != 13:
            return
        self.abort()
        count, total = struct.unpack("<IQ", msg[1:])
        base = os.path.join(desktop_dir(), "Nexus Received")
        os.makedirs(base, exist_ok=True)
        free = shutil.disk_usage(base).free
        if total > free:  # no size limit, only the disk's: refused before it fills the disk
            self.on_note("The copied files are %s and this PC's disk has %s free, so they were not received."
                         % (size_text(total), size_text(free)))
            return
        stamp = time.strftime("%Y-%m-%d %H-%M-%S")
        folder = os.path.join(base, stamp)
        for i in range(2, 10):  # two batches within one second
            try:
                os.mkdir(folder)
                break
            except FileExistsError:
                folder = os.path.join(base, "%s %d" % (stamp, i))
        else:
            return
        self._rx = {"dir": folder, "file": None, "left": 0, "got": 0, "total": total, "count": count,
                    "tops": [], "pct": -10}
        self.on_note("Receiving %d item%s (%s)…" % (count, "" if count == 1 else "s", size_text(total)))

    def _item(self, msg):
        rx = self._rx
        if len(msg) < 11 or rx["left"]:
            return self.abort()  # a new item before the last one was complete
        if rx["file"]:
            rx["file"].close()
            rx["file"] = None
        is_dir, size = msg[1], struct.unpack("<Q", msg[2:10])[0]
        try:
            parts = safe_parts(msg[10:].decode("utf-8"))
        except UnicodeDecodeError:
            parts = None
        if not parts or rx["got"] + size > rx["total"]:
            return self.abort()
        path = os.path.join(rx["dir"], *parts)
        if len(parts) == 1:
            rx["tops"].append(path)
        try:
            if is_dir:
                os.makedirs(path, exist_ok=True)
                return
            os.makedirs(os.path.dirname(path), exist_ok=True)
            rx["file"] = open(path, "xb")
        except OSError:
            return self.abort()
        rx["left"] = size
        if not size:
            rx["file"].close()
            rx["file"] = None

    def _data(self, data):
        rx = self._rx
        if not rx["file"] or len(data) > rx["left"]:
            return self.abort()
        try:
            rx["file"].write(data)
        except OSError:
            self.on_note("Writing a received file failed. Is the disk full?")
            return self.abort()
        rx["left"] -= len(data)
        rx["got"] += len(data)
        if not rx["left"]:
            rx["file"].close()
            rx["file"] = None
        pct = rx["got"] * 100 // rx["total"] if rx["total"] else 100
        if pct // 10 != rx["pct"] // 10:
            rx["pct"] = pct
            self.on_note("Receiving… %d%%" % pct)

    def _end(self):
        rx, self._rx = self._rx, None
        if rx["file"]:
            rx["file"].close()
        if rx["tops"]:
            self.on_done(rx["tops"])
        count = rx["count"]
        self.on_note("Received %d item%s into %s. They are on the clipboard: paste them with Ctrl+V."
                     % (count, "" if count == 1 else "s", rx["dir"]))
