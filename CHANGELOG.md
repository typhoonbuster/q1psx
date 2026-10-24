# Changelog

Format: one entry per roadmap step. The project was not
under git before Phase 1.4 — entries prior to that were grouped into the first
commits.

## [unreleased] — 2026-09-01

### Phase 0 — Framing

- **0.1** Reference sources cloned into `reference/` (shallow):
  `id-Software/Quake`, `id-Software/quake-rerelease-qc`, `Lameguy64/PSn00bSDK`.
  Read the READMEs, repo structure, PSn00bSDK docs and the template. No existing
  PS1 source port consulted. Findings in
  `docs/porting-notes.md` §1.
- Repo skeleton created: `CHANGELOG.md`,
  `TESTING.md`, `docs/porting-notes.md`, `engine/ tools/ iso/ scripts/
  third_party/` directories.
- **0.2** Architecture decisions validated (see
  `docs/porting-notes.md` §2): fixed-point **20.12** (the GTE's, not 16.16),
  scope **ep1 + hooks for ep2-4**, QuakeC **vanilla `progs.dat`**,
  toolchain **prebuilt PSn00bSDK v0.24 release**.
- **0.3** Legal Quake data identified: a full Steam install, `PAK0.PAK`
  + `PAK1.PAK` verified by md5. Paths recorded in `local.env` (gitignored).
  `.gitignore` created (proprietary data, reference clones, build output).

### Phase 1 — Environment & toolchain

- **1.1** PSn00bSDK v0.24 (prebuilt release) installed
  (GCC 12.3.0 `mipsel-none-elf`, `mkpsxiso` 2.03, `libpsn00b`), its `bin/` on
  the shell's `PATH` and `PSN00BSDK_LIBS` set.
- **1.2** Emulators: PCSX-Redux + DuckStation AppImages.
  PCSX-Redux works with its bundled OpenBIOS;
  DuckStation needs a PS1 BIOS (to be provided).
- **1.3** PSn00bSDK template compiled and displayed in PCSX-Redux (purple
  background + yellow square + "HELLO WORLD!") — the toolchain → CD image →
  emulator chain is validated.
- **1.4** Git repo attached to `github.com:typhoonbuster/q1psx` (branch
  `master`). GitHub's default Apache 2.0 license replaced with **GPLv2** (engine
  derived from `id-Software/Quake`). Skeleton + docs committed.
- **1.5** Reproducible build from the repo: root `CMakeLists.txt` +
  `CMakePresets.json` + `iso/{iso.xml,system.cnf}` + `engine/main.c` (minimal
  boot: GPU init, solid background). `cmake --preset default . && cmake --build
  ./build` → `build/q1psx.{bin,cue,exe}`. `q1psx.exe` recognized as a valid
  PS-EXE. Procedure in `README.md`.

### Phase 2 — PS1 application skeleton

- **2.1** Double buffering (`engine/main.c`): two framebuffers stacked in VRAM,
  swapped on `DrawSync(0)` + `VSync(0)`; solid background via `draw.isbg`; a
  sliding marker + frame counter make tearing/flicker visible in motion.
  `scripts/run-emu.sh` launches an image in PCSX-Redux. Headless boot log is
  clean (`psxgpu: setup done`, no CPU exception); on-screen no-tearing check is
  manual — no working headless screenshot path on this setup yet (PCSX-Redux
  `-no-ui` has no GPU backend, GUI + `-webserver` crashes). See `TESTING.md`.

  **2.1 validated on screen** (2026-09-01): stable background,
  marker slides with no tearing/flicker, frame counter increments.

- **2.2** Controller input (`engine/pad.{c,h}`): BIOS pad driver on port 1,
  decoded to a `PadState` — connection flag, device type, active-high button
  mask, edge-triggered `pressed` set. `main.c` gained a debug overlay
  (device / type / raw button word / held key names) and the d-pad ←→ now
  steers the marker. Headless boot clean.

  **2.2 validated on screen** (2026-09-01) with a real pad.

- **2.3** CD-ROM read (`engine/cd.{c,h}`): `CdInit` → `CdSearchFile` →
  `CdControl(CdlSetloc)` → `CdRead`/`CdReadSync`. `iso/hello.txt` (142 B) added to
  the disc image; the engine loads it at boot and prints the result to serial.
  **Validated:** headless boot log shows `size=142 sum=0x2E47`, matching the
  host-side byte checksum — full, correct read. Also shown in the on-screen
  overlay. `engine/main.c` also now logs boot milestones to serial (`printf`)
  so headless runs can be checked without a screen.

- **2.4** Memory allocator (`engine/zone.{c,h}`): faithful port of WinQuake
  `zone.c` — double-ended Hunk (16-byte aligned, mark/free-to-mark) + 48 KiB
  Zone (first-fit + coalescing). Cache (LRU eviction) deliberately omitted;
  `Cache_*` will be shimmed to `Hunk_Alloc` later. The aim: optimise for
  speed (target 60 fps NTSC) → a linear Hunk with zero per-frame allocation.
  `engine/sys.c` adds `Sys_Error` (serial + halt). `engine/main.c` takes a
  1536 KiB block via `malloc`; `engine/memtest.c` boot self-test passes all
  20 checks headless, `Mem_Report` shows 1523 KiB free after the zone.
  Rationale in `docs/porting-notes.md` §3.

### Phase 3 — Engine foundations

- **3.1a** `engine/qcommon.{c,h}`: ported from WinQuake `common.{c,h}` — `byte`
  / `qboolean` (stdbool), `link_t` + `ClearLink`/`RemoveLink`/`InsertLink*` +
  `STRUCT_FROM_LINK`, `sizebuf_t` + `SZ_*` (Hunk-backed), byte-order helpers
  (`LittleLong` identity / `BigLong` swap on the LE R3000), `Q_atoi`,
  `COM_Parse`, `va`, `COM_FileBase`/`StripExtension`/`DefaultExtension`.
  Deferred: `MSG_*` (no networking), the file system (phase 3.3).
- Self-tests reorganised into `engine/selftest.{c,h}` (was `memtest`), now run
  against the live allocator and cover both 2.4 and 3.1a. All green headless.
- `engine/zone.c`: `Memory_Init` now 16-aligns the hunk base (the PS1 libc
  heap only guarantees 8) — stock Quake trusts malloc for this.

- **3.2a** Fixed-point math layer (20.12):
  - `engine/q_fixed.h`: `q_fixed` scalar, `Q_FIX*` conversions, `q_mul`/`q_div`
    (int64 intermediates), `q_sqrt`/`q_sqrt_big` (binary digit method, no
    float). Float conversions are host-only (`#if !defined(__mips__)`).
  - `engine/mathlib.{c,h}`: port of WinQuake `mathlib` — `vec_t`/`vec3_t`,
    `Vector*` macros, `DotProduct` (now a function), `CrossProduct`,
    `VectorScale`/`MA`/`Compare`/`Inverse`, `Length`, `VectorNormalize`
    (per-component divide, not reciprocal-multiply).
  - `tools/mathtest/`: host build (`make -C tools/mathtest run`) comparing
    every op to a `double` reference over quantised inputs — 650k checks,
    0 failures.
  - `engine/selftest.c`: on-device known-answer checks (all green headless).
  Range note: dot/cross results must fit ~±524288, so one operand should be
  near unit scale (`docs/porting-notes.md` §4).

- **3.2b** Trig / angles:
  - `tools/gen_sintab.py` -> `engine/q_trig_table.c` (1024-entry 20.12 sine
    table, +1 wrap guard).
  - `engine/mathlib.c`: `q_sin`/`q_cos` (table lookup + 8-bit lerp, degrees),
    `anglemod`, `AngleVectors` (near-verbatim port), `R_ConcatRotations` /
    `R_ConcatTransforms` (3x3 / 3x4 products).
  - `tools/mathtest`: +trig checks over -720..720 deg and 5k random angle
    triples (orthonormal basis, matches analytic forward). Total 688k checks,
    0 failures. On-device checks green.

- **3.1b** `engine/cvar.{c,h}`: port of WinQuake `cvar.c`. `cvar_t.value` is
  `q_fixed` (20.12), not `float` — PSn00bSDK has no `atof`/`%f`. New
  `Q_atofix` / `Q_fixtoa` in `qcommon` (`"-1.5"` ⇄ 20.12, trims zeros).
  `Cvar_WriteVariables` deferred to phase 10.
- **3.1c** `engine/cmd.{c,h}`: `Cbuf_*` (text buffer, `\n`/`;` lines) + `Cmd_*`
  core (`TokenizeString`, `AddCommand`, `Exists`, `ExecuteString`,
  `Argc`/`Argv`/`Args`). Aliases, `exec`/`stuffcmds`/`wait`, `ForwardToServer`
  deferred. Added `Q_strcasecmp`/`Q_strncasecmp` (not in PSn00bSDK libc).
- `engine/selftest.c`: +cvar/cmd checks (register/set/parse, tokenize, buffer
  dispatch, cvar-set-via-command). All green headless; hunk clean afterwards.

- **3.3** PAK reader:
  - `engine/cd.c`: `cd_find` (CdSearchFile wrapper) + `cd_read_at` — reads an
    arbitrary byte range from a file (sector-aligned bounce via the high hunk).
  - `engine/pak.{c,h}`: `pak_open` (header + directory, LE), `pak_find`
    (case-insensitive), `pak_read_lump` (stream one lump), `pak_list`.
  - `CMakeLists.txt` + `iso/iso.xml`: bundle `pak0.pak` into the dev CD image
    from `local.env`'s `QUAKE_PAK0` (`@PAK0_ENTRY@`); never committed. Build
    logs whether it was found.
  - **Validated:** boot lists all 339 entries off the disc; dirofs 18254423,
    dirlen 21696, count 339, and lump sizes match a Python cross-check exactly
    (`progs.dat` index 266 = 413116 B).

- **3.1d / 3.4** progs.dat structures + loader:
  - `engine/pr_comp.h`: verbatim QuakeC bytecode structs + opcode enum
    (`dprograms_t`, `dstatement_t`, `ddef_t`, `dfunction_t`, `PROG_VERSION 6`).
  - `engine/progdefs.h`: `globalvars_t` / `entvars_t` from `progdefs.q1`, field
    order frozen, `float` → `vec_t` (same 4-byte layout). `PROGHEADER_CRC 5927`.
  - `engine/progs.{c,h}`: `edict_t`, the `pr_*` globals, and `PR_LoadProgs` —
    streams `progs.dat` off the pak, whole-file CRC, byte-swaps header + all 6
    lumps, checks `version` / `crc`, computes `pr_edict_size`. No interpreter,
    no float→fixed of the globals yet (Phase 7).
  - `engine/crc.{c,h}`: CRC-16/CCITT (XMODEM), verbatim from Quake `crc.c`.
  - **Validated:** boot output matches a Python parse exactly — 20940
    statements, 2091 functions, 4287 globaldefs, 218 fielddefs, 11471 globals,
    195 entityfields; file CRC-16 24778; `main`/`worldspawn`/`SUB_Null`/
    `PlayerPreThink` resolve.

**Phase 3 complete.**

### Phase 4 — offline asset cooking

Design: cook tool is **C**; ONE CD image holds all 4 fidelity levels
(`low`/`medium`/`high`/`ultra`, defined in `tools/qcook/fidelity.h`) and every
owned game; the player picks game + fidelity from the menu.

- **4.1 (textures)** `tools/qcook/`:
  - reads a `.pak`, builds a PS1 BGR555 CLUT from `gfx/palette.lmp`, extracts
    the BSP texture lump (miptex, 8bpp indexed), downscales per fidelity
    (closest box-filtered mip + aspect-preserving nearest-neighbour), writes a
    `QTX1` container (`qtx.h`: header + CLUT + entries + 8bpp pixel data;
    little-endian, no swap needed either side).
  - `qcook textures --pak … --map … --fidelity … --out …` and `qcook info …`.
  - **Verified** on `maps/start.bsp`: 81/81 textures extracted (count matches a
    Python parse), palette + pixels render correctly (PNG dump vs Quake's
    palette). `high` → 128px cap, `ultra` → native.
  - BSP geometry + lightmap cooking deferred to Phase 5.1.

- **4.2** `scripts/cook.sh` builds `qcook` and cooks all 4 fidelities of
  `start.qtx` into `cooked/id1/…`. `CMakeLists.txt` + `iso/iso.xml` bundle
  `cooked/id1/high/start.qtx` onto the disc as `START.QTX` (`@COOKED_ENTRIES@`).
  Shared format headers moved to `formats/` (included by both the tool and the
  engine).
- **4.3** `engine/tex.{c,h}`: `qtx_open` uploads the shared 256-entry CLUT to
  VRAM (512,300); `qtx_upload` DMAs an 8bpp texture to a scratch page (512,0)
  and returns its `getTPage`/`getClut` ids. `main.c` loads `START.QTX` off the
  disc and draws one texture (`tech08_1`) as a `POLY_FT4`. Headless boot clean
  (`qtx_open: 81 textures`, `qtx_upload … tpage=0088 clut=4b20`); on-screen
  check pending.

  **4.3 validated on screen** — the `tech08_1` texture renders
  correctly. **First visual render of the project.**

- **Loading screen + video refactor:**
  - `engine/vid.{c,h}`: the framebuffer / ordering-table / per-frame primitive
    buffer, extracted from `main.c` (`vid_init` / `vid_alloc` / `vid_text` /
    `vid_flip` / `vid_set_bg`), OT length 16.
  - `engine/loading.{c,h}`: `load_begin(n)` / `load_step("label")` /
    `load_done()` — draws a title + progress bar + step label and flips each
    step, so slow CD loads no longer look like a crash.
  - `main.c` slimmed to a clean boot sequence (`load_step` per stage) + a
    tidier demo loop; dropped the now-redundant `pak_list` / function-probe
    debug prints.

### Phase 5 — minimal 3D

- **5.1a** `engine/render.{c,h}` — GTE wireframe:
  - `r_init`: `InitGeom`, `gte_SetGeomOffset(160,120)`, `gte_SetGeomScreen(160)`.
  - `r_begin_view(pos, ang)`: `AngleVectors` (world = Quake coords: X fwd,
    Y left, Z up) → view `MATRIX` (rows right / -up / forward, 1.3.12 == our
    20.12), `t = -M*campos`, `gte_SetRotMatrix`/`SetTransMatrix`.
  - `r_wireframe(verts, nverts, edges, nedges, rgb)`: `gte_ldv0`+`gte_rtps`
    per vertex, `gte_stsz` near-plane skip, `LINE_F2` per visible edge.
  - `main.c`: hand-made 768×512×256 room, d-pad flies the camera. Headless
    boot clean; on-screen check pending.

  **5.1a validated on screen** — the hand-made room renders in
  perspective and the camera responds.

- **5.1b** BSP geometry:
  - `tools/qcook` `geometry` command + `formats/qgeo.h` (QGE1): world model
    vertices → SVECTOR (int16 Quake units, X fwd/Y left/Z up), unique edges
    used by model 0's faces. Verified vs a Python parse (7757 verts, 12215 of
    13710 edges for `start.bsp`).
  - `scripts/cook.sh` cooks `start.qgeo` (geometry doesn't vary by fidelity
    yet); `CMakeLists.txt` + `iso.xml` bundle it as `START.QGE`.
  - `engine/geo.{c,h}`: parse a QGE1 blob in RAM. `main.c`: boot step 6 loads
    it, `demo_loop` wireframes the real map (camera at the bounds centre, L1/R1
    pitch), 2500-line budget until culling.
  - `engine/vid.c`: `vid_alloc` / `vid_text` now guard against overflowing the
    per-frame primitive buffer (was corrupting RAM → dynarec crash).

