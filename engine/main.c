/*
 * q1psx — boot + main loop.
 *
 * Normal build: a map menu (every map on the disc, the quality levels, the
 * texture depth, dithering, the affine-warp control and the HUD style),
 * then the game (engine/game.c: the player, the weapons) over the
 * asm-rendered world with the HUD and the debug overlay; SELECT + START
 * goes back to the menu. Loads show a progress bar.
 * Bench build (Q1PSX_BENCH, CMake preset "bench"): render every cooked bench
 * viewpoint, print one "bench ..." line each (+ a packet dump of one frame for
 * tools/pktview), then "bench done" — see scripts/bench.sh.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxapi.h>
#include <psxgpu.h>
#include <hwregs_c.h>

#include "bench.h"
#include "bmfrag.h"
#include "cd.h"
#include "ents.h"
#include "game.h"
#include "gdata.h"
#include "hud.h"
#include "level.h"
#include "anim.h"
#include "menu.h"
#include "dlight.h"
#include "mdl.h"
#include "mem.h"
#include "monsters.h"
#include "music.h"
#include "q_fmt.h"
#include "snd.h"
#include "pad.h"
#include "clip.h"
#include "pmove.h"
#include "q_fixed.h"
#include "prof.h"
#include "render.h"
#include "rlist.h"
#include "settings.h"
#include "stream.h"
#include "ui.h"
#include "vid.h"
#include "world.h"
#include "zone.h"

#ifndef Q1PSX_RELEASE
#define Q1PSX_RELEASE 0       /* 1: Quake's menu (engine/menu.c), no debug overlay,
                                 no map menu, no cheats (the release presets) */
#endif

/* ---------------------------------------------------------------- player -- */

typedef struct {
	int32_t pos[3];      /* 1/8 world units (the eye) */
	int     yaw, pitch, roll;  /* binary degrees   */
} camera_t;

static camera_t cam;            /* what the renderer sees this frame     */
static int      game_active;    /* the game runs (the bench renders views) */
static int      in_menu;        /* the map menu is up (normal build)   */
static int      menu_sel;       /* the map highlighted in the menu      */
static int      world_ok;
static int      map_index;      /* into world_map_name(); R2 cycles it */

#ifdef Q1PSX_BENCH
#ifndef Q1PSX_BENCH_T
#define Q1PSX_BENCH_T    SETTINGS_WARP_FLOOR   /* the warp threshold the bench renders with,
                                                  fixed, px (T= scripts/bench.sh) */
#endif
#ifndef Q1PSX_BENCH_DRAWDIST
#define Q1PSX_BENCH_DRAWDIST Q1PSX_DRAWDIST    /* the draw distance (DRAWDIST= scripts/bench.sh) */
#endif
#ifndef Q1PSX_BENCH_FLAT
#define Q1PSX_BENCH_FLAT R_FLAT_FAR            /* the flat shading (FLAT= scripts/bench.sh) */
#endif
#ifndef Q1PSX_BENCH_EFFECTS
#define Q1PSX_BENCH_EFFECTS QL_MEDIUM          /* the effects (EFFECTS= scripts/bench.sh) */
#endif
#ifndef Q1PSX_BENCH_RES
#define Q1PSX_BENCH_RES  0                     /* the 3D view's resolution, r_res_table's
                                                  index (RES= scripts/bench.sh) */
#endif
#ifndef Q1PSX_BENCH_SEQCHK
#define Q1PSX_BENCH_SEQCHK 0                   /* 1: a view by the cooked walk drawn again by the
                                                  tree walk, the stamps compared (SEQCHK= bench.sh) */
#endif
#ifndef Q1PSX_BENCH_WARPDYN
#define Q1PSX_BENCH_WARPDYN 0                  /* 1: the game's dynamic warp control, T
                                                  its floor (WARPDYN= scripts/bench.sh) */
#endif
#endif

/* The settings (engine/settings.c); the bench renders with its own. */
static void apply_settings(void)
{
#ifdef Q1PSX_BENCH
	r_set_res(Q1PSX_BENCH_RES);
	r_set_warp(Q1PSX_BENCH_WARPDYN ? R_WARP_DYNAMIC : R_WARP_FIXED, Q1PSX_BENCH_T);
	vid_set_dither(1);
	settings.level[QL_EFFECTS] = Q1PSX_BENCH_EFFECTS;
	game_set_effects(Q1PSX_BENCH_EFFECTS);
	r_set_options(Q1PSX_BENCH_DRAWDIST, Q1PSX_BENCH_FLAT);
	game_draw_viewmodel = Q1PSX_WEAPON_2D ? 2 : 1;
	game_world_hz = Q1PSX_BENCH_WORLD_HZ;
#else
	settings_apply();
#endif
}

/* A stick through a radial dead zone (DualShocks rest a little off
   centre), the rest of its travel rescaled to 0 .. PM_AXIS; the direction
   is kept. */
#define STICK_DEAD 24

static void stick(int *x, int *y)
{
	int m = (int) q_isqrt32((uint32_t) (*x * *x + *y * *y));
	if (m <= STICK_DEAD) {
		*x = *y = 0;
		return;
	}
	int k = (m - STICK_DEAD) * PM_AXIS / (127 - STICK_DEAD);
	if (k > PM_AXIS)
		k = PM_AXIS;                           /* the square's corners */
	*x = *x * k / m;
	*y = *y * k / m;
}

/* A pad poll -> the game's command for its tick. Scheme B
   (2026-10-17):
     analog pad   left stick walk / strafe (speed follows the stick), right
                  stick look (up / down inverted, finer near the centre);
                  R2 / Square fire, L2 / Cross jump, R1 / L1 next / previous
                  weapon, Triangle walk, Circle level the view, d-pad a
                  weapon group: up shotguns and axe, right nailguns, down
                  launchers, left lightning;
     digital pad  the same buttons; d-pad walk / turn, and with Triangle
                  held strafe / look up and down (Quake's +strafe, +klook);
     SELECT + Cross noclip, SELECT + Square respawn; START the debug
     overlay; SELECT + START the map menu (run_frame).
   A DualShock is switched to analog mode by pad_init (engine/pad.c), so
   the digital scheme is for digital pads only.
   Turning and looking advance per physics tick (one per poll), so their
   speed does not depend on the frame rate. */
static void player_cmd(game_cmd_t *cmd, const pad_t *p)
{
	const int turn = 46, look = 34;          /* buttons: ~4 and ~3 degrees per tick */
	const int stick_turn = 48, stick_look = 36;   /* right stick, full deflection */
	int sel = p->held & PAD_SELECT;
	memset(cmd, 0, sizeof *cmd);
	cmd->weapon_group = -1;

#if !Q1PSX_RELEASE
	if (sel && (p->pressed & PAD_CROSS))  pl.pm.noclip = !pl.pm.noclip;
	if (sel && (p->pressed & PAD_SQUARE)) game_spawn();
#endif

	int lx = p->lx, ly = p->ly, rx = p->rx, ry = p->ry;
	if (p->analog) {                          /* (the sticks: an analog pad's only) */
		stick(&lx, &ly);
		stick(&rx, &ry);
	}

	int yaw = 0, pitch = 0;
	if (p->analog) {
		yaw   -= stick_turn * rx * (rx < 0 ? -rx : rx) / (PM_AXIS * PM_AXIS);
		pitch -= stick_look * ry * (ry < 0 ? -ry : ry) / (PM_AXIS * PM_AXIS);   /* inverted */
		cmd->fwd  = -ly;                                /* stick up = forward */
		cmd->side = lx;
		if (p->held & PAD_UP)    cmd->fwd  =  PM_AXIS;     /* (the d-pad too) */
		if (p->held & PAD_DOWN)  cmd->fwd  = -PM_AXIS;
		if (p->held & PAD_RIGHT) cmd->side =  PM_AXIS;
		if (p->held & PAD_LEFT)  cmd->side = -PM_AXIS;
		if (!sel) {
			cmd->fire = (p->held & (PAD_R2 | PAD_SQUARE)) != 0;
			cmd->jump = (p->held & (PAD_L2 | PAD_CROSS)) != 0;   /* held: the swim up (PlayerJump) */
			cmd->weapon_next = (p->pressed & PAD_R1) != 0;
			cmd->weapon_prev = (p->pressed & PAD_L1) != 0;
			cmd->walk   = (p->held & PAD_TRIANGLE) != 0;
			cmd->centre = (p->pressed & PAD_CIRCLE) != 0;
			if (p->pressed & PAD_UP)    cmd->weapon_group = 0;
			if (p->pressed & PAD_RIGHT) cmd->weapon_group = 1;
			if (p->pressed & PAD_DOWN)  cmd->weapon_group = 2;
			if (p->pressed & PAD_LEFT)  cmd->weapon_group = 3;
		}
	} else {
		if (p->held & PAD_TRIANGLE) {                  /* +strafe, +klook */
			if (p->held & PAD_LEFT)  cmd->side = -PM_AXIS;
			if (p->held & PAD_RIGHT) cmd->side =  PM_AXIS;
			if (p->held & PAD_UP)    pitch -= look;
			if (p->held & PAD_DOWN)  pitch += look;
		} else {
			if (p->held & PAD_UP)    cmd->fwd  =  PM_AXIS;
			if (p->held & PAD_DOWN)  cmd->fwd  = -PM_AXIS;
			if (p->held & PAD_LEFT)  yaw += turn;
			if (p->held & PAD_RIGHT) yaw -= turn;
		}
		if (!sel) {
			cmd->fire = (p->held & (PAD_R2 | PAD_SQUARE)) != 0;
			cmd->jump = (p->held & (PAD_L2 | PAD_CROSS)) != 0;   /* held: the swim up (PlayerJump) */
			cmd->weapon_next = (p->pressed & PAD_R1) != 0;
			cmd->weapon_prev = (p->pressed & PAD_L1) != 0;
			cmd->centre = (p->pressed & PAD_CIRCLE) != 0;
		}
	}
	if (pl.dead)
		yaw = pitch = 0;
	pl.pm.yaw   += yaw;
	pl.pm.pitch += pitch;
	if (pl.pm.pitch >  1012) pl.pm.pitch =  1012;    /* +-89 degrees */
	if (pl.pm.pitch < -1012) pl.pm.pitch = -1012;
}

/* The simulation is tick-based (Quake): a tick each vblank's poll
   (pad_frame), with that poll's held buttons and presses. The player's part
   of each tick turns the view (Quake's client turns each frame:
   CL_AdjustAngles), moves and fires; the world's runs at its own rate
   (game_world_hz, engine/ents.c ents_tick). The camera at the last tick's
   view. */
static void player_update(const pad_t *polls, int n)
{
	uint32_t t0 = prof_now();
	for (int i = 0; i < n; i++) {
		game_cmd_t cmd;
		player_cmd(&cmd, &polls[i]);
		game_tick(&cmd);
	}
	game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
	prof_add(PROF_SIM, t0, prof_now());
}

/* ------------------------------------------------------- loading bar -- */

static int load_map;             /* what the bar is loading */

/* A bordered TILE in the overlay bucket. */
static void ui_rect(int x, int y, int w, int h, int r, int g, int b)
{
	TILE *t = vid_alloc(sizeof *t);
	if (!t)
		return;
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
	vid_link(OT_OVERLAY, t);
}

/* One frame of the loading screen: the map, the level, a bar and the
   percentage (texture file + world file, in bytes read from the CD). */
Q_COLD static void loading_screen(uint32_t done, uint32_t total)
{
	enum { BX = 40, BY = 116, BW = 240, BH = 10 };
	char line[48];
	int  fill = total ? (int) (done * BW / total) : 0;   /* < 2 MB: no overflow */

	q_snprintf(line, sizeof line, "loading %s", world_map_name(load_map));
	vid_text(BX, BY - 16, line);
	q_snprintf(line, sizeof line, "%3d %%", total ? (int) (done * 100 / total) : 0);
	vid_text(BX + BW - 40, BY + BH + 8, line);
	ui_rect(BX + 1, BY + 1, fill > BW - 2 ? BW - 2 : fill, BH - 2, 230, 170, 60);  /* bar */
	ui_rect(BX, BY, BW, BH, 70, 74, 90);                                        /* frame */
	vid_flip();
}

#if defined(Q1PSX_BENCH) && !Q1PSX_BENCH_PLAY
#define LOAD_PROGRESS NULL         /* the bench measures; no screen */
#else
#define LOAD_PROGRESS loading_screen
#endif

/* the level arena's bytes the streamed assets leave free (a transient
   load's room: vram_upload's padded copies) */
#define STREAM_KEEP (8 * 1024)

/* The load overlay (docs/porting-notes.md §90.7): the boot's inits and a
   level's load (engine/load.ld), linked at the level arena's top under the
   stack (engine/q1psx.ld: __load_start to __load_end, OVL_ROOM of room),
   read from LOAD.BIN before any of it runs: the root's copy at boot, the
   level's directory's at its load (first in the directory: no seek). The
   stream's pool takes the room after the load (stream_load_done). */
extern uint8_t __load_start[], __load_end[];

Q_COLD static void ovl_load(const char *dir)
{
	char name[32];
	uint32_t lba, size;
	q_snprintf(name, sizeof name, "%s\\LOAD.BIN;1", dir);
	if (!cd_find(name, &lba, &size) && !cd_find("\\LOAD.BIN;1", &lba, &size)) {
		printf("load: no LOAD.BIN\n");
		vid_fatal();
	}
	if (size != (uint32_t) (__load_end - __load_start) ||
	    !cd_read_now((int32_t) lba, (size + 2047) / 2048, __load_start, CDW_FILE, 0)) {
		printf("load: LOAD.BIN %u B, the link's %u, or unread\n", (unsigned) size,
		       (unsigned) (__load_end - __load_start));
		vid_fatal();
	}
	FlushCache();                               /* (the cache's lines of the last load) */
}

static void load_level_main(int map);

/* Load `map` at the settings' levels (textures, geometry, models), or at
   the highest lower geometry that fits the level arena; the game data's
   models at the models' level. Sets world_ok and binds the renderer. */
static uint32_t stack_untouched(void);

#ifdef Q1PSX_BENCH
uint32_t stream_asset_size(int a);
/* everything the level's entities may ask the stream for: the streamed
   models of its items, each monster kind's model, gibs and head, the
   backpack; their RAM once loaded (kept) and while loading (whole) */
static void bench_level_wants(void)
{
	static uint8_t seen[512];
	memset(seen, 0, sizeof seen);
	uint32_t kept = 0, whole = 0, kinds = 0, monsters = 0, items = 0;
	int n = 0;
	for (int i = 2; i < num_edicts; i++) {
		const edict_t *e = &edicts[i];
		if (!e->inuse)
			continue;
		int as[MK_ASSETS + 1], na = 0;
		if (e->masset >= 0) as[na++] = e->masset;
		if (e->mdef) {
			monsters++;
			for (int k = 0; k < MK_ASSETS; k++)
				if (monsters_kind_asset(e->mdef->kind, k) >= 0)
					as[na++] = monsters_kind_asset(e->mdef->kind, k);
			if (!(kinds & 1u << e->mdef->kind)) kinds |= 1u << e->mdef->kind;
		} else if (e->masset >= 0 || e->item_model >= 0)
			items++;
		for (int k = 0; k < na; k++)
			if (as[k] < 512 && !seen[as[k]]) {
				seen[as[k]] = 1;
				kept += stream_kept(as[k]);
				whole += stream_asset_size(as[k]);
				n++;
			}
	}
	for (int k = 0; k < MK_COUNT; k++) {                /* each kind's own assets */
		uint32_t kk = 0, kw = 0;
		for (int j = 0; j < MK_ASSETS; j++) {
			int a = monsters_kind_asset(k, j);
			if (a >= 0) { kk += stream_kept(a); kw += stream_asset_size(a); }
		}
		if (kk) printf("level wants kind %d: %u KB kept, %u KB while loading\n", k, (unsigned) (kk / 1024), (unsigned) (kw / 1024));
	}
	int bp = stream_find("progs/backpack.mdl");
	if (bp >= 0 && bp < 512 && !seen[bp]) { kept += stream_kept(bp); whole += stream_asset_size(bp); n++; }
	printf("level wants %s skill %d: %u monsters of %d kinds, %u items; %d streamed assets, %u KB kept, %u KB while loading\n",
	       world_map_name(load_map), game_skill, (unsigned) monsters, __builtin_popcount(kinds), (unsigned) items, n,
	       (unsigned) (kept / 1024), (unsigned) (whole / 1024));
}
#endif

Q_COLD static int load_level(int map)
{
	world_ok = 0;
	load_map = map;
	cd_log_mark(world_map_name(map));           /* (the bench's drive journal: the load's phase) */
	stream_abort();                             /* (the arena is reset, the drive taken) */
	zone_abort();
	music_stop();                               /* one drive: the reads need it */
	ui_conback_forget();                        /* (the map's textures over it) */
	char dir[16];                               /* the level's section: its directory read
	                                               once, every file of the load found in it
	                                               (docs/porting-notes.md §84) */
	q_snprintf(dir, sizeof dir, "\\LVL\\%s", world_map_name(map));
	cd_cache_dir(dir);
	ovl_load(dir);                              /* the load's code (§90.7): the rest runs from it */
	load_level_main(map);
#ifdef Q1PSX_BENCH
	if (world_ok)
		bench_level_wants();
#endif
	printf("stack: %u bytes never reached\n", (unsigned) stack_untouched());
	return world_ok;
}

/* the load from its overlay on: the bank, the world, the entities, the
   streamed assets' pool and its preload */
Q_COLD static void load_level_main(int map)
{
	music_level(world_map_name(map));           /* (its copy of the track) */
	snd_load_bank(world_map_name(map));         /* (through the level arena, first) */
	mem_level_cap(MEM_LEVEL_WORLD);             /* (the world's room as before §45) */
	world_ok = world_load(map, settings.level[QL_TEXTURES], settings.level[QL_GEOMETRY],
	                      settings.level[QL_MODELS], LOAD_PROGRESS);
	mem_level_cap(mem_level_bytes());
	if (world_ok) {
		r_bind_world();
		stream_level(world_map_name(map));      /* the streamed assets from the level's own file */
		bmf_level();                            /* (the brush models' pieces: anew) */
		ents_load();                            /* the map's entities */
		monsters_map_start();
		ents_spawn(1);
		ents_trim();                            /* (the edicts' unused end back: the stream's) */
		bmf_reserve();                          /* (their pieces' room, the cook's: before
		                                           the stream's) */
		game_ws_sync();                         /* (the 2D gun's packets asked for: first) */
		stream_map_start(STREAM_KEEP);          /* the arena's rest: the streamed assets */
		bmf_load();                             /* (the drawn ones cut where they stand) */
		snd_map_start();                        /* (before any residency pass: it turns the
		                                           bank's static sources into the entries
		                                           snd_stream_wants reads, with this map's
		                                           voices; until then they are the file's,
		                                           20 bytes each: a garbage reach of 0 on
		                                           E2M7 divided by zero) */
		if (zone_on()) {                        /* a zoned world: the spawn's zone and its
		                                           nearest read, that zone made current */
			int32_t eye[3];
			for (int k = 0; k < 3; k++)
				eye[k] = (world.has_spawn ? world.spawn[k] : 0) * CLIP_SUB;
			eye[2] += 22 * CLIP_SUB;             /* (the view's height) */
			zone_preload(eye);
		}
		stream_preload();                       /* what the spawn's surroundings want */
		stream_load_done();                     /* (the overlay's room: the pool's from here;
		                                           nothing of the overlay runs past this) */
		game_ws_sync();                         /* (the 2D gun's packets, read: linked) */
		anim_reset();
		hud_invalidate();                       /* (the loading screen drew over the bars) */
		music_play(snd_cd_track());
	}
}

/* --------------------------------------------------------------- frame -- */

static int menu_shown = Q1PSX_RELEASE;     /* Quake's menu drawn (the release build; the bench's check) */

static void frame_work(void)
{
	if (!world_ok)
		return;
	r_set_view(cam.pos, cam.yaw, cam.pitch, cam.roll);
	anim_frame(game_tick_count);            /* the animated textures' frames */
	uint32_t t0 = prof_now();
	rl_begin();                             /* the render list: the map's brush
	                                           models' pieces first (the others
	                                           take their places among them),
	                                           the game's projectiles, effects
	                                           and view model, the items and
	                                           monsters, the particles; their
	                                           matrices come from this frame's
	                                           view */
#if !defined(Q1PSX_BENCH_NOENTS) || !Q1PSX_BENCH_NOENTS
	ents_render_bmodels();
#endif
	if (game_active)
		game_render();
#if !defined(Q1PSX_BENCH_NOENTS) || !Q1PSX_BENCH_NOENTS
	ents_render();
#endif
	if (game_active)
		game_render_particles();
#if defined(Q1PSX_BENCH) && Q1PSX_ORDER_C == 2
	if (ents_debug)                         /* (the asm's places checked at made-up points) */
		rl_order_probe();
#endif
	prof_add(PROF_ENTS, t0, prof_now());    /* (the dlights' share counted apart) */
	r_ui_room = menu_shown && menu_active() ? R_MENU_ROOM : 0;   /* (the menu's packets, after
	                                           the world's: a full arena left it none) */
	r_frame();
	rl_end();
	if (game_active)
		game_render_done();
}

static int overlay_on = !Q1PSX_RELEASE;   /* START toggles (the debug build) */
static int release_in_game;                /* the release build: a game begun (New Game) */

