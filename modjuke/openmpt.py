"""ctypes bindings for libopenmpt. Decode returned UTF-8 strings and free them with
openmpt_free_string."""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import platform
import sys
import threading
from dataclasses import dataclass, field
from typing import Iterable, Optional


class OpenMPTError(RuntimeError):
    """Raised when libopenmpt is missing or a module cannot be created."""

RENDER_INTERPOLATIONFILTER_LENGTH = 3

INTERPOLATION_FILTERS: dict[str, int] = {
    "off": 1,        # no interpolation
    "linear": 2,
    "cubic": 4,
    "sinc": 8,
}

# the filter libopenmpt itself uses unless told otherwise
DEFAULT_INTERPOLATION = "sinc"


def interpolation_length(mode) -> Optional[int]:
    """Return the filter length for a name or supported length, return None for unknown values."""
    if isinstance(mode, bool):
        return None
    if isinstance(mode, int):
        return mode if mode in INTERPOLATION_FILTERS.values() else None
    text = str(mode or "").strip().lower()
    if text in INTERPOLATION_FILTERS:
        return INTERPOLATION_FILTERS[text]
    try:
        length = int(text)
    except ValueError:
        return None
    return interpolation_length(length)


def interpolation_name(length: int) -> str:
    """Return the filter name for a supported length, or an empty string."""
    length = int(length or 0)
    if length >= 8:
        return "sinc"
    for name, want in INTERPOLATION_FILTERS.items():
        if length == want:
            return name
    return ""

_CANDIDATES = [
    "libopenmpt.so.0",
    "libopenmpt.so",
    "libopenmpt.0.dylib",
    "libopenmpt.dylib",
    "libopenmpt.dll",
    "openmpt.dll",
    "libopenmpt-0.dll",
]


def _candidate_paths() -> Iterable[str]:
    env = os.environ.get("MODJUKE_LIBOPENMPT")
    if env:
        yield env
    found = ctypes.util.find_library("openmpt")
    if found:
        yield found
    yield from _CANDIDATES
    # Windows / macOS installs by hand
    if platform.system() == "Windows":
        for base in (
            os.environ.get("ProgramFiles", r"C:\Program Files"),
            os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
        ):
            for sub in ("libopenmpt", r"libopenmpt\bin", r"openmpt\bin"):
                yield os.path.join(base, sub, "libopenmpt.dll")
        yield os.path.join(os.path.dirname(sys.executable), "libopenmpt.dll")
    if platform.system() == "Darwin":
        yield "/opt/homebrew/lib/libopenmpt.dylib"
        yield "/usr/local/lib/libopenmpt.dylib"
        yield "/opt/local/lib/libopenmpt.dylib"


def _load_library() -> ctypes.CDLL:
    errors = []
    for cand in _candidate_paths():
        try:
            if os.path.sep in cand and not os.path.exists(cand):
                continue
            return ctypes.CDLL(cand)
        except OSError as exc:  # pragma: no cover - platform dependent
            errors.append(f"{cand}: {exc}")
    hint = (
        "libopenmpt could not be found.\n"
        "  Debian/Ubuntu : sudo apt install libopenmpt0\n"
        "  Fedora        : sudo dnf install libopenmpt\n"
        "  Arch          : sudo pacman -S libopenmpt\n"
        "  macOS         : brew install libopenmpt\n"
        "  Windows       : install libopenmpt and put libopenmpt.dll on PATH\n"
        "You can also set MODJUKE_LIBOPENMPT=/path/to/libopenmpt.so"
    )
    raise OpenMPTError(hint + "\n\nTried:\n  " + "\n  ".join(errors))

LOG_FUNC = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p)

LIBOPENMPT_VERSION_MAJOR = 0
LIBOPENMPT_VERSION_MINOR = 1
LIBOPENMPT_VERSION_PATCH = 2
LIBOPENMPT_VERSION_PREREL = 3


