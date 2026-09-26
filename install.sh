#!/bin/sh
# Run from any working directory; PYTHON may select a different Python 3 interpreter.
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
exec "${PYTHON:-python3}" "$here/install.py" "$@"
