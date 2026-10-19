#!/usr/bin/env bash
#
# Build the asset cooker and cook the Quake data into ./cooked/ (never
# committed). Every map ships at every level of each quality axis
# (formats/qlevels.h) on the one CD image; the editions pick theirs.
#
# Env / local.env:
#   QUAKE_PAK0   required — Quake (id1) pak0.pak (episode 1, shareware)
#   QUAKE_PAK1   optional — the retail pak1.pak (episodes 2-4)
#   COOK_MAPS    maps to cook: a list ("start e1m1"), "ep1" or "all" (needs
#                QUAKE_PAK1). Unset: the maps already in cooked/id1 (so the
#                re-cook a build triggers keeps the disc's map set: CMake
#                globbed it at configure time), or ep1 if there are none.
#   LIGHT_GAIN   vertex light scale (default 1.5; 1 = Quake's own brightness,
#                dark on a TV)
#   QUAKE_MUSIC  a directory of trackNN.ogg / .mp3 / .wav (the 2021 rerelease:
#                <Quake>/rerelease/id1/music); default: next to pak0. They
#                become the disc's CD-DA tracks (ffmpeg converts them).
#   COOK_MUSIC   0 skips the music (default 1 when the directory exists)
#
# Output, per map, at each level N of an axis (0 low, 1 medium, 2 high,
# 3 ultra; formats/qlevels.h):
#   cooked/id1/<map>_t<N>.tex   QTX6 texture set (textures 0..3)  -> disc <MAP>_T<N>.TEX
#   cooked/id1/<map>_t<N>.tx4   QTX7, the same in 4bpp            -> disc <MAP>_T<N>.TX4
#   cooked/id1/<map>_g<N>.wld   QWVD world (geometry 0, 2, 3)     -> disc <MAP>_G<N>.WLD
#   cooked/id1/<map>_m<N>.mdl   QMM1 its own models (models 0..2) -> disc <MAP>_M<N>.MDL
# per map (with the high geometry job), the sound bank (formats/qsound.h):
#   cooked/id1/<map>.snd        QSB2 sounds                       -> disc <MAP>.SND
# once per model level, the game data (weapon models, projectiles, the
# HUD; formats/qgame.h):
#   cooked/id1/game_<N>.dat     QGM7                              -> disc GAME_<N>.DAT
# and once per model level, the streamed assets (the monsters, the items'
# models, the positional sounds; formats/qstream.h; the models reduced at
# that level: tools/qcook/game.c stream_reduce):
#   cooked/id1/stream_<N>.dat   QST1                              -> disc STREAM_<N>.DAT
# and the music, as 44.1 kHz stereo WAV for mkpsxiso's audio tracks:
#   cooked/music/trackNN.wav                              -> CD-DA track NN
# Jobs run in parallel; each one's log is shown only if it fails.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

[ -f local.env ] && set -a && . ./local.env && set +a
: "${QUAKE_PAK0:?set QUAKE_PAK0 in local.env}"

EP1="start e1m1 e1m2 e1m3 e1m4 e1m5 e1m6 e1m7 e1m8"
RETAIL="e2m1 e2m2 e2m3 e2m4 e2m5 e2m6 e2m7 e3m1 e3m2 e3m3 e3m4 e3m5 e3m6 e3m7
        e4m1 e4m2 e4m3 e4m4 e4m5 e4m6 e4m7 e4m8 end"
if [ -z "${COOK_MAPS:-}" ]; then
	COOK_MAPS=$( (ls cooked/id1/*_g2.wld cooked/id1/*_high.wld 2>/dev/null || true) |   # (_high: the
	            sed 's|.*/||; s|_g2\.wld$||; s|_high\.wld$||' | sort -u | tr '\n' ' ')   # fidelity's names)
	COOK_MAPS=${COOK_MAPS:-ep1}
fi
case "$COOK_MAPS" in
	ep1) MAPS=$EP1 ;;
	all) : "${QUAKE_PAK1:?COOK_MAPS=all needs QUAKE_PAK1 in local.env}"
	     MAPS="$EP1 $RETAIL" ;;
	*)   MAPS=$COOK_MAPS ;;
esac

