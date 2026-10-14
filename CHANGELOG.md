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
