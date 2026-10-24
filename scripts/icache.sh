#!/usr/bin/env bash
#
# The instruction cache's layout (docs/porting-notes.md §81.5): trace the
# bench's frames instruction by instruction, then order the hot code for the
# 4 KB cache (tools/icache) into engine/icache.ld.
#
# Usage:  scripts/icache.sh trace <tag> <frames> <views> [VAR=value ...]
#             builds the bench with the given settings (scripts/bench.sh's:
#             BENCH_MAPS=1, BENCH_GAME=1...; HASHONLY=1 PROFILE=0 always),
#             runs it under PCSX-Redux's interpreter with its debugger, a Lua
#             breakpoint on every fetch in the frames numbered <frames>
#             (comma separated, counted from each bench_reset: the default
#             bench's measured ones start at 9, the maps' and the game's at
#             5) from prof_frame_reset to vid_flip, <views> views (0: up to
#             "bench done"). Leaves build-bench/icache/<tag>.trace and the
#             build's .elf beside it. A few minutes a set (the 128 views:
#             ~15).
#         scripts/icache.sh self <tag>:<frame>
#             the words a trace's own build fills (its .elf and .ldmap).
#         scripts/icache.sh sim  [<tag>:<frame>[:<weight>] ...]
#         scripts/icache.sh opt  [-n iters] [-s seed] [<tag>:<frame>[:<weight>] ...]
#             the words the cache fills as the release links now (sim), or
#             the search for a better order (opt: engine/icache.ld written);
#             the reference is build-release (configure and build it first,
#             cmake --preset release). Default traces: s:10 m:6 g:6:2 (the
#             default bench's views, the 128 spawn views, the game twice);
#             opt keeps the hot list to the sections the bench's link (the
#             first trace's) and the release's hold alike, C included.
#
# The usual round:
#   scripts/icache.sh trace s 9,10 12
#   scripts/icache.sh trace m 5,6 128 BENCH_MAPS=1
#   scripts/icache.sh trace g 5,6 0 BENCH_GAME=1 STACK_MIN=4096
#   cmake --build build-release && scripts/icache.sh opt -n 20000
#   cmake --build build-release      (the new order linked; check the frames)

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR="$ROOT/build-bench/icache"
EMU=${EMU:-$(sed -n 's/^[[:space:]]*EMU[[:space:]]*=[[:space:]]*//p' "$ROOT/local.env" 2>/dev/null | tail -n 1 || true)}
: "${EMU:?set EMU (the PCSX-Redux AppImage) in local.env or the environment}"
NM=${NM:-mipsel-none-elf-nm}
cmd=${1:?usage: icache.sh trace|self|sim|opt ...}
shift
mkdir -p "$DIR"

case $cmd in
trace)
	tag=${1:?tag}; frames=${2:?frames}; views=${3:?views}; shift 3
	# the build alone (the run fails at once: no emulator)
	env "$@" HASHONLY=1 PROFILE=0 EMU=/bin/true "$ROOT/scripts/bench.sh" 1 > /dev/null 2>&1 || true
	[ -f "$ROOT/build-bench/q1psx.elf" ] || { echo "icache: the bench did not build"; exit 1; }
	cp "$ROOT/build-bench/q1psx.elf" "$DIR/$tag.elf"
	cp "$ROOT/build-bench/q1psx.ldmap" "$DIR/$tag.ldmap"   # (sim against the traced build itself)
	mkdir -p "$DIR/$tag.disc"
	cp "$ROOT/build-bench/q1psx.bin" "$DIR/$tag.disc/q1psx.bin"
	sed 's#FILE "[^"]*"#FILE "q1psx.bin"#' "$ROOT/build-bench/q1psx.cue" > "$DIR/$tag.disc/q1psx.cue"
	addr() { "$NM" "$DIR/$tag.elf" | awk -v s="$1" '$3 == s { print "0x" substr($1, length($1) - 7) }'; }
	reset=$(addr bench_reset); frame=$(addr prof_frame_reset); flip=$(addr vid_flip)
	last=$(echo "$frames" | tr ',' '\n' | sort -n | tail -1)
	sets=$(echo "$frames" | sed -E 's/([0-9]+)/[\1] = true/g')
	cat > "$DIR/$tag.lua" <<EOF
-- every instruction fetched in the bench's frames { $frames } (scripts/icache.sh)
local TRACE, LAST, VIEWS = { $sets }, $last, $views
local f = assert(io.open("$DIR/$tag.trace", "w"))
local view, frame, tracing = 0, 0, false
local runs, n, rs, rc, nextpc, lastpc = {}, 0, 0, 0, -1, -1
local function fetch(addr)
  if addr == lastpc then return end
  lastpc = addr
  if addr == nextpc then rc = rc + 1 else
    if rc > 0 then n = n + 1; runs[n] = rs; n = n + 1; runs[n] = rc end
    rs = addr; rc = 1
  end
  nextpc = addr + 4
