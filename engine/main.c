/*
 * q1psx — boot + main loop.
 *
 * Normal build: load start.bsp (fidelity "high"), free-fly camera over the
 * asm-rendered world, debug overlay.
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
static int      world_ok;
static int      fidelity = 2;   /* 0 low .. 3 ultra; L2 cycles it */

static void player_spawn(void)
{
	memset(&player, 0, sizeof player);
	player.pos[0] = world.spawn[0] * CLIP_SUB;
	player.pos[1] = world.spawn[1] * CLIP_SUB;
	player.pos[2] = (world.spawn[2] + 1) * CLIP_SUB;   /* Quake drops it to the floor */
	player.yaw    = world.spawn_yaw * ANG_TURN / 360;
}

/* Pad -> one tick of intent (phase 6's mapping):
     d-pad left/right turn, up/down walk;  Square / Circle strafe;
     L1 / R1 look up / down;  Cross jump (noclip: down);
     Triangle hold to walk (noclip: up);  START noclip;  SELECT respawn. */
static void player_cmd(pmove_cmd_t *cmd)
{
	const int turn = 46, look = 34;            /* ~4 and ~3 degrees per tick */

	if (pad.pressed & PAD_START)  player.noclip = !player.noclip;
	if (pad.pressed & PAD_SELECT) player_spawn();

	if (pad.held & PAD_LEFT)  player.yaw += turn;
	if (pad.held & PAD_RIGHT) player.yaw -= turn;
	if ((pad.held & PAD_L1) && !(pad.held & PAD_R1)) player.pitch -= look;
	if ((pad.held & PAD_R1) && !(pad.held & PAD_L1)) player.pitch += look;
	if (player.pitch >  1012) player.pitch =  1012;    /* +-89 degrees */
	if (player.pitch < -1012) player.pitch = -1012;

	cmd->fwd  = (pad.held & PAD_UP) ? 1 : (pad.held & PAD_DOWN) ? -1 : 0;
	cmd->side = (pad.held & PAD_CIRCLE) ? 1 : (pad.held & PAD_SQUARE) ? -1 : 0;
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
	player_cmd(&cmd);
	int ticks = (int) vid_stats.vbl;
	if (ticks < 1) ticks = 1;
	if (ticks > 4) ticks = 4;
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

/* ------------------------------------------------------------ fidelity -- */

/* Reload the map at the next fidelity level (textures + world), keeping the
   camera where it is. The CD reads block for a moment: fine for a debug
   control. */
static void switch_fidelity(void)
{
	fidelity = (fidelity + 1) % FIDELITY_LEVELS;
	vid_text(8, 8, "loading...");
	vid_flip();
	world_ok = world_load(fidelity);
	if (world_ok)
		r_bind_world();
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
	char line[256];

	snprintf(line, sizeof line,
	         "q1psx  %s  ~%u fps\n"
	         "frame %6u cyc  gpu wait %u\n"
	         "walk %u  mark %u  faces %u\n"
	         "nodes %u recs %u verts %u\n"
	         "polys %u  near %u  %u B%s\n"
	         "pos %d %d %d\n"
	         "pad %s type %d btn %04x",
	         world_ok ? fidelity_name[fidelity] : "no world",
	         vid_stats.vbl ? 60 / vid_stats.vbl : 60,
	         prof_stage[PROF_FRAME], vid_stats.sync_cycles,
	         prof_stage[PROF_R_WALK], prof_stage[PROF_R_SETUP], prof_stage[PROF_R_BLOCK],
	         prof_count[PROF_N_NODES], prof_count[PROF_N_BLOCKS], prof_count[PROF_N_VERTS],
	         prof_count[PROF_N_POLYS], prof_count[PROF_N_SLOW],
	         vid_stats.pkt_bytes, vid_stats.dropped ? " FULL" : "",
	         cam.pos[0] / CLIP_SUB, cam.pos[1] / CLIP_SUB, cam.pos[2] / CLIP_SUB,
	         pad.connected ? "ok" : "--", pad.type, pad.held);
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
		if ((pad.held & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1) &&
		    (pad.pressed & (PAD_L1 | PAD_R1)))
			overlay_on = !overlay_on;
		player_update();
		if (pad.pressed & PAD_L2)
			switch_fidelity();
	}
	frame_work();
	prof_add(PROF_FRAME, t0, prof_now());
	if (interactive && overlay_on)
		draw_overlay();
	if (dump)
		vid_dump();
	vid_flip();
}

/* --------------------------------------------------------------- bench -- */

#ifdef Q1PSX_BENCH

#ifndef Q1PSX_BENCH_FID
#define Q1PSX_BENCH_FID 2     /* fidelity level the bench loads (0 low .. 3 ultra) */
#endif

#define BENCH_WARMUP 8
#define BENCH_FRAMES 64

static void run_bench(void)
{
	static char names[16][8];

	printf("bench start\n");
	for (uint32_t v = 0; world_ok && v < world.num_views && v < 16; v++) {
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
		c.fwd = 1;
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

	printf("q1psx\n");

#ifdef Q1PSX_BENCH
	world_ok = world_load(Q1PSX_BENCH_FID);
#else
	world_ok = world_load(fidelity);
#endif
	if (world_ok) {
		r_bind_world();
		player_spawn();
	}

#ifdef Q1PSX_BENCH
	run_bench();
#else
	for (;;)
		run_frame(1, 0);
#endif
}
