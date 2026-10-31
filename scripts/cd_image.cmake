# q1psx — the CD image's mkpsxiso project, written when the image is built
# (CMakeLists.txt's iso target runs it), right after the cook: the files a
# cook adds (a new format's, the music's streams, the menu's bank and
# background) go on the disc at the first build. (Listed at CMake's
# configure step, they would miss that build: the cook runs after it.)
#
#   cmake -DROOT=<source dir> -DOUT=<xml> -DMUSIC=0|1
#         [-DPAK0=<pak0.pak>] -DSDK_VERSION=<v> -P scripts/cd_image.cmake
#
# The disc's layout (docs/porting-notes.md §84): the root's files (the
# executable, the game data, the console's background, the pool of streamed
# assets STREAM_0.DAT, the menu's bank), then one directory a level, its
# files in the order the engine's load reads them, so that a load is one
# sweep of the head and every read in play a hop within the level's
# section: its sound bank, its 4-bit textures, its
# world (a map the cook split takes its skeleton as
# <MAP>_G0.WLD and its chunks as <MAP>_Z2.ZON, §57), its own models, its
# own file of the streamed assets (<MAP>_S0.DAT), and with MUSIC its copy
# of its CD track (SPU_<nn>.DAT: tracks 4 to 11 go with the maps that play
# them; tracks 2 and 3, the title's and the intermission's, once in
# \MUSIC after the levels). The levels' directories sit under \LVL: the
# root's records must fit its first sector, the only one the BIOS reads,
# with SYSTEM.CNF and the executable in it. iso.xml's slots: @ROOT_ENTRIES@,
# @LEVEL_DIRS@ (inside <dir name="LVL">), @MUSIC_ENTRIES@, @PAK0_ENTRY@.

set(_cooked_dir ${ROOT}/cooked)

set(ROOT_ENTRIES "")
# the load overlay (docs/porting-notes.md §90.7): the boot's copy in the
# root, a copy first in every level's directory (its load reads it first)
string(APPEND ROOT_ENTRIES "<file name=\"LOAD.BIN\" type=\"data\" source=\"load.bin\" />\n\t\t\t")
if(EXISTS ${_cooked_dir}/id1/game_0.dat)
	string(APPEND ROOT_ENTRIES "<file name=\"GAME_0.DAT\" type=\"data\" source=\"${_cooked_dir}/id1/game_0.dat\" />\n\t\t\t")
endif()
# the console's background under the release's menu before a map is loaded
# (formats/qmenu.h)
if(EXISTS ${_cooked_dir}/id1/conback.dat)
	string(APPEND ROOT_ENTRIES "<file name=\"CONBACK.DAT\" type=\"data\" source=\"${_cooked_dir}/id1/conback.dat\" />\n\t\t\t")
endif()
# the pool of streamed assets: its directory is every level's, read at boot
# (engine/stream.c), and a level without its own file reads it
if(EXISTS ${_cooked_dir}/id1/stream_0.dat)
	string(APPEND ROOT_ENTRIES "<file name=\"STREAM_0.DAT\" type=\"data\" source=\"${_cooked_dir}/id1/stream_0.dat\" />\n\t\t\t")
endif()
if(EXISTS ${_cooked_dir}/id1/menu.snd)
	string(APPEND ROOT_ENTRIES "<file name=\"MENU.SND\" type=\"data\" source=\"${_cooked_dir}/id1/menu.snd\" />\n\t\t\t")
endif()

