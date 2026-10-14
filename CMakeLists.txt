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

psn00bsdk_add_executable(q1psx GPREL ${ENGINE_SOURCES})
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

# Headless benchmark variant (CMake preset "bench", driven by scripts/bench.sh):
# boots straight into the bench scenarios, prints "bench ..." lines to stdout,
# no debug overlay. See docs/architecture.md §9.
option(Q1PSX_BENCH "Build the headless benchmark variant" OFF)
set(Q1PSX_BENCH_FID 2 CACHE STRING "Fidelity level the bench loads (0 low .. 3 ultra)")
if(Q1PSX_BENCH)
	target_compile_definitions(q1psx PRIVATE Q1PSX_BENCH=1 Q1PSX_BENCH_FID=${Q1PSX_BENCH_FID})
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
# fidelity level ships on the disc: START<N>.TEX / START<N>.WLD with N = 0..3
# for low/medium/high/ultra. The cook re-runs whenever the cooker or a format
# header changes, so the disc never carries stale data.
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
set(_n 0)
foreach(_fid low medium high ultra)
	foreach(_ext tex wld)
		set(_src "${PROJECT_SOURCE_DIR}/cooked/id1/start_${_fid}.${_ext}")
		string(TOUPPER "${_ext}" _EXT)
		if(EXISTS "${_src}")
			string(APPEND COOKED_ENTRIES
				"<file name=\"START${_n}.${_EXT}\" type=\"data\" source=\"${_src}\" />\n\t\t\t")
		endif()
	endforeach()
	math(EXPR _n "${_n} + 1")
endforeach()
if(COOKED_ENTRIES STREQUAL "")
	message(STATUS "q1psx: no cooked assets (run scripts/cook.sh)")
else()
	message(STATUS "q1psx: bundling cooked start.bsp (4 fidelity levels)")
endif()

# --- CD image ---------------------------------------------------------------
psn00bsdk_add_cd_image(
	iso        # target
	q1psx      # -> q1psx.bin + q1psx.cue
	${PROJECT_SOURCE_DIR}/iso/iso.xml
	DEPENDS q1psx ${_cooked_stamp}
)
