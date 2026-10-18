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
| left stick / d-pad | walk forward / back, strafe left / right (§18) |
| right stick | look: turn left / right, up / down inverted (§18) |
| Square / Circle | turn left / right (pads without sticks) |
| L1 / R1 | look up / down |
| Cross | jump (noclip: down) |
| Triangle | walk slowly (noclip: up) |
| START | noclip |
| SELECT | respawn |
| SELECT + START | back to the map menu (§17) |
| L2 | next fidelity level |
| R2 | next map |
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

### 14.6 R5.3c — several faces per record

qbsp cuts large surfaces into faces of about 240 units, for lightmaps the
PS1 does not use. A record may now hold several faces: adjacent (sharing an
edge), same node side and texture. They share the 20-byte header and their
vertex indices. Record ids are the visibility ids: a record is drawn when a
leaf marking one of its faces is visible, and the marks are rewritten from
faces to records (deduplicated per leaf).

**Variants measured** (start's 12 views against the per-face records of
R5.3b, PCSX dynarec):

| grouping | worst frame | other views | polygons drawn, view 8 | e3m5 |
|---|---|---|---|---|
| none (R5.3b) | 454 k | — | 279 | 768 KB |
| same node side + texture | 434 k | up to +40 % | 483 | 678 KB |
| + connected, visibility per record | 422 k | up to +40 % | 465 | 684 KB |
| + identical marking leaves | 431 k | +5 to +9 % | 333 | 715 KB |
| **+ radius ≤ 160 units (kept)** | **436 k** | **all ≤ R5.3b** | **302** | **739 KB** |

1. Grouping only by node side and texture draws whole groups (faces from
   other rooms on the same plane) when one face is visible.
2. Connectivity alone is not enough: a big floor cut into pieces spans
   leaves with different visibility.
3. Grouping only faces marked by exactly the same leaves makes visibility
   exact. Bigger records then still let off-screen neighbours through the
   per-record sphere test.
4. Capping the merged record's radius at 160 units (about one qbsp face)
   removes that. Faces are taken breadth first over shared edges, so
   records are compact patches.

Results: images identical; start 589 → 567 KB, e1m4 677 → 655 KB, e3m5
768 → 739 KB. The 32 maps average 547 KB; three are slightly above 700 KB
(e3m5 739, e4m7 720, e3m6 704).

### 14.7 R5.3d — the last lossless crumbs (QWV6)

- **Render nodes use the clip section's plane table.** Each node used to
  embed its plane (12 B); it now holds a word offset (`plane3`), and
  nodes go from 44 to **36 bytes**.
  - The table stores distances in 1/8 units ×4096; the renderer works in
    vertex units (world × 4), so it halves them (`sra 1`).
  - The back block now ends at `node + 32 + side × 32`, one shift.
  - The walk costs about 1 % more for the indirection.
- **Clipnodes go from 8 to 6 bytes** (no padding): `num × 6` is two shifts
  and an add in the trace, which costs about 5 % more (about 260 cycles
  per tick). The physics differential tests still show 0 mismatches.

**Result: every map within the 700 KB target.**

| | |
|---|---|
| 32 maps at high, average | 514 KB |
| largest (e3m5) | 696 KB |
| e4m7 | 677 KB |
| e3m6 | 664 KB |
| e1m4 | 617 KB |
| start | 534 KB |

Images are identical throughout R5.2 → R5.3d.

**R5 summary (high fidelity, PCSX dynarec):**
- World size, all 32 maps: average 1030 → 514 KB, worst 1427 → 696 KB.
  Every map cooks and loads; 7 did not fit the 1152 KB arena before.
- start: 17 189 → 8 207 polygons; the spawn view 620 k (2 vblanks) →
  440 k cycles (1 vblank). Every episode-1 bench view runs at 60 fps.
- No lossy step was needed.

---

## §15 — Static lighting and dithering (2026-10-15)

**The goal.** quake-psx has no dynamic lighting, yet it looks clean,
noisy but smooth. What gives that look is Quake's **static** lighting (the
lightmaps qbsp / light baked into the map), smoothed by the GPU's
dithering.

**What dithering is on the PS1.** A drawing-mode bit, GP0(E1h) bit 9.
When a pixel's colour goes through the GPU's 24-bit path (Gouraud shading
or texture modulation), it is rounded to 15 bits with a 4×4 offset pattern
instead of being truncated. It costs nothing and gains nothing in speed.
Raw-textured polygons, which the world used until now, never go through
that path, so the bit alone changed nothing. Textured polygons cannot set
it either: bits 9-10 of their texpage attribute are ignored. The engine
enables it in the drawing environment (`vid.c`, `dtd = 1`).

**Cook** (`tools/qcook/world.c`):
- Each face's lightmap is located as Quake does it (`CalcSurfaceExtents`):
  one luxel per 16 texels, from `floor(min / 16) × 16`, up to 4 styles.
- Every piece vertex gets the light sampled bilinearly at its texture
  coordinates, summed over the styles at their normal brightness.
- The byte modulates the texture the way the GPU does: 128 leaves the
  texel as is, 255 about doubles it. This is the same scale as Quake's
  software colormap (light 127 = row 32, the identity row), so no
  conversion is needed.
- Faces without a lightmap in a lit map are black, as in Quake.
- `--light-gain` (`LIGHT_GAIN` in `cook.sh`) scales the light. 1.0 is
  faithful and dark (start's mean light is 50); 1.6 is much more readable
  on a TV. The default is 1.5 (chosen 2026-10-15, to try on
  a screen). It costs nothing at run time and +584 bytes over the 32
  maps.
- **Format QWV7:**
  - each record carries one light byte per vertex, padded to 4, ending
    where the polygons start: `polys - ((num_verts + 3) & ~3)`;
  - the light is part of a record slot's key, because two faces in one
    record can light a shared corner differently;
  - `QW_MAX_VERTS` drops from 63 to 42, so the scratchpad holds SXY, SZ
    and light per slot (3 × 168 B in 0x100-0x2F7).

**Renderer:**
- `dr_xf` expands the three light bytes to `0x00cccccc` while the GTE
  computes the RTPT (`x = c | c << 8`, then `x | x << 8`), into `SP_COL`.
- Polygons go out as POLY_GT4 (12 words) / POLY_GT3 (9), texture ×
  colour.
- The slow path carries the light in its pool vertices (+24): it is
  interpolated by the clip lerps and the subdivision midpoints, and
  triangles go out as GT3. Its C reference matches.

**Tools.** `pktview` decodes Gouraud polygons and applies the modulation
and the GPU's dither matrix. `wldref` does the same with perspective-
correct light.

**Cost.**
- Start's spawn view goes from 440 k to 481 k cycles (+9 %): bigger
  packets, and three colours per vertex. Every view stays at 1 vblank.
- World: start 534 → 570 KB (light bytes, plus slots split by light).

**Verification and tooling.**
- The interpreter's console doubles or drops characters on long dumps,
  so the text display lists of the dynarec and the interpreter differed
  by 115 garbled lines.
  - `vid_dump` now also prints an FNV-1a hash of the display list,
    computed on the PS1 (`H` line).
- `scripts/dlcheck.sh save|check <name>` reruns the bench and compares
  the display lists and hashes with a saved reference. An optimization
  that changes no pixel must leave them identical. It also prints the
  per-view cycle change.
- The disc image now depends on the cooked files themselves. A manual
  cook used to leave the previous data on the disc.

## §16 — The optimization pass (R6.2, 2026-10-15)

**The goal.** Maximum performance with no quality loss, even a few instructions saved per call, the resources being that limited.

**Method.** Every step was checked three ways before it was kept:
- `scripts/dlcheck.sh check <ref>`: the display lists of the 12 bench views
  are identical, word for word, to the reference;
- the interpreter's hashes (`EMU_ARGS=-interpreter`) match the dynarec's,
  because the dynarec hides load-delay mistakes (hazcheck only sees linear
  code, not branch targets);
- the bench's C-vs-asm physics diffs: 0 mismatches.

One step changes packets on purpose: one texture-window command per run of
same-texture records (16.1). Its lists were compared after dropping every
E2 command that sets the window already in force (a no-op on the GPU), and
were then identical. Reference `r62` was saved after it.

### 16.1 Face records (`draw_records`, the bulk of a frame)

- **KUSEG packets.** `s6` / `s7` run as `0x00xxxxxx` addresses, so a
  packet's address is its own OT link. The per-packet masks disappear.
  `walk_done` turns `s6` back into a KSEG0 pointer for C.
- **Inline in the walk.** `pop` falls straight into `draw_records`, which
  branches back to `walk`: no `jal` / `jr`. The arena-full test runs once per
  face block, and the block end stays in `a1`.
- **Texture cache.** 77-78 % of records use the previous record's texture
  (measured on start and e1m4). `v0` / `v1` keep its `clut << 16` /
  `tpage << 16`:
  - `v1` lives across face blocks (the walk never touches it);
  - `v0` is reloaded once per block.
  A miss reads the 8-byte `qw_tex` entry.
- **One packet-space test per record**, instead of one per polygon. `s7`
  stops `QW_MAX_REC_PKT + 8` bytes (2056) short of the arena end, so a
  record that starts below `s7` cannot overflow:
  - the cook keeps every record under `QW_MAX_REC_PKT` (2048 bytes: 52 per
    quad, 40 per triangle, 8 for a window command), and the validator
    checks it;
  - the largest record of the 32 maps needs 1580 bytes, so the cooked
    output is unchanged;
  - the slow path still tests per triangle, and so keeps `s6 <= s7`.
- **Texture window once per run.** The E2 command goes out when the
  texture changes (or in `walk_done`), not after every record.
  - It is prepended last, so it is drawn before every polygon of the run.
  - 1649 → 379 E2 commands over the bench views, with the same pixels.
  - This only holds because nothing else draws into the world bucket during
    the walk. Models drawn into it later will have to set their own
    window.
- **RTPS tail.** Records are padded to a multiple of three vertices for
  RTPT. The last 1 or 2 vertices now get one RTPS each instead of a padded
  RTPT (about 11 instructions saved per record).
- **Smaller items:**
  - culled and unstamped records skip to the next record in 1 and 9
    instructions;
  - `(nv + 3) & ~3` takes one `andi`;
  - the triangles' uv2 word is stored as is (the cook writes `pad2 = 0`,
    and the validator checks it);
  - the careful-path size test's first load sits in a delay slot;
  - the polygon counter counts down at the top of the loop.

### 16.2 The walk

- **Inlined tests.** `frustum` becomes a macro, `FRUSTUM`, used for nodes
  and leaves; it updates `s2` in place and branches to `pop` on a cull.
  The plane distance is inlined into the node path. Only the camera-leaf
  search still calls `plane_dist` (once per level, per frame).
- **Loads ahead.** The box's loads sit in the visframe test's delay slots
  (harmless when the node is skipped).
- **Cheaper tests.** `|d| <= eps` is one unsigned compare:
  `(unsigned)(d + eps) <= 2 eps`.
- **Stack word re-encoded:** node address (low 24 bits, KUSEG) | side × 4
  << 24 | no-draw << 27 | clip flags << 28.
  - `pop` takes 14 instructions to the face block instead of 18; the far
    child is `child[1 - side]` at `node - side × 4 + 24`.
  - The push's spill test doubles as the loop branch.
- **Mark loop.** It runs on an end pointer, with the next mark loaded in
  the branch delay slot (6 instructions per mark).
- **Scratchpad copies.** The plane-table and vertex-pool pointers are
  copied to the scratchpad (`SP_PLANES`, `SP_VERTPOOL`). The emulator
  charges RAM and scratchpad loads alike; the hardware does not.

### 16.3 The slow path

Before this pass, a near polygon cost about 1000 instructions, even when it
emitted almost nothing (view 0: 30 slow polygons for 23 triangles).

- **Per-vertex packet words.** The grey colour word and the rounded uv
  word are computed once per vertex, at projection. Before, they were
  computed for each of the ~3 triangles using a vertex.
  - The pool entry keeps its 32 bytes: the light byte becomes the colour
    word in place (`PV_GC`); later midpoints read its low byte. `PV_UVW`
    takes the free word.
- **Split decisions without calls.** The triangle's 3 SXY and 3 z are
  loaded once. Both rewritten tests are exact and symmetric, so the output
  does not change:
  - `|dx| + |dy| <= SPAN` becomes `|dp| <= SPAN && |dq| <= SPAN`, with
    p = x + y and q = x − y (because |a| + |b| = max(|a + b|, |a − b|));
    each half is one unsigned compare;
  - the depth ratio becomes branch-free: `4 zb < 3 za || 4 za < 3 zb`,
    exact because z > 0 past the near clip.
- **Midpoint and projection in one pass.** u, v and the light are averaged
  while the GTE runs the RTPS. Fan triangles start without a push / pop.
- **Clipper.**
  - A first pass computes the plane's distance for every input vertex
    (one small loop per plane) into the vertex's SXY slot, which is free
    until projection.
  - The edge loop keeps its output cursor in a register.
  - The crossing's interpolation is inline, two fields in flight, with
    the division's latency covered.
  - The projection loop is inline.
- **Smaller items:** the transform loop's `v` uses one `andi`, and the
  loop's last delay slot holds the next vertex's first step.

### 16.4 Profiling switch

- **Build option.** `Q1PSX_PROFILE` (CMake; default ON: debug overlay, bench
  breakdown) becomes `R_PROFILE` in the asm. Off, the renderer drops:
  - the node / record / vertex / polygon counters;
  - the face-block and slow-path timers;
  - the slow-polygon counters.
- **Release preset.** The new `release` preset turns the option off, and the
  overlay then shows frame totals only.
- **Bench.** `PROFILE=0 scripts/bench.sh` measures that build; the stage and
  counter columns then read 0.

### 16.5 Physics

The per-node loops of `pcontents` / `fcontents` lost a `b` + `nop`: the next
node's first step sits in the loop branch's delay slot. `NODE_DP`'s axial
path lost a `nop`. The rest of `pmove.s` runs once per tick; its 111 `nop`s
weigh almost nothing.

### 16.6 What the emulator does not show

PCSX-Redux charges roughly a fixed cost per instruction (§21.1 refines
this: it does model the GTE, multiply and divide latencies). On the
hardware:
- a load from main RAM stalls for several cycles (there is no data cache);
- packet stores drain through a 4-entry write buffer;
- the instruction cache is 4 KB, direct-mapped.

So where the two disagreed, the hardware won:
- index bytes are still extracted from one word with ALU operations,
  rather than loaded byte by byte (4 RAM loads);
- the grey expansion stays in ALU operations, rather than a table in RAM.

`r_render` is 3.6 KB and `slow_poly` 3 KB. The walk and the record loops
fit the cache together; a slow-path call evicts part of them.

### 16.7 Results

Frame cycles of the 12 bench views at high, against R6.1:

| | R6.1 | R6.2, profile | R6.2, release |
|---|---|---|---|
| total | 3 471 717 | 2 861 950 (−17.6 %) | 2 749 115 (−20.8 %) |
| start's spawn view (view 0) | 481 422 | 414 463 | 396 005 (−17.7 %) |
| nearest views (1, 3, 11) | | | −24 to −27 % |

Steps, as a share of the total (profile build):

| Step | Saved |
|---|---|
| KUSEG + immediate codes | 0.3 % |
| `draw_records` (inline, texture cache, one test per record, E2 per run) | 4.7 % |
| walk inlining | 3.8 % |
| stack word | 0.9 % |
| slow-path packet words and splits | 6.1 % |
| clipper | 1.5 % |
| RTPS tail | 1.1 % |

The `release` build saves another ~4 %.

`BENCH_MAPS=1 PROFILE=0`: all 128 spawn views of the 32 maps take 1
vblank. e3m4's first view, the heaviest, went from 669 k cycles (2 vblanks)
to 540 k.

### 16.8 Not done (they would cost RAM or change the format)

- **CLUT / tpage baked into each polygon's uv words.** −4 ALU operations per
  quad, but +4 bytes per polygon (~30 KB per map).
- **Marks stored × 2.** −1 instruction per mark; format change.
- **A third face word per node,** making `pop`'s block end uniform: +4
  bytes per node.

## §17 — Map menu and loading bar (2026-10-15)

**The goal.** Pick the map to test from a menu at boot, come back to
it with SELECT + START, tag START `(default)` and the visually heaviest
map `(complex)`, and stop the black screen / freeze during loads.

**Menu** (`engine/main.c`, normal build only; the bench still loads its
map directly).
- **Layout:** the maps found on the disc, in two columns of 16, plus the
  fidelity level.
  - The d-pad picks the map, L1 / R1 the fidelity level.
  - Cross or START loads the map; Circle goes back to the map already
    loaded.
  - The cursor starts on START.
- **Back to the menu:** in game, SELECT + START (both held, one just
  pressed) returns to it, with the current map highlighted.
- **Tags:**
  - `(default)`: START, where Quake begins and the skill is chosen.
  - `(complex)`: the map with the heaviest view in `BENCH_MAPS=1`. That is
    e3m4: 540 k cycles at high, R6.2, where the next ones are 419 k (e1m8)
    and 396 k (start).
  - e3m5 is the biggest map (10 827 polygons, 731 KB), but its views are
    lighter. On an episode-1 disc, the tag falls back to e1m8.

**Loading bar.**
- **CD reads.** `cd_load` takes a poll callback. `CdReadRetry` reads in the
  background, and `CdReadSync(1)` returns the sectors still to come.
  - The callback runs once per frame until the read is done; that call
    also lets CdReadRetry retry a failed read.
  - Without a callback, the read blocks as before (the bench).
- **Progress.** `world_load` reports bytes read over both files (texture
  pages, then the world), plus one step after the VRAM upload and one at
  the end.
- **Screen.** `loading_screen` draws the map, the level, a bar (two TILEs
  in the overlay bucket) and the percentage, then flips.
- **Measured.** In the emulator, about 300 KB/s (double speed): ~3 s for
  e1m1 at high (879 KB), with the bar advancing every frame.

**Verified** headless, with a scripted pad (not committed). The sequence:
menu, load e1m1, SELECT + START, START at ultra, load, SELECT + START, Circle.
Every step worked and there was no CPU exception. The bench's display lists
are unchanged.

## §18 — Analog controls (2026-10-15)

**The goal:**
- the left stick walks forward / back and strafes left / right, and the
  d-pad does the same;
- the right stick controls the camera, inverted up / down "like back in
  the day": push up to look down.

**Reading the pad** (`engine/pad.c`). The BIOS pad driver already returns
the sticks when the pad is in analog mode.
- That means type 7 (DualShock / Dual Analog, red LED) or type 5 (the
  green-LED mode). `pad.lx / ly / rx / ry` are then centred, from −128 to
  127; they are 0 otherwise.
