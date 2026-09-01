#!/usr/bin/env bash
#
# Build (if needed) and launch a q1psx CD image in an emulator.
#
# Usage:  scripts/run-emu.sh [image.bin]
#
# Env:
#   EMU   emulator command / AppImage path.
#         Unset: local.env's EMU (the PCSX-Redux AppImage).
#         PCSX-Redux ships OpenBIOS, so it needs no PS1 BIOS file.
#         DuckStation needs a BIOS: EMU="…/DuckStation-x64.AppImage -batch"
#
# Note: there is currently no reliable headless screenshot path on this setup
# (PCSX-Redux -no-ui has no GPU backend; its web API returns empty VRAM). Visual
# checks — "no tearing/flicker", "does it look right" — are done by eye here.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMG=${1:-"$ROOT/build/q1psx.bin"}
EMU=${EMU:-$(sed -n 's/^[[:space:]]*EMU[[:space:]]*=[[:space:]]*//p' "$ROOT/local.env" 2>/dev/null | tail -n 1 || true)}
: "${EMU:?set EMU (the PCSX-Redux AppImage) in local.env or the environment}"

if [ ! -f "$IMG" ]; then
    echo "building $IMG …"
    cmake --preset default "$ROOT" >/dev/null
    cmake --build "$ROOT/build" >/dev/null
fi

case "$EMU" in
    *PCSX-Redux*) exec $EMU -loadiso "$IMG" -run ;;
    *)            exec $EMU "$IMG" ;;
esac
