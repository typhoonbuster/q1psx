# q1psx — the CD image's mkpsxiso project, written when the image is built
# (CMakeLists.txt's iso target runs it), right after the cook: the files a
# cook adds (a new format's, the music's streams, the menu's bank and
# background) go on the disc at the first build. (Listed at CMake's
# configure step, they would miss that build: the cook runs after it.)
#
#   cmake -DROOT=<source dir> -DOUT=<xml> -DTEXTURE_LEVELS=low,medium,high
#         -DGEOMETRY_LEVELS=low,high -DMUSIC=0|1 -DMUSIC_MODE=xa|spu
#         [-DPAK0=<pak0.pak>] -DSDK_VERSION=<v> -P scripts/cd_image.cmake
#
# iso/iso.xml's slots: @GAME_ENTRY@ (the game data per model level, the
# console's background), @STREAM_ENTRY@ (the streamed assets per model
# level), @COOKED_ENTRIES@ (\MAPS: every map at the edition's texture and
# geometry levels, its models, its sound bank, the menu's bank),
# @MUSIC_ENTRIES@ (\MUSIC: the XA streams, or the SPU music's files),
# @PAK0_ENTRY@.

string(REPLACE "," ";" _texture_levels "${TEXTURE_LEVELS}")
string(REPLACE "," ";" _geometry_levels "${GEOMETRY_LEVELS}")
set(_cooked_dir ${ROOT}/cooked)

set(GAME_ENTRY "")
file(GLOB _games ${_cooked_dir}/id1/game_*.dat)
list(SORT _games)
foreach(_src ${_games})                                 # game_2.dat -> GAME_2.DAT
	get_filename_component(_base "${_src}" NAME)
	string(TOUPPER "${_base}" _name)
	string(APPEND GAME_ENTRY "<file name=\"${_name}\" type=\"data\" source=\"${_src}\" />\n\t\t\t")
endforeach()
# the console's background under the release's menu before a map is loaded
# (formats/qmenu.h)
if(EXISTS ${_cooked_dir}/id1/conback.dat)
	string(APPEND GAME_ENTRY "<file name=\"CONBACK.DAT\" type=\"data\" source=\"${_cooked_dir}/id1/conback.dat\" />\n\t\t\t")
endif()

set(STREAM_ENTRY "")
file(GLOB _streams ${_cooked_dir}/id1/stream_*.dat)
list(SORT _streams)
foreach(_src ${_streams})                               # stream_2.dat -> STREAM_2.DAT
	get_filename_component(_base "${_src}" NAME)
	string(TOUPPER "${_base}" _name)
	string(APPEND STREAM_ENTRY "<file name=\"${_name}\" type=\"data\" source=\"${_src}\" />\n\t\t\t")
endforeach()

set(COOKED_ENTRIES "")
set(_maps "")
set(_level_names low medium high ultra)
file(GLOB _cooked ${_cooked_dir}/id1/*.wld ${_cooked_dir}/id1/*.tex ${_cooked_dir}/id1/*.tx4
                   ${_cooked_dir}/id1/*.mdl ${_cooked_dir}/id1/*.snd)
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
	list(GET _level_names ${CMAKE_MATCH_3} _level)
	if(CMAKE_MATCH_2 STREQUAL "t" AND NOT "${_level}" IN_LIST _texture_levels)
		continue()                                      # (another edition's level)
	endif()
	if(CMAKE_MATCH_2 STREQUAL "g" AND NOT "${_level}" IN_LIST _geometry_levels)
		continue()
	endif()
	string(TOUPPER "${_base}${_ext}" _name)             # E1M1_G2.WLD
	string(APPEND COOKED_ENTRIES "<file name=\"${_name}\" type=\"data\" source=\"${_src}\" />\n\t\t\t")
	list(APPEND _maps "${_map}")
endforeach()
list(REMOVE_DUPLICATES _maps)
list(LENGTH _maps _nmaps)

# the music (docs/porting-notes.md §48): the XA streams (mkpsxiso "mixed"
# files) or the SPU music's
set(MUSIC_ENTRIES "")
set(_ntracks 0)
if(MUSIC)
	if(MUSIC_MODE STREQUAL "spu")
		file(GLOB _music ${_cooked_dir}/spu/spu_*.dat)
		set(_type data)
	else()
		file(GLOB _music ${_cooked_dir}/xa/xa_*.str)
		set(_type mixed)
	endif()
	list(SORT _music)
	foreach(_src ${_music})                             # xa_04.str -> MUSIC/XA_04.STR
		get_filename_component(_base "${_src}" NAME)
		string(TOUPPER "${_base}" _name)
		string(APPEND MUSIC_ENTRIES "<file name=\"${_name}\" type=\"${_type}\" source=\"${_src}\" />\n\t\t\t")
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
	set(_music_note "${_ntracks} music tracks (${MUSIC_MODE})")
else()
	set(_music_note "no music")
endif()
message(STATUS "q1psx disc: ${_nmaps} maps, ${_music_note}")
if(MUSIC AND _ntracks EQUAL 0)
	message(WARNING "q1psx disc: no music cooked (cooked/xa or cooked/spu: QUAKE_MUSIC, COOK_MUSIC, ffmpeg)")
endif()
