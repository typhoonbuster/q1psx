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
| Packet arenas | 2 × 96 KB | double-buffered; ~2400 `POLY_FT4` per frame |
| Ordering table | < 1 KB | a few buckets: world, sky, HUD, overlay |
| Level arena | ~700 KB | world render data + clip hulls + PVS + entities |
| Game arena | ~500 KB | progs.dat (must be slimmed at cook time) + edicts |
| Free / models | rest | MDL meshes, Phase 8 |

The level arena is a bump allocator that is reset on map load; the game arena
likewise. There is no general-purpose heap at runtime.

### Scratchpad (1 KB) — owned by the renderer during `r_frame`

| Offset | Size | Contents |
|---|---|---|
| `0x000` | 128 B | packet ptr / end, OT ptr, frame counter, camera position, frustum planes |
| `0x080` | 192 B | explicit BSP walk stack (node + clip flags per word, depth ≤ 48, checked at cook time) |
| `0x140` | 704 B | projected-vertex cache: 88 entries × 8 B (`SXY`, `SZ` + flags) |

The cook tool guarantees that no face block references more than 88 vertices,
splitting blocks if needed.

### VRAM (1024 × 512 halfwords)

- Framebuffers: 320 × 240 at (0, 0) and (0, 240).
- Textures: everything at x ≥ 320 (704 × 512 halfwords ≈ 700 KB of 8bpp).
  - Tiles are shelf-packed, not on a fixed grid.
  - A tile never straddles a texture-page boundary (u ≤ 255 from the page
    base, v inside one 256-line half).
- CLUT: Quake's palette, one 256-entry row. A debug font lives in a corner.

## 5. Frame structure

- **Simulation** runs at a fixed 60 Hz tick (Quake is tick-based). If a frame
  overruns, the sim catches up; rendering never slows the game clock.
- **Rendering** is double-buffered: the CPU builds frame N+1's packets in
  arena B while the GPU DMA draws frame N from arena A. One `VSync` per frame
  is the target; an overrun shows as 30 fps on the overlay, never as tearing.

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
scratchpad for the duration of `r_frame`.

1. `r_frame(view)` loads the rotation and translation into the GTE, builds 4
   frustum planes, finds the camera leaf, and, if the leaf changed, decompresses
   the PVS row and marks leaves, parents and faces (Quake `R_MarkLeaves`, with
   visframe counters).
2. **Walk**: iterative front-to-back BSP traversal on the scratchpad stack.
   Each node gets a visframe check and a bbox-vs-frustum test with
   clip-flag inheritance (a fully-inside node stops testing its subtree). The
   plane side picks the near child first and the visible face block.
3. **Block**: transform the block's vertices with `RTPT` (3 at a time) into
   the scratchpad cache, recording FLAG bits per vertex.
4. **Emit, fast path**: all vertices of a polygon in front and on screen →
   write the `POLY_FT4` (XY from the cache, pre-packed words copied) and
   prepend it to the world bucket. Polygons entirely off one screen edge are
   dropped with 4 compares.
5. **Slow path** (the few polygons near the camera):
   - a vertex behind the near plane → clip in view space;
   - a polygon very close (small `SZ`) → subdivide 1-2 levels to keep affine
     warp invisible.

   Same asm, separate routine, rarely taken.
6. Sky and liquids come later (R3.4) as their own passes.

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
    relative comparisons.
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
