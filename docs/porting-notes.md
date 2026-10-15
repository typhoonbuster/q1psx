# Porting notes — Quake → PlayStation 1

Log of technical choices, float→fixed conversions, asset-format adaptations and
memory limitations encountered. Kept up to date as we go.

---

## §1 — Phase 0.1: reading the reference sources (2026-09-01)

Repos cloned into `reference/` (shallow, not versioned — added to `.gitignore`).
**No existing PS1 source port was cloned or consulted** (`quakepsx`, `PSoXide`,
etc.).

### 1.1 `id-Software/Quake` (GPLv2, 1999 release)

Structure:

| Directory | Contents | Use for the port |
|---|---|---|
| `WinQuake/` | Software-rendered engine (DOS/Win/Linux/Sun), 195 `.c/.h` files | **Main architectural base** — software rendering, no GL dependency |
| `QW/` | QuakeWorld (networked multiplayer client/server) | Ignored (multiplayer out of scope) |
| `qw-qc/` | QuakeWorld QuakeC only | Not relevant (we run the pak's `progs.dat`) |

Key engine files identified (in `WinQuake/`):

- **Loop & host**: `host.c`, `host_cmd.c`, `sys_*.c` (OS layer to rewrite for
  PS1), `common.c` (arg parsing, PAK, endianness).
- **Math**: `mathlib.c/.h` — all `float` / `vec3_t`. Target of the fixed-point
  conversion (§ to come, Phase 3.2).
- **Software rendering**: `r_*.c` (high level: `r_main`, `r_bsp`, `r_alias`,
  `r_surf`, `r_edge`, `r_sprite`) and `d_*.c` (low-level rasterizer: `d_scan`,
  `d_polyse`, `d_sky`, `d_edge`…). ~8000 lines for the core. **Not ported
  as-is**: Quake's software rasterizer is replaced by the PS1's GPU + GTE. We
  keep `r_bsp`/PVS/`r_efrag` (BSP traversal, culling) as an algorithmic
  reference.
- **Server / physics / game**: `sv_main.c`, `sv_phys.c`, `sv_move.c`,
  `sv_user.c`, `world.c` (AABB-vs-BSP collision, `SV_Move`, hull tracing).
  **To port faithfully** in fixed-point (Phase 6-7).
- **QuakeC VM**: `pr_exec.c` (bytecode interpreter), `pr_edict.c` (`progs.dat`
  loading, `edict_t` management), `pr_cmds.c` (builtins), `progdefs.h`,
  `pr_comp.h`. **To port** (Phase 7).
- **File formats**: `bspfile.h` (BSP v29), `modelgen.h` (MDL alias models),
  `spritegn.h` (SPR), `wad.h` (WAD2 textures/HUD), `model.c` (loaders).
- **Client**: `cl_*.c` (prediction, snapshot parsing, entities), `sbar.c`
  (HUD), `menu.c`, `keys.c`, `console.c`, `view.c`, `chase.c`.
- **Sound**: `snd_dma.c`, `snd_mix.c`, `snd_mem.c` (software mixing + WAV
  loaders) + per-platform `snd_*.c`. On PS1 → replaced by the SPU (hardware
  ADPCM), `snd_mix` not ported.
- **Networking**: `net_*.c` — not ported (no multiplayer).

#### PAK format (confirmed in `common.c:1225`)

Little-endian, very simple:

```c
// on disk (12-byte header)
struct dpackheader_t { char id[4]; /* "PACK" */ int dirofs; int dirlen; };
// directory entry (64 bytes), dirlen / 64 entries
struct dpackfile_t   { char name[56]; int filepos; int filelen; };
```

`pak0.pak` shareware: `PAK0_COUNT = 339` files, `PAK0_CRC = 32981` (constants at
`common.c:46`). Contains among others `progs.dat`, `gfx/palette.lmp` (768 B
RGB), `gfx/colormap.lmp`, maps `maps/e1m1..e1m8.bsp` + `start.bsp`,
`maps/b_*.bsp` (ammo/health models), `progs/*.mdl`, `sound/*.wav`, `gfx.wad`.

#### `progs.dat` (confirmed `pr_comp.h:154`)

`PROG_VERSION = 6`, `dprograms_t` header: statements / globaldefs / fielddefs /
functions / strings / globals tables. Interpreted by `pr_exec.c`. Float operands
(`OP_MUL_F`, `OP_DIV_F`, comparisons, `OP_*_V`…) will have to move to
fixed-point → **this is the trickiest compatibility point** (the gamecode's
numeric semantics must stay correct: gravity, velocities, `nextthink` timers).

#### Original memory budget (confirmed `quakedef.h:81`)

```c
#define MINIMUM_MEMORY          0x550000   // 5456 KiB (~5.3 MB) — base game
#define MINIMUM_MEMORY_LEVELPAK (MINIMUM_MEMORY + 0x100000) // +1 MB mission packs
```

Quake refuses to start below **5.3 MB**. The PS1 offers **~2 MB** of main RAM
(minus kernel/BIOS, ~1.9 MB usable). **~3x factor to claw back.** Confirms that:
(a) the hunk/zone must be resized aggressively, (b) map loading will have to be
partial/streamed, (c) some episode-1 maps may need offline simplification (BSP
decimation, lightmap reduction). To watch from Phase 2.4 / 3.3 / 4.1 onward.

Quake allocation model to reproduce (`zone.c`, `quakedef.h`): `Hunk` (low-level
stack, allocated from both ends — low = permanent map data, high = temporary
cache), `Zone` (small dynamic blocks, `Z_Malloc`), `Cache` (evictable resources
between hunk low/high). To adapt to the PS1 budget (Phase 2.4).

### 1.2 `id-Software/quake-rerelease-qc` (GPLv2)

Six QuakeC codebases (base, CTF, Hipnotic, Rogue, MG1/Dimension of the Past,
MG3/Dimension of the Machine). **No engine.** The README confirms:

- Strings replaced with localization placeholders → **requires specific engine
  support** to display messages.
- "advanced QuakeC features … only available in modern compilers" → compilation
  recommended with **FTEQCC**, not the original `qcc`.
- Targets the *Quake Enhanced* engine (extended builtins, raised limits).

⇒ Confirms the framing note: **not usable as-is** with a classic-architecture
engine. Reserved for **Phase 12** (optional), with a planned fallback to a
vanilla `progs.dat`. The `progs.dat` shipped in `pak0.pak` remains the default
target (Phases 7-11).

### 1.3 `PSn00bSDK` (Lameguy64)

- **Toolchain**: `mipsel-none-elf-gcc` (or `mipsel-unknown-elf` via
  `-DPSN00BSDK_TARGET`). CMake ≥ 3.21 build, Ninja by default.
- **Install**: either the prebuilt GitHub release (bundled MIPS GCC toolchain) +
  `PATH`/`PSN00BSDK_LIBS`, or build from source (`git submodule update --init
  --recursive` then `cmake --preset default`). `mkpsxiso`, `elf2x` provided.
  **No Arch/AUR package for the SDK itself** (`mkpsxiso-git` and
  `duckstation`/`pcsx-redux` are in the AUR).
- **`libpsn00b`** — modules: `psxgpu` (GPU, DMA, OT/drawing queue, NTSC+PAL),
  `psxgte` (GTE, fixed-point matrices/vectors, rotate/translate/perspective/
  lighting), `psxapi` (BIOS, IRQ), `psxcd` (async reads, CD-DA, XA-ADPCM,
  ISO9660 parser with no file-count limit), `psxspu` (SPU, ADPCM/VAG), `psxpad`
  (controller via BIOS + manual polling), `psxpress` (MDEC), `psxsio` (serial,
  redirects stdin/stdout → handy for debug), `psxetc`, `libc`, `lzp`
  (compression). Headers in `libpsn00b/include/`.
- **Project template** (`template/`): `main.c` (double buffering + OT + primitive
  allocation + debug font), `CMakeLists.txt` (`psn00bsdk_add_executable` +
  `psn00bsdk_add_cd_image`), `iso.xml`, `system.cnf`. **Direct starting point
  for Phase 1.3 / 2.1.**
- **Useful examples**: `examples/graphics/{gte,fpscam,billboard,render2tex}`,
  `examples/cdrom/{cdbrowse,cdxa}`, `examples/sound/{vagsample,spustream,
  cdstream}`, `examples/beginner/hello`.
- **Docs**: `doc/{installation,toolchain,cmake_reference,drawing_queue,
  dev_notes,known_bugs}.md`.
- **Emulators**: the docs recommend DuckStation or PCSX-Redux, and explicitly
  advise against **ePSXe and anything MAME-based** (inaccurate).

#### Fixed-point GTE — 16.16 or 20.12

`libpsn00b/include/psxgte.h`: `#define ONE (1 << 12)` → the GTE and all of
`libpsn00b`'s math routines work in **20.12** (4096 = 1.0), not 16.16. Formats:

- `SVECTOR` = 3× `short` (16-bit signed) → vertices fed to the GTE (raw world
  coords, max ±4096 in Quake units, fit in 16 bits).
- `VECTOR` = 3× `int` (32-bit) → translations, large coordinates.
- `MATRIX`: 16-bit entries in 1.3.12; the GTE applies a 12-bit shift (`SF` bit)
  to results.
- Trig angles: `csin`/`ccos` → 131072 = 360°; high-precision versions →
  4194304 = 360°.

**16.16 is the usual fixed-point format.** Adopting 16.16 for engine
math would force a conversion to 20.12 on every GTE call (the hot rendering
path). → **Decided (Phase 0.2)**: adopt **20.12 everywhere** to
align with the hardware, with BSP/MDL geometry stored as 16-bit integers (world
units) for direct GTE ingestion. See §2.1.

---

## §2 — Phase 0.2 architecture decisions (validated 2026-09-01)

Answered via a questionnaire.

### 2.1 Fixed-point format → **20.12 everywhere** (`ONE = 1 << 12 = 4096`)

- Aligned with the GTE and `libpsn00b`: no conversion on the hot rendering path.
- **Deliberately not 16.16** (the usual format). Reason: the
  hardware mandates 20.12 for any GTE transform; a 16.16 engine would pay a
  conversion per vertex.
- Adopted conventions:
  - Engine scalar type: `q_fixed` = `int32_t`, 20.12. Macros `FIXED(x)`,
    `FIX_MUL(a,b)` (`(int)(((int64_t)a*b) >> 12)`), `FIX_DIV(a,b)`, `FIX_ONE`.
  - Vectors: `q_vec3` = 3× `q_fixed` for engine math (physics, trace, QuakeC).
    Converted to `SVECTOR` (integers, world units) when feeding the GTE.
  - Static geometry (BSP vertices, MDL vertices): stored as **16-bit integers,
    Quake world units** in the cooked format → loadable directly into
    `SVECTOR`. Quake world coords fit in ±32767 (maps ≤ ±4096).
  - Angles: Quake convention (degrees) internally for porting readability,
    converted to the GTE scale (`csin`/`ccos`) in the camera wrapper.
  - Overflow: `FIX_MUL` goes through `int64_t`. `mathlib` dot/cross products
    will be rewritten with a 64-bit accumulator.
- Dedicated PC test suite in Phase 3.2 (edge values, comparison against the
  float reference result, documented tolerance).

### 2.2 Scope → **episode 1 first, keep hooks open for ep2-4**

- Absolute priority: a stable, playable single-player episode 1.
- Design constraint: do not lock in choices that would permanently rule out
  episodes 2-4 later (e.g. do not hardcode the map list, keep map loading
  parameterizable, do not assume a single palette/wad). Overhead accepted as
  long as it stays marginal.
- Multiplayer: **out of scope**, network stack not ported.

### 2.3 QuakeC → vanilla `progs.dat` from the pak by default

- Target for Phases 7-11: the `progs.dat` (`PROG_VERSION 6`) extracted from
  `pak0.pak`, run as-is by a `pr_exec` ported to 20.12.
- `quake-rerelease-qc` remains an **optional Phase-12 attempt**, with an explicit
  fallback to vanilla if the builtins/protocol are incompatible. No commitment
  now.

### 2.4 Toolchain → **prebuilt PSn00bSDK release (GitHub)**

- `mipsel-none-elf-gcc` toolchain bundled in the release (no GCC build).
- Version pinned, install procedure documented in `README.md` (extract + `PATH`
  + `PSN00BSDK_LIBS`). `third_party/` holds the instructions, not the binaries.
- Emulators via AUR: `duckstation`, `pcsx-redux`.

### 2.5 Quake data → a full local install

- A full Quake install is at hand (`id1/` with `pak0.pak` + `pak1.pak`). Path
  referenced locally — **never copied into the repo** (`.gitignore` + a local,
  unversioned config).
- `pak0.pak` from a retail install = byte-identical to the shareware `pak0.pak`
  (`PAK0_CRC 32981`, 339 files) → usable for all episode-1 dev.
- `pak1.pak`: local Phase-12 use only, never redistributed.
- `scripts/fetch_shareware.sh` (hash-checked) is still planned for contributors
  who do not own the game (Phase 1/14).

---

## §3 — Memory architecture (Phase 2.4, decided 2026-09-01)

The aim: **whatever is fastest — target is constant 60 fps NTSC.**

### 3.1 Allocator: faithful Hunk + Zone port, no Cache

`engine/zone.{c,h}` is a near-verbatim port of WinQuake `zone.c`:

- **Hunk** — one contiguous block, allocated stack-style from both ends. Low
  end = permanent data for the current level; high end = temp / file loading.
  16-byte aligned. Reclaimed only by resetting a mark
  (`Hunk_FreeToLowMark` / `Hunk_FreeToHighMark`).
- **Zone** — 48 KiB (`DYNAMIC_SIZE 0xc000`), carved as the first hunk-low
  allocation. First-fit free list with coalescing, for small dynamic blocks
  (`Z_Malloc` / `Z_Free`).
- **Cache (LRU-evictable)** — **not implemented.** In stock Quake it swaps
  sounds/models in and out under memory pressure; on the PS1 sound lives in SPU
  RAM and we keep level data resident, so eviction would only buy disk hitches
  against the 60 fps target. `Cache_*` call sites (alias-model loader, phase 8)
  will be shimmed to `Hunk_AllocName`. Revisit only if a real map genuinely
  cannot fit.

Rationale for a linear Hunk over a general allocator: pointer-bump allocation is
the fastest possible, zero fragmentation, sequential layout = good access
patterns, and **no per-frame allocation** at all (everything is allocated at
level load). This is the performance-optimal choice.

Divergences from `zone.c`: `byte` → `uint8_t`; `Con_Printf` → `printf`;
`Q_memset`/`Q_strncpy` → libc; `Sys_Error` is `engine/sys.c` (logs to serial,
parks the CPU); the `-zone` command-line override is dropped.

### 3.2 RAM budget

Retail PS1: 2 MB total − 64 KiB BIOS/kernel = `APP_RAM` ≈ **1.94 MiB**.
PSn00bSDK's crt0 hands almost all of `APP_RAM` (from `_end` to just below the
stack, ~1.93 MiB) to the libc `malloc` heap.

