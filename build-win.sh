#!/bin/bash
# Cross-compile the 1oom-mp fork into a Windows "unzip-and-play" bundle, from macOS.
#
# Output: <workspace>/win-bundle/Surprise.zip   (unzip -> double-click a .bat to play)
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
BUNDLE="$OUTDIR/Surprise"

[ -n "$LLVM" ]   && [ -d "$LLVM" ]   || { echo "ERROR: llvm-mingw toolchain not found in $ROOT (expected llvm-mingw-*-macos-universal)"; exit 1; }
[ -n "$SDLDIR" ] && [ -d "$SDLDIR" ] || { echo "ERROR: SDL2-mingw not found in $ROOT (expected sdl2-mingw/SDL2-*/x86_64-w64-mingw32)"; exit 1; }
[ -d "$DATA" ]   || { echo "ERROR: game data dir not found: $DATA"; exit 1; }
export PATH="$LLVM/bin:$SDLDIR/bin:$PATH"

# 1. fresh copy of the source (picks up uncommitted fixes; does NOT disturb the Mac build)
rsync -a --exclude='.git' --exclude='build-win' --exclude='build-win.sh' "$SRC/" "$WORK/"
make -C "$WORK" distclean >/dev/null 2>&1 || true

# 2. cross-configure: audio ON, no SDL1/X11, link Winsock (ws2_32 -- net.c needs it on Windows)
( cd "$WORK" && sdl2_config="$SDLDIR/bin/sdl2-config" ./configure --host=x86_64-w64-mingw32 \
    --disable-hwsdl1 --disable-hwx11 LIBS="-lws2_32" )

# 3. build (the SDL2 client AND the headless server)
make -C "$WORK"

# 4. assemble the bundle. Obscured names kept from the original "gift": exe -> Play.exe, server ->
#    Server.exe, data -> data, folder -> Surprise.
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
set /p "ip=Enter the host's address (just the IP, e.g. 100.125.140.3): "
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
perl -pi -e 's/(?<!\r)\n/\r\n/g' "$BUNDLE/"*.bat   # Windows (CRLF) line endings

# 5. zip
( cd "$OUTDIR" && rm -f Surprise.zip && zip -r -9 -q Surprise.zip Surprise )
echo "Built: $OUTDIR/Surprise.zip"
