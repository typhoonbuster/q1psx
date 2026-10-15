#!/usr/bin/env bash
#
# Regression check for renderer optimizations: run the bench, extract the
# dumped display lists (packet payloads in drawing order, no addresses) and
# compare them with a saved reference. An optimization that changes no
# pixel must leave them identical word for word.
#
# Usage:  [FID=0..3] scripts/dlcheck.sh save   <name>   # record a reference
#         [FID=0..3] scripts/dlcheck.sh check  <name>   # compare against it
#
# References live in build-bench/dlref/<name>.{dl,bench}. Prints the frame
# cycles of every view, the reference's next to them, and the verdict.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:?usage: dlcheck.sh save|check <name>}
NAME=${2:?usage: dlcheck.sh save|check <name>}
REF="$ROOT/build-bench/dlref"
mkdir -p "$REF"

"$ROOT/scripts/bench.sh" "${SECS:-500}" > /dev/null 2>&1 || {
	echo "dlcheck: bench failed (see build-bench/bench.log)"; exit 1; }
LOG="$ROOT/build-bench/bench.log"
grep -E '^(P|E|D|H) ' "$LOG" > "$REF/current.dl"
grep -E '^bench view' "$LOG" | sed -E 's/^bench (view[0-9]+) .* frame=([0-9]+)\/.*/\1 \2/' > "$REF/current.bench"

if [ "$MODE" = save ]; then
	cp "$REF/current.dl" "$REF/$NAME.dl"
	cp "$REF/current.bench" "$REF/$NAME.bench"
	echo "dlcheck: saved reference '$NAME' ($(wc -l < "$REF/$NAME.dl") lines)"
	cat "$REF/$NAME.bench"
	exit 0
fi

join "$REF/$NAME.bench" "$REF/current.bench" | awk '
	{ d = $3 - $2; tot0 += $2; tot1 += $3; if ($2 > max0) max0 = $2; if ($3 > max1) max1 = $3
	  printf "%-7s %7d -> %7d  %+6d (%+.1f%%)\n", $1, $2, $3, d, 100.0 * d / $2 }
	END { printf "total   %7d -> %7d  %+.1f%%   worst %d -> %d\n", tot0, tot1, 100.0 * (tot1 - tot0) / tot0, max0, max1 }'
if cmp -s "$REF/$NAME.dl" "$REF/current.dl"; then
	echo "dlcheck: display lists IDENTICAL to '$NAME'"
else
	echo "dlcheck: display lists DIFFER from '$NAME'"
	diff "$REF/$NAME.dl" "$REF/current.dl" | head -10
	exit 2
fi
