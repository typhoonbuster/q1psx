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
| 7.x | `progs.dat` executed: player spawn, doors/buttons | required | — |
| 8.x | Enemy + shotgun + HUD working | required | recommended |
| 9.x | SPU SFX; CD-DA/XA music | required | required (SPU mixing + CD-DA playback differ in emu) |
| 10.2 | Save/load via memory card | required | required (real memory card) |
| 13.x | Full cycle-accurate pass + CD-R/flashcart + NTSC/PAL TV | PCSX-Redux | required |

_(to be completed as phases progress)_

## Real-hardware test log

_(empty — no hardware testing done)_
