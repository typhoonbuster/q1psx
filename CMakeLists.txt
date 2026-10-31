# q1psx — Quake → PlayStation 1
# Build: cmake --preset default . && cmake --build ./build
# Requires PSn00bSDK v0.24 on PATH + PSN00BSDK_LIBS set (see third_party/README.md).

cmake_minimum_required(VERSION 3.21)

project(
	q1psx
	LANGUAGES    C ASM
	VERSION      0.0.1
	DESCRIPTION  "Quake port for the Sony PlayStation (from scratch)"
	HOMEPAGE_URL "https://github.com/typhoonbuster/q1psx"
)

# --- PS1 executable -----------------------------------------------------------
file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS engine/*.c engine/*.s)

# The physics' C reference is the specification of its asm port, which the
# game links (engine/clip.s, engine/pmove.s). The bench links the C one too,
# renamed, and compares the two call by call (see run_bench in
# engine/main.c); tools/pmsim runs it on the host.
set(PHYS_C_REFERENCE ${PROJECT_SOURCE_DIR}/engine/clip.c ${PROJECT_SOURCE_DIR}/engine/pmove.c)
list(REMOVE_ITEM ENGINE_SOURCES ${PHYS_C_REFERENCE})

# The C engine, to measure what the hand-written asm gains: every asm part
# that has a C version builds that in its place, each following its asm,
# the base (docs/porting-notes.md §55): the physics (engine/clip.c,
# pmove.c), the slow path (r_slow.c), the dynamic lights (dlight.c), the
# brush models' cut (bmfrag.c), the places among a leaf's pieces (rlist.c
# rl_order_c), an entity's matrices (rlist.c), a model's frame (mdl.c), a
# point's leaf (world.c world_point_leaf_from_c), the triggers' touch walk
# (ents.c touch_gather_c), the render lists' cull (ents.c ents_seen_c) and
# the edicts' tick walk (ents.c ents_tick_loop).
# The world renderer (engine/r_world.s) has no C, written in asm from the
# start: it stays. The frames are the asm's, hash for hash (scripts/bench.sh
# ENGINE_C=1).
option(Q1PSX_ENGINE_C "The engine's C versions in place of its asm (the world renderer stays asm)" OFF)
if(Q1PSX_ENGINE_C)
	list(REMOVE_ITEM ENGINE_SOURCES
		${PROJECT_SOURCE_DIR}/engine/clip.s ${PROJECT_SOURCE_DIR}/engine/pmove.s
		${PROJECT_SOURCE_DIR}/engine/r_slow.s ${PROJECT_SOURCE_DIR}/engine/bmfrag.s
		${PROJECT_SOURCE_DIR}/engine/rl_order.s ${PROJECT_SOURCE_DIR}/engine/ent_mat.s
		${PROJECT_SOURCE_DIR}/engine/mdl_expand.s ${PROJECT_SOURCE_DIR}/engine/w_leaf.s
		${PROJECT_SOURCE_DIR}/engine/e_touch.s ${PROJECT_SOURCE_DIR}/engine/e_seen.s
		${PROJECT_SOURCE_DIR}/engine/e_tick.s ${PROJECT_SOURCE_DIR}/engine/mdl_frame.s
		${PROJECT_SOURCE_DIR}/engine/r_tess.s)
	list(APPEND ENGINE_SOURCES ${PHYS_C_REFERENCE})
	message(STATUS "q1psx: the C engine (the world renderer asm)")
endif()

psn00bsdk_add_executable(q1psx GPREL ${ENGINE_SOURCES})

# The link's script: the SDK's exe.ld with the instruction cache's hot code
# first (engine/q1psx.ld, its order in engine/icache.ld; docs/porting-notes.md
# §81.5), in place of the SDK's -T option.
get_target_property(_q1psx_link_options q1psx LINK_OPTIONS)
list(FILTER _q1psx_link_options EXCLUDE REGEX "exe\\.ld")
set_target_properties(q1psx PROPERTIES LINK_OPTIONS "${_q1psx_link_options}"
	LINK_DEPENDS "${PROJECT_SOURCE_DIR}/engine/q1psx.ld;${PROJECT_SOURCE_DIR}/engine/icache.ld")
target_link_options(q1psx PRIVATE -T${PROJECT_SOURCE_DIR}/engine/q1psx.ld -L${PROJECT_SOURCE_DIR}/engine
	-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/q1psx.ldmap)   # (the input sections' places: tools/icache)
set_property(TARGET q1psx APPEND PROPERTY LINK_DEPENDS "${PROJECT_SOURCE_DIR}/engine/load.ld")

# CMake does not scan `.include` in assembly sources: make every asm file
# depend on the .inc definition files, or editing an offset there leaves
# stale objects behind (it did: pmove.s kept an old scratchpad map).
file(GLOB ENGINE_ASM_SOURCES CONFIGURE_DEPENDS engine/*.s)
file(GLOB ENGINE_ASM_INCLUDES CONFIGURE_DEPENDS engine/*.inc)
set_source_files_properties(${ENGINE_ASM_SOURCES} PROPERTIES OBJECT_DEPENDS "${ENGINE_ASM_INCLUDES}")
target_include_directories(q1psx PRIVATE engine formats)

# The engine maps cooked structs straight onto byte blobs — the same idiom
# Quake itself relies on. Turn off the two -O2 assumptions that idiom
# violates: type-based alias analysis, and "signed arithmetic never
# overflows".
target_compile_options(q1psx PRIVATE -fno-strict-aliasing -fwrapv)

# Pipeline-hazard check of every linked executable (tools/hazcheck). The asm
# renderer is written with .set noreorder, so nothing else guards its load
# delay slots / GTE / HI-LO timing — and PCSX-Redux's dynarec hides such
# mistakes that real hardware (and the emulator's interpreter) would not.
find_program(Q1PSX_OBJDUMP NAMES mipsel-none-elf-objdump REQUIRED)
add_custom_command(TARGET q1psx POST_BUILD
	COMMAND make -s -C ${PROJECT_SOURCE_DIR}/tools/hazcheck
	COMMAND sh -c "${Q1PSX_OBJDUMP} -d -z --no-show-raw-insn $<TARGET_FILE:q1psx> | ${PROJECT_SOURCE_DIR}/tools/hazcheck/hazcheck"
	COMMENT "Checking pipeline hazards (tools/hazcheck)"
	VERBATIM)

# The instruction cache's chains (engine/r_defs.inc ICSEC): none may fall
# through, the link moving them (scripts/iccheck.sh, docs/porting-notes.md §81.5).
add_custom_command(TARGET q1psx POST_BUILD
	COMMAND sh ${PROJECT_SOURCE_DIR}/scripts/iccheck.sh ${Q1PSX_OBJDUMP} $<TARGET_OBJECTS:q1psx>
	COMMENT "Checking the instruction cache's chains (scripts/iccheck.sh)"
	COMMAND_EXPAND_LISTS
	VERBATIM)

# The RAM's layout (engine/mem.h, docs/porting-notes.md §81.7): the stack's
# room at the top of the 2 MB, Q1PSX_STACK_BYTES (8 KB: the boot log's
# "stack:" line says what a map's load reached, a guard stops the game short
# of it), the level arena everything between the BSS and it. The build fails
# when that arena comes under Q1PSX_LEVEL_MIN bytes (scripts/ramcheck.sh).
find_program(Q1PSX_NM NAMES mipsel-none-elf-nm REQUIRED)
set(Q1PSX_STACK_BYTES 8192 CACHE STRING "The stack's room at the top of RAM, bytes (the level arena takes the rest)")
set(Q1PSX_LEVEL_MIN 1272832 CACHE STRING "Least level arena the link accepts, bytes (1243 KB: its size until §81.7)")
set(Q1PSX_LEVEL_CAP 0 CACHE STRING "The level arena capped at that many bytes (0: none; a bench matching the release's)")
target_compile_definitions(q1psx PRIVATE Q1PSX_STACK_BYTES=${Q1PSX_STACK_BYTES} Q1PSX_LEVEL_CAP=${Q1PSX_LEVEL_CAP})
add_custom_command(TARGET q1psx POST_BUILD
	COMMAND sh ${PROJECT_SOURCE_DIR}/scripts/ramcheck.sh ${Q1PSX_NM} $<TARGET_FILE:q1psx> ${Q1PSX_STACK_BYTES} ${Q1PSX_LEVEL_MIN}
	COMMENT "Checking the level arena's room in main RAM (scripts/ramcheck.sh)"
	VERBATIM)

# The load overlay (docs/porting-notes.md §90.7): the boot's inits and a
# level's load code (engine/load.ld) linked at the level arena's top under
# the stack (engine/q1psx.ld's .text.load: OVL_BASE, OVL_ROOM bytes; after
# Q1PSX_STACK_BYTES above, which the base subtracts: an empty value would
# link it past the RAM's end, the link checks it), read
# from LOAD.BIN at boot and at each load, the room the stream's pool's for
# the rest of the level. The section is not allocated in the link (INFO: no
# segment, so the SDK's elf2x leaves it out of the EXE); the file is its
# bytes dumped; scripts/ovlcheck.py checks that nothing in play calls into
# it.
set(Q1PSX_OVL_ROOM 32768)
math(EXPR Q1PSX_OVL_BASE "0x801ffff0 - ${Q1PSX_STACK_BYTES} - ${Q1PSX_OVL_ROOM}" OUTPUT_FORMAT HEXADECIMAL)
target_link_options(q1psx PRIVATE -Wl,--defsym=OVL_BASE=${Q1PSX_OVL_BASE} -Wl,--defsym=OVL_ROOM=${Q1PSX_OVL_ROOM} -Wl,--defsym=STACK_BYTES=${Q1PSX_STACK_BYTES})
find_program(Q1PSX_OBJCOPY NAMES mipsel-none-elf-objcopy REQUIRED)
add_custom_command(TARGET q1psx POST_BUILD
	COMMAND ${Q1PSX_OBJCOPY} --dump-section .text.load=${CMAKE_CURRENT_BINARY_DIR}/load.bin $<TARGET_FILE:q1psx> ${CMAKE_CURRENT_BINARY_DIR}/q1psx_dump.elf
	COMMAND python3 ${PROJECT_SOURCE_DIR}/scripts/ovlcheck.py ${Q1PSX_OBJDUMP} ${PROJECT_SOURCE_DIR}/engine/load.ld $<TARGET_OBJECTS:q1psx>
	BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/load.bin ${CMAKE_CURRENT_BINARY_DIR}/q1psx_dump.elf
	COMMENT "The load overlay (LOAD.BIN) out of the link; checking its callers (scripts/ovlcheck.py)"
	COMMAND_EXPAND_LISTS
	VERBATIM)

# Renderer counters and stage timers (debug overlay, bench breakdown). Off in
# the "release" preset: the asm renderer then skips their per-node, per-record
# and per-polygon upkeep (R_PROFILE in engine/r_defs.inc).
option(Q1PSX_PROFILE "Renderer counters and stage timers" ON)
if(Q1PSX_PROFILE)
	set(_q1psx_profile 1)
else()
	set(_q1psx_profile 0)
endif()
target_compile_definitions(q1psx PRIVATE Q1PSX_PROFILE=${_q1psx_profile})
target_compile_options(q1psx PRIVATE
	$<$<COMPILE_LANGUAGE:ASM>:-Wa,--defsym,R_PROFILE=${_q1psx_profile}>)

# The release build (preset "release"): Quake's own menu
# over the game (engine/menu.c), no debug overlay, no map menu, no cheats.
# The menus' code at -Os (a menu's pace; every KB of code is RAM:
# docs/porting-notes.md §60); the loads and spawns elsewhere: Q_COLD
# (engine/mem.h)
set_source_files_properties(engine/menu.c engine/settings.c PROPERTIES COMPILE_OPTIONS -Os)

option(Q1PSX_RELEASE "Quake's menu, no debug tools" OFF)
if(Q1PSX_RELEASE)
	target_compile_definitions(q1psx PRIVATE Q1PSX_RELEASE=1)
	if(NOT Q1PSX_BENCH)
		# no printf: a console has no TTY (engine/noprint.h)
		target_compile_options(q1psx PRIVATE
			$<$<COMPILE_LANGUAGE:C>:-include ${PROJECT_SOURCE_DIR}/engine/noprint.h>)
	endif()
endif()
# Every build alike (docs/porting-notes.md §52, §119, engine/settings.h):
# the 4-bit textures at high, the geometry and the models low, the 3D gun,
# the draw distance 1536 units (engine/render.h R_DRAW_DIST).

# The dynamic lights' block hook: the asm (engine/r_world.s dl_block_asm) or
# its C reference (engine/dlight.c dl_block); both must give the same frames.
set(Q1PSX_DLIGHT_C 0 CACHE STRING "Dynamic lights: 0 the asm, 1 the C reference, 2 the asm checked by the C")
# The brush models' cut by the world's BSP: the asm (engine/bmfrag.s) or its
# C specification (engine/bmfrag.c cut_model_c); 2 runs both on every cut
# and compares them ("bmfasm" lines).
set(Q1PSX_BMF_C 0 CACHE STRING "Brush models' cut: 0 the asm, 1 the C reference, 2 the asm checked by the C")
# An alias model's or a particle's place among its leaf's brush-model pieces:
# the asm (engine/rl_order.s) or its C specification (engine/rlist.c
# rl_order_c, engine/bmfrag.c bmf_node_over...); 2 runs both on every ask,
# and on made-up ones in the bench's debug frames ("ordasm" lines).
set(Q1PSX_ORDER_C 0 CACHE STRING "Places among a leaf's pieces: 0 the asm, 1 the C reference, 2 the asm checked by the C")
# The C engine (above): each of them the C alone, and the slow path's C
# (engine/r_slow.c: R_SLOW_C, engine/r_defs.inc)
if(Q1PSX_ENGINE_C)
	set(_dlight_c 1)
	set(_bmf_c 1)
	set(_order_c 1)
	set(_slow_c 1)
	set(_zone_c 1)
else()
	set(_dlight_c ${Q1PSX_DLIGHT_C})
	set(_bmf_c ${Q1PSX_BMF_C})
	set(_order_c ${Q1PSX_ORDER_C})
	set(_slow_c 0)
	set(_zone_c 0)
endif()
target_compile_definitions(q1psx PRIVATE Q1PSX_ENGINE_C=$<BOOL:${Q1PSX_ENGINE_C}>
	Q1PSX_BMF_C=${_bmf_c} Q1PSX_ORDER_C=${_order_c} Q1PSX_DLIGHT_C=${_dlight_c})
target_compile_options(q1psx PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-Wa,--defsym,R_DLIGHT_C=${_dlight_c}>)
target_compile_options(q1psx PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-Wa,--defsym,R_SLOW_C=${_slow_c}>)
target_compile_options(q1psx PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-Wa,--defsym,ZONE_C=${_zone_c}>)   # (engine/zone.s)

# Headless benchmark variant (CMake preset "bench", driven by scripts/bench.sh):
# boots straight into the bench scenarios, prints "bench ..." lines to stdout,
# no debug overlay. See docs/architecture.md §9.
option(Q1PSX_BENCH "Build the headless benchmark variant" OFF)
# the bench's counters in the asm (engine/e_touch.s: the trigger slots tested)
target_compile_options(q1psx PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-Wa,--defsym,ASM_BENCH=$<BOOL:${Q1PSX_BENCH}>>)
set(Q1PSX_BENCH_SAMPLE 0 CACHE STRING "1: the bench samples the PC (engine/prof.c) over each view's frames")
set(Q1PSX_BENCH_VIEWS 1 CACHE STRING "1: bench the render views too; 0: physics checks only")
set(Q1PSX_BENCH_MAPS 0 CACHE STRING "1: bench every map on the disc (spawn views) instead")
set(Q1PSX_BENCH_GAME 0 CACHE STRING "1: bench the game instead: every weapon fired, both HUDs")
set(Q1PSX_BENCH_T 4 CACHE STRING "Affine-warp threshold the bench renders with, fixed (px; the game's floor: 4)")
set(Q1PSX_BENCH_BMF 1 CACHE STRING "0: the brush models drawn whole, not cut by the world's BSP (an A / B)")
set(Q1PSX_BENCH_BMFCULL 1 CACHE STRING "0: a moved brush model is cut again out of view too (an A / B)")
set(Q1PSX_BENCH_BMFCHK 0 CACHE STRING "1: every mover cut at 33 places along its ways in its room (bmfchk lines)")
set(Q1PSX_BENCH_BMFARENA 100 CACHE STRING "The brush models' cut arena, percent of the cook's size: the rooms given up and packed down in play (an A / B)")
set(Q1PSX_BENCH_BMFTRACE 0 CACHE STRING "A brush model: every piece of its C cut printed (bmftrace lines; BMF_C=1)")
set(Q1PSX_BENCH_HASHONLY 0 CACHE STRING "1: the bench dumps only the frames' hashes (no packets, no pictures)")
set(Q1PSX_BENCH_PLAY 0 CACHE STRING "1: the maps bench also runs the loading screen and 30 interactive frames per map")
set(Q1PSX_BENCH_MAP "" CACHE STRING "A map name (E1M1): the maps bench runs that map only")
set(Q1PSX_BENCH_ENTS 0 CACHE STRING "1: the map entities' scenarios (doors, buttons, plats, teleporters, the exit)")
set(Q1PSX_BENCH_MOVERS 0 CACHE STRING "1: the big movers moving in view: the frame's, the ents' and the re-cut's cycles (bmfcut lines)")
set(Q1PSX_BENCH_MONSTERS 0 CACHE STRING "1: the monsters' scenarios (one of each kind wakes, attacks, is killed; the streaming), then the crowd; 2: the crowd alone")
set(Q1PSX_BENCH_VIEW "" CACHE STRING "x,y,z,yaw,pitch: one more view of that map, dumped")
set(Q1PSX_BENCH_TICKS 0 CACHE STRING "With a VIEW: the player put there, the game run this many ticks, a picture every 8")
set(Q1PSX_BENCH_SKILL 1 CACHE STRING "The skill the bench plays: 0 easy .. 3 nightmare")
set(Q1PSX_BENCH_USE "" CACHE STRING "With TICKS: the entities of this targetname used at tick 8 (a door opened in view)")
set(Q1PSX_BENCH_RESIDENCY 0 CACHE STRING "1: the maps bench reports what the stream loads around the player at the map's items")
set(Q1PSX_BENCH_NOENTS 0 CACHE STRING "1: the map's entities are not drawn (an A / B of the brush models)")
set(Q1PSX_BENCH_WARPDYN 0 CACHE STRING "1: the views with the game's dynamic warp control (T its floor), the T each ends at printed")
set(Q1PSX_BENCH_TMAX 32 CACHE STRING "The dynamic warp control's cap with WARPDYN=1 (px; the anti-warping's: low 48, default 32, medium 16, high 8, ultra 4)")
set(Q1PSX_BENCH_VRAMCHK 0 CACHE STRING "1: each streamed image's VRAM place also searched the way before §100 (every resident image for every candidate), the two compared (stream: vramchk lines)")
set(Q1PSX_BENCH_IRQTIME 0 CACHE STRING "1: the interrupts' handler times itself: the bench lines' irq= (cycles a frame, entries x 100)")
target_compile_options(q1psx PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-Wa,--defsym,IRQTIME=$<AND:$<BOOL:${Q1PSX_BENCH}>,$<BOOL:${Q1PSX_BENCH_IRQTIME}>>>)
set(Q1PSX_BENCH_SLOWSTAT 0 CACHE STRING "1: the slow path counts its paths: the bench's slowstat lines (engine/r_slow.s STAT)")
target_compile_options(q1psx PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-Wa,--defsym,SLOWSTAT=$<AND:$<BOOL:${Q1PSX_BENCH}>,$<BOOL:${Q1PSX_BENCH_SLOWSTAT}>>>)
set(Q1PSX_BENCH_GPUTIME 0 CACHE STRING "1: each frame's display list drawn alone, the CPU waiting: the bench lines' gpu= (an emulator that counts the GPU's time: DuckStation)")
set(Q1PSX_BENCH_WORLD_HZ 30 CACHE STRING "The world's tick rate the bench runs at: 60 or 30 (the game's default; the player's: 60)")
set(Q1PSX_BENCH_CAPTURE 0 CACHE STRING "1: the capture's scenarios: the sounds read from the disc in their worst cases, played in real time for an emulator's recording")
set(Q1PSX_BENCH_ZONES 0 CACHE STRING "1: the zones' walk: every zoned map walked through its items, the chunk reads, switches and late frames (bench zones lines); 2: every map, the whole ones too (their sounds)")
set(Q1PSX_BENCH_CDLOG 0 CACHE STRING "1: the drive's journal: a cd line per read (who, where, when; engine/cd.h)")
set(Q1PSX_BENCH_CDMODEL 0 CACHE STRING "1: every read ends no sooner than a real drive would end it (DuckStation's seek model; engine/cd.c)")
set(Q1PSX_BENCH_MEMLOG 0 CACHE STRING "1: the level arena's log: a memlog line per block taken or given back, with its caller (engine/mem.c)")
if(Q1PSX_BENCH)
	target_compile_definitions(q1psx PRIVATE Q1PSX_BENCH=1
		Q1PSX_BENCH_CDLOG=${Q1PSX_BENCH_CDLOG} Q1PSX_BENCH_CDMODEL=${Q1PSX_BENCH_CDMODEL}
		Q1PSX_BENCH_MEMLOG=${Q1PSX_BENCH_MEMLOG}
		Q1PSX_BENCH_SAMPLE=${Q1PSX_BENCH_SAMPLE}
		Q1PSX_BENCH_IRQTIME=${Q1PSX_BENCH_IRQTIME}
		Q1PSX_BENCH_SLOWSTAT=${Q1PSX_BENCH_SLOWSTAT}
		Q1PSX_BENCH_VIEWS=${Q1PSX_BENCH_VIEWS} Q1PSX_BENCH_MAPS=${Q1PSX_BENCH_MAPS}
		Q1PSX_BENCH_GAME=${Q1PSX_BENCH_GAME}
		Q1PSX_BENCH_T=${Q1PSX_BENCH_T}
		Q1PSX_BENCH_BMF=${Q1PSX_BENCH_BMF}
		Q1PSX_BENCH_BMFCULL=${Q1PSX_BENCH_BMFCULL}
		Q1PSX_BENCH_BMFCHK=${Q1PSX_BENCH_BMFCHK} Q1PSX_BENCH_BMFARENA=${Q1PSX_BENCH_BMFARENA} Q1PSX_BENCH_BMFTRACE=${Q1PSX_BENCH_BMFTRACE}
		Q1PSX_BENCH_HASHONLY=${Q1PSX_BENCH_HASHONLY} Q1PSX_BENCH_PLAY=${Q1PSX_BENCH_PLAY}
		Q1PSX_BENCH_MAP="${Q1PSX_BENCH_MAP}" Q1PSX_BENCH_NOENTS=${Q1PSX_BENCH_NOENTS}
		Q1PSX_BENCH_ENTS=${Q1PSX_BENCH_ENTS} Q1PSX_BENCH_MONSTERS=${Q1PSX_BENCH_MONSTERS}
		Q1PSX_BENCH_TICKS=${Q1PSX_BENCH_TICKS} Q1PSX_BENCH_USE="${Q1PSX_BENCH_USE}" Q1PSX_BENCH_SKILL=${Q1PSX_BENCH_SKILL} Q1PSX_BENCH_RESIDENCY=${Q1PSX_BENCH_RESIDENCY}
		Q1PSX_BENCH_MOVERS=${Q1PSX_BENCH_MOVERS} Q1PSX_BENCH_ZONES=${Q1PSX_BENCH_ZONES}
		Q1PSX_BENCH_CAPTURE=${Q1PSX_BENCH_CAPTURE}
		Q1PSX_BENCH_WORLD_HZ=${Q1PSX_BENCH_WORLD_HZ}
		Q1PSX_BENCH_GPUTIME=${Q1PSX_BENCH_GPUTIME} Q1PSX_BENCH_WARPDYN=${Q1PSX_BENCH_WARPDYN} Q1PSX_BENCH_TMAX=${Q1PSX_BENCH_TMAX}
		Q1PSX_BENCH_VRAMCHK=${Q1PSX_BENCH_VRAMCHK})
	if(Q1PSX_BENCH_VIEW)
		# groups of five numbers, one definition with no spaces (a quoted string
		# with spaces does not survive the generator's command line)
		string(REPLACE "," ";" _view_list "${Q1PSX_BENCH_VIEW}")
		list(LENGTH _view_list _view_n)
		math(EXPR _view_r "${_view_n} % 5")
		if(_view_n EQUAL 0 OR NOT _view_r EQUAL 0)
			message(FATAL_ERROR "Q1PSX_BENCH_VIEW needs x,y,z,yaw,pitch (more views: more groups of five)")
		endif()
		target_compile_definitions(q1psx PRIVATE Q1PSX_BENCH_VIEW_SET=1 "Q1PSX_BENCH_VIEWS_XY=${Q1PSX_BENCH_VIEW}")
	endif()
	if(NOT Q1PSX_ENGINE_C)   # (the C engine's physics is this C: main.c checks it against itself)
		target_sources(q1psx PRIVATE ${PHYS_C_REFERENCE})
		set_source_files_properties(${PHYS_C_REFERENCE} PROPERTIES COMPILE_DEFINITIONS
			"clip_trace=clip_trace_c;clip_contents=clip_contents_c;pmove_step=pmove_step_c;clip_trace_ent=clip_trace_ent_c;clip_mover=clip_mover_c;clip_box_node0=clip_box_node0_c;clip_contents_node0=clip_contents_node0_c")
	endif()
endif()

# --- optional Quake game data on the disc ------------------------------------
# pak0.pak is never committed. If the developer pointed local.env at their copy,
# bundle it into the CD image so the engine can read it. iso.xml consumes
# @PAK0_ENTRY@ (empty when no pak is configured).
set(PAK0_ENTRY "")
set(_zone_tables zones/auto_1536_s3)   # (scripts/cook.sh's default: the repo's tables, §118)
if(EXISTS "${PROJECT_SOURCE_DIR}/local.env")
	file(STRINGS "${PROJECT_SOURCE_DIR}/local.env" _local_env)
	foreach(_line ${_local_env})
		if(_line MATCHES "^[ \t]*QUAKE_PAK0[ \t]*=[ \t]*(.+)$")
			string(STRIP "${CMAKE_MATCH_1}" _pak0_path)
		endif()
		if(_line MATCHES "^[ \t]*Q1PSX_ZONE_TABLES[ \t]*=[ \t]*(.*)$")
			string(STRIP "${CMAKE_MATCH_1}" _zone_tables)
		endif()
	endforeach()
endif()
if(DEFINED ENV{Q1PSX_ZONE_TABLES})     # (the command line's wins over local.env's, as in the cook)
	set(_zone_tables "$ENV{Q1PSX_ZONE_TABLES}")
endif()
if(_pak0_path AND EXISTS "${_pak0_path}")
	message(STATUS "q1psx: bundling pak0.pak from ${_pak0_path}")
	set(PAK0_ENTRY "<file name=\"PAK0.PAK\" type=\"data\" source=\"${_pak0_path}\" />")
else()
	message(STATUS "q1psx: no pak0.pak configured (set QUAKE_PAK0 in local.env) — disc has no game data")
endif()

# The world's zones (docs/porting-notes.md §57): a map the cook split
# (cooked/id1/<map>.skl and <map>.zon, scripts/cook.sh with the zone
# tables: the repo's zones/auto_1536_s3 unless Q1PSX_ZONE_TABLES names
# others, or none) goes on the disc as its skeleton (<MAP>.WLD) and its
# chunks (<MAP>.ZON); a map left whole, its whole world. The tables hold
# every record the PSX may draw within the draw distance, 1536 units (+ a
# margin).
# The tables the cook reads (Q1PSX_ZONE_TABLES: the configure's
# environment's, else local.env's, else the repo's zones/auto_1536_s3; a
# cook run by hand with others is not seen here) are cut
# for one draw distance, in their directory's name (pvs_1536_96: every
# record the PVS may draw within 1536 units; auto_1536_s3: the same, the
# cap chosen a map for three slots, §93; seen_1024_64: those the
# analysis' renders saw within 1024). A zoned disc drawn farther than its
# tables' cut loses every wall between the two distances, from afar: it
# is refused. The seen tables carry only what the renders saw: a wall seen
# from a place the scan did not sample is missing there (docs/porting-notes.md
# §57.8, §95).
if(_zone_tables AND _zone_tables MATCHES "(pvs|seen|auto)_([0-9]+)_s?([0-9]+)/?$")
	set(_tables_kind ${CMAKE_MATCH_1})
	set(_tables_dist ${CMAKE_MATCH_2})
	if(_tables_dist LESS 1536)
		message(FATAL_ERROR "q1psx: the zone tables ${_zone_tables} are cut at ${_tables_dist} units, the draw distance is 1536: "
		        "the walls between the two would be missing from afar. Point Q1PSX_ZONE_TABLES (local.env) at a *_1536_* set, "
		        "or at none for the whole worlds.")
	endif()
	if(_tables_kind STREQUAL "seen")
		message(STATUS "q1psx: zone tables ${_tables_kind}_${_tables_dist}: the chunks hold what the analysis' renders saw only (a wall the scan never looked at is missing there; the pvs tables are exact)")
	else()
		message(STATUS "q1psx: zone tables ${_tables_kind}_${_tables_dist}")
	endif()
endif()

# The music (docs/porting-notes.md §48, §52): Quake's CD tracks 2..11 as
# ADPCM the SPU streams (cooked/spu/spu_NN.dat, formats/qspu.h: 22.05 kHz,
# the menu's theme 32 kHz), in \MUSIC after the streamed assets; the
# bench's disc has none (Q1PSX_MUSIC OFF unless Q1PSX_BENCH_MUSIC).
option(Q1PSX_MUSIC "Put the cooked music on the disc" ON)
set(_music_glob ${PROJECT_SOURCE_DIR}/cooked/spu/spu_*.dat)
# Cooked assets (scripts/cook.sh -> cooked/, formats/qtex.h, qworld.h,
# qgame.h). Every cooked map ships as <MAP>.TX4 (its 4-bit textures),
# <MAP>.WLD (its world), <MAP>.MDL (its own models), its sound bank
# <MAP>.SND, plus the game data GAME.DAT (the guns, the projectiles, the
# HUD). The cook re-runs whenever the cooker or a format header changes, so
# the disc never carries stale data; and when its settings do (local.env's
# or the command line's: the zone tables and slots, the maps, the paks...):
# every build first runs `scripts/cook.sh --stale`, which rewrites
# cook.settings when they differ from the last cook's (cooked/settings.txt,
# shared with the other build directories).
if(_pak0_path AND EXISTS "${_pak0_path}")
	file(GLOB _qcook_deps CONFIGURE_DEPENDS
		${PROJECT_SOURCE_DIR}/tools/qcook/*.c
		${PROJECT_SOURCE_DIR}/tools/qcook/*.h
		${PROJECT_SOURCE_DIR}/tools/qcook/Makefile
		${PROJECT_SOURCE_DIR}/formats/*.h
		${PROJECT_SOURCE_DIR}/scripts/cook.sh)
	set(_cooked_stamp ${CMAKE_BINARY_DIR}/cooked.stamp)
	set(_cook_settings ${CMAKE_BINARY_DIR}/cook.settings)
	add_custom_target(cook_settings
		COMMAND ${PROJECT_SOURCE_DIR}/scripts/cook.sh --stale ${_cook_settings}
		BYPRODUCTS ${_cook_settings}
		WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
		COMMENT "Checking the cook's settings (scripts/cook.sh --stale)"
		VERBATIM)
	add_custom_command(
		OUTPUT  ${_cooked_stamp}
		COMMAND ${PROJECT_SOURCE_DIR}/scripts/cook.sh
		COMMAND ${CMAKE_COMMAND} -E touch ${_cooked_stamp}
		DEPENDS ${_qcook_deps} ${_cook_settings}
		WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
		COMMENT "Cooking Quake data (scripts/cook.sh)"
		VERBATIM)
	add_custom_target(cook DEPENDS ${_cooked_stamp})
	add_dependencies(q1psx cook)
endif()

# The cooked files the disc carries (their list is made when the image is
# built, after the cook: scripts/cd_image.cmake); globbed here to rebuild
# the image when a manual cook changes them, and for the configure's note.
file(GLOB _cooked CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/cooked/id1/*.wld
                                    ${PROJECT_SOURCE_DIR}/cooked/id1/*.skl
                                    ${PROJECT_SOURCE_DIR}/cooked/id1/*.zon
                                    ${PROJECT_SOURCE_DIR}/cooked/id1/*.tx4
                                    ${PROJECT_SOURCE_DIR}/cooked/id1/*.mdl
                                    ${PROJECT_SOURCE_DIR}/cooked/id1/*.snd
                                    ${PROJECT_SOURCE_DIR}/cooked/id1/*.dat)
if(NOT _cooked)
	message(STATUS "q1psx: no cooked assets yet (the build cooks them: scripts/cook.sh)")
else()
	message(STATUS "q1psx: cooked assets in cooked/")
endif()

set(_music "")
set(_disc_music 0)
set(Q1PSX_BENCH_MUSIC 0 CACHE STRING "1: the bench disc carries the music too")
if(Q1PSX_MUSIC AND (NOT Q1PSX_BENCH OR Q1PSX_BENCH_MUSIC))
	set(_disc_music 1)
	file(GLOB _music CONFIGURE_DEPENDS ${_music_glob})
	message(STATUS "q1psx: the music on the disc")
endif()

# --- CD image ---------------------------------------------------------------
# (psn00bsdk_add_cd_image's, with the project's file list made at the build:
# scripts/cd_image.cmake, after the cook)
set(_cd_xml ${CMAKE_CURRENT_BINARY_DIR}/cd_image.xml)
add_custom_command(
	OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/q1psx.bin ${CMAKE_CURRENT_BINARY_DIR}/q1psx.cue
	COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DOUT=${_cd_xml}
	        -DMUSIC=${_disc_music} -DPAK0=${_pak0_path}
	        -DSDK_VERSION=${PSN00BSDK_VERSION} -P ${PROJECT_SOURCE_DIR}/scripts/cd_image.cmake
	COMMAND ${MKPSXISO} -y -q -o q1psx.bin -c q1psx.cue ${_cd_xml}
	DEPENDS q1psx ${_cooked_stamp} ${_cooked} ${_music} ${PROJECT_SOURCE_DIR}/iso/iso.xml
	        ${PROJECT_SOURCE_DIR}/scripts/cd_image.cmake   # (a manual cook too)
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	COMMENT "Building CD image q1psx"
	VERBATIM)
add_custom_target(iso ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/q1psx.bin ${CMAKE_CURRENT_BINARY_DIR}/q1psx.cue)