static void draw_overlay(void)
{
	uint32_t t0 = prof_now();
	char line[384], sticks[40] = "";

	if (pad.analog)                    /* raw, before the dead zone */
		q_snprintf(sticks, sizeof sticks, "\nsticks L %4d %4d  R %4d %4d", pad.lx, pad.ly, pad.rx, pad.ry);

	char levels[24] = "no world";               /* textures, geometry, models, effects */
	if (world_ok)
		q_snprintf(levels, sizeof levels, "T%d G%d M%d E%d", world.level[QL_TEXTURES], world.level[QL_GEOMETRY],
		         world.level[QL_MODELS], settings.level[QL_EFFECTS]);
	int n = q_snprintf(line, sizeof line,
	         "q1psx  %s %s  ~%u fps\n"
	         "frame %6u cyc  gpu wait %u  arena %u KB\n",
	         world_map_name(map_index), levels,
	         vid_stats.vbl ? 60 / vid_stats.vbl : 60,
	         prof_stage[PROF_FRAME], vid_stats.sync_cycles, mem_level_used() / 1024);
#if Q1PSX_PROFILE   /* the renderer's counters / stage timers (release: none) */
	n += q_snprintf(line + n, sizeof line - n,
	         "walk %u  mark %u  faces %u\n"
	         "nodes %u recs %u verts %u\n"
	         "polys %u  near %u  ",
	         prof_stage[PROF_R_WALK], prof_stage[PROF_R_SETUP], prof_stage[PROF_R_BLOCK],
	         prof_count[PROF_N_NODES], prof_count[PROF_N_BLOCKS], prof_count[PROF_N_VERTS],
	         prof_count[PROF_N_POLYS], prof_count[PROF_N_SLOW]);
#endif
	q_snprintf(line + n, sizeof line - n,
	         "%u B%s\n"
	         "warp T %d (from %d)  ~%u tris%s\n"
	         "pos %d %d %d  sim %u  items %d\n"
	         "pad %s type %d btn %04x%s",
	         vid_stats.pkt_bytes, vid_stats.dropped ? " FULL" : "",
	         r_warp.t_eff, r_warp.t, (unsigned) r_warp.tris, settings.dither ? "" : "  no dither",
	         cam.pos[0] / CLIP_SUB, cam.pos[1] / CLIP_SUB, cam.pos[2] / CLIP_SUB,
	         prof_stage[PROF_SIM], rl_num_items(),
	         pad.connected ? "ok" : "--", pad.type, pad.held, sticks);
	vid_text(8, 8, line);
	prof_add(PROF_OVERLAY, t0, prof_now());
}

/* Run one frame; prof_stage[] / prof_count[] describe it until the next call.
   dump: 1 prints the frame's display list (vid_dump), 2 only its hash. */
#if Q1PSX_BENCH_PLAY
static int play_trace;                     /* the play bench: the first frames' stages, for a hang's place */
#define PLAY_STEP(s) do { if (play_trace > 0) printf("play step %s\n", s); } while (0)
#else
#define PLAY_STEP(s) ((void) 0)
#endif

static void run_frame(int interactive, int dump)
{
#ifdef Q1PSX_BENCH
	if (world_ok)
		zone_bench_frame(cam.pos);          /* (the bench: the view's zone read at once,
		                                       before the frame, its cycles and the
		                                       stream's turn) */
#endif
	prof_frame_reset();
	uint32_t t0 = prof_now();
	if (world_ok && game_active) {
		zone_frame();                       /* (the world's chunks first: the drive) */
		stream_frame();                     /* (between frames: the pools may move) */
	}
	if (interactive) {
		PLAY_STEP("pad");
		pad_t ticks[PAD_MAX_TICKS];
		int nticks = pad_frame(ticks, PAD_MAX_TICKS);
#if Q1PSX_RELEASE
		if (menu_active()) {                  /* the world paused under the menu */
			menu_key(&pad);
			nticks = 0;
		} else if (pad.pressed & PAD_START) {
			menu_open();                      /* togglemenu */
			nticks = 0;
		}
#else
		if ((pad.held & (PAD_SELECT | PAD_START)) == (PAD_SELECT | PAD_START) &&
		    (pad.pressed & (PAD_SELECT | PAD_START))) {
			in_menu  = 1;                      /* SELECT + START: the map menu */
			menu_sel = map_index;
			return;
		}
		if ((pad.pressed & PAD_START) && !(pad.held & PAD_SELECT))
			overlay_on = !overlay_on;
#endif
		PLAY_STEP("player");
		if (world_ok)                         /* (none before the release's New Game) */
			player_update(ticks, nticks);
	}
	if (interactive) {                      /* a trigger_changelevel's map, between frames */
		char next[16];
		if (level_change_wanted(next)) {
			int m = world_find_map(next);
			if (m < 0) {
				printf("level: %s is not on this disc\n", next);
			} else {
				game_save_parms();
				map_index = m;
				if (load_level(m))
					game_spawn_level();
			}
		}
	}
	PLAY_STEP("render");
#ifdef Q1PSX_BENCH
	if (dump) {
		anim_invalidate();                  /* a dump shows the textures' frames, the */
		hud_invalidate();                   /* whole HUD and the 2D gun's texels (pktview */
		game_ws_invalidate();               /* sees only dumped frames) */
	}
#endif
	frame_work();
	if (game_active) {
		uint32_t t1 = prof_now();
		if (world_ok) {
			game_view_tint();               /* the contents' colour over the view */
			PLAY_STEP("hud");
			hud_draw();
		}
#if Q1PSX_RELEASE
		else if (!menu_active())
			ui_conback();                   /* no world: the forced console's background
			                                   (SCR_DrawConsole; Sbar_Draw draws nothing
			                                   under a full-screen console) */
#endif
		if (menu_shown)
			menu_draw();                    /* M_Draw: over everything */
		uint32_t t2 = prof_now();
		PLAY_STEP("snd");
		snd_update(cam.pos, cam.yaw, cam.pitch, cam.roll, game_tick_count);
		uint32_t t3 = prof_now();
		prof_add(PROF_HUD, t1, t2);
		prof_add(PROF_SND, t2, t3);
	}
	prof_add(PROF_FRAME, t0, prof_now());
	if (interactive) {
		PLAY_STEP("music");
		music_tick();
	}
	if (interactive && overlay_on) {
		PLAY_STEP("overlay");
		draw_overlay();
	}
	if (dump) {
#ifdef Q1PSX_BENCH
		if (dump != 2)
			stream_dump();              /* (pktview: the streamed skins' places) */
#endif
		vid_dump(dump == 2);
	}
	PLAY_STEP("flip");
	uint32_t share = (uint32_t) vid_frame_vbl();
	vid_flip();
	r_warp_feedback(prof_stage[PROF_FRAME] + prof_stage[PROF_OVERLAY], vid_stats.vbl, share);
#if Q1PSX_BENCH_PLAY
	if (play_trace > 0) { printf("play step done\n"); play_trace--; }
#endif
}

/* ------------------------------------------------------------ map menu -- */

#ifndef Q1PSX_BENCH

#define MENU_ROWS 11       /* three columns of 11: Quake's 32 maps */

static int         complex_map = -1;      /* the "(heavy)" one            */
static const char *menu_msg = "";

/* START is where Quake begins (the skill is picked there). The heaviest
   view of all the cooked maps is e3m4's (540 k cycles at high,
   BENCH_MAPS=1 scripts/bench.sh, R6.2); an episode-1 disc falls back to
   e1m8's (419 k). */
static void mapmenu_init(void)
{
	static const char *const heaviest[] = { "E3M4", "E1M8" };
	menu_sel = 0;
	for (int m = 0; m < world_num_maps(); m++)
		if (!strcmp(world_map_name(m), "START"))
			menu_sel = m;
	for (int i = 0; i < 2 && complex_map < 0; i++)
		for (int m = 0; m < world_num_maps(); m++)
			if (!strcmp(world_map_name(m), heaviest[i]))
				complex_map = m;
}

static const char *map_tag(int m)
{
	return m == complex_map ? " (heavy)" : "";
}

/* The options under the map list (engine/settings.h's rows, Quake's
   Graphics page's): L2 / R2 pick one, L1 / R1 change it; what the value
   gains and costs under them. */
static int opt_sel;

static void opt_step(int d)
{
	settings_step(opt_sel, d);
	apply_settings();
}

static void opt_draw(void)
{
	static const char *const label[SET_ROWS] = { "effects", "dither", "hud", "3d res", "dlights", "fps cap", "tick rate" };
	char line[48], val[16];
	for (int o = 0; o < SET_ROWS; o++) {
		settings_value(o, val, sizeof val);
		int x = 16 + (o % 2) * 152, y = 128 + (o / 2) * 10;
		q_snprintf(line, sizeof line, "%-8s< %s >", label[o], val);
		vid_text(x, y, line);
		if (o == opt_sel)
			ui_rect(x - 4, y - 1, 148, 10, 60, 70, 120);
	}
	const char *hint[2];
	settings_hint(opt_sel, hint);
	vid_text(16, 210, hint[0]);
	vid_text(16, 218, hint[1]);
}

/* One frame of the menu: d-pad picks a map, L2 / R2 an option and L1 / R1
   its value, Cross / START loads the map, Circle goes back to the loaded
   one. */
static void mapmenu_frame(void)
{
	hud_invalidate();                           /* (this menu draws over the whole screen) */
	int n = world_num_maps();

	pad_read();
	if (pad.pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L1 | PAD_R1 | PAD_L2 | PAD_R2))
		snd_start(ENT_PLAYER, -1, S_MENU1, cam.pos, 255, ATTN_NORM);   /* S_LocalSound */
	if (pad.pressed & (PAD_CROSS | PAD_START | PAD_CIRCLE))
		snd_start(ENT_PLAYER, -1, S_MENU2, cam.pos, 255, ATTN_NORM);
	if (n) {
		if (pad.pressed & PAD_UP)
			menu_sel = (menu_sel + n - 1) % n;
		if (pad.pressed & PAD_DOWN)
			menu_sel = (menu_sel + 1) % n;
		if (pad.pressed & PAD_RIGHT) {
			if (menu_sel + MENU_ROWS < n)
				menu_sel += MENU_ROWS;
			else if (menu_sel < MENU_ROWS && n > MENU_ROWS)
				menu_sel = n - 1;
		}
		if ((pad.pressed & PAD_LEFT) && menu_sel >= MENU_ROWS)
			menu_sel -= MENU_ROWS;
	}
	if (pad.pressed & PAD_L2)
		opt_sel = (opt_sel + SET_ROWS - 1) % SET_ROWS;
	if (pad.pressed & PAD_R2)
		opt_sel = (opt_sel + 1) % SET_ROWS;
	if (pad.pressed & PAD_L1)
		opt_step(-1);
	if (pad.pressed & PAD_R1)
		opt_step(1);
	if ((pad.pressed & PAD_CIRCLE) && world_ok) {
		in_menu = 0;                             /* back to the loaded map */
		return;
	}
	if (n && (pad.pressed & (PAD_CROSS | PAD_START))) {
		map_index = menu_sel;
		if (load_level(map_index)) {
			game_spawn();
			in_menu  = 0;
			menu_msg = "";
			return;
		}
		menu_msg = "could not load that map";
	}

	char line[48];
	q_snprintf(line, sizeof line, "Q1PSX maps (%d)  in game: SELECT+START", n);
	vid_text(16, 6, line);
	for (int m = 0; m < n; m++) {
		int x = 24 + (m / MENU_ROWS) * 100, y = 18 + (m % MENU_ROWS) * 10;
		q_snprintf(line, sizeof line, "%s%s", world_map_name(m), map_tag(m));
		vid_text(x, y, line);
		if (m == menu_sel)
			ui_rect(x - 4, y - 1, 96, 10, 60, 70, 120);
	}
	if (!n)
		vid_text(24, 20, "no cooked map on the disc");
	opt_draw();
	vid_text(16, 228, menu_msg[0] ? menu_msg : "L2/R2 option  L1/R1 value  X load  O back");
	vid_flip();
}

#endif /* !Q1PSX_BENCH */

/* --------------------------------------------------------------- bench -- */

#ifdef Q1PSX_BENCH


#ifndef Q1PSX_BENCH_VIEWS
#define Q1PSX_BENCH_VIEWS 1   /* 0: physics checks only (VIEWS=0 scripts/bench.sh) */
#endif
#ifndef Q1PSX_BENCH_MAPS
#define Q1PSX_BENCH_MAPS 0    /* 1: every map on the disc (BENCH_MAPS=1 scripts/bench.sh) */
#endif
#ifndef Q1PSX_BENCH_PLAY
#define Q1PSX_BENCH_PLAY 0    /* 1: the maps bench goes through the game's own path too: the
                                 loading screen, then 30 interactive frames (the pad read,
                                 the HUD, the sound, the music) per map (PLAY=1) */
#endif
#ifndef Q1PSX_BENCH_HASHONLY
#define Q1PSX_BENCH_HASHONLY 0  /* 1: the dumps are hashes only (HASHONLY=1 scripts/bench.sh) */
#endif
#define BENCH_DUMP (Q1PSX_BENCH_HASHONLY ? 2 : 1)
#ifndef Q1PSX_BENCH_GAME
#define Q1PSX_BENCH_GAME 0    /* 1: the game: every weapon fired, both HUDs, one frame
                                 dumped each (BENCH_GAME=1 scripts/bench.sh) */
#endif

#define BENCH_WARMUP 8
#define BENCH_FRAMES 64

/* The physics' C reference, linked under these names (CMakeLists.txt). */
static pmove_t player;           /* the scripted path's player */

static void player_spawn(void)
{
	memset(&player, 0, sizeof player);
	player.pos[0] = world.spawn[0] * CLIP_SUB;
	player.pos[1] = world.spawn[1] * CLIP_SUB;
	player.pos[2] = (world.spawn[2] + 1) * CLIP_SUB;   /* Quake drops it to the floor */
	player.yaw    = world.spawn_yaw * ANG_TURN / 360;
}
#if Q1PSX_ENGINE_C
/* the C engine (CMake Q1PSX_ENGINE_C): its physics is the C reference, so
   the checks compare it with itself; their lines say "c-engine" for "asm" */
#define PHYS_ENGINE           "c-engine"
#define clip_trace_c          clip_trace
#define clip_contents_c       clip_contents
#define clip_box_node0_c      clip_box_node0
#define clip_contents_node0_c clip_contents_node0
#define pmove_step_c          pmove_step
static clip_mover_t clip_mover_c;             /* the mover as the checks copied it */

/* the reference's trace from that copy, as the asm build's C one has its
   own (the engine's trace before it may have changed the mover) */
static q_fixed clip_trace_ent_c(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr)
{
	memcpy(&clip_mover, &clip_mover_c, sizeof clip_mover);
	return clip_trace_ent(hull, start, end, tr);
}
#else
#define PHYS_ENGINE "asm"
q_fixed clip_trace_c(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr);
q_fixed clip_trace_ent_c(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr);
int     clip_contents_c(int hull, const int32_t p[3]);
uint32_t clip_box_node0_c(const int32_t mins[3], const int32_t maxs[3]);
int     clip_contents_node0_c(uint32_t node, const int32_t p[3]);
extern clip_mover_t clip_mover_c;
void    pmove_step_c(pmove_t *pm, const pmove_cmd_t *cmd);
#endif

static uint32_t rng_state = 1;

static uint32_t rng(void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return rng_state >> 8;
}

/* a random value in [-r, r] */
static int32_t rng_pm(int32_t r)
{
	return (int32_t) (rng() % (uint32_t) (2 * r + 1)) - r;
}

/* A random stick axis: often centred or full (a d-pad), else anywhere. */
static int rng_axis(void)
{
	switch (rng() & 3) {
	case 0:  return 0;
	case 1:  return PM_AXIS;
	case 2:  return -PM_AXIS;
	default: return rng_pm(PM_AXIS);
	}
}

/* A random point inside the map's bounding box (sub-units); `open` asks for
   one in the empty space of hull 1. */
static void rng_point(int32_t p[3], int open)
{
	const qw_node *r = world.root;
	for (int tries = 0; tries < 200; tries++) {
		for (int k = 0; k < 3; k++)
			p[k] = (r->c[k] + rng_pm(r->h[k])) * (CLIP_SUB / QW_VSCALE);
		if (!open || clip_contents_c(1, p) == QW_CONTENTS_EMPTY)
			return;
	}
}

/* clip.s against clip.c: random sweeps (half from open space, player-move
   sized; the rest anywhere, up to map-crossing lengths; one in 8 on hull 2)
   and random contents queries. Every trace_t field must match. */
