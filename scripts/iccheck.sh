#!/bin/sh
#
# The instruction cache's chains (engine/r_defs.inc ICSEC, docs/porting-notes.md
# §81.5): the link may put each .text.ic.* section anywhere (engine/q1psx.ld,
# engine/icache.ld), so none may fall through to the next one: every such
# section must end with an unconditional jump, branch or return and its delay
# slot. Fails the build otherwise, naming the section and its last two
# instructions.
#
# Usage: scripts/iccheck.sh <objdump> <object>...

OBJDUMP=$1
shift
status=0
for obj in "$@"; do
	"$OBJDUMP" -h "$obj" 2>/dev/null | grep -q ' \.text\.ic\.' || continue
	"$OBJDUMP" -d -z --no-show-raw-insn "$obj" | awk -v obj="$obj" '
		function done() {
			if (sec ~ /^\.text\.ic\./ && n > 0 && !(op2 == "j" || op2 == "jr" || op2 == "b")) {
				printf "iccheck: %s: %s falls through (its last two: %s / %s)\n", obj, sec, line2, line1
				bad = 1
			}
		}
		/^Disassembly of section / { done(); sec = $4; sub(/:$/, "", sec); n = 0; op1 = op2 = line1 = line2 = ""; next }
		/^ *[0-9a-f]+:\t/ {
			split($0, f, "\t")
			op2 = op1; line2 = line1
			op1 = f[2]; line1 = f[2] " " f[3]
			n++
		}
		END { done(); exit bad }' || status=1
done
exit $status
