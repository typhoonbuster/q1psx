#!/usr/bin/env bash
#
# Build the asset cooker and cook the Quake data into ./cooked/ (never
# committed). Every map ships with its 4-bit textures at high, its low
# geometry, its low models (docs/porting-notes.md §52, §119).
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
#   Q1PSX_ZONE_TABLES  the level-chunks analysis' zone tables, one
#                <dir>/<map>.zones a map (numbers only: each zone's records,
#                each leaf's zone and its nearest by walking distance; made
#                outside the repo, analysis/export.py): each map split into
#                its skeleton and zone chunks (docs/porting-notes.md §57),
#                or left whole where that saves no RAM (three chunks
#                resident at once: a camera leaf's zone and its two nearest,
#                tools/qcook/zones.c). Default: zones/auto_1536_s3, the
#                repo's copy of the cut in use (§93, §118; a path from the
#                repo's root, or absolute); "none" (or empty): every map
#                whole
# The command line's Q1PSX_ZONE_TABLES wins over local.env's.
#
# A cook writes what its output depends on, besides the cooker's sources
# (CMake tracks those), to cooked/settings.txt: the settings above, the paks,
# the zone tables' and the music's contents, qcook's own switches
# (QCOOK_HAND, QCOOK_SOUND_KEEP: the share of
# its energy every sound keeps, each cooked at the lowest rate that does,
# in percent; default 98, 100 or 0 its own rate; docs/porting-notes.md
# §87; QCOOK_LOD_PX: the monsters' level of detail's tolerance in pixels,
# 2 by default, §88;
# QCOOK_ANIM_TOL: every body's
# animations thinned to that many units (a frame its neighbours' average
# makes well enough left out, tools/qcook/game.c ANIM_THIN, §97; 0 none);
# QCOOK_MERGE: a node's coplanar faces merged (1 by default, 0 the cook as
# before) and QCOOK_MERGE_ERR, the light it may lose at a luxel (16 of 255
# by default, negative no bound; tools/qcook/world.c, §113);
# QCOOK_MUSIC_RING_KB: the
# SPU RAM the music's ring takes, 48 by default: the bank fills the rest
# beside a swap room, §86). `cook.sh --stale <file>`, the
# build's check before the cook (CMakeLists.txt), rewrites <file> when they
# differ from the last cook's, and the build cooks again: a change in
# local.env or on the command line, or a cook another build directory made
# with other settings (they share cooked/).
#
# Output, per map (one `qcook map` run: the world's and the models' UVs on
# the tiles of the texture set it builds):
#   cooked/id1/<map>.tx4        QTX8 its 4-bit textures           -> disc <MAP>.TX4
#   cooked/id1/<map>.wld        QWVI its world                    -> disc <MAP>.WLD
#   cooked/id1/<map>.mdl        QMM2 its own models               -> disc <MAP>.MDL
#   cooked/id1/<map>.snd        QSB2 its sound bank               -> disc <MAP>.SND
#   cooked/id1/<map>.sndset     the level's sound set: `spu`/`ram`/`disc` a sound (qcook section reads it)
# and with Q1PSX_ZONE_TABLES, a map split into zones (formats/qworld.h):
#   cooked/id1/<map>.skl        QWZJ its skeleton                 -> disc <MAP>.WLD
#   cooked/id1/<map>.zon        QZN2 its chunks                   -> disc <MAP>.ZON
# the game data (the guns, the projectiles, the HUD; formats/qgame.h):
#   cooked/id1/game.dat         QGMB                              -> disc GAME.DAT
# the release menu's (formats/qsound.h, formats/qmenu.h):
#   cooked/id1/menu.snd         QSB2, the clicks and CD track 2   -> disc MAPS/MENU.SND
#   cooked/id1/conback.dat      QCB1, the console's background    -> disc CONBACK.DAT
# the streamed assets (the monsters, the items' models, the positional
# sounds; formats/qstream.h; the models low: tools/qcook/game.c
# stream_reduce):
#   cooked/id1/stream.dat       QST1                              -> disc STREAM.DAT
# and, per map, its own file of them (docs/porting-notes.md §84: the pool's
# directory with the sectors of the assets the map's entities bring, the
# nearest to the player's start first):
#   cooked/id1/<map>_s.dat      QST1                              -> disc <MAP>/<MAP>_S.DAT
# and the music the SPU streams (formats/qspu.h): 22.05 kHz, the menu's
# theme (track 2) 32 kHz:
#   cooked/spu/spu_NN.dat       QSP1                              -> disc MUSIC/SPU_NN.DAT
# Jobs run in parallel; each one's log is shown only if it fails.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

