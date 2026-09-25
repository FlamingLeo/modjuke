"""Non-modal sample, instrument, and comment window. Uses module details supplied by the render
thread."""

from __future__ import annotations

import os
import tkinter as tk
from dataclasses import dataclass, field
from tkinter import font as tkfont
from tkinter import ttk

from .theme import (ACCENT, ACCENT_DIM, BG_INPUT, BG_PANEL, FG, FG_FAINT,
                    ON_ACCENT)

NO_COMMENT = "(no comment)"

MOD_NOTE = ("a MOD file has no comment field")

NOTHING = "start a module and this window follows it"

WHEEL_LINES = 3

_UNSET = object()

@dataclass
class SongFacts:
    """Display-ready module details, independent of the widgets."""

    title: str = "Nothing playing"
    subtitle: str = ""
    samples: list[str] = field(default_factory=list)
    instruments: list[str] = field(default_factory=list)
    comment: str = ""
    note: str = ""

    def group_label(self, kind: str, names: list[str]) -> str:
        """The heading of one list: how many slots, and how many carry a name."""
        if not names:
            return f"{kind} (none)"
        named = sum(1 for name in names if name.strip())
        if named == len(names):
            return f"{kind} ({len(names)})"
        return f"{kind} ({len(names)} slots, {named} named)"


def clean_message(message: str) -> str:
    """Normalize line endings and remove trailing blank lines, preserving the remaining comment."""
    text = (message or "").replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    while lines and not lines[-1].strip():
        lines.pop()
    return "\n".join(lines)


def names_are_the_comment(samples: list[str], comment: str) -> bool:
    """Check whether the comment duplicates the sample-name list; do not assume this from the
    format."""
    names = [name.strip() for name in samples if name.strip()]
    lines = [line.strip() for line in comment.splitlines() if line.strip()]
    return bool(names) and names == lines


def facts_of(info, folder: str = "") -> SongFacts:
    """Work out everything the window shows for info (None: nothing playing)."""
    if info is None:
        return SongFacts(title="Nothing playing", note=NOTHING)
    samples = list(info.sample_names)
    instruments = list(info.instrument_names)
    comment = clean_message(info.message)
    where = (folder or os.path.dirname(info.path) or ".").strip() or "."
    kind = info.format_long or (info.format or "?").upper()
    title = info.title or os.path.basename(info.path) or "(untitled)"
    counts = f"{len(samples)} sample{'' if len(samples) == 1 else 's'}"
    if instruments or not samples:
        counts += f", {len(instruments)} instrument{'' if len(instruments) == 1 else 's'}"
    if not comment:
        note = "this module carries no comment"
    elif (info.format or "").lower() == "mod" and names_are_the_comment(samples, comment):
        note = MOD_NOTE
    else:
        note = ""
    return SongFacts(title=title, subtitle=f"{where}, {kind}, {counts}",
                     samples=samples, instruments=instruments, comment=comment,
                     note=note)


