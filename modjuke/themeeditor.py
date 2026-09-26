"""Isolated palette editor: previews never change the running player's colours."""
from __future__ import annotations

import tkinter as tk
from tkinter import ttk

from . import theme
from .colourpicker import ask_colour

GROUPS = (
    ("Surfaces", (("BG", "Window background"), ("BG_PANEL", "Panels and tabs"),
                  ("BG_ALT", "Buttons and raised surfaces"), ("BG_STRIPE", "Alternate queue rows"),
                  ("BG_INPUT", "Inputs, queue and tracker"), ("BUTTON_OFF_BG", "Disabled buttons"))),
    ("Text and accents", (("FG", "Main text"), ("FG_DIM", "Secondary text"),
                         ("FG_FAINT", "Disabled text and hints"), ("ACCENT", "Links and active accents"),
                         ("ACCENT_DIM", "Selection and active buttons"), ("ON_ACCENT", "Text on selection"),
                         ("SEP", "Separators and volume slider"))),
    ("Status and meters", (("GREEN", "Success and normal levels"), ("AMBER", "Warnings and high levels"),
                           ("RED", "Errors and peak levels"), ("PURPLE", "Supplementary accent"),
                           ("VU_BASELINE", "Meter baseline"))),
    ("Tracker", (("TRACKER_BEAT", "Beat row background"), ("TRACKER_PLAYING", "Playing row background"),
                 ("TRACKER_FAINT", "Empty cells"), ("TRACKER_DIM", "Instrument and dim text"),
                 ("TRACKER_BRIGHT", "Notes and bright text"))),
    ("Seek bar", (("SEEK_TRACK", "Seek rail"), ("SEEK_TRACK_OFF", "Disabled seek rail"),
                  ("SEEK_FILL_OFF", "Disabled knob outline"), ("SEEK_KNOB", "Seek knob"),
                  ("SEEK_MARKER", "Hover marker"))),
    ("Tracker effects", (("EFFECT_GLOBAL", "Global effects"), ("EFFECT_VOLUME", "Volume effects"),
                         ("EFFECT_PAN", "Panning effects"), ("EFFECT_PITCH", "Pitch effects"),
                         ("EFFECT_MISC", "Other effects"))),
)
LABELS = {key: label for _group, roles in GROUPS for key, label in roles}


