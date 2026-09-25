"""Tk player interface, dialogs, and playback controls."""

from __future__ import annotations

import math
import os
import queue
import random
import sys
import threading
import time
import tkinter as tk
from tkinter import font as tkfont
from tkinter import messagebox, ttk
from typing import Optional

from . import library, reveal
from .audio import AudioError
from .cache import CACHE_NAME, AnalysisCache, cache_path
from .config import (APP_NAME, APP_TITLE, MAX_SHUFFLE_PATHS, UI_FPS_DEFAULT,
                     UI_RATE_LABELS, UI_RATE_VALUES, Settings, SAMPLE_RATE_LABELS,
                     SAMPLE_RATE_VALUES, normalise_sample_rate)
from .folderdialog import enable_select_all, pick_directory, show_path_tail, \
    system_picker_available  # noqa: F401  (show_path_tail is used by the toolbar)
from .filedialog import pick_files
from .engine import MSG_DEBUG, MSG_ERROR, MSG_INFO, MSG_WARN, PlaybackEngine
from .filters import (QueueFilter, filter_from_settings, formats_in,
                      normalise_formats, unknown_duration_count)
from .playlists import (PlaylistError, PlaylistStore, normalise_name, playlists_path,
                        read_m3u, write_m3u)
from .songinfo import SongInfoWindow
from .stats import ListeningCounter, StatsStore, stats_path
from .statsview import StatsWindow
from .trackerview import TrackerView
from .library import Analyzer, Track, format_time
from .openmpt import (DEFAULT_INTERPOLATION, get_lib, interpolation_length,
                      interpolation_name)

from . import theme
from .theme import (ACCENT, ACCENT_DIM, AMBER, BG, BG_ALT, BG_INPUT, BG_PANEL, BG_STRIPE,
                    BUTTON_OFF_BG, FG, FG_DIM, FG_FAINT, GREEN, ON_ACCENT, RED,
                    SEEK_FILL_OFF, SEEK_KNOB, SEEK_MARKER, SEEK_TRACK, SEEK_TRACK_OFF,
                    SEP, VU_BASELINE)

QUEUE_COLUMN_DEFAULTS = {"#0": 300, "folder": 170, "dur": 54, "fmt": 52, "ch": 34}
QUEUE_PANE_WEIGHT = 3   # the queue pane
INFO_PANE_WEIGHT = 2    # the info panel next to it
MIN_PANE_WIDTH = 140    # narrower than this neither pane is usable

INDENT_PER_LEVEL = 20   # pixels a ttk treeview indents one level of children
CELL_PAD = 6            # breathing room between a cell's text and the separator
REVEAL_TIMEOUT = 4.0    # seconds one "Show in folder" candidate may take
SESSION_SAVE_S = 15.0   # how often the listening position is written to the config
SEEK_MIN_WIDTH = 90     # a narrower position bar than this is useless
PATH_MIN_WIDTH = 150    # the library path keeps this much room when the window is narrow
SEARCH_MIN_WIDTH = 96    # the search box may shrink this far (the Filter button needs its room)
TREE_MIN_HEIGHT = 132   # heading + a few queue rows
TRANSPORT_ROW_GAP = 8   # vertical gap when the position group gets its own row
VOLUME_WHEEL_STEP = 5.0   # per cent the wheel moves the volume (same as the + / - keys)

MONO = ("DejaVu Sans Mono", 9) if sys.platform == "linux" else ("Consolas", 9)


def apply_theme(root: tk.Tk) -> ttk.Style:
    style = ttk.Style(root)
    root.option_add("*TCombobox*Listbox.background", BG_INPUT)
    root.option_add("*TCombobox*Listbox.foreground", FG)
    root.option_add("*TCombobox*Listbox.selectBackground", ACCENT_DIM)
    root.option_add("*TCombobox*Listbox.selectForeground", ON_ACCENT)
    root.option_add("*TCombobox*Listbox.borderWidth", 0)
    root.option_add("*TCombobox*Listbox.highlightThickness", 0)
    try:
        style.theme_use("clam")
    except tk.TclError:  # pragma: no cover
        pass
    root.configure(bg=BG)
    style.configure(".", background=BG, foreground=FG, fieldbackground=BG_INPUT,
                    bordercolor=BG_ALT, darkcolor=BG, lightcolor=BG_ALT,
                    troughcolor=BG_INPUT, focuscolor=ACCENT)
    style.configure("TFrame", background=BG)
    style.configure("Panel.TFrame", background=BG_PANEL)
    style.configure("Bar.TFrame", background=BG_PANEL)
    style.configure("TLabel", background=BG, foreground=FG)
    style.configure("Panel.TLabel", background=BG_PANEL, foreground=FG)
    style.configure("Dim.TLabel", background=BG_PANEL, foreground=FG_DIM)
    style.configure("BarDim.TLabel", background=BG_PANEL, foreground=FG_DIM)
    style.configure("Title.TLabel", background=BG_PANEL, foreground=FG,
                    font=("TkDefaultFont", 12, "bold"))
    style.configure("Head.TLabel", background=BG, foreground=FG,
                    font=("TkDefaultFont", 10, "bold"))
    style.configure("Time.TLabel", background=BG_PANEL, foreground=FG,
                    font=MONO if MONO else ("TkFixedFont", 10))
    style.configure("TButton", background=BG_ALT, foreground=FG, borderwidth=0,
                    focusthickness=0, padding=(9, 5))
    style.map("TButton",
              background=[("disabled", BUTTON_OFF_BG), ("active", ACCENT_DIM),
                          ("pressed", ACCENT_DIM)],
              foreground=[("disabled", FG_FAINT)])
    style.configure("Accent.TButton", background=ACCENT_DIM, foreground=ON_ACCENT)
    style.map("Accent.TButton",
              background=[("disabled", BUTTON_OFF_BG), ("active", ACCENT),
                          ("pressed", ACCENT)],
              foreground=[("disabled", FG_FAINT)])
    style.configure("Transport.TButton", font=("TkDefaultFont", 13), padding=(10, 4))
    style.configure("Mini.TButton", padding=(6, 1))
    style.configure("Follow.TButton", padding=(6, 1))
    style.configure("FollowOn.TButton", padding=(6, 1), background=ACCENT_DIM,
                    foreground=ON_ACCENT)
    style.map("FollowOn.TButton",
              background=[("disabled", BUTTON_OFF_BG), ("active", ACCENT), ("pressed", ACCENT)],
              foreground=[("disabled", FG_FAINT)])
    # the tab strip: the page you are on is the lit one
    style.configure("Tab.TButton", padding=(14, 3), background=BG_PANEL,
                    foreground=FG_DIM)
    style.map("Tab.TButton", background=[("disabled", BUTTON_OFF_BG), ("active", BG_ALT)],
              foreground=[("disabled", FG_FAINT), ("active", FG)])
    style.configure("TabActive.TButton", padding=(14, 3), background=BG_ALT,
                    foreground=ACCENT)
    style.map("TabActive.TButton", background=[("disabled", BUTTON_OFF_BG), ("active", BG_ALT)],
              foreground=[("disabled", FG_FAINT), ("active", ACCENT)])
    style.configure("TCheckbutton", background=BG_PANEL, foreground=FG,
                    focuscolor=BG_PANEL)
    style.map("TCheckbutton", background=[("active", BG_PANEL)],
              foreground=[("disabled", FG_DIM)])
    style.configure("TRadiobutton", background=BG, foreground=FG, focuscolor=BG)
    style.map("TRadiobutton", background=[("active", BG)], foreground=[("active", FG)])
    style.configure("TEntry", fieldbackground=BG_INPUT, foreground=FG,
                    insertcolor=FG, bordercolor=BG_ALT)
    style.map("PlaylistName.TEntry", selectbackground=[("!disabled", ACCENT_DIM)],
              selectforeground=[("!disabled", ON_ACCENT)])
    style.configure("TCombobox", fieldbackground=BG_INPUT, background=BG_ALT,
                    foreground=FG, arrowcolor=FG, bordercolor=BG_ALT)
    style.map("TCombobox",
              fieldbackground=[("readonly", BG_INPUT), ("focus", BG_INPUT)],
              foreground=[("readonly", FG)],
              background=[("disabled", BG_ALT), ("pressed", ACCENT_DIM), ("active", SEP)],
              arrowcolor=[("disabled", FG_FAINT)],
              bordercolor=[("focus", ACCENT)])
    style.configure("TScale", background=SEP, troughcolor=BG_INPUT, borderwidth=0,
                    lightcolor=SEP, darkcolor=SEP, sliderlength=18, sliderrelief="flat",
                    gripcount=0, focuscolor=ACCENT)
    style.map("TScale",
              background=[("disabled", BG_ALT), ("pressed", ACCENT),
                          ("active", ACCENT_DIM)],
              lightcolor=[("disabled", BG_ALT), ("pressed", ACCENT),
                          ("active", ACCENT_DIM)],
              darkcolor=[("disabled", BG_ALT), ("pressed", ACCENT),
                         ("active", ACCENT_DIM)])
    style.configure("TProgressbar", background=ACCENT, troughcolor=BG_INPUT, borderwidth=0)
    style.configure("Treeview", background=BG_PANEL, fieldbackground=BG_PANEL,
                    foreground=FG, borderwidth=0, rowheight=21)
    style.map("Treeview", background=[("selected", ACCENT_DIM)],
              foreground=[("selected", ON_ACCENT)])
    style.configure("Treeview.Heading", background=BG_ALT, foreground=FG_DIM,
                    relief="raised", borderwidth=1, padding=(6, 3),
                    bordercolor=SEP, lightcolor=SEP, darkcolor=SEP)
    style.map("Treeview.Heading", background=[("active", BG_ALT)])
    style.configure("TPanedwindow", background=BG)
    style.configure("TSeparator", background=BG_ALT)
    for _dir in ("Vertical", "Horizontal"):
        style.configure(f"{_dir}.TScrollbar", background=BG_ALT, troughcolor=BG,
                        bordercolor=BG, lightcolor=BG, darkcolor=BG,
                        arrowcolor=FG_DIM, borderwidth=0, arrowsize=13, gripcount=0)
        style.map(f"{_dir}.TScrollbar",
                  background=[("disabled", BG_PANEL), ("pressed", ACCENT_DIM),
                              ("active", SEP)],
                  arrowcolor=[("disabled", FG_FAINT), ("pressed", ON_ACCENT)])
    StyleCls = style
    style.configure("Path.TEntry", fieldbackground=BG_PANEL, background=BG_PANEL,
                    foreground=FG, bordercolor=BG_PANEL, lightcolor=BG_PANEL,
                    darkcolor=BG_PANEL, focuscolor=BG_PANEL, focusthickness=0,
                    selectbackground=ACCENT_DIM, selectforeground=ON_ACCENT,
                    padding=(6, 3))
    style.map("Path.TEntry",
              fieldbackground=[("readonly", BG_PANEL), ("focus", BG_PANEL)],
              foreground=[("readonly", FG)],
              bordercolor=[("focus", SEP)],
              background=[("focus", BG_PANEL), ("readonly", BG_PANEL)],
              lightcolor=[("focus", BG_PANEL)],
              darkcolor=[("focus", BG_PANEL)])
    StyleCls.configure("Dlg.TEntry", fieldbackground=BG_INPUT, foreground=FG,
                       insertcolor=FG, bordercolor=BG_ALT, lightcolor=BG_INPUT,
                       darkcolor=BG_INPUT, padding=(6, 4))
    StyleCls.map("Dlg.TEntry",
                 fieldbackground=[("focus", BG_INPUT), ("disabled", BG_PANEL)],
                 bordercolor=[("focus", ACCENT)],
                 background=[("focus", BG_INPUT), ("readonly", BG_INPUT)],
                 lightcolor=[("focus", BG_INPUT)],
                 darkcolor=[("focus", BG_INPUT)])
    StyleCls.configure("DlgError.TEntry", fieldbackground=BG_INPUT, foreground=RED,
                       insertcolor=FG, bordercolor=RED, lightcolor=BG_INPUT,
                       darkcolor=BG_INPUT, padding=(6, 4))
    StyleCls.map("DlgError.TEntry",
                 background=[("focus", BG_INPUT)],
                 lightcolor=[("focus", BG_INPUT)], darkcolor=[("focus", BG_INPUT)])
    for name, fg, extra in (("Crumb.TButton", ACCENT, {}),
                            ("CrumbHere.TButton", FG, {"font": ("TkDefaultFont", 9, "bold")})):
        StyleCls.configure(name, background=BG, foreground=fg, padding=(5, 1),
                           relief="flat", borderwidth=0, highlightthickness=0,
                           lightcolor=BG, darkcolor=BG, bordercolor=BG, focusthickness=0,
                           **extra)
        StyleCls.map(name,
                     background=[("active", BG_ALT), ("pressed", BG_ALT), ("focus", BG)],
                     foreground=[("active", ON_ACCENT)],
                     bordercolor=[("focus", BG)], lightcolor=[("focus", BG)],
                     darkcolor=[("focus", BG)])
    # labels/checkbuttons that sit on the dialog's plain background
    StyleCls.configure("DlgDim.TLabel", background=BG, foreground=FG_DIM)
    StyleCls.configure("Dlg.TCheckbutton", background=BG, foreground=FG,
                       focuscolor=BG, indicatorcolor=BG_INPUT)
    StyleCls.map("Dlg.TCheckbutton",
                 background=[("active", BG), ("focus", BG)],
                 foreground=[("disabled", FG_DIM)],
                 indicatorcolor=[("selected", ACCENT), ("!selected", BG_INPUT)])
    StyleCls.configure("Dlg.Treeview", background=BG_PANEL, fieldbackground=BG_PANEL,
                       foreground=FG, borderwidth=0, relief="flat", rowheight=22)
    StyleCls.configure("Dlg.Treeview.Heading", background=BG_ALT, foreground=FG_DIM,
                       relief="flat")
    return style


