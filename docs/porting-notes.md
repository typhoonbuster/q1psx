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

## §5 — Detailed porting conventions (to be completed)

_Cooked asset formats, GTE render pipeline: sections to fill in over phases
4-5._
