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
| 8.5 water | `VIEWS=0 scripts/bench.sh`: `pmdiff ... mismatches 0; runs started in water N` (N > 0 on start); `BENCH_GAME=1 scripts/bench.sh`: the `water tick` lines show level 2-3 in the water, the jump held raising the player, the air counting down; `build-bench/views/gwater.png` tinted with the gun, `gwater2.png` | headless bench + GUI | required (the feel of swimming, the ledge climb, the drowning) |
| 8.6 (1) | Brush models and the entity table (`docs/porting-notes.md` §28): the cook's `check:` line per map (every `.wld` passes, with its brush model and entity counts); `VIEWS=0 scripts/bench.sh`: `clipdiff ... mismatches 0` and `pmdiff ... mismatches 0` with the map's solid brush models in the traces (the boot's `ents:` line: edicts, solid brush models, drawn ones, none touching no leaf); `scripts/bench.sh 300`: the world views identical where no brush model is in view; `MAP=E1M1 VIEW=400,576,40,180,0 BENCH_MAPS=1 scripts/bench.sh 300` renders e1m1's views to `build-bench/views/E1M1.v*.png`, the last one 168 units in front of its first double door (`NOENTS=1` for the same without the entities: the A / B); the DuckStation play bench (9.z) clean with the entities spawned (up to 312 edicts, 52 solid brush models a map) | headless bench + GUI | required (doors, walls and plats seen in place on the maps, the player blocked by them) |
| 8.6 (2-4) | Triggers, movers, the level's end, the items (`docs/porting-notes.md` §29): `ENTS=1 scripts/bench.sh 2400` on e1m1 and start: the items (`item taken` lines: what each kind gives, `notify:` its message, `centerprint:` the secrets), the quad's `quad after 27 s` / `30 s` lines (its warning, its end), the views `build-bench/views/i*.png` (each kind seen from beside it, `iE1M1_quad` tinted blue, `iE1M1_hurt` red; start's dumps need start's textures: rerun `tools/pktview/pktview` with `cooked/id1/start_high.tex`); the door, button, plat, teleporter, secret and changelevel lines (states, positions, the intermission, the next map with its parms) and their views `ee1m1_*` / `estart_*` (`ee1m1_button0` / `ee1m1_button1`: the button red, then yellow once pressed, its alternate texture; the `anim:` lines: each texture's frame changes); `MAP=E1M1 VIEWS=0 scripts/bench.sh`: `clipdiff boxes N` sweeps at the explosive boxes, 0 mismatches | headless bench + GUI | required (the pickups, the doors and lifts in play, the intermission, an episode's end text) |
| 11.4 | The game loop on the vblank's polls (§30): the game bench's frames and sounds unchanged at 60 fps; in play, a 30 fps scene keeps the controls' response (two ticks per frame, each on its own poll) | headless bench + GUI | required (the feel at a low frame rate) |
| 10.3 | Quake's menu (release builds, §31): `cmake --preset release && cmake --build ./build-release` boots START paused under the main menu; `BENCH_GAME=1 scripts/bench.sh` dumps the menu's pages (`build-bench/views/m*.png`: the greyed entries, the graphics page in each warp mode); `release-ultra` carries the ultra textures and geometry alone (the configure line `bundling 32 cooked maps, textures ultra, geometry ultra`) and hides those two rows; the scripted flow: `cmake --preset release -B <dir> -DQ1PSX_PADSCRIPT=ON`, build, `scripts/headless.sh <dir>/q1psx.bin 150` — its log's `menu:` (open, keys, new game) and `settings:` lines (the levels, then the Graphics page's draw weapon 2D, draw distance 2048, flat shading far set), START loaded at boot and at each New Game, the levels changed in a game loaded only by the next New Game (`START_T0.TEX`, `START_G0.WLD`, `gdata: the models at level 1`), the effects at once | headless bench + GUI | required (the menu with a pad, the settings taking effect) |
| 9.z | DuckStation (closer to the machine than PCSX-Redux): the all-maps bench with the music disc (`cmake --preset bench -DQ1PSX_BENCH_MAPS=1 -DQ1PSX_BENCH_TEXTURES=1 -DQ1PSX_BENCH_TEX4=1 -DQ1PSX_BENCH_HASHONLY=1 -DQ1PSX_BENCH_MUSIC=1`, then DuckStation from a private home with its console and TTY log: `docs/porting-notes.md` §26.1) loads the 32 maps with no `GP0` error in its log and `music: 11 tracks on the disc`; with `-DQ1PSX_BENCH_PLAY=1` too (the loading screen and 30 interactive frames per map): a `bench play <MAP> ok` per map, no `Invalid ... read` line | DuckStation, windowed | required before any disc goes to the machine |
| 9.w | Fourth optimization pass (`docs/porting-notes.md` §34): `MAP=START BENCH_MAPS=1 HASHONLY=1 PROFILE=0 scripts/dlcheck.sh check <ref>` identical to the pass's starting point, the game bench's `H` lines identical (the graphics menu's pages aside, §35's new row), `BENCH_MAPS=1 HASHONLY=1` on the 32 maps identical, `DLIGHT_C=1` and `DLIGHT_C=2` agreeing; the cost: `SAMPLE=1` profiles (`samples` lines, symbolized offline), the `bench` frame columns and the game bench's `bench walk` lines (frames in motion by vblanks); `PKTVIEW_OCCL=1 tools/pktview/pktview ...` prints each dumped frame's `occl:` lines | headless bench | required (the view models and the near floors on screen, nothing missing at the screen's edges) |
| 11.5 | The tick rate setting (§35): `VIEWS=0 scripts/bench.sh`: `pmdiff ... mismatches 0` and `pmdiff dt ticks 3000 ... mismatches 0`; `BENCH_GAME=1 scripts/bench.sh`: the `bench rate` lines (the walk's length and the jump's apex alike at every rate, 43 units at 10 Hz; the cycles a vblank falling with the rate), the game's `H` lines unchanged at 60 Hz (the graphics menu's pages aside); `PLAY=1 BENCH_MAPS=1 MAP=START`: `bench play START ok` | headless bench + GUI | required (the feel at 32 Hz: the camera smooth, no input lost, doors and lifts) |
| 9.y | Third optimization pass (`docs/porting-notes.md` §25): `HASHONLY=1 BENCH_GAME=1 scripts/bench.sh 300` hashes identical to the previous pass's (the four views with particles aside: the draw order being Quake's lists now, their renders from the full dump differ by 2 to 15 pixels where particles share a pixel), the `snd` lines identical, `DLIGHT_C=1` and `DLIGHT_C=2` (no `dlchk` line) agreeing, `EMU_ARGS=-interpreter` agreeing, `scripts/dlcheck.sh check t8hi` identical; the `bench g*` lines' stages for the cost; the bench's `pad poll:` line shows port 1 read by the game's driver | headless bench | required (the pad on a real DualShock and the HUD after damage, a weapon change, an item) |
| 9.x | Sound and music: the game bench's `snd` lines (every start: entity, channel, volumes, the channel picked) identical between runs and under `EMU_ARGS=-interpreter`; the boot log's `snd:` bank line per map and `music:` track count; the cook's per-map `sounds —` lines (SPU bytes, halved loops, worst SNR); a listen to the mix, the spatialization, the loops' seams, the track change on a map load | headless bench + GUI | required (SPU mixing + CD-DA playback differ in emu) |
| 10.2 | Save/load via memory card | required | required (real memory card) |
| 13.x | Full cycle-accurate pass + CD-R/flashcart + NTSC/PAL TV | PCSX-Redux | required |

