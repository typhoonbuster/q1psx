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

# The engine maps cooked structs straight onto byte blobs (QBS/QSF/QTX/PAK)
# everywhere — the same idiom Quake itself relies on. Turn off the two -O2
# assumptions that idiom violates: type-based alias analysis, and "signed
# arithmetic never overflows".
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

# Cooked assets (scripts/cook.sh -> cooked/). Phase 4.3: one QTX on the disc as
# START.QTX; the full cooked/ tree gets a proper dir layout later.
set(COOKED_ENTRIES "")
if(EXISTS "${PROJECT_SOURCE_DIR}/cooked/id1/high/start.qtx")
	message(STATUS "q1psx: bundling cooked/id1/high/start.qtx as START.QTX")
	string(APPEND COOKED_ENTRIES
		"<file name=\"START.QTX\" type=\"data\" source=\"${PROJECT_SOURCE_DIR}/cooked/id1/high/start.qtx\" />\n\t\t\t")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/cooked/id1/common/start.qgeo")
	message(STATUS "q1psx: bundling cooked/id1/common/start.qgeo as START.QGE")
	string(APPEND COOKED_ENTRIES
		"<file name=\"START.QGE\" type=\"data\" source=\"${PROJECT_SOURCE_DIR}/cooked/id1/common/start.qgeo\" />\n\t\t\t")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/cooked/id1/common/start.qsrf")
	message(STATUS "q1psx: bundling cooked/id1/common/start.qsrf as START.QSF")
	string(APPEND COOKED_ENTRIES
		"<file name=\"START.QSF\" type=\"data\" source=\"${PROJECT_SOURCE_DIR}/cooked/id1/common/start.qsrf\" />\n\t\t\t")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/cooked/id1/common/start.qbsp")
	message(STATUS "q1psx: bundling cooked/id1/common/start.qbsp as START.QBS")
	string(APPEND COOKED_ENTRIES
		"<file name=\"START.QBS\" type=\"data\" source=\"${PROJECT_SOURCE_DIR}/cooked/id1/common/start.qbsp\" />")
endif()
if(COOKED_ENTRIES STREQUAL "")
	message(STATUS "q1psx: no cooked assets (run scripts/cook.sh)")
endif()

# --- CD image ---------------------------------------------------------------
psn00bsdk_add_cd_image(
	iso        # target
	q1psx      # -> q1psx.bin + q1psx.cue
	${PROJECT_SOURCE_DIR}/iso/iso.xml
	DEPENDS q1psx
)
