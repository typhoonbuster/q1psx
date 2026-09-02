#!/usr/bin/env bash
#
# Build the asset cooker and cook the Quake data into ./cooked/.
#
# One run produces every fidelity level for every game you own — that whole
# tree goes on the single CD image (see iso/iso.xml). Never committed.
#
# Env / local.env:
#   QUAKE_PAK0   required — Quake (id1) pak0.pak
#   QUAKE_PAK1   optional — id1 pak1.pak (episodes 2-4)
#   HIPNOTIC_PAK optional — Scourge of Armagon
#   ROGUE_PAK    optional — Dissolution of Eternity

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

[ -f local.env ] && set -a && . ./local.env && set +a
: "${QUAKE_PAK0:?set QUAKE_PAK0 in local.env}"

make -C tools/qcook >/dev/null
QCOOK=tools/qcook/qcook

FIDELITIES=(low medium high ultra)

# Phase 4.1: textures for one map at every fidelity. Grows to all maps / games.
cook_game() {
	local game=$1 pak=$2
	for fid in "${FIDELITIES[@]}"; do
		local out="cooked/$game/$fid"
		mkdir -p "$out"
		"$QCOOK" textures --pak "$pak" --map maps/start.bsp \
		         --fidelity "$fid" --out "$out/start.qtx"
	done
	# geometry / surfaces / BSP don't vary by fidelity yet (decimation is later)
	mkdir -p "cooked/$game/common"
	"$QCOOK" geometry --pak "$pak" --map maps/start.bsp \
	         --out "cooked/$game/common/start.qgeo"
	"$QCOOK" surfaces --pak "$pak" --map maps/start.bsp \
	         --out "cooked/$game/common/start.qsrf"
	"$QCOOK" bsp --pak "$pak" --map maps/start.bsp \
	         --out "cooked/$game/common/start.qbsp"
}

cook_game id1 "$QUAKE_PAK0"

echo
du -sh cooked/* 2>/dev/null || true
echo "cooked -> $ROOT/cooked/"
