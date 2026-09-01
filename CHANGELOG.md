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

### Next step

- **2.1** confirm on screen (no tearing/flicker), then **2.2** controller read.
