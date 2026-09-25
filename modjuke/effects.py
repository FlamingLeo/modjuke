"""Classify tracker effects by format and command. Categories follow
https://wiki.openmpt.org/Manual:_Effect_Reference."""

from __future__ import annotations

# the categories, in the order the wiki lists them
CATEGORIES = ("global", "volume", "pan", "pitch", "misc")

WIKI_COLOURS = {
    "global": "#800000",
    "volume": "#008000",
    "pan": "#008080",
    "pitch": "#808000",
    "misc": "#808080",
}

PALETTE_NAMES = {
    "global": "EFFECT_GLOBAL",
    "volume": "EFFECT_VOLUME",
    "pan": "EFFECT_PAN",
    "pitch": "EFFECT_PITCH",
    "misc": "EFFECT_MISC",
}

# the format families whose effect letters are the same set
FAMILIES = ("mod", "xm", "s3m", "it", "mptm")

# how libopenmpt's format string is recognised (checked in this order)
_FORMAT_KEYS = (
    ("mptm", "mptm"), ("openmpt", "mptm"),
    ("impulse", "it"), ("it ", "it"),
    ("scream", "s3m"), ("s3m", "s3m"),
    ("fasttracker", "xm"), ("xm", "xm"),
    ("mod", "mod"), ("pro", "mod"),
)

_LETTERS: dict = {
    "mod": {
        "global": "BDF",
        "volume": "7AC",
        "pan": "8",
        "pitch": "01234",
        "misc": "569",
        "sub": {"E": {"0": "misc", "1": "pitch", "2": "pitch", "3": "pitch",
                      "4": "pitch", "5": "pitch", "6": "global", "7": "volume",
                      "8": "pan", "9": "misc", "A": "volume", "B": "volume",
                      "C": "misc", "D": "misc", "E": "global", "F": "misc"}},
    },
    "xm": {
        "global": "BDF",
        "volume": "7AC GHLT".replace(" ", ""),
        "pan": "8PY",
        "pitch": "01234",
        "misc": "569KRZ\\",
        "sub": {"E": {"0": "misc", "1": "pitch", "2": "pitch", "3": "pitch",
                      "4": "pitch", "5": "pitch", "6": "global", "7": "volume",
                      "8": "pan", "9": "misc", "A": "volume", "B": "volume",
                      "C": "misc", "D": "misc", "E": "global", "F": "misc"},
                "X": {"1": "pitch", "2": "pitch", "5": "pan", "6": "global",
                      "9": "misc", "A": "misc"}},
    },
    "s3m": {
        "global": "ABCT",
        "volume": "DIMNRVW",
        "pan": "PXY",
        "pitch": "EFGHJU",
        "misc": "KLOQZ\\",
        "sub": {"S": {"1": "pitch", "2": "pitch", "3": "pitch", "4": "volume",
                      "5": "pan", "6": "global", "7": "misc", "8": "pan",
                      "9": "misc", "A": "misc", "B": "global", "C": "misc",
                      "D": "misc", "E": "global", "F": "misc"}},
    },
    # IT: S3M plus the S7x sound-control group and SFx
    "it": {
        "global": "ABCT",
        "volume": "DIMNRVW",
        "pan": "PXY",
        "pitch": "EFGHJU",
        "misc": "KLOQZ\\",
        "sub": {"S": {"1": "pitch", "2": "pitch", "3": "pitch", "4": "volume",
                      "5": "pan", "6": "global", "7": "misc", "8": "pan",
                      "9": "misc", "A": "misc", "B": "global", "C": "misc",
                      "D": "misc", "E": "global", "F": "misc"}},
    },
    "mptm": {
        "global": "ABCT",
        "volume": "DIMNRVW",
        "pan": "PXY",
        "pitch": "EFGHJU+*",
        "misc": "KLOQZ\\:#",
        "sub": {"S": {"1": "pitch", "2": "pitch", "3": "pitch", "4": "volume",
                      "5": "pan", "6": "global", "7": "misc", "8": "pan",
                      "9": "misc", "A": "misc", "B": "global", "C": "misc",
                      "D": "misc", "E": "global", "F": "misc"}},
    },
}

_VOLUME_LETTERS: dict = {
    "mod": {"volume": "v"},
    "xm": {"volume": "abcdv", "pitch": "ghu", "pan": "lpr"},
    "s3m": {"volume": "v", "pan": "p"},
    "it": {"volume": "abcdv", "pitch": "efgh", "pan": "p"},
    "mptm": {"volume": "abcdv", "pitch": "efgh", "pan": "p", "misc": "o"},
}

_EFFECT_TABLE: dict = {}
_VOLUME_TABLE: dict = {}
for _family in FAMILIES:
    table = {}
    for _category, _letters in _LETTERS[_family].items():
        if _category == "sub":
            continue
        for _letter in _letters:
            table[_letter] = _category
    _EFFECT_TABLE[_family] = table
    volume = {}
    for _category, _letters in _VOLUME_LETTERS[_family].items():
        for _letter in _letters:
            volume[_letter] = _category
    _VOLUME_TABLE[_family] = volume


def family_of(format_name: str) -> str:
    """Choose the effect format family; use MOD for unknown formats."""
    text = str(format_name or "").strip().lower()
    if text in FAMILIES:
        return text
    for needle, family in _FORMAT_KEYS:
        if needle in text:
            return family
    return "mod"


def category(effect: str, family: str = "mod") -> str:
    """Return the effect category, or an empty string for an empty cell."""
    text = str(effect or "").strip()
    if not text or text.startswith("."):
        return ""
    table = _LETTERS[family if family in _LETTERS else "mod"]
    letter = text[0]
    sub = table.get("sub", {}).get(letter)
    if sub is not None:
        # the parameter's first character picks the sub-command ("S70" -> "7")
        digit = text[1:2].upper()
        return sub.get(digit, "")
    found = _EFFECT_TABLE[family if family in _EFFECT_TABLE else "mod"].get(letter.upper())
    return found or ""


def volume_category(command: str, family: str = "mod") -> str:
    """Classify a volume-column command, including plain numeric volume values."""
    text = str(command or "").strip()
    if not text or text.startswith("."):
        return ""
    letter = text[0]
    if letter.isdigit():                    # a bare volume value ("64")
        return "volume"
    return _VOLUME_TABLE[family if family in _VOLUME_TABLE else "mod"].get(letter.lower(), "")


def colour_name(category_name: str) -> str:
    """The palette entry that paints category_name."""
    return PALETTE_NAMES.get(category_name, "")


def categories_for(effect: str, volume: str, family: str = "mod") -> tuple[str, str]:
    """Both columns of a cell at once, for a caller that draws them together."""
    return category(effect, family), volume_category(volume, family)
