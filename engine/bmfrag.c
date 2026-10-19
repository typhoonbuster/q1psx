#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bmfrag.h"
#include "mem.h"
#include "prof.h"
#include "q_fixed.h"
#include "render.h"
#include "rlist.h"
#include "world.h"

/* a piece's corners as the cut carries them: vertex units in the world,
   the texels of the record's uv bytes, the light byte */
typedef struct {
	int16_t  p[3];
	uint8_t  u, v, l, pad;
	uint16_t pad2;
} __attribute__((aligned(4))) bmf_v;     /* 12 bytes: copied by words */

/* corners copied three words each (the SDK's memcpy goes byte by byte;
   GCC would make this loop a memcpy call again) */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void copy_v(bmf_v *d, const bmf_v *s, int n)
{
	uint32_t *dw = (uint32_t *) d;
	const uint32_t *sw = (const uint32_t *) s;
	for (; n > 0; n--, dw += 3, sw += 3) {
		dw[0] = sw[0];
		dw[1] = sw[1];
		dw[2] = sw[2];
	}
}

#define BMF_CORNERS 12       /* a piece's corners (a quad cut by 8 planes)        */
#define BMF_PIECES  64       /* one record's pieces                                */
#define BMF_STACK   24       /* the cut's pending back pieces                      */
#define BMF_GROW    3        /* room for a model's pieces: its records' bytes and
                                corners times this, and BMF_SLACK more            */
#define BMF_SLACK   512
#define BMF_PATH    64       /* a train's corners, at most, for its room           */

typedef struct {
	const qw_leaf *leaf;
	uint8_t n;               /* corners, in the outline's order                   */
	uint8_t cut;             /* 0: the cooked polygon as it was (a quad's corners
	                            go back to the PS1 order, its diagonal kept)      */
	uint8_t quad;
	uint8_t pad;
	bmf_v   v[BMF_CORNERS];
} bmf_piece;

typedef struct {
	const qw_leaf *leaf;
	uint32_t ofs;            /* its records in the model's buffer                 */
	uint32_t len;
} bmf_group;

/* the model's own BSP, compact: per node its plane, its children (-1: a
   leaf of the model, nothing there) and, per side, its pieces' groups */
typedef struct {
	uint16_t plane3;
	int16_t  child[2];
	uint16_t first[2], count[2];
} bmf_node;

struct bmf_cache {
	int32_t    org[3];       /* the origin the pieces were cut at (1/8 units)    */
	uint8_t    valid;        /* 1 cut, 0 too cut up there (drawn whole until it
	                            moves, not cut each frame)                     */
	uint16_t   num_nodes;
	uint16_t   rmax;         /* the largest sphere its pieces' records can have
	                            (vertex units: all_culled)                     */
	bmf_node  *nodes;
	uint8_t   *recs;         /* the pieces' records (qw_record, the world's)     */
	uint32_t   recs_len, recs_cap;
	bmf_group *groups;
	uint32_t   num_groups, groups_cap;
	uint32_t   pool_first, pool_cap, pool_len;   /* its vertices in bmf_pool      */
};

static qw_vert  *bmf_pool;
static uint32_t  bmf_pool_cap, bmf_pool_used;
#ifdef Q1PSX_BENCH
uint32_t bmf_stats[7];
#endif

static inline const qw_node *node_at(uint32_t at)   /* (KUSEG, as the walk keeps them) */
{
	return (const qw_node *) (at | 0x80000000u);
}

static inline const qw_plane *plane_of(uint16_t plane3)
{
	return (const qw_plane *) ((const int32_t *) world.planes + plane3);
}

/* the plane's distance of a point, x4096 vertex units (world_point_leaf's) */
static inline int32_t plane_d(const qw_plane *pl, const int16_t p[3])
{
	int32_t dist = pl->dist >> 1;
	return pl->type < 3 ? p[pl->type] * 4096 - dist
	                    : pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - dist;
}

/* ------------------------------------------------------------- the cut -- */

static bmf_piece pieces[BMF_PIECES];
static int       num_pieces, cut_overflow;

static struct {
	uint32_t at;
	uint8_t  n;
	bmf_v    v[BMF_CORNERS];
} pending[BMF_STACK];

/* the point where edge a-b crosses the plane (da > 0 >= db or the reverse),
   from the lower endpoint whichever way the edge runs: two faces sharing
   it get the same point */
static void cross(bmf_v *o, const bmf_v *a, const bmf_v *b, int32_t da, int32_t db)
{
	if (a->p[0] > b->p[0] || (a->p[0] == b->p[0] && (a->p[1] > b->p[1] ||
	    (a->p[1] == b->p[1] && a->p[2] > b->p[2])))) {
		const bmf_v *t = a; a = b; b = t;
		int32_t td = da; da = db; db = td;
	}
	/* t = da / (da - db), x4096 (the two of one sign): both shifted down
	   together until (da << 12) fits, the division then exact to the last
	   bits (a slanted plane's d is no multiple of 4096) */
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
		o->p[k] = (int16_t) (a->p[k] + (((int32_t) b->p[k] - a->p[k]) * t + 2048 >> 12));
	o->u = (uint8_t) (a->u + (((int32_t) b->u - a->u) * t + 2048 >> 12));
	o->v = (uint8_t) (a->v + (((int32_t) b->v - a->v) * t + 2048 >> 12));
	o->l = (uint8_t) (a->l + (((int32_t) b->l - a->l) * t + 2048 >> 12));
	o->pad = 0;
	o->pad2 = 0;
}

/* R_RecursiveClipBPoly: the polygon down the world's BSP from `top`, cut at
   every plane it straddles (d > 0: the front, as the walk), its pieces in
   non-solid leaves kept in pieces[] */