`engine/main.c` takes **1536 KiB** via one `malloc()` and passes it to
`Memory_Init`. The remainder (~340 KiB minus code/stack) stays with libc malloc
for the stack and psxcd's internal allocations (`CdSearchFile` caches the ISO
path table + directory records). `HUNK_SIZE` is a `#define`, to be tuned in
phase 11 once real BSPs load.

### 3.3 Self-test

`engine/memtest.c` runs a boot-time battery against a private 96 KiB block
(alignment, non-overlap, zero-fill, mark/free-to-mark round-trips on both ends,
`Hunk_TempAlloc` reuse, `Z_Malloc`/`Z_Free` + `Z_CheckHeap` + coalescing) and
prints PASS/FAIL to serial. Result is also shown in the on-screen overlay.

---

## §4 — Fixed-point math (Phase 3.2)

### 4.1 Scalar core — `engine/q_fixed.h`

`q_fixed` = `int32_t` in **20.12** (`Q_FIX_ONE == 4096 == 1.0`), per the Phase 0.2
decision (§2.1). Plain C + `<stdint.h>` so the whole math layer also builds on
the host for unit testing.

- `Q_FIX(n)` int→fixed (multiply, not shift — left-shifting negatives is UB),
  `Q_FIX_INT(a)` fixed→int (arithmetic `>>`, floors toward −∞),
  `Q_FIX_ROUND`, `Q_FIX_FRAC`.
- `Q_FIX_FROM_FLOAT` / `Q_FIX_TO_FLOAT` are **host-only** (`#if !defined(__mips__)`)
  — for asset cooking and tests; a stray use on-device fails to compile.
- `q_mul` / `q_div` go through `int64_t` (products/dividends before the shift
  exceed 32 bits).
- `q_sqrt` / `q_sqrt_big` — digit-by-digit binary sqrt, no division, no float.
  `q_sqrt_big` takes a possibly-wide 20.12 value (a squared length) and needs
  input `< 2^52`. On hot paths the GTE `SquareRoot12()` can replace it later.

**Representable range** ≈ ±524287. Fine for coordinates (Quake maps ±4096) but a
product/dot of two large vectors overflows the *result*. Rule: at least one
operand of `DotProduct`/`CrossProduct` should be ~unit scale (a normal, a
direction) — which is how Quake uses them. For squared magnitudes use
`Length` / `VectorNormalize` (they accumulate in `int64` and call `q_sqrt_big`).

### 4.2 Vectors — `engine/mathlib.{c,h}`

Port of WinQuake `mathlib.{c,h}`. Names kept (`vec_t`, `vec3_t`,
`VectorAdd`/`Subtract`/`Copy` macros, `PITCH`/`YAW`/`ROLL`). Changes vs stock:

- `DotProduct` is now a **function** (fixed point needs a wide intermediate and
  a shift), not a bare macro. `VectorScale`/`VectorMA`/`CrossProduct` use
  `q_mul` / wide intermediates.
- `VectorNormalize` divides each component by the length directly instead of
  `ilength = 1/length; v *= ilength` — a sub-1.0 reciprocal quantises to 1/4096
  and long vectors normalise several percent short otherwise.

### 4.2b Trig — `q_sin` / `q_cos` / `AngleVectors` (phase 3.2b)

Angles stay in **degrees** (as in stock Quake, so `AngleVectors` ports almost
verbatim). `q_sin(deg)` / `q_cos(deg)` do a lookup into `q_sintab` (generated
by `tools/gen_sintab.py` into `engine/q_trig_table.c` — 1024 entries/turn +1
wrap guard, 20.12) with 8-bit linear interpolation. The one `/360` per call is
fine — `AngleVectors` runs a handful of times per frame, never per vertex.
`anglemod` reduces mod 360 (plain `%`, 360° is not a power of two in 20.12).
`R_ConcatRotations` / `R_ConcatTransforms` are the 3×3 / 3×4 matrix products
(int64 row-dot then shift). On-device the GTE can take over rotation matrices
later.

Still deferred: `BoxOnPlaneSide`, `RotatePointAroundVector`, `FloorDivMod`,
GCD — pull in when phase 5/6 needs them.

### 4.3 Tests — `tools/mathtest/`

Host build (`make -C tools/mathtest run`): unity-compiles `q_fixed.h` +
`mathlib.c` and compares every op against a `double` reference over ~50k random
inputs per op. Inputs are quantised to 20.12 first and the reference uses those
quantised values, so the tests isolate the op's own rounding (bound
= `abs_units/4096 + |ref|·rel`, `rel` ≈ 3e-4). **650k checks, 0 failures.**
`engine/selftest.c` re-checks known-answer cases on-device (no float reference
there).

---

## §5 — CD-ROM and PAK access (Phase 3.3)

### 5.1 `engine/cd.c`

- `cd_init` → `CdInit`.
- `cd_load(name, buf, cap)` — whole-file load (used for small files like
  `HELLO.TXT`; useless for an 18 MB pak).
- `cd_find(name, &cd_file_t)` — `CdSearchFile` wrapper. Blocking; caches the ISO
  path table on first use (~2 KB libc malloc). ISO paths are
  `"\\NAME.EXT;1"` (backslash or slash, `;1` optional, case-insensitive).
