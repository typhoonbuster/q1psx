# q1psx — engine architecture

Status: **validated on 2026-10-14**. It follows roadmap phases
2-6.

Decisions taken (2026-10-14):
- **Performance is constraint #1**, from the first line.
- **The whole renderer is hand-written MIPS assembly**: BSP walk, PVS, frustum,
  transform, clipping, packet emission. Everything else is lean C, with asm
  added only where a measurement says so.
- **Display: 320 × 240** (revisit only if the bench shows the GPU limits us).

Project rules: built from `id-Software/Quake` + PS1 hardware docs +
PSn00bSDK docs only (no existing PS1 port is read); 20.12 fixed point in C game
code; cook tool in C; no proprietary data committed; English in the repo.

---

## 1. What phases 2-6 taught

Phases 2-6 reached ~20 fps on `start.bsp` with no lighting and no models. The target
is a constant 60 fps. What they taught (the knowledge kept, the code dropped):

- **The per-face C path was the bottleneck.** Every face went through:
  - a C backface dot product;
  - per-vertex `RTPS` calls through a cache indexed by global vertex number
    (scattered RAM reads, and the PS1 has no data cache);
  - a `DR_TWIN` GPU command;
  - runtime subdivision that re-projected 3D midpoints;
  - a Sutherland-Hodgman clip.

  Rewriting the same algorithm in asm would give maybe ×1.5-2. The rest has to
  come from the data layout.
- **The texture window (`DR_TWIN`) is a trap.** It forced:
  - one GPU command per face;
  - power-of-two tiles;
  - a 64 px texture cap, because the fixed grid atlas had no room for more.
- **Affine warp and near-plane holes come from big polygons.** The fix belongs
  in the cooked data (small polygons) plus a runtime path for the few
  polygons near the camera.
- **Toolchain traps:**
  - The build was `-O0` for months.
  - PSn00bSDK's `gte_SetRotMatrix`/`gte_SetTransMatrix` under-declare their
    clobbers and miscompile at `-O2`.
  - `LoadImage` lengths must be a multiple of 32 bytes.

  All are recorded in `docs/porting-notes.md`.
- **The optimising was blind**: no reliable per-stage cost. This design builds
  the measuring tool first.

## 2. Principles

1. **Measure first.** Every roadmap step ships with a number from the
   benchmark harness (§9). No optimisation without a before/after.
2. **Cook everything that can be cooked.** The runtime does no format
   conversion and makes no per-polygon decision that could be precomputed
   offline.
3. **The renderer is asm end to end.** It is designed together with its data
   format, so the inner loop reads contiguous, pre-packed words.
4. **Fixed memory map.** Every region has a fixed size and owner. Nothing is
   allocated at runtime beyond per-frame bump allocators.
5. **Faithful where behaviour matters, free where only implementation does.**
   - Physics, PVS semantics and QuakeC follow Quake exactly.
   - Rendering and data layout are free.

## 3. Budgets (NTSC)

CPU: 33.8688 MHz / 59.94 Hz ≈ **565 000 cycles per frame**.

| Consumer | Budget (provisional) |
|---|---|
| Renderer (walk + transform + emit) | ≤ 300 000 |
| Simulation (physics + QuakeC, Phase 7+) | ≤ 200 000 |
| Pad, audio, DMA kick, misc | ≤ 30 000 |
| Margin | ~35 000 |

These numbers are revised as soon as the harness measures real costs.

Hardware facts the design relies on (psx-spx / PSn00bSDK docs):
- **Memory:**
  - No data cache. Every main-RAM load costs several cycles; the 1 KB
    scratchpad (`0x1F800000`) costs 1.
  - A 4-word write buffer makes stores cheap until it fills.
  - 4 KB direct-mapped I-cache: the inner loop must fit in it.
