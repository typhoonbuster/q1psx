#!/usr/bin/env bash
#
# Build the benchmark variant (CMake preset "bench" -> build-bench/) and run it
# headless; print the "bench ..." lines.
#
# Usage:  [TEXTURES=high|medium] [EFFECTS=1|2] [T=px] [WEAPON=3d|2d]
#         [VIEWS=0|1] [BENCH_MAPS=0|1] [BENCH_GAME=0|1] [PROFILE=0|1]
#         scripts/bench.sh [max-seconds]
#         TEXTURES is the build's texture level (the disc's one set, 4-bit;
#         default high); the geometry and the models are low on every disc;
#         EFFECTS the effects' level (1 medium, the default, 2 high;
#         formats/qlevels.h; docs/porting-notes.md §52).
#         T is the affine-warp threshold, fixed (default 4 px: the game's
#         dynamic control's floor).
#         DRAWDIST=<world units> builds with that draw distance (the fog,
#         the culling: docs/porting-notes.md §37; default 1536, the
#         release's; 0: none); FLAT=0 (none), 1 (far, the default: the
#         game's) or 2 (all) the flat shading. WEAPON=2d builds the 2D gun
#         (3d by default).
#         NOBMF=1 draws the brush models whole in one leaf, not cut by the
#         world's BSP (engine/bmfrag.c): an A / B. NOBMFCULL=1 cuts a moved
#         brush model again even when none of it is in view (an A / B: the
#         same frames). BMFCHK=1 (with
#         BENCH_MAPS=1 VIEWS=0) cuts every door, button, plat, train and
#         secret door at 33 places along each of its ways in the room the
#         cook measured ("bmfchk" lines: the cuts that do not fit, the
#         fullest rooms; "bmf room" lines: a room that is not the cut
#         where its model stands, every bench checks it at the load;
#         docs/porting-notes.md §44, §46).
#         MOVERS=1 moves the big movers in view (E1M6's train *39, E1M3's
#         biggest train, E3M4's biggest door; MAP= one of them, a frame
#         dumped): "bench mv.*" and "bmfcut" lines, the frame's, the ents'
#         and the re-cut's cycles; "bmfhash": every 16th frame's hash folded
#         (the identical-display-list check).
#         STACK_MIN=<bytes>: the link's least stack room (default 16384;
#         4096 with BENCH_GAME=1, whose scenarios' code leaves the stack
#         about 6.8 KB: its "stack: N bytes never reached" line shows the
#         room left; 10240 with BMF_C=2).
#         VIEWS=0 skips the render views: physics checks only (fast).
#         BENCH_MAPS=1 loads every map on the disc and benches its four spawn
#         views instead (each map falls back to a lower level if too big).
#         PROFILE=0 builds the renderer without its counters / stage timers,
#         as the "release" preset does: frame totals only (the stage and
#         counter columns read 0).
#         HASHONLY=1 dumps only the frames' hashes (`H` lines): no packets,
#         no pictures, a run several times shorter (the console is slow).
#         BENCH_GAME=1 ends with a "bench frame cache" line: the streamed
#         models' frames expanded and refused over its frames.
#         Every dump's `Q` line: its packets as a set (their hashes summed,
#         the texture-window commands left out, and their count): a change
#         of order alone keeps it (docs/porting-notes.md §63).
#         MONSTERS=1 runs the monsters' scenarios (MAP=, default E1M1: a
#         monster of each kind there woken, fighting, killed; "mon" lines,
#         dumps m<map>_<kind>_<step>, each with its frame's cycles and the
#         frame cache's refusals (engine/mdl.c: a streamed model whose
#         frame it could not expand, not drawn; docs/porting-notes.md
#         §68); then the crowd: every monster of the map awake, the
#         entities' cost a second); MONSTERS=2 the crowd alone (SAMPLE=1:
#         its PC samples, "samples crowd"); MONSTERS=3 the crowd, then a
#         minute of fight drawn (the player fires rockets turning round):
#         "mon fight" lines (the stack's high-water mark, the edicts, the
#         frames' cycles, the streamed models drawn and the frame cache's
#         refusals); MONSTERS=4 the map cleared
#         (MAP=, default E1M3: each monster in turn, the player on a spot
#         in its sight, rockets, frames drawn): "clear" lines; MONSTERS=5
#         from MAP (E1M1) each map cleared, then its exit taken to the next
#         map as a player's, to the episode's end.
#         PLAY=1 (with BENCH_MAPS=1) runs the loading screen and 30 interactive
#         frames per map; MAP=E1M1 restricts the maps bench to that map (its
#         views dumped: the brush models in them; VIEW=x,y,z,yaw,pitch adds
#         a view there, more groups of five more views, NOENTS=1 leaves the entities out, an A / B;
#         TICKS=n with a VIEW: the player put there, the game run n ticks
#         from there, the view dumped every 8 ticks: a mover, a monster
#         as the player standing there sees them); RESIDENCY=1 (with
#         BENCH_MAPS=1 VIEWS=0) reports what the stream loads around the
#         player at the map's items ("res" lines: a simulation charging a
#         model what it keeps once its skin is in VRAM, then the stream's
#         own reads there; docs/porting-notes.md §67); MUSIC=1 puts the
#         music on the bench disc (the SPU's streams; docs/porting-notes.md
#         §48).
#         DLIGHT_C=1 builds the dynamic lights' C reference instead of the
#         asm (the hashes must match); DLIGHT_C=2 the asm checked record
#         by record by the C ("dlchk" lines). BMF_C=1 builds the brush
#         models' cut in C (engine/bmfrag.c) instead of the asm
#         (engine/bmfrag.s), BMF_C=2 the asm checked cut by cut by the C
#         ("bmfasm" lines: the cuts checked, any mismatch); that build
#         carries both cuts, its stack floor lowered to 10 KB (STACK_MIN;
#         SAMPLE=1's histogram needs 2048 there). ORDER_C=1 builds an alias
#         model's or a particle's place among its leaf's brush-model pieces
#         in C (engine/rlist.c rl_order_c) instead of the asm (engine/
#         rl_order.s), ORDER_C=2 the asm checked ask by ask by the C, and
#         in each debug frame (a view's dump) at made-up places around
#         every model with pieces ("ordasm" lines: the asks checked, any
#         mismatch). Every bench prints an "order" line per view whose
#         debug frame took such places: their count, their cycles.
#         ZONES=1 walks every zoned map (MAP=: one) through its items at
#         running speed, the chunks read as in play ("bench zones" lines:
#         the switches and the patch's cycles, the reads, the late frames;
#         docs/porting-notes.md §57). WHOLE=1 puts the whole worlds on the
#         disc, the zones' A / B (CMake Q1PSX_ZONES OFF). Every bench prints
#         a "bench zone <map>" line per map loaded: the skeleton, the
#         chunk arena, the stream's pool.
#         ENGINE_C=1 builds the C engine (CMake Q1PSX_ENGINE_C: every asm
#         part's C version but the world renderer's, docs/porting-notes.md
#         §55) in build-bench-c/, beside the asm's: its frames must be the
#         asm's, hash for hash (scripts/dlcheck.sh: save a reference with
#         the asm, check it with ENGINE_C=1: every view's cycles, asm to C).
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
TEXTURES=${TEXTURES:-high}
EFFECTS=${EFFECTS:-1}
T=${T:-4}
DRAWDIST=${DRAWDIST:-1536}
FLAT=${FLAT:-1}
WEAPON=${WEAPON:-3d}
NOBMF=${NOBMF:-0}
NOBMFCULL=${NOBMFCULL:-0}
BMFCHK=${BMFCHK:-0}
STACK_MIN=${STACK_MIN:-$([ "$BENCH_GAME" = 1 ] && echo 4096 || { [ "${BMF_C:-0}" = 2 ] && echo 10240 || echo 16384; })}
VIEWS=${VIEWS:-1}
BENCH_MAPS=${BENCH_MAPS:-0}
PROFILE=${PROFILE:-1}
HASHONLY=${HASHONLY:-0}
DLIGHT_C=${DLIGHT_C:-0}
BMF_C=${BMF_C:-0}
ORDER_C=${ORDER_C:-0}
ENGINE_C=${ENGINE_C:-0}
OUT="$ROOT/build-bench$([ "$ENGINE_C" = 1 ] && echo -c || true)"   # (the C engine's beside the asm's)
PLAY=${PLAY:-0}
MUSIC=${MUSIC:-0}
MAP=${MAP:-}
VIEW=${VIEW:-}
NOENTS=${NOENTS:-0}
TICKS=${TICKS:-0}
RESIDENCY=${RESIDENCY:-0}
ENTS=${ENTS:-0}
MONSTERS=${MONSTERS:-0}
MOVERS=${MOVERS:-0}
SAMPLE=${SAMPLE:-0}
ZONES=${ZONES:-0}
WHOLE=${WHOLE:-0}
cmake --preset bench -B "$OUT" -DQ1PSX_ENGINE_C="$([ "$ENGINE_C" = 1 ] && echo ON || echo OFF)" -DQ1PSX_TEXTURES="$TEXTURES" -DQ1PSX_WEAPON="$WEAPON" -DQ1PSX_DRAWDIST="$DRAWDIST" \
      -DQ1PSX_BENCH_EFFECTS="$EFFECTS" -DQ1PSX_BENCH_VIEWS="$VIEWS" \
      -DQ1PSX_BENCH_MAPS="$BENCH_MAPS" -DQ1PSX_BENCH_GAME="$BENCH_GAME" -DQ1PSX_PROFILE="$PROFILE" \
      -DQ1PSX_BENCH_T="$T" -DQ1PSX_BENCH_HASHONLY="$HASHONLY" \
      -DQ1PSX_BENCH_FLAT="$FLAT" -DQ1PSX_BENCH_BMF="$((1 - NOBMF))" \
      -DQ1PSX_BENCH_BMFCULL="$((1 - NOBMFCULL))" \
      -DQ1PSX_BENCH_BMFCHK="$BMFCHK" -DQ1PSX_STACK_MIN="$STACK_MIN" \
      -DQ1PSX_DLIGHT_C="$DLIGHT_C" -DQ1PSX_BMF_C="$BMF_C" -DQ1PSX_ORDER_C="$ORDER_C" -DQ1PSX_BENCH_PLAY="$PLAY" -DQ1PSX_BENCH_MUSIC="$MUSIC" \
      -DQ1PSX_BENCH_MAP="$MAP" -DQ1PSX_BENCH_VIEW="$VIEW" -DQ1PSX_BENCH_NOENTS="$NOENTS" -DQ1PSX_BENCH_TICKS="$TICKS" -DQ1PSX_BENCH_RESIDENCY="$RESIDENCY" \
      -DQ1PSX_BENCH_ENTS="$ENTS" -DQ1PSX_BENCH_MONSTERS="$MONSTERS" -DQ1PSX_BENCH_SAMPLE="$SAMPLE" \
      -DQ1PSX_BENCH_MOVERS="$MOVERS" -DQ1PSX_BENCH_ZONES="$ZONES" -DQ1PSX_ZONES="$([ "$WHOLE" = 1 ] && echo OFF || echo ON)" \
      "$ROOT" >/dev/null
