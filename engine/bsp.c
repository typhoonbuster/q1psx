#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qbsp.h"
#include "bsp.h"

/* ---- point / plane ----------------------------------------------------- */

/* Signed distance of `p` (integer world units) from a cooked plane, in world
   units x4096. Axial planes are exact; non-axial carry the ~1/4096 rounding of
   the cooked normal. */
static int64_t plane_dist(const qbsp_plane *pl, const int32_t p[3])
{
	switch (pl->type) {
	case 0:  return (int64_t) pl->n[0] * p[0] - pl->dist;
	case 1:  return (int64_t) pl->n[1] * p[1] - pl->dist;
	case 2:  return (int64_t) pl->n[2] * p[2] - pl->dist;
	default: return (int64_t) pl->n[0] * p[0]
	              + (int64_t) pl->n[1] * p[1]
	              + (int64_t) pl->n[2] * p[2]
	              - pl->dist;
	}
}

/* 0 = point is on the plane's front (positive) side, 1 = back. */
int bsp_plane_side(const qbsp_plane *pl, const int32_t p[3])
{
	return plane_dist(pl, p) >= 0 ? 0 : 1;
}

int bsp_point_leaf(const bsp_t *b, const int32_t p[3])
{
	int idx = b->root;
	while (idx >= 0) {
		const qbsp_node *nd = &b->nodes[idx];
		idx = nd->child[bsp_plane_side(&b->planes[nd->plane], p)];
	}
	return -idx - 1;
}

/* ---- PVS ------------------------------------------------------------------ */

static uint8_t      pvs_buf[1024];   /* MAX_MAP_LEAFS / 8 */
static int          pvs_leaf = -1;
static const bsp_t *pvs_bsp;

int bsp_pvs_row(const bsp_t *b)
{
	int row = (b->num_visleaves + 7) >> 3;
	return row > (int) sizeof pvs_buf ? (int) sizeof pvs_buf : row;
}

const uint8_t *bsp_pvs(const bsp_t *b, int leaf)
{
	int row = bsp_pvs_row(b);

	if (leaf <= 0) {
		if (pvs_leaf > 0 && pvs_bsp == b)
			return pvs_buf;                 /* keep the last good set */
		memset(pvs_buf, 0xff, sizeof pvs_buf);
		return pvs_buf;
	}
	if (leaf == pvs_leaf && pvs_bsp == b)
		return pvs_buf;

	pvs_leaf = leaf;
	pvs_bsp  = b;

	int32_t visofs = b->leaves[leaf].visofs;
	if (visofs < 0 || (uint32_t) visofs >= b->vis_len) {
		memset(pvs_buf, 0xff, sizeof pvs_buf);
		return pvs_buf;
	}

	/* Quake RLE: a zero byte means "in[1] zero bytes follow". */
	const uint8_t *in   = b->vis + visofs;
	const uint8_t *vend  = b->vis + b->vis_len;
	uint8_t       *out   = pvs_buf;
	uint8_t       *oend  = pvs_buf + row;
	while (out < oend && in < vend) {
		if (*in) {
			*out++ = *in++;
			continue;
		}
		if (in + 1 >= vend)
			break;
		int c = in[1];
		in += 2;
		while (c-- > 0 && out < oend)
			*out++ = 0;
	}
	while (out < oend)
		*out++ = 0;
	return pvs_buf;
}

/* ---- load --------------------------------------------------------------- */

int bsp_load(const void *blob, int len, bsp_t *out)
{
	if (len < (int) sizeof(qbsp_header) || memcmp(blob, QBSP_MAGIC, 4) != 0)
		return 0;

	const qbsp_header *h = (const qbsp_header *) blob;
	const uint8_t     *p = (const uint8_t *) blob + sizeof *h;

	long pb = (long) h->num_planes    * (long) sizeof(qbsp_plane);
	long nb = (long) h->num_nodes     * (long) sizeof(qbsp_node);
	long lb = (long) h->num_leaves    * (long) sizeof(qbsp_leaf);
	long cb = (long) h->num_clipnodes * (long) sizeof(qbsp_clipnode);
	long mb = (long) h->num_marksurf  * 2;
	if ((long) sizeof *h + pb + nb + lb + cb + mb + (long) h->vis_len > len)
		return 0;
	if (h->root < 0 || (uint32_t) h->root >= h->num_nodes)
		return 0;

	out->num_planes    = (int) h->num_planes;
	out->num_nodes     = (int) h->num_nodes;
	out->num_leaves    = (int) h->num_leaves;
	out->num_visleaves = (int) h->num_visleaves;
	out->num_marksurf  = (int) h->num_marksurf;
	out->num_clipnodes = (int) h->num_clipnodes;
	out->root          = (int) h->root;
	out->hull1_root    = (int) h->hull1_root;
	out->hull2_root    = (int) h->hull2_root;
	out->vis_len       = h->vis_len;

	out->planes    = (const qbsp_plane *)    p;  p += pb;
	out->nodes     = (const qbsp_node *)     p;  p += nb;
	out->leaves    = (const qbsp_leaf *)     p;  p += lb;
	out->clipnodes = (const qbsp_clipnode *) p;  p += cb;
	out->marksurf  = (const uint16_t *)      p;  p += mb;
	out->vis       = p;

	memcpy(out->mins, h->mins, sizeof out->mins);
	memcpy(out->maxs, h->maxs, sizeof out->maxs);

	out->has_spawn = (int) h->has_spawn;
	out->spawn[0]  = h->spawn[0];
	out->spawn[1]  = h->spawn[1];
	out->spawn[2]  = h->spawn[2];
	out->spawn_yaw = h->spawn_yaw;

	pvs_leaf = -1;
	pvs_bsp  = NULL;

	printf("bsp_load: %d nodes, %d leaves (%d vis), %d clipnodes, %d marksurf, %u B vis\n",
	       out->num_nodes, out->num_leaves, out->num_visleaves,
	       out->num_clipnodes, out->num_marksurf, out->vis_len);
	if (out->has_spawn)
		printf("bsp_load: spawn at %d %d %d, yaw %d\n",
		       out->spawn[0], out->spawn[1], out->spawn[2], out->spawn_yaw);
	return 1;
}