| 11.6 | The liquids lit (§36): the cooked worlds' liquid records at light 192 (scratch script over `*_high.wld`); `MAP=START BENCH_MAPS=1 VIEW=1545,2400,96,90,30 scripts/bench.sh`: the lava textured (`START.v12.png`); the game bench's `gwater` dump carries `P 62325082` (the water's colour) | headless bench | required (the water, the slime and the lava from above and from under) |
| 11.7 | The draw distance and the flat shading (§37): the options off, `MAP=START BENCH_MAPS=1 HASHONLY=1 PROFILE=0 scripts/dlcheck.sh check <ref>` identical and the game bench's `H` lines identical (the graphics menu's pages aside); on, `DRAWDIST=768` / `FLAT=2` bench builds: the frame cycles of START's, E1M8's, E3M4's and E1M7's views, the views' pictures (the fog to black, the flat records); DuckStation `PLAY=1 BENCH_MAPS=1` with both on: 32/32 maps, no GP0 error, the hashes equal to PCSX-Redux's | headless bench + DuckStation | required (the fog while moving: no record popping in, the PVS marking redone as the camera moves; the menu rows) |
| 11.8 | The 2D weapons (§38): `qcook game` prints the 2D weapons' frames and slot (`QCOOK_WS=1` each frame's box, `QCOOK_WSDUMP=<dir>` the full-res renders); `BENCH_GAME=1 PROFILE=0 VIEWMODEL=2 scripts/bench.sh`: the weapons' frames cheaper than `VIEWMODEL=1`'s, their pictures (pktview) matching the 3D gun's; the 3D gun's pictures unchanged by the new game data (pixel comparison: the images moved in VRAM); DuckStation `PLAY=1 BENCH_MAPS=1 VIEWMODEL=2`: 32/32 maps, no GP0 error | headless bench + DuckStation | required (the gun's moves while walking and firing, the weapon switch, the light, the full-screen HUD's viewport) |
| 11.9 | The four quality settings (§39): at high everywhere, `scripts/dlcheck.sh check <ref>` on START's 12 views identical to the single-fidelity build's high, `BENCH_MAPS=1 HASHONLY=1` on the 32 maps: 8 of 128 views differ (the textures over 128 texels: a texel of rounding on a few vertices, 3 maps 6 to 14 more polygons; their pictures 0 to 1 % of pixels), the game bench's `H` lines identical (the graphics menu's pages aside) and its `snd` lines; a scratch check of every cooked world's and map models file's mapped UVs against the old high cook; each level: `GEOMETRY=0\|3`, `TEXTURES=0\|1\|3`, `TEX4=1`, `MODELS=0\|1`, `EFFECTS=0\|3` bench builds (the maps load: `BENCH_MAPS=1`; the frame cycles); `TEXTURES=3 GEOMETRY=3 BENCH_MAPS=1` (release-ultra's only levels): the 32 maps load with no fallback (E4M7 the fullest, 1135 KB of the 1152 KB arena); DuckStation headless, `PLAY=1 BENCH_MAPS=1` at textures low, geometry ultra, models low, effects ultra: 32/32 maps load and play, no GP0 error, no invalid read; the editions' discs (`<MAP>_T<n>`, `_G<n>`, `_M<n>` counts in the build's `cd_image_*.xml`); the scripted release flow (10.3); the menu's pages with the hints (`mgraphics*.png`); the hints' numbers from §39.4's measures | headless bench | required (the four rows and their hints on a TV, a level changed in a game: the next level's load; the ultra geometry's light, the low geometry's cracks) |
| 8.7 | Streaming, the monsters, the SPU pool (§40): `qcook stream` per map (its asset count, the groups); `MONSTERS=1 MAP=<map> scripts/bench.sh 1500`: per kind the `mon start / fight / shot / killed / dead` lines (the monster sees the player, attacks, takes the shotgun, dies; its sounds' `stream: in` lines on the approach), no `mon OVERLAP` line, the pictures `build-bench/views/m<map>_<kind>_[abc].png`; `MONSTERS=2 SAMPLE=1 MAP=E1M3`: the crowd's `mon crowd` lines (`ents_tick` average and worst, the traces' cost by caller) and its `samples` (symbolized offline); `VIEWS=0 scripts/bench.sh` and `MAP=E1M3 VIEWS=0`: `clipdiff` sweeps, boxes, box nodes and lines of sight, `pmdiff`, all at 0 mismatches; `ENTS=1`: every item streamed in and taken | headless bench + GUI | required (the monsters in play: their moves, attacks, sounds by distance; the music paused and resumed around a read; a long session for the pools) |
_(to be completed as phases progress)_

## Real-hardware test log

_(empty — no hardware testing done)_
