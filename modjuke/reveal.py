"""Open a module in the platform file manager, selecting it when supported."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable, Optional

__all__ = ["file_uri", "candidate_commands", "reveal", "FILE_MANAGERS"]

# file managers that can highlight a file, with the flag that asks them to
FILE_MANAGERS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("nautilus", ("--select",)),
    ("dolphin", ("--select",)),
    ("nemo", ()),
    ("thunar", ()),
    ("caja", ("--select",)),
    ("pcmanfm-qt", ()),
    ("pcmanfm", ()),
)

BUS_NAME = "org.freedesktop.FileManager1"
BUS_PATH = "/org/freedesktop/FileManager1"

Which = Callable[[str], Optional[str]]
Runner = Callable[[list, float], int]


def file_uri(path: str) -> str:
    """file:///… URI of path (absolute, percent-encoded)."""
    return Path(path).expanduser().absolute().as_uri()


def candidate_commands(path: str, which: Which = shutil.which,
                       platform: str = sys.platform) -> list[tuple[list[str], bool]]:
    """Commands to try for path, best first, as (argv, selects_the_file)."""
    path = os.path.abspath(os.path.expanduser(path))
    folder = os.path.dirname(path)

    if platform.startswith("darwin"):
        return [(["open", "-R", path], True), (["open", folder], False)]

    if platform.startswith(("win", "cygwin")):
        return [(["explorer", f"/select,{os.path.normpath(path)}"], True)]

    attempts: list[tuple[list[str], bool]] = []
    if os.path.isfile(path):
        uri = file_uri(path)
        if which("gdbus"):
            attempts.append((["gdbus", "call", "--session", "--dest", BUS_NAME,
                              "--object-path", BUS_PATH, "--method",
                              f"{BUS_NAME}.ShowItems", f"['{uri}']", ""], True))
        if which("dbus-send"):
            attempts.append((["dbus-send", "--session", f"--dest={BUS_NAME}",
                              "--type=method_call", BUS_PATH,
                              f"{BUS_NAME}.ShowItems",
                              f"array:string:{uri}", 'string:""'], True))
        for name, flags in FILE_MANAGERS:
            if which(name):
                attempts.append(([name, *flags, path], True))
    if which("gio"):
        attempts.append((["gio", "open", folder], False))
    if which("xdg-open"):
        attempts.append((["xdg-open", folder], False))
    return attempts


def _run(argv: list, timeout: float) -> int:
    """Exit status of argv, or -1 when it could not be started."""
    try:
        done = subprocess.run(list(argv), stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=timeout, check=False)
        return int(done.returncode)
    except (OSError, subprocess.SubprocessError):
        return -1


def reveal(path: str, *, runner: Optional[Runner] = None, which: Which = shutil.which,
           platform: Optional[str] = None, timeout: float = 6.0) -> tuple[bool, str]:
    """Return (success, message) after trying the available file managers."""
    platform = platform or sys.platform
    path = os.path.abspath(os.path.expanduser(path))
    if not os.path.exists(path):
        return False, f"'{path}' is not there any more"

    runner = runner or _run
    tried: list[str] = []
    for argv, selects in candidate_commands(path, which, platform):
        tried.append(argv[0])
        status = runner(argv, timeout)
        # explorer.exe reports a non-zero status even when it worked
        if status == 0 or (platform.startswith(("win", "cygwin")) and status != -1):
            name = os.path.basename(path)
            folder = os.path.dirname(path)
            if selects:
                return True, f"Showed {name} in {folder}"
            return True, f"Opened {folder} (no file manager here can select the module)"
    names = ", ".join(dict.fromkeys(tried))
    return False, f"Could not open a file manager (tried {names})" if names else \
        "No file manager found on this system"
