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

psn00bsdk_add_executable(q1psx GPREL ${ENGINE_SOURCES})

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

# The release editions (presets "release" and "release-ultra"): Quake's own
# menu over the game (engine/menu.c), no debug overlay, no map menu, no
# cheats; and the fidelity levels the disc carries: low to high on the
# "release" disc, ultra alone on the "release-ultra" one (the default and
# bench builds carry all four).
option(Q1PSX_RELEASE "Quake's menu, no debug tools" OFF)
if(Q1PSX_RELEASE)
	target_compile_definitions(q1psx PRIVATE Q1PSX_RELEASE=1)
endif()
set(Q1PSX_FIDELITIES "low;medium;high;ultra" CACHE STRING "The fidelity levels put on the disc")
# A scripted pad (engine/pad.c pad_script): the release flow checked headless
option(Q1PSX_PADSCRIPT "Replace the pad by a scripted sequence of presses" OFF)
if(Q1PSX_PADSCRIPT)
	target_compile_definitions(q1psx PRIVATE Q1PSX_PADSCRIPT=1)
endif()

# The dynamic lights' block hook: the asm (engine/r_world.s dl_block_asm) or
# its C reference (engine/dlight.c dl_block); both must give the same frames.
set(Q1PSX_DLIGHT_C 0 CACHE STRING "Dynamic lights: 0 the asm, 1 the C reference, 2 the asm checked by the C")
target_compile_options(q1psx PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-Wa,--defsym,R_DLIGHT_C=${Q1PSX_DLIGHT_C}>)
target_compile_definitions(q1psx PRIVATE Q1PSX_DLIGHT_C=${Q1PSX_DLIGHT_C})

# Headless benchmark variant (CMake preset "bench", driven by scripts/bench.sh):
# boots straight into the bench scenarios, prints "bench ..." lines to stdout,
# no debug overlay. See docs/architecture.md §9.
option(Q1PSX_BENCH "Build the headless benchmark variant" OFF)
set(Q1PSX_BENCH_FID 2 CACHE STRING "Fidelity level the bench loads (0 low .. 3 ultra)")
set(Q1PSX_BENCH_SAMPLE 0 CACHE STRING "1: the bench samples the PC (engine/prof.c) over each view's frames")
set(Q1PSX_BENCH_VIEWS 1 CACHE STRING "1: bench the render views too; 0: physics checks only")
set(Q1PSX_BENCH_MAPS 0 CACHE STRING "1: bench every map on the disc (spawn views) instead")
set(Q1PSX_BENCH_GAME 0 CACHE STRING "1: bench the game instead: every weapon fired, both HUDs")
set(Q1PSX_BENCH_T 8 CACHE STRING "Affine-warp threshold the bench renders with (px)")
set(Q1PSX_BENCH_TEX4 0 CACHE STRING "1: the bench loads the 4-bit textures")
set(Q1PSX_BENCH_DRAWDIST 0 CACHE STRING "The draw distance the bench renders with, world units (0: off)")
set(Q1PSX_BENCH_FLAT 0 CACHE STRING "The flat shading the bench renders with: 0 off, 1 far, 2 all")
set(Q1PSX_BENCH_VIEWMODEL 1 CACHE STRING "The gun the bench draws: 0 none, 1 the 3D model, 2 the 2D pictures")
set(Q1PSX_BENCH_HASHONLY 0 CACHE STRING "1: the bench dumps only the frames' hashes (no packets, no pictures)")
set(Q1PSX_BENCH_PLAY 0 CACHE STRING "1: the maps bench also runs the loading screen and 30 interactive frames per map")
set(Q1PSX_BENCH_MAP "" CACHE STRING "A map name (E1M1): the maps bench runs that map only")
set(Q1PSX_BENCH_ENTS 0 CACHE STRING "1: the map entities' scenarios (doors, buttons, plats, teleporters, the exit)")
set(Q1PSX_BENCH_MONSTERS 0 CACHE STRING "1: the monsters' scenarios (one of each kind wakes, attacks, is killed; the streaming), then the crowd; 2: the crowd alone")
set(Q1PSX_BENCH_VIEW "" CACHE STRING "x,y,z,yaw,pitch: one more view of that map, dumped")
set(Q1PSX_BENCH_NOENTS 0 CACHE STRING "1: the map's entities are not drawn (an A / B of the brush models)")
if(Q1PSX_BENCH)
	target_compile_definitions(q1psx PRIVATE Q1PSX_BENCH=1 Q1PSX_BENCH_FID=${Q1PSX_BENCH_FID}
		Q1PSX_BENCH_SAMPLE=${Q1PSX_BENCH_SAMPLE}
		Q1PSX_BENCH_VIEWS=${Q1PSX_BENCH_VIEWS} Q1PSX_BENCH_MAPS=${Q1PSX_BENCH_MAPS}
		Q1PSX_BENCH_GAME=${Q1PSX_BENCH_GAME}
		Q1PSX_BENCH_T=${Q1PSX_BENCH_T} Q1PSX_BENCH_TEX4=${Q1PSX_BENCH_TEX4}
		Q1PSX_BENCH_DRAWDIST=${Q1PSX_BENCH_DRAWDIST} Q1PSX_BENCH_FLAT=${Q1PSX_BENCH_FLAT}
		Q1PSX_BENCH_VIEWMODEL=${Q1PSX_BENCH_VIEWMODEL}
		Q1PSX_BENCH_HASHONLY=${Q1PSX_BENCH_HASHONLY} Q1PSX_BENCH_PLAY=${Q1PSX_BENCH_PLAY}
		Q1PSX_BENCH_MAP="${Q1PSX_BENCH_MAP}" Q1PSX_BENCH_NOENTS=${Q1PSX_BENCH_NOENTS}
		Q1PSX_BENCH_ENTS=${Q1PSX_BENCH_ENTS} Q1PSX_BENCH_MONSTERS=${Q1PSX_BENCH_MONSTERS})
	if(Q1PSX_BENCH_VIEW)
		# five numbers: separate definitions (a quoted string with spaces does not
		# survive the generator's command line)
		string(REPLACE "," ";" _view_list "${Q1PSX_BENCH_VIEW}")
		list(LENGTH _view_list _view_n)
		if(NOT _view_n EQUAL 5)
			message(FATAL_ERROR "Q1PSX_BENCH_VIEW needs x,y,z,yaw,pitch")
		endif()
		list(GET _view_list 0 _vx)
		list(GET _view_list 1 _vy)
		list(GET _view_list 2 _vz)
		list(GET _view_list 3 _vyaw)
		list(GET _view_list 4 _vpitch)
		target_compile_definitions(q1psx PRIVATE Q1PSX_BENCH_VIEW_SET=1 Q1PSX_BENCH_VX=${_vx}
			Q1PSX_BENCH_VY=${_vy} Q1PSX_BENCH_VZ=${_vz} Q1PSX_BENCH_VYAW=${_vyaw} Q1PSX_BENCH_VPITCH=${_vpitch})
	endif()
	target_sources(q1psx PRIVATE ${PHYS_C_REFERENCE})
	set_source_files_properties(${PHYS_C_REFERENCE} PROPERTIES COMPILE_DEFINITIONS
		"clip_trace=clip_trace_c;clip_contents=clip_contents_c;pmove_step=pmove_step_c;clip_trace_ent=clip_trace_ent_c;clip_mover=clip_mover_c;clip_box_node0=clip_box_node0_c;clip_contents_node0=clip_contents_node0_c")
