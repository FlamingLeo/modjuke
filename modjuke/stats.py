"""Local listening history with elapsed time independent of song position."""
from __future__ import annotations

import json
import math
import os
import tempfile
import time
from dataclasses import asdict, dataclass

from .config import config_dir

SAVE_INTERVAL = 30.0
MAX_GAP = 2.0
MAX_MODULES = 100000
MAX_FILE_BYTES = 64 * 1024 * 1024


def stats_path(config_file=None) -> str:
    folder = os.path.dirname(os.path.abspath(config_file)) if config_file else config_dir()
    return os.path.join(folder, "listening-stats.json")


def path_key(path) -> str:
    return os.path.normcase(os.path.abspath(os.path.expanduser(str(path))))


def _number(value, default=0.0) -> float:
    if isinstance(value, bool):
        return default
    try:
        value = float(value)
        return value if math.isfinite(value) and 0 <= value <= 1e15 else default
    except (TypeError, ValueError, OverflowError):
        return default

@dataclass
class ModuleStats:
    path: str
    title: str = ""
    plays: int = 0
    seconds: float = 0.0
    last_played: float = 0.0  # UTC epoch; displayed in the user's local timezone


class StatsStore:
    def __init__(self, path: str):
        self.path = path
        self.records: dict[str, ModuleStats] = {}
        self.since = time.time()
        self.dirty = False
        self.error = ""
        self._blocked = False
        self._last_attempt = 0.0
        self.revision = 0
        self.load()

    def load(self) -> None:
        try:
            if os.path.getsize(self.path) > MAX_FILE_BYTES:
                raise ValueError("File is too large")
            with open(self.path, encoding="utf-8") as stream:
                data = json.load(stream)
            if not isinstance(data, dict) or data.get("version") != 1 or not isinstance(data.get("modules"), list):
                raise ValueError("Unrecognised statistics format")
            if len(data["modules"]) > MAX_MODULES:
                raise ValueError("Too many modules")
            self.since = _number(data.get("since"), self.since)
            for row in data["modules"]:
                if not isinstance(row, dict) or not isinstance(row.get("path"), str) or not row["path"]:
                    continue
                path = path_key(row["path"])
                if path in self.records:
                    continue
                self.records[path] = ModuleStats(
                    path, str(row.get("title") or "")[:1000], int(_number(row.get("plays"))),
                    _number(row.get("seconds")), _number(row.get("last_played")))
        except FileNotFoundError:
            pass
        except (OSError, ValueError, TypeError) as exc:
            # Preserve unreadable history until an explicit reset succeeds.
            self.error = f"Could not read listening stats: {exc}. Existing file left untouched."
            self._blocked = True

    def record(self, path, seconds=0.0, play=False, title="", when=None) -> bool:
        path = path_key(path)
        if path not in self.records:
            if len(self.records) >= MAX_MODULES:
                self.error = "Listening stats limit reached, existing history is kept."
                return False
            self.records[path] = ModuleStats(path)
        row = self.records[path]
        row.seconds += max(0.0, float(seconds))
        row.plays += int(bool(play))
        row.last_played = time.time() if when is None else float(when)
        if title:
            row.title = str(title)[:1000]
        self.dirty = True
        self.revision += 1
        return True

    def _write(self, records, since) -> None:
        folder = os.path.dirname(os.path.abspath(self.path))
        os.makedirs(folder, exist_ok=True)
        fd, temporary = tempfile.mkstemp(dir=folder, suffix=".tmp")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                json.dump({"version": 1, "since": since,
                           "modules": [asdict(r) for r in records.values()]},
                          stream, ensure_ascii=True, allow_nan=False)
            os.replace(temporary, self.path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    def save(self, force=False, now=None) -> bool:
        if not self.dirty:
            return not self._blocked
        now = time.monotonic() if now is None else now
        if not force and now - self._last_attempt < SAVE_INTERVAL:
            return not self.error
        self._last_attempt = now
        if self._blocked:
            return False
        try:
            self._write(self.records, self.since)
        except (OSError, ValueError) as exc:
            self.error = f"Could not save listening stats: {exc}"
            return False
        self.dirty = False
        self.error = ""
        return True

    def reset(self) -> bool:
        """Only replace in-memory history after the empty file is safely written."""
        since = time.time()
        try:
            self._write({}, since)
        except (OSError, ValueError) as exc:
            self.error = f"Could not reset listening stats: {exc}"
            return False
        self.records = {}
        self.since = since
        self.dirty = self._blocked = False
        self.error = ""
        self.revision += 1
        return True

    def totals(self) -> tuple[float, int, int]:
        return (sum(r.seconds for r in self.records.values()),
                sum(r.plays for r in self.records.values()), len(self.records))

    def ranked(self, order="plays", query="") -> list[ModuleStats]:
        terms = query.casefold().split()
        rows = [r for r in self.records.values()
                if all(t in (r.path + " " + r.title).casefold() for t in terms)]
        field = order if order in ("plays", "seconds", "last_played") else "plays"
        return sorted(rows, key=lambda r: (-getattr(r, field), -r.seconds, r.path.casefold()))


class ListeningCounter:
    """Observe engine snapshots; never use song position as elapsed listening time."""
    def __init__(self, store: StatsStore, enabled: bool = True):
        self.store = store
        self.enabled = bool(enabled)
        self._require_audio = not self.enabled
        self._identity = None
        self._generation = None
        self._audio = 0.0
        self._budget = 0.0
        self._time = None
        self._active = False
        self._counted = False
        self._continue = None

    def set_enabled(self, enabled, snap, source, now=None, when=None) -> bool:
        enabled = bool(enabled)
        if enabled == self.enabled:
            return False
        # Settle the old mode, then discard elapsed time and queued audio at the boundary.
        self.observe(snap, source, now=now, when=when)
        self.enabled = enabled
        self._budget = 0.0
        self._time = None
        self._active = False
        return True

    def continue_output(self, path, play_id=None) -> None:
        """The UI replaced the audio engine, not the user's playback session."""
        key = path_key(path) if path else ""
        same_play = bool(self._identity and self._identity[2] == key
                         and (play_id is None or self._identity[1] == play_id))
        # A new output engine restores its first load with play_id 1.
        pending = bool(self._continue and self._continue[0] == key and self._continue[1]
                       and (play_id is None or play_id == 1))
        self._continue = (key, (self._counted and same_play) or pending) if key else None
        self._identity = None
        self._time = None
        self._active = False
        self._budget = 0.0

    def reset(self) -> None:
        # Keep the render baseline so Reset cannot count old audio again.
        self._budget = 0.0
        self._time = None
        self._counted = False
        self._continue = None

    def observe(self, snap, source, now=None, when=None) -> None:
        now = time.monotonic() if now is None else float(now)
        when = time.time() if when is None else float(when)
        previous_time = self._time
        self._time = now
        if not snap.loaded or not snap.path or snap.failed:
            self._active = False
            self._budget = 0.0
            return
        path = path_key(snap.path)
        identity = (source, snap.play_id, path)
        audio = max(0.0, snap.audio_seconds)
        fresh = identity != self._identity
        generation_changed = snap.generation != self._generation
        if fresh:
            self._require_audio = False
            self._identity = identity
            self._counted = bool(self._continue and self._continue[0] == path and self._continue[1])
            self._continue = None
            self._audio = 0.0
            self._budget = 0.0
        if generation_changed and not fresh:
            self._audio = audio
            self._budget = 0.0
        self._generation = snap.generation
        delta_audio = max(0.0, audio - self._audio)
        self._audio = audio
        if not self.enabled:
            self._budget = 0.0
            self._active = False
            self._require_audio = True
            return
        # EOF may still have queued audio; finished means the ring has drained.
        active = bool((snap.playing or snap.ended) and not snap.paused and not snap.finished)
        elapsed = now - previous_time if previous_time is not None else 0.0
        self._budget = min(MAX_GAP, self._budget + delta_audio)
        seconds = 0.0
        if 0 <= elapsed <= MAX_GAP:
            if active and self._active and not fresh and not generation_changed:
                seconds = min(elapsed, self._budget)
            elif snap.finished and not snap.paused and (fresh or self._active):
                # Also catch modules short enough to finish between UI ticks.
                seconds = min(elapsed, self._budget)
        elif elapsed > MAX_GAP:
            self._budget = 0.0  # do not guess over suspend / a blocked UI
        self._budget -= seconds
        play = (not self._counted and audio > 0 and not snap.paused
                and (not self._require_audio or delta_audio > 0)
                and (snap.playing or snap.ended or snap.finished))
        if play or seconds > 0:
            title = getattr(snap.info, "title", "") if snap.info else ""
            if self.store.record(path, seconds, play, title, when):
                self._counted = True
        if not active:
            # Paused PCM is retained for resume; seeks and loads discard that queue.
            self._budget = min(self._budget, max(0.0, snap.buffer_seconds)) if snap.paused else 0.0
        self._active = active
