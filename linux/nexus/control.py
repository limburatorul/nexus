"""Test mode ("--test"): a script drives this instance over TCP 24811, as on Windows. The driver
connects with the link's key (playing the main role in the handshake) and sends MSG_TEST + a
command; execute(command) answers it, on the app's own thread.
"""

import socket
import threading

from . import link

PORT = 24811


class Control:
    def __init__(self, key, name: str, version: str, execute):
        self.key, self.name, self.version, self.execute = key, name, version, execute

    def start(self) -> None:
        threading.Thread(target=self._run, daemon=True, name="nexus-control").start()

    def _run(self) -> None:
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("", PORT))
        server.listen(1)
        while True:
            sock, _ = server.accept()
            try:
                ln = link.Link(sock, False, self.key(), self.name, self.version)
                while True:
                    msg = ln.recv()
                    if msg[:1] == bytes([link.TEST]):
                        answer = self.execute(msg[1:].decode("utf-8", "replace"))
                        ln.send(bytes([link.TEST]) + answer.encode("utf-8")[:4096])
            except Exception:
                pass
            finally:
                sock.close()
