#!/usr/bin/env bash
#
# Build the asset cooker and cook the Quake data into ./cooked/ (never
# committed). Every map ships with its 4-bit textures at medium and high
# (the build takes one: CMake's Q1PSX_TEXTURES), its low geometry, its low
# models (formats/qlevels.h; docs/porting-notes.md §52).
#
# Env / local.env:
#   QUAKE_PAK0   required — Quake (id1) pak0.pak (episode 1, shareware)
#   QUAKE_PAK1   optional — the retail pak1.pak (episodes 2-4)
#   COOK_MAPS    maps to cook: a list ("start e1m1"), "ep1" or "all" (needs
#                QUAKE_PAK1). Unset: the maps already in cooked/id1 (so the
#                re-cook a build triggers keeps the disc's map set: CMake
#                globbed it at configure time), or ep1 if there are none.
#   LIGHT_GAIN   vertex light scale (default 1.66; 1 = Quake's own brightness,
#                dark on a TV; the engine's dynamic lights follow it: dlight.h
#                DL_GAIN)
#   QUAKE_MUSIC  a directory of trackNN.ogg / .mp3 / .wav (the 2021 rerelease:
#                <Quake>/rerelease/id1/music); default: next to pak0. ffmpeg
#                resamples them for the SPU music.
#   COOK_MUSIC   0 skips the music (default 1 when the directory exists)
#   Q1PSX_ZONE_TABLES  optional — the level-chunks analysis' zone tables
#                (<dir>/<map>.zones, e.g. .../zones/pvs_1536_96: derived
#                from Quake's data, never in the repo): each map split into
#                its skeleton and zone chunks (docs/porting-notes.md §57),
#                or left whole where that saves no RAM. Unset: every map
#                whole, as before
#   Q1PSX_ZONE_SLOTS   with the tables, the chunks resident at once (3, the
#                default; 4: the three nearest zones; 5: and the last left)
# The command line's Q1PSX_ZONE_TABLES and Q1PSX_ZONE_SLOTS win over
# local.env's.
#
# A cook writes what its output depends on, besides the cooker's sources
# (CMake tracks those), to cooked/settings.txt: the settings above, the paks,
# the zone tables' and the music's contents, qcook's own switches
# (QCOOK_NOHAND, QCOOK_HAND, QCOOK_NOFMAP, QCOOK_SOUND_KEEP: the share of
# its energy every sound keeps, each cooked at the lowest rate that does,
# in percent; default 95, 100 or 0 its own rate; docs/porting-notes.md
# §87; QCOOK_LOD_PX: the monsters' level of detail's tolerance in pixels,
# 2 by default, QCOOK_NOLOD=1 none, the vertices in today's order, §88;
# QCOOK_MUSIC_RING_KB: the
# SPU RAM the music's ring takes, 48 by default: the bank fills the rest
# beside a swap room, §86). `cook.sh --stale <file>`, the
# build's check before the cook (CMakeLists.txt), rewrites <file> when they
# differ from the last cook's, and the build cooks again: a change in
# local.env or on the command line, or a cook another build directory made
# with other settings (they share cooked/).
#
# Output, per map (formats/qlevels.h: 0 low, 1 medium, 2 high):
#   cooked/id1/<map>_t<N>.tx4   QTX7 4-bit textures (1, 2)        -> disc <MAP>_T<N>.TX4
#   cooked/id1/<map>_g0.wld     QWVH world, low geometry          -> disc <MAP>_G0.WLD
#   cooked/id1/<map>_m0.mdl     QMM1 its own models, low          -> disc <MAP>_M0.MDL
#   cooked/id1/<map>.snd        QSB2 its sound bank               -> disc <MAP>.SND
#   cooked/id1/<map>.sndset     the level's sound set: `spu`/`ram` a sound (qcook section reads it)
# and with Q1PSX_ZONE_TABLES, a map split into zones (formats/qworld.h):
#   cooked/id1/<map>_g0.skl     QWZI its skeleton                 -> disc <MAP>_G0.WLD
#   cooked/id1/<map>_z<N>.zon   QZN1 its chunks, textures N (1, 2) -> disc <MAP>_Z<N>.ZON
# the game data (the guns, the projectiles, the HUD; formats/qgame.h):
#   cooked/id1/game_0.dat       QGM8, low                         -> disc GAME_0.DAT
# the release menu's (formats/qsound.h, formats/qmenu.h):
#   cooked/id1/menu.snd         QSB2, the clicks and CD track 2   -> disc MAPS/MENU.SND
#   cooked/id1/conback.dat      QCB1, the console's background    -> disc CONBACK.DAT
# the streamed assets (the monsters, the items' models, the positional
# sounds; formats/qstream.h; the models low: tools/qcook/game.c
# stream_reduce):
#   cooked/id1/stream_0.dat     QST1                              -> disc STREAM_0.DAT
# and, per map, its own file of them (docs/porting-notes.md §84: the pool's
# directory with the sectors of the assets the map's entities bring, the
# nearest to the player's start first):
#   cooked/id1/<map>_s0.dat     QST1                              -> disc <MAP>/<MAP>_S0.DAT
# and the music the SPU streams (formats/qspu.h): 22.05 kHz, the menu's
# theme (track 2) 32 kHz:
#   cooked/spu/spu_NN.dat       QSP1                              -> disc MUSIC/SPU_NN.DAT
# Jobs run in parallel; each one's log is shown only if it fails.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

