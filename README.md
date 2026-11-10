# Handcam Pin

An OBS Studio video filter that perspective-corrects an angled webcam, such as a
"handcam" filming your mouse from the side. Mounting the camera at a steep angle
keeps your monitor from blocking its view of the mousepad, and the filter turns
that skewed picture back into a straight-on one. You drag four corners onto your
mousepad in a small browser editor, and the filter warps that area so it fills the
output.

![The editor: corners pinned onto a mousepad filmed at a steep angle in the live raw camera view (left), the straightened output (right)](docs/editor.jpg)

- Native plugin in plain C. It depends only on libobs, with no Qt and no
  obs-shaderfilter.
- Builds for Windows, Linux and macOS (universal), for OBS 31 and newer.
- The editor works in any browser and as an OBS dock. Both the raw camera view
  and the warped output update live while you drag.

> **Status:** tested on Windows 11 with OBS 32.2, using both the Direct3D 11 and
> OpenGL renderers. The Linux and macOS builds come from CI and haven't been tried
> on real machines yet. If you run one, please
> [open an issue](../../issues) to say whether it works.

## Install

### Windows

Open PowerShell (press Start, type `PowerShell`, press Enter), paste this line and
press Enter:

```powershell
irm https://github.com/voidfill/obs-handcam-pin/releases/latest/download/install.ps1 | iex
```

(In Command Prompt instead, run
`powershell -c "irm https://github.com/voidfill/obs-handcam-pin/releases/latest/download/install.ps1 | iex"`.)

It downloads the latest release, checks its checksum and installs it into
`C:\ProgramData\obs-studio\plugins\handcam-pin`. That folder works for every
OBS 31+ install: the installer from obsproject.com, Steam, winget and so on. If OBS
is open, it waits for you to close it. If an old copy of the plugin is in OBS's own
program folder (for example from copying files there by hand), it removes that copy,
which may need one admin prompt. You can [read the script](install/install.ps1) first.

### macOS and Linux

Paste this into a terminal:

```bash
curl -fsSL https://github.com/voidfill/obs-handcam-pin/releases/latest/download/install.sh | sh
```

It installs into your own OBS plugin folder, with no `sudo`. On Linux it finds the
regular, Flatpak and Snap OBS and uses the folder that version loads from. On
macOS, files downloaded with `curl` aren't quarantined, so Gatekeeper doesn't
complain. You can [read the script](install/install.sh) first.