class _InitialCtl(ctypes.Structure):
    _fields_ = [("ctl", ctypes.c_char_p), ("value", ctypes.c_char_p)]


class LibOpenMPT:
    """Thin wrapper around the C library handling signatures + strings."""

    def __init__(self, path: Optional[str] = None):
        self._library = ctypes.CDLL(path) if path else _load_library()
        lib = self._library
        self.lib = lib

        c_void_p, c_char_p, c_int, c_int32, c_size_t = (
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_int32,
            ctypes.c_size_t,
        )
        c_double, c_float = ctypes.c_double, ctypes.c_float

        def sig(name, res, *args):
            fn = getattr(lib, name)
            fn.restype = res
            fn.argtypes = list(args)
            return fn

        def sig_opt(name, res, *args):
            """Bind a symbol that only exists in newer libopenmpt versions."""
            try:
                return sig(name, res, *args)
            except AttributeError:
                return None

        self._get_library_version = sig("openmpt_get_library_version", ctypes.c_uint32)
        self._is_extension_supported = sig("openmpt_is_extension_supported", c_int, c_char_p)
        self._free_string = sig("openmpt_free_string", None, c_void_p)

        self._module_create_from_memory2 = sig(
            "openmpt_module_create_from_memory2",
            c_void_p,
            c_void_p, c_size_t,          # filedata, filesize
            c_void_p, c_void_p,          # logfunc, loguser
            c_void_p, c_void_p,          # errfunc, erruser
            ctypes.POINTER(c_int),       # error
            ctypes.POINTER(c_char_p),    # error_message
            ctypes.POINTER(_InitialCtl),  # ctls
        )
        self._module_destroy = sig("openmpt_module_destroy", None, c_void_p)

        self._error_func_store = sig("openmpt_error_func_store", c_int, c_int, c_void_p)
        self._error_string = sig("openmpt_error_string", c_char_p, c_int)
        self._module_error_get_last = sig("openmpt_module_error_get_last", c_int, c_void_p)
        self._module_error_get_last_message = sig(
            "openmpt_module_error_get_last_message", c_void_p, c_void_p
        )
        self._module_error_clear = sig("openmpt_module_error_clear", None, c_void_p)

        self._read_interleaved_float_stereo = sig(
            "openmpt_module_read_interleaved_float_stereo",
            c_size_t,
            c_void_p, c_int32, c_size_t,
            ctypes.POINTER(c_float),
        )
        self._read_float_stereo = sig(
            "openmpt_module_read_float_stereo",
            c_size_t,
            c_void_p, c_int32, c_size_t,
            ctypes.POINTER(c_float), ctypes.POINTER(c_float),
        )

        self._get_metadata_keys = sig("openmpt_module_get_metadata_keys", c_void_p, c_void_p)
        self._get_metadata = sig("openmpt_module_get_metadata", c_void_p, c_void_p, c_char_p)

        self._get_duration_seconds = sig("openmpt_module_get_duration_seconds", c_double, c_void_p)
        self._get_position_seconds = sig("openmpt_module_get_position_seconds", c_double, c_void_p)
        self._set_position_seconds = sig(
            "openmpt_module_set_position_seconds", c_double, c_void_p, c_double
        )
        self._set_position_order_row = sig(
            "openmpt_module_set_position_order_row", c_double, c_void_p, c_int32, c_int32
        )
        self._get_current_speed = sig("openmpt_module_get_current_speed", c_int32, c_void_p)
        self._get_current_tempo = sig("openmpt_module_get_current_tempo", c_int32, c_void_p)
        self._get_current_tempo2 = sig("openmpt_module_get_current_tempo2", c_double, c_void_p)
        self._get_current_order = sig("openmpt_module_get_current_order", c_int32, c_void_p)
        self._get_current_pattern = sig("openmpt_module_get_current_pattern", c_int32, c_void_p)
        self._get_current_row = sig("openmpt_module_get_current_row", c_int32, c_void_p)
        self._get_current_playing_channels = sig(
            "openmpt_module_get_current_playing_channels", c_int32, c_void_p
        )
        self._get_channel_vu_mono = sig(
            "openmpt_module_get_current_channel_vu_mono", c_float, c_void_p, c_int32
        )
        self._get_channel_vu_left = sig(
            "openmpt_module_get_current_channel_vu_left", c_float, c_void_p, c_int32
        )
        self._get_channel_vu_right = sig(
            "openmpt_module_get_current_channel_vu_right", c_float, c_void_p, c_int32
        )

        self._get_num_subsongs = sig("openmpt_module_get_num_subsongs", c_int32, c_void_p)
        self._get_num_channels = sig("openmpt_module_get_num_channels", c_int32, c_void_p)
        self._get_num_orders = sig("openmpt_module_get_num_orders", c_int32, c_void_p)
        self._get_num_patterns = sig("openmpt_module_get_num_patterns", c_int32, c_void_p)
        self._get_num_instruments = sig("openmpt_module_get_num_instruments", c_int32, c_void_p)
        self._get_num_samples = sig("openmpt_module_get_num_samples", c_int32, c_void_p)
        self._get_instrument_name = sig_opt(
            "openmpt_module_get_instrument_name", c_void_p, c_void_p, c_int32)
        self._get_sample_name = sig_opt(
            "openmpt_module_get_sample_name", c_void_p, c_void_p, c_int32)
        self._get_subsong_names = sig_opt("openmpt_module_get_subsong_names", c_void_p, c_void_p)
        self._select_subsong = sig("openmpt_module_select_subsong", c_int, c_void_p, c_int32)

        # pattern reading (the tracker view): order list + formatted cells
        self._get_order_pattern = sig(
            "openmpt_module_get_order_pattern", c_int32, c_void_p, c_int32)
        self._get_pattern_num_rows = sig(
            "openmpt_module_get_pattern_num_rows", c_int32, c_void_p, c_int32)
        self._format_pattern_row_channel_command = sig(
            "openmpt_module_format_pattern_row_channel_command",
            c_void_p, c_void_p, c_int32, c_int32, c_int32, c_int,
        )
        self._highlight_pattern_row_channel = sig_opt(
            "openmpt_module_highlight_pattern_row_channel",
            None, c_void_p, c_int32, c_int32, c_int32, c_void_p, c_int32,
        )

        self._set_repeat_count = sig("openmpt_module_set_repeat_count", c_int, c_void_p, c_int32)
        self._get_repeat_count = sig("openmpt_module_get_repeat_count", c_int32, c_void_p)

        self._ctl_set_text = sig("openmpt_module_ctl_set_text", c_int, c_void_p, c_char_p, c_char_p)
        self._ctl_set_floatingpoint = sig(
            "openmpt_module_ctl_set_floatingpoint", c_int, c_void_p, c_char_p, c_double
        )
        self._ctl_set_integer = sig(
            "openmpt_module_ctl_set_integer", c_int, c_void_p, c_char_p, c_int32
        )
        self._ctl_set_boolean = sig(
            "openmpt_module_ctl_set_boolean", c_int, c_void_p, c_char_p, c_int
        )

        # render parameters (the mixer's own settings, interpolation lives here)
        self._set_render_param = sig_opt(
            "openmpt_module_set_render_param", c_int, c_void_p, c_int, c_int32
        )
        self._get_render_param = sig_opt(
            "openmpt_module_get_render_param", c_int, c_void_p, c_int,
            ctypes.POINTER(c_int32),
        )

        # log callback: collect messages instead of spamming stderr
        self.log_lines: list[str] = []
        self._log_lock = threading.Lock()

        @LOG_FUNC
        def _log_cb(message, _user):  # pragma: no cover - C callback
            try:
                text = (message or b"").decode("utf-8", "replace").rstrip()
            except Exception:
                return
            if text:
                with self._log_lock:
                    self.log_lines.append(text)
                    if len(self.log_lines) > 500:
                        del self.log_lines[:250]

        self._log_cb = _log_cb  # keep alive
        self._err_func = ctypes.cast(self._error_func_store, ctypes.c_void_p)

        v = self._get_library_version()
        self.version = (v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)
        self.version_string = "%d.%d.%d%s" % (
            self.version[0],
            self.version[1],
            self.version[2],
            "-pre.%d" % self.version[3] if self.version[3] else "",
        )

    def is_extension_supported(self, ext: str) -> bool:
        return bool(self._is_extension_supported(ext.lstrip(".").lower().encode()))

    def _take_string(self, ptr) -> str:
        """Read + free a string returned by libopenmpt."""
        if not ptr:
            return ""
        try:
            return ctypes.string_at(ptr).decode("utf-8", "replace")
        finally:
            self._free_string(ptr)

    def drain_log(self) -> list[str]:
        with self._log_lock:
            lines, self.log_lines = self.log_lines, []
        return lines

    def error_string(self, code: int) -> str:
        return (self._error_string(code) or b"?").decode("utf-8", "replace")

    def open_module(self, data: bytes | bytearray | memoryview, ctls=None) -> "Module":
        """Create a Module from an in-memory buffer."""
        buf = (ctypes.c_char * len(data)).from_buffer_copy(bytes(data)) if not isinstance(
            data, (ctypes.Array,)
        ) else data
        ctl_array = None
        if ctls:
            ctl_array = (_InitialCtl * (len(ctls) + 1))()
            for i, (k, v) in enumerate(ctls.items()):
                ctl_array[i].ctl = str(k).encode()
                ctl_array[i].value = str(v).encode()
            ctl_array[len(ctls)] = _InitialCtl(None, None)
        err = ctypes.c_int(0)
        errmsg = ctypes.c_char_p(None)
        handle = self._module_create_from_memory2(
            ctypes.cast(buf, ctypes.c_void_p),
            len(data),
            self._log_cb,
            None,
            self._err_func,
            None,
            ctypes.byref(err),
            ctypes.byref(errmsg),
            ctl_array if ctl_array is not None else None,
        )
        message = (errmsg.value or b"").decode("utf-8", "replace") if errmsg.value else ""
        if not handle:
            if not message:
                message = self.error_string(err.value)
            raise OpenMPTError(message or "libopenmpt failed to open the module")
        mod = Module(self, handle)
        # keep the input buffer alive for the lifetime of the module
        mod._keepalive = buf
        return mod

    def open_file(self, path: str, ctls=None) -> "Module":
        with open(path, "rb") as fh:
            data = fh.read()
        return self.open_module(data, ctls=ctls)