_zone_tables=${Q1PSX_ZONE_TABLES-unset}            # (the command line's wins over local.env's)
_zone_slots=${Q1PSX_ZONE_SLOTS-unset}
[ -f local.env ] && set -a && . ./local.env && set +a
[ "$_zone_tables" != unset ] && Q1PSX_ZONE_TABLES=$_zone_tables
[ "$_zone_slots" != unset ] && Q1PSX_ZONE_SLOTS=$_zone_slots
: "${QUAKE_PAK0:?set QUAKE_PAK0 in local.env}"

EP1="start e1m1 e1m2 e1m3 e1m4 e1m5 e1m6 e1m7 e1m8"
RETAIL="e2m1 e2m2 e2m3 e2m4 e2m5 e2m6 e2m7 e3m1 e3m2 e3m3 e3m4 e3m5 e3m6 e3m7
        e4m1 e4m2 e4m3 e4m4 e4m5 e4m6 e4m7 e4m8 end"
if [ -z "${COOK_MAPS:-}" ]; then
	COOK_MAPS=$( (ls cooked/id1/*_g0.wld cooked/id1/*_g2.wld 2>/dev/null || true) |   # (_g2: the
	            sed 's|.*/||; s|_g[02]\.wld$||' | sort -u | tr '\n' ' ')               # cooks before §52)
	COOK_MAPS=${COOK_MAPS:-ep1}
fi
case "$COOK_MAPS" in
	ep1) MAPS=$EP1 ;;
	all) : "${QUAKE_PAK1:?COOK_MAPS=all needs QUAKE_PAK1 in local.env}"
	     MAPS="$EP1 $RETAIL" ;;
	*)   MAPS=$COOK_MAPS ;;
esac
QUAKE_MUSIC=${QUAKE_MUSIC:-"$(dirname "$QUAKE_PAK0")/../rerelease/id1/music"}

settings() {   # what the cooked files depend on, besides the cooker's sources
	echo "maps: $(for m in $MAPS; do echo "$m"; done | sort | tr '\n' ' ')"
	local p
	for p in "$QUAKE_PAK0" "${QUAKE_PAK1:-}"; do
		if [ -n "$p" ]; then echo "pak: $p $(stat -Lc '%s %Y' "$p" 2>/dev/null || true)"; fi
	done
	echo "light gain: ${LIGHT_GAIN:-1.66}"
	if [ -n "${Q1PSX_ZONE_TABLES:-}" ] && [ -d "$Q1PSX_ZONE_TABLES" ]; then
		echo "zone tables: $(cd "$Q1PSX_ZONE_TABLES" && pwd -P)," \
		     "contents $( (cat "$Q1PSX_ZONE_TABLES"/*.zones 2>/dev/null || true) | md5sum | cut -c1-32)," \
		     "slots ${Q1PSX_ZONE_SLOTS:-3}"
	else
		echo "zone tables: none"
	fi
	if [ "${COOK_MUSIC:-1}" != 0 ] && [ -d "$QUAKE_MUSIC" ] && command -v ffmpeg > /dev/null; then
		echo "music: $(cd "$QUAKE_MUSIC" && pwd -P)," \
		     "$(cd "$QUAKE_MUSIC" && (stat -Lc '%n %s %Y' track*.ogg track*.mp3 track*.wav 2>/dev/null || true) | tr '\n' ' ')"
	else
		echo "music: none"
	fi
	echo "qcook: QCOOK_NOHAND=${QCOOK_NOHAND:-} QCOOK_HAND=${QCOOK_HAND:-} QCOOK_NOFMAP=${QCOOK_NOFMAP:-} QCOOK_SOUND_KEEP=${QCOOK_SOUND_KEEP:-} QCOOK_MUSIC_RING_KB=${QCOOK_MUSIC_RING_KB:-} QCOOK_LOD_PX=${QCOOK_LOD_PX:-} QCOOK_NOLOD=${QCOOK_NOLOD:-}"
}
if [ "${1:-}" = --stale ]; then
	stamp=${2:?--stale needs the file the cook depends on}
	if [ ! -f "$stamp" ]; then                    # (a new build directory: no cook for that alone)
		: > "$stamp"
		touch -d @0 "$stamp"
	fi
	now=$(settings)
	last=$(cat cooked/settings.txt 2>/dev/null || true)
	if [ "$now" != "$last" ]; then
		echo "cook: the settings differ from the last cook's${last:+ (cooked/settings.txt)}, cooking again:"
		diff <(echo "$last") <(echo "$now") | sed -n 's/^> /  /p' || true
		echo "$now" > "$stamp"
	fi
	exit 0
