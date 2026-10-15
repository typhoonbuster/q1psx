/*
 * q1psx — hull trace, C reference.
 *
 * The PS1 build links the asm port (engine/clip.s); this file is the
 * specification it follows. tools/pmsim runs it on the host, and the bench
 * build links it too (as clip_trace_c / clip_contents_c) to compare both
 * on every call. Keep the arithmetic identical: 32-bit integers, plus
 * 32 x 32 -> 64-bit products (one MIPS mult), nothing else.
 *
 * Plane distances t = n.p - dist are x4096 in 1/CLIP_SUB units (the cook
 * stores dist pre-scaled and each clipnode's plane as a word offset,
 * formats/qworld.h).
 * |n . p| <= 4096 * 32767 * 3 < 2^30: no overflow. The asm also needs
 * positions to fit int16 (maps within +-4095 units, Quake's limit is 4096).
 */

#include <stdint.h>
#include <string.h>

#include "clip.h"
#include "world.h"

/* Quake's DIST_EPSILON is 1/32 unit; positions here are quantised to
   1/CLIP_SUB, so the margin is one whole step (4096 in plane-distance
   units): it absorbs the rounding of the next moves along the surface. */
#define DIST_EPS  4096

/* Points along the sweep are never materialised while recursing: a split
   point rounded to whole sub-units bends the direction of the sub-segment
   after it (a move running along a ramp became one tipped INTO it, and the
   player stuck at the top). Instead every plane distance is interpolated
   from the sweep's start at a 16-bit fraction f of the whole sweep:
   t(f) = ds + (dd * f >> 16), ds = n.start - dist, dd = n.(end - start). */
#define PLANE(cn) ((const qw_plane *) ((const int32_t *) world.planes + (cn)->plane3))
#define F_SHIFT   16
#define F_ONE     (1 << F_SHIFT)
#define MAX_SPLIT QW_MAX_SPLIT   /* pending splits <= clip tree depth (cook-checked) */

/* n . v (x4096). Axial planes (type 0..2) have n = 4096 on their axis. */
static int32_t dot_n(const qw_plane *pl, const int32_t v[3])
{
	if (pl->type < 3)
		return v[pl->type] * 4096;
	return pl->n[0] * v[0] + pl->n[1] * v[1] + pl->n[2] * v[2];
}

static int32_t dist_at(int32_t ds, int32_t dd, int32_t f)
{
	return ds + (int32_t) (((int64_t) dd * f) >> F_SHIFT);
}

/* (num << 16) / den clamped to 0..F_ONE, in 32 bits: both operands are
   shifted down until den fits 15 bits (the asm takes the shift from the
   GTE's leading-zero counter). */
static int32_t split_frac(int32_t num, int32_t den)
{
	if (den < 0) {
		num = -num;
		den = -den;
	}
	if (num <= 0)
		return 0;
	if (num >= den)
		return F_ONE;
	int s = 0;
	while ((den >> s) >= (1 << 15))
		s++;
	return (int32_t) (((uint32_t) (num >> s) << F_SHIFT) / (uint32_t) (den >> s));
}

static const int32_t *tr_start;
static int32_t        tr_delta[3];

static int hull_contents(int num, const int32_t p[3])
{
	while (num >= 0) {
		const qw_clipnode *cn = &world.clipnodes[num];
		const qw_plane    *pl = PLANE(cn);
		num = cn->child[dot_n(pl, p) - pl->dist < 0 ? 1 : 0];
	}
	return num;
}

/* hull_contents() of the sweep's point at fraction f */
static int hull_contents_at(int num, int32_t f)
{
	while (num >= 0) {
		const qw_clipnode *cn = &world.clipnodes[num];
		const qw_plane    *pl = PLANE(cn);
		int32_t t = dist_at(dot_n(pl, tr_start) - pl->dist, dot_n(pl, tr_delta), f);
		num = cn->child[t < 0 ? 1 : 0];
	}
	return num;
}

/* endpos = the sweep's point at fraction f, each axis rounded toward the
   open side of the plane hit (toward the start where the normal has no
   component): the impact keeps its full DIST_EPS margin from that plane */
static void set_endpos(trace_t *tr, int32_t f)
{
	for (int i = 0; i < 3; i++) {
		int64_t d = (int64_t) tr_delta[i] * f;
		int32_t q = (int32_t) (d >> F_SHIFT);                 /* floor */
		int     up = tr->plane_n[i] > 0 || (tr->plane_n[i] == 0 && tr_delta[i] < 0);
		if (up && ((uint32_t) d & (F_ONE - 1)))
			q++;                                              /* ceil */
		tr->endpos[i] = tr_start[i] + q;
	}
}

