"""Settings in ~/.config/nexus/nexus.ini, the same keys as the Windows nexus.ini.

role = main | controlled, edge = left | right | top | bottom, peer = the controlled PC's address,
key = the stretched shared password (64 hex digits), never the password itself.
"""

import configparser
import os
from dataclasses import dataclass

EDGES = ("left", "right", "top", "bottom")


def config_dir() -> str:
    return os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "nexus")


@dataclass
class Config:
    path: str
    role: str = "main"
    edge: str = "right"
    peer: str = ""
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
        edge = section.get("edge", "right")
        return cls(path=path, role=role if role in ("main", "controlled") else "main",
                   edge=edge if edge in EDGES else "right", peer=section.get("peer", "").strip(), key=key_bytes)

    def save(self) -> None:
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        parser = configparser.ConfigParser()
        parser["nexus"] = {"role": self.role, "edge": self.edge, "peer": self.peer, "key": self.key.hex()}
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            parser.write(f)
        os.replace(tmp, self.path)

    @property
    def ready(self) -> bool:
        return bool(self.key) and (self.role == "controlled" or bool(self.peer))