fi

make -s -C tools/qcook
# start clean: the disc carries every cooked file, so a map cooked earlier
# (another COOK_MAPS, an older format) must not linger
mkdir -p cooked/id1
rm -f cooked/id1/*.tex cooked/id1/*.tx4 cooked/id1/*.wld cooked/id1/*.mdl cooked/id1/*.snd \
      cooked/id1/*.skl cooked/id1/*.zon cooked/id1/*_s0.dat cooked/id1/*.sndset \
      cooked/id1/game*.dat cooked/id1/stream*.dat cooked/id1/conback.dat \
      cooked/settings.txt           # (written last: a cook cut short is cooked again)
rm -rf cooked/xa                    # (the XA streams and their carousels: none since §52)
LOGS=$(mktemp -d)
trap 'rm -rf "$LOGS"' EXIT
tools/qcook/qcook game --pak "$QUAKE_PAK0" --out cooked/id1/game > "$LOGS/game.log" 2>&1 \
	|| { echo "cook FAILED: game data"; cat "$LOGS/game.log"; exit 1; }
# the release menu's: its sound bank (the clicks, the theme's CD track) and
# the console's background, shown before any map is loaded
tools/qcook/qcook menu --pak "$QUAKE_PAK0" --sounds cooked/id1/menu.snd --conback cooked/id1/conback.dat \
	> "$LOGS/menu.log" 2>&1 || { echo "cook FAILED: menu"; cat "$LOGS/menu.log"; exit 1; }
# the streamed assets (formats/qstream.h): the monsters, the items' models,
# the positional sounds
tools/qcook/qcook stream --pak "$QUAKE_PAK0" ${QUAKE_PAK1:+--pak1 "$QUAKE_PAK1"} --out cooked/id1/stream \
                         > "$LOGS/stream.log" 2>&1 \
	|| { echo "cook FAILED: streamed assets"; cat "$LOGS/stream.log"; exit 1; }

cook_one() {   # map axis level (t / g / m, 0..2)
	local pak=$QUAKE_PAK0
	case $1 in e2*|e3*|e4*|end) pak=${QUAKE_PAK1:?$1 needs QUAKE_PAK1} ;; esac
	local name out="cooked/id1/$1_$2$3" what
	name=$(echo low medium high ultra | cut -d' ' -f$(($3 + 1)))
	case $2 in
	t) what="--textures $name --tex4 $out.tx4" ;;
	g) what="--geometry $name --world $out.wld --sounds cooked/id1/$1.snd --sndset cooked/id1/$1.sndset" ;;
	m) what="--models $name --mdl $out.mdl" ;;
	esac
	tools/qcook/qcook map --pak "$pak" --palette-pak "$QUAKE_PAK0" --map "maps/$1.bsp" $what \
	                      --light-gain "${LIGHT_GAIN:-1.66}" > "$LOGS/$1_$2$3.log" 2>&1 \
	|| { echo "cook FAILED: $1 $2$3"; cat "$LOGS/$1_$2$3.log"; return 1; }
}
export -f cook_one
export QUAKE_PAK0 QUAKE_PAK1 LOGS LIGHT_GAIN

for m in $MAPS; do
	for n in 1 2; do echo "$m t $n"; done         # textures: medium, high (the build takes one)
	echo "$m g 0"                                 # geometry: low
	echo "$m m 0"                                 # models: low
done | xargs -P "$(nproc)" -n 3 bash -c 'cook_one "$0" "$1" "$2"'

echo "cooked $(echo $MAPS | wc -w) maps: the 4-bit textures medium and high, the geometry and the models low -> $ROOT/cooked/"

# each map's own file of the streamed assets (docs/porting-notes.md §84):
# the pool's directory, the assets its entities bring, the nearest to the
# player's start first
section_one() {   # map
	local pak=$QUAKE_PAK0
	case $1 in e2*|e3*|e4*|end) pak=${QUAKE_PAK1:?$1 needs QUAKE_PAK1} ;; esac
	tools/qcook/qcook section --pak "$pak" ${QUAKE_PAK1:+--pak1 "$QUAKE_PAK1"} --stream cooked/id1/stream_0.dat \
	                          --map "maps/$1.bsp" --out "cooked/id1/$1_s0.dat" --sndset "cooked/id1/$1.sndset" \
	                          > "$LOGS/$1_s.log" 2>&1 \
	|| { echo "cook FAILED: $1 section"; cat "$LOGS/$1_s.log"; return 1; }
}
export -f section_one
for m in $MAPS; do echo "$m"; done | xargs -P "$(nproc)" -I{} bash -c 'section_one {}'
cat "$LOGS"/*_s.log | grep '^qcook: ' | sed 's|^qcook: .*/|section: |' || true

