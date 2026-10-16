/*
 * q1psx — boot + main loop.
 *
 * Normal build: a map menu (every map on the disc, the fidelity level, the
 * texture depth, dithering and the affine-warp control), then player
 * physics over the asm-rendered world with the debug overlay; SELECT +
 * START goes back to the menu. Loads show a progress bar.
 * Bench build (Q1PSX_BENCH, CMake preset "bench"): render every cooked bench
 * viewpoint, print one "bench ..." line each (+ a packet dump of one frame for
 * tools/pktview), then "bench done" — see scripts/bench.sh.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "bench.h"
#include "cd.h"
#include "mem.h"
#include "pad.h"
#include "clip.h"
#include "pmove.h"
#include "q_fixed.h"
#include "prof.h"
#include "render.h"
#include "vid.h"
#include "world.h"

/* ---------------------------------------------------------------- player -- */

typedef struct {
	int32_t pos[3];      /* 1/8 world units (the eye) */
	int     yaw, pitch;  /* binary degrees         */
} camera_t;

static camera_t cam;
static pmove_t  player;
static int      in_menu;        /* the map menu is up (normal build)   */
static int      menu_sel;       /* the map highlighted in the menu      */
static int      world_ok;
static int      map_index;      /* into world_map_name(); R2 cycles it */
static int      fid_wanted = 2; /* 0 low .. 3 ultra; L2 cycles it      */
static int      fidelity   = 2; /* the level actually loaded (a map too
                                   big for the arena falls back lower)  */
static int      fid_loaded = -1;/* the fid_wanted of the last load      */

#ifdef Q1PSX_BENCH
#ifndef Q1PSX_BENCH_T
#define Q1PSX_BENCH_T    8    /* warp threshold the bench renders with, px */
#endif
#ifndef Q1PSX_BENCH_TEX4
#define Q1PSX_BENCH_TEX4 0    /* 1: the 4-bit textures (TEX4=1 scripts/bench.sh) */
#endif
#endif

/* Menu settings (the bench has its own): the affine-warp control
   (docs/porting-notes.md §19.4), the texture depth, dithering. */
static const uint8_t  warp_t_px[]    = { 1, 2, 4, 6, 8, 10, 12, 14, 16 };
static const uint16_t warp_budgets[] = { 1000, 1500, 2000, 2500, 3000, 4000 };
static const char *const warp_mode_name[R_WARP_MODES] = { "fixed", "dynamic", "budget" };
#define NUM_WARP_T      (int) (sizeof warp_t_px / sizeof warp_t_px[0])
#define NUM_WARP_BUDGET (int) (sizeof warp_budgets / sizeof warp_budgets[0])

static int set_warp   = R_WARP_FIXED;
static int set_t      = 4;      /* 8 px */
static int set_budget = 2;      /* 2000 triangles */
static int set_tex4   = 0;      /* 8-bit textures */
static int set_dither = 1;

static void apply_settings(void)
{
#ifdef Q1PSX_BENCH
	r_set_warp(R_WARP_FIXED, Q1PSX_BENCH_T, 0);
	vid_set_dither(1);
	set_tex4 = Q1PSX_BENCH_TEX4;
#else
	r_set_warp(set_warp, warp_t_px[set_t], warp_budgets[set_budget]);
	vid_set_dither(set_dither);
#endif
}

