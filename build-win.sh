#!/bin/bash
# Cross-compile the 1oom-mp fork into a Windows "unzip-and-play" bundle, from macOS.
#
# Output: <workspace>/win-bundle/1oom-mp.zip   (unzip -> double-click a .bat to play)
#   - "Play Together.bat" : interactive multiplayer launcher -- choose HOST or JOIN (no hardcoded IP).
#       HOST shows your IP addresses, starts a server + your client, waits for the other player.
#       JOIN asks for the host's IP and connects.
#   - "Play Solo.bat"     : single-player.
#
# Lives in the git repo (engines/fork) so the packaging process is versioned. Paths are derived from this
# script's location: the cross toolchain is expected one level above the repo, in the workspace root:
#   <workspace>/llvm-mingw-*-macos-universal      (llvm-mingw)
#   <workspace>/sdl2-mingw/SDL2-*/x86_64-w64-mingw32   (SDL2 with SDL2_mixer merged in)
# Run it from anywhere:  bash engines/fork/build-win.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # the repo (engines/fork)
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"                      # workspace root (holds the toolchain + game data)

LLVM="$(ls -d "$ROOT"/llvm-mingw-*-macos-universal 2>/dev/null | head -1 || true)"
SDLDIR="$(ls -d "$ROOT"/sdl2-mingw/SDL2-*/x86_64-w64-mingw32 2>/dev/null | head -1 || true)"
SRC="$SCRIPT_DIR"                          # live source = this repo (picks up uncommitted fixes)
WORK="$ROOT/fork-win"                      # throwaway cross-build copy
DATA="$ROOT/game"                          # game data (LBX etc.)
OUTDIR="$ROOT/win-bundle"
BUNDLE="$OUTDIR/1oom-mp"

# toolchain: use a cross-gcc already on PATH if there is one (CI: apt gcc-mingw-w64-x86-64),
# else the workspace llvm-mingw (the macOS setup)
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    LLVMBIN=""
else
    [ -n "$LLVM" ] && [ -d "$LLVM" ] || { echo "ERROR: no x86_64-w64-mingw32-gcc on PATH and no llvm-mingw in $ROOT (expected llvm-mingw-*-macos-universal)"; exit 1; }
    LLVMBIN="$LLVM/bin:"
fi
[ -n "$SDLDIR" ] && [ -d "$SDLDIR" ] || { echo "ERROR: SDL2-mingw not found in $ROOT (expected sdl2-mingw/SDL2-*/x86_64-w64-mingw32)"; exit 1; }
[ -d "$DATA" ]   || { echo "ERROR: game data dir not found: $DATA"; exit 1; }
export PATH="${LLVMBIN}$SDLDIR/bin:$PATH"

# 1. fresh copy of the source (picks up uncommitted fixes; does NOT disturb the Mac build)
rsync -a --exclude='.git' --exclude='build-win' --exclude='build-win.sh' "$SRC/" "$WORK/"
make -C "$WORK" distclean >/dev/null 2>&1 || true
[ -x "$WORK/configure" ] || ( cd "$WORK" && autoreconf -i )   # fresh git checkout (CI) has no generated configure

# 2. cross-configure: audio ON, no SDL1/X11, link Winsock (ws2_32 -- net.c needs it on Windows)
( cd "$WORK" && sdl2_config="$SDLDIR/bin/sdl2-config" ./configure --host=x86_64-w64-mingw32 \
    --disable-hwsdl1 --disable-hwx11 LIBS="-lws2_32" )

# 3. build (the SDL2 client AND the headless server)
make -C "$WORK"

# 4. assemble the bundle: SDL2 client as Play.exe, headless server as Server.exe, the game data,
#    plus the .bat launchers.
rm -rf "$BUNDLE"; mkdir -p "$BUNDLE"
cp "$WORK/src/1oom_classic_sdl2.exe" "$BUNDLE/Play.exe"
cp "$WORK/src/1oom_server.exe"       "$BUNDLE/Server.exe"   # needed for the HOST option
cp "$SDLDIR/bin/SDL2.dll" "$SDLDIR/bin/SDL2_mixer.dll" "$BUNDLE/"   # imported by name; cannot rename
cp -R "$DATA" "$BUNDLE/data"

# 4a. interactive multiplayer launcher -- HOST or JOIN, no hardcoded IP.
cat > "$BUNDLE/Play Together.bat" <<'BAT'
@echo off
setlocal enabledelayedexpansion
title Master of Orion - Multiplayer
:menu
cls
echo.
echo    ============================================
echo      MASTER OF ORION  --  MULTIPLAYER
echo    ============================================
echo.
echo      [H]  Host a game   -  let someone join you
echo      [J]  Join a game   -  connect to a host
echo.
set "mode="
set /p "mode=Type H to host or J to join, then press Enter: "
if /i "%mode%"=="H" goto host
if /i "%mode%"=="J" goto join
goto menu

:host
cls
echo.
echo    Give ONE of these addresses to the other player:
echo.
powershell -NoProfile -Command "Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -ne '127.0.0.1' -and $_.IPAddress -notlike '169.254.*' } | Sort-Object @{Expression={$_.IPAddress -like '100.*'}} -Descending | ForEach-Object { '        ' + $_.IPAddress.PadRight(18) + ' (' + $_.InterfaceAlias + ')' }"
echo.
echo    A 100.x.x.x address is your Tailscale / VPN one - use that to play over the internet.
echo.
echo    Starting the game and waiting for 1 player to join...
start "MOO Server - leave this window open while playing" /min "%~dp0Server.exe" -mphost 24695 -mphumans 2 -data "%~dp0data" -log "%~dp0server-log.txt"
"%~dp0Play.exe" -mpjoin 127.0.0.1:24695 -data "%~dp0data"
taskkill /f /im Server.exe >nul 2>&1
goto end

