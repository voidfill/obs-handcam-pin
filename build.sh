#!/usr/bin/env bash
# Build handcam-pin for the host OS and stage it under dist/ in OBS's plugin layout.
#
#   ./build.sh            build (Linux: cc, macOS: clang universal, Windows: zig cc)
#   ./build.sh package    build + zip into dist/handcam-pin-<version>-<os>.zip
#   ./build.sh install    build + copy into your per-user OBS plugin folder
#
# Env: VERSION (default: contents of ./VERSION), CC, ZIG, ARCHS (macOS).
# Nothing links against libobs at build time except the Windows import library,
# which is generated from windows/obs.def. Only the vendored headers are needed.
set -euo pipefail
cd "$(dirname "$0")"

NAME=handcam-pin
VERSION="${VERSION:-$(cat VERSION)}"
VERSION="${VERSION#v}"
BUILD=build
DIST=dist
CMD="${1:-build}"

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
  # Layout of the OBS install folder (C:\Program Files\obs-studio).
  STAGE="$DIST/stage"
  mkdir -p "$STAGE/obs-plugins/64bit" "$STAGE/data/obs-plugins/$NAME"
  cp "$BUILD/$NAME.dll" "$STAGE/obs-plugins/64bit/"
  cp -r "${DATA_FILES[@]}" "$STAGE/data/obs-plugins/$NAME/"
  ;;
esac
echo "Built $NAME $VERSION ($OS) -> $STAGE/"

case "$CMD" in
build) ;;
package)
  ZIPNAME="$NAME-$VERSION-$OS.zip"
  if [ "$OS" = windows ]; then
    # Windows' bsdtar writes proper zips (forward slashes, unlike Compress-Archive on PS 5.1).
    (cd "$STAGE" && "$(cygpath -u "$SYSTEMROOT")/System32/tar.exe" -a -cf "../$ZIPNAME" obs-plugins data)
  else
    (cd "$STAGE" && zip -qry "../$ZIPNAME" .)
  fi
  echo "Packaged $DIST/$ZIPNAME"
  ;;
install)
  # Per-user / no-admin plugin folders that OBS 30+ scans.
  case "$OS" in
  linux)
    DEST="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins"
    mkdir -p "$DEST"; rm -rf "$DEST/$NAME"; cp -r "$STAGE/$NAME" "$DEST/" ;;
  macos)
    DEST="$HOME/Library/Application Support/obs-studio/plugins"
    mkdir -p "$DEST"; rm -rf "$DEST/$NAME.plugin"; cp -R "$STAGE/$NAME.plugin" "$DEST/" ;;
  windows)
    DEST="$(cygpath -u "${PROGRAMDATA:-C:\\ProgramData}")/obs-studio/plugins/$NAME"
    mkdir -p "$DEST/bin/64bit" "$DEST/data"
    cp "$STAGE/obs-plugins/64bit/$NAME.dll" "$DEST/bin/64bit/"
    rm -rf "$DEST/data"/*; cp -r "$STAGE/data/obs-plugins/$NAME/"* "$DEST/data/" ;;
  esac
  echo "Installed to $DEST (restart OBS to load a new build)"
  ;;
*) echo "unknown command: $CMD (use build, package or install)" >&2; exit 1 ;;
esac