static void player_spawn(void)
{
	memset(&player, 0, sizeof player);
	player.pos[0] = world.spawn[0] * CLIP_SUB;
	player.pos[1] = world.spawn[1] * CLIP_SUB;
	player.pos[2] = (world.spawn[2] + 1) * CLIP_SUB;   /* Quake drops it to the floor */
	player.yaw    = world.spawn_yaw * ANG_TURN / 360;
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

/* Pad -> one tick of intent:
     left stick / d-pad   walk forward / back, strafe left / right
                          (analog: speed follows the stick);
     right stick          look: left / right turn, up / down inverted
                          (push up to look down), finer near the centre;
     Square / Circle      turn left / right (pads without sticks);
     L1 / R1              look up / down;
     Cross                jump (noclip: down);
     Triangle             hold to walk slowly (noclip: up);
     START noclip;  SELECT respawn.
   Turning and looking advance per physics tick (`ticks` this frame), so
   their speed does not depend on the frame rate. */
static void player_cmd(pmove_cmd_t *cmd, int ticks)
{
	const int turn = 46, look = 34;          /* buttons: ~4 and ~3 degrees per tick */
	const int stick_turn = 48, stick_look = 36;   /* right stick, full deflection */

	if (pad.pressed & PAD_START)  player.noclip = !player.noclip;
	if (pad.pressed & PAD_SELECT) player_spawn();

	int lx = pad.lx, ly = pad.ly, rx = pad.rx, ry = pad.ry;
	stick(&lx, &ly);
	stick(&rx, &ry);

	int yaw = 0, pitch = 0;
	if (pad.held & PAD_SQUARE) yaw += turn;
	if (pad.held & PAD_CIRCLE) yaw -= turn;
	if ((pad.held & PAD_L1) && !(pad.held & PAD_R1)) pitch -= look;
	if ((pad.held & PAD_R1) && !(pad.held & PAD_L1)) pitch += look;
	yaw   -= stick_turn * rx * (rx < 0 ? -rx : rx) / (PM_AXIS * PM_AXIS);
	pitch -= stick_look * ry * (ry < 0 ? -ry : ry) / (PM_AXIS * PM_AXIS);   /* inverted */
	player.yaw   += yaw * ticks;
	player.pitch += pitch * ticks;
	if (player.pitch >  1012) player.pitch =  1012;    /* +-89 degrees */
	if (player.pitch < -1012) player.pitch = -1012;

	cmd->fwd  = -ly;                                    /* stick up = forward */
	cmd->side = lx;
	if (pad.held & PAD_UP)    cmd->fwd  =  PM_AXIS;     /* the d-pad: full */
	if (pad.held & PAD_DOWN)  cmd->fwd  = -PM_AXIS;
	if (pad.held & PAD_RIGHT) cmd->side =  PM_AXIS;
	if (pad.held & PAD_LEFT)  cmd->side = -PM_AXIS;
	cmd->up   = (pad.held & PAD_TRIANGLE) ? 1 : (pad.held & PAD_CROSS) ? -1 : 0;
	cmd->jump = (pad.pressed & PAD_CROSS) ? 1 : 0;
	cmd->walk = (pad.held & PAD_TRIANGLE) ? 1 : 0;
}

/* The simulation is tick-based (Quake): one 60 Hz tick per vblank elapsed,
   so a frame that took two vblanks advances the player twice. */
static void player_update(void)
{
	uint32_t t0 = prof_now();
	pmove_cmd_t cmd;
	int ticks = (int) vid_stats.vbl;
	if (ticks < 1) ticks = 1;
	if (ticks > 4) ticks = 4;
	player_cmd(&cmd, ticks);
	for (int i = 0; i < ticks; i++) {
		pmove_step(&player, &cmd);
		cmd.jump = 0;                          /* edge: first tick only */
	}
	cam.pos[0] = player.pos[0];
	cam.pos[1] = player.pos[1];
	cam.pos[2] = player.pos[2] + PM_EYE_HEIGHT * CLIP_SUB;
	cam.yaw    = player.yaw;
	cam.pitch  = player.pitch;
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
	         fidelity_name[load_fid], set_tex4 ? ", 4-bit" : "");
	vid_text(BX, BY - 16, line);
	snprintf(line, sizeof line, "%3d %%", total ? (int) (done * 100 / total) : 0);
	vid_text(BX + BW - 40, BY + BH + 8, line);
	ui_rect(BX + 1, BY + 1, fill > BW - 2 ? BW - 2 : fill, BH - 2, 230, 170, 60);  /* bar */
	ui_rect(BX, BY, BW, BH, 70, 74, 90);                                        /* frame */
	vid_flip();
}

#ifdef Q1PSX_BENCH
#define LOAD_PROGRESS NULL         /* the bench measures; no screen */
#else
#define LOAD_PROGRESS loading_screen
#endif

/* Load `map` at fidelity `fid`, or the highest lower level that fits the
   level arena. Sets world_ok / fidelity and binds the renderer. */