CMD_NOTE, CMD_INSTRUMENT, CMD_VOLUME_EFFECT, CMD_EFFECT, CMD_VOLUME, CMD_PARAMETER = range(6)

@dataclass


class ModuleInfo:
    path: str = ""
    title: str = ""
    format: str = ""
    format_long: str = ""
    tracker: str = ""
    artist: str = ""
    type: str = ""
    duration: float = 0.0
    num_channels: int = 0
    num_orders: int = 0
    num_patterns: int = 0
    num_instruments: int = 0
    num_samples: int = 0
    num_subsongs: int = 0
    subsong_names: list[str] = field(default_factory=list)
    sample_names: list[str] = field(default_factory=list)
    instrument_names: list[str] = field(default_factory=list)
    message: str = ""
    extra: dict = field(default_factory=dict)

    @property
    def duration_valid(self) -> bool:
        import math

        return math.isfinite(self.duration) and self.duration > 0


class Module:
    """Owns an openmpt_module * handle.  Thread-affine: use from one thread."""

    def __init__(self, lib: LibOpenMPT, handle):
        self._lib = lib
        self._h = handle
        self._closed = False
        self._keepalive = None

    @property
    def handle(self):
        if self._closed:
            raise OpenMPTError("Module already closed")
        return self._h

    def close(self) -> None:
        if not self._closed:
            self._closed = True
            try:
                self._lib._module_destroy(self._h)
            finally:
                self._h = None
                self._keepalive = None

    def __enter__(self) -> "Module":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self):  # pragma: no cover
        try:
            self.close()
        except Exception:
            pass

    def last_error(self) -> Optional[str]:
        code = self._lib._module_error_get_last(self.handle)
        if not code:
            return None
        msg = self._lib._take_string(self._lib._module_error_get_last_message(self.handle))
        self._lib._module_error_clear(self.handle)
        return msg or self._lib.error_string(code)

    def metadata(self) -> dict:
        keys = self._lib._take_string(self._lib._get_metadata_keys(self.handle))
        out = {}
        for key in filter(None, keys.split(";")):
            out[key] = self._lib._take_string(self._lib._get_metadata(self.handle, key.encode()))
        return out

    def info(self, path: str = "") -> ModuleInfo:
        md = self.metadata()
        names = ""
        if self._lib._get_subsong_names is not None:
            try:
                names = self._lib._take_string(self._lib._get_subsong_names(self.handle))
            except Exception:
                names = ""
        return ModuleInfo(
            path=path,
            title=md.get("title", "") or (os.path.basename(path) if path else ""),
            format=md.get("type", ""),
            format_long=md.get("type_long", ""),
            tracker=md.get("tracker", ""),
            artist=md.get("artist", ""),
            type=md.get("type", ""),
            duration=self.duration(),
            num_channels=self.num_channels(),
            num_orders=self.num_orders(),
            num_patterns=self.num_patterns(),
            num_instruments=self.num_instruments(),
            num_samples=self.num_samples(),
            num_subsongs=self.num_subsongs(),
            subsong_names=list(filter(None, names.split(";"))),
            sample_names=self.sample_names(),
            instrument_names=self.instrument_names(),
            message=md.get("message", "") or md.get("message_raw", ""),
            extra=md,
        )

    def duration(self) -> float:
        return float(self._lib._get_duration_seconds(self.handle))

    def position_seconds(self) -> float:
        return float(self._lib._get_position_seconds(self.handle))

    def seek_seconds(self, seconds: float) -> float:
        return float(self._lib._set_position_seconds(self.handle, float(max(0.0, seconds))))

    def seek_order_row(self, order: int, row: int) -> float:
        return float(self._lib._set_position_order_row(self.handle, int(order), int(row)))

    def set_repeat_count(self, count: int) -> bool:
        return bool(self._lib._set_repeat_count(self.handle, int(count)))

    def repeat_count(self) -> int:
        return int(self._lib._get_repeat_count(self.handle))

    def set_at_end(self, mode: str) -> bool:
        """mode: 'fadeout' | 'continue' | 'stop'"""
        return bool(self._lib._ctl_set_text(self.handle, b"play.at_end", mode.encode()))

    def set_interpolation(self, mode) -> bool:
        """Set the resampling filter, raise OpenMPTError for unknown filters or an unavailable API."""
        length = interpolation_length(mode)
        if length is None:
            raise OpenMPTError(f"Unknown interpolation filter {mode!r}")
        setter = self._lib._set_render_param
        if setter is None:
            raise OpenMPTError("This libopenmpt has no openmpt_module_set_render_param")
        if not setter(self.handle, RENDER_INTERPOLATIONFILTER_LENGTH, length):
            raise OpenMPTError(f"libopenmpt rejected interpolation filter {length}")
        return True

    def interpolation(self) -> int:
        """The filter length the mixer is really using (0: not reported)."""
        getter = self._lib._get_render_param
        if getter is None:
            return 0
        value = ctypes.c_int32(0)
        if not getter(self.handle, RENDER_INTERPOLATIONFILTER_LENGTH, ctypes.byref(value)):
            return 0
        return int(value.value)

    def set_tempo_factor(self, factor: float) -> bool:
        return bool(
            self._lib._ctl_set_floatingpoint(self.handle, b"play.tempo_factor", float(factor))
        )

    @staticmethod
    def _float_ptr(buf):
        """Accept numpy arrays, ctypes arrays or raw pointers."""
        if hasattr(buf, "ctypes"):          # numpy ndarray
            return buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        if isinstance(buf, ctypes.Array):
            return ctypes.cast(buf, ctypes.POINTER(ctypes.c_float))
        return buf

    def read_interleaved_float(self, samplerate: int, frames: int, buf) -> int:
        """Render frames stereo frames into buf (interleaved float32)."""
        if frames <= 0:
            return 0
        # guard rail: never let libopenmpt write past the caller's buffer
        capacity = getattr(buf, "shape", None)
        if capacity is not None and len(capacity) >= 1 and int(capacity[0]) < int(frames):
            raise ValueError(f"Buffer holds {capacity[0]} frames, {frames} requested")
        return int(
            self._lib._read_interleaved_float_stereo(
                self.handle, int(samplerate), int(frames), self._float_ptr(buf)
            )
        )

    def read_planar_float(self, samplerate: int, frames: int, left, right) -> int:
        return int(
            self._lib._read_float_stereo(
                self.handle, int(samplerate), int(frames),
                self._float_ptr(left), self._float_ptr(right),
            )
        )

    def current_order(self) -> int:
        return int(self._lib._get_current_order(self.handle))

    def current_pattern(self) -> int:
        return int(self._lib._get_current_pattern(self.handle))

    def current_row(self) -> int:
        return int(self._lib._get_current_row(self.handle))

    def current_speed(self) -> int:
        return int(self._lib._get_current_speed(self.handle))

    def current_tempo(self) -> int:
        return int(self._lib._get_current_tempo(self.handle))

    def current_tempo2(self) -> float:
        return float(self._lib._get_current_tempo2(self.handle))

    def playing_channels(self) -> int:
        return int(self._lib._get_current_playing_channels(self.handle))

    def channel_vu(self, channel: int) -> float:
        return float(self._lib._get_channel_vu_mono(self.handle, int(channel)))

    def channel_vu_stereo(self, channel: int) -> tuple[float, float]:
        return (
            float(self._lib._get_channel_vu_left(self.handle, int(channel))),
            float(self._lib._get_channel_vu_right(self.handle, int(channel))),
        )

    def num_channels(self) -> int:
        return int(self._lib._get_num_channels(self.handle))

    def num_orders(self) -> int:
        return int(self._lib._get_num_orders(self.handle))

    def num_patterns(self) -> int:
        return int(self._lib._get_num_patterns(self.handle))

    def num_instruments(self) -> int:
        return int(self._lib._get_num_instruments(self.handle))

    def num_samples(self) -> int:
        return int(self._lib._get_num_samples(self.handle))

    def num_subsongs(self) -> int:
        return int(self._lib._get_num_subsongs(self.handle))

    def _name_list(self, getter, count: int) -> list[str]:
        """Read one name per slot, retaining empty slots. Return no names if the getter or handle
        is unavailable."""
        if getter is None or self._closed:
            return []
        out = []
        for index in range(max(0, int(count))):
            out.append(self._lib._take_string(getter(self.handle, index)))
        return out

    def sample_names(self) -> list[str]:
        """Return sample names in slot order. In MOD files these often also form the song comment."""
        return self._name_list(self._lib._get_sample_name, self.num_samples())

    def instrument_names(self) -> list[str]:
        """The instrument names, in slot order (empty for sample-based formats)."""
        return self._name_list(self._lib._get_instrument_name, self.num_instruments())

    def order_pattern(self, order: int) -> int:
        """The pattern at order, or -1 when it is a skip/separator entry."""
        return int(self._lib._get_order_pattern(self.handle, int(order)))

    def order_list(self, limit: int = 4096) -> list[int]:
        """Every order entry (pattern index, or -1 for "+++"/"---" separators)."""
        return [self.order_pattern(index)
                for index in range(min(self.num_orders(), int(limit)))]

    def pattern_rows(self, pattern: int) -> int:
        """How many rows the pattern has (0 when unknown)."""
        return int(self._lib._get_pattern_num_rows(self.handle, int(pattern)))

    def _command(self, pattern: int, row: int, channel: int, command: int) -> str:
        ptr = self._lib._format_pattern_row_channel_command(
            self.handle, int(pattern), int(row), int(channel), int(command))
        return self._lib._take_string(ptr)

    def pattern_cell(self, pattern: int, row: int, channel: int) -> tuple[str, str, str, str]:
        """Return formatted (note, instrument, volume, effect) strings for a pattern cell."""
        note = self._command(pattern, row, channel, CMD_NOTE)
        instrument = self._command(pattern, row, channel, CMD_INSTRUMENT)
        vol_effect = self._command(pattern, row, channel, CMD_VOLUME_EFFECT)
        volume = self._command(pattern, row, channel, CMD_VOLUME)
        effect = self._command(pattern, row, channel, CMD_EFFECT)
        parameter = self._command(pattern, row, channel, CMD_PARAMETER)
        return (note, instrument, _join_volume(vol_effect, volume), _join_effect(effect, parameter))

    def pattern_data(self, pattern: int, channels: Optional[int] = None) -> dict:
        """Read a pattern on the render thread, return its index, dimensions, and formatted cells."""
        rows = self.pattern_rows(pattern)
        count = int(channels if channels is not None else self.num_channels())
        cells = [[self.pattern_cell(pattern, row, channel) for channel in range(count)]
                 for row in range(rows)]
        return {"pattern": int(pattern), "rows": rows, "channels": count, "cells": cells}


