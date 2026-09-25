"""Save module details beside the settings. Reuse them only while file size and modification time
match."""

from __future__ import annotations

import json
import os
import tempfile
from typing import TYPE_CHECKING, Any, Optional

from .config import config_dir

if TYPE_CHECKING:                       # pragma: no cover - typing only
    from .library import Track

CACHE_NAME = "analysis.json"
CACHE_VERSION = 1
# how many modules are kept (roughly 150 bytes each, so ~7 MB at the cap)
MAX_ENTRIES = 50000


def cache_path() -> str:
    """Where the analysis cache lives (next to the settings)."""
    return os.path.join(config_dir(), CACHE_NAME)


class AnalysisCache:
    """The information the analyzer produced last time, keyed by module path."""

    def __init__(self, path: Optional[str] = None):
        self.path = path or cache_path()
        self.entries: dict[str, dict] = {}
        self.dirty = False              # something was learned since the last save
        self.hits = 0
        self.misses = 0

    def load(self) -> int:
        """Read the file; returns how many records it holds (0 if unusable)."""
        try:
            with open(self.path, "r", encoding="utf-8") as fh:
                raw = json.load(fh)
        except Exception:
            return 0                    # missing, unreadable, half-written: start over
        entries = raw.get("entries") if isinstance(raw, dict) else None
        if not isinstance(entries, dict):
            return 0
        self.entries = {path: entry for path, entry in entries.items()
                        if isinstance(path, str) and isinstance(entry, dict)}
        return len(self.entries)

    def apply(self, track: "Track") -> bool:
        """Fill track from the cache when the record still fits the file."""
        entry = self.entries.get(track.path)
        if not entry or not self._matches(entry, track):
            self.misses += 1
            return False
        track.analyzed = True
        track.duration = self._duration(entry.get("dur"))
        track.fmt = str(entry.get("fmt") or "")
        channels = entry.get("ch")
        track.channels = int(channels) if isinstance(channels, (int, float)) else None
        subsongs = entry.get("sub")
        track.subsongs = int(subsongs) if isinstance(subsongs, (int, float)) else 1
        track.title = str(entry.get("title") or "")
        broken = entry.get("broken")
        track.broken = str(broken) if broken else None
        # touched records move to the end, so the cap drops the coldest entries
        self.entries.pop(track.path, None)
        self.entries[track.path] = entry
        self.hits += 1
        return True

    def remember(self, track: "Track") -> None:
        """Store what is known about track now (called as the analyzer finishes)."""
        entry = {
            "size": int(track.size or 0),
            "mtime": float(track.mtime or 0.0),
            "dur": self._json_duration(track.duration),
            "fmt": track.fmt or "",
            "ch": track.channels,
            "sub": int(track.subsongs or 0),
            "title": track.title or "",
            "broken": track.broken or None,
        }
        if self.entries.get(track.path) != entry:
            self.entries[track.path] = entry
            self.dirty = True

    def forget(self, path: str) -> None:
        if self.entries.pop(path, None) is not None:
            self.dirty = True

    def prune(self, limit: int = MAX_ENTRIES) -> int:
        """Drop the least recently touched records beyond limit."""
        extra = len(self.entries) - limit
        if extra <= 0:
            return 0
        for path in list(self.entries)[:extra]:
            del self.entries[path]
        self.dirty = True
        return extra

    def save(self) -> bool:
        """Write the file (atomically).  Returns False when there is nothing to do."""
        if not self.dirty:
            return False
        self.prune()
        payload = {"version": CACHE_VERSION, "entries": self.entries}
        try:
            directory = os.path.dirname(self.path) or "."
            os.makedirs(directory, exist_ok=True)
            fd, tmp = tempfile.mkstemp(dir=directory, suffix=".tmp")
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                json.dump(payload, fh, ensure_ascii=False, separators=(",", ":"))
            os.replace(tmp, self.path)
        except Exception:
            try:
                os.unlink(tmp)          # noqa: F821 - only bound after mkstemp worked
            except Exception:
                pass
            return False
        self.dirty = False
        return True

    @staticmethod
    def _matches(entry: dict, track: "Track") -> bool:
        try:
            if int(entry.get("size", -1)) != int(track.size or 0):
                return False
            return abs(float(entry.get("mtime", -1.0)) - float(track.mtime or 0.0)) < 1e-6
        except (TypeError, ValueError):
            return False

    @staticmethod
    def _duration(value: Any) -> Optional[float]:
        """Turn a stored duration back into seconds (None unknown, inf endless)."""
        if value == "inf":
            return float("inf")
        if isinstance(value, (int, float)):
            return float(value)
        return None

    @staticmethod
    def _json_duration(value: Optional[float]) -> Any:
        """JSON has no infinity: keep it as the string "inf"."""
        if value is None:
            return None
        try:
            if value != value:                      # NaN
                return None
            if value == float("inf"):
                return "inf"
            return round(float(value), 3)
        except (TypeError, ValueError):
            return None
