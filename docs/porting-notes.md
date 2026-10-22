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
    before a command) or HI/LO hazard; since §49 also on a load in a
    branch's delay slot that the target's first instruction reads.
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
  because the dynarec hides load-delay mistakes (hazcheck saw only linear
  code then, not branch targets: §49 added them);
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
- **VRAM: one slot** (68 x 66 texels, the largest 2D frame) placed among
  the game's pictures at an even u (a halfword of 8bpp texels).
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
- **The first test** (the gun moving far too much when walking, wrong pixel
  borders, nothing in the release build's menu):
  - the bob was counted twice: V_CalcRefdef raises the view by the bob as
    it raises the gun (`r_refdef.vieworg[2] += cl.viewheight + bob`, the
    port's game_view_pos too), so on screen the gun moves only by its 0.4
    bob ahead; the quad had the whole bob's shift as well (30 to 60 px at
    bob 4). Now the forward scale alone (about +-7 px, as the 3D gun);
  - the quad was drawn over the frame's whole texels: a texel the GPU
    reads past a scaled quad's edge was the slot's leftover from a bigger
    frame. The frame's last column and row (its transparent margin) are
    now left out of the quad;
  - the full-screen HUD (240 rows) shows the gun 24 px lower and 2 units
    lower still: the frames were cut at the status bar's 192 rows. They
    are rendered on 216 rows now (the full screen's last needed row);
  - a frame over 80 texels on a side stays 3D (the axe's four swings,
    126 x 110 at most: the slot would not fit the game's pages, the
    others' largest is 68 x 66);
  - the weapon's CLUT goes to rows 507 and 508 in turn (a CLUT cache keyed
    by the address never keeps the last weapon's);
  - the release build's Graphics page has the rows (the scripted release
    flow, `-DQ1PSX_PADSCRIPT=ON`, now sets the 2D gun, the draw distance
    and the flat shading through it: its `settings:` lines show them): the
    image run was older than them.
- **Measured** (the game bench, PCSX-Redux, `PROFILE=0`, `VIEWMODEL=2`
  against the 3D gun): the axe -6.2 %, the shotgun -4.2 %, the super
  shotgun -11.6 % (-89 k cycles a frame), the nailguns -3.1 / -5.2 %, the
  launchers -2.0 / -2.1 %, the lightning gun -5.0 %, the HUD views -2.7 %;
  -4.4 % over the bench. The pictures match the 3D gun's (pktview, which
  now reads a packet's 255 words and follows a CPU-to-VRAM copy across
  packets). DuckStation: every map played with the 2D gun, no GP0 error.


## §39 — Four quality settings, and what each option costs (2026-10-18)

Next: the fidelity split into textures, models, map geometry and effects, and on every option
a hint of what it gains and costs, subtle and part of the menu.
The one fidelity setting (low .. ultra) only ever changed the textures (and,
through them, about 5 % of the polygons at low and medium); it becomes four
settings, each with its own levels (`formats/qlevels.h`: a level's number is
the same on every axis, 2 = high is the default of each):

| Axis | Low | Medium | High | Ultra |
|---|---|---|---|---|
| Textures | ¼ of the resolution | ½ | native ≤ 128 | native ≤ 256 |
| Map geometry | no T-junction repair | — | Quake's faces, repaired | + a light grid every 128 texels |
| Models | items, torches, the rocket: ≤ 3 units of error, up to half the triangles | ≤ 1.5 units, up to a quarter | Quake's | — |
| Effects | ¼ of the particles | ½ | the port's | ×2 (a 512 pool) |

### 39.1 The format: the world no longer depends on the textures

A world file was cooked per fidelity because its faces are cut on texture
repetitions (a byte of UV: a power-of-two axis every 256 texels of the
level's resolution, another at every repetition) and its polygons carry
their UVs pre-packed, page-relative. Cooking every texture level with every
geometry level would not fit the disc (the CD-DA music takes ~600 MB; four
texture levels by three geometry levels: ~250 MB of worlds). The choice
(2026-10-18): worlds independent of the textures:

- **One layout for every level** (`tools/qcook/tex.c`, `tex_build` with no
  output: the canonical set). The slots sized at the finest texture level
  (ultra, the models' skins as they are, the animations kept). The world's
  faces are cut for it and its UVs written for it, tile-relative (no page
  position); the map's own models' UVs too, from their skin's corner inside
  its border.
- **The texture set maps it** (`QTX6` / `QTX7`, `formats/qtex.h`): besides
  the pages, the map's texture table (`qw_tex[num_slots]`: CLUT, tpage,
  texture window, the 4-bit set's own words), one `qtex_uvmap` per slot
  (`u' = u0 + ((u * su + rnd) >> 15)`: the tile's corner on an axis that
  does not wrap, the level's scale, rounding for a miptex, truncating for a
  skin as the cook divides it) and the animated textures' tiles (`qw_anim`:
  they moved from the world). A level whose pages had no room for the
  animations keeps the slots: a display slot draws its chain's first frame.
- **The loader** (`engine/world.c`): the texture file's tables are moved to
  the bottom of the level arena (the pixels are in VRAM, the rest of the
  file goes), then the world's record stream has its UVs mapped once, the
  map's models file (`<MAP>_M<n>.MDL`, `QMM1`: the items' and flames' models,
  out of the world: per model level) likewise through each model's table.
  Then everything is as before: the renderer reads `world.textures` and
  the records' UVs as it always did.
- **Names on the disc**: `<MAP>_T<n>.TEX` / `.TX4`, `<MAP>_G<n>.WLD`,
  `<MAP>_M<n>.MDL`, `GAME_<n>.DAT` (n the level); the engine reads each
  axis's levels from the directory (`world_level_mask`), the menus step
  through those. The editions (`Q1PSX_TEXTURE_LEVELS`,
  `Q1PSX_GEOMETRY_LEVELS`): release textures low to high and geometry low
  and high, release-ultra both at ultra; every model level on every disc.
- **What it costs, what it saves.** At high every face below 129 texels is
  cut and mapped exactly as before: START's 12 views give identical display
  lists, the game bench's frames too; over the 32 maps, 29 draw the same
  polygons (0 to 59 of ~35 000 UVs a texel off: the textures over 128
  texels, at half resolution at high, mapped from their full size: a
  rounding the 8-bit UVs cannot avoid) and 3 have 6 to 14 more polygons
  (256-wide textures, now cut every 256 of their texels). Low and medium
  textures no longer remove polygons (their bigger cells did: 257 k
  polygons over the maps against 271 k); they keep the rest: a quarter or a
  sixteenth of the texels, faster loads, the GPU's texture cache (the
  console's, not the emulator's). The disc: the maps' cooked data
  (textures, worlds, models) 103 MB for every level of every axis (125 MB
  for the four fidelities); the release disc 747 MB (774), the default
  791 MB (816), release-ultra 722 MB.

### 39.2 The levels

- **Textures**: the resolution, as the fidelity was (`tex_levels[]`,
  `tools/qcook/qcook.c`; the small textures are no longer repeated in their
  tile at low and medium: the world's cuts are the finest level's).
- **Map geometry** (`tools/qcook/world.c`): low leaves the T-junctions
  unrepaired (-2.3 % polygons on start, -3.1 % on e3m4; a hairline crack can
  show where a face's edge meets another's vertex); ultra cuts every cell
  again every 128 texels from its corner (`QL_LIGHT_GRID`: luxel-aligned,
  the UVs still from the cell's corner), so the vertex light follows
  Quake's lightmap closer: +37 % polygons on start (+44 % e1m1), +12 % of
  world RAM. (On the global texel grid instead: +52 %; every 64 texels:
  +120 %, e3m4 1 MB.) A map too big for the arena falls back to a lower
  geometry level (none does: 39.5).
- **Models** (`tools/qcook/decim.c`): half-edge collapses, so no vertex
  moves and every animation frame stays Quake's; a collapse's cost is the
  distance from the kept vertex to the planes the removed one stood on, in
  five dimensions (the position and the skin coordinates, Garland and
  Heckbert's quadrics with Hoppe's attributes: a collapse that slides the
  texture costs as one that bends the surface), averaged over every frame;
  the seam moves only along itself, an open edge only along itself (planes
  across it hold the outline), no triangle turns over in any frame. Low: up
  to half the triangles, no error over 3 units; medium: a quarter, 1.5.
  Quake's models are lean: over the 32 maps' items, -8.7 % of their
  triangles at low, -4.5 % at medium (the armour, the biggest key, none;
  the torch 122 to 98 / 106); in the game data only the rocket (58: 54 /
  56), the spikes and the grenade under the 24 triangles a model needs to
  be reduced. The records then gather what is left otherwise, and a few
  models carry a vertex slot or three more than Quake's (the floor
  shotgun at low: 82 triangles, 55 slots, against 84 and 52); a slot costs
  about 35 cycles in the record loop's RTPT, a drawn triangle more than
  twice that. **The view models stay Quake's at every level**: measured on
  the screen (an error of a unit at 8 units from the eye is 20 px), they
  lose nothing under 4 px; at 12 px, for 1 to 3 % of a frame, the super
  shotgun's barrels lost their outline and a few triangles turned from the
  eye. The game data comes per model level (`GAME_<n>.DAT`, `QGM6`: §38's
  2D weapons took `QGM5` the same day, the merge bumped it), its geometry
  block sized for the largest level in the game arena, so another level's
  models replace the loaded ones before a map's load (`gdata_set_models`).
- **Effects** (`engine/game.c`, at once): the particles each effect makes
  in eighths of the port's (low 2, medium 4, high 8, ultra 16: an
  explosion 12 / 24 / 48 / 96 of Quake's 1024), a rocket's or grenade's
  trail every 4th / 2nd tick or twice a tick, the teleport splash's grid
  thinned or (ultra) at Quake's own height spacing, the pool 256 or 512
  (ultra: 10 KB more of BSS; the game arena, full since the 2D guns, has
  no room for it). High is the port's exactly (the same `q_rand`
  sequence: the game bench's frames identical).

### 39.3 The hints

Every row of the Graphics page says what its value does: two lines under
the rows, centred, in Quake's bronze at a dimmer shade (88 against the
rows' 128: subtle, the menu's own font and colour), the speed first, then
the picture; "+" a gain, "-" a cost, neither for a default
(`settings_hint`, `engine/settings.c`: one table both menus read, at most
38 characters a line). The numbers are the port's measures (39.4, §34,
§35, §37, §38: the 2D gun -4.4 % over the game bench, up to -11.6 %).
The levels that wait for a load show one white line under them: "Takes
effect at the next level". The debug map menu shows the same two lines;
its maps went to three columns of 11 so that its 16 options and the hint
fit above its help line (its 13 options ran into it).

### 39.4 Measured

Frame cycles on PCSX-Redux (`PROFILE=0`, the release's renderer), each
level against high on the same axis, the other axes at high; START's 12
views, the 128 spawn views (`BENCH_MAPS=1`, four a map) and the game
bench's 12 scenarios (START's spawn, each weapon fired, the HUD, a walk at
60 and 32 Hz):

| Level | Scenarios | Total | Per scenario | Heaviest frame |
|---|---|---|---|---|
| Textures low / medium / ultra | START | +0.0 / +0.0 / -0.1 % | within 0.2 % | 637 k, as at high |
| Geometry low | START | -0.5 % | -1.6 .. +1.4 % | 637 k → 635 k |
| Geometry low | 128 views | -0.7 % (median -0.2 %) | -9.6 % (E3M5 v0) .. +5.1 % (E4M8 v0) | 738 k → 731 k |
| Geometry ultra | START | -3.3 % | -13.0 % (v1) .. +10.9 % (v0, the spawn) | 637 k → 707 k |
| Geometry ultra | 128 views | +2.6 % (median +2.8 %) | -19.5 % (E3M2 v1) .. +85 % (END v2, 68 k → 126 k) | 738 k → 913 k (E1M8 v0, +24 %) |
| Models low / medium | game bench | -1.0 / -0.6 % | -1.2 / -0.8 % at the spawn, -0.3 / -0.1 % walking | 1091 k → 1082 k / 1085 k |
| Effects low / medium / ultra | game bench | -0.4 / -0.5 / +0.7 % | the rocket's blast -1.2 / -4.7 / +1.9 % | 1091 k → 1077 k / 1040 k / 1111 k |

- **The textures** change nothing the emulator counts: the CPU does the
  same work at every resolution. What a lower level saves is the
  console's: the GPU's 2 KB texture cache hits more often, VRAM, and the
  load (START's set 387 KB at high, 151 KB at medium, 80 KB at low: about
  a second less at the drive's double speed).
- **The ultra geometry moves work** from the slow path to the record loop
  (START, `PROFILE=1`, the 12 views summed): the records' stage +30 %
  (+520 k cycles), the slow path -35 % (-655 k): the near, big polygons
  come cut in 128-texel cells already, and it splits 41 % fewer
  triangles. A view whose slow path is busy gets faster (START v1 -13 %,
  E3M2 v1 -19.5 %), a far and wide one slower (the spawn +11 %, E1M8 v0
  +24 %); five of the 128 views go over a frame, four at high (E1M5 v0,
  486 k → 578 k, joins them).
- **The low geometry** saves 2.3 % of START's polygons for -0.7 % over the
  maps, and a view can lose: a near polygon without its T-junction
  vertices is fanned into longer triangles that the warp threshold splits
  more (E4M8 v0, `PROFILE=1`: 222 → 235 slow-path triangles, +5.1 %).
- **The models**: the gain is the map's items and torches (START's spawn,
  torches and weapons in view: -8.5 k cycles a frame at low; the guns'
  share, the `ents` stage, the same); walking START -0.3 %.
- **The effects**: the particles' own cost is the `ents` stage's change:
  the rocket's blast 33.2 k cycles a frame at high, 23.6 k at low, 26.8 k
  at medium, 46.2 k at ultra (the grenade's 23.0 k: 18.6 / 19.8 / 27.7 k).
  Beyond that the scenes differ: the particles draw on the game's random
  sequence (`q_rand`, which the items, the sounds and the triggers share,
  as Quake's `rand()`), so another count shifts every later draw (medium's
  -4.7 % on the blast, +1.1 to +2.0 % on the walks: other scenes, not the
  particles' cost). The bench never fills the pool: a full one (256, 512 at
  ultra) costs in proportion.
- **The hints** say it so: textures "lighter GPU, loads" (the console's,
  unmeasured here); geometry low "under 1% faster on average", ultra "3%
  slower, heavy views up to 24%"; models "about 1%" / "under 1% faster
  with items in view"; effects "about 1%" / "under 1% faster in fights",
  ultra "1 to 2% slower".

### 39.5 Checking

- **At high** (the default): `scripts/dlcheck.sh` on START's 12 views:
  display lists identical to the single-fidelity build's high; the 128
  spawn views: 8 differ (the textures over 128 texels, 0 to 1 % of their
  pixels); the game bench's `H` lines identical to master's (with the 2D
  weapons) but for the menu's pages (the Graphics page's rows and hints),
  its `snd` lines the menu's five sounds more.
- **Every level loads**: the bench at geometry 0, 2, 3 on the 32 maps,
  textures 0, 1, 3 and models 0, 1 on START, effects 0, 1, 3 on the game
  bench: no fallback at any geometry level (the arena's fullest map, E4M7:
  1003 / 1005 / 1135 KB of 1152); textures and geometry both at ultra
  (release-ultra's only levels, `BENCH_MAPS=1`): the 32 maps load, E4M7
  1135 KB.
- **DuckStation** headless (`PLAY=1 BENCH_MAPS=1`; textures low, geometry
  ultra, models low, effects ultra): the 32 maps load and play 30 frames
  each, no GP0 error, no invalid read.
- **The editions' discs** build (release 747 MB, release-ultra 722 MB, the
  default 791 MB) and the scripted release flow (TESTING 10.3) sets the
  four rows.
- **The menu** (`mgraphics*.png`): the two lines under the rows, the next
  level's note in white, nothing over the status bar.
- **Left to the console and an eye on screen**: the textures' GPU side; the
  four rows and their hints on a TV; a level changed in a game (the next
  level's load); the low geometry's cracks, the ultra geometry's light,
  the reduced items and torches.

## §40 — Streaming, the monsters, the SPU pool (2026-10-18)

Next: the resources loaded into RAM only within a distance of the player (as SM64 does), the
monsters and their behaviour, the same loading on the fly while playing, a loading pool for the
monsters and the items, the keys and the weapons always first even when something else is loaded. Three choices made: a CD read during the game
pauses the CD-DA music (a fade) and resumes it where it was, the reads
grouped, most at the map's load; every sound with a place in the map goes
to a pool loaded by distance (the player's and the weapons' stay
resident), at full quality; far monsters run Quake's AI as they are, only
their drawing waits for their model.

### 40.1 The disc: STREAM.DAT

`qcook stream` writes one file for every map (formats/qstream.h, `QST1`):
a directory sorted by `qs_hash` of the pak name, every asset on a sector
of its own, each with its group (a monster's model, head and sounds side
by side). 167 assets for the shareware: the nine monsters' models and
heads, their projectiles (w_spike, s_light, bolt, bolt3, lavaball,
zom_gib), the gibs, the backpack, the items' alias models (weapons,
armour skins 0-2 as `armor.mdl#1` / `#2`, keys, the rune, powerups, the
flames) and 120 sounds (the monsters', the entities': doors, plats,
buttons, triggers, items, keys, the ambient entities').

- **Models: the compact format.** A frame is Quake's own trivertx words
  (x | z << 8 | y << 16 | normal << 24): the engine expands the frame it
  draws into the renderer's 8-byte vertices with two ANDs and a shift a
  vertex (engine/mdl_expand.s, into a 24 KB cache of expanded frames);
  the model's scale and origin go into its GTE matrix (Quake's
  R_AliasSetUpTransform does the same), a shift keeps a big model's
  steps in the GTE's 4.12 (the boss: 2). The skin is cut in two halves
  (front and back, by the triangles' onseam side) with a 2-texel border,
  at half the pak's resolution (heads, gibs, projectiles, items: a
  quarter; flames: a half), with a fallback at half that.
- **Sounds:** a 64-byte header (pitch, flags, ticks, length) and the SPU
  ADPCM at the WAV's own rate: every piece of a sound read in 16 KB is a
  whole number of the SPU DMA's 64-byte units.

### 40.2 The pools and the passes

Four caches, each of the one above (engine/stream.h): the disc; RAM (what
the level arena leaves once the map is in: 309 to 373 KB at high, a bump
allocator compacted when a block no longer fits); VRAM (the map's
texture pages' free rows, the 4-bit pages, GAME.DAT's free rectangles);
SPU RAM past the map's bank (§40.4).

