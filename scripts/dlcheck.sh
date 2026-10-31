#!/usr/bin/env bash
#
# Regression check for renderer optimizations: run the bench, extract the
# dumped display lists (packet payloads in drawing order, no addresses) and
# compare them with a saved reference. An optimization that changes no
# pixel must leave them identical word for word.
#
# Usage:  scripts/dlcheck.sh save   <name>   # record a reference
#         scripts/dlcheck.sh check  <name>   # compare against it
#
# The bench's settings pass through (T, PROFILE...). With
# BENCH_MAPS=1 it covers the 128 spawn views of every map on the disc, by
# display-list hash only (give it SECS=3000).
#
# References live in build-bench/dlref/<name>.{dl,bench}. Prints the frame
# cycles of every view, the reference's next to them, and the verdict.
# ENGINE_C=1 checks the C engine (built in build-bench-c/) against a
# reference the asm saved: the same frames, every view's cycles asm to C.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:?usage: dlcheck.sh save|check <name>}
NAME=${2:?usage: dlcheck.sh save|check <name>}
REF="$ROOT/build-bench/dlref"
OUT="$ROOT/build-bench$([ "${ENGINE_C:-0}" = 1 ] && echo -c || true)"   # (bench.sh's)
mkdir -p "$REF"

"$ROOT/scripts/bench.sh" "${SECS:-500}" > /dev/null 2>&1 || {
	echo "dlcheck: bench failed (see $OUT/bench.log)"; exit 1; }
LOG="$OUT/bench.log"
grep -E '^(P|E|D|H) ' "$LOG" > "$REF/current.dl"
{ grep -E '^bench (view[0-9]+|[A-Z0-9]+\.v[0-9]) ' "$LOG" || true; } |
	sed -E 's/^bench ([^ ]+) .* frame=([0-9]+)\/.*/\1 \2/' > "$REF/current.bench"   # (the game bench: none)

if [ "$MODE" = save ]; then
	cp "$REF/current.dl" "$REF/$NAME.dl"
	cp "$REF/current.bench" "$REF/$NAME.bench"
	echo "dlcheck: saved reference '$NAME' ($(wc -l < "$REF/$NAME.dl") lines)"
	cat "$REF/$NAME.bench"
	exit 0
fi

awk 'NR == FNR { ref[$1] = $2; next }
	($1 in ref) { r = ref[$1]; d = $2 - r; tot0 += r; tot1 += $2; if (r > max0) max0 = r; if ($2 > max1) max1 = $2
	  printf "%-9s %7d -> %7d  %+6d (%+.1f%%)\n", $1, r, $2, d, 100.0 * d / r }
	END { if (tot0) printf "total     %7d -> %7d  %+.1f%%   worst %d -> %d\n", tot0, tot1, 100.0 * (tot1 - tot0) / tot0, max0, max1 }' \
	"$REF/$NAME.bench" "$REF/current.bench"
if cmp -s "$REF/$NAME.dl" "$REF/current.dl"; then
	echo "dlcheck: display lists IDENTICAL to '$NAME'"
else
	echo "dlcheck: display lists DIFFER from '$NAME'"
	diff "$REF/$NAME.dl" "$REF/current.dl" | head -10
	exit 2
fi
