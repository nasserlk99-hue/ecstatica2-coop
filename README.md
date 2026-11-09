# Ecstatica Co-op

Local two-player co-op development for **Ecstatica II (1997)** and
**Ecstatica (1994)**, built on the open-source
[Ecstatica decompilation and port](https://github.com/xesf/ecstatica). One
executable runs both games; the version is detected from the installed data.

This repository contains source code and build tooling only. It does **not**
include the game data — you need a legitimate installed copy of Ecstatica or
Ecstatica II to play. No game files are tracked here; `data/` is ignored.

## Features

- Two heroes on one PC, sharing one game world and camera.
- Player 1 uses WASD and the left-hand keys; Player 2 uses the arrow keys or
  numpad.
- Both E1 heroes always run — no run key needed.
- Mutual idle-follow: if one hero is idle for 1.5 s while the other is playing,
  the idle hero follows and stops 300 units away (resumes at 500). If both are
  idle, both stand still. Any input returns control immediately.
- In-game **Controls** menu for remapping both players' movement and action
  keys. Controller remapping is not implemented yet.
- Optional split-screen view, off by default.

## Controls

| Action | Player 1 | Player 2 |
|---|---|---|
| Move / turn | W A S D | Arrow keys or Numpad 8/2/4/6 |
| Interact / pick up | Space | Numpad 5 |
| Left hand pick up / drop | E | Numpad 1 |
| Right hand pick up / drop | F | Numpad 3 |
| Fire (pistol shot) | V | Numpad - |
| Left swing | R | Numpad 7 |
| Right swing | C | Numpad 9 |
| Jump | Left Ctrl | Numpad . |
| Use held item / alt | Left Alt | Numpad + |

Full key list and gamepad mappings: [docs/controls.md](docs/controls.md).

## Build on Windows

Requirements: CMake, Visual Studio 2022 or newer, and the original game data.
The current development build uses the Visual Studio 18 2026 generator:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

The executable is written to `build\bin\Release\ecstatica.exe`. Copy it into
your Ecstatica installation folder and run it from there.

For the software renderer used by the current play build, set this in
`ecstatica.cfg`:

```ini
renderer = 0
```

## Co-op switches

| Variable | Effect |
|---|---|
| `ECSTATICA_COOP=0` | Disable local co-op |
| `ECSTATICA_COOP_NO_FOLLOW=1` | Disable idle auto-follow |
| `ECSTATICA_COOP_TRACE=1` | Write player/follow state to the debug log |
| `ECSTATICA_SPLIT_SCREEN=1` | Opt in to split screen (hardware renderer + 3D map) |

## Upstream and credits

This project is based on the [Ečstatica decompilation](https://github.com/xesf/ecstatica)
by its authors. The upstream project provides the decompilation, portable C99
port and platform layers; this repository adds and tests the local co-op work.

Upstream is credited here rather than presented as this repository's front
page. The original upstream README is kept at
[UPSTREAM-README.md](UPSTREAM-README.md) for reference.

## License

The source code is licensed under the GNU General Public License v3 — see
[LICENSE](LICENSE). Ecstatica and its game data remain the property of their
respective rights holders and are not distributed with this repository.