make -s -C tools/qcook
# start clean: the disc carries every cooked file, so a map cooked earlier
# (another COOK_MAPS, an older format) must not linger
mkdir -p cooked/id1
rm -f cooked/id1/*.tex cooked/id1/*.tx4 cooked/id1/*.wld cooked/id1/*.mdl cooked/id1/*.snd \
      cooked/id1/game*.dat cooked/id1/stream*.dat
LOGS=$(mktemp -d)
trap 'rm -rf "$LOGS"' EXIT
tools/qcook/qcook game --pak "$QUAKE_PAK0" --out cooked/id1/game > "$LOGS/game.log" 2>&1 \
	|| { echo "cook FAILED: game data"; cat "$LOGS/game.log"; exit 1; }
# the streamed assets per model level (formats/qstream.h): the monsters, the
# items' models, the positional sounds
pids=""
for n in 0 1 2; do                               # the model levels: low, medium, high
	tools/qcook/qcook stream --pak "$QUAKE_PAK0" ${QUAKE_PAK1:+--pak1 "$QUAKE_PAK1"} --level $n \
	                         --out cooked/id1/stream_$n.dat > "$LOGS/stream_$n.log" 2>&1 &
	pids="$pids $!"
done
for p in $pids; do
	wait "$p" || { echo "cook FAILED: streamed assets"; cat "$LOGS"/stream_*.log; exit 1; }
done

cook_one() {   # map axis level (t / g / m, 0..3)
	local pak=$QUAKE_PAK0
	case $1 in e2*|e3*|e4*|end) pak=${QUAKE_PAK1:?$1 needs QUAKE_PAK1} ;; esac
	local name out="cooked/id1/$1_$2$3" what
	name=$(echo low medium high ultra | cut -d' ' -f$(($3 + 1)))
	case $2 in
	t) what="--textures $name --tex $out.tex --tex4 $out.tx4" ;;
	g) what="--geometry $name --world $out.wld"
	   [ "$3" = 2 ] && what="$what --sounds cooked/id1/$1.snd" ;;   # (level-independent: once)
	m) what="--models $name --mdl $out.mdl" ;;
	esac
	tools/qcook/qcook map --pak "$pak" --palette-pak "$QUAKE_PAK0" --map "maps/$1.bsp" $what \
	                      --light-gain "${LIGHT_GAIN:-1.5}" > "$LOGS/$1_$2$3.log" 2>&1 \
	|| { echo "cook FAILED: $1 $2$3"; cat "$LOGS/$1_$2$3.log"; return 1; }
}
export -f cook_one
export QUAKE_PAK0 QUAKE_PAK1 LOGS LIGHT_GAIN

for m in $MAPS; do
	for n in 0 1 2 3; do echo "$m t $n"; done     # textures: every level
	for n in 0 2 3;   do echo "$m g $n"; done     # geometry: low, high, ultra
	for n in 0 1 2;   do echo "$m m $n"; done     # models: low, medium, high
done | xargs -P "$(nproc)" -n 3 bash -c 'cook_one "$0" "$1" "$2"'

echo "cooked $(echo $MAPS | wc -w) maps: 4 texture, 3 geometry, 3 model levels -> $ROOT/cooked/"

# the music: Quake's CD tracks 2..11 (worldspawn "sounds"), converted once
QUAKE_MUSIC=${QUAKE_MUSIC:-"$(dirname "$QUAKE_PAK0")/../rerelease/id1/music"}
if [ "${COOK_MUSIC:-1}" != 0 ] && [ -d "$QUAKE_MUSIC" ] && command -v ffmpeg >/dev/null; then
	mkdir -p cooked/music
	n=0
	for src in "$QUAKE_MUSIC"/track*.ogg "$QUAKE_MUSIC"/track*.mp3 "$QUAKE_MUSIC"/track*.wav; do
		[ -f "$src" ] || continue
		out="cooked/music/$(basename "${src%.*}").wav"
		if [ ! -f "$out" ] || [ "$src" -nt "$out" ]; then
			ffmpeg -loglevel error -y -i "$src" -ar 44100 -ac 2 -sample_fmt s16 \
			       -fflags +bitexact -flags:a +bitexact "$out" || { echo "cook FAILED: music $src"; exit 1; }
		fi
		n=$((n + 1))
	done
	echo "music: $n CD-DA tracks in $ROOT/cooked/music/"
else
	rm -rf cooked/music
	echo "music: none (set QUAKE_MUSIC / COOK_MUSIC=1, needs ffmpeg)"
fi
