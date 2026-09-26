#!/usr/bin/env python3
"""Per-user Linux installation. Tk and native audio libraries are checked, never bundled."""
from __future__ import annotations

import argparse
import contextlib
import ctypes
import ctypes.util
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import venv

FORMAT = "modjuke-user-install-v1"
RELEASE = re.compile(r"release-[a-z0-9_]+\Z")
SYSTEM_HELP = """Install the system dependencies with your distribution's package manager.
Linux Mint 22 / Ubuntu 24.04:
  sudo apt install python3-venv python3-tk libopenmpt0t64 libportaudio2 libpulse0 libasound2-plugins
Older Debian/Ubuntu releases may call the libopenmpt package libopenmpt0.
The installer itself must be run WITHOUT sudo."""


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def safe_path(path) -> Path:
    path = Path(path).expanduser()
    if not path.is_absolute() or any(ord(c) < 32 for c in str(path)):
        raise RuntimeError(f"Expected an absolute path without control characters: {path!s}")
    return path


def app_root() -> Path:
    if Path(__file__).name == "manage.py":
        return Path(__file__).resolve().parent
    data = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local/share")
    return safe_path(data).resolve() / "modjuke"


def atomic_write(path: Path, data: bytes, mode=0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".modjuke-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def switch_current(root: Path, target: str) -> None:
    fd, tmp = tempfile.mkstemp(prefix=".current-", dir=root)
    os.close(fd)
    os.unlink(tmp)
    try:
        os.symlink(target, tmp)
        os.replace(tmp, root / "current")
    finally:
        if os.path.lexists(tmp):
            os.unlink(tmp)


def exports(root: Path, bin_dir: Path) -> dict[str, Path]:
    return {
        "launcher": bin_dir / "modjuke",
        "uninstaller": bin_dir / "modjuke-uninstall",
        "desktop": root.parent / "applications/modjuke.desktop",
        "icon": root.parent / "icons/modjuke.png",
    }


def managed_icon(root: Path, manifest):
    """Find the old icon from the verified desktop entry when upgrading its location."""
    if not manifest:
        return None
    desktop = exports(root, Path(manifest["bin_dir"]))["desktop"]
    try:
        if (desktop.is_symlink() or not desktop.is_file()
                or digest(desktop.read_bytes()) != manifest["files"]["desktop"]):
            return None
        value = next(line[5:] for line in desktop.read_text(encoding="utf-8").splitlines()
                     if line.startswith("Icon="))
        icon = safe_path(value.replace("\\\\", "\\"))
        if icon.is_symlink() or not icon.resolve().is_relative_to((root.parent / "icons").resolve()):
            return None
        return icon
    except (OSError, ValueError, RuntimeError, StopIteration):
        return None


def read_manifest(root: Path):
    if root.is_symlink():
        raise RuntimeError(f"Refusing a symlink at the installation directory: {root}")
    manifest = root / "installation.json"
    if not manifest.exists():
        if root.exists() and any(root.iterdir()):
            raise RuntimeError(f"Not an installer-owned directory: {root}")
        return None
    if manifest.is_symlink():
        raise RuntimeError("Refusing a symlinked installation manifest.")
    data = json.loads(manifest.read_text(encoding="utf-8"))
    if data.get("format") != FORMAT or data.get("root") != str(root):
        raise RuntimeError("Unrecognized installation manifest, nothing was removed.")
    safe_path(data["bin_dir"])
    if set(data.get("files", {})) != set(exports(root, Path(data["bin_dir"]))):
        raise RuntimeError("Incomplete installation manifest.")
    return data


def owned_release(path: Path) -> bool:
    if path.is_symlink() or not path.is_dir() or not RELEASE.fullmatch(path.name):
        return False
    marker = path / ".modjuke-release"
    return not marker.is_symlink() and marker.is_file() and marker.read_text(errors="replace") == FORMAT


def current_target(root: Path):
    current = root / "current"
    if not os.path.lexists(current):
        return None
    if not current.is_symlink():
        raise RuntimeError("The installation's current entry is not a managed symlink.")
    target = os.readlink(current)
    if Path(target).parent != Path("releases") or not owned_release(root / target):
        raise RuntimeError("The current installation points outside a managed release.")
    return target


@contextlib.contextmanager
def install_lock(root: Path):
    import fcntl
    root.parent.mkdir(parents=True, exist_ok=True)
    with (root.parent / ".modjuke-install.lock").open("a") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("Another modjuke install or uninstall is running.") from None
        yield


def native_check() -> str:
    problems = []
    try:
        import tkinter  # noqa: F401 - no display or Tk window is needed here
    except ImportError:
        problems.append("Tkinter is unavailable for this Python interpreter.")
    if importlib.util.find_spec("ensurepip") is None:
        problems.append("Python's venv/ensurepip support is unavailable.")
    override = os.environ.get("MODJUKE_LIBOPENMPT", "")
    if override and "/" in override:
        override = str(Path(override).expanduser().resolve())
    candidates = [override, ctypes.util.find_library("openmpt"), "libopenmpt.so.0", "libopenmpt.so"]
    found = False
    chosen_override = ""
    for candidate in filter(None, candidates):
        try:
            ctypes.CDLL(candidate)
            found = True
            if candidate == override:
                chosen_override = override
            break
        except OSError:
            pass
    if not found:
        problems.append("The libopenmpt shared library could not be loaded.")
    try:
        ctypes.CDLL(ctypes.util.find_library("portaudio") or "libportaudio.so.2")
    except OSError:
        problems.append("The PortAudio shared library could not be loaded.")
    if problems:
        raise RuntimeError("\n".join(problems) + "\n\n" + SYSTEM_HELP)
    print("System Python, Tkinter, libopenmpt and PortAudio found.", flush=True)
    return chosen_override


def desktop_exec(path: Path) -> str:
    # Desktop files have string escaping followed by Exec argument escaping.
    quoted = str(path).replace("%", "%%")
    for char in ("\\", '"', "`", "$"):
        quoted = quoted.replace(char, "\\" + char)
    return '"' + quoted.replace("\\", "\\\\") + '"'


def desktop_value(path: Path) -> str:
    return str(path).replace("\\", "\\\\")


def install(root: Path) -> None:
    source = Path(__file__).resolve().parent
    for item in ("pyproject.toml", "modjuke/__main__.py", "img/modjuke.png"):
        if not (source / item).is_file():
            raise RuntimeError(f"Missing release file: {item}. Extract the complete ZIP first.")
    override = native_check()
    previous = read_manifest(root)
    bin_dir = Path(previous["bin_dir"]) if previous else (Path.home() / ".local/bin").resolve()
    safe_path(bin_dir)
    paths = exports(root, bin_dir)
    old_target = current_target(root)
    old_icon = managed_icon(root, previous)
    for key, path in paths.items():
        if os.path.lexists(path):
            if path.is_symlink() or not path.is_file() or not previous or digest(path.read_bytes()) != previous["files"][key]:
                raise RuntimeError(f"Refusing to overwrite an unrelated or modified file: {path}")
    if (root / "releases").is_symlink():
        raise RuntimeError("Refusing a symlinked releases directory.")
    root.mkdir(parents=True, exist_ok=True)
    releases = root / "releases"
    releases.mkdir(exist_ok=True)
    release = Path(tempfile.mkdtemp(prefix="release-", dir=releases))
    (release / ".modjuke-release").write_text(FORMAT)
    committed = False
    backups = {}
    try:
        environment = release / ".venv"
        print("Creating an isolated Python environment (no sudo).", flush=True)
        venv.EnvBuilder(with_pip=True).create(environment)
        python = environment / "bin/python"
        # Build from a copy: leave the downloaded source tree clean and removable.
        build_source = release / "source"
        shutil.copytree(source / "modjuke", build_source / "modjuke",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        shutil.copy2(source / "pyproject.toml", build_source / "pyproject.toml")
        shutil.copytree(source / "img", build_source / "img")
        print("Installing Python dependencies with pip, network access may be required.", flush=True)
        pip_env = os.environ.copy()
        for variable in ("PIP_TARGET", "PIP_PREFIX", "PIP_USER"):
            pip_env.pop(variable, None)
        pip_env["PIP_CONFIG_FILE"] = os.devnull
        subprocess.run([str(python), "-I", "-m", "pip", "install", "--disable-pip-version-check",
                        "--no-input", str(build_source) + "[audio]"], check=True, env=pip_env)
        subprocess.run([str(python), "-I", "-c",
                        "import tkinter, numpy, importlib.util, "
                        "from modjuke.openmpt import get_lib, "
                        "assert importlib.util.find_spec('sounddevice'), "
                        "assert importlib.util.find_spec('soundcard'), "
                        "print('Installed modjuke, libopenmpt', get_lib().version_string)"], check=True)
        shutil.rmtree(build_source)
        launcher = "#!/bin/sh\n# Managed by modjuke's per-user installer.\n"
        if override:
            launcher += "if [ -z \"${MODJUKE_LIBOPENMPT:-}\" ], then\n  export MODJUKE_LIBOPENMPT=" + shlex.quote(override) + "\nfi\n"
        launcher += "exec " + shlex.quote(str(root / "current/.venv/bin/python")) + ' -I -m modjuke "$@"\n'
        base_python = str(Path(getattr(sys, "_base_executable", sys.executable)).resolve())
        uninstaller = "#!/bin/sh\n# Managed by modjuke's per-user installer.\nexec " + shlex.quote(base_python) + " -I " + shlex.quote(str(root / "manage.py")) + ' --uninstall "$@"\n'
        desktop = ("[Desktop Entry]\nType=Application\nVersion=1.0\nName=modjuke\n"
                   "GenericName=Tracker Music Player\nComment=Play tracker modules and manage playlists\n"
                   f"Exec=/usr/bin/env {desktop_exec(paths['launcher'])}\n"
                   f"Icon={desktop_value(paths['icon'])}\n"
                   "Terminal=false\nCategories=AudioVideo,Audio,Player,\n"
                   "Keywords=tracker,module,MOD,XM,S3M,IT,music,\nStartupNotify=false\n")
        data = {"launcher": launcher.encode(), "uninstaller": uninstaller.encode(),
                "desktop": desktop.encode(), "icon": (source / "img/modjuke.png").read_bytes()}
        manifest = {"format": FORMAT, "root": str(root), "bin_dir": str(bin_dir),
                    "files": {k: digest(value) for k, value in data.items()}}
        writes = {paths[k]: (value, 0o755 if k in ("launcher", "uninstaller") else 0o644)
                  for k, value in data.items()}
        writes[root / "manage.py"] = (Path(__file__).read_bytes(), 0o644)
        writes[root / "installation.json"] = ((json.dumps(manifest, indent=2) + "\n").encode(), 0o600)
        for path in writes:
            backups[path] = (path.read_bytes(), path.stat().st_mode & 0o777) if path.exists() else None
        for path, (payload, mode) in writes.items():
            atomic_write(path, payload, mode)
        switch_current(root, str(release.relative_to(root)))
        committed = True
    finally:
        if not committed:
            for path, backup in backups.items():
                if backup is None:
                    path.unlink(missing_ok=True)
                else:
                    atomic_write(path, *backup)
            shutil.rmtree(release)
            if not previous:
                for directory in (releases, root):
                    try:
                        directory.rmdir()
                    except OSError:
                        pass
    if old_icon is not None and old_icon != paths["icon"]:
        try:
            if old_icon.is_file() and digest(old_icon.read_bytes()) == previous["files"]["icon"]:
                old_icon.unlink()
            elif os.path.lexists(old_icon):
                print(f"Kept modified old icon: {old_icon}")
        except OSError as exc:
            print(f"Note: could not clean the old icon: {exc}", file=sys.stderr)
    # Keep the immediately previous release, close the app before upgrading.
    keep = {release.name, Path(old_target).name if old_target else ""}
    for candidate in releases.iterdir():
        if candidate.name not in keep and owned_release(candidate):
            try:
                shutil.rmtree(candidate)
            except OSError as exc:
                print(f"Note: could not clean an older release: {exc}", file=sys.stderr)
    refresh_desktop(root)
    print(f"\nInstalled. Open modjuke from your application menu or run:\n  {shlex.quote(str(paths['launcher']))}")
    if str(bin_dir) not in os.environ.get("PATH", "").split(os.pathsep):
        print(f"\nFor the short command, add this to your shell profile:\n  export PATH={shlex.quote(str(bin_dir))}:\"$PATH\"")
    print(f"\nUninstall with:\n  {shlex.quote(str(paths['uninstaller']))}\nSettings, playlists, listening history and music are left untouched.")


def refresh_desktop(root: Path) -> None:
    command = shutil.which("update-desktop-database")
    if command:
        subprocess.run([command, str(root.parent / "applications")], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def uninstall(root: Path) -> None:
    manifest = read_manifest(root)
    if manifest is None:
        print("No installer-managed modjuke installation found.")
        return
    paths = exports(root, Path(manifest["bin_dir"]))
    old_icon = managed_icon(root, manifest)
    if old_icon is not None:
        paths["icon"] = old_icon
    current_target(root)  # refuse an unexpected link before removing anything
    releases = root / "releases"
    if releases.is_symlink():
        raise RuntimeError("Refusing a symlinked releases directory.")
    for key, path in paths.items():
        if path.is_file() and not path.is_symlink() and digest(path.read_bytes()) == manifest["files"][key]:
            path.unlink()
        elif os.path.lexists(path):
            print(f"Kept modified or unrelated file: {path}")
    (root / "current").unlink(missing_ok=True)
    if releases.exists():
        for candidate in releases.iterdir():
            if owned_release(candidate):
                shutil.rmtree(candidate)
        try:
            releases.rmdir()
        except OSError:
            pass
    for name in ("manage.py", "installation.json"):
        (root / name).unlink(missing_ok=True)
    try:
        root.rmdir()
    except OSError:
        print(f"Kept unrelated files in {root}")
    refresh_desktop(root)
    print("modjuke uninstalled. Your settings, playlists, listening history and music were not removed.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Install modjuke for the current Linux user, no native libraries are bundled.")
    parser.add_argument("--uninstall", action="store_true", help="remove this installer's application files, retaining user data")
    parser.add_argument("--check", action="store_true", help="check native requirements without installing")
    args = parser.parse_args()
    if args.check and args.uninstall:
        parser.error("choose --check or --uninstall, not both")
    try:
        if not sys.platform.startswith("linux"):
            raise RuntimeError("This installer is for Linux. Use pip for other platforms.")
        if sys.version_info < (3, 9):
            raise RuntimeError("Python 3.9 or newer is required.")
        if os.geteuid() == 0:
            raise RuntimeError("Run this per-user installer without sudo or root.")
        if args.check:
            native_check()
            return 0
        root = app_root()
        with install_lock(root):
            if args.uninstall:
                uninstall(root)
            else:
                install(root)
        return 0
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"\nmodjuke installer: {exc}", file=sys.stderr)
        if isinstance(exc, subprocess.CalledProcessError):
            print("Installation failed, the previous installation has not been replaced.", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nInstallation cancelled.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
