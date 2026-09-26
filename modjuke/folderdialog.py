"""Themed folder browser and system folder-picker fallbacks."""

from __future__ import annotations

import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, ttk
from typing import Iterable, Optional

from .library import natural_key, supported_extensions

# Linux pickers that talk to the desktop's own (themed) folder chooser
LINUX_PICKERS = ("zenity", "kdialog", "qarma", "matedialog", "mate-dialog")

MAX_LISTED_DIRS = 4000


def expand_path(text: str) -> tuple[str, str]:
    """Return (absolute folder path, error), accepting quotes, ~, environment variables, and file
    paths."""
    raw = (text or "").strip().strip('"').strip("'")
    if not raw:
        return "", "empty path"
    try:
        path = os.path.expandvars(os.path.expanduser(raw))
    except Exception:
        path = raw
    path = os.path.abspath(path)
    if os.path.isfile(path):
        path = os.path.dirname(path)
    if not os.path.exists(path):
        return path, "folder does not exist"
    if not os.path.isdir(path):
        return path, "not a folder"
    if not os.access(path, os.R_OK | os.X_OK):
        return path, "folder is not readable"
    return path, ""


def list_subdirectories(path: str, show_hidden: bool = False) -> list[tuple[str, str]]:
    """List immediate subfolders as (name, path), sorted naturally."""
    out: list[tuple[str, str]] = []
    try:
        with os.scandir(path) as it:
            for entry in it:
                name = entry.name
                if not show_hidden and name.startswith("."):
                    continue
                try:
                    if entry.is_dir(follow_symlinks=True):
                        out.append((name, entry.path))
                except OSError:
                    continue
                if len(out) >= MAX_LISTED_DIRS:
                    break
    except OSError:
        return out
    out.sort(key=lambda item: natural_key(item[0]))
    return out


def count_module_files(path: str, extensions: Optional[Iterable[str]] = None) -> int:
    """Playable modules directly inside path (non-recursive)."""
    exts = {e.lower() for e in (extensions or supported_extensions())}
    count = 0
    try:
        with os.scandir(path) as it:
            for entry in it:
                try:
                    if entry.is_file():
                        ext = os.path.splitext(entry.name)[1].lstrip(".").lower()
                        if ext in exts:
                            count += 1
                except OSError:
                    continue
    except OSError:
        return 0
    return count


def count_modules_recursive(path: str, extensions: Optional[Iterable[str]] = None,
                            cancel: Optional[threading.Event] = None,
                            deadline: float = 1.5, max_files: int = 40000,
                            ) -> tuple[int, int, bool]:
    """Return (modules, folders, truncated), with cancellation and best-effort limits."""
    exts = {e.lower() for e in (extensions or supported_extensions())}
    started = time.monotonic()
    modules = folders = files = 0
    stack = [path]
    truncated = False
    while stack:
        if cancel is not None and cancel.is_set():
            return modules, folders, True
        if time.monotonic() - started > deadline or files >= max_files:
            truncated = True
            break
        current = stack.pop()
        folders += 1
        try:
            with os.scandir(current) as it:
                for entry in it:
                    if cancel is not None and cancel.is_set():
                        return modules, folders, True
                    name = entry.name
                    if name.startswith("."):
                        continue
                    try:
                        if entry.is_dir(follow_symlinks=False):
                            stack.append(entry.path)
                        elif entry.is_file():
                            files += 1
                            if os.path.splitext(name)[1].lstrip(".").lower() in exts:
                                modules += 1
                    except OSError:
                        continue
        except OSError:
            continue
    return modules, folders, truncated