/* ---- hull trace (SV_RecursiveHullCheck port) ------------------------------ */

#define DIST_EPS  128   /* Quake DIST_EPSILON (1/32 unit) x4096 */

/* lerp helpers on q_fixed frac (0..ONE) */
static int32_t lerp_i(int32_t a, int32_t b, q_fixed frac)
{
	return a + (int32_t) (((int64_t) (b - a) * frac) >> Q_FIX_SHIFT);
}

static const bsp_t *tr_b;
static trace_t     *tr_t;
static int          tr_root;

/* Contents at `p` for the sub-hull rooted at clipnode `num` (num < 0 already
   is the contents). */
static int hull_contents(int num, const int32_t p[3])
{
	while (num >= 0) {
		const qbsp_clipnode *cn = &tr_b->clipnodes[num];
		num = cn->child[plane_dist(&tr_b->planes[cn->plane], p) < 0 ? 1 : 0];
	}
	return num;
}

/* Returns 1 while the sweep is still in open space, 0 once it has been
   resolved (hit something, or wholly solid). */
static int hull_recurse(int num, q_fixed p1f, q_fixed p2f,
                        const int32_t p1[3], const int32_t p2[3])
{
	if (num < 0) {
		if (num != QBSP_CONTENTS_SOLID) {
			tr_t->allsolid = 0;
			if (num == QBSP_CONTENTS_EMPTY)
				tr_t->inopen = 1;
			else
				tr_t->inwater = 1;
		} else {
			tr_t->startsolid = 1;
		}
		return 1;
	}

	const qbsp_clipnode *node = &tr_b->clipnodes[num];
	const qbsp_plane    *pl   = &tr_b->planes[node->plane];

	int64_t t1 = plane_dist(pl, p1);
	int64_t t2 = plane_dist(pl, p2);

	if (t1 >= 0 && t2 >= 0)
		return hull_recurse(node->child[0], p1f, p2f, p1, p2);
	if (t1 < 0 && t2 < 0)
		return hull_recurse(node->child[1], p1f, p2f, p1, p2);

	/* the segment crosses this plane — split it, DIST_EPS on the near side */
	int     side  = (t1 < 0);
	int64_t denom = t1 - t2;                       /* nonzero here */
	int64_t nume  = (int64_t) (t1 < 0 ? t1 + DIST_EPS : t1 - DIST_EPS) << Q_FIX_SHIFT;
	int64_t frac64 = nume / denom;
	if (frac64 < 0)         frac64 = 0;
	if (frac64 > Q_FIX_ONE) frac64 = Q_FIX_ONE;
	q_fixed frac = (q_fixed) frac64;

	q_fixed midf = p1f + (q_fixed) (((int64_t) (p2f - p1f) * frac) >> Q_FIX_SHIFT);
	int32_t mid[3] = {
		lerp_i(p1[0], p2[0], frac),
		lerp_i(p1[1], p2[1], frac),
		lerp_i(p1[2], p2[2], frac),
	};

	/* near side first */
	if (!hull_recurse(node->child[side], p1f, midf, p1, mid))
		return 0;

	if (hull_contents(node->child[side ^ 1], mid) != QBSP_CONTENTS_SOLID)
		return hull_recurse(node->child[side ^ 1], midf, p2f, mid, p2);

	if (tr_t->allsolid)
		return 0;                     /* never got out of solid */

	/* the far side is solid: this split point is the impact */
	if (!side) {
		for (int i = 0; i < 3; i++) tr_t->plane_n[i] = pl->n[i];
		tr_t->plane_d = pl->dist;
	} else {
		for (int i = 0; i < 3; i++) tr_t->plane_n[i] = (int16_t) -pl->n[i];
		tr_t->plane_d = -pl->dist;
	}

	/* back the mid point out of any residual solid (Quake does this too) */
	int guard = 0;
	while (hull_contents(tr_root, mid) == QBSP_CONTENTS_SOLID) {
		frac -= Q_FIX_ONE / 10;
		if (frac < 0 || ++guard > 16) {
			tr_t->fraction = midf;
			for (int i = 0; i < 3; i++) tr_t->endpos[i] = mid[i];
			return 0;
		}
		midf = p1f + (q_fixed) (((int64_t) (p2f - p1f) * frac) >> Q_FIX_SHIFT);
		for (int i = 0; i < 3; i++)
			mid[i] = lerp_i(p1[i], p2[i], frac);
	}

	tr_t->fraction = midf;
	for (int i = 0; i < 3; i++) tr_t->endpos[i] = mid[i];
	return 0;
}

q_fixed bsp_hull_trace(const bsp_t *b, int hull,
                       const int32_t start[3], const int32_t end[3], trace_t *tr)
{
	memset(tr, 0, sizeof *tr);
	tr->fraction = Q_FIX_ONE;
	tr->allsolid = 1;
	for (int i = 0; i < 3; i++)
		tr->endpos[i] = end[i];

	int root = (hull == 2) ? b->hull2_root : b->hull1_root;

	tr_b    = b;
	tr_t    = tr;
	tr_root = root;
	hull_recurse(root, 0, Q_FIX_ONE, start, end);

	if (tr->fraction == Q_FIX_ONE)
		for (int i = 0; i < 3; i++)
			tr->endpos[i] = end[i];

	return tr->fraction;
}