static void cut_poly(uint32_t top, const bmf_v *in, int n, int quad)
{
	bmf_v cur[BMF_CORNERS], side[2][BMF_CORNERS];
	int32_t d[BMF_CORNERS];
	int sp = 0, cutting = 0;
	uint32_t at = top;
	copy_v(cur, in, n);
	for (;;) {
		while (!(at & 1)) {
			const qw_node *nd = node_at(at);
			const qw_plane *pl = plane_of(nd->plane3);
			int front = 0, back = 0;
			for (int i = 0; i < n; i++) {
				d[i] = plane_d(pl, cur[i].p);
				front |= d[i] > 0;
				back  |= d[i] < 0;
			}
			/* in the plane (every d 0): the back, as Quake's d <= 0; touching
			   it from the front: the front whole (Quake's back part has no
			   area, nothing drawn) */
			if (!back) { at = nd->child[!front]; continue; }
			if (!front) { at = nd->child[1]; continue; }
			/* a corner in the plane goes with its neighbours' sides (both
			   when the plane passes through it, one when the outline only
			   touches it there: a corner the cut's rounding pushed in) */
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
			pending[sp].at = nd->child[1];             /* the back piece, later */
			pending[sp].n  = (uint8_t) nb;
			copy_v(pending[sp].v, side[1], nb);
			sp++;
			copy_v(cur, side[0], nf);
			n = nf;
			at = nd->child[0];
			cutting = 1;
		}
		const qw_leaf *leaf = (const qw_leaf *) ((at & ~1u) | 0x80000000u);
		if (leaf->contents != QW_CONTENTS_SOLID) {
			if (num_pieces >= BMF_PIECES) { cut_overflow = 1; return; }
			bmf_piece *pc = &pieces[num_pieces++];
			pc->leaf = leaf;
			pc->n    = (uint8_t) n;
			pc->cut  = (uint8_t) cutting;
			pc->quad = (uint8_t) quad;
			copy_v(pc->v, cur, n);
		}
		if (!sp)
			return;
		sp--;
		at = pending[sp].at;
		n  = pending[sp].n;
		copy_v(cur, pending[sp].v, n);
		cutting = 1;
	}
}

/* ------------------------------------------------------------ records -- */

/* a record of `count` pieces of one leaf (pieces[first..], source record
   src: its texture and flags), appended to the model's buffer; 0 when
   there is no room */
static int put_record(bmf_cache *c, const qw_record *src, const bmf_piece *const *pc, int count)
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
	uint32_t size  = polys + (uint32_t) (nq + nt) * 12;
	if (c->recs_len + size > c->recs_cap || c->pool_len + (uint32_t) nv > c->pool_cap)
		return 0;
	uint8_t *r = c->recs + c->recs_len;
	qw_record *h = (qw_record *) r;
	uint16_t *idx = (uint16_t *) (r + sizeof *h);
	uint8_t *light = r + polys - cbytes;
	qw_quad *q = (qw_quad *) (r + polys);
	qw_tri  *t = (qw_tri *) (q + nq);
	uint32_t base = c->pool_first + c->pool_len;
	int32_t lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 }, lsum = 0;
	int k = 0;                                   /* the record's vertex */
	for (int i = 0; i < count; i++) {
		const bmf_piece *p = pc[i];
		int v0 = k;
		for (int j = 0; j < p->n; j++, k++) {
			qw_vert *pv = &bmf_pool[base + (uint32_t) k];
			pv->x = p->v[j].p[0];
			pv->y = p->v[j].p[1];
			pv->z = p->v[j].p[2];
			pv->pad = 0;
			idx[k]   = (uint16_t) (base + (uint32_t) k);
			light[k] = p->v[j].l;
			lsum += p->v[j].l;
			for (int a = 0; a < 3; a++) {
				if (p->v[j].p[a] < lo[a]) lo[a] = p->v[j].p[a];
				if (p->v[j].p[a] > hi[a]) hi[a] = p->v[j].p[a];
			}
		}
#define UV(m) ((uint16_t) (p->v[m].u | p->v[m].v << 8))
		if (!p->cut && p->quad) {            /* outline 0 1 3 2 -> the PS1's 0 1 2 3 */
			static const uint8_t ps1[4] = { 0, 1, 3, 2 };
			for (int j = 0; j < 4; j++) {
				q->idx[j] = (uint8_t) ((v0 + ps1[j]) * 4);
				q->uv[j]  = UV(ps1[j]);
			}
			q++;
		} else if (!p->cut) {
			for (int j = 0; j < 3; j++) {
				t->idx[j] = (uint8_t) ((v0 + j) * 4);
				t->uv[j]  = UV(j);
			}
			t->pad = 0;
			t->pad2 = 0;
			t++;
		} else {
			/* the outline's fan from corner 0: quads (m, 0, m+1, m+2) in the
			   PS1's order (triangles (m, 0, m+1) and (0, m+1, m+2)), a
			   triangle for the odd one */
			int m = 1;
			for (; m + 2 < p->n; m += 2) {
				const int c4[4] = { m, 0, m + 1, m + 2 };
				for (int j = 0; j < 4; j++) {
					q->idx[j] = (uint8_t) ((v0 + c4[j]) * 4);
					q->uv[j]  = UV(c4[j]);
				}
				q++;
			}
			if (m + 1 < p->n) {
				const int c3[3] = { 0, m, m + 1 };
				for (int j = 0; j < 3; j++) {
					t->idx[j] = (uint8_t) ((v0 + c3[j]) * 4);
					t->uv[j]  = UV(c3[j]);
				}
				t->pad = 0;
				t->pad2 = 0;
				t++;
			}
		}
#undef UV
	}
	for (int j = nv; j < nidx; j++)              /* (padded with the first's) */
		idx[j] = idx[0];
	if (nidx * 2 < ibytes)
		idx[nidx] = 0;
	for (int j = nv; j < cbytes; j++)
		light[j] = 0;
	int32_t sc[3];
	uint32_t r2 = 0;
	for (int a = 0; a < 3; a++)
		sc[a] = (lo[a] + hi[a]) / 2;
	for (int j = 0; j < nv; j++) {
		const qw_vert *pv = &bmf_pool[base + (uint32_t) j];
		int32_t dx = pv->x - sc[0], dy = pv->y - sc[1], dz = pv->z - sc[2];
		uint32_t d2 = (uint32_t) (dx * dx) + (uint32_t) (dy * dy) + (uint32_t) (dz * dz);
		if (d2 > r2) r2 = d2;
	}
	h->face_id   = 0;                            /* (drawn with the models' stamp) */
	h->num_verts = (uint8_t) nv;
	h->num_quads = (uint8_t) nq;
	h->num_tris  = (uint8_t) nt;
	int avg = nv ? (lsum + nv * 2) / (nv * 4) : 0;   /* (the flat shading's, world.c's) */
	h->flags     = (uint8_t) ((src->flags & (QWF_SKY | QWF_LIQUID)) | (avg > 63 ? 63 : avg) << 2);
	h->size      = (uint16_t) size;
	h->tex       = src->tex;
	h->sr        = (uint16_t) (q_isqrt32(r2) + 1);
	h->sc[0]     = (int16_t) sc[0];
	h->sc[1]     = (int16_t) sc[1];
	h->sc[2]     = (int16_t) sc[2];
	h->polys     = (uint16_t) polys;
	c->recs_len += size;
	c->pool_len += (uint32_t) nv;
	return 1;
}

