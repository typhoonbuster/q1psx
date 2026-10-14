#include <stdint.h>
#include <string.h>

#include "clip.h"
#include "world.h"

/* Plane distances are x4096 x CLIP_SUB (positions are 1/CLIP_SUB units):
   |n . p| <= 4096 * 65535 * 3 < 2^30, no overflow for maps within +-8191. */
/* Quake's DIST_EPSILON is 1/32 unit; positions here are quantised to
   1/CLIP_SUB, so the margin is one whole step (4096 in plane-distance
   units): it absorbs the rounding of the next moves along the surface. */
#define DIST_EPS  4096

static int32_t plane_dist(const qw_plane *pl, const int32_t p[3])
{
	int32_t d = pl->dist * CLIP_SUB;
	switch (pl->type) {
	case 0:  return pl->n[0] * p[0] - d;
	case 1:  return pl->n[1] * p[1] - d;
	case 2:  return pl->n[2] * p[2] - d;
	default: return pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - d;
	}
}

/* Points along the sweep are never materialised while recursing: a split
   point rounded to whole sub-units bends the direction of the sub-segment
   after it (a move running along a ramp became one tipped INTO it, and the
   player stuck at the top). Instead every plane distance is interpolated
   from the two endpoints at a 16-bit fraction of the whole sweep. */
#define F_SHIFT 16
#define F_ONE   (1 << F_SHIFT)

static trace_t       *tr_t;
static int            tr_root;
static const int32_t *tr_start, *tr_end;

static int32_t dist_at(int32_t ds, int32_t de, int32_t f)
{
	return ds + (int32_t) (((int64_t) (de - ds) * f) >> F_SHIFT);
}

static int hull_contents(int num, const int32_t p[3])
{
	while (num >= 0) {
		const qw_clipnode *cn = &world.clipnodes[num];
		num = cn->child[plane_dist(&world.planes[cn->plane], p) < 0 ? 1 : 0];
	}
	return num;
}

/* hull_contents() of the sweep's point at fraction f */
static int hull_contents_at(int num, int32_t f)
{
	while (num >= 0) {
		const qw_clipnode *cn = &world.clipnodes[num];
		const qw_plane    *pl = &world.planes[cn->plane];
		int32_t t = dist_at(plane_dist(pl, tr_start), plane_dist(pl, tr_end), f);
		num = cn->child[t < 0 ? 1 : 0];
	}
	return num;
}

/* endpos = the sweep's point at fraction f, each axis rounded toward the
   open side of the plane just hit (toward the start where the normal has no
   component): the impact keeps its full DIST_EPS margin from that plane */
static void set_endpos(int32_t f)
{
	for (int i = 0; i < 3; i++) {
		int64_t d = (int64_t) (tr_end[i] - tr_start[i]) * f;
		int32_t q = (int32_t) (d >> F_SHIFT);                 /* floor */
		int     up = tr_t->plane_n[i] > 0 || (tr_t->plane_n[i] == 0 && d < 0);
		if (up && (d & (F_ONE - 1)))
			q++;                                              /* ceil */
		tr_t->endpos[i] = tr_start[i] + q;
	}
}

/* Returns 1 while the sweep is still in open space, 0 once it has been
   resolved (hit something, or wholly solid). f1/f2: 16-bit fractions. */
static int hull_recurse(int num, int32_t f1, int32_t f2)
{
	if (num < 0) {
		if (num != QW_CONTENTS_SOLID) {
			tr_t->allsolid = 0;
			if (num == QW_CONTENTS_EMPTY)
				tr_t->inopen = 1;
			else
				tr_t->inwater = 1;
		} else {
			tr_t->startsolid = 1;
		}
		return 1;
	}

	const qw_clipnode *node = &world.clipnodes[num];
	const qw_plane    *pl   = &world.planes[node->plane];

	int32_t ds = plane_dist(pl, tr_start);
	int32_t de = plane_dist(pl, tr_end);
	int32_t t1 = dist_at(ds, de, f1);
	int32_t t2 = dist_at(ds, de, f2);

	if (t1 >= 0 && t2 >= 0)
		return hull_recurse(node->child[0], f1, f2);
	if (t1 < 0 && t2 < 0)
		return hull_recurse(node->child[1], f1, f2);

	/* the sweep crosses this plane (so de != ds): split it where it is
	   still DIST_EPS short of the plane, on the near side. The division
	   truncates toward the start. */
	int     side = (t1 < 0);
	int32_t goal = side ? -DIST_EPS : DIST_EPS;
	int64_t fm64 = ((int64_t) (goal - ds) << F_SHIFT) / (de - ds);
	if (fm64 < f1) fm64 = f1;
	if (fm64 > f2) fm64 = f2;
	int32_t fm = (int32_t) fm64;

	/* near side first */
	if (!hull_recurse(node->child[side], f1, fm))
		return 0;

	if (hull_contents_at(node->child[side ^ 1], fm) != QW_CONTENTS_SOLID)
		return hull_recurse(node->child[side ^ 1], fm, f2);

	if (tr_t->allsolid)
		return 0;                     /* never got out of solid */

	/* the far side is solid: this split point is the impact */
	if (!side) {
		for (int i = 0; i < 3; i++) tr_t->plane_n[i] = pl->n[i];
		tr_t->plane_d = pl->dist;              /* x4096, world units */
	} else {
		for (int i = 0; i < 3; i++) tr_t->plane_n[i] = (int16_t) -pl->n[i];
		tr_t->plane_d = -pl->dist;
	}

	/* back the point out of any residual solid (another plane within a
	   rounding step), 1/10 of this sub-segment at a time like Quake; the
	   start of the sweep is the last resort */
	int32_t step = (f2 - f1) / 10 + 1;
	set_endpos(fm);
	for (int guard = 0; hull_contents(tr_root, tr_t->endpos) == QW_CONTENTS_SOLID; guard++) {
		fm = guard < 16 && fm > step ? fm - step : 0;
		set_endpos(fm);
		if (!fm)
			break;
	}

	tr_t->fraction = fm >> (F_SHIFT - Q_FIX_SHIFT);
	return 0;
}

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

	tr_t     = tr;
	tr_root  = hull == 2 ? world.hull2_root : world.hull1_root;
	tr_start = start;
	tr_end   = end;
	hull_recurse(tr_root, 0, F_ONE);

	if (tr->fraction == Q_FIX_ONE)
		for (int i = 0; i < 3; i++)
			tr->endpos[i] = end[i];
	return tr->fraction;
}

int clip_contents(int hull, const int32_t p[3])
{
	if (!world.clipnodes)
		return QW_CONTENTS_EMPTY;
	return hull_contents(hull == 2 ? world.hull2_root : world.hull1_root, p);
}