- `cd_read_at(file, offset, buf, len)` — read an arbitrary byte range. Computes
  the start sector from `CdPosToInt(&file->pos)`, allocates a sector-aligned
  bounce on the **high hunk**, does one `CdRead(nsec, …)`, `memcpy`s `len` from
  the intra-sector offset, frees the bounce. One seek + one multi-sector read
  per call — fine for now; batching / a sector cache is a Phase 4-5 concern.

### 5.2 `engine/pak.{c,h}`

PAK format (id-Software/Quake `common.c:1225`), little-endian:

```
header  (12 B):  char id[4]="PACK";  int32 dirofs;  int32 dirlen
entry   (64 B):  char name[56];      int32 filepos; int32 filelen   (x dirlen/64)
```

`pak_open` reads the header via `cd_read_at`, sanity-checks `"PACK"` +
`dirlen/64`, then caches the whole directory on the **low hunk**
(`pak_entry_t` == the on-disk 64-byte layout, so it loads straight in;
`LittleLong` on `filepos`/`filelen` is a no-op on the LE R3000 but kept).
`pak_find` is case-insensitive (`Q_strcasecmp`). `pak_read_lump` streams one
lump via `cd_read_at`.

`pak0.pak` (18.7 MB) is bundled into the dev CD image by `CMakeLists.txt`
reading `QUAKE_PAK0` from `local.env` and substituting `@PAK0_ENTRY@` in
`iso/iso.xml`. It is shareware data (freely redistributable) but still never
committed. Verified: 339 entries, all offsets/sizes match a host Python parse.

The CD layout (`iso.xml` order, `mkpsxiso`) is not yet optimised for streaming
— that is a Phase 11 concern once we know the real per-level access pattern.

---

## §6 — QuakeC progs.dat (Phase 3.4)

`engine/pr_comp.h` (bytecode structs + opcodes) and `engine/progdefs.h`
(`globalvars_t` / `entvars_t`) are near-verbatim from Quake. The **field order
and count in `progdefs.h` are frozen** — the compiled `progs.dat` and
`PROGHEADER_CRC` (5927) depend on them. The only change is the leaf type:
stock `float` → `vec_t` (20.12). Since both are 4 bytes the byte layout is
identical and the `.field` offsets baked into `progs.dat` still line up.

`engine/progs.c` `PR_LoadProgs`:
1. `pak_read_lump("progs.dat")` into a low-hunk buffer (~404 KiB).
2. `pr_crc` = CRC-16 of the whole file (for a future client/server match).
3. byte-swap the 15-int header (LE no-op), check `version == 6` and
   `crc == PROGHEADER_CRC`.
4. point `pr_functions` / `pr_strings` / `pr_globaldefs` / `pr_fielddefs` /
   `pr_statements` / `pr_global_struct` / `pr_globals` into the buffer.
5. `pr_edict_size = entityfields*4 + sizeof(edict_t) - sizeof(entvars_t)`
   (832 B here — our `edict_t` drops stock's `entity_state_t baseline`, a
   network snapshot type, to be restored in Phase 8 if needed).
6. byte-swap all 6 lump tables.

**Deferred to Phase 7 (the interpreter):** `pr_globals` still holds IEEE-float
bit patterns for `ev_float` / `ev_vector` slots. A conversion pass must walk
`pr_globaldefs`, and for each `ev_float`/`ev_vector` def rewrite
`pr_globals[def.ofs]` from `float` to 20.12. Anonymous compiler temporaries
(not covered by a def) are typically zero at load and get written at runtime by
ops that will already be fixed-point. The QuakeC ops (`OP_MUL_F`, `OP_DIV_F`,
comparisons, `OP_*_V`) become `q_mul` / `q_div` / fixed compares.

---

## §7 — Asset cooking (Phase 4)

### 7.1 Model

The cook tool `tools/qcook/` is **C** (host build). **One CD image** carries
**all four fidelity levels** *and* every game at hand (`id1` + optional
`hipnotic` / `rogue`); the player chooses game + fidelity from the in-game menu
(Phase 10.3). A PS1 CD is ~650 MB and everything-cooked is ~200-250 MB, so disc
size is not the constraint — runtime RAM/VRAM is, which is exactly what the
fidelity knob trades. Levels: `tools/qcook/fidelity.h`.

| level | atlas cell | face-area cull | lightmap / geometry (planned) | intent |
|---|---|---|---|---|
| low | 16 px | drop < 1024 u² | none / heavy merge | ep1-4 + packs all fit |
| medium | 32 px | drop < 256 u² | ¼-res / light decimation | ep1-4 fit, lit |
| high (default) | 64 px | keep all | ½-res / full BSP | ep1 target |
| ultra | 64 px | keep all | full-res / full BSP | ep1 showcase |

