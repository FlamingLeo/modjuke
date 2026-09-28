#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
default_manifest="$script_dir/build/install_manifest.txt"
mode=""
value=""

usage() {
    cat <<'USAGE'
Usage: ./uninstall.sh [--manifest FILE | --prefix PREFIX]

With no option, removes the files listed in build/install_manifest.txt when
available; otherwise removes the standard per-user installation from ~/.local.
Use --prefix when installing to a non-default prefix without an install manifest.
USAGE
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
    --manifest|--prefix)
        if [[ $# -ne 2 ]]; then
            usage >&2
            exit 2
        fi
        mode="$1"
        value="$2"
        ;;
    "")
        if [[ -f "$default_manifest" ]]; then
            mode="--manifest"
            value="$default_manifest"
        else
            mode="--prefix"
            value="$HOME/.local"
        fi
        ;;
    *)
        echo "Unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
esac

removed=0
remove_file() {
    local path="$1"
    if [[ -e "$path" ]]; then
        rm -f -- "$path"
        printf 'Removed %s\n' "$path"
        removed=$((removed + 1))
    fi
}

if [[ "$mode" == "--manifest" ]]; then
    if [[ ! -r "$value" ]]; then
        echo "Cannot read install manifest: $value" >&2
        exit 1
    fi
    while IFS= read -r path || [[ -n "$path" ]]; do
        [[ -n "$path" ]] && remove_file "$path"
    done < "$value"
    echo "Uninstall complete ($removed installed file(s) removed)."
else
    prefix="${value%/}"
    [[ -n "$prefix" ]] || prefix="/"
    for path in \
        "$prefix/bin/modjuke" \
        "$prefix/share/applications/modjuke.desktop" \
        "$prefix/share/icons/hicolor/256x256/apps/modjuke.png"; do
        remove_file "$path"
    done
    echo "Uninstall complete ($removed file(s) removed from $prefix)."
fi