static uint32_t side_first;     /* the node side's first group (cut_model's) */

/* the group of a block's records in `leaf`: the last one grows when it is
   the same leaf's, of the same node side (its records follow) */
static int put_group(bmf_cache *c, const qw_leaf *leaf, uint32_t ofs, uint32_t len)
{
	if (c->num_groups > side_first && c->groups[c->num_groups - 1].leaf == leaf &&
	    c->groups[c->num_groups - 1].ofs + c->groups[c->num_groups - 1].len == ofs) {
		c->groups[c->num_groups - 1].len += len;
		return 1;
	}
	if (c->num_groups >= c->groups_cap)
		return 0;
	bmf_group *g = &c->groups[c->num_groups++];
	g->leaf = leaf;
	g->ofs  = ofs;
	g->len  = len;
	return 1;
}

/* the pieces so far (of one record), per leaf in the order they came:
   records of up to QW_MAX_VERTS vertices, appended, their groups to the
   block */
static int put_pieces(bmf_cache *c, const qw_record *r)
{
#ifdef Q1PSX_BENCH
	bmf_stats[1] += (uint32_t) num_pieces;
#endif
	uint8_t done[BMF_PIECES];
	memset(done, 0, (size_t) num_pieces);
	for (int i = 0; i < num_pieces; i++) {
		if (done[i])
			continue;
		const qw_leaf *leaf = pieces[i].leaf;
		const bmf_piece *batch[BMF_PIECES];
		int nb = 0, nvb = 0;
		uint32_t start = c->recs_len;
		for (int j = i; j < num_pieces; j++) {
			if (done[j] || pieces[j].leaf != leaf)
				continue;
			if (nvb + pieces[j].n > QW_MAX_VERTS) {
				if (!put_record(c, r, batch, nb))
					return 0;
				nb = nvb = 0;
			}
			batch[nb++] = &pieces[j];
			nvb += pieces[j].n;
			done[j] = 1;
		}
		if (nb && !put_record(c, r, batch, nb))
			return 0;
		if (!put_group(c, leaf, start, c->recs_len - start))
			return 0;
	}
	num_pieces = 0;
	return 1;
}

/* one cooked record cut at offset `off` (vertex units): its pieces put
   when half the room is taken (a big record's polygons in several turns)
   and at its end */
static int cut_record(bmf_cache *c, const uint8_t *rp, const int16_t off[3], uint32_t top)
{
	const qw_record *r = (const qw_record *) rp;
	int nv = r->num_verts;
	const uint16_t *idx = (const uint16_t *) (rp + sizeof *r);
	const uint8_t *light = rp + r->polys - ((nv + 3) & ~3);
	const qw_quad *q = (const qw_quad *) (rp + r->polys);
	const qw_tri *t = (const qw_tri *) (q + r->num_quads);
	num_pieces = 0;
	cut_overflow = 0;
	/* the record's sphere down the world's BSP first: a node it lies
	   wholly on one side of sends every polygon there (each corner's d has
	   the centre's sign: |d(p) - d(c)| <= |n| |p - c| < 4097 sr) */
	const int16_t sc[3] = { (int16_t) (r->sc[0] + off[0]), (int16_t) (r->sc[1] + off[1]),
	                        (int16_t) (r->sc[2] + off[2]) };
	int32_t bound = r->sr * 4097;
	while (!(top & 1)) {
		const qw_node *nd = node_at(top);
		int32_t d = plane_d(plane_of(nd->plane3), sc);
		if (d > bound)
			top = nd->child[0];
		else if (d < -bound)
			top = nd->child[1];
		else
			break;
	}
	if ((top & 1) && ((const qw_leaf *) ((top & ~1u) | 0x80000000u))->contents == QW_CONTENTS_SOLID)
		return 1;                                /* (all of it in a solid leaf: no piece) */
	for (int i = 0; i < r->num_quads + r->num_tris; i++) {
		if (num_pieces > BMF_PIECES / 2 && !put_pieces(c, r))
			return 0;
		int quad = i < r->num_quads, n = quad ? 4 : 3;
		bmf_v v[4];
		for (int j = 0; j < n; j++) {
			static const uint8_t outline[4] = { 0, 1, 3, 2 };   /* PS1 corners -> the outline */
			int cj = quad ? outline[j] : j;
			int lv = (quad ? q[i].idx[cj] : t[i - r->num_quads].idx[cj]) >> 2;
			uint16_t uv = quad ? q[i].uv[cj] : t[i - r->num_quads].uv[cj];
			const qw_vert *pv = &world.verts[idx[lv]];
			v[j].p[0] = (int16_t) (pv->x + off[0]);
			v[j].p[1] = (int16_t) (pv->y + off[1]);
			v[j].p[2] = (int16_t) (pv->z + off[2]);
			v[j].u = (uint8_t) uv;
			v[j].v = (uint8_t) (uv >> 8);
			v[j].l = light[lv];
			v[j].pad = 0;
			v[j].pad2 = 0;
		}
		cut_poly(top, v, n, quad);
		if (cut_overflow)
			return 0;
	}
	return put_pieces(c, r);
}

