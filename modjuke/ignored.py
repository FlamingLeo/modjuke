"""Persistent path exclusions, independent of playlist membership and Settings drafts."""

from __future__ import annotations

import json
import os
import tempfile
from typing import Optional

from .config import config_dir

MAX_IGNORED = 100000


def ignored_path(config_file: Optional[str] = None) -> str:
    directory = os.path.dirname(os.path.abspath(config_file)) if config_file else config_dir()
    return os.path.join(directory, "ignored.json")


def path_key(path: str) -> str:
    if not isinstance(path, str) or not path or "\0" in path:
        raise ValueError("An ignored song needs a valid file path")
    return os.path.normcase(os.path.abspath(os.path.expanduser(path)))


class IgnoreError(ValueError):
    """An exclusion change could not be saved safely."""


class IgnoreStore:
    def __init__(self, path: Optional[str] = None):
        self.path = path or ignored_path()
        self._paths: set[str] = set()
        self.error = ""
        self._read_error = False
        try:
            with open(self.path, encoding="utf-8") as handle:
                raw = json.load(handle)
            if (not isinstance(raw, dict) or raw.get("version") != 1
                    or not isinstance(raw.get("paths"), list)
                    or len(raw["paths"]) > MAX_IGNORED):
                raise ValueError("Invalid ignore-list format")
            if any(not isinstance(p, str) or not os.path.isabs(p) for p in raw["paths"]):
                raise ValueError("Ignored paths must be absolute file paths")
            self._paths = {path_key(p) for p in raw["paths"]}
        except FileNotFoundError:
            pass
        except (OSError, ValueError, TypeError) as exc:
            self._read_error = True
            self.error = f"Could not read ignored songs: {exc}. File kept unchanged: {self.path}"

    def __bool__(self) -> bool:
        return bool(self._paths)

    @property
    def paths(self) -> list[str]:
        return sorted(self._paths)

    def contains(self, path: str) -> bool:
        if not self._paths or not path:
            return False
        if path in self._paths:
            return True
        try:
            return path_key(path) in self._paths
        except (ValueError, TypeError, OSError):
            return False

    def change(self, *, add=(), remove=()) -> bool:
        if self._read_error:
            raise IgnoreError(self.error)
        try:
            paths = (self._paths | {path_key(p) for p in add}) - {path_key(p) for p in remove}
            if len(paths) > MAX_IGNORED:
                raise ValueError(f"The ignore list is limited to {MAX_IGNORED} songs")
        except (ValueError, TypeError) as exc:
            raise IgnoreError(str(exc)) from exc
        if paths == self._paths:
            return False
        tmp = None
        try:
            directory = os.path.dirname(os.path.abspath(self.path))
            os.makedirs(directory, exist_ok=True)
            fd, tmp = tempfile.mkstemp(dir=directory, suffix=".tmp")
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                json.dump({"version": 1, "paths": sorted(paths)}, handle, indent=2)
                handle.write("\n")
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(tmp, self.path)
        except (OSError, ValueError) as exc:
            self.error = f"Could not save ignored songs: {exc}"
            raise IgnoreError(self.error) from exc
        finally:
            if tmp is not None:
                try:
                    os.unlink(tmp)
                except OSError:
                    pass
        self._paths = paths
        self.error = ""
        return True