**What actually varies today** (2026-09-03): `atlas_cell` (world-tile size, also
the VRAM atlas cell stride — capped at 64 px because a 20×7 grid of 64 px cells
is all the VRAM left of the debug font once the framebuffers are placed) and
`min_face_area` (world faces below this 3-D area are dropped at cook time —
`low` loses ~36 % of `start.bsp`'s faces, `medium` ~5 %). `max_model`,
`lightmap_shift`, `geom_decimate` are declared but unconsumed, so **`ultra` ==
`high`** until models / lightmaps land. On `start.bsp` the QTX blob is 24 KB /
84 KB / 294 KB (low/medium/high), the QSF 16 KB → 24 KB.

The disc carries every level as `START<N>.QTX` / `START<N>.QSF` (`N` =
`fidelity_t` order: 0 low … 3 ultra); `START.QBS` / `START.QGE` are
fidelity-independent (a dropped face stays an empty QSF slot, so the common
`marksurf[]` indices remain valid). `engine/main.c` boots at `high`; the demo
loop's **L2** button cycles the level and calls `reload_fidelity()`, which frees
the surface blob (kept as the top-most high-hunk allocation) and re-loads QTX +
QSF for the new level — a ~1 s blocking CD read, fine for a debug control. This
replaces the earlier runtime L2/R2 subdivision / far-clip knobs.

World textures tile, and the PS1 texture window only wraps **power-of-two**
sizes inside a 256×256 page — but Quake textures are only guaranteed multiples
of 16 (e.g. `quake` is 288×64). Rather than subdivide every surface per tile in
the renderer, `qcook` snaps each world tile to a power of two on each axis
(`snap_p2`, ≤ the fidelity cap) and nearest-neighbour resamples from the
closest built-in mip. Non-square textures stretch to the nearest p2 per axis —
a visible-but-acceptable tradeoff for now; revisit with per-tile subdivision or
NPOT-aware atlasing if it looks bad. Cooked UVs stay in original-texel units
and the texture window wraps them, so tiling count across a face is unchanged.

### 7.2 QTX2 texture container (`formats/qtx.h`)

`qcook textures` output: `qtx_header` (magic `"QTX2"` + 256-entry BGR555 CLUT +
count) then `qtx_entry[]` (name, cooked w/h, source w/h, **VRAM atlas slot**,
flags, data offset/len) then 4-byte-aligned 8bpp indexed pixel blocks.
Little-endian — host x86 and the PS1 R3000 are both LE, so it maps in with no
swapping.

- Entries are **1:1 with the map's miptex list** (miptex `i` → entry `i`);
  gaps get a 2×2 magenta marker so BSP faces index straight in.
- Atlas: a 20×7 grid of 64-texel cells in the free VRAM region right of the
  two framebuffers (x ≥ 320), CLUT one line below at (320, 480). Slot assigned
  at cook time: `vram_x = 320 + (i%20)*32` words, `vram_y = (i/20)*64` lines.
- Palette: Quake's global 256-colour `gfx/palette.lmp` → `r>>3 | g>>3<<5 |
  b>>3<<10` (PS1 `0BBBBBGGGGGRRRRR`). One CLUT, all textures reference it.
- Flags: `sky*` → SKY, `*` → LIQUID, `{` → MASKED (palette index 255 = clear).

### 7.3 QSF1 surface container (`formats/qsrf.h`)

`qcook surfaces` output: `qsrf_header` then `int16 verts[][4]` (SVECTOR, shared
vertex pool) then `qsrf_face[]` (texture index, first-facevert, count, flags)
then `qsrf_facevert[]` (vertex index + `u8` texel UV). **Exactly one entry per
world-model (model 0) BSP face, in order** — a face that can't be cooked (bad
texinfo, degenerate winding) becomes an empty slot (`num_fv` 0) the renderer
skips. That 1:1 rule makes a face index a stable key that `QBS1`'s
`marksurf[]` shares. Vertices are gathered in winding order from the surfedges;
per-vertex `s = pos·vecs[0] + vecs[0][3]` / `t` likewise, then shifted so the
face-local min is 0 and clamped to 0..255 (Quake's max face texture extent is
256; the last texel is lost — noted, minor).

### 7.4 QBS1 BSP-visibility container (`formats/qbsp.h`)

`qcook bsp` output: the slice of the BSP the runtime culler needs, world model
only. `qbsp_header` then, each section 4-byte aligned:
- `qbsp_plane[]` — deduplicated. Unit normal x4096 (int16), distance x4096
  (int32), plus Quake's `type` (0/1/2 axial → the point/plane test skips the
  dot product and is exact; 3 non-axial → carries the ~1/4096 normal rounding).
- `qbsp_node[]` — `int16 child[2]` (≥ 0 node index, < 0 → leaf `-(child)-1`),
  plane index, `int16 mins/maxs[3]` for frustum AABB tests. Padded to 20 B.
- `qbsp_leaf[]` — `visofs` (byte offset into `vis[]`, −1 = none), bounds,
  marksurface span, `contents`. Leaf 0 is the shared solid leaf.
- `uint16 marksurf[]` — every marksurface, each value a BSP face index ==
  QSF1 face index (the world model starts at face 0).
- `uint8 vis[]` — Quake's RLE-compressed PVS copied verbatim; a zero byte
  means "the next byte says how many zero bytes follow". Decompressed one row
  (`(visleafs + 7) >> 3` bytes) at a time on the PS1.

`start.bsp` → 1920 planes / 2909 nodes / 1568 leaves / 34 KiB vis ≈ 158 KiB.
The full decompressed PVS matrix would be ~300 KiB and precomputed per-leaf
surface lists > 1 MiB — both rejected on RAM; see the CHANGELOG 5.4 entry.

`QBS2` added the clip hulls (§9.1); `QBS3` added the spawn point — `qcook bsp`
scans the plain-text entity lump (`{ "key" "value" ... }` blocks, key order
arbitrary) for the first `info_player_start` and stores its `origin` + `angle`
in the header. The engine spawns the player there; maps with no such entity
fall back to the model-bounds centre. (The rest of the entity lump — lights,
monsters, items, triggers — waits for the QuakeC VM in Phase 7.)

MDL / SPR / lightmap cooking comes later (Phase 8+).

---

## §8 — GTE render pipeline (Phase 5)

### 8.1 View + wireframe (5.1)

- `r_begin_view(pos, ang)` builds one `MATRIX`: rows are the camera basis from
  `AngleVectors` (world = Quake coords, X fwd / Y left / Z up). Screen Y points
  down, so row 1 is `-up`. Translation `t = -M · pos` (world units, 20.12 → >>
  12). `gte_SetRotMatrix` + `gte_SetTransMatrix`, once per frame.
  **PSn00bSDK v0.24 bug:** those two macros load the matrix via `$t0/$t1/$t2`
  but only declare `$t2` clobbered. Harmless at `-O0`; at `-O2` GCC keeps `&m`
  in `$t0` across the two calls and the second one loads garbage → dead view
  transform, black screen. `engine/gte_fix.h` re-`#define`s both with the full
  clobber list (include it after `<inline_c.h>`). The engine also builds with
  `-fno-strict-aliasing -fwrapv` (`CMakeLists.txt`) — it maps cooked structs
  onto byte blobs everywhere, same as Quake.
- `gte_SetGeomOffset(160,120)` + `gte_SetGeomScreen(160)` ≈ 90° FOV at 320×240.
- Per vertex: `gte_ldv0` + `gte_rtps` + `gte_stsz` (Z) + `gte_stsxy` (packed
  screen XY). A vertex with `sz ≤ NEAR_Z (16)` is "behind" — its primitives are
  dropped (no near-plane clipping yet).

### 8.2 Textured surfaces (5.2) — `r_surfaces`

Per frame, per face (budget `R_MAXFACES 600` until PVS culling in 5.4):

1. **Project** every referenced vertex once, cached in `sv_x/sv_y/sv_state`
   indexed by the shared vertex pool (`sv_state` memset to 0 each frame). Skip
   the whole face if any vertex is behind `NEAR_Z` — crude, but avoids
   clipping.
2. **Back-face cull** on the first triangle's *screen* winding:
   `cross = (x1-x0)(y2-y0) - (y1-y0)(x2-x0)`; `cross ≥ 0` → cull. Quake winds
   faces CCW in the (right-handed) world; the Y-flip to screen space makes
   front faces CW → negative cross. *(Sign confirmed on screen 2026-09-02 —
   the world renders solid from inside.)*
3. **Texture window**: `qtx_slot(i)` gives `tpage` + `clut` + a `RECT` window.
   The window tiles the atlas cell with a true `mod (w,h)`:
   `mask = (256 - size) / 8`, offset `= (cell position in its tpage) / 8`, all
   in 8-pixel units. `Texcoord = (Texcoord & ~(mask·8)) | ((offset & mask)·8)`.
4. **Fan-triangulate** into `POLY_FT3`s. A `DR_TWIN` (set-texture-window
   command) must run *before* its polys on the OT; `addPrim` prepends and a
   higher OT bucket draws first, so within bucket 4 the triangles are linked
   **reversed**, then the `DR_TWIN` last — it ends up in front of them.
   `vid_alloc_raw` (reserve) + `vid_link` (link) keep the group together.

Known gaps, in rough priority: no perspective-correct texturing (PS1 has none —
affine, will warp on big near polys until surfaces are subdivided), no
near-plane clipping, no depth sort within a face beyond the fan order, flat
grey `setRGB0(128,128,128)` (no lightmaps — Phase 5 later / fidelity).

### 8.3 Per-frame primitive buffer

`PRIM_BYTES` (81920) of linear scratch per framebuffer. `vid_alloc*` bump-
allocate; once full they return a shared sink and set `prim_overflowed` instead
of corrupting RAM (a real overflow crashed the dynarec in 5.1b). `vid_text`
guards the same way before `FntSort`. The overlay shows `OVERFLOW` when it
trips — a sign the face budget or `PRIM_BYTES` needs tuning for that view.

### 8.4 Debug free-fly camera (5.3)

`fly_cam_t { int32 pos[3]; int32 yaw, pitch; int32 home[3]; }` +
`fly_cam_update(cam, pad)` in `engine/main.c`. Pad map: d-pad L/R = yaw, U/D =
forward/back along `AngleVectors` forward; Square/Circle = strafe along `right`;
Triangle/Cross = **world** Z up/down (stays intuitive when pitched); L1/R1 =
pitch (clamped ±89°); R2/L2 held = fast (160 u/frame) / slow (8); START =
restore the spawn pose (`home`, set once to the geometry-bounds centre — no
entity parsing yet). Speeds are per-frame, not time-scaled — retune once the
frame rate is real (post 5.4). No collision or gravity: that is Phase 6.

### 8.5 World culling — `r_world` (5.4)

Approach A: a thin port of Quake's `r_bsp.c`. Data is `engine/bsp.{c,h}` over a
`QBS1` blob (§7.4). Per frame:

1. **Camera leaf** — `bsp_point_leaf`: descend the node tree, at each node take
   `child[bsp_plane_side(plane, pos)]` (0 = positive side). Stops at a negative
   child = leaf.
2. **PVS** — `bsp_pvs(leaf)`: decompress that leaf's RLE row into a 1 KiB
   scratch (`MAX_MAP_LEAFS/8`), cached — only redone when the leaf changes.
   Bit *i* set ⇒ leaf *i+1* is potentially visible. Camera in solid (leaf 0) or
   a no-vis leaf ⇒ keep the last good set (a fly-cam clips walls constantly;
   Quake would show everything).
3. **Frustum** — 5 world-space planes rebuilt from the view basis each call.
   View-space inward normals (view X = right, Y = down, Z = fwd):
   right `(-1,0,1)`, left `(1,0,1)`, bottom `(0,-4,3)`, top `(0,4,3)`, near
   `(0,0,1)`; to world, `n = a·right − b·up + c·fwd` (so bottom → `4·up+3·fwd`,
   top → `−4·up+3·fwd`). The `4/3` is the 240-tall / 160-focal ratio; left/right
   are the 90° horizontal FOV. Not normalised — only signs of
   `n·(x − campos)` matter.
4. **Walk** — recurse the node tree, front child first (tail-looped down the
   back side). Each node/leaf: AABB-test its `int16` bounds against the still-
   active frustum planes; wholly outside one ⇒ prune the subtree, wholly inside
   one ⇒ clear that plane's bit for children (trivial accept). In each
   PVS-visible leaf, draw every marksurface once — dedup with a rolling
   `uint8` per-frame key over a `face_seen[]` table. Front-to-back emission +
   `addPrim` prepend = painter's order on the OT with no Z-buffer, which is
   exactly what a BSP gives you.

`draw_face` (project cached / back-face cull / fan + `DR_TWIN`) is shared with
`r_surfaces`. Budgets: `R_MAXWORLD` 800 faces as a backstop below the prim
buffer's ~975-`POLY_FT3` capacity. Recursion depth ≈ tree depth (~12 for
`start.bsp`); a pathological BSP could go deep — watch the stack if a big map
faults. `SELECT` cycles world / all-surfaces / wireframe for A/B debugging.

**Texture-window reset.** Every `DR_TWIN` leaves the GPU's texture-window
register set. Anything drawn afterwards inherits it — including the debug font,
which is in a lower OT bucket and so runs *after* the world. That tiled the
font texture at 64×64 and turned every glyph into a different glyph (a real
symptom seen on device, 5.4). Fix: `r_world` / `r_surfaces` emit a full-page
window (`RECT{0,0,0,0}`) into bucket 4 *before* any face; since `addPrim`
prepends, it is the last primitive executed in the bucket and restores the
default for buckets 3–0.

**Known gaps after 5.4** (deferred):
- *Frame rate.* PVS + frustum cut `start.bsp` from 4934 faces to a few
  hundred, but that is still hundreds of affine textured tris **and** a
  `DR_TWIN` per face — the window change stalls the GPU. Next wins: sort the
  visible faces by texture and emit one window per texture run; consider a
  lower internal resolution. Real profiling is Phase 11.
- *Near-plane clipping.* `draw_face` skips a face entirely if any vertex is
  behind `NEAR_Z` (no polygon clipping yet), so big wall/floor faces vanish as
  the camera nears them. Needs a proper near-plane clip (split the polygon on
  the plane, re-project).

---

## §9 — Collision (Phase 6)

### 9.1 Clip hulls (6.1)

Quake pre-expands the world at BSP-compile time into three collision hulls so
the runtime only ever traces a **point**: hull 0 (point-size, = the render
nodes), hull 1 (player box, mins `(-16,-16,-24)` maxs `(16,16,32)`), hull 2
(big monsters, `(-32,-32,-24)`..`(32,32,64)`). Hull 3 is unused in id1. Hulls
1 and 2 share one `dclipnodes` array; the world model's `headnode[1]` /
`headnode[2]` are their roots. `dclipnode` = `{int planenum; short child[2]}` —
a child `< 0` is a Quake `CONTENTS_*` value verbatim (`-2` = solid).

`qcook bsp` copies the clipnodes (as `qbsp_clipnode {u16 plane; i16 child[2]}`,
6 B) and the two roots into `QBS2`, reusing the plane table the renderer
already has. `start.bsp`: 5326 clipnodes (3030 for hull 1, the rest hull 2).

### 9.2 `bsp_hull_trace` — SV_RecursiveHullCheck port

A near-verbatim port of WinQuake `world.c`. Fixed-point choices:
- **Positions stay integer world units** (a coord x4096 would overflow 20.12's
  ~±524288 range — the map is ±4096 units).
- Plane/point distance `t = n·p - dist` is in world-units x4096 (`n` is x4096,
  `dist` is x4096), accumulated in `int64`. `DIST_EPSILON` (1/32 u) → `128`.
- The crosspoint `frac = (t1 ± eps) / (t1 - t2)` is computed as
  `((t1 ± eps) << 12) / (t1 - t2)` and **clamped in int64** to `0..ONE` before
  narrowing to `q_fixed` (the ratio can blow past int32 when the segment
  barely straddles the plane).
- `mid[i] = p1[i] + ((p2[i]-p1[i]) * frac >> 12)` — integer result.

Returns `trace_t { fraction (q_fixed 0..ONE), endpos[3], plane_n[3]/plane_d,
startsolid, allsolid, inopen, inwater }`. Recursion depth ≈ clip-tree depth
(~30-40); one trace per player move, so stack cost is bounded.

The debug fly-cam sweeps hull 1 for its whole per-frame delta (a swept test,
so fast moves can't tunnel). `startsolid`/`allsolid` → the move is allowed raw
(so you can fly out of solid); otherwise stop at `endpos`. No wall-slide yet —
that is `SV_FlyMove` in 6.3. **START** toggles clip; **L2+R2** snap to spawn.

### 9.3 Player movement (6.2 / 6.3)

`engine/pmove.{c,h}` — a 60 Hz fixed-point port of Quake's player physics
(`sv_user.c` friction/accel, `sv_phys.c` `SV_FlyMove`/`ClipVelocity`,
`client.qc` jump). No entities/QuakeC yet; it drives the debug camera.

**Number representation.** Velocities are plain `int32` **units per second**
(range ±2000, no fixed-point needed). Positions are `int32` world units — the
trace only speaks integers — with a per-axis `pos_acc` carry so displacements
under 1 u/frame still accumulate. Gravity has its own `grav_acc` carry. Plane
normals are the cooked ×4096 (`q12`); a `vel · n` dot is `int64`, `>>12` back
to units/s. `host_frametime` is a constant `1/PM_HZ`, folded into the
`* / PM_HZ` in each term.

**Per tick** (`pmove_step`): `categorize_position` (2 u down-trace → onground,
kill downward vel) → build wishdir/wishspeed from yaw (horizontal only —
pitch is aim, not motion) → jump → ground friction → accelerate (ground:
cap = wishspeed; air: cap = 30, rate = wishspeed — the `SV_AirAccelerate`
asymmetry that permits air-strafing) → gravity → `slide_move`.

**`slide_move`** = `SV_FlyMove`: for up to 4 bumps, `bsp_hull_trace` the
remaining displacement, advance to `endpos`, and on a hit clip the velocity —
trying each stored plane, falling back to the two-plane crease
(`cross(p0,p1)·v · dir`), dead-stopping if the result reverses the original
push. `ClipVelocity` with `overbounce` 1; a `< 2 u/s` component is snapped to
0 (Quake's `STOP_EPSILON`).

**Step-up**: if a grounded move was wall-blocked, redo it from `start + (0,0,18)`
then trace `-(18+2)` back down; keep the stepped result only if it landed on a
walkable floor *and* covered more horizontal ground than the flat attempt
(`SV_WalkMove`). Lets the player walk stairs / ledges ≤ 18 u.

Constants are Quake defaults: maxspeed 320, accel 10, friction 4, stopspeed
100, gravity 800, jump 270, stepsize 18, floor normal z > 0.7. Host sim on
`start.qbsp`: jump apex ≈ 44 u, accel to 320 in ~0.3 s, dead stop at walls.
Known rough edges: no edge-friction dropoff boost, no water, no `pos_acc`
consistency when a move is clipped mid-frame (bounded, cosmetic).

---

## §10 — Measurement harness (R1, 2026-10-14)

- **Timers.** Root counter 2 (sysclock/8 + a wrap IRQ, `engine/prof.c`) is
  ours. Root counter 1 is **not**: PSn00bSDK's `VSync()` reads it as an hblank
  counter (`VSync(1)` returns hblanks since the last vsync). Phases 2-6
  reprogrammed it, which silently broke that; it is left alone now.
- **Asm stage timing.** Asm reads the raw 16-bit counter at `0x1F801120` and
  adds `(t1 - t0) & 0xffff` ticks × 8. That is exact for any stage under
  ~520 k cycles, i.e. anything inside one frame.
- **The debug overlay is not free.** `snprintf` + `FntSort` for 5 lines of
  text cost ~40 k cycles (7 % of a frame). It has its own profiler stage
  (`overlay`) and is off in the bench build.
- **Headless runs.** `scripts/headless.sh` boots an image in PCSX-Redux
  `-no-ui -stdout` and stops on a regex, a crash marker or a timeout.
  PCSX-Redux block-buffers stdout when it is redirected to a file, so the
  script runs it under `stdbuf -oL`. Without that, nothing reaches the log
  until the process exits.
- **Bench.** `scripts/bench.sh` builds preset `bench` (`build-bench/`,
  `-DQ1PSX_BENCH`) and prints one line per scenario:
  `bench <name> n=<frames> vbl=<x100> <stage>=<avg>/<max> ... <counter>=<avg>`.
  Two consecutive runs give identical numbers. What the emulator reports is
  PCSX-Redux's CPU timing model. That is also what one sees in the
  PCSX-Redux GUI, but it is not cycle-exact hardware.

## §11 — Cooked data (R2, 2026-10-14)

`tools/qcook` (C, host): `qcook map --pak --map --fidelity --tex --world`
writes two files per map and fidelity level, and `qcook check <wld>` re-reads
a world and verifies every invariant (it also runs automatically after
`map`). `scripts/cook.sh` cooks the four levels; the disc carries them as
`START<N>.TEX` / `START<N>.WLD`, N = 0..3 (low..ultra).

- **Textures (`formats/qtex.h`, QTX3).** One tile per used miptex, holding one
  texture repetition at the level's resolution:

  | level | resolution | tile cap |
  |---|---|---|
  | low | native >> 2 | 32 |
  | medium | native >> 1 | 64 |
  | high | native | 128 |
  | ultra | native | 256 |

  - Downscaling uses Quake's own box-filtered mips when the size matches.
  - Tiles are shelf-packed into whole 8bpp pages (x = 320 + 128·i, y = 0 /
    256), one VRAM rect per page, CLUT at (0,480).
  - Pure black maps to `0x8000`, because `0x0000` is transparent on the PS1.
  - `start.bsp`: 68 tiles, from 1 page / 21 KB (low) to 6 pages / 357 KB
    (ultra). **Native textures fit in VRAM**, which phase 3's 64 px grid never
    allowed.
- **World (`formats/qworld.h`, QWV2).**
  - Nodes carry the two face blocks of their plane (front / back), a bbox as
    centre + half extents in vertex units (world × 4), and the plane
    (normal × 4096, distance × 4096 × 4).
  - Leaves carry marks, PVS offset, parent and bbox.
  - Faces are cut on their texture-repeat grid (Sutherland-Hodgman against
    `s = k·W`, `t = m·H`), triangulated into PS1-ordered quads (+ one
    triangle when odd) and grouped into face records of ≤ 63 deduplicated
    vertices.
  - Each record stores tpage + CLUT once; quads and tris are 12 B (4 premultiplied
    vertex indices + 4 page-relative UVs).
  - Vertices are not padded to a multiple of 3: the renderer's
    3-at-a-time loads may read up to 2 vertices past a record, and the stream
    ends with a 16-byte tail for that.
- **`start.bsp` numbers.** 14 019 pieces → 14 147 quads + 3 042 tris and
  38 883 vertices in 4 868 records. BSP depth 51, so the renderer stack holds
  64.

  | section | size |
  |---|---|
  | faces | 576 KB |
  | nodes | 163 KB |
  | leaves | 56 KB |
  | vis | 34 KB |
  | marks | 14 KB |
  | **world file** | **844 KB** |

  An earlier layout with 20-byte polygons and padded vertices was 990 KB.
  The level arena is now 1152 KB. Textures load first, go to VRAM and are
  freed, so the peak is the world file alone.
- **Memory follow-ups.** Vertices are 54 % of the face stream. Two levers:
  - share vertices across the faces of one node side (adjacent coplanar
    faces share edges), keeping per-face visibility inside the record;
  - store vertices as 6 bytes.
- **Bench viewpoints.** Cooked into the world: the spawn point at 4 yaws, plus
  the 3 leaves whose PVS reaches the most marksurfaces (2 yaws each) — the
  heaviest views of the map.
- **Engine (`engine/world.{c,h}`).** `world_load(fid)` uploads the texture
  pages, then loads the world and rewrites its links into pointers:
  - node children become pointers, with bit 0 set for a leaf;
  - parents, face blocks, marks and PVS rows become pointers;
  - a `face_visframe[]` array is allocated.

  `world_point_leaf()` follows Quake's tie rule (d > 0 → front). Headless:
  START2 loads, 836 KB of arena, and the spawn point is in leaf 1084 (empty).

## §12 — Asm renderer (R3, 2026-10-14)

`engine/r_world.s` (+ `r_defs.inc`) is the whole per-frame world renderer in
hand-written MIPS (`.set noreorder`, every hazard handled by hand).
`engine/render.c` only builds the view matrices and folds statistics into the
profiler.

- **View scale.** World vertices are ×4 (int16) and the GTE rotation is ×1.5
  (6144 in 1.3.12), so view space is world × 6. The GTE's perspective divide
  saturates at `SZ ≤ H/2` = 80, i.e. 13.3 world units instead of 80 at ×1.
  That is the root cause of the phases 2-6 renderer's wall bug (the view stuck
  to a wall without getting closer), fixed at the source. Range stays ±5461 units before IR saturation.
- **Walk.**
  - Iterative, front to back. A scratchpad stack holds one packed word per
    level: node pointer (24 bits) + clip flags + side + draw flag.
  - Node and leaf culling uses the visframe (PVS) test, then the frustum:
    the bbox goes to view space as centre (MVMVA rotation + TR) and radius
    (MVMVA with |rotation| loaded as the GTE light matrix), and is tested
    against `z∓x ≥ 0`, `3z∓4y ≥ 0` with clip-flag inheritance.
  - Visible leaves stamp their marksurfaces; after the near child, the node
    draws its face block on the camera's side when `|d| > 0.01` unit.
- **PVS marking.** On a camera-leaf change, the RLE row is decoded in place
  (no buffer) and each visible leaf and its unstamped ancestors are stamped.
- **Records.**
  - Vertices go through RTPT 3 at a time; `swc2` stores SXY / SZ straight
    into the scratchpad cache.
  - A record with a vertex nearer than `sz_fast` (256 → z < 43 u) takes the
    careful loop.
  - Quads and tris are written as raw-texture `POLY_FT4` / `POLY_FT3`. The
    words are pre-packed except `| clut` / `| tpage`. Each packet is
    prepended to the world OT bucket, with the link kept in `$fp`.
- **Checking without a screen.** The bench dumps one frame per viewpoint as a
  display list. `tools/pktview` rebuilds VRAM from the `.tex` file and
  rasterises the polygons like the GPU: affine UVs, painter's order, it
  refuses primitives > 1023 × 511 px. The output is
  `build-bench/views/view<N>.png`.
- **CMake re-cooks** whenever `tools/qcook` or `formats/` change, and the CD
  image depends on the cook stamp. A stale world file (missing
  `num_visleafs`) once made the renderer draw nothing.

First measurements (PCSX-Redux CPU timing, `start.bsp`, fidelity high):

| view | frame cycles | vbl | nodes | records | verts | polys |
|---|---|---|---|---|---|---|
| spawn | 643 k | 2 | 461 | 568 | 5410 | 2631 |
| others | 104 k – 423 k | 1 | 87 – 312 | 76 – 390 | 808 – 3495 | 414 – 1680 |

That is ~190 cycles per polygon (transform + emit) and ~320 cycles per
visible node in the walk. The spawn view is the one over budget.

### 12.1 Verifying an asm renderer you can't see — lessons from R3

- **Ground truth on the host.** `tools/pktview/wldref` renders a `.wld`
  directly, from the bench viewpoints:
  - default mode: all records, z-buffer, perspective-correct;
  - `WLDREF_PAINTER=1`: the renderer's algorithm in C (PVS marks, face
    stamps, near child → node block → far child, painter's order with no
    depth test). It matched the z-buffer image, which proves the algorithm
    is a valid painter's order;
  - a 5th argument (the bench log) overlays the asm's emitted polygon edges
    in green.
- **PCSX-Redux's dynarec is not the hardware.** Three findings:
  1. It computed an int64 Taylor-series loop wrongly in one translation unit
     (`render.c`): the sine table came out as sin(x) = x, which skewed every
     view matrix by π/2. GCC's output was checked by hand; the same image is
     correct under `-interpreter`. The table is now built with unsigned
     64-bit products and 32-bit divisions only.
  2. It forgives load-delay-slot mistakes (it hands over the new value). A
     `lw $ra` directly before `jr $ra` made `r_render` return into its own
     walk on the interpreter — and on real hardware — and crash with an
     unaligned load. Under the dynarec it "worked", and only showed as
     missing walls.
  3. Loading a GTE `VZ` register with `lwc2` from a word whose upper half is
     non-zero (the record header kept the sphere radius there) is avoided:
     VZ0 is now loaded from a sign-extended halfword (`lh` + `mtc2`).
- **Rules adopted.**
  - `tools/hazcheck` scans the linked ELF (`objdump -d -z`) after every build
    and fails it on any load-delay, GTE (register write < 2 instructions
    before a command) or HI/LO hazard.
  - Correctness runs use `EMU_ARGS=-interpreter scripts/bench.sh` (slow but
    exact). The dynarec bench stays the one for timings, because it is what
    the PCSX-Redux GUI runs.

### 12.2 Slow path (R3.3) — near polygons

`engine/r_slow.c`, called from the asm careful loops for any polygon with a
vertex nearer than `sz_fast`. It is a C reference for now; the asm port is
next.

1. **View transform** with the current matrix (view space, world × 6).
2. **Clip codes + clipping** against the near plane (z = 8 u) **and** the
   four frustum planes.
   - All vertices outside one plane → rejected; a plane no vertex crosses →
     skipped.
   - Clipping against the frustum, not just the near plane, comes from the
     GBA Quake prototype (`reference/GBA Engines/`). Nothing
     off-screen is ever generated and every projected coordinate stays
     inside the GPU's range. The first version (near plane only) spent most
     of its time subdividing off-screen pieces.
   - One division per clipped edge (14-bit factor) and per projected vertex
     (`(H << 16) / z`, then multiplies).
3. **Per-edge subdivision.**
   - An edge is split when it is longer than 96 px on screen or its depth
     ratio exceeds 4/3.
   - The decision depends only on the edge, and the midpoint is symmetric.
     The two triangles sharing an edge, even across records (the cook dedups
     vertex positions exactly), therefore always cut it the same way.
   - The first version split each triangle's longest edge and left
     T-junction cracks.
   - Max depth 5.
4. **Emission:** raw-texture `POLY_FT3` packets, prepended to the world
   bucket through `r_state.pkt` / `ot_head`. The asm syncs those around the
   call.

Bench viewpoints added by the cook: the floor at 60° pitch, and a wall at
contact distance. The first wall view had marched into a thin *water*
volume and sat exactly on its surface plane. Quake's PVS (asm and C
reference alike) rightly sees nothing beyond an opaque water surface; only
the brute-force z-buffer reference drew the wall behind it. The generator
now requires `CONTENTS_SOLID` and keeps the camera in empty space.

