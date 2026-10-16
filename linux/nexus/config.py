"""Settings in ~/.config/nexus/nexus.ini, the same keys as the Windows nexus.ini.

role = main | controlled, peer..peer4 = the controlled PCs' addresses, place..place4 = "x,y", each
one's cell on the 5 x 3 map around this PC (0,0), key = the stretched shared password (64 hex
digits), never the password itself. Before 0.3, edge = left | right | top | bottom placed the one peer.
"""

import configparser
import os
from dataclasses import dataclass, field

EDGES = ("left", "right", "top", "bottom")
DIR = ((-1, 0), (1, 0), (0, -1), (0, 1))  # a step through each edge
MAX_PEERS = 4
THIS_PC = MAX_PEERS  # this PC, where a PC number goes
MAP_W, MAP_H = 5, 3
PEER_KEYS = ("peer", "peer2", "peer3", "peer4")
PLACE_KEYS = ("place", "place2", "place3", "place4")
FREE_CELLS = ((1, 0), (-1, 0), (0, -1), (0, 1), (2, 0), (-2, 0), (1, -1), (1, 1),
              (-1, -1), (-1, 1), (2, -1), (2, 1), (-2, -1), (-2, 1))


def config_dir() -> str:
    return os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "nexus")


def on_map(x: int, y: int) -> bool:
    return -(MAP_W // 2) <= x <= MAP_W // 2 and -(MAP_H // 2) <= y <= MAP_H // 2


@dataclass
class Config:
    path: str
    role: str = "main"
    peers: list = field(default_factory=lambda: [""] * MAX_PEERS)
    places: list = field(default_factory=lambda: [(0, 0)] * MAX_PEERS)
    key: bytes = b""

    @classmethod
    def load(cls, test: bool = False) -> "Config":
        path = os.path.join(config_dir(), "test.ini" if test else "nexus.ini")
        parser = configparser.ConfigParser()
        parser.read(path, encoding="utf-8")
        section = parser["nexus"] if parser.has_section("nexus") else {}
        key = section.get("key", "")
        try:
            key_bytes = bytes.fromhex(key) if len(key) == 64 else b""
        except ValueError:
            key_bytes = b""
        role = section.get("role", "main")
        cfg = cls(path=path, role=role if role in ("main", "controlled") else "main", key=key_bytes)
        edge = section.get("edge", "right")
        for i in range(MAX_PEERS):
            cfg.peers[i] = section.get(PEER_KEYS[i], "").strip()
            place = section.get(PLACE_KEYS[i], "")
            try:
                x, y = (int(v) for v in place.split(","))
            except ValueError:  # 0,0 is this PC's cell, so a bad place gets a free one
                x, y = DIR[EDGES.index(edge) if edge in EDGES else 1] if i == 0 and not place else (0, 0)
            cfg.places[i] = (x, y)
        for i in range(MAX_PEERS):
            if cfg.peers[i] and (not on_map(*cfg.places[i]) or cfg.peer_at(*cfg.places[i]) != i):
                cfg.free_cell(i)
        return cfg

    def save(self) -> None:
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        parser = configparser.ConfigParser()
        values = {"role": self.role}
        for i in range(MAX_PEERS):
            if self.peers[i]:
                values[PEER_KEYS[i]] = self.peers[i]
                values[PLACE_KEYS[i]] = "%d,%d" % self.places[i]
        values["key"] = self.key.hex()
        parser["nexus"] = values
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            parser.write(f)
        os.replace(tmp, self.path)

    @property
    def ready(self) -> bool:
        return bool(self.key) and (self.role == "controlled" or any(self.peers))

    # ---- the map: controlled PCs on a grid around this one ----

    def peer_at(self, x: int, y: int) -> int:
        """Who stands in a cell: a PC number, THIS_PC, or -1."""
        if (x, y) == (0, 0):
            return THIS_PC
        return next((i for i in range(MAX_PEERS) if self.peers[i] and self.places[i] == (x, y)), -1)

    def neighbour(self, pc: int, edge: int) -> int:
        """Who is past an edge of a PC (THIS_PC for this one)."""
        x, y = (0, 0) if pc == THIS_PC else self.places[pc]
        return self.peer_at(x + DIR[edge][0], y + DIR[edge][1])

    def free_cell(self, pc: int) -> None:
        """A cell for a newly added PC: beside this one first."""
        for cell in FREE_CELLS:
            if self.peer_at(*cell) in (-1, pc):
                self.places[pc] = cell
                return

    def move(self, pc: int, x: int, y: int) -> bool:
        """Drags and arrow keys on the map end here; a PC already in that cell swaps places."""
        other = self.peer_at(x, y)
        if not on_map(x, y) or other in (THIS_PC, pc):
            return False
        if other >= 0:
            self.places[other] = self.places[pc]
        self.places[pc] = (x, y)
        return True


if __name__ == "__main__":  # self-check: python -m nexus.config
    import tempfile
    d = tempfile.mkdtemp()
    os.environ["XDG_CONFIG_HOME"] = d
    os.makedirs(os.path.join(d, "nexus"))
    with open(os.path.join(d, "nexus", "test.ini"), "w") as f:
        f.write("[nexus]\nrole=main\nedge=left\npeer=a\npeer2=b\nplace2=2,0\npeer3=c\nplace3=2,0\nkey=%s\n" % ("0" * 64))
    c = Config.load(True)
    assert c.places[:3] == [(-1, 0), (2, 0), (1, 0)], c.places  # legacy edge; a taken cell moves c beside us
    assert c.neighbour(THIS_PC, 1) == 2 and c.neighbour(2, 1) == 1 and c.neighbour(1, 0) == 2
    assert c.neighbour(0, 1) == THIS_PC and c.neighbour(THIS_PC, 2) == -1
    assert c.move(0, 2, 0) and c.places[0] == (2, 0) and c.places[1] == (-1, 0)  # swap
    assert not c.move(0, 0, 0) and not c.move(0, 3, 0)
    c.save()
    assert Config.load(True).places == c.places
    print("config ok")
