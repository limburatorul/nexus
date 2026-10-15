"""Who else runs Nexus on this network, as on Windows: UDP "NXUS?" asks, every Nexus answers
"NXUS!" + role byte + version + NUL + name. Not secret and not trusted: an answer only fills in an
address; connecting still needs the password.
"""

import socket
import threading
import time

PORT, TEST_PORT = 24802, 24812


class Discovery:
    def __init__(self, port: int, name: str, version: str, role, on_change):
        """role() says this PC's role now (0 main, 1 controlled); on_change() runs on its thread."""
        self.port, self.name, self.version, self.role, self.on_change = port, name, version, role, on_change
        self.found = {}  # ip -> (name, role, last heard)
        self.sock = None

    def start(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("", self.port))
        except OSError:
            sock.close()
            return  # another program has the port: no discovery, nothing else breaks
        self.sock = sock
        threading.Thread(target=self._listen, daemon=True, name="nexus-discovery").start()

    def probe(self) -> None:
        """Main PC: ask, every few seconds."""
        if self.sock:
            try:
                self.sock.sendto(b"NXUS?", ("255.255.255.255", self.port))
            except OSError:
                pass

    def expire(self) -> bool:
        gone = [ip for ip, (_n, _r, seen) in self.found.items() if time.monotonic() - seen > 10]
        for ip in gone:
            del self.found[ip]
        return bool(gone)

    def _listen(self) -> None:
        while True:
            try:
                data, addr = self.sock.recvfrom(160)
            except OSError:
                time.sleep(0.05)
                continue
            if data == b"NXUS?":
                reply = (b"NXUS!" + bytes([self.role()]) + self.version.encode() + b"\0"
                         + self.name.encode("utf-8")[:63])
                try:
                    self.sock.sendto(reply, addr)
                except OSError:
                    pass
            elif data.startswith(b"NXUS!") and len(data) > 8 and data[5] <= 1:
                _version, sep, name = data[6:].partition(b"\0")
                name = name.decode("utf-8", "replace")
                if not sep or name == self.name:  # our own answer
                    continue
                before = self.found.get(addr[0])
                self.found[addr[0]] = (name, data[5], time.monotonic())
                if not before or before[:2] != (name, data[5]):
                    self.on_change()
