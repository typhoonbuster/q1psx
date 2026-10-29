/*
 * qcook — the brush models' room, measured in the cook (docs/porting-notes.md
 * §46): engine/bmfrag.c's cut (cut_model, cut_poly, the top node ent_link
 * gives) run on the cooked world, each model where it stands and a mover at
 * every place along its ways (engine/movers.c's travel: a door's, a
 * button's, a plat's pos1 / pos2, a train's corners, a secret door's
 * stops). The most each model's cuts take (records' bytes, vertices,
 * groups) goes into its qw_bmodel: the engine's room, with no cut at the
 * load.
 *
 * The cut is the engine's, integer for integer: the same plane distances,
 * the same crossings (from the lower end of the edge), the same limits
 * (12 corners a piece, 64 pieces, 24 pending back pieces: a cut past them
 * is given up, the model drawn whole there). The texels and the light the
 * engine carries along are left out: the sizes never depend on them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "qcook.h"
#include "../../formats/qworld.h"

#define CLIP_SUB QW_CLIP_SUB
#define ANG_TURN 4096

/* ------------------------------------------------- engine/render.c trig -- */

static int16_t sintab[1025];

static void build_sintab(void)
{
	for (int a = 0; a <= 1024; a++) {
		uint32_t x    = (uint32_t) (((uint64_t) a * 1686629713u) >> 10);
		uint32_t x2   = (uint32_t) (((uint64_t) x * x) >> 30);
		uint32_t term = x;
		int32_t  sum  = (int32_t) x;
		for (uint32_t k = 1; k <= 6; k++) {
			term = (uint32_t) (((uint64_t) term * x2) >> 30) / ((2 * k) * (2 * k + 1));
			sum += (k & 1) ? -(int32_t) term : (int32_t) term;
		}
		sintab[a] = (int16_t) ((sum + (1 << 17)) >> 18);
	}
}

static int16_t r_sin(int a)
{
	a &= ANG_TURN - 1;
	if (a < 1024) return sintab[a];
	if (a < 2048) return sintab[2048 - a];
	if (a < 3072) return (int16_t) -sintab[a - 2048];
	return (int16_t) -sintab[4096 - a];
}

static int16_t r_cos(int a)
{
	return r_sin(a + 1024);
}

/* --------------------------------------------------------- the world -- */

/* the cooked file's sections (disk links: a child is a node index, or a
   leaf's with bit 31; a node's face blocks are offsets in the stream) */
static struct {
	const qw_header *h;
	const qw_node   *nodes;      /* + the sentinel */
	const qw_leaf   *leaves;
	const int32_t   *planes;     /* the clip section's table, in words */
	const qw_vert   *verts;
	const uint8_t   *faces;
	const qw_ent    *ents;       /* decoded from the file's compact table */
	const char      *strings;
} W;

#define IS_LEAF(at) ((at) & 0x80000000u)

static inline const qw_plane *plane_of(uint16_t plane3)
{
	return (const qw_plane *) (W.planes + plane3);
}

/* engine/bmfrag.c plane_d: the distance of a point, x4096 vertex units */
static inline int32_t plane_d(const qw_plane *pl, const int16_t p[3])
{
	int32_t dist = pl->dist >> 1;
	return pl->type < 3 ? p[pl->type] * 4096 - dist
	                    : pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - dist;
}

/* engine/world.c world_box_topnode (world units, inclusive) */
static uint32_t box_topnode(const int32_t mins[3], const int32_t maxs[3])
{
	int32_t lo[3], hi[3];
	for (int k = 0; k < 3; k++) {
		lo[k] = mins[k] * QW_VSCALE;
		hi[k] = maxs[k] * QW_VSCALE;
	}
	uint32_t at = W.h->root;
	while (!IS_LEAF(at)) {
		const qw_node *n = &W.nodes[at];
		const qw_plane *pl = plane_of(n->plane3);
		int32_t dist = pl->dist >> 1;
		int32_t dmin, dmax;
		if (pl->type < 3) {
			dmin = lo[pl->type] * 4096 - dist;
			dmax = hi[pl->type] * 4096 - dist;
		} else {
			dmin = dmax = -dist;
			for (int k = 0; k < 3; k++) {
				int32_t a = pl->n[k] * lo[k], b = pl->n[k] * hi[k];
				dmin += a < b ? a : b;
				dmax += a < b ? b : a;
			}
		}
		if (dmin > 0)
			at = n->child[0];
		else if (dmax <= 0)
			at = n->child[1];
		else
			break;
	}
	return at;
}

/* ------------------------------------------- the cut (engine/bmfrag.c) -- */

#define BMF_CORNERS 12
#define BMF_PIECES  64
#define BMF_STACK   24
#define BMF_PAGE    64           /* the engine's cut arena's page (engine/bmfrag.c): a
                                    model's room there is whole pages */

typedef struct { int16_t p[3]; } bmf_v;

typedef struct {
	uint32_t leaf;
	uint8_t  n, cut, quad;
	bmf_v    v[BMF_CORNERS];
} bmf_piece;

/* what a cut takes: the cache's lengths (the records' bytes, the pool's
   vertices, the groups) and the last group, for put_group's merge */
typedef struct {
	uint32_t recs, verts, groups;
	uint32_t side_first, last_leaf, last_end;
} bmf_use;

