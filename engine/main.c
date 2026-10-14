/*
 * q1psx — boot + main loop.
 *
 * Normal build: run frames forever with the debug overlay.
 * Bench build (Q1PSX_BENCH, CMake preset "bench"): run every bench scenario,
 * print one "bench ..." line each, then "bench done" — see scripts/bench.sh.
 */

#include <stdint.h>
#include <stdio.h>
#include <psxgpu.h>

#include "bench.h"
#include "cd.h"
#include "mem.h"
#include "pad.h"
#include "prof.h"
#include "vid.h"
#include "world.h"

/* --------------------------------------------------------------- frame -- */

/* One frame of engine work: simulation + rendering (nothing yet). */
static void frame_work(void)
{
}

static int overlay_on = 1;   /* L1+R1 toggles */

static void draw_overlay(void)
{
	uint32_t t0 = prof_now();
	char line[192];

	snprintf(line, sizeof line,
	         "q1psx\n\n"
	         "frame %6u cyc  %u vbl\n"
	         "gpu wait %u  vbl wait %u\n"
	         "packets %u B%s\n"
	         "pad %s %04x",
	         prof_stage[PROF_FRAME], vid_stats.vbl,
	         vid_stats.sync_cycles, vid_stats.vsync_cycles,
	         vid_stats.pkt_bytes, vid_stats.dropped ? "  DROPPED" : "",
	         pad.connected ? "ok" : "--", pad.held);
	vid_text(16, 16, line);
	prof_add(PROF_OVERLAY, t0, prof_now());
}

/* Run one frame; prof_stage[] / prof_count[] describe it until the next call. */
static void run_frame(int interactive)
{
	prof_frame_reset();
	uint32_t t0 = prof_now();
	if (interactive) {
		pad_read();
		if ((pad.held & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1) &&
		    (pad.pressed & (PAD_L1 | PAD_R1)))
			overlay_on = !overlay_on;
	}
	frame_work();
	prof_add(PROF_FRAME, t0, prof_now());
	if (interactive && overlay_on)
		draw_overlay();
	vid_flip();
}

/* --------------------------------------------------------------- bench -- */

#ifdef Q1PSX_BENCH

#define BENCH_WARMUP 8
#define BENCH_FRAMES 64

static void bench_scenario(const char *name)
{
	bench_acc_t acc;
	bench_reset(&acc, name);

	for (int i = 0; i < BENCH_WARMUP; i++)
		run_frame(0);
	for (int i = 0; i < BENCH_FRAMES; i++) {
		run_frame(0);
		/* vid_stats.vbl now holds the vblanks of the frame just measured */
		bench_add(&acc, vid_stats.vbl);
	}
	bench_print(&acc);
}

static void run_bench(void)
{
	printf("bench start\n");
	bench_scenario("empty");
	printf("bench done\n");
	for (;;)
		run_frame(0);
}

#endif

/* ---------------------------------------------------------------- main -- */

int main(void)
{
	vid_init();
	pad_init();
	prof_init();
	vid_set_bg(24, 24, 32);
	cd_init();
	mem_level_reset();

	printf("q1psx\n");

	if (world_load(2)) {
		const qw_leaf *l = world_point_leaf(world.spawn);
		printf("world: spawn %d %d %d in leaf %d (contents %d)\n",
		       world.spawn[0], world.spawn[1], world.spawn[2],
		       (int) (l - world.leaves), l->contents);
	}

#ifdef Q1PSX_BENCH
	run_bench();
#else
	for (;;)
		run_frame(1);
#endif
}