- **GTE:**
  - Runs concurrently with the CPU; reading a result before it is ready
    stalls.
  - `RTPT` projects 3 vertices in ~23 cycles, against 3 × 15 with `RTPS`.
  - `swc2` stores `SXY`/`SZ` straight from the GTE to memory, so packets get
    their XY words without passing through CPU registers.
  - The FLAG register reports screen-coordinate saturation and divide
    overflow: free off-screen / too-near detection.
- **GPU:**
  - No Z-buffer. Painter's order comes from a front-to-back BSP walk that
    *prepends* each packet to one OT bucket (last prepended = drawn first =
    farthest).

## 4. Memory map (provisional — sizes fixed after R2 measurements)

### Main RAM (2 MB)

| Region | Size | Notes |
|---|---|---|
| BIOS / kernel | 64 KB | `0x80000000-0x8000FFFF`, untouchable |
| Code + rodata + data/bss | ~200 KB | renderer asm is small; C grows with QuakeC |
| Stack | 16 KB | |
| Packet arenas | 2 × 96 KB | double-buffered; ~2400 `POLY_FT4` per frame; the last 1.5 KB of each holds the HUD's cached sprites (`vid_hud_block`, rebuilt only when what they show changes) |
| Ordering table | < 1 KB | a few buckets: world, sky, HUD, overlay |
| Level arena | 1152 KB | world render data + clip hulls + PVS (the biggest map 696 KB) |
| Game arena | 160 KB | the game data every map needs (`GAME.DAT`: weapon and projectile models, 66 KB), filled once at boot, plus the sound bank's directory slots (7 KB: the static sources rewritten in place at map start as 12-byte entries with their voice, reach and attenuation multiplier) and the dynamic lights' save buffer (17 KB) |
| QuakeC arena | (Phase 7) | progs.dat (must be slimmed at cook time) + edicts; takes part of the level arena's size |

The level arena is a bump allocator that is reset on map load. There is no
general-purpose heap at runtime.

### SPU RAM (512 KB)

The first 4 KB are the hardware's capture buffers; from 0x1010 up, the
map's sound bank (`MAPS/<MAP>.SND`, formats/qsound.h): every sound the map
can play as SPU ADPCM, 400-492 KB, uploaded by DMA at map load. No reverb
work area (reverb off). The music never touches it: it is CD-DA
(`docs/porting-notes.md` §23).

### Scratchpad (1 KB) — owned by the renderer during `r_frame`

As built (R6.2; the authoritative map is `engine/r_defs.inc`):

| Offset | Size | Contents |
|---|---|---|
| `0x000` | 256 B | BSP walk stack, 64 words: node address \| side \| no-draw \| clip flags. Deeper levels spill to RAM in 64-word blocks (depth ≤ 256) |
| `0x100` | 168 B | per record vertex: `SXY` (42 words) |
| `0x1A8` | 168 B | per record vertex: `SZ` |
| `0x250` | 168 B | per record vertex: grey colour word |
| `0x300` | 256 B | camera, table pointers, texture cache, counters, slow-path state, the model draw's saved state and the viewport (`SP_DMRA` .. `SP_DMK16`) |

The cook keeps every face record within `QW_MAX_VERTS` (42) vertices and
`QW_MAX_REC_PKT` (2048) bytes of packets.

### VRAM (1024 × 512 halfwords)

- Framebuffers: 320 × 240 at (0, 0) and (0, 240).
- Textures: 10 pages of 256 × 256 8bpp texels at x ≥ 320. A map uses pages
  0..5 (the first row and the first page of the second: start at ultra
  needs all six); pages 6..9 (x ≥ 448 on the second row) hold the game
  data's images: the weapons' skins at half resolution, the projectiles',
  the explosion sprite, the HUD's pictures (formats/qgame.h).
  - Tiles are packed, not on a fixed grid.
  - A tile never straddles a texture-page boundary (u ≤ 255 from the page
    base, v inside one 256-line half).
