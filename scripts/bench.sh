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
#         same frames). BMFARENA=<percent> shrinks the brush models' cut
#         arena (engine/bmfrag.c §90.6) to that share of the cook's size:
#         the rooms given up and packed down in play (an A / B: the same
#         frames while every drawn model still fits). BMFCHK=1 (with
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
#         STACK_BYTES=<bytes>: the stack's room (default 8192, the
#         game's: engine/mem.h; the level arena takes what the code leaves
#         under it); LEVEL_MIN=<bytes>: the least level arena the link
#         accepts (default 1272832, 1243 KB); LEVEL_CAP=<bytes> caps the
#         arena (a bench's code may leave it more than the release's: the
#         release's `ramcheck` line gives its size, for the streams' pools as
#         the game has them). The boot log's "stack: N bytes never reached"
#         line shows the stack's room left after a load.
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
#         §69); then the crowd: every monster of the map awake, the
#         entities' cost a second); MONSTERS=2 the crowd alone (SAMPLE=1:
#         its PC samples, "samples crowd"; ZONES=1's walk and MOVERS=1's
#         movers take SAMPLE=1 too, a "samples <map>" / "samples mv.<map>.<model>"
#         histogram each); MONSTERS=3 the crowd, then a
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
#         as the player standing there sees them; USE=<targetname or classname> uses
#         those entities at tick 8, a button's press: a door
#         opening in view); RESIDENCY=1 (with
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
#         carries both cuts (with SAMPLE=1 too, its level arena is under
#         1243 KB: LEVEL_MIN= lower, or STACK_BYTES=4096). ORDER_C=1 builds an alias
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
#         docs/porting-notes.md §57); ZONES=2 walks the whole maps too
#         (their sounds: "sounds asked, not in"). CAPTURE=1 plays the
#         capture's scenarios in real time through the game's loop (the
#         sounds read from the disc in their worst cases: engine/main.c
#         bench_capture), for an emulator's recording; with MUSIC=1.
#         WHOLE=1 puts the whole worlds on the
#         disc, the zones' A / B (CMake Q1PSX_ZONES OFF). Every bench prints
#         a "bench zone <map>" line per map loaded: the skeleton, the
#         chunk arena, the stream's pool.
#         CDLOG=1 writes the drive's journal: a "cd" line per read (who
#         read what, where the head came from, the vblanks it took, the
#         seek a real drive would spend) and a "cd sum" line per phase (a
#         map's load, its walk); CDMODEL=1 ends every read no sooner than a
#         real drive would (DuckStation's seek model, measured on the
#         mechanism, and the sectors at double speed): the walk's late
#         frames, the stream's reads and the music's underruns as on a
#         console, in fast forward (docs/porting-notes.md §83). Both go
#         with MUSIC=1. MUSIC_RING=<chunks> sizes the music's ring in SPU
#         RAM (6 = 2 s, the game's; 4 = 1.3 s), MUSIC_REFILL=<chunks> a
#         refill (2, the game's: 8 sectors every 0.65 s, a 16 KB staging
#         buffer; 4: 16 every 1.3 s, 32 KB, §84's),
#         MUSIC_SEEK=<ms> the seek its refills plan around a read (100, the
#         game's: a hop within a level's section; docs/porting-notes.md
#         §84); tools/cdsim/cdsim.py replays a journal on another disc
#         layout. MEMLOG=1 logs the level arena: a "memlog" line per
#         block taken or given back, with its caller's return address
#         (addr2line -e build-bench/q1psx.elf): a map's RAM block by block
#         (docs/porting-notes.md §90).
#         LOD=0 draws every streamed model whole at every distance (CMake
#         Q1PSX_LOD; the level of detail by the vertices' order,
#         docs/porting-notes.md §88): an A / B of the engine's cut alone,
#         the cook's order kept (QCOOK_NOLOD=1 cooks today's order). Every
#         view prints a "lod view" line: the streamed models drawn, those
#         cut to a band; MONSTERS=4 a "clear frames" line: the fights'
#         frames, their cycles, the models drawn and cut.
#         ENGINE_C=1 builds the C engine (CMake Q1PSX_ENGINE_C: every asm
#         part's C version but the world renderer's, docs/porting-notes.md
#         §55) in build-bench-c/, beside the asm's: its frames must be the
#         asm's, hash for hash (scripts/dlcheck.sh: save a reference with
#         the asm, check it with ENGINE_C=1: every view's cycles, asm to C).
#         WORLD_HZ=60 runs the world's edicts at 60 ticks a second (default
#         30, the game's default: half of them each tick, engine/ents.c
#         ents_tick; the player, the movers, the projectiles: 60 either
#         way; docs/porting-notes.md §72).
#         RES=<n> renders the 3D view at an internal resolution below the
#         screen's: engine/render.c r_res_table's index (0 320x240, the
#         default; 1 256x224, 2 240x180, 3 200x150; docs/porting-notes.md
#         §78). GPUTIME=1 draws each frame's display list alone, the CPU
#         waiting, and adds its time to the lines (gpu=avg/max, CPU
#         cycles): run the disc on DuckStation, which counts the GPU's time
#         (PCSX-Redux draws at once: 0). WARPDYN=1 renders the views with
#         the game's dynamic warp control (T= its floor) and prints the T
#         each view ends at ("warp <view> T <px> more <vblanks>").
#         IRQTIME=1: the interrupts' handler (engine/irq.s) times itself, the
#         lines' irq=<cycles a frame>/<interrupts a frame x 100> (all of
#         them, those that land in the wait for the vblank too).
#         VRAMCHK=1: every streamed image's VRAM place searched again the
#         way before §100 and compared ("stream: vramchk" lines at each
#         map's start: the searches, the places that differ).
#         SEQCHK=1: a view drawn by a zone's cooked walk (docs/porting-notes.md
#         §91) is drawn again by the tree walk and the two compared: the
#         leaves' and the faces' stamps, the records drawn ("seqchk" lines).
#         TESS=<maps>: the maps whose near polygons take the regular subdivision
#         before the slow path (engine/r_tess.s, §92): names separated by commas,
#         all, or none (default START,E1M1)
#         TESSK=<0..3>: the grid's finest level, 2^k cells along an axis (0: the
#         unsplit polygons alone go out as one packet, every split one to the
#         slow path; default 3)
#         TESSC=<n>: the grid's cap: a polygon whose two levels sum past n (more
#         than 2^n cells) goes to the slow path (default 4)
#         SLOWSTAT=1: the slow path counts its paths (engine/r_slow.s STAT):
#         a "slowstat" line after each view's, a frame's polygons, clipped
#         ones, triangles popped, wholly on the screen, leaves, split,
#         refined, midpoints made, forced leaves.
#
# Each line reads:
#   bench <scenario> n=<frames> vbl=<vblanks/frame x100>
#         <stage>=<avg cycles>/<max cycles> ... <counter>=<avg per frame> ...
#
# Numbers are PCSX-Redux CPU timings: good for comparing variants, not a
# cycle-exact hardware measurement (see docs/architecture.md §9).
#
# The emulator runs in fast forward (EMU= a wrapper with SPU.Speed 10): the
# 128 views take 40 s, a fight 20 s, the zones' walk 4 min. Give
# [max-seconds] two or three times that, no more; a run whose console
# stops growing is stopped after STALL seconds (30, scripts/headless.sh).

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
BMFARENA=${BMFARENA:-100}
BMFCHK=${BMFCHK:-0}
STACK_BYTES=${STACK_BYTES:-8192}
LEVEL_MIN=${LEVEL_MIN:-1272832}
LEVEL_CAP=${LEVEL_CAP:-0}
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
USE=${USE:-}
RESIDENCY=${RESIDENCY:-0}
ENTS=${ENTS:-0}
MONSTERS=${MONSTERS:-0}
MOVERS=${MOVERS:-0}
SAMPLE=${SAMPLE:-0}
ZONES=${ZONES:-0}
CAPTURE=${CAPTURE:-0}
WORLD_HZ=${WORLD_HZ:-30}
RES=${RES:-0}
GPUTIME=${GPUTIME:-0}
IRQTIME=${IRQTIME:-0}
SLOWSTAT=${SLOWSTAT:-0}
WARPDYN=${WARPDYN:-0}
SEQCHK=${SEQCHK:-0}
VRAMCHK=${VRAMCHK:-0}
TESS=${TESS:-START,E1M1}
TESSK=${TESSK:-3}
TESSC=${TESSC:-4}
LOD=${LOD:-1}
WHOLE=${WHOLE:-0}
CDLOG=${CDLOG:-0}
CDMODEL=${CDMODEL:-0}
MEMLOG=${MEMLOG:-0}
MUSIC_RING=${MUSIC_RING:-6}
MUSIC_REFILL=${MUSIC_REFILL:-2}
MUSIC_SEEK=${MUSIC_SEEK:-100}
cmake --preset bench -B "$OUT" -DQ1PSX_ENGINE_C="$([ "$ENGINE_C" = 1 ] && echo ON || echo OFF)" -DQ1PSX_TEXTURES="$TEXTURES" -DQ1PSX_WEAPON="$WEAPON" -DQ1PSX_DRAWDIST="$DRAWDIST" \
      -DQ1PSX_BENCH_EFFECTS="$EFFECTS" -DQ1PSX_BENCH_VIEWS="$VIEWS" \
      -DQ1PSX_BENCH_MAPS="$BENCH_MAPS" -DQ1PSX_BENCH_GAME="$BENCH_GAME" -DQ1PSX_PROFILE="$PROFILE" \
      -DQ1PSX_BENCH_T="$T" -DQ1PSX_BENCH_HASHONLY="$HASHONLY" \
      -DQ1PSX_BENCH_FLAT="$FLAT" -DQ1PSX_BENCH_BMF="$((1 - NOBMF))" \
      -DQ1PSX_BENCH_BMFCULL="$((1 - NOBMFCULL))" \
      -DQ1PSX_BENCH_BMFCHK="$BMFCHK" -DQ1PSX_BENCH_BMFARENA="$BMFARENA" -DQ1PSX_STACK_BYTES="$STACK_BYTES" -DQ1PSX_LEVEL_MIN="$LEVEL_MIN" -DQ1PSX_LEVEL_CAP="$LEVEL_CAP" \
      -DQ1PSX_DLIGHT_C="$DLIGHT_C" -DQ1PSX_BMF_C="$BMF_C" -DQ1PSX_ORDER_C="$ORDER_C" -DQ1PSX_BENCH_PLAY="$PLAY" -DQ1PSX_BENCH_MUSIC="$MUSIC" \
      -DQ1PSX_BENCH_MAP="$MAP" -DQ1PSX_BENCH_VIEW="$VIEW" -DQ1PSX_BENCH_NOENTS="$NOENTS" -DQ1PSX_BENCH_TICKS="$TICKS" -DQ1PSX_BENCH_USE="$USE" -DQ1PSX_BENCH_SKILL="${SKILL:-1}" -DQ1PSX_BENCH_RESIDENCY="$RESIDENCY" \
      -DQ1PSX_BENCH_ENTS="$ENTS" -DQ1PSX_BENCH_MONSTERS="$MONSTERS" -DQ1PSX_BENCH_SAMPLE="$SAMPLE" \
      -DQ1PSX_BENCH_MOVERS="$MOVERS" -DQ1PSX_BENCH_ZONES="$ZONES" -DQ1PSX_BENCH_CAPTURE="$CAPTURE" -DQ1PSX_BENCH_WORLD_HZ="$WORLD_HZ" -DQ1PSX_BENCH_RES="$RES" -DQ1PSX_BENCH_GPUTIME="$GPUTIME" -DQ1PSX_BENCH_IRQTIME="$IRQTIME" -DQ1PSX_BENCH_SLOWSTAT="$SLOWSTAT" -DQ1PSX_BENCH_WARPDYN="$WARPDYN" -DQ1PSX_BENCH_SEQCHK="$SEQCHK" -DQ1PSX_BENCH_VRAMCHK="$VRAMCHK" -DQ1PSX_TESS_MAPS="$TESS" -DQ1PSX_TESS_KMAX="$TESSK" -DQ1PSX_TESS_CAP="$TESSC" -DQ1PSX_LOD="$LOD" -DQ1PSX_ZONES="$([ "$WHOLE" = 1 ] && echo OFF || echo ON)" \
      -DQ1PSX_BENCH_CDLOG="$CDLOG" -DQ1PSX_BENCH_CDMODEL="$CDMODEL" -DQ1PSX_BENCH_MEMLOG="$MEMLOG" -DQ1PSX_MUSIC_RING="$MUSIC_RING" -DQ1PSX_MUSIC_REFILL="$MUSIC_REFILL" -DQ1PSX_MUSIC_SEEK_MS="$MUSIC_SEEK" \
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
{ [ "$CDLOG" = 1 ] || [ "$CDMODEL" = 1 ]; } && { grep '^cd sum\|^music: the ring' "$OUT/bench.log" || true; }
[ "$ZONES" != 0 ] && { grep '^zone: ' "$OUT/bench.log" || true; exit 0; }
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