static bmf_piece pieces[BMF_PIECES];
static int       num_pieces, cut_overflow;

static struct {
	uint32_t at;
	uint8_t  n;
	bmf_v    v[BMF_CORNERS];
} pending[BMF_STACK];

static void cross(bmf_v *o, const bmf_v *a, const bmf_v *b, int32_t da, int32_t db)
{
	if (a->p[0] > b->p[0] || (a->p[0] == b->p[0] && (a->p[1] > b->p[1] ||
	    (a->p[1] == b->p[1] && a->p[2] > b->p[2])))) {
		const bmf_v *t = a; a = b; b = t;
		int32_t td = da; da = db; db = td;
	}
	int32_t num = da, den = da - db, t;
	if (den < 0) {
		num = -num;
		den = -den;
	}
	while (den >= 1 << 19) {
		num >>= 1;
		den >>= 1;
	}
	t = den ? (num << 12) / den : 2048;
	if (t > 4096) t = 4096;
	for (int k = 0; k < 3; k++)
		o->p[k] = (int16_t) (a->p[k] + ((((int32_t) b->p[k] - a->p[k]) * t + 2048) >> 12));
}

static void cut_poly(uint32_t top, const bmf_v *in, int n, int quad)
{
	bmf_v cur[BMF_CORNERS], side[2][BMF_CORNERS];
	int32_t d[BMF_CORNERS];
	int sp = 0, cutting = 0;
	uint32_t at = top;
	memcpy(cur, in, (size_t) n * sizeof *cur);
	for (;;) {
		while (!IS_LEAF(at)) {
			const qw_node *nd = &W.nodes[at];
			const qw_plane *pl = plane_of(nd->plane3);
			int front = 0, back = 0;
			for (int i = 0; i < n; i++) {
				d[i] = plane_d(pl, cur[i].p);
				front |= d[i] > 0;
				back  |= d[i] < 0;
			}
			if (!back) { at = nd->child[!front]; continue; }
			if (!front) { at = nd->child[1]; continue; }
			int nf = 0, nb = 0;
			for (int i = 0; i < n; i++) {
				int h = i ? i - 1 : n - 1, j = i + 1 < n ? i + 1 : 0;
				if (nf > BMF_CORNERS - 2 || nb > BMF_CORNERS - 2) { cut_overflow = 1; return; }
				if (d[i] > 0 || (!d[i] && (d[h] > 0 || d[j] > 0))) side[0][nf++] = cur[i];
				if (d[i] < 0 || (!d[i] && (d[h] < 0 || d[j] < 0))) side[1][nb++] = cur[i];
				if ((d[i] > 0 && d[j] < 0) || (d[i] < 0 && d[j] > 0)) {
					cross(&side[0][nf], &cur[i], &cur[j], d[i], d[j]);
					side[1][nb++] = side[0][nf++];
				}
			}
			if (sp >= BMF_STACK) { cut_overflow = 1; return; }
			pending[sp].at = nd->child[1];
			pending[sp].n  = (uint8_t) nb;
			memcpy(pending[sp].v, side[1], (size_t) nb * sizeof *cur);
			sp++;
			memcpy(cur, side[0], (size_t) nf * sizeof *cur);
			n = nf;
			at = nd->child[0];
			cutting = 1;
		}
		uint32_t leaf = at & 0x7fffffffu;
		if (W.leaves[leaf].contents != QW_CONTENTS_SOLID) {
			if (num_pieces >= BMF_PIECES) { cut_overflow = 1; return; }
			bmf_piece *pc = &pieces[num_pieces++];
			pc->leaf = leaf;
			pc->n    = (uint8_t) n;
			pc->cut  = (uint8_t) cutting;
			pc->quad = (uint8_t) quad;
			memcpy(pc->v, cur, (size_t) n * sizeof *cur);
		}
		if (!sp)
			return;
		sp--;
		at = pending[sp].at;
		n  = pending[sp].n;
		memcpy(cur, pending[sp].v, (size_t) n * sizeof *cur);
		cutting = 1;
	}
}

/* put_record's size: the record of `count` pieces */
static void put_record(bmf_use *c, const bmf_piece *const *pc, int count)
{
	int nv = 0, nq = 0, nt = 0;
	for (int i = 0; i < count; i++) {
		nv += pc[i]->n;
		if (!pc[i]->cut) {
			if (pc[i]->quad) nq++; else nt++;
		} else {
			nq += (pc[i]->n - 2) / 2;
			nt += (pc[i]->n - 2) & 1;
		}
	}
	int nidx   = (nv + 2) / 3 * 3;
	int ibytes = (nidx * 2 + 3) & ~3;
	int cbytes = (nv + 3) & ~3;
	uint32_t polys = (uint32_t) (sizeof(qw_record) + ibytes + cbytes);
	c->recs  += polys + (uint32_t) (nq + nt) * 12;
	c->verts += (uint32_t) nv;
}

/* the leaves the model being measured puts pieces in, over every place of
   its travel (a bit a leaf: the cut arena's sums, bmf_arena) */
static uint8_t *leafset;