_zone_tables=${Q1PSX_ZONE_TABLES-unset}            # (the command line's wins over local.env's)
[ -f local.env ] && set -a && . ./local.env && set +a
[ "$_zone_tables" != unset ] && Q1PSX_ZONE_TABLES=$_zone_tables
Q1PSX_ZONE_TABLES=${Q1PSX_ZONE_TABLES-zones/auto_1536_s3}   # (the defaults, docs/porting-notes.md §118;
export QCOOK_SOUND_KEEP=${QCOOK_SOUND_KEEP:-98}              #  CMakeLists.txt checks the tables' draw
                                                             #  distance; 98: tools/qcook/sound.c's)
[ "$Q1PSX_ZONE_TABLES" = none ] && Q1PSX_ZONE_TABLES=
if [ -n "$Q1PSX_ZONE_TABLES" ] && [ ! -d "$Q1PSX_ZONE_TABLES" ]; then
	echo "cook: no zone tables at $Q1PSX_ZONE_TABLES (Q1PSX_ZONE_TABLES; none: every map whole)" >&2
	exit 1
fi
: "${QUAKE_PAK0:?set QUAKE_PAK0 in local.env}"

EP1="start e1m1 e1m2 e1m3 e1m4 e1m5 e1m6 e1m7 e1m8"
RETAIL="e2m1 e2m2 e2m3 e2m4 e2m5 e2m6 e2m7 e3m1 e3m2 e3m3 e3m4 e3m5 e3m6 e3m7
        e4m1 e4m2 e4m3 e4m4 e4m5 e4m6 e4m7 e4m8 end"
