#!/usr/bin/env bash
#
# Build the benchmark variant (CMake preset "bench" -> build-bench/) and run it
# headless; print the "bench ..." lines.
#
# Usage:  scripts/bench.sh [max-seconds]
#
# Each line reads:
#   bench <scenario> n=<frames> vbl=<vblanks/frame x100>
#         <stage>=<avg cycles>/<max cycles> ... <counter>=<avg per frame> ...
#
# Numbers are PCSX-Redux CPU timings: good for comparing variants, not a
# cycle-exact hardware measurement (see docs/architecture.md §9).

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SECS=${1:-60}

cmake --preset bench "$ROOT" >/dev/null
cmake --build "$ROOT/build-bench" >/dev/null

LOG="$ROOT/build-bench/bench.log" \
	"$ROOT/scripts/headless.sh" "$ROOT/build-bench/q1psx.bin" "$SECS" '^bench done' >/dev/null \
	|| { status=$?; tail -n 30 "$ROOT/build-bench/bench.log"; exit $status; }

grep '^bench ' "$ROOT/build-bench/bench.log" | grep -v '^bench \(start\|done\)'
