# Testing procedures

Rule: **systematically test on an emulator** after every step. This
file lists, per step, the validation criterion and whether a **real-hardware**
test is additionally required before checking it off.

## Emulators

| Emulator | Role | Install status |
|---|---|---|
| DuckStation | Fast dev iteration | ⬜ present as AppImage; needs a PS1 BIOS |
| PCSX-Redux | Low-level / cycle-accurate debugging | ⬜ present as AppImage (bundled OpenBIOS) |

⚠️ Do not use ePSXe or MAME-based cores (inaccurate — PSn00bSDK docs).

### Running an image

`scripts/run-emu.sh [image.bin]` builds if needed and launches PCSX-Redux
(OpenBIOS, no PS1 BIOS required).

**Headless speed.** PCSX-Redux builds since 2026-10 run `-no-ui` at about
real-time speed (the earlier ones ran unthrottled) and write the console at
~5 KB/s, so a display-list dump of ~100 KB takes 20 s: `scripts/bench.sh`
waits 420 s for the game scenario (15 dumps), `scripts/dlcheck.sh` 500 s, and
a `BENCH_MAPS=1` run needs `SECS=3000`.

**No reliable headless screenshot path yet.** PCSX-Redux `-no-ui` runs the CPU
but has no GPU backend, so its web API (`/api/v1/gpu/vram/raw`) returns empty
VRAM; the GUI + `-webserver` combination crashes on this machine. Until that is
solved, two checks are used:

1. **Automated:** `scripts/headless.sh [image] [secs] [stop-regex]` boots the
   image headless and checks for a clean kernel log — `psxgpu: setup done`
   reached, no CPU exception. Proves the program loads and runs `main()`.
2. **Bench (R1.2+):** `scripts/bench.sh` builds the bench variant and
   prints one `bench ...` line per scenario: CPU cycles per stage plus
   counters. It is the reference for every perf decision. `PROFILE=0`
   builds it like the `release` preset (no renderer counters / stage
   timers: frame totals only).
3. **Manual:** run `scripts/run-emu.sh` and look at the output (needed for
   anything about motion, tearing, flicker, or "does it look right").

## Per-step validation matrix