Result: all three near views match the perspective-correct reference
(slight affine wobble on the near wall, no cracks, no holes). Cost in C:
150-250 k cycles on the near views, up to 9 k cycles per near polygon. The
asm port (GTE transform and projection, index-based vertex pool) comes next.

### 12.3 Slow path in asm, oversize polygons, tiled VRAM textures

- **`engine/r_slow.s`** is the asm port of `r_slow.c`. The C file stays as
  the reference: `R_SLOW_C = 1` in `r_defs.inc` switches back to it.
  - GTE everywhere: MVMVA for the view transform. For projection the GTE is
    reloaded with rotation = 2·I and TR = 0, so RTPS projects view-space
    points directly. That replaces the per-vertex CPU division and stays
    exact down to 6.7 world units. The view matrix is reloaded at the end.
  - An index-based vertex pool (32 B entries, 8-bit indices); triangles
    are one word on a scratchpad stack. The C version copied 28-byte structs
    on every push and pop.
  - Cost against C: spawn 156 k → 62 k cycles, wall at contact
    252 k → 127 k. Validated under the interpreter: same triangle counts on
    all 12 views, no exception. `hazcheck` passes.
- **Oversize polygons.** A record is also "careful" when its centre is
  closer than 3 radii (its polygons may exceed 400 px). In careful loops,
  every corner must be within 500 × 250 px of corner 0, or the polygon goes
  to the slow path. Its clipping and subdivision keep every triangle inside
  the GPU's 1023 × 511 limit.
