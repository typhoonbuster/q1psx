#!/usr/bin/env bash
#
# Build the asset cooker and cook the Quake data into ./cooked/ (never
# committed). Every fidelity level goes on the one CD image.
#
# Env / local.env:
#   QUAKE_PAK0   required — Quake (id1) pak0.pak
#
# Output, per map and fidelity N (0 low, 1 medium, 2 high, 3 ultra):
#   cooked/id1/<map>_<fidelity>.tex   QTX3 texture pages  -> disc START<N>.TEX
#   cooked/id1/<map>_<fidelity>.wld   QWV2 world          -> disc START<N>.WLD

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

[ -f local.env ] && set -a && . ./local.env && set +a
: "${QUAKE_PAK0:?set QUAKE_PAK0 in local.env}"

make -s -C tools/qcook
QCOOK=tools/qcook/qcook

mkdir -p cooked/id1
for fid in low medium high ultra; do
	"$QCOOK" map --pak "$QUAKE_PAK0" --map maps/start.bsp --fidelity "$fid" \
	         --tex "cooked/id1/start_$fid.tex" --world "cooked/id1/start_$fid.wld"
done

echo "cooked -> $ROOT/cooked/"
