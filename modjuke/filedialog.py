"""File selection using the shared themed browser or the desktop picker."""

from __future__ import annotations

import fnmatch
import os
import shutil
import subprocess
import tkinter as tk
from tkinter import filedialog as native, ttk

from .folderdialog import FolderDialog, MAX_LISTED_DIRS, system_picker_available
from .library import natural_key


def pick_files_system(parent=None, *, initialdir="", title="Open files", multiple=False,
                      save=False, initialfile="", defaultextension="", filetypes=(),
                      timeout=600):
    """None means unavailable/failed, an empty result means cancellation."""
    multiple = multiple and not save
    picker = system_picker_available()
    if not picker:
        return None
    start = initialdir or os.path.expanduser("~")
    if picker == "native":
        options = dict(parent=parent, initialdir=start, title=title, filetypes=filetypes)
        try:
            if save:
                return native.asksaveasfilename(initialfile=initialfile,
                                               defaultextension=defaultextension, **options) or ""
            if multiple:
                result = native.askopenfilenames(**options)
                if isinstance(result, str):
                    return tuple(parent.tk.splitlist(result)) if parent else tuple(tk.Tcl().splitlist(result))
                return tuple(result)
            return native.askopenfilename(**options) or ""
        except (tk.TclError, OSError):
            return None
    filename = os.path.join(start, initialfile) if save else start.rstrip(os.sep) + os.sep
    if picker == "kdialog":
        filters = "\n".join(f"{patterns}|{label}" for label, patterns in filetypes)
        cmd = [shutil.which(picker), "--getsavefilename" if save else "--getopenfilename",
               filename, filters, "--title", title]
        if multiple:
            cmd += ["--multiple", "--separate-output"]
        separator = "\n"
    else:  # zenity and its compatible desktop variants
        cmd = [shutil.which(picker), "--file-selection", "--title", title, "--filename", filename]
        if save:
            cmd += ["--save", "--confirm-overwrite"]
        separator = "\x1f"
        if multiple:
            cmd += ["--multiple", "--separator=" + separator]
        for label, patterns in filetypes:
            cmd += ["--file-filter", f"{label} | {patterns}"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode == 1:
        return () if multiple else ""
    if proc.returncode != 0:
        return None
    value = proc.stdout.removesuffix("\n")
    if not value:
        return () if multiple else ""
    if save:
        original = os.path.abspath(value)
        path = original
        if defaultextension and not os.path.splitext(os.path.basename(path))[1]:
            path += defaultextension
        if os.path.isdir(path):
            return None
        # The extension may differ from the name the desktop chooser confirmed.
        if path != original and os.path.exists(path):
            confirmed = confirm_replace_system(picker, parent, path, timeout)
            if confirmed is None:
                return None
            if not confirmed:
                return ""
        return path
    paths = tuple(value.split(separator)) if multiple else (value,)
    if not all(os.path.isfile(p) for p in paths):
        return None
    return paths if multiple else paths[0]


def confirm_replace_system(picker, parent, path, timeout):
    """Ask before replacing the final path, including an appended extension."""
    text = f"Replace the existing file?\n{path}"
    if picker == "kdialog":
        cmd = [shutil.which(picker), "--warningyesno", text, "--title", "Replace file"]
    else:
        cmd = [shutil.which(picker), "--question", "--title", "Replace file", "--text", text,
               "--ok-label", "Replace", "--cancel-label", "Cancel", "--default-cancel", "--no-markup"]
    try:
        code = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).returncode
        return code == 0 if code in (0, 1) else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def pick_files(parent=None, *, initialdir="", title="Open files", multiple=False,
               save=False, initialfile="", defaultextension="", filetypes=(),
               prefer="built-in", extra_places=(), dialog_class=None):
    """Use the same built-in/system preference as the folder browser."""
    options = dict(initialdir=initialdir, title=title, multiple=multiple, save=save,
                   initialfile=initialfile, defaultextension=defaultextension, filetypes=filetypes)
    if prefer == "system":
        result = pick_files_system(parent, **options)
        if result is not None:
            return result
    cls = dialog_class or FileDialog
    return cls(parent, extra_places=extra_places, **options).show()