- **Tiled VRAM textures (`tile_min`).** At low/medium a small repetition is
  laid 2×2 / 4×4 in its VRAM tile, so one polygon covers several
  repetitions.

  | level | polys (map) | spawn cycles | spawn vbl |
  |---|---|---|---|
  | low | 9 927 | 433 k | 1 |
  | medium | 12 374 | 483 k | 1 |
  | high | 17 189 | 620 k | 2 |

  Textures stay 5 pages. Spawn polygons go from 2157 (high) to 996 (low).
- `FID=0..3 scripts/bench.sh` benches any level
  (`-DQ1PSX_BENCH_FID`).

---

## §13 — Collision and player movement (R4, 2026-10-15)

Same algorithms as phase 6 (§9): `SV_RecursiveHullCheck`, `SV_FlyMove`,
`SV_WalkMove`'s step-up, friction / accel / air accel. They are rewritten
against the cooked data, and phase 6's integer-rounding problems are fixed. The C
version is the reference for the asm port (R4.3).

### 13.1 Data

`qcook` appends a clip section to QWV2:

- `qw_clip_header` holds the counts and the hull 1 / hull 2 roots;
- then every BSP plane (`qw_plane`, 12 B: normal ×4096, axial type, dist
  ×4096);
- then every clipnode (`qw_clipnode`, 8 B: plane, two children, where a
  child < 0 is a Quake `CONTENTS_*`).

`start.bsp` costs about 60 KB. The renderer never touches it.
`world_load` points `world.planes` / `world.clipnodes` into the loaded file.

### 13.2 Numbers

- **Positions** are `int32` in **1/8 world unit** (`CLIP_SUB`).
  - Phase 6 used whole units, which rounded every bump on a gentle ramp to
    zero.
  - Plane distances `n·p − dist·8` are ×4096 ×8 and fit `int32` for maps
    within ±8191 units.
- **`DIST_EPS`** is one sub-unit (4096), against Quake's 1/32 unit. This
  margin absorbs the rounding of the next moves along a surface.
- **Velocities** are integer units/s.
  - The per-tick displacement is computed by accumulating `vel × 8` per
    axis and dividing by 60, with the remainder carried over.
  - Each bump's remaining displacement is rounded to the nearest
    sub-unit.

### 13.3 Rounding — what made the player stick, and the fixes

All of these were found with `tools/pmsim` (§13.5). Each fix keeps Quake's
behaviour, minus artefacts that only exist with integers.

1. **Split points bend the sweep.** Quake's recursion materialises the
   split point `mid` and recurses on `p1→mid`, `mid→p2`. Rounded to whole
   sub-units, `mid` changes the direction of the sub-segments.
   - Example: a slide of (0, 40, 10) running along a ramp became (0, 6, 1)
     after one split, which points *into* the ramp. It started within
     `DIST_EPS` of the ramp, so every bump returned fraction 0, and the
     player stopped at the top of the ramp.
   - Fix: no intermediate points. The recursion carries 16-bit fractions
     of the whole sweep, and each plane distance is interpolated from the
     two endpoints (`dist_at`).
   - The split fraction is solved directly: `(±eps − d_start) /
     (d_end − d_start)`, truncated toward the start.