if [ -z "${COOK_MAPS:-}" ]; then
	COOK_MAPS=$( (ls cooked/id1/*.sndset 2>/dev/null || true) | sed 's|.*/||; s|\.sndset$||' | sort -u | tr '\n' ' ')
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
		     "contents $( (cat "$Q1PSX_ZONE_TABLES"/*.zones 2>/dev/null || true) | md5sum | cut -c1-32)"
	else
		echo "zone tables: none"
	fi
	if [ "${COOK_MUSIC:-1}" != 0 ] && [ -d "$QUAKE_MUSIC" ] && command -v ffmpeg > /dev/null; then
		echo "music: $(cd "$QUAKE_MUSIC" && pwd -P)," \
		     "$(cd "$QUAKE_MUSIC" && (stat -Lc '%n %s %Y' track*.ogg track*.mp3 track*.wav 2>/dev/null || true) | tr '\n' ' ')"
	else
		echo "music: none"
	fi
	echo "qcook: QCOOK_HAND=${QCOOK_HAND:-} QCOOK_SOUND_KEEP=${QCOOK_SOUND_KEEP:-} QCOOK_MUSIC_RING_KB=${QCOOK_MUSIC_RING_KB:-} QCOOK_LOD_PX=${QCOOK_LOD_PX:-} QCOOK_ANIM_TOL=${QCOOK_ANIM_TOL:-} QCOOK_MERGE=${QCOOK_MERGE:-} QCOOK_MERGE_ERR=${QCOOK_MERGE_ERR:-}"
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
rm -f cooked/id1/*.tx4 cooked/id1/*.wld cooked/id1/*.mdl cooked/id1/*.snd \
      cooked/id1/*.skl cooked/id1/*.zon cooked/id1/*.sndset cooked/id1/*.dat \
      cooked/settings.txt           # (written last: a cook cut short is cooked again)
LOGS=$(mktemp -d)
trap 'rm -rf "$LOGS"' EXIT
tools/qcook/qcook game --pak "$QUAKE_PAK0" --out cooked/id1/game.dat > "$LOGS/game.log" 2>&1 \
	|| { echo "cook FAILED: game data"; cat "$LOGS/game.log"; exit 1; }
# the release menu's: its sound bank (the clicks, the theme's CD track) and
# the console's background, shown before any map is loaded
tools/qcook/qcook menu --pak "$QUAKE_PAK0" --sounds cooked/id1/menu.snd --conback cooked/id1/conback.dat \
	> "$LOGS/menu.log" 2>&1 || { echo "cook FAILED: menu"; cat "$LOGS/menu.log"; exit 1; }
# the streamed assets (formats/qstream.h): the monsters, the items' models,
# the positional sounds
tools/qcook/qcook stream --pak "$QUAKE_PAK0" ${QUAKE_PAK1:+--pak1 "$QUAKE_PAK1"} --out cooked/id1/stream.dat \
                         > "$LOGS/stream.log" 2>&1 \
	|| { echo "cook FAILED: streamed assets"; cat "$LOGS/stream.log"; exit 1; }

cook_one() {   # map: its textures, world, own models and sound bank, one run
	local pak=$QUAKE_PAK0
	case $1 in e2*|e3*|e4*|end) pak=${QUAKE_PAK1:?$1 needs QUAKE_PAK1} ;; esac
	local fm=""
	if [ "$FMAP" = 1 ] && [ -f "$Q1PSX_ZONE_TABLES/$1.zones" ]; then   # (the zones' tables name the
		                                 #  records of a cook without the merge: each record's faces,
		                                 #  to translate them, below)
		fm="--fmap $LOGS/$1.fmap"
		QCOOK_MERGE=0 tools/qcook/qcook map --pak "$pak" --palette-pak "$QUAKE_PAK0" --map "maps/$1.bsp" \
		              --world "$LOGS/$1.unmerged.wld" --fmap "$LOGS/$1.unmerged.fmap" \
		              --light-gain "${LIGHT_GAIN:-1.66}" > "$LOGS/$1.unmerged.log" 2>&1 \
		|| { echo "cook FAILED: $1 (unmerged)"; cat "$LOGS/$1.unmerged.log"; return 1; }
	fi
	tools/qcook/qcook map --pak "$pak" --palette-pak "$QUAKE_PAK0" --map "maps/$1.bsp" \
	                      --tex4 "cooked/id1/$1.tx4" --world "cooked/id1/$1.wld" --mdl "cooked/id1/$1.mdl" \
	                      --sounds "cooked/id1/$1.snd" --sndset "cooked/id1/$1.sndset" $fm \
	                      --light-gain "${LIGHT_GAIN:-1.66}" > "$LOGS/$1.log" 2>&1 \
	|| { echo "cook FAILED: $1"; cat "$LOGS/$1.log"; return 1; }
}
# the coplanar faces merged (QCOOK_MERGE, 1 by default; docs/porting-notes.md
# §113) renumber the world's records: the zones' tables, made on a cook
# without it, are translated through each record's Quake faces
FMAP=0
if [ "${QCOOK_MERGE:-1}" != 0 ] && [ -n "${Q1PSX_ZONE_TABLES:-}" ] && [ -d "$Q1PSX_ZONE_TABLES" ]; then
	FMAP=1
fi
export -f cook_one
export QUAKE_PAK0 QUAKE_PAK1 LOGS LIGHT_GAIN FMAP Q1PSX_ZONE_TABLES

for m in $MAPS; do echo "$m"; done | xargs -P "$(nproc)" -I{} bash -c 'cook_one {}'

echo "cooked $(echo $MAPS | wc -w) maps: the 4-bit textures high, the geometry and the models low -> $ROOT/cooked/"

# each map's own file of the streamed assets (docs/porting-notes.md §84):
# the pool's directory, the assets its entities bring, the nearest to the
# player's start first
section_one() {   # map
	local pak=$QUAKE_PAK0
	case $1 in e2*|e3*|e4*|end) pak=${QUAKE_PAK1:?$1 needs QUAKE_PAK1} ;; esac
	tools/qcook/qcook section --pak "$pak" ${QUAKE_PAK1:+--pak1 "$QUAKE_PAK1"} --stream cooked/id1/stream.dat \
	                          --map "maps/$1.bsp" --out "cooked/id1/$1_s.dat" --sndset "cooked/id1/$1.sndset" \
	                          > "$LOGS/$1_s.log" 2>&1 \
	|| { echo "cook FAILED: $1 section"; cat "$LOGS/$1_s.log"; return 1; }
}
export -f section_one
for m in $MAPS; do echo "$m"; done | xargs -P "$(nproc)" -I{} bash -c 'section_one {}'
cat "$LOGS"/*_s.log | grep '^qcook: ' | sed 's|^qcook: .*/|section: |' || true

# the world's zones (docs/porting-notes.md §57): each map with a table split
# into its skeleton and its chunks (the world's records as they are, their
# UVs on the texture set's tiles), or left whole where that saves no
# RAM (the cook says which)
if [ -n "${Q1PSX_ZONE_TABLES:-}" ] && [ -d "$Q1PSX_ZONE_TABLES" ]; then
	export Q1PSX_ZONE_TABLES
	for m in $MAPS; do echo "$m"; done | xargs -P "$(nproc)" -I{} bash -c \
	    't="$Q1PSX_ZONE_TABLES/{}.zones"; [ -f "$t" ] || exit 0
	     fm=""; [ "$FMAP" = 1 ] && fm="--fmap $LOGS/{}.unmerged.fmap $LOGS/{}.fmap"   # (QCOOK_MERGE)
	     tools/qcook/qcook zones --world cooked/id1/{}.wld --tables "$t" $fm --skeleton cooked/id1/{}.skl \
	         --chunks cooked/id1/{}.zon > "$LOGS/{}_z.log" 2>&1 \
	     || { echo "cook FAILED: zones {}"; cat "$LOGS/{}_z.log"; exit 1; }'
	echo "zones: $(ls cooked/id1/*.skl 2>/dev/null | wc -l) of $(echo $MAPS | wc -w) maps split ($Q1PSX_ZONE_TABLES)"
else
	echo "zones: none, every map whole (Q1PSX_ZONE_TABLES=none; zones/auto_1536_s3 by default)"
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