/* the model's compact BSP (nodes in DFS order), once */
static int build_nodes(bmf_cache *c, uint32_t at, int *count, int cap)
{
	if (at & 1)
		return -1;
	if (*count >= cap)
		return -2;
	int k = (*count)++;
	const qw_node *nd = node_at(at);
	c->nodes[k].plane3 = nd->plane3;
	for (int s = 0; s < 2; s++) {
		int ch = build_nodes(c, nd->child[s], count, cap);
		if (ch == -2)
			return -2;
		c->nodes[k].child[s] = (int16_t) ch;
	}
	return k;
}

/* brush model `model` cut at origin `org` (1/8 units), from world node
   `top` down */
static int cut_model(bmf_cache *c, int model, const int32_t org[3], uint32_t top)
{
	const qw_bmodel *bm = &world.bmodels[model];
	int16_t off[3];
	for (int k = 0; k < 3; k++)                   /* 1/8 units -> vertex units */
		off[k] = (int16_t) (org[k] * QW_VSCALE / CLIP_SUB);
	c->recs_len = c->num_groups = c->pool_len = 0;
	/* the nodes again in the DFS order build_nodes gave them */
	uint32_t stack[QW_MAX_BM_DEPTH + 2];
	int sp = 0, k = 0;
	if (bm->root & 1)
		return 1;
	stack[sp++] = bm->root;
	while (sp) {
		uint32_t at = stack[--sp];
		const qw_node *nd = node_at(at);
		bmf_node *bn = &c->nodes[k++];
		for (int s = 0; s < 2; s++) {
			bn->first[s] = (uint16_t) c->num_groups;
			side_first   = c->num_groups;
			const uint8_t *p = (const uint8_t *) nd->faces[s], *end = (const uint8_t *) qw_face_end(nd, s);
			while (p < end) {
				const qw_record *r = (const qw_record *) p;
				if (!r->size)
					break;
				if (!cut_record(c, p, off, top))
					return 0;
				p += r->size;
			}
			bn->count[s] = (uint16_t) (c->num_groups - bn->first[s]);
		}
		/* (preorder: the front child's subtree first, as build_nodes) */
		if (!(nd->child[1] & 1)) {
			if (sp >= QW_MAX_BM_DEPTH + 2) return 0;
			stack[sp++] = nd->child[1];
		}
		if (!(nd->child[0] & 1)) {
			if (sp >= QW_MAX_BM_DEPTH + 2) return 0;
			stack[sp++] = nd->child[0];
		}
	}
#ifdef Q1PSX_BENCH
	bmf_stats[2]++;
#endif
	return 1;
}

/* ---------------------------------------------------------------- load -- */

/* a model's records' bytes and polygon corners, for its room */
static void model_size(uint32_t at, uint32_t *bytes, uint32_t *corners, int *nodes)
{
	if (at & 1)
		return;
	const qw_node *nd = node_at(at);
	(*nodes)++;
	for (int s = 0; s < 2; s++) {
		const uint8_t *p = (const uint8_t *) nd->faces[s], *end = (const uint8_t *) qw_face_end(nd, s);
		while (p < end) {
			const qw_record *r = (const qw_record *) p;
			if (!r->size)
				break;
			*bytes   += r->size;
			*corners += (uint32_t) r->num_quads * 4 + (uint32_t) r->num_tris * 3;
			p += r->size;
		}
	}
	model_size(nd->child[0], bytes, corners, nodes);
	model_size(nd->child[1], bytes, corners, nodes);
}

#ifdef Q1PSX_BENCH
uint32_t bmf_model_bytes(int model)
{
	uint32_t bytes = 0, corners = 0;
	int nodes = 0;
	model_size(world.bmodels[model].root, &bytes, &corners, &nodes);
	return bytes;
}
#endif

/* the largest sphere a record of a model's pieces can have (vertex
   units): a piece record's corners are in its source record's box, so its
   own box is, and its sphere (put_record: about its box's centre,
   truncated, to its farthest corner, + 1) is at most that box's half
   diagonal + 0.87 + 1 */