end
trace0 = PCSX.addBreakpoint(0x80000000, 'Exec', 0x200000, 'icache kseg0', fetch)
trace1 = PCSX.addBreakpoint(0x00000000, 'Exec', 0x200000, 'icache kuseg', fetch)
trace0:disable(); trace1:disable()
bpv = PCSX.addBreakpoint($reset, 'Exec', 4, 'view', function() view = view + 1; frame = 0 end)
bpf = PCSX.addBreakpoint($frame, 'Exec', 4, 'frame', function()
  if view == 0 then return end
  frame = frame + 1
  if TRACE[frame] then
    runs, n, rs, rc, nextpc, lastpc = {}, 0, 0, 0, -1, -1
    tracing = true
    trace0:enable(); trace1:enable()
  end
end)
bpe = PCSX.addBreakpoint($flip, 'Exec', 4, 'flip', function()
  if not tracing then return end
  trace0:disable(); trace1:disable(); tracing = false
  if rc > 0 then n = n + 1; runs[n] = rs; n = n + 1; runs[n] = rc end
  f:write(string.format("F %d %d %d\n", view, frame, n / 2))
  local parts = {}
  for i = 1, n, 2 do parts[#parts + 1] = string.format("%x %d", runs[i], runs[i + 1]) end
  f:write(table.concat(parts, "\n"), "\n")
  f:flush()
  if VIEWS > 0 and view >= VIEWS and frame >= LAST then
    f:write("END\n"); f:close()
    print("icache trace done")
  end
end)
EOF
	rm -f "$DIR/$tag.trace" "$DIR/$tag.log"
	"$EMU" -no-ui -stdout -interpreter -debugger -dofile "$DIR/$tag.lua" -loadiso "$DIR/$tag.disc/q1psx.bin" -run \
		> "$DIR/$tag.log" 2>&1 &
	pid=$!
	t0=$(date +%s)
	until grep -q -E 'icache trace done|bench done|First chance exception|rror' "$DIR/$tag.log" 2>/dev/null ||
	      ! kill -0 $pid 2>/dev/null || [ $(( $(date +%s) - t0 )) -gt "${SECS:-3600}" ]; do
		sleep 2
	done
	kill $pid 2>/dev/null || true
	wait $pid 2>/dev/null || true
	echo "icache: $tag: $(grep -c '^F' "$DIR/$tag.trace" || true) frames traced in $(( $(date +%s) - t0 )) s"
	grep -E 'First chance exception|rror' "$DIR/$tag.log" | head -3 || true
	;;
self)
	make -s -C "$ROOT/tools/icache"
	s=${1:?<tag>:<frame>}; tag=${s%%:*}
	"$ROOT/tools/icache/icache" sim "$DIR/$tag.elf" "$DIR/$tag.ldmap" "$DIR/$tag.trace:$DIR/$tag.elf:${s#*:}"
	;;
sim|opt)
	make -s -C "$ROOT/tools/icache"
	ref="$ROOT/build-release"
	[ -f "$ref/q1psx.ldmap" ] || { echo "icache: build the release first (build-release/q1psx.ldmap)"; exit 1; }
	opts=()
	while [ $# -gt 0 ] && [ "${1#-}" != "$1" ]; do opts+=("$1" "$2"); shift 2; done
	specs=("$@")
	[ ${#specs[@]} -gt 0 ] || specs=(s:10 m:6 g:6:2)
	traces=()
	for s in "${specs[@]}"; do
		tag=${s%%:*}; rest=${s#*:}
		[ -f "$DIR/$tag.trace" ] || { echo "icache: no trace $tag (scripts/icache.sh trace $tag ...)"; exit 1; }
		traces+=("$DIR/$tag.trace:$DIR/$tag.elf:$rest")
	done
	if [ "$cmd" = sim ]; then
		"$ROOT/tools/icache/icache" sim "$ref/q1psx.elf" "$ref/q1psx.ldmap" "${traces[@]}"
	else
		# (the C both links hold alike, -c -m: the first trace's bench link beside the release's)
		first=${specs[0]%%:*}
		"$ROOT/tools/icache/icache" opt "$ref/q1psx.elf" "$ref/q1psx.ldmap" "$ROOT/engine/icache.ld" \
			-k "$ROOT/engine/icache.ld" -c -m "$DIR/$first.ldmap" "${opts[@]}" "${traces[@]}"
	fi
	;;
*)
	echo "usage: icache.sh trace|self|sim|opt ..."; exit 2 ;;
esac
