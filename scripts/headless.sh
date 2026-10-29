#!/usr/bin/env bash
#
# Boot a q1psx CD image in PCSX-Redux with no UI and capture its console.
#
# Usage:  scripts/headless.sh [image.bin] [max-seconds] [stop-regex]
#
# Runs until `stop-regex` appears in the console output, a CPU exception is
# reported (PCSX-Redux's interpreter: "First chance exception", "Unaligned
# address": a misaligned load the dynarec does not check), the console stops
# growing for STALL seconds (default 30: in fast forward the engine prints
# every second or so, a map's load takes a few; a silent run is the
# emulator stalled, or the engine looping: exit status 3, at once, not at
# `max-seconds`), or `max-seconds` elapse (default 20), then kills the
# emulator.
# The full log is written to $LOG (default: build/headless.log) and its tail
# is printed. Exit status: 0 = stop pattern seen (or no pattern given and the
# run timed out cleanly), 1 = exception / crash marker found, 2 = timed out
# waiting for the stop pattern, 3 = the console stalled (STALL=0: no such
# check).
#
# EMU_ARGS adds emulator flags, e.g. EMU_ARGS=-interpreter (PCSX-Redux's
# dynarec has miscomputed code here that the interpreter runs correctly).
#
# Note: -no-ui has no GPU backend — this checks that code runs and measures
# CPU cost; it cannot show what is drawn.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMG=${1:-"$ROOT/build/q1psx.bin"}
SECS=${2:-20}
STOP=${3:-}
EMU=${EMU:-$(sed -n 's/^[[:space:]]*EMU[[:space:]]*=[[:space:]]*//p' "$ROOT/local.env" 2>/dev/null | tail -n 1 || true)}
: "${EMU:?set EMU (the PCSX-Redux AppImage) in local.env or the environment}"
LOG=${LOG:-"$ROOT/build/headless.log"}
STALL=${STALL:-30}
CRASH='break 0x|Unrecoverable|Exception|First chance exception|Unaligned address|PANIC|Sys_Error|panic|Program counter|=== halting ==='

mkdir -p "$(dirname "$LOG")"
# stdout to a file is block-buffered by the emulator; force line buffering so
# the stop pattern is seen while it runs.
start_emu() {
	: > "$LOG"
	stdbuf -oL -eL "$EMU" -no-ui -stdout ${EMU_ARGS:-} -loadiso "$IMG" -run > "$LOG" 2>&1 &
	PID=$!
}
start_emu

# The AppImage sometimes finds no BIOS at all (its own OpenBIOS fallback
# unreadable: "No BIOS loaded, emulation halted") and then waits forever:
# a run that says so is started again, three times at most.
NOBIOS='No BIOS loaded'
retries=3
status=0
[ -n "$STOP" ] && status=2
last_size=0
quiet=0
for _ in $(seq 1 "$SECS"); do
	sleep 1
	if [ -n "$STOP" ] && grep -qE "$STOP" "$LOG"; then status=0; break; fi
	if grep -qE "$CRASH" "$LOG"; then status=1; break; fi
	size=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
	if [ "$size" != "$last_size" ]; then last_size=$size; quiet=0; else quiet=$((quiet + 1)); fi
	if [ "$STALL" -gt 0 ] && [ "$quiet" -ge "$STALL" ]; then status=3; break; fi
	if [ "$retries" -gt 0 ] && grep -q "$NOBIOS" "$LOG"; then
		kill "$PID" 2>/dev/null
		wait "$PID" 2>/dev/null
		retries=$((retries - 1))
		echo "headless: no BIOS loaded, the emulator started again" >&2
		sleep 2
		start_emu
		continue
	fi
	kill -0 "$PID" 2>/dev/null || break
done

kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
grep -qE "$CRASH" "$LOG" && status=1

tail -n 30 "$LOG"
case $status in
	1) echo "headless: CRASH marker found (full log: $LOG)" ;;
	2) echo "headless: timed out after ${SECS}s waiting for /$STOP/" ;;
	3) echo "headless: no console output for ${STALL}s: the emulator stalled, or the engine loops (full log: $LOG)" ;;
esac
exit $status
