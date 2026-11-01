# q1psx — Quake for the PlayStation 1

A *from-scratch* port of **Quake** to the
PlayStation 1, written from the original
[`id-Software/Quake`](https://github.com/id-Software/Quake) engine and
[`PSn00bSDK`](https://github.com/Lameguy64/PSn00bSDK) only.

> See [`CHANGELOG.md`](CHANGELOG.md) for progress.
>
> The renderer is hand-written MIPS assembly — design in
> [`docs/architecture.md`](docs/architecture.md).

## What you need

### Game data (not included, never versioned)

The engine runs the original game files: `pak0.pak` alone for the shareware
version (episode 1), or `pak0.pak` and `pak1.pak` for the full game (the four
episodes).
They are **not** in this repo.

- **`pak0.pak`** (shareware): id Software lets the original shareware
  package be shared as it is; files converted from it are not covered (see
  [License](#license)).
- **`pak1.pak`** (full version): commercial content. Usable **only** if you own
  a legal copy of the game, provided locally. Never committed, never
  redistributed.

`local.env` (never versioned) gives their paths: `QUAKE_PAK0`, and
`QUAKE_PAK1` for the full game (`scripts/cook.sh`'s header lists the other
keys). `EMU` in it names the emulator the scripts run (the PCSX-Redux
AppImage).

### PS1 toolchain

**PSn00bSDK v0.24** (prebuilt release, bundled `mipsel-none-elf-gcc` toolchain)
+ **CMake ≥ 3.21** + **Ninja**.

```bash
# 1. Get the Linux release
curl -LO https://github.com/Lameguy64/PSn00bSDK/releases/download/v0.24/PSn00bSDK-0.24-Linux.zip

# 2. Extract it where the SDK should live
PSN00BSDK=/path/to/psn00bsdk
unzip PSn00bSDK-0.24-Linux.zip && mv PSn00bSDK-0.24-Linux "$PSN00BSDK"

# 3. Expose the SDK (bash/zsh — see third_party/README.md for fish)
export PATH="$PSN00BSDK/bin:$PATH"
export PSN00BSDK_LIBS="$PSN00BSDK/lib/libpsn00b"

# 4. Verify
mipsel-none-elf-gcc --version   # -> gcc (GCC) 12.3.0
mkpsxiso                        # -> MKPSXISO 2.03 ...
```

Details and fish notes: [`third_party/README.md`](third_party/README.md).

### Test emulators

- **PCSX-Redux** — ships OpenBIOS, no PS1 BIOS needed. Used for automated
  validation.
- **DuckStation** — fast iteration; requires a PS1 BIOS (not provided).
- **Mednafen** — times the console's DMAs as the hardware does (a fault
  only a console showed, §123); requires a US PS1 BIOS (`scph5501.bin`, not
  provided) and `-force_module psx` (the disc carries no licence string).

## Build

The build cooks the Quake data it finds (`QUAKE_PAK0`, and `QUAKE_PAK1` for
the registered episodes, in `local.env`) into the disc's files.

The levels stream by zones (`docs/porting-notes.md` §57): the cook cuts
each map by the zone tables in `zones/`, made by the level-chunks analysis
outside the repo (numbers only). By default it takes `zones/auto_1536_s3`
(§93) with three chunk slots, and keeps 98 % of every sound's energy
(§87). `local.env` or the command line (which wins) may set others:
`Q1PSX_ZONE_TABLES` (another folder, or `none`: every map whole),
`QCOOK_SOUND_KEEP`. The build cooks again when the
cook's settings change (`scripts/cook.sh`'s header lists them; §118).

```bash
cmake --preset default .      # configure (toolchain = $PSN00BSDK_LIBS/cmake/sdk.cmake)
cmake --build ./build         # -> build/q1psx.bin + build/q1psx.cue
```

Two presets (the `bench` preset is `scripts/bench.sh`'s):

| Preset | Directory | What it is |
|---|---|---|
| `default` | `build/` | The development build: a map menu (SELECT + START), the debug overlay (START) |
| `release` | `build-release/` | Quake's own menu (START), saves on the memory card in slot 1, no debug tools |

Every build alike (`docs/porting-notes.md` §52, §119): the 4-bit textures
at high, the 3D gun, the 3D view at 320×240, the draw distance at 1536
world units (the walls past 512 units flat-shaded), the effects' particles
at their high counts, the dithering on. The map geometry and the models
are low on every disc; the guns are Quake's rebuilt by hand
(`tools/qcook/game.c`). In Graphics: the HUD, the
anti-warping (low, default, medium, high, ultra), the dynamic lights, the
frame rate cap (60, 45, 30) and the tick rate (60, 30). The music is Quake's CD tracks streamed
by the SPU (22.05 kHz, the menu's theme 32 kHz).

The release saves on the memory card in slot 1 (one block: Quake's twelve
slots, an autosave at each new level, the settings; a slot keeps the start
of its level), offers Quake's level select (New Game: the skill, the
episode, the level), takes a PlayStation Mouse in port 2 (Options: Use
Mouse; the mouse aims and fires, the pad in the other hand moves) and
vibrates a DualShock (Options: Vibration); Options > Customize controls
lists every button (`docs/porting-notes.md` §120).

```bash
cmake --preset release && cmake --build ./build-release
```

Run the resulting image:

```bash
pcsx-redux -loadiso build/q1psx.bin -run
# or
duckstation-qt -batch build/q1psx.cue
# or
mednafen -force_module psx build-release/q1psx.cue
```

## Layout

| Path | Role |
|---|---|
| `docs/architecture.md` | Engine design: budgets, memory map, formats, asm renderer |
| `docs/porting-notes.md` | Log of porting choices (float→fixed, formats, memory) |
| `engine/` | PS1 engine code (C + hand-written MIPS asm renderer, PSn00bSDK) |
| `tools/` | PC asset-conversion tools ("cooking") |
| `iso/` | `mkpsxiso` config, boot files |
| `scripts/` | Build and test helpers: the cook, the disc, the headless benches and checks |
| `zones/` | The zone tables the cook cuts the levels by: numbers only, from the analysis outside the repo (`docs/porting-notes.md` §57, §93, §118) |
| `third_party/` | Install instructions for external dependencies |
| `reference/` | Clones of `id-Software/Quake`, `quake-rerelease-qc`, `PSn00bSDK` (not versioned, read-only) |

## License

Code in this repo: **GPLv2** (see [`LICENSE`](LICENSE)). The engine ports id
Software's code, both under the GPL version 2 or later: the Quake engine
([`id-Software/Quake`](https://github.com/id-Software/Quake), Copyright (C)
1996-1997 Id Software, Inc.) and the QuakeC of the 2021 re-release
([`id-Software/quake-rerelease-qc`](https://github.com/id-Software/quake-rerelease-qc),
Copyright (C) 1996-2022 id Software LLC); `tools/qcook/anorms.h` is Quake's
own file. One file is under another license: `engine/q1psx.ld` is
PSn00bSDK's linker script (`ldscripts/exe.ld`, Copyright (C) 2021
spicyjpeg), modified, under the Mozilla Public License 2.0, which allows
it beside GPL code (MPL 2.0 §3.3).

Quake's game data (the `.pak` files: their maps, models, sounds, pictures,
and the music) remains the property of id Software and is not covered by
this license. The disc image the build makes carries data converted from
the paks it was given: it is for their owner alone and must not be shared,
the shareware's too (its terms cover the original package only).

Quake is a trademark of id Software LLC, a ZeniMax Media company;
PlayStation is a trademark of Sony Interactive Entertainment. This project
is not affiliated with, endorsed or sponsored by id Software, ZeniMax
Media, Microsoft or Sony.
