"""Locate the same PNG in a source checkout or an installed wheel."""
from pathlib import Path
import site
import sys
import tkinter as tk


def set_window_icon(root):
    candidates = [Path(__file__).resolve().parent.parent / "img/modjuke.png",
                  Path(sys.prefix) / "share/modjuke/modjuke.png",
                  Path(site.getuserbase()) / "share/modjuke/modjuke.png"]
    for path in candidates:
        if not path.is_file():
            continue
        try:
            image = tk.PhotoImage(master=root, file=str(path))
            root.iconphoto(True, image)
            return image
        except (OSError, tk.TclError):
            continue
    return None
