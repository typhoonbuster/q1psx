#!/bin/sh
#
# Check that a linked q1psx executable leaves the level arena its least size
# in main RAM, under the stack's room.
#
# Usage:  scripts/ramcheck.sh <nm> <elf> <stack-bytes> <level-min-bytes>
#
# The kernel starts the stack at 0x801FFFF0 (the EXE header's). The stack's
# room is its top <stack-bytes> (engine/mem.h MEM_STACK_BYTES); the level
# arena takes every byte between the executable's BSS (_end) and that room
# (engine/mem.c mem_init, docs/porting-notes.md §81.7). Nothing else marks
# the boundary: on 2026-10-18 the code and the arenas grew past the 2 MB
# (_end at 0x80200570) and the stack ran into the BSS. Run after every link
# (CMakeLists.txt); exit status 1 when the arena comes under
# <level-min-bytes>.

set -eu

NM=$1
ELF=$2
STACK=$3
NEED=$4
STACK_TOP=$((0x801FFFF0))

end=$("$NM" "$ELF" | awk '/ _end$/ { print $1 }')
if [ -z "$end" ]; then
	echo "ramcheck: no _end in $ELF" >&2
	exit 1
fi
end=$(printf '%s' "$end" | sed 's/^ffffffff//')
base=$(( (0x$end + 15) & ~15 ))
arena=$(( ((STACK_TOP - STACK) & ~15) - base ))
if [ "$arena" -lt "$NEED" ]; then
	echo "ramcheck: _end 0x$end leaves the level arena $arena bytes under a $STACK-byte stack, under $NEED: less code or BSS" >&2
	exit 1
fi
echo "ramcheck: _end 0x$end, the level arena $arena bytes ($((arena / 1024)) KB), the stack $STACK"