2. **The impact point rounds into the wall.** If the impact is rounded
   toward the start on every axis, some axes can lose most of the
   `DIST_EPS` margin. That happens when the motion and the normal disagree
   on an axis, e.g. walking up a ramp.
   - Fix: `set_endpos` rounds each axis toward the open side of the plane
     hit (toward the start where the normal has no component).
   - Quake's backup loop is kept: it steps back by 1/10 of the sub-segment
     while the point is in solid, with the sweep's start as the last
     resort.
3. **Slides tipped into their plane.** `ClipVelocity` in integers, then
   the rounding of the displacement, can leave a slide a hair into the
   plane it should run along. From inside the eps shell the next trace
   stops at fraction 0. The same plane is then hit twice, and the crease
   of two identical normals is zero, so the player dead-stops.
   - Fix: `push_out` makes `v·n ≥ 0` with the smallest change on the
     normal's dominant axis.
   - It is applied to the clipped velocity, and to every bump's
     displacement against all planes hit since the last advance (two
     passes, for creases).
4. **"Advanced" is decided by position, not fraction.** A very short
   impact can round back onto the start with a fraction > 0. Quake then
   forgets its plane list, so two walls of a corner alternate forever. The
   list is now cleared only when `endpos` differs from the current
   position.
5. **The crease slide is normalised.** Quake projects the velocity on the
   unnormalised `n0 × n1`, which scales it by sin² of the planes' angle.
   Falling into a 45° wedge halved the vertical speed on every tick: the
   player hovered down at −15 u/s. The projection now divides by `|dir|²`.
6. **The player is snapped to the floor.** `categorize_position` (a
   2-unit down-trace) sets onground and gravity stops. A landing that
   ended 1-2 units above the floor stayed there. As in QuakeWorld, the
   origin now moves to the trace end.

### 13.4 Controls (no free-fly camera any more)

| input | action |
|---|---|
| d-pad left / right | turn |
| d-pad up / down | walk |
| Square / Circle | strafe |
| L1 / R1 | look up / down |
| Cross | jump (noclip: down) |
| Triangle | walk slowly (noclip: up) |
| START | noclip |
| SELECT | respawn |
| L2 | next fidelity level |
| L1 + R1 | overlay |

One physics tick per vblank: a 2-vblank frame runs 2 ticks (at most 4).

### 13.5 Verification

- **`tools/pmsim`** (host) compiles `clip.c` + `pmove.c` unchanged.
  - The default script is the PS1 bench's: settle, run, jump, run north
    into the end wall (down the stairs, up the ramp), then turn around and
    come back up the stairs.
  - The PS1 bench prints the same positions:

    | step | result |
    |---|---|
    | settle | 544 288 28 |
    | 310 u/s reached | after 8 ticks |
    | jump apex | 43 units |
    | wall | 544 1359 −7 |
    | back | 544 208 24 |

  - `pmsim <wld> stress N` is a random walk from random open spots. It
    flags ticks inside solid, and segments that barely moved although a
    16-unit trace along the wished direction is open. Result over 5000
    segments / 983 k units: 0 and 0. The remaining blocks are walls and
    corners.
- **Cost** (PCSX-Redux dynarec, 780 ticks of the scripted path): **24 k
  cycles per tick on average, 54 k max**. Most of it is likely libgcc's
  64-bit divisions and multiplies, which the asm port can replace with
  32-bit ones on normalised operands.

### 13.6 The physics in asm (R4.3)

**Method: C specification, asm port, differential test.** The C version
(`clip.c`, `pmove.c`) was first rewritten into a form the asm can match
bit for bit:
- 32-bit integers, plus 32 × 32 → 64 products (one `mult`);
- no 64-bit division: libgcc's `__divdi3` was most of the old cost;
- the trace made iterative.

The asm then follows it operation for operation. The bench build also
links the C, renamed by CMake (`clip_trace_c`, `clip_contents_c`,
`pmove_step_c`). It compares the two on:
- 4000 random sweeps over the map, a mix of short / medium / map-crossing,
  half starting in open space, one in 8 on hull 2: every `trace_t` field;
- 4000 contents queries;
- a 6000-tick random walk (commands, turns, jumps, noclip flights, a
  teleport every 300 ticks): every `pmove_t` field.

The result is 0 mismatches under the dynarec and under the interpreter. On
a mismatch the bench prints both states and the scratchpad of a replay;
that dump found the stale-object bug below in one run.

Spec changes made in the process (behaviour unchanged within rounding):
- Quake's `sv_maxvelocity` (2000 u/s per axis) is now applied. It bounds
  every product to 32 bits.
- The wish direction of a diagonal is (fwd + side) × 1/√2: forward and
  right are orthogonal, so no square root or division is needed.
- Friction applies one 0.16 scale factor instead of three divisions.
- The trace's split fraction is `(num << 16) / den`. Both operands are
  first shifted down until den fits 15 bits.

**`engine/clip.s`**
- Axial planes (type 0..2, 78 % of `start.bsp`'s clipnodes, n = 4096 on one
  axis):
  - n·p is a shift;
  - start / delta × 4096 sit in the scratchpad, indexed by the axis;
  - per node: 3 RAM loads (plane offset + children, type, distance), no
    multiply.
- General planes go through the GTE:
  - the rotation matrix holds start, delta and a probe point as its rows;
  - one `MVMVA` (sf = 0, no translation) of the normal returns n·start,
    n·delta and n·point in MAC1-3;
  - positions must therefore fit int16 (maps within ±4095 units);
  - `r_render` reloads the matrices every frame, so borrowing them is
    free.
- The split shift comes from the GTE's leading-zero counter (LZCS/LZCR),
  followed by one 32-bit `divu`.
- Pending splits are pushed on the stack (16 B: node pointer | side, f1,
  fm, f2). F_ONE stays in `$fp`. The walk's next-node test does the next
  node's first instruction in its delay slot.
- Format changes, at no size cost:
  - distances pre-scaled to 1/8 unit;
  - each clipnode stores its plane as a word offset (index × 3), so no
    × 12 is left;
  - the cook checks that clip trees are at most 64 deep.

**`engine/pmove.s`** — a full port of `pmove.c`:
- The `pmove_t` is copied into the scratchpad (unrolled, 28 loads and
  stores) and every intermediate vector lives there.
- Dot products are 3 `mult`s, with the next operands loaded in the HI/LO
  shadow.
- `isqrt32` starts at the top bit, found with LZCS.
- It calls the C `r_sin` / `r_cos`. Inlining them would save about 10
  instructions per call; not worth it.

**Cost (PCSX-Redux dynarec, cycles):**

| | before R4 fixes | C 32-bit | asm |
|---|---|---|---|
| random walk, per tick | — | 9 400 | 6 800 |
| scripted path, per tick | 24 000 | 13 300 | 11 500 |
| pmove without its traces, per tick | — | 2 100 | 1 850 |
| trace (hard mix: long, in solid) | — | 2 400 | 1 650 |

A tick does 4-5 traces, and 8 when the step-up is retried against a wall
every tick; traces are about 3/4 of the cost.

The dynarec charges roughly per instruction. Real hardware also pays for
RAM loads (no data cache) and `mult` / `div` latency, which these numbers
do not show. The asm is written for both: scratchpad state, fewer loads.

**Open option (a call to make: RAM against CPU).** Self-contained
16-byte clipnodes, holding normal, type, distance and both children, would
remove the plane indirection. An axial node would need 3 loads and about
13 instructions instead of 17, so roughly 30 % less per node. The cost is
+20 KB on `start.bsp` (85 KB of clip data instead of 65 KB).

**Lessons**
- **CMake does not scan `.include` in assembly sources.** A changed
  scratchpad map in `p_defs.inc` left `pmove.s` on the old one. Its
  player position then overlapped `clip.s`'s new split record and came out
  as F_ONE. Every `engine/*.s` now has `OBJECT_DEPENDS` on `engine/*.inc`.
- **GAS's 2-operand `div` / `divu` is a macro.** It assembles to a zero
  check, `break 7` and an extra `mflo`. The bare instruction is
  `div $zero, rs, rt`.
- **The VZ0 trap again.** The word after a plane's n[0..1] carries the
  plane type in its upper half, and PCSX-Redux does not drop it on
  `lwc2 VZ0`. Load it with `lh` + `mtc2`, as the renderer does for its
  record spheres.
- **`prof_now` could go back by one counter wrap** when the wrap IRQ was
  still pending. A monotonic guard now adds the missing 0x10000.

---

## §14 — Geometry and RAM (R5, 2026-10-15)

### 14.1 Measurements and the decision

Every Quake map was cooked: 32 maps, low and high.

| world in RAM | high | low |
|---|---|---|
| episode 1 average | 833 KB | 643 KB |
| episodes 2-4 average | 980-1104 KB | 713-837 KB |
| worst map (e3m5) | 1427 KB | 1008 KB |
| maps above today's 1152 KB arena | 7 | 0 |

QuakeC, entities and models will need a good part of that arena.

**Decision (2026-10-15):**
- target about **700 KB of world per map** at the default fidelity
  (revisited in Phase 7-8);
- plan:
  1. R5.1: every map loads;
  2. R5.2: texture windows;
  3. R5.3: shared vertices and a compact format;
  4. measure again; lossy decimation only where still needed, models
     when they arrive.

**Where the polygons come from.** Quake's geometry is already coarse,
about 5 k faces per map. Most of our polygons come from cutting faces at
every texture repetition: the PS1 has no wrap mode, so a polygon's UVs must
stay inside one VRAM tile.
- At high, 40-55 % of the polygons are such cuts.
- The GPU's texture window (GP0 E2h) repeats a power-of-two texture in
  hardware, within the 8-bit UV range.
- Simulating it in the cook (repetitions up to 256 texels) gives high
  fidelity exactly the polygon count of low:

  | map | polygons high | with texture window | world |
  |---|---|---|---|
  | start | 17 189 | 9 927 | 926 → 752 KB |
  | e3m5 | 32 468 | 14 568 | 1427 → 1009 KB |

  Low's 1/4-resolution textures, tiled 4 times in a 64-texel VRAM tile,
  already cover 256 original texels; that is why the counts match.