def discover_places(extra: Iterable[tuple[str, str]] = ()) -> list[tuple[str, str]]:
    """Sidebar bookmarks: home, standard folders, drives and mounts."""
    places: list[tuple[str, str]] = []

    def add(label: str, path: str) -> None:
        if not path or not os.path.isdir(path):
            return
        absolute = os.path.abspath(path)
        if any(absolute == existing for _label, existing in places):
            return
        places.append((label, absolute))

    home = os.path.expanduser("~")
    add("Home", home)
    for label, env, fallback in (
        ("Desktop", "XDG_DESKTOP_DIR", "Desktop"),
        ("Documents", "XDG_DOCUMENTS_DIR", "Documents"),
        ("Downloads", "XDG_DOWNLOAD_DIR", "Downloads"),
        ("Music", "XDG_MUSIC_DIR", "Music"),
    ):
        add(label, os.environ.get(env) or os.path.join(home, fallback))
    for label, path in extra:                       # e.g. the current library
        add(label, path)

    if sys.platform == "win32":
        import string

        for letter in string.ascii_uppercase:
            add(f"{letter}:", f"{letter}:\\")
    elif sys.platform == "darwin":
        if os.path.isdir("/Volumes"):
            for name in sorted(os.listdir("/Volumes")):
                add(name, os.path.join("/Volumes", name))
    else:
        for base in ("/", "/home", "/media", "/mnt", "/opt", "/srv"):
            add(base, base)
        for base in ("/media", "/mnt", "/run/media"):
            if not os.path.isdir(base):
                continue
            try:
                names = sorted(os.listdir(base))
            except OSError:
                continue
            for name in names:
                sub = os.path.join(base, name)
                if not os.path.isdir(sub):
                    continue
                add(sub, sub)
                try:
                    inner_names = sorted(os.listdir(sub))
                except OSError:
                    continue
                for inner in inner_names:
                    inner_path = os.path.join(sub, inner)
                    if os.path.isdir(inner_path) and not inner.startswith("."):
                        add(inner_path, inner_path)
    return places


def system_picker_available() -> str:
    """Return the system picker name, or an empty string if unavailable."""
    if sys.platform in ("win32", "darwin"):
        return "native"
    for name in LINUX_PICKERS:
        if shutil.which(name):
            return name
    return ""


def pick_directory_system(parent: Optional[tk.Misc], initialdir: str = "",
                          title: str = "Open folder",
                          timeout: float = 600.0) -> Optional[str]:
    """Return the chosen folder, an empty string on cancel, or None if the system picker is
    unavailable or fails."""
    picker = system_picker_available()
    if not picker:
        return None
    if picker == "native":                          # Windows / macOS: the OS panel
        chosen = filedialog.askdirectory(parent=parent, title=title,
                                         initialdir=initialdir or os.path.expanduser("~"),
                                         mustexist=True)
        return chosen or ""
    start = initialdir or os.path.expanduser("~")
    if picker == "zenity":
        cmd = [shutil.which("zenity"), "--file-selection", "--directory",
               "--title", title, "--filename", start.rstrip(os.sep) + os.sep]
    else:                                           # kdialog / qarma / matedialog
        cmd = [shutil.which(picker), "--getexistingdirectory", start, "--title", title]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:                        # user cancelled
        return ""
    lines = [line.strip() for line in (proc.stdout or "").splitlines() if line.strip()]
    return lines[-1] if lines else ""


def pick_directory(parent: Optional[tk.Misc], initialdir: str = "",
                   title: str = "Open folder", prefer: str = "built-in",
                   extra_places: Iterable[tuple[str, str]] = (),
                   dialog_class=None) -> str:
    """Choose a folder using the preferred picker, with built-in fallback. Return an empty string
    on cancel."""
    if prefer == "system":
        chosen = pick_directory_system(parent, initialdir, title)
        if chosen is not None:
            return chosen
    dialog_cls = dialog_class or FolderDialog
    return dialog_cls(parent, start_dir=initialdir, title=title,
                      extra_places=extra_places).show()


