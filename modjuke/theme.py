"""Five shared colour palettes, switchable while the app is running."""

import sys

BG = "#1b1d23"
BG_PANEL = "#23262e"
BG_ALT = "#2a2e38"
BG_STRIPE = "#272b35"  # every second queue row (a band, not a highlight)
BG_INPUT = "#15171c"
BUTTON_OFF_BG = "#16181e"  # a disabled button's surface (an enabled one: BG_ALT)
FG = "#e7e9ee"
FG_DIM = "#939aab"
FG_FAINT = "#5c6370"   # disabled arrows / hint text
ACCENT = "#5aa9ff"
ACCENT_DIM = "#2f5f96"
GREEN = "#4ac97e"
AMBER = "#e0b050"
RED = "#e06c75"
PURPLE = "#b48ead"
SEP = "#4d5567"      # the header's column borders (and the sliders)

# the pattern view's own shades (canvas text, drawn over BG_INPUT)
TRACKER_BEAT = "#20242c"      # every fourth row, so beats are countable
TRACKER_PLAYING = ACCENT_DIM
TRACKER_FAINT = "#3f4757"     # the "..." of an empty cell
TRACKER_DIM = "#a9b1c2"
TRACKER_BRIGHT = "#ffffff"

ON_ACCENT = "#ffffff"

VU_BASELINE = "#3a3f4b"       # the line the VU bars stand on
SEEK_TRACK = "#31353f"        # the rail behind the knob
SEEK_TRACK_OFF = "#282c34"    # ...while the position bar is disabled
SEEK_FILL_OFF = "#4a5160"     # its disabled knob outline
SEEK_KNOB = "#f2f4f8"         # the knob itself
SEEK_MARKER = "#6d7686"       # the hover marker line

EFFECT_GLOBAL = "#ff8b7b"     # jumps, breaks, speed, tempo, loops
EFFECT_VOLUME = "#7bd88a"     # volume slides, set volume, channel volume
EFFECT_PAN = "#5fd0d0"        # panning and panning slides
EFFECT_PITCH = "#d8c85f"      # portamentos, vibrato, arpeggio, finetune
EFFECT_MISC = "#a8b2c4"       # offsets, retrigger, note cut, filters

# every name above, in the order the palettes below spell them out
COLOUR_NAMES = (
    "BG", "BG_PANEL", "BG_ALT", "BG_STRIPE", "BG_INPUT", "BUTTON_OFF_BG",
    "FG", "FG_DIM", "FG_FAINT",
    "ACCENT", "ACCENT_DIM", "GREEN", "AMBER", "RED", "PURPLE", "SEP",
    "TRACKER_BEAT", "TRACKER_PLAYING", "TRACKER_FAINT", "TRACKER_DIM", "TRACKER_BRIGHT",
    "ON_ACCENT", "VU_BASELINE", "SEEK_TRACK", "SEEK_TRACK_OFF", "SEEK_FILL_OFF",
    "SEEK_KNOB", "SEEK_MARKER",
    "EFFECT_GLOBAL", "EFFECT_VOLUME", "EFFECT_PAN", "EFFECT_PITCH", "EFFECT_MISC",
)