class VuMeter(tk.Canvas):
    """Channel and stereo level bars with persistent background troughs."""

    BASELINE = 38          # the channel bars stand on this line
    BOTTOM = 70            # ...and grow up to here
    GAP = 2                # between two neighbouring bars
    RAIL_H = 8             # height of each of the two stereo rails
    RAIL_STEP = 11         # ...and the distance between them

    def __init__(self, master, width=320, height=74, **kw):
        super().__init__(master, width=width, height=height, bg=BG_INPUT,
                         highlightthickness=0, **kw)
        self._bars: list[int] = []          # the troughs of the channels
        self._fills: list[int] = []         # the levels drawn over them
        self._level_bars: list[int] = []    # the two stereo rails...
        self._level_fills: list[int] = []   # ...and their levels
        self._values: tuple[float, ...] = ()
        self._levels = (0.0, 0.0)
        self._channels = 0
        self._built_for = 0                 # the width the bars are laid out for
        self.bind("<Configure>", self._on_configure)

    def apply_theme(self) -> None:
        """Apply the palette and redraw the meter."""
        self.configure(bg=BG_INPUT)
        count = self._channels                 # no bars before a module is loaded:
        self.delete("all")                     # the meter only ever draws what
        self._bars, self._fills = [], []       # it was told about
        self._level_bars, self._level_fills = [], []
        self._channels = 0                     # so set_channels() really rebuilds
        if count:
            self.set_channels(count)
        self.redraw()

    def _meter_width(self) -> int:
        """The width to lay the bars out for: the canvas' own, before it is mapped."""
        width = self.winfo_width()
        return width if width > 1 else int(self.winfo_reqwidth())

    def _on_configure(self, _event=None) -> None:
        """The panel was resized: lay the bars out for the width it has now."""
        if self._meter_width() != self._built_for:
            self._build_bars()
            self.redraw()

    def set_channels(self, count: int) -> None:
        count = max(1, min(int(count), 64))
        if count == self._channels and self._built_for == self._meter_width():
            return
        self._channels = count
        self._build_bars()

    def _build_bars(self) -> None:
        """(Re)build the troughs and their level rectangles for the current width."""
        width = self._built_for = self._meter_width()
        count = max(1, self._channels)
        self.delete("all")
        self._bars, self._fills = [], []
        self._level_bars, self._level_fills = [], []
        bar_w = max(3, min(11, (width - 10) // count))
        x = max(5, (width - count * bar_w) // 2)
        self.create_line(4, self.BASELINE - 1, width - 4, self.BASELINE - 1, fill=VU_BASELINE)
        for i in range(count):
            x0 = x + i * bar_w
            self._bars.append(self.create_rectangle(
                x0, self.BASELINE + 1, x0 + bar_w - self.GAP, self.BOTTOM,
                fill=BG_ALT, outline=""))
            self._fills.append(self.create_rectangle(
                x0, self.BASELINE + 1, x0 + bar_w - self.GAP, self.BASELINE + 1,
                fill=BG_ALT, outline=""))
        for i in range(2):
            top = 6 + i * self.RAIL_STEP
            self._level_bars.append(self.create_rectangle(
                4, top, width - 4, top + self.RAIL_H, fill=BG_ALT, outline=""))
            self._level_fills.append(self.create_rectangle(
                4, top, 5, top + self.RAIL_H, fill=BG_ALT, outline=""))

    def update_values(self, values: tuple[float, ...], levels=(0.0, 0.0)) -> None:
        self._values = values
        self._levels = levels

    def values_changed(self, values, levels) -> bool:
        """Is a redraw worth it?  (A bar is either still or it moved visibly.)"""
        def moved(old, new) -> bool:
            for a, b in zip(old or (), new or ()):
                if abs(float(a) - float(b)) > 0.002:
                    return True
            return len(old or ()) != len(new or ())
        return moved(self._values, values) or moved(self._levels, levels)

    def redraw(self) -> None:
        if not self._fills:
            return
        width = self._built_for
        n = len(self._fills)
        bar_w = max(3, min(11, (width - 10) // n))
        x_base = max(5, (width - n * bar_w) // 2)
        for i, item in enumerate(self._fills):
            v = self._values[i] if i < len(self._values) else 0.0
            frac = min(1.0, max(0.0, v) ** 0.6)
            x0 = x_base + i * bar_w
            if frac <= 0.001:                 # silence: the trough shows through
                self.coords(item, x0, self.BASELINE + 1, x0 + bar_w - self.GAP,
                            self.BASELINE + 1)
                self.itemconfigure(item, fill=BG_ALT)
                continue
            top = self.BASELINE + 1 + (1.0 - frac) * (self.BOTTOM - self.BASELINE - 1)
            self.coords(item, x0, top, x0 + bar_w - self.GAP, self.BOTTOM)
            colour = GREEN if frac < 0.75 else (AMBER if frac < 0.92 else RED)
            self.itemconfigure(item, fill=colour)
        for i, item in enumerate(self._level_fills):
            v = self._levels[i] if i < len(self._levels) else 0.0
            frac = min(1.0, max(0.0, v) ** 0.5)
            top = 6 + i * self.RAIL_STEP
            self.coords(item, 4, top, 4 + max(1, frac * (width - 8)), top + self.RAIL_H)
            colour = GREEN if frac < 0.75 else (AMBER if frac < 0.92 else RED)
            self.itemconfigure(item, fill=colour if frac > 0.002 else BG_ALT)


class SeekBar(tk.Canvas):
    """Click to seek, drag to preview and seek on release, or hover to show the time."""

    MARGIN = 7            # half of the knob, keeps the knob inside the bar
    KNOB_R = 6
    READOUT_TOP = 2       # first pixel the read-out may use
    READOUT_GAP = 2       # clear space between the read-out and the knob
    READOUT_FONT = ("TkDefaultFont", 7)      # the font of the hover read-out
    HEIGHT = 42

    def __init__(self, master, on_press=None, on_drag=None, on_release=None,
                 on_hover=None, width=400, height=None, **kw):
        self._readout_font = tkfont.Font(font=self.READOUT_FONT)
        if height is None:
            above = (self.READOUT_TOP + self._readout_font.metrics("linespace")
                     + self.READOUT_GAP + self.KNOB_R)
            height = 2 * above
        super().__init__(master, width=width, height=height, bg=BG_PANEL,
                         highlightthickness=0, bd=0, **kw)
        self.on_press = on_press
        self.on_drag = on_drag
        self.on_release = on_release
        self.on_hover = on_hover
        self._fraction = 0.0
        self._total = 0.0
        self._total_known = False
        self._hover_x = None
        self._dragging = False
        self._enabled = True
        self._items: dict = {}          # canvas items, built on the first draw
        self.bind("<Button-1>", self._press)
        self.bind("<B1-Motion>", self._motion)
        self.bind("<ButtonRelease-1>", self._release)
        self.bind("<Motion>", self._hover)
        self.bind("<Leave>", self._leave)
        self.bind("<Configure>", lambda _e: self.redraw())

    def apply_theme(self) -> None:
        """Recolour existing seek-bar items."""
        self.configure(bg=BG_PANEL)
        self._items = {}
        self.delete("all")
        self.redraw()

    def _x_of(self, fraction: float) -> float:
        span = max(self.winfo_width() - 2 * self.MARGIN, 1)
        return self.MARGIN + max(0.0, min(1.0, fraction)) * span

    def fraction_of(self, x: float) -> float:
        """Pixel -> 0..1 position (this is what makes clicking work)."""
        span = max(self.winfo_width() - 2 * self.MARGIN, 1)
        return max(0.0, min(1.0, (float(x) - self.MARGIN) / span))

    def set_fraction(self, fraction: float) -> None:
        fraction = max(0.0, min(1.0, float(fraction)))
        if abs(fraction - self._fraction) * max(self.winfo_width(), 1) < 0.4:
            return                       # sub-pixel: the drawing would not change
        self._fraction = fraction
        self.redraw()

    def get_fraction(self) -> float:
        return self._fraction

    def set_total(self, seconds: float, known: bool) -> None:
        self._total, self._total_known = float(seconds or 0.0), bool(known)

    def set_enabled(self, enabled: bool) -> None:
        if bool(enabled) != self._enabled:
            self._enabled = bool(enabled)
            self.redraw()

    def _press(self, event):
        self._dragging = True
        self._hover_x = event.x
        if self.on_press:
            self.on_press(event)
        return "break"

    def _motion(self, event):
        self._hover_x = event.x
        if self.on_drag:
            self.on_drag(event)

    def _release(self, event):
        self._dragging = False
        if self.on_release:
            self.on_release(event)

    def _hover(self, event):
        self._hover_x = event.x
        self.redraw()
        if self.on_hover:
            self.on_hover(event)

    def _leave(self, _event):
        if not self._dragging:
            self._hover_x = None
            self.redraw()

    def redraw(self) -> None:
        """Update existing canvas items instead of recreating them."""
        width, height = self.winfo_width(), self.winfo_height()
        if width < 4:
            return
        cy = max(self.KNOB_R + 8, height / 2)
        x0, x1 = self.MARGIN, width - self.MARGIN
        xk = self._x_of(self._fraction)
        track = SEEK_TRACK if self._enabled else SEEK_TRACK_OFF
        fill = ACCENT if self._enabled else SEEK_FILL_OFF

        items = self._items
        if not items or items["width"] != width or items["height"] != height:
            self.delete("all")
            items = self._items = {"width": width, "height": height}
            items["track"] = self.create_line(x0, cy, x1, cy, width=6, capstyle="round",
                                              fill=track)
            items["fill"] = self.create_line(x0, cy, xk, cy, width=6, capstyle="round",
                                             fill=fill)
            items["knob"] = self.create_oval(xk - self.KNOB_R, cy - self.KNOB_R,
                                             xk + self.KNOB_R, cy + self.KNOB_R,
                                             fill=SEEK_KNOB, outline=fill)
            items["marker"] = self.create_line(x0, cy - 8, x0, cy + 8, fill=SEEK_MARKER,
                                               state="hidden")
        else:
            self.coords(items["track"], x0, cy, x1, cy)
            self.itemconfigure(items["track"], fill=track)
            self.coords(items["fill"], x0, cy, max(x0, xk), cy)
            self.itemconfigure(items["fill"], fill=fill)
            self.coords(items["knob"], xk - self.KNOB_R, cy - self.KNOB_R,
                        xk + self.KNOB_R, cy + self.KNOB_R)
            self.itemconfigure(items["knob"], outline=fill)

        self.itemconfigure(items["knob"], state="normal" if self._enabled else "hidden")
        if xk <= x0 + 0.5:
            self.itemconfigure(items["fill"], state="hidden")
        else:
            self.itemconfigure(items["fill"], state="normal")

        xh = self._hover_x
        if xh is not None and self._enabled:
            xh = max(x0, min(x1, xh))
            self.coords(items["marker"], xh, cy - 8, xh, cy + 8)
            self.itemconfigure(items["marker"], state="normal")
            label = self._time_at(self.fraction_of(xh))
            if label:
                self._place_readout(label, xh, x0, x1, cy - self.KNOB_R - self.READOUT_GAP)
            else:
                self._hide_readout()
        else:
            self.itemconfigure(items["marker"], state="hidden")
            self._hide_readout()

    def _hide_readout(self) -> None:
        """Nothing under the pointer -> no read-out on the canvas at all."""
        for key in ("bubble", "readout"):
            item = self._items.pop(key, None)
            if item is not None:
                self.delete(item)

    def _place_readout(self, label: str, xh: float, x0: int, x1: int,
                       lowest: int) -> None:
        """Place the hover time beside the marker without crossing the canvas edge."""
        probe = self._readout_font
        if probe is None:
            probe = self._readout_font = tkfont.Font(font=("TkDefaultFont", 7))
        text_w = probe.measure(label)
        text_h = probe.metrics("linespace")
        pad = 4
        if xh - x0 > x1 - xh:                       # room on the left: put it there
            left = xh - 10 - text_w - pad
        else:
            left = xh + 10 - pad
        left = max(x0 - self.MARGIN // 2, min(x1 + self.MARGIN // 2 - text_w - 2 * pad, left))
        top = self.READOUT_TOP
        bottom = max(top + text_h, min(top + text_h, lowest))
        items = self._items
        if "bubble" not in items:      # the pointer just arrived: build it once
            items["bubble"] = self.create_rectangle(0, 0, 0, 0, fill=BG_INPUT, outline=SEP)
            items["readout"] = self.create_text(0, 0, text="", anchor="sw", fill=FG,
                                                font=("TkDefaultFont", 7))
        self.coords(items["bubble"], left, top - 1, left + text_w + 2 * pad, bottom + 1)
        self.coords(items["readout"], left + pad, bottom)
        self.itemconfigure(items["readout"], text=label)

    def _time_at(self, fraction: float) -> str:
        if not self._total_known or self._total <= 0:
            return ""
        return format_time(fraction * self._total)

TITLE_MODE_LABELS = {"track": "Title & artist", "filename": "File name"}
TITLE_MODE_VALUES = {label: mode for mode, label in TITLE_MODE_LABELS.items()}
TITLE_MAX = 120          # a window title is a label, not a screen

INTERPOLATION_LABELS = {
    "off": "No interpolation",
    "linear": "Linear",
    "cubic": "Cubic (spline)",
    "sinc": "Sinc (8 tap)",
}
INTERPOLATION_VALUES = {label: mode for mode, label in INTERPOLATION_LABELS.items()}
INTERPOLATION_HINTS = {
    "off": "Nearest-neighbour, lowest CPU usage",
    "linear": "Basic interpolation, low CPU usage",
    "cubic": "Smooth spline interpolation, medium CPU usage",
    "sinc": "8-tap sinc, higher CPU usage",
}


def interpolation_label(mode) -> str:
    """User visible name of a resampling filter (empty for an unknown one)."""
    name = INTERPOLATION_LABELS.get(str(mode or ""))
    if name is None:
        name = INTERPOLATION_LABELS.get(interpolation_name(interpolation_length(mode) or 0), "")
    return name


def spin_number(variable) -> Optional[float]:
    """Read a spinbox number, or return None for empty or invalid input."""
    try:
        value = float(variable.get())
    except (tk.TclError, TypeError, ValueError):
        return None
    return value if math.isfinite(value) else None


def now_playing_title(mode: str, filename: str = "", title: str = "",
                      artist: str = "", app_name: str = APP_NAME) -> str:
    """Build the selected title format, falling back to the file name or app title."""
    filename = (filename or "").strip()
    title = (title or "").strip()
    artist = (artist or "").strip()
    if artist in ("?", "??", "unknown", "Unknown", "-"):
        artist = ""                       # libopenmpt uses "?" for "no artist"

    if mode == "filename":
        label = filename
    else:
        label = f"{artist} - {title}" if artist and title else (title or filename)
    if not label:
        return app_name
    if len(label) > TITLE_MAX:
        label = label[:TITLE_MAX - 1].rstrip() + "\u2026"
    return f"{label} - {app_name}"


class FilterDialog(tk.Toplevel):
    """Formats, a length range and "playable only" - with a live count."""

    def __init__(self, app: "PlayerApp"):
        app._dismiss_playlist_menu()
        super().__init__(app.root)
        self.app = app
        self.title("Filter the queue")
        self.configure(bg=BG)
        self.transient(app.root)
        self.resizable(False, False)
        current = app._queue_filter()

        frm = ttk.Frame(self, padding=14)
        frm.grid(row=0, column=0, sticky="nsew")
        frm.columnconfigure(0, weight=1)

        ttk.Label(frm, text="File formats", style="Head.TLabel").grid(row=0, column=0,
                                                                     sticky="w")
        ttk.Label(frm, text="nothing ticked means every format",
                  style="Dim.TLabel", background=BG).grid(row=1, column=0, sticky="w",
                                                          pady=(0, 6))
        box = ttk.Frame(frm)
        box.grid(row=2, column=0, sticky="ew")
        found = formats_in(app._queue_source_tracks())
        known = [(name, count) for name, count in found if name]
        unnamed = sum(count for name, count in found if not name)
        self.format_vars: dict[str, tk.BooleanVar] = {}
        for index, (name, count) in enumerate(known):
            variable = tk.BooleanVar(value=name in current.formats)
            self.format_vars[name] = variable
            ttk.Checkbutton(box, text=f"{name.upper()}  ({count})", variable=variable,
                            command=self.update_preview).grid(
                row=index // 3, column=index % 3, sticky="w", padx=(0, 16))
        if not known:
            ttk.Label(box, text="no module with a known format yet - press Analyze",
                      style="Dim.TLabel", background=BG).grid(row=0, column=0, sticky="w")
        elif unnamed:
            ttk.Label(box, text=f"{unnamed} module(s) have no known format "
                                f"(Analyze reads them; 'hide modules that cannot be "
                                f"played' leaves them out)",
                      style="Dim.TLabel", background=BG).grid(
                row=len(known) // 3 + 1, column=0, columnspan=3, sticky="w", pady=(4, 0))

        buttons_row = ttk.Frame(frm)
        buttons_row.grid(row=3, column=0, sticky="w", pady=(6, 0))
        ttk.Button(buttons_row, text="All", command=lambda: self.tick_all(True),
                   style="Mini.TButton").pack(side="left")
        ttk.Button(buttons_row, text="None", command=lambda: self.tick_all(False),
                   style="Mini.TButton").pack(side="left", padx=(6, 0))

        ttk.Separator(frm, orient="horizontal").grid(row=4, column=0, sticky="ew", pady=10)
        ttk.Label(frm, text="Length (leave 0 for no limit)", style="Head.TLabel").grid(
            row=5, column=0, sticky="w")
        lengths = ttk.Frame(frm)
        lengths.grid(row=6, column=0, sticky="w", pady=(6, 0))
        self.min_var = tk.DoubleVar(value=current.min_seconds)
        self.max_var = tk.DoubleVar(value=current.max_seconds)
        ttk.Label(lengths, text="at least").grid(row=0, column=0, sticky="w")
        ttk.Spinbox(lengths, from_=0, to=36000, increment=10, width=8,
                    textvariable=self.min_var, command=self.update_preview).grid(
            row=0, column=1, padx=(8, 4))
        ttk.Label(lengths, text="seconds").grid(row=0, column=2, sticky="w", padx=(0, 18))
        ttk.Label(lengths, text="at most").grid(row=0, column=3, sticky="w")
        ttk.Spinbox(lengths, from_=0, to=36000, increment=10, width=8,
                    textvariable=self.max_var, command=self.update_preview).grid(
            row=0, column=4, padx=(8, 4))
        ttk.Label(lengths, text="seconds").grid(row=0, column=5, sticky="w")
        for variable in (self.min_var, self.max_var):
            variable.trace_add("write", lambda *_: self.update_preview())

        self.broken_var = tk.BooleanVar(value=current.hide_broken)
        ttk.Checkbutton(frm, text="Hide modules that cannot be played",
                        variable=self.broken_var, command=self.update_preview).grid(
            row=7, column=0, sticky="w", pady=(10, 0))

        self.preview = ttk.Label(frm, text="", style="Dim.TLabel", background=BG)
        self.preview.grid(row=8, column=0, sticky="w", pady=(10, 0))

        row = ttk.Frame(frm)
        row.grid(row=9, column=0, sticky="e", pady=(12, 0))
        ttk.Button(row, text="Clear", command=self.clear).pack(side="left")
        ttk.Button(row, text="Cancel", command=self.cancel).pack(side="left", padx=(8, 0))
        ttk.Button(row, text="Apply", style="Accent.TButton",
                   command=self.apply).pack(side="left", padx=(8, 0))
        self.bind("<Escape>", lambda _e: self.cancel())
        self.bind("<Return>", lambda _e: self.apply())
        self.update_preview()
        self.grab_set()                       # one dialog at a time

    def tick_all(self, value: bool) -> None:
        for variable in self.format_vars.values():
            variable.set(value)
        self.update_preview()

    def criteria(self) -> QueueFilter:
        ticked = [name for name, variable in self.format_vars.items() if variable.get()]
        return QueueFilter(
            formats=normalise_formats(ticked),
            min_seconds=max(0.0, spin_number(self.min_var) or 0.0),
            max_seconds=max(0.0, spin_number(self.max_var) or 0.0),
            hide_broken=bool(self.broken_var.get()),
        )

    def update_preview(self) -> None:
        """Say what the criteria would leave, without touching the queue."""
        criteria = self.criteria()
        found = library.search_filter(self.app._queue_source_tracks(), self.app.search_var.get())
        shown = criteria.count(found)
        note = f"{shown} of {len(found)} modules"
        if criteria.active and (criteria.min_seconds or criteria.max_seconds):
            unknown = unknown_duration_count(found)
            if unknown:
                note += f", {unknown} still without a length (they stay until read)"
        if shown == 0:
            note += ", nothing would be left"
        self.preview.configure(text=note)

    def clear(self) -> None:
        for variable in self.format_vars.values():
            variable.set(False)
        self.min_var.set(0)
        self.max_var.set(0)
        self.broken_var.set(False)
        self.update_preview()

    def apply(self) -> None:
        criteria = self.criteria()
        settings = self.app.settings
        settings.filter_formats = sorted(criteria.formats)
        settings.filter_min = criteria.min_seconds
        settings.filter_max = criteria.max_seconds
        settings.filter_hide_broken = criteria.hide_broken
        self.destroy()
        self.app.apply_filter()

    def cancel(self) -> None:
        self.destroy()


class NewPlaylistDialog(tk.Toplevel):
    """Theme-aware name prompt; invalid names and save errors stay in the dialog."""

    def __init__(self, app: "PlayerApp", paths=()):
        app._dismiss_playlist_menu()
        super().__init__(app.root)
        self.app = app
        self.paths = list(paths)  # freeze selection before focus moves
        self.result: Optional[str] = None
        self.title("New playlist")
        self.transient(app.root)
        self.resizable(False, False)
        self.configure(bg=BG)
        self._previous_focus = app.root.focus_get()
        self._previous_grab = app.root.grab_current()
        self._previous_grab_status = (self._previous_grab.grab_status()
                                      if self._previous_grab is not None else None)
        body = ttk.Frame(self, padding=18)
        body.pack(fill="both", expand=True)
        ttk.Label(body, text="New playlist", style="Head.TLabel").pack(anchor="w")
        count = len(self.paths)
        self.description = ttk.Label(body, text=(f"Add {count} selected song{'s' if count != 1 else ''}."
                                                  if count else "Create an empty playlist to fill later."),
                                      background=BG, foreground=FG_DIM)
        self.description.pack(anchor="w", pady=(6, 16))
        ttk.Label(body, text="Playlist name").pack(anchor="w")
        self.name_var = tk.StringVar()
        self.name_entry = ttk.Entry(body, textvariable=self.name_var, width=40,
                                    style="PlaylistName.TEntry")
        self.name_entry.pack(fill="x", pady=(6, 0))
        self.error = ttk.Label(body, text="", background=BG, foreground=RED,
                                wraplength=370, justify="left")
        self.error.pack(anchor="w", pady=(6, 10))
        buttons = ttk.Frame(body)
        buttons.pack(fill="x")
        self.create_btn = ttk.Button(buttons, text="Create playlist", style="Accent.TButton",
                                      command=self.submit)
        self.create_btn.pack(side="right")
        self.cancel_btn = ttk.Button(buttons, text="Cancel", command=self.close)
        self.cancel_btn.pack(side="right", padx=(0, 8))
        self.name_var.trace_add("write", self._name_changed)
        self._name_changed()
        self.bind("<Return>", self.submit)
        self.bind("<Escape>", self.close)
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.update_idletasks()
        # Centre over the player, clamped to the screen (also under bare Xvfb).
        x = max(0, min(app.root.winfo_rootx() + (app.root.winfo_width() - self.winfo_reqwidth()) // 2,
                       self.winfo_screenwidth() - self.winfo_reqwidth()))
        y = max(0, min(app.root.winfo_rooty() + (app.root.winfo_height() - self.winfo_reqheight()) // 2,
                       self.winfo_screenheight() - self.winfo_reqheight()))
        self.geometry(f"+{x}+{y}")
        self.wait_visibility()
        self.grab_set()
        self.name_entry.focus_set()

    def _name_changed(self, *_args) -> None:
        self.create_btn.state(["!disabled"] if self.name_var.get().strip() else ["disabled"])
        self.error.configure(text="")

    def submit(self, _event=None) -> str:
        try:
            name = normalise_name(self.name_var.get())
            playlist = self.app._playlists.add(name, self.paths, self.app._directory)
        except PlaylistError as exc:
            self.error.configure(text=str(exc))
            self.name_entry.focus_set()
            return "break"
        self.result = playlist.name
        self.app.status(f"Created '{playlist.name}' with {len(playlist.paths)} songs")
        self.close()
        return "break"

    def apply_theme(self) -> None:
        self.configure(bg=BG)
        self.description.configure(background=BG, foreground=FG_DIM)
        self.error.configure(background=BG, foreground=RED)

    def close(self, _event=None) -> str:
        self.destroy()
        if self.app._new_playlist_dialog is self:
            self.app._new_playlist_dialog = None
        try:
            if self._previous_grab is not None and self._previous_grab.winfo_exists():
                if self._previous_grab_status == "global":
                    self._previous_grab.grab_set_global()
                else:
                    self._previous_grab.grab_set()
            focus = self._previous_focus
            if focus is None or not focus.winfo_exists():
                focus = self.app.root
            focus.focus_set()
            if self.app.root.focus_get() is None:
                focus.focus_force()
        except tk.TclError:  # player closing
            pass
        return "break"


class PlaylistsDialog(tk.Toplevel):
    """Manage named playlists; selection actions also live beside the songs."""

    def __init__(self, app: "PlayerApp"):
        app._dismiss_playlist_menu()
        super().__init__(app.root)
        self.app = app
        self.title("Playlists")
        self.configure(bg=BG)
        self.transient(app.root)
        self.resizable(True, True)
        self.geometry("640x560")
        self.minsize(600, 510)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)

        frm = ttk.Frame(self, padding=14)
        frm.grid(row=0, column=0, sticky="nsew")
        frm.columnconfigure(0, weight=1)
        frm.rowconfigure(3, weight=1)

        top = ttk.Frame(frm)
        top.grid(row=0, column=0, sticky="ew")
        ttk.Label(top, text="Name", style="Dim.TLabel", background=BG).pack(side="left")
        self.name_var = tk.StringVar()
        self.name_entry = ttk.Entry(top, textvariable=self.name_var, width=24)
        self.name_entry.pack(side="left", padx=(8, 12), fill="x", expand=True)
        self.new_btn = ttk.Button(top, text="Create empty playlist", style="Accent.TButton",
                                  command=self.create_empty)
        self.new_btn.pack(side="left")
        self.save_as_btn = ttk.Button(frm, text="Save current queue as\u2026",
                                      command=self.apply_save_as)
        self.save_as_btn.grid(row=1, column=0, sticky="w", pady=(8, 0))
        self.queue_note = ttk.Label(frm, text="", style="Dim.TLabel", background=BG)
        self.queue_note.grid(row=2, column=0, sticky="w", pady=(4, 8))

        list_frame = ttk.Frame(frm)
        list_frame.grid(row=3, column=0, sticky="nsew")
        list_frame.columnconfigure(0, weight=1)
        list_frame.rowconfigure(0, weight=1)
        self.tree = ttk.Treeview(list_frame, columns=("count",), show="tree headings",
                                 selectmode="browse")
        self.tree.heading("#0", text="Playlists", anchor="w")
        self.tree.heading("count", text="Tracks", anchor="e")
        self.tree.column("#0", width=250, minwidth=140, stretch=True)
        self.tree.column("count", width=100, anchor="e", stretch=False)
        vsb = ttk.Scrollbar(list_frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right", fill="y")
        self.tree.pack(side="left", fill="both", expand=True)
        self.tree.bind("<Double-1>", lambda _e: self.load_selected())
        self.tree.bind("<<TreeviewSelect>>", self._on_select)
        self._row_of_name: dict[str, str] = {}

        buttons = ttk.Frame(frm)
        buttons.grid(row=4, column=0, sticky="w", pady=(10, 0))
        self.load_btn = ttk.Button(buttons, text="Load", style="Accent.TButton",
                                   command=self.load_selected)
        self.save_btn = ttk.Button(buttons, text="Save order", command=self.save_selected)
        self.rename_btn = ttk.Button(buttons, text="Rename", command=self.start_rename)
        self.delete_btn = ttk.Button(buttons, text="Delete", command=self.delete_selected)
        self.export_btn = ttk.Button(buttons, text="Export M3U\u2026", command=self.export_selected)
        self.import_btn = ttk.Button(buttons, text="Import M3U\u2026", command=self.import_m3u)
        for col, widget in enumerate((self.load_btn, self.save_btn, self.rename_btn)):
            widget.grid(row=0, column=col, sticky="w", padx=(0 if col == 0 else 6, 0))
        for col, widget in enumerate((self.delete_btn, self.export_btn, self.import_btn)):
            widget.grid(row=1, column=col, sticky="w", padx=(0 if col == 0 else 6, 0),
                        pady=(6, 0))

        self.add_selection_btn = ttk.Button(buttons, text="Add selected songs",
                                              command=self.add_selected_songs)
        self.add_files_btn = ttk.Button(buttons, text="Add files…", command=self.add_files)
        self.add_selection_btn.grid(row=2, column=0, sticky="w", pady=(6, 0))
        self.add_files_btn.grid(row=2, column=1, sticky="w", padx=(6, 0), pady=(6, 0))
        self.note = ttk.Label(frm, text="", style="Dim.TLabel", background=BG,
                              wraplength=550, justify="left")
        self.note.grid(row=5, column=0, sticky="w", pady=(8, 0))
        ttk.Label(frm, text="Tip: select songs in the queue, then right-click → Add to playlist.",
                  style="Dim.TLabel", background=BG, wraplength=550).grid(
                      row=6, column=0, sticky="w", pady=(8, 0))
        ttk.Button(frm, text="Close", command=self.close).grid(row=7, column=0, sticky="e", pady=(8, 0))

        self._renaming_from = ""
        self.refresh()
        self.name_entry.focus_set()
        self.bind("<Escape>", lambda _e: self.close())
        self.bind("<Return>", self._on_return)
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.grab_set()

    def close(self) -> None:
        """Close the dialog and restore keyboard focus, including without a window manager."""
        app = self.app
        self.destroy()
        if getattr(app, "_playlist_dialog", None) is self:
            app._playlist_dialog = None
        try:
            if app.root.winfo_exists() and app.root.focus_get() is None:
                app.root.focus_set()
                if app.root.focus_get() is None:
                    app.root.focus_force()
        except tk.TclError:
            pass

    def apply_theme(self) -> None:
        self.configure(bg=BG)
        def repaint(parent):
            for widget in parent.winfo_children():
                if isinstance(widget, ttk.Label) and str(widget.cget("background")):
                    widget.configure(background=BG)
                repaint(widget)
        repaint(self)

    def _selected_name(self) -> str:
        selection = self.tree.selection()
        if not selection:
            return ""
        for name, row in self._row_of_name.items():
            if row == selection[0]:
                return name
        return ""

    def refresh(self, select_name: str = "") -> None:
        """Keep the manager's selected destination, independent of the queue."""
        app = self.app
        select_name = select_name or self._selected_name() or app._active_playlist
        self.tree.delete(*self.tree.get_children())
        self._row_of_name.clear()
        for playlist in app._playlists.playlists.values():
            found, missing = app._playlists.resolve(playlist.name, app.tracks)
            count = str(len(found)) + (f" of {len(playlist.paths)}" if missing else "")
            row = self.tree.insert("", "end", text=playlist.name, values=(count,))
            self._row_of_name[playlist.name] = row
            if playlist.name == select_name:
                self.tree.selection_set(row)
                self.tree.see(row)
        self.queue_note.configure(text=f"{len(app.selected_song_paths())} songs selected, "
                                       f"{len(app.queue)} songs in the visible queue")
        self._refresh_buttons()
        self._refresh_note()

    def _on_select(self, _event=None) -> None:
        if self._renaming_from and self._selected_name() != self._renaming_from:
            self._cancel_rename()
        self._refresh_buttons()
        self._refresh_note()

    def _refresh_buttons(self) -> None:
        name = self._selected_name()
        selected = bool(name)
        has_queue = bool(self.app.queue)
        state = ["!disabled"] if has_queue or self._renaming_from else ["disabled"]
        self.save_as_btn.state(state)
        self.new_btn.state(["disabled"] if self._renaming_from else ["!disabled"])
        self.add_files_btn.state(["!disabled"] if selected else ["disabled"])
        self.add_selection_btn.state(["!disabled"] if selected and self.app.selected_song_paths()
                                      else ["disabled"])
        state = ["!disabled"] if selected else ["disabled"]
        for widget in (self.load_btn, self.rename_btn, self.delete_btn, self.export_btn):
            widget.state(state)
        in_playlist = self.app.order_var.get() == library.ORDER_PLAYLIST
        self.save_btn.state(["!disabled"]
                            if (selected and in_playlist
                                and name == self.app._active_playlist)
                            else ["disabled"])

    def _refresh_note(self) -> None:
        app = self.app
        if self._renaming_from:
            self.note.configure(text=f"renaming '{self._renaming_from}': type the new "
                                     f"name, then press 'Rename to\u2026' (Esc cancels)")
            return
        active = app._active_playlist
        if not active:
            self.note.configure(text="Choose a playlist above to add songs or files.")
            return
        playlist = app._playlists.get(active)
        total = len(playlist.paths) if playlist else 0
        found, missing = app._playlists.resolve(active, app.tracks)
        bits = [f"'{active}' is the loaded queue ({len(found)} of {total} tracks)"]
        if missing:
            bits.append(f"{len(missing)} missing on disk")
        if app._playlist_dirty:
            bits.append("the queue order changed - 'Save order' keeps it")
        self.note.configure(text=" \u00b7 ".join(bits))

    def create_empty(self) -> None:
        name = self.app.create_playlist(self.name_var.get())
        if name is not None:
            self.name_var.set("")
            self.refresh(name)

    def add_selected_songs(self) -> None:
        name = self._selected_name()
        paths = self.app.selected_song_paths()
        if name and paths and self.app.add_songs_to_playlist(name, paths):
            self.refresh(name)

    def add_files(self) -> None:
        name = self._selected_name()
        if name and self.app.add_files_to_playlist(name, parent=self):
            self.refresh(name)

    def _on_return(self, _event=None) -> str:
        if self.focus_get() == self.name_entry:
            if self._renaming_from:
                self.apply_save_as()
            else:
                self.create_empty()
        else:
            self.load_selected()
        return "break"

    def apply_save_as(self) -> None:
        name = self.name_var.get()
        app = self.app
        if self._renaming_from:
            old = self._renaming_from
            self._cancel_rename()
            if name.strip().casefold() == old.casefold():
                self.refresh()
                return
            renamed = app.rename_playlist(old, name)
            if renamed is not None:
                self.name_var.set("")
                self.refresh(renamed)
                self.name_entry.focus_set()
            return
        if app.new_playlist(name) is not None:
            self.close()

    def load_selected(self) -> None:
        name = self._selected_name()
        if not name:
            return
        self.app.load_playlist(name)
        self.close()

    def save_selected(self) -> None:
        name = self._selected_name()
        if name and self.app.save_playlist(name):
            self.refresh()

    def start_rename(self) -> None:
        name = self._selected_name()
        if not name:
            return
        self._renaming_from = name
        self.name_var.set(name)
        self.save_as_btn.configure(text="Rename to\u2026")
        self.name_entry.focus_set()
        self._refresh_buttons()
        self._refresh_note()

    def _cancel_rename(self) -> None:
        self._renaming_from = ""
        self.name_var.set("")
        self.save_as_btn.configure(text="Save current queue as\u2026")
        self._refresh_note()

    def delete_selected(self) -> None:
        name = self._selected_name()
        if not name:
            return
        self.app.delete_playlist(name)
        self.refresh()

    def export_selected(self) -> None:
        name = self._selected_name()
        if not name:
            return
        self.app.export_playlist(name, parent=self)
        self.refresh()

    def import_m3u(self) -> None:
        try:
            path = pick_files(
                parent=self, title="Import an M3U playlist",
                initialdir=self.app._directory or self.app.settings.last_picker_dir,
                prefer=self.app.settings.folder_picker, extra_places=self.app._extra_places(),
                filetypes=[("M3U playlists", "*.m3u *.m3u8"), ("All files", "*")])
        except tk.TclError:
            return
        if not path:
            return
        if self.app.import_playlist(path) is not None:
            self.close()


class SettingsDialog(tk.Toplevel):
    def __init__(self, app: "PlayerApp"):
        app._dismiss_playlist_menu()
        super().__init__(app.root)
        self.app = app
        self.title("Settings")
        self.configure(bg=BG)
        self.transient(app.root)
        self.resizable(False, False)
        s = app.settings

        def row(parent, r, label, widget, hint=""):
            ttk.Label(parent, text=label).grid(row=r, column=0, sticky="w", padx=(0, 12), pady=3)
            widget.grid(row=r, column=1, sticky="ew", pady=3)
            if hint:
                ttk.Label(parent, text=hint, style="Dim.TLabel", background=BG).grid(
                    row=r, column=2, sticky="w", padx=(8, 0), pady=3)
            return widget

        frm = ttk.Frame(self, padding=14)
        frm.grid(row=0, column=0, sticky="nsew")
        frm.columnconfigure(1, weight=1)

        self.backend = tk.StringVar(value=s.backend)
        row(frm, 0, "Audio backend", ttk.Combobox(
            frm, textvariable=self.backend, state="readonly", width=14,
            values=("auto", "sounddevice", "soundcard", "null")),
            "auto picks the first working one")

        self.sample_rate = tk.StringVar(value=SAMPLE_RATE_LABELS[normalise_sample_rate(s.samplerate)])
        self.sample_rate_combo = ttk.Combobox(frm, textvariable=self.sample_rate, state="readonly",
                                              width=20, values=tuple(SAMPLE_RATE_LABELS.values()))
        row(frm, 1, "Output sample rate", self.sample_rate_combo,
            "applies on Save, briefly restarts audio (device support varies)")

        self.buffer = tk.IntVar(value=s.buffer_ms)
        row(frm, 3, "Buffer (ms)", ttk.Spinbox(frm, from_=40, to=1000, increment=20,
                                               textvariable=self.buffer, width=8),
            "how much audio is rendered ahead")
        self.latency = tk.IntVar(value=s.latency_ms)
        row(frm, 4, "Device latency (ms)", ttk.Spinbox(frm, from_=20, to=500, increment=10,
                                                       textvariable=self.latency, width=8))

        ttk.Separator(frm, orient="horizontal").grid(row=5, column=0, columnspan=3,
                                                     sticky="ew", pady=10)
        ttk.Label(frm, text="Robustness", style="Head.TLabel").grid(row=6, column=0, sticky="w")

        self.stall = tk.DoubleVar(value=s.stall_timeout)
        row(frm, 7, "Stall warning (s)", ttk.Spinbox(frm, from_=0, to=600, increment=5,
                                                     textvariable=self.stall, width=8),
            "no pattern progress for this long causes warning")
        self.silence = tk.DoubleVar(value=s.silence_stall_timeout)
        row(frm, 8, "Silent stall (s)", ttk.Spinbox(frm, from_=0, to=600, increment=5,
                                                    textvariable=self.silence, width=8),
            "silent and stuck for this long causes warning")
        self.hang = tk.DoubleVar(value=s.hang_timeout)
        row(frm, 9, "Audio hang (s)", ttk.Spinbox(frm, from_=2, to=120, increment=1,
                                                  textvariable=self.hang, width=8),
            "restart wedged render thread after this long")
        self.restarts = tk.IntVar(value=s.max_restarts)
        row(frm, 10, "Restart budget", ttk.Spinbox(frm, from_=0, to=20, textvariable=self.restarts,
                                                   width=8),
            "per track, before it is marked unplayable")

        self.interp = tk.StringVar(
            value=INTERPOLATION_LABELS.get(s.interpolation,
                                           INTERPOLATION_LABELS[DEFAULT_INTERPOLATION]))
        combo = ttk.Combobox(frm, textvariable=self.interp, state="readonly", width=16,
                             values=tuple(INTERPOLATION_LABELS.values()))
        combo.bind("<<ComboboxSelected>>", lambda _e: self.preview_interpolation())
        self.interp_combo = combo
        row(frm, 2, "Resampler quality", combo)
        self.interp_hint = ttk.Label(frm, text=INTERPOLATION_HINTS[self.interpolation_mode()],
                                      style="Dim.TLabel", background=BG)
        self.interp_hint.grid(row=2, column=2, sticky="w", padx=(8, 0), pady=3)

        self.picker = tk.StringVar(value=s.folder_picker)
        row(frm, 12, "File / folder picker", ttk.Combobox(
            frm, textvariable=self.picker, state="readonly", width=14,
            values=("built-in", "system")),
            "built-in themed browser or default OS browser")

        self.title_mode = tk.StringVar(
            value=TITLE_MODE_LABELS.get(s.window_title, TITLE_MODE_LABELS["track"]))
        row(frm, 13, "Window title", ttk.Combobox(
            frm, textvariable=self.title_mode, state="readonly", width=14,
            values=tuple(TITLE_MODE_LABELS.values())),
            "what the title bar shows while a module is loaded")

        self.auto_skip = tk.BooleanVar(value=s.auto_skip_broken)
        row(frm, 14, "Auto-skip broken tracks", ttk.Checkbutton(frm, variable=self.auto_skip), "")
        self.overrun = tk.BooleanVar(value=s.overrun_guard)
        row(frm, 15, "Guard against never-ending songs",
            ttk.Checkbutton(frm, variable=self.overrun),
            "skip modules that run far past their length")

        self.update_rate = tk.StringVar(value=UI_RATE_LABELS.get(
            self.app.settings.ui_fps, UI_RATE_LABELS[UI_FPS_DEFAULT]))
        row(frm, 11, "Update rate", ttk.Combobox(
            frm, textvariable=self.update_rate, state="readonly", width=16,
            values=tuple(UI_RATE_LABELS.values())),
            "how often the visuals redraw")

        ttk.Separator(frm, orient="horizontal").grid(row=16, column=0, columnspan=3,
                                                     sticky="ew", pady=10)
        ttk.Label(frm, text="Playback", style="Head.TLabel").grid(row=17, column=0, sticky="w")
        self.auto_advance = tk.BooleanVar(value=s.auto_advance)
        row(frm, 18, "Play next track automatically",
            ttk.Checkbutton(frm, variable=self.auto_advance), "")
        self.loop_queue = tk.BooleanVar(value=s.loop_queue)
        row(frm, 19, "Repeat the queue when it ends",
            ttk.Checkbutton(frm, variable=self.loop_queue),
            "shuffle mode draws a new order each round")

        ttk.Separator(frm, orient="horizontal").grid(row=20, column=0, columnspan=3,
                                                     sticky="ew", pady=10)
        ttk.Label(frm, text="Between runs", style="Head.TLabel").grid(row=21, column=0,
                                                                      sticky="w")
        self.remember = tk.BooleanVar(value=s.remember_position)
        row(frm, 22, "Pick up where you left off",
            ttk.Checkbutton(frm, variable=self.remember),
            "open the last module again, paused where it stopped")
        self.keep_info = tk.BooleanVar(value=s.cache_analysis)
        row(frm, 23, "Keep module details",
            ttk.Checkbutton(frm, variable=self.keep_info),
            "lengths, formats and channels are read once, not every start")

        self.keep_stats = tk.BooleanVar(value=s.track_listening_stats)
        row(frm, 24, "Keep listening stats", ttk.Checkbutton(frm, variable=self.keep_stats),
            "off stops recording, existing history is kept")

        ttk.Separator(frm, orient="horizontal").grid(row=25, column=0, columnspan=3,
                                                     sticky="ew", pady=10)
        ttk.Label(frm, text="Appearance", style="Head.TLabel").grid(row=26, column=0, sticky="w")
        self.theme = tk.StringVar(value=theme.label(s.theme))
        row(frm, 27, "Colour scheme", ttk.Combobox(
            frm, textvariable=self.theme, state="readonly", width=16,
            values=tuple(theme.THEME_LABELS[name] for name in theme.THEME_NAMES)),
            "applies when you press Save")

        self.smooth_scroll = tk.BooleanVar(value=s.smooth_tracker_scrolling)
        row(frm, 28, "Smooth tracker scrolling", ttk.Checkbutton(frm, variable=self.smooth_scroll),
            "glide the current row while following")

        buttons = ttk.Frame(frm)
        buttons.grid(row=29, column=0, columnspan=3, sticky="e", pady=(14, 0))
        ttk.Button(buttons, text="Cancel", command=self.cancel).pack(side="right", padx=(8, 0))
        ttk.Button(buttons, text="Save", style="Accent.TButton",
                   command=self.save).pack(side="right")

        self.bind("<Escape>", lambda _e: self.cancel())
        self.protocol("WM_DELETE_WINDOW", self.cancel)
        # what was playing when the dialog opened, so Cancel can put it back
        self._interp_was = s.interpolation

    def destroy(self) -> None:
        super().destroy()
        # Release Tk objects on the UI thread.
        self.sample_rate_combo = None
        self.interp_combo = None
        self.interp_hint = None

    def interpolation_mode(self) -> str:
        """The resampling filter the row currently shows."""
        return INTERPOLATION_VALUES.get(self.interp.get(), DEFAULT_INTERPOLATION)

    def preview_interpolation(self) -> None:
        """Apply the filter straight away - that is how one A/Bs them."""
        mode = self.interpolation_mode()
        self.app.apply_interpolation(mode)
        self.interp_hint.configure(text=INTERPOLATION_HINTS[mode])

    def cancel(self) -> None:
        self.app.apply_interpolation(self._interp_was)
        self.app._save_settings()  # undo a preview also saved by a session checkpoint
        self.destroy()

    def rate_value(self) -> int:
        """The refresh rate the Update rate row shows, as a number."""
        return UI_RATE_VALUES.get(self.update_rate.get(), UI_FPS_DEFAULT)

    def title_mode_values(self) -> tuple[str, ...]:
        """The user-visible choices of the "Window title" row."""
        return tuple(TITLE_MODE_LABELS.values())

    def theme_name(self) -> str:
        """The palette key the Colour scheme row currently shows."""
        shown = self.theme.get()
        for key in theme.THEME_NAMES:
            if theme.THEME_LABELS[key] == shown:
                return key
        return self.app.settings.theme

    def save(self) -> None:
        s = self.app.settings
        old_audio = (s.backend, s.samplerate, s.buffer_ms, s.latency_ms)
        kept: list[str] = []

        def number(variable, label: str, default: float) -> float:
            value = spin_number(variable)
            if value is None:
                kept.append(label)
                return float(default)
            return value

        s.backend = self.backend.get()
        s.samplerate = SAMPLE_RATE_VALUES.get(self.sample_rate.get(), old_audio[1])
        s.buffer_ms = max(20, int(number(self.buffer, "Buffer", s.buffer_ms)))
        s.latency_ms = max(10, int(number(self.latency, "Device latency", s.latency_ms)))
        if (
            (old_audio != (s.backend, s.samplerate, s.buffer_ms, s.latency_ms))
            and (not self.app.restart_audio(previous_audio=old_audio))
        ):
            self.backend.set(s.backend)
            self.sample_rate.set(SAMPLE_RATE_LABELS[normalise_sample_rate(s.samplerate)])
            self.buffer.set(s.buffer_ms)
            self.latency.set(s.latency_ms)
            return  # keep the dialog open; no failed audio preference is saved
        s.stall_timeout = number(self.stall, "Stall warning", s.stall_timeout)
        s.silence_stall_timeout = number(self.silence, "Silent stall",
                                         s.silence_stall_timeout)
        s.hang_timeout = number(self.hang, "Audio hang", s.hang_timeout)
        s.max_restarts = int(number(self.restarts, "Restart budget", s.max_restarts))
        s.folder_picker = self.picker.get()
        s.window_title = TITLE_MODE_VALUES.get(self.title_mode.get(), "track")
        rate_changed = self.rate_value() != s.ui_fps
        s.ui_fps = self.rate_value()
        if rate_changed:
            self.app.apply_update_rate()      # applies at once: the tick is re-armed
        self.app.apply_interpolation(self.interpolation_mode())
        s.auto_skip_broken = bool(self.auto_skip.get())
        s.overrun_guard = bool(self.overrun.get())
        s.auto_advance = bool(self.auto_advance.get())
        s.loop_queue = bool(self.loop_queue.get())
        s.remember_position = bool(self.remember.get())
        s.cache_analysis = bool(self.keep_info.get())
        s.theme = self.app.apply_theme_name(self.theme_name())
        s.smooth_tracker_scrolling = bool(self.smooth_scroll.get())
        self.app._tracker.set_smooth_scrolling(s.smooth_tracker_scrolling)
        self.app.apply_listening_stats(self.keep_stats.get())
        self.app._save_settings()
        self.app.sync_setting_toggles()
        engine = self.app.engine
        engine.settings = s
        engine.buffer_ms = s.buffer_ms
        if kept:
            self.app.status(f"settings saved, kept the old value for "
                            f"{', '.join(kept)} (not a number)")
        else:
            self.app.status("settings saved")
        self.destroy()


class PlayerApp:
    def __init__(self, root: tk.Tk, settings: Optional[Settings] = None,
                 directory: str = "", backend: Optional[str] = None,
                 speed: float = 1.0, autoplay: bool = False,
                 config_file: Optional[str] = None):
        self.root = root
        self.settings = settings or Settings.load(config_file)
        self.config_file = config_file          # None -> the standard location
        self.queue_ui: queue.Queue = queue.Queue()   # background thread -> UI
        self.tracks: list[Track] = []
        self.queue: list[Track] = []                 # ordered playback queue
        self.queue_index = -1
        self._playing_path = ""                      # path of the running track
        self._reveal_enabled = False                 # "Show in folder" button state
        self._reveal_pending = False                 # a file manager call is in flight
        self._shuffle_paths: list[str] = []          # stored shuffle order
        self._shuffle_seed = int(self.settings.shuffle_seed) or 0
        self._row_of_path: dict[str, str] = {}
        self._base_tags: dict[str, tuple] = {}
        self.paned: Optional[ttk.Panedwindow] = None   # set while building the body
        self._opening_sash_ratio: Optional[float] = None   # the split as it opened
        self._playing_row: Optional[str] = None
        self._scan_thread: Optional[threading.Thread] = None
        self._scan_cancel = threading.Event()
        self._analyzer: Optional[Analyzer] = None
        self.analysis_cache: Optional[AnalysisCache] = None
        if self.settings.cache_analysis:
            self.analysis_cache = AnalysisCache(self._cache_file())
        self._cache_loaded = False           # the file is read by the scan thread
        self._session_restored = False       # restore the last song once per run
        self._session_state: tuple = ("", 0.0)
        self._session_saved_at = 0.0
        self._updating = False
        self._seeking = False
        self._last_snapshot = None
        self._status_text = ""
        self._finished_countdown: Optional[float] = None
        self._tick_id: Optional[str] = None
        # last text written per widget (the tick writes only what changed)
        self._last_text: dict[str, str] = {}
        # the panel content the last height measurement was taken for
        self._panel_signature_done: Optional[tuple] = None
        # the enabled/disabled state already applied to the transport buttons
        self._control_state: dict[str, bool] = {}
        self._subsong_to: Optional[int] = None
        self._subsong_state = ""
        self._song_info: Optional[SongInfoWindow] = None   # the names / comment window
        self._playlist_dialog: Optional[PlaylistsDialog] = None   # the saved-queue dialog
        self._new_playlist_dialog: Optional[NewPlaylistDialog] = None
        self._queue_menu: Optional[tk.Menu] = None
        self._menu_focus_check = None
        self._playlists = PlaylistStore(playlists_path(self.config_file))
        self._listening = ListeningCounter(StatsStore(stats_path(self.config_file)),
                                           enabled=self.settings.track_listening_stats)
        self._stats_window: Optional[StatsWindow] = None
        self._stats_warning = ""
        self._active_playlist = str(self.settings.active_playlist or "")
        self._playlist_paths: list[str] = []     # the loaded playlist's order (editable)
        self._playlist_dirty = False             # ...changed since it was last saved
        self._drag_row = ""                      # the queue row a drag has grabbed
        self._drag_moved = False
        self._drag_origin = (0, 0)
        self._closing = False
        self._directory = ""

        self.settings.theme = theme.activate(self.settings.theme)
        apply_theme(root)
        enable_select_all(root)
        root.title(APP_TITLE)
        self._window_title = APP_TITLE
        self._now_playing = False           # is the title bar showing a module?
        self._min_size: tuple[int, int] = (0, 0)   # last minsize we applied
        root.minsize(640, 400)
        if self.settings.window_geometry:
            try:
                root.geometry(self.settings.window_geometry)
            except tk.TclError:
                pass
        else:
            root.geometry("1200x740")

        self._create_pages()
        self._build_toolbar()
        self._build_transport()
        self._build_statusbar()
        self._build_body()
        self._build_tracker_page()
        self._build_tab_strip()
        self._bind_keys()

        self.root.after_idle(self._refresh_min_size)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.engine: PlaybackEngine
        self._create_engine(backend=backend or self.settings.backend, speed=speed)

        start_dir = directory or self.settings.last_directory
        if start_dir and os.path.isdir(start_dir):
            self.root.after(80, lambda: self.load_directory(start_dir, autoplay=autoplay))
        elif self.settings.queue_mode == library.ORDER_PLAYLIST:
            self.rebuild_queue()
            if autoplay and self.queue:
                self._play_index(0)
            elif not self._restore_session():
                self.status("Playlist ready - select a song and press Play, or add files via Playlists")
        else:
            self.status("choose a folder, or open Playlists to add files")

        self.root.after(60, self._tick)

    def _create_engine(self, backend: str, speed: float = 1.0) -> None:
        try:
            self.engine = PlaybackEngine(self.settings, backend=backend, speed=speed)
        except AudioError as exc:
            messagebox.showerror("No audio output", str(exc))
            self.engine = PlaybackEngine(self.settings, backend="null", speed=speed)
        self.log(MSG_INFO, f"libopenmpt {get_lib().version_string}, "
                           f"output {self.engine.output.name} @ {self.engine.samplerate} Hz")
        if self.engine.output.name == "null":
            self.log(MSG_WARN, "no sound card available: running with silent output")

    def restart_audio(self, previous_audio=None) -> bool:
        """Recreate the output clock while preserving playback state; restore old preferences if
        the change fails."""
        old = self.engine
        snap = old.snapshot()
        self._record_listening(snap)
        self._listening.continue_output(snap.path, snap.play_id)
        s = self.settings
        previous_audio = previous_audio or (old.backend_name, old._samplerate or 0,
                                             old.buffer_ms, s.latency_ms)
        old_backend = old.output.name
        kwargs = dict(blocksize=old.blocksize, speed=old.speed, device=old.device,
                      metering=old.metering)
        old.shutdown()
        error = ""
        try:
            candidate = PlaybackEngine(s, backend=s.backend, **kwargs)
            if candidate.output.name == "null" and s.backend != "null":
                candidate.shutdown()
                raise AudioError("No audio backend accepted these output settings")
        except AudioError as exc:
            error = str(exc)
            s.backend, s.samplerate, s.buffer_ms, s.latency_ms = previous_audio
            try:
                candidate = PlaybackEngine(s, backend=old_backend, **kwargs)
            except AudioError as restore_error:
                candidate = PlaybackEngine(s, backend="null", **kwargs)
                error += f"\nThe previous device also failed: {restore_error}. Running silently."
        self.engine = candidate
        self._last_snapshot = None
        self._song_key = None
        self._song_token += 1
        self._finished_countdown = None
        if snap.path and (snap.loaded or snap.loading):
            position = max(0.0, snap.position)
            if snap.loop and snap.duration_valid and snap.duration > 0:
                position %= snap.duration  # a seek addresses one pass, not the loop counter
            candidate.play_path(snap.path, position=position,
                                paused=not snap.playing, loop=s.loop_track, subsong=snap.subsong)
        if error:
            self.log(MSG_ERROR, "Audio settings not applied: " + error)
            self.status("Audio settings were not applied; previous preferences kept")
            messagebox.showerror("Audio settings not applied", error +
                                 "\n\nPrevious audio preferences have been restored.", parent=self.root)
            return False
        self.log(MSG_INFO, f"audio output: {candidate.output.name} @ {candidate.samplerate} Hz")
        return True

    def _build_toolbar(self) -> None:
        bar = ttk.Frame(self._page_player, padding=(10, 6, 10, 4))
        bar.pack(side="top", fill="x")
        self._toolbar_bar = bar
        self.open_button = ttk.Button(bar, text="Open folder…", style="Accent.TButton",
                                      command=self.choose_directory)
        self.open_button.pack(side="left")
        self.rescan_button = ttk.Button(bar, text="Rescan",
                                        command=lambda: self.load_directory(self._directory)
                                        if self._directory else self.choose_directory())
        self.rescan_button.pack(side="left", padx=(6, 0))
        self.analyze_button = ttk.Button(bar, text="Analyze", command=self.analyze_library)
        self.analyze_button.pack(side="left", padx=(6, 0))

        self.count_label = ttk.Label(bar, text="", style="Dim.TLabel", background=BG)
        self.count_label.pack(side="right", padx=10)
        self.path_var = tk.StringVar(value="")
        self.dir_entry = ttk.Entry(bar, textvariable=self.path_var, state="readonly",
                                   style="Path.TEntry", width=10)
        self.dir_entry.pack(side="left", fill="x", expand=True, padx=12)
        self.dir_entry.bind("<Button-3>", self._path_menu)
        self.dir_entry.bind("<Button-2>", self._path_menu)
        self.dir_entry.bind("<Control-c>", self.copy_path)
        self.dir_entry.bind("<Enter>", lambda _e: self.status(
            f"library: {self.path_var.get()} (Ctrl+A: select, Ctrl+C: copy, right-click: more)"), add="+")
        show_path_tail(self.dir_entry)      # keep the folder name in view
        self.add_hint(self.dir_entry,
                      "current library (Ctrl+A: select, Ctrl+C: copy, "
                      "right-click: Copy path / Change folder...)")

        bar2 = ttk.Frame(self._page_player, padding=(10, 0, 10, 6))
        bar2.pack(side="top", fill="x")
        self._queue_bar = bar2
        ttk.Label(bar2, text="Queue:").pack(side="left")
        self.order_var = tk.StringVar(value=self.settings.queue_mode)
        self.order_radios: dict[str, ttk.Radiobutton] = {}
        for mode in library.ORDER_MODES + (library.ORDER_PLAYLIST,):
            self.order_radios[mode] = ttk.Radiobutton(
                bar2, text=mode.capitalize(), value=mode, variable=self.order_var,
                command=self.on_order_changed)
            self.order_radios[mode].pack(side="left", padx=(8, 0))
        self.add_hint(self.order_radios[library.ORDER_PLAYLIST], "Queue the loaded playlist")
        self.shuffle_now_btn = ttk.Button(bar2, text="Shuffle now", command=self.reshuffle)
        self.shuffle_now_btn.pack(side="left", padx=(10, 0))
        self.playlists_btn = ttk.Button(bar2, text="Playlists",
                                        command=self.open_playlists_dialog)
        self.playlists_btn.pack(side="left", padx=(8, 0))
        self.filter_btn = ttk.Button(bar2, text="Filter", command=self.open_filter_dialog)
        self.filter_btn.pack(side="left", padx=(8, 0))
        self.add_hint(self.filter_btn,
                      "Filter (Ctrl+Shift+F): file formats, length range, hide modules that cannot be played")
        self.add_hint(self.playlists_btn,
                      "Playlists (Ctrl+P): save the queue as a new playlist or create an empty one")

        ttk.Label(bar2, text="Search:").pack(side="left", padx=(18, 4))
        self.search_var = tk.StringVar()
        self.search_var.trace_add("write", lambda *_: self.rebuild_queue(keep_playing=True))
        self.filter_label = ttk.Label(bar2, text="", style="Dim.TLabel", background=BG)
        self.filter_label.pack(side="right", padx=8)
        entry = ttk.Entry(bar2, textvariable=self.search_var, width=12)
        entry.pack(side="left", fill="x", expand=True)
        self.search_entry = entry

    def _create_pages(self) -> None:
        """The two tab pages.  They are filled before they are packed, so that
        the widget stack (and with it the layout) is decided in one place."""
        self._pages = ttk.Frame(self.root)
        self._page_player = ttk.Frame(self._pages)
        self._page_tracker = ttk.Frame(self._pages)
        self._tab = "player"

    def _build_tab_strip(self) -> None:
        """Place tabs above the pages, keeping transport and status controls outside them."""
        bar = ttk.Frame(self.root, style="Bar.TFrame", padding=(10, 5, 10, 0))
        bar.pack(side="top", fill="x")
        self._tab_bar = bar
        self.tab_buttons: dict[str, ttk.Button] = {}
        for name, label in (("player", "Player"), ("tracker", "Tracker")):
            button = ttk.Button(bar, text=label, style="Tab.TButton",
                                command=lambda key=name: self.select_tab(key))
            button.pack(side="left")
            self.tab_buttons[name] = button
        self.add_hint(self.tab_buttons["player"],
                      "Player: choose the song and view song information")
        self.add_hint(self.tab_buttons["tracker"],
                      "Tracker: the pattern view")
        self.settings_button = ttk.Button(bar, text="Settings",
                                          command=lambda: SettingsDialog(self))
        self.settings_button.pack(side="right")
        self.stats_btn = ttk.Button(bar, text="Listening stats", command=self.open_stats)
        self.stats_btn.pack(side="right", padx=(0, 6))
        self.add_hint(self.stats_btn, "Listening stats (Ctrl+H): local play counts, time and most-played modules")
        self._pages.pack(side="top", fill="both", expand=True)
        self._page_player.pack(fill="both", expand=True)
        self.tab_buttons["player"].configure(style="TabActive.TButton")

    def select_tab(self, name: str) -> None:
        """Show one page (playback carries on across the switch)."""
        if name not in ("player", "tracker") or name == self._tab:
            return
        page = self._page_player if name == "player" else self._page_tracker
        other = self._page_tracker if name == "player" else self._page_player
        other.pack_forget()
        page.pack(fill="both", expand=True)
        self._tab = name
        for key, button in self.tab_buttons.items():
            button.configure(style="TabActive.TButton" if key == name else "Tab.TButton")
        self._tracker.set_visible(name == "tracker")
        if name == "tracker":
            self._tracker_seen = True
            self._sync_tracker_song()
            self._tracker.request_visible()
        self.root.after_idle(self._refresh_min_size)

    def toggle_tracker_tab(self) -> None:
        """Ctrl+T: flip between the queue and the pattern view."""
        self.select_tab("player" if self._tab == "tracker" else "tracker")

    def _build_body(self) -> None:
        body = ttk.Frame(self._page_player, padding=(10, 4, 10, 4))
        body.pack(side="top", fill="both", expand=True)
        paned = ttk.Panedwindow(body, orient="horizontal")
        paned.pack(fill="both", expand=True)
        self.paned = paned

        left = ttk.Frame(paned)
        paned.add(left, weight=QUEUE_PANE_WEIGHT)
        columns = ("folder", "dur", "fmt", "ch")
        # Selection actions live beside the songs, not inside the manager.
        actions = ttk.Frame(left)
        actions.pack(side="top", fill="x", pady=(0, 5))
        self.add_to_playlist_btn = ttk.Button(actions, text="Add to playlist…",
                                               command=self.show_add_to_playlist_menu)
        self.add_to_playlist_btn.pack(side="left")
        self.remove_from_playlist_btn = ttk.Button(actions, text="Remove from playlist",
                                                   command=self.remove_selected_from_playlist)
        self.remove_from_playlist_btn.pack(side="left", padx=6)
        self.tree = ttk.Treeview(left, columns=columns, show="tree headings", selectmode="extended")
        self.tree.heading("#0", text="Module", anchor="w")
        self.tree.heading("folder", text="Folder", anchor="w")
        self.tree.heading("dur", text="Len", anchor="e")
        self.tree.heading("fmt", text="Fmt", anchor="w")
        self.tree.heading("ch", text="Ch", anchor="e")
        self.tree.column("#0", width=QUEUE_COLUMN_DEFAULTS["#0"], minwidth=140, stretch=True)
        self.tree.column("folder", width=QUEUE_COLUMN_DEFAULTS["folder"],
                         minwidth=70, stretch=False)
        self.tree.column("dur", width=QUEUE_COLUMN_DEFAULTS["dur"],
                         minwidth=48, anchor="e", stretch=False)
        self.tree.column("fmt", width=QUEUE_COLUMN_DEFAULTS["fmt"],
                         minwidth=40, stretch=False)
        self.tree.column("ch", width=QUEUE_COLUMN_DEFAULTS["ch"],
                         minwidth=28, anchor="e", stretch=False)
        vsb = ttk.Scrollbar(left, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right", fill="y")
        self.tree.pack(side="left", fill="both", expand=True)
        self.configure_tree_tags()
        self.tree.bind("<Double-1>", self._on_tree_double_click)
        self.tree.bind("<Return>", lambda _e: self.play_selected())
        self.tree.bind("<<TreeviewSelect>>", lambda _e: self._selection_actions())
        self.tree.bind("<Button-3>", self._queue_context_menu)
        self.tree.bind("<Shift-F10>", self._queue_context_menu)
        self.tree.bind("<Delete>", lambda _e: self.remove_selected_from_playlist() or "break")
        self.tree.bind("<Control-a>", self._select_all_songs)
        self._selection_actions()
        self.tree.bind("<Configure>", lambda _e: self.sync_column_layout(), add="+")
        self.tree.bind("<ButtonRelease-1>", lambda _e: self.sync_column_layout(), add="+")
        self.tree.bind("<Motion>", self._on_tree_motion, add="+")
        self.tree.bind("<Button-1>", self._tree_drag_start, add="+")
        self.tree.bind("<B1-Motion>", self._tree_drag_motion, add="+")
        self.tree.bind("<ButtonRelease-1>", self._tree_drag_end, add="+")

        right = ttk.Frame(paned, style="Panel.TFrame", padding=12)
        paned.add(right, weight=INFO_PANE_WEIGHT)
        self._build_info_panel(right)

    def reset_layout(self) -> None:
        """Reset column widths, pane split, and scrolling; keep window size and queue order."""
        if self._closing:
            return
        try:
            for column, width in QUEUE_COLUMN_DEFAULTS.items():
                self.tree.column(column, width=width)
            self._restore_pane_split()
            self.root.update_idletasks()
            self.sync_column_layout()          # the Module column still gives its slack
            self.tree.yview_moveto(0.0)        # and the queue starts at the top again
        except tk.TclError:                    # closing
            return
        self.status("layout reset - column widths, the pane split and scrolling are "
                    "back to how the window opened (Ctrl+0)")

    def _restore_pane_split(self) -> None:
        """Put the bar back where this window had it when it opened."""
        paned = self.paned
        if paned is None:
            return
        width = paned.winfo_width()
        if width <= 1:
            return
        ratio = self._opening_sash_ratio
        if ratio is None:                      # not measured yet (very early)
            ratio = QUEUE_PANE_WEIGHT / (QUEUE_PANE_WEIGHT + INFO_PANE_WEIGHT)
        position = int(round(ratio * width))
        paned.sashpos(0, max(MIN_PANE_WIDTH, min(position, width - MIN_PANE_WIDTH)))

    def _remember_opening_split(self) -> None:
        """Note the proportion the window opened with (called from the UI tick)."""
        if self._opening_sash_ratio is not None or self.paned is None or self._closing:
            return
        try:
            width = self.paned.winfo_width()
            sash = self.paned.sashpos(0)
        except tk.TclError:
            return
        if width > MIN_PANE_WIDTH * 2 and sash > 0:     # laid out, not mid-build
            self._opening_sash_ratio = sash / width

    def _build_tracker_page(self) -> None:
        """The pattern view (the "Tracker" tab)."""
        self._tracker = TrackerView(self._page_tracker,
                                    request_pattern=self._request_pattern,
                                    smooth_scrolling=self.settings.smooth_tracker_scrolling)
        self._tracker.pack(fill="both", expand=True, padx=10, pady=(4, 6))
        self._tracker_seen = False          # pattern data is fetched on demand
        self._song_token = 0

    def _request_pattern(self, pattern: int) -> None:
        """Request pattern data from the worker, tagging it so replies for old songs are ignored."""
        try:
            self.engine.request_pattern(int(pattern),
                                        channels=self._tracker.channels or None,
                                        token=self._song_token)
        except Exception as exc:              # engine gone: nothing to draw
            self.log(MSG_DEBUG, f"pattern request ignored: {exc!r}")

    def _sync_tracker_song(self) -> None:
        """Ask for the order list when the module (or the subsong) changed."""
        snapshot = self.snapshot()
        path = snapshot.path if snapshot else ""
        key = (path, getattr(snapshot, "subsong", 0) if snapshot else 0)
        if not path:
            self._tracker.clear()
            self._song_key = None
            return
        if key == getattr(self, "_song_key", None):
            return
        self._song_key = key
        self._song_token += 1
        try:
            self.engine.request_song(token=self._song_token)
        except Exception:
            pass

    def snapshot(self):
        """The latest engine snapshot (the tick keeps one fresh)."""
        snap = getattr(self, "_last_snapshot", None)
        if snap is None:
            try:
                snap = self.engine.snapshot()
                self._last_snapshot = snap
            except Exception:
                return None
        return snap

    def _update_tracker(self, snap) -> None:
        """Follow the output clock; render-ahead snapshots drive transport only."""
        if not self._tracker_seen:
            return                          # nobody opened it: no work, no requests
        if not (snap and snap.path):
            if self._tracker.path:
                self._tracker.clear()
            self._song_key = None
            return
        self._sync_tracker_song()
        audible = self.engine.tracker_snapshot(snap)
        running = ((audible.playing or audible.ended)
                   and not (audible.paused or audible.finished or audible.failed))
        self._tracker.set_position(audible.order, audible.row, running)

    def _build_info_panel(self, parent: ttk.Frame) -> None:
        self.title_label = ttk.Label(parent, text="Nothing playing", style="Title.TLabel",
                                     wraplength=320)
        self.title_label.pack(anchor="w")
        self.subtitle_label = ttk.Label(parent, text="double-click a module to play it",
                                        style="Dim.TLabel", wraplength=320)
        self.subtitle_label.pack(anchor="w", pady=(2, 8))

        actions = ttk.Frame(parent, style="Panel.TFrame")
        actions.pack(fill="x", pady=(0, 10))
        self.reveal_btn = ttk.Button(actions, text="Show in folder", state="disabled",
                                     command=self.reveal_current)
        self.reveal_btn.pack(side="left")
        self.song_info_btn = ttk.Button(actions, text="Song info", command=self.open_song_info)
        self.song_info_btn.pack(side="left", padx=(6, 0))

        grid = ttk.Frame(parent, style="Panel.TFrame")
        grid.pack(fill="x")
        grid.columnconfigure(1, weight=1)
        self.info_labels: dict[str, ttk.Label] = {}
        rows = [
            ("format", "Format"), ("tracker", "Tracker"), ("artist", "Artist"),
            ("size", "Module"), ("length", "Length"), ("position", "Position"),
            ("sequencer", "Sequencer"), ("resample", "Resampling"),
            ("output", "Output"),
        ]
        for r, (key, label) in enumerate(rows):
            ttk.Label(grid, text=label, style="Dim.TLabel", width=10, anchor="w").grid(
                row=r, column=0, sticky="w", pady=1)
            val = ttk.Label(grid, text="-", style="Panel.TLabel", anchor="w", wraplength=230)
            val.grid(row=r, column=1, sticky="w", pady=1)
            self.info_labels[key] = val

        self.channels_label = ttk.Label(parent, text="Channels", style="Dim.TLabel")
        self.channels_label.pack(anchor="w", pady=(12, 2))
        self.vu = VuMeter(parent, width=330, height=74)
        self.vu.pack(fill="x")

        subs = ttk.Frame(parent, style="Panel.TFrame")
        subs.pack(fill="x", pady=(10, 0))
        ttk.Label(subs, text="Subsong", style="Dim.TLabel").pack(side="left")
        self.subsong_var = tk.StringVar(value="0")
        self.subsong_spin = ttk.Spinbox(subs, from_=0, to=0, width=4, textvariable=self.subsong_var,
                                        command=self._on_subsong, state="disabled")
        self.subsong_spin.pack(side="left", padx=6)
        self.subsong_label = ttk.Label(subs, text="(single song)", style="Dim.TLabel")
        self.subsong_label.pack(side="left")
        self.loop_label = ttk.Label(subs, text="", style="Dim.TLabel")
        self.loop_label.pack(side="right")

        self.log_label = ttk.Label(parent, text="Log", style="Dim.TLabel")
        self.log_label.pack(anchor="w", pady=(12, 2))
        logframe = ttk.Frame(parent, style="Panel.TFrame")
        logframe.pack(fill="both", expand=True)
        self.log_frame = logframe
        self.log_text = tk.Text(logframe, height=4, bg=BG_INPUT, fg=FG_DIM, insertbackground=FG,
                                relief="flat", wrap="word", font=("TkDefaultFont", 8),
                                padx=6, pady=4, highlightthickness=0,
                                selectbackground=ACCENT_DIM, selectforeground=ON_ACCENT,
                                inactiveselectbackground=ACCENT_DIM)
        logsb = ttk.Scrollbar(logframe, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=logsb.set, state="disabled")
        logsb.pack(side="right", fill="y")
        self.log_text.pack(side="left", fill="both", expand=True)
        self.configure_log_tags()

        self._panel_stack = [
            (self.title_label, {"anchor": "w"}, None),
            (self.subtitle_label, {"anchor": "w", "pady": (2, 8)}, None),
            (actions, {"fill": "x", "pady": (0, 10)}, None),
            (grid, {"fill": "x"}, None),
            (self.channels_label, {"anchor": "w", "pady": (12, 2)}, "vu"),
            (self.vu, {"fill": "x"}, "vu"),
            (subs, {"fill": "x", "pady": (10, 0)}, "subsong"),
            (self.log_label, {"anchor": "w", "pady": (12, 2)}, "log"),
            (self.log_frame, {"fill": "both", "expand": True}, "log"),
        ]
        self._panel_parent = parent
        self._panel_shows = (True, True, True)    # (log, vu, subsong) packed
        self._panel_need = self._measure_panel_heights()
        parent.bind("<Configure>", self._on_panel_configure, add="+")

    def _panel_relayout(self, show_log: bool, show_vu: bool, show_subsong: bool = True) -> None:
        """(Re)pack the panel's stack, with the optional sections on or off."""
        if (show_log, show_vu, show_subsong) == self._panel_shows:
            return
        for widget, _opts, _flag in self._panel_stack:
            widget.pack_forget()
        for widget, opts, flag in self._panel_stack:
            if flag == "log" and not show_log:
                continue
            if flag == "vu" and not show_vu:
                continue
            if flag == "subsong" and not show_subsong:
                continue
            widget.pack(**opts)
        self._panel_shows = (show_log, show_vu, show_subsong)

    def _panel_stack_height(self, show_log: bool, show_vu: bool, show_subsong: bool) -> int:
        """Calculate the required panel height without repacking widgets."""
        total = self._padding_y(self._panel_parent.cget("padding"))
        for widget, opts, flag in self._panel_stack:
            if flag == "log" and not show_log:
                continue
            if flag == "vu" and not show_vu:
                continue
            if flag == "subsong" and not show_subsong:
                continue
            above, below = self._pady_pair(opts.get("pady"))
            total += widget.winfo_reqheight() + above + below
        return total

    def _measure_panel_heights(self) -> dict:
        """How much height the panel needs with all / some sections shown."""
        return {"full": self._panel_stack_height(True, True, True),
                "no_log": self._panel_stack_height(False, True, True),
                "no_meters": self._panel_stack_height(False, False, True),
                "min": self._panel_stack_height(False, False, False)}

    def _sync_panel_need(self) -> None:
        """Remeasure panel height only when its contents or available width change."""
        if not self._panel_stack or self._closing:
            return
        signature = self._panel_signature()
        if signature == self._panel_signature_done:
            return
        try:
            need = self._measure_panel_heights()
        except tk.TclError:                        # shutting down
            return
        self._panel_signature_done = signature
        if need == self._panel_need:
            return
        self._panel_need = need
        self._fit_info_panel(self._panel_parent.winfo_height())
        self._refresh_min_size()

    def _on_panel_configure(self, event) -> None:
        if self._closing:
            return
        self._fit_info_panel(int(event.height))

    def _fit_info_panel(self, height: int) -> None:
        """Hide the log, then meters, if needed; always retain controls and song details."""
        if not self._panel_stack or not height:
            return
        self._sync_panel_need()
        need = self._panel_need
        if height >= need["full"] - 2:
            self._panel_relayout(True, True, True)
        elif height >= need["no_log"] - 2:
            self._panel_relayout(False, True, True)
        elif height >= need["no_meters"] - 2:
            self._panel_relayout(False, False, True)
        else:
            self._panel_relayout(False, False, False)

    def _build_transport(self) -> None:
        outer = ttk.Frame(self.root, style="Bar.TFrame")
        outer.pack(side="bottom", fill="x")
        bar = ttk.Frame(outer, style="Bar.TFrame", padding=(10, 8, 10, 10))
        bar.pack(fill="x")

        self._transport_buttons = buttons = ttk.Frame(bar, style="Bar.TFrame")
        buttons.grid(row=0, column=0, sticky="w")
        # plain geometry glyphs only: these exist in every default UI font
        self.btn_prev = ttk.Button(buttons, text="◀◀", width=4, style="Transport.TButton",
                                   command=self.prev_track)
        self.btn_play = ttk.Button(buttons, text="▶", width=4, style="Transport.TButton",
                                   command=self.toggle_play)
        self.btn_next = ttk.Button(buttons, text="▶▶", width=4, style="Transport.TButton",
                                   command=self.next_track)
        self.btn_stop = ttk.Button(buttons, text="■", width=4, style="Transport.TButton",
                                   command=self.stop)
        for b in (self.btn_prev, self.btn_play, self.btn_next, self.btn_stop):
            b.pack(side="left", padx=2)

        self.loop_var = tk.BooleanVar(value=self.settings.loop_track)
        self.loop_btn = ttk.Checkbutton(buttons, text="Loop", variable=self.loop_var,
                                        style="TCheckbutton", command=self.on_loop_toggle)
        self.loop_btn.pack(side="left", padx=(10, 0))

        self.queue_loop_var = tk.BooleanVar(value=self.settings.loop_queue)
        self.queue_loop_btn = ttk.Checkbutton(
            buttons, text="Repeat queue", variable=self.queue_loop_var,
            style="TCheckbutton", command=self.on_queue_loop_toggle)
        self.queue_loop_btn.pack(side="left", padx=(6, 0))

        self._transport_volume = vol = ttk.Frame(bar, style="Bar.TFrame")
        vol.grid(row=0, column=1, sticky="w", padx=(18, 0))
        self.mute_btn = ttk.Button(vol, text="Mute", width=6, command=self.toggle_mute)
        self.mute_btn.pack(side="left")
        start_volume = max(0.0, min(100.0, float(self.settings.volume)))
        self.volume_var = tk.DoubleVar(value=start_volume)
        self.volume_scale = ttk.Scale(vol, from_=0, to=100, orient="horizontal", length=120,
                                      variable=self.volume_var, command=self.on_volume)
        self.volume_scale.pack(side="left", padx=6)
        self.volume_label = ttk.Label(vol, text=f"{int(round(start_volume))}%",
                                      style="BarDim.TLabel", width=5)
        self.volume_label.pack(side="left")
        for widget in (vol, self.mute_btn, self.volume_scale, self.volume_label):
            widget.bind("<MouseWheel>", self.on_volume_wheel)   # Windows / macOS
            widget.bind("<Button-4>", self.on_volume_wheel)     # X11: wheel up
            widget.bind("<Button-5>", self.on_volume_wheel)     # X11: wheel down

        self.pos_group = pos = ttk.Frame(bar, style="Bar.TFrame")
        pos.grid(row=0, column=2, sticky="ew", padx=(18, 0))
        bar.columnconfigure(2, weight=1)
        self.time_label = ttk.Label(pos, text="0:00", style="Time.TLabel", width=6)
        self.time_label.pack(side="left")
        self.queue_label = ttk.Label(pos, text="0/0", style="BarDim.TLabel", width=9,
                                     anchor="e")
        self.queue_label.pack(side="right", padx=(8, 0))
        self.duration_label = ttk.Label(pos, text="0:00", style="Time.TLabel", width=6)
        self.duration_label.pack(side="right")
        self.position_var = tk.DoubleVar(value=0.0)      # 0..1000, the UI's position
        self.seek_bar = SeekBar(pos, on_press=self._seek_press, on_drag=self._seek_drag,
                                on_release=self._seek_release, width=120)
        self.seek_bar.pack(side="left", fill="x", expand=True, padx=6)
        self.seek_bar.bind("<Button-3>", lambda _e: self.engine.seek_fraction(0.0))

        # one-row / two-row decision, fed by <Configure> (see _fit_transport)
        self._transport_outer = outer
        self._transport_row = bar
        self._transport_two_rows = False
        outer.bind("<Configure>", self._on_transport_configure, add="+")

    def _refresh_min_size(self) -> None:
        """Measure the smallest window that can show every control."""
        if self._closing or not self.root.winfo_exists():
            return
        self.root.update_idletasks()
        try:
            transport_h = (self._transport_pad_y()
                           + max(self._transport_buttons.winfo_reqheight(),
                                 self._transport_volume.winfo_reqheight())
                           + self.pos_group.winfo_reqheight() + TRANSPORT_ROW_GAP)
            fixed_h = (self._toolbar_bar.winfo_reqheight() + self._queue_bar.winfo_reqheight()
                       + self._status_bar.winfo_reqheight() + transport_h
                       + self._tab_bar.winfo_reqheight())
        except tk.TclError:                        # shutting down
            return
        body_need = max(self._panel_need["no_meters"], TREE_MIN_HEIGHT)
        min_w = max(self._width_need(self._tab_bar, self.dir_entry, PATH_MIN_WIDTH),
                    self._width_need(self._toolbar_bar, self.dir_entry, PATH_MIN_WIDTH),
                    self._width_need(self._queue_bar, self.search_entry, SEARCH_MIN_WIDTH),
                    self._transport_buttons.winfo_reqwidth()
                    + self._transport_volume.winfo_reqwidth() + 18)
        # a little slack for the panel's wrap-length texts and longer messages
        size = (int(min_w) + 12, int(fixed_h + body_need + 14))
        if size == self._min_size:                 # only touch the WM on change
            return
        self._min_size = size
        self.root.minsize(*size)

    def _transport_pad_y(self) -> int:
        """The transport bar's top+bottom padding (0 if it cannot be read)."""
        try:
            return self._padding_y(self._transport_row.cget("padding"))
        except tk.TclError:
            return 0

    @staticmethod
    def _padding_y(value) -> int:
        """Top+bottom padding of a ttk padding spec (a number, a pair or a quad)."""
        if value is None:
            return 0
        try:
            if isinstance(value, (tuple, list)):
                nums = [int(float(v)) for v in value]
            else:
                text = (str(value).replace("(", " ").replace(")", " ")
                        .replace(",", " ").split())
                nums = [int(float(v)) for v in text]
        except (TypeError, ValueError):
            return 0
        if len(nums) >= 4:
            return nums[1] + nums[3]
        if len(nums) == 2:
            return nums[1] * 2
        if len(nums) == 1:
            return nums[0] * 2
        return 0

    @staticmethod
    def _pady_pair(value) -> tuple:
        """A pady option as (above, below)."""
        if value is None:
            return (0, 0)
        try:
            nums = ([int(float(v)) for v in value]
                    if isinstance(value, (tuple, list))
                    else [int(float(v)) for v in str(value).split()])
        except (TypeError, ValueError):
            return (0, 0)
        if len(nums) >= 2:
            return (nums[0], nums[1])
        if len(nums) == 1:
            return (nums[0], nums[0])
        return (0, 0)

    @staticmethod
    def _width_need(bar, flexible, floor: int) -> int:
        """A bar's width with its flexible entry shrunk to floor."""
        try:
            return bar.winfo_reqwidth() - flexible.winfo_reqwidth() + floor
        except tk.TclError:
            return 0

    def _on_transport_configure(self, event) -> None:
        if self._closing:
            return
        self._fit_transport(int(event.width))

    def _fit_transport(self, width: int) -> None:
        """Move the position controls to a second row when space is tight."""
        if not self._transport_row.winfo_exists():
            return
        self._transport_row.update_idletasks()
        if not self._transport_buttons.winfo_reqwidth():
            return                                  # nothing measurable yet
        first_row = (self._transport_buttons.winfo_reqwidth()
                     + self._transport_volume.winfo_reqwidth() + 18
                     + 10 + 10)                     # the row's own padding
        one_row = first_row + self._pos_group_min_width() + 8
        want_two_rows = width < one_row
        if want_two_rows == self._transport_two_rows:
            return
        self._transport_two_rows = want_two_rows
        self.pos_group.pack_forget()
        if want_two_rows:
            self.pos_group.grid_configure(row=1, column=0, columnspan=3, sticky="ew",
                                          padx=0, pady=(8, 0))
        else:
            self.pos_group.grid_configure(row=0, column=2, columnspan=1, sticky="ew",
                                          padx=(18, 0), pady=0)

    def _pos_group_min_width(self) -> int:
        """Width the position group needs before its bar would become unusable."""
        needed = 0
        for widget in (self.time_label, self.duration_label, self.queue_label):
            if widget.winfo_exists():
                needed += widget.winfo_reqwidth()
        return needed + SEEK_MIN_WIDTH + 24

    def _build_statusbar(self) -> None:
        bar = ttk.Frame(self.root, padding=(10, 2, 10, 4))
        bar.pack(side="bottom", fill="x")
        self._status_bar = bar
        # unpacked first, so it keeps its place at the right edge in any window
        self.reset_btn = ttk.Button(bar, text="Reset layout", style="Mini.TButton",
                                    command=self.reset_layout)
        self.reset_btn.pack(side="right", padx=(10, 0))
        self.add_hint(self.reset_btn,
                      "Reset layout (Ctrl+0): column widths, the vertical bar, and queue scroll")
        self.health_label = ttk.Label(bar, text="", style="Dim.TLabel", background=BG, anchor="e")
        self.health_label.pack(side="right")
        self.status_label = ttk.Label(bar, text="ready", style="Dim.TLabel", background=BG,
                                      anchor="w")
        self.status_label.pack(side="left", fill="x", expand=True)

    def add_hint(self, widget, text: str) -> None:
        widget.bind("<Enter>", lambda _e: self.status(text), add="+")

    def _bind_keys(self) -> None:
        r = self.root
        r.bind_all("<Map>", self._popup_window_mapped, add="+")
        r.bind("<Unmap>", lambda e: self._dismiss_playlist_menu()
               if e.widget is r else None, add="+")
        r.bind("<space>", self._key_toggle)
        r.bind("<Left>", lambda _e: self.seek_relative(-5))
        r.bind("<Right>", lambda _e: self.seek_relative(5))
        r.bind("<Control-Left>", lambda _e: self.seek_relative(-30))
        r.bind("<Control-Right>", lambda _e: self.seek_relative(30))
        r.bind("<Control-Up>", lambda _e: self.queue_row_move(-1))
        r.bind("<Control-Down>", lambda _e: self.queue_row_move(1))
        r.bind("<Up>", lambda _e: self.select_relative(-1))
        r.bind("<Down>", lambda _e: self.select_relative(1))
        r.bind("<Prior>", lambda _e: self.prev_track())          # PageUp
        r.bind("<Next>", lambda _e: self.next_track())           # PageDown
        r.bind("<l>", lambda _e: self._toggle_loop_key())
        r.bind("<r>", lambda _e: self._toggle_queue_loop_key())
        r.bind("<m>", self._toggle_mute_key)
        r.bind("<plus>", lambda _e: self.bump_volume(5))
        r.bind("<equal>", lambda _e: self.bump_volume(5))
        r.bind("<minus>", lambda _e: self.bump_volume(-5))
        r.bind("<Control-o>", lambda _e: self.choose_directory())
        r.bind("<Control-t>", lambda _e: self.toggle_tracker_tab())
        r.bind("<Control-i>", lambda _e: self.open_song_info())
        r.bind("<Control-h>", lambda _e: self.open_stats())
        r.bind("<Control-p>", lambda _e: self.open_playlists_dialog())
        r.bind("<Control-f>", lambda _e: self.search_entry.focus_set())
        r.bind("<Control-Shift-F>", lambda _e: self.open_filter_dialog())
        r.bind("<Control-s>", lambda _e: self.reshuffle())
        r.bind("<Control-r>", lambda _e: self.reveal_current())
        r.bind("<Control-Key-0>", lambda _e: self.reset_layout())
        r.bind("<Control-KP_0>", lambda _e: self.reset_layout())
        r.bind("<F5>", lambda _e: self.load_directory(self._directory) if self._directory else None)
        r.bind("<Escape>", lambda _e: self.search_var.set(""))

        for widget, hint in (
            (self.btn_prev, "◀◀ previous track  (Page Up) or restart past 3s"),
            (self.btn_play, "▶ / ▮▮ play-pause  (Space)"),
            (self.btn_next, "▶▶ next track  (Page Down)"),
            (self.btn_stop, "■ stop and rewind"),
            (self.loop_btn, "Loop the current module (L)"),
            (self.queue_loop_btn, "Repeat queue, draw new order in shuffle (R)"),
            (self.mute_btn, "Mute / unmute  (M)"),
            (self.volume_scale, "Volume  (+ / - / scroll)"),
            (self.seek_bar, "Jump inside module (← / → 5s, Ctrl+← / Ctrl+→ 30s, right-click: go back to start)"),
            (self.reveal_btn, "Open the folder of the module that is playing (Ctrl+R)"),
            (self.song_info_btn, "Module samples, instruments and comment  (Ctrl+I)"),
            (self.tree, "Double-click / Enter: play,  ↑ / ↓: move"),
        ):
            self.add_hint(widget, hint)

    def _typing(self) -> bool:
        widget = self.root.focus_get()
        return isinstance(widget, (ttk.Entry, tk.Entry, ttk.Spinbox, tk.Spinbox))

    def _key_toggle(self, _event=None):
        if self._typing():
            return None
        self.toggle_play()
        return "break"

    def _toggle_loop_key(self):
        if self._typing():
            return None
        self.loop_var.set(not self.loop_var.get())
        self.on_loop_toggle()
        return None

    def _toggle_queue_loop_key(self):
        if self._typing():
            return None
        self.queue_loop_var.set(not self.queue_loop_var.get())
        self.on_queue_loop_toggle()
        return None

    def _toggle_mute_key(self, _event=None):
        if self._typing():
            return None
        self.toggle_mute()
        return None

    def show_path_end(self) -> None:
        """Scroll the path field to the folder name (the part that matters)."""
        try:
            self.dir_entry.after_idle(lambda: self.dir_entry.xview_moveto(1.0))
        except tk.TclError:
            pass

    def _extra_places(self) -> list[tuple[str, str]]:
        """Bookmarks for the folder dialog: the current library and the last one."""
        out = []
        if self._directory and os.path.isdir(self._directory):
            out.append(("Current library", self._directory))
        last = self.settings.last_directory
        if last and os.path.isdir(last) and last != self._directory:
            out.append(("Last library", last))
        if self.settings.last_picker_dir and os.path.isdir(self.settings.last_picker_dir):
            out.append(("Last used", self.settings.last_picker_dir))
        return out

    def choose_directory(self) -> None:
        """Open the folder picker."""
        if self._directory and os.path.isdir(self._directory):
            initial = self._directory
        else:
            initial = (self.settings.last_picker_dir or self.settings.last_directory
                       or os.path.expanduser("~"))
        path = pick_directory(self.root, initialdir=initial,
                              title="Choose a folder with tracker modules",
                              prefer=self.settings.folder_picker,
                              extra_places=self._extra_places())
        if path:
            self.settings.last_picker_dir = os.path.dirname(path) or path
            self.load_directory(path)
        else:
            self.status(f"folder selection cancelled "
                        f"({self.picker_description()})")

    def picker_description(self) -> str:
        if self.settings.folder_picker == "system":
            available = system_picker_available()
            return f"system dialog: {available}" if available else \
                "system dialog unavailable - using the built-in browser"
        return "built-in browser"

    def copy_path(self, _event=None):
        value = self.path_var.get()
        if value:
            self.root.clipboard_clear()
            self.root.clipboard_append(value)
            self.status(f"copied {value}")
        return "break"

    def _path_menu(self, event=None):
        menu = tk.Menu(self.root, tearoff=0, bg=BG_PANEL, fg=FG,
                       activebackground=ACCENT_DIM, activeforeground=ON_ACCENT,
                       bd=0)
        if self.path_var.get():
            menu.add_command(label="Copy path", command=self.copy_path)
        menu.add_command(label="Change folder…", command=self.choose_directory)
        menu.add_command(label="Rescan", command=lambda: self.load_directory(self._directory)
                         if self._directory else None)
        if event is not None:
            try:
                menu.tk_popup(event.x_root, event.y_root)
            finally:
                menu.grab_release()
        return "break"

    def load_directory(self, path: str, autoplay: bool = False) -> None:
        path = os.path.abspath(os.path.expanduser(path))
        if not os.path.isdir(path):
            messagebox.showerror("Not a folder", f"{path} is not a folder")
            return
        if self._scan_thread and self._scan_thread.is_alive():
            self._scan_cancel.set()
        self._scan_cancel = threading.Event()
        root_changed = bool(self._directory) and os.path.abspath(path) != self._directory
        if root_changed:
            self._shuffle_paths = []
            self.settings.shuffle_paths = []
        self._directory = path
        self.settings.last_directory = path
        self._save_settings()
        self.path_var.set(path)
        self.show_path_end()
        self.status(f"scanning {path} …")
        self.count_label.configure(text="scanning…")
        cancel = self._scan_cancel
        if self.analysis_cache is not None and not self._cache_loaded:
            entries = self.analysis_cache.load()
            self._cache_loaded = True
            self.queue_ui.put(("cache_loaded", (entries,)))

        def work():
            try:
                result = library.scan_library(
                    path,
                    progress=lambda count, where: self.queue_ui.put(("progress", (count, where))),
                    cancel=cancel,
                )
                self.queue_ui.put(("scanned", (result, autoplay)))
            except Exception as exc:  # pragma: no cover
                self.queue_ui.put(("error", (f"scan failed: {exc}",)))

        self._scan_thread = threading.Thread(target=work, name="scan", daemon=True)
        self._scan_thread.start()

    def _on_scanned(self, result: library.ScanResult, autoplay: bool) -> None:
        self.tracks = result.tracks
        for track in self.tracks:
            track.broken = None
        self.count_label.configure(
            text=f"{len(self.tracks)} tracks, {result.dirs} folders" +
                 (f", {result.skipped} skipped" if result.skipped else ""))
        self._refresh_min_size()
        if result.cancelled:
            self.status("scan cancelled")
            return
        for err in result.errors[:5]:
            self.log(MSG_WARN, err)
        self.status(f"found {len(self.tracks)} module files in {result.dirs} folders")
        self.log(MSG_INFO, f"library: {result.root} ({len(self.tracks)} modules)")
        self._apply_analysis_cache()
        self.rebuild_queue()
        if autoplay and self.queue:
            self._play_index(0)
        else:
            self._restore_session()

    def _cache_file(self) -> str:
        """Where the analysis cache lives: beside the settings file."""
        if self.config_file:
            return os.path.join(os.path.dirname(os.path.abspath(self.config_file)),
                                CACHE_NAME)
        return cache_path()

    def _apply_analysis_cache(self) -> None:
        """Restore module details only for files matching their cached size and modification time."""
        if self.analysis_cache is None or not self.tracks:
            return
        hits = sum(1 for track in self.tracks if self.analysis_cache.apply(track))
        if hits:
            left = len(self.tracks) - hits
            self.log(MSG_INFO, f"restored the details of {hits} modules from the cache"
                               + (f" ({left} not read yet)" if left else ""))
            self.add_hint(self.analyze_button,
                          "Analyze: read metadata for queued songs")
            self.status(f"found {len(self.tracks)} module files in "
                        f"{len({t.rel_dir for t in self.tracks})} folders,"
                        f"{hits} already analyzed")

    def _restore_session(self) -> bool:
        """Restore the last module and position paused, without starting audio automatically."""
        if not self.settings.remember_position or self._session_restored:
            return False
        self._session_restored = True
        path = self.settings.last_path
        if not path:
            return False
        index = next((i for i, t in enumerate(self.queue) if t.path == path), None)
        if index is None:                    # missing on disk any more
            return False
        position = max(0.0, float(self.settings.last_position or 0.0))
        self._play_index(index, paused=True, position=position)
        self.log(MSG_INFO, f"session: {os.path.basename(path)} at {position:.1f}s (paused)")
        return True

    def _save_session(self, force: bool = False) -> None:
        """Write what is loaded and where (throttled: the tick calls this)."""
        if not self.settings.remember_position:
            return
        snap = self._last_snapshot
        if snap is None:                       # the tick has not run yet
            try:
                snap = self.engine.snapshot()
            except Exception:
                return
        path = (snap.path or "") if (snap and snap.loaded) else ""
        position = float(snap.position) if (snap and snap.loaded) else 0.0
        state = (path, round(position, 1))
        if state == self._session_state:
            return
        now = time.monotonic()
        if not force and now - self._session_saved_at < SESSION_SAVE_S:
            return
        self._session_state = state
        self._session_saved_at = now
        self.settings.last_path = path
        self.settings.last_position = position
        self._save_settings()

    def _queue_filter(self) -> QueueFilter:
        """The criteria in force right now (kept in the settings)."""
        return filter_from_settings(self.settings)

    def _queue_source_tracks(self) -> list[Track]:
        """Unfiltered songs in the current collection, including external files."""
        if self.order_var.get() == library.ORDER_PLAYLIST:
            return self._playlists.resolve(self._active_playlist, self.tracks)[0]
        return self.tracks

    def _filtered_tracks(self) -> list[Track]:
        """Search box + filter dialog: what the queue is built from."""
        found = library.search_filter(self._queue_source_tracks(), self.search_var.get())
        return self._queue_filter().select(found)

    def apply_filter(self, announce: bool = True) -> None:
        """Re-build the queue with the current criteria and show what happened."""
        self._save_settings()
        self.rebuild_queue()
        criteria = self._queue_filter()
        if not announce:
            return
        if not criteria.active:
            self.status(f"filter cleared - all {len(self._queue_source_tracks())} modules are back")
            return
        unknown = unknown_duration_count(self._queue_source_tracks())
        note = f", {unknown} without a length yet" if unknown and (
            criteria.min_seconds or criteria.max_seconds) else ""
        self.status(f"filter: {len(self.queue)} of {len(self._queue_source_tracks())} modules shown "
                    f"({criteria.describe()}){note}")
        if not self.queue:
            self.log(MSG_WARN, f"the filter ({criteria.describe()}) hides every module")

    def open_stats(self) -> None:
        self._dismiss_playlist_menu()
        window = self._stats_window
        if window is not None and window.winfo_exists():
            window.lift()
            window.focus_set()
            return
        self._stats_window = StatsWindow(self)

    def apply_listening_stats(self, enabled: bool) -> None:
        self._listening.set_enabled(enabled, self.engine.snapshot(), self.engine)
        self.settings.track_listening_stats = bool(enabled)
        self._listening.store.save(force=True)
        window = self._stats_window
        if window is not None and window.winfo_exists():
            window.refresh(force=True)

    def _record_listening(self, snap) -> None:
        changed = self._listening.set_enabled(self.settings.track_listening_stats, snap, self.engine)
        self._listening.observe(snap, self.engine)
        store = self._listening.store
        store.save()
        if store.error != self._stats_warning:
            self._stats_warning = store.error
            if store.error:
                self.log(MSG_WARN, store.error)
        window = self._stats_window
        if window is not None and window.winfo_exists():
            window.refresh(force=changed)

    def open_song_info(self) -> None:
        """Open or raise the single song-info window."""
        self._dismiss_playlist_menu()
        window = self._song_info
        if window is not None:
            if window.winfo_exists():
                window.lift()
                window.focus_set()
                return
            self._song_info = None
        self._song_info = SongInfoWindow(self)

    def folder_of(self, path: str) -> str:
        """Use the queue folder label, otherwise a library-relative or absolute folder path."""
        if not path:
            return ""
        for track in self.tracks:
            if track.path == path:
                return track.rel_dir or "."
        folder = os.path.dirname(path)
        base = self._directory or ""
        if base:
            try:
                rel = os.path.relpath(folder, base)
            except ValueError:                  # another drive (Windows)
                rel = folder
            if rel != os.pardir and not rel.startswith(os.pardir + os.sep):
                return rel
        return folder or "."

    def _follow_song_info(self, snap) -> None:
        """Refresh song info when that window is open."""
        window = self._song_info
        if window is not None and window.winfo_exists():
            window.follow(snap)

    def open_filter_dialog(self) -> None:
        """Show the filter dialog (it applies on OK)."""
        if not self._queue_source_tracks():
            self.status("nothing to filter - open a folder or add songs to a playlist")
            return
        FilterDialog(self)

    def _anchor_path(self) -> str:
        """The track that should lead a fresh shuffle: what is playing, else
        what is selected in the queue."""
        if self._playing_path:
            return self._playing_path
        selection = self.tree.selection()
        if selection:
            row = selection[0]
            for path, candidate in self._row_of_path.items():
                if candidate == row:
                    return path
        return ""

    def _draw_shuffle_plan(self, new_seed: Optional[int] = None, first: Optional[str] = None,
                           avoid_first: str = "") -> None:
        """Reuse the saved shuffle seed unless a new order is requested."""
        seed = new_seed
        if seed is None:
            seed = self._shuffle_seed or self.settings.shuffle_seed or \
                random.SystemRandom().randrange(1, 2 ** 31)
        self._shuffle_seed = int(seed)
        self.settings.shuffle_seed = int(seed)
        if first is None:
            first = "" if avoid_first else self._anchor_path()
        plan = library.shuffle_order(self._filtered_tracks(), seed=int(seed),
                                    first=first or None, avoid_first=avoid_first or None)
        self._shuffle_paths = [t.path for t in plan]
        if len(self._shuffle_paths) <= MAX_SHUFFLE_PATHS:
            self.settings.shuffle_paths = list(self._shuffle_paths)
        else:                                   # keep the config small
            self.settings.shuffle_paths = []

    def _rotate_shuffle_to_anchor(self) -> None:
        anchor = self._anchor_path()
        if anchor:
            self._shuffle_paths = library.rotate_to_front(self._shuffle_paths, anchor)

    def _ensure_shuffle_plan(self) -> None:
        if self._shuffle_paths:
            return
        stored = [p for p in (self.settings.shuffle_paths or []) if isinstance(p, str)]
        if stored:
            # from the last session: bring the queue back exactly as it was
            self._shuffle_paths = stored
            return
        self._draw_shuffle_plan()

    def on_order_changed(self) -> None:
        """Queue-order radio buttons: switch the mode, never re-shuffle."""
        if self.order_var.get() == library.ORDER_SHUFFLE:
            if self._shuffle_paths:
                self._rotate_shuffle_to_anchor()
            else:
                self._draw_shuffle_plan()
        self.rebuild_queue()
        if self.order_var.get() == library.ORDER_SHUFFLE:
            self.status("shuffle queue ready - 'Shuffle now' draws a new order")
        elif self.order_var.get() == library.ORDER_PLAYLIST:
            if self._active_playlist:
                self.status(f"queue follows playlist '{self._active_playlist}'")
            else:
                self.status("no playlist loaded yet - open the Playlists dialog (Ctrl+P)")

    def reshuffle(self) -> None:
        """The "Shuffle now" button: draw a brand new order, current track first."""
        self.order_var.set(library.ORDER_SHUFFLE)
        self._draw_shuffle_plan(new_seed=random.SystemRandom().randrange(1, 2 ** 31))
        self.rebuild_queue()
        anchor = self.queue[0].name if self.queue else None
        self.status("new shuffle order drawn" + (f" - starting from {anchor}" if anchor else ""))

    def rebuild_queue(self, keep_playing: bool = True) -> None:
        """Rebuild the filtered queue without redrawing an existing shuffle order."""
        selected_paths = self.selected_song_paths()
        current_path = None
        if 0 <= self.queue_index < len(self.queue):
            current_path = self.queue[self.queue_index].path
        elif self._playing_path:
            current_path = self._playing_path
        needle = self.search_var.get()
        mode = self.order_var.get()
        if mode == library.ORDER_PLAYLIST and not self._ensure_playlist():
            mode = library.ORDER_DIRECTORY
            self.order_var.set(mode)
            if self._active_playlist:
                self._active_playlist = ""
                self.settings.active_playlist = ""
            self.status("the loaded playlist is gone - the queue follows "
                        "'by directory' again")
        self.settings.queue_mode = mode
        filtered = self._filtered_tracks()      # after any fallback to the library
        if mode == library.ORDER_PLAYLIST:
            self.queue = self._playlist_queue()
        elif mode == library.ORDER_SHUFFLE:
            self._ensure_shuffle_plan()
            self.queue = library.apply_path_order(filtered, self._shuffle_paths)
        else:
            self.queue = library.order_tracks(filtered, mode)
        if not keep_playing or not current_path:
            self.queue_index = -1
        else:
            self.queue_index = next(
                (i for i, t in enumerate(self.queue) if t.path == current_path), -1)
        self._populate_tree()
        if selected_paths:
            rows = [self._row_of_path[p] for p in selected_paths if p in self._row_of_path]
            self.tree.selection_set(rows)
        self._selection_actions()
        self.filter_btn.configure(
            style="Accent.TButton" if self._queue_filter().active else "TButton")
        self.root.after_idle(self.sync_column_layout)
        searching = bool(needle.strip())
        filtering = self._queue_filter().active
        playlist_note = self.playlist_label()
        if searching or filtering:
            note = ", filter" if filtering else ""
            shown = f"{len(self.queue)}/{len(self._queue_source_tracks())} shown{note}"
        else:
            shown = ""
        if playlist_note or shown:
            self.filter_label.configure(
                text=", ".join(part for part in (playlist_note, shown) if part))
        else:
            self.filter_label.configure(text="")
        self._save_settings()
        self._update_queue_label()

    def _populate_tree(self) -> None:
        self.tree.delete(*self.tree.get_children())
        self._row_of_path.clear()
        self._base_tags.clear()
        self._playing_row = None
        mode = self.order_var.get()
        if mode == library.ORDER_DIRECTORY:
            nodes: dict[tuple, str] = {}
            for index, track in enumerate(self.queue):
                parent = ""
                parts: tuple = ()
                for part in track.dir_parts:
                    parts = parts + (part,)
                    if parts not in nodes:
                        nodes[parts] = self.tree.insert(
                            parent, "end", text=f"📁 {part}", open=True, tags=("dir",))
                    parent = nodes[parts]
                self._insert_track(parent, index, track)
        else:
            for index, track in enumerate(self.queue):
                self._insert_track("", index, track)
        if 0 <= self.queue_index < len(self.queue):
            self._mark_playing(self.queue[self.queue_index].path)

    def _insert_track(self, parent: str, index: int, track: Track) -> None:
        stripe = ("alt",) if index % 2 else ()
        row = self.tree.insert(
            parent, "end", text=track.name,
            values=(track.rel_dir or ".", track.duration_text(), track.fmt.upper(),
                    track.channels or ""),
            tags=stripe,
        )
        self._row_of_path[track.path] = row
        self._base_tags[row] = stripe
        self.set_row_broken(track.path, bool(track.broken))

    def set_row_broken(self, path: str, broken: bool) -> None:
        row = self._row_of_path.get(path)
        if not row or not self.tree.exists(row):
            return
        tags = list(self._base_tags.get(row, ()))
        if broken and "broken" not in tags:
            tags.append("broken")
        elif not broken and "broken" in tags:
            tags.remove("broken")
        self._base_tags[row] = tuple(tags)
        self._apply_tags(row)

    def _apply_tags(self, row: str) -> None:
        tags = list(self._base_tags.get(row, ()))
        if row == self._playing_row:
            tags.append("playing")
        self.tree.item(row, tags=tuple(tags))

    def _set_window_title(self, text: str) -> None:
        """Show text in the title bar (only when it actually changed)."""
        if text == self._window_title:
            return
        self._window_title = text
        try:
            self.root.title(text)
        except tk.TclError:                # closing
            pass

    def _update_window_title(self, filename: str = "", title: str = "",
                             artist: str = "") -> None:
        self._set_window_title(now_playing_title(self.settings.window_title,
                                                 filename, title, artist))

    def _mark_playing(self, path: str) -> None:
        self._playing_path = path
        self._set_reveal_enabled(True)
        previous, self._playing_row = self._playing_row, None
        if previous and self.tree.exists(previous):
            self._apply_tags(previous)
        row = self._row_of_path.get(path)
        if row and self.tree.exists(row):
            self._playing_row = row
            self._apply_tags(row)
            self.tree.see(row)
            self.tree.selection_set(row)
        self._update_queue_label()

    def _update_queue_label(self) -> None:
        if self.queue and self.queue_index >= 0:
            self.queue_label.configure(text=f"{self.queue_index + 1}/{len(self.queue)}")
        else:
            self.queue_label.configure(text=f"-/{len(self.queue)}")

    def analyze_library(self) -> None:
        if not self._queue_source_tracks():
            self.status("nothing to analyze - open a folder first")
            return
        if self._analyzer and self._analyzer.is_alive():
            self.status("analysis already running")
            return
        todo = [t for t in self._queue_source_tracks() if not t.analyzed]
        if not todo:
            self.status("everything is already analyzed")
            return
        self.status(f"analyzing {len(todo)} modules in the background …")

        def on_result(track: Track, info, error):
            self.queue_ui.put(("analyzed", (track.path, info, error)))

        def on_done(done, failed):
            self.queue_ui.put(("analyzed_done", (done, failed)))

        self._analyzer = Analyzer(todo, on_result, on_done)
        self._analyzer.start()

    def _tree_columns(self) -> tuple[str, ...]:
        return ("#0",) + tuple(self.tree["columns"])

    def _column_label(self, column: str) -> str:
        return self.tree.heading(column, "text") or column

    def _heading_height(self) -> int:
        """Measure the heading region without relying on an off-screen row."""
        tree = self.tree
        try:
            limit = min(tree.winfo_height(), 80)
        except tk.TclError:
            return 0
        seen = False
        for y in range(1, max(limit, 2)):
            try:
                inside = tree.identify_region(2, y) == "heading"
            except tk.TclError:
                return 0
            if inside:
                seen = True
            elif seen:
                return y
        return 0

    def rendered_column_edges(self) -> Optional[list[tuple[int, str]]]:
        """Read actual column boundaries from a visible cell; return None before layout is
        available."""
        tree = self.tree
        columns = self._tree_columns()
        try:
            item = tree.identify_row(min(self._heading_height() + 3,
                                         max(tree.winfo_height() - 2, 1)))
            if not item and tree.get_children():
                item = tree.get_children()[0]
            if not item:
                return None
            edges = []
            for column in columns[:-1]:
                box = tree.bbox(item, column)
                if not box:
                    return None
                edges.append((int(box[0]) + int(box[2]), column))
            return edges
        except tk.TclError:
            return None

    def _column_edges(self) -> list[tuple[int, str]]:
        """[((x of the right edge of the column), column id), ...] in tree space."""
        drawn = self.rendered_column_edges()
        if drawn is not None:
            return drawn
        tree = self.tree
        edges, x = [], 2
        for column in self._tree_columns()[:-1]:
            x += int(tree.column(column, "width"))
            edges.append((x, column))
        return edges

    def _fit_module_column(self) -> None:
        """Shrink Module, then Folder, to their minimum widths so all columns remain visible."""
        tree = self.tree
        try:
            available = tree.winfo_width()
            columns = self._tree_columns()
        except tk.TclError:
            return
        if available <= 1:
            return
        overflow = sum(int(tree.column(c, "width")) for c in columns) - available
        if overflow <= 0:
            return
        for column in ("#0", "folder"):
            if overflow <= 0:
                break
            width = int(tree.column(column, "width"))
            slack = width - int(tree.column(column, "minwidth"))
            if slack > 0:
                tree.column(column, width=width - min(overflow, slack))
                overflow -= min(overflow, slack)

    def sync_column_layout(self) -> None:
        """Refit queue columns after resize, refill, or width changes."""
        if self._closing:
            return
        self._fit_module_column()

    def _on_tree_motion(self, event) -> None:
        """Show the resize cursor while hovering a heading separator."""
        try:
            over_separator = self.tree.identify_region(event.x, event.y) == "separator"
        except tk.TclError:
            return
        wanted = "sb_h_double_arrow" if over_separator else ""
        if self.tree.cget("cursor") != wanted:
            try:
                self.tree.configure(cursor=wanted)
            except tk.TclError:
                pass

    def _separator_column(self, x: int, y: int) -> str:
        """The column a heading separator under (x, y) belongs to, '' if none."""
        tree = self.tree
        if tree.identify_region(x, y) != "separator":
            return ""
        edges = self._column_edges()
        if not edges:
            return ""
        x_edge, column = min(edges, key=lambda edge: abs(edge[0] - x))
        if abs(x_edge - x) > 6:
            return ""
        # the divider between two columns fits the one you are closer to
        return column if x <= x_edge else self._right_of(column)

    def _right_of(self, column: str) -> str:
        columns = self._tree_columns()
        index = columns.index(column)
        return columns[index + 1] if index + 1 < len(columns) else column

    def _on_screen_rows(self, limit: int = 4000) -> list[tuple[str, int]]:
        """[(item, nesting depth), ...] for the rows inside the viewport."""
        tree = self.tree
        found: list[tuple[str, int]] = []

        def visit(parent: str, depth: int) -> bool:
            for item in tree.get_children(parent):
                if tree.bbox(item):
                    found.append((item, depth))
                    if len(found) >= limit:
                        return True
                elif found:
                    return True                     # scrolled past the bottom
                if tree.item(item, "open") and visit(item, depth + 1):
                    return True
            return False

        visit("", 0)
        return found

    def autofit_column(self, column: str) -> int:
        """Widen (or narrow) column to its longest visible entry, like Excel."""
        tree = self.tree
        style = ttk.Style()
        body_font = tkfont.Font(font=style.lookup("Treeview", "font") or "TkDefaultFont")
        head_font = tkfont.Font(font=style.lookup("Treeview.Heading", "font") or "TkHeadingFont")
        padding = style.lookup("Treeview.Heading", "padding") or 6
        pad = sum(int(p) for p in (padding if isinstance(padding, tuple) else (padding,))) + 8
        width = head_font.measure(self._column_label(column)) + pad
        for item, depth in self._on_screen_rows():
            text = (tree.item(item, "text") if column == "#0" else tree.set(item, column))
            extra = CELL_PAD
            if column == "#0":
                extra += INDENT_PER_LEVEL * depth       # nested rows are indented
            width = max(width, body_font.measure(str(text)) + extra)
        width = max(width, int(tree.column(column, "minwidth") or 20))
        width = min(width, max(140, tree.winfo_width() - 8))
        tree.column(column, width=width)
        self.sync_column_layout()
        note = " (the Module column also fills the window)" if tree.column(column, "stretch") else ""
        self.status(f"fitted '{self._column_label(column)}' to its widest visible entry "
                    f"({width} px){note}")
        return width

    def _on_tree_double_click(self, event) -> Optional[str]:
        """Divider -> fit the column next to it; anywhere else -> play."""
        try:
            region = self.tree.identify_region(event.x, event.y)
        except tk.TclError:
            return None
        if region == "separator":
            column = self._separator_column(event.x, event.y)
            if column:
                self.autofit_column(column)
                return "break"
        elif region == "heading":
            column = self.tree.identify_column(event.x)
            if column:
                self.autofit_column("#0" if column == "#0"
                                    else self.tree["columns"][int(column[1:]) - 1])
                return "break"
        self.play_selected()
        return None

    def _on_analyzed(self, path: str, info, error: Optional[str]) -> None:
        track = next((t for t in self.tracks + self._queue_source_tracks() if t.path == path), None)
        if track is None:
            return
        if error:
            track.broken = error
            track.analyzed = True
        elif info is not None:
            track.analyzed = True
            track.duration = info.duration
            track.fmt = info.format
            track.channels = info.num_channels
            track.subsongs = info.num_subsongs
            track.title = info.title
        row = self._row_of_path.get(path)
        if row and self.tree.exists(row):
            self.tree.item(row, values=(track.rel_dir or ".", track.duration_text(),
                                        track.fmt.upper(), track.channels or ""))
            self._apply_tags(row)
        if self.analysis_cache is not None:
            self.analysis_cache.remember(track)

    def selected_song_paths(self) -> list[str]:
        """Songs only, in queue order; selecting a folder never adds its children."""
        selected = set(self.tree.selection())
        return [t.path for t in self.queue if self._row_of_path.get(t.path) in selected]

    def _selection_actions(self) -> None:
        has_selection = bool(self.selected_song_paths())
        self.add_to_playlist_btn.state(["!disabled"] if has_selection else ["disabled"])
        can_remove = has_selection and self.order_var.get() == library.ORDER_PLAYLIST
        self.remove_from_playlist_btn.state(["!disabled"] if can_remove else ["disabled"])

    def _select_all_songs(self, _event=None) -> str:
        self.tree.selection_set(list(self._row_of_path.values()))
        return "break"

    def create_playlist(self, name: str, paths=()) -> Optional[str]:
        """Create an empty or selected-songs playlist without replacing the queue."""
        try:
            playlist = self._playlists.add(name, paths, self._directory)
        except PlaylistError as exc:
            messagebox.showerror("Playlists", str(exc), parent=self.root)
            return None
        self.status(f"Created '{playlist.name}' with {len(playlist.paths)} songs")
        return playlist.name

    def prompt_new_playlist(self, paths=()) -> Optional[str]:
        self._dismiss_playlist_menu()
        current = self._new_playlist_dialog
        if current is not None and current.winfo_exists():
            current.lift()
            current.name_entry.focus_set()
            return None
        dialog = NewPlaylistDialog(self, paths)
        self._new_playlist_dialog = dialog
        self.root.wait_window(dialog)
        return dialog.result

    def add_songs_to_playlist(self, name: str, paths) -> bool:
        paths = list(paths)
        playlist = self._playlists.get(name)
        if playlist is None:
            return False
        active = playlist.name == self._active_playlist
        before = list(self._playlist_paths if active and self._playlist_dirty else playlist.paths)
        after = list(dict.fromkeys(before + list(paths)))
        try:
            self._playlists.replace(playlist.name, after)
        except PlaylistError as exc:
            messagebox.showerror("Playlists", str(exc), parent=self.root)
            return False
        if active:
            self._playlist_paths = after
            self._playlist_dirty = False
            if self.order_var.get() == library.ORDER_PLAYLIST:
                self.rebuild_queue(keep_playing=True)
        count = len(after) - len(before)
        self.status(f"Added {count} song{'s' if count != 1 else ''} to '{playlist.name}'"
                    + (" (already present songs were skipped)" if count < len(paths) else ""))
        return True

    def add_files_to_playlist(self, name: str, parent=None) -> bool:
        if self._playlists.get(name) is None:
            return False
        self._dismiss_playlist_menu()
        extensions = sorted(library.supported_extensions())
        paths = pick_files(
            parent=parent or self.root, title=f"Add songs to {name}", multiple=True,
            initialdir=self._directory or self.settings.last_picker_dir,
            prefer=self.settings.folder_picker, extra_places=self._extra_places(),
            filetypes=[("Tracker modules", " ".join("*." + e.lstrip(".") for e in extensions)),
                       ("All files", "*")])
        if not paths:
            return False
        valid = [os.path.abspath(p) for p in paths if os.path.isfile(p)]
        if not valid:
            self.status("No existing files were selected")
            return False
        return self.add_songs_to_playlist(name, valid)

    def remove_selected_from_playlist(self) -> None:
        if self.order_var.get() != library.ORDER_PLAYLIST:
            return
        paths = set(self.selected_song_paths())
        playlist = self._playlists.get(self._active_playlist)
        if not paths or playlist is None:
            return
        before = self._playlist_paths
        after = [p for p in before if p not in paths]
        try:
            self._playlists.replace(playlist.name, after)
        except PlaylistError as exc:
            messagebox.showerror("Playlists", str(exc), parent=self.root)
            return
        self._playlist_paths = after
        self._playlist_dirty = False
        # Rebuilding the queue never tells the engine to stop the current song.
        self.rebuild_queue(keep_playing=True)
        self.status(f"Removed {len(before) - len(after)} songs from '{playlist.name}' - files kept")

    def _playlist_menu(self, parent=None) -> tk.Menu:
        menu = tk.Menu(parent or self.root, tearoff=0, bg=BG_PANEL, fg=FG,
                       activebackground=ACCENT_DIM, activeforeground=ON_ACCENT,
                       disabledforeground=FG_FAINT)
        menu.bind("<FocusOut>", self._playlist_menu_focus_out, add="+")
        return menu

    def _add_playlist_menu(self, paths, parent=None) -> tk.Menu:
        paths = list(paths)
        menu = self._playlist_menu(parent)
        menu.add_command(label="New playlist…", command=lambda: self.prompt_new_playlist(paths))
        names = self._playlists.names()
        if names:
            menu.add_separator()
            for name in names:
                menu.add_command(label=name,
                                 command=lambda n=name: self.add_songs_to_playlist(n, paths))
        return menu

    def _dismiss_playlist_menu(self) -> None:
        """Unpost the whole cascade and let Tk restore its menu focus/grab state."""
        if self._menu_focus_check is not None:
            self.root.after_cancel(self._menu_focus_check)
            self._menu_focus_check = None
        menu = self._queue_menu
        if menu is None or not menu.winfo_exists() or not menu.winfo_ismapped():
            return
        focus = str(self.root.tk.call("focus"))
        grab = str(self.root.tk.call("grab", "current", self.root._w))
        inside = lambda path: path == menu._w or path.startswith(menu._w + ".")
        status = str(self.root.tk.call("grab", "status", grab)) if grab else ""
        try:
            menu.tk.call("tk::MenuUnpost", menu._w)
        finally:
            menu.unpost()
            if grab and not inside(grab) and self.root.tk.call("winfo", "exists", grab):
                if status == "global":
                    self.root.tk.call("grab", "set", "-global", grab)
                elif status == "local":
                    self.root.tk.call("grab", "set", grab)
            if focus and not inside(focus) and self.root.tk.call("winfo", "exists", focus):
                self.root.tk.call("focus", focus)

    def _playlist_menu_focus_out(self, _event=None) -> None:
        if self._menu_focus_check is not None:
            self.root.after_cancel(self._menu_focus_check)
        self._menu_focus_check = self.root.after_idle(self._check_playlist_menu_focus)

    def _check_playlist_menu_focus(self) -> None:
        self._menu_focus_check = None
        menu = self._queue_menu
        if menu is None or not menu.winfo_exists() or not menu.winfo_ismapped():
            return
        focus = str(self.root.tk.call("focus"))
        if focus != menu._w and not focus.startswith(menu._w + "."):
            self._dismiss_playlist_menu()

    def _popup_window_mapped(self, event) -> None:
        if isinstance(event.widget, tk.Toplevel):
            self._dismiss_playlist_menu()

    def _popup_playlist_menu(self, menu, x: int, y: int) -> None:
        self._dismiss_playlist_menu()
        old = self._queue_menu
        if old is not None:
            old.destroy()
        self._queue_menu = menu
        try:
            menu.tk_popup(x, y)
        except tk.TclError:
            self._dismiss_playlist_menu()
            raise

    def show_add_to_playlist_menu(self) -> None:
        paths = self.selected_song_paths()
        if not paths:
            self.status("Select songs first (Ctrl / Shift-click to select several)")
            return
        button = self.add_to_playlist_btn
        self._popup_playlist_menu(self._add_playlist_menu(paths),
                                  button.winfo_rootx(), button.winfo_rooty() + button.winfo_height())

    def _queue_context_menu(self, event) -> str:
        tree = self.tree
        if getattr(event, "num", None) == 3:
            row = str(tree.tk.call(tree._w, "identify", "row", event.x, event.y))
            if row not in tree.selection():
                tree.selection_set(row) if row else tree.selection_remove(tree.selection())
            if row:
                tree.focus(row)
        paths = self.selected_song_paths()
        menu = self._playlist_menu()
        menu.add_command(label="Play", command=self.play_selected,
                         state="normal" if paths else "disabled")
        menu.add_cascade(label="Add to playlist", menu=self._add_playlist_menu(paths, menu),
                         state="normal" if paths else "disabled")
        if self.order_var.get() == library.ORDER_PLAYLIST:
            menu.add_separator()
            menu.add_command(label="Remove from playlist (keep files)",
                             command=self.remove_selected_from_playlist,
                             state="normal" if paths else "disabled")
        x = event.x_root if getattr(event, "num", None) == 3 else tree.winfo_rootx() + 20
        y = event.y_root if getattr(event, "num", None) == 3 else tree.winfo_rooty() + 35
        self._popup_playlist_menu(menu, x, y)
        return "break"

    def open_playlists_dialog(self) -> None:
        """The Playlists dialog (the queue bar's button, Ctrl+P).  One at a time."""
        self._dismiss_playlist_menu()
        dialog = self._playlist_dialog
        if dialog is not None and dialog.winfo_exists():
            dialog.lift()
            dialog.focus_set()
            return
        self._playlist_dialog = PlaylistsDialog(self)

    def _playlist_queue(self) -> list[Track]:
        """The loaded playlist's tracks in its order (search + filter inside)."""
        tracks, _missing = self._playlists.resolve(self._active_playlist, self.tracks)
        filtered = self._queue_filter().select(library.search_filter(tracks, self.search_var.get()))
        by_path = {t.path: t for t in filtered}
        return [by_path[p] for p in self._playlist_paths if p in by_path]

    def _ensure_playlist(self) -> bool:
        """Whether the queue can be built from a loaded playlist."""
        name = self._active_playlist
        if not name or self._playlists.get(name) is None:
            return False
        if not self._playlist_paths and not self._playlist_dirty:
            self._playlist_paths = list(self._playlists.get(name).paths)
        return True

    def playlist_label(self) -> str:
        """The queue bar's read-out while a playlist is loaded ('' when none)."""
        name = self._active_playlist
        if not name or self.order_var.get() != library.ORDER_PLAYLIST:
            return ""
        playlist = self._playlists.get(name)
        if playlist is None:
            return ""
        text = f"\u266b {name}"
        found, missing = self._playlists.resolve(name, self.tracks)
        if missing:
            text += f" \u00b7 {len(missing)} missing"
        if len(self.queue) < len(found):
            text += f" \u00b7 {len(self.queue)} of {len(found)} shown"
        if self._playlist_dirty:
            text += " \u2022"
        return text

    def _load_playlist_into(self, name: str) -> None:
        """Switch the queue to playlist mode on name (no status text)."""
        playlist = self._playlists.get(name)
        if playlist is None:
            return
        self._active_playlist = playlist.name
        self.settings.active_playlist = playlist.name
        self._playlist_paths = list(playlist.paths)
        self._playlist_dirty = False
        self.order_var.set(library.ORDER_PLAYLIST)
        self.rebuild_queue(keep_playing=True)

    def new_playlist(self, name: str) -> Optional[str]:
        """Save the current queue as a new playlist and load it (None = refused)."""
        name = str(name or "").strip()
        if not self.queue:
            self.status("nothing to save - the queue is empty")
            return None
        try:
            self._playlists.add(name, [t.path for t in self.queue], self._directory)
        except PlaylistError as exc:
            messagebox.showerror("Playlists", str(exc))
            return None
        self._load_playlist_into(name)
        self._save_settings()
        self.status(f"saved {len(self.queue)} tracks as playlist '{self._active_playlist}'")
        return self._active_playlist

    def load_playlist(self, name: str) -> bool:
        if self._playlists.get(name) is None:
            self.status(f"no playlist called '{name}'")
            return False
        self._load_playlist_into(name)
        self._save_settings()
        found, missing = self._playlists.resolve(name, self.tracks)
        note = f" - {len(missing)} of its tracks are missing on disk" if missing else ""
        self.status(f"playlist '{name}' loaded: {len(self.queue)} tracks{note}")
        return True

    def save_playlist(self, name: str) -> bool:
        """Keep the current queue order in the loaded playlist."""
        if self.order_var.get() != library.ORDER_PLAYLIST:
            self.status("queue order can only be kept while a playlist is loaded")
            return False
        if name != self._active_playlist:
            self.status(f"only the loaded playlist ({self._active_playlist or 'none'}) "
                        f"can keep a queue order")
            return False
        paths = list(self._playlist_paths)
        try:
            self._playlists.replace(name, paths, self._directory)
        except PlaylistError as exc:
            messagebox.showerror("Playlists", str(exc), parent=self.root)
            return False
        self._playlist_paths = list(paths)
        self._playlist_dirty = False
        self.rebuild_queue(keep_playing=True)   # the read-out loses its dirty mark
        self.status(f"queue order saved to '{name}'")
        return True

    def rename_playlist(self, old: str, new: str) -> Optional[str]:
        try:
            playlist = self._playlists.rename(old, new)
        except PlaylistError as exc:
            messagebox.showerror("Playlists", str(exc))
            return None
        if self._active_playlist.casefold() == str(old or "").strip().casefold():
            self._active_playlist = playlist.name
            self.settings.active_playlist = playlist.name
        self._save_settings()
        self.status(f"playlist '{old}' is now '{playlist.name}'")
        return playlist.name

    def delete_playlist(self, name: str) -> None:
        try:
            self._playlists.remove(name)
        except PlaylistError as exc:
            messagebox.showerror("Playlists", str(exc))
            return
        if self._active_playlist.casefold() == str(name or "").strip().casefold():
            self._active_playlist = ""
            self.settings.active_playlist = ""
            self._playlist_paths = []
            self._playlist_dirty = False
            self.order_var.set(library.ORDER_DIRECTORY)
        self._save_settings()
        self.status(f"playlist '{name}' deleted")
        self.rebuild_queue(keep_playing=True)

    def export_playlist(self, name: str, parent=None) -> bool:
        playlist = self._playlists.get(name)
        if playlist is None or not playlist.paths:
            self.status("that playlist has no tracks to export")
            return False
        try:
            path = pick_files(
                parent=parent or self.root, title="Export playlist as M3U", save=True,
                initialdir=self._directory or self.settings.last_picker_dir,
                prefer=self.settings.folder_picker, extra_places=self._extra_places(),
                defaultextension=".m3u", initialfile=playlist.name + ".m3u",
                filetypes=[("M3U playlists", "*.m3u"), ("All files", "*")])
        except tk.TclError:
            return False
        if not path:
            return False
        count = write_m3u(playlist.paths, path)
        if not count:
            self.status(f"Could not export playlist to {path}")
            return False
        self.status(f"exported {count} tracks to {path}")
        return True

    def import_playlist(self, m3u_path: str) -> Optional[str]:
        """Create and load a playlist from an M3U file (None = nothing in it)."""
        paths, skipped = read_m3u(m3u_path)
        if not paths:
            note = f" ({skipped} entries point at missing files)" if skipped else ""
            self.status(f"nothing importable in {os.path.basename(m3u_path)}{note}")
            return None
        base = os.path.splitext(os.path.basename(m3u_path))[0] or "Imported"
        name, taken = base, 2
        while self._playlists.get(name) is not None:
            name = f"{base} {taken}"
            taken += 1
        try:
            self._playlists.add(name, paths, self._directory)
        except PlaylistError as exc:
            messagebox.showerror("Playlists", str(exc))
            return None
        self._load_playlist_into(name)
        self._save_settings()
        note = f" - {skipped} entries skipped (missing files)" if skipped else ""
        self.status(f"imported '{name}': {len(paths)} tracks{note}")
        return name

    def queue_row_move(self, delta: int) -> None:
        """Ctrl+Up / Ctrl+Down while a playlist is loaded: move the selected row."""
        if self._typing() or self.order_var.get() != library.ORDER_PLAYLIST:
            return
        selection = self.tree.selection()
        if len(selection) != 1:
            return
        row = selection[0]
        if self.tree.get_children(row):
            return                      # a folder row (not in a playlist, still)
        row_to_path = {r: p for p, r in self._row_of_path.items()}
        if row not in row_to_path:
            return
        visible = [row_to_path[r] for r in self.tree.get_children("") if r in row_to_path]
        try:
            index = visible.index(row_to_path[row])
        except ValueError:
            return
        target = index + delta
        if not (0 <= target < len(visible)):
            return
        paths = list(self._playlist_paths)
        try:
            a = paths.index(row_to_path[row])
            b = paths.index(visible[target])
        except ValueError:
            return
        paths[a], paths[b] = paths[b], paths[a]
        self._playlist_paths = paths
        self._playlist_dirty = True
        self.save_playlist(self._active_playlist)
        self.rebuild_queue(keep_playing=True)

    def _tree_drag_start(self, event) -> None:
        """Grab a queue row to move it (playlist mode only)."""
        self._drag_row = ""
        if (self.order_var.get() != library.ORDER_PLAYLIST
                or event.state & 0x0005 or len(self.tree.selection()) > 1):
            return
        try:
            if self.tree.identify_region(event.x, event.y) not in ("cell", "tree"):
                return
            row = self.tree.tk.call(self.tree._w, "identify", "row", event.x, event.y)
        except (tk.TclError, TypeError):
            return
        if not row or self.tree.get_children(row):
            return
        self._drag_row = row
        self._drag_moved = False
        self._drag_origin = (event.x_root, event.y_root)

    def _tree_drag_motion(self, event) -> None:
        row = self._drag_row
        if not row or not self.tree.exists(row):
            return
        if not self._drag_moved:
            ox, oy = self._drag_origin
            if abs(event.x_root - ox) + abs(event.y_root - oy) < 6:
                return                  # still just a click
            self._drag_moved = True
            try:
                # the Tcl command: this Tk's Python wrapper has no tag_add
                self.tree.tk.call(self.tree._w, "tag", "add", "dragging", row)
            except (tk.TclError, TypeError, AttributeError):
                self._drag_row = ""
                return
        try:
            over = self.tree.tk.call(self.tree._w, "identify", "row", event.x, event.y)
        except (tk.TclError, TypeError):
            return
        if not over or over == row or self.tree.get_children(over):
            return
        tree = self.tree
        parent = tree.parent(row)
        children = tree.get_children(parent)
        if children.index(row) == children.index(over):
            return
        tree.move(row, parent, children.index(over))

    def _tree_drag_end(self, event) -> None:
        row = self._drag_row
        if not row:
            return
        self._drag_row = ""
        moved = self._drag_moved
        self._drag_moved = False
        if not moved:
            return
        try:
            if self.tree.exists(row):
                self.tree.tk.call(self.tree._w, "tag", "remove", "dragging", row)
        except (tk.TclError, TypeError, AttributeError):
            return
        if not self.tree.exists(row) or self.order_var.get() != library.ORDER_PLAYLIST:
            return
        row_to_path = {r: p for p, r in self._row_of_path.items()}
        visible = [row_to_path[r] for r in self.tree.get_children("") if r in row_to_path]
        paths = list(self._playlist_paths)
        shown = set(visible)
        slots = [i for i, p in enumerate(paths) if p in shown]
        if not visible or len(slots) != len(visible):
            return                      # tree and playlist disagree: leave it alone
        for slot, path in zip(slots, visible):
            paths[slot] = path
        if paths != self._playlist_paths:
            self._playlist_paths = paths
            self._playlist_dirty = True
            self.save_playlist(self._active_playlist)
            self.rebuild_queue(keep_playing=True)

    def play_selected(self) -> None:
        selection = self.tree.selection()
        if not selection:
            return
        focused = self.tree.focus()
        chosen = focused if focused in selection else selection[0]
        path = next((p for p, row in self._row_of_path.items() if row == chosen), None)
        if path is None:            # a folder row was selected
            return
        index = next((i for i, t in enumerate(self.queue) if t.path == path), None)
        if index is not None:
            self._play_index(index)

    def select_relative(self, delta: int) -> None:
        selection = self.tree.selection()
        if not selection:
            return
        # walk the flattened visible order (skipping folder rows)
        visible = self._visible_rows()
        if not visible:
            return
        try:
            pos = visible.index(selection[0])
        except ValueError:
            pos = 0
        new = visible[max(0, min(len(visible) - 1, pos + delta))]
        self.tree.selection_set(new)
        self.tree.see(new)

    def _visible_rows(self) -> list[str]:
        out: list[str] = []

        def walk(parent=""):
            for item in self.tree.get_children(parent):
                if self.tree.get_children(item):
                    walk(item)
                else:
                    out.append(item)
        walk("")
        return out

    def _play_index(self, index: int, subsong: Optional[int] = None, paused: bool = False,
                    position: float = 0.0) -> None:
        """Load the queue entry at index (optionally paused, at position)."""
        if not (0 <= index < len(self.queue)):
            return
        self._record_listening(self.engine.snapshot())
        self.queue_index = index
        track = self.queue[index]
        self._mark_playing(track.path)
        label = track.name
        self.title_label.configure(text=label)
        self.subtitle_label.configure(text=track.rel_dir or ".")
        self._now_playing = True
        self._update_window_title(track.name)
        self.settings.subsong = subsong if subsong is not None else 0
        self.subsong_var.set(str(self.settings.subsong))
        self.engine.play_path(track.path, position=max(0.0, position), paused=paused,
                              loop=self.settings.loop_track, subsong=self.settings.subsong)
        self.status(f"{track.name}: paused at {format_time(position)}" if paused
                    else f"playing {track.name}")

    def next_track(self, auto: bool = False) -> None:
        if not self.queue:
            return
        if self.queue_index + 1 < len(self.queue):
            self._play_index(self.queue_index + 1)
            return
        if not self.settings.loop_queue:
            if auto:
                self.engine.pause()
                self._now_playing = False
                self._set_window_title(APP_TITLE)
                self.status("queue finished - enable 'Repeat queue' to start over "
                            "(shuffle mode then draws a new order)")
            else:
                self.status("end of the queue - enable 'Repeat queue' to start over")
            return
        self.restart_queue()

    def restart_queue(self) -> None:
        """Repeat the queue; for shuffle, draw a new order with a different opening track."""
        finished = ""
        if 0 <= self.queue_index < len(self.queue):
            finished = self.queue[self.queue_index].path
        if self.order_var.get() == library.ORDER_SHUFFLE and len(self.queue) > 1:
            self._draw_shuffle_plan(new_seed=random.SystemRandom().randrange(1, 2 ** 31),
                                    avoid_first=finished)
            self.rebuild_queue()
            opening = self.queue[0].name if self.queue else "?"
            self.status(f"queue finished - new shuffle order, starting with {opening}")
        else:
            self.status("queue finished - starting over")
        self._play_index(0)

    def on_queue_loop_toggle(self) -> None:
        self.settings.loop_queue = bool(self.queue_loop_var.get())
        self._save_settings()
        if self.settings.loop_queue:
            self.status("repeat queue on - the queue starts over when it ends"
                        + (" (shuffle: new order each round)" if
                           self.order_var.get() == library.ORDER_SHUFFLE else ""))
        else:
            self.status("repeat queue off")
        self.log(MSG_INFO, f"repeat queue {'on' if self.settings.loop_queue else 'off'}")

    def sync_setting_toggles(self) -> None:
        """Reflect settings changed elsewhere (settings dialog) in the transport bar."""
        self.loop_var.set(bool(self.settings.loop_track))
        self.queue_loop_var.set(bool(self.settings.loop_queue))
        if self.settings.cache_analysis and self.analysis_cache is None:
            self.analysis_cache = AnalysisCache(self._cache_file())
            if not self._cache_loaded:
                self.analysis_cache.load()
                self._cache_loaded = True

    def prev_track(self) -> None:
        if not self.queue:
            return
        snapshot = self.engine.snapshot()
        if snapshot.position > 3.0 and not snapshot.finished:
            self.engine.seek(0.0)          # like every other player: restart first
            return
        if self.queue_index > 0:
            self._play_index(self.queue_index - 1)
        elif self.settings.loop_queue:
            self._play_index(len(self.queue) - 1)

    def toggle_play(self) -> None:
        snapshot = self.engine.snapshot()
        if not snapshot.loaded:
            if self.queue and self.queue_index < 0:
                self._play_index(0)
            elif self.queue_index >= 0:
                self._play_index(self.queue_index)
            return
        self.engine.toggle_pause()

    def stop(self) -> None:
        self.engine.pause()
        self.engine.seek(0.0)
        self._now_playing = False
        self._set_window_title(APP_TITLE)
        self.status("stopped")

    def seek_relative(self, seconds: float) -> None:
        if not self._typing():
            self.engine.seek_relative(seconds)

    def on_loop_toggle(self) -> None:
        self.settings.loop_track = bool(self.loop_var.get())
        self.engine.set_loop(self.settings.loop_track)
        self._save_settings()
        self.status(f"loop {'on' if self.settings.loop_track else 'off'}")

    def on_volume(self, _value=None) -> None:
        """Clamp the slider, readout, and engine volume to 0..100."""
        requested = spin_number(self.volume_var)
        volume = 100.0 if requested is None else max(0.0, min(100.0, requested))
        if requested != volume:
            self.volume_var.set(volume)      # the slider follows the read-out
        self.engine.set_volume(volume / 100.0)
        self.engine.set_muted(False)
        self.mute_btn.configure(text="Mute")
        self.volume_label.configure(text=f"{int(round(volume))}%")
        if not self._updating:
            self.settings.volume = int(round(volume))
            self.settings.muted = False
            self._save_settings()

    def set_volume_value(self, value: float) -> None:
        """Move the slider to value (clamped to 0..100) and apply it."""
        self.volume_var.set(max(0.0, min(100.0, float(value))))
        self.on_volume()

    def bump_volume(self, delta: float) -> None:
        """The + and - keys: one step up or down, unless the user is typing."""
        if self._typing():
            return
        self.set_volume_value(self._volume_now() + delta)

    def _volume_now(self) -> float:
        """The level the slider shows, as a number (0..100)."""
        value = spin_number(self.volume_var)
        return 100.0 if value is None else max(0.0, min(100.0, value))

    def on_volume_wheel(self, event=None):
        """Change volume for X11 or signed wheel events, then stop the event from scrolling the
        queue."""
        number = getattr(event, "num", 0)
        if number not in (4, 5):
            number = 0
        delta = getattr(event, "delta", 0) or 0
        if number == 4 or (not number and delta > 0):
            step = VOLUME_WHEEL_STEP
        elif number == 5 or (not number and delta < 0):
            step = -VOLUME_WHEEL_STEP
        else:
            return "break"                      # a wheel event with no direction
        self.set_volume_value(self._volume_now() + step)
        return "break"

    def toggle_mute(self) -> None:
        muted = not self.engine.muted
        self.engine.set_muted(muted)
        self.mute_btn.configure(text="Unmute" if muted else "Mute")
        self.settings.muted = muted
        self._save_settings()
        self.status("muted" if muted else "unmuted")

    def _on_subsong(self) -> None:
        try:
            index = int(self.subsong_var.get())
        except ValueError:
            return
        self.engine.select_subsong(index)

    def _on_wheel(self, event, direction: int = 0) -> None:
        """Wheel = volume, except over the position slider, where it seeks."""
        delta = direction or (1 if getattr(event, "delta", 0) > 0 else -1)
        widget = getattr(event, "widget", None)
        if widget is self.seek_bar:
            self.engine.seek_relative(2.0 * delta)
            return
        self.bump_volume(2.0 * delta)

    def _seek_press(self, event=None) -> None:
        """Click on the bar: jump to that position right away."""
        self._seeking = True
        self._press_fraction = None
        if event is not None:
            self._press_fraction = self.seek_bar.fraction_of(event.x)
            self._seek_to_x(event.x, commit=True)      # single click = seek
        return "break"

    def _seek_drag(self, event=None) -> None:
        """Dragging only moves the bar (and the time read-out) - seeking on every
        pixel would machine-gun the audio engine with ring-buffer flushes."""
        if not self._seeking:
            return
        if event is not None:
            self._seek_to_x(event.x, commit=False)

    def _seek_release(self, event=None) -> None:
        if not self._seeking:
            return
        self._seeking = False
        if event is None:
            self._commit_seek()                        # called without an event
            return
        fraction = self.seek_bar.fraction_of(event.x)
        if self._press_fraction is not None and abs(fraction - self._press_fraction) < 1e-6:
            return                                     # plain click: already done on press
        self._seek_to_x(event.x, commit=True)          # drag: the final spot wins

    def _seek_to_x(self, x: float, commit: bool) -> None:
        fraction = self.seek_bar.fraction_of(x)
        self.position_var.set(fraction * 1000.0)
        self.seek_bar.set_fraction(fraction)
        target = self._target_seconds(fraction)
        if target is not None:
            self.time_label.configure(text=format_time(target))
        if commit:
            self._commit_seek(fraction)

    def _target_seconds(self, fraction: float):
        snap = self._last_snapshot or self.engine.snapshot()
        if snap.duration_valid and snap.duration > 0:
            return max(0.0, min(1.0, fraction)) * snap.duration
        return None

    def _commit_seek(self, fraction: Optional[float] = None) -> None:
        if fraction is None:
            fraction = float(self.position_var.get()) / 1000.0
        snap = self._last_snapshot or self.engine.snapshot()
        target = self._target_seconds(fraction)
        if target is None:
            # unknown length: the bar acts as a relative window (see seek_fraction)
            self.engine.seek_fraction(fraction)
            self.status("seek")
            return
        current = snap.position % snap.duration if (snap.loop and snap.duration) else snap.position
        span = max(self.seek_bar.winfo_width() - 2 * SeekBar.MARGIN, 1)
        tolerance = max(0.25, 3.0 * snap.duration / span)
        if snap.loaded and abs(target - current) < tolerance:
            return
        self.engine.seek(target)
        self.status(f"seek to {format_time(target)}" +
                    (f" of {format_time(snap.duration)}" if snap.duration_valid else ""))
        fresh = self.engine.snapshot()
        self._last_snapshot = fresh
        self._update_from_snapshot(fresh)
        self._update_tracker(fresh)
        self.apply_update_rate(delay_ms=25)

    def _drain_thread_queue(self) -> None:
        while True:
            try:
                kind, payload = self.queue_ui.get_nowait()
            except queue.Empty:
                return
            if kind == "progress":
                count, where = payload
                self.status(f"scanning… {count} modules found ({os.path.basename(where)})")
            elif kind == "scanned":
                self._on_scanned(*payload)
            elif kind == "analyzed":
                self._on_analyzed(*payload)
            elif kind == "reveal":
                ok, message = payload
                self._reveal_pending = False
                self.log(MSG_INFO if ok else MSG_WARN, message)
                self.status(message)
            elif kind == "analyzed_done":
                done, failed = payload
                self.status(f"analysis finished: {done} modules"
                            + (f", {failed} unreadable" if failed else ""))
                self.log(MSG_INFO, f"analyzed {done} modules ({failed} failed)")
                if self.analysis_cache is not None:
                    self.analysis_cache.save()      # keep the work, even if we crash
            elif kind == "cache_loaded":
                entries, = payload
                if entries:
                    self.log(MSG_DEBUG, f"analysis cache: {entries} modules remembered")
            elif kind == "error":
                self.log(MSG_ERROR, payload)
                self.status(payload)
                self.count_label.configure(text="")

    def _handle_engine_events(self) -> None:
        for level, message in [(m.level, m.text) for m in self.engine.poll_messages()]:
            self.log(level, message)
        for kind, payload in self.engine.poll_events():
            if kind == "song":
                if int(payload.get("token", 0)) == self._song_token:
                    self._tracker.set_song(payload.get("orders", []),
                                           payload.get("rows", {}),
                                           payload.get("channels", 0),
                                           payload.get("path", ""),
                                           payload.get("format_name", ""))
                    self._tracker.request_visible()
                continue
            if kind == "pattern":
                if int(payload.get("token", 0)) == self._song_token:
                    self._tracker.add_pattern(int(payload.get("pattern", -1)),
                                              payload.get("cells") or [],
                                              payload.get("error", ""))
                continue
            if kind == "finished":
                self._on_track_finished(payload)
            elif kind == "load_failed":
                path = payload.get("path", "")
                track = next((t for t in self.tracks + self._queue_source_tracks() if t.path == path), None)
                if track is not None:
                    track.broken = payload.get("error") or "unreadable"
                    self.set_row_broken(path, True)
                self.status(f"cannot play {os.path.basename(path)}: "
                            f"{payload.get('error', '')[:60]}")
                if self.settings.auto_skip_broken:
                    self._finished_countdown = time.monotonic() + 1.2
            elif kind == "stalled":
                reason = payload.get("reason", "")
                silent_freeze = "silent" in reason
                if silent_freeze and self.settings.auto_skip_broken:
                    self.status(f"module is silent and stuck ({reason}) - skipping")
                    self.log(MSG_WARN, f"{os.path.basename(payload.get('path', ''))}: {reason}")
                    self._finished_countdown = time.monotonic() + 1.0
                else:
                    self.status(f"warning: {reason} - press Page Down to skip "
                                f"(or wait, it may still play on)")
                    self.log(MSG_WARN, f"{os.path.basename(payload.get('path', ''))}: {reason}")
            elif kind == "overrun":
                self.status("module runs past its official length - skipping")
                if self.settings.auto_skip_broken:
                    self._finished_countdown = time.monotonic() + 1.0
            elif kind == "restarted":
                self.status(f"recovered playback ({payload.get('reason', '')}), "
                            f"attempt {payload.get('restarts', 1)}")
            elif kind == "track_broken":
                path = payload.get("path", "")
                track = next((t for t in self.tracks + self._queue_source_tracks() if t.path == path), None)
                if track is not None:
                    track.broken = f"unplayable ({payload.get('reason', '')})"
                    self.set_row_broken(path, True)
                self.status(f"{os.path.basename(path)} is unplayable - skipping")
                if self.settings.auto_skip_broken:
                    self._finished_countdown = time.monotonic() + 1.0

    def _on_track_finished(self, payload: dict) -> None:
        """A module reached its natural end (engine event 'finished')."""
        self.log(MSG_INFO, f"finished {os.path.basename(payload.get('path', ''))} "
                           f"({format_time(payload.get('duration'))})")
        if payload.get("loop"):
            return                      # looping track: only the user stops it
        if self.settings.auto_advance:
            self.next_track(auto=True)

    def _update_from_snapshot(self, snap) -> None:
        self._label_text("play", self.btn_play, "▮▮" if snap.playing else "▶")

        duration = snap.duration if snap.duration_valid else 0.0
        position = snap.position
        if snap.loop and duration > 0:
            position = position % duration
        if not self._seeking:      # during a drag the label shows the drag target
            self._label_text("time", self.time_label,
                             format_time(position if snap.loaded else None))
        self._label_text("duration", self.duration_label,
                         format_time(duration) if duration else "--:--")

        self.seek_bar.set_total(duration, snap.duration_valid)
        self.seek_bar.set_enabled(snap.loaded)
        self._sync_play_controls(snap)
        if not self._seeking:
            fraction = (position / duration) if duration else 0.0
            self._updating = True
            try:
                self.position_var.set(max(0.0, min(1.0, fraction)) * 1000.0)
                self.seek_bar.set_fraction(fraction)
            finally:
                self._updating = False

        loop_note = ""
        if snap.playing:
            loop_note = (f"loop #{snap.loop_index + 1}"
                         if snap.loop and snap.loop_index else ("looping" if snap.loop else ""))
        elif snap.paused and snap.loaded:
            loop_note = "paused"
        elif snap.finished:
            loop_note = "finished"
        self._label_text("loop", self.loop_label, loop_note)

        if snap.path and self._now_playing:
            info = snap.info
            self._update_window_title(os.path.basename(snap.path),
                                      title=(info.title if info else ""),
                                      artist=(info.artist if info else ""))
        else:
            self._set_window_title(APP_TITLE)

        info = snap.info
        if info is not None:
            self._label_text("panel-format", self.info_labels["format"],
                             f"{info.format_long or info.format.upper() or '?'}")
            self._label_text("panel-tracker", self.info_labels["tracker"], info.tracker or "?")
            self._label_text("panel-artist", self.info_labels["artist"], info.artist or "?")
            self._label_text("panel-size", self.info_labels["size"],
                             f"{info.num_channels} ch, {info.num_orders} ord, "
                             f"{info.num_patterns} pat, "
                             f"{info.num_instruments or info.num_samples} smp")
            self._label_text("panel-length", self.info_labels["length"],
                             format_time(info.duration))
            self._label_text("panel-title", self.title_label,
                             info.title or os.path.basename(snap.path))
            if snap.num_subsongs > 1:
                if self._subsong_to != snap.num_subsongs - 1 \
                        or self._subsong_state != "normal":
                    self._subsong_to, self._subsong_state = snap.num_subsongs - 1, "normal"
                    self.subsong_spin.configure(state="normal", to=snap.num_subsongs - 1)
                self._label_text("panel-subsong", self.subsong_label,
                                 f"of {snap.num_subsongs}")
            else:
                if self._subsong_to != 0 or self._subsong_state != "disabled":
                    self._subsong_to, self._subsong_state = 0, "disabled"
                    self.subsong_spin.configure(state="disabled", to=0)
                self._label_text("panel-subsong", self.subsong_label, "(single song)")
        if snap.loaded:
            self._label_text("panel-position", self.info_labels["position"],
                             f"order {snap.order}, pattern {snap.pattern}, "
                             f"row {snap.row:02d}")
            self._label_text("panel-sequencer", self.info_labels["sequencer"],
                             f"speed {snap.speed}, tempo {snap.tempo}, "
                             f"{snap.playing_channels} ch on")
            self._label_text("panel-resample", self.info_labels["resample"],
                             interpolation_label(interpolation_name(snap.interpolation))
                             or interpolation_label(self.settings.interpolation) or "?")
            self._label_text("panel-output", self.info_labels["output"],
                             f"{self.engine.output.name} {self.engine.samplerate / 1000:g}kHz, "
                             f"buf {snap.buffer_seconds * 1000:.0f} ms"
                             + (f", restarts {snap.restarts}" if snap.restarts else ""))
        else:
            for key in ("position", "sequencer"):
                self._label_text("panel-" + key, self.info_labels[key], "-")

        self._follow_song_info(snap)

        if self._panel_shows[1] and self.vu.winfo_ismapped():
            self.vu.set_channels(snap.num_channels or 4)
            if self.vu.values_changed(snap.vu, snap.level):
                self.vu.update_values(snap.vu, snap.level)
                self.vu.redraw()

        self._sync_panel_need()

        bits = []
        if snap.stalled:
            bits.append(f"stalled {snap.stall_seconds:.0f}s")
        if self.engine.output.xruns:
            bits.append(f"{self.engine.output.xruns} audio glitches")
        if snap.restarts:
            bits.append(f"{snap.restarts} restarts")
        if snap.buffer_seconds:
            bits.append(f"buffer {snap.buffer_seconds * 1000:.0f} ms")
        self._label_text("health", self.health_label, ", ".join(bits))

    def _sync_play_controls(self, snap) -> None:
        """Update transport button states only when they change."""
        loaded = bool(snap.loaded)
        # (the position bar has its own guard, set from the same snapshot value)
        for key, want in (("prev", True), ("next", bool(self.queue)),
                          ("play", True), ("stop", loaded)):
            if self._control_state.get(key) == want:
                continue
            self._control_state[key] = want
            widget = {"prev": self.btn_prev, "next": self.btn_next,
                      "play": self.btn_play, "stop": self.btn_stop}[key]
            try:
                widget.state(["!disabled"] if want else ["disabled"])
            except tk.TclError:              # shutting down
                pass

    def _tick_ms(self) -> int:
        """Return the UI refresh interval; this does not change audio rendering speed."""
        try:
            fps = int(self.settings.ui_fps) or 60
        except (TypeError, ValueError):
            fps = 60
        return max(8, int(round(1000.0 / max(5, min(fps, 120)))))

    def apply_update_rate(self, delay_ms: Optional[int] = None) -> None:
        """Re-arm the tick so a new rate - or a one-off, a seek - takes effect
        straight away instead of after the frame that is already queued."""
        if self._tick_id is not None and not self._closing:
            try:
                self.root.after_cancel(self._tick_id)
            except tk.TclError:
                pass
            self._tick_id = self.root.after(self._tick_ms() if delay_ms is None
                                            else int(delay_ms), self._tick)

    def _tick(self) -> None:
        if self._closing:
            return
        try:
            self._remember_opening_split()
            self._drain_thread_queue()
            self._record_listening(self.engine.snapshot())
            self._handle_engine_events()
            snap = self.engine.snapshot()
            self._last_snapshot = snap
            self._update_from_snapshot(snap)
            self._update_tracker(snap)
            self._save_session()               # throttled: keeps the position fresh
            if self._finished_countdown is not None and time.monotonic() > self._finished_countdown:
                self._finished_countdown = None
                self.next_track(auto=True)
        except Exception as exc:  # never let the UI loop die
            self.log(MSG_ERROR, f"ui tick: {exc!r}")
        finally:
            if not self._closing:
                self._tick_id = self.root.after(self._tick_ms(), self._tick)

    def configure_tree_tags(self) -> None:
        """Give the queue's row tags their colours (also on a scheme switch)."""
        self.tree.tag_configure("playing", foreground=ON_ACCENT, background=ACCENT_DIM)
        self.tree.tag_configure("broken", foreground=RED)
        self.tree.tag_configure("dir", foreground=ACCENT)
        self.tree.tag_configure("dragging", foreground=FG_DIM)
        self.tree.tag_configure("odd", background=BG_ALT)
        self.tree.tag_configure("alt", background=BG_STRIPE)

    def configure_log_tags(self) -> None:
        """Give the log's level tags their colours (also on a scheme switch)."""
        self.log_text.tag_configure(MSG_ERROR, foreground=RED)
        self.log_text.tag_configure(MSG_WARN, foreground=AMBER)
        self.log_text.tag_configure(MSG_INFO, foreground=FG_DIM)
        self.log_text.tag_configure(MSG_DEBUG, foreground=FG_FAINT)
        # Selection colours must override log severity tags.
        self.log_text.tag_raise("sel")

    def refresh_theme(self) -> None:
        """Repaint the running window in the palette that is in force now."""
        self._dismiss_playlist_menu()
        apply_theme(self.root)                    # ttk styles + option database
        self.configure_tree_tags()
        self.configure_log_tags()
        self.log_text.configure(bg=BG_INPUT, fg=FG_DIM, insertbackground=FG,
                                selectbackground=ACCENT_DIM, selectforeground=ON_ACCENT,
                                inactiveselectbackground=ACCENT_DIM)
        for label in (self.count_label, self.filter_label, self.health_label,
                      self.status_label):
            label.configure(background=BG)
        self.vu.apply_theme()
        self.seek_bar.apply_theme()
        self._tracker.apply_theme()
        window = getattr(self, "_song_info", None)
        if window is not None:
            window.apply_theme()
        dialog = self._playlist_dialog
        if dialog is not None and dialog.winfo_exists():
            dialog.apply_theme()
        prompt = self._new_playlist_dialog
        if prompt is not None and prompt.winfo_exists():
            prompt.apply_theme()
        stats = self._stats_window
        if stats is not None and stats.winfo_exists():
            stats.apply_theme()

    def apply_theme_name(self, name: str) -> str:
        """Repaint with the selected palette and return its resolved name."""
        before = theme.current()
        chosen = theme.activate(name)
        self.settings.theme = chosen
        if chosen != before:
            self.refresh_theme()
            self.log(MSG_INFO, f"colour scheme: {theme.label(chosen)}")
        return chosen

    def _save_settings(self) -> None:
        self.settings.save(self.config_file)

    def status(self, text: str) -> None:
        self._status_text = text
        self.status_label.configure(text=text)

    def _label_text(self, key: str, widget, text: str) -> bool:
        """Change a widget label only if needed; return whether it changed."""
        if self._last_text.get(key) == text:
            return False
        self._last_text[key] = text
        try:
            widget.configure(text=text)
        except tk.TclError:                  # shutting down
            return False
        return True

    def _panel_signature(self) -> tuple:
        """What the info panel's height depends on (cheap: the texts we cached)."""
        keys = ("panel-title", "panel-format", "panel-tracker", "panel-artist",
                "panel-size", "panel-length", "panel-subsong")
        return tuple(self._last_text.get(key, "") for key in keys) + (
            self._panel_parent.winfo_width() if self._panel_parent else 0,
            self._panel_shows)

    def _set_reveal_enabled(self, enabled: bool) -> None:
        """The button only makes sense once the player holds a module."""
        if enabled == self._reveal_enabled:
            return
        self._reveal_enabled = enabled
        try:
            self.reveal_btn.state(["!disabled"] if enabled else ["disabled"])
        except tk.TclError:                      # closing down
            pass

    def apply_interpolation(self, mode: str) -> None:
        """Apply and remember the resampling filter without reloading the module."""
        mode = str(mode)
        self.settings.interpolation = mode
        self.engine.set_interpolation(mode)
        label = interpolation_label(mode) or mode
        self.status(f"resampling: {label}")
        row = self.info_labels.get("resample")
        if row is not None:
            row.configure(text=label)

    def reveal_current(self) -> None:
        """Ask a file manager to select the module, using a worker so the UI stays responsive."""
        path = self._playing_path or (self.engine.snapshot().path or "")
        if not path:
            self.status("nothing is playing yet - pick a module first")
            return
        if self._reveal_pending:                 # still waiting for the last one
            return
        self._reveal_pending = True
        self.status(f"opening the folder of {os.path.basename(path)}…")

        def work() -> None:
            try:
                ok, message = reveal.reveal(path, timeout=REVEAL_TIMEOUT)
            except Exception as exc:             # never kill the worker silently
                ok, message = False, f"could not open a file manager ({exc!r})"
            self.queue_ui.put(("reveal", (ok, message)))

        threading.Thread(target=work, name="reveal", daemon=True).start()

    def log(self, level: str, text: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        self.log_text.configure(state="normal")
        self.log_text.insert("end", f"{stamp} {text}\n", level)
        lines = int(self.log_text.index("end-1c").split(".")[0])
        if lines > 400:
            self.log_text.delete("1.0", "50.0")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def on_close(self) -> None:
        self._record_listening(self.engine.snapshot())
        store = self._listening.store
        if not store.save(force=True) and store.dirty:
            messagebox.showwarning("Listening stats not saved", store.error, parent=self.root)
        self._dismiss_playlist_menu()
        self._closing = True
        if self._tick_id is not None:
            try:
                self.root.after_cancel(self._tick_id)
            except Exception:
                pass
            self._tick_id = None
        try:
            self.settings.window_geometry = self.root.winfo_geometry()
            self._save_session(force=True)
            self._save_settings()
            if self.analysis_cache is not None:
                self.analysis_cache.save()
        except Exception:
            pass
        if self._scan_thread and self._scan_thread.is_alive():
            self._scan_cancel.set()
        if self._analyzer and self._analyzer.is_alive():
            self._analyzer.cancel.set()
        try:
            self.engine.shutdown()
        except Exception:
            pass
        self.root.destroy()


def run(args) -> int:  # pragma: no cover - entry point
    """Start the GUI."""
    root = tk.Tk()
    settings = Settings.load()
    if getattr(args, "backend", None):
        settings.backend = args.backend
    if getattr(args, "volume", None) is not None:
        settings.volume = max(0, min(100, int(args.volume)))   # --volume is 0..100
    if getattr(args, "interpolation", None):
        settings.interpolation = args.interpolation
    if getattr(args, "theme", None):
        settings.theme = args.theme      # the palette is bound in PlayerApp
    app = PlayerApp(root, settings=settings, directory=args.directory or "",
                    backend=args.backend, speed=getattr(args, "speed", 1.0),
                    autoplay=getattr(args, "autoplay", False))
    track = getattr(args, "track", None)
    if track:
        target = os.path.abspath(track)

        def start_track():
            index = next((i for i, t in enumerate(app.queue) if t.path == target), None)
            if index is not None:
                app._play_index(index)
            elif app.queue:
                app._play_index(0)

        root.after(1200, start_track)
    root.mainloop()
    return 0
