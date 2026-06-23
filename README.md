# 1oom-mp — Master of Orion 1 with online multiplayer

A fork of [1oom](https://github.com/1oom-fork/1oom) that adds **simultaneous-turn online
multiplayer** — with **team play** — plus a handful of modern conveniences, to the 1993 classic
*Master of Orion*. 1oom is a faithful, from-scratch GPLv2 reimplementation of the original DOS
engine; this fork leaves single-player untouched and layers networked play on top.

> **Status:** work in progress, actively playtested. Single-player is upstream 1oom; the
> multiplayer is new and still evolving. Expect rough edges.

---

## Features

**Multiplayer**
- **Authoritative headless server + SDL2 clients**, with **simultaneous turns** — everyone plans at
  once and the server resolves the turn once all players are ready. An optional **per-turn timer**
  keeps games moving.
- **Pre-game lobby** — pick your race and color, choose teams, and (host) set AI count, galaxy size,
  and difficulty, then ready up.
- **Team play** — put two or more players on the same team for a **locked alliance** (you can't war
  or spy on each other), **shared vision**, and a **shared victory**. Works for human teams,
  human+AI teams, and all-AI teams.
- **Live human-to-human diplomacy** — the full audience menu against real players: treaties
  (non-aggression / alliance / peace), declare war, break treaties, trade agreements, and
  **technology exchange**.
- **Interactive combat** — you fly your own ships in battle; the server runs the fight and relays it
  to the players involved, and teammates can **watch** a teammate's battle live.
- **Team coordination overlays** — see teammates' in-progress plans (planned fleets, colonization,
  live world stats) during planning, and drop **map beacons** to flag targets for your allies.
- **Autosave & crash recovery** — the server saves every turn, so a dropped or crashed game can be
  resumed with `-mpload`.

**Conveniences (single- and multiplayer)**
- **Smooth galaxy map** — click-and-drag panning and continuous zoom-to-cursor.
- **Bigger galaxies** — the **Enormous** and **Galactic** sizes are selectable (in addition to the
  stock Small–Huge).

---

## Setting up a multiplayer match