- The BIOS driver cannot switch a pad to analog mode. The player presses
  ANALOG; in an emulator, the pad must be set as an analog one.
- Forcing analog mode needs a custom SPI driver with the config-mode
  commands (PSn00bSDK's `io/pads` example), possibly later.

**Input** (`player_cmd` in `engine/main.c`).
- **Both sticks** go through a radial dead zone of 24, because DualShocks
  rest a little off centre. The rest of the travel is rescaled to
  0..`PM_AXIS` (128), keeping the direction.
- **Left stick:** forward = −ly, strafe = lx. A d-pad direction overrides
  its axis with ±128 (full speed).
- **Right stick:** a quadratic response, fine near the centre, up to 48
  (yaw) / 36 (pitch) binary degrees per tick at full deflection. The pitch
  is inverted.
- **Without sticks:** Square / Circle turn, so a digital pad can still turn.
- **Rates per tick.** Turning and looking now advance per physics tick
  rather than per frame, so their speed does not depend on the frame rate.
- **Overlay.** It shows the raw stick values when the pad is analog.

**Physics** (`pmove.c` spec + `pmove.s`, bit-exact).
- `cmd.fwd / side` are now analog, −128..128. The model is Quake's
  (`wishspeed = |wishvel|`, capped at `sv_maxspeed`):
  - `len = isqrt(f² + s²)`;
  - `wishdir = (fwd·f + right·s) / len` (forward and right are orthogonal
    unit vectors);
  - `wishspeed = maxspeed · min(len, 128) / 128`.
- **A d-pad direction** (±128 on one axis) gives exactly the previous result:
  the bench's scripted path prints the same positions.
- **Diagonals** now use `128/181` instead of the old `2896/4096` (1/√2):
  0.02 % apart.
- **Noclip** moves by `(f · s) >> 7`.

**Verified:**
- **Differential test:** the bench's 6000-tick C-vs-asm test now draws random
  axes (0, ±128 or anywhere), with 0 mismatches under both the dynarec and
  the interpreter.
- **pmsim:** the stress walk on start, e1m1 and e1m3 had 0 ticks in solid
  and 0 stuck segments.
- **Headless test with a scripted analog pad (not committed):**
  - left stick up walks forward at 320 u/s;
  - left stick right strafes right;
  - right stick right turns right at 48 per tick;
  - right stick up looks down;
  - d-pad left strafes left;
  - half deflection settles at 122 u/s;
  - a deflection inside the dead zone does not move the player.

## §19 — Texture warping and cracks (2026-10-15)

**The report:** gaps between some textures, and texture warping severe enough to break the immersion, against the GBA engines
in `reference/`, where there is no warping.

**Why the GBA version has none.** Its renderer ("Mode 5 - quake ...
correct") rasterizes in software:
- it interpolates u/z, v/z and 1/z across each span;
- it divides once per pixel (`fixdiv(1, z)` in its `AFFINE` macro).

The PS1 GPU interpolates u and v linearly (affine) and cannot do that.
The PS1 equivalent is to subdivide polygons until the affine error stays
below a few pixels on screen.

**Measured on the R6.2 renderer** (tools: `wldref`, a perspective-correct
reference; `pktview` with `PKTVIEW_BG=255,0,255`, where uncovered pixels
show up):
- **Affine error.** The screen displacement of the texture between the
  affine and the perspective-correct mapping, sampled over each drawn
  triangle:
  - 5-7 px on average over the bench views;
  - 20-40 % of the drawn area above 4 px;
  - up to ~90 px.

  Only polygons nearer than 43 units were subdivided (span 96 px, depth
  ratio 4/3); everything else is drawn as is. R5.2's texture windows made
  the polygons bigger.
- **Cracks.** 801 uncovered pixels over the 12 views, from three causes:
  1. T-junctions in the cooked data: vertices of one piece lying on the
     edge of another, from the cook's texture-grid cuts and qbsp's own
     leftovers;
  2. the slow path's subdivision splits shared edges that the fast-path
     neighbour does not split;
  3. the slow path re-projects its vertices itself (MVMVA, then RTPS in
     2·I mode), so a shared edge lands up to a pixel away from the fast
     path's RTPT one.

**The plan chosen:**
1. Repair T-junctions in the cook.
2. Subdivide every polygon by an affine-error criterion. The criterion is
   evaluated per edge from its two projected ends, so both neighbours
   decide alike. It runs on a light screen-space path that reuses the
   RTPT results.
3. Put the slow path on the same data.

The threshold follows the fidelity level: low 6 px, medium 4, high 3,
ultra 2.

### 19.1 Cook: T-junctions (step 1)

- **`face_pieces`.** Every face of the world model is first cut into its
  texture-grid pieces, for the whole map.
- **`tj_repair`.** Each piece edge gets, in order, every quantized pool
  position lying on it:
  - "on it" means within half a world unit of the segment, more than half
    a unit from its ends;
  - the search runs on a 64-unit grid;
  - their s/t come from the face's own texture vectors at that position,
    so the pool shares them with the neighbour.
- **Ear clipping** (`ear_clip`). Many pieces already had collinear
  vertices on their boundary (qbsp's own T-junction fixes, the cook's
  cuts). The fan of quads from vertex 0 paired them into zero-area
  triangles, drawing the boundary as one segment over them. A piece whose
  fan would have a zero-area GPU triangle is now ear-clipped:
  - largest ear first;
  - never an ear that would leave the rest flat;
  - pairs of triangles sharing an edge go out as one quad, which the GPU
    draws as exactly those two triangles.

**Result on start at high:**

| | before | after |
|---|---|---|
| T-junctions on piece boundaries | 217 | 0 |
| polygons | 8207 | 8337 (+1.6 %) |
| bench frame cycles | | −0.5 % |

- **Repair counts:** 253 vertices inserted into 203 pieces; 1284 pieces
  ear-clipped.
- **World size:** 541 → 543 KB on average; the largest is e3m5, 738 KB.
- **Visible cracks:** 801 → 758 px over the 12 bench views. Views 3, 4, 8
  and 9 lost theirs.
- **What remains comes from the renderer:**
  - lowering the slow path's threshold removed view 0's line of cracks
    (355 → 68 px);
  - view 10's crack runs along an edge whose two sides diverge toward the
    camera: one side is re-projected by the slow path.
### 19.2 Renderer: error-based subdivision (steps 2-3)

**Specification:** `engine/r_slow.c`. The asm port is `engine/r_slow.s`;
`R_SLOW_C = 1` in `r_defs.inc` runs the C instead, and `dlcheck` compares
the two.

**Which polygons.** `draw_records` flags a face record from its bounding
sphere (centre depth z, radius ρ = 1.5 r):
- bit 0: a vertex may be nearer than `R_SZ_FAST` (96, the clip range);
- bit 1: z < 6r, so an edge may exceed the 500 px span;
- bit 2: (z − ρ)·16 < k16·ρ, so the affine error may exceed T, with
  `k16 = 379 / √T` (`r_set_fidelity`).

A flagged record's polygons each take `CAREFUL`. It is conservative:
every corner at SZ ≥ 96, W + H ≤ 500 px, and (W + H)(zmax − zmin) ≤
2·T2·zmin, with T2 = 2T. A polygon that passes goes out as is, because
none of its edges can split. The others go to `slow_poly`. At most 13 per
view would turn out whole there.

**The rule, per edge, from its two projected ends only.**
- **Split** when L = |dx| + |dy| > 500, or when L·|dz| > T2·(za + zb).
  The second term is the screen displacement of the edge's midpoint
  between affine and perspective mapping, |ds|·|dz| / (2(za + zb)),
  with L bounding |ds|.
- **Midpoint:** the projected 3D midpoint,
  s = (sa za + sb zb) / (za + zb), rounded half up (offset 8192,
  unsigned divide); z = (za + zb) / 2; u, v, light averaged.
- **Why no cracks:** decisions and midpoints depend on the edge alone,
  and shared vertices come from the same RTPT cache values. A fast-path
  neighbour has no splitting edge, so both sides of every edge agree.
- **Limits:** off-screen triangles are culled; depth ≤ 8.
- **Clip path:** corners the GTE could not project (SZ < 96, or SXY
  saturated) are clipped against the near plane only. Crossings are
  interpolated from a canonical end. The fan is then subdivided the same
  way.
- **Quads** go straight to their two GPU halves.

**The asm kernel.**
- **Pool:** 512 entries of 32 B: SXY, z, z·T2, u, v, edge key, grey
  colour word, uv word.
- **Corners** come from the record's vertex cache, colour word included.
- **Midpoint table:** a direct-mapped table of pool indices. A slot is
  believed only if its index is below the pool count and that entry
  carries the edge's key, so it is never cleared. A collision costs a
  duplicate of the same midpoint, never a wrong one.
- **Triangle stack:** one word per triangle (3 × 9-bit indices, depth),
  ending on a negative marker.
- **Children:** pushed through a jump table on the split mask, in the C's
  order.
- **Per node:**
  - 12 loads;
  - an off-screen test on sign bits (19 instructions);
  - three split tests of ~15 instructions each.
- **Midpoint:** two divides, overlapped with the u, v and light work.

**PCSX-Redux cost model** (measured with tight loops):
- 2 cycles per instruction;
- a `mult` read straight away costs 6 more;
- a `div` costs 37 more (the R3000's 36).

These match the hardware's multiplier and divider latencies. RAM wait
states and the I-cache are not modelled.

**Alternatives studied, then dropped.** A scratch simulator ran over the
cooked geometry and every view of start, e1m1 and e3m4 (nodes, outputs,
residual error):

| Alternative | Effect on nodes | Why dropped |
|---|---|---|
| Screen midpoint | +25-30 % | |
| Geometric-mean or min(3 za) depth | +6-10 % | |
| Euclidean length instead of L1 | −17-20 % | Exactly what raising T by 20 % gives |
| Quads as quads (strips of GT4) | −3-6 % | Not worth the code |
| Receding quads zipped from their two subdivided side edges | −10-18 % | Works for only 30 % of quads |
| Skipping the off-screen test under a fully visible parent | | Only 11-30 % of nodes qualify |

**Verification.**
- Display lists asm = C, word for word, at high and low (references
  `c2hi`, `c2lo`).
- Interpreter hashes = C.
- Physics diffs: 0 mismatches.
- Uncovered pixels on the 12 start views at high (`PKTVIEW_BG`): none
  outside the undrawn sky. This was 801 before §19.1, and 758 after it.
  The e1m1 views 0-7, a wall at arm's length included, also show none.
- Residual affine error (simulated, start, on-screen polygons):

| Threshold T | 6 | 4 | 3 | 2 |
|---|---|---|---|---|
| Mean error | 1.4 px | 1.0 px | 0.77 px | 0.52 px |

  This was 5-7 px on average, and up to ~90 px, before.

### 19.3 Cost, and the open question of 60 fps

Release build (`PROFILE=0`); "1 vbl" means the frame fits one NTSC frame
(564 k cycles):

| Level (T) | start: views at 1 vbl | start: view 0 | 128 spawn views: at 1 vbl | worst (e3m4 v0) |
|---|---|---|---|---|
| before §19 (high) | 12 / 12 | 396 k | 128 / 128 | 540 k |
| low (6 px) | 11 / 12 | 683 k | 121 / 128 | 842 k |
| medium (4 px) | 7 / 12 | 767 k | 110 / 128 | 1 012 k |
| high (3 px) | 3 / 12 | 888 k | 101 / 128 | 1 159 k |
| ultra (2 px) | 3 / 12 | 1 081 k | 80 / 128 | 1 517 k |

- **Where the cost goes.** Start's view 0 at high spends 106 k on the
  walk, 370 k on the records and 437 k on the subdivision (profile
  build). The records' share includes CAREFUL, about 110 k.
- **Against the first asm port:** the new kernel is 22 % cheaper.
- **What limits it:** the work grows with the scene's near, grazing
  surfaces, whatever the kernel. At high it reaches ~500 output triangles
  per view, about 1 000 cycles each, CAREFUL included (888 k − 396 k for
  492 triangles on view 0).
- **The call made (§19.4):** one threshold for every level, 8 px by
  default, chosen in the menu, with three modes (fixed, dynamic, triangle
  budget).

### 19.4 The threshold in the menu, and three modes (2026-10-16)

**Decision.** One threshold for every fidelity level, 8 px by default (already no marked warping at 6 px in low), chosen in
the boot menu among 1, 2, 4, 6, 8, 10, 12, 14 and 16 px. Three modes,
also in the menu:

| Mode | What sets T for a frame |
|---|---|
| fixed | The chosen threshold. |
| dynamic | The chosen threshold is a floor. A frame whose CPU time (input to the end of the overlay) exceeds 7/8 of an NTSC frame, or which missed a vblank, raises T by a quarter (at least 1 px) for the next frame. Each run of 4 frames below 3/4 of a frame lowers it by 1 px, back to the floor. Cap: 32 px. |
| budget | Quake II PS1's idea, from MVG's video (§ Rendering and Optimizations, 5:55-7:58): near walls split their triangles, and the triangles off screen are clipped "to make room for new triangles". Here T rises and falls the same way against a triangle count per frame: 1000, 1500, 2000, 2500, 3000 or 4000. |

**Why no cracks.**
- T only changes between frames.
- Every edge of a frame is judged with the same T, so both sides of a
  shared edge still decide alike.
- The record bound follows T: `k16 = round(16 √(560 / T))`, a table for
  T = 1..32 in `render.c`.

**Triangle count.**
- `dr_slow_call` adds up the slow path's packet bytes (Δs6), 6 instructions
  per slow polygon, stored once per frame in `r_state.slow_bytes`.
- `r_frame` counts world bytes / 26 for the fast path (POLY_GT4: 52 bytes
  for 2 triangles), plus slow bytes / 40 (POLY_GT3).

**Measured, start at high, release build:**

| Mode | Result |
|---|---|
| Fixed 8 px | 11 of 12 views in one vblank. View 0 takes 650 k; the others ≤ 475 k. Over the 128 spawn views of the 32 maps: 122 in one vblank (101 at 3 px), mean 247 k, worst e3m4 v0 at 737 k (1 159 k at 3 px). |
| Dynamic | View 0 settles at T = 32 px, at 489 k: one vblank. The others stay at 8 px. |
| Budget | View 0 makes ~1 560 triangles at 8 px. The 2000 budget leaves it there; 1500 or less raises T. |

**Bench.** `T=<px>` (default 8) and `TEX4=1` for `scripts/bench.sh`.
- It always renders in the fixed mode.
- The display-list references are now `t8hi` (high, 8 px).
- The C reference gives the same lists at 8 px (`R_SLOW_C = 1`).

## §20 — 4-bit textures and dithering in the menu (2026-10-16)

**The question.** Dithering would let 4-bit textures (16 colours) look
acceptable, and they lighten the GPU:
- half the VRAM bytes per texel;
- twice the texels per texture-cache line (the GPU's 2 KB cache).

The aim: a visual comparison, and both settings
selectable.

**Cook (`qcook --tex4`).**
- **File:** next to each QTX3, a QTX4 (`formats/qtex.h`; disc
  `<MAP>_<N>.TX4`).
- **Layout:** the same tiles at the same texel positions in the same
  pages. The world file's UVs and texture windows stay valid, so the
  `.wld` is unchanged.
- **Colours:** each texture gets its own 16, quantized as follows:
  - weighted median cut on the palette colours its tile uses;
  - then 12 Lloyd iterations;
  - in a 3:4:2 weighted RGB metric;
  - cut to 5 bits like the 8bpp CLUT.
- **VRAM:**
  - pages at x = 320 + 64 p, y = 0;
  - one 16-entry CLUT per miptex at (16 (i mod 60), 480 + i / 60);
  - the debug font stays at x ≥ 960.
- **Engine side.** The loader uploads the rects and patches each `qw_tex`
  CLUT / tpage word with the file's table.
- **Disc:** +8.4 MB (130 MB); start at high: 168 KB against 328 KB.
- **Cook time:** unchanged (0.5 s for 32 maps × 4 levels in parallel).

**Comparison** (start at high, 8 px):
- the 12 bench views rendered by `pktview`, with `PKTVIEW_DITHER=0` for
  dithering off;
- images kept out of the repository: they are drawn from Quake's data.

| Measure | Result |
|---|---|
| Quantization error (cook) | 0.35 levels of 31 per channel, RMS |
| Pixels that differ | ~1 % per view; mean deviation 0.04 levels |
| Worst spot | View 10's multicoloured texture loses a few yellow and purple specks |
| CPU cost | Identical; the display lists differ only in CLUT / tpage words |

Quake's textures each use one or two 16-shade ramps of the palette, so 16
colours per texture nearly cover them.

**Not measured.** The GPU-side gain: PCSX-Redux does not time the GPU,
so this needs real hardware.

**Menu.**
- **Textures 8 / 4 bit:** applied on load. Circle reloads the map in
  place if the setting changed.
- **Dithering on / off:** GP0(E1h) bit 9 through the draw environment,
  from the next frame. Off, the GPU truncates the modulated colour to
  5 bits per channel, so gradients band.

## §21 — The second asm pass (2026-10-16)

**The goal.** Once the warp menu and the 4-bit textures were in:
re-read all of the asm and micro-optimize it, instruction by instruction
(even 0.1 % of performance counts). The asm is written directly, never derived
from C: the C files (`r_slow.c`, `clip.c`, `pmove.c`) stay as test oracles.

**Method.** As in §16, every step was kept only with:
- `scripts/dlcheck.sh check t8hi`: the 12 bench views' display lists are
  identical, word for word (high, T = 8);
- the interpreter's hashes (`EMU_ARGS=-interpreter`) equal to the
  reference's;
- the bench's physics diffs at 0 mismatches.

At the end, the 128 spawn views of the 32 maps (`BENCH_MAPS=1`) were also
compared, by display-list hash, with the asm from before the pass: all
identical. That covers e3m5 and the other deep BSPs, whose walk spills its
stack to RAM. `dlcheck.sh` now does this too (`BENCH_MAPS=1 SECS=3000`).

### 21.1 What PCSX-Redux charges

§16.6 said the emulator charges a fixed cost per instruction. Micro-benchmarks
(a 100-iteration loop per case, timed with the root counter) show more:

| Operation | Cycles |
|---|---|
| Most instructions, loads included | 2 |
| Store (`sw`) | 3 |
| Load in a branch delay slot | 2 (no extra) |
| `mult` result | 6 if \|rs\| < 2^11, 9 if < 2^20, else 13 (rs = the first operand) |
| `div` / `divu` result | 36 |
| MVMVA / RTPS / RTPT result | 8 / 15 / 23 |

A result read early stalls for the rest: 2 cycles fewer per instruction in
between. These are the hardware's latencies (the GTE's, and the R3000's
operand-sized multiply). The instructions cost about twice the hardware's,
so a latency is cheaper to cover in the emulator than on the console. The
rule this pass followed: a latency is filled with work the code needs
anyway. A `nop` costs what the stall would have.

### 21.2 Walk and PVS

- **General planes on the GTE.** A node's n · cam took three `mult` /
  `mflo` pairs that stalled about 23 cycles.
  - It is now one MVMVA on the colour matrix, whose first row `r_render`
    sets to the camera each frame.
  - Exact: sf 0, |n · cam| < 2^28.
  - The camera must fit int16 in vertex units, as the cooked map does.
  - `plane_dist` (the camera-leaf search) does the same.
- **FRUSTUM's MVMVA latencies.** The macro takes up to five of its caller's
  instructions into the two 8-cycle shadows.
  - The node's plane setup goes there; it now lives in a0 / a2 / a3, which
    FRUSTUM leaves alone.
  - The plane index is no longer loaded twice.
- **The push word.** Its node and its block-room test move ahead of
  `wn_push`, into the two slots a GTE register write needs before the
  command.
- **`mark_pvs`.** A remark costs about 5.2 k cycles less on the bench
  views:
  - a byte's leaves go highest first (`bgez` on bit 31), and the loop ends
    at the byte's last set bit;
  - only a row's last byte can name a leaf past the array, so it is masked
    once (`r_state.vis_lastmask`) and no leaf is bound-tested;
  - `world.vis_leaves` caps a malformed `num_visleafs` at the leaf count;
  - a leaf without a row reads one `0xff` byte again for every byte;
  - the mark timer is profile-only.
- **No `nop` left** in the camera-leaf loop, the remark test or the polygon
  loops' entries; FRUSTUM keeps its four (the "fully inside" branches).

### 21.3 Face records

**Counts.** CAREFUL runs on about 70 % of the polygons at T = 8: 554 of
801 in view 0, at about 65 instructions each, some 12 % of the frame. Its
decisions decide GT4 against the slow path's GT3s, so they must not change.
Only its code got faster:
- T2 zmin stays in LO through the x and y ranges, so v1 holds W and the
  scratchpad spill is gone;
- `SP_T4` holds 2 T2, so no shift follows the multiply;
- the x range uses MINMAX4D, with v1 as the second temporary;
- both products are read after enough work to cover their latency.

**No SZ for records CAREFUL will not see.** Only CAREFUL, `dr_nz` and the
slow path read a vertex's SZ. A record whose sphere test leaves a2 = 0 takes
a copy of the transform loop without the SZ stores: the `XFORM` macro, with
sz = 0.

**Smaller items:**
- the triangle loop loads the next polygon's word at its tail;
- `dr_next` goes straight to the record test;
- the RTPS tail reads its next index before its stores.

### 21.4 Slow path

**Counts.** One frame of view 1 at T = 8:

| Item | Count |
|---|---|
| Nodes | 595 (20 % culled off screen) |
| Leaves | 297 |
| Splits | 176, mostly two edges |
| Midpoint lookups | 306, of which 208 new |
| Clipped polygons | 26 |
| Crossings | 24 |
| Projections | 38 |

**`sm_new`.**
- **HI / LO order.** The four products go first, then the two divisions,
  with the u, v and light work in their 36-cycle shadows.
- **z T2 takes no multiply.** The pool holds pz T2 and qz T2, and T2 is
  even, so T2 (zs >> 1) = (pz T2 + qz T2) / 2 − (zs & 1) T, exactly.
- **Its first shift** is done in MIDLOOK, in the table load's slot.

**Leaves.**
- The arena test sits in the third split test's multiply shadow.
- The slow triangle count is now `slow_bytes / 40`: the path emits only
  POLY_GT3s, so it no longer counts them one by one.

**`CORNER`.** Its range test compares with a bound held in a register.

**Clip path.**
- **View entries.** They sit 32 bytes apart, at `VIEW_OFS` from their pool
  entry: one pointer reaches both.
- **The corner loop** fills its MVMVA's shadow.
- **Sutherland-Hodgman** walks the ring with ring[n] = ring[0] (no wrap
  test) and keeps the crossing out of line.
- **`before()`.** A crossing's two ends differ in z, so `r_slow.c`'s
  `before()` always settles on z. The crossing is interpolated from the end
  behind the plane, and the x / y tie-breaks were never reached.
- **`sp_lerp`** chains its six products: each field's loads and the last
  field's store sit in a product's shadow, with four registers in turn.
- **`sp_proj`** computes x H and y H in shifts (H = 160) and takes z T2 as
  the short multiply.

### 21.5 Physics: read, not changed

- **Cost.** `pmove_step` takes ~7.1 k cycles a tick, 5 k of them in three
  traces of ~1.7 k.
- **What could go faster.** The stalls left have no independent work to
  overlap:
  - the divisions by 60;
  - three-term dot products on one multiplier;
  - the trace's fixed cost, which the C ABI imposes (ten saved registers,
    the `trace_t` initialization).
- **The rewrites studied** save under 0.05 % of a frame:
  - reciprocal multiplies for the divisions by 60;
  - GTE dot products.
  That is below the 0.1 % set for this pass, and their range proofs (|velocity
  × 8| < 2^15 after a clip) would be a new risk.

### 21.6 Results

12 bench views, profile build, high, T = 8 (frame cycles):

| Step | Total | Saved |
|---|---|---|
| Before the pass (`t8hi`) | 4 757 602 | |
| 1-3: slow-path entry, walk, midpoints, CAREFUL's min / max | 4 535 148 | −222 k |
| 4-5: `mark_pvs` | 4 535 160 | (a remark: −5.2 k) |
| 6: polygon-loop entries | 4 530 346 | −4.8 k |
| 7: GTE planes, FRUSTUM shadows | 4 477 040 | −53 k |
| 8: `sm_new`, CAREFUL products, leaf tidying | 4 409 300 | −68 k |
| 9: clip path | 4 358 916 | −50 k |
| 10-11b: CAREFUL's spill, slots, x range | 4 350 216 | −8.7 k |
| 12: no SZ when CAREFUL is off | 4 342 011 | −8.2 k |
| 13: last `nop`s | 4 340 164 | −1.8 k |
| **Total** | | **−8.8 %** (worst view 671 k → 617 k) |

**All maps** (`BENCH_MAPS=1 PROFILE=0`, high, 8 px, the 128 spawn views):

| | Before | After |
|---|---|---|
| Views in one vblank | 122 | 124 |
| Mean | 247 k | 225 k |
| Worst | 737 k | 675 k (e3m4 v0) |

Still over one vblank: e3m4 v0, e1m8 v0 (634 k), start v0 (596 k) and
e1m7 v0 (577 k).

### 21.7 Not done (RAM or format: a call to make)

- **Light as a halfword per vertex** (`0xcccc`): +23 KB per map, about
  −1 % in the emulator. On the hardware it may give almost nothing, since
  the expansion hides behind RTPT.
- **CLUT / tpage baked into the polygons' uv words** (§16.8): +4 bytes per
  polygon, about 20-30 KB per map.
  - It saves 2 ORs per polygon.
  - CAREFUL would no longer reload v0 / v1.
- **The `release` preset** (no profiling) is another ~3 % faster than the
  default build, whose overlay counters cost a few instructions per node,
  record and face block.

## §22 — Controls, weapons and the HUD (2026-10-17)

**The goal.** The real controls, every weapon, and the HUD in two
versions: Quake's original (with the viewport revisited, since that bar takes
part of the screen) and one in the style of Quake II on the PlayStation. No
dynamic lights for now.

**Decisions** (asked in one batch, answered 2026-10-17):

| Question | Answer |
|---|---|
| Control mapping | Scheme B: R2 fire, L2 jump, R1 / L1 next / previous weapon, Cross jump, Square fire, Triangle walk, Circle level the view; d-pad = a weapon group (up shotguns and axe, right nailguns, down launchers, left lightning). START the debug overlay, SELECT + START the map menu, SELECT + Cross noclip, SELECT + Square respawn. Digital pads: d-pad walk / turn, L1 / R1 strafe, L2 / R2 look, Square fire, Cross jump, Circle / Triangle next / previous weapon. |
| Where the weapons' logic lives | Native C, permanently: a transcription of weapons.qc, player.qc and combat.qc in fixed point. The QuakeC VM of Phase 7 will run monsters and map entities and route the player's weapon functions here. |
| Skins in VRAM | 8 bits (the exact palette) at half resolution. |
| Inventory at the spawn | Everything, with full ammo (Quake's impulse 9), until items exist. |

### 22.1 The game data: `GAME.DAT` (formats/qgame.h, tools/qcook/game.c)

One file, cooked once from the pak and loaded at boot, holds what every map
needs: the 8 view models, the projectiles (spike, super spike, grenade,
missile, the lightning bolt), the explosion sprite and 112 HUD pictures.

- **Models are face records.** A Quake MDL's triangles are cut into records
  in the world's own format (qw_record, indices, light bytes, triangles with
  a uv word per corner), so the renderer draws them with its record loop and
  nothing else. Every frame is a vertex pool in vertex units (world x 4),
  with the vertex's light-normal index in the pool entry's spare byte. A
  vertex shared by two records is duplicated: records gather adjacent
  triangles (a 24-triangle window, the one sharing the most vertices
  first), which keeps the pools at about 1.4 x the model's vertex count.
- **View models are written nearest-first** as seen from the eye at the
  model origin, the painter's order the record loop's prepend gives.
- **Skins at half resolution**: 2 x 2 texels averaged, then the nearest
  palette colour. The screen never shows a weapon larger than that. The
  lightning bolt is cooked twice as long (one bolt per 60 units instead of
  30: half the instances).
- **Sprites** are models with one record (a quad in the view plane) per
  frame and a flag to draw only the frame's record.
- **VRAM.** The maps keep texture pages 0..5 (start at ultra needs all
  six; qtex.h QTEX_MAP_PAGES), the game data takes pages 6..9, packed by a
  skyline packer (243 K of 262 K texels), plus its own 256-colour CLUT at
  (0, 504) with index 255 transparent (the pictures' transparent colour).
- **Light tables.** 16 tables of 162 bytes: a vertex's brightness factor by
  its normal for a model yawed by k x 22.5 degrees, from Quake's fixed light
  direction (-x in the world); facing it lights a vertex up to 1.5 x.
- **Size:** 332 KB on the disc, 77 KB of geometry in a 160 KB game arena
  (engine/mem.c), 1.2 KB of tables.

### 22.2 The world format: QWV8

A leaf's `contents` becomes a byte, and the byte freed holds the leaf's
light: the average vertex light of its faces, or, for a leaf without lit
faces (open air), the average of the nearest ancestor's subtree. Models in
a leaf are lit with it; the view model with the eye's leaf (at least 24,
"always some light on the gun", as r_main.c).

### 22.3 Entities in the renderer (engine/rlist.c, engine/r_world.s)

- **The render list.** Per frame, C builds items (16 bytes: a particle, or
  a model entity of 64 bytes with its GTE matrix, frame pool, record range,
  skin, light table and light) and chains them per BSP leaf: the leaf's
  `vis` pointer carries a list id in its top byte (the pointer is KUSEG,
  its top byte was free; mark_pvs masks it). Nothing per leaf costs RAM.
- **Painter's order.** The walk draws a leaf's list when it visits the leaf
  (`draw_ents`), after the faces behind it and before the faces in front of
  it: prepended at that point, the items end up between them. A particle is
  one TILE_1 projected with the view matrix; a model goes through
  `draw_model`: the texture run in progress is closed, its GTE matrix,
  pool, skin and face-stamp word are set, its light bytes are written
  (`light_recs`: normal index -> table -> x light >> 7), then the record
  loop runs on its records. The loop is a macro now (`RECLOOP`), instanced
  for the world's face blocks and for a model, so one code path draws
  both. Afterwards the view matrix and the world's pointers come back.
- **The view model** is prepended first of all (before the walk), so it is
  drawn last, on top, like Quake's depth hack. Its matrix comes from the
  frame's own view (`r_set_view` before `game_render`: computed from the
  previous frame's, the weapon lagged a frame behind the camera while
  moving — the "swaying gait" seen on 2026-10-17). It is drawn
  without the warp subdivision (its triangles are small) but with the near
  clip: Quake clips the view model at 5 units (ALIAS_Z_CLIP_PLANE), and the
  weapons' animations pull them back to the eye (the super shotgun's pump
  leaves most of its vertices behind 5 units), so during its draw the slow
  path's near plane is 5 units, the careful test sends polygons there from
  5.2 units, nothing warps. The GTE's divide only holds down to SZ = H / 2
  (13 units at H = 160): the view model is projected with H = 40 and its
  matrix's x and y rows times 4 (the same screen coordinates, down to 3.3
  units), its records' spheres are cooked huge so the 4 x does not cull
  them. The cook drops a view model's triangles that are never seen:
  behind 3 units in every frame, or back-facing (by the vertex normals) in
  every frame — 25-40 % of them.
