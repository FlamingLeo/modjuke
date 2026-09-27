"""Persistent shuffle orders, one per queue source (library folder or playlist).

A drawn order is only replaced when a new one is explicitly drawn ("Shuffle now" or a queue repeat),
so switching sources, changing the sort order or restarting brings the same sequence back. Orders are
kept in their own file beside the settings: the settings file is rewritten often (session position),
this one only when an order changes.
"""

from __future__ import annotations

import json
import os
import tempfile
from typing import Optional

from .config import config_dir

SHUFFLES_NAME = "shuffles.json"
MAX_SOURCES = 32            # remembered orders, the least recently drawn are dropped first
MAX_PATHS = 250000          # per order, keeps a corrupt or runaway file from eating memory


def shuffles_path(config_file: Optional[str] = None) -> str:
    """Where the shuffle orders live: beside the settings file."""
    directory = os.path.dirname(os.path.abspath(config_file)) if config_file else config_dir()
    return os.path.join(directory, SHUFFLES_NAME)


def library_key(directory: str) -> str:
    return "library:" + (os.path.normcase(os.path.abspath(directory)) if directory else "")


def playlist_key(name: str) -> str:
    return "playlist:" + str(name or "").strip().casefold()


class ShuffleStore:
    def __init__(self, path: Optional[str] = None):
        self.path = path or shuffles_path()
        self._orders: dict[str, list[str]] = {}     # insertion order == least recent first
        self.error = ""
        try:
            with open(self.path, encoding="utf-8") as handle:
                raw = json.load(handle)
            if not isinstance(raw, dict) or raw.get("version") != 1 \
                    or not isinstance(raw.get("orders"), dict):
                raise ValueError("unknown format")
            for key, paths in raw["orders"].items():
                if isinstance(key, str) and isinstance(paths, list):
                    clean = [p for p in paths[:MAX_PATHS] if isinstance(p, str)]
                    if clean:
                        self._orders[key] = clean
        except FileNotFoundError:
            pass
        except (OSError, ValueError, TypeError) as exc:
            self._orders = {}
            self.error = f"Could not read saved shuffle orders ({exc}), new ones will be drawn"

    def __contains__(self, key: str) -> bool:
        return key in self._orders

    def get(self, key: str) -> list[str]:
        """The saved order for a source ([] when none was drawn yet)."""
        return list(self._orders.get(key, ()))

    def put(self, key: str, paths: list[str]) -> bool:
        paths = [p for p in paths if isinstance(p, str)][:MAX_PATHS]
        if not paths:
            return False
        if self._orders.get(key) == paths:
            return True
        self._orders.pop(key, None)
        self._orders[key] = paths
        while len(self._orders) > MAX_SOURCES:
            self._orders.pop(next(iter(self._orders)))
        return self.save()

    def rename(self, old: str, new: str) -> bool:
        if old == new or old not in self._orders:
            return True
        self._orders[new] = self._orders.pop(old)
        return self.save()

    def remove(self, key: str) -> bool:
        if self._orders.pop(key, None) is None:
            return True
        return self.save()

    def save(self) -> bool:
        tmp = None
        try:
            directory = os.path.dirname(os.path.abspath(self.path))
            os.makedirs(directory, exist_ok=True)
            fd, tmp = tempfile.mkstemp(dir=directory, suffix=".tmp")
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                json.dump({"version": 1, "orders": self._orders}, handle, separators=(",", ":"))
                handle.write("\n")
            os.replace(tmp, self.path)
            tmp = None
            self.error = ""
            return True
        except (OSError, ValueError, TypeError) as exc:
            self.error = f"Could not save the shuffle order: {exc}"
            return False
        finally:
            if tmp is not None:
                try:
                    os.unlink(tmp)
                except OSError:
                    pass