# the maps cooked: those with a sound bank
file(GLOB _banks ${_cooked_dir}/id1/*.snd)
list(SORT _banks)
set(_maps "")
foreach(_src ${_banks})
	get_filename_component(_map "${_src}" NAME_WE)      # e1m1
	if(NOT _map STREQUAL "menu")
		list(APPEND _maps "${_map}")
	endif()
endforeach()
list(LENGTH _maps _nmaps)

# a map's CD track: its bank's cd_track (formats/qsound.h qs_header, at
# byte 28, little-endian)
function(_map_track _map _out)
	file(READ ${_cooked_dir}/id1/${_map}.snd _hex OFFSET 28 LIMIT 1 HEX)
	math(EXPR _t "0x${_hex}")
	set(${_out} ${_t} PARENT_SCOPE)
endfunction()

set(LEVEL_DIRS "")
set(_nzoned 0)
set(_ncopies 0)
set(_nsections 0)
foreach(_map ${_maps})
	string(TOUPPER "${_map}" _MAP)
	set(_entries "")
	# in the load's order: the load's code, the bank, the textures, the world, the models
	string(APPEND _entries "<file name=\"LOAD.BIN\" type=\"data\" source=\"load.bin\" />\n\t\t\t\t")
	string(APPEND _entries "<file name=\"${_MAP}.SND\" type=\"data\" source=\"${_cooked_dir}/id1/${_map}.snd\" />\n\t\t\t\t")
	if(EXISTS ${_cooked_dir}/id1/${_map}_t2.tx4)
		string(APPEND _entries "<file name=\"${_MAP}_T2.TX4\" type=\"data\" source=\"${_cooked_dir}/id1/${_map}_t2.tx4\" />\n\t\t\t\t")
	endif()
	set(_wld ${_cooked_dir}/id1/${_map}_g0.wld)
	set(_zon "")
	if(EXISTS ${_cooked_dir}/id1/${_map}_g0.skl AND EXISTS ${_cooked_dir}/id1/${_map}_z2.zon)
		set(_wld ${_cooked_dir}/id1/${_map}_g0.skl)         # the skeleton, and its chunks
		set(_zon ${_cooked_dir}/id1/${_map}_z2.zon)
		math(EXPR _nzoned "${_nzoned} + 1")
	endif()
	if(EXISTS ${_wld})
		string(APPEND _entries "<file name=\"${_MAP}_G0.WLD\" type=\"data\" source=\"${_wld}\" />\n\t\t\t\t")
	endif()
	if(EXISTS ${_cooked_dir}/id1/${_map}_m0.mdl)
		string(APPEND _entries "<file name=\"${_MAP}_M0.MDL\" type=\"data\" source=\"${_cooked_dir}/id1/${_map}_m0.mdl\" />\n\t\t\t\t")
	endif()
	# what the game reads in play, right before the track: the chunks, the
	# level's streamed assets
	if(_zon)
		string(APPEND _entries "<file name=\"${_MAP}_Z2.ZON\" type=\"data\" source=\"${_zon}\" />\n\t\t\t\t")
	endif()
	if(EXISTS ${_cooked_dir}/id1/${_map}_s0.dat)
		string(APPEND _entries "<file name=\"${_MAP}_S0.DAT\" type=\"data\" source=\"${_cooked_dir}/id1/${_map}_s0.dat\" />\n\t\t\t\t")
		math(EXPR _nsections "${_nsections} + 1")
	endif()
	if(MUSIC)
		_map_track(${_map} _track)
		if(_track GREATER_EQUAL 4 AND _track LESS 100)
			if(_track LESS 10)
				set(_nn "0${_track}")
			else()
				set(_nn "${_track}")
			endif()
			if(EXISTS ${_cooked_dir}/spu/spu_${_nn}.dat)
				string(APPEND _entries "<file name=\"SPU_${_nn}.DAT\" type=\"data\" source=\"${_cooked_dir}/spu/spu_${_nn}.dat\" />\n\t\t\t\t")
				math(EXPR _ncopies "${_ncopies} + 1")
			endif()
		endif()
	endif()
	string(APPEND LEVEL_DIRS "<dir name=\"${_MAP}\">\n\t\t\t\t${_entries}</dir>\n\t\t\t")
endforeach()

# the music once: tracks 2 and 3 (the title's, the intermission's), and
# every track when no level carries copies (§48, §52)
set(MUSIC_ENTRIES "")
set(_ntracks 0)
if(MUSIC)
	file(GLOB _music ${_cooked_dir}/spu/spu_*.dat)
	list(SORT _music)
	foreach(_src ${_music})                             # spu_04.dat -> MUSIC/SPU_04.DAT
		get_filename_component(_base "${_src}" NAME_WE)
		string(REGEX REPLACE "^spu_0?" "" _t "${_base}")
		if(_t LESS 4 OR _nmaps EQUAL 0)
			string(TOUPPER "${_base}.dat" _name)
			string(APPEND MUSIC_ENTRIES "<file name=\"${_name}\" type=\"data\" source=\"${_src}\" />\n\t\t\t")
			math(EXPR _ntracks "${_ntracks} + 1")
		endif()
	endforeach()
endif()

set(PAK0_ENTRY "")
if(PAK0 AND EXISTS "${PAK0}")
	set(PAK0_ENTRY "<file name=\"PAK0.PAK\" type=\"data\" source=\"${PAK0}\" />")
endif()

set(PROJECT_SOURCE_DIR ${ROOT})                         # (iso.xml's own paths)
set(PSN00BSDK_VERSION ${SDK_VERSION})
configure_file(${ROOT}/iso/iso.xml ${OUT})
if(MUSIC)
	set(_music_note "${_ntracks} music tracks once, ${_ncopies} copies in the levels")
else()
	set(_music_note "no music")
endif()
message(STATUS "q1psx disc: ${_nmaps} levels (${_nzoned} in zones, ${_nsections} with their streamed assets), ${_music_note}")
if(MUSIC AND _ntracks EQUAL 0 AND _ncopies EQUAL 0)
	message(WARNING "q1psx disc: no music cooked (cooked/spu: QUAKE_MUSIC, COOK_MUSIC, ffmpeg)")
endif()