static int load_level(int map, int fid)
{
	world_ok = 0;
	load_map = map;
	for (int f = fid; f >= 0 && !world_ok; f--) {
		load_fid = f;
		world_ok = world_load(map, f, set_tex4, LOAD_PROGRESS);
		if (world_ok)
			fidelity = f;
	}
	fid_loaded = fid;
	if (world_ok) {
		if (fidelity != fid)
			printf("level: %s does not fit at %s, loaded at %s\n", world_map_name(map),
			       fidelity_name[fid], fidelity_name[fidelity]);
		r_bind_world();
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
		fid_wanted = (fid_wanted + 1) % FIDELITY_LEVELS;
	if (load_level(map_index, fid_wanted) && next_map)
		player_spawn();
}

/* --------------------------------------------------------------- frame -- */

static void frame_work(void)
{
	if (!world_ok)
		return;
	r_set_view(cam.pos, cam.yaw, cam.pitch);
	r_frame();
}

static int overlay_on = 1;   /* L1+R1 toggles */

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
	         "pos %d %d %d\n"
	         "pad %s type %d btn %04x%s",
	         vid_stats.pkt_bytes, vid_stats.dropped ? " FULL" : "",
	         warp_mode_name[r_warp.mode], r_warp.t, r_warp.t_eff, (unsigned) r_warp.tris,
	         world.tex4 ? "  4-bit" : "", set_dither ? "" : "  no dither",
	         cam.pos[0] / CLIP_SUB, cam.pos[1] / CLIP_SUB, cam.pos[2] / CLIP_SUB,
	         pad.connected ? "ok" : "--", pad.type, pad.held, sticks);
	vid_text(8, 8, line);
	prof_add(PROF_OVERLAY, t0, prof_now());
}

