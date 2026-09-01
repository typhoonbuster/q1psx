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

### Next step

- **3.4** raw `progs.dat` load (no execution) + `edict_t`/`entvars_t` structs
  (3.1d). Then Phase 4 — offline asset cooking.
