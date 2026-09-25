"""
Module library: recursive scanning, queue ordering and (optional) analysis.
"""

from __future__ import annotations

import math
import os
import random
import re
import threading
from dataclasses import dataclass, field
from typing import Callable, Iterable, Optional

from .openmpt import ModuleInfo, get_lib, supported_extensions

# Directory names that never contain tracker modules
SKIP_DIRS = {
    "$RECYCLE.BIN", "System Volume Information", "node_modules", "__pycache__",
    ".git", ".svn", ".hg", ".cache", ".Trash", ".Trash-1000", "AppData",
}

_DIGITS = re.compile(r"(\d+)")


def natural_key(text: str):
    """Case-insensitive natural sort key: 'mod2' < 'mod10'."""
    return tuple(
        int(part) if part.isdigit() else part.casefold() for part in _DIGITS.split(text)
    ) or ("",)

@dataclass
class Track:
    path: str
    rel_dir: str = ""          # directory relative to the library root ("" == root)
    name: str = ""             # file name, e.g. "space debries.mod"
    ext: str = ""
    size: int = 0
    mtime: float = 0.0

    # filled in lazily (when played) or by the analyzer
    analyzed: bool = False
    duration: Optional[float] = None
    fmt: str = ""
    channels: Optional[int] = None
    subsongs: int = 0
    title: str = ""
    broken: Optional[str] = None   # error message if the file failed to load

    @property
    def dir_parts(self) -> tuple[str, ...]:
        return tuple(p for p in self.rel_dir.split(os.sep) if p)

    @property
    def sort_name(self):
        return natural_key(self.name)

    def duration_text(self) -> str:
        if self.duration is None:
            return ""
        if not math.isfinite(self.duration):
            return "∞"
        return format_time(self.duration)

    def sub_label(self) -> str:
        bits = []
        if self.analyzed or self.duration is not None:
            if self.fmt:
                bits.append(self.fmt.upper())
            if self.channels:
                bits.append(f"{self.channels} ch")
        return ", ".join(bits)


def format_time(seconds: Optional[float]) -> str:
    if seconds is None or not math.isfinite(seconds):
        return "--:--"
    seconds = max(0.0, float(seconds))
    m, s = divmod(int(seconds), 60)
    h, m = divmod(m, 60)
    if h:
        return f"{h}:{m:02d}:{s:02d}"
    return f"{m}:{s:02d}"

@dataclass
class ScanResult:
    root: str
    tracks: list[Track] = field(default_factory=list)
    dirs: int = 0
    skipped: int = 0
    errors: list[str] = field(default_factory=list)
    cancelled: bool = False


def scan_library(
    root: str,
    extensions: Optional[Iterable[str]] = None,
    progress: Optional[Callable[[int, str], None]] = None,
    cancel: Optional[threading.Event] = None,
    skip_hidden: bool = True,
    max_depth: int = 32,
) -> ScanResult:
    """Walk root recursively and collect every playable module file."""
    exts = {e.lower().lstrip(".") for e in (extensions or supported_extensions())}
    result = ScanResult(root=root)
    root = os.path.abspath(os.path.expanduser(root))
    stack: list[tuple[str, int]] = [(root, 0)]
    seen: set[tuple[int, int]] = set()

    while stack:
        if cancel is not None and cancel.is_set():
            result.cancelled = True
            return result
        dirpath, depth = stack.pop()
        if depth > max_depth:
            continue
        try:
            st = os.stat(dirpath)
            key = (st.st_dev, st.st_ino)
            if key in seen:      # symlink loop protection
                continue
            seen.add(key)
            entries = list(os.scandir(dirpath))
        except OSError as exc:
            result.errors.append(f"{dirpath}: {exc}")
            continue
        result.dirs += 1
        if progress:
            progress(len(result.tracks), dirpath)
        for entry in entries:
            name = entry.name
            if skip_hidden and name.startswith("."):
                continue
            try:
                if entry.is_dir(follow_symlinks=False):
                    if name in SKIP_DIRS:
                        continue
                    stack.append((entry.path, depth + 1))
                elif entry.is_file(follow_symlinks=True):
                    ext = os.path.splitext(name)[1].lstrip(".").lower()
                    if ext not in exts:
                        continue
                    try:
                        st = entry.stat()
                        size, mtime = st.st_size, st.st_mtime
                    except OSError:
                        size, mtime = 0, 0.0
                    rel = os.path.relpath(entry.path, root)
                    rel_dir = os.path.dirname(rel)
                    if rel_dir == ".":
                        rel_dir = ""
                    result.tracks.append(
                        Track(
                            path=entry.path, rel_dir=rel_dir, name=name, ext=ext,
                            size=size, mtime=mtime,
                        )
                    )
                else:
                    result.skipped += 1
            except OSError as exc:
                result.errors.append(f"{entry.path}: {exc}")
    return result

