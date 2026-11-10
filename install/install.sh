#!/bin/sh
# Handcam Pin installer for macOS and Linux. Paste into a terminal:
#
#   curl -fsSL https://github.com/voidfill/obs-handcam-pin/releases/latest/download/install.sh | sh
#
# Installs into your user's OBS plugin folder (no sudo):
#   macOS     ~/Library/Application Support/obs-studio/plugins/handcam-pin.plugin
#   Linux     ~/.local/share/obs-studio/plugins/handcam-pin   (OBS 33+)
#             ~/.config/obs-studio/plugins/handcam-pin        (OBS 31-32, or version unknown)
#   Flatpak   the same two, under ~/.var/app/com.obsproject.Studio/{data,config}
#   Snap      ~/snap/obs-studio/<revision>/.local/share or ~/snap/obs-studio/common/.config
#             (the obs-studio snap points XDG_DATA_HOME / XDG_CONFIG_HOME there)
# and removes a copy in the other Linux location so only one is ever loaded.
# Files downloaded by curl aren't quarantined, so macOS needs no xattr step.
#
# The release workflow fills in the @...@ values. To install a zip you already have
# (or to test this script from the repo), set HANDCAM_PIN_ZIP to its path first.
set -eu

NAME=handcam-pin
VERSION='@VERSION@'
REPO='@REPO@'
SHA256_LINUX='@SHA256_LINUX@'
SHA256_MACOS='@SHA256_MACOS@'
FLATPAK_ID=com.obsproject.Studio
SNAP_NAME=obs-studio

if [ -t 2 ]; then RED=$(printf '\033[31m'); YEL=$(printf '\033[33m'); GRN=$(printf '\033[32m'); OFF=$(printf '\033[0m')
else RED=; YEL=; GRN=; OFF=; fi
say()  { printf '%s\n' "$*"; }
warn() { printf '%s%s%s\n' "$YEL" "$*" "$OFF" >&2; }
die()  { printf '\n%sHandcam Pin was not installed: %s%s\n' "$RED" "$*" "$OFF" >&2; exit 1; }
has()  { command -v "$1" >/dev/null 2>&1; }

