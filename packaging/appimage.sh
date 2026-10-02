#!/bin/sh
# Linux: one-file AppImage with Qt and libopenmpt inside.
#
#   packaging/appimage.sh [BUILD_DIR]      (default: build)
#
# Needs a Release build of modjuke in BUILD_DIR, Qt's qmake on PATH (or
# QMAKE=/path/to/qmake) and curl. Writes dist/modjuke-VERSION-x86_64.AppImage.
# The AppImage runs on systems at least as new as the one it was built on
# (glibc), so build on an older distribution for wider reach.
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
BUILD=$(cd "${1:-build}" && pwd)
VERSION=${VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
DIST=$ROOT/dist
APPDIR=$DIST/AppDir
TOOLS=$DIST/tools

[ -x "$BUILD/modjuke" ] || { echo "no $BUILD/modjuke: build modjuke first" >&2; exit 1; }
# QT_ROOT_DIR: set by aqtinstall / install-qt-action
if [ -z "${QMAKE:-}" ] && [ -n "${QT_ROOT_DIR:-}" ]; then QMAKE=$QT_ROOT_DIR/bin/qmake; fi
QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake || true)}
[ -n "$QMAKE" ] || { echo "qmake not found: put Qt's bin folder on PATH or set QMAKE" >&2; exit 1; }
export QMAKE

[ -f "$DIST/libopenmpt/libopenmpt.so.0" ] || sh packaging/libopenmpt.sh "$DIST/libopenmpt"

mkdir -p "$TOOLS"
for tool in linuxdeploy linuxdeploy-plugin-qt; do
  if [ ! -x "$TOOLS/$tool-x86_64.AppImage" ]; then
    curl -fsSL --retry 3 -o "$TOOLS/$tool-x86_64.AppImage" \
      "https://github.com/linuxdeploy/$tool/releases/download/continuous/$tool-x86_64.AppImage"
    chmod +x "$TOOLS/$tool-x86_64.AppImage"
  fi
done

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/share/doc/modjuke"
cp -r "$DIST/libopenmpt/licenses" "$APPDIR/usr/share/doc/modjuke/"
cp packaging/THIRD-PARTY.txt "$APPDIR/usr/share/doc/modjuke/"
cat > "$DIST/modjuke.desktop" <<EOD
[Desktop Entry]
Type=Application
Name=modjuke
Comment=Tracker music player
Exec=modjuke
Icon=modjuke
Terminal=false
Categories=AudioVideo;Audio;Player;
Keywords=tracker;module;MOD;XM;IT;S3M;
EOD
cp resources/modjuke.png "$DIST/modjuke.png"

export PATH="$TOOLS:$PATH"            # linuxdeploy finds its Qt plugin here
export APPIMAGE_EXTRACT_AND_RUN=1     # the tools run without FUSE
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
export OUTPUT="$DIST/modjuke-$VERSION-x86_64.AppImage"
rm -f "$OUTPUT"
# libopenmpt is loaded at run time (dlopen), so it's named explicitly (-l);
# modjuke looks for it in usr/lib next to its usr/bin.
linuxdeploy-x86_64.AppImage --appdir "$APPDIR" \
  -e "$BUILD/modjuke" -l "$DIST/libopenmpt/libopenmpt.so.0" \
  -d "$DIST/modjuke.desktop" -i "$DIST/modjuke.png" \
  --plugin qt --output appimage
echo "OK: $OUTPUT"