class ThemeEditor(tk.Toplevel):
    def __init__(self, parent, name, colours, on_save):
        super().__init__(parent)
        self.title("Theme editor")
        self.transient(parent)
        self.geometry("1000x660")
        self.minsize(900, 620)
        self.colours = dict(colours)
        self.original = dict(colours)
        self.on_save = on_save
        self.role = "BG"
        self._updating = False
        self._previous_grab = self.grab_current()
        self.name = tk.StringVar(self, value=name)
        self.hex = tk.StringVar(self)
        self.detail = tk.StringVar(self)
        self.error = tk.StringVar(self)
        body = ttk.Frame(self, padding=16)
        body.pack(fill="both", expand=True)
        body.columnconfigure(1, weight=1)
        body.rowconfigure(2, weight=1)
        heading = ttk.Frame(body)
        heading.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 10))
        ttk.Label(heading, text="Theme name").pack(side="left")
        self.name_entry = ttk.Entry(heading, textvariable=self.name, width=32)
        self.name_entry.pack(side="left", padx=10)
        ttk.Label(body, text="Choose a colour role or click the preview. Shared roles colour several components.").grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(0, 10))
        left = ttk.Frame(body)
        left.grid(row=2, column=0, sticky="nsew", padx=(0, 16))
        left.rowconfigure(0, weight=1)
        left.columnconfigure(0, weight=1)
        self.tree = ttk.Treeview(left, columns=("colour",), show="tree headings", selectmode="browse")
        self.tree.heading("#0", text="Component / role")
        self.tree.heading("colour", text="Colour")
        self.tree.column("#0", width=210, minwidth=180)
        self.tree.column("colour", width=76, stretch=False)
        self.tree.grid(row=0, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(left, orient="vertical", command=self.tree.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.tree.configure(yscrollcommand=scroll.set)
        for group, roles in GROUPS:
            node = self.tree.insert("", "end", text=group, open=True)
            for key, label in roles:
                self.tree.insert(node, "end", iid=key, text=label, values=(self.colours[key],))
        self.tree.bind("<<TreeviewSelect>>", self._select)
        right = ttk.Frame(body)
        right.grid(row=2, column=1, sticky="nsew")
        right.columnconfigure(0, weight=1)
        right.rowconfigure(1, weight=1)
        ttk.Label(right, text="Preview - sample UI", style="Head.TLabel").grid(row=0, column=0, sticky="w")
        self.preview = tk.Canvas(right, highlightthickness=0, width=580, height=430)
        self.preview.grid(row=1, column=0, sticky="nsew", pady=8)
        self.preview.bind("<Configure>", lambda _e: self.redraw())
        self.preview.bind("<Button-1>", self._pick_preview)
        ttk.Label(right, textvariable=self.detail).grid(row=2, column=0, sticky="w")
        colour_row = ttk.Frame(right)
        colour_row.grid(row=3, column=0, sticky="ew", pady=8)
        self.swatch = tk.Canvas(colour_row, width=28, height=28, highlightthickness=1)
        self.swatch.pack(side="left", padx=(0, 8))
        self.hex_entry = ttk.Entry(colour_row, textvariable=self.hex, width=10)
        self.hex_entry.pack(side="left")
        ttk.Button(colour_row, text="Choose colour…", command=self.choose).pack(side="left", padx=8)
        ttk.Button(colour_row, text="Reset colour", command=self.reset_colour).pack(side="left")
        ttk.Label(right, textvariable=self.error, wraplength=540).grid(row=4, column=0, sticky="w")
        footer = ttk.Frame(body)
        footer.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(14, 0))
        ttk.Label(footer, text="Nothing is applied or written until you save Settings.").pack(side="left")
        ttk.Button(footer, text="Cancel", command=self.destroy).pack(side="right")
        ttk.Button(footer, text="Use theme", style="Accent.TButton", command=self.save).pack(side="right", padx=8)
        self._trace = self.hex.trace_add("write", self._change)
        self.tree.selection_set(self.role)
        self._show_role()
        self.bind("<Escape>", lambda _e: self.destroy())
        self.protocol("WM_DELETE_WINDOW", self.destroy)
        self.grab_set()
        self.name_entry.focus_set()

    def _select(self, _event=None):
        selection = self.tree.selection()
        if selection and selection[0] in LABELS:
            self.role = selection[0]
            self._show_role()

    def _show_role(self):
        self._updating = True
        self.hex.set(self.colours[self.role])
        self._updating = False
        self.detail.set(LABELS[self.role] + "  ·  " + self.role)
        self.swatch.configure(background=self.colours[self.role])
        self.error.set("Use #RRGGBB, or open the colour picker.")

    def _change(self, *_args):
        if self._updating:
            return
        value = self.hex.get().strip()
        if not theme.valid_colour(value):
            self.error.set("Enter a six-digit hex colour, for example #5AA9FF.")
            return
        self.colours[self.role] = value.lower()
        self.tree.set(self.role, "colour", value.lower())
        self.swatch.configure(background=value)
        self.error.set("Preview updated. Settings and playback are unchanged.")
        self.redraw()

    def choose(self):
        try:
            value = ask_colour(self.colours[self.role], parent=self, title=LABELS[self.role])
            if value and self.winfo_exists():
                self.hex.set(value)
        except tk.TclError:  # The editor or its parent closed while the picker was open.
            pass

    def reset_colour(self):
        self.hex.set(self.original[self.role])

    def _pick_preview(self, event):
        items = self.preview.find_overlapping(event.x, event.y, event.x, event.y)
        for item in reversed(items):
            for tag in self.preview.gettags(item):
                if tag in LABELS:
                    self.tree.selection_set(tag)
                    self.tree.see(tag)
                    self.role = tag
                    self._show_role()
                    return

    def redraw(self):
        c, p = self.preview, self.colours
        if c.winfo_height() <= 1:
            return
        c.delete("all")
        c.configure(background=p["BG"])
        w = max(480, c.winfo_width())

        def box(x, y, right, bottom, role):
            c.create_rectangle(x, y, right, bottom, fill=p[role], outline="", tags=(role,))

        def text(x, y, value, role="FG", mono=False):
            c.create_text(x, y, text=value, fill=p[role], anchor="w",
                          font=("TkFixedFont" if mono else "TkDefaultFont", 10), tags=(role,))

        box(0, 0, w, 38, "BG_PANEL")
        text(12, 19, "modjuke", "FG")
        text(112, 19, "Player", "ACCENT")
        text(185, 19, "Tracker", "FG_DIM")
        box(12, 51, w-12, 83, "BG_INPUT")
        text(22, 67, "Search modules…", "FG_DIM")
        for i, (label, bg, fg) in enumerate((("Play", "ACCENT_DIM", "ON_ACCENT"),
                                            ("Next", "BG_ALT", "FG"),
                                            ("Disabled", "BUTTON_OFF_BG", "FG_FAINT"))):
            x = 12 + i*108
            box(x, 95, x+98, 125, bg)
            text(x+12, 110, label, fg)
        for i, label in enumerate(("Module one.mod", "Module two.xm", "Now playing.it")):
            y = 137+i*24
            box(12, y, w-12, y+24, "ACCENT_DIM" if i==2 else ("BG_STRIPE" if i==1 else "BG_INPUT"))
            text(22, y+12, label, "ON_ACCENT" if i==2 else "FG")
        box(12, 215, w-12, 217, "SEP")
        box(12, 225, w-12, 230, "SEEK_TRACK")
        box(12, 225, w*.42, 230, "ACCENT")
        c.create_oval(w*.42-5, 221, w*.42+5, 233, fill=p["SEEK_KNOB"], outline="", tags=("SEEK_KNOB",))
        box(w*.65, 220, w*.65+1, 235, "SEEK_MARKER")
        box(12, 244, w*.48, 248, "SEEK_TRACK_OFF")
        c.create_oval(9, 240, 19, 251, outline=p["SEEK_FILL_OFF"], tags=("SEEK_FILL_OFF",))
        for i, (bg, number) in enumerate((("TRACKER_BEAT", "00"), ("BG_INPUT", "01"), ("TRACKER_PLAYING", "02"))):
            y = 266+i*25
            box(12, y, w-12, y+25, bg)
            text(20, y+12, number, "ON_ACCENT" if i==2 else "FG_DIM", True)
            text(56, y+12, "C-4", "TRACKER_BRIGHT", True)
            text(96, y+12, "01", "TRACKER_DIM", True)
            text(129, y+12, "...", "TRACKER_FAINT", True)
            for j, (value, role) in enumerate((("A06", "EFFECT_GLOBAL"), ("v32", "EFFECT_VOLUME"),
                                              ("P20", "EFFECT_PAN"), ("G03", "EFFECT_PITCH"),
                                              ("O10", "EFFECT_MISC"))):
                text(179+j*57, y+12, value, role, True)
        box(12, 355, w-12, 357, "VU_BASELINE")
        for i, role in enumerate(("GREEN", "AMBER", "RED")):
            box(12+i*58, 366, 62+i*58, 380, role)
        text(204, 373, "Status", "PURPLE")
        # Every palette role remains visible and selectable, including subtle disabled states.
        for i, role in enumerate(theme.COLOUR_NAMES):
            x = 12+i*(w-24)/len(theme.COLOUR_NAMES)
            box(x, 397, x+(w-24)/len(theme.COLOUR_NAMES)-2, 418, role)

        if c.winfo_height() < 430:
            c.scale("all", 0, 0, 1, c.winfo_height() / 430)

    def save(self):
        if not theme.valid_colour(self.hex.get().strip()):
            self.error.set("Fix the selected hex colour before using this theme.")
            self.hex_entry.focus_set()
            return
        error = self.on_save(self.name.get().strip(), dict(self.colours))
        if error:
            self.error.set(error)
            self.name_entry.focus_set()
            return
        self.destroy()

    def destroy(self):
        if getattr(self, "_trace", None):
            self.hex.trace_remove("write", self._trace)
            self._trace = None
        parent = self.master
        previous = getattr(self, "_previous_grab", None)
        super().destroy()
        self.on_save = None
        self._previous_grab = None
        for name in ("tree", "preview", "swatch", "name_entry", "hex_entry",
                     "name", "hex", "detail", "error"):
            setattr(self, name, None)
        try:
            if previous is not None and previous.winfo_exists():
                previous.grab_set()
            if parent.winfo_exists():
                parent.focus_set()
        except tk.TclError:
            pass