static void put_group(bmf_use *c, uint32_t leaf, uint32_t ofs, uint32_t len)
{
	if (leafset)
		leafset[leaf >> 3] |= (uint8_t) (1u << (leaf & 7));
	if (c->groups > c->side_first && c->last_leaf == leaf && c->last_end == ofs) {
		c->last_end += len;
		return;
	}
	c->groups++;
	c->last_leaf = leaf;
	c->last_end  = ofs + len;
}

static void put_pieces(bmf_use *c)
{
	uint8_t done[BMF_PIECES];
	memset(done, 0, (size_t) num_pieces);
	for (int i = 0; i < num_pieces; i++) {
		if (done[i])
			continue;
		uint32_t leaf = pieces[i].leaf;
		const bmf_piece *batch[BMF_PIECES];
		int nb = 0, nvb = 0;
		uint32_t start = c->recs;
		for (int j = i; j < num_pieces; j++) {
			if (done[j] || pieces[j].leaf != leaf)
				continue;
			if (nvb + pieces[j].n > QW_MAX_VERTS) {
				put_record(c, batch, nb);
				nb = nvb = 0;
			}
			batch[nb++] = &pieces[j];
			nvb += pieces[j].n;
			done[j] = 1;
		}
		if (nb)
			put_record(c, batch, nb);
		put_group(c, leaf, start, c->recs - start);
	}
	num_pieces = 0;
}

static int cut_record(bmf_use *c, const uint8_t *rp, const int16_t off[3], uint32_t top)
{
	const qw_record *r = (const qw_record *) rp;
	const uint16_t *idx = (const uint16_t *) (rp + sizeof *r);
	const qw_quad *q = (const qw_quad *) (rp + r->polys);
	const qw_tri *t = (const qw_tri *) (q + r->num_quads);
	num_pieces = 0;
	cut_overflow = 0;
	for (int i = 0; i < r->num_quads + r->num_tris; i++) {
		if (num_pieces > BMF_PIECES / 2)
			put_pieces(c);
		int quad = i < r->num_quads, n = quad ? 4 : 3;
		bmf_v v[4];
		for (int j = 0; j < n; j++) {
			static const uint8_t outline[4] = { 0, 1, 3, 2 };
			int cj = quad ? outline[j] : j;
			int lv = (quad ? q[i].idx[cj] : t[i - r->num_quads].idx[cj]) >> 2;
			const qw_vert *pv = &W.verts[idx[lv]];
			v[j].p[0] = (int16_t) (pv->x + off[0]);
			v[j].p[1] = (int16_t) (pv->y + off[1]);
			v[j].p[2] = (int16_t) (pv->z + off[2]);
		}
		cut_poly(top, v, n, quad);
		if (cut_overflow)
			return 0;
	}
	put_pieces(c);
	return 1;
}

/* brush model `bm` cut at origin `org` (1/8 units) from world node `top`:
   1 and what it takes, 0 given up (too cut up there) */
static int cut_model(bmf_use *c, const qw_bmodel *bm, const int32_t org[3], uint32_t top)
{
	int16_t off[3];
	for (int k = 0; k < 3; k++)
		off[k] = (int16_t) (org[k] * QW_VSCALE / CLIP_SUB);
	memset(c, 0, sizeof *c);
	uint32_t stack[QW_MAX_BM_DEPTH + 2];
	int sp = 0;
	if (IS_LEAF(bm->root))
		return 1;
	stack[sp++] = bm->root;
	while (sp) {
		const qw_node *nd = &W.nodes[stack[--sp]];
		for (int s = 0; s < 2; s++) {
			c->side_first = c->groups;
			const uint8_t *p = W.faces + nd->faces[s], *end = W.faces + qw_face_end(nd, s);
			while (p < end) {
				const qw_record *r = (const qw_record *) p;
				if (!r->size)
					break;
				if (!cut_record(c, p, off, top))
					return 0;
				p += r->size;
			}
		}
		if (!IS_LEAF(nd->child[1])) {
			if (sp >= QW_MAX_BM_DEPTH + 2) return 0;
			stack[sp++] = nd->child[1];
		}
		if (!IS_LEAF(nd->child[0])) {
			if (sp >= QW_MAX_BM_DEPTH + 2) return 0;
			stack[sp++] = nd->child[0];
		}
	}
	return 1;
}

/* engine/bmfrag.c cut_where: the model cut as if its edict stood at `org`,
   from the top node ent_link gives its box there */
static int cut_where(bmf_use *c, const qw_bmodel *bm, const int32_t org[3])
{
	int32_t lo[3], hi[3];
	for (int k = 0; k < 3; k++) {
		lo[k] = ((org[k] + bm->mins[k] - CLIP_SUB) >> 3) - 2;
		hi[k] = ((org[k] + bm->maxs[k] + CLIP_SUB) >> 3) + 3;
	}
	return cut_model(c, bm, org, box_topnode(lo, hi));
}

/* a model's records (none: a trigger's, nothing drawn) */
static int has_records(uint32_t at)
{
	if (IS_LEAF(at))
		return 0;
	const qw_node *nd = &W.nodes[at];
	for (int s = 0; s < 2; s++)
		if (qw_face_end(nd, s) > nd->faces[s] && ((const qw_record *) (W.faces + nd->faces[s]))->size)
			return 1;
	return has_records(nd->child[0]) || has_records(nd->child[1]);
}

