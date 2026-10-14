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
target_include_directories(q1psx PRIVATE engine)

# The engine maps cooked structs straight onto byte blobs — the same idiom
# Quake itself relies on. Turn off the two -O2 assumptions that idiom
# violates: type-based alias analysis, and "signed arithmetic never
# overflows".
target_compile_options(q1psx PRIVATE -fno-strict-aliasing -fwrapv)

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

# Cooked assets: the cook tool and its formats land in roadmap step R2
# (see docs/architecture.md). Until then the disc carries no cooked data;
# iso.xml still expands @COOKED_ENTRIES@, so keep the variable defined.
set(COOKED_ENTRIES "")

# --- CD image ---------------------------------------------------------------
psn00bsdk_add_cd_image(
	iso        # target
	q1psx      # -> q1psx.bin + q1psx.cue
	${PROJECT_SOURCE_DIR}/iso/iso.xml
	DEPENDS q1psx
)