static void bench_clip_diff(void)
{
	enum { N = 4000 };
	int      bad = 0, hits = 0, solid = 0;
	uint32_t cyc_asm = 0, cyc_c = 0;
	for (int i = 0; i < N; i++) {
		int32_t s[3], e[3];
		int     hull = (i & 7) == 7 ? 2 : (i & 7) == 3 ? 0 : 1;
		int32_t len  = (i & 1) ? 32 * CLIP_SUB : (i & 2) ? 512 * CLIP_SUB : 4 * CLIP_SUB;
		rng_point(s, !(i & 4));
		for (int k = 0; k < 3; k++)
			e[k] = s[k] + rng_pm(len);
		trace_t ta, tc;
		uint32_t t0 = prof_now();
		clip_trace(hull, s, e, &ta);
		uint32_t t1 = prof_now();
		clip_trace_c(hull, s, e, &tc);
		uint32_t t2 = prof_now();
		cyc_asm += prof_cycles(t0, t1);
		cyc_c   += prof_cycles(t1, t2);
		hits  += tc.fraction < Q_FIX_ONE;
		solid += tc.startsolid;
		if (memcmp(&ta, &tc, sizeof ta)) {
			if (bad++ < 8)
				printf("clipdiff MISMATCH hull %d %d %d %d -> %d %d %d\n"
				       "  asm: ss %d as %d op %d wa %d f %d end %d %d %d n %d %d %d d %d\n"
				       "  c:   ss %d as %d op %d wa %d f %d end %d %d %d n %d %d %d d %d\n",
				       hull, (int) s[0], (int) s[1], (int) s[2], (int) e[0], (int) e[1], (int) e[2],
				       ta.startsolid, ta.allsolid, ta.inopen, ta.inwater, (int) ta.fraction,
				       (int) ta.endpos[0], (int) ta.endpos[1], (int) ta.endpos[2],
				       ta.plane_n[0], ta.plane_n[1], ta.plane_n[2], (int) ta.plane_d,
				       tc.startsolid, tc.allsolid, tc.inopen, tc.inwater, (int) tc.fraction,
				       (int) tc.endpos[0], (int) tc.endpos[1], (int) tc.endpos[2],
				       tc.plane_n[0], tc.plane_n[1], tc.plane_n[2], (int) tc.plane_d);
		}
	}
	/* the box entries (SOLID_BBOX entities: SV_HullForBox), aimed at: sweeps
	   from around each box through its centre and past it, every hull */
	int boxes = 0, box_hits = 0, ent_hits = 0;
	for (int b = 0; b < clip_num_bmodels; b++) {
		const clip_bmodel_t *cb = &clip_bmodels[b];
		if ((uintptr_t) cb->bm > 1 || cb->absmin[0] > cb->absmax[0])
			continue;
		boxes++;
		for (int i = 0; i < 64; i++) {
			int32_t c[3], s[3], e[3];
			for (int k = 0; k < 3; k++) {
				c[k] = (cb->absmin[k] + cb->absmax[k]) / 2 + rng_pm(8 * CLIP_SUB);
				s[k] = c[k] + rng_pm(96 * CLIP_SUB);
				e[k] = 2 * c[k] - s[k];
			}
			int hull = i % 3;
			trace_t ta, tc;
			clip_trace(hull, s, e, &ta);
			clip_trace_c(hull, s, e, &tc);
			box_hits += tc.ent == b + 1;
			if (memcmp(&ta, &tc, sizeof ta) && bad++ < 8)
				printf("clipdiff MISMATCH box %d hull %d: asm f %d end %d %d %d ent %d / c f %d end %d %d %d ent %d\n",
				       b, hull, (int) ta.fraction, (int) ta.endpos[0], (int) ta.endpos[1], (int) ta.endpos[2],
				       ta.ent, (int) tc.fraction, (int) tc.endpos[0], (int) tc.endpos[1], (int) tc.endpos[2], tc.ent);
			/* clip_trace_ent: a point missile (MOVE_MISSILE, the box a
			   monster's or not), or a mover of the hull's size skipping
			   one entry */
			const qw_bmodel *was = cb->bm;
			clip_bmodels[b].bm = (i & 4) ? CLIP_BM_MONSTER : 0;
			memset(&clip_mover, 0, sizeof clip_mover);
			if (i & 8) {
				clip_mover.missile = 1;
				hull = 0;
			} else {
				static const int16_t hb[3] = { 0, 16, 32 };
				for (int k = 0; k < 3; k++) {
					clip_mover.box[k]     = -hb[hull] * CLIP_SUB;
					clip_mover.box[k + 3] = hb[hull] * CLIP_SUB;
				}
				clip_mover.pass = i & 16 ? b + 1 : 1;
			}
			clip_mover.nomonsters = (i & 7) == 7;
			memcpy(&clip_mover_c, &clip_mover, sizeof clip_mover);
			clip_trace_ent(hull, s, e, &ta);
			clip_trace_ent_c(hull, s, e, &tc);
			ent_hits += tc.ent == b + 1;
			clip_bmodels[b].bm = was;
			if (memcmp(&ta, &tc, sizeof ta) && bad++ < 8)
				printf("clipdiff MISMATCH ent box %d hull %d missile %d monster %d pass %d: asm f %d ent %d / "
				       "c f %d ent %d\n", b, hull, (int) clip_mover.missile, (i & 4) != 0, (int) clip_mover.pass,
				       (int) ta.fraction, ta.ent, (int) tc.fraction, tc.ent);
		}
	}
	printf("clipdiff boxes %d: %d sweeps, %d hit the box (%d as entities), mismatches so far %d\n", boxes,
	       boxes * 64, box_hits, ent_hits, bad);
	int cbad = 0;
	for (int i = 0; i < N; i++) {
		int32_t p[3];
		rng_point(p, 0);
		int hull = (i & 3) == 3 ? 2 : (i & 3) == 1 ? 0 : 1;
		if (clip_contents(hull, p) != clip_contents_c(hull, p))
			cbad++;
	}
	printf("clipdiff traces %d (%d hit, %d start in solid) mismatches %d; contents %d mismatches %d; "
	       "cycles/trace " PHYS_ENGINE " %u c %u\n", N, hits, solid, bad, N, cbad,
	       (unsigned) (cyc_asm / N), (unsigned) (cyc_c / N));

	/* the hull-0 box nodes (clip_box_node0): asm against C, then the walks
	   from the node against the root's, as SV_CheckBottom has them: the
	   corners' points under a monster-sized box, the lines down from its
	   corners and its middle, around random open points */
	int nbad = 0, wbad = 0, below = 0;
	uint32_t cyc_root = 0, cyc_node = 0;
	uint32_t root = (uint32_t) (uintptr_t) world.root & 0x00ffffffu;
	for (int i = 0; i < N / 4; i++) {
		int32_t c[3], lo[3], hi[3];
		rng_point(c, 1);
		int32_t half = (i & 1) ? 16 * CLIP_SUB : 32 * CLIP_SUB;
		for (int k = 0; k < 2; k++) {
			lo[k] = c[k] - half;
			hi[k] = c[k] + half;
		}
		hi[2] = c[2] - 24 * CLIP_SUB + rng_pm(4);
		lo[2] = hi[2] - 36 * CLIP_SUB;
		uint32_t t0 = prof_now();
		uint32_t na = clip_box_node0(lo, hi);
		uint32_t tw = prof_now();
		uint32_t nc = clip_box_node0_c(lo, hi);
		if (na != nc) {
			if (nbad++ < 4)
				printf("clipdiff MISMATCH box node %d: asm %08x c %08x\n", i, (unsigned) na, (unsigned) nc);
			continue;
		}
		below += na != root;
		int32_t p[3];
		uint32_t t1 = prof_now();
		for (int q = 0; q < 4; q++) {             /* the corners: from the root */
			p[0] = q & 1 ? hi[0] : lo[0];
			p[1] = q & 2 ? hi[1] : lo[1];
			p[2] = hi[2] - CLIP_SUB;
			(void) clip_contents(0, p);
		}
		uint32_t t2 = prof_now();
		for (int q = 0; q < 4; q++) {             /* from the node (its walk counted) */
			p[0] = q & 1 ? hi[0] : lo[0];
			p[1] = q & 2 ? hi[1] : lo[1];
			p[2] = hi[2] - CLIP_SUB;
			(void) clip_contents_node0(na, p);
		}
		uint32_t t3 = prof_now();
		cyc_root += prof_cycles(t1, t2);
		cyc_node += prof_cycles(t0, tw) + prof_cycles(t2, t3);
		for (int q = 0; q < 5; q++) {
			p[0] = q == 4 ? (lo[0] + hi[0]) / 2 : q & 1 ? hi[0] : lo[0];
			p[1] = q == 4 ? (lo[1] + hi[1]) / 2 : q & 2 ? hi[1] : lo[1];
			p[2] = hi[2] - CLIP_SUB;
			int ca = clip_contents_node0(na, p), cc = clip_contents_node0_c(nc, p), cr = clip_contents(0, p);
			if (ca != cr || cc != cr) {
				if (wbad++ < 4)
					printf("clipdiff MISMATCH node contents %d: root %d node asm %d c %d\n", i, cr, ca, cc);
			}
			int32_t a[3] = { p[0], p[1], hi[2] }, b[3] = { p[0], p[1], lo[2] };
			trace_t tr, ta, tc;
			memset(&clip_mover, 0, sizeof clip_mover);
			clip_mover.nomonsters = 1;
			clip_trace_ent(0, a, b, &tr);          /* from the root */
			clip_mover.root0 = na;
			memcpy(&clip_mover_c, &clip_mover, sizeof clip_mover);
			clip_trace_ent(0, a, b, &ta);
			clip_trace_ent_c(0, a, b, &tc);
			if (memcmp(&ta, &tr, sizeof ta) || memcmp(&tc, &tr, sizeof tc)) {
				if (wbad++ < 4)
					printf("clipdiff MISMATCH node trace %d: root f %d end %d, node asm f %d end %d, c f %d end %d\n",
					       i, (int) tr.fraction, (int) tr.endpos[2], (int) ta.fraction, (int) ta.endpos[2],
					       (int) tc.fraction, (int) tc.endpos[2]);
			}
		}
	}
	/* lines of sight (clip_mover's sight): asm against C, and the verdict
	   (clear: fraction 1, not crossing contents) against a whole trace's,
	   between random points (half of them open), near and map-crossing */
	int sbad = 0, vbad = 0, clear = 0;
	uint32_t cyc_full = 0, cyc_sight = 0;
	for (int i = 0; i < N; i++) {
		int32_t s[3], e[3];
		rng_point(s, 1);
		if (i & 1) {
			rng_point(e, !(i & 2));
		} else {
			for (int k = 0; k < 3; k++)
				e[k] = s[k] + rng_pm(256 * CLIP_SUB);
		}
		trace_t tf, ta, tc;
		memset(&clip_mover, 0, sizeof clip_mover);
		clip_mover.nomonsters = 1;
		uint32_t t0 = prof_now();
		clip_trace_ent(0, s, e, &tf);
		uint32_t t1 = prof_now();
		clip_mover.sight = 1;
		memcpy(&clip_mover_c, &clip_mover, sizeof clip_mover);
		uint32_t t2 = prof_now();
		clip_trace_ent(0, s, e, &ta);
		uint32_t t3 = prof_now();
		clip_trace_ent_c(0, s, e, &tc);
		cyc_full  += prof_cycles(t0, t1);
		cyc_sight += prof_cycles(t2, t3);
		if (memcmp(&ta, &tc, sizeof ta) && sbad++ < 4)
			printf("clipdiff MISMATCH sight %d: asm f %d ent %d, c f %d ent %d\n", i, (int) ta.fraction, ta.ent,
			       (int) tc.fraction, tc.ent);
		int vf = tf.fraction >= Q_FIX_ONE && !(tf.inopen && tf.inwater);
		int va = ta.fraction >= Q_FIX_ONE && !(ta.inopen && ta.inwater);
		clear += vf;
		if (vf != va && vbad++ < 4)
			printf("clipdiff MISMATCH sight verdict %d: whole f %d op %d wa %d, sight f %d op %d wa %d\n", i,
			       (int) tf.fraction, tf.inopen, tf.inwater, (int) ta.fraction, ta.inopen, ta.inwater);
	}
	printf("clipdiff lines of sight %d (%d clear): " PHYS_ENGINE "/c mismatches %d, verdicts %d; cycles whole %u sight %u\n",
	       N, clear, sbad, vbad, (unsigned) (cyc_full / N), (unsigned) (cyc_sight / N));

	printf("clipdiff box nodes %d: %d below the root, mismatches %d, walks from them %d; "
	       "4 corners: root %u cycles, node %u (its walk too)\n", N / 4, below, nbad, wbad,
	       (unsigned) (cyc_root / (N / 4)), (unsigned) (cyc_node / (N / 4)));
}

/* w_leaf.s against world.c's C: the leaf of random points of the map's
   box, from the root and from the top node of a random box around each
   (as the bolts' and the edicts' walks start); the cycles of a walk */
static void bench_wleaf_diff(void)
{
#if !Q1PSX_ENGINE_C
	enum { N = 4000 };
	int      bad = 0;
	uint32_t cyc_asm = 0, cyc_c = 0;
	for (int i = 0; i < N; i++) {
		int32_t p[3], w[3];
		rng_point(p, i & 1);                      /* (1/8 units; half in the open) */
		for (int k = 0; k < 3; k++)
			w[k] = p[k] / CLIP_SUB;
		uint32_t at = (uint32_t) world.root;
		if (i & 2) {
			int32_t lo[3], hi[3];
			for (int k = 0; k < 3; k++) {
				int32_t r = (int32_t) (rng() % 64);
				lo[k] = w[k] - r;
				hi[k] = w[k] + r;
			}
			at = world_box_topnode(lo, hi);
		}
		uint32_t t0 = prof_now();
		const qw_leaf *a = world_point_leaf_from(at, w);
		uint32_t t1 = prof_now();
		const qw_leaf *c = world_point_leaf_from_c(at, w);
		uint32_t t2 = prof_now();
		cyc_asm += prof_cycles(t0, t1);
		cyc_c   += prof_cycles(t1, t2);
		if (a != c && bad++ < 4)
			printf("wleafdiff MISMATCH %d: from %08x at %d %d %d: asm %08x c %08x\n", i, (unsigned) at,
			       (int) w[0], (int) w[1], (int) w[2], (unsigned) (uintptr_t) a, (unsigned) (uintptr_t) c);
	}
	printf("wleafdiff %d points (half from a box's top node): mismatches %d; cycles/walk asm %u c %u\n", N, bad,
	       (unsigned) (cyc_asm / N), (unsigned) (cyc_c / N));
#endif
}

/* pmove.s (on clip.s) against pmove.c (on clip.c): a random walk with
   random commands, turns, jumps, short noclip flights and a teleport to a
   random open spot every 300 ticks. Both run each tick from the same state;
   every pmove_t field must match (on a mismatch the asm side is resynced to
   the reference so the count stays meaningful). */
static void bench_pmove_diff(void)
{
	enum { TICKS = 6000, DT_TICKS = 3000 };     /* 60 Hz ticks, then ticks of 2, 3, 6, 1 / 60 s */
	static const uint8_t dts[4] = { 2, 3, 6, 1 };
	pmove_t     a, c;
	pmove_cmd_t cmd;
	int         bad = 0, bad_dt = 0;
	uint32_t    cyc_asm = 0, cyc_c = 0;
	memset(&a, 0, sizeof a);
	memset(&cmd, 0, sizeof cmd);
	int water_runs = 0;
	for (int t = 0; t < TICKS + DT_TICKS; t++) {
		int phase = t % 300;
		cmd.dt = t < TICKS ? 1 : dts[(t / 7) & 3];
		if (phase == 0) {
			rng_point(a.pos, 1);
			if ((t / 300) & 1) {                        /* every other run starts in the water */
				int32_t p[3];
				for (int tries = 0; tries < 400; tries++) {
					rng_point(p, 0);
					if (clip_contents_c(0, p) <= QW_CONTENTS_WATER && clip_contents_c(1, p) == QW_CONTENTS_EMPTY) {
						memcpy(a.pos, p, sizeof p);
						water_runs++;
						break;
					}
				}
			}
			a.vel[0] = a.vel[1] = a.vel[2] = 0;
			a.noclip = 0;
			a.waterjump = 0;
		}
		if (phase == 150)
			cmd.waterjump = 1;                          /* CheckWaterJump's push, once a run */
		else
			cmd.waterjump = 0;
		a.noclip = phase >= 250 && phase < 280;
		if (t % 30 == 0) {
			cmd.fwd  = rng_axis();
			cmd.side = rng_axis();
			cmd.up   = (int) (rng() % 3) - 1;
			cmd.walk = (rng() & 7) == 0;
			a.pitch  = rng_pm(1000);
		}
		cmd.jump = (rng() & 7) == 0 || (phase >= 200 && phase < 230);   /* held for a while: the swim up */
		if (phase == 150) {
			a.movedir[0] = rng_pm(50);
			a.movedir[1] = rng_pm(50);
		}
		a.yaw += rng_pm(48);
		c = a;
		pmove_t before = a;
		uint32_t t0 = prof_now();
		pmove_step(&a, &cmd);
		uint32_t t1 = prof_now();
		pmove_step_c(&c, &cmd);
		uint32_t t2 = prof_now();
		if (t < TICKS) {
			cyc_asm += prof_cycles(t0, t1);
			cyc_c   += prof_cycles(t1, t2);
		}
		if (memcmp(&a, &c, sizeof a)) {
			if (t >= TICKS)
				bad_dt++;
			if (bad++ < 8)
				printf("pmdiff MISMATCH tick %d dt %d noclip %d cmd %d %d %d %d %d wj %d\n"
				       "  asm: pos %d %d %d vel %d %d %d g %d acc %d %d %d %d w %d %d %d %d\n"
				       "  c:   pos %d %d %d vel %d %d %d g %d acc %d %d %d %d w %d %d %d %d\n",
				       t, cmd.dt, c.noclip, cmd.fwd, cmd.side, cmd.up, cmd.jump, cmd.walk, cmd.waterjump,
				       (int) a.pos[0], (int) a.pos[1], (int) a.pos[2], (int) a.vel[0], (int) a.vel[1],
				       (int) a.vel[2], a.onground, (int) a.pos_acc[0], (int) a.pos_acc[1],
				       (int) a.pos_acc[2], (int) a.grav_acc, a.waterlevel, a.watertype, a.waterjump, a.jumpreleased,
				       (int) c.pos[0], (int) c.pos[1], (int) c.pos[2], (int) c.vel[0], (int) c.vel[1],
				       (int) c.vel[2], c.onground, (int) c.pos_acc[0], (int) c.pos_acc[1],
				       (int) c.pos_acc[2], (int) c.grav_acc, c.waterlevel, c.watertype, c.waterjump, c.jumpreleased);
			if (bad == 1) {
				/* replay the tick and dump the scratchpad the asm works in
				   (p_defs.inc has the map) */
				pmove_t d = before;
				pmove_step(&d, &cmd);
				const int32_t *spad = (const int32_t *) 0x1F800000;
				for (int i = 0; i < 0x1A0 / 4; i += 8)
					printf("  spad %03x: %d %d %d %d %d %d %d %d\n", i * 4, (int) spad[i],
					       (int) spad[i + 1], (int) spad[i + 2], (int) spad[i + 3], (int) spad[i + 4],
					       (int) spad[i + 5], (int) spad[i + 6], (int) spad[i + 7]);
			}
			a = c;
		}
	}
	printf("pmdiff ticks %d mismatches %d; cycles/tick " PHYS_ENGINE " %u c %u; runs started in water %d\n", TICKS,
	       bad - bad_dt, (unsigned) (cyc_asm / TICKS), (unsigned) (cyc_c / TICKS), water_runs);
	printf("pmdiff dt ticks %d (2, 3, 6, 1 / 60 s) mismatches %d\n", DT_TICKS, bad_dt);

	/* the same without clip hulls (every trace returns at once): the cost of
	   pmove itself, without its traces */
	const qw_clipnode *cn = world.clipnodes;
	world.clipnodes = NULL;
	cyc_asm = cyc_c = 0;
	cmd.fwd = PM_AXIS;
	cmd.side = PM_AXIS;
	cmd.jump = 0;
	cmd.dt = 1;
	for (int t = 0; t < 1000; t++) {
		a.noclip = 0;
		a.pos[2] = 0;
		c = a;
		pmove_t before = a;
		uint32_t t0 = prof_now();
		pmove_step(&a, &cmd);
		uint32_t t1 = prof_now();
		pmove_step_c(&c, &cmd);
		uint32_t t2 = prof_now();
		cyc_asm += prof_cycles(t0, t1);
		cyc_c   += prof_cycles(t1, t2);
	}
	world.clipnodes = cn;
	printf("pmdiff without traces: cycles/tick " PHYS_ENGINE " %u c %u\n", (unsigned) (cyc_asm / 1000),
	       (unsigned) (cyc_c / 1000));
}

/* Every map on the disc: load it (falling back to a lower geometry when it
   does not fit the arena), then its four spawn views, briefly, each followed
   by its display list's hash (scripts/dlcheck.sh compares them). */
#ifndef Q1PSX_BENCH_MAP
#define Q1PSX_BENCH_MAP ""    /* a map name: the maps bench runs that one only (MAP=E1M1) */
#endif
#ifndef Q1PSX_BENCH_VIEW_SET
#define Q1PSX_BENCH_VIEW_SET 0   /* 1: more views of that map, Q1PSX_BENCH_VIEWS_XY: x, y, z, yaw, pitch
                                    each (VIEW=x,y,z,yaw,pitch[,x,y,z,yaw,pitch ...]) */
#define Q1PSX_BENCH_VIEWS_XY 0, 0, 0, 0, 0
#endif
#ifndef Q1PSX_BENCH_NOENTS
#define Q1PSX_BENCH_NOENTS 0  /* 1: the map's entities are not drawn (NOENTS=1: an A / B) */
#endif
#ifndef Q1PSX_BENCH_TICKS
#define Q1PSX_BENCH_TICKS 0   /* with a VIEW: the game run that many ticks from there (TICKS=n) */
#endif
#ifndef Q1PSX_BENCH_USE
#define Q1PSX_BENCH_USE ""    /* with TICKS: the entities of this targetname (or classname) used at tick 8, as
                                 a button's press would (USE=t53): a door opened in view */
#endif

static void bench_stream_frames(void);

#ifndef Q1PSX_BENCH_RESIDENCY
#define Q1PSX_BENCH_RESIDENCY 0   /* 1: what the stream loads around the player (RESIDENCY=1) */
#endif

/* What the stream loads around the player (§50): the player put at the
   map's items in turn (they lie along his way), a map start's reads
   simulated there (stream_simulate: a model takes the RAM it keeps once
   its images are in VRAM, §67); the pool, what is wanted within reach,
   what would be in; the living monsters by distance, in the PVS of the
   player's leaf or not, their model in or not; then the stream's own
   reads there (stream_preload_at: the CD, the pools, what it drops, the
   last spot's assets still in) and the PVS's monsters whose model they
   left out. "res" lines. */
static void bench_residency(const char *map)
{
	int idx[64], n = 0;
	for (int i = 2; i < num_edicts && n < 64; i++)
		if (edicts[i].inuse && (edicts[i].item_model >= 0 || (edicts[i].masset >= 0 && !edicts[i].mdef)))
			idx[n++] = i;                         /* (an item: its model; FL_ITEM comes with
			                                         PlaceItem's think) */
	int step = n > 24 ? n / 24 : 1;
	uint32_t tot[13] = { 0 };
	int spots = 0;
	for (int s = 0; s < n; s += step) {
		const edict_t *it = &edicts[idx[s]];
		memcpy(pl.pm.pos, it->origin, sizeof pl.pm.pos);
		pl.pm.pos[2] += 24 * CLIP_SUB;
		memcpy(cam.pos, pl.pm.pos, sizeof cam.pos);
		cam.pos[2] += 22 * CLIP_SUB;
		cam.yaw = cam.pitch = cam.roll = 0;
		run_frame(0, 0);                          /* (the PVS from the player's leaf) */
		uint32_t w, wb, r, rb;
		stream_simulate(pl.pm.pos, STREAM_REACH, &w, &wb, &r, &rb);
		stream_preload_at(pl.pm.pos);             /* (the stream's own reads there) */
		uint16_t vis = (uint16_t) r_state.visframe;
		int near1 = 0, near2 = 0, pvs = 0, pvs_in = 0, pvs_read = 0;
		uint32_t knear = 0, kpvs = 0, kin = 0, kpvs_in = 0, models_kb = 0;
		for (int i = 2; i < num_edicts; i++) {
			edict_t *e = &edicts[i];
			if (!e->inuse || !e->mdef || e->health <= 0)
				continue;
			uint32_t d = 0;
			for (int k = 0; k < 3; k++) {
				int32_t v = (e->origin[k] - pl.pm.pos[k]) / CLIP_SUB;
				if ((uint32_t) (v < 0 ? -v : v) > d) d = (uint32_t) (v < 0 ? -v : v);
			}
			const qw_leaf *l = ent_model_leaf(e);
			int seen = l && l->visframe == vis, in = stream_sim_in(e->masset);
			uint32_t kb = 1u << e->mdef->kind;
			near1 += d <= 1000;
			near2 += d <= STREAM_REACH;
			pvs += seen;
			pvs_in += seen && in;
			pvs_read += seen && stream_model(e->masset);
			if (d <= STREAM_REACH) knear |= kb;
			if (seen) kpvs |= kb;
			if (in) kin |= kb;
		}
		kpvs_in = kpvs & kin;
		for (int k = 0; k < MK_COUNT; k++)
			if (kin & 1u << k)
				models_kb += stream_kept(monsters_kind_asset(k, 0)) / 1024;
		printf("res %s @%d,%d,%d: pool %u KB, wanted %u (%u KB), in %u (%u KB) | monsters <=1000 %d, <=2500 %d, "
		       "PVS %d (in %d, read %d) | kinds <=2500 %d, PVS %d (in %d), in %d (%u KB of models)\n", map,
		       (int) (pl.pm.pos[0] / CLIP_SUB), (int) (pl.pm.pos[1] / CLIP_SUB), (int) (pl.pm.pos[2] / CLIP_SUB),
		       (unsigned) (stream_stats.ram_size / 1024), (unsigned) w, (unsigned) (wb / 1024), (unsigned) r,
		       (unsigned) (rb / 1024), near1, near2, pvs, pvs_in, pvs_read, __builtin_popcount(knear),
		       __builtin_popcount(kpvs), __builtin_popcount(kpvs_in), __builtin_popcount(kin),
		       (unsigned) models_kb);
		tot[0] += (uint32_t) near1; tot[1] += (uint32_t) near2; tot[2] += (uint32_t) pvs;
		tot[3] += (uint32_t) pvs_in; tot[4] += (uint32_t) __builtin_popcount(knear);
		tot[5] += (uint32_t) __builtin_popcount(kpvs); tot[6] += (uint32_t) __builtin_popcount(kpvs_in);
		tot[7] += (uint32_t) __builtin_popcount(kin); tot[8] += models_kb;
		tot[9] += stream_stats.ram_size / 1024; tot[10] += wb / 1024;
		tot[11] += (uint32_t) (pvs > pvs_in);
		tot[12] += (uint32_t) (pvs > pvs_read);
		spots++;
	}
#define AVG(x) (unsigned) ((x) / spots), (unsigned) ((x) * 10 / spots % 10)
	if (spots)
		printf("res %s: %d spots | pool %u KB, wanted %u KB | monsters <=1000 %u.%u, <=2500 %u.%u, PVS %u.%u "
		       "(in %u.%u) | kinds <=2500 %u.%u, PVS %u.%u (in %u.%u), in %u.%u (%u KB) | spots with a monster "
		       "in the PVS not in: %u (the stream's reads: %u)\n", map, spots, (unsigned) (tot[9] / spots),
		       (unsigned) (tot[10] / spots), AVG(tot[0]), AVG(tot[1]), AVG(tot[2]), AVG(tot[3]), AVG(tot[4]),
		       AVG(tot[5]), AVG(tot[6]), AVG(tot[7]), (unsigned) (tot[8] / spots), (unsigned) tot[11],
		       (unsigned) tot[12]);
#undef AVG
}