class SongInfoWindow(tk.Toplevel):
    """The window itself."""

    def __init__(self, app: "object"):
        super().__init__(app.root)
        self.app = app
        self.folder = ""
        self.info: object = _UNSET             # what is on screen right now
        self._subsong = -1                     # ...and for which subsong
        self.title("Song info")
        self.configure(bg=BG_PANEL)
        self.transient(app.root)
        self.minsize(400, 320)
        self.comment_font = tkfont.Font(font=("TkFixedFont",))

        wrap = ttk.Frame(self, style="Panel.TFrame", padding=(12, 10))
        wrap.grid(row=0, column=0, sticky="nsew")
        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)
        wrap.columnconfigure(0, weight=1)
        wrap.rowconfigure(2, weight=3)          # the names list grows first
        wrap.rowconfigure(4, weight=2)          # ...then the comment

        self.title_label = ttk.Label(wrap, text="", style="Title.TLabel", anchor="w",
                                     wraplength=560, justify="left")
        self.title_label.grid(row=0, column=0, sticky="ew")
        self.subtitle_label = ttk.Label(wrap, text="", style="Dim.TLabel", anchor="w",
                                        wraplength=560, justify="left")
        self.subtitle_label.grid(row=1, column=0, sticky="ew", pady=(2, 8))

        names = ttk.Frame(wrap, style="Panel.TFrame")
        names.grid(row=2, column=0, sticky="nsew")
        names.columnconfigure(0, weight=1)
        names.rowconfigure(0, weight=1)
        self.tree = ttk.Treeview(names, columns=("n",), show="tree headings", height=9,
                                 selectmode="browse", style="Dlg.Treeview")
        self.tree.heading("#0", text="Name", anchor="w")
        self.tree.heading("n", text="#", anchor="e")
        self.tree.column("#0", width=320, stretch=True)
        self.tree.column("n", width=48, stretch=False, anchor="e")
        tree_bar = ttk.Scrollbar(names, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=tree_bar.set)
        self.tree.grid(row=0, column=0, sticky="nsew")
        tree_bar.grid(row=0, column=1, sticky="ns")

        self.comment_label = ttk.Label(wrap, text="Comment", style="Dim.TLabel", anchor="w")
        self.comment_label.grid(row=3, column=0, sticky="ew", pady=(10, 2))
        box = ttk.Frame(wrap, style="Panel.TFrame")
        box.grid(row=4, column=0, sticky="nsew")
        box.columnconfigure(0, weight=1)
        box.rowconfigure(0, weight=1)
        self.comment = tk.Text(box, height=8, wrap="word", relief="flat", bg=BG_INPUT, fg=FG,
                               insertbackground=FG, padx=6, pady=4, highlightthickness=0,
                               selectbackground=ACCENT_DIM, selectforeground=ON_ACCENT,
                               inactiveselectbackground=ACCENT_DIM,
                               font=self.comment_font, state="disabled")
        comment_bar = ttk.Scrollbar(box, orient="vertical", command=self.comment.yview)
        self.comment.configure(yscrollcommand=comment_bar.set)
        self.comment.grid(row=0, column=0, sticky="nsew")
        comment_bar.grid(row=0, column=1, sticky="ns")

        self.note_label = ttk.Label(wrap, text="", style="Dim.TLabel", anchor="w",
                                    wraplength=560, justify="left")
        self.note_label.grid(row=5, column=0, sticky="ew", pady=(8, 0))

        row = ttk.Frame(wrap, style="Panel.TFrame")
        row.grid(row=6, column=0, sticky="e", pady=(10, 0))
        self.close_btn = ttk.Button(row, text="Close", command=self.close)
        self.close_btn.pack(side="right")

        for widget in (self.tree, self.comment):
            for sequence in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
                widget.bind(sequence, self._on_wheel)
        self.node_colours()
        self.bind("<Escape>", self._on_escape)
        self.protocol("WM_DELETE_WINDOW", self.close)
        self.follow(getattr(app, "_last_snapshot", None) or app.engine.snapshot())
        self.after_idle(self.focus_set)         # so Escape reaches this window

    def follow(self, snap) -> bool:
        """Update only when the module details or subsong change."""
        info = getattr(snap, "info", None)
        kind = getattr(snap, "subsong", 0)
        if info is self.info and kind == self._subsong:
            return False
        self._subsong = kind
        self.update_info(info, self.app.folder_of(getattr(snap, "path", "")))
        return True

    def update_info(self, info, folder: str = "") -> bool:
        """Show info (a ModuleInfo, or None for "nothing playing")."""
        if info is self.info and folder == self.folder:
            return False
        self.info, self.folder = info, folder
        facts = facts_of(info, folder)
        self.title_label.configure(text=facts.title)
        self.subtitle_label.configure(text=facts.subtitle)
        self.title(f"Song info, {facts.title}" if info is not None else "Song info")
        self._fill_names(facts)
        self._fill_comment(facts)
        self.note_label.configure(text=facts.note)
        return True

    def _fill_names(self, facts: SongFacts) -> None:
        """One group per list, one row per slot - the numbering a tracker uses."""
        self.tree.delete(*self.tree.get_children(""))
        for kind, names in (("Samples", facts.samples), ("Instruments", facts.instruments)):
            group = self.tree.insert("", "end", text=facts.group_label(kind, names),
                                     open=True, tags=("group",))
            if not names:
                self.tree.insert(group, "end", text="(no slots in this module)",
                                 values=("",), tags=("dim",))
                continue
            for number, name in enumerate(names, start=1):
                empty = not name.strip()
                self.tree.insert(group, "end", text="(unnamed)" if empty else name,
                                 values=(number,), tags=("dim",) if empty else ())

    def _fill_comment(self, facts: SongFacts) -> None:
        self.comment.configure(state="normal")
        self.comment.delete("1.0", "end")
        if facts.comment:
            self.comment.insert("1.0", facts.comment)
        else:
            self.comment.insert("1.0", NO_COMMENT)
            self.comment.tag_add("dim", "1.0", "end")
        self.comment.configure(state="disabled")

    def _on_wheel(self, event) -> str:
        """Scroll the widget under the pointer, handling X11 buttons and signed wheel deltas."""
        num = getattr(event, "num", None)
        if num == 4:
            step = -1
        elif num == 5:
            step = 1
        else:
            step = -1 if getattr(event, "delta", 0) > 0 else 1
        event.widget.yview_scroll(step * WHEEL_LINES, "units")
        return "break"

    def node_colours(self) -> None:
        """The tags the tree and the comment box use (also on a scheme switch)."""
        self.tree.tag_configure("group", foreground=ACCENT)
        self.tree.tag_configure("dim", foreground=FG_FAINT)
        self.comment.tag_configure("dim", foreground=FG_FAINT)

    def apply_theme(self) -> None:
        """Apply the current theme to the window, text, and tags."""
        self.configure(bg=BG_PANEL)
        self.comment.configure(bg=BG_INPUT, fg=FG, insertbackground=FG,
                               selectbackground=ACCENT_DIM, selectforeground=ON_ACCENT,
                               inactiveselectbackground=ACCENT_DIM)
        self.node_colours()

    def close(self) -> None:
        """Close without stopping playback and return keyboard focus to the player."""
        app = self.app
        held = self.focus_get()
        self.destroy()
        if getattr(app, "_song_info", None) is self:
            app._song_info = None
        if held is not None and str(held).startswith(str(self)):
            app.root.focus_set()
            if app.root.focus_get() is None:
                app.root.focus_force()

    def _on_escape(self, _event=None) -> str:
        self.close()
        return "break"