/* ------------------------------------------- the travel (engine/movers.c) -- */

#define DOOR_START_OPEN  1
#define SECRET_1ST_LEFT  2
#define SECRET_1ST_DOWN  4
#define MAX_PLACES       1024     /* a train's corners, at most */

/* ents.c set_movedir: the map's angle, -1 up, -2 down */
static void set_movedir(int angle, int32_t movedir[3])
{
	if (angle == -1) {
		movedir[0] = 0; movedir[1] = 0; movedir[2] = 4096;
	} else if (angle == -2) {
		movedir[0] = 0; movedir[1] = 0; movedir[2] = -4096;
	} else {
		int a = angle * ANG_TURN / 360;
		movedir[0] = r_cos(a);
		movedir[1] = r_sin(a);
		movedir[2] = 0;
	}
}

/* travel_end: pos1 + movedir x (|movedir . size| - lip) */
static void travel_end(const int32_t movedir[3], const qw_bmodel *bm, int lip, const int32_t pos1[3], int32_t pos2[3])
{
	int64_t d = 0;
	for (int k = 0; k < 3; k++)
		d += (int64_t) movedir[k] * (bm->maxs[k] - bm->mins[k]);
	if (d < 0)
		d = -d;
	int32_t dist = (int32_t) (d / 4096) - lip * CLIP_SUB;
	for (int k = 0; k < 3; k++)
		pos2[k] = pos1[k] + (int32_t) ((int64_t) movedir[k] * dist / 4096);
}

static int32_t fabs_dot_size(const int32_t v[3], const qw_bmodel *bm)
{
	int64_t d = 0;
	for (int k = 0; k < 3; k++)
		d += (int64_t) v[k] * (bm->maxs[k] - bm->mins[k]);
	return (int32_t) ((d < 0 ? -d : d) / 4096);
}

/* secret_dests: fd_secret_use's two moves' ends (t_width, t_length 0: the
   cooked entities have no such keys) */
static void secret_dests(const qw_ent *q, const qw_bmodel *bm, int32_t dest1[3], int32_t dest2[3])
{
	int temp = q->spawnflags & SECRET_1ST_LEFT ? -1 : 1;
	int y = q->angle * ANG_TURN / 360;            /* mangle = (0, angle, 0) */
	int32_t sy = r_sin(y), cy = r_cos(y);
	int32_t f[3] = { cy, sy, 0 }, r[3] = { sy, -cy, 0 }, u[3] = { 0, 0, 4096 };
	int32_t width  = fabs_dot_size(q->spawnflags & SECRET_1ST_DOWN ? u : r, bm);
	int32_t length = fabs_dot_size(f, bm);
	for (int k = 0; k < 3; k++) {
		if (q->spawnflags & SECRET_1ST_DOWN)
			dest1[k] = q->origin[k] - (int32_t) ((int64_t) u[k] * width / 4096);
		else
			dest1[k] = q->origin[k] + (int32_t) ((int64_t) r[k] * width * temp / 4096);
		dest2[k] = dest1[k] + (int32_t) ((int64_t) f[k] * length / 4096);
	}
}

/* ED_LoadFromFile at skill s: the entity spawned (not easy 256, not
   medium 512, not hard 1024); an unknown class gets no edict */
static int spawned(const qw_ent *q, int skill)
{
	if ((skill == 0 && (q->spawnflags & 256)) || (skill == 1 && (q->spawnflags & 512)) ||
	    (skill >= 2 && (q->spawnflags & 1024)))
		return 0;
	return q->class != QE_UNKNOWN;
}

/* ent_find(0, name) at skill s: the first edict of that targetname (the
   edicts in the table's order); -1 none */
static int find_target(uint16_t name, int skill)
{
	if (!name)
		return -1;
	for (uint32_t i = 0; i < W.h->num_ents; i++)
		if (W.ents[i].targetname == name && spawned(&W.ents[i], skill))
			return (int) i;
	return -1;
}

/* movers_path for a train at skill s: its corners in its path's order
   (func_train_find's and train_next's place: the corner's origin less the
   model's mins), up to the first corner it comes back to (that one again);
   their count, -1 for more than MAX_PLACES - 1 */
static int train_path(const qw_ent *q, const qw_bmodel *bm, int skill, int32_t (*out)[3])
{
	static int seen[MAX_PLACES];
	int n = 0;
	for (uint16_t name = q->target; name; ) {
		int ti = find_target(name, skill);
		if (ti < 0)
			break;
		int back = 0;
		for (int j = 0; j < n && !back; j++)
			back = seen[j] == ti;
		if (!back && n == MAX_PLACES - 1)
			return -1;
		for (int k = 0; k < 3; k++)
			out[n][k] = W.ents[ti].origin[k] - bm->mins[k];
		if (back)
			return n + 1;
		seen[n++] = ti;
		name = W.ents[ti].target;
	}
	return n;
}

/* --------------------------------------------------------- the needs -- */

typedef struct {
	uint32_t recs, verts, groups;
} bmf_need;

static void most(bmf_need *m, const bmf_use *c)
{
	if (c->recs > m->recs)     m->recs   = c->recs;
	if (c->verts > m->verts)   m->verts  = c->verts;
	if (c->groups > m->groups) m->groups = c->groups;
}

