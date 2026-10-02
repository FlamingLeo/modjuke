#!/bin/sh
# Build the libopenmpt that the Linux and macOS packages bundle, from the
# official source release (checksum-verified).
#
#   packaging/libopenmpt.sh OUTDIR
#
# Writes OUTDIR/libopenmpt.so.0 (Linux) or OUTDIR/libopenmpt.0.dylib (macOS,
# arm64 + x86_64), plus OUTDIR/licenses/. The library has no dependencies
# beyond the C++ runtime: zlib, MP3 and Vorbis decoding use the copies bundled
# with libopenmpt (miniz, minimp3, stb_vorbis).
set -eu

VERSION=0.8.9
URL="https://lib.openmpt.org/files/libopenmpt/src/libopenmpt-$VERSION+release.makefile.tar.gz"
SHA256=9273b88b67973cc69e54d748ab1b749399d6d07695f1c37d0c59f88b4106074f

[ $# -eq 1 ] || { echo "usage: $0 OUTDIR" >&2; exit 2; }
OUT=$1
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK="$OUT/work"
rm -rf "$WORK"
mkdir -p "$WORK"
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

curl -fsSL --retry 3 -o "$WORK/src.tar.gz" "$URL"
if command -v sha256sum >/dev/null; then
  echo "$SHA256  $WORK/src.tar.gz" | sha256sum -c - >/dev/null
else
  echo "$SHA256  $WORK/src.tar.gz" | shasum -a 256 -c - >/dev/null
fi || { echo "checksum mismatch: $URL" >&2; exit 1; }

# no external libraries, no player/plugins/tests
FLAGS="SHARED_LIB=1 STATIC_LIB=0 OPENMPT123=0 EXAMPLES=0 TEST=0 IN_OPENMPT=0 XMP_OPENMPT=0
  NO_ZLIB=1 NO_MPG123=1 NO_OGG=1 NO_VORBIS=1 NO_VORBISFILE=1 NO_PORTAUDIO=1 NO_PORTAUDIOCPP=1
  NO_PULSEAUDIO=1 NO_SDL2=1 NO_FLAC=1 NO_SNDFILE=1"

unpack() {  # fresh source tree in $1
  mkdir -p "$1"
  tar xzf "$WORK/src.tar.gz" -C "$1" --strip-components=1
}

case "$(uname -s)" in
  Linux)
    unpack "$WORK/src"
    # LIBOPENMPT_SO_VERSION=0: the ABI version distros use (libopenmpt.so.0);
    # the Makefile's default is libtool's "current" number
    (cd "$WORK/src" && make CONFIG=gcc $FLAGS LIBOPENMPT_SO_VERSION=0 -j"$JOBS" >/dev/null)
    cp "$WORK/src/bin/libopenmpt.so.0" "$OUT/libopenmpt.so.0"
    LIB="$OUT/libopenmpt.so.0"
    ;;
  Darwin)
    # one build per architecture, joined into a universal library
    MIN=${MACOSX_DEPLOYMENT_TARGET:-12.0}
    for arch in arm64 x86_64; do
      unpack "$WORK/$arch"
      (cd "$WORK/$arch" && make CONFIG=macos ARCH=$arch MACOSX_VERSION_MIN=$MIN SOSUFFIX=.dylib $FLAGS -j"$JOBS" >/dev/null)
    done
    lipo -create "$WORK/arm64/bin/libopenmpt.dylib" "$WORK/x86_64/bin/libopenmpt.dylib" \
      -output "$OUT/libopenmpt.0.dylib"
    install_name_tool -id @rpath/libopenmpt.0.dylib "$OUT/libopenmpt.0.dylib"
    codesign --force --sign - "$OUT/libopenmpt.0.dylib"
    LIB="$OUT/libopenmpt.0.dylib"
    ;;
  *) echo "unsupported system: $(uname -s)" >&2; exit 1 ;;
esac

SRC="$WORK/src"
[ -d "$SRC" ] || SRC="$WORK/arm64"
mkdir -p "$OUT/licenses"
cp "$SRC/LICENSE" "$OUT/licenses/libopenmpt.txt"
cp "$SRC/include/miniz/LICENSE" "$OUT/licenses/miniz.txt"
cp "$SRC/include/minimp3/LICENSE" "$OUT/licenses/minimp3.txt"
# stb_vorbis carries its license at the end of the source file
sed -n '/^This software is available under 2 licenses/,$p' "$SRC/include/stb_vorbis/stb_vorbis.c" \
  > "$OUT/licenses/stb_vorbis.txt"
rm -rf "$WORK"
echo "libopenmpt $VERSION: $LIB"