- CLUTs: all under the framebuffers, x 0 .. 319 of rows 480 .. 511, the
  one VRAM no texture page reaches (the second row's pages, from x 320, go
  down to row 511): Quake's palette, one 256-entry row at (0, 480); the
  4-bit textures' 16-entry CLUTs, 20 a row, rows 481 .. 502; the game
  data's own copy of the palette with index 255 transparent at (0, 504);
  its aux pictures' CLUTs, 20 a row, rows 505 .. 506. (Until 2026-10-18
  the 4-bit and aux CLUTs ran along rows 480+ and 504 up to x 959: a map
  whose sixth page was full overwrote the menu's CLUTs, and the game
  data's pages lost a row to them.) A debug font lives in a corner
  (x ≥ 960).
- The game's images (pages 6..9) are packed by MaxRects with a 2-texel
  border of replicated edge texels around every skin and sprite sheet:
  the GPU's texture coordinates stray a texel past a triangle's edge.
- 4-bit textures (menu option, `<MAP>_<N>.TX4`, `docs/porting-notes.md`
  §20): the same tiles at the same texel positions, pages at x = 320 + 64 p
  on the first half (y = 0), and one 16-entry CLUT per texture from
  (0, 481), 20 per row.

## 5. Frame structure

- **Simulation** runs at a fixed 60 Hz tick (Quake is tick-based). If a frame
  overruns, the sim catches up; rendering never slows the game clock.
- **Rendering** is double-buffered: the CPU builds frame N+1's packets in
  arena B while the GPU DMA draws frame N from arena A. One `VSync` per frame
  is the target; an overrun shows as 30 fps on the overlay, never as tearing.