/* the cuts made, given up, and their time (the cook's log, qcook bmf) */
static struct {
	int      verbose;
	uint32_t cuts, given_up;
	double   secs;
} R;

/* the cut at one place, into the most so far (a cut given up: the model
   drawn whole there, no room needed) */
/* engine/world.c box_leaves: the leaves a box touches, in the BSP's order,
   at most `max` (the engine keeps ENT_MAX_LEAVES, 8); vertex units */
#define ENT_MAX_LEAVES 8
static int box_leaves(uint32_t at, const int32_t c[3], const int32_t h[3], uint32_t *out, int n, int max)
{
	for (;;) {
		if (IS_LEAF(at)) {
			uint32_t l = at & 0x7fffffffu;
			if (W.leaves[l].contents != QW_CONTENTS_SOLID && n < max)
				out[n++] = l;
			return n;
		}
		const qw_node  *nd = &W.nodes[at];
		const qw_plane *pl = plane_of(nd->plane3);
		int32_t dist = pl->dist >> 1;
		int32_t d, ext;
		if (pl->type < 3) {
			d   = c[pl->type] * 4096 - dist;
			ext = h[pl->type] * 4096;
		} else {
			d   = pl->n[0] * c[0] + pl->n[1] * c[1] + pl->n[2] * c[2] - dist;
			ext = (pl->n[0] < 0 ? -pl->n[0] : pl->n[0]) * h[0] + (pl->n[1] < 0 ? -pl->n[1] : pl->n[1]) * h[1] +
			      (pl->n[2] < 0 ? -pl->n[2] : pl->n[2]) * h[2];
		}
		if (d - ext > 0)
			at = nd->child[0];
		else if (d + ext < 0)
			at = nd->child[1];
		else {
			n  = box_leaves(nd->child[0], c, h, out, n, max);
			at = nd->child[1];
		}
	}
}

/* every leaf under a node (the engine's top-node test: the PVS marking
   stamps a visible leaf's ancestors) */
static void subtree_leaves(uint32_t at, uint8_t *set)
{
	while (!IS_LEAF(at)) {
		subtree_leaves(W.nodes[at].child[1], set);
		at = W.nodes[at].child[0];
	}
	uint32_t l = at & 0x7fffffffu;
	set[l >> 3] |= (uint8_t) (1u << (l & 7));
}

/* The leaves the engine draws the model from when it has no pieces
   (ents.c ents_visible, bmf_seen's `whole`): its box's leaves (ent_link:
   the box a unit wider, world_box_leaves's rounding), or, when they are
   ENT_MAX_LEAVES or more, every leaf under its top node. Into the
   model's set, at this place. */
static void box_leaves_into(const qw_bmodel *bm, const int32_t org[3])
{
	if (!leafset)
		return;
	int32_t amin[3], amax[3], c[3], h[3], lo[3], hi[3];
	for (int k = 0; k < 3; k++) {
		amin[k] = org[k] + bm->mins[k] - CLIP_SUB;
		amax[k] = org[k] + bm->maxs[k] + CLIP_SUB;
		int32_t l = amin[k] >> 1, u = (amax[k] + 1) >> 1;
		c[k] = (l + u) >> 1;
		h[k] = ((u - l) >> 1) + 1;
		lo[k] = (amin[k] >> 3) - 2;
		hi[k] = (amax[k] >> 3) + 3;
	}
	uint32_t out[ENT_MAX_LEAVES];
	int n = box_leaves(W.h->root, c, h, out, 0, ENT_MAX_LEAVES);
	if (n >= ENT_MAX_LEAVES) {
		uint32_t top = box_topnode(lo, hi);
		if (IS_LEAF(top)) {
			uint32_t l = top & 0x7fffffffu;
			leafset[l >> 3] |= (uint8_t) (1u << (l & 7));
		} else {
			subtree_leaves(top, leafset);
		}
		return;
	}
	for (int i = 0; i < n; i++)
		leafset[out[i] >> 3] |= (uint8_t) (1u << (out[i] & 7));
}

static void cut_into(const qw_bmodel *bm, const int32_t org[3], bmf_need *m)
{
	bmf_use c;
	R.cuts++;
	box_leaves_into(bm, org);
	if (!cut_where(&c, bm, org)) {
		R.given_up++;
		return;
	}
	most(m, &c);
}

/* Every place a way from a to b goes through, in the engine's units (1/8
   world unit). SUB_CalcMove puts a mover at a + delta x t / ticks each tick
   (engine/ents.c, truncated: the way back from b rounds the other way), so
   whatever the speed or the tick rate, its longest axis takes each value
   from a to b, and on a slanted way another axis takes, at that value, the
   point of the line truncated or the next unit towards b: each of these is
   cut. */
