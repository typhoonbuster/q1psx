/* engine/load.ld: the load overlay's sections (docs/porting-notes.md §90.7):
   the boot's inits and a level's load, run only from main's start and from
   load_level, linked at the level arena's top under the stack (engine/
   q1psx.ld's .text.load, OVL_BASE / OVL_ROOM from CMakeLists.txt) and read
   from LOAD.BIN there before they run; the room is the stream's pool's for
   the rest of the level. Every function here is called by main's boot, by
   load_level, or by another of these only, and none has its address taken
   but the load's two callbacks (scripts/ovlcheck.py checks it at the link).
   A function that gains a caller in play must leave this list. */
	*/world.c.obj(.text.unlikely.world_load)
	*/bmfrag.c.obj(.text.bmf_reserve)
	*/zone.c.obj(.text.unlikely.zone_preload)
	*/gdata.c.obj(.text.unlikely.gdata_load)
	*/stream.c.obj(.text.unlikely.stream_init)
	*/stream.c.obj(.text.unlikely.stream_map_start)
	*/stream.c.obj(.text.unlikely.stream_level)
	*/snd.c.obj(.text.unlikely.snd_map_start)
	*/zone.c.obj(.text.unlikely.zone_map_start)
	*/main.c.obj(.text.unlikely.loading_screen)
	*/main.c.obj(.text.unlikely.load_level_main)
	*/ents.c.obj(.text.unlikely.ents_load)
	*/render.c.obj(.text.unlikely.r_init)
	*libpsxspu_exe_gprel.a:(.text.SpuInit)
	*/bmfrag.c.obj(.text.model_d2)
	*/monsters.c.obj(.text.unlikely.monsters_map_start)
	*/bmfrag.c.obj(.text.build_nodes)
	*/ents.c.obj(.text.unlikely.ents_trim)
	*/world.c.obj(.text.unlikely.world_scan_maps)
	*/world.c.obj(.text.vram_upload)
	*/snd.c.obj(.text.snd_stop_all)
	*/snd.c.obj(.text.unlikely.snd_init)
	*/render.c.obj(.text.r_bind_world)
	*/dlight.c.obj(.text.unlikely.dl_init)
	*/cd.c.obj(.text.cd_list_dirs)
	*/cd.c.obj(.text.cd_list_dir)
	*libpsxgte_exe_gprel.a:(.text.InitGeom)
	*/music.c.obj(.text.music_level)
	*/bmfrag.c.obj(.text.model_nodes)
	*/world.c.obj(.text.world_level_mask)
	*/mdl.c.obj(.text.mdl_reset)
	*/ai.c.obj(.text.ai_map_start)
	*/game.c.obj(.text.unlikely.game_init)
	*/mem.c.obj(.text.mem_game_alloc)
	*/music.c.obj(.text.unlikely.music_init)
	*/world.c.obj(.text.load_poll)
