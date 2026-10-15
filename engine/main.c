/*
 * q1psx — boot + main loop.
 *
 * Normal build: load the first map on the disc (start, fidelity "high"),
 * player physics over the asm-rendered world, debug overlay.
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
static int      map_index;      /* into world_map_name(); R2 cycles it */
static int      fid_wanted = 2; /* 0 low .. 3 ultra; L2 cycles it      */
static int      fidelity   = 2; /* the level actually loaded (a map too
                                   big for the arena falls back lower)  */

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

/* ------------------------------------------------------- map / fidelity -- */

/* Load `map` at fidelity `fid`, or the highest lower level that fits the
   level arena. Sets world_ok / fidelity and binds the renderer. */
static int load_level(int map, int fid)
{
	world_ok = 0;
	for (int f = fid; f >= 0 && !world_ok; f--) {
		world_ok = world_load(map, f);
		if (world_ok)
			fidelity = f;
	}
	if (world_ok) {
		if (fidelity != fid)
			printf("level: %s does not fit at %s, loaded at %s\n", world_map_name(map),
			       fidelity_name[fid], fidelity_name[fidelity]);
		r_bind_world();
	}
	return world_ok;
}

/* L2: reload the map at the next fidelity level, keeping the camera where
   it is; R2: next map, at its spawn. The CD reads block for a moment: fine
   for debug controls. */
static void switch_level(int next_map)
{
	vid_text(8, 8, "loading...");
	vid_flip();
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
	char line[256];

	snprintf(line, sizeof line,
	         "q1psx  %s %s  ~%u fps\n"
	         "frame %6u cyc  gpu wait %u  arena %u KB\n"
	         "walk %u  mark %u  faces %u\n"
	         "nodes %u recs %u verts %u\n"
	         "polys %u  near %u  %u B%s\n"
	         "pos %d %d %d\n"
	         "pad %s type %d btn %04x",
	         world_map_name(map_index), world_ok ? fidelity_name[fidelity] : "no world",
	         vid_stats.vbl ? 60 / vid_stats.vbl : 60,
	         prof_stage[PROF_FRAME], vid_stats.sync_cycles, mem_level_used() / 1024,
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
}

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
			cmd.fwd  = (int) (rng() % 3) - 1;
			cmd.side = (int) (rng() % 3) - 1;
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
	cmd.fwd = 1;
	cmd.side = 1;
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

	world_scan_maps();
#ifdef Q1PSX_BENCH
	fid_wanted = Q1PSX_BENCH_FID;
#endif
	if (load_level(map_index, fid_wanted))
		player_spawn();

#ifdef Q1PSX_BENCH
	run_bench();
#else
	for (;;)
		run_frame(1, 0);
#endif
}