static void bench_bmf_arena(const char *what);

/* a VIEW's ticks (TICKS=n): the player standing there, the game running
   (his touches, the movers, the monsters, the stream), the view dumped
   every 8 ticks as <map>.v<view>t<tick> */
static void bench_view_ticks(const char *view)
{
	static char name[24];
	static int spawned = -1;
	if (spawned != load_map) {
		game_spawn();
		spawned = load_map;
	}
	struct { int32_t pos[3]; int yaw, pitch; } at;
	memcpy(at.pos, cam.pos, sizeof at.pos);
	at.yaw = cam.yaw;
	at.pitch = cam.pitch;
	memcpy(pl.pm.pos, cam.pos, sizeof pl.pm.pos);
	pl.pm.pos[2] -= 22 * CLIP_SUB;
	memset(pl.pm.vel, 0, sizeof pl.pm.vel);
	game_cmd_t cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.weapon_group = -1;
	game_active = 1;
	for (int t = 0; t <= Q1PSX_BENCH_TICKS; t++) {
	if (Q1PSX_BENCH_USE[0] && t == 8) {      /* the targets used, as a button's press would
	                                            (at tick 8: the doors' links are made at 0.1 s) */
		int used = 0;
		for (int i = 2; i < num_edicts; i++) {
			edict_t *e = &edicts[i];
			if (!e->inuse || !e->use ||                 /* a targetname, or a classname (USE=func_door_secret) */
			    ((!e->targetname || strcmp(world_string(e->targetname), Q1PSX_BENCH_USE)) &&
			     strcmp(world_string(e->classname), Q1PSX_BENCH_USE)))
				continue;
			ent_other = ent_activator = ent_player;
			ENT_CALL(e, use);
			used++;
		}
		printf("use %s: %d entities\n", Q1PSX_BENCH_USE, used);
	}
		if (t % 8 == 0) {
			memcpy(cam.pos, at.pos, sizeof cam.pos);
			cam.yaw = at.yaw;
			cam.pitch = at.pitch;
			cam.roll = 0;
			for (int i = 0; i < 2; i++)
				run_frame(0, 0);
			q_snprintf(name, sizeof name, "%st%d", view, t);
			printf("D %s\n", name);
			ents_debug = 1;
			run_frame(0, BENCH_DUMP);
			ents_debug = 0;
			rl_order_print(name);
		}
		game_tick(&cmd);
		bench_stream_frames();
	}
	game_active = 0;
	bench_bmf_arena(view);
}

/* the level arena's room the world takes, a zoned one's measure (§57):
   the skeleton and the slots, the stream's pool after them (the world's
   load in all: its "world: ... arena" line) */
/* the brush models' cut arena's traffic since the map's load (§90.6) */
static void bench_bmf_arena(const char *what)
{
	printf("bmf arena %s: rooms taken %u, given up %u, draws without room %u; %u cuts, %u cycles, the dearest %u\n",
	       what, (unsigned) bmf_stats[7], (unsigned) bmf_stats[8], (unsigned) bmf_stats[9], (unsigned) bmf_stats[2],
	       (unsigned) bmf_stats[4], (unsigned) bmf_stats[5]);
}

static void bench_zone_ram(const char *map)
{
	if (zone_on())
		printf("bench zone %s: %d zones, the skeleton %u KB + the slots %u KB, the stream's pool %u KB\n",
		       map, zone_count(), (unsigned) zone_skeleton_bytes() / 1024, (unsigned) zone_arena_bytes() / 1024,
		       (unsigned) stream_stats.ram_size / 1024);
	else
		printf("bench zone %s: whole, the stream's pool %u KB\n", map, (unsigned) stream_stats.ram_size / 1024);
}

#ifndef Q1PSX_BENCH_ZONES
#define Q1PSX_BENCH_ZONES 0   /* 1: the zones' walk (ZONES=1 scripts/bench.sh) */
#endif

/* The zones' walk (ZONES=1 scripts/bench.sh, docs/porting-notes.md §57):
   on each zoned map (MAP=: that one), the eye taken from the player's
   start through the map's items and monsters in their entity order at
   running speed (320 units a second, a frame a 60th), the stream and the
   zones' reads going on as in play (no read at once). A leg whose eye
   line a wall cuts goes through it (its frames apart: a way shorter than
   the walking distances the prefetch ranks zones by). Per map: the zone
   switches and the patch's cycles (every patch checked against the C
   one), the chunk reads (their bytes, the frames each took), the late
   events and frames (the camera in a zone not in yet, drawn with the old
   one), the longest run of them; the frames' cycles, the switching ones'
   apart. */
#define ZW_WAYPOINTS 32
#define ZW_FRAMES    3600     /* a minute of walking a map at most */
static void bench_zones(void)
{
	stream_log = 0;
	for (int m = 0; m < world_num_maps(); m++) {
		if (Q1PSX_BENCH_MAP[0] && strcmp(world_map_name(m), Q1PSX_BENCH_MAP))
			continue;
		if (!load_level(m)) {
			printf("bench map %s FAILED to load\n", world_map_name(m));
			continue;
		}
		bench_zone_ram(world_map_name(m));
		if (!zone_on())
			continue;
		game_spawn();
		game_active = 1;
		zone_sync = 0;
		cd_log_mark("walk");                    /* (the drive journal's phase: the walk's reads) */
		/* the waypoints: the start, then the items and the monsters */
		static int32_t wp[ZW_WAYPOINTS][3];
		int nwp = 0, n = 0;
		memcpy(wp[nwp++], pl.pm.pos, sizeof wp[0]);
		for (int i = 2; i < num_edicts; i++)
			n += edicts[i].inuse && (edicts[i].item_model >= 0 || edicts[i].masset >= 0);
		int step = n > ZW_WAYPOINTS - 1 ? (n + ZW_WAYPOINTS - 2) / (ZW_WAYPOINTS - 1) : 1, k = 0;
		for (int i = 2; i < num_edicts && nwp < ZW_WAYPOINTS; i++)
			if (edicts[i].inuse && (edicts[i].item_model >= 0 || edicts[i].masset >= 0) && k++ % step == 0) {
				memcpy(wp[nwp], edicts[i].origin, sizeof wp[0]);
				wp[nwp++][2] += 24 * CLIP_SUB;      /* (on the floor: a standing player's origin) */
			}
		int32_t at[3];
		memcpy(at, wp[0], sizeof at);
		uint32_t frames = 0, units = 0, walls = 0, late = 0, late_wall = 0, run = 0, worst = 0, bad = 0;
		uint32_t fsum = 0, fmax = 0, swf = 0, swsum = 0, swmax = 0, since_wall = 1u << 30, pkt_max = 0;
		int32_t room_min = 0x7fffffff;
		uint32_t skipped0 = r_model_skipped, full = 0;
		const int32_t speed = 320 * CLIP_SUB / 60;
		uint32_t reads0 = stream_stats.reads;
		prof_sample_start();                          /* (SAMPLE=1: the walk's profile) */
		for (int w = 1; w < nwp && frames < ZW_FRAMES; w++) {
			int32_t d[3], e0[3], e1[3];
			for (int c = 0; c < 3; c++) {
				d[c]  = wp[w][c] - at[c];
				e0[c] = at[c];
				e1[c] = wp[w][c];
			}
			e0[2] += 22 * CLIP_SUB;
			e1[2] += 22 * CLIP_SUB;
			uint32_t len = q_isqrt64((uint64_t) ((int64_t) d[0] * d[0] + (int64_t) d[1] * d[1] +
			                                     (int64_t) d[2] * d[2]));
			trace_t tr;
			int nf = (int) (len / (uint32_t) speed) + 1, through = clip_trace(0, e0, e1, &tr) < Q_FIX_ONE;
			units += len / CLIP_SUB;
			walls += through;
			int yaw = r_atan2(d[1], d[0]);
			for (int f = 1; f <= nf && frames < ZW_FRAMES; f++) {
				for (int c = 0; c < 3; c++)
					pl.pm.pos[c] = at[c] + (int32_t) ((int64_t) d[c] * f / nf);
				memcpy(cam.pos, pl.pm.pos, sizeof cam.pos);
				cam.pos[2] += 22 * CLIP_SUB;
				cam.yaw = yaw;
				cam.pitch = cam.roll = 0;
				uint32_t sw0 = zone_stats.switches;
				run_frame(0, 0);
				music_tick();                   /* (the music's refills as in play: the
				                                   drive shared with the chunks' reads) */
				frames++;
				since_wall = through ? 0 : since_wall + 1;
				uint32_t fc = prof_stage[PROF_FRAME];
				fsum += fc;
				if (fc > fmax) fmax = fc;
				if (vid_stats.pkt_bytes > pkt_max) pkt_max = vid_stats.pkt_bytes;
				if (r_model_room_min < room_min) room_min = r_model_room_min;
				full += r_arena_full != 0;
				if (zone_stats.switches != sw0) {
					swf++;
					swsum += fc;
					if (fc > swmax) swmax = fc;
					bad += (uint32_t) zone_check_patch();
				}
				if (zone_of_leaf(zone_bench_leaf(cam.pos)) != zone_current() &&
				    zone_of_leaf(zone_bench_leaf(cam.pos)) != (int) QZ_NONE) {
					late++;
					late_wall += since_wall < 60;     /* (in a leg through a wall, or a second after) */
					if (++run > worst) worst = run;
				} else {
					run = 0;
				}
			}
			memcpy(at, wp[w], sizeof at);
		}
		const zone_stats_t *z = &zone_stats;
		printf("bench zones %s: %u frames, %u units, %u legs of %d through walls; %u switches (patch %u cycles avg, "
		       "%u max; %u patches checked bad), %u reads (%u KB, %u frames each), %u waits for the drive; late: %u "
		       "events, %u frames (%u through walls), the longest run %u; %u evictions, %u read errors; frames %u "
		       "cycles avg, %u max; switching frames %u avg, %u max; the stream's reads %u, %u sounds asked, not in, "
		       "%u swapped from RAM; packets %u bytes at most, a model's room %d at least, %u models left out, "
		       "%u full frames\n",
		       world_map_name(m), frames, units, walls, nwp - 1, z->switches,
		       z->switches ? z->switch_cycles / z->switches : 0, z->switch_max, bad, z->loads, z->load_bytes / 1024,
		       z->loads ? z->load_frames / z->loads : 0, z->waits, z->late_events, late, late_wall, worst,
		       z->evictions, z->read_errors, frames ? fsum / frames : 0, fmax, swf ? swsum / swf : 0, swmax,
		       stream_stats.reads - reads0, stream_stats.snd_missing, stream_stats.ram_swaps, pkt_max,
		       room_min == 0x7fffffff ? -1 : (int) room_min, (unsigned) (r_model_skipped - skipped0), full);
		bench_bmf_arena(world_map_name(m));
		prof_sample_stop();
		prof_sample_print(world_map_name(m));
		game_active = 0;
		zone_sync = 1;
	}
}

#if Q1PSX_BENCH_SEQCHK
/* The view just drawn by the zone's cooked walk drawn again by the tree
   walk (the same camera, the next frame count): the leaves' stamps (the
   PVS marking's, redone by neither: equal by construction), the faces'
   stamps (the leaves visited) and the records drawn compared; the faces
   each walk alone stamped listed with a leaf that marks them, its stamp,
   its box and the camera. The frame's packets are drawn twice (no dump). */
static void seq_check(const char *name)
{
	uint32_t seq = r_state.seq;
	if (!seq || !world_ok)
		return;
	uint32_t n = world.num_faces, mark = mem_level_mark();
	uint8_t *a = mem_level_alloc((n + 7) / 8), *b = mem_level_alloc((n + 7) / 8);
	if (!a || !b) {
		mem_level_release(mark);
		return;
	}
	uint32_t recs[2];
	for (int pass = 0; pass < 2; pass++) {
		r_seq_set(pass ? 0 : seq);
		run_frame(0, 0);
		uint8_t *bits = pass ? b : a;
		memset(bits, 0, (n + 7) / 8);
		for (uint32_t f = 0; f < n; f++)
			if (world.face_visframe[f] == r_state.framecount)
				bits[f >> 3] |= (uint8_t) (1 << (f & 7));
		recs[pass] = r_state.stat_records;
	}
	r_seq_set(seq);
	uint32_t only_seq = 0, only_tree = 0, shown = 0;
	for (uint32_t f = 0; f < n; f++) {
		int ia = a[f >> 3] >> (f & 7) & 1, ib = b[f >> 3] >> (f & 7) & 1;
		if (ia == ib)
			continue;
		only_seq += ia;
		only_tree += ib;
		if (shown++ >= 6)
			continue;
		/* a leaf marking it (the marks are byte offsets, world.c) */
		uint32_t l = 0;
		for (uint32_t i = 1; i < world.num_leaves && !l; i++) {
			const uint16_t *m = (const uint16_t *) world.leaves[i].marks;
			for (uint32_t k = 0; k < world.leaves[i].num_marks; k++)
				if (m[k] == f * 2) {
					l = i;
					break;
				}
		}
		const qw_leaf *lf = &world.leaves[l];
		printf("seqchk %s: face %u by the %s only; leaf %u stamp %u (vis %u) c %d %d %d h %d %d %d; cam %d %d %d\n",
		       name, f, ia ? "cooked walk" : "tree", l, lf->visframe, r_state.visframe, lf->c[0], lf->c[1], lf->c[2],
		       lf->h[0], lf->h[1], lf->h[2], r_state.cam[0], r_state.cam[1], r_state.cam[2]);
	}
	printf("seqchk %s: faces stamped by the cooked walk only %u, by the tree only %u; records drawn %u / %u\n", name,
	       only_seq, only_tree, recs[0], recs[1]);
	mem_level_release(mark);
}
#endif

static void bench_maps(void)
{
	static char names[4][16];
	stream_log = 1;
	for (int m = 0; m < world_num_maps(); m++) {
		if (Q1PSX_BENCH_MAP[0] && strcmp(world_map_name(m), Q1PSX_BENCH_MAP))
			continue;
		if (!load_level(m)) {
			printf("bench map %s FAILED to load\n", world_map_name(m));
			continue;
		}
		printf("bench map %s geometry %s arena %u KB\n", world_map_name(m), level_name[world.level[QL_GEOMETRY]],
		       mem_level_used() / 1024);
		bench_zone_ram(world_map_name(m));
		if (Q1PSX_BENCH_RESIDENCY) {
			stream_log = 0;
			bench_residency(world_map_name(m));
			stream_log = 1;
			continue;
		}
		if (Q1PSX_BENCH_PLAY) {                 /* the game's frames on the map, as a player's */
			ents_check("load");
			game_spawn();
			ents_check("spawn");
			game_active = 1;
#if Q1PSX_BENCH_PLAY
			play_trace = 2;
#endif
			for (int i = 0; i < 30; i++) {
				run_frame(1, 0);
				ents_check("play frame");
			}
			game_active = 0;
			printf("bench play %s ok\n", world_map_name(m));
		}
		static const int16_t xv[] = { Q1PSX_BENCH_VIEWS_XY };   /* VIEW=x,y,z,yaw,pitch...: more */
		uint32_t have_extra = Q1PSX_BENCH_VIEW_SET ? sizeof xv / sizeof xv[0] / 5 : 0;
		qw_view extra;
		uint32_t nviews = Q1PSX_BENCH_MAP[0] ? world.num_views : 4u;
		if (nviews > world.num_views) nviews = world.num_views;
		for (uint32_t v = 0; v < nviews + have_extra; v++) {
			if (v >= nviews) {
				const int16_t *x = &xv[(v - nviews) * 5];
				extra.pos[0] = x[0]; extra.pos[1] = x[1]; extra.pos[2] = x[2];
				extra.yaw = x[3]; extra.pitch = x[4];
			}
			const qw_view *vw = v < nviews ? &world.views[v] : &extra;   /* (one map: all of its views) */
			cam.pos[0] = vw->pos[0] * CLIP_SUB;
			cam.pos[1] = vw->pos[1] * CLIP_SUB;
			cam.pos[2] = vw->pos[2] * CLIP_SUB;
			cam.yaw    = vw->yaw * ANG_TURN / 360;
			cam.pitch  = vw->pitch * ANG_TURN / 360;
			cam.roll   = 0;
			if (v >= nviews) {                  /* a VIEW: the player there, what he would
			                                       have streamed in around him */
				memcpy(pl.pm.pos, cam.pos, sizeof pl.pm.pos);
				pl.pm.pos[2] -= 22 * CLIP_SUB;
				stream_preload_at(pl.pm.pos);
			}
			q_snprintf(names[v & 3], sizeof names[0], "%s.v%u", world_map_name(m), v);
			bench_acc_t acc;
			bench_reset(&acc, names[v & 3]);
			for (int i = 0; i < 4; i++)
				run_frame(0, 0);
			prof_sample_start();
			for (int i = 0; i < 16; i++) {
				run_frame(0, 0);
				bench_add(&acc, vid_stats.vbl);
			}
			prof_sample_stop();
			bench_print(&acc);
			if (Q1PSX_BENCH_WARPDYN)
				printf("warp %s T %d more %d\n", names[v & 3], r_warp.t_eff, r_warp.more);
			prof_sample_print(names[v & 3]);
			printf("D %s\n", names[v & 3]);
			ents_debug = 1;                     /* (the models drawn, by name) */
			run_frame(0, Q1PSX_BENCH_MAP[0] ? 1 : 2);   /* one map: its frames in full */
			ents_debug = 0;
			run_frame(0, 0);                    /* (the cycles of a frame without the prints) */
			printf("ents view %s: %d models drawn of %d, %d in the PVS, %d leaves in front, items %d, lists %d\n",
			       names[v & 3], ents_stat[0], ents_stat[3], ents_stat[1], ents_stat[2], rl_num_items(),
			       rl_num_lists());
			printf("lod view %s: %d streamed models drawn, %d cut to a band (the farthest %d)\n", names[v & 3],
			       monsters_drawn, rl_lod_cut, rl_lod_band_hi);
			if (r_tess_stat[0] || r_tess_stat[2] || r_tess_stat[3] || r_tess_stat[4] || r_tess_stat[5])
				printf("tess view %s: %u polygons cut regularly into %u cells, %u unsplit; to the slow path: %u near, %u too fine, %u over the cap\n",
				       names[v & 3], (unsigned) r_tess_stat[0], (unsigned) r_tess_stat[1], (unsigned) r_tess_stat[4],
				       (unsigned) r_tess_stat[2], (unsigned) r_tess_stat[3], (unsigned) r_tess_stat[5]);
			if (r_state.seq)
				printf("seq view %s: the cooked walk, a frame's tests: %u leaves, %u nodes, %u sides; %u entries\n",
				       names[v & 3], (unsigned) r_seq_stat[0], (unsigned) r_seq_stat[1], (unsigned) r_seq_stat[2],
				       (unsigned) r_seq_stat[3]);
#if Q1PSX_BENCH_SEQCHK
			seq_check(names[v & 3]);
#endif
			rl_order_print(names[v & 3]);
			if (Q1PSX_BENCH_TICKS && v >= nviews)
				bench_view_ticks(names[v & 3]);
		}
	}
}

/* The game at the spawn point: each weapon selected and fired for a few
   ticks, then a frame dumped (view model, projectiles, effects, HUD); both
   HUD styles. The cost of the frame goes in the bench line. */
#ifndef Q1PSX_BENCH_ENTS
#define Q1PSX_BENCH_ENTS 0    /* 1: the map entities' scenarios (ENTS=1 scripts/bench.sh) */
#endif

/* the player at the centre of a box, still */
static void ents_place(const int32_t mn[3], const int32_t mx[3], int dz)
{
	for (int k = 0; k < 3; k++)
		pl.pm.pos[k] = (mn[k] + mx[k]) / 2;
	pl.pm.pos[2] += dz;
	memset(pl.pm.vel, 0, sizeof pl.pm.vel);
	memset(pl.pm.pos_acc, 0, sizeof pl.pm.pos_acc);
	pl.pm.onground = 0;
}

/* the player put next to edict e's world box (ent_absbox) */
static void ents_place_at(const edict_t *e, int dz)
{
	int32_t mn[3], mx[3];
	ent_absbox(e, mn, mx);
	ents_place(mn, mx, dz);
}

static void ents_ticks(int n, const game_cmd_t *cmd)
{
	for (int i = 0; i < n; i++)
		game_tick(cmd);
}

/* the view at the player's eye toward a point */
static void ents_look_at(const int32_t at[3])
{
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = at[k] - pl.pm.pos[k];
	pl.pm.yaw = r_atan2(d[1], d[0]);
	pl.pm.pitch = 0;
	game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
}

/* the frame cache's refusals since the last call (engine/mdl.c: a model
   whose frame it cannot expand is not drawn) */
static uint32_t mdl_refused_since(void)
{
	static uint32_t seen;
	uint32_t n = mdl_stat_refused - seen;
	seen = mdl_stat_refused;
	return n;
}

