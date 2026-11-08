#!/usr/bin/env bash
# Build handcam-pin for the host OS and stage it under dist/ in OBS's plugin layout.
#
#   ./build.sh            build (Linux: cc, macOS: clang universal, Windows: zig cc)
#   ./build.sh package    build + zip into dist/handcam-pin-<version>-<os>.zip
#   ./build.sh install    build + copy into your per-user OBS plugin folder
#
# Env: VERSION (default: contents of ./VERSION), CC, ZIG, ARCHS (macOS).
# Nothing links against libobs at build time except the Windows import library,
# which is generated from windows/obs.def. Only the libobs headers are needed;
# they are downloaded automatically for the version in ./OBS_VERSION.
set -euo pipefail
cd "$(dirname "$0")"

NAME=handcam-pin
VERSION="${VERSION:-$(cat VERSION)}"
VERSION="${VERSION#v}"
BUILD=build
DIST=dist
CMD="${1:-build}"

# libobs headers are not committed: fetch them for the pinned OBS release on
# first build (or when OBS_VERSION changes). Needs git + network once.
OBS_TAG="$(tr -d '[:space:]' < OBS_VERSION)"
if [ "$(cat vendor/libobs/OBS_VERSION 2>/dev/null)" != "$OBS_TAG" ]; then
  scripts/fetch-obs-headers.sh "$OBS_TAG"
fi

rm -rf "$BUILD" "$DIST"
mkdir -p "$BUILD" "$DIST"

CFLAGS=(-std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
        -Ivendor/libobs "-DPLUGIN_VERSION=\"$VERSION\"")
DATA_FILES=(data/handcam-pin.effect data/editor.html data/locale)

case "$(uname -s)" in
Linux*)   OS=linux ;;
Darwin*)  OS=macos ;;
MINGW*|MSYS*|CYGWIN*|Windows_NT) OS=windows ;;
*) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac

case "$OS" in
linux)
  CC="${CC:-cc}"
  # Undefined libobs symbols are resolved by OBS when it dlopen()s the plugin.
  "$CC" "${CFLAGS[@]}" -fPIC -fvisibility=hidden -shared -s \
    -o "$BUILD/$NAME.so" "src/$NAME.c" -lm
  STAGE="$DIST/stage"; PKG="$STAGE/$NAME"
  mkdir -p "$PKG/bin/64bit" "$PKG/data"
  cp "$BUILD/$NAME.so" "$PKG/bin/64bit/"
  cp -r "${DATA_FILES[@]}" "$PKG/data/"
  ;;
macos)
  CC="${CC:-clang}"
  ARCHS="${ARCHS:-arm64 x86_64}"
  ARCH_FLAGS=(); for a in $ARCHS; do ARCH_FLAGS+=(-arch "$a"); done
  "$CC" "${CFLAGS[@]}" "${ARCH_FLAGS[@]}" -mmacosx-version-min=11.0 -fvisibility=hidden \
    -bundle -undefined dynamic_lookup -o "$BUILD/$NAME" "src/$NAME.c"
  STAGE="$DIST/stage"; B="$STAGE/$NAME.plugin/Contents"
  mkdir -p "$B/MacOS" "$B/Resources"
  cp "$BUILD/$NAME" "$B/MacOS/$NAME"
  cp -r "${DATA_FILES[@]}" "$B/Resources/"
  sed -e "s/@VERSION@/$VERSION/g" -e "s/@NAME@/$NAME/g" macos/Info.plist.in > "$B/Info.plist"
  codesign --force --sign - "$STAGE/$NAME.plugin"   # ad-hoc; required on Apple Silicon
  ;;
windows)
  ZIG="${ZIG:-zig}"
  # Import library for obs.dll from the list of functions we call.
  "$ZIG" dlltool -m i386:x86-64 -d windows/obs.def -l "$BUILD/obs.lib"
  "$ZIG" cc -target x86_64-windows-gnu "${CFLAGS[@]}" -shared -s \
    -o "$BUILD/$NAME.dll" "src/$NAME.c" "$BUILD/obs.lib" -lshell32
  # Same layout as Linux: goes into C:\ProgramData\obs-studio\plugins (no admin, OBS 30+).
  STAGE="$DIST/stage"; PKG="$STAGE/$NAME"
  mkdir -p "$PKG/bin/64bit" "$PKG/data"
  cp "$BUILD/$NAME.dll" "$PKG/bin/64bit/"
  cp -r "${DATA_FILES[@]}" "$PKG/data/"
  ;;
esac
echo "Built $NAME $VERSION ($OS) -> $STAGE/"

case "$CMD" in
build) ;;
package)
  ZIPNAME="$NAME-$VERSION-$OS.zip"
  if [ "$OS" = windows ]; then
    # Windows' bsdtar writes proper zips (forward slashes, unlike Compress-Archive on PS 5.1).
    (cd "$STAGE" && "$(cygpath -u "$SYSTEMROOT")/System32/tar.exe" -a -cf "../$ZIPNAME" "$NAME")
  else
    (cd "$STAGE" && zip -qry "../$ZIPNAME" .)
  fi
  echo "Packaged $DIST/$ZIPNAME"
  ;;
install)
  # No-admin plugin folders that OBS 30+ scans.
  case "$OS" in
  linux)   DEST="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins"; ITEM="$NAME" ;;
  macos)   DEST="$HOME/Library/Application Support/obs-studio/plugins"; ITEM="$NAME.plugin" ;;
  windows) DEST="$(cygpath -u "${PROGRAMDATA:-C:\\ProgramData}")/obs-studio/plugins"; ITEM="$NAME"
           # Windows locks the DLL while OBS runs; bail out before deleting anything.
           if tasklist //FI "IMAGENAME eq obs64.exe" 2>/dev/null | grep -qi obs64.exe; then
             echo "Close OBS first (it locks the plugin DLL), then run ./build.sh install again." >&2; exit 1
           fi ;;
  esac
  mkdir -p "$DEST"
  rm -rf "${DEST:?}/$ITEM"   # replace the whole folder so no stale files remain
  cp -R "$STAGE/$ITEM" "$DEST/"
  echo "Installed to $DEST/$ITEM (restart OBS to load a new build)"
  ;;
*) echo "unknown command: $CMD (use build, package or install)" >&2; exit 1 ;;
esac