- **Cost.** The entity hooks add 0.37 % to the world's bench total (4 340 k
  → 4 356 k over the 12 views: the leaf hook, the prologue's stores, the
  careful test's variable bound in the model instance only — the world's
  instance keeps its immediate); the view model 65-120 k cycles on start's
  spawn view (its fast-path triangles plus the clipped ones, 700 cycles
  each through the slow path: the super shotgun's pumped frames have 80 of
  them); a particle about 40 cycles. A lean near clip for the view model
  (no subdivision machinery) would cut that by half.

### 22.4 Hull 0: the point hull on the render BSP (engine/clip.s)

Projectiles and shots trace a point. Quake's hull 0 is the world's own BSP;
the clip section only had hulls 1 and 2 (6-byte clipnodes). Rather than
cook hull 0 as clipnodes (6 bytes per node), the trace walks the render
nodes: 36-byte nodes, pointer children (bit 0: a leaf, whose contents byte
is read), the same plane table. `clip_trace` and `clip_contents` take hull
0; the loop is a macro instanced for both kinds of node (CT_LOOP). The C
reference does the same, and the bench's `clipdiff` now traces hull 0 too:
0 mismatches.

### 22.5 The game (engine/game.c)

A 60 Hz port of the player's side of the game code, in the engine's units
(positions 1/8 unit, velocities units per second, angles binary degrees,
times in ticks):

- **Weapons** (weapons.qc): the axe (a 64-unit trace, sparks on a wall),
  shotguns (6 pellets at 0.04 spread, 14 at 0.14 x 0.08; 2 shells; one
  shell fires as the shotgun), nailguns (nails from alternate sides every
  0.1 s while held, super nails), the grenade launcher (bounce, 1.5
  overbounce, rests on a floor, 2.5 s fuse), the rocket launcher, the
  lightning gun (a 600-unit trace, a bolt every 0.1 s while held, 1 cell
  each). The attack rates, punch angles, ammo use, weapon selection
  (W_ChangeWeapon, the cycles, W_BestWeapon when empty) are Quake's.
- **The weapon animations** (player.qc): the frame chains at 0.1 s
  (shotgun 1-6, axe 1-4 or 5-8 with the hit on the third, nailgun 1-8 and
  lightning 1-4 looping while the button is held and firing each step,
  launchers 1-6).
- **Projectiles** (sv_phys.c SV_Physics_Toss): hull-0 traces each tick,
  gravity for grenades with an exact 1/60 carry, the sky swallows them,
  nails leave sparks (TE_SPIKE), rockets and grenades T_RadiusDamage 120
  and an explosion (the sprite's 6 frames at 0.1 s, 48 particles instead
  of 1024).
- **Damage** (combat.qc): only the player can be hurt for now (by its own
  explosions, at half damage: rocket jumps work). Armor absorbs its percent
  (green 30, yellow 60, red 80), the face shows pain for 0.2 s, the view
  kicks (V_ParseDamage: 0.6 degree per point of damage, over 0.5 s), the
  momentum is dir x damage x 8. At 0 health the view drops to the floor;
  fire respawns after a second.
- **Particles** (r_part.c): a pool of 256, Quake's types and colour ramps
  (fire, explode, explode2, slowgrav), velocities in 1/8 unit per tick x
  256. Rocket and grenade trails spawn one particle per tick (Quake: one
  per 3 units, far too many here). A particle keeps the leaf it was spawned
  in: no per-particle BSP lookup.
- **The view** (view.c): the bob (cl_bob 0.02, a 0.6 s cycle, clamped -7..4
  units), the lean when strafing (2 degrees at 200 u/s), the punch angle
  decaying 10 degrees per second, the damage kick. The weapon follows the
  view with the bob's forward 0.4 and vertical parts and Quake's viewsize
  fudge (2 units with the status bar).
- **The lightning beam** (cl_tent.c): bolts every 60 units along the
  trace, each rolled at random, for 0.2 s after the last shot.
- **Not done:** sounds (Phase 9), dynamic lights (muzzle flash, rocket
  light, explosion light: by choice), water (the lightning's discharge, the
  player's swimming), monsters and items (Phase 8).

### 22.6 The HUD (engine/hud.c)

Both styles draw with gfx.wad's pictures into the overlay chain (vid.c): a
list of 2D packets drawn in order, first of the overlay bucket, opened by a
DR_AREA that restores the whole screen when the 3D viewport is smaller. A
DR_TPAGE goes out when the picture's page changes (the pictures sit in two
pages), then one SPRT per picture.