static uint32_t model_rmax(uint32_t at)
{
	uint32_t most = 0;
	while (!(at & 1)) {
		const qw_node *nd = node_at(at);
		for (int s = 0; s < 2; s++) {
			const uint8_t *p = (const uint8_t *) nd->faces[s], *end = (const uint8_t *) qw_face_end(nd, s);
			while (p < end) {
				const qw_record *r = (const qw_record *) p;
				if (!r->size)
					break;
				const uint16_t *idx = (const uint16_t *) (p + sizeof *r);
				int32_t lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 };
				for (int i = 0; i < r->num_verts; i++) {
					const qw_vert *v = &world.verts[idx[i]];
					const int32_t q[3] = { v->x, v->y, v->z };
					for (int k = 0; k < 3; k++) {
						if (q[k] < lo[k]) lo[k] = q[k];
						if (q[k] > hi[k]) hi[k] = q[k];
					}
				}
				uint32_t d2 = 0;
				for (int k = 0; k < 3; k++) {
					uint32_t hk = (uint32_t) (hi[k] - lo[k] + 1) / 2;
					d2 += hk * hk;
				}
				uint32_t rad = q_isqrt32(d2) + 3;
				if (rad > most)
					most = rad;
				p += r->size;
			}
		}
		uint32_t back = model_rmax(nd->child[1]);
		if (back > most)
			most = back;
		at = nd->child[0];
	}
	return most;
}

bmf_cache **bmf_models;          /* this level's caches, by brush model: an
                                    edict spawned again (a death) finds its own */

void bmf_level(void)
{
	bmf_models = 0;
	bmf_pool = 0;
	bmf_pool_cap = bmf_pool_used = 0;
	r_state.fragpool = 0;
#ifdef Q1PSX_BENCH
	memset(bmf_stats, 0, sizeof bmf_stats);
#endif
}

static int drawn_model(const edict_t *e)
{
	return e->inuse && e->draw && e->model > 0 && e->model < (int) world.num_bmodels;
}

/* the model where its edict stands */
static void cut_at(bmf_cache *c, const edict_t *e)
{
	c->org[0] = e->origin[0];
	c->org[1] = e->origin[1];
	c->org[2] = e->origin[2];
#ifdef Q1PSX_BENCH
	uint32_t t0 = prof_now();
#endif
	c->valid = (uint8_t) cut_model(c, e->model, e->origin, e->topnode);
#ifdef Q1PSX_BENCH
	uint32_t cy = prof_cycles(t0, prof_now());
	bmf_stats[4] += cy;
	if (cy > bmf_stats[5])
		bmf_stats[5] = cy;
	if (!c->valid)
		bmf_stats[3]++;
#endif
}

/* the room a model's pieces get: its records' bytes, vertices, groups */
typedef struct {
	uint32_t recs, verts, groups;
} bmf_need;

static int is_mover(const edict_t *e)            /* a travel from pos1 to pos2 */
{
	return e->class == QE_FUNC_DOOR || e->class == QE_FUNC_BUTTON || e->class == QE_FUNC_PLAT;
}

/* the places edict e's model goes through, in path (BMF_PATH + 1 of them,
   seen BMF_PATH): a door's, a button's, a plat's two ends, a train's
   corners, a secret door's place and its two moves' ends (movers_path),
   where it stands for a model that never moves; their count, 0 for a
   train without a path or with a longer one */
static int model_path(const edict_t *e, int32_t (*path)[3], const edict_t **seen)
{
	if (is_mover(e)) {
		memcpy(path[0], e->pos1, sizeof path[0]);
		memcpy(path[1], e->pos2, sizeof path[1]);
		return 2;
	}
	if (e->class == QE_FUNC_TRAIN || e->class == QE_FUNC_DOOR_SECRET) {
		int np = movers_path(e, path, seen, BMF_PATH);
		return np < 0 ? 0 : np;
	}
	memcpy(path[0], e->origin, sizeof path[0]);
	return 1;
}

/* edict e's model cut as if it stood at `org` (1/8 units), its top node
   as ent_link makes it */
static int cut_where(bmf_cache *c, const edict_t *e, const int32_t org[3])
{
	int32_t lo[3], hi[3];
	for (int k = 0; k < 3; k++) {
		lo[k] = ((org[k] + e->mins[k] - CLIP_SUB) >> 3) - 2;
		hi[k] = ((org[k] + e->maxs[k] + CLIP_SUB) >> 3) + 3;
	}
	return cut_model(c, e->model, org, world_box_topnode(lo, hi));
}

/* Room for the pieces of every brush model the spawned entities stand
   with (a trigger's has no faces), sized on what their cuts take
   (docs/porting-notes.md §44: the old room, three times each model's
   record bytes and corners, was 4 to 5 times what the cuts took and left
   the stream's RAM pool 0-6 KB on the biggest maps). Each model is cut
   in a scratch cache at the arena's top: one that never moves gets what
   its cut takes where it stands (bmf_load's cut, the same); one that
   moves twice the most its cuts take at the places it goes through and
   halfway along each way between them (model_path; and where it spawned
   when that is off its path): cut at 33 places a way, no mover of the 32
   maps took more than 1.9 times that (BMFCHK=1 scripts/bench.sh). After
   the spawn, which puts the models where they stand and gives a mover
   its travel; before the stream manager takes the arena's rest. A model
   whose path or cuts the scratch cannot take gets the old room; one
   spawned only later (a death after START's trigger_setskill: no brush
   model there hangs on the skill) none: drawn whole. */
