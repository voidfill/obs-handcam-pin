# Handcam Pin installer for Windows. Paste into PowerShell:
#
#   irm https://github.com/voidfill/obs-handcam-pin/releases/latest/download/install.ps1 | iex
#
# Installs into C:\ProgramData\obs-studio\plugins\handcam-pin, which every
# non-portable OBS 31+ loads (regular installer and Steam alike), and removes old
# copies from OBS's program folder that would otherwise be loaded instead.
# No admin rights unless such an old copy has to be removed.
#
# The release workflow fills in the @...@ values. To install a zip you already have
# (or to test this script from the repo), set $env:HANDCAM_PIN_ZIP to its path first.
#
# Keep this file pure ASCII: PowerShell 5.1's irm decodes release assets as Latin-1.

& {
Set-StrictMode -Off                        # a strict mode from the user's profile would leak in here
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest crawls with the progress bar on PS 5.1

$Name    = 'handcam-pin'
$Version = '@VERSION@'
$Repo    = '@REPO@'
$Sha256  = '@SHA256@'
$MinObs  = 31
$Files   = @("$Name.dll", "bin\64bit\$Name.dll", 'data\handcam-pin.effect', 'data\editor.html')
$Session = (Get-Process -Id $PID).SessionId
$PortableRunning = @{}   # lower-cased OBS folders running with --portable

function Say([string]$Text, [string]$Color = 'Gray') { Write-Host $Text -ForegroundColor $Color }

# Single-quoted PowerShell literal for the elevated helper script. PowerShell also
# treats the typographic quotes U+2018..U+201B as single quotes, so double those too.
function Quote([string]$s) { "'" + ($s -replace '([\u0027\u2018\u2019\u201A\u201B])', '$1$1') + "'" }

# OBS processes of this Windows session (another signed-in user's OBS doesn't matter).
function Get-OwnObs { @(Get-Process obs64 -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $Session }) }

# Same marker files OBS checks, its config folder (only a portable OBS keeps one
# next to its program files), or a running OBS started with --portable / -p.
function Test-Portable([string]$dir) {
    foreach ($f in 'portable_mode', 'obs_portable_mode', 'portable_mode.txt', 'obs_portable_mode.txt', 'config\obs-studio') {
        if (Test-Path -LiteralPath (Join-Path $dir $f)) { return $true }
    }
    return $PortableRunning.ContainsKey($dir.ToLowerInvariant())
}

# CPU type of an .exe from its PE header: 0x8664 = x64, 0xAA64 = ARM64.
function Get-ExeMachine([string]$exe) {
    try {
        $fs = [IO.File]::OpenRead($exe)
        try {
            $br = New-Object IO.BinaryReader($fs)
            $fs.Position = 0x3C; $fs.Position = $br.ReadInt32() + 4
            return [int]$br.ReadUInt16()
        } finally { $fs.Close() }
    } catch { return 0 }
}

