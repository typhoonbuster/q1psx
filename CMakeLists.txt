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

# --- CD image ---------------------------------------------------------------
psn00bsdk_add_cd_image(
	iso        # target
	q1psx      # -> q1psx.bin + q1psx.cue
	${PROJECT_SOURCE_DIR}/iso/iso.xml
	DEPENDS q1psx
)
