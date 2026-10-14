#!/usr/bin/env bash
#
# Build the benchmark variant (CMake preset "bench" -> build-bench/) and run it
# headless; print the "bench ..." lines.
#
# Usage:  [FID=0..3] [VIEWS=0|1] scripts/bench.sh [max-seconds]
#         FID picks the fidelity level the bench loads (default 2, high).
#         VIEWS=0 skips the render views: physics checks only (fast).
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
FID=${FID:-2}
VIEWS=${VIEWS:-1}
FIDNAME=$(echo low medium high ultra | cut -d' ' -f$((FID + 1)))

cmake --preset bench -DQ1PSX_BENCH_FID="$FID" -DQ1PSX_BENCH_VIEWS="$VIEWS" "$ROOT" >/dev/null
cmake --build "$ROOT/build-bench" >/dev/null

LOG="$ROOT/build-bench/bench.log" \
	"$ROOT/scripts/headless.sh" "$ROOT/build-bench/q1psx.bin" "$SECS" '^bench done' >/dev/null \
	|| { status=$?; tail -n 30 "$ROOT/build-bench/bench.log"; exit $status; }

grep '^bench ' "$ROOT/build-bench/bench.log" | grep -v '^bench \(start\|done\)' || true
grep -A2 '^\(clipdiff\|pmdiff\|pmove\)' "$ROOT/build-bench/bench.log" || true
[ "$VIEWS" = 0 ] && exit 0

# Render the dumped display lists to PNG (build-bench/views/view<N>.png) so the
# output can be checked without a screen.
make -s -C "$ROOT/tools/pktview"
mkdir -p "$ROOT/build-bench/views"
"$ROOT/tools/pktview/pktview" "$ROOT/build-bench/bench.log" \
	"$ROOT/cooked/id1/start_$FIDNAME.tex" "$ROOT/build-bench/views/view" | sed 's/^/  /'