static void ents_dump(const char *name)
{
	game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
	run_frame(0, 0);
	mdl_refused_since();
	run_frame(0, 0);                                  /* (measured: it draws what the dump will) */
	printf("mon dump %s: %d streamed models drawn, the frame cache refused %u; frame %u cycles "
	       "(block %u, slow %u, ents %u)\n", name, monsters_drawn, (unsigned) mdl_refused_since(),
	       (unsigned) prof_stage[PROF_FRAME], (unsigned) prof_stage[PROF_R_BLOCK],
	       (unsigned) prof_stage[PROF_R_SLOW], (unsigned) prof_stage[PROF_ENTS]);
	printf("D %s\n", name);
	ents_debug = 1;                                   /* (the models drawn, their pieces) */
	run_frame(0, BENCH_DUMP);
	ents_debug = 0;
	rl_order_print(name);
	printf("bmf %s: %u models with pieces, %u pieces made in %u cuts (%u given up), %u cycles, %u the dearest;"
	       " items %d, lists %d; rooms taken %u, given up %u, drawn whole for want of one %u\n", name,
	       (unsigned) bmf_stats[0], (unsigned) bmf_stats[1], (unsigned) bmf_stats[2], (unsigned) bmf_stats[3],
	       (unsigned) bmf_stats[4], (unsigned) bmf_stats[5], rl_num_items(), rl_num_lists(),
	       (unsigned) bmf_stats[7], (unsigned) bmf_stats[8], (unsigned) bmf_stats[9]);
}

static edict_t *ents_first(int cls, int (*ok)(const edict_t *))
{
	for (int i = 2; i < num_edicts; i++)
		if (edicts[i].inuse && edicts[i].class == cls && (!ok || ok(&edicts[i])))
			return &edicts[i];
	return 0;
}

static int door_with_field(const edict_t *e) { return e->owner == e && e->trigger_field; }

/* a spot in box [mn, mx] where the player fits, next to the edict e: its
   centre pushed out along x or y past e's box */
static int ents_free_spot(const edict_t *e, const int32_t mn[3], const int32_t mx[3])
{
	int32_t c[3], emin[3], emax[3];
	ent_centre(e, c);
	ent_absbox(e, emin, emax);
	for (int a = 0; a < 4; a++) {
		int k = a >> 1, s = a & 1 ? -1 : 1;
		int32_t half = (emax[k] - emin[k]) / 2 + 24 * CLIP_SUB;
		int32_t p[3] = { c[0], c[1], c[2] };
		p[k] += s * half;
		if (p[k] < mn[k] || p[k] > mx[k])
			continue;
		trace_t tr;
		clip_trace(1, p, p, &tr);
		if (tr.startsolid || tr.allsolid)
			continue;
		memcpy(pl.pm.pos, p, sizeof pl.pm.pos);
		memset(pl.pm.vel, 0, sizeof pl.pm.vel);
		pl.pm.onground = 0;
		return 1;
	}
	return 0;
}
static int touch_button(const edict_t *e)    { return e->touch != 0 && !e->health; }
static int drawn_item(const edict_t *e)      { return (e->item_model >= 0 || e->masset >= 0) && e->touch != 0; }

/* the items: the first of each kind seen from beside it, then walked onto
   with a new game's kit (what it gives, what it says); a quad's 30 s */
static void bench_items(const char *map, const game_cmd_t *cmd)
{
	static const uint8_t kinds[] = {
		QE_ITEM_HEALTH, QE_ITEM_SHELLS, QE_ITEM_SPIKES, QE_ITEM_ROCKETS, QE_ITEM_CELLS,
		QE_ITEM_ARMOR1, QE_ITEM_ARMOR2, QE_ITEM_ARMORINV, QE_WEAPON_SUPERSHOTGUN, QE_WEAPON_NAILGUN,
		QE_WEAPON_SUPERNAILGUN, QE_WEAPON_GRENADELAUNCHER, QE_WEAPON_ROCKETLAUNCHER, QE_WEAPON_LIGHTNING,
		QE_ITEM_KEY1, QE_ITEM_KEY2, QE_ITEM_SIGIL, QE_ITEM_ARTIFACT_INVULNERABILITY,
		QE_ITEM_ARTIFACT_ENVIROSUIT, QE_ITEM_ARTIFACT_INVISIBILITY, QE_ITEM_ARTIFACT_SUPER_DAMAGE,
		QE_LIGHT_TORCH_SMALL_WALLTORCH, QE_LIGHT_FLAME_LARGE_YELLOW, QE_LIGHT_FLAME_SMALL_YELLOW,
		QE_MISC_EXPLOBOX, QE_MISC_EXPLOBOX2 };
	char name[32];
	pl.health = 50; pl.armor = 0; pl.armortype = 0;
	pl.items  = IT_AXE | IT_SHOTGUN;
	pl.ammo[AMMO_SHELLS] = 25; pl.ammo[AMMO_NAILS] = pl.ammo[AMMO_ROCKETS] = pl.ammo[AMMO_CELLS] = 0;
	pl.weapon = W_SHOTGUN;
	for (unsigned i = 0; i < sizeof kinds; i++) {
		edict_t *e = ents_first(kinds[i], kinds[i] >= QE_LIGHT_TORCH_SMALL_WALLTORCH &&
		                                  kinds[i] <= QE_LIGHT_FLAME_SMALL_WHITE ? 0 : drawn_item);
		if (!e && kinds[i] >= QE_MISC_EXPLOBOX)
			e = ents_first(kinds[i], 0);
		if (!e || (e->item_model < 0 && e->masset < 0))
			continue;
		int32_t c[3], mn[3], mx[3];
		ent_centre(e, c);
		for (int k = 0; k < 3; k++) {
			mn[k] = c[k] - 400 * CLIP_SUB;
			mx[k] = c[k] + 400 * CLIP_SUB;
		}
		printf("item %s class %d at %d %d %d model %d streamed %d\n", world_string(e->classname), e->class,
		       (int) e->origin[0] / 8, (int) e->origin[1] / 8, (int) e->origin[2] / 8, e->item_model, e->masset);
		if (ents_free_spot(e, mn, mx)) {
			ents_look_at(c);
			q_snprintf(name, sizeof name, "i%s_%d", map, e->class);
			ents_dump(name);
		}
		if (!e->touch)
			continue;
		ents_place_at(e, 0);
		ents_ticks(3, cmd);
		printf("item taken %d: health %d armor %d/%d items %08x ammo %d %d %d %d weapon %d\n",
		       !e->inuse || (e->item_model < 0 && e->masset < 0), pl.health, pl.armor, pl.armortype, (unsigned) pl.items,
		       pl.ammo[0], pl.ammo[1], pl.ammo[2], pl.ammo[3], pl.weapon);
	}
	/* a quad: the tint, the face; its warning at 27 s, gone at 30 */
	pl.items |= IT_QUAD;
	game_powerup(IT_QUAD);
	ents_ticks(2, cmd);
	q_snprintf(name, sizeof name, "i%s_quad", map);
	ents_dump(name);
	ents_ticks(27 * 60, cmd);
	printf("quad after 27 s: items %08x\n", (unsigned) pl.items);
	ents_ticks(3 * 60 + 2, cmd);
	printf("quad after 30 s: items %08x\n", (unsigned) pl.items);
	/* a hurt: the flash (armour) */
	pl.armor = 100; pl.armortype = 30; pl.items |= IT_ARMOR1;
	int32_t from[3] = { pl.pm.pos[0] + 64 * CLIP_SUB, pl.pm.pos[1], pl.pm.pos[2] };
	game_damage_player(30, from);
	ents_ticks(1, cmd);
	q_snprintf(name, sizeof name, "i%s_hurt", map);
	ents_dump(name);
	pl.health = 100;
}
static int free_teleport(const edict_t *e)   { return !e->targetname; }

static void bench_ents(void)
{
	static const char *const maps[2] = { "E1M1", "START" };
	game_cmd_t cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.weapon_group = -1;
	ents_log = 1;
	game_active = 1;
	for (int mi = 0; mi < 2; mi++) {
		int m = world_find_map(maps[mi]);
		if (m < 0 || !load_level(m))
			continue;
		map_index = m;
		game_spawn();
		printf("ents bench %s: %d edicts\n", maps[mi], num_edicts);
		ents_ticks(30, &cmd);                         /* LinkDoors, the trains' first corner */
		bench_items(maps[mi], &cmd);
		game_spawn();

		/* a door's field: the chain opens, waits, closes */
		edict_t *d = ents_first(QE_FUNC_DOOR, door_with_field);
		if (d) {
			edict_t *f = d->trigger_field;
			int32_t c[3], fmin[3], fmax[3];
			ent_absbox(f, fmin, fmax);
			if (!ents_free_spot(d, fmin, fmax))
				ents_place(fmin, fmax, 0);
			ent_centre(d, c);
			printf("door *%d at %d %d %d, field %d..%d, player %d %d %d\n", d->model, (int) c[0] / 8,
			       (int) c[1] / 8, (int) c[2] / 8, (int) fmin[0] / 8, (int) fmax[0] / 8,
			       (int) pl.pm.pos[0] / 8, (int) pl.pm.pos[1] / 8, (int) pl.pm.pos[2] / 8);
			for (int t = 0; t < 360; t++) {
				game_tick(&cmd);
				if (t % 30 == 0)
					printf("door tick %d state %d origin %d %d %d ltime %d next %d\n", t, d->state,
					       (int) d->origin[0], (int) d->origin[1], (int) d->origin[2], (int) d->ltime,
					       (int) d->nextthink);
				if (t == 90) {
					ents_look_at(c);
					ents_dump(mi ? "estart_door" : "ee1m1_door");
				}
				if (t == 100)                         /* away from the field: it can close */
					pl.pm.pos[2] += 2000 * CLIP_SUB;
			}
		}

		/* a button pressed */
		edict_t *b = ents_first(QE_FUNC_BUTTON, touch_button);
		if (b) {                                      /* walked into, along its movedir */
			int32_t c[3];
			ent_centre(b, c);
			int64_t sz = 0;
			for (int k = 0; k < 3; k++)
				sz += (int64_t) b->movedir[k] * (b->maxs[k] - b->mins[k]);
			int32_t back = (int32_t) ((sz < 0 ? -sz : sz) / 4096 / 2) + 40 * CLIP_SUB;
			for (int k = 0; k < 3; k++)
				pl.pm.pos[k] = c[k] - (int32_t) ((int64_t) b->movedir[k] * back / 4096);
			pl.pm.pos[2] = c[2];
			memset(pl.pm.vel, 0, sizeof pl.pm.vel);
			pl.pm.yaw = r_atan2(b->movedir[1], b->movedir[0]);
			printf("button *%d at %d %d %d movedir %d %d %d, player %d %d %d\n", b->model, (int) c[0] / 8,
			       (int) c[1] / 8, (int) c[2] / 8, (int) b->movedir[0], (int) b->movedir[1], (int) b->movedir[2],
			       (int) pl.pm.pos[0] / 8, (int) pl.pm.pos[1] / 8, (int) pl.pm.pos[2] / 8);
			game_cmd_t walk = cmd;
			walk.fwd = PM_AXIS;
			for (int t = 0; t < 150; t++) {
				game_tick(t < 40 ? &walk : &cmd);
				if (t == 1 || t == 45) {              /* before and pressed: its alternate textures */
					int32_t keep[3];
					memcpy(keep, pl.pm.pos, sizeof keep);
					for (int k = 0; k < 3; k++)       /* seen from 80 units back */
						pl.pm.pos[k] = c[k] - (int32_t) ((int64_t) b->movedir[k] * (back + 40 * CLIP_SUB) / 4096);
					pl.pm.pos[2] = c[2] - PM_EYE_HEIGHT * CLIP_SUB;
					ents_look_at(c);
					ents_dump(t == 1 ? (mi ? "estart_button0" : "ee1m1_button0")
					                 : (mi ? "estart_button1" : "ee1m1_button1"));
					memcpy(pl.pm.pos, keep, sizeof keep);
				}
				if (t % 15 == 0)
					printf("button tick %d state %d origin %d %d %d player %d %d %d\n", t, b->state,
					       (int) b->origin[0], (int) b->origin[1], (int) b->origin[2],
					       (int) pl.pm.pos[0] / 8, (int) pl.pm.pos[1] / 8, (int) pl.pm.pos[2] / 8);
			}
		}

		/* a plat ridden up */
		edict_t *p = ents_first(QE_FUNC_PLAT, 0);
		if (p && p->state == STATE_BOTTOM) {
			int32_t mn[3], mx[3];
			ent_absbox(p, mn, mx);
			mn[2] = mx[2] = mx[2] + 25 * CLIP_SUB;
			ents_place(mn, mx, 0);
			for (int t = 0; t < 240; t++) {
				game_tick(&cmd);
				if (t % 20 == 0)
					printf("plat tick %d state %d z %d player z %d ground %d\n", t, p->state,
					       (int) p->origin[2] / 8, (int) pl.pm.pos[2] / 8, pl.pm.onground);
			}
		}

		/* a teleporter */
		edict_t *tp = ents_first(QE_TRIGGER_TELEPORT, free_teleport);
		if (tp) {
			ents_place_at(tp, 0);
			int32_t before[3];
			memcpy(before, pl.pm.pos, sizeof before);
			ents_ticks(2, &cmd);
			printf("teleport from %d %d %d to %d %d %d yaw %d vel %d %d %d\n", (int) before[0] / 8,
			       (int) before[1] / 8, (int) before[2] / 8, (int) pl.pm.pos[0] / 8, (int) pl.pm.pos[1] / 8,
			       (int) pl.pm.pos[2] / 8, pl.pm.yaw, (int) pl.pm.vel[0], (int) pl.pm.vel[1], (int) pl.pm.vel[2]);
			ents_ticks(10, &cmd);
			ents_dump(mi ? "estart_tele" : "ee1m1_tele");
		}

		/* a secret */
		edict_t *se = ents_first(QE_TRIGGER_SECRET, 0);
		if (se) {
			ents_place_at(se, 0);
			ents_ticks(3, &cmd);
			printf("secrets %d of %d\n", ent_found_secrets, ent_total_secrets);
			ents_dump(mi ? "estart_secret" : "ee1m1_secret");
		}

		/* the exit: the intermission, then the next map */
		edict_t *cl = ents_first(QE_TRIGGER_CHANGELEVEL, 0);
		if (cl) {
			ents_place_at(cl, 0);
			ents_ticks(12, &cmd);
			printf("changelevel to %s: intermission %d\n", world_string(cl->map), level_intermission);
			ents_ticks(60, &cmd);
			ents_dump(mi ? "estart_inter" : "ee1m1_inter");
			ents_ticks(90, &cmd);
			cmd.fire = 1;
			ents_ticks(2, &cmd);
			cmd.fire = 0;
			char next[16];
			if (level_change_wanted(next)) {
				int nm = world_find_map(next);
				printf("next map %s (disc %d)\n", next, nm);
				if (nm >= 0) {
					game_save_parms();
					if (load_level(nm)) {
						game_spawn_level();
						printf("now on %s, health %d items %08x shells %d\n", world.map_name, pl.health,
						       (unsigned) pl.items, pl.ammo[AMMO_SHELLS]);
						ents_ticks(10, &cmd);
						ents_dump(mi ? "estart_next" : "ee1m1_next");
					}
				}
			}
		}
	}
	ents_log = 0;
	game_active = 0;
}

/* The big movers moving in view (MOVERS=1 scripts/bench.sh): E1M6's train
   *39, E1M3's biggest train, E3M4's biggest door, each started and watched
   from a spot that sees it along its way, one 60 Hz tick a frame (each
   frame it moved and is drawn, bmf_draw cuts it again). Per mover a bench
   line (the frame's and the ents' stages) and a "bmfcut" line: the cuts,
   their cycles, the frames with a cut against those without; every 16th
   frame's hash ("mvhash", "H" lines; "bmfhash": all of them folded, the
   identical-display-list check). MAP= one of them, its frame 240 dumped
   (pktview). */
#ifndef Q1PSX_BENCH_MOVERS
#define Q1PSX_BENCH_MOVERS 0  /* 1: the big movers' scenario (MOVERS=1 scripts/bench.sh) */
#endif

#define MV_FRAMES 600
#define MV_PATH   64

static const struct {
	const char *map;
	uint8_t     cls;
	int16_t     model;           /* 0: the class's biggest (its records' bytes) */
} mv_targets[] = {
	{ "E1M6", QE_FUNC_TRAIN, 39 },
	{ "E1M3", QE_FUNC_TRAIN, 0 },
	{ "E3M4", QE_FUNC_DOOR, 0 },
};

/* the mover's centre were it at org (1/8 units) */
static void mv_centre(const edict_t *e, const int32_t org[3], int32_t c[3])
{
	for (int k = 0; k < 3; k++)
		c[k] = org[k] + (e->mins[k] + e->maxs[k]) / 2;
}

/* the line from p to the mover's centre at org clear of the world (the
   mover itself may end it) */
static int mv_sees(const edict_t *e, const int32_t p[3], const int32_t org[3])
{
	int32_t c[3];
	trace_t tr;
	mv_centre(e, org, c);
	clip_trace(0, p, c, &tr);
	return tr.fraction == Q_FIX_ONE || (tr.ent && tr.ent == e->clip);
}

/* a spot in the open that sees the mover along its way (its places and
   halfway between): out of its box at one of its first 8 places, along x
   and y both ways, at five distances, at the box's centre's height and 48
   units over its foot; the one seeing the most places, the first found
   (the nearest) of those; 0 none */
static int mv_spot(const edict_t *e, int32_t (*path)[3], int np, int32_t out[3])
{
	static const int16_t dist[5] = { 24, 64, 128, 256, 448 };
	int best = 0;
	for (int q = 0; q < np && q < 8; q++)
		for (int d = 0; d < 5; d++)
			for (int a = 0; a < 4; a++)
				for (int foot = 0; foot < 2; foot++) {
					int k = a >> 1;
					int32_t p[3];
					mv_centre(e, path[q], p);
					if (foot)
						p[2] = path[q][2] + e->mins[2] + 48 * CLIP_SUB;
					p[k] = a & 1 ? path[q][k] + e->mins[k] - dist[d] * CLIP_SUB
					             : path[q][k] + e->maxs[k] + dist[d] * CLIP_SUB;
					if (p[k] < -4000 * CLIP_SUB || p[k] > 4000 * CLIP_SUB ||
					    clip_contents(0, p) != QW_CONTENTS_EMPTY)
						continue;
					int score = 0;
					for (int w = 0; w < np; w++) {
						score += mv_sees(e, p, path[w]);
						if (w + 1 < np) {
							int32_t h[3];
							for (int j = 0; j < 3; j++)
								h[j] = (path[w][j] + path[w + 1][j]) / 2;
							score += mv_sees(e, p, h);
						}
					}
					if (score > best) {
						best = score;
						memcpy(out, p, sizeof p);
					}
				}
	return best;
}

/* the camera at eye, toward the mover's centre */
static void mv_look(const edict_t *e, const int32_t eye[3])
{
	int32_t c[3], d[3];
	mv_centre(e, e->origin, c);
	for (int k = 0; k < 3; k++)
		d[k] = (c[k] - eye[k]) / CLIP_SUB;
	int32_t hd = (int32_t) q_isqrt32((uint32_t) (d[0] * d[0] + d[1] * d[1]));
	memcpy(cam.pos, eye, sizeof cam.pos);
	cam.yaw   = r_atan2(d[1], d[0]);
	cam.pitch = r_atan2(-d[2], hd);             /* (down positive) */
	cam.roll  = 0;
}

