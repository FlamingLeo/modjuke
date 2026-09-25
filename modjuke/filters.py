"""Shared queue-filter settings and matching rules."""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Iterable

from .library import Track

@dataclass
class QueueFilter:
    """What the queue leaves out."""

    formats: frozenset[str] = field(default_factory=frozenset)   # empty == all
    min_seconds: float = 0.0        # 0 -> no lower bound
    max_seconds: float = 0.0        # 0 -> no upper bound
    hide_broken: bool = False       # leave out modules that cannot be read

    @property
    def active(self) -> bool:
        return bool(self.formats) or self.min_seconds > 0 or self.max_seconds > 0 \
            or self.hide_broken

    def describe(self) -> str:
        """Short human summary (used in the status line and the readout)."""
        parts = []
        if self.formats:
            parts.append("/".join(sorted(self.formats)))
        if self.min_seconds > 0:
            parts.append(f"≥{_seconds(self.min_seconds)}")
        if self.max_seconds > 0:
            parts.append(f"≤{_seconds(self.max_seconds)}")
        if self.hide_broken:
            parts.append("playable only")
        return ", ".join(parts) or "no filter"

    def matches(self, track: Track) -> bool:
        """Return whether a track passes. Unknown or unreadable durations bypass length limits."""
        if self.hide_broken and track.broken:
            return False
        if self.formats and (track.fmt or "").strip().lower() not in self.formats:
            return False
        if track.duration is None:
            return True                                   # not read yet: keep it
        try:
            duration = float(track.duration)
        except (TypeError, ValueError):
            return True
        if self.min_seconds > 0 and duration < self.min_seconds:
            return False
        if self.max_seconds > 0 and duration > self.max_seconds:
            return False                                  # endless songs are "too long"
        return True

    def select(self, tracks: Iterable[Track]) -> list[Track]:
        if not self.active:
            return list(tracks)
        return [track for track in tracks if self.matches(track)]

    def count(self, tracks: Iterable[Track]) -> int:
        return sum(1 for track in tracks if self.matches(track))


def normalise_formats(values: Iterable[str]) -> frozenset[str]:
    """Lower-case, trimmed, without empties - the form the filter compares against."""
    return frozenset(str(v).strip().lower() for v in values if str(v).strip())


def formats_in(tracks: Iterable[Track]) -> list[tuple[str, int]]:
    """Count library formats, most common first, for the filter dialog."""
    counts: dict[str, int] = {}
    for track in tracks:
        name = (track.fmt or "").strip().lower()
        counts[name] = counts.get(name, 0) + 1
    return sorted(counts.items(), key=lambda item: (-item[1], item[0]))


def unknown_duration_count(tracks: Iterable[Track]) -> int:
    """How many modules have no length yet (the filter cannot judge those)."""
    return sum(1 for track in tracks if track.duration is None)


def _seconds(value: float) -> str:
    value = float(value)
    if value >= 60:
        minutes, seconds = divmod(int(round(value)), 60)
        return f"{minutes}:{seconds:02d}"
    if math.isfinite(value) and abs(value - round(value)) < 0.05:
        return f"{int(round(value))}s"
    return f"{value:.1f}s"


def filter_from_settings(settings) -> QueueFilter:
    """Build a QueueFilter"""
    return QueueFilter(
        formats=normalise_formats(getattr(settings, "filter_formats", []) or []),
        min_seconds=max(0.0, float(getattr(settings, "filter_min", 0.0) or 0.0)),
        max_seconds=max(0.0, float(getattr(settings, "filter_max", 0.0) or 0.0)),
        hide_broken=bool(getattr(settings, "filter_hide_broken", False)),
    )