Then start OBS and continue with [Usage](#usage). In the OBS log you should see
`[handcam-pin] loaded version …`.

**Updating:** run the same command again. Your corners and shape are stored in
your OBS scene collection, so they're kept. **Uninstalling:** delete the folder the
installer printed.

<details><summary>Manual install from the zip</summary>

Download the zip for your OS from the [Releases](../../releases) page and close OBS.

**Windows:** right-click the zip → *Extract All…* and type `C:\ProgramData\obs-studio\plugins`
as the destination. Windows creates any missing folders, and no admin rights are
needed. (`ProgramData` is hidden in Explorer, but typing the path works.) You should
end up with:

```
C:\ProgramData\obs-studio\plugins\handcam-pin\handcam-pin.dll            ← loaded by OBS 33+
C:\ProgramData\obs-studio\plugins\handcam-pin\bin\64bit\handcam-pin.dll  ← loaded by OBS 31–32
C:\ProgramData\obs-studio\plugins\handcam-pin\data\
```

The DLL is in there twice on purpose, so the same folder works with every OBS
version. Don't also copy it into OBS's program folder (for Steam,
`steamapps\common\OBS Studio`). OBS doesn't need it there, and an old copy there can
be loaded instead of a newer one.

**Portable OBS** ignores `ProgramData`. For OBS 33+, copy the whole `handcam-pin`
folder to `<OBS folder>\plugins\`. For OBS 31–32, use the old layout (this needs
admin rights under `C:\Program Files`):

```
<OBS folder>\obs-plugins\64bit\handcam-pin.dll        ← from handcam-pin\bin\64bit\
<OBS folder>\data\obs-plugins\handcam-pin\            ← contents of handcam-pin\data\
```

**Linux:** extract the zip into `~/.local/share/obs-studio/plugins/` for OBS 33+, or
`~/.config/obs-studio/plugins/` for OBS 31–32. For the Flatpak, use
`~/.var/app/com.obsproject.Studio/data/obs-studio/plugins/` (33+) or
`~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/` (31–32). For the Snap,
use `~/snap/obs-studio/current/.local/share/obs-studio/plugins/` (33+) or
`~/snap/obs-studio/common/.config/obs-studio/plugins/` (31–32). Keep only one copy:
OBS 33 prefers the first location.

**macOS:** copy `handcam-pin.plugin` into `~/Library/Application Support/obs-studio/plugins/`.
The bundle is ad-hoc signed, not notarized. If you downloaded it in a browser and
macOS refuses to load it, run:

```bash
xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/handcam-pin.plugin
```
</details>

## Usage

1. **Add the filter.** Right-click your webcam source → *Filters* → **+** → *Handcam Pin*.
   (The editor can also add it for you, see step 3.)
2. **Turn on obs-websocket.** Tick *Tools → WebSocket Server Settings → Enable WebSocket
   server*. That's all: you don't need to copy the password, the plugin passes it
   to the editor.
3. **Open the editor.** Click *Open editor* at the top of the filter's properties. It
   opens in your browser and connects by itself. To keep it inside OBS instead, copy
   the *Dock URL* shown below the button into *Docks → Custom Browser Docks*.
4. **Pick the source** from the *Source* dropdown. Sources that already have the
   filter are listed first. For any other video source, press **Add filter**.
   The left pane shows the camera as the filter receives it (unwarped, a few
   frames per second), with the saved corners on top.
5. **Drag the corners** onto the four corners of your mousepad. The right-hand
   pane shows the real OBS output live.
6. **Set the shape.** Enter your mousepad's real size (e.g. 49 × 42 cm) so the
   result isn't stretched, or pick *Square* or *Same as camera*.

Tip: the output's pixel size changes when you change the shape, which resizes the
source in your scene. To keep its box fixed, right-click the source in the scene →
*Transform → Edit Transform* → *Bounding Box Type: Scale to inner bounds*.

### Editor controls

| Action | How |
| --- | --- |
| Move a corner | Drag its handle (a magnifier appears) |
| Move the whole quad | Drag inside it |
| Nudge | Arrow keys move the selected corner or the whole quad by 1 px. Hold *Shift* for 10 px, *Alt* for ¼ px |
| Select a corner | Click it, or press *Tab* while the canvas has focus. *Esc* deselects |
| Rotate 90° | Cycles the corners (TL←BL←BR←TR), turning the output a quarter turn |
| Mirror | Swaps left and right corners |
| Reset | Back to the default 10 % inset |
| Shape | *Mousepad size* (width × height), *Square* or *Same as camera* |
| Camera settings | Opens the source's OBS properties (focus, exposure etc. are under *Configure Video* on Windows) |
| grid / preview | Toggle the perspective grid and the live output pane |
| live | Keep the raw camera view updating. Turn it off to freeze it; *Grab frame* then takes a sharp still (up to 1920 px) |

The thick green edge is the top of the output. Changes are sent to OBS while you
drag, and changes made in OBS (sliders, the buttons in the filter dialog) appear in
the editor within about half a second.

**How the editor gets the connection details.** On every start, and when you press
*Open editor*, the plugin writes a copy of `editor.html` into OBS's
`plugin_config/handcam-pin/` folder with obs-websocket's port and password filled
in. *Open editor* and the *Dock URL* use that copy, so a password change is picked
up after the next OBS start. If the editor can't connect, it tells you to enable the
WebSocket server and retries until you do. To connect to OBS on another computer,
open the editor's *Connection* section and enter its address and password.

You can pass settings in the URL, for example
`editor.html?host=127.0.0.1&port=4455&source=Webcam&filter=Handcam%20Pin`.
Connection settings (including the password, unless you untick *remember*) are
kept in the browser's local storage.

## Filter settings

| Setting | Meaning |
| --- | --- |
| `tl_x` … `bl_y` | The four corners in normalized source coordinates (0–1, origin top-left). Default: 10 % inset. |
| `shape` | `pad` (default): use the mousepad size below. `square`: 1:1. `source`: same ratio as the camera. |
| `pad_width`, `pad_height` | Real mousepad size, labelled cm. Only the ratio matters, so mm or inches work too. Default: 45 × 40. |

You never set pixels. The output is the largest size with the chosen ratio that
fits inside the camera frame, so no pixels are invented. A 1920×1080 camera gives
1920×853 for a 90 × 40 cm pad and 1080×1080 for *Square*. The size only changes
when the shape or the camera resolution changes, never while you drag corners.

The filter dialog also has Rotate 90°, Mirror and Reset buttons and sliders for fine
tweaks. Non-convex or degenerate quads are ignored, and the last valid warp stays
active.

## Building from source

Only the libobs headers are needed, with no OBS install, SDK or CMake. They are not
committed: on the first build, `build.sh` downloads them for the OBS release pinned
in [`OBS_VERSION`](OBS_VERSION) (currently 31.1.2) into the gitignored `vendor/`
folder, so the first build needs `git` and internet. To build against another OBS
release, change `OBS_VERSION`; the next build fetches the matching headers.

```bash
./build.sh            # build into dist/stage/
./build.sh package    # build + zip
./build.sh install    # build + copy into your per-user OBS plugin folder
```

| OS | Needs | Notes |
| --- | --- | --- |
| Linux | `cc` (gcc/clang) | libobs symbols are left undefined and resolved when OBS loads the plugin |
| macOS | Xcode command line tools | Universal binary, `-undefined dynamic_lookup`, ad-hoc signed |
| Windows | [Zig](https://ziglang.org) 0.15+, Git Bash | `zig dlltool` makes `obs.lib` from `windows/obs.def`. If you call a new libobs function, add it there |

CI (`.github/workflows/build.yml`) builds on native Linux, Windows and macOS runners.
Pushing a `v*` tag publishes a GitHub release with the three zips and the two
install scripts from [`install/`](install), with that release's version and zip
checksums filled in. Both scripts also install a local zip if you set
`HANDCAM_PIN_ZIP` to its path, which is how CI tests them.

## Notes

- **Why "Open editor" isn't a link button.** OBS's URL buttons only open
  `http(s)` links, so the button asks the OS to open the bundled HTML file instead.
- **How the raw view works.** obs-websocket can only screenshot a source with its
  filters applied, so the plugin answers the editor itself (an obs-websocket vendor
  request, `handcam-pin` / `GetRawFrame`): it copies its own input while rendering,
  scaled down to about the size the editor shows it (up to 1920 px while you drag
  a corner, so the magnifier stays sharp). The filter stays on the whole time, so
  your stream doesn't flicker. Against an older plugin the editor falls back to a
  still taken by briefly switching the filter off.
- **An idle editor costs nothing.** Both live panes stop polling OBS after a minute
  without mouse or keyboard activity in the editor (handy when it stays open as a
  dock while you stream) and resume as soon as you move the mouse over it. With
  the editor closed, the plugin does no extra work at all.

## License

GPL-2.0-or-later (it links against libobs). See [LICENSE](LICENSE).