static void bench_movers(void)
{
	static char names[3][20];
	static int32_t path[MV_PATH + 1][3];
	static const edict_t *seen[MV_PATH];
	game_cmd_t cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.weapon_group = -1;
	game_active = 1;
	for (unsigned ti = 0; ti < sizeof mv_targets / sizeof mv_targets[0]; ti++) {
		const char *map = mv_targets[ti].map;
		if (Q1PSX_BENCH_MAP[0] && strcmp(map, Q1PSX_BENCH_MAP))
			continue;
		int m = world_find_map(map);
		if (m < 0 || !load_level(m)) {
			printf("bmfcut %s: not loaded\n", map);
			continue;
		}
		map_index = m;
		game_spawn();
		ents_ticks(30, &cmd);                         /* LinkDoors, the trains' first corner */
		edict_t *e = 0;
		uint32_t most = 0;
		for (int i = 2; i < num_edicts; i++) {
			edict_t *x = &edicts[i];
			if (!x->inuse || x->class != mv_targets[ti].cls || x->model <= 0 || !bmf_models ||
			    !bmf_models[x->model])
				continue;
			uint32_t b = bmf_model_bytes(x->model);
			if (mv_targets[ti].model ? x->model == mv_targets[ti].model : b > most) {
				most = b;
				e = x;
			}
		}
		if (!e) {
			printf("bmfcut %s: no such mover\n", map);
			continue;
		}
		int np = 2;
		if (e->class == QE_FUNC_TRAIN) {
			np = movers_path(e, path, seen, MV_PATH);
		} else {
			memcpy(path[0], e->pos1, sizeof path[0]);
			memcpy(path[1], e->pos2, sizeof path[1]);
		}
		int32_t eye[3] = { 0, 0, 0 };
		int spots = np > 0 ? mv_spot(e, path, np, eye) : 0;
		printf("bmfcut %s *%d: class %d, %u B of records, box %d..%d %d..%d %d..%d, %d places (first %d %d %d), "
		       "seen at %d of %d from %d %d %d\n", map, e->model, e->class, (unsigned) most,
		       (int) e->mins[0] / CLIP_SUB, (int) e->maxs[0] / CLIP_SUB, (int) e->mins[1] / CLIP_SUB,
		       (int) e->maxs[1] / CLIP_SUB, (int) e->mins[2] / CLIP_SUB, (int) e->maxs[2] / CLIP_SUB, np,
		       np > 0 ? (int) path[0][0] / CLIP_SUB : 0, np > 0 ? (int) path[0][1] / CLIP_SUB : 0,
		       np > 0 ? (int) path[0][2] / CLIP_SUB : 0, spots, np * 2 - 1, (int) eye[0] / CLIP_SUB,
		       (int) eye[1] / CLIP_SUB, (int) eye[2] / CLIP_SUB);
		if (!spots)
			continue;
		ent_activator = ent_player;                   /* started (a train without a name runs) */
		if (e->use)
			ENT_CALL(e, use);
		q_snprintf(names[ti], sizeof names[ti], "mv.%s.%d", map, e->model);
		bench_acc_t acc;
		bench_reset(&acc, names[ti]);
		uint32_t moved = 0, cut_frames = 0, cuts = 0, given_up = 0, cyc = 0, cyc_max = 0, pieces = 0;
		uint32_t f_cut = 0, f_cut_max = 0, n_ents_cut = 0, f_still = 0, n_still = 0, vbl_cut = 0;
		uint32_t fold = 2166136261u, path_fold = 2166136261u;   /* the frames', the mover's way */
		bmf_stats[5] = 0;
		prof_sample_start();
		for (int f = 0; f < MV_FRAMES; f++) {
			int32_t was[3];
			memcpy(was, e->origin, sizeof was);
			game_tick(&cmd);
			moved += memcmp(was, e->origin, sizeof was) != 0;
			for (int k = 0; k < 3; k++)
				path_fold = (path_fold ^ (uint32_t) e->origin[k]) * 16777619u;
			uint32_t c0 = bmf_stats[2] + bmf_stats[3], g0 = bmf_stats[3], y0 = bmf_stats[4], p0 = bmf_stats[1];
			mv_look(e, eye);
			run_frame(0, 0);
			bench_add(&acc, vid_stats.vbl);
			uint32_t n = bmf_stats[2] + bmf_stats[3] - c0, y = bmf_stats[4] - y0;
			cuts += n;
			given_up += bmf_stats[3] - g0;
			pieces += bmf_stats[1] - p0;
			if (n) {
				cut_frames++;
				cyc += y;
				if (y > cyc_max) cyc_max = y;
				f_cut += prof_stage[PROF_FRAME];
				if (prof_stage[PROF_FRAME] > f_cut_max) f_cut_max = prof_stage[PROF_FRAME];
				n_ents_cut += prof_stage[PROF_ENTS];
				vbl_cut += vid_stats.vbl;
			} else {
				f_still += prof_stage[PROF_FRAME];
				n_still++;
			}
			if (f % 16 == 15) {                       /* (not timed: the same state again) */
				printf("mvhash %s f%d\n", names[ti], f);
				run_frame(0, 2);
				fold = (fold ^ vid_dump_hash) * 16777619u;
			}
			if (Q1PSX_BENCH_MAP[0] && f == 240) {
				printf("D %s\n", names[ti]);
				ents_debug = 1;
				run_frame(0, 1);
				ents_debug = 0;
			}
		}
		prof_sample_stop();
		bench_print(&acc);
		prof_sample_print(names[ti]);
		printf("bmfcut %s: %d frames, moved in %u, cut in %u: %u cuts (%u given up), %u pieces a cut; "
		       "the cuts %u cycles a frame with one, %u the most, %u the dearest cut; the frame %u / %u "
		       "with a cut (ents %u, %u.%02u vbl), %u without (%u frames)\n", names[ti], MV_FRAMES,
		       (unsigned) moved, (unsigned) cut_frames, (unsigned) cuts, (unsigned) given_up,
		       (unsigned) (cuts ? pieces / cuts : 0), (unsigned) (cut_frames ? cyc / cut_frames : 0),
		       (unsigned) cyc_max, (unsigned) bmf_stats[5], (unsigned) (cut_frames ? f_cut / cut_frames : 0),
		       (unsigned) f_cut_max, (unsigned) (cut_frames ? n_ents_cut / cut_frames : 0),
		       (unsigned) (cut_frames ? vbl_cut / cut_frames : 0),
		       (unsigned) (cut_frames ? vbl_cut * 100 / cut_frames % 100 : 0),
		       (unsigned) (n_still ? f_still / n_still : 0), (unsigned) n_still);
		/* then turned a quarter left for 16 frames in 32: a moved model out
		   of view is not cut again (bmf_draw's all_culled) until it is in
		   view (the door started again; a train runs on) */
		if (e->use)
			ENT_CALL(e, use);
		uint32_t cuts2 = 0, off0 = bmf_stats[6];
		for (int f = 0; f < 256; f++) {
			game_tick(&cmd);
			for (int k = 0; k < 3; k++)
				path_fold = (path_fold ^ (uint32_t) e->origin[k]) * 16777619u;
			uint32_t c0 = bmf_stats[2] + bmf_stats[3];
			mv_look(e, eye);
			if (f & 16)
				cam.yaw += ANG_TURN / 4;
			run_frame(0, 0);
			cuts2 += bmf_stats[2] + bmf_stats[3] - c0;
			if (f % 16 == 15) {
				printf("mvhash %s t%d\n", names[ti], f);
				run_frame(0, 2);
				fold = (fold ^ vid_dump_hash) * 16777619u;
			}
		}
		printf("bmfcut %s turning: 256 frames, %u cuts, %u put off out of view\n", names[ti],
		       (unsigned) cuts2, (unsigned) (bmf_stats[6] - off0));
#if Q1PSX_BMF_C == 2
		printf("bmfasm %s: %u cuts checked, %u mismatches\n", names[ti], (unsigned) bmf_asm_checks,
		       (unsigned) bmf_asm_mismatches);
#endif
		printf("bmfhash %s: %08x\n", names[ti], (unsigned) fold);
		printf("mvpath %s: %08x (the mover's origin each tick)\n", names[ti], (unsigned) path_fold);
	}
	game_active = 0;
}

/* the release build's menu (engine/menu.c) driven by pressed keys, a frame
   of each page dumped: the main menu, single player, the new-game
   question, the options, the graphics page in each warp mode (the greyed
   entries), back out */
static int  bench_yes(void) { return 1; }
static void bench_menu_key(uint16_t buttons, const char *dump)
{
	pad_t p;
	memset(&p, 0, sizeof p);
	p.pressed = p.held = buttons;
	menu_key(&p);
	if (dump) {
		for (int i = 0; i < 2; i++)
			run_frame(0, 0);
		printf("D %s\n", dump);
		run_frame(0, BENCH_DUMP);
	}
}

static void bench_menu(void)
{
	static const menu_hooks_t hooks = { 0, bench_yes, 0 };
	menu_init(&hooks);
	menu_open();
	menu_shown = 1;
	bench_menu_key(0, "mmain");
	bench_menu_key(PAD_CROSS, "msingle");             /* Single Player */
	bench_menu_key(PAD_CROSS, "mnewgame");            /* New Game: "Are you sure" */
	bench_menu_key(PAD_CIRCLE, 0);                    /* no */
	bench_menu_key(PAD_TRIANGLE, 0);                  /* back to the main menu */
	bench_menu_key(PAD_DOWN, 0);                      /* (to Options) */
	bench_menu_key(PAD_CROSS, "moptions");
	bench_menu_key(PAD_UP, 0);                        /* (the greyed ones: to Graphics) */
	bench_menu_key(PAD_CROSS, "mgraphics");
	bench_menu_key(PAD_RIGHT, "mgraphics_fx");        /* the effects high: their hint */
	bench_menu_key(PAD_LEFT, 0);                      /* (medium again) */
	bench_menu_key(PAD_START, 0);                     /* out: options, main, the game */
	bench_menu_key(PAD_START, 0);
	bench_menu_key(PAD_START, 0);
	printf("menu after START x3: %s\n", menu_active() ? "still up" : "closed");
	menu_shown = 0;
	apply_settings();                                 /* the bench's own again */
	hud_set_style(HUD_ORIGINAL);
}

/* what a game tick costs (the tick rate question): walking, then firing
   the nailgun, then standing, at the spawn; cycles a tick, averaged */
static void bench_tick_cost(void)
{
	game_spawn();
	game_active = 1;
	static const char *const what[3] = { "walk", "fire", "idle" };
	for (int k = 0; k < 3; k++) {
		game_cmd_t cmd;
		memset(&cmd, 0, sizeof cmd);
		cmd.weapon_group = -1;
		if (k == 0) { cmd.fwd = PM_AXIS; cmd.side = 30; }
		if (k == 1) { cmd.fire = 1; }
		if (k == 1)                                    /* the nailgun */
			for (int n = 0; n < W_COUNT && pl.weapon != W_NAILGUN; n++) {
				game_cmd_t c2 = cmd;
				c2.fire = 0;
				c2.weapon_next = 1;
				game_tick(&c2);
			}
		uint32_t total = 0, worst = 0;
		int worst_t = 0;
		prof_sample_start();                          /* (SAMPLE=1: the ticks' profile) */
		for (int t = 0; t < 120; t++) {
			uint32_t t0 = prof_now();
			game_tick(&cmd);
			uint32_t c = prof_cycles(t0, prof_now());
			total += c;
			if (c > worst) { worst = c; worst_t = t; }
		}
		prof_sample_stop();
		printf("bench tick %s: %u cycles a tick on average, %u at worst (tick %d)\n", what[k],
		       (unsigned) (total / 120), (unsigned) worst, worst_t);
		static char sname[3][12];
		q_snprintf(sname[k], sizeof sname[k], "tick_%s", what[k]);
		prof_sample_print(sname[k]);
	}
	game_active = 0;
}

/* The world's rates (settings.tick) through the game's own scheduler
   (player_update) on the same polls: a second standing, two walking
   forward, a jump held 0.2 s, a second falling back. The distance walked,
   the jump's apex, the ticks run (the player's: the same at every rate)
   and the simulation's cycles (PROF_SIM) by rate. */
static void bench_tick_rates(void)
{
	for (int r = num_tick_rates - 1; r >= 0; r--) {
		game_spawn();
		game_active = 1;
		game_world_hz = tick_rates[r];
		int t0 = game_tick_count, ticks = 0, last_count = game_tick_count;
		int32_t start[3], apex = -0x7fffffff;
		uint32_t cyc = 0;
		prof_sample_start();                        /* (SAMPLE=1: each rate's profile) */
		for (int v = 0; v < 300; v++) {
			pad_t p;
			memset(&p, 0, sizeof p);
			p.connected = 1;
			p.type = 4;                               /* digital */
			if (v >= 60 && v < 180)
				p.held = PAD_UP;                      /* forward */
			if (v >= 200 && v < 212)
				p.held = PAD_CROSS;                   /* jump, held 0.2 s */
			p.pressed = v == 200 ? PAD_CROSS : 0;
			if (v == 60)
				memcpy(start, pl.pm.pos, sizeof start);
			uint32_t c0 = prof_now();
			player_update(&p, 1);
			cyc += prof_cycles(c0, prof_now());
			if (game_tick_count != last_count) {
				ticks++;
				last_count = game_tick_count;
			}
			if (v == 180) {
				int32_t d0 = pl.pm.pos[0] - start[0], d1 = pl.pm.pos[1] - start[1];
				printf("bench rate %d Hz: walked %d units in 2 s\n", tick_rates[r],
				       (int) (q_isqrt64((uint64_t) ((int64_t) d0 * d0 + (int64_t) d1 * d1)) / CLIP_SUB));
			}
			if (v == 199)
				start[2] = apex = pl.pm.pos[2];       /* the jump's base */
			if (v >= 200 && pl.pm.pos[2] > apex)
				apex = pl.pm.pos[2];
		}
		prof_sample_stop();
		printf("bench rate %d Hz: jump apex %d units, %d ticks over 300 polls (game time %d), %u cycles a poll\n",
		       tick_rates[r], (int) (apex - start[2]) / CLIP_SUB, ticks, game_tick_count - t0,
		       (unsigned) (cyc / 300));
		static char rname[5][8];
		q_snprintf(rname[r], sizeof rname[r], "rate%d", tick_rates[r]);
		prof_sample_print(rname[r]);
	}
	game_world_hz = Q1PSX_BENCH_WORLD_HZ;
	game_active = 0;
}

/* Frames in motion: the player walks forward through start's hall,
   turning left then right, each frame rendered after its vblanks' polls
   (the last frame's vblanks: as the game loop takes them), the world at
   60 and at 30 ticks a second. The leaf changes (mark_pvs), the ticks:
   frames as in play. */
static void bench_walk(void)
{
	static const uint8_t rates[2] = { 60, 30 };
	for (int r = 0; r < 2; r++) {
		game_spawn();
		game_active = 1;
		game_world_hz = rates[r];
		static char name[2][12];
		q_snprintf(name[r], sizeof name[r], "walk%d", rates[r]);
		bench_acc_t acc;
		bench_reset(&acc, name[r]);
		uint32_t sim = 0, total = 0, worst = 0, vbls[4] = { 0, 0, 0, 0 };
		int polls = 1;
		prof_sample_start();
		for (int f = 0; f < 300; f++) {
			pad_t p[4];
			for (int i = 0; i < polls && i < 4; i++) {
				memset(&p[i], 0, sizeof p[i]);
				p[i].connected = 1;
				p[i].type = 4;                         /* digital */
				p[i].held = PAD_UP;
				if (f >= 60 && f < 100)
					p[i].held |= PAD_LEFT;
				if (f >= 180 && f < 240)
					p[i].held |= PAD_RIGHT;
			}
			uint32_t t0 = prof_now();
			player_update(p, polls < 4 ? polls : 4);
			uint32_t c = prof_cycles(t0, prof_now());
			run_frame(0, 0);
			c += prof_stage[PROF_FRAME];
			sim += c - prof_stage[PROF_FRAME];
			total += c;
			if (c > worst) worst = c;
			bench_add(&acc, vid_stats.vbl);
			vbls[vid_stats.vbl < 4 ? vid_stats.vbl : 3]++;
			polls = vid_stats.vbl ? vid_stats.vbl : 1;
		}
		prof_sample_stop();
		bench_print(&acc);
		printf("bench %s: 300 frames, sim + frame %u cycles on average, %u at worst; sim %u; "
		       "1 vbl %u, 2 vbl %u, 3+ vbl %u\n", name[r], (unsigned) (total / 300), (unsigned) worst,
		       (unsigned) (sim / 300), (unsigned) vbls[1], (unsigned) vbls[2], (unsigned) vbls[3]);
		prof_sample_print(name[r]);
	}
	game_world_hz = Q1PSX_BENCH_WORLD_HZ;
	game_active = 0;
}

static void bench_game(void)
{
	snd_log = 1;                                      /* every sound start, for the diff */
	static const char *const wname[W_COUNT] = { "axe", "shotgun", "sshotgun", "nailgun",
	                                            "snailgun", "glauncher", "rlauncher", "lightning" };
	static char names[W_COUNT + 2][12];
	uint32_t expanded = mdl_stat_expanded;
	mdl_refused_since();
	game_spawn();
	game_active = 1;
	pl.pm.pitch = -40;                                  /* a little down: the floor hits */
	for (int w = 0; w < W_COUNT + 2; w++) {
		int weapon = w < W_COUNT ? w : W_RLAUNCHER;
		hud_set_style(w == W_COUNT + 1 ? HUD_Q2PSX : HUD_ORIGINAL);
		game_cmd_t cmd;
		memset(&cmd, 0, sizeof cmd);
		cmd.weapon_group = -1;
		/* select it: cycle until it is current */
		for (int n = 0; n < W_COUNT && pl.weapon != weapon; n++) {
			cmd.weapon_next = 1;
			game_tick(&cmd);
		}
		cmd.weapon_next = 0;
		cmd.fire = 1;
		for (int i = 0; i < 24; i++) {                  /* 0.4 s of fire */
			game_tick(&cmd);
			if (w == W_COUNT + 1)
				cmd.weapon_next = i == 0;               /* a switch: the Q2 HUD shows the row */
		}
		cmd.fire = 0;
		for (int i = 0; i < (w == W_LIGHTNING ? 1 : 6); i++)   /* (the beam lasts 0.2 s) */
			game_tick(&cmd);
		game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
		q_snprintf(names[w], sizeof names[w], w < W_COUNT ? "g%s" : w == W_COUNT ? "ghud1" : "ghud2",
		         w < W_COUNT ? wname[w] : "");
		bench_acc_t acc;
		bench_reset(&acc, names[w]);
		for (int i = 0; i < 4; i++)
			run_frame(0, 0);
		prof_sample_start();
		for (int i = 0; i < 16; i++) {
			run_frame(0, 0);
			bench_add(&acc, vid_stats.vbl);
		}
		prof_sample_stop();
		bench_print(&acc);
		prof_sample_print(names[w]);
		printf("D %s\n", names[w]);
		run_frame(0, BENCH_DUMP);
		for (int i = 0; i < 90; i++)                    /* the effects die down */
			game_tick(&cmd);
	}
	/* a walk: forward with the stick a little off axis, the view's bob
	   and roll and the gun's offset from the eye logged per tick, a frame
	   dumped every 6 ticks (the weapon's motion while moving) */
	{
		static char wn[8][8];
		game_spawn();
		pl.pm.pitch = 0;
		game_cmd_t cmd;
		memset(&cmd, 0, sizeof cmd);
		cmd.weapon_group = -1;
		cmd.fwd  = PM_AXIS;
		cmd.side = 20;
		for (int t = 0; t < 48; t++) {
			game_tick(&cmd);
			game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
			int32_t f[3], r[3], u[3];
			(void) f; (void) r; (void) u;
			printf("walk tick %d vel %d %d %d pos %d %d %d cam %d %d %d yaw %d pitch %d roll %d\n",
			       t, (int) pl.pm.vel[0], (int) pl.pm.vel[1], (int) pl.pm.vel[2],
			       (int) pl.pm.pos[0], (int) pl.pm.pos[1], (int) pl.pm.pos[2],
			       (int) cam.pos[0], (int) cam.pos[1], (int) cam.pos[2], cam.yaw, cam.pitch, cam.roll);
			if (t >= 18 && t % 6 == 0 && (t - 18) / 6 < 8) {
				q_snprintf(wn[(t - 18) / 6], sizeof wn[0], "gwalk%d", (t - 18) / 6);
				printf("D %s\n", wn[(t - 18) / 6]);
				run_frame(0, BENCH_DUMP);
			}
		}
	}
	hud_set_style(HUD_ORIGINAL);
	/* the water: the player put in it (a point of the map in water, as the
	   physics diff finds them), swimming forward with the jump held a while,
	   the level and type logged per tick; a frame under the surface (the tint,
	   the gun) and one at it */
	{
		int32_t p[3];
		int found = 0;
		for (int tries = 0; tries < 40000 && !found; tries++) {     /* water first, lava or slime failing that */
			rng_point(p, 0);
			int32_t eye[3]  = { p[0], p[1], p[2] + PM_EYE_HEIGHT * CLIP_SUB };
			int32_t feet[3] = { p[0], p[1], p[2] + (PM_MINS_Z + 1) * CLIP_SUB };
			int c = clip_contents_c(0, eye);                /* the eyes under: the tint; the feet too */
			found = (c == QW_CONTENTS_WATER || (tries >= 20000 && c <= QW_CONTENTS_WATER)) &&
			        clip_contents_c(0, feet) == c && clip_contents_c(1, p) == QW_CONTENTS_EMPTY;
		}
		if (found) {
			game_spawn();
			memcpy(pl.pm.pos, p, sizeof p);
			pl.pm.pitch = ANG_TURN * 20 / 360;                /* looking down: swims down */
			game_cmd_t cmd;
			memset(&cmd, 0, sizeof cmd);
			cmd.weapon_group = -1;
			cmd.fwd = PM_AXIS;
			for (int t = 0; t < 180; t++) {
				cmd.jump = t >= 60 && t < 120;
				game_tick(&cmd);
				if (t % 10 == 0)
					printf("water tick %d level %d type %d wj %d pos %d %d %d vel %d %d %d air %d\n", t,
					       pl.pm.waterlevel, pl.pm.watertype, pl.pm.waterjump, (int) pl.pm.pos[0], (int) pl.pm.pos[1],
					       (int) pl.pm.pos[2], (int) pl.pm.vel[0], (int) pl.pm.vel[1], (int) pl.pm.vel[2],
					       pl.air_finished - game_tick_count);
				if (t == 5 || t == 150) {
					game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
					for (int i = 0; i < 2; i++)
						run_frame(0, 0);
					printf("D %s\n", t == 5 ? "gwater" : "gwater2");
					run_frame(0, BENCH_DUMP);
				}
			}
		} else {
			printf("water: no point in water found on this map\n");
		}
	}
	bench_menu();
	snd_log = 0;
	bench_tick_cost();                                /* (last: they move the player) */
	bench_tick_rates();
	bench_walk();
	printf("bench frame cache: %u expansions, %u refused (every frame of the game bench)\n",
	       (unsigned) (mdl_stat_expanded - expanded), (unsigned) mdl_refused_since());
	game_active = 0;
	pad_read();                                       /* the interrupt-driven poll, seen once */
	printf("pad poll: %s type %d held %04x sticks %d %d %d %d\n", pad.connected ? "connected" : "absent",
	       pad.type, pad.held, pad.lx, pad.ly, pad.rx, pad.ry);
}

#ifndef Q1PSX_BENCH_MONSTERS
#define Q1PSX_BENCH_MONSTERS 0
#endif

/* the monsters' scenario: on MAP (default E1M1), the first monster of
   each kind there: the player put 250 units in front of it, standing; the
   monster wakes, fights (the player's health, its state, every half
   second), then the player fires the shotgun at it until it dies (its
   death, its backpack); dumps on the way */