void bmf_reserve(void)
{
#ifdef Q1PSX_BENCH
	uint32_t used = mem_level_used();
#endif
	uint32_t nb = world.num_bmodels;
	bmf_models = mem_level_alloc(nb * sizeof *bmf_models);
	if (!bmf_models)
		return;
	memset(bmf_models, 0, nb * sizeof *bmf_models);

	/* the scratch: the old room of the largest model */
	uint32_t max_bytes = 0, max_corners = 0;
	int max_nodes = 0;
	for (int i = 2; i < num_edicts; i++) {
		const edict_t *e = &edicts[i];
		if (!e->inuse || e->model <= 0 || e->model >= (int) nb)
			continue;
		uint32_t bytes = 0, corners = 0;
		int nodes = 0;
		model_size(world.bmodels[e->model].root, &bytes, &corners, &nodes);
		if (bytes > max_bytes)     max_bytes = bytes;
		if (corners > max_corners) max_corners = corners;
		if (nodes > max_nodes)     max_nodes = nodes;
	}
	if (!max_corners)
		return;
#ifdef Q1PSX_BENCH
	uint32_t t0 = prof_now(), num_cuts = 0;
#endif
	/* the needs first above the mark: the pool is then reserved over them
	   and only written by the cuts (bmf_load), after their last read */
	uint32_t mark = mem_level_mark();
	bmf_need *need = mem_level_alloc(nb * sizeof *need);
	if (!need)
		return;
	memset(need, 0, nb * sizeof *need);
	bmf_cache sc;
	memset(&sc, 0, sizeof sc);
	sc.recs_cap   = (max_bytes * BMF_GROW + BMF_SLACK + 3) & ~3u;
	sc.groups_cap = (uint32_t) max_nodes * 2 * 4 + 16;
	sc.pool_cap   = max_corners * BMF_GROW + 64;
	sc.nodes      = mem_level_alloc((uint32_t) max_nodes * sizeof *sc.nodes);
	sc.recs       = mem_level_alloc(sc.recs_cap);
	sc.groups     = mem_level_alloc(sc.groups_cap * sizeof *sc.groups);
	bmf_pool      = mem_level_alloc(sc.pool_cap * sizeof *bmf_pool);
	int32_t (*path)[3]   = mem_level_alloc((BMF_PATH + 1) * sizeof *path);
	const edict_t **seen = mem_level_alloc(BMF_PATH * sizeof *seen);
	int scratch = sc.nodes && sc.recs && sc.groups && bmf_pool && path && seen;
	for (int i = 2; i < num_edicts; i++) {
		const edict_t *e = &edicts[i];
		int m = e->model;
		if (!e->inuse || m <= 0 || m >= (int) nb || need[m].recs)
			continue;
		uint32_t bytes = 0, corners = 0;
		int nodes = 0;
		model_size(world.bmodels[m].root, &bytes, &corners, &nodes);
		if (!nodes || !corners)
			continue;
		bmf_need old = { bytes * BMF_GROW + BMF_SLACK, corners * BMF_GROW + 64, (uint32_t) nodes * 2 * 4 + 16 };
		int np = scratch ? model_path(e, path, seen) : 0, count = 0;
		if (!np || build_nodes(&sc, world.bmodels[m].root, &count, max_nodes) < 0) {
#ifdef Q1PSX_BENCH
			printf("bmf room: *%d, class %d: the old room\n", m, e->class);
#endif
			need[m] = old;
			continue;
		}
		sc.num_nodes = (uint16_t) count;
		bmf_need most = { 0, 0, 0 };
		int fits = 0;
		int off = memcmp(e->origin, path[0], sizeof path[0]) && memcmp(e->origin, path[np - 1], sizeof path[0]);
		for (int p = off ? -1 : 0; p < np; p++)          /* (-1: where it spawned) */
			for (int half = 0; half < (p >= 0 && p + 1 < np ? 2 : 1); half++) {
				int32_t org[3];
				for (int k = 0; k < 3; k++)
					org[k] = p < 0 ? e->origin[k] : path[p][k] + (half ? (path[p + 1][k] - path[p][k]) / 2 : 0);
#ifdef Q1PSX_BENCH
				num_cuts++;
#endif
				if (!cut_where(&sc, e, org))
					continue;                     /* (too cut up there: drawn whole) */
				fits++;
				if (sc.recs_len > most.recs)     most.recs   = sc.recs_len;
				if (sc.pool_len > most.verts)    most.verts  = sc.pool_len;
				if (sc.num_groups > most.groups) most.groups = sc.num_groups;
			}
		if (!fits) {
#ifdef Q1PSX_BENCH
			printf("bmf room: *%d, class %d: no cut fits, the old room\n", m, e->class);
#endif
			need[m] = old;
			continue;
		}
		uint32_t grow = np > 1 ? 2 : 1;
		need[m].recs   = most.recs * grow + 64;
		need[m].verts  = most.verts * grow + 4;
		need[m].groups = most.groups * grow + 2;
	}
	mem_level_release(mark);
	bmf_pool = 0;
#ifdef Q1PSX_BENCH
	printf("bmf room: %u cuts, %u cycles\n", (unsigned) num_cuts, (unsigned) prof_cycles(t0, prof_now()));
#endif

	/* the pool (over the needs: at least their bytes), then each model's
	   cache */
	uint32_t total = 0;
	for (uint32_t m = 1; m < nb; m++)
		total += need[m].verts;
	if (total > 65535)
		total = 65535;                            /* (the records' 16-bit indices) */
	if (total)
		bmf_pool = mem_level_alloc(total * sizeof *bmf_pool > nb * sizeof *need ? total * sizeof *bmf_pool
		                                                                         : nb * sizeof *need);
	if (!bmf_pool)
		return;
	bmf_pool_cap = total;
	r_state.fragpool = (uint32_t) bmf_pool;
	for (uint32_t m = 1; m < nb; m++) {
		if (!need[m].recs)
			continue;
		const qw_bmodel *bm = &world.bmodels[m];
		uint32_t bytes = 0, corners = 0;
		int nodes = 0;
		model_size(bm->root, &bytes, &corners, &nodes);
		if (bmf_pool_used + need[m].verts > bmf_pool_cap)
			continue;
		bmf_cache *c = mem_level_alloc(sizeof *c);
		if (!c)
			break;
		memset(c, 0, sizeof *c);
		c->nodes      = mem_level_alloc((uint32_t) nodes * sizeof *c->nodes);
		c->recs_cap   = (need[m].recs + 3) & ~3u;
		c->recs       = mem_level_alloc(c->recs_cap);
		c->groups_cap = need[m].groups;
		c->groups     = mem_level_alloc(c->groups_cap * sizeof *c->groups);
		if (!c->nodes || !c->recs || !c->groups)
			break;
		int count = 0;
		if (build_nodes(c, bm->root, &count, nodes) < 0)
			continue;
		c->num_nodes  = (uint16_t) count;
		uint32_t rmax = model_rmax(bm->root);
		c->rmax       = (uint16_t) (rmax > 0xffff ? 0xffff : rmax);
		c->pool_first = bmf_pool_used;
		c->pool_cap   = need[m].verts;
		bmf_pool_used += need[m].verts;
		bmf_models[m] = c;
#ifdef Q1PSX_BENCH
		bmf_stats[0]++;
#endif
	}
#ifdef Q1PSX_BENCH
	uint32_t sr = 0, sv = 0, sg = 0, sn = 0;
	for (uint32_t m = 1; m < nb; m++) {
		const bmf_cache *c = bmf_models[m];
		if (!c)
			continue;
		sr += c->recs_cap;
		sv += c->pool_cap * (uint32_t) sizeof *bmf_pool;
		sg += c->groups_cap * (uint32_t) sizeof *c->groups;
		sn += c->num_nodes * (uint32_t) sizeof *c->nodes;
	}
	printf("bmf: %u models, %u KB: records %u, vertices %u, groups %u, nodes %u B\n", (unsigned) bmf_stats[0],
	       (unsigned) ((mem_level_used() - used) / 1024), (unsigned) sr, (unsigned) sv, (unsigned) sg, (unsigned) sn);
#endif
}

