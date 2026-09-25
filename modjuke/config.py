"""Tiny JSON backed settings store (no external dependency)."""

from __future__ import annotations

import json
import os
import platform
import tempfile
from dataclasses import asdict, dataclass, field, fields

from . import theme

SAMPLE_RATE_CHOICES = (0, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000)
SAMPLE_RATE_LABELS = {rate: ("Device default" if rate == 0 else f"{rate / 1000:g} kHz")
                      for rate in SAMPLE_RATE_CHOICES}
SAMPLE_RATE_VALUES = {label: rate for rate, label in SAMPLE_RATE_LABELS.items()}


def normalise_sample_rate(value) -> int:
    """Unknown/corrupt stored rates use the safe default, never a huge buffer."""
    if isinstance(value, bool):
        return 0
    try:
        rate = int(value)
        if float(value) != rate:
            return 0
    except (TypeError, ValueError, OverflowError):
        return 0
    return rate if rate in SAMPLE_RATE_CHOICES else 0

# the update rates the Interface setting offers (per second)
UI_FPS_CHOICES = (60, 30, 20, 12)
# ...as the Settings dialog shows them
UI_RATE_LABELS = {60: "60 / s (smoothest)", 30: "30 / s (smooth)",
                  20: "20 / s (balanced)", 12: "12 / s (lightest)"}
UI_RATE_VALUES = {label: fps for fps, label in UI_RATE_LABELS.items()}
UI_FPS_DEFAULT = 60
UI_FPS_MIN, UI_FPS_MAX = 5, 120


def clamp_ui_fps(value) -> int:
    """A sane refresh rate from whatever is in the settings file."""
    try:
        number = int(round(float(value)))
    except (TypeError, ValueError):
        return UI_FPS_DEFAULT
    return max(UI_FPS_MIN, min(number, UI_FPS_MAX))
from typing import Any

APP_NAME = "modjuke"

# how many paths of a drawn shuffle order are remembered across restarts
MAX_SHUFFLE_PATHS = 5000
APP_TITLE = f"{APP_NAME}"


def config_dir() -> str:
    if platform.system() == "Windows":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
        return os.path.join(base, APP_NAME)
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, APP_NAME)


def config_path() -> str:
    return os.path.join(config_dir(), "config.json")

@dataclass
class Settings:
    # library
    last_directory: str = ""
    last_picker_dir: str = ""        # where the folder picker opens next time
    folder_picker: str = "built-in"   # built-in (dark browser) | system (desktop picker)

    ui_fps: int = 60
    smooth_tracker_scrolling: bool = False

    window_title: str = "track"
    queue_mode: str = "by directory"
    active_playlist: str = ""
    filter_formats: list = field(default_factory=list)
    filter_min: float = 0.0
    filter_max: float = 0.0
    filter_hide_broken: bool = False
    shuffle_seed: int = 0
    shuffle_paths: list = field(default_factory=list)
    # playback
    volume: int = 80                 # 0..100
    muted: bool = False
    loop_track: bool = False
    loop_queue: bool = False
    auto_advance: bool = True
    subsong: int = 0
    # audio
    backend: str = "auto"            # auto | sounddevice | soundcard | null
    samplerate: int = 0              # 0 == device default
    buffer_ms: int = 220
    latency_ms: int = 20
    latency_revision: int = 1
    interpolation: str = "sinc"
    # safety / robustness
    stall_timeout: float = 45.0      # seconds of "no progress" before a warning
    silence_stall_timeout: float = 25.0
    hang_timeout: float = 8.0        # wall clock stall of the render thread
    max_restarts: int = 3            # per track before it is marked unplayable
    auto_skip_broken: bool = True
    overrun_guard: bool = True
    # ui
    window_geometry: str = ""
    show_metadata: bool = True
    theme: str = theme.DEFAULT_THEME
    remember_position: bool = True
    cache_analysis: bool = True
    auto_analyze: bool = True
    track_listening_stats: bool = True
    last_path: str = ""
    last_position: float = 0.0

    @classmethod
    def load(cls, path: str | None = None) -> "Settings":
        path = path or config_path()
        try:
            with open(path, "r", encoding="utf-8") as fh:
                raw = json.load(fh)
        except Exception:
            return cls()
        if not isinstance(raw, dict):
            return cls()
        known = {f.name for f in fields(cls)}
        clean: dict[str, Any] = {}
        for key, value in (raw or {}).items():
            if key == "shuffle_paths":
                if isinstance(value, list):
                    clean[key] = [str(v) for v in value][:MAX_SHUFFLE_PATHS]
                continue
            if key == "filter_formats":
                if isinstance(value, list):
                    clean[key] = [str(v).strip().lower() for v in value if str(v).strip()]
                continue
            if key == "samplerate":
                clean[key] = normalise_sample_rate(value)
                continue
            if key == "ui_fps":
                clean[key] = clamp_ui_fps(value)
                continue
            if key == "theme":
                clean[key] = theme.normalise(value)
                continue
            if key in known:
                default = getattr(cls(), key)
                try:
                    clean[key] = type(default)(value) if isinstance(default, (int, float, str)) else value
                except Exception:
                    continue
        # Upgrade the old default once; subsequent user latency choices stay untouched.
        if "latency_revision" not in raw and clean.get("latency_ms") == 120:
            clean["latency_ms"] = 20
        return cls(**clean)

    def save(self, path: str | None = None) -> None:
        path = path or config_path()
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), suffix=".tmp")
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                json.dump(asdict(self), fh, indent=2, sort_keys=True)
            os.replace(tmp, path)
        except Exception:
            pass