- **Wants.** Every 8 frames (and at once when the player is 256 units
  from where the last pass saw him: a teleporter's arrival, no read started
  on the old place's wants) a pass asks the edicts what they use and how
  much: a distance (Chebyshev, world units) from the player, `STREAM_PIN`
  for what must always be in. A monster's kind is wanted at its nearest
  monster's distance, its whole group (model, head, sounds); a head, a
  gib, a pack, an item at its own; the keys, the rune and the weapons
  pinned (the priority: they evict anything else); an entity's
  sounds (its `noise` fields) at its box centre; a static source's sound
  at its distance over its reach (ATTN_STATIC's 333 units weigh as
  ATTN_NORM's 1000); a running powerup's warnings pinned. Models are
  wanted to 2500 units, sounds to 1500.
- **Loads.** One CD read at a time, the most wanted first, room made by
  dropping only what is wanted less (never a pinned asset). A model is
  read whole into its RAM block, its skin placed (best fit among the
  regions, turned 90 degrees when it fits better so; upright when no RAM
  is left for its turned pixels; at half resolution when VRAM is short),
  uploaded, the pixels dropped once the GPU took them (the block pinned
  against the compaction until then). A sound goes through a 16 KB
  staging buffer, piece by piece, each piece's samples DMAed to its
  place in SPU RAM before the next is read; its place chosen as the
  smallest gap that holds it, a victim chosen as the least wanted sound
  whose block with the gaps around it would make room (not just any
  less-wanted one: no evicting for nothing).
- **Failures.** A read error backs off a second; an asset with no room
  even with everything wanted less dropped waits until the assets wanted
  as much hold less (RAM, VRAM or SPU) — no rereading the same 64 KB
  every second; a broken asset never comes back.
- **The music.** A read waits for the CD-DA's fade, the music stays
  suspended while reads follow each other and resumes where it was once
  nothing is wanted (engine/music.c `music_suspend` / `music_resume`).
- **The map's start** (the loading screen) reads everything wanted at
  once, nearest first; the game then streams what the player walks to.

### 40.3 The monsters

All nine kinds of the shareware (army, dog, ogre and marksman, knight,
zombie, wizard, demon, shambler, Chthon) in native C, transcribed from the
shareware's `progs.dat` 1.06 itself: a throwaway disassembler of its
bytecode, and a generator that writes each kind's state table (frame,
next state, action, argument) from the frame functions' statements, the
frame names taken from the rerelease's `$frame` lists (the same models).
A state is a QC frame function, `name =[ $frame, next ] { code }`: one
think runs its action (ai_stand, ai_run(dist)...) or the kind's own code.

- **1.06, not the rerelease**, where they differ: the knight's attack
  skips a frame the rerelease put back; OgreCheckAttack works out a
  chance and then throws whatever it is; the marksman is monster_ogre
  under its own classname (CheckAnyAttack's generic CheckAttack, no
  sight sound, a kind apart for T_Damage's anger); ZombieFireGrenade's
  offset uses the vectors of the last makevectors before its own (the
  engine keeps QC's `v_forward / v_right / v_up`: every makevectors of
  the AI, and the player's view's each tick, PlayerPreThink's), so does
  the shambler's smash; LightningDamage's side lines are offset by the
  unnormalised beam (`f_x = -f_y; f_y = f_x` with the new f_x), up to
  9600 units off.
- **The AI** (engine/ai.c): ai.qc, fight.qc, monsters.qc, sv_move.c
  (movestep, step direction, chase, close enough, check bottom),
  checkclient with the player's PVS row; SV_Physics_Step and
  SV_Physics_Toss for what monsters throw; T_Damage with the QC's float
  damage kept x256, Killed, the anger (not at their own kind but the
  soldiers'); the ground entity kept (a lift carries a monster).
- **Combat on both sides:** the player's projectiles and a monster's are
  the game's projectiles with an owner (its box passed, the damage its);
  MOVE_MISSILE (the monsters' boxes grown by 15 for a missile: SV_Move's
  mins2 / maxs2, in the asm trace too, a clip entry marked a monster's);
  T_RadiusDamage x256, half on a shambler, findradius's order (the last
  edict first, the player last), CanDamage's lines through the monsters
  (MOVE_NOMONSTERS: before, a monster's own box hid it from every
  blast); the grenade exploding on a monster (GrenadeTouch's
  DAMAGE_AIM); the lightning gun's beam through the monsters to the wall
  and LightningDamage's three lines (the "sparks" where it hit, not
  Quake's, gone).
- **The kinds' own:** the ogre's grenades (40) and chainsaw with its
  meat; the zombies' flesh (10), their falls, their getting up when
  there is room, the crucified ones; the scrag's two spikes fired by
  helper entities 0.8 and 0.3 s after the cast, its strafing; the
  fiend's and the dog's leaps (their touch); the shambler's claws, smash
  and lightning (the glow in its hands an entity of its own); Chthon
  risen by its trigger, its lava balls (leading the player on hard),
  E1M7's `event_lightning` between the two doors targeted "lightning".
- **The world for monsters:** a teleporter takes any living creature
  (it took the player whoever touched it), the telefrag at the arrival
  (spawn_tdeath), trigger_push and trigger_monsterjump, the forced
  retouch for every edict, SV_PushMove for every edict (riders carried,
  a monster in a door's way blocking it: door_blocked's damage).
- **The model effects** (the MDL's flags, now in the stream's models):
  the gibs' and heads' blood trails, the zombie flesh's, the scrag's
  green tracer, the lava ball's fire and light, the backpack turning.

### 40.4 The SPU pool

The map's bank (formats/qsound.h, now `QSB2`) keeps only what the player
and the weapons need (world.qc's and W_Precache's sounds, the
environment's): 364 to 388 KB with nothing halved (before: 504 KB with
26 sounds at half their rate). The rest of SPU RAM (about 130 KB) is the
pool: a sound handle is the bank's index, or `SND_STREAM` + the streamed
asset; a streamed sound plays only while its samples are in; a static
source names its sound by hash, its looping voice keyed on when its
samples arrive; a sound leaving SPU RAM first stops the voices playing it.

### 40.5 What a crowd costs

The bench's crowd (`MONSTERS=2 SAMPLE=1 MAP=E1M3 scripts/bench.sh`): every
monster of E1M3 (47) awake and after the player for 600 ticks, the
entities' tick timed, the PC sampler on, and (bench builds) the traces'
cost by caller (visible, the walk's, CheckBottom's corners and lines, the
flyers', the touch passes). First measure: `ents_tick` 830 k cycles a
tick on average, 5 M at worst. Every change below keeps the results
(the asm against clip.c, call by call; the scenarios' logs), Quake's
order included where Quake has one:

- **Quake's area nodes** (SV_CreateAreaNode, SV_LinkEdict, SV_ClipToLinks,
  SV_TouchLinks; engine/ents.c, clip.h): the world's box halved four
  times across its longer side, 31 nodes numbered in Quake's walk order;
  a solid entry, a trigger, linked (appended, as InsertLinkBefore does) in
  the deepest node its box does not straddle. A trace tries the lists of
  the nodes its swept box reaches (a stack, children[0] walked first),
  skipping the subtrees with no entry (two bits a node, kept by counts);
  the touch passes gather the triggers the same way and touch them after
  (each one tested again on its turn: a touch spawns, removes and moves
  edicts). Before, every trace tried the 105 entries and every touch pass
  the 84 triggers.
- **SV_CheckBottom from one node** (`clip_box_node0`, clip.h): its four
  points under the corners and its five lines down all lie in one box
  under the monster; one walk from the root to the deepest node whose
  region holds the box (the box's extreme corners against each plane, by
  the walks' own test; general planes on the GTE), the points and lines
  from there. The corners: 7.4 k cycles to 1.6 k (and 2.5 k for the box's
  walk), a line 10 k to 3.3 k.
- **visible()'s line of sight** (clip_mover's `sight`, clip.h): visible
  only asks whether the line is clear. Once the world stops it, its
  impact is left where found (no backing out of residual solid: the
  fraction stays under 1 either way), and only the entries holding the
  line's start are tried while it is blocked: SV_ClipToLinks takes a
  trace that starts inside an entry whatever its fraction, so only such
  an entry can clear the line again (the whole swept box comes back when
  one does). The brush models a long line's box reaches were 70 % of its
  cost: 23.7 k cycles to 8.0 k. The bench checks 4000 random lines a map:
  the same verdict as the whole trace, the asm as clip.c. (The trace's
  other results are not Quake's then: no caller reads them after
  visible, each one traces its own line.)
- **The backing out of residual solid from a node** (the asm only): the
  walk from the root, before any split, also notes the first node whose
  plane the sweep does not clear by more than a sub-unit's step on each
  axis (`GUARD_M`, above any plane's |n|_1); every endpos (set_endpos
  rounds within a sub-unit of the sweep) is on the sweep's side of every
  plane above it, so the contents walk from there meets the root's. A
  monster's step 4.5 k cycles to 4.2 k.
- **The player's moves** (the bench's `pmove cost`: settle, run, jump, the
  wall, back): E1M3 46.1 k cycles a tick to 22.9 k; start 16.5 k to
  15.3 k (its few entries cost a flat list less than the area walk, the
  backing out's node more than makes up for it).
- **Smaller ones**: the swept box from the scratchpad's start and delta,
  unrolled; a relink in the same node only moves the entry to its list's
  end; the game code's `memcpy` calls inlined (`-fno-builtin` made each
  a call); the water transition's contents taken again only when the
  origin moved; the clip entries' reject loop in registers; the touch
  boxes in whole units (int16); an alias model's leaf found when it is
  drawn, not at every link.

The crowd now: `ents_tick` 287 k cycles a tick on average, 1.8 to 2.3 M
at worst (a tick where most of the 47 think: woken together, they think
together, as in Quake). Its parts a tick: the visible lines 63 k, the
steps' traces 60 k, the touch passes 22 k, CheckBottom 27 k and its
box's walk 19 k, the flyers 11 k.

### 40.6 Checking

Shortened; what was seen:

- **The physics diffs** (`VIEWS=0 scripts/bench.sh`, `MAP=E1M3` too):
  clip.s against clip.c on 4000 random sweeps, 3008 sweeps at the box
  entries (47 monsters on E1M3) as entities too, 4000 contents, 1000
  CheckBottom boxes (the node asm against C, the points and lines from
  it against the root's), 4000 lines of sight; pmove.s against pmove.c
  on 9000 ticks: no mismatch, after each change of 40.5.
- **The monsters' scenarios** (`MONSTERS=1 MAP=... scripts/bench.sh`, the
  first runs, before the area nodes): for each kind, a monster with a
  free spot in its sight; the player stands there 300 ticks (it sees him,
  comes, attacks), then shoots it with the shotgun, pictures at each
  step. E1M1: the soldier and the dog fight and die, their sounds
  streamed in on the approach. E1M2: the soldier, the ogre (its
  chainsaw) and the knight killed. E1M3: the ogre (its grenades), the
  zombies (their flesh, their falls; the shotgun never gibs one:
  Quake's), the wizard killed. E1M5: the wizard's and the demon's
  attacks, the knight's sword. The sounds by name from the `snd` lines
  (the scratchpad's hash decoder over pak0's names).
- **What those runs showed wrong**, fixed since: the bench ran the stream
  manager only on its pictures' frames (a knight on E1M2 fought unseen
  and silent, its model read the whole fight long): two frames a tick
  now, as in the game; a pass after a teleport used the old place's
  wants and started reading what was no longer near: a pass at once when
  the player is 256 units from the last pass's place. The bench's spot
  could leave the player falling or in a liquid: on a floor, out of the
  liquids now.
- **The items bench** (`ENTS=1`): every item streamed in and taken (the
  armours' skins, the weapons, the artifacts, the flames).
- **The crowd** (40.5), E1M3.

### 40.7 With the quality settings (§39, merged after)

- The items' alias models are streamed (STREAM.DAT) at Quake's detail:
  the models level reduces the projectiles (GAME_<n>.DAT) and what the
  map's models file keeps (the BSP item boxes, which it never reduced);
  the streamed monsters and items are one file at every level.
- The game data carries both sides' fields (the level, the largest
  geometry; the pages' free rectangles for the streamed skins): `QGM7`.
- The particles of the monsters' effects (blood and tracer trails,
  R_RunParticleEffect's upward ones, Chthon's lava splash) follow the
  effects level as the others do.

### 40.8 Left

- A STREAM.DAT per model level (its alias models reduced as the map's
  were, the monsters too), so the models setting reaches them again.

- The scenarios again with the fixed bench (the stream between ticks,
  the spots on a floor, `mon OVERLAP`): the shambler's attacks and
  Chthon (woken by its `use` now, its lava balls, `event_lightning`'s
  doors) were not seen; a demon was once found inside the player's box
  on E1M3 (before the area nodes), not explained.
- The game bench (`BENCH_GAME=1`) and DuckStation (the CD-DA's pause and
  resume around a read, the reads' timing) not run on this state.
- A crowd thinking together still costs a tick of 2 M cycles: spreading
  the thinks over the ticks would move them from Quake's times (a choice
  left open).
- The touch passes (22 k a tick in the crowd) and the CheckBottom box's
  walk (19 k) are C and C-shaped asm: candidates for the next pass.

## §41 — The brush models cut by the world's BSP (fourth play test, 2026-10-18)

- **The report**: almost every texture hidden in a wall showing (E1M1: black blocks standing in rooms and corridors, a
  door's buried part over the floor) and START's floor over the pit to the
  end (func_bossgate) still disappearing at times (§33).
- **The cause**: a brush model went whole into one leaf's render list (§33:
  the leaf on the eye's side of its nearest face). With no depth buffer, the
  painter's order puts all of the model before or after each world face:
  the part of a door slid into its wall pocket, a plat's sides under the
  floor, a column under the slime were painted over the walls and floors in
  front of them (their faces there unlit: black). And a model was looked at
  only when one of the leaves its box touches, 8 kept at most, was in the
  PVS: the slab touches more, and from some places none of its 8 were.
- **Quake's answer** (r_bsp.c R_DrawSolidClippedSubmodelPolygons,
  R_RecursiveClipBPoly): each polygon of a brush model is cut by the
  world's planes from the model's top node down; a piece reaching a solid
  leaf is dropped, the others are emitted with their leaf's key, so the
  span sorter puts each piece where its leaf stands in the BSP's order.
  `engine/bmfrag.c` does the same with the walk's painter's order in place
  of the keys:
  - per drawn brush model, a cache (`bmf_models`, by model: the edict
    keeps its 384 bytes, see below): its records cut where it stands, at
    the load and again only when it moves (a door, a plat, while they
    move); the pieces are records in the world's format (face id 0: drawn
    with the models' stamp), their vertices in a level pool of their own
    (`r_state.fragpool`), grouped by the model's node side and by leaf;
  - each frame, the model's own BSP from the eye, back to front: per node
    side facing the eye (the faces facing away are left out, as Quake), its
    groups whose leaf the PVS sees go into that leaf's render list (an item
    of type RI_PIECES); the walk draws a leaf's list between the faces
    behind it and those in front (`r_world.s` `ei_pieces`: the models'
    record loop with the pieces' pool, the world's matrix as it is);
  - the cut: d > 0 is the front, as the walk (Quake's `dist > 0`); a
    polygon lying in the plane goes to the back (Quake's); one touching the
    plane from the front goes whole to the front (Quake's back part there
    has no area: nothing drawn); a corner in the plane goes with its
    neighbours' sides; a crossing is computed from the lower end of its
    edge (lexicographic), so two pieces sharing an edge get the same point,
    its parameter exact whatever the plane (both distances shifted down
    together until the division fits 32 bits); an uncut polygon keeps its
    cooked corners (a quad its diagonal), a cut one is fanned from its
    first corner into quads and a last triangle; the corners' texels and
    light interpolated along the edges; a record's sphere and average light
    (the flat shading's) from its corners;
  - which models a frame looks at: as before, those whose kept leaves the
    PVS sees; a model with pieces whose box touches 8 leaves or more (its
    list may be cut short) is seen by its top node instead (its pieces'
    leaves are all under it, and the PVS marking stamps a visible leaf's
    ancestors); a model whose box lies wholly behind the eye is left there;
  - the old way stays as the fallback: a model whose pieces do not fit its
    room is drawn whole as before (none on E1M1 and START).
- **The dispatch** (`r_world.s` ei_model): the item's type is read signed
  (`lb`), RI_PIECES is 0x80, a negative type goes to `ei_pieces`; a model
  enters draw_model past its first instruction with its return set to the
  next item, so the test costs a model nothing (5 instructions, as `jal`,
  its slot, draw_model's `sw` of the return, `b ei_next` and its slot
  were); `ep_back` does ei_next's work itself.
- **On the way**: consecutive groups of one leaf were merged across the
  model's node sides, so the pressed E1M1 button's three visible faces (all
  in its recess's leaf) were drawn on the first one's side only: from the
  room, none. Groups merge within a node side now. And a pointer to the
  cache in the edict made it 388 bytes: every `&edicts[i]` then took three
  shifts and two adds instead of two shifts and an add, +0.5 to +1.9 % on
  E1M1's views with no brush model in sight; the caches are found by model
  number instead.

## §42 — The boot that found no map, and the stack's room (2026-10-19)

- **The report**: after the merge of the monsters, the streaming and the brush models' cut, the
  release broken. The release disc with every
  map (pak1: 32 maps, 386 files in `\MAPS`) booted with `world: 0 maps on
  the disc`, after `psxcd: invalid args to CdReadRetry()`.
- **The cause, twice:**
  - psxcd's ISO lookups (`CdSearchFile`, `CdOpenDir`) read a whole
    directory into a buffer malloc'd from the SDK's heap, which libc's
    start-up places between the end of the BSS (`_end`) and 0x801FFFF8,
    under the stack. The monsters and the streaming grew the code from
    180 KB to 269 KB, and the BSS with it: the heap was 21 KB, `\MAPS` is
    24 KB (12 sectors) on a disc with every map. The malloc returned NULL;
    `CdReadRetry` refused the NULL buffer (the SDK's argument check:
    without it the DMA would have written 24 KB over the kernel at address
    0). The listing came back empty, so no map; every lookup of a map's
    file failed the same way.
  - The brush models' pieces (§41) then grew the BSS past the RAM: the
    release build's `_end` was 0x80200570, 1.4 KB beyond the 2 MB. That
    is the RAM's mirror, so the kernel's first bytes. The stack, from
    0x801FFFF0 down, started inside the BSS. Nothing checked either.
- **The fix:**
  - `engine/cd.c` reads the ISO9660 volume itself, at the first lookup
    (after `music_init`, when the drive has answered):
    - the volume descriptor (sector 16);
    - the root's records;
    - the directories in it (8 at most, one level deep: the disc has the
      root and `\MAPS`).

    A lookup reads its directory's records into the level arena's top
    (mark, then release), where psxcd used its heap. It is the same seek
    and read (about 80 ms for `\MAPS`).
  - `cd_list_dir` works the same way. `cd_find` gives a file's first
    sector and size; `stream_init` uses it for `STREAM.DAT`.
  - Nothing calls psxcd's ISO code any more, so `isofs.o` is no longer
    linked. That frees 4.5 KB of code, its 2 KB descriptor buffer and the
    heap's directory copies.
  - **The level arena:** 1152 KB → 1140 KB (1136 KB with §43's code). Every preset gets back the
    stack's 16 KB (architecture.md §4). The streamed assets' RAM pool,
    which takes whatever the arena leaves, has 12 KB less on every map
    (START: 262 → 250 KB).
  - **`scripts/ramcheck.sh`** runs after every link (CMakeLists.txt, after
    hazcheck). It measures `_end` against the stack's top (0x801FFFF0) and
    fails the build under 16 KB.
- **Stack room, measured:**

  | Build | Default | Bench | Release |
  |---|---|---|---|
  | e6c2bde as merged | — | — | −1400 B (`_end` 0x80200570) |
  | `cd.c` alone | 4560 B | 49880 B | 5024 B |
  | With the arena | 16848 B | 62168 B | 17312 B |

- **A level change that stopped the game** (found by the checks below).
  `load_level` ran `stream_preload` before `snd_map_start`. The
  preload's residency pass asks `snd_stream_wants` for the static
  sources' streamed sounds, but only `snd_map_start` turns the new bank's
  static sources (the file's 20-byte entries) into the 12-byte entries
  that function reads, with this map's voices. The pass read the file's
  entries with the previous map's voices: garbage reaches, one of them 0
  on E2M7. The all-maps bench stopped there on a `break 7` (a division
  by zero), on PCSX-Redux and DuckStation alike; in a game, a level
  change could do the same.
  - `snd_map_start` now runs first. It needs nothing streamed: a static
    voice is keyed when its samples are in.
  - The preload now also wants the static sources' sounds around the
    spawn. They used to wait for the first passes of the game, each read
    a music pause.
- **The streamed skins' uploads** (found on the way). `LoadImage` sends
  `w × h / 2` words for `w × h` halfwords, rounded down. Its DMA then
  rounds them up to its 16-word unit, which the cook's 64-byte zero
  padding feeds.
  - An odd count of halfwords whose half is a multiple of 16 therefore
    went one word short. The GPU took the next command's first word for
    the last pixels: a broken command stream on the machine and on
    DuckStation; PCSX-Redux forgives it.
  - That hit 10 of STREAM.DAT's 212 images: `gib1`, `dog`, `h_demon` and
    two more.
  - `qcook`'s `cut_image` now gives every image an even count of
    halfwords (two more edge columns when both halves are odd). The
    engine reads each image's own size, so nothing else changed.
  - `vram_upload` (world.c) has the same rounding for an odd rectangle;
    no texture set or `GAME_<n>.DAT` rectangle is odd today.
- **Checking:**
  - **Every link:** `ramcheck` for the three presets (the table above).
  - **The release disc with every map:** boots with `world: 32 maps on
    the disc`; START loads, and its stream pool is 250 KB.
  - **PCSX-Redux, fast forward** (`BENCH_MAPS=1 PLAY=1 HASHONLY=1`):
    - The emulator paces itself on its audio output; a private config
      with `SPU.Speed = 10` runs the same emulation ten times faster.
      The frames' hashes of a normal-speed run are identical, for the 44
      frames both runs reached.
    - All 32 maps load and play their loading screen and 30 frames, up
      to `bench done`; E2M7 passes since the load-order fix.
  - **DuckStation:**
    - Before the load-order fix, the first 15 maps loaded and played;
      the run then stopped on E2M7's `break 7`, as PCSX-Redux did.
    - After the fix it was not run again: the emulator no longer starts
      on this machine. The display device failed once (`VK_INCOMPLETE`),
      then every start waited before the boot, at any speed.
    - **To do before a disc goes to the machine.**
  - **The cook:** none of STREAM.DAT's 212 images has an odd count of
    halfwords any more.

## §43 — The packet arena after the monsters: the world, the menu, lighter monsters (2026-10-19)

- **The report**, on the release build of §42: menu pieces and many textures no longer drawn
  (START showed it at once), the options no longer editable.
- **Seeing it.** The release build ran under PCSX-Redux with its UI and a
  Lua script (`-dofile`):
  - `PCSX.GPU.takeScreenShot()` saved the emulated screen, nothing of the
    desktop;
  - `PCSX.SIO0.slots[1].pads[1].setOverride` / `clearOverride` drove the
    pad;
  - the private config's `SPU.Speed` ran it fast.

  Seen that way, START's far rooms through the hall's arches were the flat
  sky colour, and the menu over the paused game lost its pieces. The bench
  dumps and pktview, the offline renders, had not shown it.
- **The cause: the GPU packet arena (96 KB a frame) full.**
  - START has nine `monster_zombie` with spawnflags 1: crucified ones, a
    decoration Quake draws and does not count as a monster. Since §40
    they spawn and are drawn with zombie.mdl's 347 triangles, about
    12.5 KB of packets each.
  - The walk emits front to back, so once the arena is full the farther
    records are not drawn. The menu, drawn after the world, got no room
    either: its pieces were missing, and the options seemed stuck.
  - START's 12 views, packet bytes (`MAP=START BENCH_MAPS=1 PROFILE=0
    HASHONLY=1`):

    | Commit | v0 | v4 | v6 | v8 |
    |---|---|---|---|---|
    | 8a4f883 (before the merges) | 57500 | 31408 | 26316 | 24108 |
    | 2ffa4ba (quality settings), e28946a (brush models' cut) | the same | | | |
    | b502832 (streaming and monsters), e6c2bde, 65a7647 | full | 59232 | 40228 | 38020 |

  - View 0's display list had 1249 more triangles in the game data's
    palette, on the streamed images' pages.
  - With the level arena too small for zombie.mdl (a test), the views went
    back to 8a4f883's bytes and cycles.
- **The world and the menu first.**
  - `draw_model` (r_world.s) leaves an alias model out (a monster, an
    item, a flame, a gib) when less than `R_MODEL_ROOM` (24 KB) of packet
    room is left. Brush models and the view model are always drawn. A
    left-out model costs nothing, and the room goes to the walk's farther
    records.
  - While Quake's menu is up, `r_ui_room` keeps `R_MENU_ROOM` (12 KB) at
    the arena's end: the walk stops short of it, and the menu's packets
    always fit.
  - With room to spare, nothing changes: the display lists are the same.
- **The monsters lighter, per model level.** `qcook stream --level <n>`
  writes `STREAM_<n>.DAT` for each model level. Every file holds the same
  assets in the same order. `stream_reduce` (tools/qcook/game.c) applies
  `decim.c`'s collapses:
  - **A monster's body, at every level, high included:**
    - high: half its triangles, error bound 64;
    - medium: a third, bound 96;
    - low: a quarter, bound 128.

    The animation makes every collapse costly (zombie.mdl has 198
    frames): a bound of 3 kept 84 % of the zombie's triangles, 12 kept
    68 %.
  - **The rest** (heads, gibs, projectiles, items, flames): as the map's
    own models, Quake's detail at high.
  - The vertices no triangle uses any more leave the frames.
  - At high: soldier 188 of 328 triangles, dog 264 of 426, ogre 244 of
    326, knight 145 of 213, zombie 173 of 347, wizard 107 of 157, demon
    191 of 275, shambler 220 of 284, boss 543 of 555.
  - The engine reads the directory of the models level's file at boot,
    and again when a map loads at another level (`stream_set_level`, in
    `load_level` beside `gdata_set_models`). The Graphics page's Models
    hint says so.
- **Measured** (PCSX-Redux, `PROFILE=0`), frame cycles of START's views:

  | View | 8a4f883 | the merge | now |
  |---|---|---|---|
  | v0 | 637 k | 833 k (arena full) | 786 k (85284 B) |
  | v4 | 440 k | 602 k | 511 k |
  | v6 | 435 k | 510 k | 471 k |
  | v8 | 405 k | 502 k | 451 k |

  - v4, v6 and v8 are back under one vblank. v0 was over it before the
    monsters.
  - START's preload takes 85 KB of the RAM pool, against 155 KB: more
    room for the stream, fewer reads.
- **Checking:**
  - **The release on PCSX-Redux's real GPU:**
    - START whole, the menu whole;
    - Graphics: Textures high → medium on a Left press, its hint and
      "Takes effect at the next level";
    - Models low, then the menu closed: START reloaded with `GAME` level 0
      and `\STREAM_0.DAT` (the preload: 69 KB).
  - **The all-maps play bench** (fast forward): 32 maps of 32, no arena
    full at a spawn view.
  - **The soldier at 188 triangles**, close up (`MONSTERS=1 MAP=E1M1`):
    whole, recognisable.
  - **The stack meter** (`stack: N bytes never reached` at each load):
    14.3 KB or more untouched.
  - DuckStation not run (it no longer starts on this host, §42).
  - **The links:** the default preset's needed 4 KB more, so the level
    arena is 1136 KB. Stack room: default 20.4 KB, release 20.8 KB.
- **Left: the stream's RAM on the biggest maps.** The brush models' cut
  (§41) reserves, up front, three times every brush model's corners and
  record bytes (`BMF_GROW`), 285 KB on E1M3. With the edicts, the stream's
  RAM pool comes to 0-6 KB on E2M6, E3M4, E3M6, E4M7, E2M4, E1M3 and E4M5
  (24 KB on E2M7). A monster's model needs 40-80 KB even reduced, so there
  the monsters think but are never drawn. e6c2bde had the same, with 12 KB
  more. Fixing it is a choice of where the RAM comes from: the cut's
  reserve sized to its use, the 2D weapons' 102 KB loaded only when that
  option is on, or the monsters' frames.

## §44 — The brush models' room sized on their cuts (2026-10-19)

- **The ask**: the brush models' cut room measured and sized on its real use, its RAM back to
  the monsters' streaming (§43's last item).
- **The old room** (§41's `bmf_reserve`, before the spawn): for every
  model the cooked entities name, three times its records' bytes and its
  corners (`BMF_GROW`) and 8 groups a node. What the cuts took, E1M3:

  | | reserved | cut where they spawn | most along the movers' ways |
  |---|---|---|---|
  | records | 110760 B | 25120 B | 28380 B |
  | vertices | 11250 | 2214 | 2513 |
  | groups | 4624 | 455 | 507 |

  Four to five times the use for the records and the vertices, nine for
  the groups. Over the 32 maps the old room asked 39 KB (END) to 372 KB
  (E1M6), and on the biggest maps the stream's RAM pool was left with
  0-6 KB: their monsters were never drawn.
- **Sized on the cuts.** `bmf_reserve` now runs after the spawn (which
  puts every model where it stands and gives each mover its travel) and
  still before the stream manager takes the arena's rest:
  - each spawned edict's model is cut in a scratch cache at the arena's
    top (the old room of the largest model; released after);
  - a model that never moves gets what its cut takes where it stands, plus
    64 bytes: `bmf_load`'s cut at the load is the same cut (the same
    origin, the same top node: `cut_where` builds ent_link's box);
  - a mover gets twice the most its cuts take at the places it goes
    through and halfway along each way between them, and where it spawned
    when that is off its path (`model_path`):
    - a door's, a button's, a plat's pos1 and pos2;
    - a train's corners in its path's order, each corner's origin less the
      model's mins (func_train_find's and train_next's place), up to the
      first corner it comes back to (`movers_path`, engine/movers.c; 64
      corners at most, a longer path keeps the old room). A first version
      looked for the way back to the first corner only: paths looping to
      a later one went round until the cap;
    - a secret door's place and its two moves' ends (`secret_dests`, out
      of fd_secret_use);
  - a model whose path or cuts the scratch cannot take keeps the old room
    (none on the 32 maps);
  - the needs (12 bytes a model) lie where the pool is reserved next,
    which only the cuts write (`bmf_load`), after the needs' last read:
    no arena byte is left behind (0.9-2 KB a map);
  - `cut_model` takes the origin and the top node instead of an edict, so
    the measure's cuts need no 384-byte edict copy.
- **Why twice the halfway places.** `BMFCHK=1` (bench.sh) cuts every
  mover in its own room at 33 places a way, as `bmf_draw` cuts it while it
  moves. Over the 32 maps, the 33 places' most against the sampled most,
  the worst of each kind:

  | Sampled at | doors | plats | buttons | secret doors | trains |
  |---|---|---|---|---|---|
  | the ends, the corners | 2.60 | 1.80 | 1.33 | 2.20 | 2.19 |
  | and halfway | 1.73 | 1.80 | 1.33 | 1.57 | 1.90 |
  | and the quarters | 1.73 | 1.80 | 1.00 | 1.57 | 1.78 |

  - At the corners alone, E1M6's train *39 needed more groups between two
    corners than twice its corners' most: 14 of its cuts were given up
    (drawn whole there).
  - The quarters cost 1.7 times the halfway places' time at the load
    (E3M4 1.85 s against 0.98, E1M3 1.20 against 0.68) for little more
    margin, and give a larger room (their most is larger).
  - Twice the halfway places: no cut given up at 33 places a way on the
    32 maps; the fullest room 92 % (E2M5), 51-84 % on the others. A place
    between those 33 can still take more: that cut is then given up, and
    the model drawn whole until it moves again (§41's fallback).
- **What it gives** (PCSX-Redux in fast forward, every map at its high
  levels; the stream's preload as it was at the spawn):

  | Map | Old room | Now | Freed | Stream pool | Preload | Cuts | Measure |
  |---|---|---|---|---|---|---|---|
  | START | 134 KB | 49 KB | 85 KB | 332 KB | 85 KB | 22 | 0.11 s |
  | E1M1 | 138 | 81 | 57 | 279 | 126 | 101 | 0.22 |
  | E1M2 | 154 | 83 | 71 | 227 | 179 | 118 | 0.29 |
  | E1M3 | 284 | 129 | 155 | 157 | 120 | 201 | 0.67 |
  | E1M4 | 179 | 93 | 86 | 114 | 79 | 127 | 0.31 |
  | E1M5 | 175 | 94 | 81 | 254 | 235 | 121 | 0.32 |
  | E1M6 | 372 | 198 | 174 | 261 | 227 | 183 | 0.65 |
  | E1M7 | 88 | 40 | 48 | 772 | 276 | 29 | 0.09 |
  | E1M8 | 110 | 53 | 57 | 559 | 157 | 55 | 0.15 |
  | E2M1 | 203 | 101 | 102 | 176 | 130 | 154 | 0.36 |
  | E2M2 | 154 | 92 | 62 | 173 | 162 | 88 | 0.32 |
  | E2M3 | 211 | 109 | 102 | 155 | 115 | 132 | 0.36 |
  | E2M4 | 190 | 83 | 107 | 107 | 93 | 172 | 0.28 |
  | E2M5 | 207 | 106 | 101 | 187 | 147 | 142 | 0.36 |
  | E2M6 | 274 | 171 | 103 | 96 | 76 | 172 | 0.68 |
  | E2M7 | 163 | 80 | 83 | 103 | 77 | 107 | 0.30 |
  | E3M1 | 192 | 101 | 91 | 221 | 124 | 122 | 0.35 |
  | E3M2 | 351 | 179 | 172 | 366 | 272 | 166 | 0.42 |
  | E3M3 | 164 | 111 | 53 | 349 | 215 | 117 | 0.39 |
  | E3M4 | 360 | 210 | 150 | 2 | 0 | 245 | 0.99 |
  | E3M5 | 115 | 61 | 54 | 92 | 73 | 49 | 0.35 |
  | E3M6 | 182 | 104 | 78 | 46 | 41 | 97 | 0.35 |
  | E3M7 | 152 | 65 | 87 | 320 | 215 | 113 | 0.20 |
  | E4M1 | 158 | 75 | 83 | 209 | 125 | 79 | 0.31 |
  | E4M2 | 205 | 131 | 74 | 203 | 186 | 119 | 0.31 |
  | E4M3 | 226 | 107 | 119 | 221 | 178 | 135 | 0.29 |
  | E4M4 | 94 | 50 | 44 | 138 | 114 | 71 | 0.19 |
  | E4M5 | 289 | 135 | 154 | 117 | 102 | 146 | 0.34 |
  | E4M6 | 202 | 117 | 85 | 282 | 64 | 134 | 0.32 |
  | E4M7 | 214 | 133 | 81 | 0 | 0 | 160 | 0.39 |
  | E4M8 | 230 | 140 | 90 | 183 | 147 | 132 | 0.40 |
  | END | 39 | 22 | 17 | 708 | 92 | 13 | 0.08 |

  - 17-174 KB freed a map, 91 KB on average.
  - The pools of §43's starved maps: E1M3 157 KB, E2M4 107, E2M6 96,
    E2M7 103, E4M5 117 (all 0-24 KB before). On E1M3 the monsters'
    scenario (`MONSTERS=1 MAP=E1M3`) draws 2 to 6 streamed models in 10
    of its 12 dumps (94 reads during play).
  - The measure costs 0.08-0.99 s at the load, 0.35 s on average (13-245
    cuts; E3M4's trains and big doors the dearest). A load takes 9-14 s on
    the same emulator.
- **A sound read into address 0.** With the arena that full, E3M4 and
  E4M7 had no 16 KB left for the stream's staging buffer (nor the 24 KB of
  the expanded frames' cache), yet E4M7 kept a 6 KB pool: every streamed
  sound was read to address 0 and found "not a sound this engine reads"
  (82 such lines over the two loads), never to be tried again.
  `sound_start` starts none without the buffer. With the room above,
  E4M7 now gets the buffer (its sounds stream, its pool is 0), E3M4 a
  2 KB pool and no buffer.
- **The skill.** The room covers the models of the spawned entities: a
  model spawned only later gets none and is drawn whole. Only a death
  after a skill change could do that, and only START changes the skill
  (trigger_setskill): no brush model there hangs on it. The models left
  unspawned at skill 1 are deathmatch-only walls (spawnflags 1792) and a
  few hanging on the skill (E1M2, E1M7, E2M3, E3M1, E4M1, E4M2, E4M7),
  spawned at their map's load when the skill asks for them.
- **Checking:**
  - `BMFCHK=1 BENCH_MAPS=1 VIEWS=0 scripts/bench.sh`: 33 cuts a way for
    every mover of the 32 maps (132-2970 a map), none given up; no "not a
    sound" line;
  - the change of `cut_model`'s arguments: the `bmf:`, `bmfchk`,
    `stream: pools` and `stream: preloaded` lines identical on the 32
    maps;
  - the release on PCSX-Redux's real GPU (§43's Lua script): the menu
    whole, START's far rooms through the arches, the HUD;
  - the needs moved under the pool: the cuts and the checks' lines
    identical again, every pool 1-2 KB larger;
  - the links: stack room 18192 B (default), 18592 B (release), against
    20.4 / 20.8 KB: the measure's code is 1.8 KB, `movers_path` 0.5 KB.
- **Left:**
  - **E3M4 and E4M7** still have no stream pool (2 and 0 KB), E3M6
    46 KB: their worlds take 742-783 KB of the 1136, the brush models'
    room 104-210 KB.
    The next levers: the 2D weapons' 102 KB (game arena) loaded only when
    that option is on; these maps' worlds at a lower geometry level.
  - **The cut while a model moves** (§41): a mover is cut again each time
    it has moved and is drawn, 0.1-1 M cycles for a big train or door
    (E1M6's *39 about 1 M, E1M3's trains about 0.35 M), against 564 k
    cycles a frame at 60 fps. Not measured in play yet (§49: measured,
    then cut down).
  - **The measure in the cook**: the same cuts made offline would cost
    the load nothing and could sample every way densely with a tighter
    margin; the cook would need the movers' travel (pos1 / pos2, the
    paths) and the cut on the host.

## §45 — The 2D gun's packets only while it is drawn (2026-10-19)

- **The ask**, after §44 left E3M4 and E4M7 with no stream pool: the 2D guns (102 KB) loaded only when chosen, that room back to the monsters'
  streaming (E3M4, E4M7).
- **Before**: the 2D gun's packets (§38: every frame's texels and each
  weapon's CLUT as GPU packets, 107220 bytes) were copied to the game arena
  at boot, whichever gun was chosen: 224 KB of game arena, 105 KB of them
  for a gun most never draw.
- **Now** the boot keeps the 2D gun's tables (its weapons and frames,
  1 KB) and where its packets are on the disc (`gdata.ws_lba`,
  `ws_sectors`, `ws_skew`: 53 sectors of GAME_<n>.DAT, the first one
  1068 bytes in). The packets come to RAM only while the 2D gun is the gun
  drawn:
  - the stream lends them its RAM pool's end (`stream_lend`, `stream_lent`,
    `stream_unlend`, engine/stream.c): at a map's start before its own
    buffers (the expanded frames' cache, the sounds' staging buffer, the
    pool), so the 2D gun finds its room wherever it found it in the game
    arena; during the game (the option changed in the menu) from the
    pool's end, once no read is in flight: the assets wanted least
    dropped (never a pinned one; nothing dropped when the pool cannot
    spare it), the pool compacted, then the 53 sectors read in the
    stream's turn, the music paused as for an asset's read;
  - the block sits past `pool_end`: compaction never moves it (the
    packets' links are absolute) and it goes back to the pool when another
    gun is chosen (after `DrawSync`: the GPU may still be reading a chain
    the last frame hung);
  - `game_ws_sync` (engine/game.c), at a map's start and each frame
    outside the menu, asks for the block or gives it back; once read, its
    chains' links are made absolute (`gdata_ws_link`); a new read (the
    same place after a map's load) is told by the stream's count of reads;
  - until the block is read the 3D gun is drawn (`ws_draw` returns 0): the
    2D gun shows a few frames after the menu closes.
- **The arenas**:
  - game arena 224 → 116 KB (111.5 KB used at boot, `mem: game arena` in
    the boot log);
  - level arena 1136 → 1242 KB (the 108 KB, less 2 KB for the stack: the
    lending code is 1.4 KB);
  - `MEM_LEVEL_WORLD` (1136 KB) caps the arena while the world loads
    (`mem_level_cap`): a map's geometry falls back exactly as before, and
    the room goes to what follows the world (the brush models' pieces, the
    stream's pool, the 2D gun), not to a larger world.
- **The pools** (every map at its high levels, PCSX-Redux in fast
  forward; `BENCH_MAPS=1 VIEWS=0`, `VIEWMODEL=2` for the 2D gun):

  | Map | 2D gun (as before) | 3D gun or none |
  |---|---|---|
  | START | 332 KB (+ 106 KB lent) | 438 KB |
  | E1M1 | 279 | 385 |
  | E1M2 | 227 | 333 |
  | E1M3 | 157 | 263 |
  | E1M4 | 114 | 220 |
  | E1M5 | 254 | 360 |
  | E1M6 | 261 | 367 |
  | E1M7 | 772 | 878 |
  | E1M8 | 559 | 665 |
  | E2M1 | 176 | 282 |
  | E2M2 | 173 | 279 |
  | E2M3 | 155 | 261 |
  | E2M4 | 107 | 213 |
  | E2M5 | 187 | 293 |
  | E2M6 | 96 | 202 |
  | E2M7 | 103 | 209 |
  | E3M1 | 221 | 327 |
  | E3M2 | 366 | 472 |
  | E3M3 | 349 | 455 |
  | E3M4 | 2 | 92 |
  | E3M5 | 92 | 198 |
  | E3M6 | 46 | 152 |
  | E3M7 | 320 | 426 |
  | E4M1 | 209 | 315 |
  | E4M2 | 203 | 309 |
  | E4M3 | 221 | 327 |
  | E4M4 | 138 | 244 |
  | E4M5 | 117 | 223 |
  | E4M6 | 282 | 388 |
  | E4M7 | 0 | 97 |
  | E4M8 | 183 | 289 |
  | END | 708 | 814 |

  - With the 3D gun (the default) or none, every map's pool is 106 KB
    larger; E3M4 92 KB (2 before), E4M7 97 KB (0), E3M6 152 KB (46).
  - With the 2D gun, the pools are §44's to the kilobyte.
  - `MONSTERS=1 MAP=E3M4` and `MAP=E4M7` (3D gun): streamed monsters
    drawn in their dumps (1-2 at a time), none before; 163 and 104 reads,
    no "not a sound". E3M4's 92 KB hold 28-38 of the 83 assets its
    scenario wants: some kinds are still missing at their dumps.
- **Checking:**
  - the 2D gun drawn the same: `BENCH_GAME=1 VIEWMODEL=2 HASHONLY=1`, the
    25 frame hashes identical to 000b210's (its packets in the game
    arena);
  - the release on PCSX-Redux's real GPU (Lua: the pad driven, the screen
    saved): Graphics → Draw weapon 2D (the hint "uses 106 KB of RAM"),
    the menu closed: the 3D gun, then the 2D gun 200 vblanks later; 3D
    again: the 3D gun;
  - the links: stack room 18272 B (default), 18672 B (release);
  - `BENCH_GAME=1` links with a 4 KB stack floor (bench.sh's `STACK_MIN`,
    4096 by default with it): its scenarios' code leaves the stack 6.8 KB,
    4.8 KB of it never reached.
- **Left:** in 2D the block is read again at each map's start (53
  sectors: about 0.4 s of the load at the drive's 2x).

## §46 — The brush models' room measured in the cook (2026-10-19)

- **The ask**: §44's last item, taken up by a session of its own: move
  `bmf_reserve`'s measure from the load to the cook. Each model is cut where
  it stands, and a mover densely along its travel; the needs go into the
  map's data. The load no longer pays 0.08-0.99 s, and the room can be
  tighter than §44's ×2. The format and the margin chosen: the needs in qw_bmodel at 40 bytes (hulls 1 and
  2 in int16), an exact margin with no constant.
- **The cut on the host** (`tools/qcook/bmf.c`):
  - What it ports: engine/bmfrag.c's `cut_model`, `cut_poly`, `cross`, and
    the top node of ent_link's box (`world_box_topnode`), integer for
    integer.
  - What it runs on: the cooked world as the engine reads it (the assembled
    file, its links still indices).
  - What it keeps: the same plane distances, the same crossings, the same
    limits (12 corners, 64 pieces, 24 pending back pieces, the flush past
    32 pieces, records of 42 vertices, groups merged within a node side).
  - What it leaves out: the texels and the light, since no size depends on
    them.
  - What a cut gives: the records' bytes, the pool's vertices, the groups.
- **The travel** (engine/movers.c on the host), for the classes that draw
  their model (doors, secret doors, buttons, plats, trains, walls,
  illusionaries, the gates):
  - **a door's and a button's** pos1 and pos2 (`set_movedir`, `travel_end`,
    on the engine's sine table rebuilt the same way); a door spawned open
    (DOOR_START_OPEN) at pos2, its ends swapped;
  - **a plat's** top (its origin) and bottom (`height`, or its size less 8
    units); it spawns at the bottom unless it is named;
  - **a train's** corners (a corner's origin less the model's mins), up to
    the first one it comes back to. They are found as `ent_find` would find
    them at each skill: a corner hanging on the skill would change the path
    (none does);
  - **a secret door's** place and `secret_dests`' two ends;
  - **where each spawns**: a train stands at its map origin for its first
    10 ticks.
- **The places**:
  - **Axis-aligned ways.** SUB_CalcMove puts a mover at a + delta × t / ticks
    (truncated; the way back from b rounds the other way), whatever the
    speed or the tick rate (§35). So on a way the longest axis takes every
    value between the ends, 1/8 unit apart, and the cook cuts each one.
  - **Slanted ways** (a train's). Another axis may sit one unit off the
    line at that value (truncated forward, rounded up backward); both
    places are cut.
  - **Coverage.** That is every position the engine can give a mover on
    its ways. A door reversed mid-way stays on its line: every door, plat,
    button and secret door of the id maps is axis-aligned.
- **What it costs and finds** (the 96 cooked worlds, one core):
  - 3.6 M cuts in 9.1 s, 0.02-0.34 s a world; the slanted ways' second
    places are 2 % of the cuts and change no maximum;
  - no cut given up;
  - the exact maximum against §44's sample (its ends and halfway places),
    the worst of each kind over the three geometry levels:

  | Exact / §44's sample | doors | plats | buttons | secret doors | trains |
  |---|---|---|---|---|---|
  | records | 1.73 | 1.67 | 1.33 | 1.47 | 1.98 |
  | vertices | 1.71 | 1.66 | 1.33 | 1.46 | 1.96 |
  | groups | 1.75 | 1.80 | 1.33 | 1.57 | 1.95 |

  §44's ×2 held by 1 % on E2M5's train *21.
- **The format** (formats/qworld.h, magic QWVE): qw_bmodel keeps its 40
  bytes.
  - `hull1` and `hull2` are int16. BSP29's clipnode children are 16-bit, so
    any index fits; the cook checks it. engine/clip.s loads them with `lh`
    (`BM_HULL2` 30), at the same cost.
  - `num_records` is gone: the engine never read it.
  - Three new fields, each the most over every place: `room_recs` (bytes /
    4: a record's size is a multiple of 4), `room_verts`, `room_groups`.
    They are 0 for a model no drawn entity uses.
  - No byte more in RAM. The .wld is loaded whole, so a table of its own
    would have cost 0.2-0.8 KB a map.
  - The cook writes them on the assembled file, just before writing it.
    `qcook check` checks them: all three or none, and at least a record
    header per group.
  - `qcook bmf <file.wld> [-v]` measures a cooked world again against
    them; `-v` adds each model's cuts and §44's sample.
- **The margin: none.** The room is the exact maximum, without factor or
  constant: the cook's cut is the engine's, and the places are all the
  ones a mover can take. The fallback stays (§41): a cut that does not fit
  is given up, and the model is drawn whole until it moves.
- **The engine** (`bmf_reserve`):
  - it reads the rooms: no scratch, no cut, no path at the load;
  - only the models the spawned edicts draw get a room; §44 also gave one
    to START's absent episode gates;
  - the caches and their rooms are allocated as one block and the pool as
    another, because the arena rounds each allocation to 16 bytes (§44
    made four allocations a model);
  - the measure's code and `movers_path` remain only in the BMFCHK builds,
    where bmf_check needs the paths.
- **What it gives** (PCSX-Redux in fast forward, every map at its high
  levels; §44's columns from its table):

  | Map | §44 room | Room | Freed | §44 pool | Pool | §44 preload | Preload | §44's measure |
  |---|---|---|---|---|---|---|---|---|
  | START | 49 KB | 27 | 22 | 332 | 354 | 85 | 85 | 0.11 s |
  | E1M1 | 81 KB | 44 | 37 | 279 | 316 | 126 | 126 | 0.22 s |
  | E1M2 | 83 KB | 44 | 39 | 227 | 266 | 179 | 210 | 0.29 s |
  | E1M3 | 129 KB | 71 | 58 | 157 | 215 | 120 | 198 | 0.67 s |
  | E1M4 | 93 KB | 50 | 43 | 114 | 157 | 79 | 126 | 0.31 s |
  | E1M5 | 94 KB | 48 | 46 | 254 | 299 | 235 | 235 | 0.32 s |
  | E1M6 | 198 KB | 106 | 92 | 261 | 354 | 227 | 271 | 0.65 s |
  | E1M7 | 40 KB | 21 | 19 | 772 | 791 | 276 | 276 | 0.09 s |
  | E1M8 | 53 KB | 28 | 25 | 559 | 584 | 157 | 157 | 0.15 s |
  | E2M1 | 101 KB | 54 | 47 | 176 | 223 | 130 | 130 | 0.36 s |
  | E2M2 | 92 KB | 54 | 38 | 173 | 211 | 162 | 162 | 0.32 s |
  | E2M3 | 109 KB | 58 | 51 | 155 | 206 | 115 | 160 | 0.36 s |
  | E2M4 | 83 KB | 42 | 41 | 107 | 148 | 93 | 135 | 0.28 s |
  | E2M5 | 106 KB | 60 | 46 | 187 | 233 | 147 | 147 | 0.36 s |
  | E2M6 | 171 KB | 88 | 83 | 96 | 179 | 76 | 153 | 0.68 s |
  | E2M7 | 80 KB | 42 | 38 | 103 | 142 | 77 | 124 | 0.30 s |
  | E3M1 | 101 KB | 55 | 46 | 221 | 267 | 124 | 124 | 0.35 s |
  | E3M2 | 179 KB | 93 | 86 | 366 | 452 | 272 | 272 | 0.42 s |
  | E3M3 | 111 KB | 56 | 55 | 349 | 403 | 215 | 215 | 0.39 s |
  | E3M4 | 210 KB | 112 | 98 | 2 | 84 | 0 | 44 | 0.99 s |
  | E3M5 | 61 KB | 32 | 29 | 92 | 121 | 73 | 112 | 0.35 s |
  | E3M6 | 104 KB | 57 | 47 | 46 | 94 | 41 | 71 | 0.35 s |
  | E3M7 | 65 KB | 34 | 31 | 320 | 352 | 215 | 215 | 0.20 s |
  | E4M1 | 75 KB | 43 | 32 | 209 | 241 | 125 | 125 | 0.31 s |
  | E4M2 | 131 KB | 67 | 64 | 203 | 267 | 186 | 186 | 0.31 s |
  | E4M3 | 107 KB | 61 | 46 | 221 | 267 | 178 | 252 | 0.29 s |
  | E4M4 | 50 KB | 25 | 25 | 138 | 163 | 114 | 114 | 0.19 s |
  | E4M5 | 135 KB | 71 | 64 | 117 | 181 | 102 | 133 | 0.34 s |
  | E4M6 | 117 KB | 61 | 56 | 282 | 338 | 64 | 64 | 0.32 s |
  | E4M7 | 133 KB | 70 | 63 | 0 | 54 | 0 | 28 | 0.39 s |
  | E4M8 | 140 KB | 73 | 67 | 183 | 249 | 147 | 224 | 0.40 s |
  | END | 22 KB | 12 | 10 | 708 | 718 | 92 | 92 | 0.08 s |

  - Measured before the merge with §45. Merged, every pool is 106 KB
    larger again with the 3D gun: 160-897 KB over the 32 maps.
  - 10-98 KB freed a map, 48 KB on average; the pool grows by as much
    everywhere but on the two fullest maps:
    - **E3M4:** its stream staging buffer (16 KB) now fits, so its pool
      grows by the other 82 KB;
    - **E4M7:** the stream manager always keeps the arena's last 8 KB
      (`STREAM_KEEP`); E4M7's arena was already full before, so of the
      63 KB freed, 54 make its pool.
  - Every load is 0.08-0.99 s shorter (§44's measure, gone).
  - **The starved maps** now stream their monsters: E3M4 2 → 84 KB of
    pool (44 KB preloaded), E4M7 0 → 54 KB (28 KB), E3M6 46 → 94 KB.
    With §45: E3M4 190 KB (166 KB preloaded), E4M7 160 KB (145 KB), E3M6
    200 KB (149 KB).
- **Checking:**
  - **BMFCHK.** `BMFCHK=1 BENCH_MAPS=1 VIEWS=0 scripts/bench.sh` makes
    43,098 cuts at 33 places a way, and none is given up on the 32 maps.
    The fullest rooms reach 100 % on every map, for records, vertices and
    groups: a mover's maximum falls on one of the 33 places.
  - **The rooms at each load.** Every bench load checks the cooked rooms
    against the cuts where the models stand:
    - a still model's room must equal its cut, to the byte, which shows the
      cook's cut is the engine's;
    - a mover's room must hold its cut.
    Over the 32 maps, 72 still models match exactly, 1,117 movers fit,
    and none fails (`bmf room` lines otherwise).
  - **The spawn views against §44's commit** (its code and cooked worlds,
    the same bench): 126 of the 128 frame hashes are identical, so the
    brush models' pieces are the same. The two others differ only in a
    streamed model:
    - E2M7.v0 shows no brush model, and one more monster now resident
      (+244 polygons, a pool of 142 KB instead of 103);
    - E1M4.v0 has the same packets except one model's 41, which carry its
      skin at another place in VRAM (tpage and UVs).
  - **The physics.** `VIEWS=0 scripts/bench.sh` gives clipdiff (traces
    against the brush models through `lh`, boxes, lines of sight) and
    pmdiff, 0 mismatches.
  - **The cooked files.** All but the worlds are identical to §44's cook.
    A world differs only in its magic and its qw_bmodel table (the clip
    roots and the depths are the same). `qcook check` and `qcook bmf` pass
    on the 96 worlds.
  - **The links.** The stack has 19952 B (default) and 20368 B (release),
    against §44's 18192 / 18592: the measure's code left the game.
  - **After the merge with §45**, the same campaign: `BMFCHK=1` 43,098 cuts
    with none given up and the fullest rooms at 100 %; at the loads 72
    still models exactly their cut, 1,117 movers within theirs, none off;
    the same rooms, every pool 106 KB larger; the stack 20032 B
    (default), 20432 B (release).
- **Left:**
  - **E4M7 and E3M4** keep the smallest pools: 160 and 190 KB with the 3D
    gun, 54 and 84 KB when the 2D gun borrows its 106 KB (§45). A lower
    geometry level for those worlds is the lever left.
  - **The cut while a model moves** (§41, §44): its cost in play is still
    unmeasured.

## §47 — The 2D gun's frames: their opaque spans only (2026-10-19)

- **The ask**: less RAM for the 2D guns, every gun but the axe being symmetric when not firing:
  half of each frame kept, the other half rebuilt by symmetry. Measured
  first, then done losslessly instead.
- **Where the 2D gun's bytes are.**
  - VRAM: one slot of 68 × 66 texels (4.4 KB) and its CLUT (§38). A frame
    is copied into the slot when it is shown, so halving the frames would
    save at most 2 KB of VRAM.
  - RAM: 107 KB of packets (60 frames, 103 KB of them), lent by the stream
    only while the 2D gun is drawn (§45).
- **The symmetry, measured** (every frame, its mirror about the screen's
  centre):
  - the silhouettes mirror at 94-99 % of their texels (the axe 17 %);
  - but only 17-52 % of the texels equal their mirror's (the rocket
    launcher's 17 %, the super nailgun's 52 %), the colours 1.5-3.5 levels
    of 93 apart on average: the light comes from one side (Quake's shading
    table) and the skins carry asymmetric details;
  - a frame rebuilt from its half would show symmetric lighting: a change
    from Quake's look. Left out;
  - the nailguns' frames are no mirrors of one another either:
    - the nailgun's firing frames are near-copies as they are (frames
      1 / 7, 2 / 4 / 6 / 8, 3 / 5: 99-100 % of the texels equal);
    - the super nailgun's frames 1 / 7, 2 / 6 and 3 / 5 are closer
      mirrored (56-64 %) than as they are (45-49 %), far from equal.
- **The opaque spans instead.**
  - **Before:** each frame copied whole (GP0(A0h), its w × h texels). The
    frames are 41 % transparent (60,072 opaque texels of 101,312).
  - **Now:** a frame's chain fills its rows of the slot with 0 (GP0(02h):
    the transparent index), then copies its opaque spans: a rectangle per
    run of rows over the run's spans, a halfword on each side.
  - **The runs** (tools/qcook/game.c `ws_chain`) are chosen for the fewest
    words. A copy costs its three words and a tag per 255 words, against
    the transparent texels a wider span carries. A packet a row would cost
    as much as the whole frame (102 KB): its 16 bytes are the price of 32
    transparent texels.
  - **The fill unit.** The GPU fills whole units of 16 halfwords (x down,
    the width up), so the slot starts on 32 texels. A frame whose fill
    would pass the slot's end (the axe's frame 7, 68 texels wide) is
    copied whole, as is any frame where that is smaller; the slot keeps
    its 68 texels.
  - **Shared frames.** Frames of the same texels share a chain (the rocket
    launcher's 5 and 6).
  - **The engine:**
    - `gdata_ws_link` makes a shared chain's links absolute once;
    - `ws_draw` keys the slot's content by the chain, not the frame, so
      the slot is not copied again, and a chain is still never hung in
      two frames in a row.
  - **The format.** `QGM8` (QGM7 had no fill and no shared chain).
- **What it gives:**
  - the packets go from 107,220 B (53 sectors) to 78,540 B (39 sectors):
    55 chains (54 filled then spanned, 1 whole), 475 packets;
  - while the 2D gun is drawn, the stream lends 78 KB instead of 106 (START:
    `stream: pools 382 KB ...; 78 KB lent`, 354 KB before), and a map's
    start reads 14 sectors less (about 0.1 s);
  - the 3D gun, or none, is as before (the packets are not in RAM).
- **Checking:**
  - **The cook's chains** against the old file's, simulated on the host
    (with stale texels in the slot before each): every one of the 56 2D
    frames leaves the same texels in its part of the slot.
  - **The pictures.** `BENCH_GAME=1 VIEWMODEL=2 scripts/bench.sh`: the 25
    views' pktview pictures are identical to the previous commit's, byte
    for byte. pktview now applies GP0(02h) fills. The frame hashes change
    by design: the display list carries the copies.
  - **DuckStation**: the game bench with the 2D gun ran to `bench done`,
    with no GP0 error. Its first start that afternoon never booted, as
    since §42 (the cause was not found); every later one booted. A run
    stopped by `timeout`'s SIGTERM hangs in DuckStation's "graceful
    shutdown": stop it with `timeout -s KILL`.
- **Left:** the nailgun's near-identical firing frames could become
  small patches over a shared base (about 15 KB more), at the price of
  chains linked to one another at run time.

## §48 — The music: XA streams carrying their maps' assets, the SPU alternative, the release's title (2026-10-19)

- **The asks**: the music in XA instead of CD-DA, to end the cuts and the
  stutter when the stream reads during play; then, on the options: the data interleaved so the sound never cuts; Quake's theme under the
  release's menu; START no longer loaded before New Game.

### 48.1 What cut the music (measured)

The CD-DA path (§40.2), PCSX-Redux in fast forward, E1M1's monsters'
scenario with the music on the bench disc:

| Stage | Cost |
|---|---|
| `music_suspend`'s fade | 8 frames, 133 ms |
| `CdControlB(CdlGetlocP)` | 30 k cycles, 0.9 ms |
| `CdControlB(CdlPause)`: it waits for the pause's second response | **2.40 M cycles, 71 ms: about four frames frozen** (psx-spx: 64 ms at single speed, CD-DA's) |
| the reads (3 to 9 small assets, 31 KB: 0.1 s of transfer) | **1.4 to 1.9 s of silence** (86-114 vblanks): each sound piece its own read and seek, the seeks to the data and back to the CD-DA track |
| `music_resume` | 89 k cycles (three commands), then 133 ms of fade |

What the stream asks for (the bench's sets, every asset a map's entities
use): 0.6 to 1.1 MB a map at the models' high level, 0.7 to 1.4 MB per CD
track (the union of the maps that play it: track 4 plays on six maps,
track 8 on five).

### 48.2 The choices

An XA stream plays its music while the head reads the data sectors beside
it, so data laid out next to the music arrives with no seek. Data wanted
on demand must come round again: a carousel, the assets repeated over the
whole track. A stream lasts as long as its music (59 minutes for the ten
tracks): at double speed that is 530 000 sectors, the disc has 333 000, so
the streams are read at single speed (75 sectors a second, the CD-DA's
room). The options shown (XA with a carousel; the same with direct reads
for urgent assets; the music from an SPU ring; XA without the data) and
the choices made:

- **XA with a carousel per track, the default.**
- **The SPU ring, a build-time alternative** (both do not fit one disc):
  22 kHz, 2 s, in SPU RAM.
- **The models' three levels in one asset** (shared vertices; asked again
  with the risks spelled out: rendering and RAM identical, the carousel
  10-15 % longer, the format and the expansion's input new).

### 48.3 The XA streams (formats/qxa.h, tools/qcook/carousel.c)

- **A stream per track** (`MUSIC/XA_<nn>.STR`, mkpsxiso "mixed" files of
  2336-byte sectors: the subheader twice, then the data; mkpsxiso writes
  the sync, the header and the EDC/ECC, checked on a test image): one
  sector of four is the music, XA ADPCM at 37.8 kHz, stereo, 4-bit (2016
  samples a channel a sector, 18.75 sectors a second); the three others
  are Mode 2 Form 1 data sectors, the turn's sectors one after the other
  (56.25 sectors, 112.5 KB a second). The first sector holds the header:
  the turn's assets (STREAM_<n>.DAT's indices, their sectors) and a byte a
  turn sector naming its asset. Every sector's file number is the track:
  the drive's filter plays the track's own music only.
- **The encoder**: per 28-sample unit of a channel, the filter (the
  CD-ROM's four) and the shift with the least squared error, the residual
  rounded and encoded against the decoder's own history (psx-spx's
  decoder, its +32), the shifts tried around the one the unit's residual
  needs (a full search gives the same error). 1.5 s a track.
- **The tracks**:

  | Track | Maps | Music | XA's SNR | Turn: assets, KB | A turn |
  |---|---|---|---|---|---|
  | 2 | (the menu, the episodes' ends) | 308.6 s | 38.4 dB | none | |
  | 3 | (the intermission) | 146.0 s | 54.5 dB | none | |
  | 4 | START, E1M6, E2M6, E3M6, E4M6, END | 500.3 s | 37.1 dB | 127, 1546 | 13.7 s |
  | 5 | E1M4, E2M4, E3M7, E4M4 | 365.4 s | 48.3 dB | 125, 1510 | 13.4 s |
  | 6 | E1M1, E2M1, E3M1, E4M1 | 444.6 s | 51.0 dB | 71, 844 | 7.5 s |
  | 7 | E1M7, E2M7, E4M7 | 518.2 s | 57.1 dB | 120, 1644 | 14.6 s |
  | 8 | E1M2, E2M2, E3M2, E3M4, E4M2 | 335.9 s | 54.6 dB | 133, 1614 | 14.3 s |
  | 9 | E1M3, E2M3, E3M3, E4M3 | 388.9 s | 57.3 dB | 127, 1528 | 13.6 s |
  | 10 | E1M8, E4M8 | 212.6 s | 58.7 dB | 102, 1088 | 9.7 s |
  | 11 | E1M5, E2M5, E3M5, E4M5 | 317.4 s | 57.4 dB | 124, 1564 | 13.9 s |

  265 366 sectors in all, what the CD-DA took; the release's image is
  735 MB (738 with the CD-DA), one data track.
- **A map's assets in the cook**: its monsters' kinds (each kind's
  group, the gibs and the backpack with any monster, zom_gib with the
  ogres', demons' and shamblers' melee: SpawnMeatSpray), its items'
  models (`items_streamed`), its entities' and ambients' sounds
  (`sound_map_streamed`: the QC's defaults for `sounds`, item_health's by
  its flags, every func_door's key sounds: doors.qc gives them to every
  door), leaving out the entities single player never spawns (flagged not
  easy, not medium and not hard). Against the sets the engine asked for on
  the 32 maps (the bench, every asset's distance after the spawn): none
  missing; the cook's hold 1 to 17 more (an entity of one skill, START's
  backpack and lava ball).

### 48.4 The models' three levels in one asset (formats/qstream.h qst_shared)

- **Nested levels.** decim.c's half-edge collapses go cheapest first in
  the same order at every level and stop sooner for a higher one: a lower
  level keeps a subset of a higher level's vertices (the cook checks it).
  The vertices are numbered by the lowest level that keeps them (low's in
  low's order, then medium's others, then high's): a level's vertices are
  the first `num_verts` of each frame. Every level's asset in
  STREAM_<n>.DAT is cooked from that numbering.
- **The shared asset**: qst_shared, the high level's frames, the levels'
  records (a level with another's triangles shares its block), the images
  (the same at every level: checked). `qst_level_model` makes the level's
  header and `qst_assemble` copies its bytes from any range of the shared
  asset (a sector at a time, as captured): the frames' first words, the
  level's records, the images. The cook assembles every level from the
  shared asset sector by sector and compares it with the level's asset:
  byte for byte, on every model. `qcook stream` writes the three levels
  and the pool (cooked/xa/pool.dat, the carousels' source) in one run.
- **The cost**: the pool 2349 KB against 2215 KB for high alone (+6 %).
  Low's assets are unchanged; 27 medium and 32 high models only have their
  vertices renumbered (the same sizes, the same records but their vertex
  indices): the monsters' scenarios hash the same frames on E1M3 and E1M5
  at high and at medium.

### 48.5 The engine: assets taken as they pass (engine/music.c, stream.c)

- **The music** (`music_play`): the stream's header read (at a map's
  load, a track change), the drive set to single speed, CdlModeRT (the
  drive plays the music's sectors), CdlModeSF (the track's file number),
  CdlModeSize (each data sector with its header), then CdlReadS. The CD
  interrupt reads a data sector's 12 header bytes, works out its place in
  the stream from its address, and hands it to `stream_xa_sector`. Past
  the music's last sector the drive goes back to the stream's start (the
  track loops, Quake's CDAudio_Play(track, true)). No pause during play:
  the 71 ms freeze is gone with the cuts.
- **The captures** (stream.c): an asset the carousel carries is wanted
  from farther than a read's (4000 units for a model, 3000 for a sound: a
  turn's run ahead), given its room as for a read (RAM and VRAM, or its
  SPU place), then captured: the interrupt takes each of its sectors in
  order from its first into a ring of eight (the staging buffer), by DMA
  in the background (CdGetSector2: a word every 16 cycles, the CPU running
  between; done long before the next sector); a sector missed (the ring
  full) starts the asset over at its next turn. Each frame drains the
  ring: a model's level assembled into its block (qst_assemble), a sound's
  samples to SPU RAM; the last piece in, the asset installed as a read's.
  Eight captures at most at once, armed after a residency pass or a
  capture's end.
- **What the carousel lacks** is read as before (the music suspended): an
  asset of no turn (none on the 32 maps), the 2D gun's packets lent by the
  stream (§45) when they come during play.
- **Measured** (PCSX-Redux, fast forward, the music on the bench disc):
  E1M1's monsters' scenario took 16-17 assets from track 6's carousel and
  read none during play; E1M3's (track 9, a turn of 13.6 s) took 46: from
  their arming, 7 s at the median, 0.3 to 13.6 s. The interrupt costs 333
  cycles a data sector (56 a second), the carousel 77 cycles a quiet frame
  and 13-16 k cycles a frame on average in E1M3's crowd (arming every free
  slot each frame cost 117 k: each stuck asset's room measured again);
  installing a model costs what a read's does.
- **RAM**: the stream's header 2 KB in the game arena; BSS and code about
  5 KB, given back to the stack by the arenas (the level arena 1240 KB,
  the game arena 115 KB: its boot tables 114 KB).

### 48.6 The SPU music, the build's alternative (formats/qspu.h)

- `Q1PSX_MUSIC_MODE=spu` (the `release-spu` preset): each track SPU ADPCM
  at 22.05 kHz stereo (`MUSIC/SPU_<nn>.DAT`), in chunks of 0.325 s a
  channel; the last two voices play a ring of six chunks a channel at the
  top of SPU RAM (2 s, 48 KB, taken from the streamed sounds' pool; the
  static sounds keep twelve voices). The ring's first and last blocks get
  their loop flags at the upload; after the track's last chunk its first:
  the track loops seamlessly.
- **The drive shared**: the music refills two chunks at a time when no
  read is in flight; a read (an asset's, a sound's piece, the lent block)
  starts when the ring holds the music for it, its seeks (250 ms each, the
  music lying next to the streamed assets) and the refill after
  (`music_read_ok`). Where the voices are comes from root counter 2,
  sysclock / 8, whose clock the SPU shares (44.1 kHz is sysclock / 768: a
  22.05 kHz sample is 192 ticks): no drift, no SPU interrupt.
- **The encoder** as the XA's (the SPU's five filters): 34.3 to 57 dB a
  track (track 2: 34.3, 4: 38.5, 5: 43.1, 8: 49.5, the others 52-57), 5 dB
  over the sounds' encoder (which truncates). 84.6 MB for the ten tracks;
  the release-spu image is 213 MB.
- **Measured**: the release's menu and START; E1M1's monsters' scenario
  read 19 assets during play with the ring never dry; START's preload 20
  assets (23 in the XA build: 48 KB less of SPU pool).

### 48.7 The release's title: no map before New Game

- **What Quake does**: quake.rc ends with `startdemos demo1 demo2 demo3`:
  the demo loop runs under the menu. demo1.dem's first line is its forced
  CD track, 2, which svc_cdtrack plays in place of E1M3's 9
  (CL_PlayDemo_f, cls.forcetrack): Quake's theme plays under the menu
  until demo2 (E1M4, track 5), then demo3 (E1M6, track 4). Quake did play
  the theme under its menu, as the first demo's music.
- **With no demo and no world** (con_forcedup), the console is down, and
  M_Draw draws the console's background in place of the faded game. The
  port has no demos and now loads no map before New Game: the release
  boots to `gfx/conback.lmp` stretched over the screen (Quake's rows ×
  240 / 200) with the menu over it, and track 2 looping. Quake also draws
  its version number in the picture's corner and the console's text
  (there is no console): neither is drawn. ESC closes the menu (the
  background alone, SCR_DrawConsole's), START opens it again; no game runs
  before New Game.
- **The menu's own bank**, `MAPS/MENU.SND`: its three clicks and CD track
  2 (`qcook menu`, which also cooks `CONBACK.DAT`: the picture and an
  opaque copy of the palette, loaded where the map's texture pages go).
- **New Game loads START once**: the boot logs one `START.SND` and one
  `START_G2.WLD` (START was loaded under the menu at boot, then again by
  New Game). The settings changed before New Game apply at that load.
- **The disc's order**: the maps, then the streamed assets and the music
  (the XA streams' 600 MB had pushed the maps away from the game data: the
  boot 2.7 s longer on PCSX-Redux; 950 vblanks to the menu now, 970 with
  the SPU music).

### 48.8 Checking

- The cook: the levels' nesting and the assembled assets checked on every
  model; the carousels' sets against the engine's (32 maps, none missing);
  a test image's mixed sectors (the subheaders, the data, the Form 1 EDC).
- The monsters' scenarios without music (`MONSTERS=1 MAP=E1M3|E1M5
  MODELS=2|1 HASHONLY=1`): the same `H` lines as before the shared
  numbering and the capture code.
- `MONSTERS=1 MUSIC=1 MAP=E1M1|E1M3`: the `stream: capturing` / `captured
  ... in N vblanks` / `stream: in` lines, no `stream: reading` during play,
  no `not one this engine reads`, the `xa cost` lines; `MUSIC_MODE=spu`:
  `stream: in` lines, no `ran dry`.
- The releases (XA and SPU) on PCSX-Redux with its UI and a Lua script
  (`PCSX.GPU.takeScreenShot()` at chosen vblanks, the pad driven by
  `setOverride`): the menu over the console's background, ESC (the
  background alone), START (the menu again), Single Player, New Game
  (START drawn); the boot logs (`music: track 2 ...`, then track 4 with
  its carousel; one START load); the first frame's vblank.
- Every link's `ramcheck`: the release 17.3 KB of stack, release-spu 17.7.

### 48.9 Left

- **The ears**: the XA (37.8 kHz, 4-bit) and the SPU music (22 kHz)
  against the CD's 16 bits; the music's level against the effects (the
  SPU music set as loud as the CD input was). The loop points (the
  decoders' history jumps back to the track's start).
- **DuckStation and the machine**: not run (DuckStation did not start on
  this host, §42). The XA with data at single speed, the speed switches
  around the loads, a worn drive (XA sectors carry no error correction: a
  drive that skips them skips the music, not the data).
- **The carousel's waits**: an asset comes on its next turn, up to 14.6 s
  (track 7): a monster can stay undrawn, a sound unheard, that long after
  it is wanted. The reach (4000 / 3000) is a first guess; shorter turns
  would come from fewer or smaller assets (the entities' sounds are half
  of a turn).
- **The lent block** (§45) read during play leaves the XA stream: a cut
  (when the 2D gun is chosen during a game).
- **The intermission's and the finale's tracks** (3, 2) are streams of
  their own: a seek and no carousel while they play.
- **The bench's `BENCH_GAME=1` build fails the stack check**, as it did
  before this work (6.8 KB of stack at 000b210, 5.2 KB now): the game
  scenario's code; its own threshold (`Q1PSX_STACK_MIN`) or less code in
  that build.
- **The stack's margin** is about 1 KB over the 16 KB threshold in both
  releases.

### 48.10 The first build after a pull (2026-10-19)

- **The report**: the release built with SPU music instead of XA, no background under the menu, no theme. That `build-release` was XA
  (`Q1PSX_MUSIC_MODE=xa` in its cache), but its disc was 110 MB with an
  empty `\MUSIC`, and no `CONBACK.DAT` nor `MENU.SND`: the boot found no
  stream, no background, the menu's bank missing.
- **The cause**: the SDK's `psn00bsdk_add_cd_image` writes the mkpsxiso
  project at CMake's configure step, from the cooked files there then. A
  pull brought new cooked files (the music's streams, the menu's bank and
  background); the build cooked them after the configure, and the disc
  went without them (a second build would have taken them). The cook also
  made both music formats, the SPU's listed last.
- **The fix**: the project's file list is made when the image is built,
  after the cook (`scripts/cd_image.cmake`, run by the image's own custom
  command: the same lists and level filters, and mkpsxiso after it); the
  configure's globs only rebuild the image when a manual cook changes the
  files. The cook makes the build's music only (`COOK_MUSIC_MODE`, from
  `Q1PSX_MUSIC_MODE`; both when run by hand) and says which.
- **Checked**: the music's, the background's and the menu bank's cooked
  files moved away, a new build directory configured, then built: its
  disc carries the ten XA streams, `CONBACK.DAT` and `MENU.SND` at that
  first build (`q1psx disc: 32 maps, 10 music tracks (xa)`); the release
  on PCSX-Redux shows the menu over the background, `music: track 2`;
  track 2's sectors taken from the image and decoded by ffmpeg's own XA
  decoder (its `psxstr` demuxer): 37.7 dB against the source, sample for
  sample.

## §49 — The movers' re-cut in play: measured, then cut down (2026-10-19)

- **The ask**: measure, then reduce, what `bmf_draw`'s re-cut of a moving
  brush model costs in play (§41, §44's "Left": 0.1-1 M cycles a cut at
  the load, never measured in play).
- **The scenario**: `MOVERS=1 scripts/bench.sh` (engine/main.c
  `bench_movers`):
  - the movers: E1M6's train *39, E1M3's biggest train (*3), E3M4's
    biggest door (*71);
  - each one started, then watched from a spot in the open out of its box
    at one of its first 8 places (x or y, both ways, five distances, its
    centre's height or 48 units over its foot), the one that sees its
    centre at the most places of its way and halfway between; the camera
    turns to follow it;
  - one 60 Hz tick a frame for 600 frames: each frame it moved and is
    drawn cuts it again;
  - per mover a bench line (the frame's and the ents' stages) and a
    `bmfcut` line: the cuts, their cycles in a frame with one, the dearest
    cut, the frame with a cut and without;
  - then 256 frames turned a quarter away 16 frames in 32 (the door
    started again);
  - every 16th frame's hash, folded into a `bmfhash` line: the
    identical-display-list check of every change below;
  - `MAP=` keeps one of the three and dumps its frame 240 (pktview);
    `SAMPLE=1` samples the PC over the 600 frames.
- **What it cost** (PCSX-Redux in fast forward; §44's C cut):

  | Mover | A cut, average / dearest | The frame with a cut | Without |
  |---|---|---|---|
  | E1M6 train *39 | 916 k / 1235 k | 1735 k (3.97 vbl) | 642 k |
  | E1M3 train *3 | 372 k / 671 k | 946 k (2.29 vbl) | 505 k |
  | E3M4 door *71 | 658 k / 663 k | 1180 k (3.00 vbl) | 521 k |

  - A frame at 60 fps has 564 k cycles. At the default 32 Hz tick about
    one frame in two carries a cut.
  - A model is cut again whenever it has moved and is in the PVS in front
    of the eye: behind a wall, or out of the view to the side, too.
  - Where it went (E1M6's *39, the PC sampler): the descent corner by
    corner about 45 % of a cut, `put_record` 32 %, the corners' copies
    10 %. A cut there takes 172 polygons in 120 records, 997 record-node
    and 1174 polygon-node tests, 4195 corners' distances, 21 splits, and
    makes 186 pieces in 135 records.
- **The options**:
  - A, each record's sphere down the BSP before its corners: done at
    once, being exact;
  - B1, the descent in hand-written asm;
  - B2, the whole cut in hand-written asm;
  - D, no cut out of view;
  - E, inexact ones (a cut every N ticks, or spread over frames).

  B2 and D were taken.
- **A: the record's sphere first** (04e736d). A node the record's
  bounding sphere lies wholly on one side of sends every polygon of it
  that way: each corner's distance has the centre's sign, since
  |d(p) - d(c)| <= |n| |p - c| < 4097 sr (the cook's radius is
  ceil(rad + 1), a normal at most 4096.87 long). The polygons start where
  the sphere stopped; a record wholly in a solid leaf makes no piece.
  807 k / 368 k / 534 k a cut (-12 %, -1 %, -19 %), the same frames.
- **D: no cut out of view** (7133b36):
  - **A bug found on the way.** `bmf_seen` judged a model by the leaves of
    its cached pieces even when it had moved since the cut (it moved
    while not drawn). A train that left the PVS and came back elsewhere
    stayed undrawn until its old place was seen. A stale cache now
    answers with the box's leaves, or, when they were cut short at 8,
    with the box's top node (mark_pvs stamps a visible leaf's
    ancestors).
  - **The test.** `bmf_draw` no longer cuts a moved model again when the
    record loop would cull every record of its pieces (`all_culled`):
    - r_world.s RECLOOP keeps a record whose sphere passes the four planes:
      right / left z -+ x + 2.125 r >= 0, bottom / top 3z -+ 4y + 8 r >= 0,
      in view space x6, IR saturated;
    - every piece record's centre is in the model's box, and its radius is
      at most `rmax`: each source record's box half diagonal + 3, made once
      per model at the load (`model_d2`);
    - so the box in view space (its centre, the extents |rot| h, 2 more for
      the roundings, each bound saturated as IR is) wholly behind one plane
      by more than 2.125 or 8 rmax culls them all;
    - no leaf the pieces would be drawn in can be wholly in view either: it
      holds them.
  - **Its effect.** `NOBMFCULL=1` is the A / B: the same frame hashes both
    ways. In the turned pass, E1M6's *39 was cut 192 times instead of 256
    and E3M4's *71 97 times instead of 162; E1M3's *3 stayed at 23, mostly
    out of the PVS there.
  - **A first bound was too loose.** It took 1.75 x the cooked radius
    (a cube about the sphere) and put off nothing on E1M6.
- **B2: the whole cut in hand-written asm** (`engine/bmfrag.s`,
  `bmf_cut`):
  - **The record level.** The model's nodes come in build_nodes' order.
    For each record: its sphere from the top node (axial planes on the
    CPU by sc x 4096, general ones on the GTE with sc as the first row).
  - **The polygon level, as it is.** Each polygon is unpacked into the
    next piece's slot, in a loop over its corners (their PS1 order a byte
    each), with its box (min / max per axis, branch-free, a triangle's
    taken as a quad's with its corner 2 again as the fourth).
    - Its descent: an axial plane against the box (2 halfwords, 2
      compares); a general plane on the GTE, with the corners as the
      rotation matrix's rows and a quad's fourth as the light matrix's
      first row, one or two MVMVAs of the normal.
    - In a leaf it is the piece, as it is, in the slot it was unpacked
      in: no copy.
  - **Both sides of a plane.** The polygon goes the C's way: the corners'
    distances again, the split (into the scratchpad's two buffers, the
    back piece pushed), the crossing exactly as `cross`, the cut pieces
    corner by corner.
  - **The records.** `put_pieces` / `put_record` follow the C's batching,
    so the bytes match. A lone piece as it was (most records of a big
    mover) takes a short way: its sizes known, its box its own, its
    group put at once.
  - **Squares and roots.** The radius sums the GTE's SQR of each corner's
    differences. The root goes bit by bit from the GTE's leading-zero
    count.
  - **Scratch space.** The scratchpad's per-frame part (0x000-0x2ff, as
    the physics borrows it) holds the scratch buffers, the distances and
    the state. The rotation and light matrices are borrowed too:
    r_render loads them again.
- **Checking the asm** (`BMF_C=1`: the C alone, `BMF_C=2`: both, cut by
  cut, `cut_check`):
  - the C cut, `cut_model_c`, is the specification. It and the asm share
    `bmf_pieces` / `bmf_pending`, and the C is not in the default builds;
  - the C runs first, the asm second into the same cache, each with the
    nodes' group fields poisoned first;
  - the result, the overflow flag, the piece count, the lengths and every
    byte (records, pool vertices, groups, the nodes' groups) are hashed;
    on a mismatch, a `bmfasm MISMATCH` line with the first record bytes
    that differ;
  - the check build carries both cuts: bench.sh lowers its stack floor to
    10 KB (`STACK_MIN` overrides it).
- **Pitfalls found** (all caught by the C check or the interpreter):
  - **A load in a branch's delay slot read by the target's first
    instruction.** The record's light padding then counted from a stale
    value and cleared RAM, under the interpreter (as hardware would); the
    dynarec hands the new value over. `tools/hazcheck` checked only the
    next instruction in address order; it now checks every static branch
    target too (a load in the delay slot, the target's first instruction
    reading it): none in the engine; a deliberate one in `cut_record`
    failed the link.
  - **A four-instruction macro in a delay slot**: only its first
    instruction ran.
  - **`put_pieces`' done mark** (bit 24 of a piece's n / cut / quad word)
    read into a quad test without a mask.
  - **The record pointer** never passed to `cut_record`.
  - **PCSX-Redux keeps a word's upper half in the GTE's lone 16-bit
    registers** (VZ0, R33, L13): they are loaded from sign-extended
    halfwords, as r_world.s does.
- **The stack's room.** The asm took 7.6 KB at first, 2 KB more than the
  C cut it replaces, and the default build fell under the 16 KB floor.
  Then master brought the rooms' measure into the cook (§46, which took
  most of `bmf_reserve`'s code away) and the music's XA streams (§48):
  master alone leaves 17288 B in the default build. What fits on it:
  - the corners in a loop, the box's axes in a loop (the unrolled ones
    were 3 % faster and 530 bytes more);
  - the crossing's fields in two loops;
  - the load-time code compiled for size (`BMF_COLD`: `bmf_reserve`,
    `model_nodes`, `build_nodes`, `model_d2`, `all_culled`...);
  - the lone piece's short way through the general way's radius, header
    and polygon (its own copy was 4.5 % faster, 400 bytes more again).

  The rooms now: 16824 B (default), 16872 B (release, release-ultra).
- **What it gives** (PCSX-Redux in fast forward, the same frames):

  | Mover | C (§44) | + A | + the asm | The frame with a cut | Without |
  |---|---|---|---|---|---|
  | E1M6 train *39 | 916 k | 807 k | 482 k (-47 %), dearest 803 k | 1292 k (3.11 vbl) | 647 k |
  | E1M3 train *3 | 372 k | 368 k | 218 k (-41 %), dearest 396 k | 838 k (2.10 vbl) | 498 k |
  | E3M4 door *71 | 658 k | 534 k | 361 k (-45 %), dearest 370 k | 951 k (2.03 vbl) | 608 k |

  - The C columns are measured before the merge with master (§45-§48);
    the asm's after it. After the merge, the C with A measures
    786 k / 352 k / 516 k; the asm before the merge, in its faster form,
    measured 469 k / 211 k / 346 k.
  - With D, a moved model out of view is not cut at all (above).
  - `BMFCHK`'s 33 cuts a way on the 32 maps: 4.42 G cycles down to
    3.12 G (C's own part included: `cut_where`). Before the merge, the
    load's rooms' measure went 375 M down to 255 M (3844 cuts over 33
    loads): the cook does it now (§46).
  - Where an asm cut goes (E1M6, the sampler, before the merge):
    - the polygons' descent about 15 %;
    - unpacking with the boxes 13 %;
    - the records' radius and root 14 %;
    - the records' setup and sphere 14 %;
    - the rest spread out.

    The work itself is what is left: about 2170 node tests, 172 polygons
    and 135 records a cut.
- **Checking:**
  - `MOVERS=1`, the same `bmfhash`:
    - before and after A (the first pass then: `dd76ed86`, `d60664b3`,
      `9ae23bc0`);
    - D on and off, and the C and the asm (both passes: `057752f4`,
      `9e700ae0`, `184a8cda` before the merge; after it the C and the asm
      still alike, `057752f4`, `59299081`, `88efca4d`, master's streaming
      changing E1M3's and E3M4's frames);
  - `BMF_C=2`, no mismatch:
    - before the merge: `MOVERS=1`, 2765 cuts; `BMFCHK=1 BENCH_MAPS=1
      VIEWS=0`, 48139 cuts over the 32 maps (the rooms' measure, the 33
      places a way, the loads' cuts); under the interpreter
      (`EMU_ARGS=-interpreter`, START and E1M6), 2420 cuts;
    - after it, the final asm: `MOVERS=1`, 2114 cuts; `BMFCHK`, 44295
      cuts over the 32 maps; under the interpreter, 2215 cuts;
  - the asm against the C on the frames, after the merge:
    - START's views (`HASHONLY=1`): 12 frames;
    - the entities' scenarios (`ENTS=1`): 23 frames;
    - the same hashes both ways;
  - `BMFCHK=1 BENCH_MAPS=1 VIEWS=0` with the asm:
    - no cut given up on the 32 maps;
    - the cook's rooms against the asm's cuts at every load (§46): 75
      still models' rooms their cut to the byte, 1122 movers' holding it,
      none off;
  - the links (hazcheck, ramcheck) of the default, release,
    release-ultra and bench builds;
  - not checked on the real GPU or DuckStation.
- **Left:**
  - **The work itself.** A model node's cooked box could start its
    records' spheres lower: about 5 % on E1M6, if every box holds its
    subtree's faces, which needs a check at the load. Records that stay
    wholly in one leaf could also be moved rather than cut again: their
    pieces are the polygons moved, except where sc's rounding changes,
    and the cache's offsets shift as soon as one record's cut changes.
  - **The stack's room**: 440 B over the floor in the default build.
    The unrolled corners and box (3 %) need 530 B more, the lone piece's
    own radius, header and polygon (4.5 %) 400 B more again.
  - **The worst frames stay over budget**: E1M6's train still takes 482 k
    a cut, about 40 % of the CPU at the 32 Hz tick while it is in view.

## §50 — Monsters through a wall, paused, faded; what the stream keeps near the player (2026-10-19)

- **The asks**:
  - The dead monsters after the living ones, unloaded after a few seconds with
    a fade.
  - A monster not loaded paused, so that no unseen monster hits the player.
  - On residency: loading near the monsters was the idea, as Mario 64 does, where
    the behaviour seemed random, too wide an area perhaps loaded.
  - On E1M1, once the nailgun is taken, the soldier and the ammo behind
    the wall that has not opened yet are seen through it.

### 50.1 Through the wall: the order within a leaf

- **Reproduced** with a new bench mode: `MAP=E1M1 BENCH_MAPS=1
  VIEW=112,2352,62,0,0 TICKS=128 scripts/bench.sh` puts the player at
  the view, streams in what he would have around him, runs the game and
  dumps the view every 8 ticks (`E1M1.v12t<tick>.png`).
  - The player at the nailgun touches `trigger_once` *11, which opens the
    `func_door_secret` *10 (spawnflags 3). The door first backs 16 units
    into the room behind it: `t_width` is the BSP's 14 plus
    Mod_LoadSubmodels' unit on each side. It waits a second, then slides
    away.
  - From tick 8 the soldier and the ammo box are drawn over the closed
    door.
- **The cause**: within a leaf, the render list's order did nothing of
  what Quake's z-buffer does.
  - The brush models' pieces (§41) were added first and every alias
    model after them. The list's head is drawn last: the walk goes front
    to back and prepends each packet. So an alias model was drawn over
    every piece of its leaf.
  - Closed, the door's west face lies in the nailgun room's leaves (443,
    446), nearer than the soldier's: drawn over him by the leaves' order.
    Backed 16 units, the face lies in the soldier's leaf (440) and the
    ammo box's (439), and both were drawn over it.
- **The fix** (engine/rlist.c, engine/bmfrag.c):
  - The brush models go into the lists first (`ents_render_bmodels`,
    before `game_render`). The projectiles, the items and the monsters
    follow; the particles come last (`game_render_particles`).
  - An alias model (its box's centre) or a particle takes its place among
    its leaf's pieces by its model's BSP walk. Take the deepest node that
    the point's way down shares with the pieces' node (the node's DFS
    range: `bmf_node.end`). A point beyond that node from the eye comes
    before the subtree the walk takes after it: those pieces are drawn
    over the point. A point on the eye's side comes after them.
  - A model's pieces over the point come first in the list: the first one
    under it ends that model's.
  - The alias models of a leaf among themselves by their depth (their
    GTE translation, in units of two world units), the nearest drawn
    last. Before, the monsters were drawn over the items and the items
    over the projectiles, whatever their depth.
  - The particles are drawn over the alias models of their leaf (they
    came under them), and among the pieces where they stand.
- **The cost, off the common way**:
  - `rl_add_pieces` writes what it wrote: the model goes into the item's
    free `colour` byte, stored with the type.
  - Each list's head is recorded once a frame (`rl_pieces_done`), in
    `r_lists`' upper half (the walk's ids are 7 bits).
  - The rest runs only when an alias model or a particle comes to a leaf
    with pieces (`place_by_pieces`, compiled for size): the pieces'
    nodes found by their records' offsets, the point's way down taken as
    deep as needed, its sides cached. A model whose box the eye-to-point
    segment's box misses is skipped.
  - The nodes are 16 bytes now (an index is one shift): `draw_node` is
    as cheap as before or cheaper.
- **Measured**: `BENCH_MAPS=1 HASHONLY=1 PROFILE=0 scripts/dlcheck.sh`,
  the 128 spawn views against ce9220f:
  - +0.16 % of the frame cycles in all; −0.3 % to +2.4 % per view;
  - the dearest view is E3M3.v1 (+10.6 k cycles): three monsters in a
    leaf with five brush models' pieces, about 550 cycles per monster and
    model (the plane tests, in C);
  - 22 of the 128 views' lists differ (their order). START's and E1M4's
    pictures are identical to before. On E3M3.v1 a monster that showed
    through a closed door before is hidden now (the same bug); on
    E3M3.v5 a health box's 200-pixel sliver goes under a step's edge.
- **Room**: 3 KB of code. The level arena goes 1240 → 1237 KB; the
  release's stack room is 17464 bytes.

### 50.2 The paused monsters

- `ents_tick`: a monster whose model is not resident (`stream_model`)
  neither thinks nor moves. It stays solid and can be hurt; a death
  waits for its model like the rest.
- §40's choice ("far monsters run Quake's AI as they are, only their
  drawing waits for their model") is undone.
- **Checked**: `MONSTERS=1 MAP=E3M4` (the smallest pool, 183 KB). The
  `mon` lines now print `model in` / `model OUT (paused)` and the want.
  - The zombie's line reads `OUT (paused)` for 90 ticks: state 0, frame
    0, the same position, the player's health unchanged. It acts once its
    model is in.
  - The fiend starts paused, is loaded, fights.

### 50.3 The dead

- **The body goes 6 s after the death** (`CORPSE_TICKS`), and so does a
  gibbed monster's head. `pain_finished`, unused once a monster is dead,
  holds the time (set in Killed).
  - The last second (`CORPSE_FADE`) darkens the body to black. The GPU
    has no alpha to fade with: its blend modes add, subtract or halve the
    background.
  - Until its end the body stays in the entities' active set, then
    `ents_tick` removes it.
  - Quake keeps its corpses, as SOLID_NOT; they no longer take damage
    (Killed's `DAMAGE_NO`). Nothing in the game reads them.
- **The dead's assets come after the living's**: a body's or a head's
  model, and a gib's, is wanted at `STREAM_DEAD` (0x8000) + its distance.
  - That is past every reach: never read for, kept while nothing wanted
    more needs its room, evicted first.
  - A kind is wanted at its nearest *living* member's distance.
- **Checked**: `MONSTERS=1 MAP=E1M1` / `E3M4`, with two new lines:
  - a dump in the fade's middle (`m<map>_<kind>_d.png`: the body nearly
    black);
  - `mon body: gone at t ...`, 360 ticks after the death.
  - On E3M4 the fiend's model is evicted for the living 2 s after its
    death (its want `32818`). Its body vanishes then, before its six
    seconds.

### 50.4 Residency: what is loaded today

How the stream chooses (§40.2, §43, §45, §48):

- **The wants**: every 8 frames a pass gives each asset a score, the
  smallest the most needed.
  - A monster kind's model, head and sounds are scored at the Chebyshev
    distance (the largest of |dx|, |dy|, |dz|) from the player to the
    kind's nearest member, through any wall.
  - An item's model is scored at its own distance. The keys', the rune's
    and the weapons' are pinned: always in.
  - An entity's sounds are scored at its box's centre.
- **The loads**: the most wanted asset that is not in is fetched first,
  within its reach.
  - The reaches are 2500 units for a read (1500 for a sound), and 4000
    units (3000 for a sound) for a capture from the music's carousel.
  - Room is made by evicting what is wanted less.
  - A capture waits for the asset's turn in the carousel: up to 14.6 s.
- **The unit is the kind**: one model serves every member of its kind.
  A kind costs 50-100 KB of RAM (its model with its skins and its head).

**Measured** (`BENCH_MAPS=1 RESIDENCY=1 scripts/bench.sh`, "res"
lines):

- The method: the player is put at each of a map's items in turn (up to
  24 a map; they lie along his way). A map start's reads are simulated
  there with the game's wants: nearest first, the models into the RAM
  pool and the sounds into the SPU pool, while they fit. The PVS comes
  from the player's leaf.
- **The zone is most of the map**:
  - within the 2500-unit reach: 5 to 74 living monsters (E4M3 74, E2M7
    53, E1M3 45);
  - of these, in the PVS: 0.3 to 10 (3 to 6 on most maps);
  - 2 to 5 kinds are wanted, of which 1 to 2.4 are in the PVS.
- **What is in**:
  - the wants within reach total 300-870 KB, against pools of 154-892 KB;
  - where everything fits (21 of 32 maps), every visible kind is in;
  - on the others, at 7-15 spots of 32 (E2M2, E2M3, E2M4, E2M6, E2M7,
    E3M4, E3M5, E3M6) a kind in the PVS is not in. The kinds behind
    walls, nearer by distance, took its room.
- **Other rankings, simulated** (`RESIDENCY=2..4`, bench modes kept in
  37489f1 only): the count of spots where a kind in the PVS is not in, of
  1020.

  | Ranking | Spots |
  |---|---|
  | Today (the distance) | 90 |
  | A short radius (models within 1200 units, Mario 64's way) | 96 |
  | The PVS first: a monster outside the player's PVS 2500 units farther, the same reach | 59 |
  | The PVS first, and the heads and gibs under the models (+2500) | 41 |

  The short radius drops the kinds the player sees across a big room
  and keeps the ones behind the walls nearby. The PVS is the map's own
  "near": what can be seen from where the player stands.

### 50.5 The PVS first, the heads under the models (the choice made)

- **The choice**: of the four rankings above, the last one.
- **The scores in tiers** (engine/stream.h): a score is a tier plus a
  distance in world units (`STREAM_DIST`, the low 16 bits).
  - Within a tier the nearest comes first; every tier comes after the one
    above it.
  - The reaches apply to the distance alone: the same physical zone as
    before. Everything at `STREAM_DEAD` or under is never read for.
  - The tiers, from the most wanted:
    - 0: what the player's PVS holds: a kind with a member there, a
      streamed item there;
    - `STREAM_HIDDEN`: a kind with no member in the PVS, an item out of
      it; and a visible kind's head and the gibs;
    - 2 × `STREAM_HIDDEN`: a hidden kind's head;
    - `STREAM_DEAD`: the dead's remains (§50.3).
  - The pinned items (the keys, the rune, the weapons) stay at 0, always
    in: the earlier rule.
- **The PVS** is the last frame's stamp (`leaf->visframe ==
  r_state.visframe`): the camera's leaf, the player's eye. It is read in
  `monsters_stream_wants` through the edict's cached leaf
  (`ent_model_leaf`): one comparison per monster and per pass (every 8
  frames). At a map's start no frame has stamped the new leaves yet: every
  want falls in one tier, ranked by distance as before.
- **A head** is only drawn when its monster is gibbed: wanted a tier
  under its kind's model (`stream_want_group`'s `spare`). The gibs are
  wanted a tier under the nearest member.
- **Measured** (`BENCH_MAPS=1 RESIDENCY=1`): 41 spots of 1020 where a kind
  in the PVS is not in, against 90 before.
  - Per map: E3M4 8, E2M7 6, E3M6 6, E3M5 4, E2M6 3, E1M7 2, E2M3 1,
    E2M4 1, E4M4 1; none elsewhere.
  - These maps' pools (154-306 KB) cannot hold their visible kinds with
    the pinned items at those spots.
- **The spawn views** (`BENCH_MAPS=1 HASHONLY=1 scripts/dlcheck.sh`
  against §50.1's): 9 of 128 lists differ, their pictures identical
  (E1M5's and E3M6's compared). The preload took the same assets in
  another order: the skins lie elsewhere in VRAM.
- **Room**: the release's stack room is 17128 bytes (17464 before this
  ranking).

### 50.6 Left

- **The remaining 41 spots**: a lower model level for the monsters on
  the tightest maps, or more RAM for the pool. Both are left open.
- **The carousel's latency** is unchanged: a kind that comes into the
  PVS waits for its turn (up to 14.6 s, §48.9). Until then its monsters
  are paused (§50.2): invisible and harmless.
- **The order's rare way in C** (§50.1): about 550 cycles per monster and
  brush model sharing a leaf. The GTE could take the plane tests (three
  dot products per MVMVA), in hand-written asm. (Done: §51.)
- **The fade** darkens a body to black. The GPU's blend modes cannot fade
  to the background. A 50 % blend step, or a body sinking into the floor,
  are other looks, left open.
- **Not run**: DuckStation; a hands-on play (E1M1's nailgun room, the
  doors and lifts with monsters by them, the bodies in a lit room, the
  big maps E3M4 and E4M7).

### 50.7 The merge with §49 (master 4d5d91a)

- **The asm cut's layouts** (§49, engine/bmfrag.s): the nodes are 16
  bytes now (`BN_SIZE`; `end` follows the groups' fields, at 14); the
  eye in a model's space (`cam`) goes at the cache's end, past the asm's
  offsets. `build_nodes` is §49's (`BMF_COLD`), with `end`; the rare ways
  of §50.1 share `BMF_COLD`.
- **Checked**:
  - `BMF_C=2 BMFCHK=1 BENCH_MAPS=1 VIEWS=0`: 44295 cuts, the asm against
    the C, 0 mismatches (§49's count);
  - the nailgun scenario (`TICKS=232`): the soldier hidden until the wall
    slides;
  - the 128 spawn views against master's: the 25 lists §50.1 and §50.5
    change, +0.18 % of the frame cycles in all.
- **The stack's room**: release 16648 B, default 16632 B, ultra 16648 B,
  about 250 bytes over the 16 KB floor. The next code needs room (an
  arena, or less code elsewhere).

## §51 — The order among a leaf's pieces in hand-written asm (2026-10-21)

- **The ask** (§50.6's "Left"): §50.1's rare way, an alias model or a
  particle placed among its leaf's brush-model pieces, was C compiled for
  size: about 3.7 k cycles a place on E3M3.v1 (three monsters, two doors'
  pieces in their leaf). In hand-written asm, the GTE for the general
  planes; the C kept as its check.

### 51.1 engine/rl_order.s

- **One call does it all**, `rl_order(id, n, org, box)`: the box's centre
  (moved there from `rl_add_model`), the list's chain at its first ask, the
  test of each model's box against the segment from the eye to the point,
  the items' nodes, the point's way down each model's BSP, then the item
  put after the pieces over the point and after the nearer models.
- **The same results, a different way** (the C is §50.1's):
  - every step of the way takes the eye's side with the point's (the C
    takes the eye's at the nodes it decides at). A step is 3 words: the
    child on the point's side, that child's subtree's size, the point
    beyond the plane from the eye. An item's node is under the child when
    `node - child < size`, unsigned: one compare;
  - one binary search over the model's nodes, keyed by their first
    group's records' offset, gives the C's group, then node, searches
    (the groups' offsets rise);
  - an axial plane is one compare, `v x 4096 - dist / 2` wrapping as the
    C's. A general plane takes the GTE: one MVMVA with the point and the
    eye as the rotation's rows 1 and 2 gives both dot products. When one
    of their coordinates is past 16 bits (never on id's maps: the check's
    far points) the CPU multiplies;
  - the way, the point and the segment's box are in the scratchpad, free
    in the ents stage (r_render sets its own). The GTE's rotation is left
    changed: `rl_ent_matrix` and r_render load theirs;
  - the way has no bound: the cook keeps a model within
    `QW_MAX_BM_DEPTH` nodes (`depth_of`), and the C's bound never acted.
- **A bug in §50.1's C, found on the way**: the chain began at the list's
  head as it was once the brush models were in. A brush model drawn whole
  (too cut up, or without room), added after another's pieces in the same
  leaf, heads the list. The walk then read it as a pieces item (model 0,
  a NULL cache) and followed its stale `z`. Fixed in the C, which the asm
  follows: the chain begins at the first pieces item. None of the 128
  spawn views has such a list.
- **The C as the specification**: rlist.c `rl_order_c` (the former
  `pieces_over` / `place_by_pieces`) with bmfrag.c's `bmf_may_hide`,
  `bmf_pieces_node`, `bmf_node_over`. `ORDER_C=1 scripts/bench.sh` builds
  it alone. `ORDER_C=2` runs both on every ask, and in each debug frame
  (a view's dump) at made-up places around every model with pieces: its
  box's corners, middles and centre, 8 units out; far beyond it from the
  eye and behind the eye; far along each axis, past 16 bits and past the
  axial planes' `x 4096`. Each as a particle, a model with a box and one
  without, the list put back after. `ordasm` lines. The default build
  carries the asm alone.

### 51.2 Measured (PCSX-Redux in fast forward)

- Every bench prints an `order` line per view whose debug frame took
  places among pieces: their count and cycles.
- The 128 spawn views: 28 places in 14 views, 46712 cycles in C, 25776
  in asm (-45 %). E3M3.v1 11424 -> 5624 (-51 %), E1M8.v0 7376 -> 4368,
  E3M4.v0 3144 -> 888.
- E3M3.v1's frames: the ents stage 35894 -> 30219 cycles, the frame
  454833 -> 449160 (-1.2 %). E3M2.v0: the ents stage 38553 -> 37085.
- E1M1's nailgun scenario (`VIEW=112,2352,62,0,0 TICKS=232`, the game
  running, its particles too): 49 places in 27 dumps, 79312 -> 46968
  cycles (-41 %).
- **Room**: the asm is 1652 bytes, against the C's 1676 and its way's
  216 bytes of BSS; `rl_add_model` lost the box's centre (72 bytes). The
  stack's room: release 16648 -> 16968 B, default 16632 -> 16968 B,
  ultra 16648 -> 16968 B; release-spu 17380 B.

### 51.3 Checked

- `ORDER_C=2 BENCH_MAPS=1 HASHONLY=1 scripts/bench.sh`: 32051 asks (the
  views' and the made-up ones), 0 mismatches, 0 unchecked; 1653 general
  planes' sides among them, 528 with a coordinate past 16 bits. The same
  under `EMU_ARGS=-interpreter`. `tools/hazcheck` passes.
- `BENCH_MAPS=1 HASHONLY=1 PROFILE=0 scripts/dlcheck.sh` against master
  8d85a61: the 128 views' display lists identical; their frame cycles
  -0.05 % in all.
- E1M1's nailgun scenario: the 30 dumps' hashes identical to master's
  (the soldier hidden by the wall until it slides, then in the opening;
  t32 and t232 looked at).

### 51.4 Left

- **A per-run table of the coordinates x 4096** (the point's and the
  eye's, per axis) would save 6 instructions a step for 27 a run: even at
  E3M3.v1's 4.5 steps a run, a loss for a particle's short way. Not
  built.
- **Not run**: DuckStation; a hands-on play (E1M1's nailgun room, an
  explosion by a door: each particle is a place).

## §52 — The clean-up: the options kept, the build's choices, the SPU music alone, the guns by hand (2026-10-21)

- **The ask**, the list of what stays:
  - the textures high and medium, low gone, a build parameter (high in
    the release), 4-bit only, the dithering an option;
  - the effects medium and high (medium in the release);
  - the map geometry low, the models low but the guns, rebuilt from
    Quake's own by hand (the decimation broke them), their skins kept;
  - the tick rate 60 or 30 (60), the frame rate cap 60, 45 or 30 (60);
  - the menu's music at 32 kHz, the other tracks at 22 kHz;
  - the warp control dynamic from 4 px; both HUDs; the dynamic lights on,
    in the menu;
  - the gun 3D or 2D, a build parameter (3D in the release); the flat
    shading past 512 units always; the draw distance 1536 or 1024, a build
    parameter (1536 in the release).

  Asked which music stays: the SPU alone; the XA streams and their carousels go. No long tests
  for the parameters: the
  links checked; the guns on one emulator in fast forward.
- **A disc carries** each map's 4-bit textures at the build's level
  (`<MAP>_T1.TX4` or `_T2.TX4`), its low geometry (`_G0.WLD`), its low
  models (`_M0.MDL`), its bank; `GAME_0.DAT`, `STREAM_0.DAT`; the SPU
  music (`MUSIC\SPU_NN.DAT`). The cook (7 s for 32 maps) makes the medium
  and high texture sets (`qcook map --tex4` alone: a first run wrote no
  texture at all, `tex_build` taking a missing 8-bit path for "the layout
  alone"), the low geometry and models, the low streamed assets (no
  pool), the music (`qcook spumusic --rate`: 32000 for track 2).
- **The build's choices** (CMakeLists.txt, engine/settings.h):
  `Q1PSX_TEXTURES` (`high` / `medium`: the disc's one set),
  `Q1PSX_WEAPON` (`3d` / `2d`: `Q1PSX_WEAPON_2D`), `Q1PSX_DRAWDIST`
  (`1536` / `1024`). Presets: `default`, `release`, `release-medium`,
  `bench`; `release-ultra` and `release-spu` are gone. bench.sh's
  `TEXTURES=high|medium`, `WEAPON=3d|2d`, `DRAWDIST=`, `FLAT=` (1, far, by
  default), `T=` (4 px: the game's floor), `EFFECTS=` (1).
- **The engine:**
  - Graphics: six rows (effects, dithering, HUD, dynamic lights, frame
    rate cap, tick rate), all at once; the levels' reload (`load_due`,
    "Takes effect at the next level") gone with them;
  - the warp `R_WARP_DYNAMIC` from `SETTINGS_WARP_FLOOR` (4 px); the budget
    mode gone (render.c); the bench keeps the fixed one;
  - the 8-bit texture sets' loading gone (world.c, anim.c, the stream's
    VRAM regions); `world_t.tex4` gone, so `world_t.bmodels` is at 116
    (bmfrag.s, rl_order.s: the static asserts caught it);
  - the model levels' swap at a load gone (`gdata_set_models`,
    `stream_set_level`);
  - a 2D build lends the gun its packets at each map's start (§45; the
    in-game lend gone: the gun no longer changes); a 3D build compiles no
    2D gun code and loads no 2D tables.
- **The music:** music.c is the SPU player alone (§48), each track at its
  rate: the voices' pitch rate × 4096 / 44100, a sample 393216 / pitch of
  root counter 2's ticks (192 at 22.05 kHz, 132.3 at the theme's 32 kHz:
  a chunk's ticks rounded, a tick in 0.2 s). Gone: the XA streams, the
  carousel (stream.c's captures, the CD interrupt's sectors),
  `music_suspend` / `music_resume`, the CD input's fade, formats/qxa.h,
  tools/qcook/carousel.c; the ADPCM encoder moved to
  tools/qcook/spumusic.c. The maps' asset lists (`stream_monster_needs`,
  `items_streamed`, `sound_map_streamed`) stay in the cook for the
  streaming's redesign.
- **The guns by hand** (tools/qcook/game.c, "the guns by hand"):
  - a gun's triangles written in place of the file's, between the file's
    vertices (Quake's `v_*.mdl`: their frames, the animation; their skin
    coordinates, the skin), the muzzle flash the file's own;
  - a section joins two rings (a cross-section's corners, the same order
    at both ends) by quads, cut along the shorter diagonal;
  - a triangle's winding from the file's triangle in its plane on an
    edge, else from its vertices' normals; its skin half from its
    vertices off the seam (a first rule taking it from the neighbours put
    two junction faces on the back half: the skin's blue fill on screen);
  - the view model cull (§22) then drops what the eye never sees;
  - `QCOOK_NOHAND=1` cooks the file's triangles (an A / B),
    `QCOOK_GUNS=<dir>` writes the hand-made ones (a look).

  The shotgun first, to be looked at: the receiver (x -1.7 .. 5.3),
  its step to the barrel section (x 5.9: a taper from the receiver
  stretched the skin past its painted strip), the barrel section, the
  forearm a hexagon (the file's 10-gon), the barrel's end in one piece.
  88 triangles (20 the flash's), 61 drawn after the eye cull (the file's
  78), 3904 B (4880). On PCSX-Redux's real GPU (the release, a Lua
  script: New Game, the gun at rest, then fired): the file's and the hand
  one side by side, the same at a glance.
  - The guns' geometry was Quake's own at every level, never reduced. Their
    skins are at half resolution since the 3D guns (§22: the shotgun's 100
    × 97 of 200 × 194). The eight at full resolution take about 400 K
    texels, against the game's 256 K of VRAM pages, and five are wider than
    a 256-texel page.
- **The links:**
  - stack room: default 18528 B, release 19184 B (16968 before), with the
    level arena at 1249 KB (12 KB of the code removed given back);
  - the game arena: 112032 of 117760 B;
  - the bench: 69928 B; `BENCH_GAME=1`'s 19120 B, over the 16 KB floor
    again; the 2D bench 69128 B.
- **Left:**
  - the other seven guns by hand, once the shotgun is checked;
  - the guns' full skins (the drawn gun's alone in VRAM: the streaming's
    redesign);
  - every bench baseline changed (the low geometry and models, the 4-bit
    textures, T 4, the flat shading far, 1536 units).

## §53 — The guns' textures, case by case (2026-10-21)

- **The ask**, after the hand-made shotgun (§52): the 3D guns' textures less faithful than the 2D gun's: the shotgun's grey
  middle part crooked, the super shotgun all wrong, the rocket launcher losing its muzzle's hole;
  each gun to be looked at in turn, the guns staying 3D.
- **How it was looked at:**
  - PCSX-Redux in fast forward, the debug build (every gun given at the
    spawn), a Lua script: START loaded, R1 through the eight guns, each
    shot at rest; beside the cook's 2D renders (`QCOOK_WSDUMP`: the whole
    model, Quake's skin, perspective-correct, depth-buffered), cropped and
    magnified;
  - `QCOOK_GUNDUMP=<dir>`: each gun's triangles as drawn (their order,
    their corners in the atlas and in Quake's skin, their exact places in
    every frame, the atlas's texels); a host render of them the PS1's way
    (affine, the records' order) and with one fault taken out at a time
    (perspective; a depth buffer; Quake's skin): which fault makes which
    defect.
- **The faults:**
  - **The order.** No depth buffer: a view model's triangles go in their
    records' order (the first written drawn last), nearest-first by their
    centroid's distance in frame 0, then regrouped within 24 for shared
    vertices. The nailgun's barrels came over its blocks, the super
    shotgun's gap between the barrels over one (the black notch), the
    rocket launcher's outside over its muzzle (the hole gone).
  - **The back faces.** The renderer culled no model's triangle by its
    winding (the cook dropped only a view model's never facing the eye):
    a model's back went under or over its front by the order alone (the
    super nailgun's turning barrels; a monster's far side through its
    near one).
  - **The vertices' grid.** The pools were in vertex units, a quarter of
    a world unit; Quake's guns are much finer across (the shotgun's steps
    0.014 units in y, 0.048 in z): up to 9 px off at the eye's 5 units
    (0.5 to 2.2 px on average): the shotgun's barrel crooked.
  - **The affine mapping.** The view model was drawn without the
    subdivision (§22.3): a triangle deep along the view shows its far
    texels too large (the shotgun's barrel bent, the grenade launcher's
    band, the rocket launcher's rim smeared over its middle).
  - **The half skins.** A line one of Quake's texels wide is two at half
    resolution, then spread by the screen near the eye: the rocket
    launcher's highlight a band over its middle. Not fixed (below). The
    hand-made shotgun's half skin was averaged over the file's triangles'
    texels only: its own triangles' others came from their neighbours
    (grey smears on the receiver).
- **The order** (tools/qcook/game.c `view_order`): in every frame, from
  every place the eye takes (at rest; the bob's 0.4 ahead, -1.0 and +1.6
  units; the full screen's gun 2 units lower), every two triangles whose
  pictures overlap (clipped at 5 units) must come nearer first, by their
  depths at the overlap's centroid; a pair's say is its overlap's area,
  frame 0 four times, the eye at rest twice, a tenth between two turned
  away. A triangle is written once every one in front of it is; of those,
  the one sharing the most vertices with the record; none (the frames
  disagree): the one whose turn breaks the least say. The super nailgun
  breaks 219 such cycles (its barrels turn), the axe 27, the super
  shotgun 42; the others none.
- **The back faces** (engine/r_world.s, the model instance of `POLYS`): a
  model's triangle is left out when the GTE's NCLIP of its corners is >=
  0, the emit's loads and sums in the NCLIP's eight cycles, as Quake's
  alias drawer (d_polyse.c `D_DrawNonSubdiv`). The cook winds every MDL
  triangle as the world's faces (corners 0, 2, 1: Quake's 76 MDLs all
  wind the other way, `mdl_wind`); the item boxes and the brush models
  walked there are the world's already. The slow path (near or split
  polygons) still draws a back face: the order above keeps them under.
  E1M1's 12 views: 9 identical, 3 better (a back face showing through a
  monster gone), 476 primitives fewer on the busiest (1970 -> 1494),
  its frame -1.2 % (-15 k cycles; 0 to -1.2 % on the others).
- **The vertices** (formats/qgame.h `QGF_VIEW`, `qg_view`; engine/rlist.c
  `rl_view_gun`): a gun's frames are Quake's own vertex words (4 bytes a
  vertex, the streamed models' layout, formats/qstream.h), expanded by
  `mdl_expand_shift` (x4) into the game arena's pool for it when the
  frame drawn changes; its matrix worked out in C from the camera's, the
  products in 64 bits rounded once, its steps in the columns, every row
  x4 (r_world.s's `RE_VIEW`: the near plane at SZ 120 and H 160, the
  world's; until now H 40 and only x and y x4); its translation from the
  camera directly (the bob's 0.4 ahead, the viewport's 2 units: no world
  position rounded on the way). The records share the pool's entries
  (one slot a vertex; until now one a record's vertex).
- **The cut** (`tess_build`): a gun's edges halved where the affine
  displacement at their middle, frame 0, the eye at rest, of their part
  in front of the near plane, passes 8 px (the slow path's measure,
  L |dz| / (2 (za + zb))), the triangles cut as the slow path cuts them,
  their pieces again; the decision the edge's alone (no cracks); a middle
  a vertex of its own in every frame (the ends' middle, the normal nearest
  their normals' mean, the skin coordinates' mean; the pools' words round
  it to Quake's step). The axe is not cut (its skin is even; at 8 px it
  would take 461 triangles). The shotgun 61 -> 81 triangles, the super
  shotgun 127 -> 133, the nailgun 105 -> 191, the super nailgun 166 ->
  266, the grenade launcher 30 -> 80, the rocket launcher 65 -> 125, the
  lightning gun 108 -> 201. (Every frame's error at 4 px instead: the
  rocket launcher 613, the axe 2492.)
- **The atlas** (`view_mesh`, `view_atlas`): a gun's mesh is made once,
  before its image (the hand-made triangles, the cull, the cut), and its
  image is an atlas of the texels it shows: the triangles sharing an edge
  and its texels at one resolution make a chart, a chart's box with two
  texels of the skin around it, in shelves the width of the least area.
  Half resolution throughout: 91 K texels for the eight (the half skins
  took 100 K, mostly Quake's unused fill); the game's pages leave the
  stream 82 K texels (68 K before).
- **Quake's resolution, measured, not on a disc** (`qcook game --textures
  high`): Quake's texels where the screen spreads them, half elsewhere.
  Even counting only the triangles showing over 4 px a texel at rest, the
  atlases take 217 K texels, against the ~150 K the game's pages leave
  beside the HUD and the projectiles; every frame's spread counted: 240 K
  and more, an atlas wider than a page. It needs VRAM taken from the
  stream's pool (pages 3-5 and the leftovers, 286 K texels at START), or
  the drawn gun's atlas alone, read at a switch (the streaming's
  redesign): a call left open.
- **RAM:** the guns' geometry 47968 B (62264): the game arena 99 KB at
  boot, 100 KB (115: 15 KB to the stack's gap for now, the release's room
  33 KB; for the level arena, or kept: a call left open). The 2D build's
  tables fit (100080 B).
- **Cost** (the game bench, START, each gun fired; the base 4291417
  built beside): the axe -1.3 %, the shotgun 0, the super shotgun +2.8 %
  (near-clipped triangles through the slow path), the nailgun +0.1 %, the
  super nailgun +2.0 %, the grenade launcher -0.4 %, the rocket launcher
  +0.9 %; the lightning gun's scene diverged (its beam), not comparable.
  The bench runs the game as frames take time: the gun's frame measured
  differs a little between builds.
- **The CD** (found on the way): `music_stop`'s and `stream_abort`'s cut
  read is now followed by a waited pause (`CdControlB(CdlPause)`). With
  `GAME_0.DAT` smaller, the release's New Game found no `START.SND` (the
  drive's error 20h; the map's first read, the directory's, took one of
  the menu music's refill's sectors), every time: a race the disc's layout
  had hidden.
- **The rocket launcher's muzzle** (after the first round: its hole not right yet):
  - its atlas's half texels were averaged over the texels whose corner
    is inside a triangle: the muzzle rim's, which the triangles' edges
    cross, were left out, the half texels there the beige of their
    neighbours (a beige V over the hole). A gun's atlas now counts every
    texel a triangle overlaps (`skin_cover`'s `edges`: the GPU's floor(u)
    takes them); the other models keep the corner rule (the blue fill);
  - the hole itself: Quake's guns pass parts through one another and let
    its depth buffer draw their meeting line (the muzzle's cap through the
    tube's top: the arch of the hole is that line). Counted at rest
    (frame 0, the eye at rest, the pairs facing the eye whose depths
    cross inside their overlap): the super shotgun 49 pairs, the nailgun
    34, the super nailgun 27, the lightning gun 24, the rocket launcher
    23, the axe 12, the hand-made shotgun and the grenade launcher none;
    the pixels an order cannot show right: 140, 132, 72 (the super
    shotgun), 76 (the rocket launcher), 44, 4. The way out chosen: the texels concerned repainted (the cut of the triangles along those lines, begun, would
    have added triangles). `view_repaint`: at rest, at twice the screen's
    resolution, where the order shows another triangle than a depth
    buffer would, the texel the PS1 shows there (affine) takes Quake's
    colour there (the visible triangle's texel, perspective-correct,
    unlit), when that texel is seen wrong more often than right: 73
    texels for the rocket launcher, 124 the super nailgun, 69 the
    nailgun, 63 the super shotgun, 10 the lightning gun, 2 the axe; no
    triangle more, no cycle. On PCSX-Redux the hole is the arch again,
    nearly right; its rim lighter (the light's yaw: Quake's
    own table by the gun's yaw, the 2D pictures yaw 0's) and squarer (half
    resolution).
  - `QCOOK_GUNDUMP`'s corners paired a triangle's vertices with its record
    corners in the file's order: wrong since the records wind as the
    world (a look's tool only).
- **Left:** the guns at Quake's resolution (above); the other seven guns
  by hand.

## §54 — The other seven guns by hand (2026-10-21)

- **The ask:** the other seven guns by hand as the shotgun, a capture each.
- **What they are** (each gun's parts, sections by x, frame 0; the cook's
  cull; QCOOK_GUNDUMP): Quake's other guns are already lean prisms, the
  shotgun's 56-vertex body (§52) the exception: hexagonal tubes of two or
  three sections with a cap (the nailguns' barrels: a chamfer and a cone at
  their rear, seen), square tubes and boxes (the lightning gun, the
  nailgun's plates), the rocket launcher's body and its three side tubes,
  the axe's handle (a hexagonal prism across the near plane), its head and
  blade; the flash, the file's. A middle section on its ends' line within
  0.2 units, its skin following: the super shotgun's stock (two), the
  grenade launcher's tube, the lightning gun's box (two); the others are
  steps, chamfers or tapers.
- **Dropping them made the guns heavier:** §53's cut put back more than it
  saved (the grenade launcher 78 drawn triangles -> 96, the lightning gun
  168 -> 204): it cut a long quad's two triangles into three each, the
  diagonal's middle too, where a ring would do.
- **The cut by rings** (tools/qcook/game.c `tess_quad`): a triangle and its
  neighbour across its longest edge, in one plane (normals within 5.7
  degrees), of one skin half, wound alike, make a flat quad; two opposite
  edges cut, it becomes two quads joined at their middles, again on each;
  else its triangles go as before. The decision stays the edge's alone (no
  cracks); the quads' diagonals are inside them. At the same 8 px, the
  hidden ones below gone in both: the nailgun 165 -> 117 drawn triangles,
  the super nailgun 240 -> 198, the lightning gun 168 -> 130, the grenade
  launcher 78 -> 62, the rocket launcher 98 -> 82.
- **The hidden ones** (`view_hidden`): a gun's triangles a depth buffer
  never shows, in every frame, from ten places of the eye (the bob's
  forward reach in five steps under the status bar's viewport and the full
  screen's), at twice the screen's resolution and past its edges, the near
  end clipped at 5 units, go: Quake's guns hide pieces in one another (the
  super shotgun 51 of its 109 kept, the lightning gun 36, the nailgun 22,
  the axe 20, the shotgun 19; with §53's cut, the super shotgun 133 -> 66
  drawn triangles, the shotgun 81 -> 57). Checked at rest and firing
  against the 2D renders: nothing changes.
- **By hand** (the shotgun's tables, `GUNS`, generalized: the file's
  triangles kept among sets of vertices, a range or a list: the flash, a
  cap, a piece left as it is): the super shotgun's stock without its two
  middle sections, the grenade launcher's tube without its middle one, the
  lightning gun's box without its two: 66 -> 64, 62 -> 50, 130 -> 120
  drawn triangles. The nailguns (their barrels' chamfers and cones are
  seen), the rocket launcher (its muzzle's pieces, §53's repaint) and the
  axe (no section of its handle is redundant; it stays uncut: 321 drawn
  triangles at 8 px, 197 at 16) keep the file's triangles.
- **Results** (triangles in the records, §53's last state -> now; the
  renderer then leaves out those turned away): the axe 146 -> 126, the
  shotgun 81 -> 53, the super shotgun 133 -> 64, the nailgun 191 -> 117,
  the super nailgun 266 -> 198, the grenade launcher 80 -> 50, the rocket
  launcher 125 -> 82, the lightning gun 201 -> 120: 1223 -> 810. The
  guns' geometry 47968 -> 36368 B (the game arena 87 KB at boot of 100).
  On PCSX-Redux's real GPU (the debug build, START, R1 through the guns at
  rest): each gun as before, beside the cook's 2D render.
- **Cost** (the game bench, START, each gun fired, against §53's): the axe
  -0.6 %, the shotgun -1.5 %, the super shotgun -6.5 %, the nailgun
  -1.1 %, the super nailgun -2.1 %, the grenade launcher -0.7 %, the rocket
  launcher -1.6 % (its HUD scenarios -1.9, -2.9 %), the lightning gun
  -3.5 %; against 4291417, before §53's fixes: -0.1 to -3.9 % (the
  lightning gun's beam differs from one run to the other there).
- **RAM:** the game arena at boot 87232 B (88288 with the 2D gun's
  tables): 100 -> 90 KB, the 10 KB to the stack's gap with §53's 15 (the
  release's room 43.5 KB). The 25 KB stay the stack's margin for now: the level arena stays 1249 KB.

## §55 — The C engine, to compare with the asm (2026-10-21)

- **The ask:** a CMake option to build the engine's C in place of its asm, to compare the
  performance. Then, on the way: the asm stays the base, never changed to fit the C (the C may
  follow the asm, not the reverse), and the C is not to be optimized: it is there to compare.
- **The option:** CMake `Q1PSX_ENGINE_C` (off by default). Every
  hand-written asm part that has a C version builds that in its place:
  - the physics: engine/clip.c, pmove.c (for clip.s, pmove.s);
  - the slow path: r_slow.c (for r_slow.s; R_SLOW_C = 1);
  - the dynamic lights: dlight.c's descent, record hook and restore (for
    r_world.s's; DLIGHT_C = 1);
  - the brush models' cut: bmfrag.c cut_model_c (for bmfrag.s; BMF_C = 1);
  - the places among a leaf's pieces: rlist.c rl_order_c (for
    rl_order.s; ORDER_C = 1);
  - an entity's matrices: rlist.c rl_ent_matrix (for ent_mat.s);
  - a model's frame: mdl.c mdl_expand (for mdl_expand.s).
  The world renderer (engine/r_world.s: the walk, the records, the models'
  records) has no C: it was written in asm from the start (R3.1) and stays
  in both.
- **The asm untouched:** the default and release builds are byte-identical
  to 6375501's (`q1psx.exe` compared). The one asm-side line: r_defs.inc's
  `R_SLOW_C` under `.ifndef`, set by CMake as R_DLIGHT_C is; no
  instruction changed.
- **The C made to follow the asm** (for the same frames; nothing
  optimized):
  - r_slow.c, the asm's specification until the models came, read the
    world's constants where the asm reads the words r_world.s sets for
    what it draws: the clip bound, the near plane and T2 (the view model's
    124, 120 and no split), H, the viewport's centre, the vertex pool,
    the texture's CLUT and page, each vertex's colour. It also took the
    view's matrix where the asm's MVMVA takes the one loaded (an
    entity's), and it never saw the arena full. It now reads those
    words, transforms by the GTE as the asm does (`gte_rtv0tr`), keeps the
    pool's 16-bit screen coordinates, and sets SP_FULL then gives the
    polygon up. Its algorithm stays the spec's: the vertices copied on its
    stack, a shared edge's midpoint computed again where the asm keeps it
    in a table.
  - rl_ent_matrix and mdl_expand, asm only until now, written in C from
    the asm, its way (MVMVA through the GTE macros; two masked words a
    vertex).
  - The bench's physics checks: the C engine's physics is the reference,
    so they compare it with itself (their lines say `c-engine` for
    `asm`). The reference's entity trace starts from the mover the check
    copied, as the asm build's own copy does.
- **The same frames:** display lists identical for start's 12 views and
  for the 128 spawn views of the 32 maps (scripts/dlcheck.sh: saved by the
  asm, checked with ENGINE_C=1). The monsters' 8 dumps and the movers' hash
  (E3M4's door) are identical too. The game bench matches on 22 of its 23
  hashes. The 23rd is the Graphics menu after the effects' change, drawn
  under the game's dynamic warp threshold, which rises with the C's slower
  frames (render.c `r_warp_feedback`): the asm cuts one edge that the C
  leaves whole. The walks run in real time and diverge for the same
  reason.
- **What the asm gains** (PCSX-Redux's CPU timings in cycles; per frame
  unless stated):

  | | asm | C | C / asm |
  |---|---|---|---|
  | start's 12 views | 6.22 M | 11.68 M | +88 % (1.58 -> 2.25 vbl) |
  | the 128 spawn views | 45.6 M | 88.6 M | +94 % (+22 to +147 % a view, median +101 %; 1.20 -> 1.77 vbl) |
  | of which the slow path | 25.0 M | 67.8 M | x2.71 |
  | the walk and the records (asm in both) | 17.35 M | 17.47 M | +0.7 % |
  | the entities' stage | 2.27 M | 2.32 M | +2 % |
  | START's ten weapon and HUD views | 10.8 M | 16.4 M | +52 % (one vblank more each) |
  | a hull trace | 2671 | 4771 | x1.79 |
  | a player tick (pmove and its traces) | 9791 | 15498 | x1.58 (pmove alone 2515 -> 2644) |
  | a line of sight | 6427 | 10314 | x1.60 |
  | a game tick on START (walking, firing, idle) | 28.3 k, 47.3 k, 28.8 k | 39.4 k, 67.0 k, 39.3 k | x1.36 to x1.42 |
  | the monsters' crowd on E1M1 (21 awake), a tick | 143 k | 212 k | x1.48 |
  | E3M4's door re-cut, a frame with one | 215 k | 322 k | x1.50 |

- **RAM:** the C engine leaves 55.2 KB of stack instead of 42.8 KB in the
  default build, and 55.9 KB instead of 43.5 KB in the release. The asm slow
  path's pool and midpoint table (17.6 KB) go; the C's code is a little
  larger.
- **To compare:** `scripts/dlcheck.sh save asm`, then `ENGINE_C=1
  scripts/dlcheck.sh check asm`. This prints every view's cycles, asm then
  C, and the verdict on the frames; add `BENCH_MAPS=1 HASHONLY=1 SECS=3000`
  for the 128 views. `ENGINE_C=1 scripts/bench.sh` builds in
  `build-bench-c/`, beside the asm's `build-bench/`. A C engine disc:
  `cmake --preset default -B build-c -DQ1PSX_ENGINE_C=ON .` then
  `cmake --build build-c`.

## §56 — Level chunks: what each place needs, measured (2026-10-22)

- **The ask:** before streaming the levels in chunks, a tool to find where to cut them without
  it showing: the chunk the player is in and two adjacent ones loaded, the teleporters counted,
  chunks of 64 to 128 KB, the view distance taken into account (1536 units, 1024 as a second
  run), the tool kept out of the repo.
- **The tool** lives outside the repo: a QuakeSpasm 0.97
  clone in `quakespasm-q1psx`, with `chunkscan` (`Quake/chunkscan.c`)
  and the analysis scripts (`analysis/*.py`, numpy and scipy). For each map:
  - the server is spawned;
  - the 16-unit voxels the player's origin can reach are flooded through the
    world's player hull. Doors and secret doors count as open. Plats, lifts,
    trains and pushers are ridden. Teleporters are followed. A running jump's
    air drift is bounded;
  - from every reachable eye (origin + 22) and from the intermission cameras,
    the map's cooked records (this repo's `<map>_g0.wld`) are rendered on the
    GPU into a cube of six 2048² depth buffers. A depth pass comes first; an
    equal-depth pass then writes the id of every record a pixel shows.

  The brush models are drawn against the world's depth, and the teleporters'
  boxes too. Over the 32 maps that makes 5.4 million eyes, in about 20 minutes
  on an RTX 5080.
- **What an eye needs** is the records it sees that its leaf's PVS marks and
  that the PSX's draw distance keeps. The cull is by depth: a record goes when
  its sphere's nearest depth is past D. At a frustum corner a point's depth is
  0.6247 × its distance (the 240-row view), so a record can stay drawn up to
  1.6 D away, as render.c's mark_r has it. The renders find about 1.5 % more
  records at 2048 px than at 1024 (2.5 % at 1024 over 512): the tail is
  slivers under one PSX pixel. The PVS within D, seen or hidden, is the safe
  bound.
- **What each place needs** (KB; "skeleton" = what stays resident: nodes,
  leaves, marks, PVS, clip hulls, the brush model and entity tables;
  "geometry" = the world's records and their vertices; the resident world =
  the skeleton + the eye's zone + its two nearest, the worst eye of the map):

  | map | .wld | skeleton | geometry | need/eye max 1536 (1024) | PVS/eye max | zones PVS 96: n, biggest, 4th <320 u | resident world max: PVS 96 / seen 64 | vs the .wld |
  |---|---|---|---|---|---|---|---|---|
  | start | 592 | 258 | 317 | 39 (39) | 71 | 7, 81, 0.1 % | 489 / 433 | -103 / -159 |
  | e1m1 | 600 | 258 | 326 | 34 (34) | 82 | 10, 91, 0.587 % | 531 / 446 | -69 / -154 |
  | e1m2 | 615 | 276 | 322 | 31 (31) | 65 | 7, 90, 0.0 % | 524 / 459 | -90 / -156 |
  | e1m3 | 600 | 279 | 288 | 32 (32) | 55 | 8, 86, 0.0 % | 499 / 442 | -101 / -157 |
  | e1m4 | 693 | 312 | 361 | 37 (37) | 70 | 7, 84, 0.0 % | 541 / 492 | -152 / -202 |
  | e1m5 | 555 | 247 | 288 | 33 (33) | 58 | 7, 88, 0.0 % | 506 / 417 | -49 / -138 |
  | e1m6 | 465 | 218 | 201 | 33 (33) | 74 | 7, 86, 0.766 % | 465 / 383 | -1 / -82 |
  | e1m7 | 183 | 84 | 89 | 34 (34) | 57 | 1, 78, 0.0 % | 162 / 162 | -21 / -21 |
  | e1m8 | 354 | 129 | 209 | 44 (44) | 94 | 25, 96, 29.808 % | 408 / 320 | +54 / -34 |
  | e2m1 | 649 | 277 | 351 | 31 (31) | 72 | 16, 90, 1.511 % | 545 / 457 | -104 / -192 |
  | e2m2 | 680 | 302 | 360 | 31 (31) | 65 | 9, 91, 0.0 % | 570 / 468 | -110 / -213 |
  | e2m3 | 662 | 294 | 345 | 29 (29) | 72 | 11, 92, 0.25 % | 552 / 473 | -110 / -188 |
  | e2m4 | 715 | 312 | 380 | 32 (32) | 72 | 12, 91, 0.051 % | 549 / 490 | -166 / -225 |
  | e2m5 | 627 | 255 | 347 | 30 (30) | 70 | 10, 91, 0.022 % | 522 / 430 | -105 / -197 |
  | e2m6 | 607 | 271 | 301 | 27 (27) | 71 | 11, 88, 0.014 % | 533 / 442 | -74 / -165 |
  | e2m7 | 690 | 310 | 361 | 32 (32) | 62 | 9, 89, 0.0 % | 568 / 489 | -123 / -201 |
  | e3m1 | 608 | 253 | 333 | 27 (27) | 65 | 9, 91, 0.365 % | 522 / 437 | -85 / -171 |
  | e3m2 | 401 | 181 | 177 | 29 (29) | 50 | 3, 71, 0.0 % | 388 / 338 | -13 / -63 |
  | e3m3 | 483 | 206 | 257 | 24 (24) | 57 | 5, 76, 0.0 % | 430 / 360 | -53 / -122 |
  | e3m4 | 719 | 325 | 351 | 31 (31) | 64 | 8, 90, 0.0 % | 556 / 503 | -163 / -216 |
  | e3m5 | 768 | 336 | 416 | 34 (34) | 75 | 8, 91, 0.0 % | 594 / 508 | -174 / -260 |
  | e3m6 | 746 | 323 | 400 | 38 (38) | 93 | 12, 95, 0.963 % | 595 / 509 | -150 / -237 |
  | e3m7 | 560 | 236 | 307 | 25 (25) | 54 | 7, 88, 0.0 % | 466 / 399 | -94 / -161 |
  | e4m1 | 630 | 280 | 331 | 38 (38) | 78 | 9, 90, 1.438 % | 535 / 464 | -95 / -166 |
  | e4m2 | 595 | 258 | 313 | 29 (29) | 74 | 9, 91, 0.0 % | 517 / 433 | -78 / -163 |
  | e4m3 | 559 | 270 | 265 | 29 (29) | 64 | 8, 92, 0.0 % | 535 / 451 | -24 / -108 |
  | e4m4 | 684 | 292 | 381 | 39 (39) | 87 | 17, 90, 1.534 % | 556 / 475 | -128 / -209 |
  | e4m5 | 618 | 276 | 307 | 34 (34) | 63 | 8, 90, 0.0 % | 534 / 457 | -85 / -161 |
  | e4m6 | 492 | 227 | 241 | 28 (28) | 48 | 5, 78, 0.0 % | 444 / 381 | -49 / -111 |
  | e4m7 | 763 | 352 | 388 | 27 (27) | 61 | 10, 91, 0.0 % | 618 / 518 | -145 / -245 |
  | e4m8 | 578 | 257 | 293 | 20 (20) | 42 | 7, 86, 0.0 % | 488 / 420 | -90 / -158 |
  | end | 240 | 101 | 133 | 50 (50) | 74 | 2, 89, 0.0 % | 208 / 291 | -32 / +51 |
  
  Worst over the maps: the .wld 768 KB; the resident world PVS 96 618 KB, seen 64 518 KB.

- **Findings:**
  - an eye needs little: 20-50 KB at most (the median 11-35 KB), the PVS
    within D 42-94 KB: the occlusion halves it and more;
  - the draw distance changes nothing that is seen: 1536, 1024 and no limit
    give the same needs on every map. The farthest seen record (E1M8) is
    1783 units away, its depth at the frustum corner 1067: Quake's sight
    lines end before the PSX's cull (600 units would start to cut);
  - the skeleton stays: 84-352 KB a map (at most nodes 127 KB, clip hulls
    81, leaves 59, PVS 49, entities 22), as big as the geometry (89-416 KB).
    Chunking the geometry alone frees 150 KB on the worst map (PVS zones of
    96 KB: 618 KB resident against the 768 KB .wld) or 250 KB (seen zones of
    64 KB: 518 KB), not more;
  - the literal model (disjoint chunks, the current one and two
    adjacent): grouping records by what eyes need together, 99.999 % of the
    eyes need at most three chunks, but what one second of running brings
    into view needs a fourth at up to 10 % of the places (E1M2) — while
    zones (below) keep three for every map but E1M8;
  - teleporters: 277 triggers, 73 the player can take (the others, monster
    closets). The player can get within 208 units of one unseen (median), 9
    are taken without having been seen: the rule "a teleporter seen, load its
    destination" is a complement; walking distances that count a teleporter
    as one step catch them all;
  - small maps need no cut: E1M7 (183 KB), END (240), E1M8 (354, whose open
    space makes 25 small zones and a short lead).
- **The design chosen** (the implementation: §57, on its own branch):
  - zone chunks: a zone is a set of camera leaves, its chunk every world
    record the PSX may draw from any of its leaves (PVS within D + 32
    units, records shared between zones duplicated: the disc takes it);
    only the current zone is drawn, two slots hold the nearest zones,
    prefetched by walking distance (a teleporter one step); 96 KB at most a
    zone (the average asked for); a map whose .wld fits stays whole;
  - the tables (zone -> records; leaf -> zone, its three nearest zones and
    their distances) come from the tool: `quakespasm-q1psx/zones/
    pvs_1536_96/<map>.zones` (pvs_1536_128, seen_1536_96 and seen_1536_64
    beside it). They are derived from Quake's data: never in the repo; the
    cook reads them from a directory local.env names, and without them
    cooks every map whole, as before;
  - next, the skeleton: the PVS rows per zone, the clip hulls and nodes
    where the player can be — the RAM wanted for the streamed
    assets is there too.

## §57 — Level streaming by zones (2026-10-22)

- **The ask:** the levels streamed in chunks: the chunk the player is in and the two adjacent
  ones loaded, the teleporters counted, chunks of 64 to 128 KB, for much more room for the
  streaming; then to start this implementation at once: the calls below
  were made by measure, and the alternatives are listed (§57.8). On a
  branch of its own, not merged.
- **What it does.** A zoned map's level arena keeps its skeleton (nodes,
  leaves, marks, PVS, clip hulls, the brush models and their records, the
  entities) and three chunk slots. A zone is a set of camera leaves (§56's
  tables); its chunk holds every world record the PSX may draw from any of
  them. The camera leaf's zone is the one drawn; the two zones nearest to
  it by walking distance (a teleporter one step) are read ahead into the
  other slots, from the CD, while the player moves.

### 57.1 The cook: skeleton and chunks

- `qcook zones` (`tools/qcook/zones.c`) splits a cooked world: it reads
  the whole `<map>_g0.wld` (QWVE, unchanged: the tools and the analysis
  keep reading it), the map's table, and both texture sets (`_t1.tx4`,
  `_t2.tx4`), and writes:
  - `<map>_g0.skl`, the skeleton ("QWVZ", formats/qworld.h): a QWVE
    file's header and sections, a `qw_zone_header` after the header, the
    face stream and the vertex pool holding the brush models' records only
    (their UVs mapped by the loader, as before), then the zone tables: the
    chunk directory, a byte per leaf (its zone set), the sets (a zone and
    its three nearest);
  - `<map>_z1.zon`, `<map>_z2.zon`, the chunks per texture level ("QZN1"):
    each chunk from a sector of its own, its records as the loader would
    have left them (face ids doubled, the average light in the flags, the
    UVs on that set's tiles, the vertex indices into the chunk's own pool),
    a patch list, then its pool.
- **The UVs in the cook.** Mapping a chunk's UVs when it lands would cost
  about 0.8 frame a chunk (18 k UV words, two multiplies each), so the
  cook maps them, once per texture level; the disc takes the build's
  level only (`scripts/cd_image.cmake`). Hence a `.zon` per level, and the
  zones' step runs after the textures' jobs (`scripts/cook.sh`).
- **The node array is reordered.** The renderer ends a node's back block
  where the next node's front block starts (`qw_face_end`), so every
  chunk lays out its blocks in node order, an empty block for every node
  without records in it. The world's blocks live in the chunk and the
  brush models' in the skeleton, and the chain must not run from one to
  the other: the skeleton's node array is the world's nodes that hold a
  record in some chunk ("patched"), then an end node no walk reaches
  (its `faces[0]` ends the current chunk's last block), then the world's
  nodes no chunk gives a record (their blocks empty for good: almost none,
  0-107 a map), then the brush models' nodes, then the sentinel. Children
  are pointers, parents are re-indexed: the walk's order, so the frames,
  do not change.
- **The patch list:** a word per patched node with records in the chunk,
  `skip << 24 | front words << 12 | back words` (skip: the patched nodes
  before it with none, their blocks empty at the running pointer; a run
  longer than 255 takes words of skip 255 and no blocks), the last word
  the end node's. 529-1116 words a chunk (2-4.4 KB).
- **The tables** come from `local.env`'s `Q1PSX_ZONE_TABLES` (derived
  from Quake's data: never in the repo). Without it every map is cooked
  whole, as before, and the build works anywhere. A table that does not
  match the world (its leaf and face counts) leaves the map whole.
- **A map stays whole** when its skeleton and slots would take as much as
  the whole world. With the default tables: 20 maps zoned, 12 whole
  (E1M5, E1M6, E1M7, E1M8, E2M6, E3M2, E3M3, E3M6, E3M7, E4M3, E4M6, END).

| map | zones | records in the chunks (the world's) | chunks KB | patched nodes (never) | patch words max | sets | skeleton + slots = resident vs whole (KB) |
|---|---|---|---|---|---|---|---|
| START | 7 | 5836 (4133) | 37..93 | 2394 (28) | 725 | 29 | 277 + 3 x 94 = 559 vs 592 |
| E1M1 | 10 | 9761 (4390) | 49..104 | 2332 (23) | 720 | 68 | 276 + 3 x 104 = 588 vs 600 |
| E1M2 | 7 | 6170 (4172) | 27..92 | 2382 (43) | 685 | 57 | 295 + 3 x 94 = 577 vs 614 |
| E1M3 | 8 | 5139 (3741) | 12..91 | 2184 (1) | 639 | 48 | 314 + 3 x 92 = 590 vs 599 |
| E1M4 | 7 | 6530 (4681) | 53..88 | 2591 (6) | 693 | 42 | 334 + 3 x 90 = 604 vs 693 |
| E1M5 | 7 | 5860 (3705) | 9..103 | 2041 (39) | 673 | 42 | 269 + 3 x 104 = 581 vs 555 (whole) |
| E1M6 | 7 | 7032 (2708) | 66..94 | 1547 (1) | 684 | 41 | 266 + 3 x 96 = 554 vs 465 (whole) |
| E1M7 | 1 | 1188 (1188) | 91..91 | 615 (1) | 616 | 1 | 95 + 1 x 92 = 187 vs 183 (whole) |
| E1M8 | 25 | 24925 (2617) | 53..105 | 1117 (0) | 541 | 186 | 146 + 3 x 106 = 464 vs 353 (whole) |
| E2M1 | 16 | 12331 (4649) | 20..107 | 2571 (13) | 733 | 144 | 302 + 3 x 108 = 626 vs 649 |
| E2M2 | 9 | 9054 (4805) | 30..103 | 2821 (107) | 783 | 85 | 322 + 3 x 104 = 634 vs 680 |
| E2M3 | 11 | 10245 (4584) | 52..94 | 2638 (29) | 682 | 123 | 320 + 3 x 96 = 608 vs 661 |
| E2M4 | 12 | 9660 (5142) | 31..94 | 2771 (74) | 734 | 115 | 337 + 3 x 96 = 625 vs 714 |
| E2M5 | 10 | 8764 (4392) | 31..93 | 2193 (29) | 565 | 98 | 282 + 3 x 94 = 564 vs 627 |
| E2M6 | 11 | 7852 (3978) | 5..101 | 2282 (16) | 714 | 60 | 308 + 3 x 102 = 614 vs 606 (whole) |
| E2M7 | 9 | 6922 (4736) | 8..104 | 2704 (1) | 785 | 60 | 331 + 3 x 106 = 649 vs 690 |
| E3M1 | 9 | 8893 (4356) | 39..102 | 2193 (11) | 624 | 104 | 277 + 3 x 104 = 589 vs 607 |
| E3M2 | 3 | 2953 (2384) | 66..89 | 1326 (14) | 701 | 6 | 226 + 3 x 90 = 496 vs 400 (whole) |
| E3M3 | 5 | 4974 (3556) | 57..93 | 1871 (32) | 647 | 21 | 226 + 3 x 94 = 508 vs 482 (whole) |
| E3M4 | 8 | 5635 (4855) | 3..112 | 2581 (18) | 833 | 47 | 371 + 3 x 114 = 713 vs 719 |
| E3M5 | 8 | 7223 (5371) | 29..111 | 3059 (6) | 798 | 52 | 353 + 3 x 112 = 689 vs 767 |
| E3M6 | 12 | 13263 (5559) | 51..153 | 3006 (33) | 1116 | 82 | 348 + 3 x 154 = 810 vs 745 (whole) |
| E3M7 | 7 | 6441 (4277) | 5..110 | 2290 (26) | 881 | 28 | 255 + 3 x 112 = 591 vs 560 (whole) |
| E4M1 | 9 | 8084 (4259) | 49..92 | 2547 (2) | 762 | 104 | 302 + 3 x 94 = 584 vs 630 |
| E4M2 | 9 | 7455 (4115) | 11..94 | 2010 (0) | 608 | 57 | 285 + 3 x 96 = 573 vs 595 |
| E4M3 | 8 | 7382 (3566) | 48..95 | 1963 (1) | 624 | 71 | 297 + 3 x 96 = 585 vs 558 (whole) |
| E4M4 | 17 | 15399 (5038) | 4..93 | 2433 (0) | 699 | 150 | 305 + 3 x 94 = 587 vs 683 |
| E4M5 | 8 | 7152 (4036) | 19..97 | 1920 (2) | 600 | 29 | 313 + 3 x 98 = 607 vs 618 |
| E4M6 | 5 | 3535 (3178) | 19..80 | 1596 (0) | 552 | 27 | 253 + 3 x 82 = 499 vs 492 (whole) |
| E4M7 | 10 | 9344 (5323) | 26..93 | 2856 (4) | 661 | 78 | 378 + 3 x 94 = 660 vs 763 |
| E4M8 | 7 | 5821 (3993) | 21..95 | 2040 (10) | 662 | 34 | 287 + 3 x 96 = 575 vs 577 |
| END | 2 | 1755 (1755) | 17..117 | 624 (0) | 529 | 2 | 108 + 2 x 118 = 344 vs 239 (whole) |

### 57.2 The engine

- **The switch, in hand-written asm** (`engine/zone.s` `zone_mark`): the
  renderer already finds the camera's leaf and marks the PVS only when
  that leaf changed (r_world.s `mark_call`, a `jal` render.c patches for
  the draw distance). While a zoned map is loaded, render.c points that
  call at `zone_mark` instead (`mark_patch`): the leaf's set looked up (its
  index from its offset over 28, a multiply by 2^32/7), and when the set
  is not the last call's, `zone_enter` (C) makes its zone current if its
  chunk is in; then on to the marking render.c chose (`zone_rt.mark`).
  r_world.s is unchanged, byte for byte. The cost when the leaf has not
  changed: none; when it has, 17 instructions and the multiply's wait.
- **Making a chunk current** (`zone_patch`, asm): its patch words into the
  patched nodes' `faces[]`, then `r_state.vertpool` (and the walk's copy
  in the scratchpad, `SP_VERTPOOL`, which zone_mark takes again). 12
  cycles an empty node, 32 a word: 33-48 k cycles a switch on PCSX
  (54.6 k at most, E3M5), 18-27 k on DuckStation. Before any chunk is
  current every world block is empty (the skeleton's node offsets are 0).
  The C engine (`Q1PSX_ENGINE_C`) takes zone_patch's C (zone.c, written
  from the asm); zone_mark is the renderer's (r_world.s has no C).
- **The slots and the reads** (`engine/zone.c`, between frames: `zone_frame`
  before `stream_frame`): a read's progress, the late switch (the camera's
  zone in now: made current), then the next read: the camera's zone,
  then its two nearest, into a free slot, else the slot of a chunk no set
  wants, else the least wanted's below it, never the drawn one. A chunk is
  checked when it lands (its zone, the stamp the skeleton carries).
- **Late:** the camera in a zone whose chunk is not in yet: the old zone
  stays drawn (its records only), the frames are counted, the camera's
  zone is read first. Leaf 0 (the camera in solid) keeps the zone.
- **The drive** is shared: the stream starts no read while a chunk's read
  is in flight or waiting (`zone_drive_wanted`), the SPU music refills
  only when neither reads (`stream_reading` covers a chunk's), a chunk's
  read waits for the stream's to end and for the music's ring
  (`music_read_ok`). Music on the disc, DuckStation: no underrun.
- **The bench reads a view's zone at once** (`zone_sync`): before the
  frame, outside its cycles and before the stream's turn, the camera's
  leaf is found as r_render will (`r_set_view`'s arithmetic, `fl_loop`'s
  test) and its zone read and made current; the hook's own read at once
  stays as the fallback. In play nothing waits.
- **The dynamic lights** keep working on the current chunk: the asm reads
  the vertices through `SP_VERTPOOL` as before, the C reference
  (`DLIGHT_C`, the C engine) now through `r_state.vertpool`, and the light
  bytes are put back before any switch (game_render_done, then the next
  frame's hook).
- **The loader** (world.c) takes a QWVZ like a QWVE (the brush models'
  stream and pool); zone_map_start then finds `<MAP>_Z<tex>.ZON`, checks
  its header and allocates the slots; load_level reads the spawn eye's
  zone and its two nearest at once (`zone_preload`) before the stream's
  preload.
- **The build:** CMake `Q1PSX_ZONES` (ON) puts the zoned maps' skeletons
  and chunks on the disc, OFF the whole worlds (an A / B; also forced for
  a draw distance of 0 or past 1536, which the tables do not cover);
  `scripts/bench.sh WHOLE=1` builds that disc.

### 57.3 The drive's race (found on the way, fixed)

- The first 128-view run lost E4M5 ("bad models file"): a drive error
  20h ("wrong number of parameters") during its world's read; with a first
  fix in, a second run lost every load after E4M7's (the drive no longer
  answering). The parent had it too: its game bench read a sound from the
  wrong place ("not a sound this engine reads").
- psxcd's command writes (CdCommandF) select the register bank, reset the
  parameter FIFO, write the parameters, then the command, with the CD's
  interrupt enabled; the handler selects bank 1 and resets the FIFO. An
  interrupt in between (a read's Pause answering late: psxcd's read
  callback sends that Pause and nothing waits for it in a polled read)
  sends the command without its parameters (error 20h: after a lost
  Setloc the read goes on from wherever the drive was), and writes the
  parameters into bank 1's register 2, the interrupt enable: the drive
  goes quiet.
- `engine/cd.c` now has one access layer every read goes through
  (`cd_ready`, `cd_seek`, `cd_poll`, `cd_read_sync`, `cd_read_now`): a
  command waits for the drive to have been idle two vblanks (no read, no
  blocking command); the reads started between frames wait for it frame
  after frame, never in a frame; a refused Setloc re-enables the drive's
  interrupts as CdInit leaves them and is sent again.
- After it: no drive error in three 32-map runs, the zones' walks
  (PCSX-Redux and DuckStation, the music on), the game bench (whose sound
  now reads right).
- On the way, a bug of mine: a comment had swallowed a `return` in the
  stream's sound step (the gate turned around: a sound piece's seek
  waited out a chunk's whole read, 15 M cycles). Fixed; the pattern
  searched for elsewhere.

### 57.4 Identical frames

- `scripts/dlcheck.sh save` on the parent (00dbc44), `check` on the zones:
  - start's 12 views: IDENTICAL, -0.1 % cycles;
  - the 128 spawn views (`BENCH_MAPS=1 HASHONLY=1`): IDENTICAL, -0.5 %;
  - the game bench's 23 hashes (`BENCH_GAME=1 HASHONLY=1`): IDENTICAL;
  - under `EMU_ARGS=-interpreter`: 127 of 128 views identical, the 128th
    identical but for a character the interpreter's console doubled;
  - the C engine (`ENGINE_C=1`): start's 12 and the 128 views IDENTICAL
    (+87.6 %, +93.7 %, as in §55); its game bench differs at the
    graphics menu's effects page only (§55's dynamic warp threshold).
- The frames got cheaper: a chunk's blocks hold the zone's records only,
  so the record loop skips fewer records its frame never stamped (-2 k
  cycles on start's v2, v10, v11).
- `scripts/dlcheck.sh`: the game bench (no view lines) no longer counts as
  a failed bench.

### 57.5 The RAM, measured

The world's load (the boot log's `world: ... arena`: the texture tables,
the world or skeleton, the face stamps, the map's models, the slots) and
the stream's pool after it, the parent against the zones:

| map | zones | skeleton | slots | world load before -> after (KB) | freed | stream pool before -> after (KB) |
|---|---|---|---|---|---|---|
| START | 7 | 277 | 3 x 94 | 609 -> 575 | +34 | 471 -> 499 |
| E1M1 | 10 | 276 | 3 x 104 | 617 -> 605 | +12 | 434 -> 440 |
| E1M2 | 7 | 295 | 3 x 94 | 630 -> 592 | +38 | 385 -> 417 |
| E1M3 | 8 | 314 | 3 x 92 | 613 -> 605 | +8 | 334 -> 336 |
| E1M4 | 7 | 334 | 3 x 90 | 709 -> 621 | +88 | 278 -> 360 |
| E1M5 | whole | - | - | 569 -> 569 | 0 | 420 -> 414 |
| E1M6 | whole | - | - | 477 -> 477 | 0 | 480 -> 474 |
| E1M7 | whole | - | - | 190 -> 190 | 0 | 908 -> 902 |
| E1M8 | whole | - | - | 365 -> 365 | 0 | 700 -> 694 |
| E2M1 | 16 | 302 | 3 x 108 | 666 -> 644 | +22 | 343 -> 359 |
| E2M2 | 9 | 322 | 3 x 104 | 698 -> 652 | +46 | 332 -> 372 |
| E2M3 | 11 | 320 | 3 x 96 | 677 -> 625 | +52 | 324 -> 370 |
| E2M4 | 12 | 337 | 3 x 96 | 732 -> 642 | +90 | 266 -> 350 |
| E2M5 | 10 | 282 | 3 x 94 | 643 -> 581 | +62 | 355 -> 411 |
| E2M6 | whole | - | - | 622 -> 622 | 0 | 299 -> 293 |
| E2M7 | 9 | 331 | 3 x 106 | 707 -> 665 | +42 | 260 -> 296 |
| E3M1 | 9 | 277 | 3 x 104 | 623 -> 605 | +18 | 389 -> 401 |
| E3M2 | whole | - | - | 412 -> 412 | 0 | 581 -> 575 |
| E3M3 | whole | - | - | 497 -> 497 | 0 | 520 -> 514 |
| E3M4 | 8 | 371 | 3 x 114 | 736 -> 730 | +6 | 218 -> 218 |
| E3M5 | 8 | 353 | 3 x 112 | 784 -> 706 | +78 | 242 -> 314 |
| E3M6 | whole | - | - | 763 -> 763 | 0 | 221 -> 215 |
| E3M7 | whole | - | - | 576 -> 576 | 0 | 465 -> 459 |
| E4M1 | 9 | 302 | 3 x 94 | 647 -> 601 | +46 | 361 -> 401 |
| E4M2 | 9 | 285 | 3 x 96 | 609 -> 587 | +22 | 391 -> 407 |
| E4M3 | whole | - | - | 573 -> 573 | 0 | 384 -> 378 |
| E4M4 | 17 | 305 | 3 x 94 | 699 -> 603 | +96 | 283 -> 373 |
| E4M5 | 8 | 313 | 3 x 98 | 634 -> 622 | +12 | 304 -> 310 |
| E4M6 | whole | - | - | 506 -> 506 | 0 | 456 -> 450 |
| E4M7 | 10 | 378 | 3 x 94 | 781 -> 679 | +102 | 171 -> 267 |
| E4M8 | 7 | 287 | 3 x 96 | 592 -> 590 | +2 | 373 -> 369 |
| END | whole | - | - | 248 -> 248 | 0 | 833 -> 827 |

- The worst world load: 784 -> 763 KB (E3M6, whole, is the worst now);
  the smallest stream pool: **171 -> 215 KB** (+26 %); freed on a zoned
  map: 2-102 KB.
- The level arena shrank by 6 KB (1249 -> 1243 KB): the zones' code (6
  KB) would have come out of the stack, which keeps its room (release
  43704 B, the parent's 43504; §54's 25 KB margin kept). The whole maps'
  pools lose those 6 KB.
- **Honestly, far below §56's 150-250 KB**, for measured reasons:
  - the skeleton is bigger than §56 counted: it holds the brush models'
    records and vertices and a few tables the analysis left out (+20-40
    KB a map);
  - the default tables hold every record the PVS lets the PSX draw within
    1536 + 32 units (what makes the frames identical), duplicated across
    zones, and a slot is the map's biggest chunk: one outlier sets all
    three (E3M6's 153 KB zone keeps it whole; E3M4's and E3M5's 111-112);
  - the patch words take 2-4.4 KB a chunk.

### 57.6 The zones' walk

`ZONES=1 scripts/bench.sh` (the `bench zones` lines): on each zoned map,
the eye from the player's start through 31 of its items and monsters in
entity order at running speed (320 units a second), 3600 frames at most,
the stream and the zones' reads as in play. A leg whose eye line a wall
cuts goes through it; its frames are counted apart (a way shorter than
the walking distances the prefetch ranks zones by). Every patch is checked
against a C computation of it.

- PCSX-Redux, 20 maps: 8-29 switches a map (the patch above), 15-84 reads
  (9-16 frames each: 0.15-0.27 s), 0 read errors, 0 patches wrong; late:
  0-25 events and 0-373 frames a map, nearly all on the legs through
  walls (the clear legs' worst: E3M1 26 frames, E4M2 23, E2M3 21); the
  longest late run 20-39 frames.
- DuckStation, the music on the disc: the same walk, 0 read errors, 0
  patches wrong, no `music: the ring ran dry`; reads 14-26 frames each
  (DuckStation's seeks are slower), late runs 28-59 frames.
- The frames that switch also load what comes into view (the stream's
  passes and installs): 0.9-1.6 M cycles at most; the patch itself is
  33-55 k of it.

### 57.7 DuckStation

- The AppImage no longer started headless on this host (since §42: every
  start waited before the boot). Found: the AppImage's FUSE runtime sits
  blocked writing to a pipe. Its extracted binary starts at once
  (`DuckStation-x64.AppImage --appimage-extract`, then
  `squashfs-root/AppRun -batch -nogui -earlyconsole -- q1psx.cue`, the
  private home as in §26.1).
- The play bench (`BENCH_MAPS=1 PLAY=1 HASHONLY=1 MUSIC=1`): 32 maps
  loaded and played, `bench play <MAP> ok` each, `bench done`, no GP0
  error, no drive error (psxcd printed `CdSync() timeout` once inside
  CdInit, before the engine runs: DuckStation's drive answers its init
  later than psxcd waits). The zones' walk above.

### 57.8 The choices made, and the alternatives

- **The tables: `pvs_1536_96`** (the default, `local.env`): safe by
  construction, the frames identical. The four sets, measured (the
  skeleton + the slots, from the cook; the bench's for PVS 96 and seen 64):

  | tables | maps zoned | the worst world file (cook) | its file freed a map: most / mean | the worst world load (bench) | the smallest stream pool (bench) | the frames |
  |---|---|---|---|---|---|---|
  | (whole worlds) | 0 | 767 KB | — | 784 KB | 171 KB | — |
  | pvs_1536_96 | 20 | 745 KB | 103 / 27 KB | 763 KB | 215 KB | identical |
  | pvs_1536_128 | 2 | 763 KB | 36 / 1 KB | — | — | identical |
  | seen_1536_96 | 24 | 652 KB | 132 / 47 KB | — | — | not run |
  | seen_1536_64 | 29 | 564 KB | 221 / 116 KB | 583 KB | **363 KB** | 22 of 128 spawn views' lists differ; their pictures identical (below) |

  - **seen_1536_64** gives the room asked for (+192 KB on the
    worst map's pool, E4M7 171 -> 363 KB) and cheaper frames (-1.6 % on
    the 128 views): its chunks leave out the records the 2048 px renders
    never saw, which the PSX draws and then paints over. On 36 views
    (E1M5, E3M6, E4M4, 12 each, their spawn views included) the pictures
    are identical pixel for pixel (pktview). The risk: a record seen only
    from a place the scan did not sample (16-unit eyes; §56: the renders'
    tail is slivers under a PSX pixel) would be missing there. To take
    it: `Q1PSX_ZONE_TABLES=.../zones/seen_1536_64` in local.env.
- **Three fixed slots, each the map's biggest chunk**, rather than an
  arena holding just the biggest set (a zone and its two nearest) and
  compacted when in pieces. That arena would take 10-80 KB less a map,
  but a simulation of random walks over the 32 maps' zone sets (4000
  zone changes a map) found a compaction in 15-19 % of the loads, most of
  them moving the drawn chunk (a memmove and a re-patch: 150-300 k cycles,
  a frame lost); with 64 KB of slack the drawn chunk never moved, but
  25-30 % of the loads still moved others and the slack ate the gain.
  Slots never move a chunk.
- **The current zone and its two nearest** (the model asked for); a fourth
  slot (the third nearest, which the tables give) would take one more
  chunk of RAM for fewer late frames.
- **A map stays whole** when zoning would not reduce its resident world.
- **The bench reads a view's zone at once**.

### 57.9 Left

- **The skeleton** (84-378 KB a map, the bulk of what stays): the PVS rows
  per zone (34-49 KB a map today), the clip hulls (55-81 KB) and the nodes
  (56-130 KB) kept only where the player can be. That is where the room
  wanted for the streamed assets is now.
- **The outlier zones** (E3M6's 153 KB, END's 118, E3M4's and E3M5's
  111-112): re-exported with a smaller cap by the analysis tool, E3M6
  would zone and every slot of those maps would shrink.
- The late frames when moving fast: a fourth slot, or prefetching on the
  walking distance's lead (the tables carry it; the engine does not keep
  it).
- psxcd busy-waits for every command's acknowledge: starting a read costs
  about 90 k cycles (the stream's reads as the zones'); an asynchronous
  command layer would give them back.
- The SDK's memcpy / memmove copy a byte at a time (13 cycles a byte on
  PCSX): the stream's pool compaction uses memmove.
- The patch could be unrolled (pairs of empty nodes: about -3 cycles
  each), or cut to the nodes whose blocks change by grouping the patched
  nodes by zone (dummy nodes between the groups).
- Not checked: the real machine; a hands-on play across zones.

## §58 — The skeleton's RAM: measured, and its repeats shared (2026-10-22)

- **The ask** (§57.9, from the session that built the zones): a zoned
  map keeps its whole skeleton resident, 276-378 KB, now the bulk of the
  world's RAM; study, measure and propose ways to shrink it (the PVS rows
  per zone; the clip hulls and the nodes only where the player can be;
  anything else the measures show), each option with the RAM it frees on
  the 32 maps, its CPU cost, its risk and what the asm would need, and
  implement the option the measures favour if it is self-contained. The
  frames stay the branch tip's (781649c), hash for hash. On its own
  branch (on top of the zones' branch),
  not merged.
- **The tools** (scratch, not in the repo: they read Quake-derived files):
  a C reader of the cooked worlds, skeletons and chunk files, the zone
  tables and the analysis' reachable eyes (`quakespasm-q1psx/
  scan/<map>/samples.bin`, §56); a structural comparison of two worlds
  (every section byte for byte, the leaves' and the clip hulls'
  equivalence, below).

### 58.1 What the skeleton holds

Before this section (KB; the zoned maps' skeleton from the cook, the
other sections of every map's world; "brush" = the brush models'):

| map | skeleton | nodes: world + brush | leaves: world + brush | PVS | clip: planes + clipnodes | marks | entities + strings | brush models' records + vertices |
|---|---|---|---|---|---|---|---|---|
| START | 278 | 85.1 + 17.1 | 30.9 + 12.0 | 33.4 | 22.5 + 31.2 | 10.9 | 7.9 + 1.5 | 14.0 + 3.6 |
| E1M1 | 277 | 82.8 + 13.9 | 31.4 + 10.4 | 39.9 | 21.2 + 31.7 | 11.5 | 9.5 + 0.9 | 12.8 + 2.9 |
| E1M2 | 296 | 85.3 + 14.4 | 30.4 + 10.8 | 31.0 | 24.4 + 47.7 | 11.1 | 14.3 + 1.0 | 14.3 + 3.3 |
| E1M3 | 315 | 76.8 + 26.6 | 26.5 + 19.7 | 24.9 | 32.1 + 34.3 | 9.9 | 19.9 + 1.0 | 27.4 + 5.7 |
| E1M4 | 335 | 91.3 + 21.0 | 33.7 + 16.0 | 36.7 | 34.5 + 40.7 | 12.5 | 17.4 + 1.1 | 16.6 + 3.8 |
| E1M5 | (whole) | 73.1 + 17.9 | 25.5 + 13.3 | 20.7 | 30.9 + 31.1 | 9.7 | 17.7 + 1.0 | - |
| E1M6 | (whole) | 54.4 + 31.6 | 17.7 + 22.5 | 18.2 | 19.4 + 24.3 | 6.6 | 14.8 + 0.9 | - |
| E1M7 | (whole) | 21.7 + 9.1 | 7.7 + 6.8 | 5.9 | 9.6 + 11.2 | 2.9 | 6.2 + 0.6 | - |
| E1M8 | (whole) | 39.3 + 7.8 | 13.9 + 5.7 | 15.9 | 9.3 + 18.3 | 6.3 | 9.7 + 0.7 | - |
| E2M1 | 302 | 90.8 + 18.5 | 29.9 + 13.8 | 33.9 | 21.3 + 35.8 | 11.7 | 14.0 + 0.9 | 18.5 + 4.0 |
| E2M2 | 323 | 102.9 + 13.6 | 34.4 + 10.1 | 43.3 | 28.3 + 39.3 | 12.3 | 11.2 + 0.9 | 14.9 + 3.8 |
| E2M3 | 320 | 93.8 + 17.9 | 30.0 + 13.6 | 32.6 | 30.0 + 42.5 | 11.8 | 14.3 + 1.0 | 19.6 + 4.3 |
| E2M4 | 338 | 100.0 + 16.9 | 34.4 + 12.7 | 42.1 | 24.1 + 45.1 | 12.7 | 16.8 + 1.0 | 18.6 + 4.4 |
| E2M5 | 283 | 78.1 + 16.1 | 27.0 + 11.9 | 25.6 | 24.4 + 39.2 | 11.5 | 14.8 + 0.9 | 21.0 + 4.8 |
| E2M6 | (whole) | 80.8 + 23.7 | 24.6 + 17.7 | 22.2 | 26.3 + 36.1 | 10.0 | 21.7 + 1.2 | - |
| E2M7 | 332 | 95.1 + 21.8 | 33.8 + 16.5 | 33.3 | 32.1 + 35.8 | 12.0 | 21.2 + 1.1 | 15.4 + 3.6 |
| E3M1 | 277 | 77.5 + 17.7 | 27.4 + 13.3 | 30.0 | 19.8 + 36.2 | 11.3 | 13.5 + 0.7 | 17.5 + 4.3 |
| E3M2 | (whole) | 47.1 + 27.6 | 14.3 + 18.9 | 10.1 | 15.7 + 23.4 | 5.7 | 12.0 + 0.8 | - |
| E3M3 | (whole) | 66.9 + 16.1 | 23.3 + 12.1 | 19.2 | 17.6 + 23.4 | 8.7 | 12.4 + 0.8 | - |
| E3M4 | 371 | 91.4 + 36.0 | 31.9 + 26.1 | 31.8 | 26.7 + 43.8 | 12.0 | 15.3 + 1.1 | 35.0 + 8.1 |
| E3M5 | 354 | 107.8 + 15.2 | 38.9 + 11.4 | 48.8 | 31.7 + 47.3 | 13.7 | 14.2 + 0.9 | 12.4 + 3.2 |
| E3M6 | (whole) | 106.8 + 16.8 | 38.4 + 12.2 | 43.1 | 28.1 + 37.9 | 13.9 | 17.8 + 1.2 | - |
| E3M7 | (whole) | 81.4 + 17.6 | 27.9 + 13.5 | 24.3 | 19.1 + 22.9 | 10.6 | 12.0 + 0.9 | - |
| E4M1 | 302 | 89.6 + 17.6 | 33.3 + 12.7 | 37.8 | 22.7 + 33.1 | 11.0 | 15.6 + 1.1 | 15.8 + 3.5 |
| E4M2 | 285 | 70.7 + 19.1 | 29.2 + 14.0 | 30.3 | 27.0 + 35.4 | 11.4 | 13.9 + 1.0 | 20.6 + 4.6 |
| E4M3 | (whole) | 69.0 + 27.5 | 25.1 + 21.2 | 23.8 | 29.0 + 36.1 | 8.8 | 20.0 + 1.2 | - |
| E4M4 | 306 | 85.5 + 12.0 | 34.3 + 9.3 | 40.4 | 24.5 + 43.6 | 14.1 | 21.5 + 1.4 | 9.1 + 2.1 |
| E4M5 | 314 | 67.6 + 25.2 | 26.2 + 18.5 | 22.2 | 30.2 + 44.3 | 11.4 | 21.8 + 1.1 | 28.9 + 7.0 |
| E4M6 | (whole) | 56.1 + 22.1 | 22.0 + 16.7 | 18.2 | 24.0 + 32.1 | 8.8 | 19.5 + 1.3 | - |
| E4M7 | 379 | 100.5 + 25.8 | 39.3 + 19.7 | 41.4 | 36.4 + 44.8 | 14.4 | 19.5 + 1.2 | 20.0 + 4.1 |
| E4M8 | 287 | 72.1 + 21.3 | 27.0 + 16.0 | 20.4 | 28.2 + 36.1 | 10.6 | 17.6 + 1.0 | 22.8 + 5.4 |
| END | (whole) | 21.9 + 11.5 | 8.4 + 8.7 | 5.5 | 14.9 + 11.9 | 5.0 | 9.5 + 0.9 | - |

- The nodes (22-108 KB of the world's, 8-36 KB of the brush models'),
  the clip hulls (planes 9-36, clipnodes 11-48), the leaves, the PVS
  (6-49) and, on a zoned map, the brush models' records (11-43): the
  zones' tables add 2 KB at most.
- Three findings carry the rest of this section:
  - **the brush models' leaves** (210-953 a map) are all empty leaves
    with no marks and no PVS row (Quake numbers them after the world's
    1..visleafs). Nothing reads them but their contents byte: the
    renderer's brush model walk stops at a leaf (r_world.s `bm_walk`),
    the point hull's traces read the contents (clip.c `child_of`,
    clip.s `LEAF`). 28 bytes each, and 2 of ambient (3 on a zoned map,
    its leaf set);
  - **qbsp's clip hulls repeat themselves**: hash-consing the clipnodes
    (the same plane, children of the same contents or the same shared
    node) leaves 69-87 % of them (E1M2: 8148 -> 5639);
  - **the PVS of every place the player can reach covers 78-100 % of
    the leaves**, so pruning the tree to where the player can be finds
    next to nothing (58.3).

### 58.2 The options, measured on the 32 maps

KB a map would free (A + B measured on the cooked files; the others
computed from the cooked data as described below):

| map | A: the brush models' leaves shared | B: the clip hulls' repeats shared | A + B, measured | C: a compact entity table | D: the PVS rows as deltas | E: the PVS rows per zone | F: 16-bit node children |
|---|---|---|---|---|---|---|---|
| START | 13.2 | 5.2 | **18.4** | 5.4 | 8.5 | 9.4 | 11.4 |
| E1M1 | 11.5 | 5.4 | **16.9** | 6.2 | 6.6 | 3.9 | 10.8 |
| E1M2 | 11.9 | 14.7 | **26.6** | 9.0 | 9.8 | 19.0 | 11.1 |
| E1M3 | 21.8 | 5.1 | **26.9** | 12.8 | 6.2 | 0.9 | 11.5 |
| E1M4 | 17.7 | 6.6 | **24.3** | 11.4 | 8.8 | 18.7 | 12.5 |
| E1M5 | 14.2 | 4.5 | **18.7** | 11.3 | 5.3 | - | 10.1 |
| E1M6 | 24.1 | 3.6 | **27.7** | 9.8 | 3.5 | - | 9.6 |
| E1M7 | 7.3 | 2.2 | **9.5** | 4.2 | 1.4 | - | 3.4 |
| E1M8 | 6.1 | 4.4 | **10.5** | 6.2 | 2.7 | - | 5.2 |
| E2M1 | 15.3 | 6.9 | **22.2** | 9.2 | 8.3 | 15.9 | 12.2 |
| E2M2 | 11.2 | 7.1 | **18.3** | 7.3 | 10.3 | 31.3 | 13.0 |
| E2M3 | 15.0 | 7.9 | **22.9** | 9.4 | 7.3 | 14.6 | 12.4 |
| E2M4 | 14.0 | 9.4 | **23.4** | 10.8 | 10.3 | 18.1 | 13.0 |
| E2M5 | 13.2 | 8.6 | **21.8** | 9.5 | 5.0 | 7.6 | 10.5 |
| E2M6 | 18.9 | 6.0 | **24.9** | 14.0 | 5.2 | - | 11.6 |
| E2M7 | 18.3 | 5.9 | **24.2** | 13.7 | 9.0 | 15.3 | 13.0 |
| E3M1 | 14.8 | 7.0 | **21.8** | 8.9 | 8.2 | 18.0 | 10.6 |
| E3M2 | 20.3 | 4.9 | **25.2** | 8.1 | 2.1 | - | 8.3 |
| E3M3 | 12.9 | 4.5 | **17.4** | 8.1 | 3.8 | - | 9.2 |
| E3M4 | 28.8 | 9.3 | **38.1** | 10.4 | 5.9 | 7.8 | 14.2 |
| E3M5 | 12.6 | 10.2 | **22.8** | 9.2 | 11.7 | 24.8 | 13.7 |
| E3M6 | 13.1 | 7.0 | **20.1** | 11.5 | 8.0 | - | 13.7 |
| E3M7 | 14.5 | 3.7 | **18.2** | 8.1 | 5.3 | - | 11.0 |
| E4M1 | 14.0 | 6.0 | **20.0** | 9.9 | 8.9 | 19.8 | 11.9 |
| E4M2 | 15.4 | 6.8 | **22.2** | 9.2 | 6.9 | 12.3 | 10.0 |
| E4M3 | 22.7 | 7.0 | **29.7** | 13.1 | 6.1 | - | 10.7 |
| E4M4 | 10.2 | 10.2 | **20.4** | 13.8 | 9.5 | 28.4 | 10.8 |
| E4M5 | 20.4 | 8.7 | **29.1** | 14.2 | 4.7 | 4.2 | 10.3 |
| E4M6 | 17.8 | 6.0 | **23.8** | 12.9 | 4.5 | - | 8.7 |
| E4M7 | 21.8 | 6.8 | **28.6** | 12.9 | 11.1 | 23.4 | 14.0 |
| E4M8 | 17.7 | 5.6 | **23.3** | 11.6 | 5.5 | 8.4 | 10.4 |
| END | 9.3 | 1.5 | **10.8** | 6.2 | 0.9 | - | 3.7 |

- **A, the brush models' leaves shared** (implemented, 58.4): one leaf
  per contents value after the world's (Quake's maps: one, empty). CPU:
  none. Risk: none found (every reader checked; the structural
  comparison and the frames below). The asm: nothing.
- **B, the clip hulls' repeats shared** (implemented, 58.4): a trace or
  a contents walk depends on each node's plane and children only, so a
  DAG gives every result bit for bit (the asm's guard node, `PS_GNODE`,
  is a subtree root: the same subtree). CPU: none. The asm: nothing.
- **C, a compact entity table**: the cooked `qw_ent` is 56 bytes, most
  of them zero; a class byte, a 4-byte mask of the 25 fields present and
  those fields take a third (6.2-21.8 -> 2.0-7.7 KB). `ents_spawn`
  decodes one into a `qw_ent` on the stack for the spawn functions (none
  keeps the pointer: checked), the cook's bmf.c and world_check read it
  the same way (a shared inline decoder in formats/qworld.h). CPU: at a
  map's spawn and a restart only (about 100-200 cycles an entity, under
  80 k cycles a map). Risk: low; a format change (a new magic, so the
  analysis tool's reader, `analysis/wld.py`, asserting "QWVE", needs the
  same change outside the repo). The asm: nothing. Self-contained, not
  done here: it changes the format and the engine; next in line.
- **D, the PVS rows as deltas**: each row Quake's run-length bytes, or
  the XOR against a plain row among the 8 before it, run-length coded
  (a delta's reference always plain: no chains; 68-89 % of the rows
  become deltas). 16-32 % of the PVS (0.9-11.7 KB). CPU: `mark_pvs`
  reads two streams for a delta row: about +12-20 cycles a row byte
  (39-180 bytes), +0.5-3.6 k cycles each time the camera's leaf changes,
  on top of its stamping (unchanged); ai.c's checkclient decodes the
  same in C every tenth of a second. The asm: `MARKPVS` (r_world.s)
  rewritten for the two streams, by hand; the C reference too. Risk:
  low (the same bits); the frames' cost on leaf changes.
- **E, the PVS rows per zone** (the §57.9 idea): a chunk carries the
  rows of its zone's leaves; the switch points those leaves at them.
  A slot then grows by the biggest zone's chunk + rows (+4-12 KB, × 3
  slots), so the net is 0.9-31.3 KB on the 20 zoned maps (whole maps:
  nothing). CPU: the switch rewrites up to 319 leaves' vis words, keeping
  their render-list byte: about +5 k cycles on the 33-55 k of the patch;
  the frames: none. The asm: zone.s `zone_mark` must not mark from a
  leaf whose row is not in (a late zone: the old marking kept, s5 taken
  from r_state for `mark_done`); r_world.s unchanged. Risk: behaviour in
  play, not in the bench (`zone_sync`): while a zone is late the PVS is
  the last leaf's (today the new leaf's, drawn from the old chunk), and
  ai.c's checkclient finds no row for an eye in a zone not in (it would
  keep its last row: a monster notices the player up to a late read
  later). D and E overlap: D shrinks the rows E moves.
- **F, 16-bit node children** (word offsets, a tag bit for a leaf):
  -4 bytes a node, 3.4-14.2 KB. CPU: every child fetched costs an
  `lhu`, a shift and an add (+5 cycles), 93-378 nodes a frame on start's
  views: +0.5-2 k cycles a frame, plus the hull-0 traces' nodes (clip.s),
  bmfrag's and the dynamic lights' walks. The asm: the world walk, the
  brush model walk, the camera leaf's descent, `mark_pvs`'s parents
  (unchanged: they are offsets already), clip.s's hull 0, bmfrag.s,
  the dynamic lights' asm; their C references. Risk: a large change of
  the hottest code for 11 KB a map. Not recommended before C and D.
- **The edicts** (not the skeleton, the same level arena): the pool is
  `num_ents + 2 + 96` edicts of 388 bytes whatever the skill:
  - the entities single player never spawns (all three skill flags set:
    Quake's deathmatch-only entities, 2-36 a map) keep their 56-byte entry
    and an edict slot each: 0.9-15.6 KB. The cook could leave them out
    (C's step: `ents_spawn` skips them before taking an edict, so the
    spawned edicts' numbers do not change);
  - those the skill leaves out keep their slots too: up to 38 KB (E1M3:
    98 entities at easy, 41 at hard: 37 / 16 KB). Sizing the pool by the
    skill's spawn count frees that.
  Both shrink the runtime spawns' headroom (gibs, packs, delayed uses,
  door triggers): today those slots come on top of the 96 spare ones, so
  the peak in use needs measuring first. Not the skeleton's: listed.

### 58.3 The clip hulls and the nodes where the player can be

The analysis' reachable eyes (§56's flood: walking, jumping, riding,
teleporting; 59-373 k eyes a map) against the cooked world:

| map | eyes sampled | camera leaves / world leaves | their PVS's union | world nodes above it | hull-1 clipnodes the origins reach | hull-2 monsters (easy / normal / hard) |
|---|---|---|---|---|---|---|
| START | 89184 | 325 / 1128 | 1100 (98 %) | 2361 of 2422 (97 %) | 1685 of 2585 (65 %) | 0 / 0 / 0 |
| E1M1 | 104341 | 468 / 1148 | 1137 (99 %) | 2326 of 2355 (99 %) | 2011 of 2556 (79 %) | 1 / 5 / 8 |
| E1M2 | 125041 | 548 / 1109 | 1057 (95 %) | 2314 of 2425 (95 %) | 2054 of 2887 (71 %) | 5 / 7 / 15 |
| E1M3 | 113136 | 407 / 967 | 932 (96 %) | 2096 of 2185 (96 %) | 1636 of 2130 (77 %) | 6 / 10 / 23 |
| E1M4 | 249475 | 562 / 1230 | 1189 (97 %) | 2472 of 2597 (95 %) | 2254 of 3427 (66 %) | 7 / 14 / 24 |
| E1M5 | 127544 | 342 / 932 | 804 (86 %) | 1855 of 2080 (89 %) | 1462 of 2237 (65 %) | 7 / 15 / 25 |
| E1M6 | 77947 | 213 / 648 | 635 (98 %) | 1506 of 1548 (97 %) | 995 of 1442 (69 %) | 8 / 16 / 22 |
| E1M7 | 78066 | 106 / 281 | 255 (91 %) | 576 of 616 (94 %) | 607 of 749 (81 %) | 13 / 13 / 13 |
| E1M8 | 287638 | 232 / 508 | 477 (94 %) | 1040 of 1117 (93 %) | 1256 of 1619 (78 %) | 13 / 21 / 27 |
| E2M1 | 151416 | 508 / 1092 | 1041 (95 %) | 2452 of 2584 (95 %) | 2508 of 3053 (82 %) | 2 / 5 / 7 |
| E2M2 | 149163 | 510 / 1256 | 1112 (89 %) | 2625 of 2928 (90 %) | 2519 of 3544 (71 %) | 11 / 19 / 20 |
| E2M3 | 220373 | 519 / 1095 | 1092 (100 %) | 2653 of 2667 (99 %) | 2600 of 3472 (75 %) | 5 / 10 / 19 |
| E2M4 | 165952 | 495 / 1258 | 1184 (94 %) | 2691 of 2845 (95 %) | 2386 of 3621 (66 %) | 12 / 28 / 38 |
| E2M5 | 263986 | 504 / 985 | 970 (98 %) | 2182 of 2222 (98 %) | 2551 of 2997 (85 %) | 14 / 26 / 33 |
| E2M6 | 166595 | 474 / 897 | 871 (97 %) | 2230 of 2298 (97 %) | 2046 of 2682 (76 %) | 18 / 32 / 41 |
| E2M7 | 132351 | 506 / 1236 | 1201 (97 %) | 2636 of 2705 (97 %) | 2112 of 2849 (74 %) | 11 / 21 / 25 |
| E3M1 | 245155 | 609 / 1000 | 945 (94 %) | 2084 of 2204 (95 %) | 2266 of 2822 (80 %) | 3 / 6 / 9 |
| E3M2 | 80921 | 227 / 522 | 481 (92 %) | 1246 of 1340 (93 %) | 1366 of 1797 (76 %) | 10 / 14 / 23 |
| E3M3 | 74258 | 335 / 852 | 809 (95 %) | 1782 of 1903 (94 %) | 1576 of 1999 (79 %) | 5 / 9 / 14 |
| E3M4 | 161437 | 540 / 1166 | 1103 (95 %) | 2462 of 2599 (95 %) | 2193 of 3163 (69 %) | 17 / 24 / 30 |
| E3M5 | 372598 | 672 / 1423 | 1377 (97 %) | 2976 of 3065 (97 %) | 3087 of 4092 (75 %) | 11 / 19 / 24 |
| E3M6 | 197808 | 479 / 1403 | 1214 (87 %) | 2634 of 3039 (87 %) | 1952 of 3126 (62 %) | 15 / 30 / 39 |
| E3M7 | 59068 | 375 / 1021 | 955 (94 %) | 2169 of 2316 (94 %) | 1327 of 1816 (73 %) | 18 / 22 / 24 |
| E4M1 | 155732 | 510 / 1218 | 1194 (98 %) | 2480 of 2549 (97 %) | 1952 of 2631 (74 %) | 4 / 5 / 5 |
| E4M2 | 146279 | 481 / 1067 | 1055 (99 %) | 1995 of 2010 (99 %) | 2402 of 3044 (79 %) | 10 / 16 / 18 |
| E4M3 | 148866 | 434 / 918 | 893 (97 %) | 1899 of 1964 (97 %) | 2027 of 2701 (75 %) | 7 / 16 / 25 |
| E4M4 | 269701 | 512 / 1255 | 1237 (99 %) | 2391 of 2433 (98 %) | 2995 of 3899 (77 %) | 12 / 21 / 33 |
| E4M5 | 224276 | 511 / 959 | 933 (97 %) | 1834 of 1922 (95 %) | 2718 of 3466 (78 %) | 9 / 13 / 19 |
| E4M6 | 191061 | 465 / 804 | 774 (96 %) | 1521 of 1596 (95 %) | 1884 of 2320 (81 %) | 10 / 14 / 17 |
| E4M7 | 233067 | 649 / 1436 | 1405 (98 %) | 2756 of 2860 (96 %) | 2728 of 3465 (79 %) | 12 / 17 / 21 |
| E4M8 | 182324 | 384 / 988 | 916 (93 %) | 1930 of 2050 (94 %) | 2095 of 2977 (70 %) | 13 / 23 / 35 |
| END | 164974 | 133 / 305 | 237 (78 %) | 518 of 624 (83 %) | 696 of 788 (88 %) | 10 / 14 / 14 |

- **The render nodes:** the PVS's union of every leaf an eye reaches
  covers 78-100 % of the world's leaves, and the nodes above them 83-99 %
  of the nodes: 1-17 % never drawn from a reachable place. But the game
  walks the same tree for its point queries, the hull-0 traces (shots,
  missiles, lines of sight), CheckBottom and the entities' leaves,
  anywhere monsters are (their closets too): only the render half of
  those nodes could go, with a 6-byte game tree kept for all of them.
  Nothing to gain.
- **The hull-1 clipnodes:** the origins reach 62-88 % of the world's.
  The rest is space an explosion, a lift's top or a fall can still put
  the player in, and the monster closets the monsters walk in: only
  empty regions no entity and no player can ever enter could go, which
  needs the hulls' portals (the BSP has none for them) and would free a
  part of those 12-38 % at best: 1-7 KB a map. Not worth its risk.
- **Hull 2** is the dogs', ogres', fiends', shamblers' and vores' (a box
  over 32 units wide, `ai.c hull_of`): every map but START has one at
  every skill. START alone could leave it out (17 KB, the map with the
  most room). Not done.
- **The marks per zone** (only the marks of the chunk's records, for the
  leaves its PVS reaches): 2.8-5.0 KB a zone against 2.9-14.4 KB in all;
  three slots of it cost what the whole takes. Nothing to gain.

### 58.4 A and B in the cook (implemented)

- `tools/qcook/world.c`: the brush models' leaves become one leaf per
  contents after the world's (`num_leaves` = visleafs + 2 on Quake's
  maps), their nodes' leaf children and the leaf roots follow; the
  shared leaf has no marks, no row, no parent, light 128, ambient 0. A
  map whose leaves past the world's held marks, a row or a world node's
  child would keep them all (none does). The clipnodes are hash-consed
  (`clip_share`) and numbered depth first from the roots in Quake's
  order (the world's hull 1, the brush models', then the hulls 2): hull
  1's nodes stay first. The planes are untouched (the render nodes index
  them). The cook prints `the brush models' N leaves -> 1 shared` and
  `clip hulls: N clipnodes -> M`.
- `tools/qcook/zones.c`: a table that counts the old leaves (the tables
  of §56, made before) still matches: the world's leaves are the same,
  the camera is never past them; those leaves' sets are QZ_NONE (a set
  only a brush model's leaf used is gone).
- No engine change: r_world.s, clip.s, zone.s and the C engine are as
  they were; the format is QWVE / QWVZ as before.

**Checked:**
- the structural comparison, old cook against new, the 32 worlds and
  the 20 skeletons: every section byte for byte but the leaves past the
  world's (the shared one: no marks, no row, no parent), the node
  children that pointed at them (to a leaf of the same contents), the
  clipnodes (every root's hull, the world's and each brush model's, both
  hulls, equivalent node for node) and the brush models' clip roots; the
  zone header, the world leaves' sets and the sets they use identical.
  The 40 chunk files are byte for byte the old ones but their stamps;
- `scripts/dlcheck.sh` against the tip's references (saved on 781649c,
  themselves identical to the parent's): start's 12 views, the 128 spawn
  views (`BENCH_MAPS=1 HASHONLY=1`), the game bench's 23 hashes
  (`BENCH_GAME=1 HASHONLY=1`): IDENTICAL, the frames' cycles -0.0 %
  (6209214 -> 6209152; 45344907 -> 45343594); the C engine
  (`ENGINE_C=1`): start's 12 and the 128 views IDENTICAL;
- the bench's trace checks: `clipdiff` (4000 traces, 4000 contents,
  4000 lines of sight, box nodes) and `pmdiff` (6000 ticks) 0 mismatches,
  with the same results as the tip's (629 traces hit, 1947 start in
  solid; the same pmove positions); an asm trace 2665 -> 2640 cycles;
- the zones' walk on E3M4 (`ZONES=1 MAP=E3M4`): 17 switches, 0 patches
  checked bad, 22 reads, 0 read errors, the late frames through walls.

**The RAM, measured** (the bench's `bench zone <MAP>` and `world: ...
arena` lines, KB, the tip -> this section):

| map | skeleton | the world's load | the stream's pool |
|---|---|---|---|
| START | 277 -> 259 | 575 -> 557 | 499 -> 517 (+18) |
| E1M1 | 276 -> 259 | 605 -> 587 | 440 -> 458 (+18) |
| E1M2 | 295 -> 268 | 592 -> 566 | 417 -> 443 (+26) |
| E1M3 | 314 -> 287 | 605 -> 577 | 336 -> 364 (+28) |
| E1M4 | 334 -> 310 | 621 -> 597 | 360 -> 384 (+24) |
| E1M5 | whole | 569 -> 551 | 414 -> 432 (+18) |
| E1M6 | whole | 477 -> 449 | 474 -> 502 (+28) |
| E1M7 | whole | 190 -> 180 | 902 -> 912 (+10) |
| E1M8 | whole | 365 -> 355 | 694 -> 704 (+10) |
| E2M1 | 302 -> 279 | 644 -> 620 | 359 -> 383 (+24) |
| E2M2 | 322 -> 304 | 652 -> 634 | 372 -> 390 (+18) |
| E2M3 | 320 -> 297 | 625 -> 601 | 370 -> 394 (+24) |
| E2M4 | 337 -> 314 | 642 -> 620 | 350 -> 372 (+22) |
| E2M5 | 282 -> 261 | 581 -> 559 | 411 -> 433 (+22) |
| E2M6 | whole | 622 -> 596 | 293 -> 319 (+26) |
| E2M7 | 331 -> 307 | 665 -> 641 | 296 -> 320 (+24) |
| E3M1 | 277 -> 255 | 605 -> 583 | 401 -> 423 (+22) |
| E3M2 | whole | 412 -> 386 | 575 -> 601 (+26) |
| E3M3 | whole | 497 -> 479 | 514 -> 532 (+18) |
| E3M4 | 371 -> 332 | 730 -> 692 | 218 -> 256 (+38) |
| E3M5 | 353 -> 331 | 706 -> 684 | 314 -> 336 (+22) |
| E3M6 | whole | 763 -> 743 | 215 -> 235 (+20) |
| E3M7 | whole | 576 -> 558 | 459 -> 477 (+18) |
| E4M1 | 302 -> 282 | 601 -> 581 | 401 -> 421 (+20) |
| E4M2 | 285 -> 262 | 587 -> 565 | 407 -> 429 (+22) |
| E4M3 | whole | 573 -> 543 | 378 -> 408 (+30) |
| E4M4 | 305 -> 285 | 603 -> 583 | 373 -> 393 (+20) |
| E4M5 | 313 -> 284 | 622 -> 594 | 310 -> 338 (+28) |
| E4M6 | whole | 506 -> 482 | 450 -> 474 (+24) |
| E4M7 | 378 -> 350 | 679 -> 651 | 267 -> 295 (+28) |
| E4M8 | 287 -> 263 | 590 -> 566 | 369 -> 393 (+24) |
| END | whole | 248 -> 238 | 827 -> 837 (+10) |

- **9.5-38.1 KB freed a map, 22 on average, on all 32 maps** (the whole
  ones too); the smallest stream pool 215 -> 235 KB (E3M6, whole), the
  worst world load 763 -> 743 KB; a load reads that much less from the
  disc. The zoned / whole split is unchanged (both sides shrink alike).
- Not checked: DuckStation, the real machine, the release build (no
  engine change: the same code, smaller files).

### 58.5 Left open

- **C, the compact entity table**: 4.2-14.2 KB a map, no frame cost, a
  format change (the analysis tool's reader to follow outside the repo).
  The next self-contained step (done: §59, 5.0-17.6 KB a map).
- **D, the PVS as deltas** (0.9-11.7 KB, asm `MARKPVS` by hand) and
  **E, the PVS rows per zone** (0.9-31.3 KB on the zoned maps, a
  behaviour change while a zone is late): E frees more on the zoned maps,
  D on every map, both together less than their sum.
- **The edicts by the skill's count**: up to 38 KB, once the peak of the
  runtime spawns is measured (§59.5: the slots the benches' play leaves
  free).
- **F, 16-bit node children**: 11 KB a map for the hottest code's
  rewrite; last.
- The tables' re-export (§56's tool) on the new worlds would count the
  new leaves; `analysis/export.py` already skips leaves past visleafs.

## §59 — The entities in a compact table (2026-10-22)

- **The ask** (§58.5's next step): option C of §58.2, the cooked entity
  table made compact, the frames the branch tip's (fea0ee3) hash for
  hash; and the entities single player never spawns left out of the
  cook, but only if the runtime spawns are measured never to need the
  edict slots those entities leave free (otherwise listed, not done). On
  its own branch (on top of §58's
  branch), not merged.
- **The result:**
  - the entities take 21.6 % of their old bytes: 5.0-17.6 KB less a map
    (§58.2's layout would have freed 4.2-14.2 KB);
  - the stream's pool grows by 4-18 KB, the smallest one 235 -> 249 KB
    (E3M6);
  - the entities single player never spawns stay (59.5).

### 59.1 The format (formats/qworld.h; magics QWVF, QWZF)

The entities' section starts at `ofs_ents` and runs to `ofs_strings`.
The header is unchanged: 156 bytes, the same fields.

- `uint16_t names[QE_COUNT]`: each class's classname, its string offset
  (0 for a class the map has none of). Every entity of a class has the
  same offset: a known class is its name (`ent_class`), and the cook's
  string pool keeps one copy of each string.
- Then the `num_ents` entities, in the table's order (the edicts'
  order). Each one is:
  - a 32-bit head: the class in bits 0-6 (`QE_COUNT` <= 128), a bit per
    field present in bits 7-30 (`QWE_BIT`), bit 31 zero;
  - the fields present, in the order of their bits, little-endian and
    unaligned (`QWE_FIELD_AT` gives each one's offset in the `qw_ent`
    and its bytes in the table);
  - the classname, for `QE_UNKNOWN` only.
- Then zero bytes up to the strings' 4-byte alignment.

The fields' sizes:

- **The origin's three fields:** whole world units in 2 bytes,
  multiplied by `QW_CLIP_SUB` when decoded. With the head's `QWE_FINE`
  bit they are 1/8 units in 4 bytes instead. All 6505 origins of the id1
  maps are whole.
- **One byte each:**
  - the spawnflags' high byte (the skill flags 256 / 512 / 1024) and its
    low byte (the classes' own flags), as two fields;
  - `model`: a BSP29 map has at most 256 models;
  - `sounds`.
- **6 bytes:** `mangle`, one field (it is one key, "pitch yaw roll").
- **2 bytes each:** `angle`, `wait`, `speed`, `style`, `lip`, `delay`,
  `health`, `height`, `dmg`, `count`, and the strings `targetname`,
  `target`, `message`, `map`, `killtarget`.

The 24 fields (`QWE_*`) are ordered by how often the id1 maps give
them: the origin's three, `targetname`, `angle`, `target`, the
spawnflags' high byte, `model`, the low byte, `wait`, `sounds`,
`speed`, `style`, `lip`, `message`, `mangle`, `delay`, `health`,
`height`, `dmg`, `map`, `count`, `killtarget`, then `QWE_FINE`. The
decoder's loop stops after an entity's last field: 7.6 bits on
average, against 14 in the struct's order.

What §58.2 measured, and what each step here adds (the 32 maps'
entities, in bytes):

| layout | bytes | of the 56-byte entries |
|---|---|---|
| `qw_ent[]` (QWVE) | 499184 | 100 % |
| §58.2: a class byte, a 4-byte mask, the fields present at their sizes | 172978 | 34.7 % |
| the class in the head's low bits: one 4-byte head | 164064 | 32.9 % |
| the origin in whole units: 2 bytes a field | 125710 | 25.2 % |
| the model and each spawnflags byte in a byte, the mangle one field | 120368 | 24.1 % |
| the classname by the class (the names table: 164 bytes a map) | 107788 | 21.6 % |
| as cooked (the alignment: 44 bytes in all) | 107832 | 21.6 % |

That is 65 KB less than §58.2's layout over the 32 maps, about 2 KB a
map.

**The decoder** (`qw_ent_decode`, static inline; the engine, the cook's
`bmf.c` and `world_check` share it):

1. it clears the `qw_ent` and takes the class from the head;
2. it loops over the head's field bits, copying each field's bytes to
   its offset (the origin's scaled, or copied whole with `QWE_FINE`);
3. it sets the classname: `names[class]`, or the inline one for an
   unknown class;
4. it returns where the next entity starts.

The cook's `ents_pack` (tools/qcook/ents.c) writes each entity, then
decodes it back and checks it against its `qw_ent`, byte for byte. A
`qw_ent` has no padding, so the whole struct compares. A map with a
model past 255, or a class under two names, would stop the cook.

### 59.2 The engine and the tools

- **engine/world.c:** `world.ent_names` is the names table,
  `world.ents` the entities after it.
- **engine/ents.c `ents_spawn`:** it decodes each entity in turn into a
  `qw_ent` on its stack (56 bytes) and goes on as before: the skill
  test, `ent_new` (which copies the fields into the edict), then the
  class's spawn function.
  - No spawn function keeps the pointer. `triggers_spawn`,
    `movers_spawn`, `level_spawn`, `items_spawn` and `monsters_spawn`
    read the edict `ent_new` filled, never `q` (each has `(void) q;`).
  - The table is walked once per spawn: `ents_spawn(1)` at a map's
    start, `ents_spawn(0)` at a restart.
- **CPU:** the decode, at a spawn only. Measured on PCSX-Redux:
  369-438 cycles an entity, 42-166 k cycles a map (1.2-4.9 ms). A
  restart's `bmf_load` alone takes 0.7-2.4 M.
  - The first version took about 600 cycles an entity. GCC trimmed the
    clearing `memset` to bytes 1-55 (the class's store follows it), and
    an unaligned 55-byte clear is a library call. The loop also ran 14
    bits an entity in the struct's order.
  - A compound literal's 14 word stores and the frequency order took it
    to about 400.
  - No asm reads the entities. The C engine (§55) builds the same
    `ents.c`.
- **tools/qcook:**
  - `world.c` writes the table after the brush models, the strings
    4-byte aligned after it;
  - `world_check` decodes it from a copy padded past the section, so a
    wrong head cannot read past the file. It checks the head's spare bit,
    each entity's class, model and strings, the names table's offsets
    and the padding;
  - `bmf.c` decodes the table into an array for its train paths
    (`find_target` walks it in the table's order: the edicts');
  - `zones.c` copies the section whole into the skeleton.
- **The magics:** QWVE -> QWVF, and QWVZ -> QWZF (Z for the zones, F for
  the world format it carries). The engine refuses an old file at the
  load ("bad world file"), and so does every tool.

### 59.3 Checked

- **The cook:** every entity's round trip (8914 entities on the 32
  maps); `qcook check` on the 32 worlds.
- **A structural comparison**, old cook (fea0ee3) against new (scratch,
  with an independent Python decoder), on the 32 worlds and the 20
  skeletons:
  - every section is byte for byte the old one but the entities';
  - the header's fields are the same but the magic and `ofs_strings`;
  - every decoded entity equals the old `qw_ent`, byte for byte;
  - the zone tables (chunk directory, leaf sets, sets) are the same;
  - the 40 chunk files are byte for byte the old ones but their stamps
    (the skeleton's stamp hashes the world's length).
- **`scripts/dlcheck.sh`** against the tip's references, saved on fea0ee3
  before the change:
  - start's 12 views: IDENTICAL (6209151 -> 6209152 cycles);
  - the 128 spawn views (`BENCH_MAPS=1 HASHONLY=1`): IDENTICAL
    (45343575 -> 45342963);
  - the game bench's 23 hashes (`BENCH_GAME=1 HASHONLY=1`): IDENTICAL;
  - the C engine (`ENGINE_C=1`): start's 12 views IDENTICAL (+87.9 %,
    §55's ratio), the 128 views IDENTICAL (+94.7 %).
- **The release build** links (hazcheck clean): `ramcheck` 43400 bytes
  for the stack, 43704 before. The decoder takes 304 bytes of code and
  table.

### 59.4 The RAM, measured

From the bench's `bench zone <MAP>` and `world: ... arena` lines (KB),
and the cooked worlds (the entities' section, bytes):

| map | entities | their bytes | skeleton (KB) | the world's load (KB) | the stream's pool (KB) |
|---|---|---|---|---|---|
| START | 144 | 8064 -> 1704 | 259 -> 252 | 557 -> 551 | 517 -> 523 (+6) |
| E1M1 | 174 | 9744 -> 2156 | 259 -> 252 | 587 -> 581 | 458 -> 464 (+6) |
| E1M2 | 262 | 14672 -> 3368 | 268 -> 257 | 566 -> 554 | 443 -> 455 (+12) |
| E1M3 | 364 | 20384 -> 4520 | 287 -> 272 | 577 -> 563 | 364 -> 378 (+14) |
| E1M4 | 318 | 17808 -> 3844 | 310 -> 297 | 597 -> 583 | 384 -> 398 (+14) |
| E1M5 | 323 | 18088 -> 4060 | whole | 551 -> 537 | 432 -> 446 (+14) |
| E1M6 | 271 | 15176 -> 3312 | whole | 449 -> 437 | 502 -> 514 (+12) |
| E1M7 | 113 | 6328 -> 1368 | whole | 180 -> 176 | 912 -> 916 (+4) |
| E1M8 | 177 | 9912 -> 2240 | whole | 355 -> 347 | 704 -> 712 (+8) |
| E2M1 | 256 | 14336 -> 3036 | 279 -> 268 | 620 -> 610 | 383 -> 393 (+10) |
| E2M2 | 205 | 11480 -> 2532 | 304 -> 295 | 634 -> 624 | 390 -> 400 (+10) |
| E2M3 | 261 | 14616 -> 3088 | 297 -> 285 | 601 -> 589 | 394 -> 406 (+12) |
| E2M4 | 308 | 17248 -> 3752 | 314 -> 300 | 620 -> 606 | 372 -> 386 (+14) |
| E2M5 | 270 | 15120 -> 3340 | 261 -> 249 | 559 -> 547 | 433 -> 445 (+12) |
| E2M6 | 397 | 22232 -> 4748 | whole | 596 -> 580 | 319 -> 335 (+16) |
| E2M7 | 388 | 21728 -> 4712 | 307 -> 290 | 641 -> 625 | 320 -> 336 (+16) |
| E3M1 | 247 | 13832 -> 2928 | 255 -> 244 | 583 -> 573 | 423 -> 433 (+10) |
| E3M2 | 220 | 12320 -> 2612 | whole | 386 -> 378 | 601 -> 609 (+8) |
| E3M3 | 227 | 12712 -> 2868 | whole | 479 -> 469 | 532 -> 542 (+10) |
| E3M4 | 280 | 15680 -> 3176 | 332 -> 320 | 692 -> 680 | 256 -> 268 (+12) |
| E3M5 | 259 | 14504 -> 3092 | 331 -> 320 | 684 -> 674 | 336 -> 346 (+10) |
| E3M6 | 326 | 18256 -> 3976 | whole | 743 -> 729 | 235 -> 249 (+14) |
| E3M7 | 219 | 12264 -> 2524 | whole | 558 -> 548 | 477 -> 487 (+10) |
| E4M1 | 286 | 16016 -> 3624 | 282 -> 269 | 581 -> 567 | 421 -> 435 (+14) |
| E4M2 | 254 | 14224 -> 2992 | 262 -> 251 | 565 -> 553 | 429 -> 441 (+12) |
| E4M3 | 366 | 20496 -> 4408 | whole | 543 -> 527 | 408 -> 424 (+16) |
| E4M4 | 393 | 22008 -> 4776 | 285 -> 268 | 583 -> 567 | 393 -> 409 (+16) |
| E4M5 | 398 | 22288 -> 4724 | 284 -> 267 | 594 -> 576 | 338 -> 356 (+18) |
| E4M6 | 357 | 19992 -> 4184 | whole | 482 -> 466 | 474 -> 490 (+16) |
| E4M7 | 356 | 19936 -> 4220 | 350 -> 334 | 651 -> 635 | 295 -> 311 (+16) |
| E4M8 | 322 | 18032 -> 3816 | 263 -> 249 | 566 -> 552 | 393 -> 407 (+14) |
| END | 173 | 9688 -> 2132 | whole | 238 -> 230 | 837 -> 845 (+8) |

- 5.0-17.6 KB of file less a map; the entities take 21.6 % of their old
  bytes.
- The stream's pool grows by 4-18 KB, 12 KB on average (in 2 KB steps:
  `cd_load` gives the world file whole sectors of the arena). The smallest pool is 235 -> 249 KB
  (E3M6), the worst world load 743 -> 729 KB (E3M6).
- A load reads that much less from the disc.

### 59.5 The entities single player never spawns: measured, left out of the change

These are Quake's deathmatch-only entities: all three skill flags are
set. There are 2-36 a map (36 on START and E1M7, 2 on E3M2).

- **What leaving them out would free:** their table bytes, 17-376 B a
  map now, and the edict slots the pool keeps for them (the pool is
  `num_ents + 2 + ENT_SPARE` edicts of 388 bytes): 0.8-13.6 KB.
- **What it would not change:** `ents_spawn` skips them before taking
  an edict, so no edict number changes, no train path (bmf.c's
  `find_target` never finds them, at any skill) and no frame.
- **But their slots are room for the runtime spawns:** an inhibited
  entity's slot is free for `ent_spawn`.

Measured with a bench build that counted the edicts in use (scratch,
not committed), at hard, the skill with the least room, through
`BENCH_MAPS=1 PLAY=1` (the spawn, then 30 frames of play):

| map | pool | in use after the spawn | after 30 frames | free | the SP-never entities | free without them |
|---|---|---|---|---|---|---|
| START | 242 | 110 | 110 | 132 | 36 | 96 |
| E1M1 | 272 | 160 | 162 | 110 | 14 | 96 |
| E1M2 | 360 | 240 | 241 | 119 | 13 | 106 |
| E1M3 | 462 | 327 | 330 | 132 | 17 | 115 |
| E1M4 | 416 | 292 | 294 | 122 | 11 | 111 |
| E1M5 | 421 | 311 | 312 | 109 | 5 | 104 |
| E1M6 | 369 | 250 | 255 | 114 | 12 | 102 |
| E1M7 | 211 | 75 | 75 | 136 | 36 | 100 |
| E1M8 | 275 | 166 | 170 | 105 | 8 | 97 |
| E2M1 | 354 | 214 | 216 | 138 | 14 | 124 |
| E2M2 | 303 | 185 | 185 | 118 | 16 | 102 |
| E2M3 | 359 | 217 | 218 | 141 | 16 | 125 |
| E2M4 | 406 | 294 | 294 | 112 | 8 | 104 |
| E2M5 | 368 | 229 | 235 | 133 | 16 | 117 |
| E2M6 | 495 | 376 | 376 | 119 | 11 | 108 |
| E2M7 | 486 | 353 | 353 | 133 | 15 | 118 |
| E3M1 | 345 | 206 | 206 | 139 | 16 | 123 |
| E3M2 | 318 | 218 | 218 | 100 | 2 | 98 |
| E3M3 | 325 | 202 | 203 | 122 | 9 | 113 |
| E3M4 | 378 | 267 | 269 | 109 | 9 | 100 |
| E3M5 | 357 | 239 | 239 | 118 | 12 | 106 |
| E3M6 | 424 | 290 | 290 | 134 | 8 | 126 |
| E3M7 | 317 | 209 | 212 | 105 | 10 | 95 |
| E4M1 | 384 | 235 | 235 | 149 | 17 | 132 |
| E4M2 | 352 | 231 | 232 | 120 | 16 | 104 |
| E4M3 | 464 | 347 | 352 | 112 | 18 | 94 |
| E4M4 | 491 | 351 | 353 | 138 | 25 | 113 |
| E4M5 | 496 | 339 | 340 | 156 | 25 | 131 |
| E4M6 | 455 | 278 | 279 | 176 | 14 | 162 |
| E4M7 | 454 | 287 | 291 | 163 | 15 | 148 |
| E4M8 | 420 | 299 | 300 | 120 | 21 | 99 |
| END | 271 | 157 | 157 | 114 | 6 | 108 |

- **In use after the spawn:** the world, the player, the map's spawned
  entities and what their spawn made (the plats' triggers), less those
  the spawn removed. Those are the monster kinds the port does not have
  yet (the enforcers, hell knights, vores, spawns, rotfish and
  Shub-Niggurath: `monsters_spawn` removes them), up to 73 at hard on
  E4M6.
- **The runtime spawns the benches saw:** in the first 30 frames, the
  door chains' trigger fields (0-5); in the game bench, 2 more slots;
  in the monsters' scenarios (`MONSTERS=1`, E1M1 at hard: a monster of
  each kind fighting and killed, then the crowd), 3. Without the SP-never
  entities, 94-162 slots would stay free at hard: E4M3 94, E3M7 95,
  START and E1M1 96.
- **Once the missing monster kinds come,** they take their slots. The
  free slots without the SP-never entities then become 87-115 (E4M8 87,
  E4M3 and E4M6 89). That is computed: the pool, less the world, the
  player, the map's spawned entities, the plats' triggers and the door
  fields measured above.
- **But the transient spawns grow with the play, not with the map:**
  - 3 gibs per gibbed monster, each for 10-20 s (a zombie dies only
    gibbed);
  - a backpack per grunt and ogre (and enforcer, once they come), for 2
    minutes;
  - the scrags' two helpers and the shamblers' glow, each under a
    second;
  - the lava balls, 2 per `misc_fireball` at most;
  - the teleport fogs, the teleport deaths and the delayed uses.

  A fight that gibs 25 monsters in 20 s, with the packs left on the
  floor, would pass 94 slots. Nothing measured shows the runtime spawns
  never need those slots, so the cook keeps the entities (the ask's
  condition: listed, not done). When the pool is full, `ent_spawn`
  prints `ents: no free edict` and the spawn is skipped: a gib or a
  pack that never appears, not a crash. No bench printed it.
- **To take them out:**
  - the cook's `ent_kept` leaves them out: 0.8-13.6 KB a map, at the
    cost of 2-36 of the 100-176 free slots (87-115 left once every
    monster kind is in). The tight maps gain the least (E3M6 3.0 KB,
    E3M4 3.4, E4M7 5.7), START and E1M7 the most (13.6 each);
  - or the names table's unused entry 0 (`QE_UNKNOWN`'s) could carry
    their count, and `ents_load` keep their slots in the pool: 17-376 B
    a map, no headroom change. Not done: a format oddity for a few
    hundred bytes.
- The same measure is what the "edicts by the skill" option (§58.2)
  needs. A pool sized by the skill's spawns would free the inhibited
  entities' slots: 10-99 at easy, 6-75 at normal, 6-41 at hard. The same
  caveat holds: 96 spare slots, and play decides the peak.

### 59.6 The analysis tool, outside the repo

`quakespasm-q1psx/analysis/wld.py` asserts
`h['magic'] == b'QWVE'`. It reads the header, the nodes, the leaves,
the marks, the vertices and the brush models, never the entities, and
their layout is unchanged. Accepting `QWVF` is all it needs. It is not
changed here: the tool lives outside the repo, and nothing derived from
Quake's data goes in the repo.

### 59.7 Left

- **Not checked:** DuckStation and the real machine. The change is the
  spawn's decode and smaller files: a load reads less.
- **The SP-never entities** (59.5): a call left open.
- **The other options of §58.5**, as they were:
  - D, the PVS rows as deltas;
  - E, the PVS rows per zone;
  - the edicts by the skill (the measure above is its input);
  - F, 16-bit node children.

## §60 — The executable trimmed: no printf, no 64-bit division, no debug font, the cold code for size (2026-10-22)

- **The question**: whether the 1.2 MB ELF is wholly in RAM at boot. No: the ELF carries ~0.9 MB of DWARF (the
  release preset is RelWithDebInfo); the console loads `q1psx.exe`, the
  code and the data, 310 KB at 2cb0492. The RAM's real weight is the BSS
  (1.6 MB: the level arena, the display buffers, the game arena) that
  follows them; every KB of code is a KB the stack or an arena does not
  get. Six ways were listed; the first four were taken.
- **1. No printf in the release.** A console has no TTY to read. CMake
  forces `engine/noprint.h` into every C file of a release (not a bench)
  build: `printf(...)` is `((void) 0)`, so the calls, their arguments and
  their format strings leave the code; `snprintf` (the screen's text)
  stays. A call's arguments are no longer evaluated: the one with a side
  effect, `pad_init`'s `pad_force_analog()`, is made before its printf
  now. The SDK's own 38 printf calls (precompiled) stay, with printf.
- **2. No 64-bit division.** libgcc's `__divdi3` (1.5 KB, its 256-byte
  `__clz_tab`) served 22 calls: the monsters' aims and missiles, a mover's
  time and place on its way, `vnormalize`, the world cube's clip of a
  trace, and `atan2_d256`'s table, built at its first call by a series in
  2.30 fixed point (about 5000 divisions: a 1.8 M-cycle tick, measured in
  the monsters' bench, `mon cost`). Now:
  - `q_div64_32` (engine/q_fixed.h, .c) divides an int64 by an int32 as
    C does (truncated toward zero): inline, one `div` when the dividend
    fits 32 bits (every division the game makes in practice), else a
    restoring division on the 64 bits (`q_div64_32_long`). Checked on the
    host against C's division: 19.4 M cases (edges and random), 0
    mismatches;
  - the atan table is a constant: the same integer code run once on the
    host printed its 257 values;
  - `ents_tick`'s `__builtin_ctz` (libgcc's `__ctzsi2`, which also needs
    `__clz_tab`) is a de Bruijn table of 32 bytes (checked against the
    builtin on 2^32 / 9973 values and every power of two).
- **3. No debug font in the release.** `vid_init` loaded the SDK's font
  (`FntLoad`, 2.1 KB of image and CLUT data) for `vid_text`'s fallback
  before the game data. The release loads `GAME.DAT` first (its loading
  screen draws with Quake's console font): `FntLoad` and the `FntSort`
  fallback are out of the release, the VRAM at (960, 0) free.
- **4. The cold code for size.** `Q_COLD` (engine/mem.h, GCC's `cold`:
  the function optimized for size, in its own text section) on what runs
  once a level: the world's, the entities', the stream's and the zones'
  loads and spawns, the level's start and end, the boot's inits; the
  menus' files (`menu.c`, `settings.c`) at `-Os`. The hot C keeps `-O2`:
  `-Os` everywhere would take 35.5 KB more (226 -> 190 KB of C, measured
  file by file) but slow the per-tick and per-frame C, unmeasured.
- **Measured** (the release, `q1psx.exe` 317440 -> 301056 B):

  | | code + data | |
  |---|---|---|
  | 1. printf | -9.0 KB | the strings 3.7 KB of it |
  | 2. 64-bit division, ctz | -1.4 KB | with the inline fast path (0.6 KB) |
  | 3. debug font | -3.4 KB | and 224 B of BSS |
  | 4. cold code for size | -2.9 KB | |
  | total | -16.6 KB | the BSS -0.7 KB |

  The stack's room (scripts/ramcheck.sh): 43400 -> 60776 B. The level
  arena keeps its size: what is freed goes to the stack's margin, as with
  §54's 25 KB (the choice then); `MEM_LEVEL_BYTES` can take it.
- **The same frames** (PCSX-Redux at 10x, `HASHONLY=1 PROFILE=0`,
  references saved on 2cb0492): start's 12 views' display lists
  identical (cycles +2 in 5.55 M), the game bench's hashes identical
  (frame -0.007 %, ents -0.27 %, snd -0.6 %), the monsters' bench (its
  dumps and every `mon` state line) and the movers' bench (`bmfhash`)
  identical.
- **Left:**
  - 5, the transient static buffers shared (the cut's 13 KB and the slow
    path's work area are never in use together) and 6, the monsters' code
    as overlays a map loads (26 KB of code and 8 KB of tables, only the
    map's kinds): a call left open;
  - not checked: the release run in an emulator (it prints nothing now:
    a look is the check), DuckStation, the real machine.

## §61 — The stack's need, measured under load (2026-10-22)

- **The question**: how big the stack must be, with crashes on
  the busy levels (E1M3 named) thought to be a leak or an overflow.
- **The measure**: `MONSTERS=3 MAP=E1M3` (scripts/bench.sh): the crowd
  (the 47 monsters awake and after the player, 10 s), then a minute of
  fight drawn: the player, who cannot die, fires rockets at the nearest
  monster it sees, a frame rendered every tick; "mon fight" lines every
  5 s with the stack meter (`stack_untouched`: the painted room never
  reached), the edicts in use and the pool's size (`ents_in_use`), the
  level arena, the render list's items, the frame's vblanks.
- **The stack**: 2.6 KB at most (PCSX-Redux and DuckStation alike), of
  the release's 60.8 KB (43.4 KB before §60; the link refuses under 16
  KB). The stack is not what overflows.
- **No leak to find there:** no malloc in the game (the SDK's
  `InitHeap` at boot, nothing allocating); the arenas are bump
  allocators reset at each level and nothing takes from them in play;
  the fixed pools drop when full (the edicts: 296-303 of 462 on E1M3,
  the 11 allocation sites checking a full pool).
- **DuckStation** (closer to the machine) ran the same: no invalid
  access, no GPU command error.
- **What it shows instead**: E1M3 all awake is slow: frames of 5-7
  vblanks (8-12 fps), the entities' ticks up to 1.9 M cycles (the
  crowd's thinks); 4 ticks a frame at most (pad.h `PAD_MAX_TICKS`), so
  the game slows down rather than freezing.
- **Then, a playthrough** (DuckStation, from START to E1M3, the crash usually on the way back from the key): three more
  scenarios, run on PCSX-Redux at 10x and on DuckStation's interpreter
  (the mode in use) with the music on the disc:
  - `MONSTERS=4` (MAP, E1M3 by default): the map cleared, each monster in
    turn, the player on a spot in its sight, rockets, a frame each tick;
  - `MONSTERS=5`: from MAP (E1M1) each map cleared, then its exit taken
    as a player's (the intermission, the parms, the next map's spawn), to
    E1M7: 129 monsters killed, no crash;
  - `MONSTERS=6`: E1M3's gold key among its 17 monsters (zombies,
    scrags, ogres, shamblers) awake, picked up, then the gold doors (*21,
    *22) opened and crossed both ways four times, back at the key's place
    between: no crash, the stack 4.3 KB at most, the edicts 320 of 462 at
    most, DuckStation clean.
- **Left:** the crash itself, not reproduced: what is seen (a
  freeze, a black screen, DuckStation's own message or its exit), the
  release's Graphics settings in that game, DuckStation's log (its
  logging is off in the settings in use, and the release prints nothing
  since §60).

## §62 — The monsters by hand: the Ogre first (2026-10-22)

- **The ask:** the cooked monsters not good enough: every monster rebuilt by hand from Quake's
  own model, as the guns were, the grenade thrower first, each shown on Reference (Quake at full
  resolution), Before (the cooked model) and After (the rebuilt one) sheets. The grenade thrower
  is taken to be the Ogre (`progs/ogre.mdl`: its grenade launcher, its
  chainsaw).
- **What the cook did:** a streamed monster's body was decimated
  automatically (`stream_reduce`: keep a quarter, error bound 128). The
  Ogre came out at 194 of its 326 triangles and 103 of its 169 vertices.
  The collapses fell on what carries the look: the back of the skull went
  to a spike in profile, the grenade launcher in the left hand to a flat
  shard (half its vertices gone), the hands and the feet thinner.
- **What the original holds** (every frame counted, 147 of them):
  - only 2 of its triangles are never seen (z-buffered renders from 26
    directions at two distances, every 4th frame);
  - no vertex can go without moving the surface by at least 0.8 unit in
    some frame. 22 can go within 2 units, 72 within 3. Quake's model is
    lean already: unlike the guns (§54) there are no redundant sections to
    drop for free.
- **The hand cut** (`tools/qcook/game.c` `MOBS`, `OGRE_CUTS`, `OGRE_DROP`):
  - half-edge collapses u -> v, chosen one at a time. A collapse's cost is
    its worst frame: the moved corner's distance to the new surface, and
    the skin's shift where the corner was (frame 0's texel against what the
    new triangles give there);
  - the cheapest went first, each within 3 units and 4 texels, none
    flipping a triangle in any frame;
  - the parts that carry the look are kept whole: the grenade launcher
    (vertices 0-19), the head, both hands, the feet's soles;
  - the cuts stop when the next would pass the bounds: 29 collapses, plus
    the 2 unseen triangles;
  - the result: **266 triangles, 140 vertices**, the same at every model
    level (no decimation). `QCOOK_NOHAND=1` cooks the decimated model again
    (an A / B).
- **The comparison** (the scratchpad's renderer, not in the repo):
  - Reference: Quake's model with its full skin (ultra), perspective-
    correct, z-buffered, at 640x480;
  - Before and After: the cooked streamed assets drawn the PS1's way at
    320x240: affine texels, the records' order (each primitive prepended,
    the last emitted drawn first), NCLIP's back faces left out, no depth
    buffer, the skin halves at the build's resolution (high);
  - eight sides of eight frames (stand, walk, run, the chainsaw's swing and
    smash, the grenade, a pain, the death). The After matches the Reference
    where the Before broke: the skull, the launcher, the hands, the feet,
    the dead body.
- **In the engine:** `MONSTERS=1 MAP=E1M8` with both cooks. Same monster
  states, the Ogre's dumps differ (its model), no stream error.
- **What it costs** (the stream's RAM; the triangles drawn):
  - the Ogre's asset goes 82.7 -> 105.5 KB, its frames 60.6 -> 82.3 KB
    (140 vertices x 147 frames x 4 B); its group (the model, its head, its
    sounds) 136 -> 158 KB; an Ogre draws 266 triangles instead of 194;
  - the residency report (`BENCH_MAPS=1 VIEWS=0 RESIDENCY=1`) was run on
    the 32 maps with both cooks. It counts the spots where a monster in the
    player's PVS lacks its model:
    - on master as it is (§57-§61 in, the maps whole: the tightest pools
      E4M7 209 KB, E3M6 249, E3M4 262, E3M5 268): 3 spots against 2, the
      one more on E2M7 (pool 294 KB);
    - the same with the zones (the analysis' pvs_1536_96 tables, 20 maps
      zoned: the tightest pools E3M6 249 KB, E3M4 268, E4M7 311): 2
      against 2, none more;
    - on the master of before §57-§61 (pools of 171-242 KB on those maps):
      11 against 3, E3M4 alone 5 of 29 (pool 218 KB).

    The room §58-§60 gave the pools absorbs the faithful Ogre.
- **Left as it was, seen on both:** the PS1 draws a model's triangles
  without a depth test, in the cook's order. Where a part passes behind
  another (the chainsaw behind the legs, seen from the left), it can show
  over it. A depth sort of a model's triangles (an ordering table per
  model, the GTE's AVSZ3) would fix it for every monster: proposed apart
  (done: §63).

## §63 — The monsters' triangles sorted by depth (2026-10-22)

- **The ask** (§62's proposal, taken up the same day): §62's comparisons
  left one fault the hand model cannot fix. The PS1 has no depth buffer,
  and a model's triangles went out in the cook's record order (each
  prepended to the world's bucket: the last emitted drawn first). Where a
  part passes behind another, it shows over it: the Ogre's vest over its
  near arm, the chainsaw's motor over its body. Quake draws its models
  z-buffered. The fix asked: sort each model's triangles by depth the
  PS1's way, in hand-written asm, with a small ordering table per model;
  the painter's order between the model and the world around it as it
  was; the view model (ordered by the cook, §53) and the brush models
  (their BSP walk) as they were.

### 63.1 The ordering table

- **One per monster drawn:** R_SORT_N = 16 nodes, 0-word tags, taken
  at the packet pointer (64 bytes), linked as a ring (node j -> node
  j - 1). 32 at first; 16 chosen (63.7).
- **A triangle's bucket:** AVSZ3 of its corners' SZ, the vertex cache's
  (`OTZ = ZSF3 x (SZ1 + SZ2 + SZ3) >> 12`); OTZ / 4 mod 16 picks the
  node (`andi` with `15 << 2`: the node's byte offset). The triangle is
  prepended to its node's chain, so within a bucket the last emitted is
  drawn first.
- **The scale** (rlist.c `ent_sort`, per entity):
  - the depths a monster's vertices can take: its origin's depth +-
    rho, with rho = 1.5 x its cooked radius (about the origin, over every
    frame, vertex units; 1.5 is the view matrix's scale) + 8 for the
    roundings;
  - ZSF3 = 16384 x (R_SORT_N - 2) / (6 rho) puts that range, summed over
    three corners, on 14 buckets; at most 1365, so that OTZ stays within
    16 bits with all three corners at SZ 65535;
  - the range starts anywhere on the ring: RE_ZNEAR is the bucket of its
    near end (3 x max(0, depth - rho)). 14 buckets plus the roundings' 2
    keep the range from meeting itself;
  - the origin's depth by the GTE (one MVMVA): rl_ent_matrix leaves the
    view's rotation and translation in it. One division a monster a frame
    (ZSF3).
- **The splice:** the nearest bucket's node takes the world's chain as
  it stood when the model began (fp); the model's chain starts at the
  farthest bucket's node (the nearest's predecessor on the ring). The
  table is drawn whole there, among the world's faces and the leaf's
  items where the model always was.
- **The slow path** (near or big triangles: the clip, the subdivision):
  its pieces go into the bucket of the triangle they cut. fp, the link it
  prepends to, is that bucket's head before the call and is taken back
  after it, in the asm and in the C engine's `r_slow` (RS_OT_HEAD).
- **The texture window:** a streamed model's two images (the skin's
  halves, §40) both use "no window" (stream.c). The sorted loop's texture
  changes close no run (de's did, a window command each). dm_exit_1 gives
  the table its one window command, drawn first. That is one window
  command fewer per monster (two runs: two commands before, one now).
- **Every vertex's SZ** goes to the cache. de stored it only for the
  careful test.

### 63.2 Which models

- The cook flags each monster's body: QSTM_SORT in `qst_model.flags`
  (formats/qstream.h), set for the `body` the cook already passes to the
  hand cut (§62). These are the nine kinds' bodies, dead ones too (the
  same model). They are triangles only, which the sorted loop relies on.
- Heads, gibs, items, flames, projectiles, the view model and the brush
  models are drawn as before. The flag can go on any streamed model.

### 63.3 The asm (engine/r_world.s)

- **draw_model:** RE_SORT (rlist.h; set by `ent_sort`) branches to
  dm_sort, tested together with RE_VIEW. An unsorted alias model's path
  has the same instruction count as before.
- **dm_sort:** ZSF3 (ctc2; nothing else uses AVSZ), the ring's 16 words
  in 4 rounds of 4 stores (R_SORT_N a power of two, 4 or more), node 0
  closed onto node 15, the nearest node onto fp, the chain's start kept
  in the scratchpad (SP_SORTHEAD, 0x3bc). Then ds_records, and dm_exit_1
  as for any model.
- **ds_records:** RECLOOP de's loop with deo1's draw distance (its cull
  and fog; their thresholds `ds_o_dcull` / `ds_o_fogz`, patched by
  render.c with deo1's). It differs from de in these ways:
  - no face-stamp test (a model's records are always stamped);
  - no run closed at a texture change;
  - SZ always stored;
  - triangles only.
- **The emit:** the corners' xy go into the GTE by lwc2 for the NCLIP,
  then from there into the packet by swc2. de takes them through the
  CPU's registers (lw, mtc2, sw): this costs 4.8 cycles less a triangle,
  culled or not. For the triangles turned toward the eye: lwc2 of the
  three SZ, AVSZ3 (after the xy's swc2, so that no GTE register is read
  while it runs), andi, addu, the node's load and store.
- **Registers:** gp holds the table; s1, s2 and ra are temporaries. These
  are the walk's dead ones at a leaf (pop_1 reloads s1 and s2, every
  block sets gp, SP_DMRA keeps ra). The slow path keeps s1 (the node
  across the call) and gp; the C engine's call saves gp as before.
- **The distance off** (DRAWDIST=0, a bench A/B): render.c sets the
  sorted loop's tests at their most (0x7fff). A record nearer than 32767
  view units (5461 world units) is drawn; past that it gets the fogged
  transform with every factor at 128, its colours unchanged.
- **The first version** took the xy through the CPU, as de does. It gave
  the same frames (the 128 views' and the game bench's hashes the same)
  and cost about three times as much: the 28 views +73 k cycles in all
  against +21 k (32 buckets, before §64), the game bench +4.0..4.7 k a
  frame against +0.8..1.3 k.

### 63.4 Checked

- tools/hazcheck at every link (clean).
- START's 12 views against the base (b14c05a built beside, before §64
  was merged; the engine is the same since):
  - 8 are identical;
  - 4 changed (the crucified zombies): the same packets in another order,
    and one window command fewer a zombie;
  - pktview's pictures: 0, 0, 1 and 1 pixels differ.
- **The 128 spawn views** (`BENCH_MAPS=1 HASHONLY=1`, against master
  c4063b4 built beside, §64's nine hand-made monsters in both; with 32
  buckets, then 16):
  - 100 are identical, wherever no monster is in view;
  - 28 changed. A new "Q" line per dump (vid.c: the packets' hashes
    summed, the window commands left out and counted) shows every one of
    the 28 holds the same packets as a set, with 1-4 window commands
    fewer;
  - the views that changed: START.v0, E1M2.v0, E1M4.v0, E1M5.v0 v3,
    E1M6.v0, E1M8.v0 v1 v3, E2M2.v0, E2M6.v0, E2M7.v0, E3M2.v0, E3M3.v0
    v1, E3M4.v0 v2, E3M6.v0 v1, E3M7.v2 v3, E4M3.v0 v1 v3, E4M4.v0,
    E4M5.v0, E4M6.v3, E4M8.v0.
- **The game bench** (32, then 16): 15 of its 23 dumps are reordered
  (START's zombies), the same packet sets; the 8 others are identical.
- **`MONSTERS=1 MAP=E1M8`** (before §64): its 12 dumps are reordered, the
  same packet sets. Its Ogre is far there: 0-16 pixels change.
- **Eight views around E1M8's Ogre** at 90 units (`MAP=E1M8 BENCH_MAPS=1
  VIEW=1306,488,46,180,0,...`, the Ogre at 1216 488 24, facing 270; the
  same results before and after §64's merge, with 32 and with 16):
  - pktview's pictures: from both sides, the near arm is now over the vest
    and the far arm where they had shown over it;
  - 15 of the 20 views reordered with the same packet sets, 4 identical
    (no monster in view);
  - one (v13, its back left: 10 monsters in view, the arena near its
    limit) lost a far small model of 3 records, 4 triangles shown, the
    pixels unchanged. R_MODEL_ROOM (§43) left it out sooner because the
    rings (120 bytes a monster with 32 buckets, 56 with 16) had taken the
    room.
- **The sheets** (quakespasm-q1psx/mobs, outside the repo:
  `mview.draw_psx(..., sort=N)` reproduces the engine's table, ZSF3,
  ring and order; `sheets.py --sorted=16,32,64`):
  - Reference / the records' order / sorted, eight sides of eight frames
    (stand1, walk5, run3, swing6, smash9, shoot3, pain3, death14);
  - the sorted columns match the Reference where the records' order
    showed the far parts;
  - against 64 buckets, summed over the 64 views: the records' order
    differs by 150862 pixels (330 x 330 cells), 16 buckets by 25669, 32 by
    5905. 32 was kept first; no difference was seen between the three
    and 16 was chosen (63.7).
- **The C engine** (`ENGINE_C=1`, r_world.s asm in both; before §64, 32
  buckets): the 128 views' hashes are the asm's.
- **The interpreter** (`EMU_ARGS=-interpreter`, before §64, 32 buckets):
  the twenty E1M8 views (with the Ogre's eight) give the dynarec's
  hashes.

### 63.5 What it costs

- **Cycles** (PCSX-Redux, the views' 16-frame averages, against master
  c4063b4; 16 buckets, 32's in brackets):
  - the 28 spawn views with monsters: +12.9 k cycles in all [+16.8 k],
    -0.10..+0.21 % a view; all 128 views +0.03 % [+0.04 %]. A monster
    drawn costs -367..+793 cycles [-279..+880], most 100-600 at the spawn
    views' distances. Where it is less than before, the xy through the
    GTE saved more on the triangles turned away than the sort cost on the
    others;
  - the Ogre alone at 90 units (E1M8.v16, its right side): +418 (+0.12 %)
    [+594]; eight monsters close (v12): +3557 (+0.25 %) [+4436];
  - the 100 views with none: within the noise (-465..+465);
  - the game bench (START's zombies in view): +0.30..+0.63 k a frame
    (+0.03..+0.06 %) [+0.65..+0.98 k], its walks +0.01..0.02 %;
  - `MONSTERS=3`'s minute of fight on E1M1 (before §64, 0.9 monsters drawn
    a frame on average in its first 300 ticks, then none): +482 a frame
    there, the rest within the noise;
  - `MONSTERS=2`'s crowd draws no frame (the entities' tick alone): not
    concerned.
- **RAM:**
  - code and data: the release's stack room 60776 -> 58504 B (-2272),
    the default build's 48424 -> 46024;
  - the packet arena: 64 bytes a monster drawn, less the window command
    saved (8): 56;
  - the scratchpad's word 0x3bc;
  - r_ent_t's two pad words (zsf, znear).

### 63.6 Left

- **The ring's room in the arena:** 56 bytes a monster. In a view near the
  arena's end, R_MODEL_ROOM leaves a far model out sooner (one case in the
  148 views measured, with 16 buckets as with 32). A ring sized by the
  model's distance, with fewer buckets far away, would also cost fewer
  cycles; it needs the mask in a register.
- **The same xy trick for de's triangles** (every other alias model, the
  gun, the brush models): identical frames, about 4.8 cycles a triangle.
  Proposed apart (done: §66).
- **A finer range:** the radius is over every frame (the death poses
  too). A radius per frame from the cook would give finer buckets for the
  same 16.
- **Parts that cut through each other:** a triangle sort cannot split
  them, where Quake's z-buffer decides per pixel. The sheets show few
  such places on the Ogre.

### 63.7 16 buckets

- **The question** (after the sheets): whether 16, 32 and 64 buckets cost differently,
  16 being enough if so (the three looked the same).
- **Where the count costs:** only per monster drawn. The ring's init (4
  stores and 4 instructions a round of 4 nodes), 4 bytes of the arena a
  node, an empty node for the GPU's DMA to walk. A triangle's bucket is
  the same work at any count (an immediate mask).
- **Measured** (against master c4063b4, the same checks as 32's: 100
  views identical, 28 reordered, the same packet sets; the game bench's
  dumps likewise): 16 buckets cost about 88 cycles less a monster than 32
  (the 28 views -3.9 k in all), the Ogre alone close -176, the game bench
  -0.35 k a frame (its four zombies drawn). 64 would cost about 175 more
  than 32 (four more rounds: not built).
- R_SORT_N = 16 (rlist.h, r_defs.inc). The asm takes any power of two of
  4 or more.

## §64 — The monsters by hand: the other eight (2026-10-22)

- **The ask**, after §62's Ogre: the other monsters by hand the same way, with the Reference /
  Before / After sheets.
- **The same way as §62.** The tools stay outside the repo
  (`quakespasm-q1psx/mobs`): `mobcut.py` drives `handcut.py`,
  `vhid.py` checks the dropped triangles, `sheets.py` draws the sheets.
  - What carries each monster's look is kept whole. It is picked in the
    rest frame and checked on a render (the kept vertices in red, five
    sides):
    - the soldier: the gun and its muzzle flash, the head, the hands on
      the gun, the feet;
    - the dog: its 11 small parts (the teeth, the eyes, the tongue), the
      head, the paws;
    - the knight: the sword (its strip of the skin), the hand on the hilt,
      the free hand, the head, the feet;
    - the zombie: the flesh it throws (a small part folded in its chest,
      in its hand when it throws), the head, the hands, the feet;
    - the scrag: the head, the arms' spikes, the tail's tip;
    - the fiend: the horns, the head, the claws, the feet, the tail's tip;
    - the shambler: the head, the claws, the feet;
    - Chthon: the head, the horns, the 20 spikes, the hands.
  - The collapses go cheapest first, each within 3 units and 4 texels in
    every frame.
  - Chthon's bound is 13 units. His vertices sit on a grid of 2.1-2.6
    units a step (the Ogre's 0.4-0.6), so 3 units allow no collapse at all.
- **Two flaws of §62's method, found on the way:**
  - **The hidden triangles.** §62 looked at every 4th frame (26
    directions, two distances, 320x320). The soldier's muzzle flash, 14
    triangles folded inside its body, shows only in `shoot5`, so the check
    had it as never seen. The new check:
    - every frame, from 64 directions;
    - then each candidate is looked at from outside the model (an eye on a
      sphere 1.1 times the frame's radius), zoomed till it spans 24 pixels,
      from the 96 directions in front of it, in every frame;
    - it goes only if no view shows 3 pixels of it. One or two pixels leak
      where two parts meet.

    The Ogre's 2 (§62) pass: 1 pixel at most, under the launcher in the
    hand. The zoom kept back the dog's triangle 23 (3 pixels, `leap5`) and
    Chthon's 100 and 130 (5 and 3 pixels).
  - **A collapse that leaves nothing to measure.** When u's only
    triangles are the edge's own (after the hidden ones went), the measure
    gave the collapse no cost, whatever it took away. Two tables had one:
    - the soldier's 163 -> 142, 2.5 units off in fact;
    - Chthon's 312 -> 221, a tab of his open bottom edge in the lava, 19.5
      units off.

    Such a collapse is now refused.
  - **The true error**, each cut vertex against what is left in every
    frame: 2.1 to 2.9 units for the seven bounded at 3 (the bound is per
    collapse, and the errors add up little), Chthon 12.5.
- **The result** (`tools/qcook/game.c` `MOBS`; triangles / vertices; the
  asset in the stream file):

  | Monster | Frames | Quake | Old cook | By hand | Cuts | Dropped | Kept whole | Asset |
  |---|---|---|---|---|---|---|---|---|
  | soldier | 114 | 328 / 170 | 156 / 84 | 270 / 141 | 29 | 0 | 82 | 60.5 -> 87.8 KB |
  | dog | 86 | 426 / 236 | 210 / 128 | 377 / 216 | 20 | 9 | 144 | 65.4 -> 97.9 KB |
  | knight | 97 | 213 / 108 | 123 / 63 | 189 / 96 | 12 | 0 | 57 | 44.4 -> 58.0 KB |
  | zombie | 198 | 347 / 177 | 135 / 71 | 240 / 124 | 53 | 1 | 81 | 69.6 -> 112.4 KB |
  | scrag | 54 | 157 / 80 | 75 / 39 | 140 / 72 | 8 | 1 | 42 | 24.5 -> 32.4 KB |
  | fiend | 69 | 275 / 143 | 177 / 94 | 267 / 140 | 3 | 2 | 70 | 47.9 -> 61.8 KB |
  | shambler | 94 | 284 / 144 | 202 / 103 | 278 / 141 | 3 | 0 | 91 | 53.9 -> 69.1 KB |
  | Chthon | 106 | 555 / 325 | 471 / 283 | 439 / 281 | 44 | 28 | 210 | 145.2 -> 143.9 KB |

  The fiend and the shambler change little: Quake's models are lean there,
  and only 3 collapses each fit the bound.
- **The comparison:** 64 sheets, Reference / Before / After, 8 frames of
  each monster seen from 8 sides. The frames: stand, walk, run, the
  attacks, a pain, the deaths, and the zombie's crucified frame. What the
  Before broke and the After keeps:
  - the heads, a spike in profile (the soldier, the knight, the zombie);
  - the soldier's muzzle flash, a needle;
  - the knight's sword, a needle with gaps;
  - the dog's legs and the zombie's feet.
- **`QCOOK_HAND=<files>`** applies only these tables (comma-separated).
  `QCOOK_HAND=progs/ogre.mdl` cooks the stream of before §64 byte for byte.
- **What it costs** (before: `QCOOK_HAND=progs/ogre.mdl`, master as it was;
  after: every table):
  - the eight assets go from 511 to 663 KB in the stream file (+152 KB).
    The frames are most of it: vertices × frames × 4 B, and the zombie
    has 198 frames;
  - the residency report (`BENCH_MAPS=1 VIEWS=0 RESIDENCY=1`, 32 maps)
    counts the spots where a monster in the PVS lacks its model:

    | Maps | Before | After | Where |
    |---|---|---|---|
    | whole | 3 | 20 of 1020 | E3M4 5 (pool 262 KB), E2M7 1 -> 5, E2M2 3, E2M6 3, E2M3 1, E3M5 1 |
    | zones (pvs_1536_96) | 2 | 11 of 1020 | E3M4 5 (pool 268 KB), E2M6 3, E2M7 1 |

    E1M7's 2 spots were there before and are unchanged. The maps that lose
    hold four or five of the streamed kinds. One body of each kind takes
    229-319 KB before and 266-404 KB after, in pools of 262-354 KB (their
    heads and sounds come on top).
  - the 128 spawn views (`BENCH_MAPS=1 PROFILE=0 HASHONLY=1`):
    - all the views' cycles +1.6 %; START.v0 +9.1 % (its nine crucified
      zombies), at most +17.8 % (E4M3.v3, still one vblank);
    - 24 views over one vblank before and after, none crossing it. E1M8.v0
      goes from 2 to 3 vblanks: 1.108 M -> 1.138 M cycles, against 1.13 M
      for two;
    - the packet arena never full: 61 KB of its 94 at most, as before;
  - the crowd (`MONSTERS=3 MAP=E1M3`: 47 monsters awake, then a minute of
    fight drawn): the same game (the same monsters alive at each line), the
    same frames' vblanks at the 12 lines (5 to 7), the entities' tick
    unchanged (253 k cycles on average).
- **A lighter cut, measured, not taken.** The same parts kept whole, with
  wider bounds (vertices):

  | Bounds | soldier | dog | knight | zombie | scrag | fiend | shambler |
  |---|---|---|---|---|---|---|---|
  | 3 units, 4 texels (taken) | 141 | 216 | 96 | 124 | 72 | 140 | 141 |
  | 5 units, 6 texels | 123 | 192 | 87 | 104 | 62 | 120 | 133 |
  | 8 units, 8 texels | 112 | 177 | 80 | 94 | 54 | 107 | 120 |
  | old cook | 84 | 128 | 63 | 71 | 39 | 94 | 103 |

  8 units would get about half the RAM back, but still not the old cook's.
  The parts kept whole are much of each model (the soldier's 82 vertices
  of 170). The faithful cut was kept.
- **Left:**
  - the residency on the tight maps (E3M4, E2M2, E2M6, E2M7). The options
    are a lighter cut, room in the pools, or frames streamed by
    animation: a call left open;
  - (the PS1 drawing a model's triangles in the cook's order: settled by
    §63's depth sort.)

## §65 — The freeze on E1M3: a secret door read as a monster (2026-10-22)

- **The report**: a normal playthrough from START on DuckStation, frozen on E1M3, usually on the way back from the key; the emulator left running for a look. DuckStation's status bar: "Game: 0
  FPS", "Video: 60 FPS (100%)": the game hung, not the emulator. Its save
  state (F2) read offline: DuckStation's file is a header,
  then zstd data; the CPU block after its "CPU" marker (pending ticks,
  downcount, two completion ticks, the 34 registers, pc, npc, the COP0
  registers); the 2 MB of RAM found by the release ELF's `.text` (the
  code at 0x80010000, equal but for the self-modified words).
- **What it showed**: pc in the BIOS (0xbfc0d8e0, its loop for an
  exception it does not handle), EPC 0x80035300, cause 7: a bus error on
  a load. At EPC, `monsters_stream_wants` (monsters.c, `e->mdef->kind`)
  read a byte through `mdef` = 0xffffff40. In RAM, edict 287: class 7,
  `func_door_secret`, in use; its word at `mdef`'s offset -192, its
  first stop's y (`dest1[1]`, -24 units): the door had opened.
- **The cause**: `mdef` lay in the union the monsters share with the
  movers (`pos1` .. `move_delta`, ents.h), where `dest1[1]` is. `mdef` is
  the engine's "a monster or its body" test everywhere (the lists'
  rebuild, the stream's wants, the damage, the touches, the drawing). An
  opened secret door read as a monster at the lists' next rebuild (a
  pickup: the super shotgun's), and its stop as a pointer to its kind: a
  bus error on the console and DuckStation, nothing on PCSX-Redux (it
  reads the unmapped address quietly), so no bench saw it. A door that
  only slid (pos1, pos2) left `dest1` at 0; the secret doors are the ones
  that write it.
- **The fix**: `mdef` out of the union, in every edict (0 but a
  monster's, its body's, its head's); the edict 388 -> 392 bytes (an
  index stays two shifts and two adds; 1.8 KB more of E1M3's pool, the
  largest). And nightmare's `pain_finished` (a monster's field, in the
  union) is set on a monster only: a shootable secret door's `th_pain`
  wrote it over its first stop's x.
- **Checked:**
  - `ents_check` (the bench) refuses an `mdef` that is none of the kinds'
    tables; `MONSTERS=6` now opens E1M3's five secret doors first, then
    rebuilds the lists: with `mdef` back in the union, "CORRUPT ... class
    7 ... mdef ffffffc0" (edict 55); with the fix, "edicts ok", on
    PCSX-Redux and on DuckStation's interpreter (the whole scenario run,
    no warning);
  - against 0c4aee9's references (`HASHONLY=1 PROFILE=0`): the game
    bench, the monsters' and the movers' display lists identical, the
    monsters' states and the movers' hashes identical (two dumps' cycle
    counts ±8);
  - the release built (a scratch directory: the stack 58488 B).
- **Left:** a play on DuckStation (the crash's save state was
  the old code's: it cannot resume on the new).

## §66 — The models' triangles: the corners' xy through the GTE (2026-10-22)

- **The ask** (§63's follow-up): give the
  models' record loops the emit that keeps §63's sort cheap. de and deo1
  draw every alias model but the sorted monsters (items, flames, heads,
  gibs, projectiles, the view model), the brush models and their pieces.
  Their output (the packets, their order) must not change.
- **Before:** POLYS's triangle emit for models (`.if \var`) loaded each
  corner's xy from the vertex cache (`lw`), gave it to the GTE for the
  back-face NCLIP (`mtc2`), then stored it to the packet (`sw`). These
  are three instructions a corner.
- **Now:** `lwc2 C2_SXY0..2` before the NCLIP and `swc2 C2_SXY0..2` into
  the packet after the cull, as §63's ds_tri_emit. These are two
  instructions a corner, three a triangle fewer. In PCSX-Redux's cost model
  (§21.1: lw 2, mtc2 2, lwc2 ~2.4, sw and swc2 3) that is about 4.8 cycles
  less a triangle, drawn or culled.
  - No GTE command runs while the xy are read back: the NCLIP's result
    (MAC0) is read before.
  - t6, t7 and t9 are no longer used there.
- **Left as it was:** the flat branch of that path (POLY_FT3 with `\var`)
  keeps the CPU's copy. No instance assembles it: a model's loop has no
  flat shading. The world's loops (dr, dro*) have no NCLIP.
- **Checked** against master 0c4aee9 built beside (`git archive`), the `H`
  line of every dump:
  - START's 12 views: identical;
  - the 128 spawn views (`BENCH_MAPS=1 HASHONLY=1`): 128 of 128
    identical;
  - the game bench (`BENCH_GAME=1 HASHONLY=1`, the 3D gun in every dump):
    23 of 23;
  - `MONSTERS=1 MAP=E1M8 HASHONLY=1`: 12 of 12;
  - `MOVERS=1` (E1M6's train, E1M3's, E3M4's door: the brush models
    moving): 159 of 159, and its 3 `bmfhash` lines;
  - the C engine (`ENGINE_C=1 BENCH_MAPS=1 HASHONLY=1`): the 128 views'
    hashes are the asm's;
  - the interpreter (`EMU_ARGS=-interpreter MAP=E1M1 BENCH_MAPS=1
    HASHONLY=1`, E1M1's items, flames and brush models): its 12 views the
    dynarec's;
  - hazcheck clean at every link.
- **The gain** (cycles, the views' 16-frame averages):
  - START's 12 views -0.12 % in all, -511..-1766 a view where models are
    in sight (view0 -0.19 %);
  - the 128 spawn views -0.12 % in all, up to -4.9 k a view;
  - the game bench, the 3D gun drawn: -1.8..-3.7 k a frame
    (-0.16..-0.30 %); its walks within the noise (-536, +1724);
  - the movers' frames -315..-819.
- **RAM:** the code 24 bytes shorter (3 instructions in de and deo1): the
  release's stack room +32 B (58488 -> 58520 with §65's edicts; 58504 ->
  58536 before), the default's +16 (46024 -> 46040).
- **After §65's merge** (its edicts 4 bytes longer): START's 12 views and
  the game bench's 23 dumps still master 0c4aee9's, hash for hash.
- **On the way:**
  - the base's first `BENCH_MAPS=1` run froze after E1M4.v3, while E1M5
    loaded (the log's last line a stream read, `stream: in 81f862e5`).
    The emulator was killed and the second run went through. Master's
    code, not this change: noted for the stream's and the drive's
    sessions;
  - two bench builds of one tree run at once (`ENGINE_C=1` and the
    default) when the cook must run again: one build's cook rewrites
    `cooked/` while the other lists it for its disc, and that disc came
    out with no map, no GAME_0.DAT, no STREAM_0.DAT (`world: 0 maps on
    the disc`). Build them one after the other, or run the cook first.

## §67 — The monsters' residency: the report, the slots, the frames (2026-10-22)

- **The ask**: §64's residency loss (3 -> 20 spots of 1020 with the maps
  whole, 2 -> 11 zoned): find ways back to the counts of before §64 without
  touching the monsters' look, measure them, report. Then, from the report: A, B and C to be done.
- **What the measures showed** (the session's instrumentation, not
  committed):
  - the report's simulation (`stream_simulate`) charged each model its
    whole asset. That includes its skin's pixels, 12.5-20.5 KB a body,
    which the engine drops once they are in VRAM (`trims()`);
  - a model charged as the engine holds it, and each spot also loaded by
    the stream's own code (`stream_preload_at`: the CD, VRAM, the
    evictions, the last spot's assets still in). The spots with a
    monster in the PVS not in, of 1020:

    | Bodies | Whole: report / kept bytes / the stream's reads | Zoned: the same |
    |---|---|---|
    | before §64 | 3 / 2 / 2 | 2 / 2 / 2 |
    | §64 | 20 / 2 / 4 | 11 / 2 / 2 |
    | §64's 3 heaviest at 5 units, 6 texels | 15 / 2 / 4 | 5 / 2 / 2 |
    | the same at 8 units, 8 texels | 5 / 2 / 4 | 3 / 2 / 2 |

    The 2 left everywhere are E1M7's: Chthon in the PVS past the 2500
    units' reach, as before §64;
  - the stream's reads found an engine limit: the 48 slots of `res[]`
    (`MAX_RES`) hold the models and the sounds, and `reserve()` and
    `sound_room()` gave up when none was free, with no eviction. On E2M4
    (whole) that kept the shambler out at 2 spots, RAM enough;
  - on E2-E4 the lighter cuts change only the zombie (no soldier, no dog
    there): -16 / -24 KB a map;
  - E3M4's level arena, zoned (whole), KB (on 0c4aee9: §65's edicts, 4
    bytes more each, took 1 KB of the pool since):

    | The skeleton | The zones' slots | Edicts | Brush models' rooms | MDL cache + staging | The stream's pool | Others |
    |---|---|---|---|---|---|---|
    | 322 (the world 670) | 342: 3 x 114 | 149 | 97 | 24 + 16 | 269 (263) | 16 |

    - The zones free 7 KB there. One zone's chunk is 113 KB (the others 93,
      86, 59, 55, 11, 8, 4), and every slot is sized for it.
    - The skeleton: nodes 127, clip hulls 61, leaves 32, PVS 32, the
      brush models' records 43, marks 12.
    - The edicts: 280 entities + 98.
    - The rooms: 63 movers drawn, the most of any map.
    - The levers measured, left open:
      - the seen_1536_96 tables +60 KB, seen_1536_64 +150 (§57.8's
        decision);
      - slots sized zone by zone, up to +74 (the biggest set of three
        chunks 268 KB);
      - the entities single player never spawns +3.4;
  - the frames (4 bytes a vertex a frame), lossless:
    - the frames no state plays, plus the frames equal to an earlier one:
      6-20 KB a map, no cycle;
    - deltas within an animation: 52-115 KB a map (the frames -34..-55 %),
      a decoder's cycles at each frame change;
    - streamed by animation: rejected. The zombie's pains are 101 of its
      198 frames, and a CD read (100-300 ms) would freeze a monster at its
      first pain or death.
- **A, the report** (`engine/main.c` `bench_residency`, `engine/stream.c`):
  - the simulation gives a model the room of its sectors for its read,
    then charges what it keeps: `stream_kept`, read at boot from each
    model's header by the bench (`bench_scan`);
  - the stream's own reads at each spot: `read N` on a spot's line, `(the
    stream's reads: N)` after the map's count.
- **B, the slots** (`engine/stream.c` `slot_for`): a free slot, else the
  one of the resident asset wanted least (a sound or a model), dropped if
  it is wanted less than the asset coming in. `reserve()` and
  `sound_room()` take theirs through it. The rule is RAM's: a pinned
  asset or one wanted as much stays.
- **C, the frames** (formats/qstream.h `QSTM_FMAP`):
  - a body's asset holds a frame map after its header: Quake's frame
    numbers -> the frames stored, `QST_NOFRAME` for one no state plays.
  - The frames stored follow it, in Quake's order. A frame equal to an
    earlier one (every vertex's word) is stored once.
  - The radius is still the one over every frame, so the depth sort and
    the culling do not change.
  - The cook (`tools/qcook/game.c` `MOB_FRAMES`) lists the frames each
    body's QC never plays, by Quake's numbers, from `engine/m_*.c`'s state
    tables:
    - the soldier's load1-11 and the Ogre's pull1-11;
    - the knight's second attackb1 and standing2-5;
    - the scrag's hover9-15 and magatt7-13;
    - the shambler's magic7-8 and the zombie's attb14.
  - `QCOOK_NOFMAP=1` cooks every frame (an A / B). The shared asset
    (`qst_shared`, the cook's check) carries the map.
  - The engine: `mdl_frame` takes the frame stored (`qst_frame_index`) and
    keys its cache by it, so two equal frames expand once. `install_model`
    checks the map (`frames_ok`).
  - At a map's start the bench checks every kind's states against its
    model's frames: `monsters: <kind> state N plays frame F, not in its
    model` (`stream_frame_stored`).

  | Body | Frames | Stored | Never played | Equal | KB kept: before -> after |
  |---|---|---|---|---|---|
  | soldier | 114 | 102 | 11 | 1 | 67.2 -> 60.9 |
  | dog | 86 | 78 | 0 | 8 | 79.2 -> 72.6 |
  | Ogre | 147 | 129 | 11 | 7 | 84.5 -> 75.0 |
  | knight | 97 | 92 | 5 | 0 | 39.5 -> 37.8 |
  | zombie | 198 | 194 | 1 | 3 | 99.9 -> 98.3 |
  | scrag | 54 | 40 | 14 | 0 | 17.4 -> 13.6 |
  | fiend | 69 | 69 | 0 | 0 | 42.1 (no map) |
  | shambler | 94 | 87 | 2 | 5 | 56.2 -> 52.5 |
  | Chthon | 106 | 104 | 0 | 2 | 123.4 -> 121.4 |

  - Per map, one body of each kind: 0-21 KB (E3M4 18.6, E2M6 18.6, E2M2
    16.5, E2M7 14.9, E3M5 17.0, E3M6 13.4, E4M7 5.2).
  - STREAM_0.DAT: 2365440 -> 2330624 B.
- **Checked** (master c1f8bd1's benches run first, the same commands):
  - `QCOOK_NOFMAP=1`: master's STREAM_0.DAT byte for byte; with
    `QCOOK_HAND=progs/ogre.mdl`: the stream of before §64 byte for byte;
  - every frame the states play, read through the map: the same words as
    before. A host script outside the repo read the frames from
    `engine/m_*.c`'s tables. The other 166 assets are unchanged byte for
    byte;
  - the 128 spawn views (`BENCH_MAPS=1 HASHONLY=1 PROFILE=0`): 128 of 128
    `H` lines identical; the game bench (`BENCH_GAME=1 HASHONLY=1`): 23 of
    23;
  - `MONSTERS=1 MAP=E1M8 HASHONLY=1`: 10 of 12 identical, the game lines
    all identical:
    - the other two (`mE1M8_7_c`, `_d`) draw the dead shambler, which
      master left out (149 triangles more, nothing else; the pictures side
      by side);
    - master's frame cache refused its expansion, 2 a frame
      (`mdl_stat_refused`). With the map it refuses none: the equal frames
      share an expansion, 3 fewer by then;
    - this engine with master's stream file gives master's 12 hashes;
  - the frames' check: silent on every bench. A stream with the zombie's
    walk6 left out prints `monsters: monster_zombie state 26 plays frame
    20, not in its model`;
  - the residency (`BENCH_MAPS=1 VIEWS=0 RESIDENCY=1`, whole and zoned):
    2 and 2, the stream's reads 2 and 2, E1M7's alone (E2M4's 2, whole,
    gone with B: the measure's build without it had them);
  - the links' `ramcheck`: release 58552 B (+32), default 45976 B (-64);
    the C engine built (its release 70800 B).
- **Found on the way, left as it was:**
  - the frame cache's ring (`engine/mdl.c`) takes a new expansion only at
    its head. When the bytes there hold an expansion this frame draws, it
    refuses, and the model is not drawn, even with room elsewhere in the
    ring;
  - a model drawn at every frame near the ring's start can then block
    each new expansion after a wrap. The E1M8 shambler above shows it.
    The map makes it rarer, not impossible: proposed apart.
- **Left:**
  - the options not chosen: E3M4's levers, the deltas, the lighter cuts;
  - the outside tools: `quakespasm-q1psx/mobs/mview.py` reads
    the frame map;
  - a play on the tight maps (E3M4, E2M2-E2M7).
