# q1psx — the CD image's mkpsxiso project, written when the image is built
# (CMakeLists.txt's iso target runs it), right after the cook: the files a
# cook adds (a new format's, the music's streams, the menu's bank and
# background) go on the disc at the first build. (Listed at CMake's
# configure step, they would miss that build: the cook runs after it.)
#
#   cmake -DROOT=<source dir> -DOUT=<xml> -DTEXTURES=high|medium -DMUSIC=0|1 -DZONES=0|1
#         [-DPAK0=<pak0.pak>] -DSDK_VERSION=<v> -P scripts/cd_image.cmake
#
# iso/iso.xml's slots: @GAME_ENTRY@ (the game data, low: GAME_0.DAT; the
# console's background), @STREAM_ENTRY@ (the streamed assets, low:
# STREAM_0.DAT), @COOKED_ENTRIES@ (\MAPS: every map's 4-bit textures at the
# build's level, its low geometry, its own models (low), its sound bank, the
# menu's bank), @MUSIC_ENTRIES@ (\MUSIC: the SPU music's files),
# @PAK0_ENTRY@ (docs/porting-notes.md §52). With ZONES, a map the cook
# split into zones (<map>_g0.skl beside its world) takes its skeleton as
# <MAP>_G0.WLD and its chunks at the build's texture level as
# <MAP>_Z<N>.ZON (docs/porting-notes.md §57).

if(TEXTURES STREQUAL "medium")
	set(_tex_level 1)
else()
	set(_tex_level 2)
endif()
set(_cooked_dir ${ROOT}/cooked)

set(GAME_ENTRY "")
if(EXISTS ${_cooked_dir}/id1/game_0.dat)
	string(APPEND GAME_ENTRY "<file name=\"GAME_0.DAT\" type=\"data\" source=\"${_cooked_dir}/id1/game_0.dat\" />\n\t\t\t")
endif()
# the console's background under the release's menu before a map is loaded
# (formats/qmenu.h)
if(EXISTS ${_cooked_dir}/id1/conback.dat)
	string(APPEND GAME_ENTRY "<file name=\"CONBACK.DAT\" type=\"data\" source=\"${_cooked_dir}/id1/conback.dat\" />\n\t\t\t")
endif()

set(STREAM_ENTRY "")
if(EXISTS ${_cooked_dir}/id1/stream_0.dat)
	string(APPEND STREAM_ENTRY "<file name=\"STREAM_0.DAT\" type=\"data\" source=\"${_cooked_dir}/id1/stream_0.dat\" />\n\t\t\t")
endif()

set(COOKED_ENTRIES "")
set(_maps "")
file(GLOB _cooked ${_cooked_dir}/id1/*.wld ${_cooked_dir}/id1/*.tx4
                   ${_cooked_dir}/id1/*.mdl ${_cooked_dir}/id1/*.snd)
set(_nzoned 0)
list(SORT _cooked)
foreach(_src ${_cooked})
	get_filename_component(_base "${_src}" NAME_WE)     # e1m1_g2
	get_filename_component(_ext "${_src}" EXT)          # .wld
	if(_ext STREQUAL ".snd")                            # e1m1.snd -> E1M1.SND, menu.snd -> MENU.SND
		string(TOUPPER "${_base}${_ext}" _name)
		string(APPEND COOKED_ENTRIES "<file name=\"${_name}\" type=\"data\" source=\"${_src}\" />\n\t\t\t")
		continue()
	endif()
	string(REGEX MATCH "^(.+)_([tgm])([0-3])$" _m "${_base}")
	if(NOT _m)
		continue()
	endif()
	set(_map "${CMAKE_MATCH_1}")
	if(CMAKE_MATCH_2 STREQUAL "t" AND NOT CMAKE_MATCH_3 EQUAL _tex_level)
		continue()                                      # (the other texture level)
	endif()
	if(NOT CMAKE_MATCH_2 STREQUAL "t" AND NOT CMAKE_MATCH_3 EQUAL 0)
		continue()                                      # (an older cook's level)
	endif()
	string(TOUPPER "${_base}${_ext}" _name)             # E1M1_G2.WLD
	if(ZONES AND CMAKE_MATCH_2 STREQUAL "g" AND EXISTS "${_cooked_dir}/id1/${_base}.skl"
	   AND EXISTS "${_cooked_dir}/id1/${_map}_z${_tex_level}.zon")
		set(_src "${_cooked_dir}/id1/${_base}.skl")      # the skeleton, and its chunks
		string(TOUPPER "${_map}_z${_tex_level}.zon" _zname)
		string(APPEND COOKED_ENTRIES "<file name=\"${_zname}\" type=\"data\" source=\"${_cooked_dir}/id1/${_map}_z${_tex_level}.zon\" />\n\t\t\t")
		math(EXPR _nzoned "${_nzoned} + 1")
	endif()
	string(APPEND COOKED_ENTRIES "<file name=\"${_name}\" type=\"data\" source=\"${_src}\" />\n\t\t\t")
	list(APPEND _maps "${_map}")
endforeach()
list(REMOVE_DUPLICATES _maps)
list(LENGTH _maps _nmaps)

# the music (docs/porting-notes.md §48, §52): the SPU music's files
set(MUSIC_ENTRIES "")
set(_ntracks 0)
if(MUSIC)
	file(GLOB _music ${_cooked_dir}/spu/spu_*.dat)
	list(SORT _music)
	foreach(_src ${_music})                             # spu_04.dat -> MUSIC/SPU_04.DAT
		get_filename_component(_base "${_src}" NAME)
		string(TOUPPER "${_base}" _name)
		string(APPEND MUSIC_ENTRIES "<file name=\"${_name}\" type=\"data\" source=\"${_src}\" />\n\t\t\t")
	endforeach()
	list(LENGTH _music _ntracks)
endif()

set(PAK0_ENTRY "")
if(PAK0 AND EXISTS "${PAK0}")
	set(PAK0_ENTRY "<file name=\"PAK0.PAK\" type=\"data\" source=\"${PAK0}\" />")
endif()

set(PROJECT_SOURCE_DIR ${ROOT})                         # (iso.xml's own paths)
set(PSN00BSDK_VERSION ${SDK_VERSION})
configure_file(${ROOT}/iso/iso.xml ${OUT})
if(MUSIC)
	set(_music_note "${_ntracks} music tracks")
else()
	set(_music_note "no music")
endif()
message(STATUS "q1psx disc: ${_nmaps} maps (${_nzoned} in zones), ${_music_note}")
if(MUSIC AND _ntracks EQUAL 0)
	message(WARNING "q1psx disc: no music cooked (cooked/spu: QUAKE_MUSIC, COOK_MUSIC, ffmpeg)")
endif()