:join
cls
echo.
set "ip="
set /p "ip=Enter the host's address (just the IP, e.g. 100.x.x.x): "
if "%ip%"=="" goto join
echo.
echo    Connecting to %ip% ...
"%~dp0Play.exe" -mpjoin %ip%:24695 -data "%~dp0data"
goto end

:end
endlocal
BAT

cat > "$BUNDLE/Play Solo.bat" <<'BAT'
@echo off
start "" "%~dp0Play.exe" -data "%~dp0data"
BAT

# 4b. self-updater. Update.bat is a tiny STABLE wrapper (same bytes in every version, so the
#     update overwriting it mid-run is harmless); the real logic is Update.ps1, which PowerShell
#     loads fully into memory before executing (also safe to overwrite mid-run). Two flavors:
#     - PRIVATE (preferred): ~/.config/1oom/release_token_ro baked in; pulls the latest release
#       from the private distribution repo via the API.
#     - PUBLIC fallback (no token file): the public-releases URL on the code repo.
#     Only exes/dlls/bats/ps1 are replaced -- the data/ folder is never touched.
cat > "$BUNDLE/Update.bat" <<'BAT'
@echo off
title Master of Orion - Update
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Update.ps1"
pause
BAT
TOKEN_RO_FILE="$HOME/.config/1oom/release_token_ro"
if [ -f "$TOKEN_RO_FILE" ]; then
cat > "$BUNDLE/Update.ps1" <<'PS1'
# 1oom-mp updater: fetch the latest release from the private repo and install it in place.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try {
    $t = '__RELEASE_TOKEN__'
    $dir = $PSScriptRoot
    Write-Host 'Downloading the latest version...'
    $h = @{ Authorization = ('Bearer ' + $t) }
    $r = Invoke-RestMethod -Headers $h -Uri 'https://api.github.com/repos/jacob-t-radford/1oom-mp-releases/releases/latest'
    $u = ($r.assets | Where-Object name -eq '1oom-mp-update.zip').url
    $zip = Join-Path $env:TEMP '1oom-mp-update.zip'
    & curl.exe -sL -H ('Authorization: Bearer ' + $t) -H 'Accept: application/octet-stream' -o $zip $u
    if (-not (Test-Path $zip)) { throw 'download failed (no internet?)' }
    Write-Host ('Installing ' + $r.tag_name + ' ...')
    Stop-Process -Name Play, Server -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    try {
        Expand-Archive -Force -Path $zip -DestinationPath $dir
    } catch {
        Write-Host 'A file was still in use -- retrying...'
        Start-Sleep -Seconds 3
        Expand-Archive -Force -Path $zip -DestinationPath $dir
    }
    Remove-Item $zip -ErrorAction SilentlyContinue
    Write-Host ''
    Write-Host ('Updated to ' + $r.tag_name + '!  Double-click Play.exe to play.')
} catch {
    Write-Host ''
    Write-Host ('Update failed: ' + $_.Exception.Message)
    Write-Host 'Is the game still running? Close it and run Update.bat again.'
    exit 1
}
PS1
perl -pi -e "s/__RELEASE_TOKEN__/$(tr -d ' \t\r\n' < "$TOKEN_RO_FILE")/" "$BUNDLE/Update.ps1"
else
echo "NOTE: no $TOKEN_RO_FILE -- Update.ps1 will use the PUBLIC releases URL (family updates need the private-repo token)"
cat > "$BUNDLE/Update.ps1" <<'PS1'
# 1oom-mp updater: fetch the latest public release and install it in place.
$ErrorActionPreference = 'Stop'
try {
    $dir = $PSScriptRoot
    Write-Host 'Downloading the latest version...'
    $zip = Join-Path $env:TEMP '1oom-mp-update.zip'
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri 'https://github.com/jacob-t-radford/1oom-mp/releases/latest/download/1oom-mp-update.zip' -OutFile $zip
    Write-Host 'Installing...'
    Stop-Process -Name Play, Server -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    try {
        Expand-Archive -Force -Path $zip -DestinationPath $dir
    } catch {
        Write-Host 'A file was still in use -- retrying...'
        Start-Sleep -Seconds 3
        Expand-Archive -Force -Path $zip -DestinationPath $dir
    }
    Remove-Item $zip -ErrorAction SilentlyContinue
    Write-Host ''
    Write-Host 'Updated!  Double-click Play.exe to play.'
} catch {
    Write-Host ''
    Write-Host ('Update failed: ' + $_.Exception.Message)
    Write-Host 'Is the game still running? Close it and run Update.bat again.'
    exit 1
}
PS1
fi
perl -pi -e 's/(?<!\r)\n/\r\n/g' "$BUNDLE/"*.bat "$BUNDLE/"*.ps1   # Windows (CRLF) line endings

# 5. zip: the full unzip-and-play bundle (with data -- hand to people who own the game, don't post),
#    and the NO-DATA update zip (safe to publish as a GitHub release; Update.bat downloads it).
( cd "$OUTDIR" && rm -f 1oom-mp.zip && zip -r -9 -q 1oom-mp.zip 1oom-mp )
( cd "$BUNDLE" && rm -f "$OUTDIR/1oom-mp-update.zip" && zip -9 -q "$OUTDIR/1oom-mp-update.zip" Play.exe Server.exe SDL2.dll SDL2_mixer.dll "Play Together.bat" "Play Solo.bat" Update.bat Update.ps1 )
echo "Built: $OUTDIR/1oom-mp.zip (full bundle) + $OUTDIR/1oom-mp-update.zip (publishable update)"