static void cut_way(const qw_bmodel *bm, const int32_t a[3], const int32_t b[3], bmf_need *m)
{
	int32_t d[3], len = 0;
	for (int k = 0; k < 3; k++) {
		d[k] = b[k] - a[k];
		int32_t ad = d[k] < 0 ? -d[k] : d[k];
		if (ad > len)
			len = ad;
	}
	for (int32_t s = 0; s <= len; s++) {
		int32_t org[3];
		int odd = 0;                                  /* the axes between two units there */
		for (int k = 0; k < 3; k++) {
			org[k] = a[k] + (len ? (int32_t) ((int64_t) d[k] * s / len) : 0);
			if (len && (int64_t) d[k] * s % len)
				odd |= 1 << k;
		}
		for (int v = 0; v < 8; v++) {
			if (v & ~odd)
				continue;
			int32_t at[3];
			for (int k = 0; k < 3; k++)
				at[k] = org[k] + (v >> k & 1 ? (d[k] > 0 ? 1 : -1) : 0);
			cut_into(bm, at, m);
		}
	}
}

static const char *class_name(int cls)
{
	switch (cls) {
	case QE_FUNC_DOOR:        return "door";
	case QE_FUNC_DOOR_SECRET: return "secret door";
	case QE_FUNC_BUTTON:      return "button";
	case QE_FUNC_PLAT:        return "plat";
	case QE_FUNC_TRAIN:       return "train";
	default:                  return "still";
	}
}