_THEMES: dict = {
    # a daylight window: light surfaces, dark ink, one blue accent
    "light": {
        "BG": "#f2f3f6", "BG_PANEL": "#ffffff", "BG_ALT": "#e2e5ec",
        "BG_STRIPE": "#f4f6f9", "BG_INPUT": "#eef1f6", "BUTTON_OFF_BG": "#edeff4",
        "FG": "#1b1f27", "FG_DIM": "#5a6474", "FG_FAINT": "#8b94a5",
        "ACCENT": "#1a6ef5", "ACCENT_DIM": "#b7d3fb",
        "GREEN": "#0f7a42", "AMBER": "#7a5300", "RED": "#c62b39", "PURPLE": "#7b4fa8",
        "SEP": "#b3bac7",
        "TRACKER_BEAT": "#e6eaf1", "TRACKER_PLAYING": "#b7d3fb",
        "TRACKER_FAINT": "#aab3c2", "TRACKER_DIM": "#38414f", "TRACKER_BRIGHT": "#11151c",
        "ON_ACCENT": "#10305c",
        "VU_BASELINE": "#c3cad6", "SEEK_TRACK": "#c3cad6", "SEEK_TRACK_OFF": "#d8dde5",
        "SEEK_FILL_OFF": "#b3bac7", "SEEK_KNOB": "#ffffff", "SEEK_MARKER": "#6b7484",
        # a white surface: the wiki's own effect colours, unchanged
        "EFFECT_GLOBAL": "#800000", "EFFECT_VOLUME": "#008000", "EFFECT_PAN": "#008080",
        "EFFECT_PITCH": "#808000", "EFFECT_MISC": "#808080",
    },
    # deeper than dark and bluer: a late-night window
    "midnight": {
        "BG": "#0b0f1a", "BG_PANEL": "#111726", "BG_ALT": "#1a2333",
        "BG_STRIPE": "#161e30", "BG_INPUT": "#070a12", "BUTTON_OFF_BG": "#10151f",
        "FG": "#dbe4f5", "FG_DIM": "#8493b0", "FG_FAINT": "#4d5a75",
        "ACCENT": "#4f8cff", "ACCENT_DIM": "#1f3f7a",
        "GREEN": "#3fb87a", "AMBER": "#d9a544", "RED": "#e0606c", "PURPLE": "#a48ad4",
        "SEP": "#2c3a55",
        "TRACKER_BEAT": "#0f1523", "TRACKER_PLAYING": "#1f3f7a",
        "TRACKER_FAINT": "#2a3550", "TRACKER_DIM": "#9aa8c4", "TRACKER_BRIGHT": "#ffffff",
        "ON_ACCENT": "#ffffff",
        "VU_BASELINE": "#1e2940", "SEEK_TRACK": "#1e2a42", "SEEK_TRACK_OFF": "#151d2e",
        "SEEK_FILL_OFF": "#2b3a56", "SEEK_KNOB": "#dfe7f6", "SEEK_MARKER": "#5c6d8f",
        "EFFECT_GLOBAL": "#ff8f8f", "EFFECT_VOLUME": "#6fd39a", "EFFECT_PAN": "#5ccfe6",
        "EFFECT_PITCH": "#d9cc66", "EFFECT_MISC": "#9aa8c4",
    },
    # black and white: the highest contrast the palette can offer
    "high-contrast": {
        "BG": "#000000", "BG_PANEL": "#0d0d0d", "BG_ALT": "#2b2b2b",
        "BG_STRIPE": "#222222", "BG_INPUT": "#000000", "BUTTON_OFF_BG": "#000000",
        "FG": "#ffffff", "FG_DIM": "#d0d0d0", "FG_FAINT": "#9a9a9a",
        "ACCENT": "#4fd2ff", "ACCENT_DIM": "#005f8a",
        "GREEN": "#00e676", "AMBER": "#ffc400", "RED": "#ff5252", "PURPLE": "#d0a2ff",
        "SEP": "#8a8a8a",
        "TRACKER_BEAT": "#141414", "TRACKER_PLAYING": "#005f8a",
        "TRACKER_FAINT": "#8a8a8a", "TRACKER_DIM": "#dcdcdc", "TRACKER_BRIGHT": "#ffffff",
        "ON_ACCENT": "#ffffff",
        "VU_BASELINE": "#666666", "SEEK_TRACK": "#4a4a4a", "SEEK_TRACK_OFF": "#333333",
        "SEEK_FILL_OFF": "#666666", "SEEK_KNOB": "#ffffff", "SEEK_MARKER": "#ffffff",
        "EFFECT_GLOBAL": "#ff7b7b", "EFFECT_VOLUME": "#4dff88", "EFFECT_PAN": "#4dd8e6",
        "EFFECT_PITCH": "#ffd24d", "EFFECT_MISC": "#cfcfcf",
    },
    # a monochrome amber phosphor screen
    "amber": {
        "BG": "#120c02", "BG_PANEL": "#1a1206", "BG_ALT": "#2a1d09",
        "BG_STRIPE": "#231a0a", "BG_INPUT": "#0b0700", "BUTTON_OFF_BG": "#1c1305",
        "FG": "#ffcf7f", "FG_DIM": "#c08f43", "FG_FAINT": "#8a6430",
        "ACCENT": "#ffb000", "ACCENT_DIM": "#6f4700",
        "GREEN": "#a8d05f", "AMBER": "#ffb000", "RED": "#ff7043", "PURPLE": "#d9a441",
        "SEP": "#5c3f14",
        "TRACKER_BEAT": "#170f03", "TRACKER_PLAYING": "#6f4700",
        "TRACKER_FAINT": "#6b4a1c", "TRACKER_DIM": "#e0ad5e", "TRACKER_BRIGHT": "#ffe3a8",
        "ON_ACCENT": "#ffe8b0",
        "VU_BASELINE": "#4a3208", "SEEK_TRACK": "#6b4a1c", "SEEK_TRACK_OFF": "#3d2907",
        "SEEK_FILL_OFF": "#7a5518", "SEEK_KNOB": "#ffd98a", "SEEK_MARKER": "#c58e3a",
        "EFFECT_GLOBAL": "#ff9a6b", "EFFECT_VOLUME": "#b9d97a", "EFFECT_PAN": "#7fd8c0",
        "EFFECT_PITCH": "#ffcf5c", "EFFECT_MISC": "#c9a86b",
    },
}
_THEMES["dark"] = {name: globals()[name] for name in COLOUR_NAMES}