The fastest path depends on your group. Everyone needs a working client, a copy of the
[game data](#game-data), and to be running the **same build**.

### 1. Get a client

- **Build from source** (macOS / Linux — see [Building](#building)), **or**
- **Windows, no build:** the repo includes `build-win.sh`, which cross-compiles an
  *unzip-and-double-click* Windows bundle (`1oom-mp.zip`). Its **`Play Together.bat`** launcher just
  asks **Host or Join** — no command line needed. (Building the bundle needs an `llvm-mingw` +
  `SDL2-mingw` cross-toolchain; the resulting zip is what you hand to friends.)

### 2. One person hosts

The host runs the headless server **and** joins it with their own client.

```sh
# Terminal 1 — the server (waits for 2 humans; any extra empires are AI)
1oom_server -mphost 24695 -mphumans 2 -data /path/to/moo-data

# Terminal 2 — the host's own client
1oom_classic_sdl2 -mpjoin 127.0.0.1:24695 -data /path/to/moo-data
```

Then share your address with the other players (the host's machine must be reachable on the port —
see [Connecting](#connecting-over-the-network)).

### 3. Everyone else joins

```sh
1oom_classic_sdl2 -mpjoin <host-address>:24695 -data /path/to/moo-data
```

On Windows with the bundle: double-click **`Play Together.bat`**, press **J**, and type the host's
address. (To host from the bundle instead, press **H** — it starts the server, shows your IP
addresses, and waits for one player to join.)

### 4. In the lobby

Once everyone's connected you all land in the shared lobby:

- **Each player** picks a **Race** and **Color**, and clicks their **team chip** to choose a team
  (it cycles `FFA → T1 → T2 …`). Players on the **same team number** start allied with shared sight.
- **The host** (the first slot) sets the number of **AI opponents**, the **galaxy size** and
  **difficulty**, and the optional **turn timer**.
- Everyone clicks **Ready**; the game begins.

### 5. Playing

Turns are **simultaneous**: everyone plans at the same time, and locking in your turn ("Next Turn")
readies you. The server resolves the turn once all players are ready, then plays back movement and
combat. Handy in-game controls:

| Action | Control |
|---|---|
| Pan the galaxy map | click-and-drag |
| Zoom | scroll / trackpad over the map (zoom-to-cursor) |
| Beacon a world for your team | focus a star, press **B** (up to 3; press **B** again to clear) |
| Diplomacy with another player | the **audience** menu, same as vs. the AI |

### Quick local test (one machine)

You can run the whole thing on a single computer to try it out — start the server and **two** clients
(alt-tab between the windows). `-uiscale N` shrinks the windows so they fit side by side:

```sh
1oom_server -mphost 24695 -mphumans 2 -data /path/to/moo-data &
1oom_classic_sdl2 -uiscale 2 -mpjoin 127.0.0.1:24695 -data /path/to/moo-data &
1oom_classic_sdl2 -uiscale 2 -mpjoin 127.0.0.1:24695 -data /path/to/moo-data &
```

### Connecting over the network

Any TCP reachability between the players and the host works:
- **Same LAN:** use the host's local `192.168.x.x` address.
- **Over the internet:** a mesh VPN such as [Tailscale](https://tailscale.com) is easiest — every
  machine gets a stable `100.x.x.x` address and **no port-forwarding** is needed. Otherwise forward
  the port on the host's router, or run `1oom_server` on a public-IP box / VPS.
- **Dedicated server:** `1oom_server` is headless (no SDL or audio), so it runs fine on a minimal
  Linux VPS — build it, copy the game data up, open the port, and have everyone connect as clients.

### Saving & resuming

The server writes a per-turn autosave (`mp_autosave.blob`) to your 1oom user directory, and a player
can trigger a named save in-game (**Esc → Save**). Resume any of them by passing the file to the
server with `-mpload`:

```sh
1oom_server -mphost 24695 -mpload ~/.config/1oom/mp_autosave.blob -data /path/to/moo-data
```

The human count is baked into the save, so `-mphumans` isn't needed when resuming. Everyone rejoins
as before.

> **Same build, both ends.** Multiplayer syncs game state as raw structures, so the server and every
> client must be built from the **same commit**. A version handshake rejects mismatches at join — if
> someone can't connect, have them pull and rebuild.

---

## Building

You need a C toolchain, the autotools, and SDL2 (for the graphical client). The original Master of
Orion **game data is not included** — see [Game data](#game-data).

**macOS (Homebrew):**
```sh
brew install sdl2 sdl2_mixer libsamplerate autoconf automake pkg-config
autoreconf -i && ./configure && make -j4
```

**Debian / Ubuntu:**
```sh
sudo apt install build-essential libsdl2-dev libsdl2-mixer-dev libsamplerate0-dev \
                 autoconf automake pkg-config
autoreconf -i && ./configure && make -j4
```

This produces, under `src/`:
- `1oom_server` — the headless multiplayer server (no SDL or audio; ideal for a dedicated box)
- `1oom_classic_sdl2` — the graphical client
- `1oom_cmdline` — a text client

See [`COMPILING`](COMPILING) for the full list of `./configure` options.

## Game data

1oom is only the engine — it needs the original **Master of Orion (1993, v1.3)** data files (the
`*.LBX` files) from a legitimately-owned copy of the game. Those assets are copyrighted and are
**not** included in this repository.

**Where to get them:** install an original copy of MOO1 — for example GOG's *Master of Orion 1+2*
bundle. The `*.LBX` files live in the game's install directory.

**Pointing the engine at them:** pass the folder that contains the `.LBX` files with `-data` (every
binary — server, client, tools — takes the same flag):

```sh
1oom_classic_sdl2 -data "/path/to/Master of Orion" -mpjoin <host>:24695
1oom_server       -data "/path/to/Master of Orion" -mphost 24695 -mphumans 2
```

Alternatively, run the binary from *inside* the MOO1 directory (it looks there automatically), or
copy the `1oom_*` executables and their DLLs into that directory. All players in a multiplayer game
must use the **same data version (1.3)**. See upstream
[1oom](https://github.com/1oom-fork/1oom) for more on locating the files.

> The Windows `1oom-mp.zip` bundle ships a `data/` folder so the person you hand it to can play
> without locating the files themselves — share it only with people who own the game; **don't post
> it publicly**, since it contains those copyrighted assets.

## Credits & license

- A fork of **1oom** by [1oom-fork](https://github.com/1oom-fork/1oom) and contributors. See
  [`README.1oom.md`](README.1oom.md) for the upstream project's notes and version history.
- *Master of Orion* © 1993 SimTex / MicroProse. This project ships **no** original game assets.
- Free Software under the **GNU General Public License v2** — see [`COPYING`](COPYING).