- **Original** (sbar.c): the inventory bar at rows 192-215 (the owned
  weapons, the selected one lit, the four ammo counts in the small yellow
  digits, keys, powerups, runes) and the status bar at 216-239 (armor,
  face, health, ammo, red digits when low; the score bar when dead). The 3D
  viewport is the 192 rows above (Quake's viewsize 100): the GTE's centre
  moves to (160, 96), the GPU's draw area ends at row 192. The frustum and
  record culling keep the 240-row planes (conservative: what lies in the
  48 rows under the bar is still processed, the GPU clips it); making them
  exact would cost every node 4 instructions in both modes or a second copy
  of the walk.
- **Q2 PSX style**: the whole screen, the face and health at the bottom
  left, armor next to them, the ammo and its icon at the bottom right, keys
  and powerups at the top left, the weapon row at the top right for 2 s
  after a change.

### 22.7 Checking it without a screen

`BENCH_GAME=1 scripts/bench.sh` runs the game at start's spawn: each weapon
selected, fired for 0.4 s, then a frame dumped (view model, projectiles,
effects, HUD), plus one frame per HUD style; `pktview` renders them
(build-bench/views/g*.png) from the map's and GAME.DAT's images, with
sprites, tiles, DR_TPAGE and the draw area supported now; the dump carries
the viewport's height (`V <rows>`). The dumps' hashes under the interpreter
equal the dynarec's.

**Cost** (start's spawn view, high, 8 px, profile build): 617 k cycles
without the game, 680-735 k with the view model and the HUD (the super
shotgun's pump frames the worst); the lightning beam's frame 815 k (10
bolts of 26 triangles plus their sparks). The dynamic warp mode keeps such
frames in one vblank. The bench also walks 48 ticks with the stick a
little off axis, logging the bob, roll and the gun's offset (the "walk"
lines) and dumping frames (`gwalk*.png`): the bob is Quake's (−2.5 .. +4
units at 320 u/s), the roll 0.4 degree at that stick angle. The game
scenario's 15 frames hash the same under the dynarec and the interpreter
(the interpreter's console doubles characters in the `P` lines, so only
the `H` lines compare; `pktview` then reports oversized primitives from the
garbled lines, which are not in the frames).

### 22.8 Fixes after the first play session (2026-10-17)

- **The controls were the digital scheme's.** The pad in use reported
  itself digital (type 4: a DualShock powers up in digital mode until its
  ANALOG button is pressed, and most emulators start it that way too), so
  the digital-pad mapping (L1 / R1 strafe, L2 / R2 look...) was in effect.
  Two changes: `engine/pad.c` switches the pad to analog mode itself at
  boot with the DualShock configuration commands on SIO0 (0x43 enter
  config mode, 0x44 analog on and locked, 0x43 leave), one byte at a time
  with the /ACK pulse between bytes and timeouts for pads that do not
  answer (the BIOS driver cannot send them, so it is stopped while they
  go out; a pad plugged in later gets two more tries, two seconds apart);
  and the digital scheme now keeps scheme B's buttons (R2 / Square fire,
  L2 / Cross jump, R1 / L1 weapons, Circle level the view), the d-pad
  walking and turning, with Triangle held for strafing and looking (Quake's
  +strafe and +klook). The BIOS reads an analog pad's 9 bytes instead of
  5 in its vblank handler, busy-waiting at 250 kbps: about 4 k cycles a
  frame more (the 12-view bench total 4 356 k → 4 360 k); a driver of our
  own, sending the next byte from the /ACK interrupt, would get most of
  that back (the optimization pass).
- **Blue pixels on the guns** (the super shotgun's and nailguns' barrels).
  Two causes, both in the cook (`tools/qcook/game.c`). The skins were
  packed edge to edge in the game's VRAM pages, and the GPU's texture
  coordinates stray a texel past a triangle's edge, so the neighbour's
  texels showed: every skin and sprite sheet now has a 2-texel border of
  its own edge texels (the HUD's pictures are drawn as sprites, texel for
  texel, and need none), and the UVs are clamped to the image. And a Quake
  skin's unused area is a flat blue fill that no triangle samples at full
  resolution, but which the 2 × 2 reduction averaged into the edge texels:
  the reduction now only averages the texels inside a triangle's texture
  coordinates (rasterized with a texel of margin), and the blocks outside
  take their used neighbours' colours. The 4 pages overflowed with the
  borders: the projectiles' skins (the rocket's is 288 × 196 for a model
  24 units long) go in at a quarter of their resolution (`skin_div` per
  model; the view models stay at a half) and the packer is MaxRects (best
  short side fit) instead of the skyline, 208 K texels of 256 K placed.
- **The view model's back-face culling** (cook time) is geometric now: a
  triangle is dropped when its plane keeps every eye position on its back
  in every frame (the eye sits in a small box about the model's origin:
  −0.4 × bob forward, bob and the view size's 2 units down), the file's
  winding convention read off the model's vertex normals by majority. The
  vertex-normal test it replaces could keep or drop the wrong triangles
  near silhouettes. A few more triangles survive (the super shotgun's pump
  frame 733 k → 755 k cycles).
- **PCSX-Redux** was updated on this machine (2026-10-17) and its headless
  mode now runs at about real-time speed with the console at ~5 KB/s
  (TESTING.md): the game scenario takes 6 minutes, hence
  `scripts/bench.sh`'s 420 s default for it. The same update times the
  CD drive: the boot's 128 `CdSearchFile` calls (one per map file, each
  parsing the directory again) took 10 s there, as they would on the
  drive. `world_scan_maps` now reads the MAPS directory once
  (`cd_list_dir`: CdOpenDir / CdReadDir) and marks the maps present.
- `scripts/bench.sh` used to run the previous binary when the build
  failed (the build's output went to /dev/null): it aborts now, with the
  build log's tail (`build-bench/build.log`).

## §23 — Sound and music (2026-10-17)

Next: the sound effects and the music. Quake's sound
engine is `snd_dma.c` (channels, spatialization, ambients) over a software
mixer (`snd_mix.c`); the PS1's SPU mixes 24 ADPCM voices itself, so the
port keeps the former and hands the latter to the hardware. The music is
Quake's CD audio, which the PS1 has natively.

### 23.1 The cooked sound bank: `<MAP>.SND` (formats/qsound.h, tools/qcook/sound.c)

- **One bank per map.** Quake precaches, on every map, worldspawn's list
  (world.qc) and the weapons' (W_Precache): 68 sounds, 405 KB as SPU
  ADPCM; the map's ambient entities add up to 170 KB (drip1 alone, 12.5 s,
  is 79 KB); the SPU has 512 KB less its first 4 KB. So the cook picks per
  map: the core (weapons, the player's pain, death, jump and landing, the
  axe, the menu's clicks, talk: 39 sounds, 196 KB) always; the water set
  (drowning, swimming, splashes, 88 KB) when the BSP has water leaves or
  water ambient levels; lava, slime, sky (the wind), the teleporters'
  (50 KB) when it has teleporters, the items' (22 KB) when it has items,
  the boss's; then the ambients its entities spawn with (misc.qc's
  `ambientsound` calls: the ambient_* entities, the torches and flames
  (fire1), triggers.qc's teleporter hum unless SILENT) and the two area
  ambients (water1, wind2). When the total still exceeds the SPU, the
  longest looping ambients are halved in sample rate, largest first, and
  the cook says so (`1 halved` on start, e1m2, e3m5: drip1). e1m2 comes to
  492 KB, e2m6 416 KB, e1m7 402 KB.
- **WAV → ADPCM.** `GetWavinfo`'s rules (snd_mem.c): 8 or 16-bit mono, the
  `cue ` chunk's first point is the loop start, a LIST chunk after it with
  a "mark" purpose gives the loop's length (every id ambient loops from 0
  to its end). The encoder is the SPU's: 16-byte blocks of 28 samples,
  the filter (5) and shift (13) with the least squared error per block,
  predicting from the decoder's own history. A loop start is moved to a
  block boundary by prepending silence (under 2.5 ms), and the loop's last
  block is filled with the loop's first samples so the repeat is seamless
  (the loop runs at most 27 samples long); its last block is flagged end +
  repeat, a one-shot's end only (the voice keys off). The worst signal to
  noise over a bank is 15 dB (tornoff2, power: noisy sounds), the usual
  for the format.
- **The file:** a directory of `qs_sound` (name hash, SPU offset, length,
  pitch = rate × 4096 / 44100, loop flag, length in 60 Hz ticks), the
  static sounds (`qs_static`: origin, sound, volume × 255, attenuation
  × 64), the CD track (worldspawn "sounds"), the names (debugging), then
  the ADPCM. 32 banks, cooked by the "high" job of each map (they do not
  depend on the fidelity level).
- **The leaves' ambient levels** (water, sky: Quake's
  `ambient_level[AMBIENT_WATER / SKY]`) go in the world file, QWV9, 2
  bytes per leaf after the vertex pool (`ofs_ambient`); slime and lava
  have no sound in Quake.

### 23.2 The engine (engine/snd.c, engine/music.c)

- **Loading.** Before the world: `snd_load_bank` reads the bank through
  the level arena, uploads the ADPCM to SPU RAM at 0x1010 by DMA
  (`SpuWrite`) and keeps the directory and the static list in two fixed
  slots of the game arena (7 KB); `world_load` then resets the arena. The
  game's named sounds (S_* in snd.h, 69 names) are resolved by hash once
  per bank; a sound the cook left out of this map is simply silent.
