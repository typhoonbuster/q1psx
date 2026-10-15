#!/usr/bin/env bash
#
# Build the asset cooker and cook the Quake data into ./cooked/ (never
# committed). Every map ships at every fidelity level on the one CD image.
#
# Env / local.env:
#   QUAKE_PAK0   required — Quake (id1) pak0.pak (episode 1, shareware)
#   QUAKE_PAK1   optional — the retail pak1.pak (episodes 2-4)
#   COOK_MAPS    maps to cook: a list ("start e1m1"), "ep1" (default) or
#                "all" (needs QUAKE_PAK1)
#
# Output, per map and fidelity (low medium high ultra = disc N 0..3):
#   cooked/id1/<map>_<fidelity>.tex   QTX3 texture pages  -> disc <MAP>_<N>.TEX
#   cooked/id1/<map>_<fidelity>.wld   QWV2 world          -> disc <MAP>_<N>.WLD
# Jobs run in parallel; each one's log is shown only if it fails.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

[ -f local.env ] && set -a && . ./local.env && set +a
: "${QUAKE_PAK0:?set QUAKE_PAK0 in local.env}"

EP1="start e1m1 e1m2 e1m3 e1m4 e1m5 e1m6 e1m7 e1m8"
RETAIL="e2m1 e2m2 e2m3 e2m4 e2m5 e2m6 e2m7 e3m1 e3m2 e3m3 e3m4 e3m5 e3m6 e3m7
        e4m1 e4m2 e4m3 e4m4 e4m5 e4m6 e4m7 e4m8 end"
case "${COOK_MAPS:-ep1}" in
	ep1) MAPS=$EP1 ;;
	all) : "${QUAKE_PAK1:?COOK_MAPS=all needs QUAKE_PAK1 in local.env}"
	     MAPS="$EP1 $RETAIL" ;;
	*)   MAPS=$COOK_MAPS ;;
esac

make -s -C tools/qcook
# start clean: the disc carries every cooked file, so a map cooked earlier
# (another COOK_MAPS, an older format) must not linger
mkdir -p cooked/id1
rm -f cooked/id1/*.tex cooked/id1/*.wld
LOGS=$(mktemp -d)
trap 'rm -rf "$LOGS"' EXIT

cook_one() {   # map fidelity
	local pak=$QUAKE_PAK0
	case $1 in e2*|e3*|e4*|end) pak=${QUAKE_PAK1:?$1 needs QUAKE_PAK1} ;; esac
	tools/qcook/qcook map --pak "$pak" --palette-pak "$QUAKE_PAK0" --map "maps/$1.bsp" \
	                      --fidelity "$2" --tex "cooked/id1/$1_$2.tex" --world "cooked/id1/$1_$2.wld" \
	                      > "$LOGS/$1_$2.log" 2>&1 \
	|| { echo "cook FAILED: $1 $2"; cat "$LOGS/$1_$2.log"; return 1; }
}
export -f cook_one
export QUAKE_PAK0 QUAKE_PAK1 LOGS

for m in $MAPS; do for f in low medium high ultra; do echo "$m $f"; done; done \
	| xargs -P "$(nproc)" -n 2 bash -c 'cook_one "$0" "$1"'

echo "cooked $(echo $MAPS | wc -w) maps x 4 fidelity levels -> $ROOT/cooked/"