The remaining world on e3m5 after that would be:

| part | size |
|---|---|
| nodes | 190 KB |
| vertices | 300 KB |
| polygons | 175 KB |
| record headers | 125 KB |
| leaves | 65 KB |
| PVS | 50 KB |
| clip hulls | 95 KB |

These are R5.3's targets.

### 14.2 R5.1 — every map loads

- **BSP depth up to 110** (e3m5); the renderer's walk stack was 64 words of
  scratchpad. Rather than giving it scratchpad the caches need, the full
  64-word block is spilled to RAM (`r_walk_spill`, 3 blocks, so depth ≤
  256 = `QW_MAX_DEPTH`).
  - It is refilled when `pop` finds the scratchpad stack empty: that test
    already existed, so pops cost nothing.
  - Pushes pay 2 instructions: about +1 % on the walk.
  - Validation: with `WALK_SHIFT` forced to 8-entry blocks, the dumped
    display lists of the 12 `start.bsp` views are identical word for word
    to a normal run.
- **Clip hulls up to 65 deep** (hull 2 of e1m2): the trace's pending-split
  limit is now `QW_MAX_SPLIT` = 128, and the cook checks it.
- **Disc layout:** every cooked map ships at 4 levels as
  `\MAPS\<MAP>_<N>.TEX/.WLD`.
  - They sit in a subdirectory because the BIOS reads only the root
    directory's first sector. With the maps in the root, `SYSTEM.CNF`
    sorted past it and the disc booted into the OpenBIOS shell.
  - PSn00bSDK's `CdSearchFile` reads whole multi-sector directories.
- **Cook:** `scripts/cook.sh` cooks in parallel.
  - `COOK_MAPS` = `ep1` (default), `all` (needs `QUAKE_PAK1`) or a list.
  - Retail maps take the palette from pak0 (`qcook --palette-pak`).
- **Engine:**
  - `world_scan_maps` probes the disc and `world_load(map, fidelity)`
    loads one map.
  - A map too big for the arena falls back to the highest level that
    fits: e1m4 high is 1160 KB, so it loads at medium.
  - **R2** cycles the maps; the overlay shows the map and the arena use.
- **Bench:** `BENCH_MAPS=1 scripts/bench.sh` loads every map and benches
  its four spawn views. All 9 episode-1 maps load and render.

### 14.3 R5.2 — texture windows

**GPU side.** GP0 E2h sets the texture window:
`texcoord = (texcoord & ~(mask × 8)) | ((offset & mask) × 8)`, per axis,
in 8-texel units. A texture whose size on an axis is a power of two, placed
in VRAM at a multiple of that size, then repeats in hardware: the polygon's
8-bit UV is a virtual coordinate (0..255) and the window folds it onto the
texture.

**Cook** (`tools/qcook/tex.c`, `world.c`):
- **Window per axis.** A power-of-two axis wraps; smaller than 8 texels
  (the window's granularity) is resampled up to 8. Other axes keep the old
  scheme: VRAM repetitions (`tile_min`) and a cut at every tile.
  - 11 % of Quake's textures are not powers of two (buttons 48×48,
    doors 128×192, signs 240×192). Most are 64 or 128 wide, so their u
    axis still wraps.
- **Packing.** The shelf packer is replaced by first-fit on a grid of 8×8
  cells, biggest first. A wrapped axis starts at a multiple of its size.
- **Cutting.** A wrapped axis is cut every 256 cooked texels (a byte of
  UV).
  - The cut grid starts at the face's own first repetition boundary
    (`floor(smin / w) × w`), not at a global grid. This keeps the virtual
    coordinate in phase with the texture and cuts each face as little as
    possible. It does better than the earlier simulation: start high
    9 927 → 8 207 polygons.
  - u = 256 does not fit a byte, so it is clamped to 255: a 1/256
    squeeze on polygons that span a whole cell, invisible.
- **Format QWV3.**
  - A per-map texture table `qw_tex {clut, tpage, window}` (8 B per
    miptex);
  - records name their texture (`tex`, +8) instead of carrying
    clut / tpage (same 20 bytes: `sr` +10, `sc` +12).
  - `qcook check` validates the indices.

**Renderer** (`engine/r_world.s`):
- **Per record.** Once its polygons are emitted (fast path and slow path),
  the record prepends a 2-word packet with its E2 command. The bucket is
  built by prepending, so the command lands in front of its polygons.
- **Reset.** `r_render` first prepends E2 = 0: it ends up last in the
  world bucket, so the overlay and the next frame start without a window.
- **Texture table.** Looked up once per record (the table's base is in the
  scratchpad); the clut / tpage words go to the scratchpad for the slow
  path, which no longer reads them from the record.

**Tools.** `pktview` applies E2 packets and `wldref` applies the table's
windows. The images match the reference; the larger polygons show a little
more affine warp, as expected (an eye on screen decides).

**Results** (PCSX-Redux dynarec, start, high):

| | before | after |
|---|---|---|
| polygons, map | 17 189 | 8 207 |
| polygons, spawn view | 2 157 | 774 |
| spawn view, cycles | 620 k (2 vbl) | 426 k (1 vbl) |
| world, start | 938 KB | 720 KB |

- Every bench view of every episode-1 map now runs in 1 vblank; E1M7's
  spawn view goes from 566 k (2 vbl) to 424 k.
- Some near views spend more in the slow path (view 3: 80 k → 212 k),
  because larger polygons are more often near or oversize. They stay at
  1 vbl; the slow path is the next thing to tune.
- Low-fidelity textures shrink from 304 to 25 KB (no more repetitions in
  VRAM). Low and high now differ mostly in texture resolution: 7 963
  against 8 207 polygons.
- **World per map at high:**

  | map | before | after |
  |---|---|---|
  | start | 938 KB | 720 KB |
  | e1m1 | 984 KB | 724 KB |
  | e1m2 | 1080 KB | 738 KB |
  | e1m3 | 978 KB | 694 KB |
  | e1m4 | 1160 KB (fell back to medium) | 823 KB |
  | e1m5 | 918 KB | 658 KB |
  | e1m6 | 654 KB | 520 KB |
  | e1m7 | 291 KB | 213 KB |
  | e1m8 | 580 KB | 412 KB |
  | e3m5 | 1427 KB | 933 KB |

  Every episode-1 map now fits at high. Four are still above the 700 KB
  target, for R5.3.

### 14.4 R5.3a — shared vertices

Each Quake vertex was stored once per face record that used it: 3.4 copies
on average (e1m4: 27 509 vertices, 8 145 distinct).
- **Format QWV4.** A per-map vertex pool (`qw_vert`, 8 B, `lwc2`-ready).
- **Records.** They hold 16-bit indices into the pool, padded with copies
  of the first to a multiple of 3, then to 4 bytes, plus the offset of
  their first polygon (`polys`, +18).
  - The padding is there because the renderer transforms three vertices
    at a time and once read up to two vertices past the list.
  - The polygons' per-record slot indices (× 4, into the scratchpad
    cache) are unchanged.
- **Renderer.** `dr_xf` gathers the three vertices of each RTPT through the
  indices (`lhu`, `sll 3`, `addu` the pool base). The slow path and its C
  reference fetch through the pool the same way.
- **Cook.** A hash table on (x, y, z) builds the pool; `qcook check`
  validates every index.

**Results.**
- World, high:

  | map | before | after |
  |---|---|---|
  | start | 720 KB | 648 KB |
  | e1m4 | 823 KB | 741 KB |
  | e1m7 | 213 KB | 191 KB |

  Episode 1 now spans 191-741 KB; only e1m4 is still above 700 KB.
- The gather costs about 5 % of the heaviest frame: start's spawn view
  426 k → 450 k cycles, still 1 vblank everywhere.
- The images are identical to R5.2.
- Caching transformed vertices across records (each pool vertex
  transformed once per frame) would need about 8 B of RAM per vertex,
  which is what this step saves; not done.
- `scripts/cook.sh` now empties `cooked/id1` first: a map cooked earlier
  in an older format had stayed on the disc and failed to load.

### 14.5 R5.3b — compact nodes and leaves

**Format QWV5.**
- Nodes go from 56 to **44 bytes**, leaves from 36 to **28 bytes**.
- Both keep the bbox at +0 / +8 (the frustum test takes either). The
  halfwords after c[2] and h[2] now hold `visframe` and `type` (leaves:
  `num_marks`), so VZ0 / VZ1 are loaded with `lh` + `mtc2`.
- Parents are 16-bit word offsets into the node array (index × 11,
  `QW_NOPARENT` = 0xFFFF); `mark_pvs` adds the array base, kept in
  `r_state.nodes`. The cook rejects maps with more than 5957 nodes (Quake's
  largest has about 3600).
- A node keeps two face pointers: the back block ends where the next
  node's front block starts. A sentinel node closes the array, and the
  walk computes the end at `node + 40 + side × 40`; C code uses
  `qw_face_end()`.

**Results.**
- World, high:

  | map | before | after |
  |---|---|---|
  | start | 636 KB | 589 KB |
  | e1m4 | 729 KB | 677 KB |
  | e3m5 | 823 KB | 768 KB |

- **All 32 maps:** average 565 KB; three above the 700 KB target (e3m5
  768, e4m7 738, e3m6 726), e2m4 exactly at 700.
- The walk costs about 3 % more (lh + mtc2, the block-end arithmetic):
  start's spawn view 450 k → 454 k cycles, still 1 vblank.
- Images identical to R5.2.

**Build note.** A re-cook that a build triggers now keeps the maps already
in `cooked/id1`: CMake globbed the disc's file list at configure time, so a
build that changed the set failed at the ISO step. `COOK_MAPS` still
selects a set explicitly.

**What is left without loss is small:**
- record headers (20 B; `size` can be derived): about 10 KB per map;
- clipnodes 8 → 6 B: about 10 KB per map.

That is not enough for e3m5's −68 KB. The step after this is R5.4: measure
again, then lossy decimation for the few maps still over budget, or a
budget revised once QuakeC and the models are measured (a call to make).