# OBS install folders: the registry key (written by the OBS installer and Steam),
# Steam libraries, the default folder, and wherever OBS is running from right now.
function Get-ObsDirs([string[]]$extra) {
    $dirs = @($extra)
    foreach ($view in 'Registry64', 'Registry32') {
        try {
            $key = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $view).OpenSubKey('SOFTWARE\OBS Studio')
            if ($key) { $dirs += [string]$key.GetValue(''); $key.Close() }
        } catch {}
    }
    $steam = $null
    try { $steam = [string](Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch {}
    if ($steam) {
        $libs = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            $text = Get-Content -LiteralPath $vdf -Raw -Encoding UTF8   # Steam writes UTF-8 without BOM
            foreach ($m in [regex]::Matches($text, '"path"\s+"([^"]+)"')) {
                $libs += $m.Groups[1].Value -replace '\\\\', '\'
            }
        }
        foreach ($lib in $libs) { $dirs += Join-Path $lib 'steamapps\common\OBS Studio' }
    }
    $dirs += Join-Path $env:ProgramFiles 'obs-studio'

    $found = @{}
    foreach ($d in $dirs) {
        if (-not $d) { continue }
        try { $full = [IO.Path]::GetFullPath($d) } catch { continue }
        if ($full.Length -gt 3) { $full = $full.TrimEnd('\') }   # keep "D:\" a root, not "D:"
        if (Test-Path -LiteralPath (Join-Path $full 'bin\64bit\obs64.exe')) { $found[$full.ToLowerInvariant()] = $full }
    }
    return @($found.Values)
}

function Install-HandcamPin {
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'OBS plugins need 64-bit Windows.' }
    $label = if ($Version.StartsWith('@')) { 'Handcam Pin' } else { "Handcam Pin $Version" }
    Say "Installing $label..." White

    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("$Name-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        # 1. Get the release zip.
        if ($env:HANDCAM_PIN_ZIP) {
            $zip = (Resolve-Path -LiteralPath $env:HANDCAM_PIN_ZIP).Path
            Say "Using $zip"
        } elseif ($Version.StartsWith('@')) {
            throw 'this is the unreleased script from the repository. Use the one-liner from the README, or set $env:HANDCAM_PIN_ZIP to a release zip.'
        } else {
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            $url = "https://github.com/$Repo/releases/download/v$Version/$Name-$Version-windows.zip"
            $zip = Join-Path $tmp "$Name.zip"
            Say "Downloading $url"
            try { Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing }
            catch { throw "download failed ($($_.Exception.Message)). Check your internet connection and try again." }
            if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $Sha256) {
                throw 'the download is corrupted (checksum mismatch). Try again.'
            }
        }

        # 2. Unpack and check every file is there before touching anything.
        $unpacked = Join-Path $tmp 'unpacked'
        Expand-Archive -LiteralPath $zip -DestinationPath $unpacked
        $src = Join-Path $unpacked $Name
        foreach ($f in $Files) {
            if (-not (Test-Path -LiteralPath (Join-Path $src $f))) {
                throw "$zip is not a Handcam Pin Windows release zip, or one too old for this installer."
            }
        }

        # 3. Only now, with the files ready, ask to close OBS (it locks the DLL).
        #    Remember where a running OBS lives, and whether it runs portable, first.
        $running = @(Get-OwnObs | ForEach-Object {
            try { Split-Path (Split-Path (Split-Path $_.Path)) } catch {}
        })
        try {
            foreach ($p in @(Get-CimInstance Win32_Process -Filter "Name = 'obs64.exe'" -OperationTimeoutSec 5 -ErrorAction Stop)) {
                if ($p.ExecutablePath -and $p.CommandLine -cmatch '\s(--portable|-p)(\s|$)') {   # case-sensitive, like OBS
                    $PortableRunning[(Split-Path (Split-Path (Split-Path $p.ExecutablePath))).ToLowerInvariant()] = $true
                }
            }
        } catch {}
        if (Get-OwnObs) {
            Say 'OBS is running. Close OBS (File > Exit) to continue, or press Ctrl+C to cancel.' Yellow
            while (Get-OwnObs) { Start-Sleep -Milliseconds 500 }
            Start-Sleep -Seconds 1   # give Windows a moment to release the plugin DLL
        }

        $obsDirs = Get-ObsDirs $running
        if (-not $obsDirs) {
            Say 'Could not find OBS Studio on this PC. Installing anyway; install OBS 31 or newer to use it.' Yellow
        }
        foreach ($dir in $obsDirs) {
            $exe = Join-Path $dir 'bin\64bit\obs64.exe'
            $ver = (Get-Item -LiteralPath $exe).VersionInfo
            $kind = if (Test-Portable $dir) { 'portable OBS' } else { 'OBS' }
            Say "Found $kind $($ver.ProductVersion) in $dir"
            if ($ver.FileMajorPart -lt $MinObs) {
                Say "  Handcam Pin needs OBS $MinObs or newer. Update OBS, or the plugin won't load." Yellow
            }
            if ((Get-ExeMachine $exe) -eq 0xAA64) {
                Say '  This is the ARM64 build of OBS, which cannot load Handcam Pin (x64 only so far). The x64 build of OBS works.' Yellow
            }
            if ($kind -eq 'portable OBS') {
                Say '  Portable OBS ignores C:\ProgramData. See "Portable OBS" in the README to install there.' Yellow
            }
        }

        # 4. Old copies in OBS program folders (e.g. from copying the zip there by
        #    hand). OBS 31/32 may load such a copy instead of ours; OBS 33 loads
        #    ours first and skips it, but an old copy is never what anyone wants.
        $stale = @()
        foreach ($dir in $obsDirs) {
            if (Test-Portable $dir) { continue }   # a portable OBS's own copy is the one it uses
            $stale += Join-Path $dir 'obs-plugins\64bit\handcam-pin.dll'
            $stale += Join-Path $dir 'obs-plugins\64bit\handcam-pin.pdb'
            $stale += Join-Path $dir 'data\obs-plugins\handcam-pin'
        }
        $stale = @($stale | Where-Object { Test-Path -LiteralPath $_ })

        # 5. Replace the plugin folder, then remove the old copies. Whatever fails
        #    for lack of rights is retried in one elevated PowerShell (one UAC prompt).
        $base = Join-Path $env:ProgramData 'obs-studio\plugins'
        $dest = Join-Path $base $Name
        $needAdmin = $false
        try {
            if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
            New-Item -ItemType Directory -Path $base -Force | Out-Null
            Copy-Item -LiteralPath $src -Destination $base -Recurse -Force
        } catch {
            # Usually a folder an elevated installer created. Step 6 verifies the retry.
            $needAdmin = $true
        }
        $staleLeft = @()
        foreach ($p in $stale) {
            try { Remove-Item -LiteralPath $p -Recurse -Force } catch { $staleLeft += $p }
        }

        if ($needAdmin -or $staleLeft) {
            Say 'Administrator rights are needed to finish. Windows will ask you to allow this.' Yellow
            foreach ($p in $staleLeft) { Say "  remove old copy: $p" }
            $lines = @('$ErrorActionPreference = ''Stop''')
            if ($needAdmin) {
                $lines += "if (Test-Path -LiteralPath $(Quote $dest)) { Remove-Item -LiteralPath $(Quote $dest) -Recurse -Force }"
                $lines += "New-Item -ItemType Directory -Path $(Quote $base) -Force | Out-Null"
                $lines += "Copy-Item -LiteralPath $(Quote $src) -Destination $(Quote $base) -Recurse -Force"
            }
            foreach ($p in $staleLeft) { $lines += "Remove-Item -LiteralPath $(Quote $p) -Recurse -Force" }
            $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($lines -join "`n"))
            $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
            try {
                Start-Process $powershell -Verb RunAs -Wait -ArgumentList '-NoProfile', '-EncodedCommand', $encoded
            } catch {
                # Declined. Without the plugin folder that's fatal; for old copies, step 6 lists them.
                if ($needAdmin) { throw 'administrator rights were declined. Run the command again and click Yes.' }
                Say 'Administrator rights were declined, so the old copies stay.' Yellow
            }
        }

        # 6. Check the result rather than trusting the steps above.
        foreach ($f in $Files) {
            if (-not (Test-Path -LiteralPath (Join-Path $dest $f))) { throw "$dest\$f is missing after copying. Run the command again." }
        }
        $still = @($stale | Where-Object { Test-Path -LiteralPath $_ })
        if ($still) {
            Say 'Could not remove these old copies. Delete them by hand, or OBS may keep loading the old version:' Yellow
            foreach ($p in $still) { Say "  $p" Yellow }
        }
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }

    Say ''
    Say "$label is installed in $dest" Green
    Say ''
    Say 'Next:' White
    Say '  1. Start OBS.'
    Say '  2. Right-click your webcam source > Filters > + > Handcam Pin.'
    Say '  3. Tools > WebSocket Server Settings > tick "Enable WebSocket server".'
    Say '  4. In the filter, click "Open editor" and drag the corners onto your mousepad.'
    Say ''
    Say 'To update later, run the same command again. To uninstall, delete the folder above.'
}

try {
    Install-HandcamPin
} catch {
    Say ''
    Say "Handcam Pin was not installed: $($_.Exception.Message)" Red
}
}
