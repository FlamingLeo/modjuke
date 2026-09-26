"""Canvas tracker view with output-timed highlighting, continuous rows, and cached pattern data
from the render worker."""

from __future__ import annotations

from math import ceil
from time import monotonic

import tkinter as tk
from tkinter import font as tkfont
from tkinter import ttk
from typing import Callable, Optional

from .theme import ACCENT_DIM as ACCENT_DIM
from .theme import (TRACKER_PLAYING, BG_ALT, BG_INPUT, EFFECT_GLOBAL, EFFECT_MISC,
                    EFFECT_PAN, EFFECT_PITCH, EFFECT_VOLUME, FG_DIM, FG_FAINT,
                    ON_ACCENT, SEP, TRACKER_BEAT, TRACKER_BRIGHT, TRACKER_DIM,
                    TRACKER_FAINT)
from . import effects

# how many rows one notch of the wheel moves the view
SCROLL_STEP = 3

# characters per column (tracker convention: "C-5", "01", "v64", "A0F")
NOTE_CHARS = 3
INSTRUMENT_CHARS = 2
VOLUME_CHARS = 3
EFFECT_CHARS = 3
COLUMN_GAP = 1          # a space between two columns of the same channel
CHANNEL_SEPARATOR = 1   # the │ between two channels
GUTTER_CHARS = 4        # "00 │"
EMPTY_NOTE = "..."
EMPTY_INSTRUMENT = ".."
EMPTY_VOLUME = "..."
EMPTY_EFFECT = "..."
BEAT_ROWS = 4           # every 4th row is tinted (and the row numbers brighten)

LAYOUTS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("full", ("note", "instrument", "volume", "effect")),
    ("no-sample", ("note", "volume", "effect")),
    ("shared", ("note", "shared")),
    ("notes", ("note",)),
)
LAYOUT_PAD = {"full": 2, "no-sample": 2, "shared": 1, "notes": 0}
COLUMN_CHARS = {
    "note": NOTE_CHARS,
    "instrument": INSTRUMENT_CHARS,
    "volume": VOLUME_CHARS,
    "effect": EFFECT_CHARS,
    "shared": max(VOLUME_CHARS, EFFECT_CHARS),
}


def layout_width(columns: tuple[str, ...]) -> int:
    """Characters one channel occupies in a layout (without the separator)."""
    return sum(COLUMN_CHARS[name] for name in columns) + COLUMN_GAP * (len(columns) - 1)


def channel_width(name: str, columns: tuple[str, ...]) -> int:
    """Characters one channel occupies in the grid, separator included."""
    return layout_width(columns) + LAYOUT_PAD[name] + CHANNEL_SEPARATOR


