#!/usr/bin/env bash
#
# Build the benchmark variant (CMake preset "bench" -> build-bench/) and run it
# headless; print the "bench ..." lines.
#
# Usage:  [TEXTURES=0..3] [GEOMETRY=0|2|3] [MODELS=0..2] [EFFECTS=0..3] [T=px] [TEX4=0|1]
#         [VIEWS=0|1] [BENCH_MAPS=0|1] [BENCH_GAME=0|1] [PROFILE=0|1]
#         scripts/bench.sh [max-seconds]
#         TEXTURES, GEOMETRY, MODELS, EFFECTS pick the quality levels the
#         bench loads and runs at (formats/qlevels.h: 0 low, 1 medium, 2 high,
#         3 ultra; default 2, high, each).
#         T is the affine-warp threshold, fixed (default 8 px, the game's).
#         TEX4=1 loads the 4-bit textures.
#         DRAWDIST=<world units> renders with that draw distance (the fog,
#         the culling: docs/porting-notes.md §37); FLAT=1 (far) or 2 (all)
#         with the flat shading. VIEWMODEL=0 / 2: no gun / the 2D gun (1, the
#         3D model, by default).
#         VIEWS=0 skips the render views: physics checks only (fast).
#         BENCH_MAPS=1 loads every map on the disc and benches its four spawn
#         views instead (each map falls back to a lower level if too big).
#         PROFILE=0 builds the renderer without its counters / stage timers,
#         as the "release" preset does: frame totals only (the stage and
#         counter columns read 0).
#         HASHONLY=1 dumps only the frames' hashes (`H` lines): no packets,
#         no pictures, a run several times shorter (the console is slow).
#         MONSTERS=1 runs the monsters' scenarios (MAP=, default E1M1: a
#         monster of each kind there woken, fighting, killed; "mon" lines,
#         dumps m<map>_<kind>_<step>; then the crowd: every monster of the
#         map awake, the entities' cost a second); MONSTERS=2 the crowd
#         alone (SAMPLE=1: its PC samples, "samples crowd").
#         PLAY=1 (with BENCH_MAPS=1) runs the loading screen and 30 interactive
#         frames per map; MAP=E1M1 restricts the maps bench to that map (its
#         views dumped: the brush models in them; VIEW=x,y,z,yaw,pitch adds
#         a view there, NOENTS=1 leaves the entities out, an A / B); MUSIC=1 puts the music on the bench disc (the .cue
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
TEXTURES=${TEXTURES:-2}
GEOMETRY=${GEOMETRY:-2}
MODELS=${MODELS:-2}
EFFECTS=${EFFECTS:-2}
T=${T:-8}
TEX4=${TEX4:-0}
DRAWDIST=${DRAWDIST:-0}
FLAT=${FLAT:-0}
VIEWMODEL=${VIEWMODEL:-1}
VIEWS=${VIEWS:-1}
BENCH_MAPS=${BENCH_MAPS:-0}
PROFILE=${PROFILE:-1}
HASHONLY=${HASHONLY:-0}
DLIGHT_C=${DLIGHT_C:-0}
PLAY=${PLAY:-0}
MUSIC=${MUSIC:-0}
MAP=${MAP:-}
VIEW=${VIEW:-}
NOENTS=${NOENTS:-0}
ENTS=${ENTS:-0}
MONSTERS=${MONSTERS:-0}
SAMPLE=${SAMPLE:-0}
cmake --preset bench -DQ1PSX_BENCH_TEXTURES="$TEXTURES" -DQ1PSX_BENCH_GEOMETRY="$GEOMETRY" \
      -DQ1PSX_BENCH_MODELS="$MODELS" -DQ1PSX_BENCH_EFFECTS="$EFFECTS" -DQ1PSX_BENCH_VIEWS="$VIEWS" \
      -DQ1PSX_BENCH_MAPS="$BENCH_MAPS" -DQ1PSX_BENCH_GAME="$BENCH_GAME" -DQ1PSX_PROFILE="$PROFILE" \
      -DQ1PSX_BENCH_T="$T" -DQ1PSX_BENCH_TEX4="$TEX4" -DQ1PSX_BENCH_HASHONLY="$HASHONLY" \
      -DQ1PSX_BENCH_DRAWDIST="$DRAWDIST" -DQ1PSX_BENCH_FLAT="$FLAT" -DQ1PSX_BENCH_VIEWMODEL="$VIEWMODEL" \
      -DQ1PSX_DLIGHT_C="$DLIGHT_C" -DQ1PSX_BENCH_PLAY="$PLAY" -DQ1PSX_BENCH_MUSIC="$MUSIC" \
      -DQ1PSX_BENCH_MAP="$MAP" -DQ1PSX_BENCH_VIEW="$VIEW" -DQ1PSX_BENCH_NOENTS="$NOENTS" \
      -DQ1PSX_BENCH_ENTS="$ENTS" -DQ1PSX_BENCH_MONSTERS="$MONSTERS" -DQ1PSX_BENCH_SAMPLE="$SAMPLE" "$ROOT" >/dev/null
cmake --build "$ROOT/build-bench" > "$ROOT/build-bench/build.log" 2>&1 \
	|| { echo "bench: build failed (see build-bench/build.log)"; tail -n 20 "$ROOT/build-bench/build.log"; exit 1; }

LOG="$ROOT/build-bench/bench.log" \
	"$ROOT/scripts/headless.sh" "$ROOT/build-bench/q1psx.bin" "$SECS" '^bench done' >/dev/null \
	|| { status=$?; tail -n 30 "$ROOT/build-bench/bench.log"; exit $status; }

grep '^bench ' "$ROOT/build-bench/bench.log" | grep -v '^bench \(start\|done\)' || true
grep '^\(clipdiff\|pmdiff\|pmove\)' "$ROOT/build-bench/bench.log" | grep -v MISMATCH || true
grep -A2 'MISMATCH' "$ROOT/build-bench/bench.log" | head -n 30 || true
[ "$MONSTERS" != 0 ] && grep '^\(mon\|stream\)' "$ROOT/build-bench/bench.log" | head -n 400
[ "$MONSTERS" = 2 ] && exit 0
[ "$VIEWS" = 0 ] || { [ "$BENCH_MAPS" = 1 ] && [ -z "$MAP" ]; } || [ "$HASHONLY" = 1 ] && exit 0
[ "$ENTS" = 1 ] && MAP=E1M1          # (its dumps: e1m1's textures, the first map)
[ "$MONSTERS" != 0 ] && MAP=${MAP:-E1M1}

# Render the dumped display lists to PNG (build-bench/views/<name>.png) so the
# output can be checked without a screen (the game's dumps need GAME.DAT's
# images too: the models level's file); a single map's bench (MAP=...) with
# that map's textures.
make -s -C "$ROOT/tools/pktview"
mkdir -p "$ROOT/build-bench/views"
TEXEXT=tex; [ "$TEX4" = 1 ] && TEXEXT=tx4
TEXMAP=start; [ -n "$MAP" ] && TEXMAP=$(echo "$MAP" | tr 'A-Z' 'a-z')
"$ROOT/tools/pktview/pktview" "$ROOT/build-bench/bench.log" \
	"$ROOT/cooked/id1/${TEXMAP}_t$TEXTURES.$TEXEXT" "$ROOT/build-bench/views/" \
	"$ROOT/cooked/id1/game_$MODELS.dat" "$ROOT/cooked/id1/stream.dat" | sed 's/^/  /'
