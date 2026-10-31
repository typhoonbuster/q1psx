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
(`SV_WalkMove`). Lets the player walk stairs / ledges ≤ 18 u. (§76: the
"farther" rule is QuakeWorld's; SV_WalkMove keeps a step that lands on a
floor.)

Constants are Quake defaults: maxspeed 320, accel 10, friction 4, stopspeed
100, gravity 800, jump 270, stepsize 18, floor normal z > 0.7. Host sim on
`start.qbsp`: jump apex ≈ 44 u, accel to 320 in ~0.3 s, dead stop at walls.
Known rough edges: no edge-friction dropoff boost (§76 added it), no water, no `pos_acc`
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
  on a TV. The default was 1.5 (chosen 2026-10-15, to try on
  a screen); it is 1.66 since 2026-10-24 (1.5 too dark in play).
  It costs nothing at run time and +584 bytes over the 32 maps. The
  engine's dynamic lights follow it (`engine/dlight.h` `DL_GAIN`,
  53/32 = 1.656: the models' share, and the world's vertices' 53/128).
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
  light gain; × 53/128 since the gain is 1.66, §15), saturating at 255. `dist` comes from a 1024-entry table
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
0), gravity only out of the water and not jumping out of it (§75:
Quake's rule is the waist out, not the feet), the move,
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
  wind the other way, `mdl_wind`); the brush models walked there are the
  world's already (the item boxes were not: §94 winds them). The slow
  path (near or split polygons) still draws a back face: the order above
  keeps them under (the guns'; a box has none: §94 tests the NCLIP before
  the slow path too).
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

## §68 — Single player only: what only multiplayer used, out of the cook (2026-10-22)

- **The ask:** the port will never run multiplayer, neither deathmatch
  nor coop. What
  exists only for it goes: the assets, the maps' resources, the engine's
  and the cook's code. The gain is measured (RAM, disc). For §59.5's
  entities single player never spawns, the cook was chosen, not the
  runtime. Single player must see and play exactly what it did.
- **What only multiplayer used** (id's QC 1.06, read in the rerelease's
  `quakec`; the 32 maps' entity lumps):
  - **the entities single player never spawns:**
    - the coop and deathmatch starts (109 + 225): `SelectSpawnPoint`
      looks for them only in coop or deathmatch. The table already left
      them out (§28);
    - what is on no skill: the three skill flags (not easy 256, not
      medium 512, not hard 1024), which `ED_LoadFromFile` inhibits at
      every skill when deathmatch is 0 (deathmatch alone ignores them).
      471 entities on the 32 maps, 2-36 a map: 213 weapons, 83 ammo
      boxes, 26 powerups, 19 health boxes, 15 armours, 80 `func_wall`,
      5 buttons, 3 doors, 11 `trigger_once`, 4 `trigger_multiple`, 6
      teleporters and their 6 destinations;
  - **what the cook made for them alone:**
    - their brush models (1-10 a map, 109 in all): 909 faces cooked, 852
      render nodes, 9.5 KB of shared clipnodes;
    - the textures only those faces used: 1 or 2 on 27 maps. E2M6's and
      E4M3's are animated: their frame and CLUT were copied into the
      display slot every frame, though nothing drew them;
    - the item boxes only their items used, on 15 maps. START has no item
      at all in single player: its 10 boxes and its bank's items group
      (`weapons/lock4`, `pkup`, `items/armor1`, `damage3`) were
      deathmatch's;
  - **four sounds in every map's bank** (`tools/qcook/sound.c` ALWAYS),
    5.5 + 8.0 + 3.8 + 4.3 KB of SPU RAM:
    - `items/itembk2.wav`, an item's respawn (`SUB_regen`). Only
      deathmatch sets its think time: in single player a taken item is
      gone;
    - `player/teledth1.wav`, a player telefragged (`PainSound`,
      `GibPlayer`). `tdeath_touch` hurts a player only when another
      player arrives on him;
    - `player/axhit1.wav`, a player's pain from an axe (`axhitme`). Only
      `W_FireAxe`, a player's axe, sets it, and only a player's pain reads
      it: a player's axe on a player;
    - `player/tornoff2.wav`, a player leaving the game
      (`ClientDisconnect`);
  - **the main menu's Multiplayer row:** not drawn since §52 (the menu
    draws rows 0 and 2 of `gfx/mainmenu.lmp`), still cooked into
    GAME_0.DAT's aux pages with the Help and Quit rows;
  - **the engine's game code** was already single player's: no
    deathmatch, coop, teamplay, frags, obituaries or respawns (the QC
    transcribed for single player, §29, §40). Left of it:
    `tdeath_touch`'s test that the arriving one is not the player (always
    true when the player is touched: the owner is never the one touching),
    and comments on a greyed Multiplayer entry (menu.h, pad.c, main.c).
- **Not multiplayer's:** STREAM_0.DAT (one copy of each asset for every
  map: an item model deathmatch-only on one map is single player's on
  another); the player's model (never cooked: the port draws no player
  body); the sounds single player plays rarely (`misc/talk.wav`, the
  teleporters' `misc/r_tele*`).

### 68.1 The cook (tools/qcook)

- **`ents.c`:** `ents_parse` leaves out what single player never spawns
  (`ent_sp_never`: the coop and deathmatch starts, the entities on no
  skill). Every reader of the list sees only what single player spawns:
  - the entity table and its strings;
  - `ents_drawn_models`: no faces cooked for their brush models (the
    still models' buried faces too), no textures packed for them;
  - `items.c`: no boxes for their items;
  - `sound.c`: no sound group for them alone, no static sources;
  - `bmf.c` (through the table): no room measured for their models any
    more (START 19 -> 12 models). The engine never reserved one:
    `bmf_reserve` takes the spawned edicts' models.
- **`world.c`:** a brush model no entity uses (in the id1 maps, one of
  those entities') keeps its 40-byte record, so the entities' model
  numbers stay, but not its trees:
  - its render nodes leave the node array. The others are numbered down
    in order (a parent before its children, the sentinel last); their
    children and parents, the leaves' parents, the world's root and the
    used models' roots follow;
  - the face stream is unchanged: a dropped node's blocks are empty (no
    face of the model is drawn), and the cook dies otherwise;
  - its root becomes a leaf of its tree (depth 0), its clip roots the
    world's: `clip_share` then leaves its clipnodes out;
  - the cook prints `N brush models no entity uses: X render nodes -> Y`.
- **`sound.c`:** ALWAYS without the four sounds, each with its reason.
- **`game.c`:** `AUX_BANDS`: `gfx/mainmenu.lmp` keeps the two 20-row
  entries the menu draws, Single Player and Options, stacked. They are
  cut after the whole picture is quantized: its 16 colours are the same.
  k-means over the two rows alone (30 of its 34 colours) would choose
  others.
- **The format is unchanged** (QWVF, QSB2, QGM8): the files are smaller.
  A checkout's older cooked files still load; CMake cooks again by itself
  (the cooker changed).

### 68.2 The engine

- `snd.h` / `snd.c`: the four names out of the named table (4 lookups
  fewer at a map's load).
- `menu.c`: `main_rows` {0, 1}, the cooked picture's rows.
- `triggers.c` `tdeath_touch`: the player touched kills the arriving
  one, with no test of who arrives. The QC's `teledeath2` classname
  (read by the obituaries and a player's gib sound) stays out.
- No asm touched; hazcheck clean.

### 68.3 Checked

Against master a200736 built beside (`git archive`, its cook with the same
zone tables), and before the merge against c1f8bd1 with the same results:

- **The cook:** every world's check. The 32 worlds cooked with every tree
  kept (the same cooker, `used` all set: scratch) against the shipped
  ones, compared by a script (scratch):
  - every section byte for byte but the nodes, the leaves' parents, the
    clipnodes and the brush models' roots;
  - each node kept equal to its old one, its children and parent
    renumbered; the world's hulls and each used model's node for node;
  - the dropped nodes' face blocks empty.
- **The display lists:** the textures' places move (the atlas repacked
  on 27 maps for a texture, on 15 for a box; the stream's skins then sit
  elsewhere in VRAM), so the dumps' `H` lines differ:
  - START's 12 views 12 of 12, the 128 spawn views 94 of 128, the game
    bench's 23 dumps all, the monsters' (`MONSTERS=1`, E1M1) 8 of 8;
  - with every GP0 command decoded and its CLUT, texture page base, UVs,
    texture window and VRAM copy places masked (a script, scratch), the
    packets are the same, in the same order, but:
    - E2M6's and E4M3's 8 views: two VRAM copies fewer each (the
      animated texture only a deathmatch wall drew, its frame and its
      CLUT);
    - the menu's `mmain` and `msingle`: one texture page command more
      (the aux pictures repacked: the cursor's dot, and in `msingle` the
      single player menu's picture, in the other aux page);
- **The pictures** (pktview, each map's dumps with its own textures; the
  maps bench dumped in full by a temporary change, not committed):
  - START's 12, the game bench's 23 (the menu's pages among them: the
    main menu as before) and the monsters' 8: identical;
  - 125 of the 128 spawn views identical. E2M6.v1 differs by 1 pixel,
    E4M4.v3 by 2, E4M2.v0 by 48 (one row). Each is a polygon's edge whose
    UV reaches its texture's tile end: the GPU reads the texel past it,
    which is the atlas' neighbour, master's or this one's;
- **The play:** the physics checks (`clipdiff`, `pmdiff`, START's `pmove`
  lines) the same results; the monsters' scenarios the same states,
  positions and health at every logged tick (the stream there: 32 of 44
  assets in at the first one, not 28: the pool is bigger).
- **The links' `ramcheck`:** release 58552 -> 58664 B, default 45976 ->
  46088 B (+112: the four names and their strings, the named slots).

### 68.4 The gain

The bench's lines (`bench zone`, `zone:`, `world:`, `snd:`, `stream:
pools`), KB:

| map | entities | the edicts' pool | the skeleton, or the whole world | the stream's pool | the map's SPU bank | the stream's VRAM (K texels) |
|---|---|---|---|---|---|---|
| START | 144 -> 108 | 91.7 -> 78.1 | 252 -> 240 | 522 -> 552 (+30) | 377 -> 340 | 315 -> 319 |
| E1M1 | 174 -> 160 | 103.1 -> 97.8 | 252 -> 250 | 463 -> 471 (+8) | 371 -> 350 | 315 -> 315 |
| E1M2 | 262 -> 249 | 136.4 -> 131.5 | 257 -> 256 | 454 -> 460 (+6) | 364 -> 343 | 379 -> 379 |
| E1M3 | 364 -> 347 | 175.1 -> 168.6 | 272 -> 266 | 376 -> 389 (+13) | 364 -> 343 | 379 -> 383 |
| E1M4 | 318 -> 307 | 157.6 -> 153.5 | 297 -> 294 | 397 -> 403 (+6) | 371 -> 350 | 381 -> 381 |
| E1M5 | 323 -> 318 | 159.5 -> 157.6 | whole 537 -> 537 | 444 -> 446 (+2) | 364 -> 343 | 365 -> 367 |
| E1M6 | 271 -> 259 | 139.8 -> 135.3 | whole 437 -> 435 | 512 -> 519 (+7) | 377 -> 356 | 367 -> 367 |
| E1M7 | 113 -> 77 | 79.9 -> 66.3 | whole 176 -> 171 | 915 -> 934 (+19) | 388 -> 367 | 415 -> 417 |
| E1M8 | 177 -> 169 | 104.2 -> 101.2 | whole 347 -> 347 | 711 -> 714 (+3) | 362 -> 340 | 407 -> 407 |
| E2M1 | 256 -> 242 | 134.1 -> 128.8 | 268 -> 266 | 391 -> 399 (+8) | 364 -> 343 | 323 -> 323 |
| E2M2 | 205 -> 189 | 114.8 -> 108.7 | 295 -> 294 | 399 -> 406 (+7) | 364 -> 343 | 391 -> 391 |
| E2M3 | 261 -> 245 | 136.0 -> 130.0 | 285 -> 281 | 405 -> 415 (+10) | 364 -> 343 | 379 -> 379 |
| E2M4 | 308 -> 300 | 153.8 -> 150.8 | 300 -> 295 | 384 -> 393 (+9) | 371 -> 350 | 419 -> 419 |
| E2M5 | 270 -> 254 | 139.4 -> 133.4 | 249 -> 245 | 444 -> 454 (+10) | 371 -> 350 | 389 -> 391 |
| E2M6 | 397 -> 386 | 187.6 -> 183.4 | whole 580 -> 576 | 333 -> 342 (+9) | 364 -> 343 | 391 -> 395 |
| E2M7 | 388 -> 373 | 184.1 -> 178.5 | 290 -> 289 | 334 -> 342 (+8) | 371 -> 350 | 373 -> 375 |
| E3M1 | 247 -> 231 | 130.7 -> 124.7 | 244 -> 241 | 432 -> 442 (+10) | 371 -> 350 | 345 -> 345 |
| E3M2 | 220 -> 218 | 120.5 -> 119.7 | whole 378 -> 376 | 608 -> 611 (+3) | 377 -> 356 | 371 -> 371 |
| E3M3 | 227 -> 218 | 123.1 -> 119.7 | whole 469 -> 469 | 541 -> 544 (+3) | 377 -> 356 | 387 -> 387 |
| E3M4 | 280 -> 271 | 143.2 -> 139.8 | 320 -> 318 | 267 -> 273 (+6) | 377 -> 356 | 373 -> 375 |
| E3M5 | 259 -> 247 | 135.3 -> 130.7 | 320 -> 316 | 345 -> 353 (+8) | 364 -> 343 | 419 -> 423 |
| E3M6 | 326 -> 318 | 160.7 -> 157.6 | whole 729 -> 729 | 247 -> 250 (+3) | 377 -> 356 | 391 -> 391 |
| E3M7 | 219 -> 209 | 120.1 -> 116.3 | whole 548 -> 546 | 486 -> 492 (+6) | 377 -> 356 | 399 -> 399 |
| E4M1 | 286 -> 269 | 145.5 -> 139.1 | 269 -> 265 | 433 -> 444 (+11) | 364 -> 343 | 327 -> 327 |
| E4M2 | 254 -> 238 | 133.4 -> 127.3 | 251 -> 250 | 439 -> 446 (+7) | 364 -> 343 | 433 -> 435 |
| E4M3 | 366 -> 348 | 175.8 -> 169.0 | whole 527 -> 525 | 422 -> 432 (+10) | 377 -> 356 | 359 -> 375 |
| E4M4 | 393 -> 368 | 186.0 -> 176.6 | 268 -> 267 | 407 -> 419 (+12) | 348 -> 327 | 405 -> 407 |
| E4M5 | 398 -> 373 | 187.9 -> 178.5 | 267 -> 260 | 354 -> 370 (+16) | 384 -> 363 | 393 -> 393 |
| E4M6 | 357 -> 343 | 172.4 -> 167.1 | whole 466 -> 464 | 488 -> 495 (+7) | 348 -> 327 | 379 -> 379 |
| E4M7 | 356 -> 341 | 172.0 -> 166.3 | 334 -> 333 | 309 -> 317 (+8) | 371 -> 350 | 379 -> 379 |
| E4M8 | 322 -> 301 | 159.1 -> 151.2 | 249 -> 248 | 406 -> 414 (+8) | 377 -> 356 | 411 -> 411 |
| END | 173 -> 167 | 102.7 -> 100.4 | whole 230 -> 230 | 844 -> 846 (+2) | 362 -> 340 | 457 -> 457 |

- **The stream's pool:** +2..+30 KB a map, 8.6 on average (the arena gives
  it whole 2 KB). The smallest pool is 247 -> 250 KB (E3M6), the tight
  maps gain little (E3M4 +6, E4M7 +8). START gains the most (+30): its
  deathmatch walls, boxes and 36 entities. The edicts' pool is
  `num_ents + 2 + 96` slots of 388 bytes: 0.8-13.6 KB of it.
- **SPU RAM:** every bank 21-22 KB smaller (START 37: its items group).
  That room goes to the streamed sounds' pool (`stream_spu_pool`: the
  rest of SPU RAM).
- **VRAM:** the textures' freed rows go to the stream's regions: +0..+16 K
  texels (E4M3 the most).
- **The disc** (2 KB sectors, the high textures' set):
  - the maps' files 863 KB smaller, 22.3-59.1 KB a map: the banks 711 KB
    of it, the worlds 90, the textures 50, the boxes 12. In whole sectors
    40272 -> 39426 KB (-846 KB);
  - GAME_0.DAT 407904 -> 397664 B (the menu's rows);
  - the medium textures' set (release-medium) -16 KB;
  - the release image 74890032 -> 73883376 B.
- **A load** reads that much less: 22-59 KB a map.
- **The frames:** the 128 views -0.02 %: E2M6's and E4M3's eight views
  -1.3..-1.9 k cycles each (the two VRAM copies a frame), the other 120
  within the noise (-0.5..+0.5 k), START's too.

### 68.5 Left

- **The dead models' records:** 40 bytes each (1-10 a map). Dropping them
  renumbers the models: the entities, the textures' animations
  (`qw_anim.models`), the cook's rooms. Not done.
- **The texels past a tile's edge** (68.3's 51 pixels): a polygon whose
  UV reaches its tile end reads the atlas' neighbour, on the PS1 as in
  pktview. Master shows the same bleed with other neighbours. A gutter
  or the UVs clamped in the cook would end it: a change of its own
  (done: §71, a gutter).
- **The aux pages' freed rows:** the main menu's rows leave 80 rows of the
  two aux pages unused (248 + 184 of 256 + 256). The stream does not use
  the aux pages; lending it their free rows is a change of its own. The
  single player menu's Load and Save rows are as dead (not
  multiplayer's, left).
- **Not checked:** DuckStation and the real machine (the files smaller,
  no engine path changed but the named sounds' table and the menu's
  rows); the C engine (`ENGINE_C=1`), which builds the same C changes.

## §69 — The frame cache: a place past the frame's own expansions (2026-10-22)

- **The ask** (§67's find, proposed apart): the streamed models' frame
  cache (`engine/mdl.c`) refused an expansion while its ring still had
  room. It must refuse only when no place keeps every expansion the frame
  draws. The bench prints the refusals, and the frames stay master's but
  for models now drawn.
- **The flaw:**
  - `mdl_frame` put a new expansion at the ring's head only. When the
    bytes there held one the frame draws (its stamp), it returned NULL,
    and `rl_model_stream` drew nothing for that model;
  - a model drawn at every frame just past the head then blocks each new
    expansion of the frame, room elsewhere or not.
- **Measured on master** (its code with this change's prints, run first;
  a200736, then cf4a7b4 with §68's cook):
  - `MONSTERS=3 MAP=E1M3`, the minute of fight, on both: 129 models left
    out (7, 17, 82 and 23 in four of the twelve 300-frame windows); 138
    with every frame cooked (`QCOOK_NOFMAP=1`);
  - `MONSTERS=1 MAP=E1M8` on cf4a7b4: `mE1M8_7_c` and `_d` refuse 2 a
    frame, two Scrags (wizard.mdl, 576 B each). The ring had just
    wrapped: the head was at 0, where an Ogre drawn that frame held
    0..1120 of the ring's 24576 B. This is the case §67 foresaw;
  - the same dumps on a200736 refused only with every frame cooked: a
    Scrag (frame 32) and an Ogre (frame 18) at head 2376, where a Scrag's
    spike (w_spike.mdl, drawn that frame) held 2816..3168;
  - §67 read that picture as E1M8's dead shambler: it is the Scrag. The
    Ogre shows no pixel in that view (a build that refuses it alone gives
    the same picture as both drawn);
  - the game bench: 0 refusals (23 expansions).
- **The fix** (`engine/mdl.c`):
  - the place: the head; when expansions the frame draws are in the way,
    just past the furthest of them, again until the bytes are free; past
    the ring's end, from its start, once. An expansion stays in one
    piece, and one the frame draws never moves;
  - a refusal now means no gap big enough between the frame's
    expansions, or all 32 entries drawn in the frame;
  - one scan both checks a place and lists the entries it overwrites (a
    32-bit mask), where master scanned twice. A free entry's offset is
    `NOWHERE`, past any place, so the scan needs no asset test; an entry
    keeps its end, not its length;
  - the new entry: one of those overwritten, else a free one, else the
    one drawn longest ago. Master took the free or oldest one first, so
    an overwrite lost one cached expansion more;
  - the miss is a function apart (`expand_new`): mdl_frame is then a
    leaf, its hits save no register, a miss is a tail jump. With the miss
    inline, the fix's prologue saved 7 registers on every call (+25
    cycles a hit).
- **The bench prints the refusals:**
  - each `mon dump` line: the frame cache refused N, in the frame
    measured (the one before the dump, which draws the same);
  - each `mon fight` line: over its 300 frames;
  - at the game bench's end: `bench frame cache: N expansions, M
    refused`.
- **Checked** against master cf4a7b4's runs (the same commands,
  PCSX-Redux in fast forward), and before the merge against a200736's:
  - the 128 spawn views (`BENCH_MAPS=1 HASHONLY=1 PROFILE=0`): 128 of 128
    `H` lines identical; the game bench (`BENCH_GAME=1 HASHONLY=1`): 23
    of 23, 0 refused (both masters);
  - `MONSTERS=1 MAP=E1M8 HASHONLY=1`: 0 refusals. On cf4a7b4, 10 of 12
    identical; the 2 others (`mE1M8_7_c`, `_d`) gain the two Scrags
    (render items 42 -> 44). In pktview's pictures, 10 are identical and
    the two others differ by 122 pixels, a far Scrag (the other one
    hidden). With every frame cooked: 12 of 12 (master refuses none
    there);
  - on a200736: 12 of 12 with the frame map. With every frame cooked, 0
    refusals and the frame map's 12 hashes: `mE1M8_7_c` and `_d` gain
    the Scrag and the Ogre (items 119 -> 121; 2618 pixels, all in the
    Scrag's box);
  - `MONSTERS=3 MAP=E1M3`: 0 refusals in the twelve windows, with both
    cooks;
  - the C engine (`ENGINE_C=1 MONSTERS=1 MAP=E1M8 HASHONLY=1`): its game
    drifts from the asm's (3 of 12 dumps on cf4a7b4, 10 on a200736, on
    master too). Against master's C engine on cf4a7b4: 12 of 12, no
    refusal either way. On a200736: 9 of 12; the 3 others are where
    master's refused (5, 1 and 1 a frame, with the frame map), render
    items 111 -> 116 and 63 -> 64, pixels only on the models now drawn
    (251, 3, 3);
  - a host harness (outside the repo) ran random frames of draws. An
    oracle checked every refusal (no gap between the frame's expansions,
    or every entry the frame's), and every pool returned had to be intact
    at the frame's end. Master refused with room in every workload (88496
    of 90054 in one). The new ring never did, and never overwrote one of
    its frame's.
- **The cost** (PCSX-Redux; a measure build timed each call on root
  counter 2, not committed; on a200736, the fight's refusals the same on
  7903159):
  - the fight on E1M3 (3600 frames, 6165 calls):
    - a hit 378 -> 321 cycles: the leaf -10, and the entries hit sit
      lower in the table (their index 15.9 -> 13.1);
    - a miss's own work 2808 -> 1710 cycles, the expansion apart (~2100);
    - 1.03 scans a miss (6 places past a frame's expansion in 182);
    - mdl_frame 882 -> 727 cycles a frame, the 129 models drawn included;
  - E1M8's monsters' bench (312 calls): a hit 283 -> 261, a miss's own
    work 2868 -> 1746;
  - the frames, against cf4a7b4:
    - the 128 views -0.0045 % (-178..+82 a view);
    - the game bench -430..+65 a frame, its walks -9 and -29;
    - E1M8's dumps -32..-152; the two with the Scrags +73.0 k, the
      models drawn;
    - the fight's windows where master refused +0.21..+2.41 %, the models
      drawn; the others -0.05..+0.02 %;
  - each place past a frame's expansion costs one scan more (~600
    cycles). At worst, two passes over the ring, one scan per expansion
    jumped (never seen in the benches).
- **RAM:** the code +100 B (+68 the fix, +32 the miss apart): the
  release's stack room 58664 -> 58552 B, the default's 46088 -> 45992
  (7903159).
- **Left:**
  - the lookup: 32 entries at 15 cycles each in PCSX's cost model (`lh`,
    two `nop`s, the loop's branch and counter). An (asset, frame) key word
    and a sentinel would make it ~8 an entry (about -90 cycles a hit at
    index 13); or asm;
  - the ring can still refuse when the frame's own expansions leave no
    gap big enough (the harness's heavier workloads; no bench scene).
    The ring's size and a compaction at the frame's start are the levers,
    if a map shows it;
  - the play on PCSX-Redux's and DuckStation's GPU: the crowds (E1M3,
    E3M4) all drawn.

## §70 — The C left, measured: where the cycles go (2026-10-22)

- **The question**: space and cycles are tight, and too much of the engine is still C: the C not yet in asm, and
  what moving it would gain.
- **The measure**: the PC sampler (`SAMPLE=1`, PCSX-Redux at 10x,
  `PROFILE=0`), its 32-byte blocks put on the bench ELF's functions (a
  scratch symbolizer: asm = the `.s` objects' symbols, C = the `.c`
  objects', the rest the SDK):
  - start's 12 views and E1M3's (drawing): the asm 57-61 % (r_world.s,
    r_slow.s), the SDK's waits 37-38 % (vblank, the GPU's queue), the C
    2-5 %;
  - the game bench (START, walking, firing): the asm 66 %, the C 7 %;
  - E1M3's fight (`MONSTERS=3`: the 47 monsters awake, rockets, a frame
    drawn each tick; now sampled, "samples fight"): clip.s 35.6 % (the
    monsters' contents and traces), r_world.s 15.9 %, r_slow.s 15.0 %,
    the C 16.8 %, the waits 16.7 %.
- **The C, file by file** (the release's code; share of the fight, the
  game bench, E1M3's views):

  | file | code | fight | game | views |
  |---|---|---|---|---|
  | stream.c | 14.3 KB | 4.5 % | 1.7 % | 0.2 % |
  | ents.c | 20.3 KB | 3.5 % | 1.8 % | 0.7 % |
  | ai.c | 17.0 KB | 2.8 % | 0.2 % | 0 |
  | world.c | 6.3 KB | 1.4 % | 0 | 0 |
  | game.c | 36.2 KB | 0.6 % | 0.6 % | 0 |
  | monsters.c | 6.4 KB | 0.6 % | 0.6 % | 1.0 % |
  | rlist.c | 5.5 KB | 0.5 % | 0.6 % | 1.0 % |
  | render.c, snd.c, items.c, hud.c, bmfrag.c, vid.c, dlight.c | 37 KB | 0.1-0.4 % each | | 0.1-0.8 % each |
  | the monsters' own (m_*.c), movers, triggers, menus, loads, zones, cd, music | ~80 KB | 0 | 0 | 0 |

  The hottest C functions in the fight: `held_by_more` 3.6 % (stream.c),
  `world_point_leaf_from` 1.3 %, `touch_gather` 1.2 %, `ents_tick` 0.6 %,
  ai.c's `traceline` 0.6 %, `next_wanted` 0.4 %, `monsters_render` 0.4 %,
  then 0.1-0.3 % each (`ai_visible`, `movestep`, `step_direction`,
  `ai_physics_step`, `check_water_transition`, `ent_link`, `area_of`,
  `items_render`, `rl_add_particle`). The bench's own loop and
  `prof_now` take 1.5 % of it.
- **What it says**: the C is not the brake: two thirds of the busiest
  frames are asm already, and a third of them the physics' asm (the
  monsters' movement checks). All the hot C in asm at the ×1.4-1.8 §55
  measured would take about 5 % off E1M3's busy frames, 2-3 % off the
  game's, 1-2 % off the views'.
  - `held_by_more` is quadratic: `next_wanted` calls it for every asset
    stuck for room, and it walks every resident slot; a sum over the
    resident assets sorted by score, once a pass, makes it a lookup (29 %
    of the crowd's sim-only ticks, §61's bench).
  - the physics' 36 %: fewer calls (what the monsters ask each tick:
    CheckBottom's four contents and its traces, the walk's probes, the
    lines of sight) is the lever there, the code being asm already.
  - the code's size: the C is 190 KB, mostly cold or rarely run (no
    sample in 120 KB of it); asm would not shrink it much. The RAM's
    levers stay those of §60 (overlays, shared buffers, -Os).

## §71 — The texels past a tile's edge: a gutter, the packer on the texel grid (2026-10-22)

- **The ask** (§68.5's follow-up): world polygons show a row or a column
  of wrong texels along an edge (§68.3: E4M2.v0's row y 152, E2M6.v1,
  E4M4.v3, with other colours in master's pictures). Find the cause in
  the cook, measure it, propose the fixes with their costs, implement the
  choice made. Quake's look, nothing invented.

### 71.1 The cause

- **The cook.** `tools/qcook/world.c` cuts a face at every repetition of
  a texture axis whose size is not a power of two (no texture window can
  wrap it). A piece's UVs run from its cell's corner, 0 to the tile's
  size N (the finest level's layout); a set maps them onto its own tiles
  (`qtex_uvmap`: `(u * su + 16384) >> 15`, so at half resolution
  u = rep - 1 lands on N too). A polygon along a cut, or ending where a
  repetition ends, has the UV N on its far edge. `tex.c` packed the tiles
  edge to edge (8 × 8-texel cells): texel N is the neighbour's, another
  texture's texel drawn with this one's CLUT.
- **The GPU** rounds the texture coordinates it interpolates to the
  nearest texel: its stepper starts half a texel up (Mednafen's and
  DuckStation's rasterizers, which follow the hardware: `(u << 12) +
  (1 << 11)`, the texel at `>> 24`). Every pixel whose exact U lies in
  [N - 0.5, N) reads texel N: a band half a texel wide along each such
  edge, a whole row or column of pixels once the texture is magnified.
  pktview cut the coordinates instead, so it showed only the pixels lying
  exactly on a top or left edge whose UV is N: §68's 51 pixels.
- **A power-of-two axis** has no such edge: the texture window folds
  texel N onto the tile's first, the texture goes on.
- **Quake** (`r_surf.c` `R_DrawSurface`, `d_scan.c` `D_DrawSpans8`): a
  face's surface repeats the texture modulo its size, and the spans clamp
  s and t inside the surface ("-1 (-epsilon) so we never wander off the
  edge of the texture", `d_edge.c`). No other texture's texel ever shows.
- **The page's edge:** no drawn tile ended at u or v 256 on such an axis
  (6 tiles did, animation frames, which are never drawn): its far UV would
  have wrapped to 0 in the byte. The gutter rules it out.

### 71.2 Measured

- **The polygons** whose UV reaches their tile's end on an axis that does
  not wrap (the 32 low worlds' records, the brush models' included, mapped
  onto each set's tiles; a host script reading the cook's tile table,
  scratch): 2224 of 263119 at medium, 2218 at high (0.85 %), on 29 maps
  (none on E3M3, E3M4, END), with 100 tiles in all (a texture counted
  once a map). Most are on their tile's bottom edge. By texture (high):
  `lgmetal` 320 × 192 1009 (E1M8 801), `lgmetal4` 309 (E3M5), the
  48 × 48 buttons 183 (`+0button`, `+0shoot`, animated), `arch7` 99,
  `lgmetal3` 74, then the 192-high windows, doors and arches
  (`window01_*`, `wexit01`, `door0*`, `church7`...).
- **The pictures** (pktview with the GPU's rounding, 71.5): 1725 pixels
  of 23 of the 128 spawn views read a neighbour's texel (E1M8.v0 465,
  E1M8.v1 244, E3M6.v3 238, E3M6.v1 210, E4M6.v1 87, E4M2.v0 45...): lines
  along the repetitions' seams (E1M8's walls), across E4M2's floor.
  pktview's truncation showed 528 of them, in 13 views.

### 71.3 The options

A was chosen, with the packer revised.

| option | the look | VRAM (the 4-bit pages' rows, 32 maps) | disc | the CPU |
|---|---|---|---|---|
| A: a gutter repeating the tile's first column / row | the texture goes on past the tile's end, as the window does on a power-of-two axis and Quake's surfaces do | packer as it was: high +1156 rows (+36 a map, at most +96), medium +298; revised packer: high +510 (+16 a map, at most +64), medium -27 | high +148 KB / +65 KB, medium +38 KB / -3 KB | 0 |
| B: the same gutter repeating its last column / row | Quake's texel at a face's end (the spans' clamp); along the cook's cuts the last texel shows 1.5 wide, the next piece's first 0.5 | as A | as A | 0 |
| C: the UVs clamped to N - 1 at the load | the 2200 polygons' texture squeezed by a texel (1-4 %): a deformation Quake does not have | 0 (the tiles' sizes in the table: a format change) | 0 | 0 a frame |

- A and B differ by 1185 pixels over the same 23 views; on E4M2's floor
  no difference shows.
- **The packer's cost** is the fragmentation, not the gutters' texels
  (23 K in all at high, 89 rows): 96 + 96 + 64 rows filled a page exactly,
  97 + 97 + 64 does not; five 48-wide buttons took a 256-texel row, five
  52-wide ones do not. Measured on the way (scratch): the gutter only on
  the axes some polygon reaches (+946 rows at high: the same), the
  animation frames without one (the engine would fill the display tile's:
  +233 at high, -104 at medium; not chosen).

### 71.4 The cook (tools/qcook/tex.c)

- **The gutter:** a miptex tile with an axis that does not wrap gets one
  texel past its end on that axis (`tex_slot` `gut_x`, `gut_y`): the
  right column, the bottom row, the corner, which repeat the tile's first
  column, row and texel. The tile's size, its UVs and their map are
  unchanged; the models' skins keep their own border (§22.8).
- **The packer:** first fit on the texel grid, over the map's six pages
  by page, row, then column (a taken texel moves the column past it).
  A wrapped axis still starts at a multiple of its size (the window);
  an animated texture's tiles at a multiple of 4 texels, 4 wide (the
  engine moves them in halfwords of the 4-bit pages); any other tile at
  any texel. Eight orders are packed (biggest first by area, height,
  width or longest side, each also with the tiles that wrap on both axes
  first) and the one using the fewest rows is kept (the first on a tie;
  the last tie-break is the slot, so the result does not depend on
  qsort). On its own (no gutter) it uses 240 rows fewer at medium and 144
  at high than the 8-texel cells did. A set with no gutter keeps its
  place: E3M3's, E3M4's and END's are byte for byte master's.
- **The 4-bit set:** each texture's 16 colours are chosen from the tile
  alone, as before; the gutter is coded with them.
- **The animated textures:** `qw_anim_tile`'s size includes the gutter, so
  the engine's move (`engine/anim.c`) brings the frame's gutter into the
  display tile: 13 halfwords by 49 rows for a 48 × 48 button instead of
  12 by 48. No engine code changed.
- **The format is unchanged** (QTX7): the pages' places and the tables'
  words change, CMake cooks again by itself.

### 71.5 pktview

- `PKTVIEW_UVROUND=1` rounds a polygon's interpolated texture coordinates
  to the nearest texel, as the GPU does: the pictures then show what the
  machine shows at the tiles' edges (and every texel half a texel
  earlier). The default still cuts them, so earlier pictures compare.

### 71.6 Checked

Against master cf4a7b4 built beside (a worktree of origin/master, its cook
with the same zone tables):

- **The cooked files:** the worlds, the skeletons, the models, the banks,
  GAME_0.DAT and STREAM_0.DAT byte for byte master's; 58 of the 64
  texture sets and 26 of the 40 zone chunk files differ (the tiles'
  places: the chunks' UVs are mapped onto them).
- **The tiles** (a host script reading both sets, scratch): every tile of
  the 64 sets (4380, 460 with a gutter) shows master's texels through its
  CLUT, colour for colour; every gutter is its tile's first column, row
  and texel; the animated tiles sit on halfwords; no two tiles share a
  texel; no far UV passes 255. On master's own sets the same script finds
  1003 places where a neighbour's texel sits in a gutter's.
- **The display lists:** START's 12 views, the 128 spawn views (dumped in
  full by a change not committed), the game bench's 23 dumps, the
  monsters' 8 (E1M1): the `H` lines differ (the textures' places); with
  every texture place masked (§68.3's script), the same packets in the
  same order, but the 60 spawn views with an animated button: its frame
  moved 13 × 49 halfwords instead of 12 × 48.
- **The pictures** (pktview, each map with its own textures):
  - pktview as it was: START's 12, the game's 23 and the monsters' 8
    identical; the 128 views 528 pixels in 13 views (E1M8.v0 267,
    E1M8.v1 163, E4M2.v0 47...);
  - `PKTVIEW_UVROUND=1`: START's and the monsters' identical, the game's
    73 pixels in 5 dumps (`gwalk0`, `gwalk1`, `gwalk3`, `gsshotgun`,
    `glightning`: a column along one of START's walls), the 128 views
    1725 pixels in 23 views;
  - each of them a texel past a tile's end in master: the tiles' texels
    are master's, the packets the same, and a cook with other places (the
    first try's 4-texel grid) gives the same 128 pictures pixel for
    pixel.
- **The frames' cycles:** START's 12 views and the game's 23 dumps within
  2 cycles; the 128 views -0.004 % (46266 k -> 46264 k), each view within
  350 cycles, where two runs of master alone differ by up to 500 a view;
  the vblanks the same.
- **The play:** the physics checks (`clipdiff`, `pmdiff`, START's
  `pmove` lines) the same, 0 mismatches; the monsters' states, positions,
  health and stream counts at every logged tick the same.
- **The release link:** the executable byte for byte master's
  (`ramcheck` 58664 B), hazcheck clean (no asm touched).
- **After the merge with master's §69 and §70** (7031c45, built beside
  again): START's 12 views, the 128 views (`HASHONLY=1`), the game's 23
  and the monsters' 8: the packets the same with the places masked, the
  pictures as above (the game's 73 pixels, START's and the monsters'
  none), the monsters' lines the same; the cycles the same but three game
  dumps 118-348 cycles fewer, run after run (`gaxe`, `gsnailgun`,
  `gglauncher`: their sound and entity stages; the bigger texture sets
  put STREAM_0.DAT 29 sectors later on the bench's disc, so a streamed
  sound comes in another frame), the vblanks the same; the release's
  executable byte for byte master's (`ramcheck` 58552 B).

### 71.7 The cost

- **VRAM**, the 4-bit pages' rows over the 32 maps (the stream gets the
  rest, engine/stream.c): high 28624 -> 29134 (+510, +16 a map: E1M5
  +64, E2M2 and E1M4 +53, E2M4, E4M5 and E4M8 +36; 11 maps the same,
  E1M1, E2M6 and E4M1 13-16 rows fewer), medium 7480 -> 7453 (-27: six
  maps 8-48 rows fewer, START 32). The packer alone would give 240 and
  144 rows back: the gutters take 213 and 654. In the stream's pool
  (`stream: pools`, high): 69 K texels less in all, 2.2 K a map, at most
  8 K (E1M5: 367 -> 359 K); E1M1, E2M6 and E4M1 gain 1-2 K. Its RAM the
  same.
- **The disc:** the high sets 3805760 -> 3871040 B (+65 KB over 32 maps),
  the medium ones 1098176 -> 1094720 B (-3 KB); the zone chunks the same
  sizes.
- **The CPU:** none (the same packets). The GPU moves 637 halfwords for a
  button's frame instead of 576.
- **The cook:** eight packings a set instead of one, still a hundredth of
  a second a set.

### 71.8 Left

- **Not checked:** DuckStation and the machine (no engine path changed,
  the pages' places only); the C engine (the same cooked files).
- **The half texel:** the GPU shows every texel from half a texel before
  its place (its rounding), Quake from its place (`D_DrawSpans8` cuts):
  the whole texture sits half a texel early on every polygon. Integer UVs
  cannot undo it.
- **The items' boxes** (`items.c` `cook_box`) shift a face's UVs by whole
  repetitions but do not cut it: a box texture with an axis that is not a
  power of two would read past its tile. None in id1 (their sides are
  16 or 32 texels).
- **pktview's default** still cuts the coordinates (the older pictures'
  way); `PKTVIEW_UVROUND=1` is the GPU's.

## §72 — The world's cycles: the stream's lookup, the monsters' fewer calls (2026-10-22)

- **The request**: §70's levers: (1) `held_by_more`'s quadratic walk,
  (2) fewer physics calls from the monsters, Quake's behavior kept,
  (3) the hot C in hand-written asm; and the ticks split: the player's
  at 60 Hz, the rest at 30.
- **(1) The stream's lookup** (`engine/stream.c`): `next_wanted` asked,
  for each asset stuck for room, what the assets wanted less hold
  (`held_by_more`: every resident slot walked). Now the resident assets
  are sorted by their score once a search (insertion sort, the prefix
  sums of their RAM, VRAM and SPU bytes) and each question is a binary
  search; a search that found nothing is not repeated until something
  changes (a slot freed, an asset in, a pass, a stuck mark). The crowd's
  stream cost -71 %; every bench identical.
- **(2a) The sight lines from the PVS** (`engine/ai.c` `ai_visible`): a
  trace from eye to eye is asked only when the eye's leaf sees the
  other's leaf (its PVS row, the render list's id in the leaf's top
  byte masked off): a leaf the row leaves out cannot hold a point the
  trace reaches, so the answer is the same. The crowd's visible traces
  7.8 a tick -> 0, its `ents_tick` -13 %; every bench identical.
- **(2b) The triggers a monster can touch** (`engine/ents.c`): a moving
  monster (an item, what is thrown) walked every trigger slot its box
  reaches, though most touches return at once for all but the player
  (id's QC: `if (other.classname != "player") return;` first: the items,
  `trigger_multiple`/`once`/`secret`, `changelevel`, `setskill`,
  `onlyregistered`, a plat's inside trigger, a `PLAYER_ONLY`
  teleporter). Those are flagged (`FL_ITEM`, ours `FL_TOUCH_PLAYER`) and
  each area node keeps a second list, the other slots (door fields,
  teleporters, `hurt`, `push`, `monsterjump`, `tdeath`, fireballs), in
  the same link order, with its subtree counts: the other edicts walk
  only those, the player all of them. The crowd's `ents_tick` -4 %
  (cumulated with (2a): 261k -> 219k cycles a tick); every bench
  identical.
- **(T) The ticks split** (the player's part at 60 ticks, the rest at 30). A tick each
  vblank's poll, always (`engine/main.c` `player_update`: the player's
  old 30 Hz path, its accumulator and its camera lerp, gone):
  - each 60th: the player's part (pmove, the weapons, the view, its
    trigger touches, the powerups), the movers (`MOVETYPE_PUSH`: they
    carry the player, SV_PushMove), the projectiles (the player's shots
    and those aimed at it), the particles (stepped each 60th already),
    the dynamic lights' decay, the flashes and kicks;
  - the other edicts (the monsters, what they throw, the items, the
    triggers' and delays' thinks) at the world's rate, the Graphics
    menu's "World tick rate" (`settings.tick`, 30 by default, 60 as
    before): at 30, `ents_tick` runs them in two halves by their index's
    parity, a half each tick, each over 2 60ths (`game_dt` 2; the movers
    in between with `game_dt` 1, a `pushers` bitmap from the lists'
    rebuild). Why halves and not every edict every other tick: the
    monsters think at 10 Hz whatever the rate (id's frame macros,
    `nextthink = time + 0.1`), their thinks are most of the cost and do
    not shrink with it; every other tick would put two ticks' thinks in
    one frame, a half each tick keeps the 60 Hz spread;
  - `force_retouch` is counted down every other tick at 30 (both halves
    retouched);
  - drawn (`ents_draw_pose`, `monsters_render`, `items_render`): an edict
    that moves by its velocity (a fall, a throw, a gib's spin) whose half
    was not run this tick is drawn a 60th on, where the 60 Hz world would
    have it; at 60, where it is.
  - The measures (bench `WORLD_HZ=60|30`, 30 the default): at 60 every
    bench is the previous commit's (START's views, the game bench but
    the Graphics menu's two pages, the row's new label, value and hint;
    the monsters' scenario and crowd lines; the movers' 159 frames); at
    30 the movers' ways the same tick for tick (`mvpath` lines, new);
    the crowd (E1M3, 47 monsters woken at once): a tick 219k -> 206k
    cycles on average (-6 %), the worst tick 1.17-1.51M -> 0.65-0.93M
    (-45 %: monsters woken together think together, now half each
    tick); START's walk 55.8k -> 49.6k sim cycles a frame (-11 %); the
    monsters' scenario: each one's death within a tick of the 60 Hz one.
  - Left at 60 by choice: the projectiles (to go
    to 30 they would need the same drawing a 60th on).
- **The room** (the release's `ramcheck`): 58552 -> 57768 B for the stack
  (-784 B: the touch lists' code, the sight lines' PVS test, the drawn
  pose, the movers' bitmap; the stream's sorted table lives on
  `next_wanted`'s stack, 768 B for a moment, not for good, and the two
  trigger lists share one walk, their next links in two rows).
- **(3a) A point's leaf: in asm, and asked less** (the fight sampled again
  after (T), `MONSTERS=3 MAP=E1M3 SAMPLE=1`: `world_point_leaf_from` had
  become 11.9 % of its busy samples, (2a)'s sight lines asking it of both
  eyes, from the root, every time):
  - the eye's leaf kept: the player's for its eye's place (dropped with
    the map: a new map's spawn could hold the old eye's place), a
    monster's in its edict (the alias models' half of the leaves union,
    `am`: unused past `leaves[0]` for them), keyed by the eye's place in
    whole units and cleared when it is linked; `ai_checkclient` and the
    check's own row take it too. 11.9 % -> 3.4 %;
  - a sight line's PVS answer kept: a monster's last (its eye's leaf, its
    target's, the bit: the same two leaves give the same answer), the
    player's read in `check_pvs` when the check took that row from its
    eye's leaf; else the row's walk;
  - `engine/w_leaf.s`: the walk by hand. Both children read with the
    plane, an axial plane's side one subtraction from the coordinate kept
    shifted on the stack (no multiply), the child picked and its leaf bit
    tested in the branches' delay slots (no load waits at the step's end):
    34-40 cycles a node where the C took ~50. `world_point_leaf_from_c`
    follows it (the C engine's); the bench's `wleafdiff` line compares
    them on 4000 points a map, half walked from a random box's top node:
    0 mismatches on START (and its interpreter), E1M3, E2M1; a walk 688 /
    976 / 1319 cycles against the C's 825 / 1159 / 1533 (the timer's
    reads included).
  - The fight's busy samples -11.4 % (266951 -> 236489; the asm's share
    57.2 -> 60.4 %, the C's 26.1 -> 15.4 %); every bench identical
    (`WORLD_HZ=60` against the references; the release's room 57352 B).
- **(3b) The touch walk in asm** (`engine/e_touch.s`, `touch_gather`):
  e's box in registers (box16's floor / ceiling / int16, no trip through
  the stack), the area nodes' stack in the frame, a slot's x span tested
  first, its other loads in the branches' delay slots, four saved
  registers where the C's prologue saved nine. The walk takes a list's
  descriptor (`touch_walk_t`: its counts, heads, nexts, the slots and
  their edicts; `p_defs.inc` TW_*, E_ABSMIN / E_ABSMAX, static-asserted).
  `touch_gather_c` follows it (the C engine's); the bench's `touchdiff`
  line walks both lists of 4000 boxes (half about a random trigger slot,
  most of those reaching it) with both: 0 mismatches on START, E1M1,
  E1M3 (and its interpreter); a walk 843 / 943 cycles against the C's
  997 / 1103. The bench's slot count (`trigtests`) is the asm's own
  (`ASM_BENCH`, a symbol the bench build defines for every asm file).
- **The stream's wants of the edicts' sounds**: a pass walked every edict
  for its five noises; a bitmap of those with a streamed one, made with
  the lists (their noises are set when they spawn), is walked instead.
  Every bench identical (`WORLD_HZ=60`); the release's room 57064 B.
- **Two exact shortcuts in `engine/ents.c`**: a monster's step physics
  is not called on a tick it has nothing to do (on the ground, flying or
  swimming, no think due, still where its contents were taken: each part
  of `ai_physics_step` would return at once; the crowd's tick -1.7 %);
  `ents_model_frame` (the textures' alternates, each frame: every
  animated chain's models looked for in the drawn brush models) starts
  at the model's first entry, a table made with the lists, where it
  walked the list from its start. Every bench identical (`WORLD_HZ=60`).
- **(3c) The render lists' cull in asm** (`engine/e_seen.s`,
  `ents_seen_streamed` / `ents_seen_items`): `monsters_render` and
  `items_render` walked every streamed or item edict each frame, a call
  to `ent_model_leaf` and its tests for each, though most are out of the
  PVS. The asm gives the next position whose edict is in use, has its
  model and a leaf the frame's PVS stamp marks (the leaf looked for
  exactly where the C looked: the same leaves kept), the C draws that
  one: a list pointer, the edict's address by shifts (x 392), its three
  fields read together, the leaf's stamp read in a branch's delay slot
  (with no leaf yet, a harmless read at address 6). `ents_seen_c`
  follows it (the C engine's); the fight compares both lists each frame
  (`seendiff`, untimed): 36867 walks, 0 mismatches. The cull ~0.55 % of
  the fight's busy samples where the C's loops took 0.9 %.
- **(3d) The edicts' tick walk in asm** (`engine/e_tick.s`,
  `ents_tick_loop`): `ents_tick`'s loop over the active bitmap, its
  dispatch by kind and its asleep test; the C keeps the tick's frame
  (the player's touches, the half's mask, `force_retouch`, `game_dt`)
  and calls it. Ten saved registers once a tick, the de Bruijn product's
  latency spent clearing the bit and finding the table, the edict's
  address by shifts, each field read again after any call (a call can
  remove it, change its kind, move it), the bit cleared in memory (the
  calls wake edicts), the word's bits the snapshot taken when it is
  reached, as the C's. `ents_physics_pusher` and `ents_run_think` are
  global for it, `ents_ctz_tab` too. The C loop is the C engine's
  (`ents_tick_loop` there); GCC had inlined the physics' helpers into
  `ents_tick` (4.6 KB): the release's room +720 B (57144 B). Every bench
  identical (`WORLD_HZ=60`); at 30 the asm's runs are the C engine's
  (`MONSTERS=1`, `MOVERS=1`, the crowd); the crowd's tick -0.9 %.

## §73 — Two crashes a bot's long runs found (2026-10-23)

A local build with a bot at the pad (a demo video's, not committed) played
START to E1M3 for long stretches on PCSX-Redux and DuckStation. Two crashes
came out of it, neither of which a bench reaches.

### 73.1 The o32 argument area in §72's asm frames

`ents_tick_loop` (`engine/e_tick.s`) kept `ra` and `s0`-`s2` at 0-12 of its
frame, and `ents_seen_*` (`engine/e_seen.s`) the same. Those 16 bytes are
the o32 ABI's argument area: a callee may write its `a0`-`a3` there.
`ents_run_think` jumps to the think as a tail call (`jr t9`, no frame of
its own), so a think runs on the tick loop's frame. The bot's build keeps
its `printf`s, and `execute_changelevel` ends in one: its varargs prologue
wrote `a1`-`a3` (0 secrets, 6, 56 s) over the kept `s0`-`s2`. `s0` came
back 0, and `ents_tick`'s `game_dt` store after the call (`sw -6984(s0)`)
went to 0xffffe4b8 (PCSX-Redux's "write to unknown address" at E1M1's
intermission). DuckStation ran the rodata at a later return. In the
release, a think whose tail call ends in one of the eleven functions that
write their caller's argument area could do the same (`demon_check_attack`
and `vram_upload` write at 0, where `ra` was).

Both frames now keep the 16 bytes free, as `r_world.s` and `zone.s` do:
`ra` and the `s` registers above them, 16 bytes more of stack. The frames
are otherwise the same (`MONSTERS=3 MAP=E1M3`: every line identical,
`seendiff` 0 mismatches).

An audit of the release's code checked every function's writes above
its frame, through its tail calls, and every function that keeps
registers in its own lowest 16 bytes while it calls:
- `pmove_step` and its helpers (`engine/pmove.s`) and `r_render` keep
  registers there;
- none of their callees writes there today.

### 73.2 The lights' node stamps at the frame count's wrap

The walk's lit-block gate compares a node's `dlstamp` with the frame
count, both 16 bits.
- `r_frame` restarts the count at 1 past 65534 and clears the faces'
  stamps, but it left the nodes' stamps.
- So a node a light reached 65534 frames earlier passed the gate again,
  18 minutes into a map (or less at 30 fps), with no light that frame.
- `dr_light_rec`'s test runs at least once: it took the table's stale
  first light.
- When that sphere reached the record, `dl_light_record_asm` cleared the
  block cache's entries from the table's start to its end in steps of 32
  until the two met. With an empty table they never meet, and the loop
  zeroed every 48th word of RAM, its own code included.

The bot met it when it stood still and fired in one place (its muzzle
flashes stamped the same nodes):
- E1M2 crashed after 1120 s in the map (67200 frames);
- E1M3 crashed after about 1216 s over its lives.

The wrap now clears the nodes' stamps too: `world.nodes` holds them all,
the zones patching only the nodes' face pointers. The cost is a loop over
the nodes once every 65534 frames.
- The bot then stood 10102 s in E1M2 (nine wraps) with no crash.
- START's views are identical (`HASHONLY=1 scripts/dlcheck.sh`).

### 73.3 A monster that never started

`walkmonster_start` (`engine/monsters.c` `monster_start`) adds `random ()
x 0.5` to the fresh edict's `nextthink`, 0. Quake's float is never 0
there (and its `time` is 1.0 at the spawn). Here the share of
`TICKS(50)` is 0 one time in 30 (`frand () x 30 >> 16`), and a think at 0
never runs (`SV_RunThink`'s `thinktime <= 0`). So
`walkmonster_start_go` never ran: the monster stood at its spawn, solid,
`takedamage` 0, frozen for good. The bot met E1M2's first ogre like that
in the corridor after a death's restart.

The fix: a 0 is the first tick (1); 1..29 are unchanged (all due at once,
as Quake's are). `MONSTERS=1` changes from its first line, the 60 ticks
before the scenarios: a monster of E1M1's that the bench had frozen now
starts, and the scenarios' random draws follow another course.

## §74 — The frame cache's lookup in asm: a key word, a sentinel (2026-10-23)

- **The ask** (§69's follow-up, on a branch of its own): make `mdl_frame`'s lookup cheaper, the frames unchanged.
  It walked 32 entries at 15 cycles each in PCSX's cost model (`lh`, two
  `nop`s, the loop's branch and counter): a hit cost ~320 cycles in the
  E1M3 fight. The candidates were a key word and a sentinel, or asm.
- **The table** (`engine/mdl.c` `mdl_cache_t`) holds the entries by
  field:
  - the keys together: asset << 16 | the frame stored, `FREE`
    (0xffffffff) for none. key[32] is the lookup's sentinel, and three
    more words are read past it;
  - then the offsets, the ends and the stamps, the ring and this frame's
    stamp;
  - the miss is `mdl_expand_new`: §69's placement, the same choices, on
    the arrays.
- **The asm** (`engine/mdl_frame.s`):
  - the key: the frame map's entry for a body (§67), the asset and the
    cache's address in the shadows of the flags' branches;
  - the key is written at key[32], then four keys a turn: each compare
    in the shadow of the next key's load, the pointer's step in a
    branch's delay slot (9 instructions, 18 cycles: 4.5 an entry);
  - a hit stamps its entry and returns the ring + its offset. The
    sentinel's is a miss: a tail jump to `mdl_expand_new`, a2 the frame
    stored.
- **The C** (`mdl.c`'s `mdl_frame`, the C engine's and the
  specification): the same key and sentinel, a plain loop. GCC makes it
  `lw`, `addiu`, `bne`, `nop`: 8 cycles an entry.
- **Measured** (PCSX-Redux in fast forward; a measure build timed each
  call on root counter 2, not committed; on master 153b83d). The fight:
  `MONSTERS=3 MAP=E1M3`, 3600 frames, 5941 hits at index 13.5 on
  average, 124 misses. E1M8: `MONSTERS=1`, 268 hits at index 10.0, 31
  misses.

  | Cycles | master (§69) | C, key + sentinel | asm |
  |---|---|---|---|
  | a hit, the fight | 324 | 210 | 134 |
  | a hit, E1M8 | 277 | 187 | 122 |
  | a miss's own work, the fight (the expansion apart) | 1685 | 1438 | 1329 |
  | a miss's own work, E1M8 | 1724 | 1509 | 1381 |
  | mdl_frame a frame, the fight | 665 | 469 | 339 |

  - The frames against master:
    - the fight -0.048 % (-338 cycles a frame, every window lower);
    - the game bench -85..-1435 a frame, its walks -192 and -292;
    - E1M8's dumps -96..-1976 (-0.055 %);
    - the 128 views -0.012 % (-583..+222 a view).
- **Checked** against master 153b83d's runs (the same commands, run
  first; the same results on a93d630 before its ticks landed):
  - `BENCH_MAPS=1 HASHONLY=1 PROFILE=0`: 128 of 128 `H` lines
    identical; `BENCH_GAME=1 HASHONLY=1`: 23 of 23; `MONSTERS=1 MAP=E1M8
    HASHONLY=1`: 12 of 12; 0 refusals in those and in the fight;
  - the asm against the C, call by call: a check build (not committed)
    called the C lookup after each `mdl_frame` and compared the pools.
    8878 calls (E1M8, the fight, the game bench), 0 mismatches;
  - the interpreter (`EMU_ARGS=-interpreter MONSTERS=1 MAP=E1M8
    HASHONLY=1`): 12 of 12 master's;
  - the C engine (`ENGINE_C=1 MONSTERS=1 MAP=E1M8 HASHONLY=1`): 12 of
    12 master's C engine's;
  - §69's host harness on the new C: every pool it returned is
    master's, call by call (five workloads, millions of calls); never a
    refusal with room, never an overwrite;
  - hazcheck clean;
  - after the merge with master af8e353 (§73's crashes, the asm walks),
    against its runs: 128 of 128 views, 23 of 23, E1M8's 12 of 12, the
    interpreter's and the C engine's 11 of 11, 0 refusals, the asm by
    the C 8819 calls with 0 mismatches; the fight -0.044 %.
- **RAM:** the table +16 B (the sentinel's 4 words). The stack room on
  af8e353: release 57080 -> 57016 B, default 44568 -> 44504.
- **Left:**
  - eight keys a turn: ~4.25 an entry, more landing code;
  - a hint per asset (the entry its last hit took, a byte an asset):
    most hits without a scan, for RAM. A call left open.

## §75 — Gravity with the feet in the water (2026-10-23)

- **The report**: in the demo video the player crept through E1M3's
  flooded basement toward the gold key, then came back at full speed.
  Other shallow water did the same: the player held between the surface
  and the water, with no jump out of it.
- **The cause**: `pmove_step` added gravity only when `check_water` found
  no water at all. Quake's `SV_Physics_Client` adds it unless
  `SV_CheckWater` returns true, and that returns `waterlevel > 1`: the
  waist under, not the feet.
  - With the feet alone in the water (level 1), a Quake player keeps
    falling; here it stopped.
  - WaterMove's drag at level 1 (`1 − 0.8 / 60` a tick, §27.1) then
    brought a slow descent to 0 before the floor.
  - The player hung there, at level 1 and in the air. SV_AirMove's
    30-unit cap moved it, with no friction and no ground to jump from;
    the swim's kick and CheckWaterJump need level 2.
  - The bot met it at the basement's steps: 306 units in 7.9 s, 10 units
    over the floor (z −358, the floor's −368).
- **The fix**: `check_water` (`engine/pmove.s`, `engine/pmove.c`) returns
  Quake's value, 1 with the waist under. The level's other readers are
  unchanged: the step-up allowed in the water (any level), the drag, the
  game's sounds and damage. The asm's end is shorter too: level 2 or 3
  in one store after the eyes' test, each return without the jump to a
  common exit.
- **Checked**:
  - `VIEWS=0 scripts/bench.sh`: pmdiff 6000 ticks, 0 mismatches (7 runs
    started in the water).
  - E1M3's basement: `BENCH_MAPS=1 MAP=E1M3 VIEW=-551,-401,-336,143,0
    TICKS=200`, with a print of the player every 4 ticks (not
    committed), the jump held at ticks 48-55, the stick forward from 96.
    Before: z −358 to the end, the jump did nothing, about 45 units/s
    forward. After: on the floor (−367) at tick 12, a jump of 37 units,
    316 units/s forward.
  - Not run: the game bench's water lines (`BENCH_GAME=1`). They change
    at the surface: a player swimming up now falls back once the waist is
    out, as in Quake.

## §76 — Gameplay against id's code: an audit (2026-10-23)

- **The ask**: after §75, look for other places where play departs from
  Quake's, fix them, on a branch tested before master.
- **Read against id's**: the player's physics (`engine/pmove.c` /
  `pmove.s`: sv_user.c, sv_phys.c), the player's game (`engine/game.c`:
  client.qc, player.qc, weapons.qc, combat.qc, PF_aim, view.c), the items
  (items.qc). The monsters' AI, the movers and the triggers were spot
  checked: they are transcribed line by line already (§28-§29, §50) and
  matched, but for the player's `show_hostile` below.

### 76.1 The player's movement (asm and C)

- **Edge friction** (SV_UserFriction, sv_edgefriction 2): the friction
  doubles when the leading edge is over a dropoff. That edge is 16 units
  ahead along the velocity, at the feet, with nothing within 34 units
  down. The test is a point trace: the world and the brush models
  (nomonsters), asking a line of sight's question (`clip_mover.sight`:
  its fraction 1 or not). Quake's start inside solid counts as 1, so
  running into a wall doubles it too, as in Quake. The speed is the one
  in the plane, z left out, as Quake's. §9 had listed this as missing.
- **SV_WalkMove's rules**, which §9 had taken from QuakeWorld:
  - a wall is a vertical plane only (Quake's "step": normal z 0); a steep
    slope no longer starts a step-up;
  - the step's move is level (velocity z 0, the move's z 0);
  - the step is kept whenever its way down lands on a floor, not only
    when it went farther than the plain slide;
  - the way down is 18 units less the move's own z, plus the 2 units it
    had (the ground is met without Quake's gravity on it).
- **Wall friction** (SV_WallFriction): when the step's move is stopped by
  a wall the view faces within 60 degrees of head-on (the view's forward,
  pitch and all, · the normal < −0.5), the speed along the wall is
  multiplied by (1 + that dot + 0.5). It needed the keep rule above: on
  flat ground a slide along a wall is a step kept.
- **Not reproduced**: SV_TryUnstick. Quake nudges the player in 8
  directions when the step made no progress, which costs up to 16 traces
  a tick against a wall. Its own comment calls it a remedy for floats.
- **Cost**: the edge trace, about 1.6 k cycles a walking tick (START's
  scripted walk: 15313 → 16947 cycles a tick; pmdiff's random walk
  10173 → 10357).

### 76.2 The weapons

- **Autoaim** (PF_aim, sv_aim 0.93: Quake's default) for the shotguns,
  the nailguns, the rockets and a level grenade (`w_aim`):
  - the forward when a line 2048 units along it, from 20 over the
    origin, hits a target (DAMAGE_AIM: a monster, an explosive box);
  - else the target most ahead within the 0.93 cone that the line from
    there reaches first: the forward's heading, raised or lowered to its
    origin;
  - integer math in whole units (the cone's dot ×4096: 3809);
  - the line cut to the asm traces' ±4095-unit cube first, as the
    bullets' now are too. A 2048-unit shot from near a map's edge
    overflowed the traces' int16 there.
- **W_BestWeapon**: the lightning gun only with the waist out of the
  water. The automatic changes could pick it under water, where it
  discharges.
- **PlayerPreThink**: an empty weapon is changed for the best one as soon
  as its attack is over (it waited for the next press of fire).
- **The lightning's discharge** keeps the weapon (W_SetCurrentAmmo); the
  rule above changes it.
- **An ammo pickup** ends a firing animation (ammo_touch's
  W_SetCurrentAmmo, player_run).
- **The axe's 4th frame** (axe4 / axeb4) shows for its 0.1 s; it was
  skipped.
- **SuperDamageSound** every second during the nailguns' and the
  lightning's held fire too (player_nail1/2, player_light1/2).
- **show_hostile**: firing sets the player's for a second, so a monster
  within 500 units sees it from behind too (FindTarget).
- **A grenade off level**: one random draw each for the right and up
  offsets (crandom () × v_right × 10 + crandom () × v_up × 10), not one
  per axis.

### 76.3 Death, pain, the view

- **Death** (PlayerDie): MOVETYPE_TOSS. The velocity is kept, and a kick
  of up to 300 units/s is added when not rising. The body falls and
  lands (`dead_toss`, the player's box, no monsters met). Gibbed
  (GibPlayer): the view is the head's (ThrowHead: VelocityForDamage's
  flight, MOVETYPE_BOUNCE with overbounce 1.5, the eye 8 units over the
  feet) and the three gibs are thrown. The view had stayed where the
  player died, mid-air too.
- **Respawn**: jump does it too, as fire does (PlayerDeathThink: any
  button).
- **Pain**: none while invisible (player_pain: the eyes have no pain
  frames).
- **The view up a step** (V_CalcRefdef's oldz): on the ground, a higher
  origin is followed at 80 units/s, at most 12 units below it. The gun
  rides with it. It is reset at a spawn and a teleport.
- **CheckWaterJump's reach**: 24 × cos (pitch / 3). 1.06's makevectors
  of the player entity's angles, normalize discarded.
- **The double-barrelled shotgun's name**: id's "Double-barrelled
  Shotgun".

### 76.4 Checked, unchanged

- The items (health, armor, ammo, weapons, keys, powerups).
- CheckPowerups.
- WaterMove's air, burns and sounds.
- T_Damage and T_RadiusDamage on the player, the projectiles'
  touches.
- PlayerPostThink's landing.
- The AI's FindTarget, visible, range and infront.
- The doors and the triggers.
- The projectiles' water sound (SV_CheckWaterTransition): Quake's toss
  physics only checks on a tick that hit something, so it is almost never
  heard. Nothing to add.
- Left: V_DriftPitch (the digital pad's look; the analog pad looks as
  mlook does, which never drifts), and trigger_push on grenades.

### 76.5 Checking

- `VIEWS=0 scripts/bench.sh`: pmdiff 9000 ticks (6000 + 3000 at 2-6
  60ths), 0 mismatches. START's scripted walk ends where it did (settle,
  run, jump apex 43, wall, back up the stairs).
- `ENGINE_C=1 VIEWS=0` (the C engine): 0 mismatches.
- `BENCH_GAME=1 HASHONLY=1` runs, 0 frame cache refusals. Its frames
  change: the shots are aimed.
- Checks with prints, not committed (`TICKS=` at a `VIEW=`):
  - E1M1, 410 units from a soldier, the view 10 degrees up, one shell:
    the aim picks it, levels the shot, and it loses 4 health. 0.5 s
    later the empty shotgun is changed for the lightning gun.
  - START's stairs: the view lags 12 units at most and rises 10
    sub-units a tick.
  - START, a gib in the air: the head flies, bounces off the ceiling and
    the floor, the view 16 units down.
  - The axe: frames 1-4, 0.1 s each, then 0.
- **The release's room for the stack**: 57016 → 54216 bytes (code
  +2.8 KB: the aim 936 B, the dead body's toss in game_tick, the
  physics' asm).

## §77 — The RAM, the cycles kept: what was never used, what is never used at once (2026-10-23)

- **The ask**: a pass to save RAM while keeping the CPU cycles; every byte
  counts. It runs on the branch of §75-§76.
- **Where it stands**:
  - the release's room for the stack (scripts/ramcheck.sh) was 57016 B on
    master 1afdae4, and 54216 B after §76's code;
  - the level arena (1243 KB) holds the world, the edicts and the stream's
    pool, which takes what the others leave.

### 77.1 Static RAM: the executable and its BSS

- **The particle pool, 512 → 256** (−10240 B). The ultra effects level
  had the 512. §52 left only medium and high in the Graphics page
  (`QL_EFFECT_MASK`), both on 256. The pool's size is now a constant.
- **libc's vsnprintf out** (−4120 B of code, +1056 B of `q_snprintf`).
  The SDK's printf is the BIOS's (a 12-byte stub), so only our own
  snprintf pulled in the 4 KB formatter. `engine/q_fmt.c` does the
  screen's needs:
  - %d %u %x %s %c and %%, a width, the '-' and '0' flags;
  - C99's return value (the overlay's lines add them).

  52 calls changed. A host test against libc's snprintf matched on the
  formats in use, edges included (truncation, negatives, zeros): 0
  mismatches.
- **Work areas never in use at once share one block** (−17472 B), the
  slow path's (`r_slow.s`: the pool, the view entries, the midpoint
  table, 17600 B):
  - the brush models' cut (`bmf_pieces`, `bmf_pending`: 13376 B), which
    runs in `ents_render_bmodels` before `r_frame` draws;
  - a pusher's moved edicts and their old origins (`ents_moved`,
    `ents_moved_from`: 4096 B), used within one pusher's move in the
    tick.

  `slow_poly` keeps nothing in the block from one polygon to the next
  but one thing: entries 0..2's key words, never written, so 0 (a
  corner is no midpoint's). Entry 3's is cleared by each polygon, a
  midpoint table slot is believed only for an entry this polygon wrote,
  and the view entries are written before they are read. So the others
  start at +96, past those three keys. They are aliases defined in
  r_slow.s (`.set`), the C sides `extern`; the C engine (no r_slow.s)
  keeps its own arrays. §60 had listed this sharing as left.
- **The release**: the stack's room 54216 → 85496 B. Against master,
  +28480 B, §76's code paid for.

### 77.2 The level arena: the edicts' pool cut after the spawn

- `ents_load` sized the pool for every entity of the map plus 96 spare
  (ENT_SPARE: those the QC spawns in play). But the entities not on the
  skill and the unknown ones never take a slot, and a removed one
  leaves a hole for the spares.
- The pool is now the arena's last allocation (`ents_load`). After the
  first spawn, `ents_trim` keeps the slots used and ENT_SPARE free ones,
  counting the holes, and gives the rest back: the stream's pool, which
  follows (`stream_map_start`), takes it.
- The free slots stay ENT_SPARE at the least, as the design wants.
  E1M3's crowd (`MONSTERS=3`: 47 monsters, a minute of fight, gibs)
  peaked at 306 of 389 (§61 saw 320 of 462).
- **The stream's pool**: +11.4 KB on average over the 32 maps (0 on
  START, +22 on E1M3, +28 on E4M6).

### 77.3 Checking

- **The 128 spawn views** (`BENCH_MAPS=1 HASHONLY=1 PROFILE=0`, against
  master built beside, run one after the other: two builds sharing the
  cooked files must not cook at once): every `H` and `Q` line identical.
  Frame cycles +0.0069 % (64 views up, 54 down, typically ±463: the code
  moved in the 4 KB instruction cache). The entities' share is
  −2233 cycles in all, the drawing's +5327, and the drawing's code is
  unchanged.
- **The movers** (`MOVERS=1`): E1M6's and E1M3's hashes are master's.
  E3M4's differs by one quad (+40 B of packets) with 306 vblanks against
  308 and its worst frame 2.04 M cycles against 2.46 M. Run without the
  pool's cut, it is master's again (fdd33021): the larger stream's pool
  keeps a model resident.
- **The crowd** (`MONSTERS=3 MAP=E1M3`): no edict refused, the stack
  63 KB never reached.

### 77.4 Narrower: the edict, the projectile, a 64-bit product

- **The edict, 392 → 384 bytes.** `oldorigin` was the secret door's
  alone, and it has no `pos1`: the two share their words in the mover's
  part of the union. 4 bytes of padding take it to 384, which the asm's
  walks multiply by in two shifts and an add (`e_tick.s`, `e_seen.s`: 256
  + 128; 392 took three shifts and two adds). The `E_*` offsets past it
  moved 12 bytes down (`p_defs.inc`, the static asserts).
  - The level arena gives 8 bytes an edict more to the stream's pool.
  - `MOVERS=1`: every line as before, the entities' share −613..−921
    cycles a frame.
  - `MONSTERS=3 MAP=E1M3`: every fight line as before, the fight's frames
    −0.12 %, the entities' tick −149 cycles.
- **The projectile, 96 → 80 bytes** (−768 B):
  - its type, ground, frame and landing flags are bytes;
  - its blast's damage, its model and its sounds are halfwords;
  - its angles stay whole words. In halfwords GCC merged the pitch's and
    the roll's updates into one word with sign extensions (2-4
    instructions a spinning projectile a tick), and the yaw's light
    bucket divides it.
- **The projectile's move in 32 bits.** `vel x 8 x 1092 x dt >> 16` was
  an int64 product: about 25 instructions an axis a tick. Under 40000
  units/s it fits 32 bits, and the arithmetic shift is the same: the
  same result.
- **The game bench** (`BENCH_GAME=1`): every line as before (polygons,
  packets), frame cycles −0.025 % (9 of the 10 views lower).
- **The release**: the stack's room 86200 B (master 57016).

### 77.5 Products in 32 bits where the values are bounded

A product of int64s expands into a long sequence on the R3000 (the high
words' partial products): each one in C costs tens of instructions of
code, and cycles. Where the operands are bounded, the product fits 32
bits and the result is the same: C's division truncates toward zero as
`q_div64_32` does, and an arithmetic shift is the same in both. Changed:

- **The monsters' physics** (`engine/ai.c`): SV_FlyMove's and
  ClipVelocity's dot products, the move's end and time left, the toss's
  move. The velocity is capped at 2000 a component before each move
  (`check_velocity`), and a clip or a crease never lengthens it. Every
  product stays under 2·10^8.
- **The attacks**: the monsters' missiles' directions (`d × 600 / len`
  and the like: a distance under 65536 sub-units), the soldier's spread,
  the boss's lead, the dog's and the fiend's leap speed test.
- **The movers' travel and the secret door's stops** (sums under 2^30),
  a trigger's facing test, `vnormalize` (vectors under 2^16), the view's
  bob, the particles' speeds, a shot's end.
- **Kept in 64 bits**, where a value can overflow: the lengths of
  vectors between two places (squares over 2^32), a damage's push (a
  telefrag's 50000 × 8 × 4096), the armour's share with the quad, the
  monsters' spread from `crand` × 100 × a vector, a mover's place along
  a long way.

The release's stack room: 86200 → 90184 B (−3984 B of code).

- **More cold code** (`Q_COLD`, §60's -Os in its own section): what runs
  at a spawn, a level's start or the boot but was outside the marked
  functions: the items' spawns and drop to the floor, the doors' link,
  the trains' first stop, a trigger's setup and direction, a door's
  travel, the level's spawn, the parms, the boot's inits, the HUD's
  style. −432 B, nothing per frame.
- **Checked**:
  - `MONSTERS=1` on E1M1, E1M3 and E1M7 (each kind's scenarios, the
    boss), against the sources before 77.5: every state line the same,
    but for a stream's read count at one line on E1M7 (48 against 47:
    when a read ends follows the cycles);
  - the release's stack room is 90616 B.

## §78 — The 3D view's internal resolution (2026-10-23)

- **The ask**: a setting that lowers the internal resolution of the 3D
  view, the framebuffer staying 320×240, to try lower resolutions even
  where they play badly. The steps chosen: 256×224, 240×180 and
  200×150, after a first set of halves and quarters (160×120 down to
  40×30). It runs on the branch of §75-§77.
- **Where**: Options → Graphics → 3D resolution (`settings.res`,
  `engine/render.c` `r_res_table`, applied at once); `RES=<index>` in
  `scripts/bench.sh`.

### 78.1 The target and its stretch

- **The target R** is the viewport's bottom-right w×h corner in the back
  buffer. The 3D view draws into it: the world, its models, the
  particles, the gun. At `OT_TARGET` (bucket 5, after `OT_SKY`'s VRAM
  transfers, before the world), a packet sets the draw area and the
  drawing offset to R, sets the mask bit (GP0(E6h)) and clears R with a
  TILE of the background's colour.
- **The stretch** at `OT_STRETCH` (bucket 1, after the 2D gun, before
  the overlay) resets the mask bit and the viewport's area and offset.
  It then draws raw textured quads whose texture is the framebuffer
  itself (15-bit pages) over the viewport.
- **In place**: the source of the pixel (x, y) is R's texel
  (x w / 320, y h / rows), never left of the pixel nor above it.
  - The GPU draws each triangle top-down and each row left to right, so
    it reads every pixel of R before writing over it.
  - The quad's second triangle reads only below and right of the
    diagonal, which its first triangle never wrote.
  - No other VRAM is used: R lies inside the frame it is stretched over.
- **Its pages**: R starts left of x 128, so the stretch is two quads.
  - Page 64 serves the columns up to the seam: a column whose source is
    a whole texel past VRAM x 128 (the screen's column 80, 64 and 16).
  - Page 128 serves the rest: its u reaches 192 at x 320, where page 64's
    would need 256.
  - R's rows lie in one page: h ≤ rows − 16, so the second buffer's R
    starts at VRAM row 256 or below it.
- **The texels' rounding** (§71): the GPU rounds a pixel's u, v to the
  nearest texel. With a share above 1/2 the rounding stays inside R, and
  an emulator that cuts instead (PCSX-Redux) stays inside too.
  - A share of 1/2 or less would read past R's last column. The first set
    of halves and quarters moved its quads half a texel instead: they
    started k pixels before the viewport, m texels before R, with
    k w / 320 = m − 1/2. That is exact only for binary fractions.
  - The quad was dropped with that first set.
- **The mask bit**: the texel 0 is transparent, and a black pixel of R is
  exactly that. With GP0(E6h) bit 0 set while R draws, every pixel of R
  carries bit 15, and the stretch draws them all.
  - GP0(E6h) also acts on CPU-to-VRAM and VRAM-to-VRAM transfers.
  - `OT_SKY`'s transfers (the animated textures, the 2D gun's frames) are
    before the set. The reset comes before the overlay and the next
    frame's transfers.
- **The texture cache** is cleared (GP0(01h)) before each quad: R is new,
  and page 128's lines alias page 64's.
- **The clear**: the stretch covers the viewport, so such a frame's draw
  environment clears nothing (`isbg` 0, set again once the list is
  kicked). A 320×240 fill is saved.
- **The packets**: 140 B of the frame's arena (`vid_res_frame`, cold
  code). An arena too full for them leaves the frame native: nothing
  would fit after them anyway.

### 78.2 The projection

- **The field of view is kept**: the GTE's H is half the target's width,
  and its offset is the target's centre.
- **The slow path** (`r_slow.s` `sp_proj`): x H = 5x << shift as built.
  `r_set_res` patches its two words to x 15 (240×180) or x 1 (256×224),
  and the walk's shift (`r_o_hshift`).
  - 200×150's H of 100 is 25 << 2, which two words cannot multiply.
  - Its entry word becomes a jump to `sp_proj_25`, two x 5 steps, then
    back into sp_proj: 6 cycles more a vertex, at that resolution only.
- **The off-screen tests' bounds**, so that a polygon wholly off the
  target is left out:
  - the far polygons' `lui v0, 240` and `lui v0, 320` (every `POLYS`,
    the sorted models'; `FARSITE` offsets in `r_render`);
  - the slow path's triangles' −320 and −240 (`sp_o_bounds`).
- **256×224's pixels are not square**: on the screen they are 1.25 wide
  and 1.07 high.
  - H is the columns' (128). The view's y row is scaled by 7/6, to the
    rows' H of 149.3, in `r_set_view`: the rotation, |rotation| and the
    translation. The models' composed matrices follow.
  - The frustum's vertical planes are patched, 3z ± 4y → 7z ± 8y, exact
    at 240 rows (`YSITE`, 2 B a site).
  - The boxes' radius 3 rz + 4 ry → 7 rz + 8 ry, exact.
  - The records' and the sorted models' spheres: 8 r → 24 r. The sphere
    is an ellipsoid there, whose reach is
    sqrt(64 b² + 49) × 1.5 r = 17.5 r. One instruction, rho << 4, gives
    24 r; 32 r cost records (START's view 10: 37 → 52, +1.3 %).
  - The C's frustum tests (`ent_in_view`, the brush models'
    `all_culled`) read the unscaled y row (`r_state.yrow`, `ytr`).
- **The 192-row viewport** (the original status bar): the rows go in
  proportion, at most rows − 16. 256 then gets 176 rows, its y row
  scaled by 55/48, and the 7z ± 8y planes are conservative there.
- **The warp bound** stays in the target's pixels:
  k16 = sqrt(448 × 2H / T), H the larger of the columns' and the rows'.
  A target pixel is wider than a screen pixel, so fewer polygons split.
- **The 2D gun**: its corners scaled per axis, centred in the target.

### 78.3 Measured

| START's 12 views | 320×240 | 256×224 | 240×180 | 200×150 |
|---|---|---|---|---|
| CPU, frame cycles (PCSX-Redux, T 4) | 6.42 M | −4.1 % | −11.7 % | −18.3 % |
| GPU, list drawn alone (DuckStation, `GPUTIME=1`) | 3.02 M | +25.2 % | not measured | not measured |

- **The CPU's gain is the slow path's**: fewer splits, the warp counted
  in the target's pixels. Its `r_slow` stage falls by about as much as
  the frame.
- **The GPU's**:
  - the stretch, one 320×240 textured quad of 15-bit texels, costs about
    60-76 k cycles a frame on DuckStation;
  - that is more than 256×224 saves by filling 25 % fewer pixels;
  - the GPU took 114-348 k cycles a frame at 320×240, the CPU 137-806 k,
    so the frame waits on the CPU, and the GPU's extra time changes no
    frame rate.
- **RAM**: the release's room for the stack 90616 → 87560 B (−3056 B).
  - The set-up is cold code (the patches, the plan); the stretch is
    built a frame from a few words.
  - The patch tables are 2 B a site.
- **At 320×240**: the 128 views' display lists are identical to the
  branch's before this (`H` and `Q` lines).

### 78.4 Checking

- **pktview** reads the target: the drawing offset (E5), the mask bit
  (E6) as each pixel's bit 15, a 15-bit texture over the framebuffer from
  what the dump drew there (in place, as the GPU), the cache's clears
  before a primitive. The dump's `R` line marks a frame drawn into a
  target.
  `PKTVIEW_UVROUND=1` gives the GPU's rounding.
- **`GPUTIME=1`** (bench builds): each frame's display list is drawn
  alone, the CPU waiting, and the bench lines get `gpu=avg/max`. Only an
  emulator that counts the GPU's time gives a figure: DuckStation, on
  the bench's `.cue`. PCSX-Redux draws at once and gives 0.

## §79 — The CPU's pass: the split test, the warp's control, the cook (2026-10-24)

- **Why**: the game is CPU-bound (§78.3: the GPU at most 62 % of a 60 Hz
  frame, the CPU up to 143 %). Three steps, on one
  branch: a tighter estimate of the warp (the slow path's splits), the
  warp's floor or its dynamic control, then fewer vertices from the cook.
- **Where the CPU goes** (START's views, PCSX-Redux's PC sampler, by
  label): in the slow path, the edge tests themselves (three multiplies a
  triangle) lead, then a triangle's pop and loads. The midpoints'
  divisions come far behind (2.7 % of the samples). The polygons that enter
  the slow path and leave unsplit are few: 0-10 a view, so the record's
  and the polygon's gates are close to exact.

### 79.1 The edge's length

- The split test bounded an edge's screen length by |dx| + |dy|, which is
  up to 41 % over on a diagonal. Perspective makes most of a near floor's
  and ceiling's edges diagonal on screen, so they split more often than
  their warp warrants.
- Now the length is max + 3/8 min of |dx|, |dy|: an octagon within 3 %
  under and 7 % over the true length. The threshold is T px of
  displacement in every direction; a diagonal was held to about 1.4 times
  stricter than an axis.
- **The asm**: NOSPLIT still tests |dx| + |dy| (the cheap bound: no split
  there is no split by the true length either). An edge it marks goes to
  `REFINE` (`sp_refine`, before `sp_split`): the octagon, the multiply,
  the same threshold. The C's `split_edge` takes the octagon directly. The
  splits are the same, as the octagon never exceeds |dx| + |dy|.
- **Measured** (T 4):
  - START's 12 views: the slow path's triangles 4201 → 3585 (−15 %), the
    frames −1.85 % (the heavy views −2.4 to −4.5 %, the light ones up to
    +2.8 %: REFINE's cost on views whose splits stay);
  - the 128 views: −1.04 %, 77 faster and 51 slower; the views over a
    vblank 24 → 22;
  - the C engine's frames are the asm's (128 of 128 `H` lines).
- **Tried and left out**:
  - the screen's midpoint (u, v and z perspective-correct there) instead
    of the 3D midpoint: +6 % triangles and +59 % pieces off screen. The
    3D midpoint lies nearer the far end, which suits the near geometry
    running off the screen;
  - REFINE skipping the edges within 1/8 of an axis (their octagon within
    7 % of |dx| + |dy|): its check cost more than the multiplies it
    saved (−1.40 % against −1.85 %).
- **The gates** (the record's sphere, CAREFUL's box) keep |dx| + |dy|.
  They pass a few more polygons the slow path then keeps whole (54 → 103
  over START's 12 views). That is cheaper than the octagon in every
  careful polygon.

### 79.2 The warp's control: no warp spent for nothing

- **What it did**: the dynamic control (§19.4) raises T by a quarter on
  each frame over 7/8 of the cap's vblanks, up to 32 px, and lowers it by
  1 px after 4 frames with room. A view that misses the vblank even at
  32 px stayed at 32: the most warp, at 30 fps all the same.
- **Now** (`r_warp_feedback`, `r_warp.more`): 30 frames in a row at 32 px
  missing the vblanks aimed at (the cap's, a load's and a view's first
  frames aside) make the control aim at one vblank more, from the floor.
  The view is drawn at 4 px, at the frame rate it gets anyway.
  - It aims at the cap's vblanks again when a frame at the floor is
    lighter than the first one there by what 32 px lacked.
  - It also tries again after 128 frames, then 256, up to 1024 when the
    tries fail.
  - A map's start resets it.
- **Measured** (`WARPDYN=1`, the bench's new option: the game's control,
  each view's last T printed):
  - START's 12 views (68 frames each): the view no T fits (view 0) went
    from T 32 to 4 at the same 2 vblanks, and the next view from 14 to 5
    (it no longer comes down from 32); the others are the same;
  - the 128 views (20 frames each): line for line the old control's
    (mean 1.031 vblanks, 4 over one, mean T 28.3). That T is mostly left
    over from the views before: it comes down 1 px every 4 frames.
- **Tried and left out**:
  - aiming one vblank more after 8 frames, not 30: a load's or a view's
    first frames passed for a view no T fits. 8 views over a vblank
    against 4, mean T 20.1 against 28.3;
  - raising T by a half on a missed vblank (a quarter otherwise): no
    change on these benches, whose views start at the T the ones before
    left.
- **The floor stays at 4 px**: the control gets 124 of the 128 spawn views
  to a vblank by itself. A higher floor would add warp everywhere, and
  frames only where T is still climbing. The views it cannot fit need
  fewer cycles at any T: §79.3.

### 79.3 The cook: fewer polygons, bigger records (measured, not built)

- **The record stage's costs** (the 128 views' counters, a least-squares
  fit): about 585 cycles a record and 184 a polygon. Records weigh 42 % of
  the stage at START's grouping (§14.6: connected faces of a node's side
  with one texture and the same marking leaves, at most 160 units). The fit
  is not causal: see the regrouping below.
- **The polygons that pay for nothing** (START's heaviest view, pktview
  `PKTVIEW_OCCL=1`): 2047 processed, 1166 emitted (the backfacing models'
  halves, the off-screen ones), 299 of those wholly painted over (29 %).
- **Coplanar faces merged** (a scratch analysis in the cook: a node's
  faces with one texture, side and texture cell, merged while their union
  is convex): −3.6 % polygons on start, −4.4 % e1m1, −5.7 % e1m3, −3.6 %
  e2m1, −2.5 % e3m4. That is about 1.5 % of a heavy frame. The cost is a
  coarser light: the merged pieces lose the vertices on their inner edges,
  where the light was sampled. Not built.
- **Bigger records**, the 32 maps cooked again each time, the 128 views'
  totals against the kept grouping (45.43 M cycles, 9674 records, 40230
  polygons):

  | grouping | records | polygons | frames |
  |---|---|---|---|
  | radius 240 units | −4.1 % | +2.1 % | +0.6 % |
  | radius 320 units | −5.4 % | +2.7 % | +0.8 % |
  | marks not checked, 160 | −13.5 % | +4.2 % | +0.7 % |
  | marks not checked, 240 | −24.0 % | +10.7 % | +2.7 % |

  Every variant draws the faces its records add in more views (the slow
  path's share rises with them) and loses more than the records save. The
  §14.6 grouping stays.

## §81 — The asm pass: the interrupts, the slow path, every asm file (2026-10-24)

- **The ask**: on a branch of its own (from the CPU pass's), the slow
  path first, then every asm file, drawing on
  a list of known R3000A / PS1 techniques: the load and branch delay
  slots, the GTE's scheduling, a minimal exception handler in place of
  the BIOS's, the scratchpad, the DMA, the ordering table, self-modifying
  code. Every gain is kept, however small; RAM gains too.
- **The references** (the branch's base, PCSX-Redux at 10×, `HASHONLY=1
  PROFILE=0`, `scripts/dlcheck.sh`): START's 12 views and the physics
  checks (the default bench), the 128 spawn views (`BENCH_MAPS=1`), the
  game (`BENCH_GAME=1`), the monsters (`MONSTERS=1`), E1M3's crowd
  (`MONSTERS=2`) and the movers (`MOVERS=1`). Each step below keeps every
  display list, every monster line (cycles aside) and every mover hash,
  under the dynarec and the interpreter.

### 81.1 The interrupts without the kernel's dispatch

- **Before**: psxetc (PSn00bSDK v0.24) reached its callbacks through the
  BIOS. The kernel's exception handler saves every register into its
  thread block and runs its priority chains. It then jumps to the custom
  exit (`HookEntryInt`) that calls the callbacks, and
  `ReturnFromException` restores the thread block. That is ~2.7 k cycles
  an interrupt on PCSX-Redux (§25.7). Every frame takes the vblank, the
  GPU's DMA end and the profile timer's wrap; the CD adds its own while
  it streams.
- **Now** (`engine/irq.s`, `engine/irq.c`):
  - the RAM exception vector (0x80000080) jumps to `q_exc`;
  - on an interrupt, `q_exc` saves the registers a C callback may change
    (26 words), runs the callbacks on its own stack with the program's
    gp, as psxetc's `_global_isr` does (each pending, enabled source
    acknowledged, then its callback, again until none is pending), and
    returns with `rfe`;
  - a return onto a GTE command skips it, as the kernel does: the command
    ran before the interrupt (psx-spx);
  - every other exception (the syscalls: `EnterCriticalSection`...) goes
    on to the kernel's handler, through its vector's four words copied
    into `q_bios_vec` (`lui` / `addiu` / `jr k0`, checked at the install:
    OpenBIOS and Sony's);
  - `irq.c` is psxetc's API with the same calls and effects
    (`InterruptCallback`, `DMACallback`, `ResetCallback`,
    `SetDMAPriority`...), so the link leaves psxetc's object out. psxgpu,
    psxcd and psxspu register their callbacks there as before.
    `q_dma_isr` is the DMA master handler.
- **Frames**:
  - START's 12 views −0.6 %; view 3 now always fits one vblank (`vbl`
    106 → 100);
  - the 128 views −0.7 % (44.60 M → 44.30 M), the worst 1 133 717 →
    1 116 015;
  - the game: walk60 −0.4 %, walk30 −1.4 % (its worst frame 1 250 240 →
    1 197 352);
  - E1M3's crowd −0.6 %; the movers −0.6 %.
- **RAM**: psxetc's 4 KB interrupt stack becomes 1 KB. 500 bytes are
  reached on every bench; the bench prints `isr stack: N bytes never
  reached` before `bench done`. The release's stack room: 86 664 →
  90 048 B (+3.3 KB).
- **Left**: a check on DuckStation (Sony's BIOS: a run before the
  rule against DuckStation runs here went through START's views 0-7) and
  on the machine.

### 81.2 The pad's poll by interrupts

- **Before** (§25.7): the game polled port 1 in the vblank callback, a
  byte at a time, waiting for each answer and each /ACK: ~10 k cycles a
  frame on PCSX-Redux, ~12 k on the machine (9 bytes of ~40 µs). The
  interrupt-driven poll written then cost more, as each byte paid the
  kernel's dispatch.
- **Now** (`engine/pad.c`):
  - the vblank selects the pad and sends the first byte;
  - each byte's /ACK interrupt (SIO0, IRQ 7) reads its answer and sends
    the next one;
  - the last answer, which no /ACK follows, comes by the receive
    interrupt;
  - the last answer puts the poll in the ring, as the vblank did.
  - A poll still going at the next vblank (no pad, or one that stopped
    answering) ends there with what came.
  - `pad_frame` waits for this vblank's poll while its bytes keep coming
    (at most ~120 µs since the last one), so the game reads the same
    poll as before.
- **Measured** (the bench's new `IRQTIME=1`: the handler times itself;
  `irq=` is its cycles a frame, every interrupt, those in the wait for
  the vblank too):
  - START's views in one vblank: 10 740 → 3 800 cycles of interrupts a
    frame, 3 → 12 interrupts;
  - views over two vblanks: 21 140 → 7 270.
  - The frames' own counts rise by 2.3-2.9 k in the one-vblank views: the
    bytes' interrupts now land in the frame, where the old poll ran in
    the vblank handler, in the wait. The work is the same (identical
    display lists), so the CPU's load a vblank falls by ~6.9 k (1.2 % of
    a frame).
  - The game: walk60 −2.8 %, walk30 −3.7 % (sim + frame, against §81.1).
    walk30's frames in one vblank go from 103 to 150 of 300.
- **RAM**: the release's stack room 90 048 → 89 600 B (the driver's
  code).
- **Left**: a check on DuckStation and the machine (the pad,
  digital and analog, plugged in and out while playing). PCSX-Redux
  answers the receive interrupt: the last byte comes by it.

### 81.3 The asm, instruction by instruction

The rule: every change that saves cycles and costs none anywhere, however
few, goes in; a change that wins at one setting and loses at another is
measured at both and left out. The views are measured at the bench's
T = 4 (its floor, the dearest) and at T = 32: the game's warp control
holds the 128 spawn views at T 28 on average (`WARPDYN=1`).

- **The slow path's REFINE** (`engine/r_slow.s`): three of its four delay
  slots held nops, for want of a register. dz now goes in a0 (MIDLOOK and
  sm_new reset it, sp_refine and sp_leaf_max set it again for sp_emit):
  the 128 views −0.34 % at T = 4, −0.05 % at T = 32.
- **The hull trace** (`engine/clip.s`): the split's |den| and fm, the
  clamp and the open sweep's end fill four delay slots; ct_done returns
  in four instructions. −7 cycles a trace, −23 a pmove tick.
- **CAREFUL's ranges** (`engine/r_world.s`, every record loop): the last
  MINMAX's delay slot held a nop. A triangle's y0 now loads in the x
  range's slot (into t6, free in a triangle), and 2 T2 zmin (`mflo`) in
  the y range's; W + H moves to t9. Two instructions fewer for a careful
  triangle, one for a quad: the 128 views −0.18 % at T = 4, −0.12 % at
  T = 32.
- **The brush models' cut** (`engine/bmfrag.s`), checked cut by cut
  against its C (`BMF_C=2`, dynarec and interpreter: 6165 cuts, 0
  mismatches):
  - pr_radius's loop: the counter and the pointer in the GTE's two
    spacing slots, MAXU's last in the loop's delay slot;
  - COPYV (a corner, 3 words) with v0 and at in turn: 6 instructions,
    not 9;
  - gs_corner's sides: a corner behind goes straight to the back copy, one
    in front skips the back's tests; each way's first instruction in the
    slots, gs_corner's first in the loop's;
  - the box (pr_bpiece, pr_bcut) with t6 and a0 in turn; cross without
    its three needless nops; the slots of fd_side, gs_side, gs_gen,
    cr_poly, cr_sph_gen, pr_count filled.
  - The cut of the three movers bench: E1M6's train −1.8 %, E1M3's −2.7 %,
    E3M4's door −2.0 % (385 k → 379 k cycles a frame for the train).
- **The entities' asm** (`engine/e_tick.s`, `e_touch.s`, `e_seen.s`,
  `w_leaf.s`, `ent_mat.s`, `mdl_expand.s`, `rl_order.s`), their slots
  filled where an instruction of the way taken fits:
  - e_tick: the word's two loads swapped; `negu`, the edict loop's first
    instruction, in the two slots that lead there; the monster's health
    in the monster test's slot; 70's first load in the slots of the ways
    that end there;
  - e_touch: the slot loop's first instruction in both slots that enter
    it, the node loop's in its own (`touchdiff` 830 → 807 cycles a walk);
  - w_leaf: two of a general plane's four HI/LO spacers do work; e_seen's
    found way and leaf lookup, ent_mat's untransformed way, mdl_expand's
    odd vertex, rl_order's way to `done`: one instruction each.
  - Checked: the physics diffs (0 mismatches), the monsters' and the
    crowd's lines, `ORDER_C=2` (0 mismatches over 12 views of E1M1).
- **pmove** (`engine/pmove.s`): DOT_IN / DOT_II take their last
  instruction as an argument, none where no multiply or divide follows
  within two; friction moves clip_mover's address and two loads into HI/LO
  spacers; accelerate, categorize, push_out, isqrt32's loop and
  slide_move's bump put the way taken's first instruction in their slots.
  pmdiff 0 mismatches; a tick without traces 2455 → 2411 cycles.
- **The point hull's trace** (`engine/clip.s`, shots and lines of sight):
  LEAF's slot does NODE_DSDD's `move` (t0, set again on a leaf's way), the
  loop entering NODE_DSDD past it: a line of sight 7935 → 7850 cycles.
  The slot first held the next node's plane `lhu`: on a leaf the pointer
  is odd (its bit 0 tags it), so that load is misaligned, an address
  error on the console. The dynarec does not check alignment; the
  interpreter stopped there (`LoadAddressError` in clipdiff's traces).
  Every load this pass put in a delay slot or above a branch was then
  checked for the way that does not need it: each reads the scratchpad,
  the stack, the edict, a frame of its own, or an aligned array's next
  entry (a valid read past its end at most).
- **Measured and left out** (each loses at T = 32, where the game runs,
  or costs RAM: the RAM comes first):
  - the split test by the edge's length in one pass (no REFINE; gp and sp
    as temporaries): −1.06 % at T = 4, +0.19 % at T = 32;
  - REFINE with gp and sp, kept on slow_poly's entry: −0.28 % at T = 4,
    +0.04 % at T = 32;
  - MIDLOOK's found test's delay slot: no instruction to put there that
    the other path does not pay back;
  - the near check after a careful transform (NZ: the record loops' and
    the sorted models') two SZ a turn, an odd count's first alone (4.5
    instructions a vertex, not 6): the 128 views −9.8 k cycles at T = 4
    and at T = 32, for 360 bytes of code (its ten copies);
  - the flat transforms (XFORM light 0 and 2, nothing to light while the
    GTE works) making the next three vertices' pool addresses in the
    RTPT's shadow: the 128 views −22 k at T = 32, −8.6 k at T = 4, for
    216 bytes (a prologue in each of its six copies).

### 81.4 The interrupts' handler, again

- **The pad's middle bytes by a short way** (`engine/irq.s` q_irq): when
  SIO0 is the only source pending and the poll is in its middle bytes
  (not the id byte, not the last), five registers are kept, the answer
  stored, the next byte sent and the handler left, as `poll_isr` does
  (the poll's state is `pad_poll` in `engine/pad.c`, at irq.s's PP_*
  offsets). 7 of an analog pad's 9 bytes go that way. Interrupts: 3 800 →
  3 130 cycles a frame (`IRQTIME=1`, START's one-vblank views).
- **EPC kept on the way in**: a callback that made a syscall
  (EnterCriticalSection...) would take over EPC, k0 and k1 in the
  kernel's handler, so q_full stores EPC with the registers and q_ret sets
  k1 again.
- **The rest**: Cause is read in the vector's delay slot (the jump has its
  target); the return's GTE test runs before the registers come back, its
  load delay filled; the dispatch's handler-table address loads in a load
  delay, the pass starts again in the callback's return's slot.
- **A note on the interpreter's logs**: with 12 interrupts a frame,
  PCSX-Redux's interpreter now and then lands one on the BIOS's `putchar`
  call. Its console then shows that character twice (`bbench`, a hash
  with a doubled digit). The frames themselves are identical; a diff of
  the dumps must drop the doubled character first.

### 81.5 The instruction cache

- **The cache** (psx-spx): 4 KB, direct mapped, 256 lines of 16 bytes; a
  miss fills its line from the missed word to the line's end. The data
  have none (the scratchpad stands for it). PCSX-Redux does not model it:
  no bench count ever showed it. DuckStation counts a cycle a word filled
  in its interpreter and with its recompiler's "ICache" option (off by
  default); the machine pays at least that, RAM's latency on top.
- **Measured** (`scripts/icache.sh trace`): PCSX-Redux's interpreter with
  its debugger, a Lua breakpoint over all of RAM records every fetch of
  the bench's frames (from prof_frame_reset to vid_flip, run-length
  coded); tools/icache simulates the cache over them. The traces: START's
  12 views, the 128 spawn views (2 frames each), the game bench (the
  weapons, the HUDs, the walk). As linked until now, 17-20 % of the
  fetched words came from a fill: 44.5 k words a frame on START's views,
  25-30 k on the 128 views, 94 k in the game. The renderer's hot loops
  sat on the same lines by chance: sp_o_bounds (the slow path's off-screen
  test), dro3_xf (the release's transform) and the walk's node test on
  lines 0x5f0-0x707; dro3_quad, sp_refine and walk_leaf on 0x754-0x8c7.
  They evicted each other polygon by polygon.
- **Done**:
  - r_world.s and r_slow.s cut into chains (ICSEC, r_defs.inc): runs of
    code only a jump enters, each ending with an unconditional jump or
    return and its slot, each in a section of its own. scripts/iccheck.sh
    fails the build when one falls through; tools/hazcheck's straight-line
    rules stop at such a jump's slot (the next chain is not what runs
    next).
  - The link's own script, engine/q1psx.ld: the SDK's exe.ld with
    engine/icache.ld's sections first in .text (from 0x80010000, a 4 KB
    boundary: the hot code's lines no longer move with the code before
    it), then the renderer's other chains (within a branch's reach: a
    branch spans 128 KB), then the rest in the old order. The link also
    writes q1psx.ldmap.
  - The patch sites of the far tests and the frustum (render.c
    r_set_res) were offsets from r_render, which the chains no longer keep:
    each pair is a word now (the first's address, the second's distance in
    bits 21-30, unused by a RAM address), the tables' size unchanged.
  - engine/icache.ld from tools/icache's search: 100 sections, 34.2 KB
    (those with more than 0.01 % of the fetches that the bench's and the
    release's links hold alike: the renderer's chains, the slow path, the
    interrupt handler, the entities' and the models' asm, memset, the
    render list's and the stream's C), ordered by a hill climb on the
    simulated fills (START's views, half of the 128 views, the game twice;
    seeds in parallel, then more from the best). A section the bench's link
    lacks or holds at another size stays out (`-m`: dl_restore_asm,
    touch_gather, the C that the bench builds otherwise): the hot code lies
    at the same addresses in both links, so the bench measures what the
    release runs.
- **Result** (the release's link, the same traces):

  | Frames | Words filled before | After | |
  |---|---|---|---|
  | START's 12 views | 533 646 | 263 745 | −50.6 % |
  | 64 of the 128 views (searched on) | 1 890 356 | 992 659 | −47.5 % |
  | the other 64 (not searched on) | 1 601 884 | 921 296 | −42.5 % |
  | the game bench, 12 frames | 1 128 089 | 574 604 | −49.1 % |

  New traces of the bench's link with this order agree: 267 511 words on
  START's views, 574 047 on the game's. A frame of START's views fills
  ~22 k words instead of 44.5 k, one of the 128 views 15 k instead of
  27.3 k, one of the game 47.9 k instead of 94 k: in DuckStation's count
  22.5 k, 12.3 k and 46 k cycles a frame (4.0 %, 2.2 % and 8.1 % of a
  vblank); more on the machine.
- **Cost**: no instruction changes, so every display list stays the same
  and PCSX-Redux's counts barely move (+0.003 %: its dynarec's blocks fall
  elsewhere); 40 bytes of code (the pairs' decoding in
  r_set_viewport). The order holds while the hot code keeps its size: an
  edit in it shifts what follows, `scripts/icache.sh` makes the order again
  (trace the three sets, `opt`, link).
- **Left**: a check on DuckStation (the interpreter, or the
  recompiler with "Enable Recompiler ICache") and on the machine.

### 81.6 The rest of the list, item by item

The list's items, each measured on this build or looked up in its code;
§81.1-81.5 cover the interrupts, the delay slots and the instruction cache.

- **The frame's budget**: an NTSC frame is 263 lines of 3413 video cycles,
  about 566 000 CPU cycles (33.8688 MHz); the bench counts each frame's
  vblanks (`vbl=100`: one) and its cycles. 580 000 is not a figure this
  project uses.
- **Load and branch delay slots** (the list's "up to 10 % nops"): the asm
  objects of the release hold 493 nops in 13 774 instructions on master
  (3.6 %), 373 in 13 989 now (2.7 %); the whole program 7.2 % (the C's, as
  GCC schedules it). Executed (the traces of §81.5): 0.8 % of the
  instructions of START's views, 0.9 % of the game's.
- **The scratchpad** (1 KB): full (r_defs.inc's SP_* map, from 0x000 to
  0x3ff): the walk's stack, a record's vertices' SXY / SZ / colour, the
  loops' state and constants; the physics borrows its first part. The
  largest reader of RAM left is the slow path's vertex pool (sp_o_bounds'
  `lh`, 16 KB of entries): it does not fit. PCSX-Redux charges RAM and
  scratchpad alike; on the machine a RAM read waits (DuckStation counts
  RAM_READ_TICKS, 6) where the scratchpad's does not.
- **The GTE** (§3): the commands' costs (psx-spx: RTPS 15, RTPT 23, NCLIP
  8, MVMVA 8, AVSZ3 5, SQR 5; the bench's emulator charges the first three
  the same, §21's table); a register written two instructions before the
  command that reads it
  (tools/hazcheck checks every link); each result read was ranked by its
  wait in the profile (a scratchpad tool). The longest left is the flat
  transforms' `swc2` after their RTPT: filling it costs 216 bytes (§81.3,
  left out).
- **RAM accesses and their width** (§4.1, §4.3): loads and stores are 26 %
  of the executed instructions on START's views, 29 % in the game. The
  hot halfword loads are the scratchpad's (the SXY halves: no wait on the
  machine) and the slow path pool's; a word and shifts in their place
  cost PCSX-Redux 2 cycles more a vertex (`lh` 3, `lw` 2 and two ALU
  instructions of 2): left out (§34.1's 4-`lbu` case, the same verdict).
- **DMA** (§4.2): already wherever there is a channel: the display lists
  (GPU, linked list), the ordering table's clear (OTC, the world's 8
  entries), the CD's sectors, the SPU's and VRAM's uploads. RAM to RAM
  has no channel: the stream's copies stay the CPU's. The CPU waits on a
  burst: the OT's clear is 8 words; the GPU's linked-list transfer runs
  while the CPU builds the next frame (two buffers, vid.c).
- **The ordering table** (§5): the world's polygons go to their buckets in
  the BSP's order, one prepend each, no list walked; the models through
  a 16-node depth ring (§63); the table and the packet arena are double
  buffered (the GPU draws one frame while the CPU builds the next).
- **Interrupts off** (§6.3): only at boot (the SDK's ResetGraph and CdInit,
  irq.c's ResetCallback) and in two short Fast critical sections (COP0's
  SR: the pad driver's stop, the CD's interrupt repair); none in a frame.
- **Self-modifying code** (§7.1): in use. The graphics options patch the
  record loops' entries and thresholds (render.c r_set_options), the
  internal resolution the far tests, the frustum's planes and the slow
  path's projection (r_set_res), the draw distance mark_pvs's call; each
  then flushes the instruction cache. The memory recovery the list cites
  (code that only boots, its RAM taken back): the release's boot-only
  functions come to ~4.6-5.8 KB by the static call graph (tail calls and
  function pointers make it a lower bound to check by hand). The buffers
  that could lie over them are touched during the boot itself (projs,
  particles: game_init clears them) or are bigger than the code (r_ents
  10 KB, r_items 6.4 KB); an overlay file loaded at boot above the BSS
  would do it. Not done: a caller missed is a crash on a rare way.
- **Asm for the critical code** (§7.2): the renderer, the physics, the
  entities' hot loops (§55: the C engine's frames +88 to +94 %), the
  interrupts (§81.1); memcpy / memset are the SDK's.
- **The open points** (§10): the budget (above), the GTE's costs (the
  emulator's, the waits measured), the CPU during DMA (above), the BIOS
  handler's cost (~2.7 k cycles an interrupt on PCSX-Redux, gone: §81.1),
  self-modifying code (used, above).

### 81.7 The stack's room and the level arena (2026-10-24)

- **Before**: the level arena was a BSS array of 1243 KB, and the stack
  had what was left between the BSS's end and the top of RAM: 89.7 KB in
  the release. Nobody sized it: the RAM each pass freed (§60, §77, this
  branch) fell into that gap. The deepest stack measured: 2.0-2.5 KB on
  every bench (the 32 maps' loads and views, the game, the monsters, E1M3's
  crowd fight and key run, the movers), 4.3 KB in §61 on older code. The
  explosive boxes' chain (Quake's barrel_explode within T_RadiusDamage,
  kept: a box's blast kills its neighbour at once) goes 3 deep at most in
  the single-player maps (e3m1).
- **Now** (`engine/mem.c` mem_init, 12 KB chosen):
  - the stack keeps the top `MEM_STACK_BYTES` (12 KB, CMake
    `Q1PSX_STACK_BYTES`) under the kernel's stack top (0x801FFFF0);
  - the level arena is every byte from the BSS's end to there, zeroed at
    boot as the BSS was: whatever the program frees from now on goes to
    it by itself;
  - the stack's room is painted at boot; its lowest 1 KB is a guard
    (`MEM_STACK_GUARD`): vid_flip checks a sixteenth of it a frame (all of
    it in 16 frames, 16 words a frame), and a guard word written stops the
    game on a red screen (vid_fatal: the GPU's fill, the interrupts off)
    rather than let a deeper stack overwrite the arena unseen;
  - scripts/ramcheck.sh fails the link when the arena comes under 1243 KB
    (`Q1PSX_LEVEL_MIN`, its size until now); bench.sh's STACK_MIN gives way
    to STACK_BYTES, LEVEL_MIN and LEVEL_CAP (a bench's code may leave its
    arena more than the release's: LEVEL_CAP=<the release's> for pools as
    the game has them).
- **Result**: the release's level arena 1243 → 1318 KB (+75 KB), so every
  map's stream pool +75 KB: E4M7 237 → 312 KB, E3M6 270 → 345, E3M4 273
  → 348, E3M5 283 → 358, E2M4 313 → 388. The release's image (code, data,
  BSS) ends at 0x800B36B0.
- **Checked**: with the arena capped at its old 1243 KB, the six reference
  sets identical (the mechanism alone); at full size, five identical and
  the movers' E3M4 frames different: its start preloads 38 assets (311
  KB) instead of 36 (267 KB), as §77.3 saw with a pool cut the other way
  (the same packet counts, the skins placed elsewhere in VRAM); the
  movers' reference saved again. The default bench under the
  interpreter, the release's boot and menu loop (240 frames) under the
  interpreter; a bench built with a 2 KB stack stops at its first frame on
  the guard (`vid: the stack reached its guard`).

## §82 — The zones cut finer: the sets on a halfword, four slots (2026-10-24)

A branch of its own. The request: the levels and their BSP
are not split enough; with more monsters' models to stream (their levels of
detail, §81's POC on its branch), the stream pool must grow, and the chunks'
cut should be more aggressive, the adjacent chunks being loaded anyway.

- **What the cut is today** (§57):
  - a zone's chunk holds every record the PSX may draw from any of its
    leaves within 1536 units, so the chunks overlap (E1M1: 9761 records
    in its chunks for the world's 4390);
  - only the camera's zone is drawn. The two nearest are read ahead, not
    drawn with it;
  - the zones are merged while their chunk fits the cap (96 KB × 0.94),
    and a slot is the map's biggest chunk;
  - the default build has no tables (`local.env`), so every map is
    whole. E4M7's 312 KB pool is its whole world's (414 KB with the
    tables, 345 the smallest then, E3M6 whole).
- **The floor of a finer cut:** a chunk can be no smaller than what one
  camera leaf sees. Exported with smaller caps (the analysis outside the
  repo: `analysis/export.py`, `ZSETS=pvs:64,seen:48,seen:32`; the
  export's estimates, over the 32 maps):

  | Tables | Zones a map | The biggest chunk (the slot) |
  |---|---|---|
  | pvs, cap 96 (today) | 1-25 | 78-149 KB |
  | pvs, cap 64 | 3-148 | 58-106 KB |
  | seen, cap 64 | 2-25 | 56-65 KB |
  | seen, cap 48 | 7-75 | 44-65 KB |
  | seen, cap 32 | 24-286 | 31-65 KB |

  "seen" keeps only the records the analysis' 2048 px renders saw from
  the camera places sampled (§56, §57.8). Below 48 KB the slots no longer
  shrink, and the zones multiply.
- **§57's tables** (pvs 96 and seen 64, exported 2026-10-22) count the
  brush models' leaves one by one, as the worlds had them before §58. The
  cook takes them, the world's own leaves being the same. Exported again
  from today's worlds, they cook to the same bytes (the skeletons and
  chunks of the 20 and 29 maps they zone); only the zones' byte counts
  moved, which the cook does not read.
- **The halfword sets** (the format `QWZG`, was `QWZF`):
  - a leaf's zone set is a `uint16_t`, no longer a byte; the finer
    tables make more than 255 sets (E2M5: the cook stopped there);
  - `QZ_LEAF_NONE` (0xffff) marks the camera in solid;
  - engine/zone.s `zone_mark` reads a halfword at twice the index (one
    instruction more, only when the camera's leaf changes);
  - the skeleton +1 KB a map, and no other change with today's tables
    (the 32 maps' pools the same).
- **Four or five slots** (`Q1PSX_ZONE_SLOTS=4` or `5`, cook.sh → `qcook
  zones --resident`):
  - engine/zone.c wants as many zones as it has slots, four at most: the
    camera's and its three nearest (the tables' `near[2]`). With 3 slots,
    nothing changes;
  - a fifth slot (`MAX_RES` 5) only keeps the last chunk left;
  - the command line's `Q1PSX_ZONE_SLOTS` wins over `local.env`'s, as the
    tables' does.
- **The build follows the cook's settings:** CMake cooked again only when
  the cooker or a format changed, so other tables or slots in `local.env`
  built nothing new.
  - A cook writes what its output depends on to `cooked/settings.txt`: the
    maps, the paks, the light gain, the zone tables (their path, a hash of
    their contents) and the slots, the music's sources, and qcook's
    `QCOOK_NOHAND`, `QCOOK_HAND` and `QCOOK_NOFMAP`.
  - Every build first runs `scripts/cook.sh --stale <build>/cook.settings`
    (17 ms). When the settings differ from the last cook's, it rewrites
    that file, prints the lines that changed, and the cook runs again.
  - The build directories share `cooked/`: one that finds another's cook
    there, made with other settings, cooks again too.
- **Measured** (32 maps, the arena capped as the release's, 1318 KB;
  `BENCH_MAPS=1 VIEWS=0 RESIDENCY=1`):

  | Tables | Maps zoned | The smallest pools (KB) | E4M7 |
  |---|---|---|---|
  | none (the default build) | 0 | E4M7 312 | 312 |
  | pvs 96, 3 slots (today's tables) | 20 | E3M6 345, E3M4 354, E4M7 414 | 414 |
  | pvs 64, 3 slots | 27 | E3M6 409, E3M4 474, E4M7 480 | 480 |
  | seen 64, 3 slots | 29 | E3M4 504, E4M7 510, E3M6 543 | 510 |
  | seen 64, 4 slots | 27 | E3M4 440, E4M7 448, E3M6 477 | 448 |
  | seen 48, 3 slots | 30 | E3M6 543, E3M4 546, E4M7 552 | 552 |
  | seen 48, 4 slots | 27 | E3M6 477, E3M4 496, E4M7 504 | 504 |
  | seen 48, 5 slots | 26 | E3M6 411, E3M4 446, E4M7 456 | 456 |

  - With seen 48 and 3 slots every map but E1M7 and END is zoned (their
    pools 926 KB and more), and the pools grow 106-198 KB. With 4 slots
    E1M6, E1M8 and E3M2 stay whole too (pools of 600-795 KB), with 5 one
    more map.
  - With seen 64 and 3 slots E3M2 stays whole too (694 KB), with 4 slots
    E1M6 and E1M8 as well. Its smallest pools and E4M7's are 37-56 KB
    under seen 48's with as many slots.
  - In every run the spots where a visible monster lacks its model are the
    same two, E1M7's (Chthon, out of reach).
- **The frames** (`BENCH_MAPS=1 HASHONLY=1`, the 128 spawn views, against
  today's tables):
  - seen 48: 27 display lists differ, the records the renders never saw
    left out: −1.3 % cycles, E1M8.v3 from 2 vblanks to 1;
  - seen 64: 22 differ, −1.0 % cycles, E1M8.v3 from 2 vblanks to 1 too;
  - the pictures (pktview), E1M8's, START's, E3M6's and E1M6's 48 views:
    identical pixel for pixel with either (10 of seen 64's 22 differing
    lists among them). The records left out were drawn and painted over;
  - pvs 64: the 128 lists identical to today's, as by construction
    (−0.3 % cycles).
- **The zones' walk** (`ZONES=1`, at running speed):

  | Tables | Maps | Switches | Reads | Late frames | Of them on clear legs | The longest run |
  |---|---|---|---|---|---|---|
  | pvs 96, 3 slots (today's) | the 20 they zone | 360 | 764 | 2838 | 128 | 44 |
  | pvs 64, 3 slots | the same 20 | 681 | 1491 | 6960 | 541 | 58 |
  | seen 64, 3 slots | the same 20 | 477 | 952 | 3015 | 156 | 33 |
  | seen 64, 4 slots | the same 20 | 485 | 1001 | 2300 | 110 | 32 |
  | seen 48, 3 slots | the same 20 | 696 | 1422 | 4748 | 335 | 39 |
  | seen 48, 4 slots | the same 20 | 702 | 1576 | 3943 | 317 | 54 |
  | seen 48, 5 slots | the same 20 | 705 | 1478 | 3733 | 327 | 54 |
  | pvs 64, 3 slots | the 27 they zone | 898 | 1916 | 8629 | 779 | 58 |
  | seen 64, 3 slots | the 29 they zone | 702 | 1350 | 4284 | 411 | 36 |
  | seen 64, 4 slots | the 27 they zone | 629 | 1225 | 2766 | 174 | 32 |
  | seen 48, 3 slots | the 30 they zone | 1037 | 2077 | 7370 | 828 | 45 |
  | seen 48, 3 slots | the 27 zoned with 4 | 908 | 1789 | 5954 | 471 | 39 |
  | seen 48, 4 slots | the same 27 | 917 | 1981 | 4814 | 391 | 54 |
  | seen 48, 3 slots | the 26 zoned with 5 | 892 | 1764 | 5885 | 471 | 39 |
  | seen 48, 4 slots | the same 26 | 901 | 1942 | 4748 | 391 | 54 |
  | seen 48, 5 slots | the same 26 | 904 | 1803 | 4425 | 397 | 54 |

  - Smaller zones mean twice the switches and reads, and more frames drawn
    with the last zone while the camera's is read (E1M8, 75 zones with
    seen 48: 989, the longest run 42 frames). The fourth slot takes a fifth
    of them back with seen 48, a quarter with seen 64.
  - seen 64 has two thirds of seen 48's switches and reads on the 20 maps
    (fewer zones, each bigger): its late frames stay near today's with 3
    slots (3015 against 2838, 156 against 128 on clear legs) and go under
    them with 4 (2300 and 110), its longest runs the shortest (32-33
    frames).
  - The fifth slot saves some reads when the eye comes back (−7 % late
    frames) but none on the clear legs, for 45-66 KB of every map's pool:
    four is the better trade.
  - pvs 64 is the worst of the three: its zones are small (E1M1 145, E3M6
    129) and each still carries a big chunk (what one leaf's PVS holds, up
    to 106 KB), so its reads are many and long. Its longest late run is 58
    frames (E1M1).
  - Interleaving the disc (each level's chunks next to each other, the
    streaming redesign) would shorten every read.
- **To take it:** `Q1PSX_ZONE_TABLES=<the analysis>/zones/seen_1536_64` (or
  `seen_1536_48`) and `Q1PSX_ZONE_SLOTS=3` or `4` in `local.env`:
  - seen 64, 3 slots: the smallest pool +159 KB (E4M7 +96), the walk's
    late frames near today's;
  - seen 64, 4 slots: +95 KB (E4M7 +34), fewer late frames than today's;
  - seen 48, 3 slots: +198 KB (E4M7 +138), 1.7 times today's late frames.

  The tables are derived from Quake's data and stay outside the repo.
- **The cut chosen** (2026-10-24, after playing it): seen 64
  with 4 slots, the compromise the streaming redesign starts from.
- **Left, the bigger share:** the skeleton, 241-335 KB, always resident.
  E4M7's: nodes 126 KB, clip hulls 74, PVS 41, leaves 39, marks 14. The
  nodes also serve the entities' links and the leaf lookups, the hulls
  every monster's moves, and the PVS the AI too. Splitting them by zone
  means deciding what the game does where no chunk is loaded.
- **Also left:** drawing the camera's chunk with its neighbours' (chunks
  without the duplicated records) needs the renderer to walk more than one
  chunk's blocks.
- **Left for later, the loads' triggers** (2026-10-24, seen 48
  with 4 slots in play): on START, going back and forth between the
  teleporter to E1 and the one to E3 shows zones not loaded yet on the way.
  - START has 35 zones with seen 48, 7 with today's tables. The walk had
    0 late frames today, 124 with 4 slots (27 on clear legs).
  - The prefetch ranks zones by walking distance from the camera's leaf,
    starting when that leaf's set changes, not by where the player
    heads.

## §83 — The streaming redesign, its first milestone: the drive's journal, a real drive's seeks, the layouts replayed (2026-10-24)

A branch of its own (on master c11c97b
with §82's cut: the seen tables at 64 KB, four slots). The redesign
wanted: one section of the disc per level, holding the level and every
resource it uses (its chunks, its streamed models and sounds, its SPU
music), duplicated into the section when the image is built and
interleaved with the level's data, so that the head is always near the
next read; the goal is the fewest seeks, and the SPU music's ring back from
2 s to 1 s, the SPU RAM freed for the level's sounds. The music tracks 4 to
11 duplicated once a map that plays them add 232 MB (tracks 2 and 3,
the title's and the intermission's, stay once). The disc's format and the
memory's architecture being structural, this first step measures before
it builds: a bench tool whose spec was validated, the measure of
today's disc with it, and the layouts' options for a decision.

### 83.1 Why a new bench

§82's walk (`ZONES=1 scripts/bench.sh`) made 1225 chunk reads over the 27
zoned maps, 47.5 KB each, in 8.8 frames a read on PCSX-Redux: the transfer
alone at double speed is 9.6 frames. PCSX-Redux (src/core/cdrom.cc
`computeSeekDelay`) charges a seek 3 µs a sector of distance plus 1 ms: 1 to
180 ms over the whole disc, and the bench's disc carried no music. So that
bench could not show what the redesign saves: the seeks.

What a console's drive spends on a seek, DuckStation's model (src/core/
cdrom.cpp `GetTicksForSeek`, from measures of the mechanism):

| the hop | DuckStation | PCSX-Redux |
|---|---|---|
| ahead within a track (8 to 22 sectors) | the sectors' time (13-147 ms) | 1 ms |
| under 60 to 330 sectors (by the place on the disc) | 50 ms | 1-2 ms |
| under 7200 sectors (14.7 MB) | 100 ms | up to 22 ms |
| farther, the sled | 50 ms + 340 ms × ln d / ln 324000 + 510 ms × d / 324000: 200 ms at 7200, 300 ms at 8000, 400 ms at 42000, 900 ms across a 74-minute disc | 25-180 ms |

### 83.2 Today's disc

The release disc (150 MB): `\MAPS` at sectors 9531-29505 (each map's SND,
ZON, WLD, MDL, TX4 in a row), `STREAM_0.DAT` at 29505 (1138 sectors,
2.3 MB), `\MUSIC` at 30644-75918 (tracks 2 to 11, 1801-6381 sectors each).
In play the head goes between three places:

| the hop | sectors | a console (DuckStation's model) |
|---|---|---|
| a map's ZON ↔ STREAM_0.DAT | 500 (START) to 19 800 (E1M1) | 100 ms under 7200, else 300-340 ms |
| a map's ZON ↔ its SPU track | 8900 to 59 500 | 310-440 ms |
| STREAM_0.DAT ↔ a SPU track | 8500 to 42 500 | 310-400 ms |

The music's ring (6 chunks of 0.325 s a channel) is refilled 2 chunks at a
time, 8 sectors (53 ms of transfer) every 0.65 s; on a console each refill
from the data's place costs a sled seek there and another back: about
0.7 s of the drive for 53 ms of music. A chunk read (24 sectors, 160 ms)
costs the same two seeks around it.

### 83.3 The drive's journal (engine/cd.c)

Every read of the disc now starts through one function,
`cd_read_start(lba, sectors, dst, who, tag)` (the seek, then
`CdReadRetry`), with who asks it (`CDW_DIR` a directory or a header,
`CDW_FILE` a whole file, `CDW_ZONE` a world chunk, `CDW_MODEL` a streamed
model, `CDW_SOUND` a streamed sound's piece, `CDW_MUSIC` the music's
refill) and for what (the zone, the asset, the music's chunk);
`cd_read_now` takes the same, and `cd_read_cut` replaces the raw
`CdReadBreak` of a read broken off. The release builds ignore the two
arguments. In the bench build (`CDLOG=1 scripts/bench.sh`, CMake
`Q1PSX_BENCH_CDLOG`):

- a `cd` line per read, when it ends: `cd <who> <tag> <lba> <sectors>
  <from> <start> <end> <seek_us> <held> <status>`: the head's place before
  it (the last read's end), the vblanks it was started and ended at, the
  seek the model below gives it, the polls the model held it, and `.`,
  `e` (an error) or `x` (cut);
- `cd_log_mark(name)` names the phase that begins (`load_level`: the map;
  the zones' walk: `walk`; the bench's end: `end`) and prints a `cd sum`
  line totalling the phase before it: the reads and sectors by who, the
  vblanks, the modeled seeks, the transfer, the polls held, the hops by
  class (near: under 330 sectors; mid: under 7200; sled) and the longest.

The zones' walk now ticks the music every frame (`music_tick`, as the
game's frame does): until now the bench's walk never refilled the ring,
so a bench disc with the music played the ring's first two seconds round
and round, and the drive never saw the refills.

### 83.4 The drive's model (`CDMODEL=1`, CMake `Q1PSX_BENCH_CDMODEL`)

DuckStation's seek model ported to the engine's bench build (integer
microseconds: a 16-step table for log2, the sectors-a-track table by
minute, the sled's logarithm and linear parts; checked on the host against
the float model over 1124 pairs of places: within 91 µs). When a read's
real end comes (PCSX-Redux's), `cd_poll` answers "still reading" and
`cd_ready` "busy" until the modeled drive would be done: the seek from the
head's place plus 28224 ticks of root counter 2 a sector (6.667 ms at
double speed), counted from the seek command. `cd_read_sync` waits
likewise. So the zones' late frames, the stream's reads and the music's
underruns come out as on a console's drive, in fast forward.

What the model leaves out: the pause after each read is PCSX-Redux's (the
real one is in the same range); the engine's own gaps are real either way
(two vblanks of settle before each command, engine/cd.h; the reads polled
once a frame). PCSX-Redux's seek lies inside the modeled one, never past
it, so nothing is counted twice.

Two knobs for the music, in every build (CMake `Q1PSX_MUSIC_RING`,
`Q1PSX_MUSIC_SEEK_MS`; `MUSIC_RING=` and `MUSIC_SEEK=` in bench.sh): the
ring's chunks a channel (6 = 2 s, 3 = 1 s) and the seek `music_read_ok`
plans around a read (250 ms: three of them, before a read may start).

### 83.5 The replay on another layout (tools/cdsim/cdsim.py)

The host tool (Python, the repo's first; the analysis' scripts outside the
repo are Python too) reads a journal and the image it was recorded on
(its ISO9660 directories give every file's sectors), and charges each
read a seek by a model (`--model duck`, the default, or `redux`) and its
transfer, per phase and in all (the loads, the walks). With `--layout`
(another image, or a table of `<lba> <sectors> <bytes> <path>` lines;
`--table` writes one) every read is first moved to where its file is on
that layout: a read is a (file, offset) through the recording disc, and
lands on the nearest copy of the file that holds that offset — a path may
appear several times (a resource duplicated), as parts (`<path>@<sector>`:
the file from that sector on, a track cut around a level's data) or
condensed (`<path>~`: a level's subset of the streamed assets, a read
landing at its offset scaled to the copy's size). `--no-dir` leaves the
directory and header reads out (a layout whose levels carry their own
sector tables reads none); `--check` compares the engine's seek figures
with the tool's.

### 83.6 Measured: today's disc

`CDLOG=1 ZONES=1 MUSIC=1 scripts/bench.sh 1500` on the bench disc (the 32
maps, the music; PCSX-Redux in fast forward, the drive PCSX-Redux's):

- **The walks** (27 zoned maps, 2242 s of walking): 11 653 reads. 1134
  chunks (24 sectors each, 11.5 frames a read), 7050 pieces of streamed
  sounds (1 to 8 sectors), 43 streamed models, 3426 music refills (8
  sectors, one every 39 vblanks: 0.65 s). Late frames 3845 (§82 measured
  2766 on the same maps with a disc without the music: the refills now take
  the drive's turns).
- **The sounds are read ten times over.** A walk reads 33 to 67 distinct
  sounds in 380 to 580 pieces: the SPU room the streamed sounds get is
  97-133 KB (512 KB less the capture buffers, the map's bank of 327-367 KB
  and the music's 48 KB ring), the eye runs through the map at 320 units a
  second, and the sounds within reach change faster than the room holds
  them. Each piece is its own seek.
- **The engine's gaps:** two to three vblanks between a read's end and the
  next one's start (the settle before a command, the reads polled once a
  frame), 4.2 on average over the walks' 11 653 reads: 70 ms of idle drive
  a read.
- **The loads** (33, START twice): 54 reads a load on average (7 directory
  reads of 27 sectors, 4 files, 4 chunks, 20-26 models, 17-20 sound
  pieces, the ring's 3 fills), 10.7 s each on PCSX-Redux.

The same journal charged a console drive's seeks (tools/cdsim, DuckStation's
model):

| | reads | seeks | of the time | the music's | transfer | hops near / mid / sled |
|---|---|---|---|---|---|---|
| the walks | 11 653 | 2793 s | **125 %** | 1212 s | 549 s (24.5 %) | 3805 / 980 / 6868 |
| the loads | 1786 | 194 s (5.9 s a load) | 55 % | 10 s | 165 s (5 s a load) | 1184 / 312 / 290 |

Today's disc cannot be played as the walk plays it on a console: the seeks
alone would take longer than the walk (every music refill a sled seek
there and one back, 0.35 s each). A map's load would take about 16 s
(PCSX-Redux's 10.7 s less its seeks, plus 5.9 s of a console's). What a
console does instead, the model in the engine shows below.

### 83.7 The layouts replayed (tools/cdsim)

Candidate layouts written as tables from the bench disc's files (a
generator in the session's scratch; the real one is the redesign's build
step): one section a map, holding the map's five files, its streamed
assets (the subset the walk and the load read, 167-530 sectors, as a
condensed copy of STREAM_0.DAT) and its track whole (`sections`), or with
the map's data in the middle of the track (`middle`); the root's files
first, tracks 2 and 3 once at the end; the directory reads left out
(`--no-dir`: a level carries its own table of sectors). The same journal
on each:

| layout | the walks' seeks | of the time | the music's | hops near / mid / sled | the loads' seeks |
|---|---|---|---|---|---|
| today (the directory reads left out too) | 2793 s | 125 % | 1212 s | 3805 / 980 / 6868 | 124 s |
| sections | 983 s | 44 % | 339 s | 5518 / 6135 / 0 | 105 s |
| middle | 988 s | 44 % | 342 s | 4760 / 6890 / 3 | 106 s |

- The sections take every sled seek away: each music refill costs 100 ms
  (a hop within the section, over 330 sectors) instead of 350, each
  chunk read likewise, the sounds' pieces 50 or 100 ms. The disc: 367 MB
  of sections (3205 to 7504 sectors each), 14 MB of tracks 2 and 3, 20 MB
  of root files (PAK0 included): 403 MB.
- Four sections pass 7200 sectors (E2M6, E2M7, E3M6, E4M7: tracks 4 and 7
  with the biggest data), where a hop from one end to the other would be a
  200 ms sled seek; the data in the middle of the track keeps every hop
  under it (3 of 11 653 sled in the replay, none that matter).
- A carousel (the data repeated along the track) does not fit the disc at
  double speed: every 60 s of music, 881 MB with half the stream data;
  every 120 s, 631 MB; and a hop to the next copy is still over 330 sectors
  (100 ms), the same as the section's.
- What the layout cannot take away is the floor: 50 to 100 ms a read, and
  11 653 reads a walk set: 983 s. Of them the music's refills are 3426
  (339 s), the sounds' pieces 7050 (about 590 s), the chunks 1134 (about
  60 s).

### 83.8 What the measures say for the design

The seeks of today's disc are the sled ones (the music 40 000 sectors
away, STREAM_0.DAT 20 000): the sections remove them and divide the walks'
seek time by 2.8. The rest is the count of reads times the drive's floor,
and three things decide it:

1. **The music's refills.** 8 sectors every 0.65 s is 92 seeks a minute
   (and 92 back). Refilling 4 chunks at a time (16 sectors, every 1.3 s,
   two chunks of margin) halves them; a 1 s ring (3 chunks) refilling a
   chunk at a time doubles them (184 a minute, each 100 ms there and 50-100
   back): the 1 s ring needs the refills to be near-free, which only
   reading them in the same sweep as the data would make them.
2. **The sounds' pieces.** 7050 of the walks' 11 653 reads; the same sound
   read ten times over. The room (97-133 KB, +24 KB with a 1 s ring), the
   eviction (by distance, the nearest kept) and the piece (a sound at a
   time, 1 to 8 sectors) are the engine's; a zone's sounds read with its
   chunk (one read, the sounds laid next to the chunk) would be one seek a
   zone.
3. **The loads.** 47 reads a load, in a known order (the bank, the
   skeleton, the textures, the models, the chunks' header, the spawn's
   chunks, the preload's assets by distance, the ring's fill): laid out in
   that order, a load is one sweep (the seeks' 5.9 s to about 0.5 s).

**The same walk under the drive model** (`CDMODEL=1`, the music's 2 s
ring):

| | today's drive (PCSX-Redux) | a console's (the model) |
|---|---|---|
| chunk reads, frames each | 1134, 11.5 | 909, 23.9 |
| late events, late frames | 271, 3845 | 445, **10 156** |
| streamed sounds' pieces read | 7050 | **1350** |
| streamed models read | 43 | 5 |
| music refills, underruns | 3426, 0 | 3230, 0 |
| seeks (modeled), the drive busy | 2792 s if charged | 1540 s, 84 % |

On a console the music holds (the others wait for its room:
`music_read_ok`), the camera's zone comes late 2.6 times as many frames,
and the sounds mostly never come: the stream reads a fifth of the pieces
it reads on the emulator. That is today's disc played at running speed on
the machine, by the model.

**The 1 s ring under the model** (`MUSIC_RING=3 CDMODEL=1`): the music
refills a chunk at a time (4 sectors every 19.5 vblanks, 5843 refills over
the walks, never dry), and nothing else gets the drive: 4 chunk reads and 3
of the stream's in 27 walks, 35 306 late frames of 97 200. `music_read_ok`
lets a read start only when the ring holds the music for it, its refill
after and three seeks of 250 ms: a 0.975 s ring never does. The 1 s ring
asks for refills that cost next to nothing — on a layout where they do,
with a seek guess to match (`MUSIC_SEEK=`), which is the redesign's.

### 83.9 Left

- **The layout's decision** is left open (§83.8): the sections with the
  data in the middle of the track, the refills' size, the sounds' reads,
  the loads' order.
- **The build step** that writes the sections: each level's files and its
  subset of the streamed assets (the carousel cook of §48 listed a map's
  assets from its entities, `map_needs` in e49202c's tools/qcook/
  carousel.c; it went with §52), the track copied, a per-level table of
  sectors in place of the directory reads, mkpsxiso's file order. The
  engine then takes a level's assets from its section (the stream's
  directory per level, the music's track by its copy).
- **Calibration:** the model is DuckStation's, from measures of the
  mechanism; a DuckStation run of `CDLOG=1 ZONES=1 MUSIC=1` gives
  the emulator's own vblanks per read to set against the `seek_us` figures,
  the machine the final word.
- **The model's reach:** a read waited for synchronously (`cd_read_sync`,
  the loads) is held too but counts no `held` polls; the pause after a read
  is PCSX-Redux's; a read's start is known to the vblank.

## §84 — The disc in sections, one a level (2026-10-24)

The decision on §83.8's options: the sections (A), the music's
refills four chunks at a time (B) and the loads' files in reading order
(D) first; a zone's sounds read with its chunk (C) next. The 1 s ring is
out: under the drive model its refills, one chunk at a time, would take
about 60 % of the drive even in a section; the ring's size (6 chunks, 2 s,
or 4, 1.3 s, +16 KB of SPU RAM for the sounds) is a build knob decided on
the bench.

### 84.1 The layout (scripts/cd_image.cmake, iso/iso.xml)

mkpsxiso lays the files out in the order the project lists them, so the
project is the layout:

- **The root**: SYSTEM.CNF, the executable, GAME_0.DAT, CONBACK.DAT,
  STREAM_0.DAT (the pool of streamed assets: its directory is every
  level's, read at boot; a level without its own file reads it), MENU.SND
  (the release menu's bank, from `\MAPS` before).
- **One directory a level**, `\LVL\E1M1`, its files in the order `load_level`
  reads them, so that a load is one forward sweep of the head and every
  read in play a hop within the section: `E1M1.SND` (the bank, first),
  `E1M1_T2.TX4` (the textures), `E1M1_G0.WLD` (the world, or the
  skeleton of a zoned map), `E1M1_M0.MDL` (its own models), then what the
  game reads in play right before the track: `E1M1_Z2.ZON` (the chunks),
  `E1M1_S0.DAT` (the level's own file of the streamed assets, §84.2), and
  `SPU_06.DAT`, its copy of its CD track (tracks 4 to 11 go with the maps
  that play them: 32 copies, 232 MB more).
- **`\MUSIC`** after the levels: tracks 2 and 3 once (the title's, the
  intermission's); every track when the disc carries no level.
- No `\MAPS` directory any more. The levels' directories sit under `\LVL`:
  a first try put them in the root, whose records then took two sectors;
  ISO9660 sorts a directory's records by name, so SYSTEM.CNF and
  Q1PSX.EXE landed in the second sector, the one the BIOS never reads, and
  the disc did not boot (PCSX-Redux's OpenBIOS stayed in its shell).

The section's hops: a level's data (1.1-1.3 MB with its streamed assets)
then its track (5-12.5 MB): the chunks and the stream file lie right
before the track, so the farthest music chunk is under 7200 sectors from
them (100 ms) on every map, the 50 ms hops within the data.

### 84.2 The cook (tools/qcook/section.c, tools/qcook/zones.c)

- **`qcook section`** writes a map's own file of the streamed assets,
  `<map>_s0.dat`: the pool's directory (STREAM_0.DAT's, the same 174
  entries in the same order), each asset's sector in this file, or
  `QST_ABSENT` for the assets the map's entities never bring; then the
  assets' bytes copied from the pool. Which assets: the lists the XA
  carousels used (§48; `stream_monster_needs`, `items_streamed`,
  `sound_map_streamed` on the entities single player spawns at some
  skill). Their order: a monster group's assets at the distance of its
  nearest monster from the player's start (the gibs and the backpack at
  the nearest monster's), the nearest first; the items and the world's
  sounds after, in the pool's order: the engine's preload reads by
  distance, so it sweeps the file forward. The 32 files: 31 to 106 of the
  174 assets, 0.46-1.2 MB each (28 MB in all).
- **`qcook zones`** orders the chunks in the `.ZON` file: the player's
  start's zone and its three nearest first (the load's first reads, in
  one sweep: the leaf found as the engine finds it, `leaf_of_point`),
  then outward through the sets' neighbourhoods (neighbouring zones side
  by side on the disc), any zone no set reaches last. The directory's
  entry per zone is unchanged, only the sectors; the skeleton's bytes
  change with them (another cook: the stamps still match).
- `scripts/cook.sh` runs `qcook section` for every map after the stream
  and the maps; the build's glob takes `*_s0.dat`.

### 84.3 The engine

- **One directory read a load** (engine/cd.c): `cd_cache_dir("\LVL\E1M1")`
  at the load's start reads the level's directory once and keeps its
  files (12 at most); `cd_find` answers from it for that directory's
  files without a read (seven directory reads a load before, §83.6, each a
  seek), and says "not there" without a read too. A directory two levels
  deep (`\LVL\E1M1`) is looked up in its parent's records, read once, and
  noted (`find_dir`). `cd_list_dirs("\LVL")` lists the levels:
  `world_scan_maps` finds the disc's maps there (a directory named as a
  map), and the levels the disc carries from one map's files. `MAX_DIRS`
  8 → 40.
- **The paths**: the bank, the textures, the world, the models and the
  chunks in `\LVL\<MAP>\`; the menu's bank in the root.
- **The stream's level file** (engine/stream.c `stream_level`, after the
  world is in): `\<MAP>\<MAP>_S0.DAT`'s directory (two sectors) gives each
  asset its sector in the level's file, `base_lba` the file's; an asset the
  file lacks is `absent` and never read (a bench build prints it if it is
  wanted all the same: the cook's lists missed it); without the file, or
  with another pool's directory, the pool's sectors as before.
- **The music's copy** (engine/music.c `music_level`): `music_play` takes
  the track's copy from the level's directory when it has one (no read:
  the cached directory), else `\MUSIC`'s.
- **The refills** (B): `MUSIC_REFILL_CHUNKS` (CMake `Q1PSX_MUSIC_REFILL`,
  4: 16 sectors every 1.3 s, two chunks of margin in a 6-chunk ring; 2:
  as before). The staging buffer the refills and the sounds' pieces share
  grows to hold a refill (stream.c `STAGE_SECTORS`: 16 sectors, 32 KB,
  +16 KB of the level arena; a sound's pieces are 16 sectors too). The
  seek the refills plan around a read, `MUSIC_SEEK_MS`, defaults to 100
  (a hop within the section).

### 84.4 Measured

The disc read back (tools/cdsim `--table`): the root in one sector, `\LVL`
then the 32 levels, each in the order above, `\MUSIC` last; the release
disc 471 MB (the levels' stream files 28 MB, the track copies 232 MB); the
longest hop from a map's chunks to its track's end 7212 sectors (E4M7, the
track's last 12 sectors). `BENCH_MAPS=1 HASHONLY=1 PROFILE=0`: the 128
spawn views' `H` lines identical to master's (c11c97b, built beside with
the same cook). No `wanted, not in the level's file` in any run: the cook's
lists hold what the walks and the loads want.

§83.6's walk and loads again, on the new disc (`CDLOG=1 ZONES=1 MUSIC=1`,
and `CDMODEL=1`; the 27 zoned maps, 33 loads):

| | today's disc, PCSX-Redux | today's, a console (the model) | **the sections, PCSX-Redux** | **the sections, a console** | the sections, a console, the 1.3 s ring |
|---|---|---|---|---|---|
| chunk reads, frames each | 1134, 11.5 | 909, 23.9 | 1214, 7.9 | **1183, 10.9** | 1167, 11.0 |
| late events, late frames | 271, 3845 | 445, 10 156 | 231, 2697 | **250, 3485** | 256, 3728 |
| streamed sounds' pieces read | 7050 | 1350 | 11 293 | **8565** | 7318 |
| music refills (sectors each), underruns | 3426 (8), 0 | 3230 (8), 0 | 1741 (16), 0 | **1708 (16), 0** | 3410 (8), 0 |
| the walks' seeks (the model's) | 2792 s if charged | 1540 s | 1049 s if charged | **870 s** | 1006 s |
| hops near / mid / sled | 3805 / 980 / 6868 | 1528 / 104 / 3862 | 10 076 / 4224 / 0 | **7510 / 3981 / 0** | 5391 / 6549 / 0 |
| a load: reads, the model's seeks | 54, 5.9 s | 54, 6.0 s | 50, 3.6 s | **50, 3.6 s** | 51, 3.6 s |

- **On a console** (the model): the camera's zone comes late a third as
  often as on today's disc (10 156 → 3485 frames, fewer than today's disc
  on the emulator, 3845); a chunk read takes 10.9 frames instead of 23.9
  (the emulator read today's disc in 11.5); the sounds load again (8565
  pieces against 1350); the music refills half as often (1708, 16 sectors)
  and never runs dry; not one sled seek is left (3862 before); the drive's
  seeks 1540 → 870 s over the walks.
- **The 1.3 s ring** (`MUSIC_RING=4`, +16 KB of SPU RAM for the sounds):
  with four chunks a ring the refills are of two (the rule refills when
  fewer than three chunks are left: 3410 refills of 8 sectors), never dry;
  late frames +7 % (3728), the seeks 1006 s. Viable; left open.
- **The loads**: the seeks 5.9 → 3.6 s a load (no sled hop to `\MAPS`'s
  directory, to STREAM_0.DAT or to `\MUSIC`); 50 reads a load, each one
  after the other in the file: in the model a read that continues where
  the last one ended costs a track jump back (the disc turned during the
  pause: 53-147 ms, as much as a near seek), so the files in reading order
  save the hops, not the per-read floor. A load as one read (the four
  files are contiguous now) would.
- The engine's `seek_us` on the new disc against cdsim's model: within
  0.05 ms.

### 84.5 Left

- **C**, a zone's sounds read with its chunk: the sounds' pieces are still
  the walks' most reads (8565 of 11 500 on a console), ten times the
  distinct sounds.
- The ring's size (6 or 4 chunks) and the refill's (4 or 2): build knobs,
  a call to make on the machine.
- A load as one read of the level's contiguous files.
- The 1T-back cost of consecutive reads is the model's; the machine's own
  figure is for a DuckStation run of `CDLOG=1` to show.
- The lod-poc branch's levels of detail (§81) go into the level's stream
  file by the same cook once merged (the pool holds them).

## §85 — The sounds at half their rate where the ear loses nothing (2026-10-24)

The decision after §84: before any change to the sounds' residency
(§83.8's C), the one lever on their bytes, alone.

### 85.1 Why the rate

The SPU plays its own ADPCM and nothing else: 16 bytes a block of 28
samples, 4.57 bits a sample, read from its 512 KB; no other codec exists in
the hardware (the CD-ROM's XA is 4-bit ADPCM too, played as a stream, never
stored), and a software codec on the R3000 would end as ADPCM in SPU RAM
all the same. So a sound's bytes in SPU RAM are its samples, and its
sample rate is the one lever. Today: 11 025 Hz for 217 of Quake's 225
sounds (8-bit mono sources), 22 050 for 8 (195 KB of ADPCM between them,
`weapons/lstart.wav` and `lhit.wav`, the fish's, Shub-Niggurath's): 50
kbit/s a sound; the game's 225 sounds are 293 s and 1903 KB of ADPCM, a
level's 79-135 sounds 601-958 KB (its bank, 327-367 KB, resident; the
rest streamed into 93-133 KB), four times what the SPU holds.

Measured over the 225 sounds, the share of each one's energy above a
quarter of its rate — what halving the rate removes (2.76 kHz at 11 025
Hz, 5.5 kHz at 22 050):

| halved, the loss | at 11 025 Hz | KB | at 22 050 Hz | KB |
|---|---|---|---|---|
| under 1 % (transparent) | 100 sounds | 839 | 5 | 170 |
| 1 to 5 % (acceptable) | 57 | 401 | 1 | 4 |
| 5 to 15 % (noticeable) | 35 | 324 | 0 | 0 |
| over 15 % (bothersome) | 25 | 144 | 2 | 22 |

The bothersome are the metal and the highs: `weapons/ric1` (81 %), `tink1`
(59 %), `pkup` (42 %), `items/armor1` (41 %), `misc/h2ohit1`, `lock4`,
`secret`, `player/gib`, `h2ojump`, `r_tele3`, the wizard's `hit` and
`wsight`, `z_gib`, `power`, `medkey`, `fish/idle`, `weapons/lhit`. The
transparent ones are the rumbles, hums and cries: `lstart`,
`ambience/wind2`, the player's deaths and pains, `r_exp3`, `sgun1`,
`ambience/water1`.

A level's whole sound set with the transparent halved: 450-728 KB; with
the acceptable too: 377-638 KB, under the 460 KB the SPU leaves beside
the music on 9 maps of 32 (START, E1M1, E1M7, E1M8, E2M1, E3M3, E3M6,
E4M1, END).

### 85.2 The cook (tools/qcook/sound.c)

`QCOOK_SOUND_HALVE=<percent>` (default 1, then 5 in §86; 0 none;
`cooked/settings.txt` records it, so the build cooks again when it
changes): every sound the cook loads, the banks' and the streamed ones
alike, whose halving would lose no more than that share of its energy is
cooked at half its rate (`halve_by_class`, in `sound_add`); §86 added a
second halving, 5512 → 2756 Hz, for a sound whose spectrum at 5512 Hz
held no more than `QCOOK_SOUND_HALVE2` (1 %) above 1.38 kHz. §87 replaces
both by a rate per sound (`QCOOK_SOUND_KEEP`). The loss is the spectrum's share
above a quarter of the rate, by an FFT over the sound (`halve_loss`): a
first try measured it as the energy `wav_halve`'s 9-tap half-band filter
removed, and that filter is soft (0.82 at a tenth of the rate, 0.56 at
0.15), so `player/death5` read 29 % where its spectrum holds 0.1 % above
2.76 kHz, and almost nothing qualified (the banks lost 2 KB). The same
softness dulled every halved sound's mids, so the halving's low-pass is
now a 95-tap windowed sinc (Blackman) cut at a quarter of the rate: flat
to a fifth of it, 60 dB down past three tenths. The bank's budget still
halves other sounds for room when it must (the longest loops first); a
sound halved by its class is not halved again. The cook's lines say how
many and how much: `qcook: sounds — ... (n halved by their class, k KB
saved; m for room)`, `qcook: streamed — ... (n halved by their class, k
KB saved)`.

Nothing changes in the engine: a sound's pitch is the bank's or the
stream's (`qs_sound.pitch`, `qst_sound.pitch`: 0x200 at 5512 Hz), its
length in ticks the same.

### 85.3 The measure of quality

The bench counts the streamed sounds asked to play while not in
(`stream_stats.snd_missing`, engine/snd.c `sfx_get`; the walk's `bench
zones` line: `n sounds asked, not in`): a missing sound is what the player
would notice of the streaming, and the figure the SPU room moves.

### 85.4 Measured

`QCOOK_SOUND_HALVE=1` (the default), the 32 maps cooked again:

- **The banks**: 327-367 KB → **265-296 KB**. E1M1's 350 → 280 KB: 24
  sounds at 5512 Hz (the player's deaths, pains and landings, `r_exp3`,
  `sgun1`, `spike2`, `grenade`, `bounce`, `ax1`, `wind2`, the water's,
  `damage3`, `talk`, `menu3`, the demon's landing) and `lstart` from 22 050
  to 11 025 Hz; the ricochets, tinks, pickups and the rest as they were.
- **The streamed sounds**: 121 sounds, 1160 → **842 KB** (59 at 5512 Hz);
  STREAM_0.DAT 2276 → 1954 KB, the levels' files with it.
- **The pool the streamed sounds get**: 93-133 → **164-195 KB**, with the
  2 s music ring (+16 KB more with the 1.3 s one).
- The frames: nothing of the sounds reaches a display list (the 128 views
  were checked identical to master's on this branch's disc, §84.4).

The zones' walk under the drive model (`CDMODEL=1 ZONES=1 MUSIC=1
CDLOG=1`, the 27 maps), the halving off against on:

| | `QCOOK_SOUND_HALVE=0` | `=1` |
|---|---|---|
| streamed sounds asked to play, not in | 23 360 | **4 250** (−82 %) |
| the sounds' pieces read | 8574 | **5827** (−32 %) |
| the walks' seeks (the model's) | 871 s | 775 s |
| late events, late frames | 250, 3505 | 251, 3516 |
| music underruns | 0 | 0 |

With the pool at 164-195 KB and the sounds half their size, the player
hears five sixths of what the streaming lost; the drive reads a third
fewer sound pieces. The chunks' late frames are the music's and the
chunks' own affair and do not move.

### 85.5 Left

- **The threshold is the ear's**: `QCOOK_SOUND_HALVE=5` takes 57
  more sounds (1-5 % of their energy above a quarter of the rate: the
  teleporter's `r_tele1/2/4`, `guncock`, `shotgn2`, the player's `pain2/5/6`
  and `udeath`, the menu's clicks), another 100 KB or so; 0 restores
  today's sounds. A build cooks again when the setting changes
  (`cooked/settings.txt`).
- The bank's budget still halves the longest loops when a bank does not
  fit; none needs it now.
- §83.8's C1 (the bank by the episode's position) and C2 (the reach, the
  eviction's hysteresis) are not built.

## §86 — Every sound of a level resident: the bank fills the SPU, the rest in main RAM (2026-10-24)

The plan after §85: load only the sounds a level needs — taking the
episode into account for the weapons, and checking the entities exist (the
multiplayer ones left out) — and keep them all resident: the whole set in
SPU RAM where it fits in the 460 KB beside the music, else the SPU filled
and the rest in main RAM, taken from the level's stream pool. One adjustment
to the plan: a sound in main RAM must be in SPU RAM to play, so the SPU keeps
a swap room for the RAM-held ones, sized by the cook.

### 86.1 The level's set (tools/qcook/sound.c `sound_build`)

- **The entities**: the cook's `ents_parse` leaves out what single player
  never spawns (`ent_sp_never`: the coop and deathmatch starts, every
  entity inhibited at the three skills), so the maps' deathmatch weapons
  and items (one to four `weapon_*` a map, `item_*`, `func_wall`) bring
  nothing. The set is built from the entities that remain.
- **The weapons** (`episode_weapons`): Quake takes the inventory away when
  a map leads to START (client.qc `SetNewParms`), so a player entering a
  map can hold the shotgun, the axe, and every `weapon_*` single player
  spawns on any path from START to it through the maps'
  `trigger_changelevel` targets (the secret levels: E1M4 → E1M8 → E1M5,
  E2M3 → E2M7 → E2M4, E3M4 → E3M7 → E3M5, E4M5 → E4M8 → E4M6; END from
  START). Read from the paks' maps at each cook (32 entity lumps). A
  weapon's sound stays only for a weapon the player can hold or an entity
  that plays it (`WEAPON_SOUNDS`, from weapons.qc, ogre.qc, shambler.qc,
  misc.qc): `shotgn2` the super shotgun; `rocket1i` the nailgun; `spike2`
  the super nailgun or a `trap_spikeshooter`; `ric1/2/3`, `tink1` the
  nails (both nailguns, the spike shooter); `grenade`, `bounce` the
  grenade launcher or an ogre; `r_exp3` either launcher, an ogre or a
  `misc_explobox`; `sgun1` the rocket launcher; `lstart` the lightning
  gun; `lhit` the lightning gun or a shambler. E1M1 drops 29 KB (six
  sounds), START 60 KB (thirteen), the late maps nothing.
- **The whole set**: the always set so filtered (the player's, the
  water's, lava's, slime's and sky's by the map's contents, the
  teleporters', the items'), the static sources' ambients, the monsters'
  kinds' sounds (`stream_monster_needs`), the entities' sounds (doors,
  plats, buttons, triggers, items, keys: `sound_map_streamed_n`, which
  also counts the entities bringing each). 66 to 134 sounds a map, 298 to
  572 KB at §85's rates (the second halving included).
- **The split**: the always set first, then the ambients, the monsters'
  (the most numerous kind first), the entities' (the most entities first);
  SPU RAM takes them in that order up to 512 KB less the capture buffers,
  the music's ring (48 KB, `QCOOK_MUSIC_RING_KB`) and the swap room; the
  rest go to main RAM. The swap room is the biggest RAM sound plus 16 KB
  (32-35 KB today), the split made again when it grows. The bank
  (`<MAP>.SND`) holds the SPU set; `<map>.sndset` lists every sound as
  `spu` or `ram` with the swap room, for `qcook section`.
- **The level's file** (`qcook section --sndset`): a banked sound is
  `QST_BANKED` (not in the file: the engine never wants it of the stream);
  a RAM sound is flagged `QSTF_RAM` and the RAM sounds lie first, together,
  for one read. The models follow by distance as before.

The 32 maps: 10 whole in the SPU (START 298 KB, E1M1 373, E1M7, E1M8,
E2M1, E2M3, E3M3, E4M1, E4M8, END); the others 418-428 KB in the bank and
9 to 158 KB in RAM (E4M3 158, E2M4 147, E1M3 147, E4M7 132, E2M7 123,
E1M5 115, E4M5 103, E1M2 102; the rest under 100), 1 to 35 sounds. The
pool STREAM_0.DAT 1954 → 1770 KB (the second halving).

### 86.2 The engine (engine/stream.c, engine/snd.c)

- `stream_level` reads the level's directory as in §84 and notes the
  banked sounds (silent: wanted by their monster's group, they are in the
  bank, where `snd.c`'s `sound_of_hash` finds them first) and the RAM
  sounds' sectors (`ram_first`, `ram_sectors`; each asset's `ram_ofs`).
- `stream_map_start` reads the RAM block in one go (`cd_read_now`, 9 to
  158 KB: 5 to 80 sectors) into the level arena before the pool, which
  shrinks by as much; the `stream: pools` line says how many sounds and
  KB. The block does not survive the arena (`stream_abort`).
- A wanted RAM sound (`ram_start`): its place in the swap room as a disc
  read's (`sound_room`: the slot, the SPU place, the evictions), then one
  DMA from the block (`SpuWrite`), polled by `sound_step`'s `SR_UPLOAD`
  step; no drive, so `stream_reading` leaves it out and the music's and
  the chunks' reads go on. The preload does the same at once. The bench
  counts them (`ram_swaps`: the walk's line, `swapped from RAM`) apart
  from the disc's reads.
- A sound neither banked nor flagged (a level file without a sound set)
  streams from the disc as before.

### 86.3 Measured

The 128 spawn views: identical to master's. Every map loads with its bank
and its block (`stream: \LVL\E4M3\E4M3_S0.DAT, ... 28 for RAM (180 KB)`;
no `wanted, not in the level's file`, no block refused). The stream's pool
after the block: E4M7 283 KB (its 154 KB block), E4M3 334 KB, 1000 KB on
START. The stack: 9896 bytes of 12 288 never reached.

The zones' walk under the drive model (`CDMODEL=1 ZONES=1 MUSIC=1
CDLOG=1`, the 27 maps), §85's run against this one:

| | §85 (the sounds halved, streamed from the disc) | **§86 (every sound resident)** |
|---|---|---|
| the sounds' disc reads | 5827 | **0** |
| sounds put in the swap room from RAM | — | 3294 |
| streamed sounds asked to play, not in | 4250 | **294** (−93 %) |
| late events, late frames | 251, 3516 | 238, **3053** |
| the walks' seeks (the model's) | 775 s | **272 s** |
| music refills, underruns | 1735, 0 | 1661, 0 |
| a load: reads, PCSX-Redux's seconds, the model's seeks | 58, 10.8 s, 3.8 s | **35, 9.0 s, 2.5 s** |
| the banks | 265-295 KB | 297-427 KB |

The 294 sounds still missing are the swap's latency: a RAM sound wanted
and played before its DMA is through (the wants come every eighth frame,
by distance, as before), where §85's streaming read it from the disc in
tens of frames. The drive now carries the chunks, the models and the
music alone: a third of §85's seeks, and the chunks come late 13 % less.

The models' reads went 35 (§84, the drive saturated by the sounds: the
models starved) → 1443 in §85's run → 158 here. Not the resident slots:
`MAX_RES` 96 instead of 48 measured the same walk (157 model reads, 3078
late frames, 294 sounds missing) for 1.5 KB of stack (9896 → 8360 bytes
never reached); 48 stays. §85's 1443 were the sounds, smaller, filling
the SPU room and the slots the models shared; with the sounds in their
bank and their swap room the models stream by distance as they should.

### 86.4 Left

- **The swap's latency**: the 294 misses (0.5 % of what the player hears)
  would go with a want one pass earlier for the RAM sounds, or by playing
  a sound as soon as its DMA ends when it was asked for meanwhile.
- **The RAM**: the biggest blocks take 154-180 KB of the pool (E4M7's
  pool 439 → 283 KB, E4M3's 334); `QCOOK_SOUND_HALVE=5` with the second
  halving at 1 % is the setting to hear; a stricter one grows the
  blocks, a looser one shrinks them.
- **A listen**: the halved sounds (the player's cries, the
  explosions, the ambiences at 5512 Hz; the rumbles at 2756), on
  DuckStation; `QCOOK_SOUND_HALVE=0` restores today's.
- The music's ring at 4 chunks (`Q1PSX_MUSIC_RING=4`, +16 KB: the cook's
  `QCOOK_MUSIC_RING_KB=32` to match) stays a knob.

## §87 — A rate per sound, at a constant share of its energy (2026-10-24)

The observation after §86: the halvings gave an uneven fidelity —
a sound losing 1 % of its energy beside one losing 5 %, where the ear
starts to notice — and the very low sounds could go lower still. The lever
asked for: a sample rate of its own for every sound, chosen for a constant
fidelity, the SPU playing it at the pitch of that rate (which is what the
halvings did in steps of two). A second idea, subsampling a sound whose
band is narrow and restoring it by the pitch, buys nothing beyond it: the
SPU's pitch multiplies every frequency (a resampling), it shifts no band,
so a sound with content up to f₂ needs 2 f₂ samples a second whatever its
lowest frequency, unless it is transposed, which would not be Quake's
sound.

### 87.1 Measured first

Over the 225 sounds, the lowest rate keeping a share of each one's energy
(the spectrum's cumulative energy, 15 % of room above the frequency it
reaches the share at, 2756 Hz at the least), and the maps' sets (§86's,
beside the 426 KB SPU RAM leaves for a bank with a 34 KB swap room):

| energy kept | the 225 sounds | maps whole in the SPU | the biggest RAM block | RAM over the 32 maps |
|---|---|---|---|---|
| 95 % | 834 KB | 29 | 7 KB | 15 KB |
| 97 % | 912 KB | 23 | 53 KB | 265 KB |
| 98 % | 975 KB | 18 | 90 KB | 662 KB |
| 99 % | 1078 KB | 8 | 143 KB | 1713 KB |
| 99.5 % | 1187 KB | 1 | 203 KB | 3145 KB |
| §86's halvings (5 %, then 1 %) | 884 KB | 10 | 155 KB | 1619 KB |
| native | 1903 KB | 0 | 516 KB | 11 218 KB |

The halvings were the most compressive rule but the least even: at 99 %
`weapons/guncock` keeps 10 036 Hz where §86 halved it (it loses 2.3 % at
5512), `player/death5` goes to 3671 Hz where §86 kept 5512. A perceptual
rule, keeping every 1/6-octave band within 40 dB of the loudest, keeps the
noise floor too and saves almost nothing (1529 KB).

### 87.2 The cook (tools/qcook/sound.c)

`QCOOK_SOUND_KEEP=<percent>` (default 98, chosen after a first
try at 95; 100 or 0: a sound's own rate; `cooked/settings.txt` records it)
replaces
`QCOOK_SOUND_HALVE` and `QCOOK_SOUND_HALVE2`. For every sound the cook
loads: `keep_freq` reads off the spectrum the frequency below which the
share of its energy lies; the rate is 2.3 times it (15 % of room above
the Nyquist), 2756 Hz at the least, rounded to a pitch the SPU has
(`rate × 4096 / 44100` whole); `wav_resample` takes the sound there: the
clean halvings of §85 while the rate allows, then one step by a
windowed-sinc resampler at a ratio in (1/2, 1] (Blackman, 95 taps, the
low-pass at 0.45 of the new rate, unity gain), the loop's start scaled
with it. The bank's and the streamed sounds' headers carry the pitch as
before; nothing changes in the engine.

### 87.3 Measured, at 95 % first

At 95 %, the 32 maps cooked again: 29 maps whole in the SPU (banks
235-436 KB), E1M3 and E4M3 with one sound (19 KB) in RAM, E2M4 one (5
KB); STREAM_0.DAT 1770 → 1618 KB. E1M1's 93 sounds: 7 at 11 025 Hz
(`ric1`, `tink1`, `armor1`, `secret`, `lock4`, `pkup`, `h2ohit1`...), 55
between 2778 and 10 530 Hz (`gib` 10 530, `r_tele5` 7730, `pain5` 5017,
`death5` 3101, `water1` 3338), 30 at the 2756 Hz floor (`guncock`,
`r_exp3`, `wind2`, the ogre's and the player's low sounds...). The maps all
load (`BENCH_MAPS=1 VIEWS=0`: no bank out of bounds, no block refused).
The shotgun and the explosions at 2756 Hz were the point to hear; what
else could make room was looked at, and 98 % chosen with §87.5.

### 87.4 The loops, the variants, what is rarely heard (measured)

- **The loops**: 23 sounds loop, the big ones the ambients (`drip1` 12.5 s
  and 34 KB, `swamp1`/`swamp2` 6.2 s and 25 KB each, `buzz1` 16, `comp1`
  16, `suck1` 10 s and 15 KB, `fl_hum1` 12, `fire1` 10). None has a
  shorter period repeating within the loop at 3 or 10 % error: they are
  noises (drips, bubbles, crackle), not hums; a shorter loop would bring
  the same drips back more often. About 70 KB on the maps that have them,
  the ear's call; not done.
- **The variants Quake draws at random**: the teleporters' fog
  `misc/r_tele1-5` (24 KB, on nearly every map), the swamps' two ambients
  (two entities, 51 KB), `weapons/ric1-3` (10 KB), the player's five
  deaths (9 KB) and six pains (6 KB), a monster's two idles... Keeping
  fewer would need the engine to pick among the kept (three silent
  teleports in five otherwise) and departs from Quake's variety; the fogs
  are handled by §87.5 instead, the rest weighted down there.
- **Rarely used, or not grave to lose**: little is left, the cook already
  filtering by the map's contents, teleporters, items, kinds and weapons;
  `misc/talk` (1 KB), the menu's three clicks (2.7 KB, the pause menu's),
  the drowning sounds (3-8 KB, water maps only).

### 87.5 The teleporters' fog in main RAM, two in the swap room (2026-10-24)

The idea for the rarest sounds: keep two of the five fogs
resident, pick among those two, bring another in after each teleport;
from main RAM rather than the disc (the five take 24-31 KB of a pool of
400-1000 KB, where two slots reloaded from disc copies would save 15 KB
for reads in play).

- **The cook**: the five fogs leave the always set for the pool
  (`TELE_SOUNDS`, in `sound_streamed`) and join a map's set when it has
  teleporters, in a last class of the split (4): the first to go to main
  RAM when a map does not fit. Any other variant among several (a name
  ending in a digit with siblings in the set) has its weight divided by
  their count: it leaves the SPU before the sound every event plays. The
  swap room is the two biggest fogs in RAM (pinned), the biggest other RAM
  sound, and 16 KB.
- **The engine** (engine/snd.c `snd_tele_fog`, `snd_stream_wants`;
  engine/triggers.c `play_teleport`): `named[S_R_TELE*]` resolve to the
  bank's or the stream's as before (`sound_of_hash`); each pass pins two
  fog variants (`STREAM_PIN`: in the swap room, never evicted) where they
  are the stream's; a teleport plays one at random among the fogs playable
  now (the bank's five on a whole map, the two pinned on the others:
  `random() * n`, Quake's arithmetic), and a played pinned one gives its
  place to the next of the five, brought in from RAM by the next pass's
  DMA. A map without teleporters has them absent (never wanted).

**Measured at 98 %**: 23 maps whole in the SPU (banks 265-434 KB); nine
with a RAM block: E1M2 10 KB (one fog), E4M5 10, E2M6 18 (two fogs),
E1M5 23 (three), E2M7 43 (the five fogs, `damage`, `damage2`), E4M7 57
(four fogs, the keys' `medkey`/`runekey`, `inv3`), E2M4 60 (the fogs,
`basesec1`, `doormv1`, `r_item2`, the pentagram's three...), E1M3 69 and
E4M3 80 (the fogs, then the secret doors', the pentagram's, the plats');
the swap room 32-52 KB; STREAM_0.DAT 1706 KB. Every map loads
(`BENCH_MAPS=1 VIEWS=0`).

### 87.6 Left

- A listen at 98 % (`QCOOK_SOUND_KEEP`, per cook).
- The 15 % of room and the 2756 Hz floor are constants of
  `halve_by_class`; the floor could go lower for the hums.
- The long noise loops shortened (§87.4), if the 70 KB are wanted.

## §88 — The level of detail by the vertices' order (2026-10-25)

The idea for the monsters' cost at a distance, after §81's level
POC (three whole models swapped by distance, −2.75 % on E1M1's views
for +125 KB of RAM and a visible switch, not merged): store a model's
vertices in the order of their visual relevance, the most needed first,
and keep in the model's asset a small table giving, as a function of the
distance, how many of them to draw; beyond its distance a vertex is
simply absent, nothing is swapped and no second model is held. A proof
of concept on the hand-made soldier (on a branch of its own,
throwaway: `tools/lodpoc/mdl_lod.py`, a page with a distance slider,
Quake's data kept outside the repository) set the ordering; this section
is the cook and the engine.

### 88.1 The order

`tools/qcook/lod.c` (`lod_plan_make`) ports the POC's ordering to the
cook, on a body's hand-made model (§62, §64: the `MOBS` table applied
first, the frames `MOB_FRAMES` never plays left out). It collapses the
vertices one by one onto a neighbour, the cheapest first (a progressive
mesh by half-edge collapses):

- the cost of collapsing v onto u is v's move seen along the normals of
  its live triangles, the root of the sum of the squares, the worst of
  the frames played, plus the error v already carries from earlier
  collapses onto it (the accumulated error of the chain), times 3 when
  the skin would stretch across the model (the two corners' texels more
  than 24 apart on the triangle's side of the skin) (§110: the rims and a
  memory of the model's own vertices in the accumulated error's place,
  which let thin parts go far past the tolerance);
- a collapse that turns any triangle over in any frame is refused;
- after a collapse the costs of the vertices around v and u are worked
  out again (the POC only re-evaluated those around u: a vertex whose
  only triangle with the target vanished kept a stale cost, and its
  cached target could be a vertex already gone; here a parent is always
  alive when its child collapses, so a parent always has a lower new
  index);
- the collapses stop when four triangles are left (in practice a body's
  last collapses are refused long before: 102 of the soldier's 141
  vertices, 148 of the dog's 216, 93 of the boss's 281 within the draw
  distance).

The errors are made monotonic along the order; a collapse takes effect
beyond the depth where its error projects to the tolerance, `error x 160
/ tolerance` (the GTE's H is 160 at 320 wide, a 90-degree field:
`r_proj_h`). `QCOOK_LOD_PX` is the tolerance in pixels, 2 by default
(chosen after the first measurements at 1; part of
`scripts/cook.sh`'s settings line: a change cooks again),
`QCOOK_NOLOD=1` cooks today's order with nothing cut (an A / B). The
table is cut in bands of 64 world units of the model's depth up to the
draw distance of 1536 units (24 bands, `QST_LOD_BANDS`): band b takes
the collapses whose depth is under its near edge, so a vertex at its
threshold moves by at most the tolerance anywhere in its band, and band
0 is the whole model (an error of 0 collapses nothing there). No
hysteresis: a vertex appears or goes at a move of at most the tolerance.
The nine bodies order in 2 s.

The soldier at 2 px, the cooked default (its vertices / its triangles,
the band's near edge in units): 0: 141/270, 128: 131/250, 192: 122/232,
384: 112/212, 576: 90/168, 768: 65/118, 1152: 32/48, 1472: 27/38 (the
POC gave 86/160 at 400, 37/64 at 800, 23/34 at 1536). At 1 px, the first
cooked: 192: 137/262, 384: 122/232, 576: 105/198, 768: 90/168, 1152:
61/110, 1472: 43/74 (the POC: 137/262 at 200, 121/230 at 400, 103/194 at
600, 86/160 at 800, 56/102 at 1200, 37/64 at 1536: the same order within
the band's quantization). The nine bodies at 2 px, the vertices at the
bands 0 / 384 / 768 / 1472 (and the collapses that happen within the
draw distance): soldier 141 / 112 / 65 / 27 (116), dog 216 / 137 / 80 /
55 (161), ogre 140 / 110 / 79 / 32 (109), knight 96 / 76 / 55 / 14 (82),
zombie 124 / 78 / 41 / 25 (99), wizard 72 / 62 / 44 / 15 (59), demon 140
/ 127 / 88 / 37 (105), shambler 141 / 137 / 104 / 46 (99), boss 281 /
215 / 206 / 147 (136: its vertices' grid is four times coarser, §64, its
first collapses cost 0.4 units and go at 64 units). At 1 px: soldier
141 / 122 / 90 / 43 (102), dog 216 / 151 / 101 / 70 (148), ogre 140 /
113 / 93 / 54 (88), knight 96 / 84 / 61 / 33 (63), zombie 124 / 92 / 57
/ 32 (95), wizard 72 / 63 / 49 / 30 (43), demon 140 / 134 / 109 / 65
(76), shambler 141 / 138 / 127 / 89 (52), boss 281 / 215 / 211 / 190
(93).

### 88.2 The asset

`formats/qstream.h`, `QSTM_LOD`, set on every sorted body (`QSTM_SORT`:
the nine monsters; with `QCOOK_NOLOD=1` too, nothing cut):

- the vertices renumbered by the order: those never collapsed first,
  then the collapses from the last to the first, so that a band's
  vertices are a frame's first `num_verts[b]`; the frames' words and the
  records' indices follow it (`frame_plan_make`, `cook_compact` through
  the levels' `order`);
- after the frame map, a `qst_lod`: `num_verts[QST_LOD_BANDS]` and
  `num_min` (the last band's), then the parents of the vertices that
  collapse within the draw distance, `uint16_t[num_verts - num_min]`
  (a parent is the collapse's target through the collapses before it,
  always a lower index), 168-376 bytes a body at 2 px (136-344 at 1);
- the records are the ones the cook packed before (the file's order, the
  window preferring the triangle sharing the most vertices: the same
  count and the same slots as master's), but inside each one the slots
  are ordered by the farthest band of their triangles and the triangles
  by their band, the longest-lasting first, and the record ends with its
  own table (`QST_LOD_REC_BYTES`, 48: per band its slots and its
  triangles still drawn, a byte each), so that what a band draws of a
  record is a prefix of its index list and a prefix of its polygons;
  the record's size field covers the table, `polys` is unchanged;
- the shared asset the cook checks each level against (`qst_shared`,
  `qst_assemble`) carries the block (`ofs_lod`, `len_lod`).

The nine bodies grow by 360-1096 bytes each at 2 px (the soldier 83 316
→ 84 028, the dog 93 504 → 94 600, the boss 145 276 → 146 220; 328-1064
at 1 px), STREAM_0.DAT by 10 KB (1802 → 1812 KB); on a map only the
kinds present are resident (5-8 of the nine: 2-6 KB).

### 88.3 The engine

- `engine/rlist.c` `rl_model_stream`: the origin's depth, which the sort
  already took from the GTE (`ent_sort`, the view's rotation and
  translation left by `rl_ent_matrix`), gives the band (`z >> 8`: 64
  world units are 256 vertex units; clamped to the last); a nearer band
  with the same vertex count stands for it (one expansion in the cache
  for the bands of one cut). The band is the entity's `lod_band` (x 2,
  its entry in each record's table; it shares `cam`'s word, a brush
  model's) and goes into the frame cache's key as `band << 11`
  (`mdl_frame`'s fourth argument, or'ed into the key after the frame
  stored: a body stores fewer than 2048 frames, the cook checks it;
  `mdl_forget` compares the asset's half as before).
- `engine/mdl.c` `mdl_expand_new`: the band's vertices expanded
  (`mdl_expand`, the first `num_verts[b]`), the others copied from their
  parents in index order (`engine/mdl_expand.s` `mdl_lod_copy`: two
  loads and two stores a vertex, the next parent's index loaded ahead; a
  parent's slot is final when its child's is written). The pool keeps its
  `num_verts x 8` bytes: the records index it as before, a collapsed
  vertex's slot holding its parent's position and normal, so a corner
  whose vertex went reads its own texels at the surviving vertex's place
  (the POC's default) with the parent's light.
- `engine/r_world.s`: `draw_model` puts a sorted model's band in
  `SP_LODBAND` and lights it through `light_recs_lod` (light_recs with
  the band's slot count of each record read from its table: the dead
  slots are not lit); `ds_records` reads each record's two counts first
  (the band's entry at the record's end: a load, two adds, two byte
  loads; a record with no triangle left at the band is passed over before
  its sphere test), keeps them in `SP_LODCNT`, transforms the band's
  slots (the light bytes stay the record's own: their offset uses the
  full count) and emits the band's triangles. Nothing in the asset is
  written: the same records serve every monster of the kind at its own
  band in the same frame. The world's and the items' record loops
  (`RECLOOP de`) are untouched: only sorted bodies carry tables.
- The C engine (`ENGINE_C=1`): `mdl_frame`'s key and `mdl_lod_copy` in
  C (`engine/mdl.c`); the renderer stays asm.
- `Q1PSX_LOD=0` (CMake; `LOD=0 scripts/bench.sh`) draws band 0 at every
  distance, the engine's A / B.
- The bench: a `lod view` line per view (the streamed models drawn, those
  cut to a band past 0, the farthest band), a `clear frames` line for
  `MONSTERS=4` (the fights' frames, their cycles, the models drawn and
  cut).

### 88.4 A first cut, measured and dropped

The first engine built drew a band as a prefix of the records: the
triangles sorted by the band they vanish at, the records cut in that
order (per image, since a record has one texture), and the table giving
per band the last record of each image and its live triangles, the
renderer patching those two records' counts and the first image's last
record's size (a step over its dead records to the second image's first)
for the draw and putting them back after. It measured −1.42 % on the 128
spawn views (−3.53 % on the 28 with a cut model) but lost on near
monsters (E3M4.v2 +1.0 %, E4M5.v0 +1.2 %, E3M7.v3 +0.35 %): the vanish
order scatters a band's triangles over the body, so the records packed
badly, 12 instead of 9 for the soldier with 472 instead of 341 record
slots to transform (+8 % to +50 % over the nine), and the near bands paid
it at every frame. The per-record tables keep master's packing and cut
inside every record instead; they cost a few loads a record and 48
bytes a record, and beat the prefix everywhere (below).

### 88.5 Measured

PCSX-Redux in fast forward, this branch against master bb97a2c built
beside it (a detached worktree with the same cooked data; the stream
cooked by each side's cook), at 1 px first, then at the 2 px
chosen:

- `BENCH_MAPS=1 HASHONLY=1 PROFILE=0`, the 128 spawn views: 100 `H` lines
  identical (no body cut there), 28 differ, every one of them cheaper.
  At 1 px −4.93 % over the 28 (17 676 287 → 16 805 060 cycles; the best
  E4M3.v3 −11.0 %, E4M3.v1 −9.6 %, START.v0 −10.4 %; the least E4M5.v0
  −0.26 %, one body at band 3), −1.99 % over the 128. At 2 px −9.01 %
  over the 28 (→ 16 084 097; E4M3.v3 −21.7 %, E4M3.v1 −18.9 %, E3M7.v2
  −17.5 %, E3M6.v0 −15.3 %, E1M5.v0 −14.9 %; the least E4M5.v0 −2.4 %),
  −3.64 % over the 128.
- `MAP=E1M1 MONSTERS=4` (the map cleared): the 333 fight frames
  1 371 337 → 1 319 179 x 256 cycles at 1 px (−3.80 %), → 1 265 844 at 2 px
  (−7.69 %), the same 1525 streamed models drawn, 593 of them cut to a
  band past 0; the stack's margin 9960 → 9944 bytes never reached.
- `MAP=E1M3 MONSTERS=3` (the crowd, then a minute of fight): the fight's
  first 1500 frames, a 300-frame window each, 682 445 → 657 810 → 640 684
  cycles a frame (1 px −3.6 %, 2 px −6.1 %), 681 236 → 652 913 → 632 736
  (−4.2, −7.1 %), 650 810 → 632 120 → 616 114 (−2.9, −5.3 %), 710 362 →
  687 715 → 664 286 (−3.2, −6.5 %), 825 602 → 806 908 → 786 505 (−2.3,
  −4.7 %); the last 2100 frames +0.01 % to +0.02 % at both (one streamed
  model drawn a frame, near: the tables' reads); the minute −1.5 % at 1
  px, −2.8 % at 2; the crowd itself (the entities' tick) unchanged; the
  stack 10 112 → 10 104.
- `BENCH_GAME=1 HASHONLY=1 PROFILE=0`: −7.8 % over the twelve scenarios at
  1 px (12 160 808 → 11 210 184 cycles; −6.6 % grlauncher to −9.4 %
  gglauncher), −8.8 % at 2 px (→ 11 089 358; −8.2 % grlauncher to
  −11.6 % gglauncher): the start of E1M1 looks down its hall at the
  soldiers and the dog far away.
- RAM: the release's level arena 1 344 048 → 1 343 008 bytes (−1040: the
  code), the assets above.
- The pictures at 2 px (the same views as the 1 px check below): the
  same bodies identical, 31-120 pixels on E1M8's far ones (40 on v0's,
  120 on v7's two), the ogre of E3M7.v4 the same 1480 pixels of a texel's
  shift (band 1 cuts the same 19 vertices at both tolerances), E3M7.v7's
  three bodies 61.
- The checks (TESTING 11.61): the C engine (`ENGINE_C=1`) gives the asm's
  128 `H` lines; `LOD=0` gives master's `Q` line (the packet set) on each
  of the 28 views whose `H` differs (the slots and triangles reordered
  inside master's records; +0.05 % of cycles over the 128: the tables'
  reads); the `QCOOK_NOLOD=1` cook gives master's `H` lines on the 128
  views and the game bench's (the engine's new code is neutral with
  nothing cut); the pictures of E1M8's and E3M7's twelve views each,
  master's beside the branch's (pktview, a host diff): the differing
  pixels lie within the bodies' boxes alone, none at all on most of the
  cut bodies (E1M8.v1 at band 7, v3 at 16, E3M7.v2, v5 with six bodies to
  band 18, v8 to band 22), 15-59 on the far ones (E1M8.v0, v7, v8, v9:
  one to two pixels along an edge), and the ogre of E3M7.v4 at band 1
  (64-128 units, 19 vertices of an error under 0.4 units gone) has 1463 of
  its box's 2618 pixels changed by a texel's shift, 924 of them by 32 or
  less on a channel, 490 by 64 or less, the shape the same; hazcheck and
  iccheck pass at every link.

### 88.6 Left

- The tolerance: 2 px cooked (`QCOOK_LOD_PX`; the 1 px cook
  measured first stays in §88.5 for the scale), to be seen on
  DuckStation; and so is the band's edge (the near one here: a vertex
  moves by at most the tolerance anywhere in its band; the band's middle
  would cut more).
- The band comes from the origin's depth: a big model's near side is
  nearer than its origin (the shambler's by 40 units), its vertices there
  a little over the tolerance at the band's near edge.
- The other alias models (the items, the heads, the gibs): the same cook
  path would order them (no hand table: every frame counts), but their
  record loop (`RECLOOP de`) has no table lookup, their triangles are
  drawn in the records' order (a painter's order, not sorted: reordering
  them inside the records changes which triangle shows where two
  overlap), and few of them are far and large; measured nothing, left.
- The records' tables cost a load, two adds and two byte loads a record
  and 48 bytes a record; a model with no cut at its band pays them too
  (+0.01-0.02 % a frame with one near body drawn). A per-model "whole"
  flag in `SP_LODBAND` would spare them at band 0, for a branch a record.
- The frame cache holds one expansion per (asset, frame, band of a
  distinct cut): bands of one cut fold onto the nearest, but a body
  walking through the bands expands its frames again at each (an
  expansion is ~4 cycles a vertex plus 12 a parent copy); no refusal seen
  in the benches.

## §90 — The stream's pool grown: nine RAM levers in three batches (2026-10-28)

(Provisional number: §89, the draw distance of 1024, is on a branch of its
own, not merged; the numbers get settled at the merge.)

The stream's pool is the first thing to run short: on E1M3 (release) it is
348 KB and full at the spawn, 13 KB free (the RAM report of 2026-10-25,
outside the repository, on master 795bfb2 with the pvs_1536_64 tables and
three slots: 368 KB then). Every byte the rest of the RAM gives up goes to
it, since it takes the level arena's rest. Nine levers were chosen out
of eleven, in three batches, each built and measured on this branch, one
commit a lever, to validate before any merge:

1. the two packet arenas sized on what a frame writes (§90.1);
2. ENT_SPARE 96 → 48 (§90.2);
3. the staging buffer: the music refilled by 2 chunks, 16 KB (§90.3);
4. the stack 12 → 8 KB (§90.4);
5. a compact `edict_t` (§90.5);
6. the brush models' rooms as a cut cache sized by the PVS (§90.6);
7. the loads' and menus' code as an overlay in the arena's top (§90.7);
8. nodes 36 → 20 bytes, leaves 28 → 16 (§90.8);
9. the PVS as deltas decoded at the marking (§90.9).

**The measure, at each lever.** The bench build with the release's arena
(`LEVEL_CAP=`, the release's `ramcheck` line, which moves with the BSS and
the stack): `BENCH_MAPS=1 VIEWS=0 RESIDENCY=1` for every map's pool (`bench
zone` lines; every map at every lever in §90.11), the smallest pool and
E4M7's; E1M3's arena block by block
(`MEMLOG=1 MAP=E1M3 BENCH_MAPS=1 VIEWS=0`: a `memlog` line per block, its
caller by addr2line; the RAM report's own instrumentation stays on its
throwaway branch). The frames: the 128 spawn views, the game bench, the
monsters' fights and the map cleared, hash for hash against the branch's
base (795bfb2) with the same cooked data (`seen_1536_64`, four slots: the
tables master's 1536-unit draw distance was cut for; the main checkout's
`local.env` names the 1024 tables of §89 today). PCSX-Redux in fast
forward.

**The base (795bfb2, release arena 1 343 008 B):** the smallest pool
E4M7's 308 KB, E1M3's 348, START's 631; E1M3's arena: textures 1.9 KB,
skeleton 267, face stamps 7.3, models 4.8, four slots 256, touch lists
8.7, edicts 149.4 (389 × 384), brush models 69.9 (57 drawn: their caches
49.9, their pieces' vertices 20.8), the RAM sounds 136 (24 of them), the
frame cache 24, the staging buffer 32, the pool 348 (+ 8 KB kept under the
stack); the table block by block at each lever in §90.10.

### 90.1 The packet arenas: 2 × 96 KB → 2 × 80 KB

The two display lists live in the BSS (`engine/vid.c` `buf`: a DISPENV, a
DRAWENV, eight ordering-table words and the arena, 98 420 B each). What a
frame writes was measured in every bench: the lines' `pkt=` (the arena
bytes used, the HUD's 1.5 KB block past them left out) and, new, `room=`
(the least room an alias model found at `draw_model`'s test: under
`R_MODEL_ROOM`, 24 576 B, a monster, an item or a gib is left out so the
farther world records keep their room, r_defs.inc) and `mskip=` (the
models so left out), the walk's and the fights' lines carrying the same
three (`bmfrag`-free counters in bench builds: `.if ASM_BENCH` in
r_world.s, `r_model_room_min`, `r_model_skipped`).

| scenario | packets at most (B) | a model's room at least (B) |
|---|---|---|
| the 128 spawn views | 53 588 (E3M4.v0) | 52 104 (E3M4.v0) |
| the game bench (12 scenarios) | 54 856 (lightning) | 50 132 |
| E1M3's crowd fight (MONSTERS=3) | 40 880 | 58 116 |
| E1M8's fight (Chthon) | 57 872 | 51 632 |
| E1M1 cleared (MONSTERS=4) | 63 924 | 46 004 |
| E3M4's crowd fight | 77 660 | 34 468 |
| the zones' walk (27 maps, legs through walls) | 76 932 (E4M1) | 30 356 (E2M6) |

With 96 KB an arena, a model's test sees 94 672 − (the bytes before it);
an arena of A bytes shrinks every room by 98 304 − A, so no frame of a
scenario changes while its least room stays over 24 576 after that: the
views allow 70 776 B, the game bench 72 748, E1M8's fight 71 248, E1M1
cleared 76 876; E3M4's crowd and the walk through walls (the camera in a
wall, every leaf around drawn) would need 88 412 and 92 524 to keep every
frame, and their 77.7 KB of records need an end past them (the walk stops
at the arena's end less 2 096 B: `r_arena_full`, the farthest records
lost). The choice: **80 KB** (81 920 B, the walk's end at 78 288 B): the
views, the game bench, the fights of E1M3 and E1M8 and E1M1 cleared keep
every frame (checked below); E3M4's crowd fight and the walk's legs through
walls lose a far model in the frames where the room runs under 24 KB (the
`mskip=` counts below), no frame fills. 16 KB a buffer: −32 768 B of BSS,
the release's arena 1 343 008 → 1 375 776 B.

**Checked at 80 KB** (the base run first with the same counters, then
this): the 128 spawn views' `H` lines identical (no `mskip`, no `FULL`),
the game bench's 23 identical, E1M3's crowd fight line for line (the
cycles aside: the counters' own instructions), E1M8's fight and E1M1
cleared the same (one cycle count aside), E3M4's crowd fight the same
lines with 47 models left out over its 3600 frames (46 of them in the
first 300, the crowd all in view at 77.4 KB of packets, the room down to
18.1 KB; one in the next 300; none after), the walk 543 models left out
over 27 maps × 3600 frames (15 maps, E4M2's 114 the most: its legs through
walls at 60.7 KB), no frame full anywhere. Every map's pool +32 KB: the
smallest E4M7's 308 → 340 KB, E1M3's 348 → 380, START's 631 → 663. A run
of E1M8's fight stalled in PCSX-Redux itself (1 % of a CPU, no console
line for eleven minutes, the same build fine before and after):
`headless.sh` now stops a run whose console stays silent for `STALL`
seconds (30), exit status 3, instead of sitting through its cap; the
caps given to `bench.sh` are two or three times a fast-forward run's
need.

### 90.2 ENT_SPARE 96 → 48

The edict pool is sized at the map's spawn (`ents_trim`, §77.2): the slots
in use plus ENT_SPARE free ones for what the game spawns in play (a
DelayedUse relay, a teleport's fog and death trigger, a gib, a backpack,
a wizard's missiles, a shambler's lightning), 384 bytes each. What the
play needs, measured on the base build (`MONSTERS=3`: the crowd awake, a
minute of rockets; `MONSTERS=4`: the map cleared monster by monster, then
30 s of bodies and gibs):

| scenario | at the spawn | the peak | the pool | over the spawn |
|---|---|---|---|---|
| E1M3's crowd fight | 293 | 306 | 389 | +13 |
| E3M5's crowd fight | 233 | 235 | 329 | +2 |
| E1M8's fight (Chthon) | 157 | 162 | 253 | +5 |
| E1M1 cleared | 145 | 177 | 241 | +32 |

A cleared map is the deepest: each monster killed with rockets leaves two
to four gibs for 10-20 s and a backpack for two minutes (`DropBackpack`:
every monster with ammo). A spare of 48 covers these runs with 16 slots
to spare on E1M1 cleared. What a full pool costs: `ent_spawn` returns 0
and the caller gives up: a gib or a backpack not thrown (`throw_gib`,
`drop_backpack`), a missile not fired (the wizard), and in
`sub_use_targets` a delayed use never done (its DelayedUse edict): a map's
logic, not only its look. The spawn's own edicts (the doors' fields, the
plats' triggers) are counted before the trim and take none of the spare.

### 90.10 E1M3's arena, block by block, at each lever

`MEMLOG=1 MAP=E1M3 BENCH_MAPS=1 VIEWS=0 LEVEL_CAP=<the release's arena>`:
a `memlog` line per block the level arena hands out or takes back, its
caller resolved on the host (the symbol table of the run's own ELF: the
one `chain`'s runs keep beside each log; addr2line's line); a block a
release cuts into (a file read whole, its tables kept at its base; the
edict pool trimmed) is what is left of it. The release's blocks are these
at 0x800ad1d0 (0x800b51d0 before §90.1), the bench build's base differs.
Bytes; the pool takes the rest less 8 256 (`STREAM_KEEP` + 64).

| block (caller) | base (795bfb2) | §90.1 | §90.2 | §90.3 | §90.4 | §90.5 | §90.6 | §90.7 | §90.9 |
|---|---|---|---|---|---|---|---|---|---|
| the texture tables (cd_load, the file 124 928 read whole) | 1 936 | 1 936 | 1 936 | 1 936 | 1 936 | 1 936 | 1 936 | 1 936 | 1 936 |
| the skeleton `E1M3_G0.WLD` (cd_load; §90.9: its PVS rows 25 516 → 19 897 B, the file two sectors shorter) | 274 432 | 274 432 | 274 432 | 274 432 | 274 432 | 274 432 | 274 432 | 274 432 | 270 336 |
| `face_visframe` (world_load) | 7 488 | 7 488 | 7 488 | 7 488 | 7 488 | 7 488 | 7 488 | 7 488 | 7 488 |
| the map's models `E1M3_M0.MDL` (cd_load) | 4 096 | 4 096 | 4 096 | 4 096 | 4 096 | 4 096 | 4 096 | 4 096 | 4 096 |
| their tables (world_load: 256, 32, 8 × 48) | 672 | 672 | 672 | 672 | 672 | 672 | 672 | 672 | 672 |
| the zone slots, 4 × 65 536 (zone_map_start) | 262 144 | 262 144 | 262 144 | 262 144 | 262 144 | 262 144 | 262 144 | 262 144 | 262 144 |
| the touch lists, `model_draw` (ents_load: 7 120, 1 792, 224; from §90.2's 341 slots 6 352, 1 600, 224) | 9 136 | 9 136 | 8 176 | 8 176 | 8 176 | 8 176 | 8 176 | 8 176 | 8 176 |
| the edicts (ents_load; 445 asked, 389 kept by ents_trim, 341 from §90.2; 320 B each from §90.5) | 149 376 | 149 376 | 130 944 | 130 944 | 130 944 | 109 120 | 109 120 | 109 120 | 109 120 |
| the brush models (bmf_reserve: 432, the caches 49 888, their vertices 21 264; §90.6: 432, the headers and nodes 12 448, the page tables 720, the arena 25 792, the vertices 14 400) | 71 584 | 71 584 | 71 584 | 71 584 | 71 584 | 71 584 | 53 792 | 53 792 | 53 792 |
| the RAM sounds (stream_map_start) | 139 264 | 139 264 | 139 264 | 139 264 | 139 264 | 139 264 | 139 264 | 139 264 | 139 264 |
| the frame cache (stream_map_start) | 24 576 | 24 576 | 24 576 | 24 576 | 24 576 | 24 576 | 24 576 | 24 576 | 24 576 |
| the staging buffer (stream_map_start) | 32 768 | 32 768 | 32 768 | 16 384 | 16 384 | 16 384 | 16 384 | 16 384 | 16 384 |
| **the stream's pool** (stream_map_start) | 357 280 | 390 048 | 409 440 | 425 824 | 429 920 | 449 392 | 464 112 | 478 784 | 482 064 |
| the arena | 1 343 008 | 1 375 776 | 1 375 776 | 1 375 776 | 1 379 872 | 1 377 520 | 1 374 448 | 1 389 120 | 1 388 304 |

(The base's, §90.1's, §90.5's, §90.6's, §90.7's and §90.9's columns are their runs' logs;
§90.2 to §90.4 are those logs' blocks with what each lever changes, the
edicts and the touch lists at 341 slots, the staging buffer, the arena:
their pools agree with their runs' `bench zone` lines, 399 and 419 KB.)

### 90.3 The staging buffer: the music refilled by two chunks, 16 KB

The stream's staging buffer (`engine/stream.c` `STAGE_SECTORS`) is the
music's refill buffer too: a refill of `MUSIC_REFILL_CHUNKS` chunks a
channel, 2 × 4 KB each, so 16 sectors (32 KB) with refills of 4 (§84's
choice: 16 sectors every 1.3 s, fewer seeks on a console). With refills
of 2 the buffer is 8 sectors, 16 KB (its floor: a sound's pieces), the
drive asked every 0.65 s for 8 sectors. §84.4 had measured
`MUSIC_RING=4` (a shorter ring, whose rule gives 2-chunk refills) viable
under the drive model: 3728 late frames against 3485, the ring never
dry. Here the ring keeps its 6 chunks (2 s) and only the refill halves:
`Q1PSX_MUSIC_REFILL` 2 by default (CMakeLists.txt, `MUSIC_REFILL=` in
bench.sh, `engine/music.h`).

### 90.4 The stack: 12 → 8 KB

The stack's room (`engine/mem.h` `MEM_STACK_BYTES`, CMake
`Q1PSX_STACK_BYTES`, the bench's `STACK_BYTES=`) was the chosen 12 KB
since §81.7: every bench reaches 2.0-2.6 KB (`stack: N bytes never
reached` after each load, the fights' and the clears' lines), §61's
deepest 4.3 KB (E1M3's gold key scenario), the guard the lowest KB
(`mem_stack_ok` in `vid_flip`: a red screen when it is reached). At 8 KB
the margin past the guard is 4.4 KB over this branch's deepest load
(2 568 B) and 2.7 KB over §61's deepest. The 4 KB go to the arena
(`scripts/ramcheck.sh` reads the stack's room from the build).

**At 48** (`engine/ents.c` ENT_SPARE): the fights' and the clears'
gameplay lines are the base's line for line; the pools after the trim
leave E1M3's crowd fight 35 slots at its peak (306 of 341), E3M5's 46
(235 of 281), E1M8's 43 (162 of 205), E1M1 cleared 16 (177 of 193) and
E1M3 cleared 21 (320 of 341: 20 monsters killed with rockets in the
scenario's time, 22 alive, the bodies' 30 s); no `no free edict` in any
run. Every pool +18-19 KB (48 × 384 = 18 432 B): the smallest, E4M7's,
340 → 359 KB; E1M3's 380 → 399.

Left open: a rampage gibbing a dozen monsters within two minutes
(their backpacks live that long) would pass 48 and lose a gib, a backpack
or a delayed use, where 96 lost nothing; a reserve kept from the looks
(a gib or a meat spray refused when under a few free slots, the logic's
spawns never) would make 48 as safe as 96 for the maps' logic, for a
line in `throw_gib` and `spawn_meat_spray`, not done (id's code has no
such rule: its pool is 600).

**Measured** (`CDMODEL=1 MUSIC=1 ZONES=1`: the zones' walk of the 27
zoned maps with the music, every read held to a console drive's seeks,
§83): refills of 4 (`MUSIC_REFILL=4`) against 2, the same build
otherwise:

| | refills of 4 (32 KB) | refills of 2 (16 KB) |
|---|---|---|
| the ring run dry | 0 | 0 |
| chunk reads | 1193 (11.1 frames each) | 1195 (11.3) |
| frames waiting for the drive | 2129 | 2656 |
| late frames (the camera in a zone not in yet) | 3150 | 3293 |

The music never runs dry; the drive is asked twice as often for the
music (8 sectors every 0.65 s), the chunks wait a little more for it:
+143 late frames over 27 minutes of walking (+4.5 %), none of them a
dry ring. 16 KB to every pool.

(§90.3 and §90.4 change CMake cache defaults, `Q1PSX_MUSIC_REFILL` and
`Q1PSX_STACK_BYTES`: a build directory configured before them keeps 4 and
12288 in its cache; configure it again with `-DQ1PSX_MUSIC_REFILL=2
-DQ1PSX_STACK_BYTES=8192`, or start it afresh; `scripts/bench.sh` passes
both every time. The release's `ramcheck` line says which stack it has.)

**Measured at 8 KB:** the deepest point of every run on this branch stays
the loads' 2 568 B (`stack: 5624 bytes never reached` after each of the 32
maps), E1M3's crowd fight, E1M3 cleared and its gold key scenario
(`MONSTERS=6`: 2 128 B) under it, the guard never reached; the 128 views'
`H` lines identical; the release's arena 1 375 776 → 1 379 888 B (the stack
at 0x801fdff0); every pool +4 KB: E4M7's 359 → 363 KB (379 with §90.3's
16), E1M3's 399 → 403 (419).

**Batch 1 in all:** the release's arena 1 343 008 → 1 379 888 B (+36 880),
every pool +71 KB (32 + 18.4 + 16 + 4): the smallest, E4M7's, 308 → 379
KB; E1M3's 348 → 419; START's 631 → 702.

### 90.5 The edict: 384 → 320 bytes

`edict_t` (`engine/ents.h`) is the QC's entity, 384 bytes a slot (E1M3:
389 slots, 149 KB). The report's field table showed what could go
(`structures-octet-par-octet.md`); the layout now, 320 bytes (256 + 64:
the asm's index × size stays two shifts and an add, `e_tick.s`,
`e_seen.s`):

- the seven function pointers (think, think1, use, touch, blocked,
  th_die, th_pain: 28 bytes) are a byte each, an index into one table
  (`engine/ent_fns.h`: `ENT_FN_LIST`, every function an edict slot ever
  holds, 104 of them, `EF_<name>`; `ent_fns.c` the table, 420 bytes of
  rodata; the class files' functions are no longer static; a call is
  `ENT_CALL(e, slot)`, 20 sites; `sub_calc_move` takes a number; the
  kinds' tables (`mdef_t`) hold the numbers for th_pain / th_die): −20;
- `absmin` / `absmax` (24 bytes, SV_LinkEdict's world box, a unit wider,
  an item's 15 more across) are computed where needed (`ent_absbox`, from
  the origin and the local box: the link, the clip entries, the touch
  lists' boxes, the pushers' sweep, the AI's reach tests, a few bench
  spots; `e_touch.s` computes e's box on its frame for the area nodes'
  split tests, 24 instructions a call): −24;
- an item's `aflag`, `item_extra`, `item_model` are halfwords (amounts,
  a type, a model index), the word of padding gone: −10;
- the mover's `finaldest` (its move's end: `move_from + move_delta`,
  kept in every path of `sub_calc_move` now): the union 84 → 80: −4;
- `t_length`, `t_width` (the secret door's overrides from the map, which
  no id1 map gives and the cook does not keep; the plat's unread
  defaults) and `style` (set at the spawn, never read): −6.

What stays as it was: the four edict pointers (owner, enemy,
trigger_field, groundentity: 118 reads of `enemy` alone; indices would
be a wide rewrite for 8 bytes), the two strings, the vectors as words
(the asm reads them so), the monster's half of the union (80 bytes, now
the larger). 288 (256 + 32) would take the pointers as halfwords, the
velocity and movedir as halfwords and the strings as numbers; 256 the
union cut in two. Left open.

**Checked** (against the branch's previous commit built beside, the same
cooked data): the 128 spawn views' `H` lines identical, the game bench's
23, the C engine's 128 (`ENGINE_C=1`), E1M3's crowd fight and E1M3 cleared
line for line (the cycles aside), `MOVERS=1` (165 `H`, `bmfhash` and
`mvpath` lines identical), `ENTS=1` (83 lines: the doors', buttons',
plats', teleporters' and secrets' scenarios identical), `MONSTERS=1` on
E1M1 (every state line identical), the physics diffs (clipdiff,
wleafdiff, touchdiff 4000 boxes, pmdiff: 0 mismatches), the fight under
PCSX-Redux's interpreter (no exception: `e_touch.s`'s new loads). The
first build had the doors standing still: a comment of two lines had
swallowed the move delta's copy in `sub_calc_move` (the `ENTS=1` and
`MONSTERS=1` references caught it: a door's origin stayed at 0 while its
state went up and down).

**Measured.** E1M3's edicts 130 944 → 109 120 B (341 × 320, the slots
§90.2 leaves); every pool
+18-19 KB: E4M7's 379 → 397 KB, E1M3's 419 → 438, START's 702 → 710 (its
158 slots); the code +2 368 B (the table's 420 B of rodata, the box
computed at 19 sites of ents.c inline, `ENT_CALL`'s loads; with the box
a call everywhere the code grows 448 B only, for the cycles below +0.6
point), the release's arena 1 379 888 → 1 377 520 B. The cycles: the box
computed where it was read (three times at a link: the link's own, the
touch slot's, the clip entry's; both boxes of every touch pair; 24
instructions a touch walk in `e_touch.s`) cost E1M3's crowd (47 monsters
awake) +3.3 % of the entities' tick (194 129 → 200 570 cycles a tick),
the fight's frames +0.06 %, E1M3 cleared +0.22 %, the 128 views +0.10 %
(the brush models' box tests each frame). Keeping `absmin` / `absmax`
would give those cycles back for 24 bytes an edict (344, or 352 for the
asm's two shifts and an add): a call left open.

### 90.6 The brush models' rooms: one cut arena, sized by the PVS

Until now every drawn brush model had its own room for its pieces for the
whole level (§46: the cook's measure of the most its cuts take anywhere on
its travel; E1M3: 57 models, 49.9 KB of caches + 20.8 KB of vertices),
though a camera sees a few of them at once. Now the rooms are shared:

- **The cook** (`tools/qcook/bmf.c` `arena_of`, `box_leaves_into`) notes,
  while it measures each model's room at every place of its travel, the
  leaves its pieces fall in and the leaves the engine would draw it from
  without pieces (its box's, `ent_link`'s way: eight at most, every leaf
  under its top node when they are cut short), a bitset a model; then for
  every leaf with a PVS row (the world's, not solid: in a wall the camera
  sees everything and the models are drawn whole there) it sums the rooms
  of the models whose set touches a leaf the row sees, each room in whole
  pages of 64 bytes as the engine gives it; the largest sum, plus the
  largest single room (one more model always fits), goes into the header
  (`qw_header` `bmf_arena_recs`: records and groups, bytes;
  `bmf_arena_verts`: vertices), magic QWVF → QWVG, QWZG → QWZH. Over the
  32 maps the arenas come to 65 % of the rooms (625 152 B + 44 000
  vertices against 957 248 B + 67 288); the maps whose rooms are few are
  given them all (START, E1M7, END: the sum plus the largest room
  reaches the total).
- **The engine** (`engine/bmfrag.c`): `bmf_reserve` keeps a header and the
  compact BSP (`bmf_node`s) per drawn model for the level, and takes one
  arena of records and groups and one pool of vertices, the cook's sizes
  (never past the rooms' total, never under the largest room), in pages
  of 64 bytes, a bit and an owner byte a page. Each frame, before any
  model is drawn (`ents_render_bmodels`), every model the frame draws
  (the PVS sees it, the eye faces its box) is marked as the frame's
  (`bmf_keep`), and when one of them has no room each such model takes
  it (`bmf_prepare`, `bmf_take`): the lowest free run; else, while the
  free pages are too few, the resident model not drawn this frame that
  was drawn the longest ago gives its room up (`bmf_release`), then the
  blocks are packed down (`bmf_compact`: four words a turn; a vertex
  block that moves has its records' indices renumbered; nothing of the
  frame's lists points into the blocks yet) and the free pages are one
  run. A model given room is cut where it stands at its draw
  (`bmf_draw`); one given up is cut again when it is drawn next; one
  that can get no room (the frame's models hold it all: more than the
  cook's sets gave its leaf) is drawn whole that frame, its box's way, as
  a model too cut up is. At the load the models the spawn's leaf's PVS
  sees are cut first, then the others while there is room (`bmf_load`);
  `bmf_seen` answers by the box for a model without pieces; the frame
  count's wrap clears the marks (`bmf_wrap`, r_frame). The cut itself,
  the records' and the vertices' layout and the render lists are
  untouched. The PVS row's decoding, the AI's and the load's, is one
  (`world_vis_row`). The bench counts the rooms taken and given up and
  the draws without room (`bmf_stats[7..9]`: the `bmf arena` lines of the
  fights, the clear and the walk; a load's `models without room`).

Two designs were tried first. Each model's room given at its first draw
and taken from the oldest drawn when none is free, without packing: a
mover's room came back fragmented and a window of whole pages was rare
(E1M3's doors drawn whole). A window of pages held by the fewest models
not drawn this frame, the blocks packed down first: a model marked as the
frame's while the list was walked could still lose its room to the next
in the list, and a window had to skip every block of the frame's, so a
view whose models filled the arena found none (E1M7's spawn: one model
whole). Marking every model of the frame before any takes room, and
giving up the longest undrawn until the pages suffice before packing, is
what holds: the cook's bound is a sum, and after packing the free pages
are that sum's rest in one run. Marking every frame (a pass over the
visible models with the in-front test, the mark a call) cost the
movers' entities +0.5..+1.4 % and E3M4's walk +3.0 %; the pass runs only
in a frame with a model in front without room now (`bmf_resident`, the
cache's byte read in ents.c), the draws marking as before.

**Checked** (the previous commit built beside for the `MOVERS=1`,
`ENTS=1` and `MONSTERS=1 MAP=E1M1` references; the cooked data anew,
QWVG): the 128 spawn views' `H` lines identical, the C engine's 128 the
same, the game bench's 23, `MOVERS=1`'s 165 (the three movers cut as
often, 599, 595 and 419 times, the cuts' cycles the same), `ENTS=1`'s
83, `MONSTERS=1`'s state lines, E1M3's crowd fight and E1M3 cleared line
for line (the cycles aside); no draw without room in any run (the views,
the fights, the clear, the walk's 27 maps).

**Measured.** E1M3's brush models 71 584 → 53 792 B (the model table 432,
the headers and nodes 12 448, the page tables 720, the arena 25 792, the
vertices 14 400); the release's arena 1 377 520 → 1 374 448 B (the code
+3 072 B: the allocator, the packing, the load's two passes). The pools,
each lever at its release's arena (the table of §90.11): E1M3's 438 →
453 KB, E4M7's 397 → 418, START's 710 → 706; +10.1 KB a map on average
(E3M2's +24 the most), the maps given every room losing the page tables,
the rounding and the code's 3 KB (START, E1M7, E1M8 −4, END −3, E3M6
−1). At the loads 448 models of 33 had no room (up to 34 a map): the
rest of the arena past the spawn's view; they take it at their first
draw. The cost: the walk's frames +0.50 % over the 27 maps (E3M4's
+2.0 %: 137 rooms taken, 106 given up), E1M3 cleared +0.9 % (60 taken,
29 given up, 145 cuts), E1M3's crowd fight the same to 0.01 % (33 taken
at its first frames, none given up), the movers' frames +0.2..+0.8 %
(the in-front test of a model without room each frame, `bmf_draw`'s
mark);
the walk takes 1276 rooms over its 27 maps (683 given up, 1276 cuts,
59.7 M cycles in all, the dearest 481 344), its late frames 2314 → 2263
(the pools). The walk's PC samples (`ZONES=1 SAMPLE=1`, new: the walk and
the movers sample like the crowd) put the packing at 0.7 % of E3M4's
walk when it looked the blocks up model by model (every model scanned
for the lowest block, 63 × 63 a pass): walking the owner bytes once took
E3M4's +3.3 % to +2.0 %; the cuts themselves are 0.05 %.

Three of the branch's bugs were a replacement's slip: a comment of two
lines closing on the next line swallowed the statement between (the move
delta's copy in `sub_calc_move`, §90.5; the no-room return of `bmf_draw`
here, which then cut into a null pointer and wrote the kernel's vectors),
and a line left over before a rewritten condition nested the re-cut of
`bmf_draw` under "no room", so a moved door kept its old pieces
(`MOVERS=1`: 0 cuts). The `ENTS=1`, `MONSTERS=1` and `MOVERS=1`
references and the kernel-write breakpoint under PCSX-Redux's interpreter
(`PCSX.addBreakpoint(0x80000000, 'Write', 0x10000, ...)`, the Lua in the
session's scratchpad) found them.

### 90.7 The loads' code as an overlay at the arena's top

The code that runs only at the boot's inits and at a level's load sat in
the executable for the whole game. Now it is a **load overlay**: linked
at the level arena's top, under the stack, read there from the disc
before it runs, and left to the stream's pool for the rest of the level.

- **What goes in** (`engine/load.ld`, one input section a function):
  the functions only `main`'s boot and `load_level` reach, found with a
  call graph over the objects (every `R_MIPS_26` call, every address
  taken by `HI16` / `LO16` in code or `R_MIPS_32` in data; the scratchpad's
  script, kept as `scripts/ovlcheck.py` at the link): `world_load` and
  its texture upload, `bmf_reserve` with the models' BSP builders,
  `ents_load`, `ents_trim`, `monsters_map_start`, `ai_map_start`,
  `stream_init`, `stream_level`, `stream_map_start`, `snd_init`,
  `snd_map_start`, `snd_stop_all`, `zone_map_start`, `zone_preload`,
  `gdata_load`, `r_init`, `r_bind_world`, `dl_init`, `game_init`,
  `music_init`, `music_level`, `mdl_reset`, `menu_init`, the disc's
  listings `cd_list_dir` / `cd_list_dirs` with `world_scan_maps`,
  `world_level_mask`, `mem_game_alloc`, the loading screen and its poll,
  the load's body `load_level_main`, and the SDK's `SpuInit` and
  `InitGeom`: 36 functions, 15 016 bytes in the release. What stays:
  the respawn's spawners (`ents_spawn`, `movers_spawn`, `items_spawn`,
  `triggers_spawn`, `monsters_spawn`, `game_spawn_level`, 8.9 KB:
  `game_restart_level` runs them in play), what runs before the drive is
  up (`vid_init`, `pad_init`, `prof_init`, `cd_init` and their SDK
  callees) or before the overlay is read (`stream_abort`, `zone_abort`,
  `cd_cache_dir`, `load_level`'s own prologue), `settings_defaults`
  (the menu's reset), `stream_preload` (the bench's residency scenario
  calls it at every view), `menu_init` (the game bench's), the menu
  itself (3.4 KB: it runs over the paused game, the pool full).
- **Where** (`engine/q1psx.ld`): an output section `.text.load` at
  `OVL_BASE`, the stack's bottom less `OVL_ROOM` (32 KB; CMakeLists.txt's
  `--defsym`s from the stack's size), written first in the script so that
  `.text`'s wildcard leaves its input sections to it, `.text` pinned to
  the region's origin after it; `(INFO)`: not allocated, in no segment,
  so the SDK's elf2x leaves it out of the EXE (the EXE 315 392 → 299 008
  B) and `_end` moves down; the link asserts the overlay fits its room.
  The file is the section's bytes dumped (`objcopy --dump-section`),
  `LOAD.BIN`, put in the root for the boot and first in every level's
  directory (`scripts/cd_image.cmake`: the load's first read, no seek).
- **When** (`engine/main.c` `ovl_load`): at boot right after `cd_init`,
  before any init that lives in it; at a level's load right after the
  directory's cache, before `load_level_main`: `cd_find`, one read of its
  sectors to `__load_start`, `FlushCache`; a size other than the link's
  or a failed read is fatal. The arena's bump allocations stay under it
  (the world's cap, then the level's blocks: 400 KB and more of pool
  under it on every map); `stream_map_start` takes the pool over the
  whole rest but clips its end to `__load_start` until `stream_load_done`
  (after `stream_preload`, the last of the load; the 2D gun's lent block
  at the pool's real end is written after it), so the preload cannot
  reach the code it runs from.
- **The check** (`scripts/ovlcheck.py`, at every link): each overlay
  function is called by `main`, by `load_level` or by another overlay
  function only, through direct calls, and none has its address taken but
  the loading screen and its poll (the load passes them on). It found
  `settings_defaults` (the menu's reset), `stream_preload` and
  `menu_init` (the benches) on the way; a function that gains a caller in
  play fails the build.

**Checked**: the 128 spawn views' `H` lines identical, the C engine's
128, the game bench's 23, `MOVERS=1`'s 165, `ENTS=1`'s 83, `MONSTERS=1`'s
state and `H` lines, E1M3's crowd fight and E1M3 cleared line for line
(the cycles aside), the walk's 27 maps (the same late frames, 2263); the
boot and every load through the overlay in all of them (a wrong read is
fatal, on the screen and the console); the release's disc image with
LOAD.BIN in the root and in the 32 level directories.

**Measured.** The release's arena 1 374 448 → 1 389 120 B (+14 672: the
overlay's 15 016 less `ovl_load`, `stream_load_done` and the split's
344); every pool +14 KB at the release's arena (E1M3's 453 → 467 KB,
E4M7's 418 → 432, START's 706 → 720; the table of §90.11), E1M3's blocks
unchanged but the pool, 464 112 → 478 784 B (§90.10). A load reads 8
sectors more (LOAD.BIN, 15 016 B, first in the level's directory: no
seek of its own; the boot's from the root). The frames and their cycles
are the same, the code moved, not changed: the walk's 27 maps 10 071 664
→ 10 070 607 cycles a frame summed, E1M3 cleared 6 480 597 → 6 480 590.

### 90.8 Nodes and leaves on 16-bit indices: measured, not built

The lever as planned: a node 36 → ~20 bytes, a leaf 28 → ~16, pointers
rebuilt on the fly from 16-bit indices in the walk, the marking and the
other descents, −55 to −70 KB a map. The structures (`formats/qworld.h`)
and what reads each field, by the link's call graph and the asm:

| node field | bytes | read by |
|---|---|---|
| `c[3]`, `h[3]` (the box, vertex units) | 12 | the walk's frustum test (`lwc2` at +0 and +8, `lh` the z of each: r_world.s), the dynamic lights' reach (dlight.c), the box walks (world.c) |
| `visframe` | 2 | the walk's gate, the marking's stamp |
| `plane3` | 2 | every descent (the walk, w_leaf.s, clip.s, bmfrag.s, rl_order.s, the C) |
| `parent` | 2 | the marking (the ancestors stamped from each visible leaf) |
| `dlstamp` | 2 | the walk's dynamic-light gate |
| `child[2]` | 8 | every descent: pointers with the leaf flag in bit 0 |
| `faces[2]` | 8 | the walk's record loop (the block on the camera's side, its end at the next node's `faces[0]`), the zone switch (`zone_patch` writes them for the nodes whose records a chunk holds) |

| leaf field | bytes | read by |
|---|---|---|
| `c[3]`, `h[3]` | 12 | the walk's leaf test (its render list, pieces and models), `mark_pvs_far` (the draw distance's reach), the box walks |
| `visframe` | 2 | the walk, the marking, the entities' leaves (e_seen.s) |
| `num_marks`, `marks` | 6 | the walk (the records stamped from a visible leaf) |
| `vis` | 4 | the marking (its row; the top byte the leaf's render list) |
| `parent` | 2 | the marking |
| `contents`, `light` | 2 | the traces, the models' light |

**What 16-bit indices give.** The children: 4 bytes a node (an index
with the leaf flag, the pointer rebuilt by a shift and an add: two
instructions more a descent, in the walk's hottest chain and in
clip.s's point and box walks, bmfrag.s's cut and rl_order.s, ~0.3 % of
a frame by the walk's 250-400 nodes). The leaf's `marks`: an index into
the marks table (under 65 536 marks a map: 7 397 the most, E4M7), 2
bytes; its `vis`: an offset into the rows (under 64 KB a map since
§90.9: 39 465 B the most, E3M5), 2 bytes; `num_marks` could go too (the
next leaf's start ends this one's) but the 22 bytes left pad to 24 (the
`lwc2` loads want the box word-aligned). The node's `faces[2]` cannot
be indices: the records of a patched node live in whichever zone slot
holds its chunk (§57, §82), so the walk needs an address there, and the
patch rewrites it; the back block's start can be the front's plus a
16-bit length (6 bytes for the two), nothing less without a table the
record loop would read at every node. The stamps and the parent stay.
So a node comes to 32 bytes (30 padded) and a leaf to 24: −4 bytes
each, 7 to 20 KB a map (E1M3 15.4 KB, E4M7 20.1 KB, E3M6 19.7 KB, E1M7
4.5 KB; the per-map counts below), a quarter of the lever's promise.

**What 20 and 16 would take.** The boxes: 12 of the 36 and 28 bytes.
Stored as eighths of the parent's box (a byte a coordinate: 6 bytes a
node and a leaf), the walk would rebuild each box from its parent's
kept on the walk stack: six multiplies and the GTE loads from registers
instead of `lwc2`, 20-25 cycles a node and a leaf visited, 2-3 % of a
frame; the far marking and the dynamic lights would rebuild them too.
With the stamps moved to tables of their own (no gain: the same bytes
elsewhere) a node would be 20-22 bytes and a leaf 12-14. That is the
lever's promise, at a cost the constant 60 fps rule does not allow
without an explicit decision.

| map | nodes | leaves | −4 B each (KB) | boxes to a byte too (KB) |
|---|---|---|---|---|
| START | 2786 | 1130 | 15.3 | 38.2 |
| E1M1 | 2730 | 1150 | 15.2 | 37.9 |
| E1M3 | 2877 | 969 | 15.0 | 37.6 |
| E1M6 | 2416 | 650 | 12.0 | 30.0 |
| E1M7 | 832 | 283 | 4.4 | 10.9 |
| E3M4 | 3598 | 1168 | 18.6 | 46.5 |
| E3M5 | 3466 | 1425 | 19.1 | 47.8 |
| E3M6 | 3511 | 1405 | 19.2 | 48.0 |
| E4M7 | 3588 | 1438 | 19.6 | 49.1 |
| the 32 maps | 86 763 | 32 171 | 464.6 (14.5 a map) | 1 162 (36.3 a map) |

Not built: the 16-bit layout is a rewrite of every descent in the asm
(r_world.s, w_leaf.s, clip.s, bmfrag.s, rl_order.s, zone.s, e_seen.s)
and the cook's writer and zone cutter for −4 bytes a node and a leaf;
the box compression is a call on cycles. Either is a lever of
its own after this batch.

### 90.9 The PVS rows as deltas, decoded at the marking

Quake's PVS rows (a bit a leaf, a zero byte followed by its run's length)
take 6 to 50 KB a map in the skeleton (E1M3 25.5 KB, E3M5 50.0, E4M7
42.4, START 34.2; 929 KB over the 32 maps). Rows of nearby leaves differ
in a few bits; measured over the cooked worlds (the scratchpad's scripts
before the cook was changed): every row stored as the XOR against the
previous leaf's row, RLE'd, would take 618 KB (−33 %), against the best
of the previous eight rows 545 KB (−41 %), but a marking would then
decode a chain of rows. Chain-free: a row either plain or the XOR
against a plain row of a leaf before it, the plain rows the anchors,
chosen greedily among the last four plain rows within 255 leaves: 742 KB
(−20 %), a marking decoding two rows at most; eight or thirty-two
anchors give no more. So:

- **The cook** (`tools/qcook/world.c`): each row of the world's leaves,
  in leaf order, decoded, XORed against each of the last four anchors
  within 255 leaves, and written as a byte `back` (0: plain; else the
  anchor is the leaf `back` before) followed by Quake's RLE of the row or
  of the smallest XOR; a plain row becomes the newest anchor. Every row
  is then decoded back and compared with Quake's (the cook dies on a
  difference). Rows over 256 bytes (2 048 leaves) are refused: the
  engine's buffer. `qw_vis_decode` (qcook.h) decodes a cooked row for the
  cook's other readers (the cut arena's sums, bmf.c). Magic QWVG → QWVH,
  QWZI for the skeleton; a `PVS rows` line per map (E1M3's: `PVS rows:
  25516 B -> 19897 B, 859 of 967 rows the XOR against an earlier plain
  row`).
- **The engine**: the marking (`r_world.s` `mark_pvs`, `mark_pvs_far`)
  expands the camera leaf's row into `r_vis_row` (render.c, 256 bytes)
  first (`vis_expand`: a plain row from its RLE; a delta's base row, the
  leaf `back` before it, then the delta XORed in, its runs skipped; no
  row: every bit), then stamps the visible leaves and their ancestors
  from the buffer, a byte at a time (the run-skipping of the old loop
  gone: the zeros are in the buffer). `world_vis_row` (the AI's client
  check, the cut arena's load) decodes the same way in C; the AI's
  `row_has` (a monster's sight through the PVS) reads one byte of the
  row and of its base, walked to and no further, as before.

**Checked**: the 128 spawn views' `H` lines identical, the C engine's
128, the game bench's 23, `MOVERS=1`'s 165, `ENTS=1`'s 83, `MONSTERS=1`'s
state and `H` lines (the AI's sight through the rows: `pvs_sees`), E1M3's
crowd fight and E1M3 cleared line for line (the cycles aside), the walk's
27 maps (the late frames the same); the cook's own decode of every row.

**Measured.** The rows: 928 627 → 741 893 B over the 32 maps (−20.1 %;
E3M5 49 973 → 39 465, E4M7 42 378 → 32 751, START 34 235 → 26 553, E1M3
25 516 → 19 897, E1M7 6 042 → 4 969); the deltas 859 of E1M3's 967 rows,
1 297 of E4M7's 1 436. E1M3's skeleton 274 432 → 270 336 B (the file
in whole sectors: two fewer), its pool 478 784 → 482 064 B (§90.10);
every pool +4.7 KB a map on average at the release's arena (E4M7's 432 →
441 KB, E3M5's 586 → 595, E1M3's 467 → 470, START's 720 → 725, END's
−1: its rows were 5.7 KB; §90.11); the release's arena 1 389 120 →
1 388 304 B (the expansion's code and the buffer). The marking: the
row expanded first, about 1 300 instructions more on E1M3's 121-byte
rows at a leaf change (2 000 on E4M7's 180), where the old loop skipped
the runs; the walk's 27 maps show +0.09 % on the frames' cycles, within
the builds' noise, their late frames the same (2263), the views' first
frames the same.

### 90.11 Every map's stream pool at each lever

`BENCH_MAPS=1 VIEWS=0 RESIDENCY=1 LEVEL_CAP=<the lever's release arena>`:
the `bench zone` lines' pool, KB (§90.3's run is with §90.4's; §90.8 was
measured, not built; the smallest pool is E4M7's throughout).

| map | base (795bfb2) | §90.1 | §90.2 | §90.3-4 | §90.5 | §90.6 | §90.7 | §90.9 |
|---|---|---|---|---|---|---|---|---|
| the release's arena (B) | 1 343 008 | 1 375 776 | 1 375 776 | 1 379 872 | 1 377 520 | 1 374 448 | 1 389 120 | 1 388 304 |
| START | 631 | 663 | 682 | 702 | 710 | 706 | 720 | 725 |
| E1M1 | 578 | 610 | 629 | 649 | 659 | 672 | 686 | 690 |
| E1M2 | 466 | 498 | 517 | 537 | 551 | 555 | 569 | 576 |
| E1M3 | 348 | 380 | 399 | 419 | 438 | 453 | 467 | 470 |
| E1M4 | 438 | 470 | 489 | 509 | 526 | 539 | 553 | 560 |
| E1M5 | 463 | 495 | 514 | 534 | 552 | 567 | 581 | 584 |
| E1M6 | 549 | 581 | 600 | 620 | 636 | 648 | 662 | 664 |
| E1M7 | 988 | 1020 | 1039 | 1059 | 1065 | 1061 | 1075 | 1076 |
| E1M8 | 773 | 805 | 823 | 843 | 854 | 850 | 865 | 866 |
| E2M1 | 525 | 557 | 576 | 596 | 610 | 622 | 636 | 641 |
| E2M2 | 491 | 523 | 542 | 562 | 574 | 584 | 598 | 608 |
| E2M3 | 504 | 536 | 555 | 575 | 589 | 596 | 610 | 615 |
| E2M4 | 350 | 382 | 400 | 420 | 439 | 454 | 468 | 477 |
| E2M5 | 542 | 574 | 593 | 613 | 628 | 639 | 653 | 656 |
| E2M6 | 372 | 404 | 423 | 443 | 466 | 476 | 491 | 494 |
| E2M7 | 358 | 390 | 409 | 429 | 451 | 464 | 478 | 485 |
| E3M1 | 560 | 592 | 611 | 631 | 644 | 658 | 672 | 679 |
| E3M2 | 609 | 641 | 660 | 680 | 694 | 718 | 733 | 734 |
| E3M3 | 633 | 665 | 684 | 704 | 717 | 733 | 747 | 748 |
| E3M4 | 363 | 395 | 414 | 434 | 451 | 471 | 485 | 488 |
| E3M5 | 479 | 511 | 530 | 550 | 565 | 572 | 586 | 595 |
| E3M6 | 454 | 486 | 505 | 525 | 543 | 542 | 556 | 561 |
| E3M7 | 583 | 615 | 634 | 654 | 667 | 674 | 688 | 691 |
| E4M1 | 528 | 560 | 579 | 599 | 615 | 621 | 635 | 642 |
| E4M2 | 537 | 569 | 588 | 608 | 623 | 633 | 647 | 653 |
| E4M3 | 355 | 387 | 406 | 426 | 446 | 462 | 476 | 481 |
| E4M4 | 494 | 526 | 545 | 565 | 586 | 587 | 601 | 610 |
| E4M5 | 405 | 437 | 456 | 476 | 497 | 519 | 534 | 537 |
| E4M6 | 551 | 583 | 602 | 622 | 639 | 653 | 667 | 670 |
| E4M7 | 308 | 340 | 359 | 379 | 397 | 418 | 432 | 441 |
| END | 904 | 936 | 955 | 975 | 985 | 982 | 996 | 995 |
| mean | 521 | 553 | 572 | 592 | 607 | 617 | 631 | 636 |

### 90.12 The pools by the zone cut: 64 or 96 KB a zone, 3 or 4 slots

The same runs as §90.11 (`BENCH_MAPS=1 VIEWS=0 RESIDENCY=1`, the release's
arena: 1 343 008 B before the batch, 1 388 304 B after it) over the four
cuts of the PVS zone tables at the 1536 draw distance (`Q1PSX_ZONE_TABLES`
pvs_1536_64 / pvs_1536_96: every record the PSX could draw from the zone,
hidden or not; the `seen` tables are not used, their chunks miss records),
three or four chunk slots (`Q1PSX_ZONE_SLOTS`). The pool, KB, before and
after the nine levers; "before" is master 795bfb2, "after" 90b52bd.

The cook keeps a map whole when its skeleton and slots would take as
much RAM as the whole world (`tools/qcook/zones.c`). A PVS zone's chunk
is the union of its leaves' PVS needs, so a 64 KB cap cuts the maps into
many zones (E1M1 145, E3M6 129, START 55) whose largest chunk still runs
past the cap (E1M1's slots 90 KB each, E3M6's 110): with 64 and four
slots 17 of the 32 maps stay whole (E1M1, E1M3, E1M5, E1M6, E1M7, E1M8,
E2M2, E2M6, E3M2, E3M3, E3M6, E3M7, E4M1, E4M2, E4M3, E4M6, END), with
64 and three 5 (E1M6, E1M7, E1M8, E3M2, END), with 96 and four 30 (every
map but E4M4, E4M7), with 96 and three 12 (E1M5, E1M6, E1M7, E1M8, E2M6,
E3M2, E3M3, E3M6, E3M7, E4M3, E4M6, END). A whole map's pool is the
world's rest, the same under every cut.

The batch's gain is the same under every cut, +88 to +133 KB a map (the
arena's +45 KB, then the map's blocks): the cut and the slots pick the
base, the levers add to it.

The smallest pool after the batch: 64/4 E4M7's 399 KB (before 268); 64/3
E4M7's 471 KB (before 340); 96/4 E4M7's 313 KB (before 180); 96/3 E3M4's
402 KB (before 274). The mean over the 32 maps: 64/4 478 -> 593; 64/3
532 -> 647; 96/4 459 -> 575; 96/3 487 -> 602.

| map | 64/4 before | 64/4 after | 64/3 before | 64/3 after | 96/4 before | 96/4 after | 96/3 before | 96/3 after |
|---|---|---|---|---|---|---|---|---|
| START | 581 | 677 | 657 | 753 | 573 | 667 | 605 | 699 |
| E1M1 | 518 | 632 | 570 | 682 | 518 | 632 | 530 | 644 |
| E1M2 | 416 | 528 | 494 | 606 | 410 | 522 | 448 | 558 |
| E1M3 | 318 | 442 | 382 | 504 | 318 | 442 | 328 | 450 |
| E1M4 | 388 | 510 | 464 | 586 | 336 | 458 | 424 | 546 |
| E1M5 | 425 | 548 | 465 | 586 | 425 | 548 | 425 | 548 |
| E1M6 | 549 | 664 | 549 | 664 | 549 | 664 | 549 | 664 |
| E1M7 | 988 | 1076 | 988 | 1076 | 988 | 1076 | 988 | 1076 |
| E1M8 | 773 | 866 | 773 | 866 | 773 | 866 | 773 | 866 |
| E2M1 | 475 | 593 | 553 | 671 | 441 | 559 | 465 | 581 |
| E2M2 | 389 | 506 | 433 | 550 | 389 | 506 | 435 | 552 |
| E2M3 | 464 | 575 | 540 | 651 | 428 | 539 | 480 | 591 |
| E2M4 | 294 | 419 | 372 | 497 | 230 | 357 | 318 | 445 |
| E2M5 | 494 | 608 | 570 | 684 | 454 | 568 | 516 | 630 |
| E2M6 | 320 | 444 | 384 | 506 | 320 | 444 | 320 | 444 |
| E2M7 | 310 | 437 | 386 | 513 | 256 | 383 | 296 | 423 |
| E3M1 | 496 | 613 | 576 | 693 | 486 | 605 | 504 | 623 |
| E3M2 | 609 | 734 | 609 | 734 | 609 | 734 | 609 | 734 |
| E3M3 | 609 | 726 | 625 | 740 | 609 | 726 | 609 | 726 |
| E3M4 | 323 | 448 | 397 | 522 | 271 | 396 | 277 | 402 |
| E3M5 | 423 | 539 | 501 | 617 | 323 | 439 | 399 | 517 |
| E3M6 | 322 | 429 | 386 | 493 | 322 | 429 | 322 | 429 |
| E3M7 | 535 | 643 | 557 | 665 | 535 | 643 | 535 | 643 |
| E4M1 | 464 | 578 | 544 | 658 | 464 | 578 | 510 | 624 |
| E4M2 | 483 | 599 | 551 | 669 | 483 | 599 | 505 | 621 |
| E4M3 | 351 | 475 | 389 | 515 | 351 | 475 | 351 | 475 |
| E4M4 | 390 | 504 | 482 | 596 | 382 | 498 | 476 | 592 |
| E4M5 | 389 | 521 | 457 | 589 | 357 | 489 | 367 | 499 |
| E4M6 | 545 | 664 | 591 | 710 | 545 | 664 | 545 | 664 |
| E4M7 | 268 | 399 | 340 | 471 | 180 | 313 | 274 | 407 |
| END | 904 | 995 | 904 | 995 | 904 | 995 | 904 | 995 |
| mean | 478 | 593 | 532 | 647 | 459 | 575 | 487 | 602 |

## §93 — The zone cut per level (2026-10-29)

In play with the PVS zone tables (`pvs_1536_64`, three slots;
the `seen` tables are out: their chunks miss records the PSX draws),
chunks were found loading late: on E1M1 the zone past the secret's teleporter
(the yellow armour's, reached under the water) came in only once the
player had gone through it, and the underwater part only once in the
water; elsewhere too. He asked for every map's prefetch to be looked at
again, then, on the cut's numbers, for the cut itself to be redone,
variable per level and coherent (E1M1 in five chunks, ten at most).

### 93.1 What was wrong: the cap, not the prefetch

The prefetch (§57.2, §82) loads the camera's zone and its nearest by
walking distance, one a slot. Measured over the scan's eyes (`analysis/lead.py`: for every sampled eye, the zones within L units of
walking, teleporters a step, that its leaf's set does not hold), under
`pvs_1536_64` and own + 3 nearest, an eye has an unprefetched zone within
320 units on 24 % of the eyes over the 32 maps (E1M1 54 %, E1M8 93 %,
END 91 %, E3M6 70 %), within 160 units on 15 %; 2 nearest: 28 %, 4: 21 %,
5: 18 %. The prefetch rule is not the cause: the cut is. A PVS zone's
chunk holds every record the PSX could draw from the zone's leaves within
the draw distance, hidden or not (§57.8's `pvs` tables), and that need
is 43 KB for E1M1's median leaf, 82 KB for its worst (E1M8 59 / 94 KB,
START 36 / 71): a 64 KB cap is under one leaf's need, so the cut leaves
the leaves alone (E1M1 145 zones, E1M8 148, E3M6 129, START 55; 8.9 MB
of chunks for E1M1's 568 KB world), every zone has many neighbours
within a few steps, and three slots cannot hold them. The cap over a
leaf's need, the cut is sound and the lead comes back by itself
(`cutcurve.py`, own + 3 nearest):

| cap | E1M1 zones | its largest chunk | eyes with a zone within 320 u not prefetched, 32 maps (E1M1, E1M8) |
|---|---|---|---|
| 64 KB | 145 | 83 KB | 24 % (54, 93) |
| 96 | 10 | 91 | 0.9 % (0.1, 25.8) |
| 128 | 7 | 111 | 0.0 % (0, 0) |
| 160 | 4 | 149 | 0.0 % |
| 192 | 4 | 175 | 0.0 % |
| 256 | 3 | 203 | 0.0 % |

### 93.2 What zoning gives with PVS chunks

Little, and only with small chunks and few slots. The resident world
of a zoned map is its skeleton and `slots` chunks of the largest; a
chunk is at least a leaf's need and the cut fills every zone to the cap,
so three slots of 91 KB (cap 96) hold 273 KB against E1M1's 323 KB of
records: +50 KB for ten zones; cap 128 and over, or four slots, cost
more than the whole map. An arena holding the wanted chunks at their
real size instead of fixed slots (§57.8's alternative) was measured too
(`set max` in `cutcurve.py`): nothing, every chunk is near the cap. The
pool each cut leaves at the release's arena, with three slots (the
§90.12 runs' whole pools, the curve's chunks):

| map | whole | 96 KB | 128 | 160 | 192 |
|---|---|---|---|---|---|
| E1M1 | 633 | 683 (10 z) | whole | whole | whole |
| E1M4 | 458 | 567 (7) | 486 (4) | 486 (4) | whole |
| E2M4 | 357 | 460 (12) | 373 (6) | whole | whole |
| E2M7 | 383 | 474 (9) | 390 (6) | whole | whole |
| E3M4 | 396 | 474 (8) | whole | whole | whole |
| E3M5 | 440 | 583 (8) | 493 (6) | whole | whole |
| E3M6 | 429 | 538 (12) | 466 (5) | whole | whole |
| E4M4 | 497 | 604 (17) | 523 (8) | whole | whole |
| E4M7 | 305 | 416 (10) | 335 (6) | whole | whole |

With four slots only E1M4, E2M4, E2M7, E3M5, E3M6, E4M4 and E4M7 gain
anything, 2 to 53 KB, at cap 96. Three slots were chosen.

### 93.3 The cut per level (the export's `auto` tables)

`analysis/export.py` (the QuakeSpasm tool, outside the repo: §56) has a
variant `auto:<slots>` writing `zones/auto_<D>_s<slots>/`: for each map,
the largest cap of `AUTO_CAPS` (256, 192, 160, 128, 96 KB) whose
resident world, the skeleton (the world file less the records and their
vertices) plus `slots` chunks of the largest, frees `AUTO_MIN_GAIN`
(32 KB) over the whole world; none: one zone, and the cook keeps the map
whole as before (§57.1). The largest chunk is judged after the leaves no
eye reached have added their records (`complete()`: the step the export
always did after the cut, now a function the choice runs per cap), since
they can grow a chunk past the cap (E3M6's 95 → 148 KB on the second
scan below). Two clean-ups on the way, for every variant: a zone whose
chunk is under 16 KB (a monster closet whose PVS is itself, the door
shut when vis ran: E2M1 had three of 0 to 5 KB) is joined to the zone
nearest by walking (`merge_small`), and the pieces of the voxel graph
whose leaves' PVS costs under 32 KB are dropped before the cut
(`Map.drop_islands`: an item's seed in a closet or an alcove the player
never enters; E4M7 had four zones of 0.5 KB from them). The leaf lines
(the three nearest zones by walking distance) and the cook (`qcook
zones`, §57.1) are unchanged; `local.env` points at `auto_1536_s3` with
`Q1PSX_ZONE_SLOTS=3`. `Q1PSX_SCAN` picks the scan folder (`analyse.py`);
`lead.py` and `cutcurve.py` (the measures of §93.1 and §93.2) sit in
`analysis/` beside them.

### 93.4 The scan's flood: out of the water, seeded where the player goes

The teleporter's own case was another flaw. The zone tables rank the
zones by walking distance over the scan's voxel graph, teleporters an
edge from the voxels whose player box touches the trigger to its
destination (§56); E1M1's scan had no teleporter edge: no reached voxel
touched the trigger. A probe added to `chunkscan.c`
(`CHUNKSCAN_PROBE="x y z0 z1"`: a column's voxel states after the flood)
showed why: the flood swims up through water voxels and climbs only
into voxels a floor holds (within 80 units below), and the air over a
liquid's surface has no floor within reach, so the flood never left the
water; the armour room's floor is 32 units over the last water voxel.
Quake's player gets out by the water jump (225 units a second up: 32
units) and keeps the 18-unit step while still touching the water. Three
changes to `Quake/chunkscan.c` (built with `make USE_SDL2=1`):

- **Out of the water:** from a water voxel, the one to three voxels
  above (48 units) are reached when empty and not water. E1M1's secret:
  577 eyes in the armour room, 43 teleporter edges.
- **Seeds where the player goes:** the items (`item_*`, `weapon_*`) and
  the player's teleporters (a `trigger_teleport` without a targetname;
  every empty voxel whose player box touches the trigger) seed the flood
  as the starts and the teleport destinations did: a place past a move
  the flood does not make (a shot door, a lift a button calls) is
  reached from there. A monster closet's teleporter pad lies in solid:
  no seed. Eyes +0 to +24 % a map (E1M8, E3M4 +18 %, E3M1 +15 %).
- **The trigger's reach:** the box the player's origin touches the
  trigger from is grown by half a voxel (8 units) more: a trigger 10
  units deep in a wall (E2M7's, E2M6's) was touched from no voxel
  centre.

The command line's map list is cut past about 28 maps (Quake's line):
`scan_all3.sh` scans in three runs. Teleporter edges in the scan, before / after (the maps that changed): E1M1 0 / 43,
E1M2 66 / 116, E1M3 2 / 27, E1M4 0 / 98, E1M5 17 / 165, E1M6 48 / 114,
E1M7 33 / 75, E1M8 0 / 12, E2M1 32 / 166, E2M2 53 / 83, E2M6 0 / 184,
E3M1 208 / 445, E3M2 60 / 221, E3M3 36 / 234, E3M4 168 / 523, E3M6 6 / 15,
E3M7 346 / 441, E4M1 34 / 46, E4M2 242 / 368, E4M3 87 / 1342, E4M4 38 /
667, E4M5 0 / 558, E4M6 10 / 450, E4M7 124 / 398, E4M8 40 / 154, START
338 / 499; the teleporters some reached voxel touches, 74 / 277 → 144 /
277 (`teleport.py`); the rest are monster closets' pads (E2M4's 8,
E2M5's 4, E2M6's 8: in solid, `CHUNKSCAN_PROBE` shows) and E2M7's one,
a 26 x 10 x 34 trigger in solid rock that no path reaches.

### 93.5 Measured

The export's `auto:3` tables from the third scan (`zones/auto_1536_s3`),
cooked with `Q1PSX_ZONE_SLOTS=3`, at the release's arena (1 388 304 B,
`LEVEL_CAP`): 17 maps zoned, 15 whole; `BENCH_MAPS=1 VIEWS=0 RESIDENCY=1`
for the pools, `ZONES=1` for the zones' walk (§57.6: 3600 frames a map,
the late frames those drawn with the camera's zone not in; the legs
through walls apart), against `pvs_1536_64` with three slots (27 maps
zoned; the walk's figures are the second scan's tables, the same cut),
`BENCH_MAPS=1 HASHONLY=1` for the 128 spawn views against the whole
worlds (`WHOLE=1`): the 128 `H` lines identical.

| map | pool whole | the cut: cap, zones | chunks KB | pool | reads KB auto / 64 | late frames (clear legs), longest run: auto / 64 |
|---|---|---|---|---|---|---|
| START | 667 | 96 KB, 7 | 37..91 | 699 | 1360 / 3223 | 0 (0), 0 / 206 (37), 24 |
| E1M1 | 633 | whole | - | 632 | - / 8072 | whole / 547 (42), 56 |
| E1M2 | 522 | 96 KB, 9 | 25..90 | 558 | 3210 / 5005 | 96 (0), 22 / 265 (0), 20 |
| E1M3 | 442 | whole | - | 442 | - / 1820 | whole / 116 (0), 15 |
| E1M4 | 458 | 96 KB, 7 | 52..83 | 558 | 2738 / 4779 | 150 (0), 20 / 369 (22), 47 |
| E1M5 | 548 | whole | - | 548 | - / 3482 | whole / 202 (12), 20 |
| E1M6 | 664 | whole | - | 664 | - / - | whole / whole |
| E1M7 | 1076 | whole | - | 1076 | - / - | whole / whole |
| E1M8 | 866 | whole | - | 866 | - / - | whole / whole |
| E2M1 | 558 | 96 KB, 16 | 20..98 | 599 | 4197 / 6145 | 199 (0), 23 / 509 (0), 22 |
| E2M2 | 507 | 96 KB, 9 | 29..96 | 564 | 2991 / 4661 | 119 (7), 22 / 212 (53), 28 |
| E2M3 | 538 | 96 KB, 11 | 51..92 | 591 | 3616 / 6211 | 132 (19), 22 / 436 (22), 41 |
| E2M4 | 357 | 96 KB, 11 | 30..102 | 415 | 3298 / 4209 | 149 (0), 29 / 203 (0), 21 |
| E2M5 | 568 | 96 KB, 10 | 31..91 | 630 | 2494 / 4331 | 95 (20), 21 / 288 (54), 44 |
| E2M6 | 444 | whole | - | 444 | - / 6056 | whole / 296 (76), 18 |
| E2M7 | 383 | 96 KB, 8 | 42..97 | 435 | 3405 / 3218 | 84 (7), 18 / 153 (21), 18 |
| E3M1 | 605 | 96 KB, 10 | 34..97 | 635 | 3923 / 7390 | 201 (21), 22 / 486 (67), 36 |
| E3M2 | 734 | whole | - | 734 | - / - | whole / whole |
| E3M3 | 726 | whole | - | 726 | - / 1739 | whole / 61 (0), 19 |
| E3M4 | 396 | 96 KB, 5 | 101..103 | 426 | 2917 / 3105 | 120 (0), 24 / 281 (97), 52 |
| E3M5 | 440 | 128 KB, 6 | 29..121 | 475 | 1979 / 2835 | 200 (0), 27 / 192 (28), 16 |
| E3M6 | 429 | 96 KB, 12 | 50..98 | 519 | 1970 / 6086 | 95 (0), 23 / 418 (42), 49 |
| E3M7 | 643 | whole | - | 643 | - / 3624 | whole / 169 (52), 19 |
| E4M1 | 579 | 96 KB, 9 | 49..91 | 624 | 4642 / 7167 | 252 (0), 41 / 453 (0), 49 |
| E4M2 | 599 | whole | - | 599 | - / 3058 | whole / 129 (35), 17 |
| E4M3 | 475 | whole | - | 475 | - / 2742 | whole / 128 (8), 16 |
| E4M4 | 497 | 96 KB, 16 | 37..95 | 580 | 5913 / 6577 | 353 (11), 34 / 543 (4), 44 |
| E4M5 | 489 | 96 KB, 6 | 53..90 | 511 | 1336 / 2042 | 43 (0), 22 / 66 (0), 14 |
| E4M6 | 664 | whole | - | 664 | - / 1237 | whole / 62 (0), 14 |
| E4M7 | 305 | 96 KB, 10 | 25..92 | 401 | 1628 / 2304 | 89 (0), 22 / 99 (0), 17 |
| END | 995 | whole | - | 995 | - / - | whole / whole |
| the 32 maps | mean 575 | 17 zoned | | mean 603 | 51617 / 111118 | 2377 (85) / 6889 (672) |

E1M1 stays whole, so its secret has nothing left to load; the walk's
late frames off the legs through walls go from 672 to 85 (E3M1 21,
E2M5 20, E2M3 19, E4M4 11, E2M2 7, E2M7 7: 7 to 16 frames at a time,
the drive busy with the stream's read), the reads from 111 to 52 MB
over the 32 maps (the switches from 921 to 319, the chunks 25 to 121
KB), E4M7's pool from 305 KB whole (or 416 at 64/3) to 401, E2M4's
from 357 to 415; the pools' mean 603 KB against 647 at 64 / 3 slots
(§90.12: that cut zoned 27 maps, with the late frames above). Two
things the zones still carry: the leaves no eye reached (300 to 1500
records a map on ten maps, E2M7 1490, E3M4 1513: the records go to the
zone sharing most of them that stays under the cap, else the one that
grows least) and E3M5's cap of 128 KB (its chunks at 96 would free
under 32 KB).

**Left to do:** the main checkout's `local.env` (it named
`seen_1024_64`): `Q1PSX_ZONE_TABLES=.../zones/auto_1536_s3/` and
`Q1PSX_ZONE_SLOTS=3`; at the 1024 draw distance the same export runs
with `D=1024` (`export.py`'s argument) from the same scan, the chunks
smaller. The DuckStation checks: TESTING 11.70.

## §91 — The world's walk cooked: a zone's sequence in its chunk (2026-10-28)

The ask, on START and E1M1 only (the two maps whose stream pool has
room), on a branch of its own, never merged: cook what the renderer
recomputes every frame of the world's walk, the visibility and the
painter's order per camera leaf, so that the engine runs a list instead
of the tree; then (§92) the near polygons' tessellation. The CPU is the
bottleneck and the walk about 15 % of a heavy view.

### 91.1 What the walk does, and what a camera leaf fixes

The tree walk (`engine/r_world.s`, `walk` to `pop`) takes, at each node
the PVS stamps, the camera's side of the node's plane (`d = n . cam -
dist`, behind when `d < 0`, no faces when `|d|` is under the backface
epsilon), the near subtree first, then the block on that side, then the
far subtree; a leaf stamps its marks (the faces the record loop then
draws) and draws its render list. The frustum culls the nodes' and the
leaves' boxes along the way, and each record's sphere in the loop.

For a camera in one leaf, the side of most planes is fixed: the leaf's
cell (the ancestor half-spaces) lies on one side of them. So the order
can be cooked once per zone: the cell of every camera leaf of the zone is
cut out (`tools/qcook/seq.c`, a box cut by the ancestors' planes, the
cooked ones), each node plane's distance range over the cells' corners
taken, and a node whose plane leaves every cell on one side by more than
the epsilon and a quarter unit gets that side's order once. The others
keep both orders (VAR): the planes that cross the zone, and the cells'
own boundary planes, since a camera right on one has `d = 0`, is behind
by the leaf lookup (`d <= 0`) and in front by the walk (`d < 0` is
behind), the order flipped for that frame. An axial boundary at a whole
vertex unit with the cells before it stays fixed all the same: `d` is a
multiple of 4096 there, never 0 nor within the epsilon. The rest is
left to the frame, from the same words the tree reads: the PVS stamps
(mark_pvs, unchanged, its ancestors' stamps the gates), the frustum
(the leaves' boxes, the nodes' the cook chose), the side of the VAR
planes, the records' spheres.

Not cooked, by choice: the per-leaf visible records (a mask on
the chunk's records). The draw distance's marking (`mark_pvs_far`) stamps
a leaf by the camera's own position and keeps its marks until the camera
has moved 64 units: the set drawn depends on where the camera was at the
last marking, not on its leaf alone, so a per-leaf set could not give the
same lists; and the visframe check costs what a mask bit would.

### 91.2 The words (formats/qworld.h "the cooked walk")
...

A chunk's `ofs_seq` (the old pad word of `qw_chunk`, 0 for the maps not
asked) points at a `qw_seq_header` and its words. An entry's type is in
bits 30-31; an offset is the node's or the leaf's byte offset from the
leaves' array, 24 bits signed (the nodes come first in the skeleton, so
theirs are negative), the one base register the interpreter keeps:

| word | what the renderer does |
|---|---|
| LEAF `0 | test << 29 | offset` | out of the PVS (its stamp), nothing; else its box against the frustum when `test` (culled: nothing), its marks stamped, its render list drawn |
| BLOCK `1 | side << 29 | offset` | the node's face block on `side` through the record loop, as the tree's `pop` sends it (`gp` the node when a dynamic light reaches it) |
| NODE `2 | test << 29 | offset`, then its subtree's words | out of the PVS: the subtree skipped; when `test`: the box against the frustum (culled: skipped), the planes it is inside of off for the subtree, which ends with an END that puts the flags back |
| VAR `3 | 0 << 28 | offset`, then F's words and B's << 16 | the camera's side of the node's plane as the walk takes it; in front: F (the front subtree's words, ending with an END), the front block, B (likewise); behind: B, the back block, F; no block within the epsilon |
| END `3 | 1 << 28`, STOP `3 | 2 << 28` | a frame's end; the walk's end |

The cook emits a NODE word where the subtree has `QCOOK_SEQ_PRUNE`
entries or more (a PVS gate), tested when its score (leaves x 100,
blocks x 10, records x 8) reaches `QCOOK_SEQ_TEST`; a VAR where the plane
crosses the zone; nothing else for a fixed node: its near subtree, its
block (when the chunk has records in it), its far subtree. The frames
the renderer keeps (a tested node's flags, a VAR's phase) go on the
walk's scratchpad stack; the cook bounds their nesting to
`QW_SEQ_MAX_DEPTH` (60), keeping room under a tested node for the VARs
below it, and gives a zone no walk when the VARs alone nest deeper.

### 91.3 The engine (engine/r_world.s seq_start, engine/zone.c, engine/render.c)

`r_state.seq` is the sequence to run this frame, or 0: zone.c sets it
(`seq_select`) whenever the drawn chunk or the camera's set changes: the
drawn zone's walk while the camera's leaf is one of that zone's leaves and
the chunk carries one, the tree walk otherwise (a late frame, the camera
in solid, the 30 maps cooked without). r_render reads it after the
marking and the dynamic lights' stamps, where the tree walk starts, and
jumps to `seq_start`: an interpreter of the words with the walk's own
registers (s1 the sequence, s2 the clip flags, s3 the frame stack, s0 the
leaves' array), each entry a load and a dispatch on its type.

The record loops exit by branches to `walk`, and draw_ents comes back by
a branch to `pop_1`: thirteen branch words (the three ways out of `dr`
and of its three options' copies, `ei_item`'s) are pointed at the
interpreter while a cooked walk runs and back at the tree when it does
not (`r_seq_set`: the 16-bit offsets rewritten, the cache flushed, on a
change only; the loops' first words that `r_set_options` turns into jumps
are left to it, their copies having sites of their own). A leaf's render
list is drawn through draw_ents as the tree draws it; the sorted models
and the brush models use s1, s2 and s0 as the walk's dead registers at a
leaf, so the interpreter keeps its three in RAM across it and takes them
back at `seq_ents_back`.

### 91.4 Why the lists are the tree's

The tree culls a subtree by its node's box; a leaf's box lies in its
ancestors', so a leaf whose box passes has every ancestor's passing: the
node tests prune, they never decide. A face is drawn when a leaf marking
it (one of its node's descendants, the leaf in front of it) passed and
stamped it, and its sphere passes: both tests are the interpreter's, from
the same words; the planes a node's test turns off are planes its
subtree is inside of, which cull nothing when tested again, and the
record loop's shortcut for a block fully inside (`_inside`) skips a
sphere test that cannot cull. So the records drawn and their order are
the tree's whatever nodes the cook chose to test, as long as the leaves
are tested (`QWSQ_EXACT`). The fast walk (`QCOOK_SEQ_FAST=1`) tests no
leaf: records whose sphere passes while every leaf of theirs is out of
the frustum go to the GPU too, off screen, the same pixels.

`QCOOK_SEQ_CHECK=1` runs the words through a C interpreter of the asm
for cameras in each cell (its corners pulled in, its centre), the PVS of
the camera leaf, no frustum, and compares the events (the leaves visited,
the blocks drawn) with the tree walk's from the same camera, step for
step.

### 91.5 What the cook says (START, E1M1; seen_1536_64, 4 slots, `QCOOK_SEQ_TEST=600`)

| map | zones | words (KB) | a zone at most | leaves | blocks | nodes gated (tested) | VAR | of them a cell's own boundary (behind, before) | camera leaves | frames deep | the biggest chunk | the pool |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| START | 11 | 24226 (94) | 3642 | 2526 | 620 | 2385 (1703) | 3649 | 62, 2554 | 1128 | 45 | 64632 -> 79216 B | 666 -> 591 KB |
| E1M1 | 19 | 44457 (173) | 4158 | 5394 | 1077 | 4941 (3257) | 6207 | 108, 2749 | 1148 | 57 | 66968 -> 78812 B | 614 -> 563 KB |

- The VAR planes are a third of the words and most of the frame's work
  (below): a zone of a hundred camera leaves is partitioned by a hundred
  planes at least, and the planes of its own boundaries that its cells
  lie before but not behind a whole vertex unit add the rest.
- `QCOOK_SEQ_CHECK=1`: 29344 cameras on START, 29045 on E1M1, 0 differ
  from the tree walk (the C interpreter, no frustum).
- The 30 other maps and a default cook (`QCOOK_SEQ=none`): the 81
  skeletons and chunk files byte-identical to master's cook (md5).

### 91.6 Measured (the bench build, PCSX-Redux, T 4, the 12 views of each map)

Every list identical to master's in the exact walk, whatever the
knobs (the `H` lines, and the dumps' files as a whole). The cycles:

| START, 12 views | frame | the walk (v0) | the walk (v10, a light view) |
|---|---|---|---|
| master 795bfb2 (the tree) | 5 815 178 | 113 635 | 65 537 |
| the cooked walk, exact, nodes tested from a score of 600, gates from 3 entries (VAR frames on the stack) | +9.1 % | 176 770 | 120 943 |
| the same, gates from 2 entries | +10.7 % | 189 781 | 130 437 |
| the same, nodes tested from a score of 200 | +10.2 % | 194 240 | 119 954 |
| the fast walk (no leaf box tests; 8 of 12 lists differ: off-screen records drawn, the pictures the same) | +15.0 % | 194 287 | 148 138 |
| the exact walk, the VAR's side kept in its word, no frame (the branch's) | +10.6 % | 170 473 | 112 866 |
| the branch with the walk off (`QCOOK_SEQ=none`, the default: the tree walk, the hook's three instructions a frame) | +0.0 % (5 815 329) | 113 635 | 65 568 |

| E1M1, 12 views (exact, 600, VAR frames) | frame | the walk (v0) | the walk (v4, the heaviest) |
|---|---|---|---|
| master | 4 584 935 | 50 942 | 175 347 |
| the cooked walk | +4.7 % | 79 260 | 198 146 |
| the branch with the walk off | +0.0 % (4 585 163) | 50 954 | 175 291 |

- A frame of START's v0 (the branch's walk): 1520 entries dispatched,
  347 VAR sides taken, 58 node boxes and 72 leaf boxes tested; the PC
  sampler puts the interpreter's labels at about 12 % of the frame's
  samples (seq_node, seq_hi, seq_next, sv_side, seq_block_n, sl_mark...)
  where the tree walk's share was about 14 % of a smaller frame.
- The record loop and the slow path read +2..4 % in the runs whose
  chains the cache order listed first: the interpreter's chains took
  places of the traced order (engine/icache.ld). They are not listed
  any more (the walk is off by default), the order master's.

### 91.7 Where the cycles of the walk go, and why the sequence loses

The tree walk is lean where a sequence per zone pays: a node it visits
costs its stamp's check, its plane's side, a push and a pop, about 35
instructions, and a leaf its stamp, its box when the flags still ask
for one, its marks; nothing of a node out of the PVS but its stamp's
check. The sequence has to carry the zone's union of PVS, so a camera
whose PVS covers most of the zone (START's hall) dispatches 1500
entries a frame at 8 instructions each, and its VAR planes (a third of
the words, 350 a frame taken in that hall) cost a side, a word written
and two ENDs each, 60-70 instructions: twice a tree node. What a camera
leaf fixes (the side of the planes that leave the zone on one side) is
the cheap part of the tree's work; what it cannot fix (the frustum's
tests, the marks, the sides of the planes partitioning the zone) is
the walk's cost, and the sequence pays it with an interpreter's
overhead on top. A sequence per camera leaf would drop the VARs (a
leaf's cell is on one side of all but its own boundaries) at a hundred
times the words; per-leaf record masks would drop the marks but cannot
follow mark_pvs_far's marks (§91.1).

So the cooked walk is built, exact and off: `QCOOK_SEQ=none` is the
cook's default; `QCOOK_SEQ="start e1m1"` cooks and runs it (the bench's
`seq view` lines count its tests, `SEQCHK=1` draws each view by both
walks and compares their stamps). Kept for the record and for a walk
whose costs change (per-leaf stamps in the records in place of the
marks, the frustum tests of the leaves made cheaper): then the sequence
would be worth measuring again. The second step, the near
polygons' tessellation (§92), is the one with the room.

## §92 — The near polygons on a regular grid before the slow path (2026-10-28)

The second step of the plan behind §91: the near polygons' tessellation
settled ahead of the slow path, so that their triangles go out at the fast
path's price. The plan's shape was a tessellation cooked per record and per
distance band, the reverse of the monsters' bands (§88), picked per record by
its distance; what is built is its runtime form, for the reasons of 92.6:
the polygon the record loop hands to the slow path is cut on a regular grid
whose fineness its own corners decide, in the frame. The slow path stays for
what the grid does not take. Provisional section number, as §91's: §89 lives
on the draw distance's branch, not merged.

### 92.1 What the slow path does with a polygon, and what costs

The record loop's CAREFUL test (§19.4, §79) sends a polygon to the slow path
when a corner is nearer than SZ 96, when its box is wider than the GPU's span
(W + H > 500) or when its affine error can show: (W + H) x depth range > 2
T2 zmin. `slow_poly` (engine/r_slow.s) then takes its corners from the
record's vertex cache (its near clip when a corner is too near or saturated),
and subdivides it triangle by triangle on a stack: each triangle's three
edges are tested, an edge splitting when |ds| x |za − zb| > T2 (za + zb) (|ds|
the octagon length) or when |dx| + |dy| > 500, the split edges' midpoints
projected from the ends' screen positions and depths (a division each), the
two, three or four children pushed; a triangle wholly off the screen is
dropped before its tests. About 950 PCSX cycles a triangle emitted on the
heavy views (§79.2), 54 % of START v0's frame.

Two things in that make a polygon cheap or dear. The splits follow the near
corner: an edge from a near corner to a far one is split at its near half
again and again while its far half stays whole, so the triangles crowd where
the depth gradient is, and the slow path emits few of them (START v0: 103
polygons, 355 triangles, `SLOWSTAT=1`). And the off-screen children are
dropped at once: a floor polygon running under the camera and out of the
screen on three sides costs only its visible part.

### 92.2 The grid

The level an edge needs follows from the slow path's own rule. Halving an
edge halves |ds| and |dz| together; after m halvings the half at the nearer
end, zm deep, has za + zb = 2 zm + |dz| / 2^m (the others more), so an edge
passes at every level from the first m with

    |ds| x |dz| <= 4^m x 2 T2 zm + 2^m x T2 |dz|,   |dx| + |dy| <= 500 x 2^m

(m = 0 is the slow path's test itself). The polygon's four edges and the
diagonal the GPU draws (corners 1-2, the two triangles' shared edge) are
measured once each: the top and bottom edges' greatest level is the rows'
(nu = 2^ku cells along a row), the left and right ones' the columns' (nv =
2^kv rows), and the diagonal's raises the larger of the two to its own. A
wall seen along its length is cut along it alone (8 x 1); a floor running
away from the eye gets its rows.

The grid's points are made in the world, not on the screen: the midpoint of
two points is the mean of their vertex, uv (x 256) and light, rounded down,
the same from either side, so a shared edge gets the same points from both
polygons at one level; they are projected by the GTE three at a time (RTPT,
as the record's vertices, in the walk's own setup: no division). Every cell
is a POLY_GT4 prepended to the world bucket, the last row of a triangle
(cut as a quad whose fourth corner is its third again) POLY_GT3s; a cell
whose four points are off the screen on one side is left out; nothing more
once the arena's bound is reached (a cell goes out only while it ends at
most 40 bytes past it, the slow path's own overshoot).

The work follows the screen as the slow path's does: the two columns are
made and projected first, then strip by strip (row j to j + 1); a strip
whose four column points are off the screen on one side is skipped whole,
the others get their rows' inner points made and projected as they come
(a row's right column point's pad word marks it done).

A polygon whose five edges pass at level 0 is what the slow path emits
unsplit, as its two triangles: it goes out as one packet straight from the
cache, the same pixels (the GPU draws a quad as those two triangles).

The slow path keeps: a corner nearer than SP_SZCLIP or projected off the
GTE's range (its near clip), a level past KMAX (3: 8 cells along an axis; a
polygon right under the eye), and the polygons whose two levels sum past
CAP (more than 2^CAP cells): there the slow path's splits, which follow the
near corner alone, are the fewer, and the uniform grid pays for cells the
slow path never makes. KMAX and CAP are build settings (92.3), their values
measured in 92.5.

What the grid changes on the screen: the slow path's midpoints are
projected from the screen positions and depths, s = (sa za + sb zb) / (za +
zb), the grid's from the world through the GTE; both are the perspective
midpoint, within the GTE's rounding. The split pattern is the difference: the
slow path splits a failing edge and nothing else, the grid cuts the whole
polygon at the level its worst edge needs, so a cut polygon is warped less
than the slow path's and its texture moves by up to the threshold where the
slow path's did not split. A shared edge between a cut polygon and an uncut
neighbour (or two cut at different levels) has points on one side only: a
T-junction, a crack of up to a pixel where the GPU's rounding of the two
sides differs (the slow path has none, §19.4). 92.4 measures the pictures.

### 92.3 The engine

- `engine/r_tess.s`, hand-written: `tess_poly(a0 = record, a1 = polygon, a2 =
  4 / 3)`, slow_poly's contract (s6 and fp advanced, s0-s7 kept), its code
  cut small for the instruction cache (§81.5): the edge test (`tess_edge`),
  a corner's load (`tess_corner`), a line's midpoints (`tess_line`) and a
  line's projection (`tess_proj`) are subroutines; `tess_near`, `tess_fine`,
  `tess_capped` restore and jump to `slow_poly`. The grid lives in
  r_slow.s's pool from its entry 256 (32 B a point: vertex, uv x 256, light,
  the colour and uv words, SXY, SZ), where the slow path's midpoint indices
  never reach, so its hash cannot believe a slot pointing into it.
- `engine/r_tess.c`: the C version, decision for decision and packet for
  packet (`r_slow` calls `r_tess` first; the C engine's frames are the
  asm's, 92.5).
- `engine/r_world.s` RECLOOP: the slow call site `\q_ts` (dr's and its
  options' copies') is a `jal` the engine points at `tess_poly` or
  `slow_poly` when a world is bound: `r_tess_select` (engine/render.c), by
  the map's name against CMake `Q1PSX_TESS_MAPS` ("START,E1M1" by default;
  names separated by commas, `all`, or nothing): the other maps' frames
  stay the slow path's, byte for byte (92.5). `Q1PSX_TESS_KMAX` (3) and
  `Q1PSX_TESS_CAP` (4) reach the asm as `--defsym` and the C as defines.
- The bench: `TESS=<maps>`, `TESSK=`, `TESSC=`; the per-view line's
  `tess=a/b/c/d/e/f` (polygons cut, their cells, to the slow path near, too
  fine, unsplit as one packet, over the cap).
- No RAM: no cooked data, the grid in the pool that is free during the walk
  (§77); the code 2.5 KB in the hot chains (tess_poly 1 744 B, the four
  subroutines 796 B) and 96 B of exits.

### 92.4 The pictures (pktview from the dumps, against master 795bfb2's)

The 12 views of each map, rendered from the bench's dumps by pktview on the
host and compared pixel by pixel with master's (the boards, reference |
after | the differing pixels, one a view, in the side-session notes'
`boards-92/`, outside the repo as everything drawn from Quake's data). The
difference is where it was expected: the cut polygons' textures move by up
to a texel (a mean difference of 8 to 27 on 255, the pixels that differ at
all), over the whole of a big near polygon, so the counts are large where a
floor or a wall fills the screen; a few pixels go dark where the reference
was lit (and about as many the other way, dark texels moving), the most on
START v10: that is where a crack would show, and only an eye on
DuckStation can tell them from the texels' moves.

| START view | px differ | % | mean diff | diff > 64 | dark where lit |
|---|---|---|---|---|---|
| v0 | 14 257 | 18.6 | 23.6 | 58 | 71 |
| v1 | 11 690 | 15.2 | 13.1 | 12 | 5 |
| v2 | 8 038 | 10.5 | 15.3 | 0 | 0 |
| v3 | 17 847 | 23.2 | 13.8 | 12 | 7 |
| v4 | 19 027 | 24.8 | 19.3 | 190 | 75 |
| v5 | 18 408 | 24.0 | 18.7 | 149 | 59 |
| v6 | 22 841 | 29.7 | 16.7 | 117 | 38 |
| v7 | 26 433 | 34.4 | 15.8 | 88 | 43 |
| v8 | 21 914 | 28.5 | 14.4 | 16 | 2 |
| v9 | 18 366 | 23.9 | 17.0 | 65 | 33 |
| v10 | 31 516 | 41.0 | 27.2 | 1 511 | 231 |
| v11 | 2 816 | 3.7 | 11.2 | 0 | 1 |

| E1M1 view | px differ | % | mean diff | diff > 64 | dark where lit |
|---|---|---|---|---|---|
| v0 | 4 185 | 5.4 | 9.9 | 0 | 0 |
| v1 | 4 648 | 6.1 | 10.1 | 0 | 0 |
| v2 | 29 801 | 38.8 | 8.5 | 0 | 0 |
| v3 | 3 394 | 4.4 | 11.1 | 0 | 0 |
| v4 | 4 253 | 5.5 | 16.7 | 36 | 7 |
| v5 | 1 404 | 1.8 | 22.3 | 1 | 0 |
| v6 | 9 889 | 12.9 | 17.2 | 52 | 24 |
| v7 | 0 | identical | | | |
| v8 | 12 871 | 16.8 | 13.9 | 1 | 11 |
| v9 | 10 759 | 14.0 | 18.7 | 125 | 25 |
| v10 | 23 455 | 30.5 | 12.7 | 51 | 2 |
| v11 | 6 153 | 8.0 | 8.2 | 0 | 0 |

### 92.5 Measured (the bench build, PCSX-Redux, T 4, geometry low)

The 12 views of each map, a frame's cycles and the slow stage's (`r_slow`:
the slow path and the grid together), master 795bfb2 against the branch's
default (KMAX 3, CAP 4):

| START | frame master | frame grid | d | slow master | slow grid |
|---|---|---|---|---|---|
| v0 | 804 227 | 790 453 | −1.7 % | 272 148 | 258 367 |
| v1 | 543 541 | 532 432 | −2.0 % | 360 512 | 349 419 |
| v2 | 164 721 | 153 669 | −6.7 % | 102 452 | 91 328 |
| v3 | 551 799 | 501 727 | −9.1 % | 362 359 | 312 334 |
| v4 | 569 897 | 550 845 | −3.3 % | 214 658 | 198 090 |
| v5 | 504 972 | 488 048 | −3.4 % | 246 570 | 229 557 |
| v6 | 572 800 | 537 166 | −6.2 % | 288 174 | 255 545 |
| v7 | 450 615 | 433 854 | −3.7 % | 231 872 | 214 985 |
| v8 | 511 573 | 486 985 | −4.8 % | 244 557 | 220 010 |
| v9 | 607 273 | 578 362 | −4.8 % | 322 210 | 291 741 |
| v10 | 278 808 | 257 566 | −7.6 % | 144 362 | 123 248 |
| v11 | 254 952 | 255 972 | +0.4 % | 201 021 | 202 058 |
| total | 5 815 178 | 5 567 079 | −4.3 % | | |

| E1M1 | frame master | frame grid | d | slow master | slow grid |
|---|---|---|---|---|---|
| v0 | 417 511 | 410 148 | −1.8 % | 176 484 | 169 121 |
| v1 | 125 961 | 120 460 | −4.4 % | 80 679 | 75 136 |
| v2 | 43 884 | 44 688 | +1.8 % | 15 774 | 16 544 |
| v3 | 128 564 | 124 268 | −3.3 % | 86 510 | 82 184 |
| v4 | 1 068 847 | 969 598 | −9.3 % | 475 170 | 378 992 |
| v5 | 69 157 | 70 405 | +1.8 % | 12 303 | 13 545 |
| v6 | 962 575 | 917 693 | −4.7 % | 413 925 | 368 765 |
| v7 | 74 177 | 74 868 | +0.9 % | 27 803 | 28 494 |
| v8 | 596 954 | 566 130 | −5.2 % | 311 476 | 280 337 |
| v9 | 721 021 | 697 388 | −3.3 % | 431 912 | 408 257 |
| v10 | 292 015 | 256 580 | −12.1 % | 193 765 | 158 295 |
| v11 | 84 269 | 80 659 | −4.3 % | 48 723 | 45 063 |
| total | 4 584 935 | 4 332 885 | −5.5 % | | |

The walk and the block stages do not move (the sequence is off). START v0
stays at two vblanks (790 k cycles: the plan's one-vblank goal is out of
this step's reach, 92.6). The polygons' fate on the heavy views (the
`tess=` counters: cut / cells / near / too fine / unsplit / over the cap):
START v0 30 / 166 / 12 / 2 / 16 / 18 of its 83, E1M1 v4 85 / 344 / 7 / 2 /
19 / 3 of 121, E1M1 v6 88 / 304 / 9 / 1 / 20 / 1 of 121.

The forms tried on the way, the 12 views' totals (START, E1M1):

| form | START | E1M1 |
|---|---|---|
| one level for both axes (the worst edge's), the whole grid made | +19.9 % | +10.4 % |
| a level an axis, the whole grid made | −0.4 % | −4.4 % |
| a level an axis, the strips off the screen skipped | −0.0 % | −3.1 % |
| + the unsplit polygons as one packet, KMAX 0 (the grid never) | +4.5 % | +3.4 % |
| KMAX 1 | +0.7 % | +1.0 % |
| KMAX 2 | −3.3 % | −2.1 % |
| KMAX 3 | −0.9 % | −3.9 % |
| a gate instead: in only with CAREFUL's measure 0 / 2 / 4 doublings over its threshold, KMAX 3 | +0.7 / +1.4 / +5.7 % | −2.5 / −2.0 / +0.1 % |
| the cap, KMAX 3, CAP 3 | −4.6 % | −3.5 % |
| CAP 4 (kept) | −4.3 % | −5.5 % |
| CAP 5 | −4.3 % | −5.3 % |
| KMAX 2, CAP 4 | −3.3 % | −2.0 % |

The game bench on E1M1 (`BENCH_GAME=1`, 300 frames, the walk at 60 and 30
Hz ticks): sim + frame 572 915 to 537 812 cycles a frame on average (−6.1
%), the frames at two vblanks 111 to 104 of 300, at three or more 2 both;
at 30 Hz 528 692 to 488 173 (−7.7 %), 110 to 103; the ticks the same.

The other maps: the 128 views (`BENCH_MAPS=1 HASHONLY=1`) with the grid on
START and E1M1 against the same build with `TESS=none`: 120 `H` lines
identical (the 30 maps), the 8 of START and E1M1 differ; `TESS=none`'s
START lists are master's. The C engine (`ENGINE_C=1`): E1M1's 12 lists the
asm's. START's are not, with the grid and without it alike (the same
hashes both ways, half the words of the asm's dumps): the C engine's START
dumps had gone from the asm's before this branch, left as it is (92.7).
hazcheck and iccheck at every link; the residency and the zones are
untouched (no data, no RAM).

### 92.6 Why the grid wins little, and why the cooked form was not built

The slow path's 950 cycles a triangle are an average over triangles it
chooses well: its splits follow the near corner (an edge from a near
corner to a far one is split at its near half again and again, the far
half kept whole), and its children off the screen are dropped before any
test, so START v0's 103 polygons come out as 355 triangles (`SLOWSTAT=1`:
847 popped, 303 midpoints). The uniform grid cuts the whole polygon at the
level its worst edge needs: 4^k cells for a polygon the slow path finishes
in four triangles. Per triangle the grid costs about a fifth of the slow
path's (a midpoint ~100 cycles, a cell ~110, the GTE's projection), but on
the polygons with a level of 3 on both axes it makes ten times the
triangles and loses; the strips take the off-screen rows out of the count
but not the levels; hence the cap, past which the slow path's shape is the
cheaper. The other way round was tried too: a gate letting in only the
polygons whose warp measure (CAREFUL's product) is 4 or 16 times over the
threshold, the rest to the slow path at once; it lost (the table above):
the slow path is dearer than the grid even on a polygon it splits once or
twice, through its stack of triangle copies and its division a midpoint.
What is left on the table is the fixed cost a polygon pays to find its
levels (the prologue and five edge tests, about 800 cycles) when it goes to
the slow path after all (START v0: 32 of 83).

The planned cooked form, a tessellation per record and distance band in the
chunk, was not built: a level-2 grid of a quad is 25 vertices and 16
polygons (about 400 bytes), every record a player can walk up to would
carry its bands, megabytes a map against the chunks' 44 KB; the levels
depend on the view (the edge's screen length and its two depths), which a
band by distance only approximates; and the cracks at a band's border would
be the same T-junctions as the grid's, to stitch in the cook. The runtime
grid has the plan's intent (the triangles at a fast price, chosen by the
polygon's distance) without the RAM.

### 92.7 Left

- The T-junctions: a cut polygon's edge points are not the neighbour's.
  Stitching (the neighbour's grid points along a shared edge: the edge's
  level is the same from either side when both polygons are cut, but a
  polygon the fast path draws whole has none) is not built; the pixel
  counts of 92.4 and a DuckStation look decide whether the grid
  stays on, and on which maps (`Q1PSX_TESS_MAPS`).
- The fixed cost: the five edge tests read the cache the CAREFUL macro just
  read; a cheaper first pass (the level-0 test alone, the loop only on the
  edges that fail) and CAREFUL's own zmin and ranges passed down would take
  a third off it.
- The slow path itself is the better shape (adaptive) at the worse price:
  its midpoints projected by the GTE (RTPS from the world, as the grid's)
  instead of a division each, and its stack holding indices instead of
  three 32-byte copies a push, would bring its 950 cycles down for every
  polygon, the grid's included (the near-clipped ones most).
- The C engine's START dumps differ from the asm's, grid or not (92.5):
  not this branch's, to be traced on master.
- The ceiling the plan named (START v0 under a vblank) needs the walk and
  the block stages too (113 k and 391 k of its 790 k): the grid alone
  cannot reach it.

## §94 — The item boxes drawn inside out up close (2026-10-29)

A DuckStation screenshot: a medkit a few steps away hollow, its
lid's texture at its bottom front, the inner sides of its far walls on
top; the ammo boxes behind it as flat. Provisional section number, as
§91's and §92's.

### 94.1 What happened

An item box (`maps/b_bh25.bsp`, `b_nail0.bsp`...) is one of a map's own
models (tools/qcook/items.c `cook_box`: each face a fan of triangles,
one record a texture; `QGF_MAP | QGF_FLAT`), drawn by the model loop
(r_world.s `RECLOOP de`, the `POLYS` instance with `var = 1`) as a
monster or a gun is. That loop leaves a triangle turned away out by the
GTE's NCLIP of its corners (§54), but only on its emit: a triangle that
fails CAREFUL (a corner nearer than the model's bound, a box wider than
the GPU's span, or the affine bound) goes to the slow path, which clips,
splits and draws it whatever its facing. §54 noted it and relied on the
guns' cooked order (a triangle written once every one in front of it
is) to keep their back faces under. A box has no such order: its faces
come in the file's order, per texture, so up close (the whole box fails
the affine bound) its back faces are drawn, and, prepended after the
front ones, over them: the picture above. From afar the emit's NCLIP
took the box's... back faces: `cook_box` wound its fans as the BSP's
faces come, and §54's note that those were "the world's already" was
wrong: a BSP face's corners and a MDL's triangle wind the same way
(the right-hand normal of corners 0, 1, 2 points into the model on all
82 triangles of `armor.mdl` and all 6 faces of `b_bh25.bsp`), and the
cook swaps the MDLs' (`mdl_word`: 0, 2, 1) but left the boxes'. From
afar that showed the box's far faces through its near ones: at a
distance a box looks the same inside out, so nobody saw it.

Not the merge's (§90) nor the grid's (§92): the same pictures with
`TESS=none`, and from detached worktrees of master 795bfb2 and 90b52bd.

### 94.2 The fix

- `engine/r_world.s`, `POLYS`'s `_tri_far` for `var = 1`: the three
  corners' SXY into the GTE (`lwc2`), the NCLIP issued at once, its MAC0
  read after the off-screen tests (its eight cycles spent there), the
  triangle left out when MAC0 >= 0 (`_tri_off`), else the slow path as
  before. About 12 instructions on a triangle that was going to cost the
  slow path's 950 cycles. The near exit (`_tri_near`: a corner within
  SZCLIP) keeps its back faces: those corners' SXY are not their
  projection, so the NCLIP's sign is not to be trusted there; the player
  standing on a box can still see one of its inner faces for a frame.
- `tools/qcook/items.c` `cook_box`: the fan wound `{0, e + 1, e}`, the
  MDLs' way after the cook's swap, so the emit's NCLIP and the new test
  keep a box's front faces.

### 94.3 Checked

- `BENCH_MAPS=1 MAP=E1M1 VIEW=...` with six views on E1M1's items
  (`VIEW=x,y,z,yaw,pitch` in whole degrees, pitch positive down; the
  bench's own 12 views first): a medkit at 34 units seen level (v15),
  a nail box at 46 (v16), the medkit from above and in front, the
  screenshot's angle (v17), the shells box by the spawn (v14), medkits from
  64 and 124 units (v12, v13); pktview's pictures, cropped at 3-4 x. v17
  shows the lid on top with its big cross, the small crosses on the
  sides; the nail box's "NIN" is Quake's own glyphs (not a mirror).
- Against the run before the fixes (the same build, the same cooked
  data but the boxes' winding): the lists of 5 of the 17 views change
  (v8, v12, v13, v15, v16), their pixels by 0, 7, 2, 55 and 0: the
  removed back faces were under the front ones in those views; v15
  -1.9 % cycles, v16 -3.8 %, v8 -0.6 % (two soldiers near the spawn),
  the others the same to the cycle (the bench is deterministic: three
  runs of v0 and v4 at the same count).
- `BENCH_GAME=1 MAP=E1M1`: the 10 gun and HUD views' lists all change,
  their pixels on 8 of them none, `gsshotgun` 0.5, `glightning` 37
  (the lightning bolt's triangles turned away, which Quake's alias
  drawer culls too, under its others);
  the walk's frames 0.7-1.6 px. The gun views' cycles +0.3 % (`gaxe`
  517 827 -> 519 219: the NCLIP on every near triangle of the gun, none
  of them culled, `slowtri` the same); walk60 537 812 -> 515 561 a frame
  on average (the frames at 2 vblanks 104 -> 99).
- hazcheck, iccheck, ovlcheck, ramcheck at the link; the release built
  for a DuckStation look.

### 94.4 Left

- The guns' near triangles pay the NCLIP for nothing (their cooked cull
  already dropped the back ones): a flag of the model (the view model's
  `SZCLIP` path) could skip it, +0.3 % on the gun views.
- The near exit's back faces (the eye within SZCLIP of a box).

## §95 — Walls seen through from afar, a block of noise on E1M2: the brush models' pieces' CLUT (2026-10-29)

### 95.1 The reports

- E1M1: a wall transparent from afar, in a corridor (the capture: the hall's
  ceiling lights showing above the doorway at the corridor's end, where the
  wall over the doorway should be).
- E1M3: the same at the level's start, the ogre behind the wall the key's
  trap opens seen through it (door *68, targetname t83, the ogres at
  (34, -1466, 72) and (28, -1352, 72)).
- E1M2: a block of noise under the yellow armor (1340, -544, 384) and wall
  pieces gone; the doors *5 and *6 (t53) beside the armor rise on the button
  *56 (x 1545).

No save state; the analysis went by the code and the bench.

### 95.2 What the engine does with distance (read, unchanged)

- The record loops (r_world.s RECLOOP `_o_dcull`, the models' `ds_o_dcull`)
  cull a record only when its nearest depth (z - rho) is past the draw
  distance: a wall nearer than what is behind it is never dropped alone.
  The far marking (`mark_pvs_far`) skips a leaf only when its box is past
  1.61 x the distance + 64 on an axis; the leaves' boxes are the BSP's.
- A brush model is drawn when the PVS stamps one of its pieces' leaves
  (ents.c `ents_visible`, bmfrag.c `bmf_seen`: exact for a model with
  pieces, its box's eight leaves otherwise), `box_in_front` and
  `all_culled` are frustum tests, conservative. The door *68 of E1M3,
  benched from three places of the courtyard (VIEW=-440,-1250,110,338,0
  and nearer), draws its pieces at every distance.
- What can show the world behind a wall: the zones. The walk draws the
  camera zone's chunk alone (§57.2); a record the chunk lacks lets what
  the chunk holds behind it show. The `pvs` tables hold every record the
  PVS may draw within their distance (§57.8: exact, the 128 spawn views
  identical); the `seen` tables only what the analysis' renders saw, from
  the eyes it sampled. A table cut at 1024 units under a 1536 draw
  distance lacks every wall between the two, from afar. The main
  checkout's local.env points at `seen_1024_64` (the §89 branch's draw
  distance, not merged), the worktree's at `pvs_1536_96`: which disc image
  was played decides what the captures show. CMakeLists.txt now refuses a
  zoned disc whose tables are cut under its draw distance (the tables'
  distance read from their directory's name) and says when the tables are
  the seen ones.
- The PVS rows as deltas (§90.9): E1M1's 12 views before and after §90 have
  the same lists (the bisect worktrees' logs).

### 95.3 E1M2: the button scenario in the bench

- `USE=<targetname>` added to the bench (main.c `bench_view_ticks`): with
  TICKS, the entities of that targetname are used at tick 8 (the doors'
  links are made at 0.1 s), as a button's press would. `BENCH_MAPS=1
  MAP=E1M2 VIEW=1432,-408,350,236,-12,1624,-376,350,211,-6 TICKS=128
  USE=t53`: the eyes are the analysis' sampled ones nearest the capture's
  place (scan/e1m2/samples.bin: the two that saw the most faces east of
  the armor), the second view the capture's. The doors rise (their cuts'
  origin z 107, 214, ... 1312 in 1/8 units, re-cut at every dump, 11-28
  groups), the pictures right at every tick, no noise; the screen area
  under the armor is drawn with ordinary textured primitives (texture
  page 6, in-range UVs).
- `BMFARENA=<percent>` added (bmfrag.c `bmf_reserve`): the cut arena at
  that share of the cook's size, the rooms given up and packed down in
  play (§90.6). At 50 % the scenario takes 28-29 rooms, gives 7-8 up,
  draws none without room, and its 48 lists are the full arena's, hash
  for hash: the eviction and the compaction change nothing. After a
  ticked view the bench prints the arena's traffic (`bmf arena` line).
- The noise is not reproduced. The pieces' vertex indices are all pool
  indices (put_record), renumbered as a block moves; the records' texture
  is the source record's. What the bench does not cover: a long play's
  state, DuckStation's GPU (the PCSX frames are the bench's).

### 95.4 The cause, read in a DuckStation save state: the pieces' CLUT

A play of the ram-batches worktree's release (master a09c161,
`auto_1536_s3`, three slots) left seven save states (slot 1 on E1M1,
2-7 on E1M2). A state holds the 2 MB of RAM and the VRAM: its camera
(`cam`, 1/8 units; `yaw` x 4096 a turn), its frame (the VRAM's first
320 x 240), the display list's packets and the edicts can all be read
offline (the state reading of §73, the symbols from the build's
`q1psx.map`; the RAM's base = the ELF's `.text` found in the state, less
0x10000; the first 4 KB of `.text` differ: the options' patched words).

State 2 (E1M2, the camera at 1460, -326, 334, yaw 217, the doors *5 and
*6 up) shows the block of noise. In its RAM the block's packets are
POLY_GT4s whose CLUT halfword is 0x0024 or 0x002a (a CLUT read at y 0:
the frame buffer, hence the noise) under the right texture page (6) and
in-range UVs; the same frame's other faces of that texture carry 0x784b,
the texture's CLUT (world.textures[11]). No texture table in the RAM
holds those two words. The polygons sit in the brush models' cut arena
(bmfrag.c), in records of door *6 whose texture is 11, listed in the
leaves' render lists as pieces (RI_PIECES), several items a leaf.

The record loop keeps the current texture's words in two registers, v0 =
CLUT << 16 and v1 = tpage << 16, and the run's index in SP_LASTTEX: a
record of the run's texture reuses them (3 in 4 do). The walk's plane
tests use v0 (`wn_side`: n . cam x 4096 - dist), so the walk reloads v0
from SP_CLUT16 each time it enters a face block (draw_records' entry,
and the brush models' own walk, bm_walk). `ei_pieces`, the pieces' entry
into the models' record loop from a leaf's render list, did not: the
run going on, a first piece of the run's texture (the door's wall
texture, the wall's own around it) took the camera's distance to the
last plane as its CLUT, and so did the pieces after it in the list. A
CLUT landing on zeroed VRAM reads every texel as 0x0000, the GPU's
transparent: the door is not drawn, the wall behind it shows (E1M1's
corridor door seen through from the hall, E1M3's trap door with the
ogre behind: the reports' "transparent walls"); one landing on the frame
buffer or a texture reads noise (E1M2's block); one on a dark area, a
black piece (E1M2's pieces gone). The bench never showed it because the
bench is profiled (R_PROFILE), and the profiled loop's entry reloads v0
(the `.if R_PROFILE` words at `\p\()_records`): the release's does not.

**The fix** (r_world.s ei_pieces): v0 reloaded from SP_CLUT16 before the
branch to de_records, one load.

### 95.5 Checked

- The mechanism in the bench: a bench-only count at ei_pieces (not
  kept) on the state-2 camera, 20 frames, doors up (`MAP=E1M2
  VIEW=1460,-326,334,217,-3 TICKS=128 USE=t53`): 714 entries, 630 whose
  first record had the run's texture. The profiled bench stays clean
  with v0 poisoned there (`lui v0, 0x1234`: no such CLUT in 68 681
  textured packets): the profile's reload hides it.
- Unprofiled (`PROFILE=0`), without the fix, the three state cameras
  (2, 3, 4) with the doors used: 144 textured packets with a CLUT out of
  the tables' rows (0xffcb: the plane distance's high half), 8 a frame
  in the state-3 camera's 18 dumps (the view's and its 17 ticks);
  with the fix: none; packet for packet, the two runs' 66 dumps differ
  in those 144 packets only, in their CLUT / first-UV word (v0's low
  half had gone into the UVs too), 18 `H` lines of 66.
- The 128 spawn views unprofiled (`PROFILE=0 BENCH_MAPS=1 HASHONLY=1`)
  without and with the fix: 3 `H` lines change (E1M7.v0, E2M3.v0,
  E3M2.v0: the bug was in those frames too); with the fix, E1M2's 4
  views hash as the profiled bench's. The release builds.
- `USE=<name>` now also matches a classname (`USE=func_door_secret`),
  `scripts/bench.sh`'s help with it.

### 95.6 Left

- The DuckStation look (TESTING 11.74): E1M1's corridor door from the
  hall, E1M3's trap door from the courtyard, E1M2's doors by the yellow
  armour after the button.
- The main checkout's tables are `auto_1536_s3` now (§93), the CMake
  check accepts them (`auto_<D>_s<slots>`).

## §96 — E1M1's bridge darkened at a corner, the shambler's lightning aimed at the floor (2026-10-29)

Two reports from the main checkout's release (master a394d1b), read
in the DuckStation save states as §95's (slot 1: E1M1, the camera on
the shore at 404, 2750, -34 looking down the extended bridge; slot 2:
E1M3, the menu open after the fight).

### 96.1 The shambler's lightning

`cast_lightning` (m_shambler.c, Quake's CastLightning) traced from the
shambler's origin 40 up toward the enemy's 16 up, `self.origin + dir x
600`: the port scaled the unit direction by 600 in the positions' 1/8
units, 75 world units, so the trace ended 75 units from the shambler's
feet, 40 under the beam's start: the bolt sloped into the floor and the
damage (LightningDamage along the trace) rarely reached the player.
Now `600 x SUB`. The ogre's, zombie's and wizard's `x 600 / len` are
velocities (units a second): right as they were.

### 96.2 The bridge: a shadow at a corner, spread by the vertex lights

A piece of the bridge was seen missing. The save state's frame shows
the bridge's top drawn whole (its two cut pieces, both in the display
list, both after the slime in the draw order, the texture window of
their texture in effect, checked packet by packet against the chain's
links) but with a dark wedge over its near half. Its top face (the
model's face 5164, 224 x 224 units) is one polygon with four vertex
lights: 158, 48, 148, 144 (x 1.66). Quake's lightmap for it (15 x 15
luxels) is bright but for a diagonal shadow in one corner, 4 luxels (64
texels) wide along one edge over 9 rows: the vertex there samples 29.
The GPU's Gouraud spreads that corner over half of the face, where Quake
keeps the shadow to its luxels.

The cook already cuts the world's faces on a 128-texel grid at the
ultra geometry level for this (QL_LIGHT_GRID, §39.2). The brush models'
faces now take a light grid at every level, but only the faces whose
four corner lights misrepresent their lightmap: `bm_face_needs_grid`
(world.c) takes the bilinear of the samples at the face's (s, t)
extents as the Gouraud the GPU will draw and compares it with the
lightmap at every luxel of the face's box; past `QL_BM_LIGHT_ERR`
(formats/qlevels.h) at one luxel, the face is cut every
`QL_BM_LIGHT_GRID` texels (world.c `face_pieces`). The cook prints the
count (`brush models' light grid: N faces`). Every brush model's face
on a 64-texel grid was tried first: +7 to +36 KB a cooked world,
dropped. The settings, measured on four maps (the cook's `brush models'
rooms` line: the records' bytes, the cut arena's; the baseline has no
grid):

| grid, threshold | E1M1 faces, records, arena | E1M2 | E1M5 | E2M6 |
|---|---|---|---|---|
| none | 0, 19 360, 12 544 | 0, 19 692, 17 920 | 0, 20 112, 11 712 | 0, 39 216, 37 568 |
| 64, 40 | 25, 22 660, 16 192 | 59, 26 020, 22 400 | 83, 28 928, 18 432 | 133, 56 628, 52 032 |
| 64, 56 | 17, 22 140, 16 000 | 43, 24 848, 21 888 | 51, 25 640, 15 808 | 89, 52 808, 48 704 |
| 64, 72 | 12, 21 548, 15 424 | 40, 24 768, 21 824 | 34, 24 260, 14 720 | 61, 48 528, 45 056 |
| 128, 40 | 25, 20 428, 13 504 | 59, 21 468, 19 200 | 83, 22 372, 13 376 | 133, 43 736, 41 344 |
| **128, 56** | 17, 20 140, 13 376 | 43, 21 204, 18 880 | 51, 21 548, 12 800 | 89, 42 620, 40 320 |

128 and 56 kept: 1 to 3 KB a map of records and arena together; the
bridge's dark corner is held to a quarter of its face (a 64-texel cell
would hold it to the cell, at twice the records). The bridge's top goes
from one polygon to four cells: its cut at the extended place 912 B of
records in 14 groups before, 1 180 B in 14 groups after (the 64-texel grid: 2 156 B, 280 vertices). The bench's
`BMFTRACE=<model>` (BMF_C=1) prints every piece of a brush model's C
cut with its leaf, written for this (the pieces were all there).

### 96.3 Checked

- The save state's bridge pieces: the cache (14 groups, 912 B, valid,
  resident), the render lists (5 of the groups, the others' leaves not in
  the lists: the models' sides), the display list's packets of the top's
  texture (a quad and the slow path's 60 triangles over the whole top),
  the chain's links from the slime's quad to them (drawn after it), the
  texture-window commands met on the way (the top's own, right before
  them), the page's texels under the top's CLUT (none transparent): the
  top is drawn; its lights are the fault. BMF_C=2 on the scenario: 21
  cuts, 0 mismatches between the asm cut and the C.
- `BENCH_MAPS=1 MAP=E1M1 VIEW=404,2750,-34,176,4 TICKS=704 USE=t2` (the
  bridge extended by its button's target t2, 88 dumps): the dark wedge
  on the bench's pictures before, the top evenly lit after (the shadow's corner cell darker alone). The cooked worlds: +1 030 B a map on the mean, +3 260 at most (32 maps).
- `BENCH_MAPS=1 HASHONLY=1` after (128 views) against the §95 run's:
  8 of 128 `H` lines change (E1M6.v3, E1M7.v0, E3M2.v0, E3M3.v0, E4M5.v0, E4M5.v1, E4M6.v0, E4M7.v0: a brush model with a cut face in view), the others identical.
- The shambler: `MONSTERS=3 MAP=E1M3` and `MONSTERS=4 MAP=E1M3` cast no
  bolt (the bench's `sham bolt` line, kept: a bolt's start, end and
  length, the enemy's distance); the fix is the units alone, unchecked in
  the bench. DuckStation: E1M3's shambler.
- The release builds.

## §97 — The registered game's six monsters, by hand, and the animations thinned (2026-10-28)

(Provisional number then: §96 was another session's, merged into master on 2026-10-31 with §109-§110.)

### 97.1 The ask

- The six monsters the port still lacked, the registered game's: the
  enforcer, the hell knight, the spawn, the vore, the rotfish and
  Shub-Niggurath with END's end. monsters.c `kind_of` had no kind for
  them: removed at their spawn, 423 monsters on hard over 21 maps (E4M6
  73 of 81, E4M7 55 of 80, E4M5 39 of 94, E2M1 26 of 46...).
- Their models remade by hand as §62/§64, the skins cooked at high (half
  the skin; ultra is the native resolution); comparison sheets for a
  look.
- Fewer animation frames for the monsters that have many, with an
  interpolation only if it costs few cycles.

### 97.2 The code, 1.06's

- The source: pak0's progs.dat 1.06 (it holds every monster's QC),
  disassembled by a throwaway tool outside the repo (the statements, the
  constants named); each frame function's state, next state and code
  printed as `ST()` lines, the bodies transcribed by hand; the frames'
  names from the rerelease's QC (`reference/quake-rerelease-qc`).
- engine/m_enforcer.c (106 states): `enforcer_fire` (EF_MUZZLEFLASH,
  LaunchLaser from 30 forward, 8.5 right and 16 up towards the enemy's
  origin, 600 a second, 5 s to live); Laser_Touch (enfstop.wav on
  CHAN_WEAPON at ATTN_STATIC, the origin 8 units back along the flight,
  SpawnBlood's particles through the protocol: the velocity x 0.02 x 16
  to a char, back, x 15 / 16; 15 damage, else TE_GUNSHOT); SightSound's
  four sounds by rint (random () x 3); `enf_pain` draws its random before
  the pain_finished test (1.06's order); the backpack with 5 cells.
- engine/m_hknight.c (166 states): `hknight_shot` (vectoangles' whole
  degrees, the yaw offset x 6, from the box's centre + 20 forward, the
  height flipped back from makevectors' pitch and jittered, 300 a
  second; the knightspike: TE_KNIGHTSPIKE's 20 particles of 226 and
  hknight/hit.wav, the second tracer's trail); CheckForCharge and
  CheckContinueCharge; `hknight_melee`'s turn is the QC's global
  `hknight_type`, every hell knight's (reset at each map, as the progs'
  globals are); `hknight_pain`'s two rules (5 s since the last, or
  random () x 30 over the damage). The knight's sword sounds come with
  it (the cook's SHARED_SOUNDS).
- engine/m_tarbaby.c (64 states): MOVETYPE_BOUNCE leaps (the engine's
  toss physics, its touch), Tar_JumpTouch (10 + 10 x random () at over
  400 a second, never on another spawn), the leap again at the fourth fly
  frame, the death's radius 120 from itself, TE_TAREXPLOSION
  (R_BlobExplosion at the explosions' scale: 48 particles, pt_blob and
  pt_blob2, the r_exp3 sound, no light), the edict gone (BecomeExplosion's
  sprite in the game's pool).
- engine/m_shalrath.c (48 states): its start a tenth of a second late
  (monster_shalrath sets walkmonster_start as a think: `monster_start`
  is one now); ShalMissile (towards the enemy + 10 up at 400 a second,
  avelocity 300, its first home after the distance x 0.002 s, 0.1 at
  least), ShalHome every 0.2 s at 250 (350 in nightmare) on its owner's
  enemy, gone with it; ShalMissileTouch (no sky test, 110 on a zombie,
  the radius 40, two r_exp3: the QC's and TE_EXPLOSION's); the voor
  trail.
- engine/m_fish.c (93 states): swimmonster_start (`MS_SWIM`; 1.06 counts
  each fish twice in the level's total, swimmonster_start and its _go:
  kept), the bite within 60 units, (random () + random ()) x 3, a pain
  at every hurt; its body non-solid at the last death frame.
- engine/m_oldone.c (66 states): no AI, its idle loop; DAMAGE_YES; nopain
  gives its health back (40000 does not fit the edicts' 16 bits: 30000,
  the same, the teleporter's telefrag of 50000 alone reaches it); not
  FL_MONSTER, so its death is not counted (1.06's). Its death is the
  game's end (97.3).
- The framework: `A_CHARGEMELEE` (ai_charge then ai_melee, the hell
  knight's 27 swings), `sight_fn` (a kind's own SightSound), `MS_*`
  (walk, fly, swim), `ai_face` taking the QC's world for an enemy an
  ai_run cleared earlier in the same think (CheckContinueCharge's run
  then ai_charge).
- The projectiles in the game's pool (engine/game.c): PJ_LASER
  (MOVETYPE_FLY: MOVE_NORMAL; EF_DIMLIGHT's light), PJ_KSPIKE and
  PJ_VSPIKE (MOVE_MISSILE), the vore's ball's enemy in the proj_t's
  union with a beam's far end (80 bytes still).

### 97.3 END's end

- finale_1 (Shub's th_die): the train removed, `level_finale_begin`
  (intermission 2: the camera at info_intermission, the finale's
  picture, the empty text; no way out, the QC's exit time + 10^7 s), a
  timer edict: finale_2 a second on (TE_TELEPORT 100 units before Shub,
  r_tele1), finale_3 2 s on (Shub thrashes, its death cry, light style 0
  `abcdefghijklmlkjihgfedcb`), the thrash a letter each 0.1 s (1-15 three
  times, then 16-20), finale_4 (pop2, fifty gibs at -999 from a 2 x 5 x 5
  grid over its body, the text, the player's statue, CD track 3, the
  light back to `m`).
- The world's light style 0: the port bakes the light into the vertices
  and animates no style; Shub's pulse is drawn over the viewport as a
  dim, the letter's light ((c - 'a') x 22 / 256) at the nearest power of
  a half: a black (an opaque tile), b 1/16, c 1/8, d-e 1/4, f-i 1/2, j and
  up as is (the GPU's (B + F) / 2 with F black, once per halving);
  `level_lightstyle0`, `vid_dim`, at 10 letters a second as R_AnimateLight.
- The text: the cook takes finale_4's string as END's (qg_finale "END").
  The statue: progs/player.mdl's frame 1 alone (stream_model.frame1, the
  cook's `qc_mdl_one_frame`: 14 KB, not its 143 frames).
- The bench (MONSTERS=1 MAP=END): Shub telefragged (50000), the four
  parts dumped (the fog, the dark thrash, the gibs and the statue, the
  text).

### 97.4 The cook

- The groups G_ENFORCER .. G_OLDONE (`G_LAST_MONSTER`), from pak1.
- A body without its hand table (a new monster's first cook,
  QCOOK_NOHAND's A / B) cooks the low level's reduction at every level:
  the shared asset's check (§88: one LOD block, one frame map for every
  level) failed on the old per-level reduction.
- The image table 256 -> 512 (the stream's every model).
- The game arena 90 -> 92 KB: the stream's directory 179 -> 224 assets,
  56 bytes each in the engine's table.
- The clip entries 128 -> 160 (engine/clip.h): on hard, E4M3 needs 157,
  E4M5 146, E2M6 and E4M7 136, E4M8 132; past 128 a monster had no entry
  (neither solid nor hit). +1.5 KB.

### 97.5 The models by hand

The §64 tools (outside the repo, `quakespasm-q1psx/mobs`,
now reading `pak0:pak1`): the kept-whole rules (`mobcut.py` RULES), the
hidden triangles over every frame, the collapses within 3 units and 4
texels (Shub 13 units: its vertices' grid is Chthon's), the hidden ones
confirmed at 24 px (`vhid.py`: none seen 3 px).

| Monster | Kept whole | Triangles | Vertices | Hidden | Asset (high) |
|---|---|---|---|---|---|
| enforcer | the laser gun and its flash, the head, the hands on the gun, the feet | 424 -> 304 | 222 -> 166 | 8 (6 of the flash's) | 94 KB |
| hell knight | the sword, the horns, the eight spikes, the head, both hands, the feet | 452 -> 380 | 250 -> 221 | 14 | 168 KB |
| spawn | its dome (the face) | 256 -> 246 | 130 -> 125 | 0 | 56 KB |
| vore | the head, the hands and claws, the legs' points | 414 -> 330 | 208 -> 167 | 2 | 45 KB |
| rotfish | the spines, the head, the tail's tip | 172 -> 124 | 97 -> 73 | 0 | 40 KB |
| Shub | the tentacles' tips | 364 -> 250 | 182 -> 126 | 2 | 54 KB |

(The cook's old reduction for comparison: 192, 220, 136, 138, 45 and 356
triangles.) The spawn's blob deforms too much in its leaps for more cuts
at 3 units. Sheets (Reference ultra | the old cook high | by hand high,
8 frames x 8 sides each) to look at.

### 97.6 The animations thinned

- A frame f of a body is left out when the floor average of its
  neighbours' vertex bytes (f - 1 and f + 1, played frames of the same
  animation: the names alike less the digits) puts every vertex within
  the body's tolerance of f's: never two neighbours out, never a frame
  another one shares (§67's equal frames: the ogre's stand6 and stand8
  were lost before that rule). Its map entry is `QST_LERP | f`
  (formats/qstream.h): the frame cache's key part as is, so the lookup
  (engine/mdl_frame.s) is unchanged.
- On the cache's miss (engine/mdl.c `mdl_expand_new`): the two
  neighbours' words averaged into the pool's upper half
  (engine/mdl_expand.s `mdl_frame_avg`: (a & b) + ((a ^ b) & 0xfefefefe)
  / 2, no carry between the bytes, a's normal) and expanded in place.
  About 2000 cycles a rebuild (~15 a vertex); the E1M3 fight with every
  body thinned at 3 units: +60..171 cycles a frame (+0.01..0.03 %), the
  same expansions, 10-26 rebuilds every 300 frames.
- The tolerance: 3 units (the hand cuts' bound), 13 for Chthon and Shub
  (`ANIM_THIN`, `QCOOK_ANIM_TOL=<units>` for every body). 135 KB of
  frames less over the 15 bodies:

| Body | Frames rebuilt | KB less | Body | Frames rebuilt | KB less |
|---|---|---|---|---|---|
| soldier | 18 of 102 | 9 | boss | 20 of 104 | 21 |
| dog | 6 of 78 | 5 | enforcer | 23 of 101 | 14 |
| ogre | 10 of 129 | 6 | hell knight | 16 of 162 | 14 |
| knight | 10 of 92 | 3 | spawn | 23 of 60 | 11 |
| zombie | 53 of 194 | 25 | vore | 2 of 35 | 1 |
| wizard | 10 of 40 | 2 | rotfish | 18 of 65 | 5 |
| demon | 11 of 69 | 5 | Shub | 22 of 65 | 10 |
| shambler | 8 of 87 | 4 | | | |

- Wider tolerances (Quake's models, every other frame at most): the hell
  knight 26 frames at 3 units, 38 at 5, 50 at 8, 72 at most; the zombie
  87, 88, 89; the soldier 44, 47, 49; the ogre 26, 39, 55. Sheets of each
  body's four worst rebuilt frames (Quake's | rebuilt, four sides): at 3
  units no difference shows.
- An interpolation at every rendered frame (QuakeSpasm's model lerp) is
  not done: each visible body re-blended every frame at a weight that
  changes (the cache's hits gone, a multiply-based blend ~50 cycles a
  vertex), ~10k cycles a 200-vertex body a frame: 2-4 bodies on screen,
  +3..6 % a frame.

### 97.7 RAM and residency on hard

- The pools shrink: ~20 KB everywhere (the code, the game arena, the clip
  table), and where the new kinds' sounds overflow the SPU bank into the
  level's RAM block (§86): E4M7's RAM sounds 118 -> 216 KB, E4M5 78 ->
  176, E4M6 32 -> 148, E2M6 88 -> 142. The bench's pools (`SKILL=2
  RESIDENCY=1`, base a394d1b -> this branch): E4M7 426 -> 293 KB, E4M5
  533 -> 404, E4M6 684 -> 534, E2M6 466 -> 388, E2M4 439 -> 398, E3M6 542
  -> 456; the wants (every kind's models and the items at once) grow with
  the kinds: E2M6 611 -> 836 KB, E4M7 381 -> 739.
- Residency (the spots with a monster in the player's PVS whose model is
  not in, after the stream's reads), 32 maps, ~1000 spots: the base 2
  (E1M7 1, E4M4 1 before the reads, 0 after), this branch 5 (E2M6 3 / 3,
  E1M7 1 / 1, E4M4 1 / 0). E4M7, the smallest pool, has none.
- The levers left open: the new kinds' sounds at a lower rate or
  streamed (back to §52's), the edicts in the stream's pool, a wider
  thinning tolerance for the hell knight (8 units: ~44 KB of its frames,
  ~30 more than at 3).

### 97.8 Checked

- `MONSTERS=1` on E2M1 (enforcer), E2M3 (hell knight, rotfish), E4M3
  (spawn), E3M6 (vore), END (vore, rotfish, Shub and the finale): each
  woken, fighting, hurt, killed (the spawn's blast, the vore's ball
  homing, Shub's four finale dumps right).
- The 128 spawn views (`BENCH_MAPS=1 HASHONLY=1`): E1 and START identical
  to master's; 14 views of E2-E4 change (the new monsters in them); the
  thinning changes none of the 128.
- The asm and C engines' E1M3 monster dumps identical (16 `H` lines);
  `mdl_frame_avg` against its formula (asm and C: 0 of 64 words wrong).
- The game state with and without the thinning: the E1M3 fight's
  positions and expansions the same.

### 97.9 Fourteen dynamic sound voices

- The SPU's 24 voices were: the ambients 2, Quake's 8 dynamic channels,
  12 for the static sounds (one per distinct sound), the music 2. A map
  has 6 distinct static sounds at most (E3M1; most 1-4): the dynamic
  channels now take 14, the static sounds 6 (the choice made: more
  than Quake's 8).
- The fight bench's `mon sounds` lines: the sounds started, those cut by
  another while still playing (the channels full), by their own entity's
  channel (Quake's rule), stopped when their samples left SPU RAM, and
  those unheard (not in SPU RAM). E4M7 on hard, the 80 monsters woken:
  in the first 5 s, 8 cut by another with 8 channels, 2 with 14; none
  swapped out or unheard.
- Streaming the RAM block's sounds with the zone chunks was measured
  first: the three resident zones' sounds want up to 157 KB of SPU room
  on E4M7 (141 E4M5, 119 E2M4, 94 E2M7) against a 43-53 KB swap room, and
  15 of the 32 maps are whole (E1M3, E2M6, E4M3, E4M6 among the heaviest);
  the sound fidelity map by map instead (the RAM block at 98 / 97 / 96 /
  95 % of the energy kept): E4M7 177 / 136 / 108 / 84 KB, E4M5 147 / 107 /
  82 / 59, E4M3 146 / 103 / 73 / 50, E2M4 127 / 83 / 55 / 34, E4M6 125 /
  85 / 60 / 21, E2M6 119 / 72 / 43 / 15, E1M3 115 / 76 / 52 / 15, E2M7
  106 / 65 / 40 / 15. The choice: an experiment of the chunk streaming
  on a branch of its own.

### 97.10 Left

- A look at the sheets (the six hand cuts, the 15 bodies'
  rebuilt frames) and on DuckStation (the six monsters, END's end).
- The RAM choice (97.7).
- This branch's first commit says §96 (the number taken since).

## §98 — Sounds from the disc: the fogs' and powerups' sounds out of the RAM block, the zones' chunks tried (2026-10-29)

The question: the main RAM a level's sounds take (§86's RAM block: the
sounds the SPU cannot hold, read at the map's start, 0 to 127 KB a map)
given back by reading them from the disc, without the late sounds the
streaming of before §86 had (a sound read when its entity neared, and
played or not). The disc may carry as many copies as it takes: each
zone's sounds copied right after its chunk (§57), read by the drive in
the same pass as the chunk, no seek. That way was built and measured
(98.1-98.3, 98.5), then removed before the merge (its branch keeps it,
c511f6c); what went in is 98.4. (§96 and
§97 are on branches not merged yet.)

### 98.1 The zones' sounds, first form: every tied entity sound out of the bank (measured, dropped)

- The cook (tools/qcook/sound.c `zone_sounds`) ties an entity's sound
  (§86's class 3: doors, plats, buttons, triggers, items, keys) to the
  zones of the zone tables' camera leaves (`Q1PSX_ZONE_TABLES/<map>.zones`)
  when every entity bringing it stands in a zone; never tied: a
  `func_train`'s (it goes from zone to zone), an entity's outside every
  zone, the powerups' sounds while they run (the player's, anywhere).
- Those sounds left the bank for a room in the SPU: the worst camera
  leaf's resident zones' (its zone and its two nearest), 16 KB of play.
- The 17 zoned maps tie 6 to 27 sounds (24 to 98 KB), and the worst
  camera leaf's three zones want 80 to 100 % of them (E4M7 79 of 93 KB,
  E2M4 89 of 98, E3M4 75 of 75): the room the SPU keeps is the room the
  bank gave. The RAM block grew on the ten maps that had one and on
  E3M6 (E1M2 68 → 77 KB, E3M4 47 → 64, E3M5 11 → 31, E3M6 0 → 11, E4M5
  68 → 84, E4M7 99 → 101); the six others kept none.

### 98.2 The zones' sounds as built: the split's overflow (removed)

- The split stays §86's. A tied sound it puts out of the SPU goes to
  the zones' chunks instead of RAM (`zone <name> <zones>` in
  `<map>.sndset`); the swap room grows by the worst camera leaf's
  zones' sounds (pinned while their zones are in): the two pinned fogs,
  the biggest other sound out, those zones' sounds, 16 KB of play; the
  split is made again while it grows (the bank shrinks: more sounds out,
  the cascade).
- `qcook section`: a zone's sound is not in the level's file
  (`QST_ABSENT`). `qcook zones --sndset --stream`: each zone's sounds
  copied after its chunk, a sector each, the pool file's bytes (the
  asset's `qst_sound` header and samples); the skeleton (on the branch
  QWZI → QWZJ, `qw_zone_header` 48 → 56 bytes) carries `qz_zone_sounds[num_zones]`
  and `qz_sound[]` (the hash, the copy's sector); both texture sets'
  .ZON files keep one layout. A map the zone cook leaves whole with
  zones' sounds is refused. The 61 copies of E4M7 checked byte for byte
  against the pool file at the skeleton's sectors.
- The engine (engine/zone.c `zone_sound_wants`, `zone_sound_lba`;
  engine/stream.c `sound_lba`): each residency pass pins the sounds of
  the zones in the slots; a zone's sound is read from its copy after a
  ready slot's zone (the drive just there), else it waits (-2: none of
  its zones in; -1 is the cook's miss, the bench's backoff as before).
- Two bugs the walk found: the load's synchronous reads
  (`sound_read_sync`) went to the level's file at `QST_ABSENT` (the
  sector before the file: "not a sound this engine reads", 13 of E4M7's
  sounds broken for the map), and a zone's sound wanted before its
  chunk was in was backed off for the map (4 more); the walk's sounds
  asked and not in 96 → 0 on E4M7.
- The walk (`ZONES=1 MUSIC=1 CDMODEL=1`, the 17 zoned maps, the drive's
  seek model) against the same branch cooked without them (master's
  data): no sound asked and not in on either; the stream's pool +28 KB
  on E4M7, +12 E2M7, +6 E1M4, +4 E1M2, −2 E2M4 and E3M4, −4 E4M5, the
  same elsewhere; the zones' sounds read again at every switch that
  brings their zones back (the stream's reads E2M4 0 → 140, E2M7 2 →
  114, E4M7 0 → 8); the late frames 3182 → 3189 in all.

### 98.3 A sound is heard from zones not in

- A sound tied to the zone its entity stands in is heard to 1000 units
  (a full one, ATTN_NORM), from leaves whose resident zones may not hold
  it. The walk with both levers (98.4) missed E4M7's teleporter hum
  (`ambience/hum1.wav`, a trigger_teleport's static sound) 55 times:
  wanted at 376 units, none of its zones (0, 6, 8, 9) in.
- The tie then took the zones of every camera leaf whose box comes
  within 1000 units of the entity (the branch's
  `QCOOK_ZONE_SOUND_REACH=<units>`, 0: the entity's leaf alone): a
  camera within earshot has the sound's zone among its own. No sound missed on the 32 maps' walk after it.
  The sets grow, the swap room with them (E4M7 126 → 146 KB), and the
  zones' sounds give little over 98.4 alone: the RAM block (the cook's
  figures, without the sectors' padding) E1M4 5 → 0 KB, E3M4 7 → 0,
  E4M5 11 → 0, E4M7 31 → 21, E1M2 28 → 28, E2M4 59 → 57, E2M7 45 → 46.

### 98.4 The fogs' and the powerups' sounds read from the level's file (merged)

- In the RAM blocks of the 18 maps that have one (1280 KB with the
  sectors' padding), the teleporters' fogs take 548 KB (the five
  `misc/r_tele*`, 36 KB a map, the split's last class) and the
  powerups' sounds while they run 264 KB (`items/inv2`, `inv3`,
  `protect2`, `protect3`, `damage2`, `suit2`); the rest (468 KB) are the
  monsters' and the entities' sounds.
- Neither needs RAM to be in time. A fog is played among the variants
  in (engine/snd.c `snd_tele_fog`: two pinned in the swap room, the
  played one replaced by the next at the following pass, now read from
  the disc). A powerup's sounds were wanted from the pickup on
  (game.c `game_stream_wants`), but the ring's breath (`inv3`) plays at
  once and the pentagram's (`protect3`) at the first hit: the artifacts
  now want them by their distance as they want their pickup's sound
  (engine/items.c `powerup_item`: `noise1`, `noise2`, never played
  from there), so they are in before the pickup.
- The split marks them `disc` when it puts them out of the SPU
  (`QCOOK_NO_DISC_SOUNDS=1`: in RAM as before); `qcook section` keeps a
  `disc` sound in the level's file without `QSTF_RAM`: the stream reads
  it when wanted, as any streamed asset. The swap room's sizing counts
  them as it counted them in RAM.

### 98.5 What the zones' sounds cost (measured with both levers)

- The walk of the 32 maps (`ZONES=2`: the whole maps walked too, a
  bench option added for their sounds) with both levers and the
  earshot tie: no sound asked and not in; the pools +818 KB in all.
- The frames' cycles on the walk: +15.2 % E2M4, +17.3 % E2M7, +20.9 %
  E4M7 (the maps with the most zones' sounds), +2.2 % E1M2, within
  ±1.1 % on the others. The PC samples of E4M7's walk against master's data: the
  busy samples +13.6 %, of it the stream's model reads (`vram_place`,
  `model_in`, `memmove`, the drive's calls: none in master's walk, 119
  in this one) and `zone_sound_lba`'s scan (3.2 % of the busy time),
  `next_wanted`, `stream_find_hash` and the entities' wants (more
  entities with a streamed sound). The model reads: the stream's 48
  resident slots (`MAX_RES`, models and sounds alike) hold the zones'
  pinned sounds (24 on E4M7), and the monsters' models are evicted and
  read again.
- The disc: the .ZON files +3.8 MB a texture set (E2M4 +1196 KB, E4M7
  +946, E2M7 +676, E1M2 +628, E1M4 +146, E4M5 +100, E3M4 +86).
- The zones' sounds were therefore removed: at most
  11 KB over 98.4 on five maps, for the drive's reads at the switches,
  the slots and the cycles, and about 650 bytes of code even off.
  Fixable if ever wanted again: the copy's sector in a table refreshed
  at each slot change (O(1)), the zones' sounds in slots of their own.

### 98.6 98.4 measured

- `ZONES=2 MUSIC=1 CDMODEL=1` (the 32 maps walked) with 98.4 (the
  branch's zones' sounds off) against master's data (the zoned maps'
  walk, the whole maps' pools from their loads): no sound asked and not
  in on the 32 maps.
- The RAM blocks (the cook's figures): E1M2 68 → 28 KB, E1M3 115 → 60,
  E1M4 41 → 5, E1M5 80 → 40, E1M6 25 → 0, E2M2 25 → 0, E2M4 112 → 59,
  E2M6 75 → 27, E2M7 90 → 45, E3M2 55 → 0, E3M4 47 → 7, E3M5 11 → 0,
  E3M7 11 → 0, E4M3 127 → 63, E4M4 11 → 0, E4M5 68 → 11, E4M6 28 → 0,
  E4M7 99 → 31. The stream's pool +806 KB in all (the same engine on
  both sides): E4M7 +77, E4M3 +72, E4M5 +64, E1M3, E2M4 and E3M2 +62,
  E2M6 +56, E2M7 +51, E1M5 and E3M4 +46, E1M2 +45, E1M4 +42, E4M6 +32,
  E1M6 and E2M2 +28, E3M5 and E4M4 +12, E3M7 +11; the 14 others as they
  were. The release's level arena 1 383 376 → 1 383 216 bytes (the
  artifacts' two sounds: `powerup_item` and its callers, 148 bytes of
  code).
- The merged code (the zones' sounds out) cooked and walked again
  (`ZONES=2`): its sound sets and banks byte for byte the measured ones
  (E1M1, E1M3, E2M4, E4M3, E4M7), no sound asked and not in on the 32
  maps, the zoned maps' cycles within −1.0..+0.9 %.
- The frames' cycles on the 17 zoned maps' walks within −1.0..+1.0 %
  (+0.03 % on average); the stream's disc reads in a walk 0 to 22 (E2M4
  22, E2M7 17, E4M7 10). The late frames 3182 → 3248: E4M4 +62 with the
  drive's work the same (75 chunk reads, no sound read) and the walk 31
  vblanks shorter: its frames 1 % cheaper (the 12 KB more), and the walk
  advances one step a frame, so cheaper frames outrun the drive; E3M4,
  1 % dearer, 25 late frames fewer, the same way.

### 98.7 Left

- DuckStation: the teleporters' fogs where monsters teleport
  in on a map that reads them from the disc (E1M2, E1M3, E1M4, E1M5,
  E1M6, E2M2, E2M4, E2M6, E2M7, E3M2, E3M4, E3M5, E3M7, E4M3, E4M4,
  E4M5, E4M6, E4M7), a ring of shadows picked up (the breath at once:
  E1M3, E3M2, E4M3, E4M7), a pentagram hit (E2M4, E3M4, E4M7), a quad's
  and a biosuit's ends.
- The rest of the RAM blocks (the monsters' and the entities' sounds,
  468 KB over 18 maps before 98.4) has no such lead: a monster's sound
  is wanted when it is near, as before §86.

## §99 — The powerups' sounds after a teleport: the RAM swaps beside the drive, the fogs' rotation last, the breath and the hit in RAM (2026-10-30)

§98's worst cases, recorded on DuckStation through a new bench mode
(99.1), showed two powerup sounds missed at a pickup (99.2); the three
fixes chosen (99.3-99.5) were recorded the same way (99.6).

### 99.1 The capture bench (`CAPTURE=1`)

`CAPTURE=1 MUSIC=1 scripts/bench.sh` (engine/main.c `bench_capture`)
plays scripted scenarios in real time through the game's loop
(`run_frame(1, 0)`: a tick a vblank poll), the pad's commands replaced
by the script's steps: `JUMP` (the player put at a point, facing a
way), `TELE` (into a teleporter's trigger, by its brush model), `GOTO`
(walked to a point, a jump when stalled 8 ticks), `USE` (a targetname's
entities used), `HURT` (damage from the world), `ENDSOON` (the four
powerups' ends brought to now + n hundredths), `WAIT`, `NOTE` (a line
on the screen and in the log). Each step prints a `capture:` line,
each scenario a `capture <map> end` line (its frames, the sounds asked
and not in, the stream's reads), and the stream's log (`stream: in`,
`out`, and `no room for` from `set_stuck`) runs during the scenarios.
The four scenarios:

- E2M4's scrag trap: t67, then t68 four seconds later (eight fogs).
- E4M7's six teleporters in a row (*98, *24, *26, *48, *19, *23:
  twelve fogs, the zones switching at each).
- E4M7 from its start, the maps' worst lead for a powerup's sounds:
  teleporter *23 lands 367 units from a ring of shadows that is 2584
  units from the spawn, out of every sound's reach there (none of its
  sounds in); the ring picked up about a second later, its end then
  brought forward. (The chain above passes 814 units from the ring at
  *24 and has its sounds in by *23.)
- E4M7's synthetic cuts, which the maps never allow: onto the other
  ring, onto the pentagram (hit 1 s and 3 s later), onto the biosuit,
  the three ends then brought forward (their warnings 1.5 s after the
  last cut). A cut lands on the item: its pickup sound plays on the
  cut's own tick, with no lead at all.

The DuckStation recordings are made outside the repo: the AppImage
extracted, a private `settings.ini`, the menu's Tools > Media Capture
pressed through AT-SPI, FFV1 + PCM, cut into 640x480 AV1 clips with
the log's events as captions. PCSX-Redux runs the same bench headless.

### 99.2 What the first recording showed (§98 as merged)

- E2M4's eight fogs and the chain's twelve: none missed, two variants
  in each time.
- The ring after *23: at the pickup, 1.2 s after the teleport, the
  pickup sound (`items/inv1.wav`, RAM) and the first breath
  (`items/inv3.wav`, disc) missed.
- The cuts: the pickups' sounds (RAM: the same on master), the first
  breath, the pentagram's first hit (`items/protect3.wav`, disc, 1 s
  after) and the biosuit's warning (`items/suit2.wav`, disc, 1.5 s
  after) missed.
- Why: after a teleport the world's chunks are read first
  (`zone_drive_wanted`), and `stream_frame` returned before anything
  else, the RAM swaps included though they take no drive; and each fog
  played pinned the next variant at `STREAM_PIN`, read before anything
  else and holding the drive.

### 99.3 The RAM swaps beside the drive's reads

- engine/stream.c: a RAM sound's swap has its own state (`swap_a`, no
  longer `sread`'s); `stream_frame` runs the drive's step (`drive_step`:
  a model's read, a sound's pieces, the world's chunks first, the next
  read), then `swap_step`: the swap in flight in once the SPU's DMA is
  done, the next one started while it is free, whatever the drive does.
- `next_wanted(which)`: the drive's assets, the RAM sounds, or both (the
  preload); `none_wanted` a bit for each; the unwanted assets left by
  its first test (most of them).
- One SPU DMA at a time: a swap starts only with the DMA free and after
  the drive's step in the frame; a disc sound's piece and the music's
  refill (engine/music.c `upload`) wait for a swap's DMA in flight (the
  loading screen calls `stream_frame` back to back, and a cheap frame
  can bring `music_tick` within a swap's DMA).
- `set_stuck` logs `no room for` lines (the stream's log).

### 99.4 The fogs' rotation after every living want

engine/snd.c: a pinned fog played no longer pins the next variant at
once. The variant coming in (`tele_in`) is wanted at `TELE_IN_SCORE`,
the tier above the dead's at the sounds' reach: after every living
want, and it takes room only from what nothing else wants. The two
pinned stay pinned until it is in; then it takes the played one's pin.
One rotation at a time. On E2M4 and E4M7 it found no room most of the
time (the pool full of what is wanted more): the same two variants
play meanwhile, none missed.

### 99.5 The ring's breath and the pentagram's hit in RAM; the artifacts' sounds in the order heard

- tools/qcook/sound.c: `items/inv3.wav` (the breath, from the pickup)
  and `items/protect3.wav` (the hit, from the first one) leave the disc
  list: out of the SPU they go to the RAM block again. inv3 on E1M3,
  E3M2, E4M3, E4M5 and E4M7 (10 sectors each), protect3 on E2M4, E2M6
  and E4M7 (2 sectors each): 112 KB of RAM blocks over the 7 maps, of
  the 812 KB §98 gave back. The warnings (inv2, protect2, damage2,
  suit2) and the fogs stay on the disc.
- engine/items.c: an artifact's `noise1` is now the sound heard first
  (the breath, the hit) and `noise2` the warning. At one distance the
  stream wants an entity's noises in their order (engine/ents.c: the
  rank is added to the distance), so the ring's warning was wanted one
  unit ahead of the breath; the recording showed the warning's read
  evict the breath at the pickup, and the breath swapped in again.

### 99.6 Measured

- `CAPTURE=1 MUSIC=1` on PCSX-Redux, then recorded on DuckStation (the
  four scenarios; the misses are the `sndmiss` lines):

| scenario | §98 as merged (DuckStation) | 99.3-99.5 (PCSX-Redux) | 99.3-99.5 (DuckStation) |
|---|---|---|---|
| E2M4's trap, 8 fogs | none missed | none | none (2 variants in each time) |
| E4M7's six teleports, 12 fogs | none missed | none | none (2 or 3 variants in) |
| E4M7, the ring after *23 | the pickup sound, the first breath | none | none |
| the cuts | 3 pickups, the breath, the first hit, the biosuit's warning | 3 pickups, the breath, the biosuit's warning | the same |

  (§98's recording took *23 at the chain's end, the ring's sounds out
  then; the other two columns take it from the map's start, as 99.1
  says.) On DuckStation the ring's pickup sound was in 67 ms after the
  teleport and the breath 117 ms after, while the world's chunks were
  read; the ring was picked up 1.19 s after the teleport; its warning
  (disc) was in 2.8 s after, before its end brought forward. The
  pentagram's first hit, 1 s after its cut, now plays. Before the
  artifacts' sounds were put in the order heard, PCSX-Redux once missed
  the breath three times after *23 in the chain's form, and DuckStation
  showed the warning's read evict it at the pickup.
- `ZONES=2 MUSIC=1 CDMODEL=1` (the 32 maps walked under the drive's
  model): no sound asked and not in on any map; the RAM swaps E2M4 237,
  E1M3 232, E2M7 132, E1M5 81, E4M3 74, E4M7 33, E1M2 25, E2M6 13, four
  maps 1 to 4.
- The release's level arena 1 383 216 → 1 382 864 bytes: 352 bytes of
  code (the swaps' step, the fog coming in, the DMA waits).
- The stream's pool a level (`BENCH_MAPS=1 VIEWS=0
  LEVEL_CAP=1382864`: the release's arena; the zone tables
  `auto_1536_s3`, 3 slots), KB, with the RAM block of sounds and
  §93.5's pool (before §98; the arena 5.3 KB larger then):

| map | the cut | RAM block | pool | §93.5 |
|---|---|---|---|---|
| START | 7 zones | 0 | 694 | 699 |
| E1M1 | whole | 0 | 626 | 632 |
| E1M2 | 9 zones | 34 | 599 | 558 |
| E1M3 | whole | 94 | 479 | 442 |
| E1M4 | 7 zones | 6 | 595 | 558 |
| E1M5 | whole | 50 | 589 | 548 |
| E1M6 | whole | 0 | 686 | 664 |
| E1M7 | whole | 0 | 1071 | 1076 |
| E1M8 | whole | 0 | 861 | 866 |
| E2M1 | 16 zones | 0 | 594 | 599 |
| E2M2 | 9 zones | 0 | 586 | 564 |
| E2M3 | 11 zones | 0 | 586 | 591 |
| E2M4 | 11 zones | 78 | 468 | 415 |
| E2M5 | 10 zones | 0 | 625 | 630 |
| E2M6 | whole | 36 | 490 | 444 |
| E2M7 | 8 zones | 60 | 482 | 435 |
| E3M1 | 10 zones | 0 | 630 | 635 |
| E3M2 | whole | 20 | 771 | 734 |
| E3M3 | whole | 0 | 721 | 726 |
| E3M4 | 5 zones | 8 | 467 | 426 |
| E3M5 | 6 zones | 0 | 482 | 475 |
| E3M6 | 12 zones | 0 | 514 | 519 |
| E3M7 | whole | 0 | 650 | 643 |
| E4M1 | 9 zones | 0 | 619 | 624 |
| E4M2 | whole | 0 | 593 | 599 |
| E4M3 | whole | 96 | 522 | 475 |
| E4M4 | 16 zones | 0 | 587 | 580 |
| E4M5 | 6 zones | 34 | 550 | 511 |
| E4M6 | whole | 0 | 691 | 664 |
| E4M7 | 10 zones | 64 | 450 | 401 |
| E4M8 | whole | 0 | 591 | - |
| END | whole | 0 | 990 | 995 |

  The mean 621 KB (603 at §93.5); the least E4M7 450, E3M4 467, E2M4
  468. 99.5's RAM costs E1M3, E3M2, E4M3, E4M5 and E4M7 20 KB of pool
  each against §98 as merged (E4M7 24), E2M4 and E2M6 4.

### 99.7 The sounds' energy at 99 % (measured, kept at 98)

`QCOOK_SOUND_KEEP=99` (§87.2), cooked with the same tables and slots
and loaded at the release's arena (`BENCH_MAPS=1 VIEWS=0
LEVEL_CAP=1382864`): the RAM blocks 580 → 1614 KB over the 32 maps
(E1M3 94 → 158 KB, E4M3 96 → 156, E2M4 78 → 140, E1M6 0 → 58), the
maps whose sounds all fit in the SPU 14 → 8, the fogs on the disc on
six maps more; the stream's pool 1034 KB smaller in all, 0 to 68 KB a
map (the least: E3M4 467 → 403 KB, E4M7 450 → 404, E2M4 468 → 406);
the levels' streamed files +1.7 MB on the disc. The energy kept stays
98 %.

### 99.8 Left

- The cuts' misses: a pickup sound played on the cut's own tick (no
  lead at all), and the biosuit's warning 1.5 s after a cut (read from
  the disc behind the world's chunks). In the maps a warning comes
  27 s after the pickup, and the artifact wants its sounds as the
  player nears it.
- DuckStation: the four clips; in play, a ring of shadows
  picked up after a teleport (E4M7's *23), a pentagram hit early
  (E2M4, E2M6, E4M7), the fogs where monsters teleport in.

## §100 — §97 merged with §98-§99: the stream's stalls and thrash, the merged kinds' sounds from the disc, the teleporters' hum, the hell knight at 8 units (2026-10-30)

§97's and §99's branches were
merged into master, then the RAM a level measured again. The merged
build's walk of the 32 maps (`ZONES=2 MUSIC=1 CDMODEL=1`, at the
release's arena by `LEVEL_CAP`), which §97's checks had not run, found
frames of 16 to 41 M cycles on E2M4, E2M6 and E4M7 (half a second to more
than one), the stream reading a model every 12 frames there (E2M6: 3
reads in the walk's minute before the merge, 290 after) and E4M7's
teleporters' hum missing for 105 to 191 frames. The levers §97.7 and the
first look named were all built but one (100.6); measured in 100.7.

### 100.1 What the merge did to the pools

The stream's pool lost 22 to 28 KB on every map (the merged code, the
game arena, the clip table: the release's level arena 1 382 864 →
1 359 472 bytes) and more where the new kinds' sounds overflowed the
SPU into the level's RAM block (§86): E4M7's 64 → 152 KB, E4M5 34 → 118,
E4M6 0 → 90, E2M6 36 → 90; on normal E4M7's pool 450 → 329 KB, E4M6 691
→ 564, E4M5 550 → 433, E2M6 490 → 410 (the mean 621 → 580; on hard 575).

### 100.2 Where the frames went

- The sampler (`SAMPLE=1 MAP=E2M6 ZONES=2`): `vram_place` 3.5 % of every
  sample of the walk, as much as the renderer's dearest functions.
- The drive's step timed in the walk's frames: a model's install 1.1 to
  1.6 M cycles, the worst 10 M (the VRAM search again after each
  eviction); with the search fixed (100.3), the compactions: 3 to 6.6 M
  cycles each, the SDK's `memmove` going byte by byte.
- The stream's log: the thrash was the PVS's lower tier (§50.5): the
  kinds' heads, the gibs, the items out of view, read into each room a
  pass made and dropped at the next want of a model in view (E2M6 in a
  minute: `h_ogre` 37 reads, `h_zombie` 36, `h_demon` 34, `h_shams` 31,
  `quaddama` 29; 351 model reads in all, 288 sound swaps).

### 100.3 The VRAM search page by page

engine/stream.c `vram_place`: in each region the candidates (the corner,
the corners each image of its page leaves) were tested against every
resident image (`vram_fits`), the images walked again for every region:
up to 2 x 40 x 193 x 96 tests a search, 1 to 5 M cycles. The images are
sorted by page once a search (`occ_by_page`, in res[]'s order), a region
takes its page's alone, and a region whose fit cannot beat the best
found is passed. The same candidates in the same order: the same places.
`VRAMCHK=1` (scripts/bench.sh) runs the old search beside the new one
and compares them: E2M6's walk, 600 searches and more, none differ.

### 100.4 The stream's work a frame; the compaction by words

- `WORK_FRAME` (engine/stream.c): a frame's evictions of models (1 each)
  and compactions of the pool (4 each) stop at 8; past it a read's room
  waits for the next frame (`reserve` → -2, the room made kept, the
  asset not marked stuck) and a model's install too (`install_model` →
  2: `installing`, the bytes read kept, its places made again from the
  start). The map's preload is not held to it.
- `block_down`: the pool's blocks slid down four words a turn (they are
  16-aligned, 16 bytes a multiple), as bmfrag.c's `pages_down` does: the
  SDK's `memmove` goes byte by byte.

### 100.5 The thrash: the models out of view held off; a margin between wants

- `ROOM_BACKOFF`: a model of the PVS's lower tier (score ≥
  `STREAM_HIDDEN`: a head, a gib, an item out of view) dropped for room
  is not read again for 24 passes (~3 s), or until it comes into view
  (its score under `STREAM_HIDDEN` clears it). E2M6's walk: 290 → 128
  reads.
- `EVICT_MARGIN`: a model takes the room of what is wanted less than it
  by 256 units at least (a pinned want and the sounds by nothing): two
  kinds at about the same distance no longer push each other out as the
  player walks between them.

### 100.6 The sounds; the hell knight; the edicts

- The teleporters' hum (`ambience/hum1.wav`) is an ambient (the split's
  class 1, tools/qcook/sound.c): a `trigger_teleport` is a brush, no
  origin, so the engine makes its static source (the brush's centre) and
  the cook made none, and the hum came with the entities' sounds, after
  the monsters', out of E4M7's SPU. Now in the SPU on E2M6, E4M5, E4M6
  and E4M7.
- The registered game's kinds' sounds (groups `G_ENFORCER` to
  `G_OLDONE`) take the split's last class (5, after the fogs): the
  first out of the SPU, read from the level's file when their kind is
  wanted (`disc`, §98, with its model: `stream_want_group`), so the
  sounds they had pushed out of the SPU into the RAM block (the other
  kinds', the entities') are back in it. Marked `disc` alone, out of
  the SPU only where they fell, they freed 44 KB over the 32 maps: they
  kept their place in the SPU by their weight and the RAM block held
  the others. Their rate stays 98 % as the others' (the lower rate the
  lever also offered is not needed then; 98 was the choice for
  the ears).
- The hell knight's animations thinned at 8 units (`ANIM_THIN`): 31 of
  162 stored frames rebuilt (16 at 3 units), the model 154 312 → 141 052
  bytes (13 KB, under §97.6's estimate: never two neighbours left out).
- Not built: the edicts in the stream's pool. The array cannot move (the
  edicts point at each other: owner, enemy, goalentity, movetarget,
  groundentity, the globals) nor grow into the pool (the brush models'
  room, which depends on the spawned edicts, and the stream's buffers
  lie between them; the load overlay holds the arena's top during the
  load); what it would give is ENT_SPARE's 48 free slots, 15 to 18 KB a
  map.

### 100.7 Measured

At the release's arena (1 358 720 bytes: the levers' 752 bytes of
code), the zone tables `auto_1536_s3` and 3 slots, on PCSX-Redux in
fast forward:

- The walks of the 32 maps (`ZONES=2 MUSIC=1 CDMODEL=1`), the merged
  build before the levers → after, normal | hard: frames over 5 M
  cycles 3 → 0 | 3 → 0, the worst 37.0 → 2.9 M | 22.7 → 3.0 M (E2M6
  37.0 → 1.5, E2M4 20.7 → 1.9, E4M7 16.6 → 1.7 on normal); sounds asked
  and not in 191 → 0 | 105 → 0; the maps' mean frame 414 426 → 404 825
  cycles (−2.3 %) | 442 782 → 431 279 (−2.6 %); the late frames 2814 →
  2826 | 2445 → 2426; the stream's reads 865 → 1230 | 1036 → 1319, the
  merged kinds' sounds now read with their kind (E4M5 32 → 197, E4M6 44
  → 180, E2M7 25 → 144), E2M6's models 290 → 139.
- The first form of 100.6 (the merged kinds' sounds `disc` only where
  they fell out of the SPU): the RAM blocks 1052 → 1008 KB, the pools'
  mean 580 → 581 KB, the reads 865 → 536.
- `VRAMCHK=1` on E2M6's walk: 600 searches and more, no place differs.
- Residency on hard (`BENCH_MAPS=1 VIEWS=0 RESIDENCY=1 SKILL=2`): the
  spots with a monster in the PVS not in 3 → 2, after the stream's reads
  2 → 1 (E1M7's one, there before the merge; E2M6's gone).
- The stream's pool a level (`BENCH_MAPS=1 VIEWS=0`, `SKILL=2` for
  hard), KB, with the RAM block of sounds; the pools' mean 621 before
  the merge, 580 merged, 593 now (hard 575 → 588); the RAM blocks 580
  KB before the merge, 1052 merged, 614 now:

| map | the cut | RAM block | normal | hard |
|---|---|---|---|---|
| START | 7 zones | 0 | 670 | 670 |
| E1M1 | whole | 0 | 603 | 598 |
| E1M2 | 9 zones | 34 | 576 | 567 |
| E1M3 | whole | 96 | 453 | 443 |
| E1M4 | 7 zones | 6 | 571 | 564 |
| E1M5 | whole | 50 | 565 | 550 |
| E1M6 | whole | 0 | 663 | 663 |
| E1M7 | whole | 0 | 1047 | 1048 |
| E1M8 | whole | 0 | 837 | 834 |
| E2M1 | 16 zones | 0 | 565 | 561 |
| E2M2 | 9 zones | 0 | 563 | 562 |
| E2M3 | 11 zones | 0 | 557 | 554 |
| E2M4 | 11 zones | 80 | 440 | 435 |
| E2M5 | 10 zones | 0 | 598 | 595 |
| E2M6 | whole | 38 | 461 | 455 |
| E2M7 | 8 zones | 62 | 453 | 447 |
| E3M1 | 10 zones | 0 | 601 | 600 |
| E3M2 | whole | 20 | 747 | 742 |
| E3M3 | whole | 0 | 694 | 690 |
| E3M4 | 5 zones | 14 | 436 | 434 |
| E3M5 | 6 zones | 0 | 457 | 455 |
| E3M6 | 12 zones | 0 | 484 | 477 |
| E3M7 | whole | 0 | 625 | 625 |
| E4M1 | 9 zones | 0 | 591 | 589 |
| E4M2 | whole | 0 | 568 | 565 |
| E4M3 | whole | 96 | 497 | 488 |
| E4M4 | 16 zones | 0 | 558 | 551 |
| E4M5 | 6 zones | 34 | 516 | 509 |
| E4M6 | whole | 20 | 633 | 625 |
| E4M7 | 10 zones | 64 | 416 | 408 |
| E4M8 | whole | 0 | 565 | 559 |
| END | whole | 0 | 963 | 963 |

- PCSX-Redux hung twice in about fifteen fast-forward runs (once at
  E2M6's walk's start, once in a residency run), ignoring the TERM:
  scripts/headless.sh now kills it after two seconds (its `wait` held
  the bench to its timeout); both runs passed again at once.

### 100.8 Left

- DuckStation: E2M4, E2M6 and E4M7 walked and fought (no
  stall, the monsters and their sounds there), E4M7's teleporters'
  hum, a gib's head and pieces after a close death on those maps.
- The player's pain sounds cutting each other (Quake's channel rule: a
  sound on an entity's channel replaces the one playing there): a
  report, not looked at yet.

## §101 — The pad's polls cut by their own wait: half of them lost on DuckStation (2026-10-30)

Found while an external robot played the release in DuckStation (the
emulator in use, its interpreter) for a video: the player ran at a third
to a half of its speed and a jump held for ten ticks could be lost.

### 101.1 What the game got

- Read in DuckStation's RAM from outside: the ring of polls
  (`engine/pad.c` ring, its snapshots' `connected`) alternated, 50 % of
  the polls whole on the title, 30 to 60 % in play on START; a lost poll
  is a pad not connected for its tick (no buttons, the sticks centred),
  so a held button was pressed again at the next whole poll (the menu's
  cursor jumped two rows) and the walk's speed fell with the friction of
  the empty ticks.
- `pad_poll` sampled every half millisecond: the lost polls stopped at
  their 2nd or 3rd byte of 9. Counters in a diagnostic build (the polls'
  ends by the handler, by the vblank, by the frame's wait): every lost
  poll was cut by `poll_wait`, none during the boot (no frame loop yet:
  no wait), with the interrupts enabled at the wait's entry.
- A 16 times longer `POLL_WAIT` changed nothing; with the handler's
  short way off (§81.4: every byte by the C handler) all the polls were
  lost.

### 101.2 The cause

`poll_wait` (the frame's wait for a poll in flight, §81) tested
`(uint16_t) (TIMER_VALUE(2) - pad_poll.t) < POLL_WAIT`, the timer loaded
first, the last byte's time after. The wait spins while the poll's bytes
come, an interrupt each: one landing between the two loads made `t`
fresher than the timer read before it, the 16-bit difference wrapped to
about 65 535 and the poll was cut there. Since §81 (the poll by
interrupts) on every machine and emulator where the frame waits on a poll
in flight; the headless checks poll no pad (the scripted pad bypasses the
port), so none saw it.

### 101.3 The fix

`t` read first, then the timer: an answer taken between the two reads
makes the wait a little longer, never a cut; a poll that really stops
(no pad, a pad unplugged) is still cut 512 ticks (~120 us) after its last
byte. 16 bytes of code less (the level arena 1 358 720 -> 1 358 736).

### 101.4 Measured

On DuckStation, the release built from the branch: the title's
snapshots 48 of 48 whole (50 % before), in play every sample of 8 whole;
the robot's START route, with the Hard corridor's running jump over the
water, passes at the first try (it fell short every time before).

## §102 — The super shotgun's blue pixels: the skin's fill at its triangles' edges (2026-10-30)

Seen in the robot's videos: a few blue pixels on the super shotgun's
barrels in some frames, depending on the gun's bob, the turn and the
view's pitch; the other guns showed none any more.

### 102.1 The cause

§53's gun atlases keep every texel a triangle overlaps at all (`skin_cover`
with `edges`: the GPU's `floor(u)` takes the texels an edge crosses; the
rocket launcher's muzzle rim needed it). Where a triangle's texture
coordinates graze the skin's unused area, those texels are the fill, a
flat blue in id's skins (palette 208): kept as shown, never replaced by
their painted neighbours, and the GPU drew them when a pose put the
triangle's edge on them. The super shotgun had 12 of them, the axe 13,
the shotgun 3, the super nailgun 5; the guns whose fill is another colour
had some too (the nailgun's black: 17, the rocket launcher's: 12).

### 102.2 The fix

`view_atlas` (tools/qcook/game.c): the skin's fill is the colour most
texels outside every triangle have; a texel only an edge crosses (not
inside a triangle by the centre rule) that has the fill's colour, with no
texel inside a triangle of that colour next to it, is not shown: it takes
its painted neighbours' colour as the unused texels do. The neighbour
test keeps a fill-coloured part that is painted: the rocket launcher's
black muzzle keeps its rim (§53). The cook prints, a gun, the texels
taken and the fill's texels left next to a shown one: 0 for the super
shotgun, the axe, the super nailgun, the grenade launcher and the
lightning gun.

## §103 — The monsters through a liquid's surface: cut by it, past it not drawn (2026-10-30)

Seen in the robot's videos: a monster standing in water was drawn whole,
its legs over the water's surface. Quake's liquids are opaque (the
software renderer's `r_wateralpha` 1, the GL ports' default): its depth
buffer hides what lies on the far side of a surface, the part of a body
under it seen from above, the part above it seen from under it. Here
the body's triangles are sorted among themselves and placed among the
leaf's faces (§63), with no depth to hide them by; drawing the surface
after them would hide them but still pay for them, the triangles under
the surface transformed and sent for nothing.

### 103.1 Which side

`monsters_render` (engine/monsters.c), a body at a time (`monster_draw`):

- the eye in a liquid or not: the view's leaf, the last frame's (the
  world's walk finds it after the entities), as the view's tint takes it;
- a body whose leaf is near a liquid (Quake's water ambient level for the
  leaf, vis's: the liquids its PVS sees within ~1000 units; 0 for most
  leaves), or any body when the eye is in a liquid: its feet and top (its
  box's bottom and top, a unit inside) in a liquid or not, kept in the
  edict's spare byte with a hash of the place they were taken at
  (`body_wet`: taken again only when the body has moved). The monster's
  own water level does not tell: Quake's `SV_CheckWaterTransition` keeps
  the contents at its origin, 1 or the empty's -1, and nothing until it
  first moves;
- both on the side the eye is not on: not drawn at all (no frame
  expanded, no matrix, no packet: E2M6's zombies under its water, seen
  from above);
- through the surface: its height found by halves between the feet and
  the top (to half a unit), and the model cut there (`rl_water`,
  engine/rlist.c): the surface in the model's own z (its matrix's z row
  is the world's when it has no pitch nor roll, a monster's), and the
  side shown, in the entity (`water_z`, `water_m`: the words a brush
  model's camera takes, free for a monster), `RE_WATER` set.

### 103.2 The cut

- `dm_sort` (engine/r_world.s) puts the word in `SP_WATER` (a free word
  of the slow path's saves), 0 for every other model; `dm_exit_1` clears
  it for what follows.
- `ds_records`, at a record's triangles, with a word: each slot of the
  record whose vertex's z is on the hidden side gets its light word's top
  bit (`ds_water`: the pool's z against the surface, the side's sign
  folded in). A front triangle with a marked corner (the three light
  words ORed, beside the NCLIP) goes to `ds_tri_water`: all three
  marked, left out; else to the slow path, as a triangle too near, by its
  own entry (`slow_wpoly`: the world's polygons pay nothing).
- `slow_wpoly` (engine/r_slow.s): a marked corner takes its mark off the
  colour and sends the triangle to the clip path (`WCORNER`); there the
  surface's plane comes before the near plane's (`sp_water`): each
  corner's height over the surface from its pool entry, a
  Sutherland-Hodgman against it, the crossings interpolated from the
  hidden end (`sp_lerp`: view x, y, z, u, v, light) so two triangles on
  one edge make the same point, then the near plane's clip and the
  subdivision as before. Eight view entries (a triangle, two crossings at
  the surface, two at the near plane), +64 bytes.
- `r_slow.c` (the C engine's) does the same in the same order.

### 103.3 Seen from the other side

Quake links an entity into every leaf its box touches (the client's
efrags, the server's leafnums) and draws it when the PVS sees any of
them. The port takes the leaf of its centre (§72). id's maps were vis'd
with the liquids opaque: the leaves on one side of a surface are not in
the PVS of the other side's, so a body through a surface whose centre is
on the other side from the eye was never drawn (the fiend wading in
E2M2's pit, its centre above the water: from under it, nothing of its
legs). With the eye in a liquid, a body near one whose feet are in one
and whose centre's leaf is not seen is now seen by its feet's leaf (a
second pass over the streamed bodies, only while the eye is in a
liquid). The other way (a body's centre under a surface and its top
above it, seen from above) is left: rare, and the scan would be every
frame's.

### 103.4 Measured

- `BENCH_MAPS=1 MAP=E2M2 VIEW=...` (pktview): the fiend in the end's pit
  from 19 units above the water shows its back alone (its idle frame's z
  4..99, the surface at 84: 6 units of it out of the water), the build
  with the cut off the whole body, legs over the surface; with
  `TICKS=40` from under the water, its legs to the surface (nothing of
  it before §103.3); the zombie wading at (168, -8) from under the water,
  its head and shoulders gone over the surface.
- The cost, `BENCH_MAPS=1 HASHONLY=1 PROFILE=0` on START's and episode
  1's 36 views against the base built beside: +0.11 % (-0.01 to +0.6 % a
  view, views without a monster too: the hot code's addresses moved by
  the two words `ds_records` and `dm_sort` gained, the I-cache's order
  made for the old ones). A first version that tested every monster
  triangle's marks in the loop and every body's water each frame cost
  +1.6 %; the triangles' test now lives in the water loop's copy only.
- The C engine (`ENGINE_C=1`, `r_slow.c`): E2M2's views.

## §104 — A telefrag wrapped a monster's 16-bit health (robot run, 2026-10-31)

Found by the robot's black box on E2M1, Hard: four enforcers teleported
into a trap's room at once (Quake's monsters take the teleporters) had
15616 and 31152 health a tick later, all but immortal, and one blocked
the corridor to the exit. `spawn_tdeath`'s trigger hits what stands at
the destination for 50000 (`ent_damage`), and `ent_damage_x256` stored
`health - take` in the edict's 16 bits: 80 - 50000 = -49920, +15616 once
wrapped (and 15616 - 50000 = +31152 for a second hit). Quake's health is
a float: the monster died there. The difference is now taken in 32 bits
and kept at -32768 at least: the telefragged monster dies, its gib test
(`health < -35` and the like) as Quake's. Shub-Niggurath's 30000 (§97,
its 40000 in the 16 bits) still dies to a telefrag.

## §105 — The player stuck in a floor for good: SV_CheckStuck (robot run, 2026-10-31)

Found by the robot on E2M1, Hard, after the lift at (264, 1512) set the
player down and he walked east along y 1512: his origin at z 23.875
where the floor's top in hull 1 is 24.0, an eighth of a unit inside the
solid. Every trace of the move then started in the solid: no move, no
ground, no jump, for good (a softlock; the evidence is kept outside the
repo: the DuckStation state, the black box, the capture). Quake runs
`SV_CheckStuck` every frame before the player's move (sv_phys.c): in a
solid, back to the last frame's place, else nudged 0 to 17 units up and
a unit aside, the first open place. The port had none (pmove.c leaves
out `SV_TryUnstick` too, a different remedy). `check_stuck` (game.c)
does it before `pmove_step`, the world's hull 1 its test (one point's
contents a tick), the last place only when this level's and near.


## §106 — A fight's frame measured where it happened: E2M7's (2026-10-31)

- **The question**: the robot's run of episode 2 on Hard (its DuckStation
  captures, the frames repeated counted while the view moves) played at
  26-44 fps on average a level, a third to a half of the time at 30, and
  12-13 fps in E2M7's biggest fight (16 to 21 monsters within 700 units,
  15 missiles in the air); the frame rate follows the monsters near and
  the fight (E2M7: 47 fps with none near, 23 with six or more). Where
  does a fight's frame go?
- **The measure**: `MAP=E2M7 MONSTERS=3 SAMPLE=1 PROFILE=0 SKILL=2
  VIEW=1780,1803,-304,204,0`, the player where the robot fought: the
  crowd (the level's 78 monsters on Hard, all awake), then the minute of
  fight (the player's rockets, a frame drawn each tick), PCSX-Redux's
  cycles:
  - the fight: 3.25 vblanks a frame on average (~18 fps); the drawing's
    own cycles 1.48 M with 8 streamed models drawn (its first 300 ticks),
    0.95 M with 9 (its last);
  - the crowd alone (nothing drawn): `ents_tick` 351 k cycles a tick
    (0.62 vblank), 1.45 M at most; its traces half of it (178 k): the
    walk's 40 %, the flight's 16 %, `CheckBottom`'s 12 %, the corners'
    10 %, the sight lines' 8 %, the touches' 7 %, the attacks' 5 %; its
    samples the collision's 60 %, the game's logic 24 %;
  - the fight's samples, the game's work alone (the bench's aim left
    out):

    | part | share |
    |---|---|
    | the models drawn (the sorted monsters' triangles, the records' vertex light) | 23.8 % |
    | the wait for the vblank (`_default_vsync_halt`) | 16.0 % |
    | the collision (clip.s, w_leaf.s: the monsters' traces, contents, leaves) | 15.5 % |
    | the slow path (r_slow.s: the near polygons) | 11.7 % |
    | the world's walk and faces (r_world.s) | 11.0 % |
    | the game's logic (ai.c, ents.c, e_*.s, game.c) | 9.0 % |
    | the render's setup (rlist.c, render.c, the frame cache, the HUD) | 5.9 % |
    | the dynamic lights on the faces (the rockets') | 4.0 % |
    | the stream, the sound, the BIOS | 2.0 % |

    The hottest functions: `ds_tri` 7.1 %, `world_point_leaf_from`
    3.0 %, `sp_o_bounds` 2.9 %, `lrl_v` 2.8 %, `ds_o_dcull` 2.3 %,
    `deo1_tri` 2.1 %, `ct_root` 2.0 %.
- **What it says**: in a fight drawn, the drawing outweighs the
  simulation (the models 24 %, the world with its slow path and lights
  27 %, against the collision's 15.5 % and the logic's 9 %); without the
  drawing the crowd's cost is the collision's, as §70 found. The waits
  are the double buffer's, a frame taking 3 to 4 vblanks.
- **The bench's tools** (no change to the game):
  - with `MONSTERS=2` or `3`, `VIEW=` puts the crowd's player there (the
    view's eye, the origin 22 units under it) and preloads the stream
    there;
  - a sampled run (`SAMPLE=1`) prints no sound, stream or missile log
    (`snd_log`, `stream_log`, `game_monster_missile`'s line): they took
    15 % of the first sampled fight;
  - the sampler counts 64-byte blocks: its 8192 entries cover 512 KB, the
    whole text (311 KB; past 256 KB lay the SDK's vblank wait, stream.c,
    zone.c, the triggers), the block's size on the `sample` line;
  - `prof_sample_pause`: the fight's aim (a sight line to every monster
    each tick, 9.6 % of the samples) and the `e_seen` check are left out.

## §107 — A fight's two levers: the sorted bodies lit by their pool, the grid on every map measured (2026-10-31)

- **The request**: §106's two first levers, measured on its fight
  (`MAP=E2M7 MONSTERS=3 SAMPLE=1 PROFILE=0 SKILL=2
  VIEW=1780,1803,-304,204,0`, the frames' cycles compared window by
  window: the game is the same, the models drawn the same in each).
- **(1) The sorted bodies' lights by their pool's entries**:
  `light_recs_lod` wrote a light byte for each slot of each record at
  the body's band (the slot's vertex's normal through the yaw's table,
  times the light), records shared by every body of the model, so a
  light computed for a frame could not be kept for the next (eight
  zombies in view write the same bytes in turn). The fight lit 4.66 M
  slots for 1.99 M vertices of the bands and 3.37 M entries of the pools
  (a band's pool holds every vertex: the band's expanded, the others
  copies of their parents, engine/mdl.c, and the band's slots reference
  both): 2.3 slots a band's vertex, 1.4 a pool's entry.
  - `light_pool` (r_world.s, in `light_recs_lod`'s place) writes the
    light into byte 7 of each entry of the pool (0 since `mdl_expand`:
    the word `z | normal << 16`), two entries in flight (the next one's
    factor read while the multiply runs); the pool's entries come in
    `lod_band`'s upper half (rlist.c, `RE_LODBAND`'s, split in
    `draw_model`);
  - `XFORM`'s light 3: the three loads of a turn read byte 7 of the
    vertices the transform just loaded (their addresses still in the
    registers), the record's light bytes left alone; the sorted loop
    (`ds_xf`, its fogged `ds_xfg`) takes it, and `ds_records` no longer
    works out where a record's light bytes are (two loads and four
    instructions a record);
  - the unsorted alias models (the items, the view model) and the brush
    models keep `light_recs` and their records' bytes.
  - The display lists identical: the 128 views (`BENCH_MAPS=1
    HASHONLY=1 PROFILE=0`) and E1M1's monsters' frames (`MONSTERS=1
    MAP=E1M1`, 16 frames, every packet), against references the base
    saved (`scripts/dlcheck.sh`).
  - The fight's drawing -2.62 % (13.68 M -> 13.32 M cycles over its 12
    windows, each -1.6 to -3.3 %), its samples -1.3 % (389730 ->
    384809); the 128 views -0.3 % (30 faster, E4M5.v1 -3.2 %, E4M5.v0
    -2.9 %, E3M4.v2 -2.2 %; none slower).
- **(2) The near polygons' grid (§92) on every map**, measured, not
  changed: master cuts START's and E1M1's alone (`Q1PSX_TESS_MAPS`);
  `TESS=all`:
  - the fight's drawing -1.75 % (13.68 M -> 13.44 M), its samples -1.0 %
    (the slow path 11.7 -> 6.5 %, r_tess.s's own 4.3 %): the bodies, not
    the world, weigh in a fight;
  - the 128 views (with (1)) -5.73 %, the 120 outside START and E1M1
    -6.11 % (39.95 M -> 37.51 M): 103 faster (END.v2 -30 %, E4M2.v1
    -20.4 %, E4M7.v1 -19.8 %, E4M3.v0 -17.9 %), 17 slower (E3M2.v2
    +5.8 %, E3M5.v0 +3.7 %); 18 views past a vblank's 564 k cycles ->
    16;
  - the pictures move by a texel where a polygon is cut (§92's boards):
    a call left open.

## §108 — The near polygons' grid on every map (2026-10-31)

The choice after §107's measure: the regular grid before the slow
path (§92) cuts every map's near polygons, not START's and E1M1's alone.
`Q1PSX_TESS_MAPS` defaults to `all` (CMakeLists.txt; `scripts/bench.sh`'s
`TESS` too). A build folder's cache that still holds the old default
(`START,E1M1`) is moved to `all` at its next configure (the build runs
it when CMakeLists.txt changes); any other list is kept as it is. The
measure is §107's: the 128 views -5.73 % (103 faster, 17 slower, 18 past
a vblank -> 16), E2M7's fight's drawing -1.75 %; the pictures move by a
texel where a polygon is cut (§92's boards). The 128 views' display
lists change with it: a `scripts/dlcheck.sh` reference saved before must
be saved again. Checked: the release folder's cache moved to `all` at
its build; E2M3's views benched with the defaults draw what `TESS=all`
drew (its frames' cycles within 20 of them), not the old default's.

## §109 — A door seen through: the brush models' triangles turned round (2026-10-31)

- **The report**: on E2M1 a door was transparent until it began to open,
  and then only one of its four triangles showed (the robot's capture).
  It is the door at x 737-751, y -255 to -129: four `func_door`s (`*13`
  up, `*14` down, `*15` north, `*16` south), four wedges whose faces
  toward the corridors are triangles meeting in an X.
- **The cause**: the model loop (r_world.s `RECLOOP de`: `draw_model`, a
  brush model's blocks, the pieces of it the world's BSP cuts, §46)
  leaves out a triangle whose NCLIP is 0 or more, the alias models'
  back faces (`tri_emit`, §54; `_tri_far`, §93); a quad is never tested.
  The cook wrote a brush model's polygons in Quake's winding, the
  world's (the walk takes the world's faces by their planes, not by
  NCLIP), which gives a face seen from its front a positive NCLIP: every
  triangle of a brush model was left out seen from the front and drawn
  seen from behind. Most brush faces are quads (a fan of quads, a
  triangle when the count is odd) and lost only their odd triangle; the
  X door's faces are triangles, and the closed door, inside the wall's
  leaves, is drawn as pieces the cut makes (engine/bmfrag.c), triangles
  as often as not: all of it gone.
- **The fix**: the cook turns a brush model's polygons the alias models'
  way round (tools/qcook/world.c `bmodel_turn`: corners 1 and 2
  swapped, a quad's too, which keeps its diagonal: the GPU's two
  triangles are the same), and the cut keeps the turn in every piece it
  makes of them. The item boxes were turned in §93 (their fans).
- **Checked**: `BENCH_MAPS=1 MAP=E2M1 SKILL=2 VIEW=772,-156,46,186,0
  TICKS=64` (the door opening, a picture every 8 ticks: closed, then four
  triangles parting), the door from both corridors (`VIEW=690,-192,48,0,0`
  and `620,-192,48,0,0`; `870,-192,48,180,0` with the barrier beside it
  opened, `USE=t44`) before and after the turn (pktview); E2M1's twelve
  other views: the same pixels, but for 42 of one view (a dither step on
  a lift's triangle). The display lists change wherever a brush model's
  triangle is drawn: a `scripts/dlcheck.sh` reference saved before must
  be saved again.

## §110 — The ogre's arm lost at a distance: the level of detail's cost measured for real (2026-10-31)

### 110.1 The cause

Every ogre lost its right arm in some animations, the chainsaw flying
alone like a thrown fist. Not the hand cut (§62): the ogre whole (band 0)
has its arm. From band 1 (64 units) on, the forearm was gone in `swing5`
and the frames like it, the chainsaw held by nothing. §88's cost sees a
collapse's move along the normals of the vertex's triangles; the forearm
is a tube open at the wrist (its end inside the hand: the model's only
four border edges), and its rings collapsed one onto the next toward the
shoulder, each move along the tube, square to every side's normal: a
cost of about 0 a step (§88.5's ogre of E3M7.v4: 19 vertices under 0.4
units at band 1), band 1 taking all of them. At rest the arm hangs along
the body and the loss hardly shows; swung out, 30 units of arm are gone.

### 110.2 The measure

A scratch tool (outside the repository) draws a cooked body at its bands
the way §64's sheets do, and measures each band's true error: the
two-sided distance between the whole model's surface and the band's
(points sampled on each surface, the nearest point of the other's
triangles), over every 8th frame played, in pixels at the band's near
depth (the GTE's H of 160); the tolerance is 2 px (§88). Before, every
body but Shub-Niggurath (whose collapses all lie past the draw distance)
was over it somewhere, the bands 1 / 2 / 4 / 8 / 12:

| body | before (px) | after (px) |
|---|---|---|
| soldier | 6.2 / 3.1 / 1.6 / 2.5 / 6.2 | 0.6 / 0.7 / 1.7 / 1.6 / 1.9 |
| dog | 4.7 / 5.6 / 2.8 / 6.7 / 4.5 | 0.8 / 2.3 / 1.4 / 1.9 / 1.7 |
| ogre | 39.2 / 19.6 / 9.8 / 8.0 / 5.5 | 0.0 / 0.7 / 1.4 / 1.4 / 1.7 |
| knight | 0.6 / 1.2 / 18.8 / 9.4 / 8.0 | 0.6 / 1.4 / 0.7 / 1.4 / 1.7 |
| zombie | 1.3 / 2.2 / 5.4 / 6.1 / 4.1 | 1.3 / 1.5 / 1.8 / 2.1 / 1.9 |
| wizard | 1.0 / 1.2 / 1.1 / 3.8 / 3.9 | 1.0 / 1.2 / 1.1 / 1.5 / 1.2 |
| demon | 19.1 / 9.6 / 5.8 / 7.3 / 7.1 | 0.0 / 0.0 / 1.0 / 1.3 / 2.8 |
| shambler | 0.0 / 0.0 / 1.1 / 4.7 / 4.0 | 0.0 / 0.0 / 1.1 / 1.4 / 1.3 |
| boss | 134.3 / 67.1 / 33.6 / 16.8 / 11.2 | 0.0 / 0.0 / 0.0 / 1.2 / 1.7 |
| enforcer | 11.3 / 5.7 / 2.8 / 2.2 / 6.9 | 0.7 / 0.7 / 1.4 / 1.8 / 1.7 |
| hknight | 15.4 / 7.7 / 21.6 / 12.1 / 8.8 | 0.0 / 1.7 / 1.4 / 2.5 / 1.9 |
| tarbaby | 7.6 / 3.8 / 2.4 / 1.1 / 0.7 | 0.0 / 0.0 / 0.6 / 1.0 / 1.1 |
| shalrath | 38.2 / 19.1 / 9.6 / 6.1 / 6.8 | 1.0 / 1.2 / 1.2 / 1.4 / 1.5 |
| fish | 27.7 / 13.9 / 6.9 / 3.5 / 2.3 | 0.6 / 1.8 / 1.3 / 1.7 / 1.7 |

Seen beside Quake's (the worst frames): the knight (`death10`) and the
hell knight (`slice9`) without their swords at band 4, Chthon (`shockc1`,
53.7 units) without the spikes of his back at band 1, the shalrath
(`attack1`) with its legs' ends short at band 1: all the same way, a part
drawn back into its root step by step, each step cheap.

### 110.3 The cost, two terms

`tools/qcook/lod.c`, the ordering's cost, the larger of two distances,
the worst frame played (times the skin's 3 as before; a collapse that
turns a triangle over still refused):

- **the rims**, added to §88's normals (the root of the sum of the
  squares): v's move across an edge at it where the surface ends, within
  the edge's triangle's plane, which no normal sees. A border's edge (one
  triangle: the forearm's open end), or a fold's (two triangles turned
  back on each other, their normals more than 90 degrees apart: a thin
  sheet's edge) by the part its other triangle's normal misses (the
  normals' cosine squared: all of it at 180 degrees, none at 90), the two
  triangles' mean. Alone it brought the near bands within the tolerance
  but not the far ones (the hell knight still lost its sword at band 8,
  10.8 px);
- **the memory**: the model's own vertices that the corners of the
  triangles the collapse changes stand for (each vertex keeps a list:
  itself and those collapsed onto it), their farthest from the triangles
  after the collapse with a corner among them (the whole model's when
  none is left: a part going), what the steps lose together: a tube
  drawn back ring by ring, a blade whose tip loses its last triangles
  (the tip's own vertex is a corner, not an end, of the collapse: the
  first try, the two ends' lists alone, left the sword's 36.7 units).
  It can only raise a cost, so it is taken lazily: for the cheapest by
  the first term, which is then chosen again; a vertex next to one whose
  triangles changed takes it again when wanted. It replaces §88's
  accumulated error (a vertex's own chain, added to every later step of
  it: blind to a neighbour's loss, too dear elsewhere).

The bands, the numbering, the tables and the engine are unchanged; the
stream's cook takes 3.2 s (2.8 before).

### 110.4 Measured

- The errors above (after): every band of every body within 2.8 px (the
  demon's band 12; 2.5 below it), most within 2: the memory measures the
  vertices, the tool samples between them.
- The triangles drawn, bands 1-15 summed per body (the cook's tables):
  35 747 -> 38 034 (+6.4 %): Chthon +33.3 %, the ogre +20.0 %, the fish
  +8.6 %, the hell knight +6.1 %, the knight +5.6 %, the dog +4.3 %, the
  enforcer +2.5 %, the shambler +0.9 %; the zombie -2.8 %, the soldier
  -2.1 %, the wizard -1.6 %, the spawn -1.3 %, the demon -0.8 %, the
  shalrath -0.6 % (the accumulated error cost them more than it caught).
  The ogre's band 1 keeps its 266 triangles (228 before), band 8 164
  (144).
- §106's fight (`MAP=E2M7 MONSTERS=3 SAMPLE=1 PROFILE=0 SKILL=2
  VIEW=1780,1803,-304,204,0`, with §109) against §108's: the drawing
  -0.50 % (13.44 M -> 13.38 M cycles over its 12 windows, each -1.6 to
  +0.3 %), the same streamed models drawn in each window; its samples
  386 011 -> 384 637.
- The pictures: the ogre in `swing5` at bands 0 / 1 / 4 / 8, the hell
  knight in `slice9` at 0 / 4 / 8 (its sword kept, thinner at 8), the
  crucified zombie at 0 / 4, the shalrath in `attack9` at 0 / 8, each
  beside Quake's.
- The display lists of a body drawn past band 0 change (most views with a
  monster): a `scripts/dlcheck.sh` reference saved before must be saved
  again.

## §111 — The vblank's wait given back: a late frame no longer waits for the next vblank (2026-10-31)

- **The question**: §106's fight spent 16 % of its samples in the SDK's
  wait for the vblank (`_default_vsync_halt`). `vid_flip` waited for the
  previous frame's drawing (`DrawSync(0)`), then always for the next
  vblank (`VSync(0)`, then the cap's share), and only then showed the
  buffer the GPU had finished (`PutDispEnv`) and started the GPU on the
  list just built (`DrawOTagEnv`): a frame already late still waited for
  the next vblank, half a vblank on average in a fight of 3 to 4 vblanks
  a frame. The CPU should begin the next frame at once and the flip
  happen at the vblank without it, with no tearing, no arena written
  while the GPU has yet to read it, the caps' pacing kept, the warp's
  control fed as before, the pad's interrupt poll and the per-vblank
  tick ring as they are.

### 111.1 The flip at the vblank

- **The vblank's interrupt** (`vid_vblank`, the SDK's `VSyncCallback`,
  after its own count; the pad's poll begun first, `pad_vblank`, §81.2,
  no longer the callback itself): the buffer started last is shown once
  the GPU is done with it (the SDK's queue empty and the GPU idle:
  `DrawSync`'s test, without its wait) and the cap's share of vblanks has
  passed since the last buffer shown; a buffer built meanwhile, waiting
  for it, is started there, into the buffer just hidden. Only GP1(05h),
  the display's start, is written (the first of `PutDispEnv`'s four
  words: the display's ranges and mode, the same for both buffers, are
  set once, `vid_init`'s `PutDispEnv` of buffer 1, the first frame's
  buffer 0 then started at once).
- **`vid_flip`**, the frame built: `DrawSync(0)` as before (the previous
  frame's list read: its arena is the next frame's, and every upload the
  frame queued, the stream's `LoadImage`s, done), then
  - the previous frame on display already (the interrupt showed it: this
    frame missed its share), this frame's buffer is free: its list starts
    at once, the interrupt shows it once drawn, and the next frame begins
    at once, no vblank waited for;
  - else (on time) this frame waits for the vblank that shows the
    previous one, the interrupt starting its list there, and the next
    frame begins at that vblank: the cap's pacing, as before.
- **What holds**: a frame's list starts before `vid_flip` returns, so
  whatever the next frame queues (the stream's uploads) comes after it
  in the SDK's queue, as before, and the arena the next frame writes is
  the one the previous frame's list was read from; the static chains a
  frame links (the 2D gun's frames and CLUT, §45) are relinked two
  frames later at the soonest, after a `DrawSync(0)`, as before; a
  buffer is shown at a vblank only, and its list starts only while the
  other buffer is on display (late: the previous frame shown already;
  on time: in the interrupt, right after the show): no tearing.
- **The interrupt's work**: the show (one GP1 word) and, on time, the
  list's start: `DrawOTagEnv` with the SDK's queue empty starts the DMA
  at once (`EnqueueDrawOp` masks the interrupts by IRQ_MASK and gives
  the mask back: the CPU's own interrupt bit stays off in the handler,
  no nesting); the DMA's end runs the queue as before (the SPU's DMAs,
  the CD's, untouched). The interrupts' stack: 524 bytes never reached,
  as before. `vid_flip`'s critical section (IRQ_MASK cleared) covers the
  test and the start or the wait's setting: the interrupt cannot show
  the previous frame between them.
- **A frame shown sooner**: a late frame is shown at the first vblank
  after it is drawn (§106's wait: about half a vblank sooner, with the
  frame after it begun at once); a frame on time is shown at the same
  vblank as before.

### 111.2 The pacing and the game's time

- Frames within their share are paced as before: each begins at a
  vblank, a share after the last, and shown a share after the last (60:
  every vblank; 45: 1, 1, 2 in turn; 30: every second one).
- A frame that misses its share is shown for an uneven number of
  vblanks (a frame of 1.3 vblanks: 1 or 2 in turn, 46 fps where it ran
  at 30), as with the 45 fps cap; the 30 fps cap stays for a steady
  pacing while the frames fit in two vblanks.
- The game's time is §30's: a frame takes the polls since the last
  frame's start, one tick each (the ring unchanged); a late frame's next
  one begins between two vblanks and takes the polls that came, so the
  ticks a frame shows and the vblanks it is shown for differ by one now
  and then where the frames are late (the 45 fps cap's unevenness).

### 111.3 The stats; the warp's control

- `vid_stats.vbl`: the vblanks from a frame's start to the next one's
  (the polls the next one takes): the overlay's fps, the benches' `vbl=`,
  the walk's polls.
- `vid_stats.vbl_whole`: the warp's control's input (`r_warp_feedback`,
  §79.2): the frame's vblanks as it took them waiting for the vblank,
  its share when on time, else its time (from the last frame's start)
  rounded up to vblanks. The control raises, lowers and gives up (its
  `more`: aim at a vblank more from the floor) for the same frames as
  before. Giving up now costs frames a second where it cost none (a frame
  just over a vblank at 32 px, shown at ~57 fps, drawn at the floor in
  about 1.5 vblanks: ~40 fps), and the control keeps the warp's look
  first, as before (§79.2; the warp must not distract).

### 111.4 Measured

PCSX-Redux in fast forward, the base master d7de758 with the bench's
measures (the fight's vblanks a frame over each window of 300 frames;
`bench pace`), against the branch:

- **§106's fight** (`MAP=E2M7 MONSTERS=3 SAMPLE=1 PROFILE=0 SKILL=2
  VIEW=1780,1803,-304,204,0`): 3.53 -> 3.02 vblanks a frame on average
  (17.0 -> 19.9 fps, +17 %), each window -11 to -18 % (4.32 -> 3.84 in
  the first, 3.16 -> 2.66 in the last); the same game in each window
  (the monsters alive, the streamed models drawn; the frames' own cycles
  within 0.03 %); the samples 384 726 -> 323 316 (-16.0 %: the minute of
  fight played in 16 % less time), the SDK's wait (16.0 %) gone, the
  waits left 0.4 % (as before).
- **The same on master 6ff9264** (§114's shared budget and §115's light
  merged in; master's vid.c, vid.h, pad.c and pad.h with the bench's
  measures against the merged branch): 3.50 -> 2.95 vblanks a frame (17.1
  -> 20.3 fps, -15.6 %), each window -11 to -18 %, the same game in each
  (the frames' cycles within 0.05 %); the samples 380 993 -> 315 540
  (-17.2 %; the SDK's wait 17.1 % there). §114's lighter frames (the
  drawing -3.6 %) moved master's fight by 1 % alone (3.53 -> 3.50 vblanks
  a frame: what they saved was mostly waited away at the vblank); with
  the flip they show. The 128 views identical to master's (a `m373`
  reference); the release's arena 1 351 984 bytes.
- **The game bench** (`BENCH_GAME=1`): the guns' views 2.00 -> 1.56-1.68
  vblanks a frame (their frames 0.88-0.93 M cycles), the rocket
  launcher's 3.00 -> 2.31, the lightning gun's 2.00 -> 1.87, the HUDs'
  2.00 -> 1.81; the walk at 60 Hz 1.72 -> 1.33 vblanks a frame (frames
  at one vblank 140 -> 254 of 300, at two 156 -> 45), at 30 Hz 1.50 ->
  1.15 (153 -> 257 at one); its 23 dumps (the guns, the HUDs, the menus,
  the water) identical.
- **The pacing** (`bench pace`, 60 frames, nothing drawn): within their
  share 60, 80 and 120 vblanks at 60, 45 and 30 fps, each buffer shown
  for its share, as before; missing it (1.3 shares) 120 -> 78 vblanks at
  60 (shown for 1 or 2: 43 / 17), 140 -> 104 at 45, 180 -> 156 at 30
  (2 or 3: 26 / 34).
- **The 128 views** (`BENCH_MAPS=1 HASHONLY=1`, `scripts/dlcheck.sh`
  saved on the base): the display lists identical; their cycles +0.04 %
  (86 views slower, 40 faster, -1 320 to +2 060 cycles: the pad's
  interrupts land elsewhere in a frame begun sooner after its vblank).
- **The game's own path** (`BENCH_MAPS=1 PLAY=1 MAP=E1M1`): the loading
  screens, then 30 frames as in play (the pad's polls, the ticks, the
  HUD, the sound, the music): through; E1M1's views past a vblank at
  1.37 to 1.81 vblanks a frame (their work between one and two vblanks:
  2 with the old flip).
- **The RAM** (the release's objects): vid.c +344 bytes of code, +28 of
  BSS; pad.c -12.

### 111.5 The bench's measures

- The fight's lines (`MONSTERS=3`) give each window's vblanks a frame
  (`VSync`'s count over its 300 frames, the lines' own time left out),
  where they gave the last frame's alone.
- The game bench ends with `bench pace` lines: under each cap, 60 frames
  whose work fits in their share (0.8 of it) and 60 that miss it (1.3),
  nothing drawn: the vblanks they took and how long each buffer was
  shown (`vid_shown_for`, counted where a buffer is shown, bench builds
  only).

### 111.6 Left

- DuckStation (TESTING 11.90): no tearing in a fight and in
  quick turns, the caps' pacing (45 and 30 within their share), the pad
  (a press, the walk's speed, the menu's cursor), the sound and the
  music, the streaming (the monsters' skins in at a fight, a level's
  zones), the loading screens. The branch was tested on DuckStation
  before its merge into master (2026-10-31).
- PCSX-Redux draws at once: the GPU's own time (DuckStation's, a
  console's) shows a late frame later, at the first vblank after its
  drawing; a frame whose previous one is drawn after its vblank (the GPU
  the slower: §78 measured it the faster) still waits for the next one.
- A real console: GP1(05h) written at the vblank's start (the interrupt)
  rather than after `VSync` returned; the same register, earlier in the
  blanking.
- The warp's `more` (111.3) trades frames a second for the floor's look
  now: a call left open, whether a frame just over a vblank should keep
  the 32 px warp instead.

## §114 — The monsters' shared detail budget (2026-10-31)

(The number is provisional: §111-§113 are the levers' branches, not
merged.)

- **The request**: the compromise that keeps the bodies readable and
  coherent, chosen from the shared-budget LOD's proof of concept. That
  is an HTML page outside the repo: it draws the cooked bodies band by
  band as the engine does, reads `STREAM_0.DAT` at run time and holds no
  data of its own. The result goes in a release to play before
  any merge.
- **What the page showed**:
  - The starting rule pulls a near body straight to L: a body's band is
    `max(d, L)`, d its depth's band, L a level the bodies in view share
    (the least level whose total fits the budget, a floor at most).
    Example: a shambler at 68 units goes from band 1 to band 10.
  - The error that rule adds: band b's collapses are within 2 px at 64 b
    units (§110), about 0.8 b world units, so 128 b / d px at depth d.
    The shambler above shows up to 19 px of error: unreadable.
  - The fix is a cap: a body's band at most k times its depth's band, so
    its error at most k times today's (3 px at a band's start: §88's
    bands of 42.7 units, z >> 8 in the view's units).
- **The choice** (the page's `tune.mjs`, the cooked tables alone): 300
  random fights a size, 64-900 units from the eye, the 13 kinds without
  the bosses. Triangles saved against the distance alone, and the error
  added on the bodies raised:

  | budget, floor, cap | 10 / 14 / 20 / 26 bodies | error added, worst / mean |
  |---|---|---|
  | 800, 16, none | 2.4 / 23.3 / 45.6 / 46.5 % | 32 / 7.5 px |
  | 800, 16, k 2 | 2.3 / 20.1 / 29.1 / 29.1 % | 6.0 / 4.8 px |
  | 800, 16, k 1.5 | 1.9 / 15.5 / 19.5 / 19.6 % | 4.5 / 4.0 px |
  | 800, 16, k 1.25 | 1.4 / 9.9 / 11.9 / 11.8 % | 3.7 / 3.5 px |
  | **600, 16, k 1.5** | 12.2 / 19.6 / 19.5 / 19.6 % | 4.5 / 4.0 px |

  k = 1.5 keeps the error within 4.5 px, near the worst §110 leaves
  today (2.8 px at 64 b units, 4.2 px at the engine's band starts). The
  budget only decides how often the bodies are raised, never how far.
  600 vertices was chosen on the fight below; the page and the release
  default to it.
- **The engine** (engine/rlist.c, `rl_model_stream`, `rl_begin`):
  - The settings: CMake `Q1PSX_LOD_BUDGET` (600 expanded vertices a
    frame; 0: none, the code compiled out), `Q1PSX_LOD_FLOOR` (16),
    `Q1PSX_LOD_CAP` (384, k = 1.5 in 256ths); `scripts/bench.sh` takes
    `LODBUDGET`, `LODFLOOR` and `LODCAP`.
  - A body's band d is taken from its depth as before. Then `lod_count`:
    - its cap c = z x `Q1PSX_LOD_CAP` >> 16 (z its depth in the view's
      units, d = z >> 8; at least d);
    - its band max(d, min(L, c)), L the frame's level;
    - it adds to the frame's sums what each level would draw of it, as
      expanded vertices (the band's `num_verts`: the cooked qst_lod, no
      new data): the base, its depth's band; the levels between d and its
      cap, theirs; past its cap, its cap's (a difference array);
    - a body wholly past the draw distance (`r_draw_z`, render.c) is not
      counted.
  - `rl_begin` (`lod_level`) finds the least level within the budget
    from the last frame's sums. L moves one band a frame toward it, and
    goes down only once a lower level fits within 7/8 of the budget:
    - a crowd turning into view is raised one band a frame;
    - no level flickers at the budget's edge.
  - About 150 bytes of BSS (the release's arena 1 352 416 B). The bench's
    fight prints a `mon lod` line a window: the level, the bodies raised.
- **Measured**: §106's fight (`MAP=E2M7 MONSTERS=3 SAMPLE=1 PROFILE=0
  SKILL=2 VIEW=1780,1803,-304,204,0`), against the budget off
  (`LODBUDGET=0`: master's bands, the code compiled out), window by
  window. The game is the same: the same models drawn and monsters alive
  in each window.

  | | budget 600 (the default) | budget 800 |
  |---|---|---|
  | drawing (12 windows) | -3.57 % (13.38 M -> 12.90 M cycles) | -0.40 % |
  | windows at work | 7 | 3 |
  | those windows | -3.6 to -8.1 % | -0.2 to -3.0 % |
  | the idle windows (its sums) | +0.1 % | +0.1 % |
  | level a frame | 1-14 on average, 16 at most | about 6, 8 at most |
  | bodies raised a frame | 3.9 to 7.7 | about 4.5 |
  | samples | -1.09 % | |
  | models left out for the packets' room | 203 -> 1 | 203 -> 22 |

  The models left out (`r_model_skipped`) are monsters not drawn at all
  in master's busiest window, for want of packet room.
- **For a test**: the release (`cmake --preset release`) carries the
  600-vertex budget; `-DQ1PSX_LOD_BUDGET=0` builds without it. To be
  seen in play on DuckStation: a big fight's near and mid-range
  monsters, no part lost, the bodies' detail changing one band at a
  time.

## §115 — The monsters lit as Quake lights them (2026-10-31)

(The number is provisional, like §114's.)

- **The report**: the maps were brightened to 1.66 (the cook's
  `LIGHT_GAIN`, §79), but the monsters seemed left as they were. Very
  dark, they could not be seen in places; E4M2 is the darkest.
- **The gain is every map's**: `scripts/cook.sh` passes `--light-gain`
  to every map's cook. The monsters' light (a leaf's, `qw_leaf.light`)
  was made from the faces' gained vertex lights.
- **Why the monsters fell behind the walls** (a scratch tool on the
  paks; for every monster's spawn point, before the gain):
  - **The light's source.** Quake lights a model with R_LightPoint
    (r_light.c): the lightmap where a line straight down from its origin
    first meets a lit face, the floor under it. The cook took the
    average of the leaf's faces, the walls and ceilings in it. Quake's
    is 1.04-1.27 times brighter (median per map):

    | map | Quake's light, median | the leaf average, median | Quake / port, median |
    |---|---|---|---|
    | E1M1 | 81 | 72 | 1.24 |
    | E2M7 | 49 | 42 | 1.18 |
    | E4M2 | 38 | 34 | 1.24 |
    | E4M3 | 45 | 36 | 1.19 |

  - **The shading.** Quake shades an alias model from its light up to
    twice it on the side the light comes from (r_alias.c: ambient +
    shade x cos, both the model's light below 96). The port's light
    table (`gdata.dots`, tools/qcook/game.c) spans 1 to 1.5 times.
  - Against the walls at 1.66 times Quake's, the bodies came out about
    30 % darker than Quake drew them beside the same walls.
- **The fix**:
  - The cook (tools/qcook/world.c):
    - a leaf's light is now Quake's R_LightPoint, ported
      (`light_point_rec`: the front side first, the node's faces at the
      crossing within Quake's CalcSurfaceExtents, then the back side;
      sky and liquids skipped);
    - it is averaged over the points of a 3 x 3 x 3 grid in the leaf's
      box that lie in the leaf, at the cook's gain;
    - a leaf no line reaches keeps the faces' average.
    - The monsters' leaves are covered at 95-100 %. Their light (median
      before the gain) comes out at Quake's own: E1M1 80 (Quake 81),
      E4M2 42 (38), E4M3 45 (45). The cook's log prints a line a map.
  - The engine (engine/rlist.c `rl_model_stream`):
    - the streamed models' light (the monsters, their heads, the items'
      models; not the flat ones) is multiplied by 4/3 before its bound,
      which puts the lit side at Quake's twice;
    - the game's models and the gun keep the table's span, because the
      2D gun's pictures are cooked at it.
- **Checked**: E1M1's monsters' frames (`MONSTERS=1 MAP=E1M1`), before
  and after, side by side. The grunt and the dog in the dark corners,
  nearly black before, now stand out; the grunt near has Quake's lit
  side. The cycles are unchanged (the light is a value a body).

## §116 — The Options menu's Brightness (2026-10-31)

(The number is provisional, like §114's and §115's.)

- **The request**: a brightness slider in the options, for the darkest
  levels (E4M2).
- **Quake's**: the Options menu's Brightness (menu.c `M_Options_Draw`,
  `M_AdjustSliders`) moves `v_gamma` from 1 down to 0.5, a twentieth a
  step. `BuildGammaTable` (view.c) takes the palette through
  255 x ((i + 0.5) / 255.5) ^ gamma: it lifts the dark colours most and
  white not at all.
- **On the PlayStation** (engine/gamma.c): every texel is drawn through a
  CLUT, so the CLUTs take the same curve on their 5-bit channels. The
  eleven tables are computed on the host (each channel to 8 bits, through
  `BuildGammaTable`, back to 5 bits) and held as constants.
  - Which CLUTs:
    - the map's textures' (VRAM rows 481-502: the world, its brush
      models, the sky, the liquids, the far walls drawn flat);
    - the game's palette (row 504: the monsters, the items, the gun, the
      status bar's pictures);
    - the console's (row 480).
    - The menu's 4-bit pictures (rows 505-506), the 2D gun's CLUTs (its
      packets) and the particles' colours keep theirs.
  - Each upload of one of those goes through the current level's table
    (engine/world.c `vram_upload`, ui.c), a row at a time from a
    640-byte buffer. Black with its STP bit clear, the GPU's transparent
    texel, stays so.
  - A change sends the map's and the game's CLUTs again through the new
    level's table, from copies of them as cooked:
    - the map's are kept after its texture tables, 1.3-3.2 KB a map;
    - the game's palette is kept in a 512-byte array.
    - Reading the rows back from VRAM instead was tried on the host and
      dropped: a 5-bit table merges neighbours near white, and a colour
      drifted by up to 2 steps after a few moves of the slider.
  - Nothing changes in a frame's work. The release's level arena is
    1 352 416 -> 1 349 888 B (the code, the palette's copy, the row's
    buffer), less the map's copy while it plays.
- **The menu**: Brightness, Quake's row, between Reset to defaults and
  the volumes; its slider at (1 - `v_gamma`) / 0.5; `settings.brightness`
  0-10, 0 by default (Quake's 1); Reset to defaults puts it back.
- **Checked**:
  - on the host (engine/gamma.c with a VRAM in an array): ten moves of
    the slider, the CLUTs at each level exactly the table's image of the
    cooked colours, at 0 the cooked colours again, the transparent texels
    and the STP bits kept;
  - in the game bench (`BENCH_GAME=1`), its walk through the release menu
    now moves the slider 5 steps and back:
    - the CLUTs go to level 5, then back to 0;
    - the map's kept CLUTs hash as START's cooked ones do (FNV
      `dd8a42d1`);
    - the walk goes on as before.
  - The release builds with it.
  - The colours on screen are to be judged on DuckStation:
    PCSX-Redux runs without a GPU here.

## §113 — Two small CPU levers: the coplanar faces merged in the cook, lighter dynamic lights (2026-10-31)

The fourth lever after §106's profile (the vblank wait and the slow
path went to their own branches, §111 and §112; the section numbers are
provisional): two small ones, a slight loss allowed for a real gain, each
in its own commit and behind an option that can be turned off.

### 113.1 The coplanar faces merged in the cook (`QCOOK_MERGE`)

§79.3 measured it without building it: a node's faces with one texture,
side and cell, merged while their union stays convex, −2.5 to −5.7 % of
the polygons. qbsp cuts a surface every 240 texels for its lightmaps,
which the PS1 does not use; the cook then cuts each face on its own cells
(§39.1: a wrapped axis every 256 cooked texels from the face's own origin),
so a cell of a big floor held two to four pieces of different faces.

- **The cook** (`tools/qcook/world.c`, before the T-junctions' repair and
  the records):
  - a *cluster*: the faces of a node's side with one texture mapping
    (texinfo), marked by the same leaves (always drawn together: the
    records' own rule, §14.6) and connected by their edges; a brush
    model's face cut on its light grid (§96) stays out;
  - the cluster is cut on one cell grid: on a wrapped axis, of the origins
    on a repetition within a cell of its lowest face's, the one whose
    cells its faces' boxes reach fewest of (a face alone keeps its own
    origin, as before);
  - in each cell its pieces are merged while their union is convex (the
    hull's area within 0.25 square units of theirs), at most 32 vertices
    a piece: the hull's corners and every vertex of either on the hull's
    outline;
  - then every vertex lying on a straight run of each piece holding it
    (within 1/16 unit) is dropped from all of them: the inner edges' ends
    the merges left on the outlines, and the vertices qbsp put into the
    walls' edges for them. A vertex that any piece keeps as a corner stays
    in every piece holding it, so no edge loses a vertex its neighbour
    keeps: no new crack (the low geometry repairs no T-junction, §39.2;
    the others' repair runs after);
  - a merged piece's vertex takes its light from the lightmap of its face
    that holds it; the record draws its faces' pieces as one (their pairs
    with the record: the leaves' marks unchanged).
- **The light** (`QCOOK_MERGE_ERR`, 16 of 255 at the cook's gain by
  default; negative: no bound). Three checks against Quake's lightmap:
  a merge whose dropped points the merged piece's corners light worse by
  more than that (the fan from its corner 0, as the record draws it) is
  not made; a vertex whose run's ends would light it worse by more is kept
  (in every piece: the crack rule); and a cluster whose cut on the common
  grid lights one of its faces' luxels worse by more than that than the
  faces' own cuts did takes its own cuts back, unmerged (the grid moves
  the cuts, so the points where the light is sampled: without that check
  E4M7.v7's floor lost its pool of light, the luminance 19.9 → 17.8, its
  frame +13 %).
- **The zones' tables** (`tools/qcook/zones.c`, `scripts/cook.sh`): they
  name the records of a cook without the merge (§93's scan), so the merged
  worlds no longer matched them and the 17 zoned maps stayed whole (the
  frames +0.47 %, measured without the merge; the pools down to −74 KB:
  E4M7 442 → 368 KB). With the
  merge and a table, `cook.sh` also cooks the map without it, `qcook map
  --fmap` writes each world record's Quake faces for both, and `qcook
  zones --fmap` gives a zone every record holding a face of one of its
  records: the same 17 maps zoned, every chunk a superset by faces.
- **The options**: `QCOOK_MERGE=0` cooks as before (the 83 world, skeleton
  and chunk files byte for byte master's); `QCOOK_MERGE_ERR=<0..255>`. Both
  are in `cooked/settings.txt`'s line: a build re-cooks when they change.
- **Measured** (PCSX-Redux, master d7de758's cook against the branch's):
  - the cook, the 32 maps: the polygons 264 270 → 258 223 (−2.29 %), the
    records 141 054 → 139 650 (−1.0 %), the world files −179 KB;
  - the 128 views (`BENCH_MAPS=1 HASHONLY=1 PROFILE=0`, zoned): 40 228 823
    → 40 007 841 cycles (−0.55 %), 73 faster, 53 slower; the views past a
    vblank 16 → 15 (E3M3.v1 575 883 → 483 397, −16.1 %, two vblanks to
    one; E4M8.v2 −14.2 %; the worst E3M7.v2 +8.2 %, its pixels the same:
    its near polygons cut otherwise by the grid and the slow path); with
    the profile: the records' stage −1.35 %, the slow path −0.28 %, the
    polygons drawn −1.24 %, the vertices −1.35 %;
  - E2M7's fight of §106 (`MAP=E2M7 MONSTERS=3 SAMPLE=1 PROFILE=0
    SKILL=2 VIEW=1780,1803,-304,204,0`): the drawing 13 376 539 →
    13 215 610 cycles over its 12 windows (−1.20 %; each −1.9 to +0.15 %),
    the same streamed models drawn in each;
  - the stream's pool: +1 to +21 KB a map (E3M5 +21, E2M7 +14, the mean
    619.6 → 626.6 KB).
- **On the way** (the 128 views, with the profile, against the base):

  | variant | polygons (cook) | frames | records' stage | slow path |
  |---|---|---|---|---|
  | no light bound | −6.00 % | −0.24 % | −3.22 % | +1.32 % |
  | bound 32, no cluster check | −5.00 % | −0.29 % | −2.72 % | +0.96 % |
  | bound 16, no cluster check | −3.41 % | +0.05 % | −1.99 % | +1.20 % |
  | bound 32, the cluster check | −4.06 % | −0.56 % | −2.13 % | +0.13 % |
  | **bound 16, the cluster check** | **−2.29 %** | **−0.54 %** | **−1.35 %** | **−0.28 %** |

  A bigger piece near the eye goes whole to the slow path (its corner
  under the near clip, or the grid's levels past their cap) where the
  grid took the pieces it replaced: unbound, the slow path took back most
  of what the records saved. The merges without the drop give +2.1 %
  polygons (the merged pieces keep their straight runs and are cut into
  ears), the drop without the merges −0.05 %. Caps on a merge's growth
  (its union within 1.25 or 1.5 times its larger piece: −2.9 and −4.5 %
  polygons) or extent (128, 192 units: +1.4 and +0.3 %, the common grid
  adding pieces the merges no longer take back), and leaving the floors
  out (−4.5 %): their frames, measured while the zoned maps still loaded
  whole, were no better than the unbound merge's.
- **The pictures**: pktview's boards (before | after | difference x4) of
  START's, E1M1's, E3M3's, E3M7's, E4M7's and E2M7's 12 views each, in the
  side-session notes (`boards-113a/`): the largest mean change E2M7.v0's
  2.7 levels (the floor's light dithered otherwise), then E3M3.v1 1.95 and
  E4M7.v10 1.8; a texture moves by a texel where a near polygon is now cut
  otherwise (E1M1.v5's pillar).
- **The display lists** of every map change: a `scripts/dlcheck.sh`
  reference saved before must be saved again.
- **Left**: a look on DuckStation; the ultra geometry's light
  grid with the merge (the disc carries the low geometry alone: not
  cooked); the zones' tables are best made again on the merged cook by
  the scan (§93) the next time they are.

### 113.2 Lighter dynamic lights (`Q1PSX_DLIGHT_MINLIGHT`)

§106's profile gave the dynamic lights on the world 4.0 % of E2M7's
fight's samples. The sampler's 64-byte blocks spread a small function's
samples over its neighbours (`dl_restore_asm` kept 5 300 of them in a
build where it returned at once), so the cost was measured by turning
parts off (temporary builds, not kept; the fight of §106, its drawing
over the 12 windows, 13 215 468 cycles with §113.1):

| off | the fight's drawing |
|---|---|
| every dynamic light (none pushed, none on the models) | −10.38 % |
| the world's lights (the models keep theirs) | −9.11 % |
| the models' lights alone (`dl_add_at`) | −1.17 % |
| the world's but the walk's gate (the nodes stamped, never matched) | −6.15 % |
| the world's lighting (stamped, the records tested, none lit) | −4.73 % |

So on the world: the nodes' stamping about 3.0 %, the blocks' gate, the
records' tests and the Gouraud path the stamped blocks take about 1.4 %,
the lit records (the save, the planes' terms, the vertices, the restore)
about 4.7 %. Counted in the C reference: 1.54 lights live a frame (none
in 224 frames of 3 600, one in 1 791, two in 1 113, three or more in
472: rockets of 200 units, explosions of 350 decaying, the muzzle flash),
144 nodes visited a light for 31 records lit. A cap on the lights' count
would rarely act; the cost is each light's reach.

- **No loss** (`engine/r_world.s` dsn_*, its C reference `mark_nodes`):
  the stamping's descent bounds a light by its reach, radius − minlight,
  not its radius. A lit vertex lies within radius − minlight of the light
  (R_AddDynamicLights lights a point of a plane at |d| only within
  radius − |d| − minlight of the impact point), so a box or a plane
  farther holds none: the frames are the same (the game bench's 23
  `H` lines, the floor at 0). A light whose radius is not over its
  minlight is not pushed for the world. The muzzle flash (minlight 32)
  gains: the shotgun's, the nailguns' views −0.04 to −0.28 %. The
  models' light (`dl_add_at`) takes its root from the world's table
  (`dl_sqrt`, then exact: isqrt32's value, a few compares where the bit
  loop took its 16 steps).
- **The floor** (`Q1PSX_DLIGHT_MINLIGHT`, CMake, 32 by default; the
  bench's `DLIGHT_MINLIGHT=`): every light on the world's faces takes at
  least that minlight, Quake's own way to end a light's faint edge, the
  value it gives its muzzle flash (32). Quake adds `rad − dist` to a
  luxel down to `rad − dist = minlight` (R_AddDynamicLights), so a
  light's add stays Quake's wherever it reaches; its last 32 units of
  light (13 of the 255 vertex light levels at the cook's gain) are not
  drawn, and its reach is 32 units shorter (an explosion's 350 → 318, a
  rocket's 200 → 168): fewer nodes stamped, records tested and vertices
  lit. The models keep Quake's light (`dl_add_at`: no minlight in
  R_DrawAliasModel). 0 is each light's own minlight, Quake's look.
- **Measured**:

  | floor | the fight's drawing | the rocket launcher's view | ghud1 / ghud2 |
  |---|---|---|---|
  | 0 (no loss) | −0.37 % | −0.07 % | −0.14 / −0.19 % |
  | **32 (the default)** | **−1.73 %** | **−4.05 %** | **−3.43 / −3.43 %** |
  | 48 | −2.49 % | −8.54 % | −5.65 / −5.63 % |
  | 64 | −3.24 % | −10.54 % | −8.52 / −8.49 % |

  (the fight against §113.1's, the same models drawn in every window;
  the game bench's views, `BENCH_GAME=1 HASHONLY=1 PROFILE=0`, against
  §113.1's: 1 282 794 cycles for the rocket launcher's, still three
  vblanks.) The C reference (`DLIGHT_C=1`) and the asm checked by it
  (`DLIGHT_C=2`, no `dlchk` line) draw the asm's frames; under the
  interpreter the game bench's frames are the dynarec's but the last
  menu page's (`mgraphics_fx`), which differs there on §113.1 as well.
- **The pictures** (pktview's boards, before | after | difference x4, in
  the side-session notes, `boards-113b/`): eight frames of the fight with
  an explosion within 250 units of the player (a dump the bench took for
  this measure, not kept) and the game bench's rocket views, each at 32
  and 64 against 0. At 32 the frames differ by 0.04 to 0.35 levels on
  average (the glow's outer ring); at 64 by 0.26 to 1.83 (the farthest
  walls an explosion lights stay darker: E2M7's arch beside the player,
  the luminance 26.8 → 25.0).
- **Left**: an eye on DuckStation (an explosion against a near
  wall, the rockets in a dark corridor, the muzzle flash) and the floor's
  value (48 and 64 measured, not chosen). Not done: the stamping cut by
  the view's frustum (the walk draws no block outside it; how many of a
  light's 144 nodes lie outside was not measured; the descent would
  carry the frustum's planes as the walk does: a larger asm change), and
  the lit records' save and restore (their share of the 4.7 % not
  measured apart).

### 113.3 Both, on master 5cf05f5

Master moved during the work (§111, §114, §115, §116); merged into the
branch, `QCOOK_MERGE=0` still cooks the 32 worlds byte for byte as
master's own qcook does. Both levers against both off (`QCOOK_MERGE=0
DLIGHT_MINLIGHT=0`, master's frames), one run each: E2M7's fight's
drawing 12 858 120 → 12 511 011 cycles (−2.70 %; each window −3.63 to
−1.12 %, the same models drawn), the 128 views 40 293 952 → 40 076 813
(−0.54 %; 78 faster, 48 slower; 16 → 15 views past a vblank), the same
17 maps zoned.