| Step | Validation criterion | Emulator | Real hardware |
|---|---|---|---|
| 1.3 | An image displays (PSn00bSDK template) | required | — |
| 2.1 | Solid background, double buffering, no flicker/tearing | required | recommended (tearing only shows on a real TV) |
| 2.2 | Controller state shown in debug | required | required (real controller) |
| 2.3 | Test file read from the CD image | required | — |
| 2.4 | Hunk/zone allocator: unit tests + no leak at boot | required (+ PC tests) | — |
| 3.2 | Fixed-point layer: PC test suite green | PC tests | — |
| 3.3 | `pak0.pak` contents listed (screen or serial) | required | — |
| 4.3 | One cooked texture displayed (2D sprite) | required | — |
| 5.x | BSP render wireframe → textured, framerate measured | required | recommended |
| 6.x | Movement + floor/wall collisions correct | required | required (controller feel) |
| R0 | Skeleton: build + CD image + headless boot clean | headless | — |
| R1.x | Empty frame on screen + its cycle cost; bench numbers stable across runs | headless bench + GUI | recommended (timing differs from emulators) |
| R2.x | Cooked data passes the host validator; loads with no error | host + headless | — |
| R3.x | Bench cycles per stage on every viewpoint; `start.bsp` 60 fps, no hole / flicker / warp | headless bench + GUI (an on-screen look) | required (final 60 fps verdict) |
| R4.x | `tools/pmsim` stress: 0 ticks in solid, 0 abnormal blocks; the bench's scripted path prints the same positions as `pmsim`; asm vs C differential tests (`VIEWS=0 scripts/bench.sh`, also with `EMU_ARGS=-interpreter`): 0 mismatches; walls, floors, stairs, jump feel right | host + headless bench + GUI | required (controller feel) |
| R5.x | Every cooked map loads and renders (`BENCH_MAPS=1 scripts/bench.sh`); world size per map against the 700 KB target; no visual regression (display lists / `wldref`) | headless bench + GUI (an eye on the warp) | recommended |
| R6.x | Optimizations: `scripts/dlcheck.sh check <ref>` reports identical display lists (an intended packet change must be shown pixel-neutral, then saved as the new reference); interpreter hashes equal the dynarec's; the bench's physics diffs show 0 mismatches; cycles per view with and without `PROFILE=0` | headless bench | recommended (RAM wait states, write buffer and I-cache are not emulated) |
| R6.3 | Warp + cracks (bench threshold `T=<px>`, default 8): `R_SLOW_C = 1` saves a `dlcheck` reference from the C spec, the asm must match it word for word (high and low); interpreter hashes equal; `PKTVIEW_BG=255,0,255` shows no uncovered pixel outside the sky; cycles per fidelity level (`PROFILE=0`, `BENCH_MAPS=1`) | headless bench | required (an eye on the warp, in motion) |
| R6.4 | 4-bit textures + dithering: `TEX4=1 scripts/bench.sh` renders the views from the QTX4 files (`PKTVIEW_DITHER=0` for dithering off); the display lists differ from the 8-bit run only in CLUT / tpage words | headless bench | required (the GPU gain only shows on hardware) |
| 8.2-8.4 | Weapons, HUD, controls: `BENCH_GAME=1 scripts/bench.sh` fires every weapon at start's spawn and renders a frame per weapon and per HUD style to `build-bench/views/g*.png` (the view model, projectiles, effects, the HUD) for a look; its `H` hashes under `EMU_ARGS=-interpreter` equal the dynarec's; `VIEWS=0 scripts/bench.sh`'s `clipdiff` includes hull 0 (0 mismatches); the view dumps stay identical to `t8hi` (no entity in them) | headless bench | required (the feel of the controls, the weapons' look in motion) |
| R6.5 | Asm micro-optimizations: `scripts/dlcheck.sh check t8hi` identical; `EMU_ARGS=-interpreter SECS=2400` gives the same `H` lines (compare the `^H ` lines of the reference and of `build-bench/bench.log`: the interpreter's console doubles characters in the `P` lines); the bench's physics diffs at 0 mismatches; the 128 spawn views by hash: `BENCH_MAPS=1 PROFILE=0 SECS=3000 scripts/dlcheck.sh check maps8` | headless bench | recommended (RAM wait states, write buffer and I-cache are not emulated) |
| 7.x | `progs.dat` executed: player spawn, doors/buttons | required | — |
| 8.x | Enemy + shotgun + HUD working | required | recommended |
| 8.2 dlights | Dynamic lights: the game bench's nailgun / lightning frames show the muzzle flash on the walls and the gun (`gnailgun.png`, `glightning.png`), the rocket's light along its flight, the explosion's; the hashes agree under the interpreter; `t8hi` (no game) identical; the per-frame cost in the `bench g*` lines | headless bench + GUI | recommended (the look of the flash) |
| 9.x | Sound and music: the game bench's `snd` lines (every start: entity, channel, volumes, the channel picked) identical between runs and under `EMU_ARGS=-interpreter`; the boot log's `snd:` bank line per map and `music:` track count; the cook's per-map `sounds —` lines (SPU bytes, halved loops, worst SNR); a listen to the mix, the spatialization, the loops' seams, the track change on a map load | headless bench + GUI | required (SPU mixing + CD-DA playback differ in emu) |
| 10.2 | Save/load via memory card | required | required (real memory card) |
| 13.x | Full cycle-accurate pass + CD-R/flashcart + NTSC/PAL TV | PCSX-Redux | required |

_(to be completed as phases progress)_

## Real-hardware test log

_(empty — no hardware testing done)_