- **Voices.** 0-1 the area ambients, 2-9 the dynamic channels
  (`MAX_DYNAMIC_CHANNELS` 8), 10-23 the static sounds — one voice per
  *distinct* static sound: Quake's `S_Update` sums the volumes of every
  static source of one sound into the first channel playing it ("so we
  don't mix five torches every frame"), and that combined channel is a
  looping voice here (e2m6's 85 torches are one voice). A voice is keyed
  on with ADSR 0x000F / 0x0000 (full at once; off at once at key-off or
  the end flag), volume = Quake's 0..255 × 64, pitch from the bank.
- **`snd_start` is `S_StartSound`:** `SND_PickChannel` (the same entity's
  same channel is always replaced, channel 0 never replaces, a monster
  never takes the player's, else the first to end), `SND_Spatialize`
  (the view entity full volume; otherwise `(1 − dist × atten / 1000) ×
  (1 ± right · dir)` per side, with an integer square root in 1/8 units),
  inaudible starts dropped, the channel's end = now + the sound's ticks
  (a loop never ends), and the same sound started this frame offset by a
  random tenth of a second (in 28-sample blocks).
- **`snd_update`** once per frame, with the camera and the game's tick:
  `S_UpdateAmbientSounds` (the camera leaf's two levels × 0.3, under 8
  silent, fading 100 per second), the ended dynamic channels freed, the
  others respatialized (Quake keeps a channel's origin where it started),
  every static source spatialized and summed into its sound's voice. The
  listener's right vector is AngleVectors' with roll.
- **Events (engine/game.c), as the QC plays them:** the axe swing
  (W_Attack, CHAN_WEAPON), its wall hit (axhit2), the guns (guncock,
  shotgn2, rocket1i, spike2, grenade, sgun1), the lightning's lstart on
  CHAN_AUTO at every W_Attack (every 0.1 s while firing, as in Quake) and
  lhit every 0.6 s (`t_width`); the spikes' tink1 or, one time in five,
  ric1-3 (cl_tent.c TE_SPIKE, entity −1); the grenade's bounce on its own
  entity; r_exp3 at an explosion; the jump (PlayerJump, CHAN_BODY); the
  landing (PlayerPostThink: land, or land2 with 5 damage when the tick's
  starting z velocity was under −650); the pain (player_pain → PainSound:
  not while the weapon animates, one every half second, pain1-6 by
  `rint(random() × 5) + 1`); death1-5, or gib / udeath below −40 health.
  The water, slime and lava variants wait for swimming. The map menu
  clicks (menu1 on a move, menu2 on a choice). ATTN_NORM is 64, Quake's
  1.0 × 64; the player's position is the sound's origin.
- **Music:** `music_play(track)` sets the drive to CD-DA with auto-pause,
  seeks the TOC's track and plays; the auto-pause callback restarts it
  (Quake loops the map's track); `load_level` stops it before the CD
  reads and plays the bank's track after. The bench's disc has no audio
  tracks: `CdGetToc` reports one track and nothing plays.

### 23.3 The disc

`scripts/cook.sh` converts the rerelease's `trackNN.ogg` (`QUAKE_MUSIC`,
found next to pak0 by default; `COOK_MUSIC=0` skips) to 44.1 kHz stereo
WAV with ffmpeg, and CMake lists them as `<track type="audio">` after the
data track (`Q1PSX_MUSIC`, off for the bench preset); the Quake track
number is the disc's (track 1 is the data). The 10 tracks make the image
about 650 MB. The banks are `MAPS/<MAP>.SND`.

### 23.4 Checking it

The game scenario logs every start (`snd e<entity> c<channel> <hash>
v<volume> a<attenuation> L<left> R<right> ch<channel> k<skip>`): the
entity, channel, volumes and the channel picked are Quake's logic on the
game's deterministic events, so the lines must match between the dynarec
and the interpreter, and between runs. The boot log lists the bank
(`snd: \MAPS\START.SND;1 — 71 sounds, 487 KB of SPU RAM, 48 static
sounds, CD track 4`) and the disc's tracks. The listening is done by ear.

## §24 — Dynamic lights (2026-10-17)

After the sound: the dynamic lights. Quake's are
`cl_dlights` (cl_main.c): the muzzle flash (EF_MUZZLEFLASH, set by the
player's firing frames: shot1, nail1 and nail2, light1 and light2,
rocket1 — a light 16 up and 18 forward of the player, radius 200 + rand
& 31, minlight 32, for 0.1 s), a rocket's (EF_ROCKET: radius 200 for the
frame, at the rocket), an explosion's (TE_EXPLOSION: radius 350 for 0.5 s,
decaying 300 a second). `R_PushDlights` marks the surfaces each light's
sphere straddles by descending the BSP (R_MarkLights: front child only
when the light is more than its radius in front of the plane, back child
only when behind, else the node's own surfaces and both children), and
`R_AddDynamicLights` adds light to a marked surface's lightmap texels:
`rad = radius − |plane distance|`, nothing under the light's minlight,
then `rad − dist` per texel, `dist` an octagonal approximation of the
distance in the plane to the light's impact point ("sd + td / 2").
Entities (and the view model) add `radius − distance` of each light to
their light.

### 24.1 On vertex lighting, in the walk (engine/dlight.c, engine/r_world.s)

The world is lit per vertex (the cook samples the lightmap at each
record's vertices into its light bytes), so the lightmap texel becomes
the vertex. Three enumerations were tried before the right one:

- R_MarkLights as is (descend by planes, light the straddled nodes'
  records): 60-90 k cycles for a muzzle flash, 550 k for an explosion —
  a straddled plane runs through the whole map, and its node's face block
  (hundreds of records) had to be scanned for the few near the light.
- By leaves (the light's sphere against the boxes down to the leaves,
  then their marked faces, a record table built at load): 89 k — the
  marks of the leaves near the player are many and shared.
- **In the walk.** The walk already holds, for each node's face block it
  is about to draw, the records that are drawn (stamped) and the node's
  plane. When `r_state.nlights` is set, the block goes through
  `dr_lights` before the record loop (`r_world.s`, one load and branch
  per block otherwise): for each live light, the node's box against the
  light's sphere (a whole block skipped at once), then `rad = radius −
  |distance to the node's plane|`, nothing under minlight, the impact
  point; for each stamped record (sky and liquid excepted) whose sphere
  the light reaches (Chebyshev distance of the centre, less the radius,
  under `radius − minlight`), its light bytes saved once a frame (a bit
  per record, 52-byte entries in a 24 KB buffer of the game arena) and
  each vertex within `rad − minlight` of the impact point gets `rad −
  dist` × 3/8 (a quarter world unit per vertex unit, × 1.5 for the cook's
  light gain), saturating at 255. `dist` comes from a 1024-entry table
  of square roots on the squared distance >> 11 (Quake's octagonal
  "sd + td / 2" approximated the same length). `dl_restore` puts the
  bytes back after `r_frame`, as Quake rebuilds a dlit lightmap each
  frame; `dl_push` lists the live lights for the renderer in vertex
  units. Only drawn records cost anything, and only the blocks the light
  reaches cost more than the box test.
- The routine exists twice: `dl_block` in C (the reference, documented
  arithmetic) and `dl_block_asm` (the one built; `DLIGHT_C=1
  scripts/bench.sh` builds the C instead, and the frames' hashes must
  agree). The first comparison disagreed because of the stub that calls
  either from the walk: it saved the walk's registers at the bottom of
  its stack frame, and the o32 ABI lets a C callee write its incoming
  argument registers to the caller's first 16 bytes — `dl_block` did,
  and the record loop then ran on a corrupted block range (15 % smaller
  frames). The stub keeps those 16 bytes free now.
- The nodes a light reaches are stamped before the frame (`dl_push`: a
  descent from the root, a node's box outside the sphere ending its
  subtree, the coming frame count written in the node's spare halfword),
  and the walk takes the hook only for a stamped node: two instructions
  a block, instead of the box test per block per light that cost 85 k a
  frame when every block went through the hook. Entities: `dl_add_at` sums `radius − distance` (exact, after a
  Manhattan reject) and the game adds it × 1.5 to the leaf's light before
  `rl_model` clamps at 170 (the view model from its 24 minimum, as
  R_DrawViewModel).

### 24.2 The game (engine/game.c)

`pl.muzzleflash` is raised where the QC sets EF_MUZZLEFLASH (anim_start
for the shot and rocket chains, every tick of the nail and lightning
chains) and consumed by `game_render`, which allocates the lights
(CL_AllocDlight: the same key's slot, else a dead one, else the first),
one per rocket in flight with its own key, and calls `dl_push` before the
render list is drawn; `explode_at` allocates the explosion's; `dl_decay`
runs per tick (300 / 60 = 5 units). A new map clears them.

### 24.3 Checking and cost

The game bench's frames carry the muzzle flash (the weapon dumps come
0.4 s into firing, so the shotgun's has passed but the nailguns' and the
lightning's are on) and the rocket's and explosion's lights; its hashes
must agree between the dynarec and the interpreter and between the C
reference and the asm (`HASHONLY=1` runs dump only the hashes: a run
takes a third of the time). The world dumps (`t8hi`) have no game and
stay identical. The bench's `dlight` stage (`dl_push`'s node
stamping and the restore) and the `r_block` stage (the hook runs inside
the record loop) give the cost. The first version (a stub per stamped
block computing every light's plane terms, a stub per stamped record):
a muzzle flash frame 80 k cycles over the same frame unlit (778 k
against 698 k on start's spawn), an explosion with a rocket in flight
570 k over (1 271 k, three vblanks). The second: no per-block stub (gp
holds the node), each stamped record of a reached block tested against
the lights' spheres inline in the loop's spare registers, and a light's
plane terms computed the first time it lights a record of the block and
cached while the block lasts (`dl_blk`): the muzzle flash frame still
80 k over (the inline test runs on every stamped record of a reached
node, and the upper nodes' boxes hold the whole hall), the explosion
600 k. Then R_MarkLights' own rule in the node stamping — a node whose
plane is farther than the radius has no face to light, whatever its
box — so the floor and the near walls' nodes stay and the hall's big
far planes drop out: the muzzle flash 77 k, the explosion 600 k —
what the block gate saved, the C descent stamping the nodes paid (it
visited every node the explosion's sphere reaches, 1 500 of them).
Then the stamping moved into r_render after the PVS marking (a stub
around the C call), so a node out of the PVS ends its subtree, and the
vertex loop fetches the next vertex and the light byte in the
multiplies' HI/LO gaps: the muzzle flash frame 76 k over the unlit one
(the C descent 20 k in `r_walk`: 101 nodes; `r_block` 45 k: 40 records,
245 vertices, the inline tests; the restore 8 k), the explosion with a
rocket in flight 575 k (596 nodes, 263 records, 1 499 vertices for two
lights, 263 restores) — still the next targets: the descent and the
restore in asm, one entry for the stub and the routine, the first
passing light's test not run twice. The world
dumps (`t8hi`) are identical with the gates in place (the 12-view total
4 356 k → 4 376 k: the node stamp's load and compare per block, the
record gate); the game scenario's 15 frames hash the same under the
dynarec and the interpreter, and with the C reference and the asm.

## §25 — The third optimization pass: the whole engine (2026-10-17)

The rule for this pass: whenever cycles saved exceed cycles added, by a single one,
the change goes in. Measured with the game bench's hash-only mode on
start's spawn (`HASHONLY=1 BENCH_GAME=1 scripts/bench.sh 300`, profile
build, high, 8 px, the original HUD: 192 rows), every step keeping the
15 frames' hashes identical to the pass's start (`bench_hash_asm13`),
the C and asm dynamic lights agreeing, the interpreter agreeing with the
dynarec, and the 12 world views identical (`scripts/dlcheck.sh check
t8hi`).

### 25.1 What was done, in order, with its measured gain

Cycles a frame on the shotgun view (773.6 k at the start), the other
views in brackets where they differ.

1. **The dynamic lights' node descent in asm**, inline in r_render after
   the PVS marking (`engine/r_world.s`, after `mark_done`): a stack of
   pending front children on sp (`DSN_STACK`, the tree's depth), the back
   child walked first; and **the restore in asm** (`dl_restore_asm`,
   the padded words back). The C versions stay as the `Q1PSX_DLIGHT_C=1`
   references. −10.8 k (the rocket launcher's explosion −64 k, the HUD
   views −30 k).
2. **One entry for the record hook**: `dr_light_full` saves what the
   loop keeps and falls into `dl_light_record_asm` with the first light
   that reached the record, whose sphere test is not run again; the
   sky / liquid flag test moved before the sphere tests. A first version
   forgot that the hook's loop loads each light's origin into t0-t2 and
   entered after that load: the hashes caught it on the second view.
3. **The light gate after the record's frustum test** (RECLOOP: the
   `bnez $gp` moved past the four plane tests and the affine bits), so a
   culled record is never lit; the hook now keeps a1, a2 (the near and
   affine bits), v0, v1 and reloads a3 and at. With 2: −31 k (the
   muzzle flash lights mostly the floor under the player, out of view).
4. **The sound's spatialization** (`engine/snd.c`): the static sources
   rebuilt at map start into 12-byte entries with their voice, their
   reach (the Manhattan distance past which they are silent, exact
   since it bounds the real one) and `dist_m` (the attenuation as a
   16.16 multiplier: `dist x256 = len x dist_m >> 16`, no division);
   `isqrt_fast` (the dynamic lights' `dl_sqrt` table on the squared
   length shifted by an even count into [2^19, 2^21), one Newton step:
   within ±1) instead of the 16-iteration bit loop; the ambients take
   the renderer's view leaf (found this frame at the quarter unit)
   instead of a second BSP descent. The bench's `snd` lines (every
   start's volumes) stayed identical. 15.9 k → 3.6 k.
5. **The entities**: r_part.c's particle lists (a 16-bit `next` in the
   particle's padding: the active ones walked newest first, as Quake
   draws them; the free list LIFO) instead of a scan of all 256 slots
   twice a frame; the projectile loops bounded by the highest slot in
   use; `q_isqrt64` with a 32-bit path (a quarter of the work, exact);
   the entity matrices by the GTE (`engine/ent_mat.s`: the view's
   rotation times the model's columns by three MVMVAs, the translation
   by a fourth with TR, bit-exact with the C's `>> 12`). `ents`
   14.2 k → 6.4 k (the lightning 60 k → 44 k). The particles' draw
   order changed with the lists (newest first): the frames with
   particles hash differently; their renders differ from the previous
   ones by 2 (the HUD views) to 15 pixels (the explosion) where two
   particles of one depth bucket cover the same pixel (§25.3).
6. **The HUD cached** (`engine/hud.c`, Quake's `sb_updates`): the
   sprites are built only when what they show changes (a 12-word state:
   style, health, armour, ammo, items, weapon, the pain face, the
   weapon row's 2 s) into the last `VID_HUD_BYTES` of each frame
   arena's packet area (`vid_hud_block`: no RAM of its own; the walk's
   end moved before it), tagged with that state; a frame whose block
   shows the state links it (`vid_ovl_append`, at the overlay chain's
   end), one whose other buffer's block shows it copies that chain
   (links rebuilt), else it builds. A block is read by the GPU only
   while its own buffer's list draws, so the buffer being built is
   always free to write. 9.8 k → 0.45 k.
7. **The pad polled by the game, port 1 only** (`engine/pad.c`): the
   BIOS driver polls both ports a byte at a time, waiting for each,
   inside the vblank handler: 13 k cycles a frame on PCSX-Redux (a bench
   with `StartPAD` off: 663 k against 676 k), landing anywhere in the
   renderer (the `r_walk` stage's min / max 15 k apart). The game's
   driver does the same for port 1 in `VSyncCallback`: 10 k, −2.8 k. An
   interrupt-driven poll (a byte per /ACK interrupt, the CPU never
   waiting) was written first: the kernel's interrupt dispatch costs
   2.7 k a byte on PCSX-Redux, 22 k a frame, worse than the BIOS; on the
   real machine, whose serial byte takes ~40 µs, it would be the better
   driver (noted; the synchronous one shipped).
8. **The leaf's list flag in its vis word's sign bit** (`rlist.c
   leaf_list`: the id | 0x80 in the top byte, 127 lists a frame): the
   walk's `srl` + `bnez` per leaf become one `bltz`; draw_ents strips
   the bit. ~1 k.

Tried and reverted: **the static sounds' loop in asm** (the same
arithmetic, the C kept as its reference and checked against it: no
difference) came out 85 cycles a frame slower than the compiler's loop
(the multiplies' HI/LO gaps and the load delays cost the hand-written
version what the compiler schedules around). And, with the lesson, a
**lean clip path for the view model** (slow_poly's clip path without the pool, the stack and the split
tests, the same packets) gained 2 k on the shotgun view and lost 12 k on
the axe (its bail-out to slow_poly for an edge over SPAN came after the
clip work). A bench with the view model off had shown its share of
`r_slow` at 24 k for 32 polygons: slow_poly's clip path is ~750 cycles a
polygon already, the divisions and the GTE work most of it. The world's
own 76 near polygons cost 172 k (their subdivision, inherent). Measure a
stage's share with a disabling bench before writing a rewrite.

Measured and left for later (RAM): the **192-row frustum** when the
status bar is on (the view is 192 rows high, its planes `3z ± 5y`
instead of `3z ± 4y`; `R_YSLOPE` in `engine/r_defs.inc` builds it): −6
to −8 k a frame (1 %: few records lie wholly under row 192), but the
full-screen style needs the 240-row planes, so it costs a second copy
of the walk and the record loop (~4 KB) or a mask per test (~9 k, a
loss). Not built.

### 25.2 The cost table (profile build, start's spawn, cycles a frame)

| view | start of the pass | end | stages at the end |
|---|---|---|---|
| axe (unlit) | 702.7 k | 672.6 k | r_walk 117 k, r_block 357 k, r_slow 187 k, ents 5.0 k, snd 3.6 k, hud 0.45 k |
| shotgun | 773.6 k | 712.8 k | r_walk 118 k, r_block 385 k, r_slow 196 k, ents 6.4 k, dlight 2.6 k |
| super shotgun | 842.3 k | 783.9 k | r_slow 257 k (161 near polygons) |
| nailgun | 779.4 k | 714.6 k | |
| grenade launcher | 764.9 k | 704.6 k | |
| rocket launcher (explosion, two lights) | 1 271 k | 1 128 k | dlight 13 k, ents 25 k |
| lightning | 919.7 k | 850.7 k | ents 44 k (the bolts) |
| HUD original / q2psx | 924.5 k / 930.6 k | 834.2 k / 845.3 k | |

The pad's 10 k is in every figure (the vblank lands inside the frame,
in whichever stage is running: the rocket launcher view's `snd` reads
15 k, 3.6 k of it the sound, the rest the poll of its second vblank).
The world's slow path is the largest fixed cost left (172 k of the
shotgun's 196 k); after it the fast path's 385 k, twice optimized.

### 25.3 Checking

For every step: the hash-only game bench against the pass's start, the
`snd` lines identical, the dlight C reference (`DLIGHT_C=1`) hashing the
same as the asm, the checker build (`DLIGHT_C=2`, no `dlchk` line), the
interpreter (`EMU_ARGS=-interpreter`) the same as the dynarec, the full
dump's renders for the four views whose hashes changed with the
particles' order compared pixel by pixel with the previous renders
(the grenade launcher's identical; the HUD views 2 pixels apart, the
explosion 15: particles of one depth bucket on the same pixel, drawn in
the other order), and `scripts/dlcheck.sh check t8hi` identical.

## §26 — DuckStation: the loads that hung at 99 % (2026-10-17)

The report: levels such as E4M5 and E2M1 hang at 99 % of the loading bar
on DuckStation, load on PCSX-Redux; and will it run on the machine? The
headless bench runs on PCSX-Redux, which is lenient; DuckStation is the
better proxy for the hardware, so it was made to run the bench too.

### 26.1 Running DuckStation headless

The AppImage (0.1-11826) takes no `-settings`; its data live in its XDG
data directory, under the home. A private home keeps the usual settings
untouched: `HOME=<dir>` with a copy of `settings.ini` edited for
`[Logging] LogToConsole = true`, `[BIOS] TTYLogging = true` (the game's
`printf` lines come out as `I/TTY:`), `[GPU] Renderer = Software`,
`[Audio] Backend = Null`, and a symlink to the BIOS directory; then
`DuckStation-x64.AppImage -batch -nogui -earlyconsole -- q1psx.cue`
(Qt needs a real display: no `offscreen` platform in the AppImage; a
window opens). The bench disc carries the music with
`-DQ1PSX_BENCH_MUSIC=1` (the `.cue`, not the `.bin`: the TOC comes from
it). The all-maps bench then loads the 32 maps at the chosen fidelity,
with the music started after each, as the game does.

### 26.2 What was found

1. **The hang: `LoadImage`'s DMA rounding.** PSn00bSDK sends a
   rectangle's data by DMA in units of 16 words and rounds a shorter last
   unit up (its log: "transfer data length (328) is not a multiple of
   16, rounding"), so up to 60 bytes past the data reach the GPU as
   commands. DuckStation logged `Unimplemented GP0 command 0xF8` with a
   FIFO of 4-bit pixel data, then a VRAM-to-VRAM copy (`0x94`) took the
   next words as its coordinates: the command stream is lost and the
   next `DrawSync` never returns — the load hung at E1M7 in the bench,
   the first map whose 4-bit CLUT row is an odd count of CLUTs (32 bytes
   each: 41 CLUTs = 1 312 bytes = 328 words; the page rectangles are
   multiples of 128 bytes). PCSX-Redux ignores stray words. The machine
   would behave like DuckStation. Fixed in the loader (`vram_upload`: a
   rectangle whose size is not a multiple of 64 bytes goes up from a
   zero-padded copy in the level arena, GP0 NOPs after the data, the
   copy released after a `DrawSync`) for the world's textures and the
   game data; and in the cook (rectangles 64-aligned, zeros after them),
   which a recook brings. With the fix, DuckStation loads all 32 maps
   (medium, 4-bit) with the music playing between them.
2. **The TOC.** `CdGetToc` right after `CdInit` returned 0 tracks on
   DuckStation (the drive is not ready yet; PCSX-Redux answers at once):
   no music at all there. `music_init` retries for up to 3 s (a quarter
   second apart): 11 tracks on the second try.
3. **The track index.** `CdGetToc` fills `toc[0]` with track 1;
   `music_play(track)` used `toc[track]`: one track off, and the last
   track (E1M5's and E4M5's 11) read past the table, a zeroed entry:
   `CdlSetloc` 00:00:00. Fixed (`toc[track - 1]`).

4. **The ambients' leaf, a wild read every frame.** `update_ambients`
   took the camera's leaf (`r_state.view_leaf`, and `world_point_leaf`'s
   result before it) as a pointer to index `world.leaves`; the walk keeps
   its node and leaf pointers in KUSEG (`0x00xxxxxx`, render.c
   `r_bind_world`) while the leaf table is a KSEG0 pointer, so the
   difference was garbage and `world.ambient[idx]` a read far outside RAM
   every frame: 0 on PCSX-Redux (silent ambients), an "Invalid byte read"
   logged by DuckStation's play bench, a bus error on the machine. Fixed:
   the pointer brought back to KSEG0 (`| 0x80000000`) before the index.
   The play bench (`PLAY=1`: the loading screen, then 30 interactive
   frames per map, the pad read, the HUD, the sound, the music) exists
   for this: the view benches never ran the game's frame.

The two levels reported (E4M5, E2M1) have even CLUT counts at medium:
their hang at 99 % is most likely the fourth fault — the first game
frame after the load, with the water or wind ambient on, read the wild
address and DuckStation stopped there, the last frame on screen being
the bar's (the start map, without area ambients, never took that path).
To be confirmed on the fixed build. Hardware: the synchronous pad poll
(§25.1 item 7) and the CD-DA code are the parts no emulator fully
proves; the rest of this section is the kind of thing DuckStation
catches and PCSX-Redux does not, so the play bench runs there before a
disc goes to the machine (TESTING.md 9.z).

## §27 — The water (2026-10-17)

Next: the water's mechanics, then the progression mechanics (§28,
to come). Ported line by line from id's code, in the engine's split:
the server's physics (sv_user.c, sv_phys.c) in `pmove_step`, the QC's
decisions (client.qc, player.qc, misc.qc) in the game.

### 27.1 The physics (engine/pmove.c, the reference; engine/pmove.s)

`pmove_step` now runs Quake's order within the tick: categorize, the
wish; **SV_ClientThink** — `SV_WaterJump` while FL_WATERJUMP holds
(the push's `movedir` drives the horizontal velocity, the flag up to
2 s or until out of the water), else `SV_WaterMove` at water level 2
or 3 (the wish along the view with its pitch, the stick's full
deflection sv_maxspeed, sinking at 60 units/s when idle, |wishvel|
capped at 320 then x 0.7, the friction 1 − 4/60 on the whole velocity,
then 10 x wishspeed / 60 a tick toward the wish, never past wishspeed
less the speed), else the ground's friction and acceleration or the
air's; **PlayerPreThink** — WaterMove's drag `1 − 0.8 x level / 60`,
CheckWaterJump's push (`cmd.waterjump`, the game found the ledge:
FL_WATERJUMP for 2 s, velocity_z 225), PlayerJump (held: in the water
a kick of 100 / 80 / 50 by its kind every tick, on the ground +270
once per press, FL_JUMPRELEASED stopping the pogo); **SV_Physics_Client**
— `SV_CheckWater` (the contents at the feet, the waist, the eyes, hull
0), gravity only out of the water and not jumping out of it, the move,
the step-up allowed in the water too (SV_WalkMove's rule). The jump
moved after the friction and acceleration, as Quake's PreThink runs
after its think: the jump tick now gets the ground's acceleration.
Fixed point: wishvel x64, the drag and the friction as 0.16 factors
truncating toward zero (a drift of one unit a second does come to a
stop, which a rounded factor would not give), the normalized wish
x4096. pmove_t grew to 80 bytes (level, type, the water jump's ticks,
FL_JUMPRELEASED, movedir), the cmd's jump is held, its `waterjump` is
the tick's push.

The asm port (engine/pmove.s): `check_water`, `water_move`,
`water_jump`, the drag table `pm_water_drag`, the PreThink block; the
scratchpad map moved up by 16 bytes for the larger copy. The bench's
pmove diff starts every other run in the water (7 of 20 on start) and
pushes a water jump and a held jump into each: 6 000 ticks, 0
mismatches (`VIEWS=0 scripts/bench.sh`).

### 27.2 The game (engine/game.c)

`player_water` (client.qc WaterMove): 12 s of air, the gasps, the
drowning 2, 4, .. up to 15 then 10 a second through `pain_finished`,
the lava's 10 x level every 0.2 s and the slime's 4 x level every
second (no envirosuit yet), the sounds in (inh2o, inlava, slimbrn2)
and out (outwater); `check_water_jump` (CheckWaterJump: at level 2, a
wall 24 units ahead at the waist and nothing at the eyes; `movedir` =
−50 x the wall's normal); the swim sound (water1 / water2) once a
second while the jump is held under water, the jump sound only when
the ground jump will happen; the splash (h2ojump) on landing in the
water; the pains (player.qc PainSound: drown1 / drown2 with a bubble
when the eyes are under, lburn1 / lburn2 in slime and lava, every
time, the drowning being paced by WaterMove); the death (h2odeath and
20 bubbles when the eyes are under, unless gibbed); the lightning's
discharge (W_FireLightning: the cells to 0, a radius damage of 35 x
cells on the player). The bubbles (misc.qc) are effects: a sprite
(progs/s_bubble.spr, 2 frames, now in GAME.DAT) rising at 15 units/s,
MOVETYPE_NOCLIP, every half second the random drift and the count
(frame 1 at the 4th think — bubble_split's copy keeps the place, the
original goes, a noclip entity's water level never being set — gone
at the 20th); `death_bubbles` a spawner at the player, one every 0.1 s
while the eyes are under. The view: `game_view_tint` →
`vid_tint` (V_SetContentsColor): a semi-transparent flat over the
viewport at the overlay chain's start, B/2 + F/2 (the GPU's mode 0:
Quake's 128/255 for the water; the slime's and lava's 150/255 shown at
50 % too), water (130, 80, 50), slime and solid (0, 25, 5), lava
(255, 80, 0). Not done: the underwater warp of the screen (Quake's
sine ripple of the view, a GPU-side effect to design), the envirosuit,
the damage and bonus flashes (V_ParseDamage's red, the pickups'), the
water surfaces' turbulent texture.

### 27.3 Checking

The physics diff (27.1). The game bench's `water tick` lines (the
level, the type, the water jump's ticks, the position, the velocity,
the air left per 10 ticks: a swim forward with the jump held from tick
60 to 120 at a point of start's water) and two frames, `gwater` under
the surface (the tint, the gun) and `gwater2`. The walk scenario's
frames (`gwalk*`) changed with the jump's place in the tick.


## §28 — The map's entities, stage 1: brush models and the entity table (2026-10-17)

The order chosen for the progression mechanics (native C
line by line from id's QC, the QuakeC VM dropped): first the cook's brush
models and entity table, then triggers, teleporters, changelevel, then
doors, buttons, plats, trains, then items. This stage: every brush model
cooked and drawn, the solid ones clipping the player, the entities
spawned into edicts from a cooked table. Doors, plats and buttons stand
where the map put them until the next stages move them.

### 28.1 The cook: QWVA (formats/qworld.h, tools/qcook/world.c, ents.c, sound.c)

- **Brush models.** Quake's submodels `*1..` (doors, plats, buttons,
  walls, triggers) have their own BSP trees in the same node array; the
  cook only cut and emitted model 0's faces. Now every model's faces are
  cut into pieces (T-junctions repaired within a model: a door's edges
  only take its own vertices, it moves), every model's tree is marked
  (`reach[node] = model + 1`) and its nodes' face blocks are emitted like
  the world's, per node side, into the one face stream — with `face_id`
  0 (the renderer's stamp for a model's records) and no leaf pairs: a
  brush model has no visibility slot, the game decides when it is drawn.
  `qw_bmodel` (40 bytes): the bounds in the physics' units spread by a
  unit (Mod_LoadSubmodels), the hull-0 root (a node index, the loader
  makes it a KUSEG pointer like the children), the hull 1 / 2 clipnode
  roots (the clip section always held all clipnodes), the subtree's
  depth (<= `QW_MAX_BM_DEPTH` 60: the renderer's own stack) and record
  count. Episode 1's maps: 27 to 106 brush models, 146 (e1m1) to 395
  (e1m3) records, depths <= 18. The texture cook packed the world
  model's textures only (a door whose texture the world does not use
  had no faces at all, found by the e1m1 door view's A / B): it packs
  every drawn model's now — the triggers' brushes excepted
  (`ents_drawn_models`: no faces cooked, no textures packed for a
  `trigger_*` model, Quake never shows them). e1m1 at high: 66 tiles in
  5 pages, 276 brush model records.
- **The entity table.** The lump is parsed once (`ents.c`, keys and
  values kept as strings; sound.c and the player start use the same
  parse) and cooked into `qw_ent` records (56 bytes): the class as an
  enum (`QE_*`, 86 classnames; an unknown one keeps its name), the keys
  the id1 maps use (the 24 keys of the 2 paks: spawnflags, model "*N",
  origin in 1/8 units, `angle` and `mangle` in whole degrees as given
  (-1 / -2 for SetMovedir), speed, wait and delay x100, lip, height,
  dmg, health, count, sounds, style) and the strings (target,
  targetname, killtarget, message with Quake's "\n", map, classname) as
  offsets into a deduplicated pool. Left out: worldspawn (its worldtype
  and message go to the header), info_null, lights without a targetname
  (removed at spawn by Quake), the ambient_* sounds (the bank places
  them), the coop and deathmatch starts. 113 to 364 entities a map, 0.3
  to 1.5 KB of strings.
- **The sounds.** The bank adds what the spawn functions precache
  (doors.qc, plats.qc, buttons.qc, triggers.qc, items.qc, misc.qc): by
  class, `sounds` (with the QC's defaults: secret doors 3, plats 2,
  secrets 1) and worldtype (the keys' and key doors' sounds, the latter
  only for doors with a key flag: the QC precaches them for every door).
  The budget's second and third passes when the loops' halving is not
  enough: the sounds above 11 kHz, then the non-core sounds (items,
  doors) biggest first — e1m1 ends at 507 KB with 22 halved.

### 28.2 The engine

- **Edicts (engine/ents.c, ents.h).** A pool in the level arena (the
  map's entities + 64), spawned from the table at the load: the brush
  entities get the model's box (`setmodel`), SOLID_BSP ones go to the
  traces' table (`clip_bmodels`, in spawn order like Quake's area
  lists), illusionaries are drawn and not solid, triggers get their box
  and stay unseen (InitTrigger), point entities their origin; monsters
  wait for 8.1, episode gates for the serverflags. `SV_LinkEdict` finds
  the leaves a box touches (`world_box_leaves`, Quake's
  SV_FindTouchedLeafs, solid leaves left out, 8 kept).
- **Collision (engine/clip.c, clip.s).** `clip_trace` is SV_Move now:
  the world's hull from `ct_core`, then every listed brush model whose
  box the swept box reaches (SV_MoveBounds: the hull's box around the
  sweep, a unit wider), traced in its own space (the hull of the mover's
  size: the point hull walks the model's render nodes) into a scratch
  trace, its end point put back, and taken when it is hit first or the
  sweep starts inside it (SV_ClipToLinks, the startsolid kept). The
  trace's new `ent` (the pad halfword after the normal) names what was
  hit: the next stages' groundentity. `clip_contents` stays the world's
  (Quake's SV_PointContents). The asm mirrors the C: the old body became
  `ct_core` (returning through a scratchpad word, its loops' `bal`s take
  ra), the wrapper keeps the call's arguments and the loop's state in
  the scratchpad past pmove's words (0x1D4-0x24C).
- **Rendering (engine/rlist.c, r_world.s draw_bmodel).** A brush model
  is a render-list entity (`RE_BMODEL`, the r_ent_t grown to 80 bytes
  with the model-space camera): the view's rotation, the model's origin
  as the translation, the map's pool and texture table, the records'
  cooked light kept (no `light_recs`). Drawn, `draw_bmodel` walks the
  model's subtree like the world walk, front to back from the
  model-space camera (SP_CAM and the colour matrix's first row swapped
  for it, the world's kept in `bm_area`): the frustum test on the
  nodes' boxes with the model's matrix, each node's face block on the
  camera's side through draw_model's record loop (entered at
  `de_records`; `dm_exit` tests `SP_BMWALK` and comes back to the walk),
  no PVS. So the node sides sort a model's own faces and cull its back
  ones, as the world's. Its own 64-word stack in RAM; the walk's s1 / s2
  (dead at a leaf) and gp (set before every world block) serve.
- **Where in the painter's order.** The walk draws a leaf's list when
  it visits the leaf, so a model is drawn over everything the walk
  visits after that leaf (the faces of the ancestors it is in front of,
  the farther subtrees) and under what it visited before. `ents_render`
  attaches the model to the leaf of its box that is visible (the PVS
  stamp, the visible set cached per stamp), in front of the eye (its
  box's depth along the view's forward row) and nearest: right whenever
  the model's volume lies in that leaf;
  a model spanning a drawn face's plane can come out over it (a lift's
  shaft, a long bridge) — the price of no span sorting, to be looked at
  with the movers. One frame of latency when the camera changes leaf
  (the PVS stamps are the last frame's), none in place.
- **Not done here, for the next stages:** the dynamic lights do not
  reach brush models (the walk's light hook is the world instance's),
  the brush models' ambient hums (a brush entity's hum needs the brush
  centre: the bank skips them still), the entities' sounds at spawn.

### 28.3 Checking and cost

- The cook's `check:` per map (the brush model table, the roots, the
  entities' strings and classes); 32 maps x 4 levels recooked.
- `VIEWS=0 scripts/bench.sh`: `clipdiff traces 4000 (623 hit, 1962 start
  in solid) mismatches 0`, `pmdiff ticks 6000 mismatches 0` — the asm and
  the C agree with START's 15 solid brush models in every trace.
- The door view (`E1M1.v12.png`): the double door in
  its frame, lit as cooked, sorted against the room; 5 models drawn of
  e1m1's 31, 77 records in the frame against 52 without them. The
  cooked views 4-11 of a map other than START (the leaves with the most
  faces, the near-geometry views) can sit inside geometry: use `VIEW=`.
- `scripts/bench.sh 300`: the 12 world views pixel-identical to the
  references, and the game's frames too: no brush model is in START's
  bench views (its 15 drawn ones are elsewhere in the hall); the water
  frames' references were the renders before pktview blended tiles, they
  are refreshed. `MAP=E1M1 BENCH_MAPS=1 scripts/bench.sh 300` renders
  e1m1's views (`E1M1.v*.png`): its doors and buttons in the big rooms
  (views 4-9) — the spawn's PVS sees none.
- DuckStation's play bench (§26.1, the recipe in the scratchpad's
  `run_ds.sh`): the 32 maps load and play their 30 frames with the
  entities spawned (up to 312 edicts and 52 solid brush models a map),
  0 GP0 errors, 0 invalid reads, 11 music tracks.
- `MAP=E1M1 VIEW=400,576,40,180,0 BENCH_MAPS=1 scripts/bench.sh 300`
  renders a view 168 units in front of e1m1's first double door
  (`E1M1.v12.png`), `NOENTS=1` the same without the entities: the A / B
  of the brush models' drawing. (The view goes to the build as five
  definitions: its first form, one quoted string parsed by `sscanf`,
  came out as yaw 40 on the PS1 — PSn00bSDK's `sscanf` does not take
  "%d %d %d %d %d" — and the camera looked the other way: an evening
  spent proving the walk right. Host-side checks of a cooked world are
  in the scratchpad's `wldvis.py` / `wldwalk.py`: the brush models a
  view's PVS sees, the walk's visited leaves.)
- Costs (PCSX-Redux): a trace 1 825 → 2 646 cycles, a physics tick
  7 744 → 9 818 (the box test over START's 15 solid models on every
  trace: the swept box in registers, the models' world boxes
  precomputed); the entities' stage: measured part by part on e1m1's
  door view (5 models drawn of 31), 10.3 k at first — the scan of every
  drawable edict 1.6 k, the leaf choice 4.1 k, the render-list entries
  4.3 k — then the list of the models the PVS sees kept per stamp (the
  scan goes), the leaves as pointers (no index multiply), a single
  visible leaf taken as is, the depth only otherwise, and a static
  model's matrix copied from the view's (no GTE call): 5.8 k on that
  view, +0.15 k at START's spawn in the game bench (5.04 k → 5.19 k).
  Next: Quake's area nodes (SV_CreateAreaNode) with the movers'
  relinking, so a trace only tests the models of its cells.

## §29 — The map's entities, stages 2 to 4: triggers, movers, the level's end, the items (2026-10-18)

The rest of the mechanics, in the order chosen, native C transcribed from
id's QC (the 1.06 strings from `reference/Quake/qw-qc`): the triggers
and teleporters and the level's end, then the movers, then the items.
The monsters (8.1) are still left out at the spawn; everything here
works without them and leaves their hooks (`th_pain`, `th_die`, the
kill count, `trigger_monsterjump`'s box).

### 29.1 The runtime (engine/ents.c, ents.h)

- **Edicts as the QC sees them.** `edict_t` carries the fields the
  transcribed QC uses (think / think1 / use / touch / blocked / th_die /
  th_pain as C function pointers, nextthink and ltime in ticks, the
  movers' pos1 / pos2 / dest / finaldest, wait / delay / speed / lip /
  dmg / health / count / sounds, target / targetname / killtarget /
  message / map as offsets into the cooked string pool, items, flags);
  the QC's globals `other`, `activator`, `damage_attacker` are
  `ent_other`, `ent_activator`, `ent_damage_attacker`. Strings compare
  by their offsets (the cook's pool is deduplicated).
- **The tick.** `ents_tick` runs after the player's move each tick:
  the player's edict synced from `pl`, its touch pass (SV_TouchLinks:
  every SOLID_TRIGGER edict with a touch whose absbox overlaps the
  player's; an item's box is 15 units wider, FL_ITEM), then the thinks
  and the pushers in edict order over an active bitmap (an edict with
  nothing pending leaves it). SV_Physics_Pusher with its local `ltime`:
  the move happens while `ltime` runs, the think when `nextthink` falls
  inside the tick's movetime, exactly as Quake. SUB_CalcMove is exact
  in ticks (the position at tick n is `from + delta x n / ticks`, the
  last tick lands on the destination) instead of a velocity integrated
  in floats.
- **Pushing the player.** SV_PushMove: a pusher moving into the player
  or carrying it (the player's ground entity, from pmove's `onground =
  1 + the floor's trace ent`) moves it by the same amount through the
  traces (the pusher taken out of them meanwhile); a player left stuck
  puts the pusher back (`ltime` and origin) and calls its `blocked`
  (the doors' crush damage, the plats' reversal).
- **SV_Impact.** pmove reports the last brush model a slide ran into
  (`pmove_t.touched`, the C and the asm both); the game calls its touch
  with `other` = the player: buttons pressed by walking into them,
  doors opened by touching them.
- **Damage on edicts.** `ent_damage` (T_Damage for a shootable door,
  button, trigger or barrel: health, `th_pain` / `th_die`, the quad x4
  from the player), `ents_radius_damage` (T_RadiusDamage with
  CanDamage's traces), `game_damage_player` for what hurts the player
  (trigger_hurt, a crushing door, a trap's spike).
- **The skill.** ED_LoadFromFile's filter: an entity flagged not easy
  (256), not medium (512) or not hard (1024) is not spawned on that
  skill (`game_skill`, 1 until START's skill gates set it for the next
  map, as the `skill` cvar).

### 29.2 Triggers, teleporters, the level's end (engine/triggers.c, level.c)

triggers.qc and misc.qc: trigger_multiple / once / secret (the "You
found a secret area!" message, the count), relay, counter ("There are
more to go...", "Only N more to go...", "Sequence completed!"),
teleport (the fog at both ends, the destination's angles, 300 u/s
forward, `teleport_time` 0.7 s during which the player cannot back up:
sv_user.c's hack on the command), push (the wind sound every 1.5 s),
hurt, setskill, onlyregistered (registered = the disc carries E2M1),
monsterjump (box only), the walls, the bubbles' spawner. client.qc's
level end: trigger_changelevel, execute_changelevel (the intermission
camera from a random info_intermission, the CD's track 3, the HUD
replaced by the stats: "Completed", the time, the secrets and the
kills over Quake's pictures), IntermissionThink (fire or jump after 2
s), ExitIntermission and GotoNextMap. The map change happens outside
the tick (`level_change_wanted`, main.c loads the map and spawns the
player with the parms: SetChangeParms / DecodeLevelParms, the keys and
powerups taken away, the health within 50 .. 100, at least 25 shells;
START with a rune sends the player to info_player_start2). A death
restarts the level with the inventory it was entered with.

**The episodes' ends.** ExitIntermission's texts (SVC_FINALE: e1m7's
for the shareware game and for the registered one, e2m6, e3m6, e4m7,
and the four runes' text) are QuakeC strings of the pak's progs.dat:
`qcook game` walks that function's statements (each `world.model ==
"maps/..."` test, EQ_S then IFNOT, guards a block; each WriteString
argument with a line break in it, a constant qcc copies into OFS_PARM1,
belongs to the innermost block around it; outside every block: the
runes') and writes them to GAME.DAT (`qg_finale`, 2.7 KB). The text
comes out 8 characters a second over the finale picture; the shareware
game's sell screen is not reproduced.

### 29.3 Doors, plats, buttons, trains (engine/movers.c)

doors.qc, plats.qc, buttons.qc, transcribed: linked doors (LinkDoors:
the touching doors of a group open together, the field around them
that opens them), keys ("You need the silver key", "... runekey", "...
keycard" by worldtype), shootable doors, toggles, secret doors (their
two moves), plats and their inside trigger, buttons (touched, shot or
used), trains along their path_corners, misc_teleporttrain (end.bsp's
ball: its teleport.mdl drawn, the alias model's +-16 box; its avelocity
turns nothing under NetQuake's SV_PushMove). The QC's
`frame` (a button's or func_wall's `+a` textures) is kept in the edict:
the renderer does not animate textures yet (`+0..+9`, `+a`). The travel is the 1.06 QC's: `pos2 = pos1 + movedir
x (|movedir . size| - lip)`.

### 29.4 The items (engine/items.c, tools/qcook/items.c)

- **The models.** Each map cooks its own models (formats/qgame.h
  `QIM_*`, the world file's models section `qw_model` + geometry): the
  items' alias models (skins at a quarter of their resolution), the
  ammo and health boxes (the pak's `maps/b_*.bsp`, cut into records over
  their own textures: they tile like the world's), the torches' and
  flames' models (half resolution, fullbright), the fireball, the
  explosive boxes. Only what the map's entities use is cooked. Their
  textures go among the map's (tex_build's extras); when a map's six
  pages overflow (start and e1m5 at ultra), the models' textures are
  squeezed by levels (the boxes' a mip down, then the skins at half, then
  the flames', then the boxes' once more) until they fit: start at ultra
  ends at level 3, e1m5 at level 2, the other levels and maps at 0.
  Identical box textures (the ammo boxes' `rockettop`) are shared.
- **The game.** items.qc: health (rotten 15, normal 25, mega 100 and its
  rot down to 100), armour (30 / 60 / 80 %: 100, 150, 200), ammo
  (shells 20 / 40, nails 25 / 50, rockets 5 / 10, cells 6 / 12, the old
  `item_weapon`), weapons (the ammo they come with, the switch to the
  better one: W_BestWeapon's order), keys (their names and sounds by
  worldtype), the rune (serverflags), the four powerups (30 s each), the
  messages ("You got the shells", "You receive 25 health", "You got the
  Quad Damage"...) on the notify lines, the bonus flash ("bf"). Each item
  drops to the floor 0.2 s after the map starts (PlaceItem: the box
  through hull 1, offset by the hull's mins less the box's; "fell out of
  level" removes it). misc.qc: the flames (animated 10 a second), the
  fireballs (fire_fly, MOVETYPE_TOSS: gravity 800, the brush models only,
  SV_Impact on what they hit, the player hurt through its touch pass),
  the explosive boxes (20 health, 160 of radius damage, the explosion),
  the spike shooters (spikes that hurt the player: 9, 18 for a super
  spike) and their timed variant.
- **Powerups (client.qc CheckPowerups, combat.qc).** The ring, the
  pentagram (T_Damage absorbs nothing: the armour still takes its share
  and the screen still flashes, as in the QC's order, then the health is
  spared, protect3 every 2 s), the quad (x4 on what the player deals, its
  own explosions included; damage3 once a second when firing), the suit
  (no slime burns, lava every second instead of 0.2 s, air kept); the
  warnings 3 s before the end ("Quad Damage is wearing off", ...) with
  their sounds and a bonus flash each second; EF_DIMLIGHT (a 200-unit
  light on the player) under the quad or the pentagram; PlayerDie clears
  the times and the ring.
- **The screen's colour (V_UpdatePalette).** Quake lerps the palette
  toward each shift's colour by its percent: the contents (water, slime,
  lava), the damage (by armour against blood, 3 x the damage's count, up
  to 150, fading 150 a second), the bonus (gold, 50, fading 100 a
  second), the powerup (quad blue, suit green, ring grey, pentagram
  yellow). The GPU can only blend a flat as B + F, B - F or (B + F) / 2:
  the contents keep their 50 % blend; the others are composed on the mid
  grey (128) and drawn as an added and a subtracted flat while the kept
  contrast is above 3/4, else as the half blend whose flat lands the
  grey where Quake's lerp does (`vid_shift`). Exact on the grey,
  approximate on the light and dark ends.
- **The damage report.** Quake accumulates a frame's damage (dmg_take,
  dmg_save) and the client reacts once (V_ParseDamage: the flash, the
  kick from the last inflictor, the pain face); the game does the same
  per tick (`damage_report`), the world's inflictor (lava, falls) kicking
  from the map's origin as Quake's does.
- **The HUD.** The faces of the powerups (Sbar_DrawFace's order), 666 and
  the disc for the pentagram (gfx.wad's `disc`, an aux picture now), the
  inventory's weapons flashing for a second when picked up (cl.item_
  gettime: also at a level's start, as Quake's client starts with no
  items), the armour's picture by its item, the notify lines
  (Con_DrawNotify: the last four printed, 3 s, from the top, word-
  wrapped at 38 columns).

### 29.5 Box hulls (engine/clip.c, clip.s)

A SOLID_BBOX entity without a brush model (the explosive boxes, the
monsters to come) goes into the traces' table with no `qw_bmodel`: its
exact box. The trace builds Quake's box hull for it (SV_InitBoxHull's
six clipnodes on six axial planes, SV_HullForBox's distances: the box
grown by the mover's, `mins - the hull's maxs` to `maxs - the hull's
mins`) and traces it as a clip hull: the C swaps the world's clipnode
and plane tables for the box's around `trace_core`, the asm swaps s1 /
s2 around `ct_core` (one more branch per traced model, the box's
distances written per trace). The player walks into the barrels, the
shots and the nails hit them, the explosions chain.

### 29.6 A trap found on the way

The edict pool must be cleared whole at each spawn: `ent_spawn` takes the
first slot not in use anywhere in the pool, past the map's entities too,
and the level arena's old bytes there (the previous map's data) could read
as live edicts — DuckStation's invalid read in e1m5's play bench, on its
second load. `ents_check` (bench builds) validates every live edict.

### 29.7 Checking

- `ENTS=1 scripts/bench.sh 2400` (the entity bench, extended): the
  first item of each kind on e1m1 and start, seen from beside it and
  walked onto with a new game's kit: what it gives and says (megahealth
  50 -> 150, shells 25 -> 45, nails 25, armour 100 / 30 then 150 / 60,
  the super shotgun and the nailgun switched to with their ammo, the suit
  and the quad), the quad's warning at 27 s and its end at 30, a hurt
  with armour (the red flash, the pain face); the torches' flames on
  start. Then the doors, button, plat, teleporter, secret and
  changelevel scenario of stage 3 unchanged.
- The world views: START's flames are drawn now (18 to 161 pixels per
  view, the torches in sight); the references refreshed after a look.

## §30 — The game loop decoupled from the frame (2026-10-18)

The question of 2026-10-17: does a slow frame slow the game or its
controls, as Quake keeps them apart? Before this step the game's time
was already the vblanks' (a frame ran as many 1/60 s ticks as vblanks it
took, 1 to 4), but the pad was read once per frame: at 30 fps every pair
of ticks got the same command, and a press shorter than a frame could
be missed.

- **One poll per tick.** The pad driver polls the pad in the vblank
  callback (§25); each poll is now kept in a ring of 8 snapshots (the
  buttons, the sticks, the type; written by the interrupt, read by the
  frame, the reader 2 slots behind the writer so a slot is never read
  while written). `pad_frame` hands the frame the polls since the last
  one, oldest first: `player_update` runs one game tick per poll, each
  with the command built from that poll (`player_cmd`: the presses are
  that poll's edges, the turning one tick's worth). At 60 fps a frame
  takes one poll and runs one tick, as before; at 30 fps two polls, two
  ticks, two different commands. Beyond 4 polls (a load), the older ones
  are dropped, their presses carried into the first kept one.
- **The menus** read the frame's merged state (`pad_read`: held now,
  pressed = any press since the last read).
- **Why the ticks stay in the frame loop.** Running them inside the
  vblank interrupt would decouple them from the renderer entirely, but
  the physics (pmove.s, clip.s) borrows the renderer's scratchpad areas:
  a tick cannot interrupt a frame. The game's time and input are the
  vblank's; only the moment the ticks run is the frame's. Quake's own
  split is the same in spirit: the client samples a usercmd per frame
  and the server runs the physics at the frame's rate; here both come at
  60 Hz whatever the picture does.

## §31 — Quake's menu and the release editions (2026-10-18)

The goal: Quake's own menu in the release build only (the
default build keeps its debug map menu and overlay), with a graphics
page holding every rendering setting plus the dynamic lights on / off,
the settings that do nothing in the current mode greyed; and the ultra
fidelity taken out of the release disc into a disc of its own.

- **The editions (CMakePresets.json, CMakeLists.txt).** `release`:
  `Q1PSX_RELEASE` (Quake's menu, no debug overlay, no map menu, no
  SELECT cheats), the renderer's counters off, the maps at low, medium
  and high (`Q1PSX_FIDELITIES`); `release-ultra`: the same with the maps
  at ultra alone (build-release-ultra). The default and bench builds
  carry all four levels. The engine reads the levels from the disc
  (`world_scan_maps`: any `<MAP>_<N>.WLD`, `world_fid_mask`), and the
  settings step through those only.
- **The settings (engine/settings.c).** Moved out of main.c and shared by
  both menus: the fidelity and the texture depth (taken at a map's
  load), dithering, the warp control (fixed / dynamic / budget), the
  threshold, the budget, the HUD style, the dynamic lights (new: when
  off, no light is pushed to the renderer nor added to the models; the
  game still allocates and decays them), the volumes (new: Quake's
  `volume`, 0.7, scales the voices; `bgmvolume`, 1, the SPU's CD input;
  the defaults keep the old mix: the master at full, the CD input at
  0.7, the voices x 0.7).
- **The menu (engine/menu.c).** menu.c transcribed for the pad, with only
  what the port does (2026-10-18: the modes and options it does
  not handle taken out rather than greyed): the main menu (qplaque,
  ttl_main, mainmenu's Single Player and Options rows with the spinning
  dot), the single player menu (sp_menu's New Game row), the new game
  question when a game is under way (SCR_ModalMessage: "Are you sure you
  want to start a new game?", Cross yes, Circle no), the options
  (M_Options_Draw's Reset to defaults, CD Music Volume and Sound Volume
  with their sliders) and, in Video Options' place, Graphics: fidelity
  (only when the disc has more than one level), textures, dithering,
  warp control, threshold, budget, HUD, dynamic lights. The pad stands for the keys:
  the d-pad for the arrows (repeating after half a second, ten a
  second), Cross ENTER, Triangle / Circle ESCAPE, START the menu key.
  The sounds are Quake's (menu1 to move, menu2 to enter, menu3 to
  change). The cursor skips greyed entries.
- **What is greyed, and the threshold.** The budget is the budget mode's
  alone: greyed in the fixed and dynamic modes. The fidelity is greyed on
  the ultra disc (one level). The threshold is not greyed: in the fixed
  mode it is the threshold, and the dynamic and budget modes use it as
  their floor (render.c: T never comes down below it), so it reads
  "Min threshold" there. (The example given put the threshold on the fixed mode alone: this is the one place the menu departs from it, on
  render.c's actual behaviour; to be confirmed.)
- **The menu's colours, broken on the first test (2026-10-18).** The aux
  pictures' CLUTs ran along row 504 from x 256, and the 4-bit textures'
  along rows 480+ up to x 959; the second row's texture pages reach row
  511 from x 320: a map whose sixth page is full (start at high, since the
  items' and the animations' tiles) wrote its texels over the menu's
  CLUTs (the plaque, the bronze text, the main menu in random colours),
  and the game data's own pages lost a row (a line of the inventory bar)
  to the aux CLUTs at boot. Every CLUT now lives under the framebuffers
  (x < 320, rows 480 .. 511: architecture.md §4), and pktview loads the
  game data before the map, the console's order, so the bench's frames
  show what the console does.
- **The fidelity and the textures during a game** take effect at the
  next level's load (the level's entities live in the level arena with
  its world: reloading the world mid-level would respawn them); the
  graphics page says so when they differ from the level's.
- **Quake's flow.** In single player the world stops while a menu is up
  (host.c: "always pause in single player if in console or menus"):
  no tick runs. Quake boots into its demo loop under the menu; with no
  demos to play (they need the monsters), the release build boots on
  START with a new game's kit, paused under the main menu: leaving the
  menu plays START, New Game starts it over. The menu is drawn last,
  over the HUD, with Draw_FadeScreen's darkening.
- **Checking:** `BENCH_GAME=1 scripts/bench.sh` ends with the menu driven
  by key presses and a frame of each page dumped (`mmain`, `msingle`,
  `mnewgame`, `moptions`, `mgraphics`, `mgraphics_dyn`,
  `mgraphics_budget`). The release flow itself runs headless with a
  scripted pad (`-DQ1PSX_PADSCRIPT=ON`: engine/pad.c's `pad_script`
  replaces the polls by presses at given vblanks, an analog pad's
  answers): boot under the menu, Single Player, New Game (START loaded
  again), a walk, the menu again, Options, Graphics (the warp to dynamic,
  the budget skipped greyed, the dynamic lights off, the fidelity
  changed), out, firing; the `menu:` and `settings:` lines trace it. A
  fidelity changed before any game is taken when the menu is left (START
  loaded again at it); during a game, at the next level.

## §32 — The animated textures (2026-10-18)

Quake animates the textures named `+0name` .. `+9name` (a frame every 0.2
s: R_TextureAnimation) and switches an entity whose `frame` is set to the
alternates `+aname` .. `+jname` (a pressed button turns from red to
green, a toggled func_wall). The PS1's faces carry their texture's UVs
baked into their records, so the animation is done in VRAM:

- **The cook (tools/qcook/tex.c, world.c).** A chain (the frames and
  alternates of one name, all of one size; one with a missing frame is
  left still, as Quake refuses it) that a drawn face uses gets every frame
  packed as a tile, and a display tile of its own cut from its first
  frame; every face of the chain draws with the display tile (`remap`).
  The world file (`QWVC`) lists the chains (`qw_anim`): the display tile,
  the frames' and the alternates' tiles (page, texels, texture index for
  the 4-bit set's CLUT) and the brush models drawing a chain that has
  alternates. When a map's six pages overflow, the models' textures are
  squeezed first (§29.4), the animation is left out last: low and medium
  animate every map unsqueezed, high squeezes start and e1m1's models a
  little, ultra squeezes 8 maps and leaves start still. 30 of the 32 maps
  have animated textures (e1m1: the buttons, the planet, the slipgate).
- **The engine (engine/anim.c).** Each frame, before the scene: per chain,
  the frame of the moment (`(tick / 12) % frames`, the alternates while a
  listed brush model's edict has its frame set) and, when it changed, a
  VRAM-to-VRAM move (GP0 80h) of that frame's tile onto the display tile,
  with its 16-colour CLUT in the 4-bit set, at the head of the display
  list (the bucket drawn first), then a texture cache clear (GP0 01h).
  No RAM, a few moves every 0.2 s. The world paused under the menu stops
  them, as Quake's client time does.
- **pktview** copies VRAM too, and the bench's dumped frames copy every
  chain's frame again (`anim_invalidate`): pktview sees no frame but the
  dumped ones. `ENTS=1 scripts/bench.sh` dumps e1m1's first button before
  and after it is pressed (`ee1m1_button0`: red, `ee1m1_button1`: the
  alternate, yellow) and logs the frame changes (`anim:` lines).

## §33 — The second play test: a floor gone, the face, the HUD, the frame rate (2026-10-18)

- **START's floor over the pit to the end** (func_bossgate, *38: a slab
  384 x 416 units, 16 thick) "disappeared". It was drawn, in the wrong
  order: the renderer has no depth buffer, the walk paints back to front,
  and a brush model goes whole into one leaf of the world. Its dark side
  faces, buried under the room's floor, were painted over the floor (a
  black band along the pit's edge), and from other angles the pit's walls,
  painted after the slab, covered it. Two changes:
  - the cook leaves out the faces of the brush models that never move
    (func_wall, func_illusionary, the episode and boss gates) that are
    buried in the world: the space in front of each corner and of the
    centre is solid (67 faces on start, none of which Quake shows: its
    z-sorted spans hide them);
  - a brush model whose box spans several leaves is drawn in the leaf
    holding the point of its box nearest the eye, moved 2 units toward the
    eye: the leaf on the eye's side of its nearest face, so the model comes
    after the world beyond that face (the pit under the slab) and before
    what stands between it and the eye. When that leaf is solid or out of
    the PVS, the old choice (the visible leaf in front nearest the eye).
- **The status bar's face** was upside down: Quake's sb_faces[4], for 100
  health and more, is "face1"; "face5" is the one near death.
- **The original HUD, part by part.** Its rows (192 .. 239) lie below the
  view: no frame clears them, each framebuffer keeps what was last drawn
  there. The inventory bar's ammo counts, weapons and items and the status
  bar's armour, face, health and ammo are each drawn into a framebuffer only
  when what they show differs from what that framebuffer shows (their bar's
  columns behind them, then their pictures), straight into the frame's
  overlay chain. The menu's fade, the intermission, a load, the other HUD
  style invalidate both framebuffers. Quake redrew the whole bar for each
  page after any change (sb_updates); a part is the unit here. The Q2-style
  HUD lies over the view, which is cleared every frame: it is drawn every
  frame from its cached packets as before. The bench's dumped frames draw
  every part (pktview sees only dumped frames).
- **The dead player's bar**: Sbar_SoloScoreboard over the scorebar
  ("Monsters: n / n", "Secrets : n / n", the level's time, its name), as
  Quake shows it, where only the empty scorebar was drawn.
- **A frame rate cap** in Graphics (and the debug menu): 60 (none), 45, 30
  or 15 frames a second. vid_flip shows a frame no sooner than its share of
  vblanks after the last (45: 1, 1, 2 in turn). The game's time and input
  stay the vblanks' (§30); the dynamic warp measures a frame against its
  share (a capped 30 fps frame is not "over budget" for taking two
  vblanks).
- **The tick rate** (the question: would 32 ticks a second, or 20 as
  Quake's network rate, gain much?). Measured at start's spawn
  (`BENCH_GAME=1`, `bench tick` lines): a tick costs 21 k cycles standing,
  21 k walking, 48 k firing the nailgun (its nails' traces and
  particles): 4 to 9 % of a 60 fps frame. At 32 ticks a second a 60 fps
  frame would save about half of that, 2 to 4 %; the renderer is the other
  90 %. Against that: the physics (pmove.s) and every timing of the game
  are built on 1/60 s ticks, and a camera moved 32 times a second needs
  interpolating to stay smooth at 60 fps (a tick of lag). Not done at
  first ("too little for the cost"); the rule is that every cycle
  counts: done the next day, §35. (The bench's worst tick, 270 k cycles,
  is the bench's own `centerprint:` line written through the BIOS's TTY
  when the walk crosses the skill hall's trigger, §34: the release build
  prints nothing there.)
- **pkt** in the bench lines: the most packet arena bytes a frame used, and
  FULL when a frame's walk ran out of it (its nearest records lost). The
  128 spawn views peak at 64.8 KB of the 94 KB.

## §34 — The fourth optimization pass: measured with a PC sampler (2026-10-18)

After the second play test: still too slow, and no monsters yet.

### 34.1 Tools

- **A PC sampler** (`SAMPLE=1 scripts/bench.sh`, CMake
  `Q1PSX_BENCH_SAMPLE`): root counter 1 interrupts 2000 times a second over
  the bench's measured frames (and the tick and tick-rate runs); the
  handler counts the interrupted address (EPC) by 32-byte block of the
  code; `prof_sample_print` writes every block hit on `samples` lines.
  Symbolized offline with addr2line (and gas listings, `-Wa,-alcmn`, for
  the addresses inside the asm macros).
- **What a sample means on PCSX-Redux**: the emulator takes interrupts
  only between its compiled blocks (the dynarec ends a block at every
  branch; the interpreter checks at taken branches and jumps). A sample
  at X stands for the time of the block that branched (or fell through)
  to X, not for X: a sample is credited to the basic block ending at a
  branch to its address.
- **The cost model, re-measured** (micro-benchmarks, this build of
  PCSX-Redux): `lw` 2 cycles, `lbu` / `lhu` / `lh` 3, `sw` and `swc2` 3,
  `lwc2` about 2.4, ALU 2; a load from RAM costs what one from the
  scratchpad does (no wait states). Byte and halfword loads cost more than
  word loads here, and the console's RAM reads cost more than the
  emulator says: unpacking a record's corner offsets with four `lbu`
  instead of a `lw` and shifts saved 2 cycles a polygon on the emulator
  and added three RAM reads on the console. Not kept.
- **pktview `PKTVIEW_OCCL=1`**: per dumped frame, the polygons that end
  with no pixel of their own, and why (off the draw area, under 2 x 2 px,
  painted over), and the overdraw.

### 34.2 Where the time goes

- **Start's twelve views** (the release build's first map): the renderer
  is 98 % of the busy time; the slow path (near and big polygons) 43 %,
  most of it in its triangle loop (pop, unpack, off-screen test, three
  edge tests: about 250 cycles a triangle, a fifth of them culled off
  screen). View 0, the spawn, 658 k cycles: two vblanks.
- **The game bench** (start's spawn, each weapon fired): the view model
  costs 29 k to 117 k cycles a frame (4 to 14 %; the super shotgun the
  most), the dynamic lights about 9 % of the samples when a weapon fires.
- **A tick**: 21 k cycles standing or walking, 48 k firing the nailgun;
  the collision traces are 40 to 48 % of it (the hull walk itself),
  the player's touch pass 9 to 13 %.
- **What is drawn for nothing** (start v0): 1253 polygons, 850 with a
  pixel, 580 seen in the end; 263 under 2 x 2 px (far geometry inside
  bigger records), 33 off the draw area; overdraw 1.15 to 1.5. Rejecting
  the records whose sphere projects under 5 px saved 1.4 % on v0, cost
  0.1 to 0.3 % on eight views of ten (the test on every record), and
  changed 119 pixels on v0: not kept (a candidate for a graphics option,
  the rule for anything that changes the picture, if it can be
  made free when off).
- **The bench's worst tick** (270 k to 330 k cycles once a run): 47 of its
  53 samples in the BIOS: the bench's `centerprint:` line printed through
  the TTY when the walk crosses the skill hall's trigger. The release
  build prints nothing there.

### 34.3 Changes (display lists identical: start's views, the game bench)

- **R_MarkLights' one-side descent.** The dynamic lights' node marking
  descended both children of every node it reached; Quake's descends only
  the light's side of a plane farther than the radius. Here: farther than
  radius + DL_PRUNE (64 vertex units), a margin over dl_sqrt's and the
  roundings' error, so no vertex on the far side could have taken any
  light. -4 k cycles a frame with one light, -10 to -14 k with several.
- **A brush model's top node.** camera_side_leaf (§33) descended from the
  root for each multi-leaf model in sight every frame. At link time the
  model now keeps the deepest node whose region holds every point the
  search may take (its box, 2 units beyond, truncated to units, a unit to
  spare: world_box_topnode, the same integer test as the descent); the
  search starts there. Checked against the root's descent on the 32 maps
  (0 differences): e1m1's views with doors in sight -24.7 k cycles (-3 to
  -23 %).
- **The dynamic lights' squared distance on the GTE**: SQR squares the
  vertex's offset's three terms at once (sf 0, exact) where three mult /
  mflo pairs waited on HI / LO: -1.2 k cycles a frame with one light,
  -19 k with the rocket's explosions.
- **A polygon wholly behind the near plane** (CAREFUL's near exit, the
  farthest corner's SZ under SP_ZNEAR too: the GTE's SZ is 0 behind the
  eye) was sent to the slow path, which projected its corners and drew
  nothing. Now skipped: the view models' backs (clipped at 5 units, as
  Quake does) took that path every frame: -8.8 k to -62.6 k cycles on the
  game bench.
- **A polygon wholly off one side of the screen** (CAREFUL's far exits,
  no corner near) is tested with the slow path's own rule (all x < 0,
  all y < 0, all x >= 320, all y >= 240: its triangles would all be
  culled; a saturated corner lies on the same side as its exact
  projection) and skipped. The sign bits of the packed SXY words carry the
  < 0 tests; `w - (240 << 16)` has the sign of y - 240 and
  `(w << 16) - (320 << 16)` that of x - 320. Of the slow path's calls,
  30 to 40 % drew nothing (7 to 38 a view on start, most of them off
  screen).
- **A node or leaf already inside the frustum** (no clip flag left) went
  on loading its box into the GTE for a test it would not run; the flags
  are now tested first (wn_inside): about -4 k cycles a frame.
- **A world block's records under a node inside the frustum**: the walk
  pushes a node with its clip flags after its box's test; with none left,
  its faces (inside Quake's node bounds, which the cook widens by a unit)
  lie inside every plane, and a record's sphere can be culled by none. The
  record loop's world instance then reads only the sphere's depth and
  skips the four plane tests: -7.7 k cycles on start's spawn view, -6.5 k
  a frame on the game bench.
- **Totals**: start's views -0.4 to -4.2 % (-2.9 % summed), the game
  bench -3.3 to -6.6 % (-5.1 %), e1m1's twelve views -3.2 % (before the
  last two steps). The 128 spawn views of the 32 maps identical to the
  pass's start, 124 of them in one vblank.
- **Checks**: `EMU_ARGS=-interpreter` gives the same hashes (start's views,
  the game bench) and 0 physics mismatches; `DLIGHT_C=1` and `DLIGHT_C=2`
  agree with the asm (they had not compiled since §25's pass left a
  stray brace in dlight.c: fixed).
- **Frames in motion** (the game bench's `bench walk` lines: start's hall
  walked forward and turned for 300 frames, each after its vblanks'
  polls): at 32 ticks a second 244 frames in one vblank, 55 in two, 1 in
  three; the simulation 21.6 k cycles a frame (36.4 k at 60 Hz). The
  profile in motion: the slow path 28 % of the samples (40 % of the busy
  time: walls and floors near the eye), the rest of the renderer 36 %,
  everything else under 4 %. The slow path follows the warp threshold:

| Threshold | Frame on average | 1 vblank | 2 vblanks |
|---|---|---|---|
| 8 px (the default) | 461 k cycles | 244 | 55 |
| 12 px | 392 k | 251 | 48 |
| 16 px | 379 k (-18 %) | 259 | 40 |

  (A picture's trade-off, the player's setting: the defaults left as
  they are.)
- **Outside the renderer** (display lists identical):
  - the UI's sprites and the overlay chain's tags written as whole words
    (setSprt / setXY0 / setUV0 / setWH / setRGB0 wrote bytes and
    halfwords, the tag went through bitfields), and a line of text built
    in one run (`ui_chars`: one tpage check, one arena block chained as
    the calls would chain it): the HUD with a centerprint up 7.8 k -> 5.3 k
    cycles a frame;
  - a sound voice's volume registers written only when they change (every
    voice was set every frame, a multiply, a divide and two SPU writes
    each): -1.4 k cycles a frame standing, -0.5 k walking;
  - the lightning's bolts (one every 60 units, each looking its leaf up
    from the root every frame) descend from the beam box's top node:
    -3.7 k cycles a frame on the lightning view.
- **Draw weapon** (Graphics, the debug menu): Quake's r_drawviewmodel. The
  weapon costs 20 k to 95 k cycles a frame after the steps above (the
  super shotgun the most: its barrels cross the near plane); off, it is
  not drawn. On by default (what changes the picture is the
  player's choice).

## §35 — The tick rate setting (2026-10-18)

The choice: 10, 20 (Quake's network rate), 30, 32 (the default) or 60 game
ticks a second, in the settings.

- **Time stays in 60ths of a second.** Every time of the game (nextthink,
  die, attack_finished, the HUD's timers, every `TICKS()`) is unchanged; a
  tick covers `game_dt` 60ths, the polls since the last one, and only the
  rates act over it:
  - pmove, C and asm (pmdiff 0 mismatches over ticks of 1 and of 2, 3, 6
    and 1 60ths): friction, acceleration and the water's friction (tables
    by dt), gravity, the move and the slide's remainder (x dt), the
    PlayerPreThink drag (a table by dt and water level), the water jump's
    count (-dt);
  - over a tick longer than a 60th, the move adds gravity's midpoint
    (g dt^2 / 2, pm_grav_mid): Quake's step moves at the tick's end speed,
    which lowers a jump as the ticks lengthen (32 units at 10 Hz); with
    it a jump tops out at 50 to 51 units at 32, 30 and 20 Hz, as at 60
    (43 at 10 Hz, where a step of 32 units lifts the player off slopes
    and a jump pressed in the air is lost, as in Quake);
  - SV_Physics_Pusher's move (host_frametime: dt), the projectiles' and
    the tossed items' moves and gravity, the screen flashes', the kick's
    and the punch's decays;
  - the particles and the dynamic lights' decay run each 60th of the tick
    in turn: they move as at 60 Hz (the client's work in Quake).
- **The scheduler** (main.c player_update): each vblank's poll turns the
  view (Quake's client turns each frame); a tick runs when an accumulator
  of `rate` a poll reaches 60 (at 32 Hz: 2, 2, ... 1 polls), with the last
  poll's held buttons and every press since, fire and jump included
  (CL_SendMove's `state & 3`: a tap between two ticks counts). At 60 Hz, a
  tick per poll as before.
- **The camera** is drawn between the last two ticks' views (CL_LerpPoint),
  late by the longest tick less a poll (32 and 30 Hz: one vblank; 20: two;
  10: five), the view model with it; no lerp across a jump of 64 units or
  a spawn (game_view_cut). The view's position is a tick's, its angles a
  poll's (game_view_pos, game_view_angles).
- **Measured** (the game bench's `bench rate` lines: a second standing,
  two walking, a jump held 0.2 s, a second falling, through the scheduler):

| Rate | Walked in 2 s | Jump apex | Cycles a vblank |
|---|---|---|---|
| 60 Hz | 628 units | 50 units | 21.5 k |
| 32 Hz | 626 | 51 | 14.8 k (-31 %) |
| 30 Hz | 626 | 51 | 13.2 k (-38 %) |
| 20 Hz | 628 | 51 | 10.6 k (-51 %) |
| 10 Hz | 636 | 43 | 7.6 k (-64 %) |

  A walking tick of 2 60ths costs about a third more than one of 1 (the
  longer traces); a firing tick saves more (48 k a tick at 60 Hz). The
  60 Hz path's own cost went down (the sticks' dead zone skipped on a
  digital pad); pmove's dt factors cost about 80 cycles a tick.

## §36 — The liquids' light and the view's colour (third play test, 2026-10-18)

Reported: the lava's surface untextured, and the tint when swimming in water the one of lava.

- **The liquids were black.** Quake's turbulent textures (`*` names:
  water, slime, lava, the teleporters) are TEX_SPECIAL: no lightmap. The
  software renderer draws them unlit, the texels as they are
  (D_DrawTurbulent8: no colormap); GLQuake too. The cook lit every face
  from its lightmap (face_light_at), and a face without one is black in a
  lit map: every liquid record carried light 0, the GPU's modulation made
  them black. A liquid's vertices now take 128 at the cook's light gain
  (192 at 1.5: the texture as a wall at Quake's normal light 128 shows
  it). Checked on the cooked worlds (start's 87 liquid records, e1m1's
  285, e1m3's 112, e1m7's 44: all at 192) and on a view over start's lava
  (`MAP=START BENCH_MAPS=1 VIEW=1545,2400,96,90,30`).
- **The leaf light leaves the liquids out.** A model is lit by its leaf's
  average face light (the cook); the liquids counted there as 0 and
  darkened the leaves with water in them. R_LightPoint skips them
  (SURF_DRAWTILED: "no lightmaps"): so does the cook now.
- **The view's colour by contents** follows WinQuake's V_SetContentsColor:
  none in the empty and in the solid, cshift_lava, cshift_slime, and
  cshift_water for any other contents. The port had QuakeWorld's
  (the solid as slime, nothing for the other contents).
- **The water's colour is Quake's.** `cshift_water = { {130,80,50}, 128 }`
  in WinQuake's view.c (shared by the software renderer and GLQuake) and
  in QuakeWorld's: V_UpdatePalette moves every palette colour half-way to
  (130, 80, 50), a brown-orange; the lava's is (255, 80, 0) at 150, the
  slime's (0, 25, 5) at 150. The port draws the water's as a 50 % flat
  over the view (`P 62325082`: B/2 + (130, 80, 50)/2 in the dump), the
  same colour. A blue one was expected: the values were checked in
  the three versions and a comparison sent (one underwater frame of start
  with Quake's three shifts); kept as Quake has it, a decision left open.

## §37 — The draw distance and the flat shading (2026-10-18)

Next: the draw distance and flat shading options, the two graphics options proposed during pass 4 (§34) for gains that change
the picture (the rule: such a gain becomes the player's option).
Both are in Graphics (and the debug map menu), off by default.

- **Nothing while off.** The record loops (RECLOOP `dr`, `de`) are left
  as they were: the options run in copies of them, `dro1` (the draw
  distance), `dro2` (the flat shading), `dro3` (both) and `deo1` (the
  models' draw distance), entered through self-modifying code. render.c
  `r_set_options` writes the first word of `dr_records` / `de_records` as
  a jump to the copy, points the dynamic lights' hook's three returns
  (`dlr_ret0..2`) at it, turns the walk's `jal mark_pvs` into `jal
  mark_pvs_far`, sets the thresholds as `slti` immediates, then calls
  `FlushCache()`; with both off every word is put back. The display lists
  of START's views and of the game bench are identical with the options
  off (the graphics menu's pages aside: two rows more). The record loop's
  second half became a macro (POLYS: the quads and triangles, CAREFUL, the
  slow path's calls), instantiated Gouraud or flat.
- **The draw distance** (off, 2048, 1536, 1024, 768 world units): a
  depth D. The background turns black and so does the fog: a vertex's
  light is multiplied by a factor of its SZ (r_fogtab, by SZ / 64, 128
  down to 0 over the last quarter of D); a record wholly beyond D (z - rho
  >= D, view units) is culled, black anyway. A record reaching into the
  fog (z + rho past 0.75 D) whose centre is in it is drawn flat, its
  average light darkened by the factor at its centre (cheaper than
  Gouraud); one reaching in from before it (a big one) and the models go
  through the fogged transform (XFORM fog: SZ / 64 -> the factor, a mult
  per vertex). The PVS marking (`mark_pvs_far`) leaves out a leaf whose box
  lies farther than 1.61 D + 64 units on an axis (1.61: a frustum
  corner's distance over its depth), marked again once the camera has
  moved 64 units; particles past the fog's middle are not drawn. A dynamic
  light's block keeps its records' own lights (the fogged transform).
- **The flat shading** (off, far, all): a record without a dynamic light
  on its block, beyond 512 units (far) or any (all), is drawn in one
  colour: its average light (the loader puts it in the record's flags,
  bits 2 - 7), times the fog's factor at its centre when the draw
  distance is on. Its vertices are transformed without their lights
  (their colour words take that colour for the slow path, whose triangles
  stay Gouraud); its quads and triangles go out as POLY_FT4 / POLY_FT3
  (40 and 32 bytes instead of 52 and 40).
- **Measured** (PCSX-Redux, `PROFILE=0`, frame cycles, the spawn views;
  scratch scripts over `DRAWDIST=` / `FLAT=` bench builds):

| Setting | START (10 views) | E1M8 | E3M4 | E1M7 |
|---|---|---|---|---|
| draw distance 768 | -1.2 % (v0 637 k -> 550 k) | -3.5 % (v0 -9.1 %, v7 -9.4 %) | -2.9 % (v5 -13.3 %) | -4.7 % (v0 683 k -> 501 k) |
| draw distance 1024 | +0.4 % (v0 -0.5 %) | | | |
| flat, all | -1.2 % (v0 -2.3 %) | -1.4 % | -1.1 % | -1.5 % |
| flat, far (512) | +0.1 % (v0 -1.4 %) | | | |
| 1024 + flat all | -0.1 % | | | |

  The draw distance helps the deep views, the heaviest (START's hall:
  under an NTSC frame at 768; E1M7's arena -27 %); a view whose geometry
  is all near pays its tests and fogged transforms (+0.3 to +2.4 %), with
  room to spare. In Quake's rooms little lies past 1024 units: 2048 and
  1536 change little either way. Most of a heavy frame is near geometry:
  the slow path is about 40 % of the busy time (E1M8's profile), which
  the warp threshold, not these options, reduces (8 -> 16 px: -18 %).
  First versions cost more than they saved (a fog over the last half of
  D, a bit per option in a2: +3 % at 1024, the flat shading of far
  records only: +0.6 %); the depths are now kept in registers from the
  join to the decisions, the fog is the last quarter, and records in it
  go flat.
- **Checked**: DuckStation, every map played with the draw distance at
  768 and the flat shading on (`PLAY=1 BENCH_MAPS=1`): 32/32, no GP0
  error, no invalid read. Its frames against PCSX-Redux's on the same
  binary: with the options off, the packet counts of the 128 views are
  equal and 97 hashes differ (the animated textures and the 30 played
  frames follow each emulator's timing); with them on, 9 views' counts
  differ too, by whole particles (3 words) and model polygons on maps
  with moving things (E1M4, E3M3, E3M7, E4M5, END): the particles' cut
  and the culling depend on positions the timing changes. The patched
  loops run alike on both (flat packets are 3 words shorter: the counts
  would show a copy not entered).

## §38 — The 2D weapons (2026-10-18)

Next, an optimization to choose in the menu: the guns as 2D sprites, a flat picture of the 3D
render being much lighter to animate than the 3D render. Graphics' "Draw weapon" (and the debug map menu's) is now
off / 3D / 2D, 3D by default.

- **The cook draws the frames** (tools/qcook/game.c, the 2D weapons'
  section): every frame of the eight v_*.mdl as the eye sees it in the
  status bar's viewport (320 x 192, the projection's centre (160, 96),
  H 160: render.c), the gun at rest (at the eye, the viewport's 2 units
  up), drawn as Quake draws an alias model: z-buffered, the triangles
  wound backwards dropped (the winding read off the model, as cook_mdl
  does), clipped at 5 units (ALIAS_Z_CLIP_PLANE), the vertices lit by the
  light table of yaw 0 at a fixed light (QG_WS_L0 = 86: the engine scales
  the colour, light x 128 / 86, up to the 3D gun's bound 170), the full
  skin's texels perspective-correct. Then halved (2 x 2 samples, two
  covered make a texel: the 3D guns' skins are halved too), trimmed with
  a transparent margin, and a weapon's frames quantized together to 255
  colours of 15 bits (median cut; index 0 transparent, the others with
  the STP bit so black stays opaque).
- **Memory.** The question: why the textures fit and the sprites would
  not: the 3D gun is one skin a weapon (halved, in VRAM) and its frames
  are vertex positions (61 KB for the eight); a 2D gun is a picture a
  frame. Over the 60 frames: 333 KB at the screen's resolution (146 of
  them the axe's, whose swings sweep the screen), 94 K texels halved.
  GAME.DAT (`QGM5`) carries them as ready display-list packets (102 KB):
  each frame's texels a chain of packets (GP0(A0h) to the slot, then
  GP0(01h): the texture cache cleared), each weapon's CLUT one packet to
  (0, 507), a row free under the aux CLUTs. The loader makes the links
  absolute; the game arena grew to 224 KB (the overflow, the dynamic
  lights' tables past its end, wrote through a NULL into the BIOS: the
  arena now stops with a message instead).
- **VRAM: one slot** (110 x 98 texels, the largest frame) placed among the
  game's pictures at an even u (a halfword of 8bpp texels).
- **The engine** (game.c `ws_draw`): when the frame shown changes, its
  chain (and on a weapon's change, its CLUT's) is hung as it is at the
  head of the ordering table (OT_SKY: copied before anything draws, in
  the GPU's own order: no race with the frame the GPU is drawing); a
  chain is never hung in two frames in a row, so the GPU never follows a
  link the CPU rewrites. Then one POLY_FT4, twice the texels' size, in a
  bucket between the world and the overlay (OT_WEAPON: under the tint and
  the HUD, clipped to the viewport). The 3D gun's moves on the quad: the
  bob up and ahead (a shift, and a scale about the view's centre at the
  gun's depth at rest, frame 0's vertices' mean), the full-screen
  viewport's 2 units, the kick's and the damage's pitch (the camera's
  pitch over the gun's: a shift), the lean's roll (a turn about the view's
  centre); its light as the 3D gun's. The shading does not follow the
  yaw (Quake's shadedots by the entity's yaw): the frames are shaded at
  yaw 0.
- **Quake's R_DrawViewModel draws no gun under the Ring of Shadows**
  (`cl.items & IT_INVISIBILITY`): neither does the port now, 3D or 2D.
- **Measured** (the game bench, PCSX-Redux, `PROFILE=0`, `VIEWMODEL=2`
  against the 3D gun): the axe -6.2 %, the shotgun -4.2 %, the super
  shotgun -11.6 % (-89 k cycles a frame), the nailguns -3.1 / -5.2 %, the
  launchers -2.0 / -2.1 %, the lightning gun -5.0 %, the HUD views -2.7 %;
  -4.4 % over the bench. The pictures match the 3D gun's (pktview, which
  now reads a packet's 255 words and follows a CPU-to-VRAM copy across
  packets). DuckStation: every map played with the 2D gun, no GP0 error.