- **5.2 (cook side)** — surface + atlas cooking:
  - `formats/qtx.h` → `QTX2`: entries are now 1:1 with miptex (gaps get a 2x2
    marker) and each carries a VRAM atlas slot. `qcook textures` packs the
    map's textures into a 20x7 grid of 64-texel cells right of the
    framebuffers (world cap 64px; fidelity tuning of that is Phase 11).
  - `formats/qsrf.h` (`QSF1`) + `qcook surfaces`: per world-model face — a
    convex polygon of shared vertices, per-vertex texel UVs (shifted so the
    face-local min is 0), texture index. `start.bsp`: 7757 verts / 4934 faces
    / 24430 faceverts. Cross-checked vs a Python parse (69 distinct textures,
    winding + UV ranges sane).
  - `qcook textures`: world tiles are now snapped to a power of two on each
    axis (`snap_p2`, ≤ cap) — the PS1 texture window can only tile
    power-of-two sizes, and Quake textures are multiples of 16, not always
    p2. Non-square textures stretch to the nearest p2 per axis (a tradeoff).
  - `scripts/cook.sh` + `CMakeLists.txt` + `iso.xml`: cook + bundle
    `START.QSF`.

- **5.2 (engine)** — textured surface rendering:
  - `engine/tex.{c,h}` rewritten for `QTX2`: `qtx_open` uploads the whole
    atlas + the shared 256-colour CLUT to VRAM in one pass; `qtx_slot(i)`
    returns `{tpage, clut, RECT twin}` for face `i`. Texture window:
    `mask = (256 - size) / 8`, offset from the cell's position in its tpage.
  - `engine/surf.{c,h}`: map a `QSF1` blob in RAM (pointers into the Hunk
    blob, no copy).
  - `engine/render.c` `r_surfaces`: lazily GTE-projects each referenced
    vertex (cached per frame), skips faces with any vertex behind the near
    plane, back-face culls on screen winding, fan-triangulates into
    `POLY_FT3`s preceded by one `DR_TWIN` (emitted reversed so the window
    runs first on the OT). Face budget 600 until **5.4**.
  - `engine/vid.{c,h}`: `vid_alloc_raw` / `vid_link` split (reserve without
    linking, then link) so a `DR_TWIN` and its polys stay adjacent;
    `PRIM_BYTES` 65536 → 81920; `vid_prim_overflowed()`.
  - `engine/main.c`: 7th boot step loads `START.QSF`; `demo_loop` draws
    `r_surfaces` when textures + surfaces both loaded, `SELECT` toggles back
    to the wireframe.
  - **Confirmed on screen** (2026-09-02): `start.bsp` renders textured
    and correct. Frame rate is very low — the whole world model is
    transformed and drawn every frame with no visibility culling. Expected;
    **5.4** (PVS + frustum) is the fix and no optimisation happens before it.

- **5.3** — debug free-fly camera. `fly_cam_t` + `fly_cam_update` in
  `engine/main.c`: d-pad L/R yaw + U/D forward-back, Square/Circle strafe,
  Triangle/Cross world up-down, L1/R1 pitch (±89°), R2/L2 fast/slow, START
  snaps back to the spawn pose. No collision or gravity (by design — that is
  Phase 6). Overlay reworked to show the control map + camera pose.
  **Confirmed on screen** (2026-09-02).

- **5.4** — PVS + frustum culling (the performance step). Approach A
  (thin port of Quake's `r_bsp.c`).
  - `formats/qbsp.h` (`QBS1`) + `qcook bsp`: the world BSP the culler needs —
    a deduplicated plane table, the node tree, leaves (bounds + marksurface
    span + contents), all marksurfaces (each a BSP/QSF face index), and
    Quake's RLE-compressed PVS copied verbatim. Plane normals x4096 int16,
    distances x4096 int32; every section 4-byte aligned. `start.bsp` → 1920
    planes / 2909 nodes / 1568 leaves / 34 KiB vis ≈ 158 KiB.
  - `qcook surfaces`: now emits exactly one `qsrf_face` per world-model BSP
    face (uncookable ones become empty slots the renderer skips) so a face
    index is a stable key shared with `marksurf[]`.
  - `engine/bsp.{c,h}`: `bsp_load` (pointers into the blob), `bsp_point_leaf`
    (descend the node tree), `bsp_pvs` (decompress one RLE row into a 1 KiB
    scratch, cached by leaf; keeps the last good set when the camera clips
    into solid).
  - `engine/render.c` `r_world`: build 5 world-space frustum planes from the
    view basis, locate the camera leaf, walk the BSP front-to-back —
    AABB-reject whole subtrees against the frustum (trivial-accept clears a
    plane for children), and in each PVS-visible leaf draw every face once
    (dedup via a rolling per-frame key). `draw_face` extracted from
    `r_surfaces` and shared.
  - `engine/main.c`: 8th boot step loads `START.QBS`; `SELECT` now cycles
    world (PVS+frustum) / all-surfaces / wireframe.
  - Fix: a per-face `DR_TWIN` left the GPU texture window set; the debug font
    (lower OT bucket, drawn after) inherited it and every glyph came out
    garbled. `r_world` / `r_surfaces` now emit a full-page window primitive
    that, linked first, runs last in the bucket and restores the default.
  - **Confirmed on screen** (2026-09-02): the world culls correctly.
    Two issues left open for later:
    * frame rate still low — PVS+frustum cut the face count but PS1 fill-rate
      and a texture-window change per face are the new wall (batch by texture
      / lower res — an optimisation pass, Phase 11 or sooner);
    * no near-plane clipping — `draw_face` drops a whole face when any vertex
      is behind `NEAR_Z`, so large faces pop out near the camera.

- **6.1** — AABB-vs-BSP collision, fixed-point. Approach A (extend QBS1),
  approved.
  - `formats/qbsp.h` → `QBS2`: adds Quake's `dclipnodes` (hulls 1 & 2 share
    the one array) + the world model's `headnode[1]` / `headnode[2]` roots.
    `qcook bsp` validates clipnode plane/child indices. `start.bsp` → 5326
    clipnodes, +37 KiB (≈ 195 KiB total).
  - `engine/bsp.{c,h}` `bsp_hull_trace`: a fixed-point port of Quake's
    `SV_RecursiveHullCheck` + `SV_HullPointContents`. Sweeps a point through
    clip hull 1 or 2, fills `trace_t { fraction (0..ONE), endpos, plane_n/d,
    startsolid/allsolid/inopen/inwater }`. Positions are integer world units;
    plane distances and the `t1/t2` crossing values are ×4096 (int64
    intermediates); `DIST_EPSILON` → 128; the crosspoint fraction is clamped
    in int64 before narrowing.
  - `engine/main.c`: the fly camera sweeps hull 1 on every move when clip is
    on. **START** toggles clip on/off (noclip), **L2+R2** snaps to the spawn
    pose (was START). Overlay shows `clip:ON/OFF`.
  - Host trace test (grid classify + axis sweeps on the real `start.qbsp`) +
    headless boot clean. Awaiting on-screen check.

- **6.2 / 6.3** — player movement (`engine/pmove.{c,h}`). A 60 Hz fixed-point
  port of Quake's player physics:
  - friction (`SV_UserFriction`) + acceleration toward a wished direction
    (`SV_Accelerate`), with the weak-air-control variant (`SV_AirAccelerate`:
    accel cap 30 u/s in the air) when not grounded;
  - `slide_move` = `SV_FlyMove` + `ClipVelocity`: up to 4 trace-and-clip
    bumps, the multi-plane / crease-slide logic, dead-stop on reversal;
  - gravity (800 u/s²) and jump (270 u/s off the ground), each with a
    sub-unit accumulator so 60 Hz integer steps stay accurate;
  - `categorize_position`: a 2-unit down-trace each tick for ground state;
  - step-up: when a wall blocks a grounded move, retry lifted by 18 units and
    trace back down (`SV_WalkMove`), keep it only if it got further onto a
    floor.
  - Velocities are integer units/s; positions integer world units with a
    per-axis carry. `engine/main.c` now drives a `pmove_t` (d-pad turn/move,
    Square/Circle strafe, L1/R1 look, Cross jump, Triangle walk, START
    noclip, L2+R2 home); the view is the player eye + 22 units.
  - Host sim on the real `start.qbsp`: settles on floors, jump apex ≈ 44 u
    (Quake ≈ 45), accelerates to 320 u/s in ~0.3 s, stops dead at walls.
    Headless boot clean. Awaiting on-screen check.

- **6.1b** — spawn at `info_player_start`. `qcook bsp` scans the BSP entity
  lump (plain text, `{ "k" "v" ... }`) for the first `info_player_start` and
  stores its `origin` + `angle` in the header (`QBS2` → `QBS3`). `engine/bsp`
  loads them; `engine/main.c` spawns the player there (yaw included), falling
  back to the model-bounds centre when a map has none. `start.bsp` → spawn
  (544, 288, 32) yaw 90; host sim confirms it is open space and the player
  settles on the floor at z≈29. L2+R2 now also restores the spawn facing.

### Performance & clean-rendering pass (priority — before any more features)

**The big one: the build was `CMAKE_BUILD_TYPE: Debug` (`-O0`) the whole
time.** Every too-slow measurement above was on an unoptimised build.
`CMakePresets.json` now uses `RelWithDebInfo` (`-O2 -g`). Profiling with a
Hblank-timer stopwatch had shown the BSP walk at ~53 µs/node and `draw_face`
at ~76 µs/face — ~10× what the code should cost — which is exactly an `-O0`
penalty. All the int32 / marking / culling work above still stands and
compounds with `-O2`.


The goal: **map + textures only** (no lighting, no models) at a
constant 60 fps NTSC, no flicker, nothing vanishing.

- **Instrumentation.** `engine/vid.c` installs a `VSyncCallback` counter;
  `vid_flip` records `frame_vbl` (vblanks the last frame took — 1 = 60 fps,
  2 = 30, …), whether the GPU was still busy on return (`vid_gpu_busy` →
  GPU-bound), and prim-buffer bytes used. The overlay shows smoothed
  vbl/frame, an fps estimate, the bound, face count and prim bytes.
- **Near-plane clipping.** `draw_face` no longer drops a whole face when a
  vertex is behind the camera — it Sutherland-Hodgman-clips the polygon
  against `view_z == NEAR_CLIP` (20), projecting the new edge-crossing
  vertices (world-space lerp + `rtps`, UVs lerped too). `r_begin_view`
  stashes the forward matrix row for the view-space Z test.
- **Fewer primitives.** Faces triangulate as a fan of `POLY_FT4` quads (+ one
  odd `POLY_FT3`) instead of all `POLY_FT3` — roughly halves the prim/DMA
  count for the quad-heavy world.
- **Sky.** `QTX_F_SKY` faces are skipped (a 64 px tile stretched over a
  skybox face thrashes the texture cache and looks wrong); the flat
  background colour (now a dim blue-grey) shows through as sky.

Progress so far (device): map renders correctly, ~20 fps (up from 12 on the
`-O0` build). Back-face over-culling fixed (exact 3D plane test, `QSF1`→
`QSF2` cooked face normals). BSP walk bounded by `R_MarkLeaves`-style node
marking. `view_z` / `plane_dist` moved off `__muldi3`. `engine/gte_fix.h`
works around a PSn00bSDK `-O2` GTE-macro miscompile. Overlay toggle on
L1+R1; Hblank-timer profiling (`vid_ticks`).

**Still open, next session:** (1) frame rate — a fresh `-O2` profile, then
either overdraw (per-face frustum cull / lower res) or `draw_face` cost;
(2) affine texture deformation when moving — PS1 has no perspective
correction, fix = subdivide faces at cook time; (3) the floor face at the
camera gets near-plane-clipped into a stretched triangle / hole —
subdivision + a cleaner near-clip. Then the full `cooked/` tree and
**Phase 7** (QuakeC VM).

