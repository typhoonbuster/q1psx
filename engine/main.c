/*
 * q1psx — boot + main loop.
 *
 * Normal build: a map menu (every map on the disc, the fidelity level, the
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
#include <psxgpu.h>
#include <hwregs_c.h>

#include "bench.h"
#include "cd.h"
#include "ents.h"
#include "game.h"
#include "gdata.h"
#include "hud.h"
#include "level.h"
#include "anim.h"
#include "menu.h"
#include "dlight.h"
#include "mem.h"
#include "monsters.h"
#include "music.h"
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
#include "vid.h"
#include "world.h"

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
static int      fidelity   = 2; /* the level actually loaded (a map too
                                   big for the arena falls back lower)  */
static int      fid_loaded = -1;/* the settings.fidelity of the last load      */

#ifdef Q1PSX_BENCH
#ifndef Q1PSX_BENCH_T
#define Q1PSX_BENCH_T    8    /* warp threshold the bench renders with, px */
#endif
#ifndef Q1PSX_BENCH_TEX4
#define Q1PSX_BENCH_TEX4 0    /* 1: the 4-bit textures (TEX4=1 scripts/bench.sh) */
#endif
#ifndef Q1PSX_BENCH_DRAWDIST
#define Q1PSX_BENCH_DRAWDIST 0   /* the draw distance, world units (DRAWDIST=1024 scripts/bench.sh) */
#endif
#ifndef Q1PSX_BENCH_FLAT
#define Q1PSX_BENCH_FLAT 0       /* the flat shading, R_FLAT_* (FLAT=1 scripts/bench.sh) */
#endif
#ifndef Q1PSX_BENCH_VIEWMODEL
#define Q1PSX_BENCH_VIEWMODEL 1  /* the gun: 0 none, 1 3D, 2 2D (VIEWMODEL=2 scripts/bench.sh) */
#endif
#endif

