#!/bin/sh
#
# Check that a linked q1psx executable leaves its stack room in main RAM.
#
# Usage:  scripts/ramcheck.sh <nm> <elf> <stack-bytes>
#
# The kernel starts the stack at 0x801FFFF0 (the EXE header's) and it grows
# down towards the end of the executable's BSS (_end): nothing else marks
# the boundary, and the SDK's heap sits in the same gap. On 2026-10-18 the
# code and the arenas grew past it (the monsters, the streaming, the brush
# models' pieces: _end at 0x80200570, beyond the 2 MB): the stack ran into
# the BSS and the BSS's top, mirrored, into the kernel's RAM. Run after
# every link (CMakeLists.txt); exit status 1 under <stack-bytes>.

set -eu

NM=$1
ELF=$2
NEED=$3
STACK_TOP=$((0x801FFFF0))

end=$("$NM" "$ELF" | awk '/ _end$/ { print $1 }')
if [ -z "$end" ]; then
	echo "ramcheck: no _end in $ELF" >&2
	exit 1
fi
end=$(printf '%s' "$end" | sed 's/^ffffffff//')
room=$((STACK_TOP - 0x$end))
if [ "$room" -lt "$NEED" ]; then
	echo "ramcheck: _end 0x$end leaves $room bytes for the stack, under $NEED: shrink an arena (engine/mem.h)" >&2
	exit 1
fi
echo "ramcheck: _end 0x$end, $room bytes for the stack"