static void mon_log(const char *what, const edict_t *e)
{
	if (game_ents_ticks) {                        /* the entities' cost since the last line */
		printf("mon cost: ents_tick avg %u max %u cycles, %u traces a tick (%u ticks)\n",
		       (unsigned) (game_ents_cycles / game_ents_ticks), (unsigned) game_ents_max,
		       (unsigned) (ai_traces / game_ents_ticks), (unsigned) game_ents_ticks);
		game_ents_cycles = game_ents_max = game_ents_ticks = 0;
		ai_traces = 0;
	}
	printf("mon %s t %d state %d frame %d pos %d %d %d yaw %d ideal %d health %d enemy %d as %d "
	       "flags %x | player health %d armor %d at %d %d %d | stream %d/%d in, %u reads, %u halved | "
	       "model %s, want %u\n", what,
	       (int) ent_time, e->mstate, e->frame, (int) e->origin[0] / CLIP_SUB, (int) e->origin[1] / CLIP_SUB,
	       (int) e->origin[2] / CLIP_SUB, e->ang[1] * 360 / 65536, e->ideal_yaw, e->health,
	       e->enemy ? (int) (e->enemy - edicts) : -1, e->attack_state, (unsigned) e->flags, pl.health,
	       pl.armor, (int) pl.pm.pos[0] / CLIP_SUB, (int) pl.pm.pos[1] / CLIP_SUB, (int) pl.pm.pos[2] / CLIP_SUB,
	       stream_stats.resident, stream_stats.wanted, (unsigned) stream_stats.reads,
	       (unsigned) stream_stats.halved, e->inuse && stream_model(e->masset) ? "in" : "OUT (paused)",
	       (unsigned) stream_score(e->masset));
}

/* the stream manager's frames between two ticks (the game draws about
   two a tick at 32 Hz: its passes, reads and uploads run on them) */
static void bench_stream_frames(void)
{
	stream_frame();
	music_tick();                                 /* (the music's stream goes on too) */
	stream_frame();
	music_tick();
}

/* the scenario's monster and the player both solid, their boxes
   overlapping (a step or a push that went through): the first tick of
   each overlap printed */
static void mon_overlap(const char *what, int t, const edict_t *e)
{
	static int was;
	const edict_t *p = ent_player;
	int lap = e->solid != SOLID_NOT && p->solid != SOLID_NOT && e->health > 0 &&
	          e->origin[0] + e->mins[0] < p->origin[0] + p->maxs[0] &&
	          e->origin[0] + e->maxs[0] > p->origin[0] + p->mins[0] &&
	          e->origin[1] + e->mins[1] < p->origin[1] + p->maxs[1] &&
	          e->origin[1] + e->maxs[1] > p->origin[1] + p->mins[1] &&
	          e->origin[2] + e->mins[2] < p->origin[2] + p->maxs[2] &&
	          e->origin[2] + e->maxs[2] > p->origin[2] + p->mins[2];
	if (lap && !was)
		printf("mon OVERLAP %s t %d: state %d frame %d flags %x as %d movetype %d pos %d %d %d vel %d %d %d | "
		       "player %d %d %d vel %d %d %d ground %d\n", what, t, e->mstate, e->frame, (unsigned) e->flags,
		       e->attack_state, e->movetype, (int) e->origin[0] / CLIP_SUB, (int) e->origin[1] / CLIP_SUB,
		       (int) e->origin[2] / CLIP_SUB, (int) e->velocity[0], (int) e->velocity[1], (int) e->velocity[2],
		       (int) pl.pm.pos[0] / CLIP_SUB, (int) pl.pm.pos[1] / CLIP_SUB, (int) pl.pm.pos[2] / CLIP_SUB,
		       (int) pl.pm.vel[0], (int) pl.pm.vel[1], (int) pl.pm.vel[2], pl.pm.onground);
	was = lap;
}

/* engine/mdl_expand.s's mdl_frame_avg against its formula, on made-up
   vertex words (the frames rebuilt from their neighbours, §97) */
static void bench_frame_avg_check(void)
{
	uint32_t a[64], b[64], d[64], x = 0x9e3779b9u, bad = 0;
	for (int i = 0; i < 64; i++) {
		x = x * 1664525u + 1013904223u; a[i] = x;
		x = x * 1664525u + 1013904223u; b[i] = x;
	}
	mdl_frame_avg(d, a, b, 64);
	for (int i = 0; i < 64; i++) {
		uint32_t w = a[i] & 0xff000000u;
		for (int k = 0; k < 24; k += 8)
			w |= ((a[i] >> k & 0xff) + (b[i] >> k & 0xff)) >> 1 << k;
		bad += d[i] != w;
	}
	printf("mon frame average: %u of 64 words wrong\n", (unsigned) bad);
}

static void bench_monsters(void)
{
	bench_frame_avg_check();
	const char *mapname = Q1PSX_BENCH_MAP[0] ? Q1PSX_BENCH_MAP : "E1M1";
	int m = world_find_map(mapname);
	if (m < 0 || !load_level(m)) {
		printf("mon: no %s\n", mapname);
		return;
	}
	map_index = m;
	stream_log = 1;
	snd_log = 1;
	game_active = 1;
	game_cmd_t cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.weapon_group = -1;
	game_spawn();
	ents_ticks(60, &cmd);                         /* the monsters' delayed starts */
	for (int k = 0; k < (Q1PSX_BENCH_MONSTERS >= 2 ? 0 : MK_COUNT); k++) {   /* (2, 3: the crowd only) */
		/* a monster of the kind with a free spot in its sight: 450 .. 100
		   units off (its ranged attack's reach first), its facing first,
		   then around it */
		edict_t *e = 0;
		int32_t spot[3];
		trace_t tr;
		for (int i = 2; i < num_edicts && !e; i++) {
			edict_t *c = &edicts[i];
			if (!c->inuse || !c->mdef || c->mdef->kind != k || c->health <= 0)
				continue;
			static const int turns[16] = { 0, 45, -45, 90, -90, 135, -135, 180,
			                               22, -22, 67, -67, 112, -112, 157, -157 };
			for (int a = 0; a < 16 && !e; a++)          /* (the second eight: farther, for */
				for (int d = a < 8 ? 450 : 1000; d >= 100 && !e; d -= 25)   /* Chthon's lava, */
				for (int up = 0; up <= (a < 8 ? 0 : 512) && !e; up += 64) {  /* higher too) */
					int yaw = (c->ang[1] >> 4) + turns[a] * ANG_TURN / 360;
					spot[0] = c->origin[0] + d * CLIP_SUB * r_cos(yaw) / 4096;
					spot[1] = c->origin[1] + d * CLIP_SUB * r_sin(yaw) / 4096;
					spot[2] = c->origin[2] + up * CLIP_SUB;
					clip_trace(1, spot, spot, &tr);
					if (tr.startsolid || tr.allsolid)
						continue;
					/* standing on a floor (no fall), out of the liquids */
					int32_t down[3] = { spot[0], spot[1], spot[2] - 64 * CLIP_SUB };
					clip_trace(1, spot, down, &tr);
					if (tr.startsolid || tr.fraction >= Q_FIX_ONE)
						continue;
					memcpy(spot, tr.endpos, sizeof spot);
					int32_t feet[3] = { spot[0], spot[1], spot[2] - 23 * CLIP_SUB };
					int fc = clip_contents(0, feet);   /* (a swimmer's: in its water) */
					if (fc != QW_CONTENTS_EMPTY && !(c->mdef->fly == MS_SWIM && fc == QW_CONTENTS_WATER))
						continue;
					int32_t ea[3] = { spot[0], spot[1], spot[2] + 22 * CLIP_SUB }, eb[3];
					ai_view_org(c, eb);
					if (ai_traceline(ea, eb, 1, c) >= Q_FIX_ONE)   /* in its sight */
						e = c;
				}
		}
		if (!e) {
			printf("mon kind %d: no monster with a free spot in sight\n", k);
			continue;
		}
		memcpy(pl.pm.pos, spot, sizeof pl.pm.pos);
		memset(pl.pm.vel, 0, sizeof pl.pm.vel);
		pl.health = 100;
		pl.dead = 0;
		ents_look_at(e->origin);
		if (e->use && e->solid == SOLID_NOT) {    /* the boss: its trigger's use, it rises */
			ent_activator = ent_player;
			ENT_CALL(e, use);
		}
		char name[32];
		q_snprintf(name, sizeof name, "m%s_%d_a", mapname, k);
		mon_log("start", e);
		for (int t = 0; t < 8; t++) {             /* the stream's reads, while the frames run */
			game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
			run_frame(0, 0);
			stream_frame();
		}
		ents_dump(name);
		for (int t = 0; t < 300; t++) {           /* it sees the player, fights */
			ents_look_at(e->origin);
			game_tick(&cmd);
			bench_stream_frames();
			mon_overlap("fight", t, e);
			if (t % 30 == 0) {
				mon_log("fight", e);
				if (t == 0)
					printf("mon sees: visible %d checkclient %d range %d infront %d\n",
					       ai_visible(e, ent_player), ai_checkclient(e) != 0, ai_range(e, ent_player),
					       ai_infront(e, ent_player));

			}
			if (t == 150) {
				q_snprintf(name, sizeof name, "m%s_%d_b", mapname, k);
				ents_dump(name);
			}
			if (pl.dead)
				break;
		}
		/* the player answers: the shotgun until it dies */
		pl.health = 100;
		pl.dead = 0;
		pl.items |= IT_SHOTGUN;
		pl.ammo[AMMO_SHELLS] = 100;
		game_give_weapon(W_SHOTGUN);
		cmd.fire = 1;
		for (int t = 0; t < 600 && e->health > 0; t++) {
			ents_look_at(e->origin);
			game_tick(&cmd);
			bench_stream_frames();
			mon_overlap("shot", t, e);
			if (t % 30 == 0)
				mon_log("shot", e);
		}
		cmd.fire = 0;
		if (e->health > 0 && e->mdef && e->mdef->kind == MK_OLDONE) {
			/* Shub-Niggurath: the shotgun never kills it; the teleporter's
			   telefrag (tdeath_touch's 50000), then the finale's parts
			   dumped: the fog, the thrash, the gibs and the statue, the
			   text */
			ent_damage(e, ent_world, ent_world, 50000);
			static const int16_t at[4] = { TICKS(150), TICKS(400), TICKS(900), TICKS(1500) };
			int32_t t0 = ent_time;
			for (int part = 0; part < 4; part++) {
				while (ent_time < t0 + at[part]) {
					game_tick(&cmd);
					bench_stream_frames();
				}
				printf("mon finale %d: t %d, intermission %d, light style dim %d, shub %s, text %s\n", part,
				       (int) (ent_time - t0), level_intermission, level_style0_dim(),
				       e->inuse ? "there" : "gone", level_finale_text && level_finale_text[0] ? "out" : "none");
				q_snprintf(name, sizeof name, "m%s_%d_f%d", mapname, k, part);
				ents_dump(name);
			}
			continue;
		}
		mon_log("killed", e);
		for (int t = 0; t < 120; t++) {           /* its death, its backpack */
			game_tick(&cmd);
			bench_stream_frames();
		}
		mon_log("dead", e);
		q_snprintf(name, sizeof name, "m%s_%d_c", mapname, k);
		ents_look_at(e->origin);
		{                                         /* down at the body */
			int32_t dz = pl.pm.pos[2] + pl.viewheight - e->origin[2] + 16 * CLIP_SUB;
			int32_t dx = e->origin[0] - pl.pm.pos[0], dy = e->origin[1] - pl.pm.pos[1];
			pl.pm.pitch = r_atan2(dz, (int32_t) q_isqrt64((uint64_t) ((int64_t) dx * dx + (int64_t) dy * dy)));
		}
		ents_dump(name);
		/* its body darkened in its last second (a picture halfway), then
		   gone (monsters.h CORPSE_TICKS); a zombie the shotgun never kills */
		int32_t end = e->health <= 0 ? e->pain_finished : ent_time;
		while (e->inuse && ent_time < end - CORPSE_FADE / 2) {
			game_tick(&cmd);
			bench_stream_frames();
		}
		q_snprintf(name, sizeof name, "m%s_%d_d", mapname, k);
		if (e->inuse)
			ents_dump(name);
		while (e->inuse && ent_time < end + 2) {
			game_tick(&cmd);
			bench_stream_frames();
		}
		if (e->health <= 0)
			printf("mon body: %s at t %d (dead at %d)\n", e->inuse ? "STILL THERE" : "gone", (int) ent_time,
			       (int) (end - CORPSE_TICKS));
		pl.pm.pitch = 0;
	}
	printf("mon frames: %u expansions, %u of them rebuilt from their neighbours (QST_LERP)\n",
	       (unsigned) mdl_stat_expanded, (unsigned) mdl_stat_lerped);
	/* the crowd: every monster of the map awake and after the player (who
	   cannot die), the entities' cost a second */
	int awake = 0;
	for (int i = 2; i < num_edicts; i++) {
		edict_t *c = &edicts[i];
		if (!c->inuse || !c->mdef || c->health <= 0 || c->takedamage == DAMAGE_NO)
			continue;
		c->enemy = ent_player;
		ai_found_target(c);
		awake++;
	}
	printf("mon crowd: %d monsters awake\n", awake);
	ents_area_stats();
	game_ents_cycles = game_ents_max = game_ents_ticks = 0;
	ai_traces = 0;
	memset(ai_tr_n, 0, sizeof ai_tr_n);
	memset(ai_tr_cyc, 0, sizeof ai_tr_cyc);
	prof_sample_start();
	for (int t = 0; t < 600; t++) {
		pl.health = 100;
		pl.dead = 0;
		game_tick(&cmd);
		bench_stream_frames();
		if (t % 60 == 59) {
			int alive = 0;
			for (int i = 2; i < num_edicts; i++)
				alive += edicts[i].inuse && edicts[i].mdef && edicts[i].health > 0;
			printf("mon crowd t %d: %d alive, ents_tick avg %u max %u cycles, %u traces a tick\n", t + 1, alive,
			       (unsigned) (game_ents_cycles / game_ents_ticks), (unsigned) game_ents_max,
			       (unsigned) (ai_traces / game_ents_ticks));
			static const char *const tag[AI_TR_TAGS] = { "trigtests", "visible", "attack", "bottom", "walk",
			                                              "fly", "corners", "touch" };
			printf("mon crowd costs:");
			for (int g = 0; g < AI_TR_TAGS; g++) {   /* a tick's: count x average = cycles */
				printf(" %s %u.%u x %u = %u", tag[g], (unsigned) (ai_tr_n[g] / game_ents_ticks),
				       (unsigned) (ai_tr_n[g] * 10 / game_ents_ticks % 10),
				       (unsigned) (ai_tr_n[g] ? ai_tr_cyc[g] / ai_tr_n[g] : 0),
				       (unsigned) (ai_tr_cyc[g] / game_ents_ticks));
				ai_tr_n[g] = ai_tr_cyc[g] = 0;
			}
			printf(" | %d clip entries, %d touch\n", clip_num_bmodels, ents_num_touch());
			game_ents_cycles = game_ents_max = game_ents_ticks = 0;
			ai_traces = 0;
		}
	}
	prof_sample_stop();
	prof_sample_print("crowd");
	if (Q1PSX_BENCH_MONSTERS == 3) {
		/* the fight (a busy level played): the player, who cannot die,
		   fires rockets at the nearest monster it sees (turning round when
		   none), a frame drawn every tick; the stack's high-water mark, the
		   edicts and the pools logged */
		pl.items |= IT_ROCKET_LAUNCHER;
		game_give_weapon(W_RLAUNCHER);
		cmd.fire = 1;
		int peak = 0;
		uint32_t fsum = 0, fdrawn = 0, pkt_max = 0;  /* (the frames' cycles, the monsters drawn,
		                                                the packet bytes a frame at most, the
		                                                least room a model found) */
		int32_t room_min = 0x7fffffff;
		uint32_t skipped0 = r_model_skipped, full = 0;
		mdl_refused_since();
		prof_sample_start();                         /* (SAMPLE=1: the fight's profile) */
		for (int t = 0; t < 3600; t++) {
			pl.health = 100;
			pl.dead = 0;
			pl.ammo[AMMO_ROCKETS] = 100;
			edict_t *aim = 0;                    /* the nearest living monster it sees */
			int32_t best = 0;
			for (int i = 2; i < num_edicts; i++) {
				edict_t *c = &edicts[i];
				if (!c->inuse || !c->mdef || c->health <= 0 || !ai_visible(ent_player, c))
					continue;
				int32_t dx = (c->origin[0] - pl.pm.pos[0]) >> 3, dy = (c->origin[1] - pl.pm.pos[1]) >> 3;
				int32_t d2 = dx * dx + dy * dy;
				if (!aim || d2 < best) {
					aim = c;
					best = d2;
				}
			}
			if (aim)
				ents_look_at(aim->origin);
			else
				pl.pm.yaw += ANG_TURN / 240;     /* none: a turn in 4 s */
			game_tick(&cmd);
			game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
			run_frame(0, 0);
			fsum += prof_stage[PROF_FRAME];
			fdrawn += (uint32_t) monsters_drawn;
			if (vid_stats.pkt_bytes > pkt_max)
				pkt_max = vid_stats.pkt_bytes;
			if (r_model_room_min < room_min)
				room_min = r_model_room_min;
			full += r_arena_full != 0;
#if !Q1PSX_ENGINE_C
			if (!(t & 15))
				ents_seen_check();                /* (e_seen.s against its C, untimed; a
				                                     frame in 16: the samples' profile) */
#endif
			int max, used = ents_in_use(&max);
			if (used > peak)
				peak = used;
			if (t % 300 == 299) {
				int alive = 0;
				for (int i = 2; i < num_edicts; i++)
					alive += edicts[i].inuse && edicts[i].mdef && edicts[i].health > 0;
				static uint32_t exp0, lerp0, st0, sl0, rp0, ct0, ms0;
				printf("mon fight t %d: %d alive, edicts %d (peak %d) of %d, stack %u never reached, "
				       "level arena %u KB, items %d, vbl %u; frame %u cycles, %u.%u streamed models drawn, "
				       "the frame cache refused %u, packets %u bytes at most, a model's room %d at least, "
				       "%u models left out, %u full frames, %u expansions (%u rebuilt)\n",
				       t + 1, alive, used, peak, max, (unsigned) stack_untouched(),
				       (unsigned) (mem_level_used() / 1024), rl_num_items(), (unsigned) vid_stats.vbl,
				       (unsigned) (fsum / 300), (unsigned) (fdrawn / 300), (unsigned) (fdrawn * 10 / 300 % 10),
				       (unsigned) mdl_refused_since(), pkt_max, room_min == 0x7fffffff ? -1 : (int) room_min,
				       (unsigned) (r_model_skipped - skipped0), full, (unsigned) (mdl_stat_expanded - exp0),
				       (unsigned) (mdl_stat_lerped - lerp0));
				exp0  = mdl_stat_expanded;
				lerp0 = mdl_stat_lerped;
				printf("mon sounds t %d: %u started, %u cut by another (the dynamic channels full), %u by their "
				       "own channel, %u stopped (their samples swapped out), %u unheard (not in SPU RAM)\n", t + 1,
				       (unsigned) (snd_stat_started - st0), (unsigned) (snd_stat_stolen - sl0),
				       (unsigned) (snd_stat_replaced - rp0), (unsigned) (snd_stat_cut - ct0),
				       (unsigned) (stream_stats.snd_missing - ms0));
				st0 = snd_stat_started; sl0 = snd_stat_stolen; rp0 = snd_stat_replaced;
				ct0 = snd_stat_cut; ms0 = stream_stats.snd_missing;
				fsum = fdrawn = pkt_max = full = 0;
				room_min = 0x7fffffff;
				skipped0 = r_model_skipped;
			}
		}
		cmd.fire = 0;
		prof_sample_stop();
		prof_sample_print("fight");
		bench_bmf_arena("fight");
#if !Q1PSX_ENGINE_C
		printf("seendiff %u walks: mismatches %u\n", (unsigned) ents_seen_checked, (unsigned) ents_seen_bad);
#endif
	}
}

/* MONSTERS=4: the map cleared, as a player would: each living monster in
   turn, the player put on a free spot in its sight (the scenarios' search,
   any kind), rockets until it dies (10 s at most), a frame drawn every
   tick; then the bodies' and gibs' time. "clear" lines: what was killed,
   the stack meter, the edicts, the arena. */