/* The settings (engine/settings.c); the bench renders with its own. */
static void apply_settings(void)
{
#ifdef Q1PSX_BENCH
	r_set_warp(R_WARP_FIXED, Q1PSX_BENCH_T, 0);
	vid_set_dither(1);
	settings.tex4 = Q1PSX_BENCH_TEX4;
	r_set_options(Q1PSX_BENCH_DRAWDIST, Q1PSX_BENCH_FLAT);
	game_draw_viewmodel = Q1PSX_BENCH_VIEWMODEL;
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
	int m = (int) q_isqrt64((uint64_t) (*x * *x + *y * *y));
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

/* The simulation is tick-based (Quake). Every vblank's poll (pad_frame) turns
   the view (Quake's client turns each frame: CL_AdjustAngles); a tick runs
   every 60 / game_tick_hz polls on average (an accumulator: at 32 Hz, 2, 2,
   ... 1 polls), over the polls since the last one (game_dt 60ths of a
   second), with the last poll's held buttons and every press since. At
   60 Hz, a tick per poll. Below, the camera is drawn between the last two
   ticks' views (CL_LerpPoint), late by the longest tick less a poll; a jump
   of more than 64 units (a teleporter, a new level) is not lerped. */
static void player_update(const pad_t *polls, int n)
{
	static int        hz = 60, acc, since, span = 1;
	static int32_t    prev[3], last[3];
	static game_cmd_t presses = { .weapon_group = -1 };
	uint32_t t0 = prof_now();
	if (hz != game_tick_hz || game_view_cut) {  /* a new rate, a new spawn: afresh */
		game_view_cut = 0;
		hz = game_tick_hz;
		acc = since = 0;
		span = 1;
		memset(&presses, 0, sizeof presses);
		presses.weapon_group = -1;
		memset(game_view_lerp, 0, sizeof game_view_lerp);
		game_view_pos(last);
		memcpy(prev, last, sizeof prev);
	}
	if (hz == 60) {                             /* a tick per poll, no lerp */
		for (int i = 0; i < n; i++) {
			game_cmd_t cmd;
			player_cmd(&cmd, &polls[i]);
			game_tick(&cmd);
		}
		game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
		prof_add(PROF_SIM, t0, prof_now());
		return;
	}
	for (int i = 0; i < n; i++) {
		game_cmd_t cmd;
		player_cmd(&cmd, &polls[i]);
		presses.weapon_next |= cmd.weapon_next;   /* a press between two ticks */
		presses.weapon_prev |= cmd.weapon_prev;   /* counts (Quake's key "impulse" */
		presses.centre      |= cmd.centre;        /* bit, CL_SendMove's state & 3): */
		presses.fire        |= cmd.fire;          /* fire and jump too, held or */
		presses.jump        |= cmd.jump;          /* tapped */
		if (cmd.weapon_group >= 0)
			presses.weapon_group = cmd.weapon_group;
		since++;
		if ((acc += hz) < 60)
			continue;
		acc -= 60;
		cmd.weapon_next  = presses.weapon_next;
		cmd.weapon_prev  = presses.weapon_prev;
		cmd.centre       = presses.centre;
		cmd.fire         = presses.fire;
		cmd.jump         = presses.jump;
		cmd.weapon_group = presses.weapon_group;
		memset(&presses, 0, sizeof presses);
		presses.weapon_group = -1;
		game_dt = since;
		game_tick(&cmd);
		game_dt = 1;
		span  = since;
		since = 0;
		memcpy(prev, last, sizeof prev);
		game_view_pos(last);
	}
	game_view_angles(&cam.yaw, &cam.pitch, &cam.roll);
	memcpy(cam.pos, last, sizeof cam.pos);
	memset(game_view_lerp, 0, sizeof game_view_lerp);
	{
		static const uint16_t inv[7] = { 0, 65535, 32768, 21846, 16384, 13108, 10923 };   /* 65536 / span */
		int lag = (60 + hz - 1) / hz - 1;       /* polls: the longest tick less one */
		int num = since - lag + span;           /* of span: where between the two */
		if (num < 0)    num = 0;
		if (num > span) num = span;
		int32_t far = 0;
		for (int k = 0; k < 3; k++) {
			int32_t d = last[k] - prev[k];
			far |= d > 64 * CLIP_SUB || d < -64 * CLIP_SUB;
		}
		if (!far && num < span) {
			int32_t f = num * inv[span];            /* 0.16: where between the two */
			for (int k = 0; k < 3; k++) {
				game_view_lerp[k] = -(((last[k] - prev[k]) * (65536 - f)) >> 16);
				cam.pos[k] += game_view_lerp[k];
			}
		}
	}
	prof_add(PROF_SIM, t0, prof_now());
}

/* ------------------------------------------------------- map / fidelity -- */

/* ------------------------------------------------------- loading bar -- */

static int load_map, load_fid;   /* what the bar is loading */

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
static void loading_screen(uint32_t done, uint32_t total)
{
	enum { BX = 40, BY = 116, BW = 240, BH = 10 };
	char line[48];
	int  fill = total ? (int) (done * BW / total) : 0;   /* < 2 MB: no overflow */

	snprintf(line, sizeof line, "loading %s  (%s%s)", world_map_name(load_map),
	         fidelity_name[load_fid], settings.tex4 ? ", 4-bit" : "");
	vid_text(BX, BY - 16, line);
	snprintf(line, sizeof line, "%3d %%", total ? (int) (done * 100 / total) : 0);
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

/* Load `map` at fidelity `fid`, or the highest lower level that fits the
   level arena. Sets world_ok / fidelity and binds the renderer. */
static int load_level(int map, int fid)
{
	world_ok = 0;
	load_map = map;
	stream_abort();                             /* (the arena is reset, the drive taken) */
	music_stop();                               /* one drive: the reads need it */
	snd_load_bank(world_map_name(map));         /* (through the level arena, first) */
	for (int f = fid; f >= 0 && !world_ok; f--) {
		load_fid = f;
		world_ok = world_load(map, f, settings.tex4, LOAD_PROGRESS);
		if (world_ok)
			fidelity = f;
	}
	fid_loaded = fid;
	if (world_ok) {
		if (fidelity != fid)
			printf("level: %s does not fit at %s, loaded at %s\n", world_map_name(map),
			       fidelity_name[fid], fidelity_name[fidelity]);
		r_bind_world();
		ents_load();                            /* the map's entities */
		stream_map_start(STREAM_KEEP);          /* the arena's rest: the streamed assets */
		monsters_map_start();
		ents_spawn(1);
		stream_preload();                       /* what the spawn's surroundings want */
		anim_reset();
		hud_invalidate();                       /* (the loading screen drew over the bars) */
		snd_map_start();
		music_play(snd_cd_track());
	}
	return world_ok;
}

/* L2: reload the map at the next fidelity level, keeping the camera where
   it is; R2: next map, at its spawn. */
static void switch_level(int next_map)
{
	if (next_map)
		map_index = (map_index + 1) % (world_num_maps() ? world_num_maps() : 1);
	else
		settings.fidelity = settings_fid_step(settings.fidelity, 1);
	if (load_level(map_index, settings.fidelity) && next_map)
		game_spawn();
}

/* --------------------------------------------------------------- frame -- */

static void frame_work(void)
{
	if (!world_ok)
		return;
	r_set_view(cam.pos, cam.yaw, cam.pitch, cam.roll);
	anim_frame(game_tick_count);            /* the animated textures' frames */
	uint32_t t0 = prof_now();
	rl_begin();                             /* the render list: the game's
	                                           projectiles, effects and view
	                                           model, the map's brush models;
	                                           their matrices come from this
	                                           frame's view */
	if (game_active)
		game_render();
#if !defined(Q1PSX_BENCH_NOENTS) || !Q1PSX_BENCH_NOENTS
	ents_render();
#endif
	prof_add(PROF_ENTS, t0, prof_now());    /* (the dlights' share counted apart) */
	r_frame();
	rl_end();
	if (game_active)
		game_render_done();
}

static int overlay_on = !Q1PSX_RELEASE;   /* START toggles (the debug build) */
static int menu_shown = Q1PSX_RELEASE;     /* Quake's menu drawn (the release build; the bench's check) */
static int release_in_game;                /* the release build: a game begun (New Game, or
                                              the menu left on the first map) */
#if Q1PSX_RELEASE && !defined(Q1PSX_BENCH)
static void release_start_map(void);
#endif

static void draw_overlay(void)
{
	uint32_t t0 = prof_now();
	char line[384], sticks[40] = "";

	if (pad.analog)                    /* raw, before the dead zone */
		snprintf(sticks, sizeof sticks, "\nsticks L %4d %4d  R %4d %4d", pad.lx, pad.ly, pad.rx, pad.ry);

	int n = snprintf(line, sizeof line,
	         "q1psx  %s %s  ~%u fps\n"
	         "frame %6u cyc  gpu wait %u  arena %u KB\n",
	         world_map_name(map_index), world_ok ? fidelity_name[fidelity] : "no world",
	         vid_stats.vbl ? 60 / vid_stats.vbl : 60,
	         prof_stage[PROF_FRAME], vid_stats.sync_cycles, mem_level_used() / 1024);
#if Q1PSX_PROFILE   /* the renderer's counters / stage timers (release: none) */
	n += snprintf(line + n, sizeof line - n,
	         "walk %u  mark %u  faces %u\n"
	         "nodes %u recs %u verts %u\n"
	         "polys %u  near %u  ",
	         prof_stage[PROF_R_WALK], prof_stage[PROF_R_SETUP], prof_stage[PROF_R_BLOCK],
	         prof_count[PROF_N_NODES], prof_count[PROF_N_BLOCKS], prof_count[PROF_N_VERTS],
	         prof_count[PROF_N_POLYS], prof_count[PROF_N_SLOW]);
#endif
	snprintf(line + n, sizeof line - n,
	         "%u B%s\n"
	         "warp %s %dpx  T %d  ~%u tris%s%s\n"
	         "pos %d %d %d  sim %u  items %d\n"
	         "pad %s type %d btn %04x%s",
	         vid_stats.pkt_bytes, vid_stats.dropped ? " FULL" : "",
	         warp_mode_name[r_warp.mode], r_warp.t, r_warp.t_eff, (unsigned) r_warp.tris,
	         world.tex4 ? "  4-bit" : "", settings.dither ? "" : "  no dither",
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
	prof_frame_reset();
	uint32_t t0 = prof_now();
	if (world_ok && game_active)
		stream_frame();                     /* (between frames: the pools may move) */
	if (interactive) {
		PLAY_STEP("pad");
		pad_t ticks[PAD_MAX_TICKS];
		int nticks = pad_frame(ticks, PAD_MAX_TICKS);
#if Q1PSX_RELEASE
		if (menu_active()) {                  /* the world paused under the menu */
			menu_key(&pad);
			nticks = 0;
			if (!menu_active() && !release_in_game) {   /* left on the first map: playing it, */
				if (settings.fidelity != fid_loaded || settings.tex4 != world.tex4)
					release_start_map();      /* at the fidelity just chosen */
				release_in_game = 1;
			}
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
				if (load_level(m, settings.fidelity))
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
		game_view_tint();                   /* the contents' colour over the view */
		PLAY_STEP("hud");
		hud_draw();
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

#define MENU_ROWS 16       /* two columns of 16: Quake's 32 maps */

static int         complex_map = -1;      /* the "(complex)" one          */
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
	if (!strcmp(world_map_name(m), "START"))
		return " (default)";
	return m == complex_map ? " (complex)" : "";
}

/* The options under the map list: L2 / R2 pick one, L1 / R1 change it. */
enum { OPT_FID, OPT_TEX, OPT_DITHER, OPT_WARP, OPT_T, OPT_BUDGET, OPT_HUD, OPT_DLIGHTS, OPT_WEAPON, OPT_FPS,
       OPT_TICK, OPT_DRAWDIST, OPT_FLAT, OPT_COUNT };
static int opt_sel;

static void opt_step(int d)
{
	switch (opt_sel) {
	case OPT_FID:    settings.fidelity = settings_fid_step(settings.fidelity, d); break;
	case OPT_TEX:    settings.tex4 ^= 1; break;
	case OPT_DITHER: settings.dither ^= 1; break;
	case OPT_WARP:   settings.warp = (settings.warp + R_WARP_MODES + d) % R_WARP_MODES; break;
	case OPT_T:      settings.t = (settings.t + num_warp_t + d) % num_warp_t; break;
	case OPT_BUDGET: settings.budget = (settings.budget + num_warp_budget + d) % num_warp_budget; break;
	case OPT_HUD:    settings.hud = (settings.hud + HUD_STYLES + d) % HUD_STYLES; break;
	case OPT_DLIGHTS: settings.dlights ^= 1; break;
	case OPT_WEAPON: settings.viewmodel = (settings.viewmodel + 3 + d) % 3; break;
	case OPT_FPS:    settings.fps_cap = (settings.fps_cap + num_fps_caps + d) % num_fps_caps; break;
	case OPT_TICK:   settings.tick = (settings.tick + num_tick_rates + d) % num_tick_rates; break;
	case OPT_DRAWDIST: settings.drawdist = (settings.drawdist + num_draw_dists + d) % num_draw_dists; break;
	default:         settings.flat = (settings.flat + R_FLAT_MODES + d) % R_FLAT_MODES; break;
	}
	apply_settings();   /* warp, dithering, the HUD at once; textures, fidelity on load */
}

static void opt_draw(void)
{
	static const char *const label[OPT_COUNT] = {
		"fidelity", "textures", "dither", "warp", "threshold", "budget", "hud", "dlights", "weapon",
		"fps cap", "tick rate", "distance", "flat",
	};
	char line[48], val[16];
	for (int o = 0; o < OPT_COUNT; o++) {
		switch (o) {
		case OPT_FID:    snprintf(val, sizeof val, "%s", fidelity_name[settings.fidelity]); break;
		case OPT_TEX:    snprintf(val, sizeof val, "%s", settings.tex4 ? "4 bit" : "8 bit"); break;
		case OPT_DITHER: snprintf(val, sizeof val, "%s", settings.dither ? "on" : "off"); break;
		case OPT_WARP:   snprintf(val, sizeof val, "%s", warp_mode_name[settings.warp]); break;
		case OPT_T:      snprintf(val, sizeof val, "%d px", warp_t_px[settings.t]); break;
		case OPT_BUDGET: snprintf(val, sizeof val, "%d tris", warp_budgets[settings.budget]); break;
		case OPT_HUD:    snprintf(val, sizeof val, "%s", hud_style_name[settings.hud]); break;
		case OPT_DLIGHTS: snprintf(val, sizeof val, "%s", settings.dlights ? "on" : "off"); break;
		case OPT_WEAPON: snprintf(val, sizeof val, "%s", viewmodel_name[settings.viewmodel]); break;
		case OPT_FPS:    snprintf(val, sizeof val, "%d", fps_caps[settings.fps_cap]); break;
		case OPT_TICK:   snprintf(val, sizeof val, "%d Hz", tick_rates[settings.tick]); break;
		case OPT_DRAWDIST:
			if (draw_dists[settings.drawdist])
				snprintf(val, sizeof val, "%d", draw_dists[settings.drawdist]);
			else
				snprintf(val, sizeof val, "off");
			break;
		default:         snprintf(val, sizeof val, "%s", flat_name[settings.flat]); break;
		}
		int x = 16 + (o % 2) * 152, y = 168 + (o / 2) * 10;
		snprintf(line, sizeof line, "%-9s< %s >", label[o], val);
		vid_text(x, y, line);
		if (o == opt_sel)
			ui_rect(x - 4, y - 1, 148, 10, 60, 70, 120);
	}
}

/* One frame of the menu: d-pad picks a map, L2 / R2 an option and L1 / R1
   its value, Cross / START loads the map, Circle goes back to the loaded
   one (reloading it if the textures or the fidelity changed). */
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
		opt_sel = (opt_sel + OPT_COUNT - 1) % OPT_COUNT;
	if (pad.pressed & PAD_R2)
		opt_sel = (opt_sel + 1) % OPT_COUNT;
	if (pad.pressed & PAD_L1)
		opt_step(-1);
	if (pad.pressed & PAD_R1)
		opt_step(1);
	if ((pad.pressed & PAD_CIRCLE) && world_ok) {
		if (world.tex4 != settings.tex4 || fid_loaded != settings.fidelity)
			load_level(map_index, settings.fidelity);   /* same map, same place */
		if (world_ok) {
			in_menu = 0;                         /* back to the loaded map */
			return;
		}
		menu_msg = "could not reload that map";
	}
	if (n && (pad.pressed & (PAD_CROSS | PAD_START))) {
		map_index = menu_sel;
		if (load_level(map_index, settings.fidelity)) {
			game_spawn();
			in_menu  = 0;
			menu_msg = "";
			return;
		}
		menu_msg = "could not load that map";
	}

	char line[48];
	snprintf(line, sizeof line, "Q1PSX - choose a map (%d on the disc)", n);
	vid_text(16, 6, line);
	for (int m = 0; m < n; m++) {
		int x = 24 + (m / MENU_ROWS) * 148, y = 16 + (m % MENU_ROWS) * 10;
		snprintf(line, sizeof line, "%s%s", world_map_name(m), map_tag(m));
		vid_text(x, y, line);
		if (m == menu_sel)
			ui_rect(x - 4, y - 1, 140, 10, 60, 70, 120);
	}
	if (!n)
		vid_text(24, 20, "no cooked map on the disc");
	opt_draw();
	vid_text(16, 218, "L2/R2 option  L1/R1 value  X load  O back");
	vid_text(16, 228, menu_msg[0] ? menu_msg : "in game: SELECT + START = this menu");
	vid_flip();
}

#endif /* !Q1PSX_BENCH */

/* --------------------------------------------------------------- bench -- */

#ifdef Q1PSX_BENCH

#ifndef Q1PSX_BENCH_FID
#define Q1PSX_BENCH_FID 2     /* fidelity level the bench loads (0 low .. 3 ultra) */
#endif

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
q_fixed clip_trace_c(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr);
q_fixed clip_trace_ent_c(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr);
int     clip_contents_c(int hull, const int32_t p[3]);
uint32_t clip_box_node0_c(const int32_t mins[3], const int32_t maxs[3]);
int     clip_contents_node0_c(uint32_t node, const int32_t p[3]);
extern clip_mover_t clip_mover_c;
void    pmove_step_c(pmove_t *pm, const pmove_cmd_t *cmd);

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
	       "cycles/trace asm %u c %u\n", N, hits, solid, bad, N, cbad,
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
	printf("clipdiff lines of sight %d (%d clear): asm/c mismatches %d, verdicts %d; cycles whole %u sight %u\n",
	       N, clear, sbad, vbad, (unsigned) (cyc_full / N), (unsigned) (cyc_sight / N));

	printf("clipdiff box nodes %d: %d below the root, mismatches %d, walks from them %d; "
	       "4 corners: root %u cycles, node %u (its walk too)\n", N / 4, below, nbad, wbad,
	       (unsigned) (cyc_root / (N / 4)), (unsigned) (cyc_node / (N / 4)));
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
	printf("pmdiff ticks %d mismatches %d; cycles/tick asm %u c %u; runs started in water %d\n", TICKS,
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
	printf("pmdiff without traces: cycles/tick asm %u c %u\n", (unsigned) (cyc_asm / 1000),
	       (unsigned) (cyc_c / 1000));
}

/* Every map on the disc: load it (falling back to a lower fidelity when it
   does not fit the arena), then its four spawn views, briefly, each followed
   by its display list's hash (scripts/dlcheck.sh compares them). */
#ifndef Q1PSX_BENCH_MAP
#define Q1PSX_BENCH_MAP ""    /* a map name: the maps bench runs that one only (MAP=E1M1) */
#endif
#ifndef Q1PSX_BENCH_VIEW_SET
#define Q1PSX_BENCH_VIEW_SET 0   /* 1: one more view of that map, Q1PSX_BENCH_VX .. VPITCH (VIEW=x,y,z,yaw,pitch) */
#define Q1PSX_BENCH_VX 0
#define Q1PSX_BENCH_VY 0
#define Q1PSX_BENCH_VZ 0
#define Q1PSX_BENCH_VYAW 0
#define Q1PSX_BENCH_VPITCH 0
#endif
#ifndef Q1PSX_BENCH_NOENTS
#define Q1PSX_BENCH_NOENTS 0  /* 1: the map's entities are not drawn (NOENTS=1: an A / B) */
#endif

static void bench_maps(void)
{
	static char names[4][16];
	stream_log = 1;
	for (int m = 0; m < world_num_maps(); m++) {
		if (Q1PSX_BENCH_MAP[0] && strcmp(world_map_name(m), Q1PSX_BENCH_MAP))
			continue;
		if (!load_level(m, Q1PSX_BENCH_FID)) {
			printf("bench map %s FAILED to load\n", world_map_name(m));
			continue;
		}
		printf("bench map %s fid %s arena %u KB\n", world_map_name(m), fidelity_name[fidelity],
		       mem_level_used() / 1024);
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
		qw_view extra;                          /* VIEW=x,y,z,yaw,pitch: one more */
		int have_extra = Q1PSX_BENCH_VIEW_SET;
		extra.pos[0] = Q1PSX_BENCH_VX; extra.pos[1] = Q1PSX_BENCH_VY; extra.pos[2] = Q1PSX_BENCH_VZ;
		extra.yaw = Q1PSX_BENCH_VYAW; extra.pitch = Q1PSX_BENCH_VPITCH;
		uint32_t nviews = Q1PSX_BENCH_MAP[0] ? world.num_views : 4u;
		if (nviews > world.num_views) nviews = world.num_views;
		for (uint32_t v = 0; v < nviews + (uint32_t) have_extra; v++) {
			const qw_view *vw = v < nviews ? &world.views[v] : &extra;   /* (one map: all of its views) */
			cam.pos[0] = vw->pos[0] * CLIP_SUB;
			cam.pos[1] = vw->pos[1] * CLIP_SUB;
			cam.pos[2] = vw->pos[2] * CLIP_SUB;
			cam.yaw    = vw->yaw * ANG_TURN / 360;
			cam.pitch  = vw->pitch * ANG_TURN / 360;
			cam.roll   = 0;
			snprintf(names[v & 3], sizeof names[0], "%s.v%u", world_map_name(m), v);
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
			prof_sample_print(names[v & 3]);
			printf("D %s\n", names[v & 3]);
			ents_debug = 1;                     /* (the models drawn, by name) */
			run_frame(0, Q1PSX_BENCH_MAP[0] ? 1 : 2);   /* one map: its frames in full */
			ents_debug = 0;
			run_frame(0, 0);                    /* (the cycles of a frame without the prints) */
			printf("ents view %s: %d models drawn of %d, %d in the PVS, %d leaves in front, items %d\n",
			       names[v & 3], ents_stat[0], ents_stat[3], ents_stat[1], ents_stat[2], rl_num_items());
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

static void ents_dump(const char *name)
{
	game_view(cam.pos, &cam.yaw, &cam.pitch, &cam.roll);
	for (int i = 0; i < 2; i++)
		run_frame(0, 0);
	printf("mon dump %s: %d streamed models drawn\n", name, monsters_drawn);
	printf("D %s\n", name);
	run_frame(0, BENCH_DUMP);
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
	int32_t c[3];
	ent_centre(e, c);
	for (int a = 0; a < 4; a++) {
		int k = a >> 1, s = a & 1 ? -1 : 1;
		int32_t half = (e->absmax[k] - e->absmin[k]) / 2 + 24 * CLIP_SUB;
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
			snprintf(name, sizeof name, "i%s_%d", map, e->class);
			ents_dump(name);
		}
		if (!e->touch)
			continue;
		ents_place(e->absmin, e->absmax, 0);
		ents_ticks(3, cmd);
		printf("item taken %d: health %d armor %d/%d items %08x ammo %d %d %d %d weapon %d\n",
		       !e->inuse || (e->item_model < 0 && e->masset < 0), pl.health, pl.armor, pl.armortype, (unsigned) pl.items,
		       pl.ammo[0], pl.ammo[1], pl.ammo[2], pl.ammo[3], pl.weapon);
	}
	/* a quad: the tint, the face; its warning at 27 s, gone at 30 */
	pl.items |= IT_QUAD;
	game_powerup(IT_QUAD);
	ents_ticks(2, cmd);
	snprintf(name, sizeof name, "i%s_quad", map);
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
	snprintf(name, sizeof name, "i%s_hurt", map);
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
		if (m < 0 || !load_level(m, Q1PSX_BENCH_FID))
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
			int32_t c[3];
			if (!ents_free_spot(d, f->absmin, f->absmax))
				ents_place(f->absmin, f->absmax, 0);
			ent_centre(d, c);
			printf("door *%d at %d %d %d, field %d..%d, player %d %d %d\n", d->model, (int) c[0] / 8,
			       (int) c[1] / 8, (int) c[2] / 8, (int) f->absmin[0] / 8, (int) f->absmax[0] / 8,
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
			memcpy(mn, p->absmin, sizeof mn);
			memcpy(mx, p->absmax, sizeof mx);
			mn[2] = mx[2] = p->absmax[2] + 25 * CLIP_SUB;
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
			ents_place(tp->absmin, tp->absmax, 0);
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
			ents_place(se->absmin, se->absmax, 0);
			ents_ticks(3, &cmd);
			printf("secrets %d of %d\n", ent_found_secrets, ent_total_secrets);
			ents_dump(mi ? "estart_secret" : "ee1m1_secret");
		}

		/* the exit: the intermission, then the next map */
		edict_t *cl = ents_first(QE_TRIGGER_CHANGELEVEL, 0);
		if (cl) {
			ents_place(cl->absmin, cl->absmax, 0);
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
					if (load_level(nm, Q1PSX_BENCH_FID)) {
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

/* the release build's menu (engine/menu.c) driven by pressed keys, a frame
   of each page dumped: the main menu, single player, the new-game
   question, the options, the graphics page in each warp mode (the greyed
   entries), back out */
static int  bench_yes(void) { return 1; }
static int  bench_fid(void) { return Q1PSX_BENCH_FID; }
static int  bench_tex(void) { return Q1PSX_BENCH_TEX4; }
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
	static const menu_hooks_t hooks = { 0, bench_yes, bench_fid, bench_tex };
	menu_init(&hooks);
	menu_open();
	menu_shown = 1;
	bench_menu_key(0, "mmain");
	bench_menu_key(PAD_CROSS, "msingle");             /* Single Player */
	bench_menu_key(PAD_CROSS, "mnewgame");            /* New Game: "Are you sure" */
	bench_menu_key(PAD_CIRCLE, 0);                    /* no */
	bench_menu_key(PAD_TRIANGLE, 0);                  /* back to the main menu */
	bench_menu_key(PAD_DOWN, 0);                      /* (Multiplayer greyed: to Options) */
	bench_menu_key(PAD_CROSS, "moptions");
	bench_menu_key(PAD_UP, 0);                        /* (the greyed ones: to Graphics) */
	bench_menu_key(PAD_CROSS, "mgraphics");
	bench_menu_key(PAD_DOWN, 0);
	bench_menu_key(PAD_DOWN, 0);
	bench_menu_key(PAD_DOWN, 0);                      /* the warp control */
	bench_menu_key(PAD_RIGHT, "mgraphics_dyn");       /* dynamic: the threshold a floor */
	bench_menu_key(PAD_RIGHT, "mgraphics_budget");    /* budget: its budget live */
	bench_menu_key(PAD_RIGHT, 0);                     /* back to fixed */
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
		snprintf(sname[k], sizeof sname[k], "tick_%s", what[k]);
		prof_sample_print(sname[k]);
	}
	game_active = 0;
}

/* The tick rates (settings.tick) through the game's own scheduler
   (player_update) on the same polls: a second standing, two walking
   forward, a jump held 0.2 s, a second falling back. The distance walked, the jump's
   apex, the ticks run and the simulation's cycles (PROF_SIM) by rate. */
static void bench_tick_rates(void)
{
	for (int r = num_tick_rates - 1; r >= 0; r--) {
		game_spawn();
		game_active = 1;
		game_tick_hz = tick_rates[r];
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
		snprintf(rname[r], sizeof rname[r], "rate%d", tick_rates[r]);
		prof_sample_print(rname[r]);
	}
	game_tick_hz = 60;
	game_active = 0;
}

/* Frames in motion: the player walks forward through start's hall,
   turning left then right, each frame rendered after its vblanks' polls
   (the last frame's vblanks: as the game loop takes them), at 60 and at
   32 ticks a second. The leaf changes (mark_pvs), the ticks, the camera
   between ticks: frames as in play. */
static void bench_walk(void)
{
	static const uint8_t rates[2] = { 60, 32 };
	for (int r = 0; r < 2; r++) {
		game_spawn();
		game_active = 1;
		game_tick_hz = rates[r];
		static char name[2][12];
		snprintf(name[r], sizeof name[r], "walk%d", rates[r]);
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
	game_tick_hz = 60;
	game_active = 0;
}

static void bench_game(void)
{
	snd_log = 1;                                      /* every sound start, for the diff */
	static const char *const wname[W_COUNT] = { "axe", "shotgun", "sshotgun", "nailgun",
	                                            "snailgun", "glauncher", "rlauncher", "lightning" };
	static char names[W_COUNT + 2][12];
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
		snprintf(names[w], sizeof names[w], w < W_COUNT ? "g%s" : w == W_COUNT ? "ghud1" : "ghud2",
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
				snprintf(wn[(t - 18) / 6], sizeof wn[0], "gwalk%d", (t - 18) / 6);
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
	       "flags %x | player health %d armor %d at %d %d %d | stream %d/%d in, %u reads, %u halved\n", what,
	       (int) ent_time, e->mstate, e->frame, (int) e->origin[0] / CLIP_SUB, (int) e->origin[1] / CLIP_SUB,
	       (int) e->origin[2] / CLIP_SUB, e->ang[1] * 360 / 65536, e->ideal_yaw, e->health,
	       e->enemy ? (int) (e->enemy - edicts) : -1, e->attack_state, (unsigned) e->flags, pl.health,
	       pl.armor, (int) pl.pm.pos[0] / CLIP_SUB, (int) pl.pm.pos[1] / CLIP_SUB, (int) pl.pm.pos[2] / CLIP_SUB,
	       stream_stats.resident, stream_stats.wanted, (unsigned) stream_stats.reads,
	       (unsigned) stream_stats.halved);
}

/* the stream manager's frames between two ticks (the game draws about
   two a tick at 32 Hz: its passes, reads and uploads run on them) */
static void bench_stream_frames(void)
{
	stream_frame();
	stream_frame();
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

static void bench_monsters(void)
{
	const char *mapname = Q1PSX_BENCH_MAP[0] ? Q1PSX_BENCH_MAP : "E1M1";
	int m = world_find_map(mapname);
	if (m < 0 || !load_level(m, Q1PSX_BENCH_FID)) {
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
	for (int k = 0; k < (Q1PSX_BENCH_MONSTERS == 2 ? 0 : MK_COUNT); k++) {   /* (2: the crowd only) */
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
					if (clip_contents(0, feet) != QW_CONTENTS_EMPTY)
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
			e->use(e);
		}
		char name[32];
		snprintf(name, sizeof name, "m%s_%d_a", mapname, k);
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
				snprintf(name, sizeof name, "m%s_%d_b", mapname, k);
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
		mon_log("killed", e);
		for (int t = 0; t < 120; t++) {           /* its death, its backpack */
			game_tick(&cmd);
			bench_stream_frames();
		}
		mon_log("dead", e);
		snprintf(name, sizeof name, "m%s_%d_c", mapname, k);
		ents_look_at(e->origin);
		{                                         /* down at the body */
			int32_t dz = pl.pm.pos[2] + pl.viewheight - e->origin[2] + 16 * CLIP_SUB;
			int32_t dx = e->origin[0] - pl.pm.pos[0], dy = e->origin[1] - pl.pm.pos[1];
			pl.pm.pitch = r_atan2(dz, (int32_t) q_isqrt64((uint64_t) ((int64_t) dx * dx + (int64_t) dy * dy)));
		}
		ents_dump(name);
		pl.pm.pitch = 0;
	}
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
}

static void run_bench(void)
{
	static char names[16][8];

	printf("bench start\n");
	if (Q1PSX_BENCH_MONSTERS) {
		bench_monsters();
		printf("bench done\n");
		for (;;)
			run_frame(0, 0);
	}
	if (Q1PSX_BENCH_MAPS) {
		bench_maps();
		printf("bench done\n");
		for (;;)
			run_frame(0, 0);
	}
	if (Q1PSX_BENCH_ENTS) {
		bench_ents();
		printf("bench done\n");
		return;
	}
	if (Q1PSX_BENCH_GAME) {
		bench_game();
		printf("bench done\n");
		for (;;)
			run_frame(0, 0);
	}
	if (world_ok) {
		bench_clip_diff();
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
		snprintf(names[v], sizeof names[v], "view%u", v);

		bench_acc_t acc;
		bench_reset(&acc, names[v]);
		for (int i = 0; i < BENCH_WARMUP; i++)
			run_frame(0, 0);
		for (int i = 0; i < BENCH_FRAMES; i++) {
			run_frame(0, 0);
			bench_add(&acc, vid_stats.vbl);
		}
		bench_print(&acc);
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
	printf("bench done\n");
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
	if (load_level(map_index, settings.fidelity))
		game_spawn_level();
}

static void release_new_game(void)
{
	release_start_map();
	release_in_game = 1;
}

static int release_started(void) { return release_in_game; }
static int release_fid(void)     { return fid_loaded; }
static int release_tex4(void)    { return world.tex4; }
#endif

int main(void)
{
	/* the BIOS console (printf) goes out on SIO1 at its boot-time baud rate;
	   an emulator that times the serial port (PCSX-Redux since 2026-10)
	   would cap the bench's display-list dumps at a few KB/s */
	SIO_MODE(1) = 0x004d;                  /* 8 bits, 1 stop bit, x1 */
	SIO_BAUD(1) = 8;
	vid_init();
	pad_init();
	prof_init();
	cd_init();
	snd_init();
	music_init();
	r_init();
	vid_set_bg(R_SKY_RGB);    /* flat "sky" until R3.4 */

	printf("q1psx\n");

	world_scan_maps();
	settings_fid_mask = world_fid_mask();
	settings_defaults();
	apply_settings();
	gdata_load();
	stream_init();
	dl_init();
	game_init();
#ifdef Q1PSX_BENCH
	settings.fidelity = Q1PSX_BENCH_FID;
	if (!Q1PSX_BENCH_MAPS && Q1PSX_BENCH_MAP[0] && world_find_map(Q1PSX_BENCH_MAP) >= 0)
		map_index = world_find_map(Q1PSX_BENCH_MAP);  /* MAP=: the physics diffs there too */
	if (load_level(map_index, settings.fidelity))
		player_spawn();
	run_bench();
#elif Q1PSX_RELEASE
	/* Quake starts its demo loop under the menu; with no demos to play,
	   the first map waits under it, a new game's kit in hand */
	static const menu_hooks_t hooks = { release_new_game, release_started, release_fid, release_tex4 };
	menu_init(&hooks);
	game_active = 1;
	release_start_map();
	release_in_game = 0;
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
