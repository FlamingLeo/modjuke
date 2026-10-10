#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$script_dir/build"
prefix=""

usage() {
    cat <<'USAGE'
Usage: ./install.sh [--prefix PREFIX]

Configures, builds and installs modjuke. With no option, an existing
installation is updated in place: the prefix is the one recorded in
build/install_manifest.txt, else the one of the modjuke found on PATH,
else ~/.local.
USAGE
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
    --prefix)
        if [[ $# -ne 2 ]]; then
            usage >&2
            exit 2
        fi
        prefix="$(realpath -m -- "$2")"
        ;;
    "")
        ;;
    *)
        echo "Unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
esac

# find an existing installation
if [[ -z "$prefix" && -f "$build_dir/install_manifest.txt" ]]; then
    installed="$(grep -m1 '/bin/modjuke$' "$build_dir/install_manifest.txt" || true)"
    prefix="${installed%/bin/modjuke}"
fi
if [[ -z "$prefix" ]] && installed="$(command -v modjuke)"; then
    candidate="$(dirname -- "$(dirname -- "$installed")")"
    [[ -f "$candidate/share/applications/modjuke.desktop" ]] && prefix="$candidate"
fi
[[ -n "$prefix" ]] || prefix="$HOME/.local"

if pgrep -x modjuke > /dev/null; then
    echo "modjuke is running. Close modjuke, then run this script again." >&2
    exit 1
fi

if [[ -e "$prefix/bin/modjuke" ]]; then
    action="Updated"
    printf 'Updating the modjuke installation in %s\n' "$prefix"
else
    action="Installed"
    printf 'Installing modjuke to %s\n' "$prefix"
fi

# configure, build, install
configure=(cmake -S "$script_dir" -B "$build_dir"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$prefix"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
# an existing build keeps its generator
[[ -f "$build_dir/CMakeCache.txt" ]] || configure+=(-G Ninja)
"${configure[@]}"
cmake --build "$build_dir" --parallel 2
cmake --install "$build_dir"

if ! "$prefix/bin/modjuke" --check; then
    echo "modjuke is installed, but it cannot load libopenmpt. Install libopenmpt (see README.md)." >&2
    exit 1
fi
printf '%s %s to %s.\n' "$action" "$("$prefix/bin/modjuke" --version)" "$prefix"

case ":$PATH:" in
    *":$prefix/bin:"*)
        ;;
    *)
        printf 'To run modjuke from a terminal, add this line to your shell profile:\n'
        printf "  export PATH=\"%s/bin:\$PATH\"\n" "$prefix"
        ;;
esac
