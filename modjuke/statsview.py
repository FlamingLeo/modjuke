"""Non-modal, theme-aware listening history (all data stays on this device)."""
from __future__ import annotations

from datetime import datetime
import os
import tkinter as tk
from tkinter import messagebox, ttk
import time

from .theme import BG, FG_DIM, RED

ORDERS = {"Most played": "plays", "Most listening time": "seconds", "Recently played": "last_played"}
MAX_ROWS = 1000


def listening_time(seconds) -> str:
    seconds = max(0, int(seconds))
    hours, rem = divmod(seconds, 3600)
    minutes, secs = divmod(rem, 60)
    return f"{hours}h {minutes:02d}m {secs:02d}s" if hours else f"{minutes}m {secs:02d}s"


def local_date(timestamp) -> str:
    try:
        return datetime.fromtimestamp(timestamp).strftime("%Y-%m-%d %H:%M") if timestamp else "-"
    except (ValueError, OSError, OverflowError):
        return "-"


class StatsWindow(tk.Toplevel):
    def __init__(self, app):
        app._dismiss_playlist_menu()
        super().__init__(app.root)
        self.app = app
        self.title("Listening stats")
        self.transient(app.root)
        self.configure(bg=BG)
        self.geometry("980x570")
        self.minsize(760, 440)
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        body = ttk.Frame(self, padding=14)
        body.grid(row=0, column=0, sticky="nsew")
        body.columnconfigure(0, weight=1)
        body.rowconfigure(3, weight=1)
        self.summary = ttk.Label(body, text="", font=("TkDefaultFont", 11))
        self.summary.grid(row=1, column=0, sticky="w", pady=(10, 14))
        tools = ttk.Frame(body)
        tools.grid(row=2, column=0, sticky="ew", pady=(0, 8))
        ttk.Label(tools, text="Rank by").pack(side="left")
        self.order = tk.StringVar(self, value="Most played")
        self.order_box = ttk.Combobox(tools, textvariable=self.order, values=tuple(ORDERS),
                                      state="readonly", width=22)
        self.order_box.pack(side="left", padx=(6, 20))
        self.order_box.bind("<<ComboboxSelected>>", lambda _e: self.refresh(force=True))
        ttk.Label(tools, text="Search").pack(side="left")
        self.query = tk.StringVar(self)
        self.search = ttk.Entry(tools, textvariable=self.query)
        self.search.pack(side="left", fill="x", expand=True, padx=(6, 0))
        table = ttk.Frame(body)
        table.grid(row=3, column=0, sticky="nsew")
        table.columnconfigure(0, weight=1)
        table.rowconfigure(0, weight=1)
        self.tree = ttk.Treeview(table, columns=("plays", "time", "last", "folder"), selectmode="browse")
        for key, label, width, anchor in (("#0", "Module", 240, "w"), ("plays", "Plays", 65, "e"),
                                         ("time", "Listening time", 135, "e"),
                                         ("last", "Last played (local)", 155, "w"), ("folder", "Folder", 260, "w")):
            self.tree.heading(key, text=label)
            self.tree.column(key, width=width, minwidth=50, anchor=anchor, stretch=key in ("#0", "folder"))
        self.tree.heading("plays", command=lambda: self.rank_by("Most played"))
        self.tree.heading("time", command=lambda: self.rank_by("Most listening time"))
        self.tree.heading("last", command=lambda: self.rank_by("Recently played"))
        self.tree.grid(row=0, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(table, orient="vertical", command=self.tree.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        horizontal = ttk.Scrollbar(table, orient="horizontal", command=self.tree.xview)
        horizontal.grid(row=1, column=0, sticky="ew")
        self.tree.configure(yscrollcommand=scroll.set, xscrollcommand=horizontal.set)
        self.tree.bind("<<TreeviewSelect>>", self.show_path)
        self.path = tk.StringVar(self)
        self.path_entry = ttk.Entry(body, textvariable=self.path, state="readonly")
        self.path_entry.grid(row=4, column=0, sticky="ew", pady=(8, 0))
        self.note = ttk.Label(body, background=BG, foreground=FG_DIM, wraplength=900, justify="left")
        self.note.grid(row=5, column=0, sticky="w", pady=(8, 0))
        self.error = ttk.Label(body, background=BG, foreground=RED, wraplength=900, justify="left")
        self.error.grid(row=6, column=0, sticky="w", pady=(4, 0))
        buttons = ttk.Frame(body)
        buttons.grid(row=7, column=0, sticky="ew", pady=(10, 0))
        self.reset_btn = ttk.Button(buttons, text="Reset stats…", command=self.reset_stats)
        self.reset_btn.pack(side="left")
        self.close_btn = ttk.Button(buttons, text="Close", command=self.close)
        self.close_btn.pack(side="right")
        self._row_paths = {}
        self._last_refresh = 0.0
        self._revision = -1
        self._trace = self.query.trace_add("write", lambda *_: self.refresh(force=True))
        self.bind("<Escape>", lambda _e: self.close())
        self.bind("<Control-f>", lambda _e: self.search.focus_set())
        self.bind("<Configure>", self._resize, add="+")
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.refresh(force=True)
        self.search.focus_set()

    def _resize(self, event) -> None:
        if event.widget is self:
            width = max(400, self.winfo_width() - 35)
            self.note.configure(wraplength=width)
            self.error.configure(wraplength=width)

    def rank_by(self, value) -> None:
        self.order.set(value)
        self.refresh(force=True)

    def refresh(self, force=False) -> None:
        now = time.monotonic()
        if not force and now - self._last_refresh < 1.0:
            return
        self._last_refresh = now
        store = self.app._listening.store
        self.error.configure(text=store.error)
        if not force and self._revision == store.revision:
            return
        self._revision = store.revision
        seconds, plays, modules = store.totals()
        self.summary.configure(text=f"{listening_time(seconds)} listened, {plays:,} plays, {modules:,} modules")
        selected = self.tree.selection()
        selected_path = self._row_paths.get(selected[0]) if selected else None
        y = self.tree.yview()[0]
        rows = store.ranked(ORDERS[self.order.get()], self.query.get())
        self.tree.delete(*self.tree.get_children())
        self._row_paths.clear()
        for record in rows[:MAX_ROWS]:
            item = self.tree.insert("", "end", text=os.path.basename(record.path),
                                     values=(record.plays, listening_time(record.seconds),
                                             local_date(record.last_played), os.path.dirname(record.path)))
            self._row_paths[item] = record.path
            if record.path == selected_path:
                self.tree.selection_set(item)
        self.tree.yview_moveto(y)
        self.show_path()
        shown = ("No listening recorded yet. Start a module to begin." if not modules else
                 f"{min(len(rows), MAX_ROWS):,} of {len(rows):,} matching modules shown.")
        self.note.configure(text=f"{shown}\nLocal history since {local_date(store.since)}. "
                            "Note that pauses, seeks, track loops and audio restarts do not add plays. "
                            "Time measures running playback. "
                            "Stats are saved every 30 seconds and on exit.")

    def show_path(self, _event=None) -> None:
        selected = self.tree.selection()
        self.path.set(self._row_paths.get(selected[0], "") if selected else "")

    def reset_stats(self) -> None:
        if not messagebox.askyesno("Reset listening stats?",
                                   "Permanently clear all listening time and play counts?\n\n"
                                   "Your music files, playlists and playback will not be changed.\n"
                                   "A playing song will start a fresh stats entry.", parent=self):
            return
        if self.app._listening.store.reset():
            self.app._listening.reset()
        self.refresh(force=True)

    def apply_theme(self) -> None:
        self.configure(bg=BG)
        self.note.configure(background=BG, foreground=FG_DIM)
        self.error.configure(background=BG, foreground=RED)

    def destroy(self) -> None:
        self.query.trace_remove("write", self._trace)
        super().destroy()
        for name in ("summary", "order_box", "search", "tree", "path_entry", "note", "error", "reset_btn", "close_btn"):
            setattr(self, name, None)

    def close(self) -> None:
        app = self.app
        self.destroy()
        if app._stats_window is self:
            app._stats_window = None
        try:
            app.root.focus_set()
            if app.root.focus_get() is None:
                app.root.focus_force()
        except tk.TclError:
            pass
