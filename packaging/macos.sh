#!/bin/sh
# macOS: modjuke.app with Qt and libopenmpt inside, on a disk image.
#
#   packaging/macos.sh [BUILD_DIR]      (default: build)
#
# Needs a Release build (BUILD_DIR/modjuke.app) and Qt's macdeployqt on PATH.
# For Apple silicon and Intel, configure with
# -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64". Writes dist/modjuke-VERSION-macos.dmg.
# The app is signed ad hoc only (no Apple Developer ID), so macOS asks for
# confirmation on first start (System Settings > Privacy & Security > Open Anyway).
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
BUILD=$(cd "${1:-build}" && pwd)
VERSION=${VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
DIST=$ROOT/dist
STAGE=$DIST/dmg
APP=$STAGE/modjuke.app

[ -d "$BUILD/modjuke.app" ] || { echo "no $BUILD/modjuke.app: build modjuke first" >&2; exit 1; }
command -v macdeployqt >/dev/null || { echo "macdeployqt not found: put Qt's bin folder on PATH" >&2; exit 1; }
[ -f "$DIST/libopenmpt/libopenmpt.0.dylib" ] || sh packaging/libopenmpt.sh "$DIST/libopenmpt"

rm -rf "$STAGE"
mkdir -p "$STAGE"
ditto "$BUILD/modjuke.app" "$APP"
mkdir -p "$APP/Contents/Frameworks" "$APP/Contents/Resources/licenses"
# loaded at run time (dlopen) from Contents/Frameworks
cp "$DIST/libopenmpt/libopenmpt.0.dylib" "$APP/Contents/Frameworks/"
cp "$DIST/libopenmpt/licenses/"* "$APP/Contents/Resources/licenses/"
cp packaging/THIRD-PARTY.txt "$APP/Contents/Resources/"

macdeployqt "$APP"
# every binary needs a signature on Apple silicon; ad hoc, after macdeployqt
# has rewritten the library paths
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

ln -s /Applications "$STAGE/Applications"
OUTPUT="$DIST/modjuke-$VERSION-macos.dmg"
rm -f "$OUTPUT"
hdiutil create -volname modjuke -srcfolder "$STAGE" -format UDZO "$OUTPUT"
echo "OK: $OUTPUT"