# what the settings file stores / --theme accepts, in menu order
THEME_NAMES = ("dark", "light", "midnight", "high-contrast", "amber")
# ...and what the Settings dialog shows
THEME_LABELS = {
    "dark": "Dark (default)",
    "light": "Light",
    "midnight": "Midnight",
    "high-contrast": "High contrast",
    "amber": "Amber CRT",
}
DEFAULT_THEME = "dark"

# spellings accepted for a palette name (keys are already normalised)
_ALIASES = {
    "default": "dark", "contrast": "high-contrast", "highcontrast": "high-contrast",
    "hc": "high-contrast", "ambercrt": "amber", "crt": "amber", "lighttheme": "light",
}

_current = DEFAULT_THEME


def normalise(name) -> str:
    """A palette key for whatever spelling came in (unknown -> the default)."""
    key = str(name or "").strip().lower().replace("_", "-").replace(" ", "-")
    key = _ALIASES.get(key.replace("-", ""), _ALIASES.get(key, key))
    return key if key in _THEMES else DEFAULT_THEME


def current() -> str:
    """The name of the palette the module currently holds."""
    return _current


def palette(name) -> dict:
    """Every colour of one palette (unknown names fall back to the default)."""
    return dict(_THEMES[normalise(name)])


def label(name) -> str:
    """The palette's name as the Settings dialog shows it."""
    return THEME_LABELS[normalise(name)]


def _rebind(old: dict, new: dict) -> list:
    """Update imported colours only when they still match the old palette."""
    moved = []
    for module in list(sys.modules.values()):
        name = getattr(module, "__name__", "") or ""
        if not name.startswith("modjuke.") or module is sys.modules[__name__]:
            continue
        namespace = getattr(module, "__dict__", None)
        if not namespace:
            continue
        for key, value in old.items():
            if namespace.get(key) == value:
                namespace[key] = new[key]
                moved.append((name, key))
    return moved


def activate(name) -> str:
    """Activate a palette and return the name actually selected."""
    global _current
    key = normalise(name)
    old = _THEMES[_current]
    new = _THEMES[key]
    if key != _current:
        _rebind(old, new)
        globals().update({colour: new[colour] for colour in COLOUR_NAMES})
        _current = key
    return _current
