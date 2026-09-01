# External dependencies

Nothing is versioned here (see `.gitignore`): this folder only holds install
instructions. The project is developed *from scratch* — the only dependencies
are the PS1 SDK and the emulators.

## PSn00bSDK v0.24 (PS1 SDK + MIPS toolchain)

- **Source**: <https://github.com/Lameguy64/PSn00bSDK/releases/tag/v0.24>
- **File**: `PSn00bSDK-0.24-Linux.zip`
  (sha256 `5ada7c9478d795b22bde96ee23d6869290c14a740bcde3d57c01c342a26ded35`)
- The Linux release **already includes** GCC 12.3.0 `mipsel-none-elf`, binutils,
  `mkpsxiso` 2.03, `elf2x`, `dumpsxiso`, `lzpack`, `ninja` and `libpsn00b`
  (headers + libs + template). The separate `gcc-mipsel-none-elf-*.zip` is
  **not** needed with this release.
- Licenses (see PSn00bSDK's `LICENSE.md`): SDK core (libs, CMake scripts, some
  tools) **MPL 2.0**; examples + template MPL 2.0 with permission to
  copy/modify without sharing; `mkpsxiso` / `dumpsxiso` **GPLv2 or later**;
  bundled GCC toolchain **GPLv3** (sources unpatched, available from GNU).

### Install

Extracted into a directory of one's choice (no sudo), `$PSN00BSDK` below.
Pinned version: **v0.24**.

```bash
unzip PSn00bSDK-0.24-Linux.zip
mv PSn00bSDK-0.24-Linux "$PSN00BSDK"
```

### Shell exposure

**fish** — a drop-in in fish's `conf.d/` (`psn00bsdk.fish`):

```fish
if test -d $PSN00BSDK/bin
    fish_add_path -g $PSN00BSDK/bin
    set -gx PSN00BSDK_LIBS $PSN00BSDK/lib/libpsn00b
end
```

**bash/zsh** — in `~/.bashrc` / `~/.zshrc`:

```bash
export PATH="$PSN00BSDK/bin:$PATH"
export PSN00BSDK_LIBS="$PSN00BSDK/lib/libpsn00b"
```

### Verification

```
mipsel-none-elf-gcc -dumpmachine   # mipsel-none-elf
mipsel-none-elf-gcc --version      # gcc (GCC) 12.3.0
mkpsxiso                           # MKPSXISO 2.03 ...
```

The CMake toolchain file is `$PSN00BSDK_LIBS/cmake/sdk.cmake` (referenced
automatically by the `default` preset).

### CMake 4.x note

CMake 4.4 configures and builds the v0.24 template without issue. If an internal
script's `cmake_minimum_required(VERSION <3.5)` ever causes a problem, pass
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.

## Emulators

Installed as AppImages:

| Emulator | File | BIOS | Use |
|---|---|---|---|
| PCSX-Redux | `PCSX-Redux.AppImage` | bundled OpenBIOS (no BIOS required) | validation, low-level debugging |
| DuckStation | `DuckStation-x64.AppImage` | **PS1 BIOS required** (in its data directory's `bios/`) | fast iteration once a BIOS is provided |

Headless PCSX-Redux launch (auto screenshot): see `scripts/` *(harness to be
committed — Phase 2.1)*.

## `reference/` (outside the repo)

**Read-only** clones for consultation, never built or modified:

| Repo | Role |
|---|---|
| `id-Software/Quake` | architectural reference engine |
| `id-Software/quake-rerelease-qc` | 2021 QuakeC (optional Phase 12) |
| `Lameguy64/PSn00bSDK` | docs + examples (the SDK we build against is the release, not this clone) |

No existing PS1 source port is cloned or consulted.