endif()

# --- optional Quake game data on the disc ------------------------------------
# pak0.pak is never committed. If the developer pointed local.env at their copy,
# bundle it into the CD image so the engine can read it. iso.xml consumes
# @PAK0_ENTRY@ (empty when no pak is configured).
set(PAK0_ENTRY "")
if(EXISTS "${PROJECT_SOURCE_DIR}/local.env")
	file(STRINGS "${PROJECT_SOURCE_DIR}/local.env" _local_env)
	foreach(_line ${_local_env})
		if(_line MATCHES "^[ \t]*QUAKE_PAK0[ \t]*=[ \t]*(.+)$")
			string(STRIP "${CMAKE_MATCH_1}" _pak0_path)
		endif()
	endforeach()
endif()
if(_pak0_path AND EXISTS "${_pak0_path}")
	message(STATUS "q1psx: bundling pak0.pak from ${_pak0_path}")
	set(PAK0_ENTRY "<file name=\"PAK0.PAK\" type=\"data\" source=\"${_pak0_path}\" />")
else()
	message(STATUS "q1psx: no pak0.pak configured (set QUAKE_PAK0 in local.env) — disc has no game data")
endif()

# Cooked assets (scripts/cook.sh -> cooked/, formats/qtex.h + qworld.h). Every
# cooked map ships at every fidelity level: <MAP>_<N>.TEX / .TX4 / .WLD with
# N = 0..3 for low/medium/high/ultra (.TX4: the 4-bit textures), plus the
# game data GAME.DAT (formats/qgame.h: weapon models, projectiles, the HUD).
# The cook re-runs whenever the cooker or a format header changes, so the
# disc never carries stale data.
if(_pak0_path AND EXISTS "${_pak0_path}")
	file(GLOB _qcook_deps CONFIGURE_DEPENDS
		${PROJECT_SOURCE_DIR}/tools/qcook/*.c
		${PROJECT_SOURCE_DIR}/tools/qcook/*.h
		${PROJECT_SOURCE_DIR}/tools/qcook/Makefile
		${PROJECT_SOURCE_DIR}/formats/*.h
		${PROJECT_SOURCE_DIR}/scripts/cook.sh)
	set(_cooked_stamp ${CMAKE_BINARY_DIR}/cooked.stamp)
	add_custom_command(
		OUTPUT  ${_cooked_stamp}
		COMMAND ${PROJECT_SOURCE_DIR}/scripts/cook.sh
		COMMAND ${CMAKE_COMMAND} -E touch ${_cooked_stamp}
		DEPENDS ${_qcook_deps}
		WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
		COMMENT "Cooking Quake data (scripts/cook.sh)"
		VERBATIM)
	add_custom_target(cook DEPENDS ${_cooked_stamp})
	add_dependencies(q1psx cook)
endif()

set(COOKED_ENTRIES "")
set(GAME_ENTRY "")
if(EXISTS "${PROJECT_SOURCE_DIR}/cooked/id1/game.dat")
	set(GAME_ENTRY "<file name=\"GAME.DAT\" type=\"data\" source=\"${PROJECT_SOURCE_DIR}/cooked/id1/game.dat\" />")
endif()
# the streamed assets (formats/qstream.h: read while the game runs, engine/stream.c)
set(STREAM_ENTRY "")
if(EXISTS "${PROJECT_SOURCE_DIR}/cooked/id1/stream.dat")
	set(STREAM_ENTRY "<file name=\"STREAM.DAT\" type=\"data\" source=\"${PROJECT_SOURCE_DIR}/cooked/id1/stream.dat\" />")
endif()
set(_maps "")
file(GLOB _cooked CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/cooked/id1/*.wld
                                    ${PROJECT_SOURCE_DIR}/cooked/id1/*.tex
                                    ${PROJECT_SOURCE_DIR}/cooked/id1/*.tx4
                                    ${PROJECT_SOURCE_DIR}/cooked/id1/*.snd)
list(SORT _cooked)
foreach(_src ${_cooked})
	get_filename_component(_base "${_src}" NAME_WE)     # e1m1_high
	get_filename_component(_ext "${_src}" EXT)          # .wld
	if(_ext STREQUAL ".snd")                            # e1m1.snd -> E1M1.SND (formats/qsound.h)
		string(TOUPPER "${_base}${_ext}" _name)
		string(APPEND COOKED_ENTRIES
			"<file name=\"${_name}\" type=\"data\" source=\"${_src}\" />\n\t\t\t")
		continue()
	endif()
	string(REGEX MATCH "^(.+)_(low|medium|high|ultra)$" _m "${_base}")
	if(NOT _m)
		continue()
	endif()
	set(_map "${CMAKE_MATCH_1}")
	if(NOT "${CMAKE_MATCH_2}" IN_LIST Q1PSX_FIDELITIES)
		continue()                                      # (another edition's level)
	endif()
	set(_fids low medium high ultra)
	list(FIND _fids "${CMAKE_MATCH_2}" _n)
	string(TOUPPER "${_map}_${_n}${_ext}" _name)        # E1M1_2.WLD
	string(APPEND COOKED_ENTRIES
		"<file name=\"${_name}\" type=\"data\" source=\"${_src}\" />\n\t\t\t")
	list(APPEND _maps "${_map}")
endforeach()
list(REMOVE_DUPLICATES _maps)
list(LENGTH _maps _nmaps)
if(_nmaps EQUAL 0)
	message(STATUS "q1psx: no cooked assets (run scripts/cook.sh)")
else()
	message(STATUS "q1psx: bundling ${_nmaps} cooked maps at ${Q1PSX_FIDELITIES}")
endif()

# The music: cooked/music/trackNN.wav (scripts/cook.sh, from the rerelease's
# tracks) become CD-DA tracks after the data track; the bench's disc has
# none (Q1PSX_MUSIC OFF).
option(Q1PSX_MUSIC "Put the cooked music on the disc as CD-DA tracks" ON)
set(AUDIO_TRACKS "")
set(_music "")
set(Q1PSX_BENCH_MUSIC 0 CACHE STRING "1: the bench disc carries the music too (the loads after CD-DA playback)")
if(Q1PSX_MUSIC AND (NOT Q1PSX_BENCH OR Q1PSX_BENCH_MUSIC))
	file(GLOB _music CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/cooked/music/track*.wav)
	list(SORT _music)
	foreach(_src ${_music})
		string(APPEND AUDIO_TRACKS "<track type=\"audio\" source=\"${_src}\" />\n\t")
	endforeach()
	list(LENGTH _music _ntracks)
	message(STATUS "q1psx: ${_ntracks} CD-DA music tracks")
endif()

# --- CD image ---------------------------------------------------------------
psn00bsdk_add_cd_image(
	iso        # target
	q1psx      # -> q1psx.bin + q1psx.cue
	${PROJECT_SOURCE_DIR}/iso/iso.xml
	DEPENDS q1psx ${_cooked_stamp} ${_cooked} ${_music} ${PROJECT_SOURCE_DIR}/cooked/id1/game.dat   # (a manual cook too)
)
