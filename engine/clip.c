/*
 * q1psx — hull trace, C reference.
 *
 * The PS1 build links the asm port (engine/clip.s); this file is the
 * specification it follows. The bench build links it too (as
 * clip_trace_c / clip_contents_c) to compare both on every call. Keep the arithmetic identical: 32-bit integers, plus
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

/* A node of the hull being traced: hulls 1 and 2 are clipnodes (num = the
   index, < 0 = contents); hull 0, the point hull, is the world's own BSP
   (num = the node's address, a child leaf becomes its contents). The
   planes come from the one table either way. */
typedef intptr_t hnum;

static const qw_plane *plane_of(int hull, hnum num)
{
	uint32_t p3 = hull ? world.clipnodes[num].plane3 : ((const qw_node *) num)->plane3;
	return (const qw_plane *) ((const int32_t *) world.planes + p3);
}

static hnum child_of(int hull, hnum num, int side)
{
	if (hull)
		return world.clipnodes[num].child[side];
	uintptr_t c = ((const qw_node *) num)->child[side];
	if (c & 1)
		return ((const qw_leaf *) (c - 1))->contents;
	return (hnum) c;
}

static hnum root_of(int hull)
{
	if (hull)
		return hull == 2 ? world.hull2_root : world.hull1_root;
#ifdef __mips__
	return (hnum) ((uintptr_t) world.root & 0x00ffffffu);   /* KUSEG, like its children */
#else
	return (hnum) world.root;
#endif
}

static int hull_contents(int hull, hnum num, const int32_t p[3])
{
	while (num >= 0) {
		const qw_plane *pl = plane_of(hull, num);
		num = child_of(hull, num, dot_n(pl, p) - pl->dist < 0 ? 1 : 0);
	}
	return (int) num;
}

/* hull_contents() of the sweep's point at fraction f */
static int hull_contents_at(int hull, hnum num, int32_t f)
{
	while (num >= 0) {
		const qw_plane *pl = plane_of(hull, num);
		int32_t t = dist_at(dot_n(pl, tr_start) - pl->dist, dot_n(pl, tr_delta), f);
		num = child_of(hull, num, t < 0 ? 1 : 0);
	}
	return (int) num;
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
   solid) ends it at once, as Quake's chain of "return false" does. One
   hull from one root: the world's, or a brush model's in its own space. */
static int tr_sight;    /* the sweep a line of sight (clip_mover_t's sight) */

static q_fixed trace_core(int hull, hnum root, const int32_t start[3], const int32_t end[3],
                          trace_t *tr)
{
	memset(tr, 0, sizeof *tr);
	tr->fraction = Q_FIX_ONE;
	tr->allsolid = 1;
	for (int i = 0; i < 3; i++)
		tr->endpos[i] = end[i];

	tr_start = start;
	for (int i = 0; i < 3; i++)
		tr_delta[i] = end[i] - start[i];

	struct { hnum num; int32_t side, f1, fm, f2; } split[MAX_SPLIT];
	int     nsplit = 0;
	hnum    num = root;
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
			hnum    sn   = split[nsplit].num;
			int     side = split[nsplit].side;
			int32_t fm   = split[nsplit].fm;
			if (hull_contents_at(hull, child_of(hull, sn, side ^ 1), fm) != QW_CONTENTS_SOLID) {
				num = child_of(hull, sn, side ^ 1);
				f1  = fm;
				f2  = split[nsplit].f2;
				continue;
			}
			if (tr->allsolid)
				break;                       /* never got out of solid */

			/* the far side is solid: this split point is the impact */
			const qw_plane *pl = plane_of(hull, sn);
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
			if (tr_sight) {                      /* a line of sight: as found */
				tr->fraction = fm >> (F_SHIFT - Q_FIX_SHIFT);
				break;
			}
			for (int guard = 0; hull_contents(hull, root, tr->endpos) == QW_CONTENTS_SOLID; guard++) {
				fm = guard < 16 && fm > step ? fm - step : 0;
				set_endpos(tr, fm);
				if (!fm)
					break;
			}
			tr->fraction = fm >> (F_SHIFT - Q_FIX_SHIFT);
			break;
		}

		const qw_plane *pl = plane_of(hull, num);
		int32_t ds = dot_n(pl, start) - pl->dist;
		int32_t dd = dot_n(pl, tr_delta);
		int32_t t1 = dist_at(ds, dd, f1);
		int32_t t2 = dist_at(ds, dd, f2);

		if (t1 >= 0 && t2 >= 0) {
			num = child_of(hull, num, 0);
			continue;
		}
		if (t1 < 0 && t2 < 0) {
			num = child_of(hull, num, 1);
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
		num = child_of(hull, num, side);
		f2  = fm;
	}
	return tr->fraction;
}

