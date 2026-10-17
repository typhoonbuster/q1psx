/*
 * pmsim — run the engine's player physics on the host (debug tool).
 *
 *   pmsim <world.wld>                    the PS1 bench's scripted path
 *   pmsim <world.wld> stress [segments]  random walk over the whole map
 *
 * Compiles engine/clip.c and engine/pmove.c unchanged for the PC and feeds
 * them the clip hulls of a cooked world file. Much faster to iterate on than
 * a headless emulator run.
 *
 * The default script is the one engine/main.c runs in the bench build; both
 * must print the same positions. The stress mode walks random directions
 * from random open spots (teleporting every 10 segments of 60 ticks), and
 * reports any tick that ends inside solid and any segment that barely moved
 * although the way ahead was open — both should be 0. Build with
 * -DPM_DEBUG for a per-bump trace.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clip.h"
#include "pmove.h"
#include "world.h"

world_t world;   /* only the clip-hull fields are used */

clip_bmodel_t clip_bmodels[CLIP_MAX_BMODELS];   /* (no brush models on the host) */
int           clip_num_bmodels;

int16_t r_sin(int a) { return (int16_t) lround(sin(a * 2 * M_PI / 4096) * 4096); }
int16_t r_cos(int a) { return (int16_t) lround(cos(a * 2 * M_PI / 4096) * 4096); }

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: pmsim <world.wld>\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc((size_t) n);
	if (fread(d, 1, (size_t) n, f) != (size_t) n) return 1;
	fclose(f);
	qw_header h;
	memcpy(&h, d, sizeof h);
	const qw_clip_header *ch = (const qw_clip_header *) (d + h.ofs_clip);
	world.planes     = (const qw_plane *) (ch + 1);
	world.clipnodes  = (const qw_clipnode *) (world.planes + ch->num_planes);
	world.hull1_root = ch->hull1_root;
	world.hull2_root = ch->hull2_root;

	pmove_t pm;
	memset(&pm, 0, sizeof pm);
	pm.pos[0] = h.spawn[0] * CLIP_SUB;
	pm.pos[1] = h.spawn[1] * CLIP_SUB;
	pm.pos[2] = (h.spawn[2] + 1) * CLIP_SUB;
	pm.yaw    = h.spawn_yaw * 4096 / 360;

	pmove_cmd_t c;
	memset(&c, 0, sizeof c);
	for (int t = 0; t < 30; t++) pmove_step(&pm, &c);
	printf("settle pos %d %d %d ground %d\n", pm.pos[0] / CLIP_SUB, pm.pos[1] / CLIP_SUB, pm.pos[2] / CLIP_SUB,
	       pm.onground);
	if (argc > 2 && !strcmp(argv[2], "stress")) {
		/* random walk: random yaw for 60 ticks, sometimes jumping or
		   strafing; flag any tick inside solid and any segment that barely
		   moved (candidates for "stuck") */
		int segs = argc > 3 ? atoi(argv[3]) : 200, solid = 0, stuck = 0, blocked = 0;
		double total = 0;
		srand(1);
		for (int s = 0; s < segs; s++) {
			/* every 10 segments, teleport to a random open spot above a
			   floor, so the walk covers the whole map */
			while (s % 10 == 0) {
				int32_t p[3] = { (rand() % 8192 - 4096) * CLIP_SUB, (rand() % 8192 - 4096) * CLIP_SUB,
				                 (rand() % 4096 - 2048) * CLIP_SUB };
				int32_t dn[3] = { p[0], p[1], p[2] - 256 * CLIP_SUB };
				trace_t tr;
				if (clip_contents(1, p) != QW_CONTENTS_EMPTY)
					continue;
				clip_trace(1, p, dn, &tr);
				if (tr.fraction >= Q_FIX_ONE || tr.startsolid)
					continue;
				memcpy(pm.pos, tr.endpos, sizeof pm.pos);
				memset(pm.vel, 0, sizeof pm.vel);
				break;
			}
			pm.yaw = rand() & 4095;
			c.fwd  = PM_AXIS;
			c.side = ((rand() % 3) - 1) * PM_AXIS;
			int32_t p0[3] = { pm.pos[0], pm.pos[1], pm.pos[2] };
#ifdef PM_DEBUG
			printf("=== seg %d yaw %d side %d\n", s, pm.yaw, c.side);
#endif
			for (int t = 0; t < 60; t++) {
				c.jump = (t == 20 && (rand() & 3) == 0);
#ifdef PM_DEBUG
				printf("-- t %d pos %d %d %d vel %d %d %d g %d\n", t, pm.pos[0], pm.pos[1], pm.pos[2],
				       pm.vel[0], pm.vel[1], pm.vel[2], pm.onground);
#endif
				pmove_step(&pm, &c);
				if (clip_contents(1, pm.pos) == QW_CONTENTS_SOLID) {
					if (solid++ < 10)
						printf("SOLID seg %d t %d pos %d %d %d\n", s, t, pm.pos[0], pm.pos[1], pm.pos[2]);
				}
			}
			double dx = (pm.pos[0] - p0[0]) / 8.0, dy = (pm.pos[1] - p0[1]) / 8.0;
			double moved = sqrt(dx * dx + dy * dy);
			/* a slow segment is suspect only if the way ahead is open: trace
			   16 units along the wished direction from where we ended */
			int32_t sy = r_sin(pm.yaw), cy = r_cos(pm.yaw);
			double sd = c.side / (double) PM_AXIS;
			double wx = cy + sy * sd, wy = sy - cy * sd, wl = sqrt(wx * wx + wy * wy);
			int32_t ahead[3] = { pm.pos[0] + (int32_t) (wx / wl * 128), pm.pos[1] + (int32_t) (wy / wl * 128),
			                     pm.pos[2] };
			trace_t tr;
			clip_trace(1, pm.pos, ahead, &tr);
			total += moved;
			blocked += moved < 16;
			if (moved < 16 && tr.fraction > Q_FIX_ONE / 4) {
				stuck++;
				printf("stuck seg %3d yaw %4d side %2d from %d %d %d moved %.2f vel %d %d %d free %d\n", s, pm.yaw,
				       c.side, p0[0], p0[1], p0[2], moved, pm.vel[0], pm.vel[1], pm.vel[2], tr.fraction);
			}
		}
		printf("stress: %d segments, %.0f units walked, %d blocked by walls, %d solid ticks, %d stuck segments\n",
		       segs, total, blocked, solid, stuck);
		return 0;
	}

	/* the PS1 bench's scripted path (engine/main.c): run, jump, run north
	   into the end wall, turn around and come back up the stairs */
	c.fwd = PM_AXIS;
	int t310 = -1;
	for (int i = 0; i < 60; i++) {
		pmove_step(&pm, &c);
		if (t310 < 0 && sqrt((double) pm.vel[0] * pm.vel[0] + (double) pm.vel[1] * pm.vel[1]) >= 310)
			t310 = i + 1;
	}
	printf("run: 310 u/s after %d ticks, pos %d %d %d\n", t310, pm.pos[0] / CLIP_SUB, pm.pos[1] / CLIP_SUB,
	       pm.pos[2] / CLIP_SUB);
	int32_t z0 = pm.pos[2], zmax = z0;
	c.jump = 1;
	for (int i = 0; i < 90; i++) {
		pmove_step(&pm, &c);
		c.jump = 0;
		if (pm.pos[2] > zmax)
			zmax = pm.pos[2];
	}
	printf("jump apex %d units, now %d %d %d (ground %d)\n", (zmax - z0) / CLIP_SUB, pm.pos[0] / CLIP_SUB,
	       pm.pos[1] / CLIP_SUB, pm.pos[2] / CLIP_SUB, pm.onground);
	for (int i = 0; i < 300; i++)
		pmove_step(&pm, &c);
	printf("wall: stopped at %d %d %d vel %d %d\n", pm.pos[0] / CLIP_SUB, pm.pos[1] / CLIP_SUB,
	       pm.pos[2] / CLIP_SUB, pm.vel[0], pm.vel[1]);
	pm.yaw += 2048;
	for (int t = 0; t < 300; t++) {
		pmove_step(&pm, &c);
		if (t % 20 == 19)
			printf("  back t%3d pos %8.2f %8.2f %7.2f vel %4d %4d %4d ground %d\n", t, pm.pos[0] / 8.0,
			       pm.pos[1] / 8.0, pm.pos[2] / 8.0, pm.vel[0], pm.vel[1], pm.vel[2], pm.onground);
	}
	printf("back: at %d %d %d (ground %d)\n", pm.pos[0] / CLIP_SUB, pm.pos[1] / CLIP_SUB, pm.pos[2] / CLIP_SUB,
	       pm.onground);
	return 0;
}