#ifndef Q1PSX_BENCH_BMFCHK
#define Q1PSX_BENCH_BMFCHK 0     /* 1: the movers cut along their ways in their rooms (BMFCHK=1 scripts/bench.sh) */
#endif

#if Q1PSX_BENCH_BMFCHK
/* every mover cut in its own room at 33 places a way, as bmf_draw cuts it
   while it moves: those that do not fit there (drawn whole) and the
   fullest the rooms got ("bmfchk" lines; bmf_load then cuts the models
   where they stand again) */
static void bmf_check(void)
{
	static int32_t path[BMF_PATH + 1][3];        /* (the arena's rest is the stream's by now) */
	static const edict_t *seen[BMF_PATH];
	uint32_t keep[6], n = 0, fail = 0, full[3] = { 0, 0, 0 }, t0 = prof_now();
	memcpy(keep, bmf_stats, sizeof keep);
	for (int i = 2; i < num_edicts; i++) {
		const edict_t *e = &edicts[i];
		if (!e->inuse || e->model <= 0 || e->model >= (int) world.num_bmodels || !bmf_models[e->model])
			continue;
		bmf_cache *c = bmf_models[e->model];
		int np = model_path(e, path, seen);
		for (int w = 0; w + 1 < np; w++)
			for (int step = 0; step <= 32; step++) {
				int32_t org[3];
				for (int k = 0; k < 3; k++)
					org[k] = path[w][k] + (path[w + 1][k] - path[w][k]) * step / 32;
				n++;
				if (!cut_where(c, e, org)) {
					fail++;
					printf("bmfchk *%d, class %d, way %d at %d/32: given up (%s)\n", e->model, e->class, w,
					       step, cut_overflow ? "too cut up" : "no room");
					continue;
				}
				uint32_t f[3] = { c->recs_len * 100 / c->recs_cap, c->pool_len * 100 / c->pool_cap,
				                  c->num_groups * 100 / c->groups_cap };
				for (int k = 0; k < 3; k++)
					if (f[k] > full[k])
						full[k] = f[k];
			}
	}
	memcpy(bmf_stats, keep, sizeof keep);
	printf("bmfchk %s: %u cuts, %u given up; the fullest rooms: records %u%%, vertices %u%%, groups %u%%; "
	       "%u cycles\n", world.map_name, (unsigned) n, (unsigned) fail, (unsigned) full[0],
	       (unsigned) full[1], (unsigned) full[2], (unsigned) prof_cycles(t0, prof_now()));
}
#endif

/* after each spawn (the load's, a death's): the drawn models cut where
   they stand (here, not at a first sight: a room's doors coming into view
   in one frame) */
void bmf_load(void)
{
	if (!bmf_models)
		return;
#if Q1PSX_BENCH_BMFCHK
	bmf_check();
#endif
	for (int i = 2; i < num_edicts; i++) {
		const edict_t *e = &edicts[i];
		if (drawn_model(e) && bmf_models[e->model])
			cut_at(bmf_models[e->model], e);
	}
#ifdef Q1PSX_BENCH
	printf("bmf: %u pieces in %u cuts (%u given up), %u cycles\n", (unsigned) bmf_stats[1],
	       (unsigned) bmf_stats[2], (unsigned) bmf_stats[3], (unsigned) bmf_stats[4]);
#endif
}

/* ---------------------------------------------------------------- draw -- */

static uint16_t draw_vis;
static int32_t  draw_cam[3];

/* the model's nodes back to front from the eye: at each, the subtree
   beyond its plane, its pieces on the eye's side, the subtree before it */
