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
#include <psxgpu.h>

#include "bench.h"
#include "cd.h"
#include "mem.h"
#include "pad.h"
#include "prof.h"
#include "render.h"
#include "vid.h"
#include "world.h"

/* ---------------------------------------------------------------- camera -- */

typedef struct {
	int32_t pos[3];      /* world units          */
	int     yaw, pitch;  /* binary degrees       */
} camera_t;

static camera_t cam;
static int      world_ok;

static void camera_spawn(void)
{
	cam.pos[0] = world.spawn[0];
	cam.pos[1] = world.spawn[1];
	cam.pos[2] = world.spawn[2] + 22;          /* Quake eye height */
	cam.yaw    = world.spawn_yaw * ANG_TURN / 360;
	cam.pitch  = 0;
}

/* Debug free-fly (until R4 brings the player): d-pad turn / move, Square /
   Circle strafe, L1 / R1 look up / down, Triangle / Cross rise / sink,
   START back to spawn. */
static void camera_fly(void)
{
	const int speed = 8, turn = 48, look = 32;

	if (pad.held & PAD_LEFT)  cam.yaw += turn;
	if (pad.held & PAD_RIGHT) cam.yaw -= turn;
	if ((pad.held & PAD_L1) && !(pad.held & PAD_R1)) cam.pitch -= look;
	if ((pad.held & PAD_R1) && !(pad.held & PAD_L1)) cam.pitch += look;
	if (cam.pitch >  1000) cam.pitch =  1000;
	if (cam.pitch < -1000) cam.pitch = -1000;

	int32_t fx = r_cos(cam.yaw), fy = r_sin(cam.yaw);    /* forward, x4096 */
	int fwd  = (pad.held & PAD_UP) ? 1 : (pad.held & PAD_DOWN) ? -1 : 0;
	int side = (pad.held & PAD_CIRCLE) ? 1 : (pad.held & PAD_SQUARE) ? -1 : 0;
	cam.pos[0] += (fwd * fx + side * fy) * speed >> 12;
	cam.pos[1] += (fwd * fy - side * fx) * speed >> 12;
	if (pad.held & PAD_TRIANGLE) cam.pos[2] += speed;
	if (pad.held & PAD_CROSS)    cam.pos[2] -= speed;
	if (pad.pressed & PAD_START) camera_spawn();
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
	         world_ok ? fidelity_name[2] : "no world",
	         vid_stats.vbl ? 60 / vid_stats.vbl : 60,
	         prof_stage[PROF_FRAME], vid_stats.sync_cycles,
	         prof_stage[PROF_R_WALK], prof_stage[PROF_R_SETUP], prof_stage[PROF_R_BLOCK],
	         prof_count[PROF_N_NODES], prof_count[PROF_N_BLOCKS], prof_count[PROF_N_VERTS],
	         prof_count[PROF_N_POLYS], prof_count[PROF_N_SLOW],
	         vid_stats.pkt_bytes, vid_stats.dropped ? " FULL" : "",
	         cam.pos[0], cam.pos[1], cam.pos[2],
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
		camera_fly();
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
		cam.pos[0] = vw->pos[0];
		cam.pos[1] = vw->pos[1];
		cam.pos[2] = vw->pos[2];
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
	world_ok = world_load(2);
#endif
	if (world_ok) {
		r_bind_world();
		camera_spawn();
	}

#ifdef Q1PSX_BENCH
	run_bench();
#else
	for (;;)
		run_frame(1, 0);
#endif
}