/* entity q's model, every place it can take: its needs */
static void model_needs(const qw_ent *q, const qw_bmodel *bm, bmf_need *need, bmf_need *engine)
{
	static int32_t path[MAX_PLACES][3];
	memset(need, 0, sizeof *need);
	memset(engine, 0, sizeof *engine);
	int32_t org[3];
	memcpy(org, q->origin, sizeof org);
	int np = 0;
	switch (q->class) {
	case QE_FUNC_DOOR: case QE_FUNC_BUTTON: {
		int32_t movedir[3];
		int lip = q->lip ? q->lip : q->class == QE_FUNC_DOOR ? 8 : 4;
		set_movedir(q->angle, movedir);
		memcpy(path[0], q->origin, sizeof path[0]);
		travel_end(movedir, bm, lip, path[0], path[1]);
		if (q->class == QE_FUNC_DOOR && (q->spawnflags & DOOR_START_OPEN)) {
			memcpy(org, path[1], sizeof org);         /* spawned open: pos1 and pos2 swapped */
			memcpy(path[1], path[0], sizeof path[1]);
			memcpy(path[0], org, sizeof path[0]);
		}
		np = 2;
		break;
	}
	case QE_FUNC_PLAT:
		memcpy(path[0], q->origin, sizeof path[0]);
		memcpy(path[1], q->origin, sizeof path[1]);
		path[1][2] = q->height ? q->origin[2] - q->height * CLIP_SUB
		                       : q->origin[2] - (bm->maxs[2] - bm->mins[2]) + 8 * CLIP_SUB;
		if (!q->targetname)
			memcpy(org, path[1], sizeof org);         /* spawned at the bottom */
		np = 2;
		break;
	case QE_FUNC_DOOR_SECRET:
		memcpy(path[0], q->origin, sizeof path[0]);
		secret_dests(q, bm, path[1], path[2]);
		np = 3;
		break;
	default:
		break;
	}
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	uint32_t cuts0 = R.cuts, up0 = R.given_up;
	cut_into(bm, org, need);                          /* where it spawns */
	if (q->class == QE_FUNC_TRAIN) {
		/* its path at each skill it spawns at (a corner hanging on the
		   skill changes it) */
		static int32_t prev[MAX_PLACES][3];
		int nprev = -1;
		for (int skill = 0; skill <= 2; skill++) {
			if (!spawned(q, skill))
				continue;
			int n = train_path(q, bm, skill, path);
			if (n < 0)
				die("train *%d: a path of more than %d corners", q->model, MAX_PLACES - 1);
			if (n == nprev && !memcmp(prev, path, (size_t) n * sizeof path[0]))
				continue;
			for (int w = 0; w + 1 < n; w++)
				cut_way(bm, path[w], path[w + 1], need);
			if (n == 1)
				cut_into(bm, path[0], need);
			memcpy(prev, path, (size_t) n * sizeof path[0]);
			nprev = n;
			np = n;
		}
	} else {
		for (int w = 0; w + 1 < np; w++)
			cut_way(bm, path[w], path[w + 1], need);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double secs = (double) (t1.tv_sec - t0.tv_sec) + (double) (t1.tv_nsec - t0.tv_nsec) * 1e-9;
	R.secs += secs;

	/* the engine's measure before (§44), for the report: where it spawns,
	   the path's places and halfway between them, twice the most for a
	   mover */
	if (R.verbose) {
		uint32_t keep_cuts = R.cuts, keep_up = R.given_up;
		bmf_need e = { 0, 0, 0 };
		int off = np && memcmp(org, path[0], sizeof org) && memcmp(org, path[np - 1], sizeof org);
		if (!np || off)
			cut_into(bm, org, &e);
		for (int p = 0; p < np; p++)
			for (int half = 0; half < (p + 1 < np ? 2 : 1); half++) {
				int32_t at[3];
				for (int k = 0; k < 3; k++)
					at[k] = path[p][k] + (half ? (path[p + 1][k] - path[p][k]) / 2 : 0);
				cut_into(bm, at, &e);
			}
		*engine = e;
		R.cuts = keep_cuts;
		R.given_up = keep_up;
		printf("bmf *%d %s: %d places, %u cuts (%u given up), %.3f s; most: %u B, %u verts, %u groups;"
		       " the engine's places: %u B, %u verts, %u groups (x%.2f %.2f %.2f)\n",
		       q->model, class_name(q->class), np, R.cuts - cuts0, R.given_up - up0, secs,
		       need->recs, need->verts, need->groups, e.recs, e.verts, e.groups,
		       e.recs ? (double) need->recs / e.recs : 0, e.verts ? (double) need->verts / e.verts : 0,
		       e.groups ? (double) need->groups / e.groups : 0);
	}
}

/* the classes whose edict draws its brush model (draw = 1: engine/movers.c,
   triggers.c brush_static): those the engine cuts */
static int drawn_class(int cls)
{
	switch (cls) {
	case QE_FUNC_DOOR: case QE_FUNC_DOOR_SECRET: case QE_FUNC_BUTTON: case QE_FUNC_PLAT:
	case QE_FUNC_TRAIN: case QE_FUNC_WALL: case QE_FUNC_ILLUSIONARY: case QE_FUNC_EPISODEGATE:
	case QE_FUNC_BOSSGATE:
		return 1;
	}
	return 0;
}

/* the cooked world in `f` (len bytes), as the engine reads it */
static qw_bmodel *world_view(uint8_t *f, size_t len)
{
	if (len < sizeof(qw_header) || memcmp(f, QWORLD_MAGIC, 4))
		die("bmf: not a %s world file", QWORLD_MAGIC);
	W.h       = (const qw_header *) f;
	W.nodes   = (const qw_node *) (f + W.h->ofs_nodes);
	W.leaves  = (const qw_leaf *) (f + W.h->ofs_leaves);
	W.planes  = (const int32_t *) (f + W.h->ofs_clip + sizeof(qw_clip_header));
	W.verts   = (const qw_vert *) (f + W.h->ofs_verts);
	W.faces   = f + W.h->ofs_faces;
	W.strings = (const char *) (f + W.h->ofs_strings);
	const uint16_t *names = (const uint16_t *) (f + W.h->ofs_ents);
	const uint8_t *p = (const uint8_t *) (names + QE_COUNT);
	qw_ent *ents = xmalloc((W.h->num_ents + 1) * sizeof *ents);
	for (uint32_t i = 0; i < W.h->num_ents; i++)
		p = qw_ent_decode(p, names, &ents[i]);
	free((void *) W.ents);
	W.ents = ents;
	build_sintab();
	return (qw_bmodel *) (f + W.h->ofs_bmodels);
}

/* each drawn brush model's room: the most of its entities' cuts (one
   entity a model in the id maps) */
static uint8_t *leafsets;         /* [num_bmodels][leafbytes]: each model's (rooms_of) */
static size_t   leafbytes;

static bmf_need *rooms_of(const qw_bmodel *bms)
{
	bmf_need *room = xcalloc(W.h->num_bmodels, sizeof *room);
	leafbytes = ((size_t) W.h->num_leaves + 7) / 8;
	free(leafsets);
	leafsets = xcalloc(W.h->num_bmodels, leafbytes);
	for (uint32_t i = 0; i < W.h->num_ents; i++) {
		const qw_ent *q = &W.ents[i];
		if (!drawn_class(q->class) || q->model <= 0 || (uint32_t) q->model >= W.h->num_bmodels ||
		    !has_records(bms[q->model].root))
			continue;
		bmf_need need, engine;
		leafset = leafsets + (size_t) q->model * leafbytes;
		model_needs(q, &bms[q->model], &need, &engine);
		leafset = 0;
		bmf_need *r = &room[q->model];
		if (need.recs > r->recs)     r->recs   = need.recs;
		if (need.verts > r->verts)   r->verts  = need.verts;
		if (need.groups > r->groups) r->groups = need.groups;
	}
	return room;
}

/* The cut arena (engine/bmfrag.c, docs/porting-notes.md §90.6): the most
   the pieces of the models visible at once take. For every leaf with a
   PVS row (the world's, not solid: the camera in a wall sees everything
   and draws the models whole there), the models whose pieces lie, at any
   place of their travel, in a leaf the row sees; their rooms summed
   (records and groups in bytes, vertices, each model's in whole pages, as
   the engine gives it); the largest sum, plus the largest single room
   (one more model always fits). Written in the header; a map without a
   row (no vis) takes every model's room. */
static void arena_of(const bmf_need *room, uint32_t *recs_out, uint32_t *verts_out, uint32_t *strict_recs,
                     uint32_t *strict_verts, uint32_t *total_recs, uint32_t *total_verts)
{
	uint32_t nb = W.h->num_bmodels, nl = W.h->num_leaves, nv = W.h->num_visleafs;
	uint32_t rowbytes = (nv + 7) / 8;
	uint8_t *row = xmalloc(rowbytes + 1), *rowset = xmalloc(leafbytes);
	const uint8_t *vis = (const uint8_t *) W.h + W.h->ofs_vis;
	uint32_t best_r = 0, best_v = 0, big_r = 0, big_v = 0, tot_r = 0, tot_v = 0, rows = 0;
	uint32_t *mrecs = xcalloc(nb, sizeof *mrecs), *mverts = xcalloc(nb, sizeof *mverts);
	for (uint32_t m = 1; m < nb; m++) {
		if (!room[m].verts)
			continue;
		/* (bmf_group: 12 bytes; qw_vert: 8) */
		mrecs[m]  = (room[m].recs + room[m].groups * 12u + BMF_PAGE - 1) / BMF_PAGE * BMF_PAGE;
		mverts[m] = (room[m].verts * 8u + BMF_PAGE - 1) / BMF_PAGE * (BMF_PAGE / 8);
		tot_r += mrecs[m];
		tot_v += mverts[m];
		if (mrecs[m] > big_r) big_r = mrecs[m];
		if (mverts[m] > big_v) big_v = mverts[m];
	}
	for (uint32_t l = 1; l <= nv && l < nl; l++) {
		const qw_leaf *lf = &W.leaves[l];
		if (lf->vis == QW_NONE || lf->contents == QW_CONTENTS_SOLID)
			continue;
		qw_vis_decode(vis, W.leaves, l, row, rowbytes);
		rows++;
		memset(rowset, 0, leafbytes);                        /* the row's bit l - 1 as leaf l's */
		for (uint32_t lf2 = 1; lf2 <= nv; lf2++)
			if (row[(lf2 - 1) >> 3] >> ((lf2 - 1) & 7) & 1)
				rowset[lf2 >> 3] |= (uint8_t) (1u << (lf2 & 7));
		uint32_t sr = 0, sv = 0;
		for (uint32_t m = 1; m < nb; m++) {
			if (!mrecs[m])
				continue;
			const uint8_t *set = leafsets + (size_t) m * leafbytes;
			int seen = 0;
			for (size_t b = 0; b < leafbytes && !seen; b++)
				seen = (set[b] & rowset[b]) != 0;
			if (seen) {
				sr += mrecs[m];
				sv += mverts[m];
			}
		}
		if (sr > best_r) best_r = sr;
		if (sv > best_v) best_v = sv;
	}
	if (!rows) {                                             /* (no vis: every model) */
		best_r = tot_r;
		best_v = tot_v;
	}
	*strict_recs  = best_r;
	*strict_verts = best_v;
	*total_recs   = tot_r;
	*total_verts  = tot_v;
	*recs_out  = best_r + big_r > tot_r ? tot_r : best_r + big_r;
	*verts_out = best_v + big_v > tot_v ? tot_v : best_v + big_v;
	free(row);
	free(rowset);
	free(mrecs);
	free(mverts);
}

void bmf_rooms(uint8_t *f, size_t len)
{
	qw_bmodel *bms = world_view(f, len);
	bmf_need *room = rooms_of(bms);
	uint32_t n = 0, recs = 0, verts = 0, groups = 0;
	for (uint32_t m = 1; m < W.h->num_bmodels; m++) {
		const bmf_need *r = &room[m];
		if (r->recs & 3 || r->recs / 4 > 0xffff || r->verts > 0xffff || r->groups > 0xffff)
			die("brush model %u: a room of %u B, %u vertices, %u groups", m, r->recs, r->verts, r->groups);
		bms[m].room_recs   = (uint16_t) (r->recs / 4);
		bms[m].room_verts  = (uint16_t) r->verts;
		bms[m].room_groups = (uint16_t) r->groups;
		n += r->verts != 0;
		recs += r->recs;
		verts += r->verts;
		groups += r->groups;
	}
	uint32_t ar, av, sr, sv, tr, tv;
	arena_of(room, &ar, &av, &sr, &sv, &tr, &tv);
	qw_header *h = (qw_header *) f;
	h->bmf_arena_recs  = ar;
	h->bmf_arena_verts = av;
	free(room);
	printf("qcook:   brush models' rooms: %u models, %u B of records, %u vertices, %u groups; "
	       "%u cuts (%u given up), %.2f s; the cut arena %u B + %u vertices (visible at once %u B + %u, "
	       "the rooms in all %u B + %u)\n", n, recs, verts, groups, R.cuts, R.given_up, R.secs, ar, av, sr, sv,
	       tr, tv);
}

/* `qcook bmf <file.wld> [-v]`: a cooked world's rooms measured again (each
   model's cuts and their cost, and §44's sampled most with -v), against
   the ones it carries */
int cmd_bmf(int argc, char **argv)
{
	if (argc < 3)
		die("usage: qcook bmf <file.wld> [-v]");
	R.verbose = argc > 3 && !strcmp(argv[3], "-v");
	long len;
	uint8_t *f = read_file(argv[2], &len);
	const qw_bmodel *bms = world_view(f, (size_t) len);
	bmf_need *room = rooms_of(bms);
	int differ = 0;
	for (uint32_t m = 1; m < W.h->num_bmodels; m++)
		if (bms[m].room_recs * 4u != room[m].recs || bms[m].room_verts != room[m].verts ||
		    bms[m].room_groups != room[m].groups) {
			printf("bmf *%u: the file's room %u B, %u verts, %u groups; measured %u B, %u verts, %u groups\n",
			       m, bms[m].room_recs * 4u, bms[m].room_verts, bms[m].room_groups, room[m].recs,
			       room[m].verts, room[m].groups);
			differ++;
		}
	printf("bmf %s: %u cuts, %u given up, %.2f s; %d rooms differ from the file's\n", argv[2], R.cuts,
	       R.given_up, R.secs, differ);
	free(room);
	free(f);
	return differ != 0;
}
