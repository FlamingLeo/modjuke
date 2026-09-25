"""
modjuke entry point.

    modjuke                     # open the player, restore the last library
    modjuke ~/mods              # open the player with a library
    modjuke ~/mods --autoplay   # ... and start playing immediately
    modjuke --scan ~/mods       # headless: list what would be queued (no GUI)
    modjuke --check             # show which audio backends work on this machine
    modjuke --theme amber       # start with a different colour scheme
"""

from __future__ import annotations

import argparse
import os
import sys

from . import __version__
from .config import APP_TITLE
from .theme import THEME_NAMES
from .openmpt import INTERPOLATION_FILTERS


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="modjuke", description=APP_TITLE,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("directory", nargs="?", default="",
                        help="folder with tracker modules (scanned recursively)")
    parser.add_argument("--track", metavar="FILE", default="",
                        help="play this module on startup")
    parser.add_argument("--backend", choices=("auto", "sounddevice", "soundcard", "null"),
                        default=None, help="audio output backend (default: from the settings)")
    parser.add_argument("--volume", type=int, default=None, metavar="0..100")
    parser.add_argument("--speed", type=float, default=1.0, metavar="N",
                        help="play N times faster than real time (testing/feature-demo)")
    parser.add_argument("--interpolation", choices=tuple(INTERPOLATION_FILTERS), default=None,
                        help="libopenmpt mixer resampling filter: off (no interpolation), "
                             "linear, cubic or sinc (default: from the settings)")
    parser.add_argument("--autoplay", action="store_true", help="start playing right away")
    parser.add_argument("--theme", choices=THEME_NAMES, default=None, metavar="NAME",
                        help="colour scheme: " + ", ".join(THEME_NAMES)
                             + " (default: from the settings)")
    parser.add_argument("--scan", metavar="DIR", default="",
                        help="headless: scan DIR and print the queue, then exit")
    parser.add_argument("--order", choices=("alphabetical", "by directory", "shuffle"),
                        default="by directory", help="queue order used by --scan")
    parser.add_argument("--analyze", action="store_true",
                        help="with --scan: read the metadata of every module as well")
    parser.add_argument("--check", action="store_true",
                        help="report which audio backends are available, then exit")
    parser.add_argument("--version", action="version", version=f"modjuke {__version__}")
    return parser


def _setup_environment() -> None:
    """Make sure ~/.local/bin style installs find everything."""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if here not in sys.path:
        sys.path.insert(0, here)


def scan_and_report(directory: str, order: str = "by directory", analyze: bool = False) -> int:
    """Headless listing: shows exactly which files the player would queue."""
    from .library import format_time, order_tracks, scan_library
    from .openmpt import get_lib, OpenMPTError

    try:
        lib = get_lib()
    except OpenMPTError as exc:
        print(exc, file=sys.stderr)
        return 2
    print(f"libopenmpt {lib.version_string}")
    result = scan_library(directory)
    print(f"{result.root}: {len(result.tracks)} modules in {result.dirs} folders"
          + (f", {len(result.errors)} warnings" if result.errors else ""))
    for warning in result.errors[:10]:
        print(f"  ! {warning}")
    if analyze:
        print("  (reading metadata of every module, this opens all files)")
    for i, track in enumerate(order_tracks(result.tracks, order)):
        line = f"  {i + 1:>4}. {os.path.join(track.rel_dir, track.name) if track.rel_dir else track.name}"
        if analyze:
            try:
                with lib.open_file(track.path) as module:
                    info = module.info(track.path)
                line += (f"   [{info.format or '?'}, {info.num_channels}ch, "
                         f"{format_time(info.duration)}]")
            except Exception as exc:
                line += f"   [UNREADABLE: {str(exc)[:40]}]"
        print(line)
    return 0


def check_backends() -> int:
    from .audio import create_output
    from .openmpt import OpenMPTError, get_lib

    try:
        lib = get_lib()
        from .openmpt import supported_extensions
        print(f"libopenmpt      : {lib.version_string} ({lib._library._name})")
        print(f"formats         : {len(supported_extensions())} playable extensions")
    except OpenMPTError as exc:
        print(f"libopenmpt      : NOT FOUND\n{exc}", file=sys.stderr)
        return 2
    for name in ("sounddevice", "soundcard", "null"):
        try:
            out, _ring = create_output(prefer=name, samplerate=None, blocksize=1024)
            print(f"{name:<15} : ok  ({out.samplerate} Hz, {out.description})")
            out.stop()
        except Exception as exc:
            print(f"{name:<15} : unavailable ({exc})")
    print("\nTk front-end    : "
          + ("available" if _tk_available() else "NOT available (install python3-tk)"))
    return 0


def _tk_available() -> bool:
    """Tkinter present and importable?  (checked without importing it)"""
    import importlib.util
    try:
        return importlib.util.find_spec("tkinter") is not None
    except Exception:
        return False


def main(argv: list[str] | None = None) -> int:
    _setup_environment()
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.check:
        return check_backends()
    if args.scan:
        return scan_and_report(args.scan, args.order, args.analyze)

    if not _tk_available():
        print("Tkinter is not available - install python3-tk (or run with --scan).",
              file=sys.stderr)
        return 2

    from .ui import run
    if args.speed != 1.0:
        print(f"note: playing at {args.speed}x speed", file=sys.stderr)
    if args.volume is not None and not 0 <= args.volume <= 100:
        print(f"note: --volume is 0..100, using {max(0, min(100, args.volume))}",
              file=sys.stderr)
    return run(args)

if __name__ == "__main__":
    sys.exit(main())