static void clear_map(void)
{
	game_cmd_t cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.weapon_group = -1;
	ents_ticks(60, &cmd);
	pl.items |= IT_ROCKET_LAUNCHER;
	game_give_weapon(W_RLAUNCHER);
	int killed = 0, peak = 0, skipped = 0;
	uint32_t ctot = 0, cframes = 0, cdrawn = 0, ccut = 0, cpkt = 0;   /* the fights' frames */
	int32_t croom = 0x7fffffff;
	uint32_t cskipped0 = r_model_skipped, cfull = 0;
	static uint8_t tried[1024];
	memset(tried, 0, sizeof tried);
	for (;;) {
		edict_t *e = 0;
		int32_t spot[3];
		trace_t tr;
		for (int i = 2; i < num_edicts && i < 1024 && !e; i++) {
			edict_t *c = &edicts[i];
			if (!c->inuse || !c->mdef || c->health <= 0 || tried[i])
				continue;
			static const int turns[8] = { 0, 45, -45, 90, -90, 135, -135, 180 };
			for (int a = 0; a < 8 && !e; a++)
				for (int d = 450; d >= 100 && !e; d -= 25) {
					int yaw = (c->ang[1] >> 4) + turns[a] * ANG_TURN / 360;
					spot[0] = c->origin[0] + d * CLIP_SUB * r_cos(yaw) / 4096;
					spot[1] = c->origin[1] + d * CLIP_SUB * r_sin(yaw) / 4096;
					spot[2] = c->origin[2];
					clip_trace(1, spot, spot, &tr);
					if (tr.startsolid || tr.allsolid)
						continue;
					int32_t down[3] = { spot[0], spot[1], spot[2] - 64 * CLIP_SUB };
					clip_trace(1, spot, down, &tr);
					if (tr.startsolid || tr.fraction >= Q_FIX_ONE)
						continue;
					memcpy(spot, tr.endpos, sizeof spot);
					int32_t feet[3] = { spot[0], spot[1], spot[2] - 23 * CLIP_SUB };
					int fc = clip_contents(0, feet);   /* (a swimmer's: in its water) */
					if (fc != QW_CONTENTS_EMPTY && !(c->mdef->fly == MS_SWIM && fc == QW_CONTENTS_WATER))
						continue;
					int32_t ea[3] = { spot[0], spot[1], spot[2] + 22 * CLIP_SUB }, eb[3];
					ai_view_org(c, eb);
					if (ai_traceline(ea, eb, 1, c) >= Q_FIX_ONE)
						e = c;
				}
			if (!e) {
				tried[i] = 1;
				skipped++;
			}
		}
		if (!e)
			break;
		tried[e - edicts] = 1;
		memcpy(pl.pm.pos, spot, sizeof pl.pm.pos);
		memset(pl.pm.vel, 0, sizeof pl.pm.vel);
		int kind = e->mdef->kind, t = 0;
		cmd.fire = 1;
		for (; t < 600 && e->inuse && e->health > 0; t++) {
			pl.health = 100;
			pl.dead = 0;
			pl.ammo[AMMO_ROCKETS] = 100;
			ents_look_at(e->origin);
			game_tick(&cmd);
			game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
			run_frame(0, 0);
			ctot += prof_stage[PROF_FRAME] >> 8;
			cframes++;
			cdrawn += (uint32_t) monsters_drawn;
			ccut += (uint32_t) rl_lod_cut;
			if (vid_stats.pkt_bytes > cpkt)
				cpkt = vid_stats.pkt_bytes;
			if (r_model_room_min < croom)
				croom = r_model_room_min;
			cfull += r_arena_full != 0;
		}
		cmd.fire = 0;
		for (int u = 0; u < 60; u++) {            /* its death, its gibs */
			pl.health = 100;
			pl.dead = 0;
			game_tick(&cmd);
			game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
			run_frame(0, 0);
		}
		int max, used = ents_in_use(&max);
		if (used > peak)
			peak = used;
		killed += e->health <= 0 || !e->inuse;
		printf("clear %d: kind %d %s in %d ticks, edicts %d (peak %d) of %d, stack %u never reached, "
		       "level arena %u KB, vbl %u\n", (int) (e - edicts), kind,
		       e->health <= 0 || !e->inuse ? "dead" : "ALIVE", t, used, peak, max,
		       (unsigned) stack_untouched(), (unsigned) (mem_level_used() / 1024), (unsigned) vid_stats.vbl);
	}
	for (int u = 0; u < 1800; u++) {              /* the bodies' and gibs' time, 30 s */
		pl.health = 100;
		pl.dead = 0;
		game_tick(&cmd);
		game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
		run_frame(0, 0);
	}
	int max, used = ents_in_use(&max);
	int alive = 0;
	for (int i = 2; i < num_edicts; i++)
		alive += edicts[i].inuse && edicts[i].mdef && edicts[i].health > 0;
	printf("clear done %s: %d killed, %d alive (%d with no spot), edicts %d (peak %d) of %d, stack %u never "
	       "reached, level arena %u KB\n", world.map_name, killed, alive, skipped, used, peak, max,
	       (unsigned) stack_untouched(), (unsigned) (mem_level_used() / 1024));
	printf("clear frames %s: %u fight frames, %u x 256 cycles, %u streamed models drawn, %u cut to a band, "
	       "packets %u bytes at most, a model's room %d at least, %u models left out, %u full frames\n",
	       world.map_name, (unsigned) cframes, (unsigned) ctot, (unsigned) cdrawn, (unsigned) ccut, cpkt,
	       croom == 0x7fffffff ? -1 : (int) croom, (unsigned) (r_model_skipped - cskipped0), cfull);
	bench_bmf_arena("clear");
}

/* MONSTERS=4: MAP (E1M3) loaded and cleared. MONSTERS=5: from MAP (E1M1),
   each map cleared, then its exit taken as a player's (the intermission,
   the parms, the next map's load and spawn), to the episode's end. */
static void bench_clear(void)
{
	const char *mapname = Q1PSX_BENCH_MAP[0] ? Q1PSX_BENCH_MAP : Q1PSX_BENCH_MONSTERS == 5 ? "E1M1" : "E1M3";
	int m = world_find_map(mapname);
	if (m < 0 || !load_level(m)) {
		printf("clear: no %s\n", mapname);
		return;
	}
	map_index = m;
	game_active = 1;
	game_spawn();
	for (int n = 0; n < 12; n++) {
		clear_map();
		if (Q1PSX_BENCH_MONSTERS != 5)
			return;
		game_cmd_t cmd;
		memset(&cmd, 0, sizeof cmd);
		cmd.weapon_group = -1;
		edict_t *cl = ents_first(QE_TRIGGER_CHANGELEVEL, 0);
		if (!cl) {
			printf("clear: %s has no exit\n", world.map_name);
			return;
		}
		ents_place_at(cl, 0);
		ents_ticks(12, &cmd);
		ents_ticks(150, &cmd);                    /* the intermission */
		cmd.fire = 1;
		ents_ticks(2, &cmd);
		cmd.fire = 0;
		char next[16];
		if (!level_change_wanted(next)) {
			printf("clear: %s's exit wants no map\n", world.map_name);
			return;
		}
		int nm = world_find_map(next);
		printf("clear: next map %s (disc %d)\n", next, nm);
		if (nm < 0)
			return;
		game_save_parms();
		map_index = nm;
		if (!load_level(nm)) {
			printf("clear: %s failed to load\n", next);
			return;
		}
		game_spawn_level();
		pl.items |= IT_ROCKET_LAUNCHER;
	}
}

/* the floor under (x, y) from z down 512 units, the player's origin on it
   (24 units up: 0 if none) */
static int floor_at(int32_t x, int32_t y, int32_t z, int32_t out[3])
{
	trace_t tr;
	int32_t a[3] = { x, y, z }, b[3] = { x, y, z - 512 * CLIP_SUB };
	clip_trace(1, a, a, &tr);
	if (tr.startsolid || tr.allsolid)
		return 0;
	clip_trace(1, a, b, &tr);
	if (tr.fraction >= Q_FIX_ONE)
		return 0;
	memcpy(out, tr.endpos, sizeof tr.endpos);
	return 1;
}

/* a phase of MONSTERS=6: `ticks` ticks walking along `yaw` (binary
   degrees) or, with a monster in sight, firing at the nearest one; a frame
   drawn each tick */
static void key_phase(const char *what, int ticks, int yaw)
{
	game_cmd_t cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.weapon_group = -1;
	for (int t = 0; t < ticks; t++) {
		pl.health = 100;
		pl.dead = 0;
		pl.ammo[AMMO_ROCKETS] = 100;
		edict_t *aim = 0;
		int32_t best = 0;
		for (int i = 2; i < num_edicts; i++) {
			edict_t *c = &edicts[i];
			if (!c->inuse || !c->mdef || c->health <= 0 || !ai_visible(ent_player, c))
				continue;
			int32_t dx = (c->origin[0] - pl.pm.pos[0]) >> 3, dy = (c->origin[1] - pl.pm.pos[1]) >> 3;
			int32_t d2 = dx * dx + dy * dy;
			if (!aim || d2 < best) {
				aim = c;
				best = d2;
			}
		}
		if (aim && best < 600 * 600) {
			ents_look_at(aim->origin);
			cmd.fire = 1;
			cmd.fwd = 0;
		} else {
			pl.pm.yaw = yaw;
			cmd.fire = 0;
			cmd.fwd = PM_AXIS;
		}
		game_tick(&cmd);
		game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
		run_frame(0, 0);
	}
	int max, used = ents_in_use(&max), alive = 0;
	for (int i = 2; i < num_edicts; i++)
		alive += edicts[i].inuse && edicts[i].mdef && edicts[i].health > 0;
	const edict_t *d21 = 0;
	for (int i = 2; i < num_edicts; i++)
		if (edicts[i].inuse && edicts[i].model == 21)
			d21 = &edicts[i];
	printf("key %s: at %d %d %d, keys %x, door *21 at %d %d %d, %d alive, edicts %d of %d, stack %u never "
	       "reached, vbl %u\n", what, (int) (pl.pm.pos[0] / CLIP_SUB), (int) (pl.pm.pos[1] / CLIP_SUB),
	       (int) (pl.pm.pos[2] / CLIP_SUB), (unsigned) (pl.items & (IT_KEY1 | IT_KEY2)),
	       d21 ? (int) (d21->origin[0] / CLIP_SUB) : 0, d21 ? (int) (d21->origin[1] / CLIP_SUB) : 0,
	       d21 ? (int) (d21->origin[2] / CLIP_SUB) : 0, alive, used, max, (unsigned) stack_untouched(),
	       (unsigned) vid_stats.vbl);
}

/* MONSTERS=6: E1M3's gold key (the crash reported 2026-10-22, usually on the way back from the key): the monsters within
   1200 units of the key awake and after the player; the key picked up
   among them; then the gold doors (*21, *22, x -496) from their east and
   west sides in turn, walked into, through and back, four times */
static void bench_key(void)
{
	int m = world_find_map("E1M3");
	if (m < 0 || !load_level(m)) {
		printf("key: no E1M3\n");
		return;
	}
	map_index = m;
	game_active = 1;
	game_spawn();
	pl.items &= ~(IT_KEY1 | IT_KEY2);
	pl.items |= IT_ROCKET_LAUNCHER;
	game_give_weapon(W_RLAUNCHER);
	game_cmd_t cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.weapon_group = -1;
	ents_ticks(60, &cmd);
	edict_t *key = ents_first(QE_ITEM_KEY2, 0);
	if (!key) {
		printf("key: no gold key\n");
		return;
	}
	int awake = 0;
	for (int i = 2; i < num_edicts; i++) {
		edict_t *c = &edicts[i];
		if (!c->inuse || !c->mdef || c->health <= 0 || c->takedamage == DAMAGE_NO)
			continue;
		int32_t dx = (c->origin[0] - key->origin[0]) >> 3, dy = (c->origin[1] - key->origin[1]) >> 3;
		if (dx * dx + dy * dy > 1200 * 1200)
			continue;
		c->enemy = ent_player;
		ai_found_target(c);
		awake++;
	}
	printf("key: %d monsters awake around the key\n", awake);
	/* the secret doors opened (the crash: an opened one's stop read
	   as a monster's kind once the lists were rebuilt), then a temporary
	   edict spawned and removed: the lists rebuilt with them open */
	int secrets = 0;
	for (int i = 2; i < num_edicts; i++) {
		edict_t *c = &edicts[i];
		if (c->inuse && c->class == QE_FUNC_DOOR_SECRET && c->use) {
			ent_activator = ent_player;
			ENT_CALL(c, use);
			secrets++;
		}
	}
	key_phase("secret doors opening", 240, 0);
	edict_t *tmp = ent_spawn();
	if (tmp)
		ent_remove(tmp);
	key_phase("lists rebuilt", 60, 0);
	printf("key: %d secret doors opened, edicts %s\n", secrets, ents_check("secret doors") ? "ok" : "CORRUPT");
	int32_t at[3];
	if (!floor_at(key->origin[0], key->origin[1], key->origin[2] + 32 * CLIP_SUB, at)) {
		printf("key: no floor at the key\n");
		return;
	}
	memcpy(pl.pm.pos, at, sizeof pl.pm.pos);
	memset(pl.pm.vel, 0, sizeof pl.pm.vel);
	key_phase("picked", 300, 0);
	int32_t west[3], east[3];
	int ok_w = floor_at(-560 * CLIP_SUB, -160 * CLIP_SUB, 0, west), ok_e = floor_at(-430 * CLIP_SUB, -160 * CLIP_SUB, 0, east);
	printf("key: the door's sides: west %d, east %d\n", ok_w, ok_e);
	for (int n = 0; n < 4; n++) {
		if (ok_w) {
			memcpy(pl.pm.pos, west, sizeof pl.pm.pos);
			memset(pl.pm.vel, 0, sizeof pl.pm.vel);
			key_phase("west to east", 300, 0);                   /* yaw 0: +x */
		}
		if (ok_e) {
			memcpy(pl.pm.pos, east, sizeof pl.pm.pos);
			memset(pl.pm.vel, 0, sizeof pl.pm.vel);
			key_phase("east to west", 300, ANG_TURN / 2);       /* -x */
		}
		memcpy(pl.pm.pos, at, sizeof pl.pm.pos);              /* back at the key's place */
		memset(pl.pm.vel, 0, sizeof pl.pm.vel);
		key_phase("at the key again", 200, 0);
	}
	printf("key done, edicts %s\n", ents_check("key done") ? "ok" : "CORRUPT");
}

/* the bench's end: the interrupt stack's room (engine/irq.c), then the
   line scripts/bench.sh waits for */
static void bench_done(void)
{
	extern int irq_stack_room(void);
	cd_log_mark("end");                         /* (the drive journal's last phase totalled) */
	printf("isr stack: %d bytes never reached\n", irq_stack_room());
	printf("bench done\n");
}

static void run_bench(void)
{
	static char names[16][8];

	printf("bench start\n");
	zone_sync = 1;                              /* (a view's zone read before its frame) */
	if (Q1PSX_BENCH_ZONES) {
		bench_zones();
		bench_done();
		for (;;)
			run_frame(0, 0);
	}
	if (Q1PSX_BENCH_MONSTERS == 6) {
		bench_key();
		bench_done();
		for (;;)
			run_frame(0, 0);
	}
	if (Q1PSX_BENCH_MONSTERS >= 4) {
		bench_clear();
		bench_done();
		for (;;)
			run_frame(0, 0);
	}
	if (Q1PSX_BENCH_MONSTERS) {
		bench_monsters();
		bench_done();
		for (;;)
			run_frame(0, 0);
	}
	if (Q1PSX_BENCH_MAPS) {
		bench_maps();
		bench_done();
		for (;;)
			run_frame(0, 0);
	}
	if (Q1PSX_BENCH_ENTS) {
		bench_ents();
		bench_done();
		return;
	}
	if (Q1PSX_BENCH_MOVERS) {
		bench_movers();
		bench_done();
		for (;;)
			run_frame(0, 0);
	}
	if (Q1PSX_BENCH_GAME) {
		bench_game();
		bench_done();
		for (;;)
			run_frame(0, 0);
	}
	if (world_ok) {
		bench_clip_diff();
		bench_wleaf_diff();
#if !Q1PSX_ENGINE_C
		ents_touch_diff(4000);
#endif
		bench_pmove_diff();
	}
	for (uint32_t v = 0; Q1PSX_BENCH_VIEWS && world_ok && v < world.num_views && v < 16; v++) {
		const qw_view *vw = &world.views[v];
		cam.pos[0] = vw->pos[0] * CLIP_SUB;
		cam.pos[1] = vw->pos[1] * CLIP_SUB;
		cam.pos[2] = vw->pos[2] * CLIP_SUB;
		cam.yaw    = vw->yaw * ANG_TURN / 360;
		cam.pitch  = vw->pitch * ANG_TURN / 360;
		cam.roll   = 0;
		q_snprintf(names[v], sizeof names[v], "view%u", v);

		bench_acc_t acc;
		bench_reset(&acc, names[v]);
		for (int i = 0; i < BENCH_WARMUP; i++)
			run_frame(0, 0);
		for (int i = 0; i < BENCH_FRAMES; i++) {
			run_frame(0, 0);
			bench_add(&acc, vid_stats.vbl);
		}
		bench_print(&acc);
		if (Q1PSX_BENCH_WARPDYN)
			printf("warp %s T %d more %d\n", names[v], r_warp.t_eff, r_warp.more);
		printf("D view %u\n", v);
		run_frame(0, BENCH_DUMP);
	}
	/* scripted movement (no pad in headless runs): settle, accelerate,
	   jump, run north into the end wall (stairs down, ramp up), then turn
	   around and come back up the stairs. Same path as tools/pmsim. */
	{
		pmove_cmd_t c;
		uint32_t    cyc_sum = 0, cyc_max = 0, ticks = 0;
		memset(&c, 0, sizeof c);
		c.dt = 1;
		player_spawn();
#define PM_TICK() do { \
			uint32_t t0_ = prof_now(); \
			pmove_step(&player, &c); \
			uint32_t cy_ = prof_cycles(t0_, prof_now()); \
			cyc_sum += cy_; ticks++; \
			if (cy_ > cyc_max) cyc_max = cy_; \
		} while (0)
		for (int i = 0; i < 30; i++)
			PM_TICK();
		printf("pmove settle pos %d %d %d ground %d\n", (int) player.pos[0] / CLIP_SUB,
		       (int) player.pos[1] / CLIP_SUB, (int) player.pos[2] / CLIP_SUB, player.onground);
		c.fwd = PM_AXIS;
		int t310 = -1;
		for (int i = 0; i < 60; i++) {
			PM_TICK();
			int32_t sp = (int32_t) q_isqrt64((uint64_t) ((int64_t) player.vel[0] * player.vel[0] +
			                                             (int64_t) player.vel[1] * player.vel[1]));
			if (t310 < 0 && sp >= 310)
				t310 = i + 1;
		}
		printf("pmove run: 310 u/s after %d ticks, pos %d %d %d\n", t310,
		       (int) player.pos[0] / CLIP_SUB, (int) player.pos[1] / CLIP_SUB, (int) player.pos[2] / CLIP_SUB);
		int32_t z0 = player.pos[2], zmax = z0;
		c.jump = 1;
		for (int i = 0; i < 90; i++) {
			PM_TICK();
			c.jump = 0;
			if (player.pos[2] > zmax)
				zmax = player.pos[2];
		}
		printf("pmove jump apex %d units, now %d %d %d (ground %d)\n", (int) (zmax - z0) / CLIP_SUB,
		       (int) player.pos[0] / CLIP_SUB, (int) player.pos[1] / CLIP_SUB,
		       (int) player.pos[2] / CLIP_SUB, player.onground);
		for (int i = 0; i < 300; i++)
			PM_TICK();
		printf("pmove wall: stopped at %d %d %d vel %d %d\n", (int) player.pos[0] / CLIP_SUB,
		       (int) player.pos[1] / CLIP_SUB, (int) player.pos[2] / CLIP_SUB,
		       (int) player.vel[0], (int) player.vel[1]);
		player.yaw += 2048;
		for (int i = 0; i < 300; i++)
			PM_TICK();
		printf("pmove back: at %d %d %d (ground %d)\n", (int) player.pos[0] / CLIP_SUB,
		       (int) player.pos[1] / CLIP_SUB, (int) player.pos[2] / CLIP_SUB, player.onground);
		printf("pmove cost: %u ticks, avg %u max %u cycles/tick\n", (unsigned) ticks,
		       (unsigned) (cyc_sum / ticks), (unsigned) cyc_max);
#undef PM_TICK
	}
	bench_done();
	for (;;)
		run_frame(0, 0);
}

#endif

/* ---------------------------------------------------------------- main -- */

#if Q1PSX_RELEASE && !defined(Q1PSX_BENCH)
/* the release menu's hooks: "map start" with a new game's parms */
static void release_start_map(void)
{
	int m = world_find_map("START");
	map_index = m >= 0 ? m : 0;
	game_new_parms();
	if (load_level(map_index))
		game_spawn_level();
}

static void release_new_game(void)
{
	release_start_map();
	release_in_game = 1;
}

static int release_started(void) { return release_in_game; }

static int release_no_world(void) { return !world_ok; }

/* The release's boot: Quake starts its demo loop under the menu (quake.rc
   "startdemos demo1 demo2 demo3"); the port has no demos and loads no map
   until New Game, so its screen is Quake's with neither (con_forcedup): the
   console's background, the menu over it (M_Draw), and the music of the
   first demo, CD track 2, which demo1.dem forces (the menu's bank,
   MAPS/MENU.SND: its clicks and that track; tools/qcook/menu.c) */
static void release_title(void)
{
	snd_load_bank("MENU");
	ui_conback_load();
	music_play(snd_cd_track());
}
#endif

/* The stack's high-water mark: the stack's room (mem.h MEM_STACK_BYTES) is
   painted first thing at boot (mem_init, with the level arena zeroed); each
   map's load prints how much of it was never reached. */
static uint32_t stack_untouched(void)
{
	return mem_stack_untouched();
}

int main(void)
{
	mem_init();
	/* the BIOS console (printf) goes out on SIO1 at its boot-time baud rate;
	   an emulator that times the serial port (PCSX-Redux since 2026-10)
	   would cap the bench's display-list dumps at a few KB/s */
	SIO_MODE(1) = 0x004d;                  /* 8 bits, 1 stop bit, x1 */
	SIO_BAUD(1) = 8;
	vid_init();
	pad_init();
	prof_init();
	cd_init();
	ovl_load("");                          /* the boot's inits below and the loads' code (§90.7) */
	snd_init();
	music_init();
	r_init();
	vid_set_bg(R_SKY_RGB);    /* flat "sky" until R3.4 */

	printf("q1psx\n");

	world_scan_maps();
	for (int a = 0; a < QL_AXES; a++)
		settings_level_mask[a] = world_level_mask(a);
	settings_defaults();
	apply_settings();
	gdata_load(settings.level[QL_MODELS]);
	stream_init(settings.level[QL_MODELS]);
	dl_init();
	game_init();
	printf("mem: game arena %u of %u B\n", (unsigned) mem_game_used(), (unsigned) MEM_GAME_BYTES);
#ifdef Q1PSX_BENCH
	game_skill = Q1PSX_BENCH_SKILL;              /* (SKILL=: the entities spawned on it) */
	if (!Q1PSX_BENCH_MAPS && Q1PSX_BENCH_MAP[0] && world_find_map(Q1PSX_BENCH_MAP) >= 0)
		map_index = world_find_map(Q1PSX_BENCH_MAP);  /* MAP=: the physics diffs there too */
	if (load_level(map_index))
		player_spawn();
	run_bench();
#elif Q1PSX_RELEASE
	static const menu_hooks_t hooks = { release_new_game, release_started, release_no_world };
	menu_init(&hooks);
	game_active = 1;
	release_title();                           /* (no map before New Game) */
	menu_open();
	for (;;)
		run_frame(1, 0);
#else
	mapmenu_init();
	in_menu = 1;
	game_active = 1;
	for (;;) {
		if (in_menu)
			mapmenu_frame();
		else
			run_frame(1, 0);
	}
#endif
}
