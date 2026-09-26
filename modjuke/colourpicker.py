"""A small palette-aware colour picker, independent of OS dialog styling."""
from __future__ import annotations

import colorsys
import tkinter as tk
from tkinter import ttk

import numpy as np

from . import theme


class ColourPicker(tk.Toplevel):
    WIDTH, HEIGHT = 256, 192

    def __init__(self, parent, initial, title="Choose colour"):
        super().__init__(parent)
        self.withdraw()
        self.title(title)
        self.transient(parent)
        self.resizable(False, False)
        self.configure(background=theme.BG)
        self.result = None
        self._previous_grab = self.grab_current()
        self._updating = False
        self._paint_job = None
        self._painted_hue = None
        self._square_image = self._hue_image = None
        self._traces = []
        self.hex = tk.StringVar(self)
        self.rgb = [tk.StringVar(self) for _ in range(3)]
        self.error = tk.StringVar(self)
        self.hue = self.saturation = self.value = 0.0
        body = ttk.Frame(self, padding=16)
        body.pack(fill="both", expand=True)
        ttk.Label(body, text="Choose colour", style="Head.TLabel").grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, 12))
        left = ttk.Frame(body)
        left.grid(row=1, column=0, sticky="n", padx=(0, 18))
        self.square = tk.Canvas(left, width=self.WIDTH, height=self.HEIGHT,
                                background=theme.BG_INPUT, highlightthickness=1,
                                highlightbackground=theme.SEP, highlightcolor=theme.ACCENT)
        self.square.pack()
        self.square.bind("<Button-1>", self._pick_sv)
        self.square.bind("<B1-Motion>", self._pick_sv)
        ttk.Label(left, text="Saturation →    Brightness ↑").pack(anchor="w", pady=(5, 8))
        self.hue_strip = tk.Canvas(left, width=self.WIDTH, height=18,
                                   background=theme.BG_INPUT, highlightthickness=1,
                                   highlightbackground=theme.SEP, highlightcolor=theme.ACCENT)
        self.hue_strip.pack()
        self.hue_strip.bind("<Button-1>", self._pick_hue)
        self.hue_strip.bind("<B1-Motion>", self._pick_hue)
        ttk.Label(left, text="Hue").pack(anchor="w", pady=(4, 0))
        right = ttk.Frame(body)
        right.grid(row=1, column=1, sticky="nsew")
        ttk.Label(right, text="Current          New").grid(row=0, column=0, columnspan=3, sticky="w")
        self.swatches = tk.Canvas(right, width=216, height=40, highlightthickness=1,
                                  highlightbackground=theme.SEP, background=theme.BG_INPUT)
        self.swatches.grid(row=1, column=0, columnspan=3, sticky="w", pady=(5, 14))
        self.swatches.create_rectangle(0, 0, 108, 42, fill=initial, outline="", tags="original")
        self.swatches.create_rectangle(108, 0, 218, 42, fill=initial, outline="", tags="new")
        self.scales = []
        self.entries = []
        for index, name in enumerate(("Red", "Green", "Blue")):
            ttk.Label(right, text=name).grid(row=index+2, column=0, sticky="w", pady=6)
            scale = ttk.Scale(right, from_=0, to=255, length=120,
                              command=lambda value, i=index: self._scale_changed(i, value))
            scale.grid(row=index+2, column=1, padx=8)
            scale.bind("<Button-1>", self._scale_press)
            entry = ttk.Entry(right, textvariable=self.rgb[index], width=4)
            entry.grid(row=index+2, column=2)
            entry.bind("<Return>", self._accept_key)
            self.scales.append(scale)
            self.entries.append(entry)
        ttk.Label(right, text="Hex").grid(row=5, column=0, sticky="w", pady=(12, 0))
        self.hex_entry = ttk.Entry(right, textvariable=self.hex, width=12)
        self.hex_entry.grid(row=5, column=1, columnspan=2, sticky="w", padx=8, pady=(12, 0))
        self.hex_entry.bind("<Return>", self._accept_key)
        ttk.Label(body, textvariable=self.error, wraplength=490).grid(
            row=2, column=0, columnspan=2, sticky="w", pady=(12, 8))
        footer = ttk.Frame(body)
        footer.grid(row=3, column=0, columnspan=2, sticky="ew")
        cancel = ttk.Button(footer, text="Cancel", command=self.destroy)
        cancel.pack(side="right")
        cancel.bind("<Return>", lambda _e: self._cancel_key())
        self.accept_btn = ttk.Button(footer, text="Use colour", style="Accent.TButton", command=self.accept)
        self.accept_btn.pack(side="right", padx=8)
        self.accept_btn.bind("<Return>", self._accept_key)
        for variable, callback in [(self.hex, self._hex_changed), *[(v, self._rgb_changed) for v in self.rgb]]:
            self._traces.append((variable, variable.trace_add("write", callback)))
        self._set_hex(initial)
        self._draw_hue()
        self.bind("<Escape>", lambda _e: self._cancel_key())
        self.protocol("WM_DELETE_WINDOW", self.destroy)
        self.update_idletasks()
        x = max(0, parent.winfo_rootx() + (parent.winfo_width() - self.winfo_reqwidth()) // 2)
        y = max(0, parent.winfo_rooty() + (parent.winfo_height() - self.winfo_reqheight()) // 2)
        self.geometry(f"+{x}+{y}")
        self.deiconify()
        self.update_idletasks()
        self.grab_set()
        self.hex_entry.focus_set()
        if self.focus_get() != self.hex_entry:
            self.hex_entry.focus_force()
        self.hex_entry.selection_range(0, "end")

    @staticmethod
    def _rgb_value(colour):
        return tuple(int(colour[i:i+2], 16) for i in (1, 3, 5))

    def _set_hex(self, colour):
        r, g, b = self._rgb_value(colour)
        hue, saturation, value = colorsys.rgb_to_hsv(r/255, g/255, b/255)
        if saturation:
            self.hue = hue
        self.saturation, self.value = saturation, value
        self._sync((r, g, b))

    def _sync(self, rgb=None):
        if rgb is None:
            rgb = tuple(round(v*255) for v in colorsys.hsv_to_rgb(self.hue, self.saturation, self.value))
        colour = "#%02x%02x%02x" % rgb
        self._updating = True
        try:
            self.hex.set(colour)
            for value, variable, scale in zip(rgb, self.rgb, self.scales):
                variable.set(str(value))
                scale.set(value)
        finally:
            self._updating = False
        self.swatches.itemconfigure("new", fill=colour)
        self.error.set("Choose a colour, or enter RGB values (0-255) / #RRGGBB.")
        self.accept_btn.state(["!disabled"])
        if self._paint_job is None:
            self._paint_job = self.after_idle(self._draw_square)
        self.hue_strip.delete("marker")
        x = self.hue * (self.WIDTH-1)
        for offset, colour in ((0, "#000000"), (1, "#ffffff")):
            self.hue_strip.create_line(x+offset, 0, x+offset, 18, fill=colour, tags="marker")

    def _hex_changed(self, *_args):
        if self._updating:
            return
        value = self.hex.get().strip()
        if not theme.valid_colour(value):
            self._invalid("Enter a six-digit hex colour, for example #5AA9FF.")
            return
        self._set_hex(value)

    def _rgb_changed(self, *_args):
        if self._updating:
            return
        values = [v.get().strip() for v in self.rgb]
        if any(not v.isascii() or not v.isdigit() or len(v) > 3 for v in values):
            self._invalid("Red, green and blue must be whole numbers from 0 to 255.")
            return
        rgb = tuple(int(v) for v in values)
        if any(v > 255 for v in rgb):
            self._invalid("Red, green and blue must be whole numbers from 0 to 255.")
            return
        self._set_hex("#%02x%02x%02x" % rgb)

    def _invalid(self, message):
        self.error.set(message)
        self.accept_btn.state(["disabled"])

    def _scale_changed(self, index, value):
        if self._updating:
            return
        rgb = list(self._rgb_value(str(self.swatches.itemcget("new", "fill"))))
        rgb[index] = max(0, min(255, round(float(value))))
        self._set_hex("#%02x%02x%02x" % tuple(rgb))

    def _scale_press(self, event):
        event.widget.focus_set()
        event.widget.tk.call("ttk::scale::Jump", event.widget, event.x, event.y)
        return "break"

    def _pick_sv(self, event):
        self.saturation = min(1.0, max(0.0, (event.x-1)/(self.WIDTH-1)))
        self.value = 1.0-min(1.0, max(0.0, (event.y-1)/(self.HEIGHT-1)))
        self._sync()
        return "break"

    def _pick_hue(self, event):
        self.hue = min(1.0, max(0.0, (event.x-1)/(self.WIDTH-1)))
        self._sync()
        return "break"

    def _draw_hue(self):
        row = bytes(round(v*255) for x in range(self.WIDTH)
                    for v in colorsys.hsv_to_rgb(x/(self.WIDTH-1), 1, 1))
        data = f"P6\n{self.WIDTH} 18\n255\n".encode() + row*18
        self._hue_image = tk.PhotoImage(master=self, data=data, format="PPM")
        self.hue_strip.create_image(0, 0, anchor="nw", image=self._hue_image)
        self.hue_strip.tag_raise("marker")

    def _draw_square(self):
        self._paint_job = None
        if self._painted_hue != self.hue:
            base = colorsys.hsv_to_rgb(self.hue, 1, 1)
            top = 1 + np.linspace(0, 1, self.WIDTH)[:, None] * (np.asarray(base)-1)
            pixels = np.rint(np.linspace(1, 0, self.HEIGHT)[:, None, None] * top[None, :, :] * 255)
            data = pixels.astype(np.uint8).tobytes()
            image = tk.PhotoImage(master=self, data=f"P6\n{self.WIDTH} {self.HEIGHT}\n255\n".encode()+data,
                                  format="PPM")
            self.square.delete("gradient")
            self.square.create_image(0, 0, anchor="nw", image=image, tags="gradient")
            self._square_image = image
            self._painted_hue = self.hue
        self.square.delete("marker")
        x, y = self.saturation*(self.WIDTH-1), (1-self.value)*(self.HEIGHT-1)
        for radius, colour in ((5, "#000000"), (4, "#ffffff")):
            self.square.create_oval(x-radius, y-radius, x+radius, y+radius,
                                    outline=colour, tags="marker")

    def _accept_key(self, _event=None):
        self.accept()
        return "break"

    def _cancel_key(self):
        self.destroy()
        return "break"

    def accept(self):
        if not self.accept_btn.instate(["disabled"]):
            self.result = self.hex.get().strip().lower()
            self.destroy()

    def destroy(self):
        if self._paint_job is not None:
            self.after_cancel(self._paint_job)
            self._paint_job = None
        for variable, token in self._traces:
            variable.trace_remove("write", token)
        self._traces.clear()
        previous, parent = self._previous_grab, self.master
        self._previous_grab = None
        super().destroy()
        self.hex = self.error = None
        self.rgb.clear()
        self.scales.clear()
        self.entries.clear()
        self.square = self.hue_strip = self.swatches = self.hex_entry = self.accept_btn = None
        self._square_image = self._hue_image = None
        try:
            if previous is not None and previous.winfo_exists():
                previous.grab_set()
            if parent.winfo_exists():
                parent.focus_set()
        except tk.TclError:
            pass


def ask_colour(initial, *, parent, title="Choose colour"):
    dialog = ColourPicker(parent, initial, title)
    parent.wait_window(dialog)
    return dialog.result