# the world's zones (docs/porting-notes.md §57): each map with a table split
# into its skeleton and its chunks at both texture levels (their UVs on that
# set's tiles: after the textures' jobs), or left whole where that saves no
# RAM (the cook says which); Q1PSX_ZONE_SLOTS the chunks resident at once
# (3: the camera's zone and its two nearest; 4: its three nearest; 5: and the
# last one left, kept)
if [ -n "${Q1PSX_ZONE_TABLES:-}" ] && [ -d "$Q1PSX_ZONE_TABLES" ]; then
	export Q1PSX_ZONE_TABLES
	export Q1PSX_ZONE_SLOTS=${Q1PSX_ZONE_SLOTS:-3}
	for m in $MAPS; do echo "$m"; done | xargs -P "$(nproc)" -I{} bash -c \
	    't="$Q1PSX_ZONE_TABLES/{}.zones"; [ -f "$t" ] || exit 0
	     tools/qcook/qcook zones --resident "$Q1PSX_ZONE_SLOTS" --world cooked/id1/{}_g0.wld --tables "$t" --skeleton cooked/id1/{}_g0.skl \
	         --tex4 cooked/id1/{}_t1.tx4 --chunks cooked/id1/{}_z1.zon \
	         --tex4 cooked/id1/{}_t2.tx4 --chunks cooked/id1/{}_z2.zon > "$LOGS/{}_z.log" 2>&1 \
	     || { echo "cook FAILED: zones {}"; cat "$LOGS/{}_z.log"; exit 1; }'
	echo "zones: $(ls cooked/id1/*_g0.skl 2>/dev/null | wc -l) of $(echo $MAPS | wc -w) maps split ($Q1PSX_ZONE_TABLES)"
else
	echo "zones: none, every map whole (Q1PSX_ZONE_TABLES in local.env names the tables)"
fi

# the music: Quake's CD tracks 2..11 (worldspawn "sounds"), from the
# rerelease's tracks, resampled once by ffmpeg (22.05 kHz; the menu's theme,
# track 2, 32 kHz) -> cooked/spu/trackNN.wav -> spu_NN.dat (formats/qspu.h)
if [ "${COOK_MUSIC:-1}" != 0 ] && [ -d "$QUAKE_MUSIC" ] && command -v ffmpeg >/dev/null; then
	mkdir -p cooked/spu
	tracks=""
	for src in "$QUAKE_MUSIC"/track*.ogg "$QUAKE_MUSIC"/track*.mp3 "$QUAKE_MUSIC"/track*.wav; do
		[ -f "$src" ] || continue
		nn=$(basename "${src%.*}" | sed 's/^track//')
		rate=22050; [ "$nn" = 02 ] && rate=32000
		out="cooked/spu/track$nn.wav"
		if [ ! -f "$out" ] || [ "$src" -nt "$out" ] || [ "$(cat "$out.rate" 2>/dev/null)" != "$rate" ]; then
			ffmpeg -loglevel error -y -i "$src" -ar "$rate" -ac 2 -sample_fmt s16 \
			       -fflags +bitexact -flags:a +bitexact "$out" || { echo "cook FAILED: music $src"; exit 1; }
			echo "$rate" > "$out.rate"
		fi
		tracks="$tracks $nn"
	done
	rm -f cooked/spu/spu_*.dat
	for nn in $tracks; do echo "$nn"; done | xargs -P "$(nproc)" -I{} bash -c \
	    'rate=22050; [ {} = 02 ] && rate=32000
	     tools/qcook/qcook spumusic --music cooked/spu --track {} --rate $rate --out cooked/spu/spu_{}.dat \
	         > "$LOGS/spu_{}.log" 2>&1 \
	     || { echo "cook FAILED: the SPU music of track {}"; cat "$LOGS/spu_{}.log"; exit 1; }'
	cat "$LOGS"/spu_*.log | grep '^qcook: ' | sed 's|^qcook: .*/|music: |' || true
	echo "music: $(echo $tracks | wc -w) tracks in $ROOT/cooked/spu/"
else
	echo "music: none (set QUAKE_MUSIC / COOK_MUSIC=1, needs ffmpeg)"
fi

settings > cooked/settings.txt      # (what the next build's --stale compares its own with)