/* Run one frame; prof_stage[] / prof_count[] describe it until the next call. */
static void run_frame(int interactive, int dump)
{
	prof_frame_reset();
	uint32_t t0 = prof_now();
	if (interactive) {
		pad_read();
		if ((pad.held & (PAD_SELECT | PAD_START)) == (PAD_SELECT | PAD_START) &&
		    (pad.pressed & (PAD_SELECT | PAD_START))) {
			in_menu  = 1;                      /* SELECT + START: the map menu */
			menu_sel = map_index;
			return;
		}
		if ((pad.held & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1) &&
		    (pad.pressed & (PAD_L1 | PAD_R1)))
			overlay_on = !overlay_on;
		player_update();
		if (pad.pressed & PAD_L2)
			switch_level(0);
		else if (pad.pressed & PAD_R2)
			switch_level(1);
	}
	frame_work();
	prof_add(PROF_FRAME, t0, prof_now());
	if (interactive && overlay_on)
		draw_overlay();
	if (dump)
		vid_dump();
	vid_flip();
	r_warp_feedback(prof_stage[PROF_FRAME] + prof_stage[PROF_OVERLAY], vid_stats.vbl);
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
static void menu_init(void)
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
enum { OPT_FID, OPT_TEX, OPT_DITHER, OPT_WARP, OPT_T, OPT_BUDGET, OPT_COUNT };
static int opt_sel;

static void opt_step(int d)
{
	switch (opt_sel) {
	case OPT_FID:    fid_wanted = (fid_wanted + FIDELITY_LEVELS + d) % FIDELITY_LEVELS; break;
	case OPT_TEX:    set_tex4 ^= 1; break;
	case OPT_DITHER: set_dither ^= 1; break;
	case OPT_WARP:   set_warp = (set_warp + R_WARP_MODES + d) % R_WARP_MODES; break;
	case OPT_T:      set_t = (set_t + NUM_WARP_T + d) % NUM_WARP_T; break;
	default:         set_budget = (set_budget + NUM_WARP_BUDGET + d) % NUM_WARP_BUDGET; break;
	}
	apply_settings();   /* warp and dithering at once; textures, fidelity on load */
}

static void opt_draw(void)
{
	static const char *const label[OPT_COUNT] = {
		"fidelity", "textures", "dither", "warp", "threshold", "budget",
	};
	char line[48], val[16];
	for (int o = 0; o < OPT_COUNT; o++) {
		switch (o) {
		case OPT_FID:    snprintf(val, sizeof val, "%s", fidelity_name[fid_wanted]); break;
		case OPT_TEX:    snprintf(val, sizeof val, "%s", set_tex4 ? "4 bit" : "8 bit"); break;
		case OPT_DITHER: snprintf(val, sizeof val, "%s", set_dither ? "on" : "off"); break;
		case OPT_WARP:   snprintf(val, sizeof val, "%s", warp_mode_name[set_warp]); break;
		case OPT_T:      snprintf(val, sizeof val, "%d px", warp_t_px[set_t]); break;
		default:         snprintf(val, sizeof val, "%d tris", warp_budgets[set_budget]); break;
		}
		int x = 16 + (o / 3) * 152, y = 186 + (o % 3) * 10;
		snprintf(line, sizeof line, "%-9s< %s >", label[o], val);
		vid_text(x, y, line);
		if (o == opt_sel)
			ui_rect(x - 4, y - 1, 148, 10, 60, 70, 120);
	}
}

/* One frame of the menu: d-pad picks a map, L2 / R2 an option and L1 / R1
   its value, Cross / START loads the map, Circle goes back to the loaded
   one (reloading it if the textures or the fidelity changed). */
static void menu_frame(void)
{
	int n = world_num_maps();

	pad_read();
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
		if (world.tex4 != set_tex4 || fid_loaded != fid_wanted)
			load_level(map_index, fid_wanted);   /* same map, same place */
		if (world_ok) {
			in_menu = 0;                         /* back to the loaded map */
			return;
		}
		menu_msg = "could not reload that map";
	}
	if (n && (pad.pressed & (PAD_CROSS | PAD_START))) {
		map_index = menu_sel;
		if (load_level(map_index, fid_wanted)) {
			player_spawn();
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
		int x = 24 + (m / MENU_ROWS) * 148, y = 20 + (m % MENU_ROWS) * 10;
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

#define BENCH_WARMUP 8
#define BENCH_FRAMES 64

/* The physics' C reference, linked under these names (CMakeLists.txt). */
q_fixed clip_trace_c(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr);
int     clip_contents_c(int hull, const int32_t p[3]);
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
		int     hull = (i & 7) == 7 ? 2 : 1;
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
	int cbad = 0;
	for (int i = 0; i < N; i++) {
		int32_t p[3];
		rng_point(p, 0);
		int hull = (i & 3) == 3 ? 2 : 1;
		if (clip_contents(hull, p) != clip_contents_c(hull, p))
			cbad++;
	}
	printf("clipdiff traces %d (%d hit, %d start in solid) mismatches %d; contents %d mismatches %d; "
	       "cycles/trace asm %u c %u\n", N, hits, solid, bad, N, cbad,
	       (unsigned) (cyc_asm / N), (unsigned) (cyc_c / N));
}

/* pmove.s (on clip.s) against pmove.c (on clip.c): a random walk with
   random commands, turns, jumps, short noclip flights and a teleport to a
   random open spot every 300 ticks. Both run each tick from the same state;
   every pmove_t field must match (on a mismatch the asm side is resynced to
   the reference so the count stays meaningful). */
static void bench_pmove_diff(void)
{
	enum { TICKS = 6000 };
	pmove_t     a, c;
	pmove_cmd_t cmd;
	int         bad = 0;
	uint32_t    cyc_asm = 0, cyc_c = 0;
	memset(&a, 0, sizeof a);
	memset(&cmd, 0, sizeof cmd);
	for (int t = 0; t < TICKS; t++) {
		int phase = t % 300;
		if (phase == 0) {
			rng_point(a.pos, 1);
			a.vel[0] = a.vel[1] = a.vel[2] = 0;
			a.noclip = 0;
		}
		a.noclip = phase >= 250 && phase < 280;
		if (t % 30 == 0) {
			cmd.fwd  = rng_axis();
			cmd.side = rng_axis();
			cmd.up   = (int) (rng() % 3) - 1;
			cmd.walk = (rng() & 7) == 0;
			a.pitch  = rng_pm(1000);
		}
		cmd.jump = (rng() & 31) == 0;
		a.yaw += rng_pm(48);
		c = a;
		pmove_t before = a;
		uint32_t t0 = prof_now();
		pmove_step(&a, &cmd);
		uint32_t t1 = prof_now();
		pmove_step_c(&c, &cmd);
		uint32_t t2 = prof_now();
		cyc_asm += prof_cycles(t0, t1);
		cyc_c   += prof_cycles(t1, t2);
		if (memcmp(&a, &c, sizeof a)) {
			if (bad++ < 8)
				printf("pmdiff MISMATCH tick %d noclip %d cmd %d %d %d %d %d\n"
				       "  asm: pos %d %d %d vel %d %d %d g %d acc %d %d %d %d\n"
				       "  c:   pos %d %d %d vel %d %d %d g %d acc %d %d %d %d\n",
				       t, c.noclip, cmd.fwd, cmd.side, cmd.up, cmd.jump, cmd.walk,
				       (int) a.pos[0], (int) a.pos[1], (int) a.pos[2], (int) a.vel[0], (int) a.vel[1],
				       (int) a.vel[2], a.onground, (int) a.pos_acc[0], (int) a.pos_acc[1],
				       (int) a.pos_acc[2], (int) a.grav_acc,
				       (int) c.pos[0], (int) c.pos[1], (int) c.pos[2], (int) c.vel[0], (int) c.vel[1],
				       (int) c.vel[2], c.onground, (int) c.pos_acc[0], (int) c.pos_acc[1],
				       (int) c.pos_acc[2], (int) c.grav_acc);
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
	printf("pmdiff ticks %d mismatches %d; cycles/tick asm %u c %u\n", TICKS, bad,
	       (unsigned) (cyc_asm / TICKS), (unsigned) (cyc_c / TICKS));

	/* the same without clip hulls (every trace returns at once): the cost of
	   pmove itself, without its traces */
	const qw_clipnode *cn = world.clipnodes;
	world.clipnodes = NULL;
	cyc_asm = cyc_c = 0;
	cmd.fwd = PM_AXIS;
	cmd.side = PM_AXIS;
	cmd.jump = 0;
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
   does not fit the arena), then its four spawn views, briefly. */
static void bench_maps(void)
{
	static char names[4][16];
	for (int m = 0; m < world_num_maps(); m++) {
		if (!load_level(m, Q1PSX_BENCH_FID)) {
			printf("bench map %s FAILED to load\n", world_map_name(m));
			continue;
		}
		printf("bench map %s fid %s arena %u KB\n", world_map_name(m), fidelity_name[fidelity],
		       mem_level_used() / 1024);
		for (uint32_t v = 0; v < 4 && v < world.num_views; v++) {
			const qw_view *vw = &world.views[v];
			cam.pos[0] = vw->pos[0] * CLIP_SUB;
			cam.pos[1] = vw->pos[1] * CLIP_SUB;
			cam.pos[2] = vw->pos[2] * CLIP_SUB;
			cam.yaw    = vw->yaw * ANG_TURN / 360;
			cam.pitch  = vw->pitch * ANG_TURN / 360;
			snprintf(names[v], sizeof names[v], "%s.v%u", world_map_name(m), v);
			bench_acc_t acc;
			bench_reset(&acc, names[v]);
			for (int i = 0; i < 4; i++)
				run_frame(0, 0);
			for (int i = 0; i < 16; i++) {
				run_frame(0, 0);
				bench_add(&acc, vid_stats.vbl);
			}
			bench_print(&acc);
		}
	}
}

static void run_bench(void)
{
	static char names[16][8];

	printf("bench start\n");
	if (Q1PSX_BENCH_MAPS) {
		bench_maps();
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
		run_frame(0, 1);
	}
	/* scripted movement (no pad in headless runs): settle, accelerate,
	   jump, run north into the end wall (stairs down, ramp up), then turn
	   around and come back up the stairs. Same path as tools/pmsim. */
	{
		pmove_cmd_t c;
		uint32_t    cyc_sum = 0, cyc_max = 0, ticks = 0;
		memset(&c, 0, sizeof c);
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

int main(void)
{
	vid_init();
	pad_init();
	prof_init();
	cd_init();
	r_init();
	vid_set_bg(40, 44, 60);   /* flat "sky" until R3.4 */
	apply_settings();

	printf("q1psx\n");

	world_scan_maps();
#ifdef Q1PSX_BENCH
	fid_wanted = Q1PSX_BENCH_FID;
	if (load_level(map_index, fid_wanted))
		player_spawn();
	run_bench();
#else
	menu_init();
	in_menu = 1;
	for (;;) {
		if (in_menu)
			menu_frame();
		else
			run_frame(1, 0);
	}
#endif
}