sha256_of() {   # main checks that one of the two exists before downloading
  if has sha256sum; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

unzip_to() {   # zip, destination folder
  if has unzip; then unzip -q "$1" -d "$2"
  elif has bsdtar; then bsdtar -xf "$1" -C "$2"
  elif has python3; then python3 -m zipfile -e "$1" "$2"
  else die "need unzip (or bsdtar or python3) to unpack the plugin. Install unzip and run this again."; fi
}

# Only this user's OBS (another signed-in user's doesn't lock our files). If pgrep
# is missing, assume OBS is closed rather than waiting forever.
obs_running() {
  if [ "$OS" = macos ]; then pgrep -u "$(id -u)" -x OBS >/dev/null 2>&1
  else pgrep -u "$(id -u)" -x obs >/dev/null 2>&1   # also matches Flatpak and Snap OBS
  fi
}

# Major OBS version, or empty if unknown.
native_obs_major() {
  has obs || return 0
  if has timeout; then t="timeout 10"; else t=; fi
  # Unknown (empty) is fine: the legacy location works on every supported OBS.
  $t obs --version </dev/null 2>/dev/null | sed -n 's/^OBS Studio - \([0-9][0-9]*\).*/\1/p' | head -n 1 || true
}
flatpak_obs_major() {
  LC_ALL=C flatpak info "$FLATPAK_ID" </dev/null 2>/dev/null | sed -n 's/^ *Version: *\([0-9][0-9]*\).*/\1/p' | head -n 1
}
snap_obs_major() {   # "Name Version Rev ..." header, then "obs-studio 32.2.0 1234 ..."
  LC_ALL=C snap list "$SNAP_NAME" </dev/null 2>/dev/null | awk 'NR == 2 { print $2 }' | sed -n 's/^\([0-9][0-9]*\).*/\1/p'
}
snap_obs_rev() {     # numeric revision ("x1" for sideloaded snaps), or empty
  LC_ALL=C snap list "$SNAP_NAME" </dev/null 2>/dev/null | awk 'NR == 2 { print $3 }' | sed -n '/^x\{0,1\}[0-9][0-9]*$/p'
}

# install_linux <label> <data root> <config root> <major version or empty>
install_linux() {
  # POSIX sh has no "local", so these names are prefixed to keep main's variables intact.
  il_label=$1; il_new=$2/obs-studio/plugins; il_legacy=$3/obs-studio/plugins; il_major=$4
  if [ -n "$il_major" ] && [ "$il_major" -ge 33 ]; then il_dest=$il_new; il_other=$il_legacy
  else il_dest=$il_legacy; il_other=$il_new; fi
  if [ -n "$il_major" ] && [ "$il_major" -lt 31 ]; then
    warn "$il_label is version $il_major. Handcam Pin needs OBS 31 or newer: update OBS, or the plugin won't load."
  fi
  # Keep one copy: OBS 33 loads the new location first and skips the legacy one,
  # so a stale copy in the new location would shadow this install after an OBS upgrade.
  if [ -e "$il_other/$NAME" ]; then rm -rf "${il_other:?}/$NAME"; say "Removed old copy: $il_other/$NAME"; fi
  mkdir -p "$il_dest"
  rm -rf "${il_dest:?}/$NAME"
  cp -R "$SRC" "$il_dest/"
  chmod 755 "$il_dest/$NAME/$NAME.so" "$il_dest/$NAME/bin/64bit/$NAME.so"
  for f in "$NAME.so" "bin/64bit/$NAME.so" data/editor.html data/handcam-pin.effect; do
    [ -f "$il_dest/$NAME/$f" ] || die "copying to $il_dest failed ($f is missing). Run the command again."
  done
  say "${GRN}Installed for $il_label:${OFF} $il_dest/$NAME"
}

main() {
  [ "$(id -u)" != 0 ] || die "don't run this with sudo. OBS plugins go in your own home folder."
  case "$(uname -s)" in
    Darwin) OS=macos ;;
    Linux)  OS=linux
            [ "$(uname -m)" = x86_64 ] || die "only x86_64 Linux builds exist so far (this is $(uname -m))." ;;
    *) die "unsupported system $(uname -s). On Windows, use the PowerShell command from the README." ;;
  esac
  case "$VERSION" in @*) label="Handcam Pin" ;; *) label="Handcam Pin $VERSION" ;; esac
  say "Installing $label..."

  TMP=$(mktemp -d 2>/dev/null || mktemp -d -t "$NAME")
  trap 'rm -rf "$TMP"' EXIT
  trap 'exit 130' INT TERM   # still let Ctrl+C stop the script (the EXIT trap cleans up)

  # 1. Get and unpack the release zip (before asking anyone to close OBS).
  if [ -n "${HANDCAM_PIN_ZIP:-}" ]; then
    zip=$HANDCAM_PIN_ZIP; [ -f "$zip" ] || die "$zip not found."
    say "Using $zip"
  else
    case "$VERSION" in @*) die "this is the unreleased script from the repository. Use the command from the README, or set HANDCAM_PIN_ZIP to a release zip." ;; esac
    has sha256sum || has shasum || die "need sha256sum or shasum to check the download."
    url="https://github.com/$REPO/releases/download/v$VERSION/$NAME-$VERSION-$OS.zip"
    zip=$TMP/$NAME.zip
    say "Downloading $url"
    if has curl; then curl -fsSL -o "$zip" "$url" || die "download failed. Check your internet connection and try again."
    elif has wget; then wget -q -O "$zip" "$url" || die "download failed. Check your internet connection and try again."
    else die "need curl or wget."; fi
    if [ "$OS" = macos ]; then want=$SHA256_MACOS; else want=$SHA256_LINUX; fi
    got=$(sha256_of "$zip")
    [ "$got" = "$want" ] || die "the download is corrupted (checksum mismatch). Try again."
  fi
  mkdir "$TMP/unpacked"
  unzip_to "$zip" "$TMP/unpacked"
  # Check every file before touching anything.
  if [ "$OS" = macos ]; then
    SRC=$TMP/unpacked/$NAME.plugin
    set -- "Contents/MacOS/$NAME" Contents/Resources/editor.html Contents/Resources/handcam-pin.effect
  else
    SRC=$TMP/unpacked/$NAME
    set -- "$NAME.so" "bin/64bit/$NAME.so" data/editor.html data/handcam-pin.effect
  fi
  for f in "$@"; do
    [ -f "$SRC/$f" ] || die "$zip is not a Handcam Pin $OS release zip, or one too old for this installer."
  done

  # 2. OBS reads its plugin list at startup, so it has to be closed for the update to apply.
  if obs_running; then
    if [ "$OS" = macos ]; then warn "OBS is running. Quit OBS (Cmd+Q) to continue, or press Ctrl+C to cancel."
    else warn "OBS is running. Close OBS (File > Exit) to continue, or press Ctrl+C to cancel."; fi
    while obs_running; do sleep 1; done
  fi

  # 3. Copy.
  if [ "$OS" = macos ]; then
    dest="$HOME/Library/Application Support/obs-studio/plugins"
    mkdir -p "$dest"
    rm -rf "${dest:?}/$NAME.plugin"
    cp -R "$SRC" "$dest/"
    xattr -dr com.apple.quarantine "$dest/$NAME.plugin" 2>/dev/null || true
    for f in "Contents/MacOS/$NAME" Contents/Resources/editor.html Contents/Resources/handcam-pin.effect; do
      [ -f "$dest/$NAME.plugin/$f" ] || die "copying to $dest failed ($f is missing). Run the command again."
    done
    say "${GRN}Installed:${OFF} $dest/$NAME.plugin"
  else
    found=
    if has obs; then
      install_linux "OBS" "${XDG_DATA_HOME:-$HOME/.local/share}" "${XDG_CONFIG_HOME:-$HOME/.config}" "$(native_obs_major)"
      found=1
    fi
    if has flatpak && flatpak info "$FLATPAK_ID" </dev/null >/dev/null 2>&1; then
      install_linux "Flatpak OBS" "$HOME/.var/app/$FLATPAK_ID/data" "$HOME/.var/app/$FLATPAK_ID/config" "$(flatpak_obs_major)"
      found=1
    fi
    if has snap && snap list "$SNAP_NAME" </dev/null >/dev/null 2>&1; then
      # Inside the snap, XDG_DATA_HOME is ~/snap/obs-studio/<revision>/.local/share. Write to
      # the revision folder itself, never through "current": snapd creates that symlink on
      # the first run and refuses to start the snap if a real folder is in its place.
      # Without a revision, use the legacy location (common/.config), which OBS 31-33 load.
      snap_rev=$(snap_obs_rev)
      if [ -n "$snap_rev" ]; then
        install_linux "Snap OBS" "$HOME/snap/$SNAP_NAME/$snap_rev/.local/share" "$HOME/snap/$SNAP_NAME/common/.config" "$(snap_obs_major)"
      else
        install_linux "Snap OBS" "$HOME/snap/$SNAP_NAME/unknown-revision" "$HOME/snap/$SNAP_NAME/common/.config" ""
      fi
      found=1
    fi
    if [ -z "$found" ]; then
      warn "Could not find OBS (no obs command, Flatpak or Snap). Installing for a regular OBS install anyway."
      install_linux "OBS" "${XDG_DATA_HOME:-$HOME/.local/share}" "${XDG_CONFIG_HOME:-$HOME/.config}" ""
    fi
  fi

  say ""
  say "${GRN}$label is installed.${OFF}"
  say ""
  say "Next:"
  say "  1. Start OBS."
  say "  2. Right-click your webcam source > Filters > + > Handcam Pin."
  say "  3. Tools > WebSocket Server Settings > tick \"Enable WebSocket server\"."
  say "  4. In the filter, click \"Open editor\" and drag the corners onto your mousepad."
  say ""
  say "To update later, run the same command again. To uninstall, delete the folder above."
}

# Everything runs from here, so a download cut off halfway through runs nothing.
main "$@"