cmake --build "$OUT" > "$OUT/build.log" 2>&1 \
	|| { echo "bench: build failed (see $OUT/build.log)"; tail -n 20 "$OUT/build.log"; exit 1; }

LOG="$OUT/bench.log" \
	"$ROOT/scripts/headless.sh" "$OUT/q1psx.bin" "$SECS" '^bench done' >/dev/null \
	|| { status=$?; tail -n 30 "$OUT/bench.log"; exit $status; }

grep '^bench ' "$OUT/bench.log" | grep -v '^bench \(start\|done\)' || true
[ "$RESIDENCY" = 1 ] && { grep '^res' "$OUT/bench.log" || true; }
grep '^\(clipdiff\|pmdiff\|pmove\)' "$OUT/bench.log" | grep -v MISMATCH || true
grep -A2 'MISMATCH' "$OUT/bench.log" | head -n 30 || true
[ "$BMF_C" = 2 ] && { grep '^bmfasm' "$OUT/bench.log" | head -n 40 || true; }
[ "$ORDER_C" = 2 ] && { grep '^ordasm' "$OUT/bench.log" | grep -v ' 0 mismatches, 0 unchecked;' | head -n 40 || true
	awk '/^ordasm .*checked/ { c += $3; m += $8; u += $10; g += $16; w += substr($17, 2) }
	     END { printf "ordasm: %d asks checked, %d mismatches, %d unchecked; the general planes %d (%d wide)\n", c, m, u, g, w }' \
		"$OUT/bench.log"; }
[ "$BMFCHK" = 1 ] && { grep '^bmfchk\|^bmf room' "$OUT/bench.log" || true; }
[ "$MOVERS" = 1 ] && { grep '^\(bmfcut\|bmfhash\)' "$OUT/bench.log" || true; }
[ "$ZONES" = 1 ] && { grep '^zone: ' "$OUT/bench.log" || true; exit 0; }
[ "$MOVERS" = 1 ] && [ -z "$MAP" ] && exit 0
[ "$MONSTERS" != 0 ] && { grep '^\(mon\|stream\)' "$OUT/bench.log" | head -n 400 || true; }   # (head: SIGPIPE)
[ "$MONSTERS" = 2 ] && exit 0
[ "$VIEWS" = 0 ] || { [ "$BENCH_MAPS" = 1 ] && [ -z "$MAP" ]; } || [ "$HASHONLY" = 1 ] && exit 0
[ "$ENTS" = 1 ] && MAP=E1M1          # (its dumps: e1m1's textures, the first map)
[ "$MONSTERS" != 0 ] && MAP=${MAP:-E1M1}

# Render the dumped display lists to PNG (build-bench/views/<name>.png) so the
# output can be checked without a screen (the game's dumps need GAME.DAT's
# images too: the models level's file); a single map's bench (MAP=...) with
# that map's textures.
make -s -C "$ROOT/tools/pktview"
mkdir -p "$OUT/views"
TEXLEVEL=2; [ "$TEXTURES" = medium ] && TEXLEVEL=1
TEXMAP=start; [ -n "$MAP" ] && TEXMAP=$(echo "$MAP" | tr 'A-Z' 'a-z')
"$ROOT/tools/pktview/pktview" "$OUT/bench.log" \
	"$ROOT/cooked/id1/${TEXMAP}_t$TEXLEVEL.tx4" "$OUT/views/" \
	"$ROOT/cooked/id1/game_0.dat" "$ROOT/cooked/id1/stream_0.dat" | sed 's/^/  /'