/* the hulls' boxes (SV_MoveBounds takes the mover's mins and maxs: the
   point hull's are 0, the others' Quake's player and large sizes) */
static const int32_t hull_box[3][6] = {
	{ 0, 0, 0, 0, 0, 0 },
	{ -16 * CLIP_SUB, -16 * CLIP_SUB, -24 * CLIP_SUB, 16 * CLIP_SUB, 16 * CLIP_SUB, 32 * CLIP_SUB },
	{ -32 * CLIP_SUB, -32 * CLIP_SUB, -24 * CLIP_SUB, 32 * CLIP_SUB, 32 * CLIP_SUB, 64 * CLIP_SUB },
};

/* SV_InitBoxHull / SV_HullForBox: a box as a 6-node clip hull on axial
   planes (node i on plane i: axis i / 2, the box's max for an even i, its
   min for an odd one; the outer side empty, the inner one the next node,
   the last one solid), the distances set for each box traced */
static const qw_clipnode box_clipnodes[6] = {
	{ 0,  { QW_CONTENTS_EMPTY, 1 } }, { 3,  { 2, QW_CONTENTS_EMPTY } },
	{ 6,  { QW_CONTENTS_EMPTY, 3 } }, { 9,  { 4, QW_CONTENTS_EMPTY } },
	{ 12, { QW_CONTENTS_EMPTY, 5 } }, { 15, { QW_CONTENTS_SOLID, QW_CONTENTS_EMPTY } },
};
static qw_plane box_planes[6] = {
	{ { 4096, 0, 0 }, 0, 0, 0 }, { { 4096, 0, 0 }, 0, 0, 0 },
	{ { 0, 4096, 0 }, 1, 0, 0 }, { { 0, 4096, 0 }, 1, 0, 0 },
	{ { 0, 0, 4096 }, 2, 0, 0 }, { { 0, 0, 4096 }, 2, 0, 0 },
};

clip_mover_t clip_mover;

/* MOVE_MISSILE's mins2 / maxs2 */
static const int32_t missile_box[6] = { -15 * CLIP_SUB, -15 * CLIP_SUB, -15 * CLIP_SUB,
                                         15 * CLIP_SUB,  15 * CLIP_SUB,  15 * CLIP_SUB };

/* SV_Move: the world, then (SV_ClipToLinks) every listed brush model whose
   box the swept box reaches, traced in the model's space (start and end
   less its origin, the hull of the mover's size) and taken when it is hit
   first or the sweep starts inside it; a start inside any of them marks
   the trace. The entries are tried by area node (clip.h): the nodes the
   swept box reaches, in index order (Quake's depth-first walk), each one's
   list in order. The mover's box (`box`: mins[3], maxs[3]) grows the
   boxes, `box2` (MOVE_MISSILE's, else the same) bounds the sweep and grows
   the monsters' boxes; all entries but `pass` (index + 1), the boxes left
   out with `nomonsters`. */
/* a node as the asm walks hold it (clip_box_node0's: bit 0 set, a leaf) */
static hnum node0_hnum(uint32_t node)
{
	if (node & 1)
		return ((const qw_leaf *) (uintptr_t) (node - 1))->contents;
	return (hnum) (uintptr_t) node;
}

