"""Themed manager for immediate, reversible song exclusions."""

from __future__ import annotations

import os
import tkinter as tk
from tkinter import ttk

from . import theme
from .assets import set_window_icon
from .filedialog import pick_files
from .folderdialog import enable_select_all


class IgnoreDialog(tk.Toplevel):
    def __init__(self, app, parent):
        super().__init__(parent)
        self.app = app
        self.title("Ignored songs")
        self.configure(bg=theme.BG)
        self.transient(parent)
        self.geometry("780x440")
        self.minsize(540, 300)
        self._icon = set_window_icon(self)
        enable_select_all(self)
        body = ttk.Frame(self, padding=14)
        body.pack(fill="both", expand=True)
        ttk.Label(body, text="Ignored songs", style="Head.TLabel").pack(anchor="w")
        ttk.Label(body, text="Changes apply and save immediately, independently of Settings Save/Cancel.\n"
                  "Songs are hidden everywhere, music files and saved playlist memberships are kept.",
                  style="Dim.TLabel").pack(anchor="w", pady=(4, 10))
        frame = ttk.Frame(body)
        frame.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(frame, columns=("path",), selectmode="extended")
        self.tree.heading("#0", text="Song")
        self.tree.heading("path", text="File path")
        self.tree.column("#0", width=210, minwidth=120)
        self.tree.column("path", width=470, minwidth=180)
        vertical = ttk.Scrollbar(frame, orient="vertical", command=self.tree.yview)
        horizontal = ttk.Scrollbar(frame, orient="horizontal", command=self.tree.xview)
        self.tree.configure(yscrollcommand=vertical.set, xscrollcommand=horizontal.set)
        self.tree.grid(row=0, column=0, sticky="nsew")
        vertical.grid(row=0, column=1, sticky="ns")
        horizontal.grid(row=1, column=0, sticky="ew")
        frame.rowconfigure(0, weight=1)
        frame.columnconfigure(0, weight=1)
        self.note = ttk.Label(body, style="Dim.TLabel", wraplength=720)
        self.note.pack(anchor="w", pady=8)
        buttons = ttk.Frame(body)
        buttons.pack(fill="x")
        self.add_btn = ttk.Button(buttons, text="Add files…", command=self.add_files)
        self.add_btn.pack(side="left")
        self.restore_btn = ttk.Button(buttons, text="Restore selected", command=self.restore_selected)
        self.restore_btn.pack(side="left", padx=6)
        self.close_btn = ttk.Button(buttons, text="Close", command=self.destroy)
        self.close_btn.pack(side="right")
        self._row_paths = {}
        self.tree.bind("<<TreeviewSelect>>", lambda _e: self._selection())
        self.bind("<Escape>", self._escape)
        self.protocol("WM_DELETE_WINDOW", self.destroy)
        self.refresh()
        self.tree.focus_set()

    def refresh(self):
        selected = {self._row_paths[r] for r in self.tree.selection() if r in self._row_paths}
        self.tree.delete(*self.tree.get_children())
        self._row_paths.clear()
        for path in self.app._ignored.paths:
            row = self.tree.insert("", "end", text=os.path.basename(path), values=(path,))
            self._row_paths[row] = path
            if path in selected:
                self.tree.selection_add(row)
        count = len(self._row_paths)
        self.note.configure(text=self.app._ignored.error or
                            f"{count} ignored song{'s' if count != 1 else ''}. "
                            "Restoring never starts playback. Copies at other paths are not ignored.")
        self._selection()

    def _selection(self):
        self.restore_btn.state(["!disabled"] if self.tree.selection() else ["disabled"])

    def add_files(self):
        paths = pick_files(parent=self, title="Ignore songs", multiple=True,
                           initialdir=self.app._directory or self.app.settings.last_picker_dir,
                           prefer=self.app.settings.folder_picker,
                           extra_places=self.app._extra_places(),
                           filetypes=[("All files", "*")])
        if paths:
            self.app.change_ignored(add=paths, parent=self)

    def restore_selected(self):
        paths = [self._row_paths[r] for r in self.tree.selection() if r in self._row_paths]
        if paths:
            self.app.change_ignored(remove=paths, parent=self)

    def apply_theme(self):
        self.configure(bg=theme.BG)

    def _escape(self, _event=None):
        self.destroy()
        return "break"

    def destroy(self):
        if self.app._ignore_dialog is self:
            self.app._ignore_dialog = None
        super().destroy()
        self._icon = None
        self.add_btn = self.restore_btn = self.close_btn = None
        self.tree = None
        self.note = None
        self._row_paths.clear()
