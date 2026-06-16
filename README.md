# 1oom-mp — Master of Orion 1 with online multiplayer

A fork of [1oom](https://github.com/1oom-fork/1oom) that adds **simultaneous-turn network
multiplayer** (plus a few modern conveniences) to the 1993 classic *Master of Orion*. 1oom is a
faithful, from-scratch GPLv2 reimplementation of the original DOS engine; this fork leaves the
single-player game intact and layers networked play on top.

> **Status:** work in progress, actively playtested. Single-player is upstream 1oom; the
> multiplayer is new and still evolving. Expect rough edges.

## Multiplayer features

- **Authoritative headless server** + SDL2 clients, with **simultaneous-turn** play — everyone
  plans at once and the server resolves the turn when all players are ready.
- **Pre-game lobby**: pick race and color, host chooses AI count and galaxy size, everyone readies up.
- **Live human-to-human diplomacy** mirroring the AI audience menu — propose treaties
  (non-aggression / alliance / peace), declare war, break a treaty, **form trade agreements**,
  **exchange technology**, and break trade.
- **Interactive combat**: you control your own ships in battle; the server runs the fight and
  relays it to the players involved.
- **Result playback**: galaxy-map fleet-movement animation, ground-invasion animations (shown to
  both attacker and defender), and orbital-bombing results.
- **Autosave & crash recovery**: the server saves every turn, so a dropped or crashed game can be
  resumed with `-mpload`.
- **Cross-platform networking**: the server and client build on macOS and Linux today, and the
  socket layer includes Winsock support so the client can be built for Windows.

## Building

You need a C toolchain and SDL2 (for the graphical client). The original Master of Orion **game
data is not included** — see [Game data](#game-data).

**macOS (Homebrew):**
```sh
brew install sdl2 sdl2_mixer libsamplerate autoconf automake pkg-config
./configure && make -j4
```

**Debian / Ubuntu:**
```sh
sudo apt install build-essential libsdl2-dev libsdl2-mixer-dev libsamplerate0-dev \
                 autoconf automake pkg-config
./configure && make -j4
```

This produces, under `src/`:
- `1oom_server` — the headless multiplayer server (no SDL or audio; ideal for a dedicated box)
- `1oom_classic_sdl2` — the graphical client
- `1oom_cmdline` — a text client

## Game data

1oom needs the data files from an original **Master of Orion (1993)** installation (for example a
GOG copy) — the `*.LBX` files. These are copyrighted and are **not** distributed here. Point any
binary at them with `-data <dir>`. See upstream [1oom](https://github.com/1oom-fork/1oom) for help
locating the data.

## Playing multiplayer

Every player must run the **same build** and use the **same game data**.

**Host** — runs the authoritative server (a player's machine or a dedicated box):
```sh
1oom_server -mphost 24695 -mphumans 2 -data /path/to/moo-data
```
- `-mphost <port>` — host a game on `<port>`.
- `-mphumans <N>` — number of human players to wait for; the remaining `-new` empires are AI.
- `-mpload <file>` — resume a saved game instead of starting a new one. The per-turn autosave is
  written to your 1oom user directory as `mp_autosave.blob`.

**Join** — every player (including the host) runs a client:
```sh
1oom_classic_sdl2 -mpjoin <host-address>:24695 -data /path/to/moo-data
```
Use `127.0.0.1` for a local test, or the host's reachable address. For internet play any TCP
reachability works — a mesh VPN such as [Tailscale](https://tailscale.com) is the easiest (no
port-forwarding required), or run the headless server on a public-IP host.

### Dedicated server

Because `1oom_server` is headless, it runs fine on a minimal Linux VPS: build the server, copy your
game data up, open the port, and have all players connect as clients.

## Credits & license

- A fork of **1oom** by [1oom-fork](https://github.com/1oom-fork/1oom) and contributors. See
  [`README.1oom.md`](README.1oom.md) for the upstream project's notes and version history.
- *Master of Orion* © 1993 SimTex / MicroProse. This project ships **no** original game assets.
- Free Software under the **GNU General Public License v2** — see [`COPYING`](COPYING).
