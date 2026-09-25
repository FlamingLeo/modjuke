"""Named playlists stored beside settings, with ordered paths and M3U import/export."""

from __future__ import annotations

import json
import os
import tempfile
from dataclasses import dataclass, field
from functools import wraps
from typing import Iterable, Optional

from .library import Track

from .config import config_dir

PLAYLISTS_NAME = "playlists.json"
NAME_MAX = 80            # a playlist name is a label, not a file name
MAX_PLAYLISTS = 500      # plenty; the file is meant to stay openable at a glance
MAX_TRACKS = 100000      # one playlist cannot grow past a sane queue


def playlists_path(config_file: Optional[str] = None) -> str:
    """Where the playlist file lives: beside the settings file."""
    if config_file:
        return os.path.join(os.path.dirname(os.path.abspath(config_file)),
                            PLAYLISTS_NAME)
    return os.path.join(config_dir(), PLAYLISTS_NAME)


class PlaylistError(ValueError):
    """A playlist name or operation the store refuses (the UI shows it)."""


def normalise_name(name) -> str:
    """The playlist name behind whatever text the user typed - or raise."""
    text = str(name or "").strip()
    if not text:
        raise PlaylistError("A playlist needs a name")
    if len(text) > NAME_MAX:
        raise PlaylistError(f"A name is {NAME_MAX} characters at most")
    if "/" in text or "\\" in text:
        raise PlaylistError("A playlist name cannot contain a slash")
    if text in {".", ".."}:
        raise PlaylistError("That name is reserved")
    return text


def _unique(paths: Iterable[str]) -> list[str]:
    """The paths in order, duplicates dropped (a hand-edited file may have them)."""
    seen: set[str] = set()
    out: list[str] = []
    for path in paths:
        text = str(path)
        if text and text not in seen:
            seen.add(text)
            out.append(text)
    return out

@dataclass
class Playlist:
    name: str
    paths: list[str] = field(default_factory=list)
    root: str = ""                 # the library it was made from (informational)


def _transaction(method):
    """A failed atomic save must leave the in-memory collection unchanged too."""
    @wraps(method)
    def call(self, *args, **kwargs):
        before = {k: Playlist(p.name, list(p.paths), p.root)
                  for k, p in self.playlists.items()}
        try:
            return method(self, *args, **kwargs)
        except PlaylistError:
            self.playlists = before
            raise
    return call