def choose_layout(available_chars: int, channels: int) -> tuple[tuple[str, ...], str, int]:
    """Prefer the widest layout that fits every channel, otherwise show a scrollable notes-only
    channel window."""
    available = max(1, int(available_chars))
    total = max(1, int(channels))
    for name, columns in LAYOUTS:
        if channel_width(name, columns) * total <= available:
            return columns, name, total
    name, columns = LAYOUTS[-1]
    fits = max(1, available // channel_width(name, columns))
    return columns, name, min(int(fits), total)


def shared_cell(volume: str, effect: str) -> tuple[str, str]:
    """Return the effect when present, otherwise volume, along with its column kind."""
    if effect:
        return effect, "effect"
    if volume:
        return volume, "volume"
    return "", ""


def build_lines(orders, rows_of) -> tuple[list[tuple], dict[int, int]]:
    """Flatten orders into rows, skipping negative pattern numbers and retaining placeholders for
    empty patterns."""
    lines: list[tuple] = []
    block_start: dict[int, int] = {}
    for index, pattern in enumerate(orders):
        block_start[index] = len(lines)
        if pattern < 0:                       # a skip: no rows, nothing drawn
            continue
        rows = rows_of.get(pattern, 0)
        if rows <= 0:
            lines.append(("empty", index, 0))
            continue
        for row in range(rows):
            lines.append(("row", index, row))
    return lines, block_start


def center_top(line: int, visible: int, total: int) -> int:
    """Center the playing line where possible, clamping the view to the song."""
    visible = max(1, int(visible))
    total = int(total)
    if total <= visible:
        return 0
    return max(0, min(int(line) - visible // 2, total - visible))


def center_top_float(line: float, visible: int, total: int) -> float:
    """Float version for smooth interpolation between rows."""
    visible = max(1, int(visible))
    total = int(total)
    if total <= visible:
        return 0.0
    return max(0.0, min(float(line) - visible / 2.0, float(total - visible)))


class TrackerView(ttk.Frame):
    """The pattern grid (canvas-drawn, only the visible lines are built)."""

    def __init__(self, master, request_pattern: Optional[Callable[[int], None]] = None,
                 smooth_scrolling: bool = False, **kw):
        super().__init__(master, **kw)
        self.request_pattern = request_pattern
        self.mono = tkfont.Font(font=("TkFixedFont",))
        try:                                   # a couple of points smaller than default
            size = max(7, int(self.mono.cget("size")) - 1)
            self.mono = tkfont.Font(font=("TkFixedFont", size))
        except Exception:                      # pragma: no cover - odd font setups
            pass
        self.char_w = max(4, self.mono.measure("0"))
        self.line_h = max(10, self.mono.metrics("linespace") + 1)

        self.orders: list[int] = []            # pattern index per order (-1: skip)
        self.rows_of: dict[int, int] = {}      # pattern -> its row count
        self.patterns: dict[int, list] = {}    # pattern -> rows of cells
        self.known: set[int] = set()           # patterns the worker answered for
        self.errors: dict[int, str] = {}       # pattern -> why it could not be read
        self.pending: set[int] = set()         # requested, not answered yet
        self.channels = 0
        self.path = ""

        self.current_order = -1
        self.current_row = 0
        self.playing = False

        self.follow = True
        self.smooth_scrolling = bool(smooth_scrolling)
        self.visible = True                    # the tracker tab may be hidden
        self.top = 0                           # whole-row follow / manual scroll target
        self._view_top = 0.0                    # fractional line currently drawn
        self._target_top = 0
        self._glide = None
        self._hidden_glide = None
        self._row_time = None
        self._smooth_line: float | None = None
        self._smooth_interval: float | None = None
        self._last_tick = None
        self.channel_offset = 0
        self.lines: list[tuple] = []           # ("row", order, row) / ("empty", order, 0)
        self.block_start: dict[int, int] = {}  # order -> line index of its first row line
        self.family = effects.family_of("")
        self.columns: tuple[str, ...] = ("note",)
        self.layout_name = "notes"
        self.channel_pad = LAYOUT_PAD["notes"]
        self.shown_channels = 0
        self._line_items: dict[int, list[int]] = {}
        self._waiting_top = None
        self._have_frame = False
        self._paint_top = 0.0

        self._build()
        self.bind("<Configure>", lambda _e: self._on_resize())
        self.canvas.bind("<Configure>", lambda _e: self._on_resize())
        self.bind("<Prior>", lambda _e: self._scroll(-(self._visible_lines() - 1)))
        self.bind("<Next>", lambda _e: self._scroll(self._visible_lines() - 1))
        self.bind("<Home>", lambda _e: self._jump_to_start())
        self.bind("<f>", lambda _e: self._toggle_follow())
        for widget in (self, self.canvas, self.header_canvas):
            widget.bind("<MouseWheel>", self._on_wheel)
            widget.bind("<Button-4>", lambda _e: self._scroll(-SCROLL_STEP))
            widget.bind("<Button-5>", lambda _e: self._scroll(SCROLL_STEP))
            widget.bind("<Shift-MouseWheel>", self._on_wheel_h)
            widget.bind("<Shift-Button-4>", lambda _e: self._scroll_channels(-1))
            widget.bind("<Shift-Button-5>", lambda _e: self._scroll_channels(1))

    def _build(self) -> None:
        strip = ttk.Frame(self, style="Bar.TFrame", padding=(6, 3))
        strip.pack(side="top", fill="x")
        self.state_label = ttk.Label(strip, text="No module loaded", style="Dim.TLabel")
        self.state_label.pack(side="left")
        self.channel_btn_right = ttk.Button(strip, text="›", width=2, style="Mini.TButton",
                                            command=lambda: self._scroll_channels(1))
        self.channel_btn_right.pack(side="right")
        self.channel_label = ttk.Label(strip, text="", style="Dim.TLabel")
        self.channel_label.pack(side="right", padx=4)
        self.channel_btn_left = ttk.Button(strip, text="‹", width=2, style="Mini.TButton",
                                           command=lambda: self._scroll_channels(-1))
        self.channel_btn_left.pack(side="right")
        self.follow_btn = ttk.Button(strip, text="Following" if self.follow else "Follow",
                                     width=9,
                                     style="FollowOn.TButton" if self.follow else "Follow.TButton",
                                     command=self._toggle_follow)
        self.follow_btn.pack(side="right", padx=(0, 10))

        self.header_canvas = tk.Canvas(self, height=self.line_h, bg=BG_ALT,
                                       highlightthickness=0, bd=0)
        self.header_canvas.pack(side="top", fill="x")
        self.canvas = tk.Canvas(self, bg=BG_INPUT, highlightthickness=0, bd=0)
        self.canvas.pack(side="top", fill="both", expand=True)

    def set_song(self, orders: list[int], rows: dict[int, int], channels: int,
                 path: str = "", format_name: str = "") -> None:
        """The song's order list and pattern lengths (from the render worker)."""
        self.family = effects.family_of(format_name)
        self.orders = [int(o) for o in orders]
        self.rows_of = {int(k): int(v) for k, v in rows.items()}
        self.channels = int(channels or 0)
        self.path = path
        self.patterns.clear()
        self.known.clear()
        self.errors.clear()
        self.pending.clear()
        self._waiting_top = None
        self._have_frame = False
        self.channel_offset = 0
        self.follow = True
        self._rebuild_lines()
        self.top = 0
        self._snap_scroll()
        if self.follow:
            self._scroll_to_playing(force=True)
        self.redraw()

    def add_pattern(self, pattern: int, cells: list, error: str = "") -> None:
        """Cache pattern data or its error so failed requests are not repeated."""
        pattern = int(pattern)
        self.pending.discard(pattern)
        self.known.add(pattern)
        if error:
            self.errors[pattern] = error
        else:
            self.patterns.pop(pattern, None)
            self.patterns[pattern] = list(cells) if cells else []
        if not self.visible:
            return
        if self._waiting_top is not None:
            if not self._missing_patterns(self._waiting_top):
                self._waiting_top = None
                line = self._line_of_playback()
                if line is not None:
                    self.top = center_top(line, self._visible_lines(), len(self.lines))
                    row_time = self._row_time
                    self._snap_scroll()
                    self._row_time = row_time
                self.redraw()
            else:
                self._update_labels()
        elif any(self.orders[self.lines[i][1]] == pattern for i in self._line_items):
            self.redraw()
        else:
            self._update_labels()

    def clear(self) -> None:
        """No module (stop, or a fresh start)."""
        self.orders = []
        self.rows_of = {}
        self.patterns = {}
        self.known = set()
        self.errors = {}
        self.pending = set()
        self._waiting_top = None
        self._have_frame = False
        self.channels = 0
        self.path = ""
        self.current_order = -1
        self.current_row = 0
        self.playing = False
        self.lines = []
        self.block_start = {}
        self.top = 0
        self._snap_scroll()
        self.channel_offset = 0
        self._rebuild_lines()
        self.redraw()

    def set_position(self, order: int, row: int, playing: bool = False) -> None:
        """Highlight the audible row immediately, glide only the viewport between updates."""
        hidden_follow = not self.visible and self.smooth_scrolling and self.follow
        if hidden_follow:
            self._glide, self._hidden_glide = self._hidden_glide, None
        now = monotonic()
        timely = self._last_tick is not None and 0 <= now - self._last_tick <= 0.5
        self._last_tick = now
        order = int(order or 0)
        row = int(row or 0)
        playing = bool(playing)
        old_line = self._line_of_playback()
        was_playing = self.playing
        moved = (order, row) != (self.current_order, self.current_row)
        if (self.follow and (self.visible or hidden_follow) and playing and was_playing
                and old_line is not None and order in self.block_start):
            if self.visible or moved:
                self._advance_scroll(now)
        else:
            self._glide = None
            self._row_time = None
        self.current_order = order
        self.current_row = row
        self.playing = playing
        if moved:
            line = self._line_of_playback()
            if self.follow and line is not None:
                interval = now - self._row_time if self._row_time is not None else 0.12
                adjacent = old_line is not None and 0 < line - old_line <= 4
                animate = (playing and was_playing and (self.visible or hidden_follow)
                           and adjacent and timely
                           and self._row_time is not None and interval > 0)
                # Add a tiny overlap (one UI tick) so the previous glide is
                # still in flight when the next row is observed. This hides
                # the quantisation gap where the old glide has finished at
                # fraction 1.0 but the next row hasn't yet been seen (up to
                # ~16 ms).
                duration = max(0.025, interval + 0.016)
                self._follow_scroll(line, animate=animate, now=now,
                                    duration=duration,
                                    adjacent=adjacent)
            self._row_time = now if playing and self.follow and (self.visible or hidden_follow) else None
            if self.visible:
                self._redraw_playing_lines()
        elif playing and self._row_time is None and self.follow and (self.visible or hidden_follow):
            self._row_time = now
        if hidden_follow:
            self._hidden_glide, self._glide = self._glide, None
        self._request_neighbours()

    def set_smooth_scrolling(self, enabled: bool) -> None:
        enabled = bool(enabled)
        if enabled == self.smooth_scrolling:
            return
        self.smooth_scrolling = enabled
        self._hidden_glide = None
        self._glide = None
        self._smooth_line = None
        self._smooth_interval = None
        self._row_time = None
        if not enabled and self.follow:
            if self.visible:
                self._move_view(float(self.top))
            else:
                self._snap_scroll()

    def set_visible(self, visible: bool) -> None:
        """Retain smooth-follow phase across tabs without drawing hidden frames."""
        now = monotonic()
        continuing = (self.smooth_scrolling and self.follow and self.playing
                      and self._last_tick is not None and 0 <= now - self._last_tick <= 0.5)
        if continuing:
            motion = self._glide if self.visible else self._hidden_glide
            if visible:
                self._glide, self._hidden_glide = motion, None
                self._advance_scroll(now)
            else:
                self._hidden_glide, self._glide = motion, None
        else:
            self._glide = self._hidden_glide = None
            self._row_time = None
        self.visible = bool(visible)
        if not self.visible:
            self._waiting_top = None
        if self.visible:
            if self.follow and not continuing:
                self._scroll_to_playing(force=True)
            self.redraw()

    def _rebuild_lines(self) -> None:
        """Flatten the order list into the lines the canvas draws."""
        self.lines, self.block_start = build_lines(self.orders, self.rows_of)

    def _visible_lines(self) -> int:
        try:
            return max(1, int(self.canvas.winfo_height() // self.line_h))
        except tk.TclError:
            return 1

    def _available_chars(self) -> int:
        try:
            width = self.canvas.winfo_width()
        except tk.TclError:
            return 40
        return max(8, (width - 8) // self.char_w)

    def _choose_layout(self) -> tuple[tuple[str, ...], str, int]:
        """Which columns and how many channels fit."""
        return choose_layout(self._available_chars() - GUTTER_CHARS, self.channels)

    def _clamp_channel_offset(self) -> None:
        highest = max(0, self.channels - self.shown_channels)
        self.channel_offset = max(0, min(self.channel_offset, highest))

    def redraw(self) -> None:
        """Redraw everything (scroll, resize, new data)."""
        if not self.winfo_exists():
            return
        if self._waiting_top is not None and self.follow and self.visible:
            if self._await_patterns(self._waiting_top):
                self._redraw_playing_lines()
                return
        self.columns, self.layout_name, self.shown_channels = self._choose_layout()
        self.channel_pad = LAYOUT_PAD[self.layout_name]
        self._clamp_channel_offset()
        self._draw_channel_header()
        self._draw_lines()
        self._update_labels()

    def apply_theme(self) -> None:
        """Apply the palette and redraw canvas items."""
        self.header_canvas.configure(bg=BG_ALT)
        self.canvas.configure(bg=BG_INPUT)
        self.redraw()

    def _draw_channel_header(self) -> None:
        canvas = self.header_canvas
        canvas.delete("all")
        canvas.configure(height=self.line_h)
        layout_w = layout_width(self.columns)
        cell = layout_w + self.channel_pad + CHANNEL_SEPARATOR
        x = 6 + GUTTER_CHARS * self.char_w
        for slot in range(self.shown_channels):
            channel = self.channel_offset + slot
            name = f"{channel + 1:02d}"
            canvas.create_text(x + (layout_w * self.char_w) / 2, self.line_h / 2,
                               text=name, fill=SEP, font=self.mono, anchor="center")
            x += cell * self.char_w
        hint = self._channel_hint()
        if hint:
            canvas.create_text(x + 6, self.line_h / 2, text=hint, fill=FG_FAINT,
                               font=self.mono, anchor="w")

    def _channel_hint(self) -> str:
        if self.channels <= 0:
            return ""
        first = self.channel_offset + 1
        last = self.channel_offset + self.shown_channels
        if self.shown_channels >= self.channels:
            return ""
        return f"channels {first}-{last} of {self.channels} (Shift+wheel)"

    def _update_labels(self) -> None:
        if not self.orders:
            self.state_label.configure(text="No module loaded - press play to watch it here")
            self.channel_label.configure(text="")
            self.follow_btn.state(["disabled"])
            self.channel_btn_left.state(["disabled"])
            self.channel_btn_right.state(["disabled"])
            return
        self.follow_btn.state(["!disabled"])
        self.channel_btn_left.state(["!disabled"] if self.channel_offset > 0 else ["disabled"])
        self.channel_btn_right.state(
            ["!disabled"] if self.channel_offset + self.shown_channels < self.channels
            else ["disabled"])
        self.follow_btn.configure(style="FollowOn.TButton" if self.follow else "Follow.TButton")
        self.follow_btn.configure(text="Following" if self.follow else "Follow")
        where = f"Order {self.current_order + 1:02d}/{len(self.orders):02d}"
        if self.current_pattern() >= 0:
            where += f", pattern {self.current_pattern():02d}"
        where += f", row {self.current_row:02d}"
        where += f", {self.channels} channels"
        if self._waiting_top is not None or self.pending:
            where += ", reading…"
        if self.errors:
            bad = ", ".join(f"{pattern:02d}" for pattern in sorted(self.errors))
            where += f", pattern {bad} could not be read"
        if not self.follow:
            where += ", scrolled"
        self.state_label.configure(text=where)

    def current_pattern(self) -> int:
        if 0 <= self.current_order < len(self.orders):
            return self.orders[self.current_order]
        return -1

    def _line_of_playback(self) -> Optional[int]:
        """Return the playing line, or None if its order has no mapped start."""
        start = self.block_start.get(self.current_order)
        if start is None:
            return None
        return start + self.current_row

    def _visible_range(self):
        first = max(0, int(self._view_top))
        height = max(1, self.canvas.winfo_height()) / self.line_h
        return range(first, min(len(self.lines), ceil(self._view_top + height)))

    def _draw_lines(self) -> None:
        self.top = max(0, min(self.top, self._max_top()))
        if self.top != self._target_top:
            self._snap_scroll()
        self._view_top = max(0.0, min(self._view_top, float(self._max_top())))
        if self._await_patterns(self._view_top):
            self._update_labels()
            return
        canvas = self.canvas
        canvas.delete("all")
        self._line_items = {}
        for index in self._visible_range():
            self._line_items[index] = self._draw_line(index, index - self._view_top)
        self._playing_line = self._line_of_playback()
        self._paint_top = self._view_top
        self._have_frame = not self._missing_patterns(self._view_top)

    def _draw_line(self, index: int, offset: float) -> list[int]:
        """Draw a row and return all its canvas items, including its background."""
        line = self.lines[index]
        items: list[int] = []
        canvas = self.canvas
        y = offset * self.line_h
        play_line = self._line_of_playback()
        is_playing = (play_line is not None and index == play_line and self.path != "")
        band = (0, y, canvas.winfo_width() or 400, y + self.line_h)
        kind, order, row = line
        if kind == "empty":
            items.append(canvas.create_rectangle(
                *band, fill=TRACKER_PLAYING if is_playing else BG_INPUT, outline=""))
            pattern = self.orders[order] if order < len(self.orders) else -1
            items.append(canvas.create_text(6 + GUTTER_CHARS * self.char_w,
                                            y + self.line_h / 2,
                                            text=f"(empty pattern {pattern:02d})",
                                            fill=TRACKER_DIM if is_playing else TRACKER_FAINT,
                                            font=self.mono, anchor="w"))
            return items
        if is_playing:
            background = TRACKER_PLAYING
        elif row % BEAT_ROWS == 0:
            background = TRACKER_BEAT
        else:
            background = BG_INPUT
        items.append(canvas.create_rectangle(*band, fill=background, outline=""))
        number_colour = (ON_ACCENT if is_playing
                         else (FG_DIM if row % BEAT_ROWS else FG_FAINT))
        items.append(canvas.create_text(6, y + self.line_h / 2, text=f"{row:02d}",
                                        fill=number_colour, font=self.mono, anchor="w"))
        items.append(canvas.create_text(6 + 3 * self.char_w, y + self.line_h / 2,
                                        text="│", fill=SEP, font=self.mono, anchor="w"))
        cells = self.patterns.get(self.orders[order]) if order < len(self.orders) else None
        layout_w = layout_width(self.columns)
        x = 6 + GUTTER_CHARS * self.char_w
        for slot in range(self.shown_channels):
            channel = self.channel_offset + slot
            cell = None
            if cells is not None and row < len(cells) and channel < len(cells[row]):
                cell = cells[row][channel]
            items.extend(self._draw_cell(x, y, cell, is_playing))
            x += (layout_w + self.channel_pad + CHANNEL_SEPARATOR) * self.char_w
        return items

    def _draw_cell(self, x: float, y: float, cell, is_playing: bool) -> list[int]:
        """One channel's four (or fewer) columns at x."""
        items = []
        note, instrument, volume, effect = cell if cell else ("", "", "", "")
        bright = TRACKER_BRIGHT
        dim = TRACKER_BRIGHT if is_playing else TRACKER_DIM
        faint = TRACKER_DIM if is_playing else TRACKER_FAINT
        for name in self.columns:
            width = COLUMN_CHARS[name]
            if name == "note":
                text, colour = ((note or EMPTY_NOTE), bright if note else faint)
            elif name == "instrument":
                text, colour = ((instrument or EMPTY_INSTRUMENT), dim if instrument else faint)
            elif name == "volume":
                text, colour = ((volume or EMPTY_VOLUME),
                                self._effect_colour(volume, False) if volume else faint)
            elif name == "effect":
                text, colour = ((effect or EMPTY_EFFECT),
                                self._effect_colour(effect, True) if effect else faint)
            else:                                   # the shared column
                shared, kind = shared_cell(volume, effect)
                text = shared or EMPTY_EFFECT
                colour = (self._effect_colour(shared, kind == "effect") if kind else faint)
            items.append(self.canvas.create_text(x, y + self.line_h / 2, text=text,
                                                 fill=colour, font=self.mono, anchor="w"))
            x += (width + COLUMN_GAP) * self.char_w
        # the channel separator (drawn dim, it is furniture)
        items.append(self.canvas.create_text(x - COLUMN_GAP * self.char_w,
                                             y + self.line_h / 2, text="│", fill=SEP,
                                             font=self.mono, anchor="w"))
        return items

    def _effect_colour(self, command: str, is_effect_column: bool) -> str:
        """Read effect colours from the active palette on each call."""
        found = (effects.category(command, self.family) if is_effect_column
                 else effects.volume_category(command, self.family))
        return {"global": EFFECT_GLOBAL, "volume": EFFECT_VOLUME, "pan": EFFECT_PAN,
                "pitch": EFFECT_PITCH, "misc": EFFECT_MISC}.get(found, TRACKER_BRIGHT)

    def _scroll(self, lines: int) -> str:
        """Scroll from the playing row on the first wheel movement, stop following, and clamp to
        the song."""
        if not lines:
            return "break"
        self._waiting_top = None
        if self.follow:
            line = self._line_of_playback()
            if line is not None:
                self.top = center_top(line, self._visible_lines(), len(self.lines))
            self.follow = False
        self.top = max(0, min(self.top + int(lines), self._max_top()))
        self._snap_scroll()
        self._draw_lines()
        self._update_labels()
        return "break"

    def _max_top(self) -> int:
        """The last line the view may start at (so the song's end stays in view)."""
        return max(0, len(self.lines) - self._visible_lines())

    def _scroll_channels(self, delta: int) -> str:
        self.channel_offset = max(0, self.channel_offset + delta)
        self._clamp_channel_offset()
        self.redraw()
        return "break"

    def _jump_to_start(self) -> str:
        self._waiting_top = None
        self.follow = False
        self.top = 0
        self._snap_scroll()
        self._draw_lines()
        self._update_labels()
        return "break"

    def _snap_scroll(self) -> None:
        self._hidden_glide = None
        self._glide = None
        self._row_time = None
        self._smooth_line = None
        self._smooth_interval = None
        self._target_top = self.top
        self._view_top = float(self.top)

    def _move_view(self, top: float) -> None:
        top = max(0.0, min(top, float(self._max_top())))
        delta = top - self._view_top
        if abs(delta) < 1e-9:
            return
        self._view_top = top
        if not self.visible:
            return
        self.canvas.move("all", 0, -delta * self.line_h)
        wanted = self._visible_range()
        for index in list(self._line_items):
            if index not in wanted:
                for item in self._line_items.pop(index):
                    self.canvas.delete(item)
        for index in wanted:
            if index not in self._line_items:
                self._line_items[index] = self._draw_line(index, index - top)
        self._paint_top = top

    def _advance_scroll(self, now: float) -> None:
        # Advance the active glide (visible or the hidden one kept for the
        # tracker tab). Called every UI tick for true per-frame interpolation,
        # not only on row changes.
        glide = self._glide if self.visible else self._hidden_glide
        if glide is None:
            return
        origin, target, started, duration = glide
        fraction = max(0.0, min(1.0, (now - started) / duration))
        self._move_view(origin + (target - origin) * fraction)
        if fraction >= 1.0:
            if self.visible:
                self._glide = None
            else:
                self._hidden_glide = None

    def tick(self, now: float | None = None) -> None:
        """Advance a smooth glide. Called every UI tick."""
        if not self.smooth_scrolling:
            return
        if now is None:
            now = monotonic()
        self._advance_scroll(now)

    def _follow_scroll(self, line: Optional[int], *, animate: bool = False,
                       now: float = 0.0, duration: float = 0.12,
                       adjacent: bool = False) -> None:
        """Move cached canvas rows in pixels, adding only newly exposed rows."""
        if line is None:
            return
        visible = self._visible_lines()
        want = center_top(line, visible, len(self.lines))
        if adjacent:
            # Normal playback: never freeze the highlight on a missing edge
            # pattern. The row is drawn as a placeholder until its pattern
            # arrives, so the current row never lags. Far seeks still wait.
            pass
        elif self._await_patterns(want):
            return
        self.top = self._target_top = want
        if not self.visible and not self.smooth_scrolling:
            self._snap_scroll()
            return
        delta = want - self._view_top
        if self.smooth_scrolling and animate and visible >= 4 and 0 < delta < visible / 2:
            self._glide = (self._view_top, float(want), now, duration)
        else:
            self._glide = None
            if abs(delta) > visible:  # seek / loop: never animate through unplayed rows
                self._snap_scroll()
                if self.visible:
                    self._draw_lines()
            else:
                self._move_view(float(want))

    def _scroll_to_playing(self, force: bool = False) -> None:
        """Centre the playing row (on Follow, on new data, on a tab switch)."""
        if not force and not self.follow:
            return
        line = self._line_of_playback()
        if line is None:
            return        # that pattern is not read yet - do not scroll to the top
        want = center_top(line, self._visible_lines(), len(self.lines))
        if self._await_patterns(want):
            return
        self.top = want
        self._snap_scroll()
        self._draw_lines()

    def _redraw_playing_lines(self) -> None:
        """Redraw the old and new highlighted rows."""
        line = self._line_of_playback()
        for index in (line, getattr(self, "_playing_line", -1)):
            if index is None:
                continue
            if index in self._line_items:
                for item in self._line_items.pop(index, []):
                    self.canvas.delete(item)
                self._line_items[index] = self._draw_line(index, index - self._paint_top)
        self._playing_line = line
        self._update_labels()

    def _toggle_follow(self) -> None:
        self._waiting_top = None
        self.follow = not self.follow
        if self.follow:
            self._scroll_to_playing(force=True)
        else:
            self._glide = self._hidden_glide = None
            self._row_time = None
            self._smooth_line = None
            self._smooth_interval = None
        self.redraw()

    def _on_wheel(self, event) -> str:
        step = 3 if getattr(event, "delta", 0) < 0 else -3
        return self._scroll(step)

    def _on_wheel_h(self, event) -> str:
        return self._scroll_channels(1 if getattr(event, "delta", 0) < 0 else -1)

    def _on_resize(self) -> None:
        self.line_h = max(10, self.mono.metrics("linespace") + 1)
        if self.follow:
            self._scroll_to_playing(force=True)
        self.redraw()

    def _missing_patterns(self, top: float) -> list[int]:
        """Unknown patterns covering a viewport, including its partial edge rows."""
        first = max(0, int(top))
        last = min(len(self.lines), ceil(top + max(1, self.canvas.winfo_height()) / self.line_h))
        missing = []
        for index in range(first, last):
            kind, order, _row = self.lines[index]
            pattern = self.orders[order]
            if kind == "row" and pattern >= 0 and pattern not in self.known and pattern not in missing:
                missing.append(pattern)
        return missing

    def _await_patterns(self, top: float) -> bool:
        """Do not replace a complete following viewport with unfilled cells."""
        if not (self.visible and self.follow and self.request_pattern is not None):
            return False
        if not self._missing_patterns(top):
            self._waiting_top = None
            return False
        self._waiting_top = top
        self._glide = self._hidden_glide = None
        self._row_time = None
        self._smooth_line = None
        self._smooth_interval = None
        if not self._have_frame:
            self.canvas.delete("all")
            self._line_items = {}
            self.canvas.create_text(12, self.line_h, text="Reading pattern data…",
                                    fill=FG_DIM, font=self.mono, anchor="w")
        return True

    def wanted_patterns(self) -> list[int]:
        """Request the seek destination first, not the old grid held on screen."""
        top = self._waiting_top if self._waiting_top is not None else self._view_top
        return [p for p in self._missing_patterns(top) if p not in self.pending]

    def _request_neighbours(self) -> None:
        """Bound outstanding work, fill the viewport before a small neighbouring pattern."""
        if self.request_pattern is None:
            return
        current = self.current_pattern()
        wanted = self.wanted_patterns()
        if current >= 0 and current not in self.known and current not in self.pending:
            wanted = [current] + [p for p in wanted if p != current]
        if not wanted and not self.pending and self._waiting_top is None:
            # Never preload the entire module or queue expensive speculative reads.
            for order in (self.current_order + 1, self.current_order - 1):
                if 0 <= order < len(self.orders):
                    pattern = self.orders[order]
                    if (pattern >= 0 and pattern not in self.known
                            and 0 < self.rows_of.get(pattern, 0) * self.channels <= 512):
                        wanted = [pattern]
                        break
        for pattern in wanted:
            if len(self.pending) >= 3:
                break
            if pattern in self.pending or pattern in self.known:
                continue
            self.pending.add(pattern)
            self.request_pattern(pattern)

    def request_visible(self) -> None:
        """Public: fill in what the current scroll position shows (tab switch)."""
        self._request_neighbours()
