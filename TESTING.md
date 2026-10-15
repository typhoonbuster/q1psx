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

**No reliable headless screenshot path yet.** PCSX-Redux `-no-ui` runs the CPU
but has no GPU backend, so its web API (`/api/v1/gpu/vram/raw`) returns empty
VRAM; the GUI + `-webserver` combination crashes on this machine. Until that is
solved, two checks are used:

1. **Automated:** `scripts/headless.sh [image] [secs] [stop-regex]` boots the
   image headless and checks for a clean kernel log — `psxgpu: setup done`
   reached, no CPU exception. Proves the program loads and runs `main()`.
2. **Bench (R1.2+):** `scripts/bench.sh` builds the bench variant and
   prints one `bench ...` line per scenario: CPU cycles per stage plus
   counters. It is the reference for every perf decision.
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
| 7.x | `progs.dat` executed: player spawn, doors/buttons | required | — |
| 8.x | Enemy + shotgun + HUD working | required | recommended |
| 9.x | SPU SFX; CD-DA/XA music | required | required (SPU mixing + CD-DA playback differ in emu) |
| 10.2 | Save/load via memory card | required | required (real memory card) |
| 13.x | Full cycle-accurate pass + CD-R/flashcart + NTSC/PAL TV | PCSX-Redux | required |

_(to be completed as phases progress)_

## Real-hardware test log

_(empty — no hardware testing done)_