ORDER_ALPHABETICAL = "alphabetical"
ORDER_DIRECTORY = "by directory"
ORDER_SHUFFLE = "shuffle"
ORDER_MODES = (ORDER_ALPHABETICAL, ORDER_DIRECTORY, ORDER_SHUFFLE)

ORDER_PLAYLIST = "playlist"


def _alphabetical_key(track: "Track"):
    return (track.sort_name, natural_key(track.rel_dir), track.path.casefold())


def order_tracks(tracks: Iterable[Track], mode: str, rng: Optional[random.Random] = None) -> list[Track]:
    items = list(tracks)
    if mode == ORDER_ALPHABETICAL:
        return sorted(items, key=_alphabetical_key)
    if mode == ORDER_DIRECTORY:
        return sorted(
            items,
            key=lambda t: (
                tuple(natural_key(p) for p in t.dir_parts),
                t.sort_name,
                t.path.casefold(),
            ),
        )
    if mode == ORDER_SHUFFLE:
        items = sorted(items, key=_alphabetical_key)
        rng = rng or random.SystemRandom()
        rng.shuffle(items)
        return items
    return items


def shuffle_order(
    tracks: Iterable[Track],
    seed: Optional[int] = None,
    first: Optional[str] = None,
    avoid_first: Optional[str] = None,
) -> list[Track]:
    """Shuffle reproducibly with a seed. Optionally lead with first or avoid starting with
    avoid_first."""
    items = sorted(list(tracks), key=_alphabetical_key)
    rng = random.Random(seed) if seed is not None else random.SystemRandom()
    rng.shuffle(items)
    if avoid_first and len(items) > 1 and items[0].path == avoid_first:
        swap = rng.randrange(1, len(items))
        items[0], items[swap] = items[swap], items[0]
    if first:
        index = next((i for i, t in enumerate(items) if t.path == first), None)
        if index:                      # None -> not in this queue, 0 -> already first
            items.insert(0, items.pop(index))
    return items


def apply_path_order(tracks: Iterable[Track], path_order: Iterable[str]) -> list[Track]:
    """Apply saved path order, adding unlisted tracks alphabetically at the end."""
    rank = {path: i for i, path in enumerate(path_order)}
    items = list(tracks)
    known = sorted((t for t in items if t.path in rank), key=lambda t: rank[t.path])
    unknown = sorted((t for t in items if t.path not in rank), key=_alphabetical_key)
    return known + unknown


def rotate_to_front(path_order: list[str], path: str) -> list[str]:
    """Rotate the saved order to start with path, preserving the remaining sequence."""
    if path in path_order:
        index = path_order.index(path)
        if index:
            return path_order[index:] + path_order[:index]
    return list(path_order)


def search_filter(tracks: Iterable[Track], needle: str) -> list[Track]:
    needle = (needle or "").strip().casefold()
    if not needle:
        return list(tracks)
    terms = [t for t in needle.split() if t]
    out = []
    for track in tracks:
        hay = f"{track.rel_dir}{os.sep}{track.name} {track.fmt} {track.title}".casefold()
        if all(term in hay for term in terms):
            out.append(track)
    return out


class Analyzer(threading.Thread):
    """Read module details in a cancellable background thread."""

    def __init__(
        self,
        tracks: list[Track],
        on_result: Callable[[Track, Optional[ModuleInfo], Optional[str]], None],
        on_done: Callable[[int, int], None],
        cancel: Optional[threading.Event] = None,
    ):
        super().__init__(name="analyzer", daemon=True)
        self.tracks = list(tracks)
        self.on_result = on_result
        self.on_done = on_done
        self.cancel = cancel or threading.Event()
        self.done = 0
        self.failed = 0

    def run(self) -> None:
        lib = get_lib()
        ctls = {
            "load.skip_samples": "1",
            "load.skip_plugins": "1",
            "load.skip_subsongs_init": "1",
        }
        for track in self.tracks:
            if self.cancel.is_set():
                break
            info = None
            error = None
            try:
                with lib.open_file(track.path, ctls=ctls) as mod:
                    info = mod.info(track.path)
                    try:
                        mod.set_at_end("stop")
                    except Exception:
                        pass
            except Exception as exc:
                error = str(exc)
            self.done += 1
            if error:
                self.failed += 1
            try:
                self.on_result(track, info, error)
            except Exception:
                pass
        try:
            self.on_done(self.done, self.failed)
        except Exception:
            pass