def _join_volume(letter: str, value: str) -> str:
    """Combine the volume command and value, treating dots as empty fields."""
    letter = (letter or "").strip()
    value = (value or "").strip()
    letter_empty = not letter or letter.startswith(".")
    value_empty = not value or value.startswith(".")
    if letter_empty and value_empty:
        return ""
    if letter_empty:
        letter = "v"                       # formats without a volume column
    if value_empty:
        value = ".."
    return f"{letter}{value}"


def _join_effect(letter: str, parameter: str) -> str:
    """The effect column as one string ("A0F"), empty when the cell has none."""
    letter = (letter or "").strip()
    parameter = (parameter or "").strip()
    if not letter or letter.startswith("."):
        return ""
    if parameter.startswith("."):
        parameter = "00"
    return f"{letter}{parameter or '00'}"

_lib_singleton: Optional[LibOpenMPT] = None
_lib_lock = threading.Lock()


def get_lib(path: Optional[str] = None) -> LibOpenMPT:
    """Return the process-wide LibOpenMPT instance (loaded once)."""
    global _lib_singleton
    with _lib_lock:
        if _lib_singleton is None:
            _lib_singleton = LibOpenMPT(path)
        return _lib_singleton

_KNOWN_EXTENSIONS = (
    "mod s3m xm it mptm stm nst m15 ulm mtm itp 669 psm psm16 amf ams dsym "
    "dmf dsm far imf j2b md? j2b mms okt plm umx wow xmf "
    "c67 cba digi dtm gdm ice imf it lst mdl med mmcmp mo3 mpm mt2 mus nst "
    "okt pat pt36 ptm puma sfx sfx2 st26 stp ult wow uax"
).split()


def supported_extensions(lib: Optional[LibOpenMPT] = None) -> set[str]:
    lib = lib or get_lib()
    out = set()
    for ext in _KNOWN_EXTENSIONS:
        if ext.isalnum() or "." in ext:
            try:
                if lib.is_extension_supported(ext):
                    out.add(ext)
            except Exception:
                pass
    return out
