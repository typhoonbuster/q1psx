#!/usr/bin/env bash
#
# Build the benchmark variant (CMake preset "bench" -> build-bench/) and run it
# headless; print the "bench ..." lines.
#
# Usage:  [FID=0..3] [T=px] [TEX4=0|1] [VIEWS=0|1] [BENCH_MAPS=0|1] [BENCH_GAME=0|1] [PROFILE=0|1]
#         scripts/bench.sh [max-seconds]
#         FID picks the fidelity level the bench loads (default 2, high).
#         T is the affine-warp threshold, fixed (default 8 px, the game's).
#         TEX4=1 loads the 4-bit textures.
#         VIEWS=0 skips the render views: physics checks only (fast).
#         BENCH_MAPS=1 loads every map on the disc and benches its four spawn
#         views instead (each map falls back to a lower level if too big).
#         PROFILE=0 builds the renderer without its counters / stage timers,
#         as the "release" preset does: frame totals only (the stage and
#         counter columns read 0).
#         HASHONLY=1 dumps only the frames' hashes (`H` lines): no packets,
#         no pictures, a run several times shorter (the console is slow).
#         PLAY=1 (with BENCH_MAPS=1) runs the loading screen and 30 interactive
#         frames per map; MUSIC=1 puts the music on the bench disc (the .cue
#         then carries the tracks: docs/porting-notes.md §26).
#         DLIGHT_C=1 builds the dynamic lights' C reference instead of the
#         asm (the hashes must match); DLIGHT_C=2 the asm checked record
#         by record by the C ("dlchk" lines).
#
# Each line reads:
#   bench <scenario> n=<frames> vbl=<vblanks/frame x100>
#         <stage>=<avg cycles>/<max cycles> ... <counter>=<avg per frame> ...
#
# Numbers are PCSX-Redux CPU timings: good for comparing variants, not a
# cycle-exact hardware measurement (see docs/architecture.md §9).

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BENCH_GAME=${BENCH_GAME:-0}
# PCSX-Redux (builds since 2026-10) runs headless at about real-time speed and
# writes the console at ~5 KB/s: the game scenario's 15 dumps need ~6 minutes
SECS=${1:-$([ "$BENCH_GAME" = 1 ] && echo 420 || echo 60)}
FID=${FID:-2}
T=${T:-8}
TEX4=${TEX4:-0}
VIEWS=${VIEWS:-1}
BENCH_MAPS=${BENCH_MAPS:-0}
PROFILE=${PROFILE:-1}
HASHONLY=${HASHONLY:-0}
DLIGHT_C=${DLIGHT_C:-0}
PLAY=${PLAY:-0}
MUSIC=${MUSIC:-0}
FIDNAME=$(echo low medium high ultra | cut -d' ' -f$((FID + 1)))

cmake --preset bench -DQ1PSX_BENCH_FID="$FID" -DQ1PSX_BENCH_VIEWS="$VIEWS" \
      -DQ1PSX_BENCH_MAPS="$BENCH_MAPS" -DQ1PSX_BENCH_GAME="$BENCH_GAME" -DQ1PSX_PROFILE="$PROFILE" \
      -DQ1PSX_BENCH_T="$T" -DQ1PSX_BENCH_TEX4="$TEX4" -DQ1PSX_BENCH_HASHONLY="$HASHONLY" \
      -DQ1PSX_DLIGHT_C="$DLIGHT_C" -DQ1PSX_BENCH_PLAY="$PLAY" -DQ1PSX_BENCH_MUSIC="$MUSIC" "$ROOT" >/dev/null
cmake --build "$ROOT/build-bench" > "$ROOT/build-bench/build.log" 2>&1 \
	|| { echo "bench: build failed (see build-bench/build.log)"; tail -n 20 "$ROOT/build-bench/build.log"; exit 1; }

LOG="$ROOT/build-bench/bench.log" \
	"$ROOT/scripts/headless.sh" "$ROOT/build-bench/q1psx.bin" "$SECS" '^bench done' >/dev/null \
	|| { status=$?; tail -n 30 "$ROOT/build-bench/bench.log"; exit $status; }

grep '^bench ' "$ROOT/build-bench/bench.log" | grep -v '^bench \(start\|done\)' || true
grep '^\(clipdiff\|pmdiff\|pmove\)' "$ROOT/build-bench/bench.log" | grep -v MISMATCH || true
grep -A2 'MISMATCH' "$ROOT/build-bench/bench.log" | head -n 30 || true
[ "$VIEWS" = 0 ] || [ "$BENCH_MAPS" = 1 ] || [ "$HASHONLY" = 1 ] && exit 0

# Render the dumped display lists to PNG (build-bench/views/<name>.png) so the
# output can be checked without a screen (the game's dumps need GAME.DAT's
# images too).
make -s -C "$ROOT/tools/pktview"
mkdir -p "$ROOT/build-bench/views"
TEXEXT=tex; [ "$TEX4" = 1 ] && TEXEXT=tx4
"$ROOT/tools/pktview/pktview" "$ROOT/build-bench/bench.log" \
	"$ROOT/cooked/id1/start_$FIDNAME.$TEXEXT" "$ROOT/build-bench/views/" \
	"$ROOT/cooked/id1/game.dat" | sed 's/^/  /'