/* SV_RecursiveHullCheck, iterative. A sub-sweep that ends in an open leaf
   "returns 1" to the innermost pending split, which then tries its far
   side; anything that resolves the trace (an impact, or never leaving
   solid) ends it at once, as Quake's chain of "return false" does. */
q_fixed clip_trace(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr)
{
	memset(tr, 0, sizeof *tr);
	tr->fraction = Q_FIX_ONE;
	tr->allsolid = 1;
	for (int i = 0; i < 3; i++)
		tr->endpos[i] = end[i];

	if (!world.clipnodes) {
		tr->allsolid = 0;
		tr->inopen = 1;
		return tr->fraction;
	}

	int root = hull == 2 ? world.hull2_root : world.hull1_root;
	tr_start = start;
	for (int i = 0; i < 3; i++)
		tr_delta[i] = end[i] - start[i];

	struct { int32_t num, side, f1, fm, f2; } split[MAX_SPLIT];
	int     nsplit = 0;
	int     num = root;
	int32_t f1 = 0, f2 = F_ONE;

	for (;;) {
		if (num < 0) {
			if (num != QW_CONTENTS_SOLID) {
				tr->allsolid = 0;
				if (num == QW_CONTENTS_EMPTY)
					tr->inopen = 1;
				else
					tr->inwater = 1;
			} else {
				tr->startsolid = 1;
			}
			if (!nsplit)
				break;                       /* the whole sweep is open */

			/* the near side of the innermost split was open: its far side */
			nsplit--;
			const qw_clipnode *cn   = &world.clipnodes[split[nsplit].num];
			int                side = split[nsplit].side;
			int32_t            fm   = split[nsplit].fm;
			if (hull_contents_at(cn->child[side ^ 1], fm) != QW_CONTENTS_SOLID) {
				num = cn->child[side ^ 1];
				f1  = fm;
				f2  = split[nsplit].f2;
				continue;
			}
			if (tr->allsolid)
				break;                       /* never got out of solid */

			/* the far side is solid: this split point is the impact */
			const qw_plane *pl = PLANE(cn);
			if (!side) {
				for (int i = 0; i < 3; i++) tr->plane_n[i] = pl->n[i];
				tr->plane_d = pl->dist;
			} else {
				for (int i = 0; i < 3; i++) tr->plane_n[i] = (int16_t) -pl->n[i];
				tr->plane_d = -pl->dist;
			}

			/* back the point out of any residual solid (another plane
			   within a rounding step), 1/10 of the split's sub-sweep at a
			   time like Quake; the sweep's start is the last resort */
			int32_t step = (uint32_t) (split[nsplit].f2 - split[nsplit].f1) / 10 + 1;
			set_endpos(tr, fm);
			for (int guard = 0; hull_contents(root, tr->endpos) == QW_CONTENTS_SOLID; guard++) {
				fm = guard < 16 && fm > step ? fm - step : 0;
				set_endpos(tr, fm);
				if (!fm)
					break;
			}
			tr->fraction = fm >> (F_SHIFT - Q_FIX_SHIFT);
			break;
		}

		const qw_clipnode *cn = &world.clipnodes[num];
		const qw_plane    *pl = PLANE(cn);
		int32_t ds = dot_n(pl, start) - pl->dist;
		int32_t dd = dot_n(pl, tr_delta);
		int32_t t1 = dist_at(ds, dd, f1);
		int32_t t2 = dist_at(ds, dd, f2);

		if (t1 >= 0 && t2 >= 0) {
			num = cn->child[0];
			continue;
		}
		if (t1 < 0 && t2 < 0) {
			num = cn->child[1];
			continue;
		}

		/* the sweep crosses this plane (so dd != 0): split it where it is
		   still DIST_EPS short of the plane, on the near side, and do the
		   near side first */
		int     side = t1 < 0;
		int32_t fm   = split_frac((side ? -DIST_EPS : DIST_EPS) - ds, dd);
		if (fm < f1) fm = f1;
		if (fm > f2) fm = f2;
		if (nsplit == MAX_SPLIT)
			break;                               /* cannot happen (cook checks depth) */
		split[nsplit].num  = num;
		split[nsplit].side = side;
		split[nsplit].f1   = f1;
		split[nsplit].fm   = fm;
		split[nsplit].f2   = f2;
		nsplit++;
		num = cn->child[side];
		f2  = fm;
	}
	return tr->fraction;
}

int clip_contents(int hull, const int32_t p[3])
{
	if (!world.clipnodes)
		return QW_CONTENTS_EMPTY;
	return hull_contents(hull == 2 ? world.hull2_root : world.hull1_root, p);
}
