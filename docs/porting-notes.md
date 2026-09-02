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

| level | max texture | lightmap | geometry | intent |
|---|---|---|---|---|
| low | 64 | none | heavy merge | ep1-4 + packs all fit |
| medium | 64 | ¼-res | light decimation | ep1-4 fit, lit |
| high (default) | 128 | ½-res | full BSP | ep1 target |
| ultra | ≤256 (native) | full-res | full BSP | ep1 showcase |

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

MDL / SPR / lightmap cooking comes later (Phase 8+).

---

## §8 — GTE render pipeline (Phase 5)

### 8.1 View + wireframe (5.1)

- `r_begin_view(pos, ang)` builds one `MATRIX`: rows are the camera basis from
  `AngleVectors` (world = Quake coords, X fwd / Y left / Z up). Screen Y points
  down, so row 1 is `-up`. Translation `t = -M · pos` (world units, 20.12 → >>
  12). `gte_SetRotMatrix` + `gte_SetTransMatrix`, once per frame.
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