def show_path_tail(entry: tk.Misc, tail_only_unfocused: bool = True):
    """Show the end of a long path when unfocused and its start when editing. Preserve the full
    text."""

    def to_tail(_event=None) -> None:
        try:
            if tail_only_unfocused and entry.winfo_toplevel().focus_get() is entry:
                return
            entry.xview_moveto(1.0)
        except tk.TclError:
            pass

    def to_head(_event=None) -> None:
        try:
            entry.xview_moveto(0.0)
        except tk.TclError:
            pass

    entry.bind("<FocusIn>", to_head, add="+")
    entry.bind("<FocusOut>", to_tail, add="+")
    entry.bind("<Configure>", lambda _e: entry.after_idle(to_tail), add="+")
    entry.after_idle(to_tail)
    return to_tail


def enable_select_all(widget: tk.Misc) -> None:
    """Bind Ctrl+A and the platform SelectAll event for entry, spinbox, and text widgets."""

    def select_all(event):
        target = event.widget
        try:
            if target.winfo_class() in ("Text", "tkText"):
                target.tag_add("sel", "1.0", "end-1c")
                target.mark_set("insert", "1.0")
            else:
                target.selection_range(0, "end")
                target.icursor("end")
                target.xview_moveto(0.0)
        except tk.TclError:
            pass
        return "break"

    for cls in ("Entry", "TEntry", "Spinbox", "TSpinbox", "Text", "TCombobox"):
        for sequence in ("<Control-a>", "<Control-A>", "<Control-slash>", "<<SelectAll>>"):
            widget.bind_class(cls, sequence, select_all)

CRUMB_CHAR_WIDTH = 8      # rough px per character in the crumb font
CRUMB_PADDING = 20        # button padding + separator
ELLIPSIS_LABEL = "\u2026"


def crumb_targets(path: str) -> list[tuple[str, str]]:
    """[(label, full path), ...] for every component of path, root first."""
    if not os.path.isabs(path):
        return [(path, path)]
    parts = [part for part in path.split(os.sep) if part]
    root_label = os.sep if os.sep == "/" else path[:3]
    targets = [(root_label, root_label)]
    accumulated = root_label
    for part in parts:
        accumulated = os.path.join(accumulated, part)
        targets.append((part, accumulated))
    return targets


def fit_crumbs(targets: list[tuple[str, str]], available: int,
               measure=None, sep_width: int = 9) -> list[tuple[str, str]]:
    """Fit breadcrumb buttons, keeping the root and trailing folders. The ellipsis links to the
    deepest hidden folder."""
    if len(targets) <= 2:
        return list(targets)
    if measure is None:
        measure = lambda label: len(label) * CRUMB_CHAR_WIDTH + CRUMB_PADDING   # noqa: E731

    def row_width(shown):
        return sum(measure(label) for label, _path in shown) + sep_width * (len(shown) - 1)

    hidden = 0
    shown = list(targets)
    while hidden < len(targets) - 2:
        candidate = [targets[0]] + ([(ELLIPSIS_LABEL, targets[hidden][1])] if hidden else []) \
            + targets[1 + hidden:]
        if row_width(candidate) <= available:
            shown = candidate
            break
        hidden += 1
        shown = candidate
    return shown