- **The pad** is polled by the game's own driver (`engine/pad.c`), port 1
  only, in the vblank callback (10 k cycles a frame on PCSX-Redux, where a
  serial byte takes ~535 cycles; the BIOS's two-port driver took 13 k). The
  DualShock is switched to analog mode at boot by the same port code.

## 6. World format (cooked by `qcook`)

One file per map and fidelity level. Every section is 4-byte aligned and laid
out in the order the walker reads it.

- **Nodes** (32 B each): plane (`int16 n[3]`, `uint8 type`, `int32 dist`),
  `int16 child[2]`, `int16 bbox[6]`, and offsets to the node's **two face
  blocks** (faces whose plane faces the front side, faces facing the back).
  - This is Quake's model: surfaces belong to the node whose plane they lie
    on.
  - The side test the walker already does for front-to-back order also
    selects the visible block.
  - **There is no per-face backface test at all.**
- **Leaves**: contents, PVS offset, bbox, marksurface range.
- **PVS**: Quake's RLE rows, kept as-is; one row is decompressed per
  leaf change.
- **Face visibility**: Quake's visframe scheme. Marking a PVS leaf stamps its
  marksurfaces; the walker draws a face group only if it was stamped this
  frame.
- **Brush models** (QWVA, 2026-10-17): Quake's submodels keep their own BSP
  trees in the node array, their faces cooked on those nodes like the
  world's (per side, `face_id` 0: no visibility slot). A `qw_bmodel` table
  gives each one its root, bounds and clip hull roots; the game draws a
  model by having the renderer walk its subtree from the model-space camera
  (the same node-side sorting and backface selection as the world), and
  lists the solid ones for the traces. **Entities** are cooked from the
  lump into a `qw_ent` table (class enum, the id1 keys, strings in a pool);
  the engine spawns them into edicts (`engine/ents.c`). See
  `docs/porting-notes.md` §28.
- **Face block** = header + local vertex list (≤ 88 `SVECTOR`s) + polygon list.
  Each polygon is:
  - 4 vertex indices (1 byte each);
  - the 4 GPU words it needs, **pre-packed**: `clut|uv0`, `tpage|uv1`,
    `uv2`, `uv3`.

  Emitting a quad = 4 projected XY words from the cache + 4 copied words +
  tag/code. Triangles use the FT3 layout.
- **Cook-time polygon splitting**: every Quake face is cut
  1. along its **texture-repeat boundaries**, so each piece's UVs sit inside a
     single texture tile — no texture window, no `DR_TWIN`, no power-of-two
     constraint;
  2. down to a **maximum piece size** in world units.

  The piece size is a fidelity knob (bigger = fewer polygons but more affine
  warp) and is tuned with the harness.

## 7. Textures

- 8bpp, Quake's global palette, one CLUT.
- **Native size up to 256 per tile**: removing the texture window removes the
  64 px cap and the power-of-two snapping. `ultra` becomes real (native
  textures).
- Fidelity scales textures down by a factor (`high` = native ≤ 128, `low` =
  ½ or ¼). The split boundaries follow the original texel grid, so tiling
  stays correct at any scale.
- Packed per map by `qcook` (shelf packer). VRAM coordinates and
  `tpage`/`clut` words are written into the world file's pre-packed
  polygons.

## 8. Renderer pipeline (asm)

All stages live in `engine/r_*.s`. The renderer owns the GTE and the
scratchpad for the duration of `r_frame`. Its GTE matrices:
- **rotation:** the view;
- **light matrix:** |rotation|, for bounding-box radii;
- **colour matrix:** the camera in its first row, so a general plane's
  n · cam is one MVMVA (`docs/porting-notes.md` §21.2).

1. `r_frame(view)` loads the rotation and translation into the GTE, builds 4
   frustum planes, finds the camera leaf, and, if the leaf changed, decompresses
   the PVS row and marks leaves, parents and faces (Quake `R_MarkLeaves`, with
   visframe counters).
2. **Walk**: iterative front-to-back BSP traversal on the scratchpad stack.
   Each node gets a visframe check and a bbox-vs-frustum test with
   clip-flag inheritance (a fully-inside node stops testing its subtree). The
   plane side picks the near child first and the visible face block.
3. **Block**: transform each visible face record's vertices into the
   scratchpad cache:
   - `RTPT` 3 at a time, `RTPS` for the last 1-2;
   - the light bytes are expanded into colour words while the GTE runs.
4. **Emit, fast path**: all vertices of a polygon in front and on screen →
   write the `POLY_GT4` / `GT3` (XY and colour from the cache, uv words from
   the record, CLUT / tpage from the texture cache) and prepend it to the
   world bucket.
   - One packet-space test per record.
   - One texture-window command per run of same-texture records
     (`docs/porting-notes.md` §16.1).
   - **Entities** (`engine/rlist.h`, `docs/porting-notes.md` §22.3): a
     leaf's render list (particles, model entities) is drawn when the walk
     visits the leaf, with the same record loop; the view model first of
     all, on top.
5. **Slow path** (polygons whose affine error may exceed the fidelity
   level's threshold, or that come too near; `docs/porting-notes.md` §19):
   - a per-polygon conservative test (`CAREFUL`) in the record loop keeps
     the rest on the fast path;
   - corners from the record's vertex cache; a corner the GTE could not
     project → clip against the near plane in view space;
   - subdivision per edge: an edge splits while its affine error exceeds
     T px or |dx| + |dy| > 500, at its projected 3D midpoint, decided
     from its two ends alone (crack-free).

   Same asm, separate routine (`engine/r_slow.s`, spec `engine/r_slow.c`).
6. The HUD and the debug text go to the overlay bucket, after the world:
   the overlay chain (`vid.c`) draws 2D packets in order over the whole
   screen, whatever the 3D viewport (240 rows, or 192 above the original
   status bar).
7. Sky and liquids come later (R3.4) as their own passes.

ABI with C: asm entry points follow the MIPS o32 convention. They preserve
`$s0-$s8`, `$gp`, `$sp`, and use the stack slots PSn00bSDK requires when they
call C (see `reference/PSn00bSDK/doc/dev_notes.md`).

Debug safety net for the asm: a small C reference walker behind a debug flag
lets us diff visited-node and polygon counts against the asm walker on the
same viewpoint. It is not a second renderer, only the walk logic.

## 9. Measurement harness (built first)

- **Cycle counter**: root counter 2 at sysclock/8 (≈ 4.23 MHz, 8-cycle
  resolution) with a wrap IRQ extending it to 32 bits. `PROF_BEGIN/END(stage)`
  is a few instructions, usable from asm and C.
- **Bench mode** (build flag `Q1PSX_BENCH`): boots straight into the map and
  steps through a fixed list of viewpoints. The list includes the known 20 fps
  spots and a wall at contact distance. Each is rendered N frames, then the
  per-stage cycle counts and polygon counts go to stdout.
- **Headless run**: PCSX-Redux `-no-ui -stdout` captures the bench output
  without a screen, so variants can be run and compared unattended.
  - Caveat 1: that mode has no GPU backend, so it measures CPU cost only.
  - Caveat 2: emulator timing is not cycle-exact hardware. It is good for
    relative comparisons. PCSX-Redux charges 2 cycles per instruction and
    3 per store, and models the GTE, multiply and divide latencies; it
    does not model RAM wait states, the write buffer or the I-cache
    (`docs/porting-notes.md` §21.1).
  - GPU fill and the final "is it 60 fps and clean" verdict stay on the GUI /
    real hardware.

## 10. Proposed roadmap (after phases 2-6)

**R0 — Skeleton**
- R0.1 Remove `engine/`, `formats/`, every
  host tool (`tools/qcook/`, `tools/mathtest/`, `tools/gen_sintab.c`) and
  `scripts/cook.sh` from master. Keep the build system, ISO, `run-emu.sh`,
  docs and third_party notes. A placeholder `engine/main.c` keeps the build,
  CD image and headless boot check green.
- R0.2 This document validated, the roadmap rewritten.

**R1 — Perf skeleton** (measured from day one)
- R1.1 Boot, 320 × 240 double buffer, packet arenas, OT, DMA submit.
  *Validation: empty frame on screen, its cost in cycles printed.*
- R1.2 Cycle profiler + bench mode + headless runner script.
  *Validation: per-stage numbers stable across runs.*
- R1.3 Pad + minimal debug text (own font, same packet arenas).

**R2 — Data**
- R2.1 Fixed memory map (linker regions / static arenas) + CD loader.
- R2.2 `qcook`: palette + native-size texture tiles, shelf-packed, per
  fidelity.
- R2.3 `qcook` world format (§6), with a host-side validator that checks
  every invariant: block ≤ 88 verts, tree depth ≤ 48, UVs inside their tile.

**R3 — Asm renderer**
- R3.1 Block transform + `POLY_FT4` emit for one static block.
  *Bench: cycles per vertex and per polygon.*
- R3.2 Walk + frustum + PVS marking in asm: the whole of `start.bsp`, fast
  path only. *Bench: full-frame cost on every viewpoint.*
- R3.3 Slow path: near clip + depth-based subdivision.
  *Validation: a wall at contact distance fills the screen with no hole, and
  an on-screen look finds the warp acceptable.*
- R3.4 Sky, liquids, debug overlay.
  *Validation: `start.bsp` at 60 fps on every bench viewpoint + a visual
  check (no flicker, nothing vanishing or deforming).*

**R4 — Collision + movement**: Quake's hull trace + player move, rewritten
lean, same algorithms. First in C (the reference, with a host simulator),
then in asm like the renderer (2026-10-15: everything CPU-heavy goes
to asm). *Validation: as phase 6, plus identical positions between C,
asm and the host simulator on the scripted path.*

Then Phase 7 (QuakeC) onward as planned, each phase with a cycle budget
from §3.

## 11. Open questions

1. ~~Resolution~~ — 320 × 240 (2026-10-14).
2. **Second timing reference** (optional, unanswered): DuckStation is a useful
   cross-check for emulator timings but needs a BIOS dumped from a
   console.