static q_fixed trace_move(int hull, const int32_t box[6], const int32_t box2[6], int pass,
                          int nomonsters, uint32_t root0, int sight, const int32_t start[3],
                          const int32_t end[3], trace_t *tr)
{
	tr_sight = sight;
	if (!world.clipnodes || !world.root) {
		memset(tr, 0, sizeof *tr);
		tr->fraction = Q_FIX_ONE;
		tr->inopen = 1;
		for (int i = 0; i < 3; i++)
			tr->endpos[i] = end[i];
		return tr->fraction;
	}
	trace_core(hull, hull == 0 && root0 ? node0_hnum(root0) : root_of(hull), start, end, tr);
	if (!clip_num_bmodels)
		return tr->fraction;

	int32_t bmin[3], bmax[3];                     /* SV_MoveBounds: a unit wider */
	for (int k = 0; k < 3; k++) {
		int32_t lo = start[k] < end[k] ? start[k] : end[k];
		int32_t hi = start[k] < end[k] ? end[k] : start[k];
		bmin[k] = lo + box2[k] - CLIP_SUB;
		bmax[k] = hi + box2[k + 3] + CLIP_SUB;
	}
	uint32_t reach = 1;                           /* the area nodes the box reaches */
	for (int a = 0; a < CLIP_AREAS && (reach >> a); a++) {
		if (!((reach >> a) & 1))
			continue;
		const clip_area_t *an = &clip_areas[a];
		if (!(an->flags & CLIP_AREA_LEAF)) {
			int axis = an->flags & CLIP_AREA_Y;
			if (bmax[axis] > an->dist)
				reach |= 2u << a;                     /* children[0]: the next node */
			if (bmin[axis] < an->dist)
				reach |= 1u << an->child1;
		}
		for (uint16_t link = an->head; link; link = CLIP_ENTRY(link)->next) {
			const clip_bmodel_t *cb = CLIP_ENTRY(link);
			const qw_bmodel     *bm = cb->bm;
			int i = cb->self - 1;
			int k;
			if (i + 1 == pass || ((uintptr_t) bm <= 1 && nomonsters))
				continue;
			const int32_t *lo = bmin, *hi = bmax;     /* (a blocked line of sight: */
			if (sight && tr->fraction < Q_FIX_ONE)    /* the entries holding its start) */
				lo = hi = start;
			for (k = 0; k < 3; k++)
				if (lo[k] > cb->absmax[k] || hi[k] < cb->absmin[k])
					break;
			if (k < 3)
				continue;
			int32_t ls[3], le[3];
			for (k = 0; k < 3; k++) {
				ls[k] = start[k] - cb->origin[k];
				le[k] = end[k] - cb->origin[k];
			}
			trace_t t;
			if ((uintptr_t) bm <= 1) {                /* a SOLID_BBOX entity: its box grown by */
				const int32_t *g = bm ? box2 : box;   /* the mover's (SV_HullForEntity) */
				for (k = 0; k < 3; k++) {
					box_planes[2 * k].dist     = (cb->absmax[k] - cb->origin[k] - g[k]) * 4096;
					box_planes[2 * k + 1].dist = (cb->absmin[k] - cb->origin[k] - g[k + 3]) * 4096;
				}
				const qw_clipnode *wc = world.clipnodes;
				const qw_plane    *wp = world.planes;
				world.clipnodes = box_clipnodes;      /* (the hull's tables, as the asm swaps s1 / s2) */
				world.planes    = box_planes;
				trace_core(1, 0, ls, le, &t);
				world.clipnodes = wc;
				world.planes    = wp;
			} else {
				hnum root = hull == 0 ? (hnum) bm->root : hull == 2 ? bm->hull2 : bm->hull1;
				trace_core(hull, root, ls, le, &t);
			}
			for (k = 0; k < 3; k++)
				t.endpos[k] += cb->origin[k];
			if (t.allsolid || t.startsolid || t.fraction < tr->fraction) {
				int was_startsolid = tr->startsolid;
				t.ent = (int16_t) (i + 1);
				*tr   = t;
				if (was_startsolid)
					tr->startsolid = 1;
			} else if (t.startsolid) {
				tr->startsolid = 1;
			}
		}
	}
	return tr->fraction;
}

q_fixed clip_trace(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr)
{
	return trace_move(hull, hull_box[hull], hull_box[hull], 1, 0, 0, 0, start, end, tr);   /* the player's box passed */
}

q_fixed clip_trace_ent(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr)
{
	uint32_t root0 = clip_mover.root0;            /* (one sweep's) */
	int      sight = clip_mover.sight;
	clip_mover.root0 = 0;
	clip_mover.sight = 0;
	return trace_move(hull, clip_mover.box, clip_mover.missile ? missile_box : clip_mover.box,
	                  clip_mover.pass, clip_mover.nomonsters, root0, sight, start, end, tr);
}

int clip_contents(int hull, const int32_t p[3])
{
	if (!world.clipnodes || !world.root)
		return QW_CONTENTS_EMPTY;
	return hull_contents(hull, root_of(hull), p);
}

uint32_t clip_box_node0(const int32_t mins[3], const int32_t maxs[3])
{
	if (!world.clipnodes || !world.root)
		return 0;
	uintptr_t n = (uintptr_t) root_of(0);
	while (!(n & 1)) {
		const qw_node  *nd = (const qw_node *) n;
		const qw_plane *pl = plane_of(0, (hnum) n);
		int32_t lo, hi;                           /* n.p - dist at the box's extremes */
		if (pl->type < 3) {
			lo = mins[pl->type] * 4096 - pl->dist;
			hi = maxs[pl->type] * 4096 - pl->dist;
		} else {
			lo = hi = -pl->dist;
			for (int k = 0; k < 3; k++) {
				int32_t a = pl->n[k] * mins[k], b = pl->n[k] * maxs[k];
				lo += a < b ? a : b;
				hi += a < b ? b : a;
			}
		}
		if (lo >= 0)
			n = nd->child[0];
		else if (hi < 0)
			n = nd->child[1];
		else
			break;
	}
	return (uint32_t) n;
}

int clip_contents_node0(uint32_t node, const int32_t p[3])
{
	if (!world.clipnodes || !world.root || !node)
		return QW_CONTENTS_EMPTY;
	return hull_contents(0, node0_hnum(node), p);
}