- **Cooked-texture fidelity levels + runtime reload.** The disc now carries
  all four levels as `START<N>.QTX` / `START<N>.QSF` (`N` = `fidelity_t`
  order). `formats/fidelity.h` reworked: `atlas_cell` (world-tile / atlas
  cell size, 16/32/64 px, capped at 64 by VRAM) and `min_face_area` (cook
  drops world faces below this 3-D area — `low` −36 %, `medium` −5 % of
  `start.bsp`'s faces) are the two knobs consumed today; `max_model` /
  `lightmap_shift` / `geom_decimate` are declared but not yet used, so
  `ultra` == `high` for now.
  - `qcook surfaces` takes `--fidelity`; dropped faces stay empty QSF slots
    so the fidelity-independent `START.QBS` `marksurf[]` indices stay valid.
    `qcook textures` grid stride now follows `atlas_cell`. `scripts/cook.sh`
    cooks QTX + QSF per level; `CMakeLists.txt` bundles all eight.
  - `engine/main.c`: `reload_fidelity()` frees the surface blob (held as the
    top-most high-hunk allocation) and re-loads QTX + QSF for the new level
    (a ~1 s blocking CD read). **L2** cycles the level; this replaces the
    earlier runtime L2/R2 subdivision / far-clip A/B knobs. Overlay shows the
    active level. Headless boot clean (`START2.*` = high). On-screen check +
    the frame rate at a lower fidelity pending.

## [unreleased] — 2026-10-14

Phases 2-6 reached ~20 fps on `start.bsp` (no lighting, no models) against a
constant-60 fps target. From here the engine is built performance first,
with **the whole renderer hand-written in MIPS assembly**. Architecture:
`docs/architecture.md` (validated: 320 × 240). Roadmap phases 2-6 are
followed by R0-R4.

### R0 — Skeleton

- **R0.1** The phases 2-6 code (up to `83c20c7`) removed from master:
  `engine/`, `formats/`, `tools/qcook/`, `tools/mathtest/`,
  `tools/gen_sintab.c`, `scripts/cook.sh`. Kept: CMake build, presets, ISO
  config, `scripts/run-emu.sh`, docs, `third_party/` notes.
  `CMakeLists.txt` no longer bundles cooked assets (back in R2) or adds the
  `formats/` include dir. A placeholder `engine/main.c` (solid background,
  double buffer, `VSync` loop) keeps the build, CD image and headless boot
  check green — headless boot prints `q1psx: R0 skeleton`, no exception.
- **R0.2** `docs/architecture.md` validated, with its plan: phases
  2-6 summed up, then the roadmap (R0 skeleton, R1 perf
  skeleton + benchmark harness, R2 data + `qcook`, R3 asm renderer, R4
  collision + movement), then Phase 7+ unchanged. `docs/porting-notes.md`,
  `TESTING.md`, `README.md` updated.

### R1 — Perf skeleton

- **R1.1** Video core + cycle profiler.
  - `engine/vid.{c,h}`: 320 × 240 NTSC, framebuffers at (0,0)/(0,240), each
    with an 8-bucket ordering table and a 96 KB packet arena. The CPU builds
    frame N+1 while the GPU DMA draws frame N; `vid_flip` waits for the GPU
    and the vblank, shows the finished frame and kicks the next DMA.
    `vid_frame` (`ot`, `pkt`, `end`, `dropped`) is the packet interface the
    asm renderer will write through; `vid_stats` reports vblanks/frame, GPU
    and vblank wait cycles, arena bytes used and dropped packets.
  - `engine/prof.{c,h}`: root counter 2 at sysclock/8 (8-cycle ticks), a
    wrap IRQ extends it to 32 bits; `prof_now()` / `prof_cycles()`.
  - Headless: an empty frame costs 40 k cycles of work — all of it the debug
    overlay (`snprintf` + `FntSort`) — then ~523 k cycles of vblank wait; one
    vblank per frame, stable over consecutive seconds.
- **R1.2** Benchmark harness.
  - `engine/prof`: per-frame stage accumulators (`frame`, `sim`, `r_setup`,
    `r_walk`, `r_block`, `r_slow`, `overlay`) and counters (`nodes`,
    `blocks`, `verts`, `polys`, `slow`).
  - `engine/bench.{c,h}`: per-scenario averages / maxima, one parseable
    `bench ...` line per scenario.
  - CMake option `Q1PSX_BENCH` + preset `bench` (`build-bench/`): boots
    straight into the scenarios, no overlay, prints `bench done`.
  - `scripts/headless.sh`: PCSX-Redux `-no-ui` runner that stops on a regex,
    a crash marker or a timeout (under `stdbuf -oL` — the emulator
    block-buffers redirected stdout). `scripts/bench.sh`: build + run + print
    the bench lines. Two runs: `bench empty n=64 vbl=100 frame=21/24`,
    identical.
- **R1.3** `engine/pad.{c,h}`: BIOS pad driver on port 1, `held` / `pressed`
  (edge) bits. The debug overlay uses the SDK font (`FntSort`), which writes
  into our packet arena; there is no need for a font of our own. L1+R1 toggles
  the overlay, which now also shows the pad state. Headless: pad driver up,
  bench unchanged.

### R2 — Data

- **R2.1** Fixed memory map + CD loader. `engine/mem.{c,h}`: a static 1 MB
  level arena with a bump allocator, reset on map load, plus mark/release for
  transient data. No runtime heap. `engine/cd.{c,h}`: whole-file loads into
  the arena (double speed, `CdReadRetry` ×3). Boot sanity check: `HELLO.TXT`
  reads back 142 B, sum 0x2E47, the same as in phase 2. BSS is now 1.25 MB (2 × 96 KB
  packet arenas + the level arena).
- **R2.2 / R2.3** `qcook` + the engine loader. Full detail in
  `docs/porting-notes.md` §11.
  - `tools/qcook/` rewritten (`qcook.c` pak + BSP29 parsing, `tex.c`,
    `world.c`).
  - `qcook map` writes a QTX3 texture set: tiles of one texture repetition per
    fidelity level, shelf-packed into whole 8bpp pages.
  - It also writes a QWV2 world:
    - faces sit on their BSP node in front/back blocks and are cut on the
      texture-repeat grid (no texture window);
    - pieces become PS1-ordered quads/tris in records of ≤ 63 vertices with
      tpage/CLUT stored once;
    - the file carries the PVS, a node/leaf bbox per element, and bench
      viewpoints (spawn + the heaviest PVS leaves).
  - `qcook check` verifies every invariant.
  - `scripts/cook.sh` cooks the 4 levels and CMake bundles them as
    `START<N>.TEX/.WLD`.
  - `start.bsp`: 17 189 polys / 38 883 verts / 4 868 records, world 844 KB.
    Textures: 1 page (low) up to 6 pages (ultra, native — they fit VRAM).
  - `engine/world.{c,h}`: uploads the pages, loads the world and rewrites its
    links into pointers; `world_point_leaf()`. Level arena raised to 1152 KB.
    Headless: START2 loads (836 KB of arena), spawn in leaf 1084 (empty).

### R3 — Asm renderer

- **R3.1 / R3.2** The world renderer in hand-written MIPS assembly. Full
  detail in `docs/porting-notes.md` §12.
  - `engine/r_world.s`:
    - GTE view setup;
    - camera-leaf lookup and PVS marking (RLE decoded in place);
    - iterative front-to-back BSP walk on a scratchpad stack, with visframe
      and view-frustum culling (MVMVA centre + |R| radius, clip-flag
      inheritance) and marksurface stamping;
    - per-node face blocks on the camera's side: RTPT 3 vertices at a time
      into the scratchpad, then `POLY_FT4` / `POLY_FT3` prepended to the
      world OT bucket.
  - `engine/render.c`: view matrices (world ×4 × 1.5 → view ×6, so the
    GTE's perspective divide stays exact down to 13 units), frame/visframe
    stamps with wrap handling, profiler hooks.
  - Free-fly camera in `main.c` until R4.
  - `tools/pktview`: host rasteriser for the bench's display-list dumps →
    PNG.
  - CMake re-cooks when the cooker or the formats change.
  - Bench (`start.bsp`, high):

    | view | cycles | vbl | polys |
    |---|---|---|---|
    | spawn | 643 k | 2 | 2631 |
    | the 9 others | 104-423 k | 1 | — |

    Images correct. Polygons nearer than 43 u are skipped until R3.3.
- **R3 hardening** — verification tools and fixes found with them. Details:
  `docs/porting-notes.md` §12.1.
  - Per-record bounding-sphere frustum cull: spawn 643 k → 553 k cycles.
  - **Sine table** rebuilt without signed int64 terms. PCSX-Redux's dynarec
    computed the old loop as sin(x) = x (correct under its interpreter),
    which skewed and zoomed every view matrix.
  - **`lw $ra` / `jr $ra` load-delay hazard** in the `r_render` epilogue.
    Hardware and the interpreter returned into the middle of the walk; the
    dynarec hid it.
  - The record sphere's VZ0 is loaded from a sign-extended halfword.
  - With the correct matrix, **every bench viewpoint runs in 1 vblank under
    the dynarec** (spawn 487-532 k cycles depending on the run, 2157 polys).
    The interpreter's
    stricter timing model puts the spawn at 578 k (2 vbl).
  - Images match the z-buffer reference, apart from the near polygons
    (R3.3).
  - `tools/pktview/wldref`: ground-truth z-buffer render, C reference of the
    walk (painter's mode), overlay of the asm's polygon edges.
  - `tools/hazcheck`: post-link pipeline-hazard check that fails the build.
  - `scripts/headless.sh` takes `EMU_ARGS` (e.g. `-interpreter`).
  - **Controls:** the pad reader no longer filters on the pad type (it
    dropped PCSX-Redux's input), and the overlay shows the pad state.
- **R3.3 (functional, C reference)** Slow path for near polygons. Full
  detail in `docs/porting-notes.md` §12.2.
  - `engine/r_slow.c`, called from the asm careful loops:
    - view transform;
    - clip codes, then clipping against the near plane **and the four
      frustum planes** (idea taken from the GBA Quake
      prototype);
    - crack-free per-edge subdivision (split on screen length > 96 px or
      depth ratio > 4/3);
    - raw `POLY_FT3` emission into the world bucket.
  - The cook adds two near bench views: the floor at 60° pitch, and a wall
    at contact distance. The camera stays in empty space: the first attempt
    landed inside a water volume.
  - Images match the reference on every view, including the wall at
    contact. Cost in C: up to 250 k cycles on near views. The asm port is
    next.
  - Profiler: `r_slow` stage and a `slowtri` counter; slow-path timing uses
    16-bit timer deltas (immune to the wrap-IRQ race that produced bogus
    500 k-cycle maxima).
- **R3.3 asm** `engine/r_slow.s`: the slow path ported to hand-written asm,
  with the C version kept behind `R_SLOW_C`. Details: `docs/porting-notes.md`
  §12.3.
  - GTE MVMVA transform, RTPS projection through a 2·I matrix, index-based
    vertex pool, one-word triangle stack.
  - 2-2.5× faster than C; interpreter-validated (identical triangle
    counts).
  - Careful loops now also route polygons that could exceed the GPU's
    size limit to the slow path.
  - Fidelity: `tile_min` repeats small textures inside their VRAM tile at
    low / medium, so polygons span several repetitions. Map polygons:
    17 189 (high) → 12 374 (medium) → 9 927 (low). The spawn view runs in
    **1 vblank at low (433 k cycles) and medium (483 k)**; high needs 2
    (620 k).
  - `FID=n scripts/bench.sh` benches any fidelity level.
- **Fidelity switch** (as in phases 2-6): **L2** reloads the map at the next level
  (low → medium → high → ultra), keeping the camera where it is. The overlay
  shows the current level.
- **R4.1 / R4.2 — Collision + player movement** (C reference). Full detail:
  `docs/porting-notes.md` §13.
  - The cook exports the clip hulls (all BSP planes + clipnodes, hulls 1
    and 2) into QWV2. `engine/clip.c`: a fixed-point port of
    `SV_RecursiveHullCheck`.
  - `engine/pmove.c`: phase 6's player physics (Quake `SV_FlyMove`,
    friction / accel / air accel, jump, step-up), at a fixed 60 Hz tick
    (one tick per vblank, at most 4 per frame).
  - The pad now drives the player instead of the free-fly camera:
    - d-pad: turn / walk;
    - Square / Circle: strafe;
    - L1 / R1: look;
    - Cross: jump;
    - Triangle: walk slowly;
    - START: noclip;
    - SELECT: respawn.
  - Fixes for sticking on ramps, found with the new host simulator:
    - positions in 1/8 unit;
    - the trace interpolates plane distances along the whole sweep, so a
      rounded split point no longer tips a slide into the slope;
    - impact points are rounded toward the open side;
    - slides are kept off every plane already hit;
    - the crease projection is normalised (Quake's halved the speed in a
      45° wedge);
    - a grounded player is snapped to the floor (it used to hover up to 2
      units above it).
  - `tools/pmsim`: runs `clip.c` + `pmove.c` on the PC.
    - Same scripted path as the PS1 bench, with identical positions.
    - Stress mode over 5000 random segments (983 k units): 0 ticks in
      solid, 0 blocked with the way ahead open.
  - Cost on the dynarec: **24 k cycles per tick on average, 54 k max** (4-10 %
    of a 60 fps frame). The asm port is next (2026-10-15).
- **R4.2 validated:** a test of the controls on the pad finds
  them fine for now; they can be refined later.
- **R4.3 — physics in asm** (2026-10-15: everything CPU-heavy goes to
  asm). Details: `docs/porting-notes.md` §13.6.
  - **C rewritten as the specification** (`clip.c`, `pmove.c`):
    - 32-bit arithmetic plus 32 × 32 → 64 products only (no libgcc 64-bit
      division);
    - an iterative trace;
    - Quake's sv_maxvelocity cap (2000 u/s per axis), which bounds every
      product;
    - diagonal wish directions by 1/√2 instead of a square root and
      divisions;
    - friction as one division.
    - This rewrite alone took the physics from 24 k to ~9.4 k cycles per
      tick.
  - **`engine/clip.s`:**
    - axial planes (78 %) on the CPU from start / delta × 4096 kept in the
      scratchpad;
    - general planes through one GTE MVMVA, with start / delta / probe
      point as the matrix rows;
    - the split division in 32 bits, its shift taken from LZCS/LZCR;
    - pending splits on the stack.
  - **`engine/pmove.s`:** a full port, with the `pmove_t` and every
    intermediate vector in the scratchpad.
  - **Clip format** (same size):
    - distances pre-scaled to 1/8 unit;
    - clipnodes store their plane's word offset (index × 3);
    - the cook checks that clip trees are at most 64 deep.
  - **Differential tests** in the bench. The C is linked as
    `clip_trace_c` / `pmove_step_c`:
    - 4000 sweeps + 4000 contents queries: 0 mismatches;
    - 6000 random-walk ticks (commands, jumps, noclip, teleports): every
      `pmove_t` field identical;
    - the scripted path gives identical positions;
    - all of it holds under the dynarec and under the interpreter.
    - `VIEWS=0 scripts/bench.sh` runs only these checks.
  - **Cost (dynarec):**

    | measure | before R4.3 | now |
    |---|---|---|
    | random walk | 24 k | 6.8 k (C reference: 9.4 k) |
    | scripted path (≈ 8 traces / tick against the end wall) | 13.3 k | 11.5 k |
    | one trace | — | ~1.6 k |

    Traces are about 3/4 of the cost.
  - **Fixes found on the way:**
    - CMake did not rebuild asm objects when an `.inc` changed: a stale
      `pmove.s` overwrote the player position. Asm sources now depend on
      `engine/*.inc`.
    - GAS expands the 2-operand `div` / `divu` into a checked macro; the
      bare form is `div $zero, rs, rt`.
    - `prof_now` could go back by one counter wrap when the wrap IRQ was
      still pending (bogus 524 k-cycle maxima).
- **R5 decided** (2026-10-15):
  - plan: every map loadable, then texture windows, then shared vertices
    and a compact format; lossy decimation only where still needed;
  - target about 700 KB of world per map.
  - Measurements of all 32 maps and a texture-window simulation (high
    fidelity −40 to −55 % polygons at no VRAM cost):
    `docs/porting-notes.md` §14.1.
- **R5.1 — every map loads.** Details: `docs/porting-notes.md` §14.2.
  - **Renderer walk stack:** Quake BSPs reach 110 levels. The 64-word
    scratchpad stack now spills whole blocks to RAM (depth ≤ 256). Forced
    spills give identical display lists; the walk costs about +1 %.
  - **Physics:** the trace's pending-split limit is 128 (hull 2 reaches
    65).
  - **Disc:** every cooked map ships as `\MAPS\<MAP>_<N>.TEX/.WLD`. The
    subdirectory is needed because the BIOS reads only the root
    directory's first sector.
  - **Cook:** `scripts/cook.sh` runs in parallel. `COOK_MAPS` takes `ep1`
    (default), `all` (retail pak1, with the palette taken from pak0) or a
    list.
  - **Engine:** `world_load(map, fidelity)` falls back to a lower level
    when a map does not fit the arena (e1m4 high → medium). **R2** cycles
    the maps.
  - **Bench:** `BENCH_MAPS=1` benches every map's spawn views; all 9
    episode-1 maps load and render.
- **R5.2 — texture windows.** Details: `docs/porting-notes.md` §14.3.
  - **Cook:**
    - every power-of-two texture axis wraps in hardware through the GPU
      texture window (GP0 E2h);
    - textures are packed aligned to their size;
    - faces are cut only every 256 texels, from their own repetition
      boundary;
    - axes that are not powers of two keep the old per-repetition cuts.
  - **Format QWV3:** a per-map texture table (clut, tpage, window); records
    hold a texture index.
  - **Renderer:** one E2 packet per drawn record, prepended after its
    polygons, plus a reset at the end of the world bucket.
  - **Tools:** `pktview` and `wldref` apply the window; images match the
    reference.
  - **Results:**
    - start high: 17 189 → 8 207 polygons;
    - spawn view: 620 k cycles (2 vbl) → 426 k (1 vbl);
    - every episode-1 bench view now runs in 1 vblank;
    - world size: start 938 → 720 KB, e1m4 1160 → 823 KB (fits at high
      now), e3m5 1427 → 933 KB;
    - low-fidelity textures 304 → 25 KB.
  - **Costs:** some near views spend more in the slow path (larger
    polygons). Affine warp is slightly more visible on big polygons; the
    eye on screen decides.
- **R5.3a — shared vertices.** Details: `docs/porting-notes.md` §14.4.
  - **Format QWV4:** a per-map vertex pool; records hold 16-bit indices
    (padded to threes) and their polygon offset.
  - **Engine:** the renderer, the slow path and the tools gather through
    the pool.
  - **World, high:** start 720 → 648 KB, e1m4 823 → 741 KB; episode 1 now
    spans 191-741 KB.
  - **Cost:** about +5 % on the heaviest frame (426 k → 450 k, still
    1 vblank). Images identical to R5.2.
  - `cook.sh` starts from an empty `cooked/id1`, so a stale map of an
    older format can no longer reach the disc.
- **R5.3b — compact nodes and leaves.** Details: `docs/porting-notes.md`
  §14.5.
  - **Format QWV5:**
    - nodes 56 → 44 B, leaves 36 → 28 B;
    - visframe and type in the bbox padding (VZ loaded with lh + mtc2);
    - 16-bit parent word offsets;
    - two face pointers per node plus a sentinel node.
  - **World, high:** start 636 → 589 KB, e1m4 729 → 677 KB, e3m5 823 →
    768 KB. All 32 maps average 565 KB; 3 remain above 700 KB.
  - **Cost:** about +3 % on the walk; images identical.
  - **Build:** a build-triggered re-cook keeps the cooked map set (it
    broke the ISO step).
- **R5.3c — several faces per record.** Details: `docs/porting-notes.md`
  §14.6.
  - A record merges faces that qbsp cut for lightmaps: adjacent, same node
    side and texture, marked by exactly the same leaves (visibility stays
    exact), within a 160-unit radius (the frustum test stays tight).
  - Records are the visibility ids, and the marks are rewritten to them.
  - Every bench view is at or below its R5.3b cost (start's spawn view
    454 k → 436 k); images identical.
  - World: e3m5 768 → 739 KB, e1m4 677 → 655 KB; the 32 maps average
    547 KB, and 3 are slightly above 700 KB.
  - Looser grouping variants were measured and rejected: they drew up to
    +70 % polygons.
- **R5.3d — the last lossless crumbs.** Details: `docs/porting-notes.md`
  §14.7.
  - **Format QWV6:** render nodes reference the clip section's plane table
    (44 → 36 B); clipnodes go from 8 to 6 B.
  - **World:** all 32 maps fit the 700 KB target (largest e3m5 696 KB,
    average 514 KB).
  - **Cost:** walk +1 %, trace +5 % (about 260 cycles per tick); images
    identical; physics still bit-exact.
  - **R5 summary:** world 1030 → 514 KB on average, 1427 → 696 KB at
    worst; start's spawn view 620 k → 440 k cycles (2 → 1 vblank); no
    lossy step needed.
- **Static lighting + dithering** (2026-10-15: the quake-psx look).
  Details: `docs/porting-notes.md` §15.
  - **Cook:** Quake's lightmaps are sampled per vertex (bilinear, all
    styles) into one light byte per record vertex (format QWV7). 128 =
    texture as is, matching Quake's software colormap. `LIGHT_GAIN` scales
    it (default 1.0, faithful).
  - **Renderer:** POLY_GT4 / GT3, texture × light. The colours are expanded
    while the GTE computes the RTPT, and the slow path interpolates them.
    Dithering is enabled in the drawing environment (polygons cannot set
    it).
  - **Cost:** start's spawn view 440 k → 481 k cycles (1 vblank); world
    534 → 570 KB.
  - **Verification:**
    - `vid_dump` prints a hash of the display list (the interpreter's
      console garbles long dumps); dynarec and interpreter hashes match;
    - new `scripts/dlcheck.sh` for the optimization pass;
    - the disc image now depends on the cooked files.
- **R6.2 — maximum optimization pass, no quality loss** (2026-10-15).
  Details: `docs/porting-notes.md` §16.
  - **Face records:**
    - `draw_records` runs inline in the walk, with packets through KUSEG;
    - a texture cache, since 3 records in 4 reuse the previous texture;
    - one packet-space test per record instead of per polygon
      (`QW_MAX_REC_PKT`, checked by the cook's validator);
    - one texture-window command per run of same-texture records
      (1649 → 379 over the bench views, same pixels);
    - RTPS for the last 1-2 vertices instead of a padded RTPT.
  - **Walk:** frustum and plane distance inlined; a cheaper stack word and
    `|d| <= eps` test; the mark loop on an end pointer.
  - **Slow path:**
    - the colour and uv words are computed once per vertex;
    - exact split tests with no calls (`|dp|, |dq| <= SPAN`, branch-free
      depth ratio);
    - midpoint and projection in one pass;
    - an inline clipper with a per-plane distance pass and a pipelined
      lerp.
  - **Profiling switch:** CMake `Q1PSX_PROFILE` and a `release` preset
    without the renderer's counters and timers; `PROFILE=0
    scripts/bench.sh` measures it.
  - **Result:**
    - 12 bench views −17.6 % (profile build), −20.8 % (release); start's
      spawn view 481 k → 396 k cycles;
    - all 128 spawn views of the 32 maps take 1 vblank (e3m4 669 k →
      540 k);
    - display lists identical, interpreter hashes = dynarec, physics
      diffs 0 mismatches.
- **Light gain 1.5 by default** (2026-10-15): `cook.sh` and `qcook`
  default to `--light-gain 1.5` instead of 1.0 (faithful but dark).
  - Same cycles.
  - World sizes +584 bytes over the 32 maps.
  - New display-list reference `lit15`.
- **Map menu + loading bar** (2026-10-15). Details:
  `docs/porting-notes.md` §17.
  - **Menu.** At boot, a menu lists every map on the disc (two columns)
    and the fidelity level; SELECT + START in game comes back to it. START
    is tagged `(default)` and e3m4 `(complex)` (the heaviest view of all
    maps, 540 k cycles).
  - **Loading bar.** Loads show the map, a progress bar and a
    percentage: CD reads run in the background (`CdReadRetry` +
    `CdReadSync(1)` polled once per frame). There is no more black
    screen.
- **Analog controls** (2026-10-15). Details:
  `docs/porting-notes.md` §18.
  - **Left stick / d-pad:** walk and strafe, with a speed that follows the
    stick.
  - **Right stick:** the camera, inverted up / down; a quadratic response
    fine near the centre.
  - **Square / Circle** turn (pads without sticks).
  - **Physics:** `pmove` takes analog commands the Quake way
    (`wishspeed = |wishvel|`, capped). The d-pad gives exactly the
    previous result. C and asm are bit-exact under the random analog
    differential test (dynarec and interpreter).
  - **Analog mode** needs the pad's ANALOG button: the BIOS driver cannot
    force it.
- **Warping and cracks, step 1: T-junction repair in the cook**
  (2026-10-15). Details: `docs/porting-notes.md` §19.
  - **Diagnosis:**
    - the PS1 GPU maps textures affinely, whereas the GBA reference divides
      per pixel; the affine error averages 5-7 px over the bench views;
    - cracks come from cooked T-junctions, from the slow path's
      subdivision, and from its separate projection.
  - **Repair:** qcook cuts every face first, inserts the vertices lying on
    other pieces' edges, and ear-clips pieces with collinear boundary
    vertices, pairing the triangles back into quads.
  - **Result on start:** 0 T-junctions, +1.6 % polygons, world +2 KB on
    average.
  - **Tools:** `pktview` takes `PKTVIEW_BG` (a clear colour that makes
    cracks visible); `wldref`'s overlay reads Gouraud packets.
- **Warping and cracks, steps 2-3: error-based subdivision**
  (2026-10-15). Details: `docs/porting-notes.md` §19.2-19.3.
  - **Rule:** an edge splits while its affine error exceeds T px, or while
    |dx| + |dy| > 500. T follows the fidelity level: low 6, medium 4,
    high 3, ultra 2. The midpoint is the projected 3D midpoint, computed
    from the edge's two ends only, so neighbours split alike and no
    crack opens.
  - **Where:**
    - `draw_records` tests each polygon of a flagged record
      conservatively (`CAREFUL`);
    - the others go to the rewritten slow path, which reuses the fast
      path's RTPT results and clips against the near plane only.
  - **Spec and port:** `engine/r_slow.c` is the specification.
    - `engine/r_slow.s` is a new kernel: a midpoint table that never needs
      clearing, one stack word per triangle, children through a jump
      table, divides overlapped with work.
    - Display lists are identical, word for word, at high and low (`dlcheck`
      references `c2hi`, `c2lo`); interpreter hashes match.
  - **Result:**
    - cracks: none left on the bench views (only the undrawn sky shows);
    - simulated mean affine error on start: 1.4 / 1.0 / 0.8 / 0.5 px from
      low to ultra (5-7 px, up to ~90, before);
    - spawn views at 1 vblank (release, 128 views): low 121, medium 110,
      high 101, ultra 80 (all 128 before); the worst, e3m4's, goes from
      540 k to 842 k (low) … 1 517 k (ultra).
  - **Open:** reconciling 60 fps with the threshold (§19.3).
- **Warp threshold in the menu, three modes** (2026-10-16). Details:
  `docs/porting-notes.md` §19.4.
  - One threshold for every fidelity level, 8 px by default, chosen in
    the boot menu: 1, 2, 4, 6, 8, 10, 12, 14 or 16 px.
  - **Modes:**
    - **fixed**;
    - **dynamic:** T rises when a frame overruns and comes back to the
      chosen floor when frames have room again;
    - **budget**, after Quake II PS1: T rises and falls against a triangle
      count per frame, from 1000 to 4000.
  - T only changes between frames, so there is still no crack.
  - **Start at high, release:**
    - fixed 8 px: 11 of 12 views in one vblank;
    - dynamic: view 0 settles at 32 px within one vblank.
  - **All 128 spawn views at high, fixed 8 px:** 122 in one vblank (101 at
    3 px); worst e3m4 v0 737 k (1 159 k).
  - **Bench:** `T=<px>`; reference `t8hi`. The C reference matches it.
- **4-bit textures and dithering in the menu** (2026-10-16). Details:
  `docs/porting-notes.md` §20.
  - `qcook --tex4` writes a QTX4 next to each QTX3 (disc `<MAP>_<N>.TX4`).
    The tiles stay in place; each texture gets its own 16 colours (median
    cut, then Lloyd). The engine patches the CLUT / tpage words at load.
  - **Comparison on start:**
    - ~1 % of pixels differ from 8 bits, mean deviation 0.04 levels;
    - only a few multicoloured textures lose specks of colour;
    - CPU cost identical; the GPU gain needs real hardware.
  - **Dithering on / off** in the menu (the draw environment's bit).
  - **Tools:**
    - `pktview` reads QTX4 and takes `PKTVIEW_DITHER=0`;
    - `scripts/bench.sh` takes `TEX4=1`;
    - the disc grows by 8.4 MB.
- **Second asm pass** (2026-10-16). Details: `docs/porting-notes.md` §21.
  - **Cost model, measured:** PCSX-Redux charges the GTE (MVMVA 8, RTPS 15,
    RTPT 23), `mult` (6 / 9 / 13 by the size of rs) and `div` (36)
    latencies, and 3 cycles per store. Filling latencies with needed work
    became the rule.
  - **Walk:**
    - a general plane's n · cam is one MVMVA on the colour matrix, whose
      first row is the camera;
    - FRUSTUM's two MVMVA shadows take the node's plane setup;
    - `mark_pvs` stops at a byte's last set bit and tests no bound per
      leaf (a mask on the row's last byte: `r_state.vis_lastmask`).
  - **Records:**
    - CAREFUL keeps T2 zmin in LO, so its spill is gone (`SP_T4` = 2 T2);
    - a record CAREFUL will not see skips the SZ stores (the `XFORM`
      macro);
    - no `nop` at the polygon loops' entries.
  - **Slow path:**
    - `sm_new` is scheduled around HI / LO, and its z T2 needs no multiply;
    - the clip path is rewritten around its latencies: view entries next
      to the pool, a pipelined `sp_lerp`, `sp_proj` in shifts;
    - the slow triangle count is `slow_bytes / 40`.
  - **Physics:** read; what is left there is under 0.05 % of a frame.
  - **Result:**
    - 12 bench views: −8.8 % (4 757 602 → 4 340 164 cycles), display
      lists identical;
    - 128 spawn views (release): 124 in one vblank (122), mean 225 k
      (247 k), worst e3m4 v0 675 k (737 k), display-list hashes identical
      to the asm from before the pass.
  - **Tools:** with `BENCH_MAPS=1`, the bench prints each spawn view's
    display-list hash and `scripts/dlcheck.sh` compares them; `vid_dump`
    can print the hash alone.
- **Controls, weapons and the HUD** (2026-10-17). Details:
  `docs/porting-notes.md` §22.
  - **Game data:** `qcook game` cooks the 8 view models, the projectiles,
    the explosion sprite and 112 HUD pictures into `GAME.DAT` (formats/
    qgame.h): models become face records in the world's format, one vertex
    pool per frame, skins at half resolution in VRAM pages 6..9 (maps keep
    0..5). QWV8: a leaf's light byte (models are lit with their leaf's).
  - **Renderer:** a render list per BSP leaf (`engine/rlist.c`), drawn by
    the walk in the painter's order; particles as tiles, models through
    the record loop (now the `RECLOOP` macro) with their own GTE matrix and
    per-vertex light from the normal tables; the view model first, on top,
    through the fast path only. The 3D viewport can be 192 rows (the GTE's
    centre and the GPU's draw area follow); the overlay chain draws 2D
    packets in order over the whole screen.
  - **Hull 0** (the point hull) traces the render BSP: `clip_trace` /
    `clip_contents` in asm and C, `clipdiff` 0 mismatches.
  - **The game** (`engine/game.c`, native C, permanent): every weapon
    (weapons.qc), the weapon animations (player.qc), projectiles with
    hull-0 traces, grenades that bounce, explosions (sprite + particles),
    the lightning beam, T_Damage / T_RadiusDamage on the player (rocket
    jumps), death and respawn, the view's bob, lean, punch and damage
    kicks. Everything is given at the spawn (no items yet). No sounds, no
    dynamic lights, no water.
  - **HUD** (`engine/hud.c`): Quake's status and inventory bars over a
    192-row view, or a full-screen Quake II PSX-style layout; chosen in
    the menu.
  - **Controls, scheme B:** R2 / Square fire, L2 / Cross jump, R1 / L1
    next / previous weapon, Triangle walk, Circle level the view, d-pad
    weapon groups; START the overlay, SELECT + START the menu, SELECT +
    Cross noclip, SELECT + Square respawn; a digital-pad mapping too.
  - **Tools:** `BENCH_GAME=1 scripts/bench.sh` fires every weapon and
    dumps a frame per weapon and per HUD; `pktview` renders sprites, tiles,
    DR_TPAGE, the draw area and GAME.DAT's images, and takes the frame's
    name and viewport from the dump.
  - **Cost:** start's spawn view 617 k → 680-735 k cycles with the view
    model and the HUD; 815 k while the lightning gun fires.
- **The weapon while moving** (2026-10-17: a swaying gait Quake
  never had). The view model's matrix was built from the previous frame's
  camera (the render list came before `r_set_view`), so the weapon lagged a
  frame behind while moving. Fixed; the bob and roll are Quake's (view.c,
  verified tick by tick in the bench's walk). The view model is now clipped
  at 5 units like Quake's (its pumped-back frames put most of it behind the
  eye) through the slow path without subdivision, projected with H = 40 and
  x4 rows so the GTE's divide holds; the cook drops its never-seen
  triangles. `sp_lerp` no longer clobbers the near plane's register.
- **The first play session: controls and blue pixels** (2026-10-17).
  Details: `docs/porting-notes.md` §22.8.
  - The pad reported itself digital, so the digital mapping was in effect:
    `engine/pad.c` now switches a DualShock to analog mode itself at boot
    (the configuration commands on SIO0, with timeouts and retries), and
    the digital scheme keeps scheme B's buttons (Triangle held: strafe and
    look on the d-pad).
  - Blue pixels on the barrels: the skins packed edge to edge bled into
    each other on the GPU's stray texel (a 2-texel border of their own
    edge texels now, UVs clamped), and the skins' unused blue fill was
    averaged into the edge texels by the half-resolution reduction (only
    the texels inside a triangle count now, the rest takes its neighbours'
    colours). Projectile skins at a quarter resolution, a MaxRects packer:
    208 K of 256 K texels placed.
  - The view model's cook-time back-face culling is geometric (the plane
    against the eye's box in every frame) instead of vertex-normal based.
  - PCSX-Redux's newer builds run headless at real-time speed: the game
    bench's default limit is 420 s (TESTING.md); the boot's map scan reads
    the MAPS directory once instead of searching 128 files (10 s on a
    drive). `bench.sh` aborts when the build fails instead of running
    the previous binary.
- **Sound and music** (2026-10-17; CD-DA so the music takes no RAM). Details:
  `docs/porting-notes.md` §23.
  - `qcook map --sounds`: one bank per map (`formats/qsound.h`, QSB1,
    `MAPS/<MAP>.SND`): Quake's always-precached set less what the map
    cannot trigger (no water: no drowning...), the ambient sounds its
    entities spawn with, the area ambients; WAVs read as `GetWavinfo` does
    (cue point and mark length = the loop), encoded to SPU ADPCM
    (best filter and shift per block, loops block-aligned); the longest
    looping ambients halved in rate when the 508 KB of the SPU overflow.
    The world file carries each leaf's water and sky ambient levels
    (QWV9).
  - `engine/snd.c`: snd_dma.c on 24 voices — 2 area ambients (the camera
    leaf's levels, faded), 8 dynamic channels (`SND_PickChannel`,
    `SND_Spatialize`, the end tick, the same-frame offset), one looping
    voice per distinct static sound with the sources' volumes summed
    (Quake's combine). `engine/game.c` plays what the QC plays: the
    weapons, the lightning's lstart / lhit, spike tinks and ricochets,
    the grenade's bounce, explosions, jump, landing (land2 and 5 damage
    from −650), pain (every 0.5 s, not while the weapon animates), death
    and gibs; the map menu's clicks.
  - `engine/music.c`: the map's CD track (worldspawn "sounds") in a loop
    through CD-DA with auto-pause; stopped for the CD reads of a map
    load. `scripts/cook.sh` converts the rerelease's tracks with ffmpeg,
    CMake lists them as audio tracks (`Q1PSX_MUSIC`, off for the bench).
  - The game bench logs every sound start (deterministic: the dynarec
    and the interpreter must agree line by line).
- **Dynamic lights** (2026-10-17). Details: `docs/porting-notes.md`
  §24. Quake's cl_dlights (the muzzle flash at the firing frames, a
  rocket's, an explosion's with its decay), applied in the renderer's
  walk: a face block about to be drawn goes through `dr_lights`
  (`r_world.s`, asm; `engine/dlight.c dl_block` is the C reference), and
  its stamped records within a light's reach get `rad − dist` on their
  vertices' light bytes as R_AddDynamicLights gives it to lightmap texels
  (`rad = radius − |plane distance|`, the minlight cutoff, the distance
  to the impact point from a square-root table), saved once a frame and
  restored after it; entities and the view model add `radius − distance`.
  Two enumerations tried first (R_MarkLights' planes, the leaves) cost
  60-550 k cycles a frame on the bookkeeping alone. The bench has a
  hash-only mode (`HASHONLY=1`) and a `DLIGHT_C=1` build for the
  C-versus-asm check.
- **The water** (2026-10-17). Details: `docs/porting-notes.md` §27. Quake's swimming in
  `pmove_step` (SV_CheckWater, SV_WaterMove, SV_WaterJump, PlayerPreThink's
  drag, PlayerJump held with FL_JUMPRELEASED; the C reference and the asm,
  0 mismatches over 6 000 ticks with runs started in the water), the game's
  WaterMove (air, drowning, lava and slime burns, the sounds in and out),
  CheckWaterJump, the swim and splash sounds, the water pains and death,
  the lightning's discharge, the bubbles (a new sprite in GAME.DAT: recook
  it), the view's tint by the contents (a semi-transparent flat). The
  jump moved to Quake's place in the tick (after the acceleration) and is
  now a held button. `tools/pktview` now blends semi-transparent tiles with
  the page's mode (the tint shows in `gwater.png`). Verified on DuckStation
  (the play bench: 32 maps, no invalid read, no GPU error).
- **Levels hanging at 99 % on DuckStation** (2026-10-17: on DuckStation, not on PCSX-Redux). Details:
  `docs/porting-notes.md` §26. Reproduced headless (DuckStation from a
  private home with its console and TTY log, the all-maps bench with the
  music disc): the load hung at the first map whose 4-bit CLUT row is an
  odd count of CLUTs. `LoadImage` sends the data by DMA in 16-word units,
  rounding up: up to 60 bytes past a rectangle's data go to the GPU as
  commands (`GP0 0xF8`, a VRAM copy with garbage coordinates), the GPU
  loses the stream and the next `DrawSync` never returns; PCSX-Redux
  ignores the stray words. Now a rectangle whose size is not a multiple
  of 64 bytes goes up from a zero-padded copy (`vram_upload`, the world
  and the game data), and the cook aligns rectangles to 64 bytes with
  zeros after them. Also: `CdGetToc` right after `CdInit` answered 0
  tracks on DuckStation (the drive not ready yet; retried for 3 s now), and
  `music_play` indexed the table of contents one track off (`toc[0]` is
  track 1). The bench disc can carry the music (`MUSIC=1`), and the maps
  bench can go through the game's own path, the loading screen and 30
  interactive frames per map (`PLAY=1`): both ran on DuckStation. The
  play bench found a fourth fault: the ambients' leaf index mixed the
  walk's KUSEG leaf pointer with the KSEG0 leaf table (a wild read every
  frame: silent on PCSX-Redux, a bus error on the machine); fixed.
- **Third optimization pass, the whole engine** (2026-10-17). Details: `docs/porting-notes.md`
  §25. The dynamic lights' node descent and restore in asm, one entry for
  the record hook, the light gate after the record's frustum test; the
  sound's statics precomputed (voice, reach, attenuation multiplier), a
  table square root, the renderer's view leaf for the ambients; r_part.c's
  particle lists, bounded projectile loops, the entity matrices by the
  GTE; the HUD's sprites cached in the frame arenas' tail and rebuilt
  only when what they show changes; the pad polled by the game (port 1,
  in the vblank callback); the leaf list flag in the vis word's sign.
  Start's spawn, shotgun view: 773.6 k → 712.8 k cycles (−7.9 %), the
  rocket's explosion 1 271 k → 1 128 k, the HUD views −10 %; `snd` 15.9 k
  → 3.6 k, `hud` 9.8 k → 0.45 k, `ents` 14 k → 6 k. Hashes identical
  throughout (the particles' draw order aside: 2 to 15 pixels differ
  where two particles share a pixel), C = asm,
  interpreter = dynarec, the world views identical. Measured and left to
  a later call: the 192-row frustum (−7 k for ~4 KB of code).
- **Brush models and the entity table** (2026-10-17: the progression mechanics' first stage: doors, lifts, buttons).
  Details: `docs/porting-notes.md` §28. The cooked world is QWVA: every
  brush model's faces on its own BSP nodes in the face stream (face id
  0, T-junctions repaired within a model), a `qw_bmodel` table (bounds,
  hull-0 root, clip roots, depth), the entity lump cooked into a `qw_ent`
  table (86 classes, the 24 keys of the id1 maps, a string pool) and the
  spawn functions' sounds in the bank (by class, `sounds`, worldtype;
  the budget's second and third halving passes); the textures of every
  drawn model packed (the world's only before: a door with its own
  texture had no faces), the triggers' brushes left out. The engine spawns the
  table into edicts (`engine/ents.c`): the brush models drawn by a walk
  of their subtree from the model-space camera (`draw_bmodel`, the record
  loop of draw_model, their cooked light kept), attached to the visible
  leaf of their box nearest the eye; the solid ones in the traces
  (`clip_trace` = SV_Move: the world, then SV_ClipToLinks over them, in C
  and asm, the trace's `ent`). Every map recooked (32 x 4). Checks:
  `clipdiff` and `pmdiff` 0 mismatches with START's 15 solid models in
  the traces; the 12 world views and the game's frames pixel-identical to
  the references (no model in them; the water frames' references were
  the pre-blend renders, refreshed); e1m1's views with its doors in
  place; DuckStation's play bench over the 32 maps clean (0 GP0 errors,
  0 invalid reads). Costs: a trace 1 825 → 2 646 cycles and a physics tick 7 744 →
  9 818 on START (the box test over its solid models: Quake's area
  nodes come with the movers), the entities' stage +0.15 k at START's
  spawn and 5.8 k on e1m1's door view with 5 models drawn (the visible
  models listed per PVS stamp, the leaves as pointers, a static model's
  matrix copied from the view's).
- **Triggers, teleporters, the level's end, doors, plats, buttons,
  trains, items** (2026-10-17; stages 2 to 4). Details:
  `docs/porting-notes.md` §29. The edicts run as the QC does (think /
  touch / use / blocked, SV_Physics_Pusher with its `ltime`, SUB_CalcMove
  exact in ticks, the player pushed and carried, SV_Impact through pmove's
  new `touched`, damage on edicts, the skill filter); triggers.qc and
  misc.qc (multiple, once, secret, relay, counter, teleport, push, hurt,
  setskill, onlyregistered); client.qc's level end (the intermission with
  its stats, the next map with the parms, the episodes' end texts read
  from progs.dat by `qcook game`); doors.qc, plats.qc, buttons.qc
  (linked doors, keys, secret doors, plats, buttons, trains); items.qc
  and misc.qc (health, armour, ammo, weapons, keys, the rune, the four
  powerups with their warnings, flames, fireballs, explosive boxes, spike
  shooters), their models cooked per map (squeezed at ultra on the two
  maps whose pages overflowed); the screen's colour shifts (damage, bonus,
  powerups) on the GPU's blends; the HUD's powerup faces, 666 and the
  disc, the weapons' flash, the notify lines; box hulls in the traces
  (SV_HullForBox, C and asm) for the barrels. Every map recooked; GAME.DAT
  is `QGM3`, the world `QWVB`.
- **The game loop on the vblank** (2026-10-17). §30: the
  pad's polls kept per vblank in a ring, one game tick per poll with that
  poll's command; the time and the input at 60 Hz whatever the frame rate.
- **Quake's menu and the release editions** (2026-10-18). §31: the
  `release` preset boots START under Quake's main menu (Single Player /
  New Game, Options with Quake's rows, the volumes working, the rest
  greyed; a Graphics page with every rendering setting and the dynamic
  lights' switch, the budget greyed outside the budget mode, the threshold
  a floor in the dynamic modes); the world paused under the menu; low to
  high fidelity on its disc, ultra alone on the `release-ultra` disc. The
  settings moved to `engine/settings.c`, shared with the debug build's
  map menu; the sound effects' and the music's volumes separate.
- **The animated textures** (§32): Quake's `+0..+9` frames every 0.2 s and
  the `+a..` alternates of a pressed button or a toggled wall, done in
  VRAM: the cook packs every frame of a chain a face uses and a display
  tile its faces draw with (the world is `QWVC`, its `qw_anim` table); the
  engine moves the frame of the moment onto the display tile (GP0 80h, the
  4-bit set's CLUT with it) at the head of the display list. When a map's
  pages are full, the models' textures are squeezed first and the
  animation left out last (start at ultra). pktview copies VRAM too.
- **The menu's colours and its entries** (first test, 2026-10-18: a graphics bug, then the modes and options the port does not handle removed). The aux pictures' and the 4-bit textures' CLUTs
  ran into the texture pages' rows (x >= 320, rows 480 .. 511): a map's
  full sixth page overwrote the menu's palettes. Every CLUT now sits under
  the framebuffers (x < 320, rows 480 .. 511); GAME.DAT is `QGM4`, the
  4-bit sets `QTX5`; pktview loads the game data first as the console does.
  The menu keeps only Single Player (New Game), Options (Reset to defaults,
  the two volumes, Graphics), and the fidelity row only on a disc with a
  choice. A row of the inventory bar, overwritten at boot by the old aux
  CLUT row, is right again.
- **The second play test** (2026-10-18). §33: START's floor over the pit
  painted in the wrong order (the buried faces of the still brush models
  left out at the cook, a spanning model drawn in the leaf on the eye's
  side of its nearest face); the status bar's face no longer upside down;
  the original HUD drawn part by part, each part only into a framebuffer
  that does not show it yet; the dead player's solo scoreboard; a frame
  rate cap (60 / 45 / 30 / 15) in Graphics. The tick rate measured: 21 k
  to 48 k cycles a tick, a lower rate would save 2 to 4 % of a frame: not
  done.
- **The fourth optimization pass** (after the second play test: still too slow, no monsters yet). §34: a PC sampler in the bench (`SAMPLE=1`),
  the emulator's cost model re-measured (byte and halfword loads 3
  cycles, word loads 2), pktview's occlusion statistics. Display lists
  identical: the dynamic lights' node marking descends only the light's
  side of a far plane (R_MarkLights), the brush models' leaf search starts
  from their top node (e1m1 up to -23 %), the lights' squared distance by
  the GTE's SQR, a polygon wholly behind the near plane or off one side of
  the screen no longer goes through the slow path (the view models' backs:
  the game bench -3.9 %, start -2.4 %).
- **The tick rate setting** (2026-10-18). §35: 10, 20, 30, 32 (the
  default) or 60 game ticks a second in Graphics; the time stays in 60ths,
  a tick covers its polls and the rates act over them (pmove C and asm,
  gravity's midpoint over a long tick so a jump keeps its height), the
  view turns every vblank and the camera is drawn between two ticks. The
  simulation's cost a vblank -31 % at 32 Hz, -64 % at 10.
- **The liquids lit, the view's colour by contents** (third play test).
  §36: the liquids (water, slime, lava, teleporters), unlit in Quake, were
  cooked black (no lightmap): their vertices now take Quake's normal light
  at the cook's gain, and the leaves' light leaves them out as
  R_LightPoint does. The view's colour follows WinQuake's
  V_SetContentsColor (nothing in the solid, the water's for any other
  contents); the water's colour is Quake's own (130, 80, 50). Every map
  recooked.
- **The draw distance and the flat shading** (2026-10-18). §37: two
  graphics options, off by default and free while off (the record loops'
  copies entered by self-modifying code). The draw distance (2048 to 768
  units) fogs to black over its last quarter, culls what lies beyond,
  draws the fog's records flat and skips the PVS's far leaves: the
  heaviest views -9 to -27 % at 768. The flat shading (far, all) draws a
  record in its average light (POLY_FT4 / FT3): -1.1 to -1.5 %.
- **The 2D weapons** (2026-10-18). §38: "Draw weapon: 2D" in
  Graphics: the cook draws every frame of the eight view models (Quake's
  alias drawing, halved, 255 colours a weapon) into GAME.DAT (`QGM5`) as
  ready display-list packets; the engine hangs a frame's packets in the
  ordering table when it changes (one VRAM slot) and draws one quad with
  the 3D gun's bob, kick, lean and light: the game bench -4.4 % (the
  super shotgun's frame -11.6 %). The gun is no longer drawn under the
  Ring of Shadows (R_DrawViewModel).
- **The 2D weapons, the first test** (§38): the walk's bob counted
  once (the view rises with the gun: only its forward part shows), the
  quad without the frame's transparent last column and row (no leftover
  texels at its edges), the frames rendered on 216 rows for the
  full-screen HUD, the axe's swings left 3D (their size), the CLUT in two
  rows in turn; the scripted release flow sets the new Graphics rows.

- **Four quality settings instead of one fidelity, and what each option
  costs** (2026-10-18: textures, models, map geometry and effects apart, a hint on every option). §39: textures (the resolution),
  map geometry (low: no T-junction repair, -2.3 % polygons; ultra: the
  faces cut every 128 texels for the vertex light, +37 %), models (the
  map's items and the projectiles simplified by edge collapses, -9 % /
  -5 % of their triangles; the guns stay Quake's) and effects (the
  particles, a quarter to twice the port's). The world file no longer
  depends on the textures: one per geometry level (`<MAP>_G<n>.WLD`,
  QWVD), its UVs the finest texture level's, mapped at load onto the
  texture set's tiles (`<MAP>_T<n>.TEX`, QTX6 / QTX7: the texture table,
  the UV maps, the animations); the map's models per model level
  (`<MAP>_M<n>.MDL`, QMM1), the game data too (`GAME_<n>.DAT`, QGM6).
  At high every map but three draws the same polygons as before (the
  textures over 128 texels: a texel of rounding on a few vertices), and
  the disc carries every combination in less room (the maps' cooked data
  103 MB for every level, 125 before; the release disc 747 MB, 774
  before). The Graphics page and the debug map menu show, under the rows,
  what the highlighted value gains and costs, in the port's measures
  (PCSX-Redux frame cycles against high: geometry low -0.7 %, ultra +2.6 %
  with the heaviest view +24 %; models low -1.0 %; effects low -0.4 %,
  ultra +0.7 %, a rocket's blast +1.9 %; the textures' gain the console's
  GPU and the loads); the debug menu's rows no longer run into its help
  line. Every map loads at every geometry level, at ultra textures and
  geometry too (release-ultra's), and plays on DuckStation.
- **Streaming, the monsters, the SPU pool** (2026-10-18). §40: a
  disc file a map (`STREAM.DAT`, `QST1`) holds what the map may need and
  not always: the nine monsters' models, heads, projectiles and sounds,
  the gibs, the backpack, the items' models, every positional sound. The
  engine keeps RAM, VRAM and SPU pools filled by distance to the player
  (every 8 frames), the keys, the rune and the weapons always first; one
  CD read at a time, the CD-DA paused around it and resumed where it
  was. The nine monsters of the shareware in native C from `progs.dat`
  1.06 (state tables generated from its bytecode, the AI and sv_move.c
  transcribed): their attacks, missiles, deaths, gibs, teleporters and
  pushers for them. The map's sound bank keeps only the player's and the
  weapons' sounds (nothing halved any more). Quake's area nodes for the
  traces and the touch passes, visible()'s line of sight, CheckBottom's
  box node, the residual-solid walk from a node (the asm only): E1M3's
  47 monsters awake 830 k cycles a tick to 287 k, the player's moves on
  E1M3 -50 %, start's -7 %, every trace's result kept. Merged with the
  quality settings: the streamed items at Quake's detail (the models level
  reduces the projectiles), the game data `QGM7`, the monsters' particles
  at the effects level.
- **The brush models cut by the world's BSP** (fourth play test). §41:
  as Quake's R_DrawSolidClippedSubmodelPolygons, every brush model's
  polygons are cut by the world's planes from its top node: the pieces in
  solid leaves dropped (a door in its wall pocket, a plat's sides under
  the floor, a column under the slime: the black blocks), the others
  drawn in their own leaves, in the walk's painter's order. A model is
  looked at when the PVS sees one of its pieces' leaves (START's slab
  over the pit touches more leaves than a box keeps: it went with them).
  The models are cut at the load and again when they move; the pressed
  E1M1 button, lost on the way, shown again.
- **The boot that found no map, and the stack's room** (2026-10-19: the release broken after the merges). §42:
  - **Every map found again.** The release disc with every map booted
    with no map: psxcd's lookups malloc a whole directory from the SDK's
    heap, 21 KB once the monsters were in, against `\MAPS`'s 24 KB. The
    engine now reads the disc's directories itself, into the level
    arena's top, and psxcd's ISO code is no longer linked.
  - **The stack back under the BSS.** The merged build's BSS ended
    1.4 KB past the 2 MB, with the stack inside it. The level arena goes
    from 1152 to 1140 KB, giving the stack 16 KB in every preset (the
    streaming pool loses 12 KB). `scripts/ramcheck.sh` fails the build
    after each link when the stack gets less than 16 KB.
  - **A level change could stop the game.** The static sounds' wants
    were read before their table was built: a division by zero on E2M7.
    `snd_map_start` now runs before the stream's preload.
  - **Skin uploads.** 10 streamed skin images (gib1, dog, h_demon...)
    sent their upload one word short, which breaks the GPU's command
    stream on the machine. The cook now gives every image an even count
    of halfwords.
- **The world and the menu drawn again; the monsters lighter** (after §42: menu pieces and many textures no longer drawn). §43:
  - **The cause.** START's nine crucified zombies, at zombie.mdl's 347
    triangles each, filled the GPU's 96 KB packet arena. The far rooms
    went undrawn, and so did the menu's pieces over the paused game.
  - **The world first.** An alias model is now left out when less than
    24 KB of packet room is left; the menu keeps 12 KB of its own.
  - **Lighter monsters.** The streamed assets come per model level
    (`STREAM_<n>.DAT`), each monster's body reduced at every level (half
    its triangles at high, a third at medium, a quarter at low), the
    vertices it no longer uses dropped. START's views 4, 6 and 8 are back
    under one vblank (602 k → 511 k cycles), and its preload uses 85 KB
    of the stream's RAM instead of 155.
  - **The stack** is measured at every load (`stack:` in the log), and
    the link's threshold is configurable (`Q1PSX_STACK_MIN`).
- **The brush models' room sized on their cuts** (2026-10-19: the RAM back to the monsters' streaming). §44:
  - **Measured at the load.** After the spawn, each brush model is cut
    where it stands, and a door, button, plat, train or secret door also
    at the places it goes through and halfway along each way. A model
    that never moves gets what its cut takes, a mover twice its most.
    The old room asked three times every model's bytes and corners: four
    to five times what the cuts take.
  - **17-174 KB back to the stream on every map** (91 KB on average): the
    monsters are drawn again on E1M3 (6 → 157 KB of pool), E2M4, E2M6,
    E2M7 and E4M5. The measure costs 0.1-1.0 s of the load.
  - **Checked along the travels:** `BMFCHK=1` cuts every mover at 33
    places a way in its room: none given up on the 32 maps.
  - **A train's whole path:** its corners up to the first one it comes
    back to, wherever its loop closes.
  - **No sound read without its buffer:** on E3M4 and E4M7, too full for
    the stream's staging buffer, every streamed sound was read to address
    0 and given up for good. Those two maps still have no room for the
    monsters (their worlds take 742-783 KB).
- **The 2D gun's packets only while it is drawn** (2026-10-19: that room back to the monsters' streaming). §45:
  - **106 KB more for the monsters on every map** with the 3D gun (the
    default) or none: the 2D gun's packets no longer sit in the game arena
    (224 → 116 KB); the level arena grows to 1242 KB, the world keeping
    its old 1136 KB. E3M4's pool 2 → 92 KB, E4M7's 0 → 97 KB: their
    monsters are drawn.
  - **The 2D gun as before:** chosen, it borrows the end of the stream's
    pool (first at a map's start, so it fits wherever it did); chosen
    during a game, it shows a few frames after the menu closes; left, its
    room goes back to the stream. Its frames are drawn the same (the
    game bench's hashes).
- **The brush models' room measured in the cook** (§46):
  - **No measure at the load.** The cook cuts each brush model as the
    engine does, wherever its entity can stand: a door, button, plat,
    train or secret door at every 1/8 unit of its travel. The most the
    cuts take goes into the map's `qw_bmodel`, which keeps its 40 bytes
    (the clip roots now 16-bit). Every load is 0.08-0.99 s shorter.
  - **An exact room**, with no factor or constant. §44's ×2 held by 1 %
    on E2M5's train *21. The stream gets 10-98 KB more on every map
    (48 KB on average).
  - **The starved maps stream their monsters**: E3M4 goes from 2 to 84 KB
    of pool, E4M7 from 0 to 54 KB, E3M6 from 46 to 94 KB; with §45's
    106 KB, E3M4 has 190 KB, E4M7 160 KB and E3M6 200 KB.
  - **Checked:**
    - `BMFCHK=1`: no cut given up on the 32 maps;
    - at each load, a still model's room equals its cut to the byte;
    - the spawn views are identical to §44's, except where a streamed
      model is now resident.
- **The 2D gun's frames, their opaque spans only** (2026-10-19: less RAM for the 2D guns). §47:
  - **28 KB less RAM while the 2D gun is drawn** (106 → 78 KB lent by the
    stream; START's pool 354 → 382 KB): each frame fills its rows of the
    VRAM slot with transparency, then copies only its opaque spans; two
    identical frames share their packets. Not a pixel changes (the
    pictures compared byte for byte).
  - **The halves rebuilt by symmetry, left out**: the guns' outlines are
    symmetric, their light and skins are not (17-52 % of the texels equal
    their mirror's): the gun would look lit from both sides.

- **The music: XA streams carrying their maps' assets** (2026-10-19: XA streaming in place of the CD-DA, to end the music's cuts and the stutter during play, the data interleaved). §48:
  - **What cut it**: every streamed read paused the CD-DA (a blocking
    `CdlPause`: about four frames frozen) for 1.4-1.9 s of silence (the
    seeks to the data and back).
  - **Each CD track an XA stream**, read at single speed: one sector of
    four is the music (37.8 kHz stereo, played by the drive), the three
    others carry, over and over, the streamed assets of the maps that play
    the track (a carousel: a turn of 7.5-14.6 s). The engine takes the
    assets it wants as they pass: no seek, no cut, no freeze. The track
    loops from its stream's start. The disc keeps its size (735 MB, one
    data track).
  - **One asset for the three model levels**: a model's vertices numbered
    by the lowest level keeping them, each level assembled from the shared
    asset (the frames the same: the scenarios hash the same pictures).
  - **The SPU music, a build alternative** (`release-spu`): 22 kHz stereo
    from a 2 s ring at the top of SPU RAM, the drive refilling it between
    the assets' reads, wherever they lie (a 213 MB disc).
  - **Measured**: E1M3's monsters' scenario took 46 assets from its
    carousel (7 s at the median from their arming), read none; the
    carousel costs 77 cycles a quiet frame, 13-16 k in E1M3's crowd.
- **The release's title** (Quake's theme under the menu, START no longer loaded before New Game). §48.7:
  - **Quake's theme under the menu**: track 2, the one Quake's first demo
    forces (demo1.dem's first line), looping (no demos here).
  - **No map before New Game**: the menu over the console's background
    (`gfx/conback.lmp`, what Quake shows with no world and no demo); START
    loaded once, at New Game. The menu's clicks from their own small bank.
- **The release's first build after a pull** (built with SPU music, no background, no theme). §48.10: the disc's file list was made at
  CMake's configure, before the build's cook made the music's streams and
  the menu's files: that disc had none of them. The list is now made when
  the image is built, after the cook; the cook makes the build's music
  format only (XA by default).
- **The movers' re-cut in play, measured and cut down** (§49):
  - **Measured**: `MOVERS=1 scripts/bench.sh` moves E1M6's train *39,
    E1M3's train *3 and E3M4's door *71 in view. Each frame one has moved
    and is drawn, it is cut again: 916 k, 372 k and 658 k cycles a cut,
    the frame then 3.97, 2.29 and 3.00 vblanks.
  - **Each record's sphere down the BSP first**: its polygons start where
    the sphere stops (exact).
  - **No cut out of view**: a moved model whose pieces the record loop
    would all cull is not cut again until it is in view (the same
    frames). On the way, a fix: a model that moved while not drawn was
    judged by its old pieces' leaves, and a train back in the PVS
    elsewhere stayed undrawn.
  - **The whole cut in hand-written asm** (`engine/bmfrag.s`): 482 k,
    218 k and 361 k a cut (-47 %, -41 %, -45 %). The C stays as its
    check (`BMF_C=1|2`): no byte differs over 44295 cuts on the 32 maps.
  - **`tools/hazcheck` checks the branches' targets too**: a load in a
    delay slot read by the target's first instruction, which made the
    asm clear RAM under the interpreter while the dynarec hid it.

- **Monsters seen through a closed wall** (2026-10-19: on E1M1, the soldier and the ammo behind a wall not yet open seen through it). §50.1:
  - **The cause**: within a BSP leaf, every alias model was drawn over
    every piece of a brush model. E1M1's secret wall by the nailgun backs
    16 units into the soldier's room before sliding, into his leaf.
  - **The fix**: a monster, an item, a projectile or a particle takes its
    place among a leaf's door pieces by the door's own BSP, as Quake's
    z-buffer would decide. The alias models of a leaf are drawn by their
    depth (before: the monsters over the items over the projectiles).
  - **Cost**: +0.16 % of the frame on the 128 spawn views (+2.4 % at
    worst: three monsters among five doors' pieces); 3 KB of the level
    arena for the code.
- **Monsters paused while their model is not loaded** (else unseen monsters hit the player). §50.2: no
  think, no move, no attack until the model is in.
- **The dead fade and go** (the dead after the living, unloaded after a few seconds with a fade). §50.3:
  - a body (or a head) is removed 6 s after the death, darkened to black
    in its last second;
  - the dead's models are wanted under every living one's, never read
    for.
- **What the stream keeps near the player, measured** (too wide an area seemed loaded). §50.4:
  - today a kind is wanted at its nearest member's distance through the
    walls, within 2500 units (4000 for the music's carousel): most of a
    map;
  - on 8 maps with tight pools, a monster kind visible from the player
    is not loaded at 7-15 spots of 32;
  - simulated rankings: a short radius does worse (96 spots of 1020
    against 90 today); the PVS first does better (59), with the heads
    and gibs under the models better still (41).
- **The PVS first, the heads under the models** (the one chosen of the
  four). §50.5:
  - what the player can see from where he stands is loaded first, by
    distance; the rest after it, in the same zone, and evicted first;
  - a monster's head (only drawn when it is gibbed) and the gibs are
    loaded after the models;
  - measured: a visible kind missing at 41 spots of 1020 (90 before),
    only on the maps with the tightest pools.
- **The order among a leaf's pieces in hand-written asm** (§51): a
  monster's, an item's or a particle's place among a leaf's door pieces
  (§50.1) is now `engine/rl_order.s`, the GTE taking the general planes:
  - 45 % cheaper on the 128 spawn views (E3M3.v1: 11.4 k -> 5.6 k
    cycles, its frame -1.2 %), 41 % on E1M1's nailgun scenario;
  - the C kept as its check (`ORDER_C=1|2`): 32051 asks, 0 mismatches,
    under the interpreter too; the views' display lists identical;
  - a bug of §50.1's C fixed on the way (a list headed by a door drawn
    whole read a stale chain);
  - 320 more bytes of stack room (16968 B in the release).
- **The clean-up** (2026-10-21: what stays). §52:
  - **The build's choices:** the textures high or medium (4-bit only,
    `Q1PSX_TEXTURES`), the gun 3D or 2D (`Q1PSX_WEAPON`), the draw
    distance 1536 or 1024 (`Q1PSX_DRAWDIST`); the geometry and the models
    low on every disc; presets `release` (high, 3D, 1536) and
    `release-medium`.
  - **Graphics:** the effects (medium by default, or high), the dithering,
    the HUD, the dynamic lights, the frame rate cap (60, 45, 30), the tick
    rate (60, 30); the warp control dynamic from 4 px and the far walls
    flat, always.
  - **The music:** the SPU's alone (the XA streams and their carousels
    gone), the menu's theme at 32 kHz.
  - **The guns rebuilt by hand** from Quake's own models, the shotgun
    first: 61 triangles drawn instead of 78, the same look.
  - **12 KB more for the streaming:** the code removed gave the stack
    14 KB, 12 of them back to the level arena (1249 KB).
- **The guns' textures, case by case** (2026-10-21: faithful textures, the 2D gun as the reference). §53:
  - **the order:** a gun's triangles drawn farthest first, as the eye
    sees them in every frame (the nailgun's barrels no longer over its
    blocks, the super shotgun's black notch gone, the rocket launcher's
    muzzle hole back);
  - **the back faces left out**, as Quake does, for every model (the
    super nailgun's turning barrels; a monster's far side no longer shows
    through): E1M1's busiest view 476 primitives fewer, -1.2 %;
  - **Quake's own vertices** for the guns (they were on a quarter-unit
    grid, up to 9 px off near the eye: the shotgun's barrel crooked);
  - **the long triangles cut** where the PS1's flat texture mapping bent
    the skin by over 8 px (the shotgun's barrel, the grenade launcher's
    band);
  - **the skins as atlases** of the texels shown: 91 K texels for the
    eight guns instead of 100 K;
  - the guns' geometry 47.8 KB instead of 62.3: 15 KB of RAM freed (to
    the stack for now);
  - a release bug found on the way: after the menu's music, New Game
    could load START without its sounds (a CD read cut short); fixed.
  - **the rocket launcher's muzzle hole** (not right yet): its
    half texels now count the rim's texels the triangles' edges cross,
    and where Quake's depth buffer drew one part through another (the
    hole's arch), the texels the PS1 shows there are repainted with
    Quake's colours: the arch is back, no triangle more.
  - Left open: Quake's skin resolution near the eye needs about
    130 K more texels of VRAM (the rocket launcher's highlight is still a
    band).
- **The other seven guns by hand** (as the shotgun, a capture each). §54:
  - Quake's other guns turned out lean already (prisms of two or three
    sections); where a section was redundant (the super shotgun's stock,
    the grenade launcher's tube, the lightning gun's box) it is gone, the
    nailguns, the rocket launcher and the axe keep Quake's triangles;
  - the anti-warp cut now cuts a flat quad by rings instead of its two
    triangles in three each (dropping sections had made it put more back);
  - the triangles a gun always hides in itself are gone (the super
    shotgun's half);
  - 1223 -> 810 triangles for the eight guns, the same look on the real
    GPU; 11.6 KB less RAM.
- **The C engine, to compare with the asm** (to compare the performance). §55:
  - CMake `Q1PSX_ENGINE_C` builds each asm part's C version in its place:
    the physics, the slow path, the dynamic lights, the brush models' cut,
    the order among a leaf's pieces, the entity matrices and the model
    frames. The world renderer stays asm: it never had a C version;
  - the asm builds are unchanged byte for byte. Only the C was fixed, to
    follow the asm (the slow path's C had fallen behind the models and the
    gun), and none of it was optimized;
  - the frames are the same (start's views, the 128 map views, the
    weapons). The C engine takes +88 % on start's views and +94 % on the
    maps' (its slow path x2.7); the physics takes x1.6 to x1.8, the
    monsters x1.5;
  - `ENGINE_C=1 scripts/bench.sh` builds it in build-bench-c/, and
    `scripts/dlcheck.sh` compares the asm with the C, view by view.
- **Level chunks: the measures** (a tool to find where to cut the levels without it showing). §56:
  - a tool outside the repo: QuakeSpasm with a scanner.
    It floods every place the player can reach on the 32 maps and renders,
    from 5.4 million eyes, the PSX's cooked records with their ids. That
    gives what each place must have in RAM, under the PSX's own PVS and
    draw-distance rules;
  - an eye needs 20-50 KB of the map's geometry at most. The draw distance
    (1536 or 1024) changes nothing seen;
  - the map's skeleton (nodes, PVS, clip hulls…, 84-352 KB) stays resident.
    Zones of 96 KB (the current one drawn, the two nearest prefetched,
    teleporters counted as one step) free 150 KB on the worst map; seen
    zones of 64 KB would free 250 KB;
  - the zone tables for the cook are ready outside the repo. The
    implementation follows on its own branch.
- **Level streaming by zones** (the levels streamed in chunks, for much more room for the streaming). §57:
  - `qcook zones` splits a world into its skeleton (QWVZ: nodes, leaves,
    PVS, clip hulls, the brush models, the entities) and a chunk per zone
    (QZN1, per texture level: the records a zone's leaves may draw, as the
    loader would leave them), from §56's tables (`Q1PSX_ZONE_TABLES` in
    local.env; never in the repo; without them every map stays whole);
  - the engine keeps the skeleton and three slots: the camera leaf's zone,
    drawn, and its two nearest by walking distance (a teleporter one step),
    read between frames while the player moves. The switch hangs on the
    renderer's camera-leaf call through a hand-written asm hook
    (`engine/zone.s`), r_world.s unchanged; a late chunk leaves the old
    zone drawn, counted;
  - the frames are the parent's, hash for hash: start's 12 views, the 128
    spawn views (under the interpreter too), the game bench; the C engine
    too. And 0.5 % cheaper (the chunks' blocks carry fewer records to
    skip);
  - the RAM: 20 maps zoned, 12 kept whole; 2-102 KB freed on a zoned map,
    the smallest stream pool 171 -> 215 KB. Below §56's estimate: the
    skeleton holds the brush models' geometry, and the frame-identical
    tables plus fixed slots cost more than the analysis' best case. The
    tighter "seen" tables would give 363 KB (the pictures identical on the
    views checked, 22 of 128 display lists not): left open;
  - the CD: a race between psxcd's command writes and the drive's
    interrupt lost Setlocs (another file's sectors read: the parent's
    corrupted sound) and could silence the drive; every read now waits for
    the drive to settle (`engine/cd.c`);
  - `ZONES=1 scripts/bench.sh`: the zones' walk (switches, reads, late
    frames, on PCSX-Redux and DuckStation with the music); DuckStation
    runs again headless from its extracted AppImage.
- **The skeleton's RAM** (§57.9: a zoned map's skeleton, 276-378 KB, is
  now the bulk of the world's RAM). §58:
  - measured on the 32 maps: what the skeleton holds; the PVS of every
    place the player reaches covers 78-100 % of the leaves and the nodes
    above it 83-99 %, so pruning the tree or the clip hulls to where the
    player can be frees next to nothing (the game's traces and point
    queries walk the same trees anywhere monsters are);
  - two repeats shared in the cook, the frames the tip's hash for hash
    (start's 12 views, the 128 spawn views, the game bench; the C engine
    too) and no engine change: the brush models' leaves (210-953 a map,
    read for their contents only) are one shared leaf, the clip hulls'
    identical subtrees one clipnode (69-87 % of the clipnodes left). 9.5-38
    KB freed a map, 22 on average, on all 32 maps; the smallest stream
    pool 215 -> 235 KB, the worst world load 763 -> 743 KB;
  - the next options measured: a compact entity table (4-14
    KB a map), the PVS rows as deltas (1-12 KB, `mark_pvs` by hand), the
    PVS rows per zone (1-31 KB on the zoned maps, a change while a zone is
    late), the edicts the skill never uses (up to 38 KB, the runtime
    headroom to measure), 16-bit node children (3-14 KB, the hottest
    asm).
- **The entities in a compact table** (§58.2's option C). §59:
  - the world file (QWVF; zoned skeletons QWZF) holds each class's
    classname once, then per entity a 32-bit head (the class, a bit per
    field present) and the fields that are not zero: the origin in whole
    world units in 2 bytes (1/8 units in 4 if needed), the model and each
    spawnflags byte in a byte, the fields ordered by how often the id1
    maps give them. `qw_ent_decode` (formats/qworld.h) gives the 56-byte
    `qw_ent` back; `ents_spawn` decodes each entity on its stack, and the
    cook checks every entity's round trip byte for byte;
  - the entities take 21.6 % of their old bytes: 5.0-17.6 KB less a map
    (§58.2's layout: 4.2-14.2), the stream's pool +4..+18 KB, the
    smallest 235 -> 249 KB (E3M6); the decode costs 42-166 k cycles at a
    spawn; the frames the tip's hash for hash (start's 12 views, the 128
    spawn views, the game bench; the C engine too);
  - the entities single player never spawns (2-36 a map) stay: the
    runtime spawns used at most 9 slots in the benches against 94-162
    left free without them at hard, but gibs (10-20 s) and backpacks (2
    minutes) grow with the play, so nothing shows they never need the
    slots. Listed with the measures (0.8-13.6 KB a map);
  - the analysis tool's reader outside the repo (`wld.py`) asserts
    "QWVE": accepting "QWVF" is all it needs (it reads no entity).
- **The executable trimmed** (2026-10-22). §60: the release prints
  nothing (no printf, its strings gone: a console has no TTY), no 64-bit
  division left (`q_div64_32` exact, inline, one `div` in practice; the
  atan table a constant, its 1.8 M-cycle first call gone; a de Bruijn
  ctz), no SDK debug font in the release, the code that runs once a level
  compiled for size (`Q_COLD`, the menus' files at -Os). The release's
  code and data -16.6 KB (`q1psx.exe` 317440 -> 301056 B), the stack's
  room 43.4 -> 60.8 KB; the frames identical (start's views, the game,
  the monsters', the movers' benches).
- **The monsters by hand: the Ogre first** (the monsters rebuilt by hand from Quake's own models, the Ogre first). §62:
  - the cook's automatic decimation had turned the Ogre's skull into a
    spike in profile and its grenade launcher into a flat shard;
  - rebuilt from Quake's own model: 29 collapses picked one at a time
    against every frame (each under 3 units and 4 texels), the head, the
    hands, the launcher and the feet kept whole, the 2 never-seen
    triangles gone: 266 triangles, 140 vertices (Quake's 326 / 169, the
    old 194 / 103);
  - compared Reference / Before / After on 8 sides of 8 frames: the After
    matches Quake where the Before broke;
  - the cost: 23 KB more in the stream's pool. On master as it is now, a
    visible monster lacks its model at 3 spots of every map's against 2 (one
    more on E2M7).
- **The monsters' triangles sorted by depth** (§62's proposal). §63:
  - the PS1 drew a model's triangles in the cook's order with no depth
    test, so a far part showed over a near one (the Ogre's vest over its
    near arm, from either side);
  - each monster drawn now has its own ordering table: 16 nodes in the
    packet arena, a ring (16, not 32, the choice made: the same look on
    the sheets, 88 cycles less a monster). Each triangle goes into the bucket of its
    corners' mean depth (the GTE's AVSZ3, its scale from the model's
    radius). The table is spliced whole into the world's bucket where the
    model always was;
  - hand-written asm in r_world.s: dm_sort and ds_records, deo1's loop for
    triangles only. The corners' xy go through the GTE (lwc2 for the
    NCLIP, swc2 to the packet), which pays for most of the sort;
  - the cook flags the nine kinds' bodies (QSTM_SORT). Heads, items,
    projectiles, the view model and the brush models are drawn as before;
  - checked:
    - 100 of the 128 spawn views identical; the 28 with monsters hold the
      same packets reordered (the dumps' new set hash, the "Q" line);
    - the C engine's frames are the asm's;
    - the Ogre's sheets and E1M8's close views show the near parts over
      the far ones;
  - the cost:
    - -367..+793 cycles a monster drawn (+0.03 % over the 128 views),
      +418 for the Ogre alone at 90 units;
    - 56 bytes of the packet arena a monster;
    - 2.3 KB of code.
- **The monsters by hand: the other eight** (the other monsters the same way, with the Reference / Before / After sheets). §64:
  - the soldier, the dog, the knight, the zombie, the scrag, the fiend,
    the shambler and Chthon rebuilt from Quake's own models;
  - what carries their look kept whole: the heads, hands and feet, the
    soldier's gun and muzzle flash, the knight's sword, the dog's teeth,
    the horns, the spikes;
  - each collapse under 3 units and 4 texels in every frame (Chthon 13
    units: his coarse vertex grid);
  - 64 sheets compared: the After matches Quake where the Before broke
    (spiked heads, the muzzle flash and the sword as needles, the dog's
    legs, the zombie's feet);
  - two flaws of the method fixed:
    - the hidden-triangle check now looks at every frame and zooms on each
      candidate (it had taken the soldier's muzzle flash for never seen);
    - a collapse that leaves nothing to measure is refused;
  - `QCOOK_HAND=<files>` cooks only those tables (an A / B);
  - the cost:
    - the eight assets +152 KB;
    - a visible monster lacks its model at 20 spots of 1020 against 3
      (maps whole), 11 against 2 (zones), mostly on E3M4 and E2M2-E2M7;
    - the spawn views +1.6 % (E1M8.v0 from 2 to 3 vblanks, none crossing
      one);
    - the crowd's frames unchanged.
- **The freeze on E1M3** (2026-10-22: frozen after the gold key, on
  DuckStation). §65: read from a save state: a bus error in the
  stream's wants, an opened secret door's stop read as a monster's kind
  (`mdef` shared a union with the movers' fields). `mdef` now in every
  edict, 0 but a monster's; nightmare's pain timer set on monsters only;
  the bench's edict check refuses a bad `mdef`, and `MONSTERS=6` opens
  E1M3's secret doors first. The frames, the monsters and the movers as
  before.
- **The models' triangles: the corners' xy through the GTE** (§63's
  follow-up). §66:
  - de and deo1 (every alias model but the sorted monsters, the view
    model, the brush models) load a triangle's corners into the GTE with
    `lwc2` for the back-face NCLIP and store them from there with `swc2`;
    they went through the CPU's registers before (`lw`, `mtc2`, `sw`);
  - three instructions fewer a triangle, about 4.8 cycles, drawn or
    culled; the frames identical (START's views, the 128 spawn views, the
    game bench, the monsters', the movers'; the C engine, the
    interpreter);
  - the 128 views -0.12 %, the game bench with the 3D gun -0.16..-0.30 %.
- **The monsters' residency: the report, the slots, the frames**
  (2026-10-22). §67:
  - measured: §64's loss (20 / 11 spots of 1020 against 3 / 2) was the
    residency report's: its simulation charged each model its skin, which
    the engine drops once in VRAM. Charged as the engine holds them, and
    loaded at each spot by the stream's own code, §64's monsters are at
    the counts of before;
  - A: the report charges a model what it keeps (its header, read at the
    bench's boot) and counts the stream's own reads at each spot;
  - B: when the stream's 48 slots (models and sounds) are taken, the
    asset wanted least gives its slot to one wanted more (E2M4's
    shambler, kept out at 2 spots);
  - C: a monster's body keeps only the frames its QC plays, each one once
    (a frame map, formats/qstream.h `QSTM_FMAP`; `QCOOK_NOFMAP=1` the A /
    B): 1.5-9.5 KB a body, 0-21 KB a map, the frames drawn the same;
  - the residency: 2 spots of 1020, whole and zoned (E1M7's Chthon, out
    of reach as before). The views and the game bench identical. The
    monsters' bench draws E1M8's dead shambler, which master's frame
    cache refused; the cache's flaw itself is left for apart.
- **Single player only: what only multiplayer used, out of the cook**
  (2026-10-22).
  §68:
  - the entities single player never spawns leave the cook's entity list
    (the coop and deathmatch starts, the 471 entities on no skill: 2-36 a
    map), and with them what was cooked for them alone: their brush
    models' faces and trees (109 models: 909 faces, 852 render nodes, 9.5
    KB of clipnodes), the textures only those faces used (27 maps), the
    item boxes only their items used (15 maps; START's 10, START has no
    item in single player), START's items sound group;
  - the four sounds only multiplayer plays leave every map's bank and the
    engine's table: an item's respawn, a player telefragged, a player's
    axe pain, a player leaving (21-22 KB of SPU RAM a map);
  - the main menu picture keeps the two rows the menu draws, not its
    Multiplayer row (nor Help and Quit); `tdeath_touch`'s test of a
    player arriving on the player goes;
  - the gain: the stream's pool +2..+30 KB a map (8.6 on average, E3M6's
    smallest 247 -> 250), the edicts 0.8-13.6 KB, SPU RAM 21-37 KB, the
    maps' files 22-59 KB a map (863 KB in all), GAME_0.DAT 10 KB, the
    stack +112 B;
  - the frames: the same packets but for the textures' places (the atlas
    repacked) and two VRAM copies fewer on E2M6 and E4M3; the pictures
    the same but 51 pixels of three spawn views, texels read one past
    their tile's edge.
- **The frame cache's refusals** (§67's find). §69:
  - the streamed models' frame cache (`engine/mdl.c`) took a new
    expansion only at its ring's head, and refused it when the bytes there
    held one the frame draws, room elsewhere or not. Master's minute of
    fight on E1M3 left out 129 models; E1M8's monsters' dumps left out two
    Scrags, blocked by an Ogre after the ring's wrap (before §68's cook,
    with every frame cooked: a Scrag and an Ogre; §67 read that Scrag as
    the dead shambler);
  - the ring now takes the place past the frame's own expansions in the
    way, else from its start: a refusal means no gap big enough between
    them, or all 32 entries drawn in the frame. One scan checks a place
    and lists what it overwrites; the miss is a function apart, so a hit
    saves no register;
  - the bench prints the refusals: the `mon dump` and `mon fight` lines,
    and a `bench frame cache` line at the game bench's end;
  - 0 refusals in those runs. The views and the game bench are identical
    to master's, the monsters' bench too but E1M8's two dumps, which now
    draw the two Scrags;
  - mdl_frame in the fight: a hit 378 -> 321 cycles, a miss's own work
    2808 -> 1710 (the expansion apart), 882 -> 727 cycles a frame. The
    code +100 B (the release's stack room 58664 -> 58552 B).
- **The texels past a tile's edge: a gutter, the packer on the texel
  grid** (§68's follow-up; the gutter chosen repeats the
  tile's start, with the packer revised). §71:
  - the cause: the cook cuts a face at every repetition of a texture axis
    that is not a power of two, so a piece's UV on its far edge is its
    tile's size, and the GPU, which rounds its texture coordinates to the
    nearest texel, read the atlas neighbour's texel along that edge: 2218
    polygons of 263119 (0.85 %) on 29 maps (E1M8's `lgmetal` walls, the
    buttons, the 192-high doors and windows), 1725 wrong pixels in 23 of
    the 128 spawn views as the GPU shows them;
  - every tile with such an axis has a one-texel gutter past its end
    there that repeats its first column / row, as the texture window does
    on a power-of-two axis and Quake's surfaces do; an animated texture's
    gutter moves with its frame;
  - the packer works on the texel grid (an animated texture's tiles on
    halfwords) and keeps the best of eight orders: the gutters cost high
    +510 page rows over 32 maps (2.2 K texels of the stream's VRAM a map,
    8 K at most on E1M5), medium -27; the disc +65 KB (high), -3 KB
    (medium);
  - the frames: the same packets (the textures' places aside) and the
    same cycles; the pictures change only where a texel past a tile's
    end showed. pktview's `PKTVIEW_UVROUND=1` rounds the texture
    coordinates as the GPU does.
- **The world's cycles: the stream's lookup, the monsters' fewer calls,
  the ticks split** (2026-10-22: §70's levers, the player at 60 ticks and the rest at 30). §72:
  - the stream: what the more wanted assets hold, a binary search in the
    resident assets sorted by score once a search (no walk of every slot
    for every stuck asset), and no search again on frames where nothing
    changed: the crowd's stream cost -71 %;
  - a monster's sight line answered from the PVS before its trace (a
    leaf its eye's row leaves out holds nothing the trace reaches), and
    its touches walked only on the triggers whose touch can do something
    to it (each area node's second list: not the items, the
    `trigger_multiple`s, the changelevels... which return at once for
    all but the player): the crowd's ticks 261k -> 219k cycles, every
    bench identical;
  - the player, the movers and the projectiles each 60th; the other
    edicts at the world's rate, the Graphics menu's "World tick rate"
    (30 by default, 60 as before): at 30 half of them each tick, over
    two 60ths, and a falling or thrown one drawn a 60th on between its
    moves. At 60 every bench is the previous commit's; at 30 the movers'
    ways the same, the crowd -6 % a tick and its worst tick -45 %.
- **Two crashes found by long runs** (2026-10-23, `docs/porting-notes.md`
  §73):
  - the §72 asm kept registers in the o32 argument area a callee may
    write; a think run on the tick loop's frame by a tail call did, and
    the demo build crashed at E1M1's intermission. The 16 bytes are now
    kept free;
  - the frame count's wrap cleared the faces' stamps but not the nodes'
    light stamps, so 18 minutes into a map a node lit 65534 frames
    earlier ran the light test on no light. A loop then zeroed RAM. The
    wrap now clears those stamps too.
  - one monster in 30 never started: Quake's spawn delay, random () x 0.5
    s, came out as 0 ticks, and a think at 0 never runs. The monster
    stood frozen, solid and unhurt. A 0 is now the first tick.
- **The frame cache's lookup in asm** (§69's follow-up). §74:
  - the cache's entries by field, the keys together: asset << 16 | the
    frame stored, a sentinel past the last;
  - `engine/mdl_frame.s`: four keys a turn, each compare in the shadow
    of the next load (4.5 cycles an entry, 15 before); a miss, a tail
    jump to `mdl_expand_new` (§69's placement, unchanged). The C engine
    builds the same key and sentinel in C;
  - a hit in the E1M3 fight 324 -> 134 cycles (the C's sentinel loop:
    210), mdl_frame 665 -> 339 cycles a frame; the frames identical to
    master's (the views, the game bench, the monsters'), the asm checked
    by the C call by call (8878 calls, 0 mismatches);
  - the fight -0.048 %, the game bench -85..-1435 cycles a frame; the
    table +16 B.
- **Gravity with the feet in the water** (2026-10-23, `docs/porting-notes.md`
  §75): gravity stopped as soon as the feet were in the water; Quake stops
  it only with the waist under (`SV_CheckWater`'s return). A player who
  came down slowly into shallow water hung above the floor, crept at the
  air's speed cap and could not jump. Fixed in the asm and the C (0
  mismatches between them). In E1M3's basement the player now lands,
  jumps and walks at full speed.
- **Gameplay audited against id's code** (2026-10-23,
  `docs/porting-notes.md` §76):
  - movement (asm and C): edge friction (sv_edgefriction 2), Quake's
    SV_WalkMove rules (a wall is vertical, the step kept when it lands on
    a floor), wall friction;
  - weapons: autoaim (sv_aim 0.93); the lightning gun never auto-chosen
    under water; an empty weapon changed as its attack ends; the axe's
    4th frame; the quad's sound during held fire; firing wakes nearby
    monsters behind;
  - death tosses the body (a gib's head flies); the view follows a step
    up at 80 units/s; no pain sound while invisible; respawn on jump too;
    the double-barrelled shotgun's name;
  - costs: about 1.6 k cycles a walking tick (the edge trace), 2.8 KB of
    code.
- **The RAM, the cycles kept** (2026-10-23, `docs/porting-notes.md` §77):
  - the release's stack room 57016 → 85496 B against master (+28.5 KB):
    the particle pool 512 → 256 (the ultra level is gone), libc's
    vsnprintf replaced by a 1 KB `q_snprintf`, the brush models' cut and
    a pusher's lists inside the slow path's block (never in use at once);
  - the level arena: the edict pool cut to the map's slots and 96 free
    after the spawn, the rest to the stream's pool (+11.4 KB a map on
    average, +28 KB at most);
  - the 128 views' display lists identical, frame cycles +0.007 % (the
    instruction cache's placement);
  - the edict 392 → 384 bytes (the secret door's start in its unused
    `pos1`, the asm's multiply two shifts and an add), the projectile
    96 → 80, its move's product in 32 bits: the same game, its frames
    −0.025..−0.12 %; the release's stack room 86200 B;
  - 32-bit products wherever the values are bounded (the monsters'
    physics and attacks, the movers, `vnormalize`), more cold code:
    −4.4 KB of code, the same game (the monsters' scenarios line for
    line); the release's stack room 90616 B (master 57016: +33.6 KB).
- **The 3D view's internal resolution** (2026-10-23, `docs/porting-notes.md`
  §78):
  - a Graphics row, 3D resolution: 320×240, 256×224, 240×180, 200×150;
    the framebuffer, the HUD and the menus stay 320×240;
  - the 3D view drawn into the viewport's bottom-right corner, then
    stretched over the viewport in place by two raw quads textured from
    the framebuffer (the mask bit set while it draws: no transparent
    texel);
  - the field of view kept: the GTE's H, the slow path's product (x 15,
    x 1 patched, x 25 a second entry), the off-screen bounds; 256×224's
    pixels not square: the view's y row x 7/6 and the frustum's vertical
    planes patched to 7z ± 8y;
  - START's 12 views: frame cycles −4.1 / −11.7 / −18.3 % (fewer splits:
    the warp in the target's pixels); 256×224's GPU time +25 % on
    DuckStation (the stretch); the 128 views identical at 320×240;
    the release's stack room 87560 B (−3056 B);
  - bench: `RES=`, `GPUTIME=1`; pktview reads the target.
- **Brighter light** (2026-10-24, `docs/porting-notes.md` §15): the cook's
  light gain 1.5 → 1.66 (`LIGHT_GAIN`, `qcook --light-gain`): the world's
  vertex light, the liquids, the leaves' light for the models; the
  dynamic lights follow (`DL_GAIN`: the models' × 53/32, the world's
  vertices' × 53/128 in the asm and the C). Every map cooked again.
- **The split test by the edge's length** (2026-10-24,
  `docs/porting-notes.md` §79.1): the slow path's warp test measures an
  edge as max + 3/8 min of |dx|, |dy| (was |dx| + |dy|, up to 41 % over on
  a diagonal): the asm re-tests the edges |dx| + |dy| marks (`REFINE`),
  the C the same; START's 12 views −15 % triangles, frames −1.85 %; the
  128 views −1.04 %, 24 → 22 over a vblank; the C engine's frames the
  asm's.
- **The warp's control spends no warp for nothing** (2026-10-24,
  `docs/porting-notes.md` §79.2): a view that misses its vblanks even at
  32 px for 30 frames is drawn at the floor (4 px) with a vblank more, as
  it takes it anyway; back to the cap's when lighter, or tried again after
  128 frames and more; START's heavy view T 32 → 4 at the same 30 fps, the
  128 views line for line; bench: `WARPDYN=1` (the game's control, each
  view's T printed).
- **The cook's polygons and records, measured** (2026-10-24,
  `docs/porting-notes.md` §79.3): merging a node's coplanar faces would
  save 2.5-5.7 % of the polygons (a coarser light), bigger records cost
  more than they save (+0.6 to +2.7 % on the 128 views): neither built.
- **The interrupts without the kernel's dispatch** (2026-10-24,
  `docs/porting-notes.md` §81.1): the RAM exception vector runs the
  callbacks itself (`engine/irq.s`, psxetc's API in `engine/irq.c`), the
  syscalls still the kernel's; ~2.7 k cycles an interrupt gone on
  PCSX-Redux: frames −0.6 to −1.4 %, the game's worst −53 k; the
  interrupt stack 4 KB → 1 KB.
- **The pad's poll by interrupts** (2026-10-24, `docs/porting-notes.md`
  §81.2): each byte's /ACK interrupt sends the next one, the CPU no
  longer waits for the port; interrupts 10.7 k → 3.8 k cycles a vblank;
  the game −2.8 / −3.7 %; the bench's `IRQTIME=1` times the handler.
- **The asm, instruction by instruction** (2026-10-24,
  `docs/porting-notes.md` §81.3, §81.4): the delay slots and the load,
  HI/LO and GTE spacers of every asm file do work where an instruction of
  the way taken fits (the slow path's REFINE, CAREFUL's ranges, the hull
  traces, the brush models' cut, the entities, pmove); the pad's middle
  bytes by a short way through the interrupt handler (3.8 k → 3.1 k
  cycles a vblank); START's views −1.5 %, the 128 views −1.2 %, the
  movers' cuts −1.8 to −2.7 %, a pmove tick without traces 2455 → 2411
  cycles; every display list identical under the dynarec and the
  interpreter. Two gains that cost code (NZ two vertices a turn, the flat
  transforms' addresses under the RTPT: 576 bytes) were taken back out.
  A misaligned load in a delay slot (the point hull's leaf test) that the
  dynarec ran and the interpreter stopped on was fixed before release;
  `headless.sh` now ends a run on the interpreter's exception messages.
- **The instruction cache's layout** (2026-10-24, `docs/porting-notes.md`
  §81.5): the renderer's asm cut into chains (ICSEC) and linked by the
  project's own script (`engine/q1psx.ld`), the hot ones first in the
  order `engine/icache.ld` gives; `tools/icache` simulates the 4 KB cache
  over every fetch of traced frames (`scripts/icache.sh`: PCSX-Redux's
  interpreter, a Lua breakpoint) and searches the order. The words the
  cache fills a frame: START's views −50.6 %, the 128 views −45.2 %, the
  game −49.1 %; no instruction changed (PCSX-Redux, which has no instruction
  cache, counts within 0.003 %); `scripts/iccheck.sh` fails a chain that falls
  through.
- **The rest of the optimization list** (2026-10-24,
  `docs/porting-notes.md` §81.6): each item measured or looked up: the
  executed nops (0.8-0.9 %), the scratchpad (full), the GTE's waits, the
  RAM accesses, the DMA, the ordering table, the critical sections,
  self-modifying code (in use; the boot code's RAM not taken back).
- **The level arena takes the stack's gap** (2026-10-24,
  `docs/porting-notes.md` §81.7): the stack keeps 12 KB at the top of RAM
  (its deepest measured: 2.0-2.5 KB), a guard of its lowest 1 KB checked
  each frame stops the game on a red screen before it could overwrite the
  arena, and the level arena takes every byte between the BSS and it:
  1243 → 1318 KB in the release, every map's stream pool +75 KB (E4M7
  237 → 312 KB); RAM freed later goes to the arena by itself.