static void draw_node(const bmf_cache *c, int k)
{
	while (k >= 0) {
		const bmf_node *bn = &c->nodes[k];
		const qw_plane *pl = plane_of(bn->plane3);
		int32_t dist = pl->dist >> 1;
		int32_t d = pl->type < 3 ? draw_cam[pl->type] * 4096 - dist
		          : pl->n[0] * draw_cam[0] + pl->n[1] * draw_cam[1] + pl->n[2] * draw_cam[2] - dist;
		int side = d <= 0;                        /* the eye's: the front side's faces when d > 0 */
		draw_node(c, bn->child[side ^ 1]);
		for (int g = bn->first[side]; g < bn->first[side] + bn->count[side]; g++) {
			const bmf_group *gr = &c->groups[g];
			if (gr->leaf->visframe == draw_vis)
				rl_add_pieces(gr->leaf, c->recs + gr->ofs, gr->len);
		}
		k = bn->child[side];
	}
}

#ifndef Q1PSX_BENCH_BMF
#define Q1PSX_BENCH_BMF 1        /* 0: the models drawn whole (NOBMF=1 scripts/bench.sh, an A / B) */
#endif
#ifndef Q1PSX_BENCH_BMFCULL
#define Q1PSX_BENCH_BMFCULL 1    /* 0: a moved model cut again out of view too (NOBMFCULL=1, an A / B) */
#endif

static int32_t clamp16(int32_t v)
{
	return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

/* Would the record loop cull every record of a model's pieces where it
   now stands? r_world.s RECLOOP keeps a record whose sphere (centre
   through the GTE, IR saturated, view space x6) passes the four planes:
   right / left z -+ x + 2.125 r >= 0, bottom / top 3z -+ 4y + 8 r >= 0.
   Every piece record's centre is in the model's box and its radius at
   most rmax, so the view-space box (its centre's coordinates, the
   extents |rot| h, 2 more for the roundings, saturated as IR is) wholly
   behind one plane by more than its 2.125 or 8 rmax culls them all; no
   leaf it would be drawn in can be wholly in view either (it holds the
   piece). Then the frame draws nothing of it: it is not cut again until
   it is in view (bmf_seen finds it by its box meanwhile). */
static int all_culled(const edict_t *e, int32_t rmax)
{
	const int16_t *rot = r_state.rot, *ar = r_state.absrot;
	int32_t c[3], h[3], lo[3], hi[3];
	for (int k = 0; k < 3; k++) {                 /* 1/8 units -> vertex units */
		c[k] = (e->absmin[k] + e->absmax[k]) >> 2;
		h[k] = ((e->absmax[k] - e->absmin[k]) >> 2) + 1;
	}
	for (int i = 0; i < 3; i++) {
		int32_t v = ((rot[3 * i] * c[0] + rot[3 * i + 1] * c[1] + rot[3 * i + 2] * c[2]) >> 12) + r_state.tr[i];
		int32_t x = ((ar[3 * i] * h[0] + ar[3 * i + 1] * h[1] + ar[3 * i + 2] * h[2]) >> 12) + 3;
		lo[i] = clamp16(v - x);
		hi[i] = clamp16(v + x);
	}
	int32_t lr = 2 * rmax + (rmax >> 3) + 1, tb = 8 * rmax;
	return hi[2] - lo[0] + lr < 0 || hi[2] + hi[0] + lr < 0 ||
	       3 * hi[2] - 4 * lo[1] + tb < 0 || 3 * hi[2] + 4 * hi[1] + tb < 0;
}

int bmf_seen(const bmf_cache *c, const edict_t *e, uint16_t vis, int whole)
{
	if (c->org[0] != e->origin[0] || c->org[1] != e->origin[1] || c->org[2] != e->origin[2]) {
		/* cut elsewhere (it moved while not drawn): its pieces' leaves say
		   nothing of where it is now; its box's leaves do, or its top node
		   when they were cut short (every leaf of its box is under it, and
		   the PVS marking stamps a visible leaf's ancestors) */
		if (e->num_leaves < ENT_MAX_LEAVES)
			return whole;
		return ((const qw_node *) ((e->topnode & ~1u) | 0x80000000u))->visframe == vis;
	}
	if (!c->valid)
		return whole;
	for (uint32_t g = 0; g < c->num_groups; g++)
		if (c->groups[g].leaf->visframe == vis)
			return 1;
	return 0;
}

int bmf_draw(edict_t *e, uint16_t vis)
{
	bmf_cache *c = bmf_models[e->model];
	if (!Q1PSX_BENCH_BMF)
		return 0;
	if (c->org[0] != e->origin[0] || c->org[1] != e->origin[1] || c->org[2] != e->origin[2]) {
		if (Q1PSX_BENCH_BMFCULL && all_culled(e, c->rmax)) {
#ifdef Q1PSX_BENCH
			bmf_stats[6]++;
#endif
			return 1;                             /* moved, none of it in view: not cut yet */
		}
		cut_at(c, e);                             /* moved: cut again */
	}
	if (!c->valid)
		return 0;
	draw_vis = vis;
	for (int k = 0; k < 3; k++)                   /* the eye in the model's space */
		draw_cam[k] = r_state.cam[k] - e->origin[k] * QW_VSCALE / CLIP_SUB;
#ifdef Q1PSX_BENCH
	extern int ents_debug;
	if (ents_debug)
		printf("  bmf *%d org %d %d %d: %u nodes, %u groups, %u B of records, %u verts\n", e->model,
		       (int) e->origin[0], (int) e->origin[1], (int) e->origin[2], c->num_nodes,
		       (unsigned) c->num_groups, (unsigned) c->recs_len, (unsigned) c->pool_len);
#endif
	draw_node(c, c->num_nodes ? 0 : -1);
	return 1;
}