class FileDialog(FolderDialog):
    """Folder browser layout, navigation and colours, with file selection controls."""

    listing_label = "Folders and files"
    hidden_label = "Show hidden files and folders"

    def __init__(self, parent=None, *, initialdir="", title="Open files", multiple=False,
                 save=False, initialfile="", defaultextension="", filetypes=(), extra_places=()):
        self.multiple = multiple and not save
        self.save = save
        self.initialfile = initialfile
        self.defaultextension = defaultextension
        self.filetypes = tuple(filetypes) or (("All files", "*"),)
        self.selection_mode = "extended" if self.multiple else "browse"
        self.accept_label = "Save" if save else "Add files" if self.multiple else "Open"
        self.selection_hint = "Ctrl/Shift to select several files" if self.multiple else "Double-click a folder to open it"
        self._pending_replace = ""
        self._selected_files = ()
        super().__init__(parent, start_dir=initialdir, title=title, extra_places=extra_places)
        self.preview.configure(wraplength=390)
        if not self._cwd:
            self.navigate(os.path.expanduser("~"), initial=True)
        self.bind("<Control-a>", self._select_all_files)
        self.bind("<Control-A>", self._select_all_files)

    def _build_selection(self, outer):
        row = ttk.Frame(outer)
        row.grid(row=3, column=0, sticky="ew", pady=(8, 0))
        row.columnconfigure(1, weight=1)
        ttk.Label(row, text="File name").grid(row=0, column=0, padx=(0, 8))
        self.name_var = tk.StringVar(self, value=self.initialfile)
        self.name_entry = ttk.Entry(row, textvariable=self.name_var, style="Dlg.TEntry")
        self.name_entry.grid(row=0, column=1, sticky="ew")
        self.name_entry.bind("<Return>", lambda _e: self.accept())
        self._name_trace = self.name_var.trace_add("write", self._name_changed)
        ttk.Label(row, text="File type").grid(row=1, column=0, padx=(0, 8), pady=(6, 0))
        self.filter_box = ttk.Combobox(row, state="readonly",
                                       values=tuple(label for label, _ in self.filetypes))
        self.filter_box.current(0)
        self.filter_box.grid(row=1, column=1, sticky="ew", pady=(6, 0))
        self.filter_box.bind("<<ComboboxSelected>>", lambda _e: self.refresh())

    def _name_changed(self, *_):
        self._selected_files = ()
        self._pending_replace = ""
        if hasattr(self, "ok_button"):
            self.ok_button.configure(text=self.accept_label)

    def navigate(self, path, initial=False):
        result = super().navigate(path, initial)
        if result:
            self._selected_files = ()
            self._pending_replace = ""
            if not self.save:
                self.name_var.set("")
            if hasattr(self, "ok_button"):
                self.ok_button.configure(text=self.accept_label)
        return result

    def refresh(self):
        if not self._cwd:
            return
        patterns = self.filetypes[max(0, self.filter_box.current())][1].split()
        entries = []
        try:
            with os.scandir(self._cwd) as listing:
                for entry in listing:
                    if not self._show_hidden.get() and entry.name.startswith("."):
                        continue
                    try:
                        is_dir = entry.is_dir()
                        if not is_dir and (not entry.is_file() or not any(
                                fnmatch.fnmatchcase(entry.name.casefold(), p.casefold()) for p in patterns)):
                            continue
                    except OSError:
                        continue
                    entries.append((not is_dir, entry.name, entry.path))
                    if len(entries) >= MAX_LISTED_DIRS:
                        break
        except OSError as exc:
            self._flash_error(str(exc))
            return
        self.dirs_tree.delete(*self.dirs_tree.get_children())
        self._selected_files = ()
        if not self.save:
            self.name_var.set("")
        parent = os.path.dirname(self._cwd)
        if parent != self._cwd:
            self.dirs_tree.insert("", "end", iid="..", text="  ..", tags=("up",))
        for is_file, name, path in sorted(entries, key=lambda e: (e[0], natural_key(e[1]))):
            self.dirs_tree.insert("", "end", iid=path, text="  " + name + ("" if is_file else os.sep),
                                  tags=("file" if is_file else "folder",))
        suffix = " (listing limit reached)" if len(entries) == MAX_LISTED_DIRS else ""
        self._status.set(f"{sum(e[0] for e in entries)} files · {sum(not e[0] for e in entries)} folders" + suffix)

    def _on_dir_selected(self, _event=None):
        paths = tuple(p for p in self.dirs_tree.selection() if p != ".." and os.path.isfile(p))
        if paths:
            names = tuple(os.path.basename(p) for p in paths)
            if self.multiple:
                self.name_var.set(names)
            else:
                self.name_var.set(names[0])
            self._selected_files = paths
        elif not self.save and (self.dirs_tree.selection() or self._selected_files):
            self.name_var.set("")

    def _select_all_files(self, event):
        if event.widget is self.dirs_tree and self.multiple:
            self.dirs_tree.selection_set([p for p in self.dirs_tree.get_children()
                                          if "file" in self.dirs_tree.item(p, "tags")])
            return "break"

    def activate_selected(self):
        selected = self.dirs_tree.selection()
        item = self.dirs_tree.focus()
        if not item or item not in selected:
            item = selected[0] if selected else ""
        if item == "..":
            self.go_up()
        elif item and os.path.isdir(item):
            self.navigate(item)
        elif item and os.path.isfile(item):
            self._on_dir_selected()
            self.accept()
        elif item:
            self._flash_error("The selected file no longer exists")

    def go_to_typed(self):
        path = self._absolute(self.path_var.get())
        if os.path.isfile(path):
            if self.navigate(os.path.dirname(path)):
                self.name_var.set(os.path.basename(path))
                self.name_entry.focus_set()
        else:
            super().go_to_typed()

    def _absolute(self, path):
        return os.path.abspath(os.path.join(self._cwd, os.path.expandvars(os.path.expanduser(path))))

    def accept(self):
        text = self.name_var.get()
        if self._selected_files:
            paths = self._selected_files
        elif not text:
            selected = self.dirs_tree.selection()
            if selected and (selected[0] == ".." or os.path.isdir(selected[0])):
                self.activate_selected()
            else:
                self._flash_error("Choose a file or enter its name")
            return
        else:
            whole = self._absolute(text)
            if os.path.isdir(whole):
                self.navigate(whole)
                return
            try:
                names = self.tk.splitlist(text) if self.multiple and not os.path.isfile(whole) else (text,)
            except tk.TclError:
                self._flash_error("Use braces or quotes around each file name containing spaces")
                return
            paths = tuple(self._absolute(name) for name in names)
        if not paths:
            self._flash_error("Choose a file")
            return
        if self.save:
            path = paths[0]
            if "\0" in path:
                self._flash_error("File names cannot contain a null character")
                return
            if self.defaultextension and not os.path.splitext(os.path.basename(path))[1]:
                path += self.defaultextension
            if os.path.isdir(path) or not os.path.isdir(os.path.dirname(path)):
                self._flash_error("Choose a file in an existing folder")
                return
            if not os.access(os.path.dirname(path), os.W_OK | os.X_OK) or (
                    os.path.exists(path) and not os.access(path, os.W_OK)):
                self._flash_error("The selected file or folder is not writable")
                return
            if os.path.exists(path) and self._pending_replace != path:
                self._pending_replace = path
                self._status.set("File exists. Choose Replace file to overwrite it, or change the name.")
                self.ok_button.configure(text="Replace file")
                return
            paths = (path,)
        elif not all(os.path.isfile(p) and os.access(p, os.R_OK) for p in paths):
            self._flash_error("Select existing, readable files")
            return
        self.result = tuple(paths) if self.multiple else paths[0]
        self.destroy()

    def use_system_dialog(self):
        chosen = pick_files_system(self, initialdir=self._cwd, title=self.title(),
                                   multiple=self.multiple, save=self.save,
                                   initialfile=self.name_var.get() if self.save else "",
                                   defaultextension=self.defaultextension, filetypes=self.filetypes)
        if chosen is None:
            self._status.set("System dialog unavailable, use this browser instead")
            return
        self.result = chosen
        self.destroy()

    def destroy(self):
        # Remove the variable callback and pending timers before releasing Tk objects.
        if getattr(self, "_name_trace", None):
            self.name_var.trace_remove("write", self._name_trace)
            self._name_trace = None
        commands = set(self._tclCommands or ())
        for timer in self.tk.splitlist(self.tk.call("after", "info")):
            command = self.tk.call("after", "info", timer)[0]
            if command in commands:
                self.after_cancel(timer)
        super().destroy()
        for name in ("name_var", "path_var", "_show_hidden", "_status", "name_entry",
                     "filter_box", "path_entry", "crumbs", "places_tree", "dirs_tree",
                     "preview", "ok_button"):
            setattr(self, name, None)

    def show(self):
        previous = self.grab_current()
        focus = self.master.focus_get() if self.master else None
        try:
            return super().show() or (() if self.multiple else "")
        finally:
            try:
                if previous is not None and previous.winfo_exists():
                    previous.grab_set()
                if focus is not None and focus.winfo_exists():
                    focus.focus_set()
            except tk.TclError:
                pass