class FolderDialog(tk.Toplevel):
    """Themed folder browser with keyboard navigation and cancellable background counts."""

    listing_label = "Folders here"
    hidden_label = "Show hidden folders"
    selection_hint = "the chosen folder is scanned recursively"
    accept_label = "Use this folder"
    selection_mode = "browse"

    def __init__(self, parent: Optional[tk.Misc], start_dir: str = "",
                 title: str = "Open folder",
                 extra_places: Iterable[tuple[str, str]] = ()):
        from .ui import ACCENT, BG, FG     # lazy: avoids a circular import

        super().__init__(parent)
        self.withdraw()
        self.title(title)
        self.configure(bg=BG)
        self.result: Optional[str] = None
        self._bg, self._fg, self._accent = BG, FG, ACCENT
        self._start_dir = start_dir or os.path.expanduser("~")
        self._cwd = ""
        self._places = discover_places(extra_places)
        self._place_of: dict[str, str] = {}
        self._count_queue: queue.Queue = queue.Queue()
        self._count_token = 0
        self._count_cancel: Optional[threading.Event] = None
        self._count_thread: Optional[threading.Thread] = None
        self._show_hidden = tk.BooleanVar(value=False)
        self._status = tk.StringVar(value="")

        self._build()
        self.navigate(self._start_dir, initial=True)

        self.transient(parent)
        self.protocol("WM_DELETE_WINDOW", self.cancel)
        self.bind("<Escape>", lambda _e: self.cancel())
        self.bind("<Alt-Up>", lambda _e: self.go_up())
        self.bind("<Control-l>", lambda _e: self._focus_path())
        self.bind("<Control-h>", lambda _e: self._toggle_hidden())
        self.bind("<F5>", lambda _e: self.refresh())
        self._crumb_fit = 0            # the width the row drawn is fitted for
        self._crumb_timer = None
        self.bind("<Configure>", self._on_resize, add="+")
        self.after(30, self._place_window)

    def _build(self) -> None:
        outer = ttk.Frame(self, padding=(12, 10, 12, 10))
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(2, weight=1)

        top = ttk.Frame(outer)
        top.grid(row=0, column=0, sticky="ew")
        top.columnconfigure(1, weight=1)
        ttk.Label(top, text="Folder").grid(row=0, column=0, padx=(0, 8))
        self.path_var = tk.StringVar()
        self.path_entry = ttk.Entry(top, textvariable=self.path_var, style="Dlg.TEntry")
        self.path_entry.grid(row=0, column=1, sticky="ew")
        self.path_entry.bind("<Return>", lambda _e: self.go_to_typed())
        self.path_entry.bind("<FocusIn>", self._select_path_text)
        show_path_tail(self.path_entry)
        ttk.Button(top, text="Go", style="Accent.TButton",
                   command=self.go_to_typed).grid(row=0, column=2, padx=(6, 0))
        ttk.Button(top, text="Up", command=self.go_up).grid(row=0, column=3, padx=(6, 0))

        self.crumbs = ttk.Frame(outer)
        self.crumbs.grid(row=1, column=0, sticky="ew", pady=(8, 6))

        middle = ttk.Frame(outer)
        middle.grid(row=2, column=0, sticky="nsew")
        middle.columnconfigure(1, weight=1)
        middle.rowconfigure(0, weight=1)

        places_box = ttk.Frame(middle)
        places_box.grid(row=0, column=0, sticky="nsw", padx=(0, 10))
        places_box.rowconfigure(1, weight=1)
        ttk.Label(places_box, text="Places", style="DlgDim.TLabel").grid(row=0, column=0,
                                                                         sticky="w")
        self.places_tree = ttk.Treeview(places_box, show="tree", height=14,
                                        selectmode="browse", style="Dlg.Treeview")
        self.places_tree.grid(row=1, column=0, sticky="ns")
        self.places_tree.column("#0", width=190, minwidth=120)
        self.places_tree.bind("<<TreeviewSelect>>", self._on_place_selected)
        for label, path in self._places:
            item = self.places_tree.insert("", "end", text="  " + label, tags=("place",))
            self._place_of[item] = path
        self.places_tree.tag_configure("place", foreground=self._fg)
        places_scroll = ttk.Scrollbar(places_box, orient="vertical",
                                      command=self.places_tree.yview)
        self.places_tree.configure(yscrollcommand=places_scroll.set)
        places_scroll.grid(row=1, column=1, sticky="ns")

        dirs_box = ttk.Frame(middle)
        dirs_box.grid(row=0, column=1, sticky="nsew")
        dirs_box.rowconfigure(1, weight=1)
        dirs_box.columnconfigure(0, weight=1)
        ttk.Label(dirs_box, text=self.listing_label, style="DlgDim.TLabel").grid(row=0, column=0,
                                                                             sticky="w")
        self.dirs_tree = ttk.Treeview(dirs_box, show="tree", selectmode=self.selection_mode,
                                      style="Dlg.Treeview")
        self.dirs_tree.grid(row=1, column=0, sticky="nsew")
        self.dirs_tree.column("#0", width=420, minwidth=200)
        self.dirs_tree.tag_configure("up", foreground=self._accent)
        self.dirs_tree.bind("<Double-1>", lambda _e: self.activate_selected())
        self.dirs_tree.bind("<Return>", lambda _e: self.activate_selected())
        self.dirs_tree.bind("<<TreeviewSelect>>", self._on_dir_selected)
        dirs_scroll = ttk.Scrollbar(dirs_box, orient="vertical", command=self.dirs_tree.yview)
        self.dirs_tree.configure(yscrollcommand=dirs_scroll.set)
        dirs_scroll.grid(row=1, column=1, sticky="ns")

        self._build_selection(outer)

        footer = ttk.Frame(outer)
        footer.grid(row=4, column=0, sticky="ew", pady=(10, 0))
        footer.columnconfigure(0, weight=1)
        self.preview = ttk.Label(footer, textvariable=self._status, style="DlgDim.TLabel",
                                 background=self._bg)
        self.preview.grid(row=0, column=0, sticky="w")
        if system_picker_available():
            ttk.Button(footer, text="System dialog…",
                       command=self.use_system_dialog).grid(row=0, column=1, padx=(8, 0))
        ttk.Button(footer, text="Cancel", command=self.cancel).grid(row=0, column=2, padx=(8, 0))
        self.ok_button = ttk.Button(footer, text=self.accept_label, style="Accent.TButton",
                                    command=self.accept)
        self.ok_button.grid(row=0, column=3, padx=(8, 0))

        checks = ttk.Frame(outer)
        checks.grid(row=5, column=0, sticky="ew", pady=(6, 0))
        ttk.Checkbutton(checks, text=self.hidden_label, variable=self._show_hidden,
                        style="Dlg.TCheckbutton", command=self.refresh).pack(side="left")
        ttk.Label(checks, text=self.selection_hint,
                  style="DlgDim.TLabel", background=self._bg).pack(side="right")

        self.geometry("880x560")
        self.minsize(640, 400)
        self.after(150, self._focus_path)

    def _build_selection(self, outer) -> None:
        """File browsers add their filename and filter controls here."""

    def _crumb_space(self) -> int:
        """Measure space from the dialog width, not the breadcrumb content, to avoid resize loops."""
        width = self.winfo_width()
        if width <= 1:                                 # not mapped yet
            width = max(self.winfo_reqwidth(), 640)
        return max(width - 40, 260)

    def _on_resize(self, event) -> None:
        """Schedule a breadcrumb refit for dialog-width changes, ignore child resize events."""
        if event.widget is not self:
            return
        if abs(self._crumb_space() - self._crumb_fit) > 40 and not self._crumb_timer:
            self._crumb_timer = self.after(150, self._apply_crumb_width)

    def _apply_crumb_width(self) -> None:
        self._crumb_timer = None
        try:
            if not self.winfo_exists():
                return
            if self._crumb_space() == self._crumb_fit:
                return                    # the row is already fitted for this width
            self._rebuild_crumbs()
        except tk.TclError:                            # closed while waiting
            pass

    def _place_window(self) -> None:
        self.deiconify()
        parent = self.master
        try:
            self.update_idletasks()
            if isinstance(parent, (tk.Tk, tk.Toplevel)) and parent.winfo_viewable():
                x = parent.winfo_rootx() + (parent.winfo_width() - self.winfo_width()) // 2
                y = parent.winfo_rooty() + (parent.winfo_height() - self.winfo_height()) // 3
                self.geometry(f"+{max(0, x)}+{max(0, y)}")
        except tk.TclError:
            pass
        # now that the dialog has its real width, fit the breadcrumbs exactly
        self._apply_crumb_width()

    def _focus_path(self) -> None:
        try:
            self.path_entry.focus_set()
            self.path_entry.selection_range(0, "end")
        except tk.TclError:
            pass

    def _select_path_text(self, _event=None) -> None:
        try:
            self.path_entry.selection_range(0, "end")
        except tk.TclError:
            pass

    def _toggle_hidden(self) -> None:
        self._show_hidden.set(not self._show_hidden.get())
        self.refresh()

    def navigate(self, path: str, initial: bool = False) -> bool:
        """Show path, keeps the current folder and reports the problem if it
        is not a readable folder."""
        resolved, error = expand_path(path)
        if error:
            self._flash_error(f"{resolved or path}: {error}")
            return False
        self._cwd = resolved
        self.path_var.set(resolved)
        self._rebuild_crumbs()
        self.refresh()
        if not initial:
            try:
                self.path_entry.icursor("end")
            except tk.TclError:
                pass
        return True

    def go_to_typed(self) -> None:
        text = self.path_var.get()
        resolved, error = expand_path(text)
        if error:
            self.bell()
            self._flash_error(f"{resolved or text}: {error}")
            return
        self.navigate(resolved)

    def go_up(self) -> None:
        parent = os.path.dirname(self._cwd.rstrip(os.sep)) or os.sep
        if parent and parent != self._cwd:
            self.navigate(parent)

    def _measure_crumb(self, label: str) -> int:
        """Pixel width of the button a crumb label ends up in."""
        probe = ttk.Button(self.crumbs, text=label, style="Crumb.TButton")
        try:
            return probe.winfo_reqwidth()
        finally:
            probe.destroy()

    def _measure_separator(self) -> int:
        probe = ttk.Label(self.crumbs, text=os.sep, style="DlgDim.TLabel")
        try:
            return probe.winfo_reqwidth()
        finally:
            probe.destroy()

    def _render_crumbs(self, targets: list[tuple[str, str]]) -> None:
        for child in self.crumbs.winfo_children():
            child.destroy()
        for index, (label, path) in enumerate(targets):
            if index:
                ttk.Label(self.crumbs, text=os.sep, style="DlgDim.TLabel").pack(side="left")
            style = "CrumbHere.TButton" if index == len(targets) - 1 else "Crumb.TButton"
            ttk.Button(self.crumbs, text=label, style=style,
                       command=lambda p=path: self.navigate(p)).pack(side="left")

    def _rebuild_crumbs(self) -> None:
        """Draw measured breadcrumbs, replacing leading folders with an ellipsis as needed."""
        targets = crumb_targets(self._cwd)
        available = self._crumb_space()
        self._crumb_fit = available            # what this row is fitted for
        if len(targets) <= 2:
            self._render_crumbs(targets)
            return

        shown = fit_crumbs(targets, available, measure=self._measure_crumb,
                           sep_width=self._measure_separator())
        for _attempt in range(4):
            self._render_crumbs(shown)
            row = list(self.crumbs.winfo_children())
            total = sum(child.winfo_reqwidth() for child in row)
            if total <= available or len(shown) <= 2:
                break
            # the estimate was optimistic (long labels cost more): drop one more
            shown = fit_crumbs(targets, available - (total - available) - 4,
                               measure=self._measure_crumb,
                               sep_width=self._measure_separator())

    def refresh(self) -> None:
        pairs = list_subdirectories(self._cwd, self._show_hidden.get())
        self.dirs_tree.delete(*self.dirs_tree.get_children())
        parent = os.path.dirname(self._cwd.rstrip(os.sep)) or os.sep
        if parent and parent != self._cwd:
            self.dirs_tree.insert("", "end", iid="..", text="  ..", tags=("up",))
        for name, path in pairs:
            self.dirs_tree.insert("", "end", iid=path, text="  " + name)
        children = self.dirs_tree.get_children()
        if children:
            self.dirs_tree.selection_set(children[0])
            self.dirs_tree.focus(children[0])
        if len(pairs) >= MAX_LISTED_DIRS:
            self._status.set(f"Showing the first {MAX_LISTED_DIRS} folders …")
        self._queue_preview(self._cwd)

    def select_path(self, path: str) -> bool:
        """Highlight a sub-folder of the current folder (used by tests/scripts)."""
        if self.dirs_tree.exists(path):
            self.dirs_tree.selection_set(path)
            self.dirs_tree.focus(path)
            self.dirs_tree.see(path)
            return True
        return False

    def _on_place_selected(self, _event=None) -> None:
        selection = self.places_tree.selection()
        if not selection:
            return
        path = self._place_of.get(selection[0])
        if path and os.path.abspath(path) != os.path.abspath(self._cwd):
            self.navigate(path)

    def _on_dir_selected(self, _event=None) -> None:
        selection = self.dirs_tree.selection()
        if not selection or selection[0] == "..":
            return
        self._queue_preview(selection[0])

    def activate_selected(self) -> None:
        selection = self.dirs_tree.selection()
        if not selection:
            return
        item = selection[0]
        if item == "..":
            self.go_up()
        else:
            self.navigate(item)

    def _queue_preview(self, path: str) -> None:
        """Count modules in a background worker, cancelling the previous request."""
        self._count_token += 1
        token = self._count_token
        self._status.set(f"Counting modules in {os.path.basename(path) or path} …")
        direct = count_module_files(self._cwd)
        same = os.path.abspath(path) == os.path.abspath(self._cwd)

        if self._count_cancel is not None:
            self._count_cancel.set()
        cancel = threading.Event()
        self._count_cancel = cancel

        def work():
            try:
                modules, folders, truncated = count_modules_recursive(path, cancel=cancel)
            except Exception:
                modules, folders, truncated = 0, 0, True
            self._count_queue.put((token, path, modules, folders, truncated, direct, same))

        if self._count_thread is not None and self._count_thread.is_alive():
            self._count_thread.join(timeout=0.2)
        self._count_thread = threading.Thread(target=work, daemon=True, name="folder-count")
        self._count_thread.start()
        self.after(100, self._poll_count)

    def _poll_count(self) -> None:
        try:
            token, path, modules, folders, truncated, direct, same = \
                self._count_queue.get_nowait()
        except queue.Empty:
            if self.winfo_exists():
                self.after(100, self._poll_count)
            return
        if token != self._count_token or not self.winfo_exists():
            return
        label = os.path.basename(path) or path
        more = "+" if truncated else ""
        if modules:
            where = "this folder" if same else f"{label}"
            text = (f"{more}{modules} playable module{'s' if modules != 1 else ''} in {where}"
                    f", {folders} folder{'s' if folders != 1 else ''} scanned")
        else:
            text = f"No playable modules in {label}"
        if not same:
            text += f", {direct} directly in the current folder"
        if truncated:
            text += " (counting stopped early - the folder is huge)"
        self._status.set(text)

    def _flash_error(self, message: str) -> None:
        self._status.set("! " + message)
        try:
            self.path_entry.configure(style="DlgError.TEntry")
        except tk.TclError:
            return
        self.after(1600, lambda: self._reset_entry_style())

    def _reset_entry_style(self) -> None:
        try:
            self.path_entry.configure(style="Dlg.TEntry")
        except tk.TclError:
            pass

    def chosen_path(self) -> str:
        """The folder that "Use this folder" would open."""
        selection = self.dirs_tree.selection() if self.dirs_tree.get_children() else ()
        if selection and selection[0] != ".." and os.path.isdir(selection[0]):
            return selection[0]
        return self._cwd

    def accept(self) -> None:
        resolved, error = expand_path(self.chosen_path())
        if error:
            self.bell()
            self._flash_error(f"{self.chosen_path()}: {error}")
            return
        self.result = resolved
        self.destroy()

    def use_system_dialog(self) -> None:
        """Hand over to the desktop picker (only shown when one exists)."""
        chosen = pick_directory_system(self, self._cwd, self.title())
        if chosen is None:
            self._status.set("No system dialog available on this system")
            return
        self.result = chosen or ""
        self.destroy()

    def cancel(self) -> None:
        self.result = None
        self.destroy()

    def show(self) -> str:
        """Modal: run the dialog and return the chosen path ("" = cancelled)."""
        self.grab_set()
        self.wait_window(self)
        return self.result or ""