class PlaylistStore:
    """The playlist file: a small ordered set of named, ordered path lists."""

    def __init__(self, path: str):
        self.path = path
        self.playlists: dict[str, Playlist] = {}     # keyed by casefolded name
        self._external_tracks: dict[str, Track] = {}
        self.load()

    @staticmethod
    def _key(name: str) -> str:
        return name.casefold()

    def load(self) -> None:
        """Reload playlists, tolerating missing files and damaged entries."""
        self.playlists = {}
        try:
            with open(self.path, "r", encoding="utf-8") as fh:
                raw = json.load(fh)
        except Exception:
            return
        items = raw.get("playlists") if isinstance(raw, dict) else None
        if not isinstance(items, list):
            return
        for entry in items:
            try:
                if not isinstance(entry, dict):
                    continue
                name = normalise_name(entry.get("name", ""))
                paths = _unique(entry.get("paths") or [])
                root = str(entry.get("root") or "")
            except Exception:
                continue
            if len(paths) > MAX_TRACKS:
                paths = paths[:MAX_TRACKS]
            if self._key(name) in self.playlists:
                continue            # a duplicate in a broken file: keep the first
            self.playlists[self._key(name)] = Playlist(name, paths, root)

    def save(self) -> None:
        tmp = None
        try:
            folder = os.path.dirname(os.path.abspath(self.path))
            os.makedirs(folder, exist_ok=True)
            fd, tmp = tempfile.mkstemp(dir=folder, suffix=".tmp")
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                json.dump({"playlists": [
                    {"name": p.name, "root": p.root, "paths": p.paths}
                    for p in self.playlists.values()
                ]}, fh, indent=2)
            os.replace(tmp, self.path)
        except OSError as exc:
            raise PlaylistError(f"Could not save playlists: {exc}") from exc
        finally:
            if tmp and os.path.exists(tmp):
                try:
                    os.unlink(tmp)
                except OSError:
                    pass

    def names(self) -> list[str]:
        return [p.name for p in self.playlists.values()]

    def get(self, name: str) -> Optional[Playlist]:
        return self.playlists.get(self._key(name))

    @_transaction
    def add(self, name, paths: Iterable[str], root: str = "") -> Playlist:
        name = normalise_name(name)
        if self._key(name) in self.playlists:
            raise PlaylistError(f"A playlist called '{name}' already exists")
        if len(self.playlists) >= MAX_PLAYLISTS:
            raise PlaylistError(f"{MAX_PLAYLISTS} playlists is the limit")
        paths = _unique(paths)
        if len(paths) > MAX_TRACKS:
            raise PlaylistError(f"{MAX_TRACKS} tracks is the playlist limit")
        playlist = Playlist(name, paths, root or "")
        self.playlists[self._key(name)] = playlist
        self.save()
        return playlist

    @_transaction
    def replace(self, name, paths: Iterable[str], root: str = "") -> Playlist:
        """A playlist's new contents and order (the 'Save order' action)."""
        playlist = self.get(name)
        if playlist is None:
            raise PlaylistError(f"No playlist called '{name}'")
        paths = _unique(paths)
        if len(paths) > MAX_TRACKS:
            raise PlaylistError(f"{MAX_TRACKS} tracks is the playlist limit")
        playlist.paths = paths
        if root:
            playlist.root = root
        self.save()
        return playlist

    @_transaction
    def rename(self, old, new) -> Playlist:
        old_key = self._key(normalise_name(old))
        if old_key not in self.playlists:
            raise PlaylistError(f"No playlist called '{old}'")
        new = normalise_name(new)
        new_key = self._key(new)
        if new_key in self.playlists:
            raise PlaylistError(f"A playlist called '{new}' already exists")
        # rebuild in place: the renamed playlist keeps its position in the list
        rebuilt: dict[str, Playlist] = {}
        for key, playlist in self.playlists.items():
            if key == old_key:
                playlist.name = new
                rebuilt[new_key] = playlist
            else:
                rebuilt[key] = playlist
        self.playlists = rebuilt
        self.save()
        return self.playlists[new_key]

    @_transaction
    def remove(self, name) -> None:
        key = self._key(normalise_name(name))
        if key not in self.playlists:
            raise PlaylistError(f"No playlist called '{name}'")
        del self.playlists[key]
        self.save()

    def resolve(self, name, tracks) -> tuple[list, list[str]]:
        """Return tracks in saved path order, plus the missing paths."""
        playlist = self.get(name)
        if playlist is None:
            return [], []
        by_path = {t.path: t for t in tracks}
        found: list = []
        missing: list[str] = []
        seen: set[str] = set()
        for path in playlist.paths:
            if path in seen:
                continue
            seen.add(path)
            track = by_path.get(path)
            if track is None:
                try:
                    stat = os.stat(path)
                    if not os.path.isfile(path):
                        raise OSError("Not a file")
                except OSError:
                    self._external_tracks.pop(path, None)
                    missing.append(path)
                    continue
                track = self._external_tracks.get(path)
                if track is None or (track.size, track.mtime) != (stat.st_size, stat.st_mtime):
                    track = Track(path=path, name=os.path.basename(path),
                                  rel_dir=os.path.dirname(path),
                                  ext=os.path.splitext(path)[1].lstrip(".").lower(),
                                  size=stat.st_size, mtime=stat.st_mtime)
                    self._external_tracks[path] = track
            found.append(track)
        return found, missing


def write_m3u(paths: Iterable[str], path: str) -> int:
    """Write an M3U file (absolute paths, one per line).  Returns the count."""
    lines = ["#EXTM3U"]
    count = 0
    for entry in paths:
        text = str(entry).strip()
        if not text:
            continue
        lines.append(text)
        count += 1
    try:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("\r\n".join(lines) + "\r\n")
    except OSError:
        return 0
    return count


def read_m3u(path: str) -> tuple[list[str], int]:
    """Return (existing paths, skipped count). Resolve relative entries beside the M3U file."""
    base = os.path.dirname(os.path.abspath(path))
    out: list[str] = []
    seen: set[str] = set()
    skipped = 0
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    except OSError:
        return [], 0
    for line in lines:
        entry = line.strip()
        if not entry or entry.startswith("#"):
            continue
        if len(entry) >= 2 and entry[0] == entry[-1] and entry[0] in "\"'":
            entry = entry[1:-1].strip()
        if not entry:
            continue
        if not os.path.isabs(entry):
            entry = os.path.join(base, entry)
        entry = os.path.normpath(os.path.expanduser(entry))
        if entry in seen:
            continue
        seen.add(entry)
        if os.path.isfile(entry):
            out.append(entry)
        else:
            skipped += 1
    return out, skipped
