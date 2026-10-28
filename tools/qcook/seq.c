/*
 * q1psx — the zones' cooked walk (formats/qworld.h "the cooked walk",
 * docs/porting-notes.md §91): a zone's chunk ends with the tree walk's
 * steps, flat, for a camera in any of the zone's leaves. `qcook zones`
 * (zones.c) calls seq_build per zone for the maps QCOOK_SEQ names.
 *
 * What is decided here and what is left to the frame: the renderer's walk
 * (engine/r_world.s) takes, at each node, the camera's side of its plane
 * (d = n . cam - dist, behind when d < 0, no faces when |d| <= the backface
 * epsilon), the near subtree first, the block on that side, the far
 * subtree; it skips what the PVS stamps leave out and what the frustum
 * culls. The side is the one thing a camera in the zone's leaves cannot
 * change for most planes: each camera leaf's cell (the ancestor
 * half-spaces) is cut out, and a node whose plane leaves every cell on
 * one side by more than the epsilon gets that side's order once (the
 * subtrees, the block between). A node whose plane crosses or touches a
 * cell keeps both orders (VAR), the renderer taking the side as the walk
 * does: the cells' own boundary planes among them, since a camera right
 * on one (d = 0, behind by the leaf lookup, in front by the walk) flips
 * the order, but an axial boundary at a whole vertex unit behind which the
 * cell lies has d a multiple of 4096 there, never within the epsilon,
 * never 0 (the front side's only), and stays fixed. The PVS, the frustum
 * and the records' spheres are the frame's, as they are in the walk.
 *
 * Options (the environment; scripts/cook.sh writes them to settings.txt):
 *   QCOOK_SEQ            the maps that get the walk: "start e1m1", "all",
 *                        "none" (the default: measured, the cooked walk
 *                        costs more frame time than the tree walk)
 *   QCOOK_SEQ_FAST=1     no leaf box tests (the records' spheres alone
 *                        cull: the pictures the same, more off-screen
 *                        records drawn); default: the tree walk's lists
 *   QCOOK_SEQ_TEST=<n>   a node's box is tested when its subtree's score
 *                        (leaves x 100 + blocks x 10 + records x 8, the
 *                        leaves x 20 when fast) reaches n (default 200)
 *   QCOOK_SEQ_PRUNE=<n>  an untested node still gets a PVS gate when its
 *                        subtree has n entries or more (default 2)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qworld.h"

#define BACKFACE_EPS   164          /* engine/r_defs.inc, x4096 x4 per world unit */
#define SIDE_MARGIN    (BACKFACE_EPS + 4096)   /* a quarter unit past the epsilon */
#define CELL_BOUND     8192.0       /* world units: the cells start from this box */
#define CELL_MAX_FACES 160
#define CELL_MAX_V     96

/* ------------------------------------------------------------ options -- */

int seq_wanted(const char *wld_path)
{
	const char *opt = getenv("QCOOK_SEQ");
	if (!opt)
		opt = "none";                        /* (measured slower than the tree walk: off unless asked) */
	if (!strcmp(opt, "none") || !*opt)
		return 0;
	if (!strcmp(opt, "all"))
		return 1;
	const char *base = strrchr(wld_path, '/');
	base = base ? base + 1 : wld_path;
	size_t n = strcspn(base, "_.");
	for (const char *p = opt; *p;) {
		while (*p == ' ' || *p == ',')
			p++;
		size_t k = strcspn(p, " ,");
		if (k == n && !strncasecmp(p, base, n))
			return 1;
		p += k;
	}
	return 0;
}

static int env_int(const char *name, int def)
{
	const char *v = getenv(name);
	return v && *v ? atoi(v) : def;
}

/* ------------------------------------------------------------- cells -- */

typedef struct { int n; double v[CELL_MAX_V][3]; } cpoly;
typedef struct { int nf; cpoly f[CELL_MAX_FACES]; } cell_t;

/* a polygon kept on the side `keep` (>0: n . p - d >= 0, <0: <= 0) of the
   plane; the crossing points appended to cap */
static void cpoly_clip(const cpoly *in, cpoly *out, const double n[3], double d, int keep, cpoly *cap)
{
	out->n = 0;
	for (int i = 0; i < in->n; i++) {
		const double *a = in->v[i], *b = in->v[(i + 1) % in->n];
		double sa = (n[0] * a[0] + n[1] * a[1] + n[2] * a[2] - d) * keep;
		double sb = (n[0] * b[0] + n[1] * b[1] + n[2] * b[2] - d) * keep;
		if (sa >= 0 && out->n < CELL_MAX_V)
			memcpy(out->v[out->n++], a, sizeof out->v[0]);
		if ((sa >= 0) != (sb >= 0)) {
			double t = sa / (sa - sb), p[3];
			for (int k = 0; k < 3; k++)
				p[k] = a[k] + (b[k] - a[k]) * t;
			if (out->n < CELL_MAX_V)
				memcpy(out->v[out->n++], p, sizeof p);
			if (cap->n < CELL_MAX_V)
				memcpy(cap->v[cap->n++], p, sizeof p);
		}
	}
}

/* the cell cut by a plane: its faces clipped, the cap (the plane's own
   face: the crossing points, sorted around their centre) added */
static void cell_clip(cell_t *c, const double n[3], double d, int keep)
{
	cpoly cap, tmp;
	cap.n = 0;
	int nf = 0;
	for (int i = 0; i < c->nf; i++) {
		cpoly_clip(&c->f[i], &tmp, n, d, keep, &cap);
		if (tmp.n >= 3)
			c->f[nf++] = tmp;
	}
	c->nf = nf;
	if (cap.n < 3 || nf >= CELL_MAX_FACES)
		return;
	/* a basis in the plane, the points by their angle about the centre
	   (a convex cap: the angular order is the outline's); duplicates
	   dropped */
	double u[3], w[3], ctr[3] = { 0, 0, 0 };
	int ax = fabs(n[0]) < fabs(n[1]) ? (fabs(n[0]) < fabs(n[2]) ? 0 : 2) : (fabs(n[1]) < fabs(n[2]) ? 1 : 2);
	double e[3] = { 0, 0, 0 };
	e[ax] = 1;
	u[0] = n[1] * e[2] - n[2] * e[1]; u[1] = n[2] * e[0] - n[0] * e[2]; u[2] = n[0] * e[1] - n[1] * e[0];
	double lu = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
	for (int k = 0; k < 3; k++) u[k] /= lu;
	w[0] = n[1] * u[2] - n[2] * u[1]; w[1] = n[2] * u[0] - n[0] * u[2]; w[2] = n[0] * u[1] - n[1] * u[0];
	for (int i = 0; i < cap.n; i++)
		for (int k = 0; k < 3; k++)
			ctr[k] += cap.v[i][k] / cap.n;
	double ang[CELL_MAX_V];
	int idx[CELL_MAX_V];
	for (int i = 0; i < cap.n; i++) {
		double x = 0, y = 0;
		for (int k = 0; k < 3; k++) {
			x += (cap.v[i][k] - ctr[k]) * u[k];
			y += (cap.v[i][k] - ctr[k]) * w[k];
		}
		ang[i] = atan2(y, x);
		idx[i] = i;
	}
	for (int i = 1; i < cap.n; i++)           /* (insertion sort: a few dozen points) */
		for (int j = i; j > 0 && ang[idx[j]] < ang[idx[j - 1]]; j--) {
			int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
		}
	cpoly *f = &c->f[nf];
	f->n = 0;
	for (int i = 0; i < cap.n; i++) {
		const double *p = cap.v[idx[i]];
		if (f->n) {
			const double *q = f->v[f->n - 1];
			if (fabs(p[0] - q[0]) + fabs(p[1] - q[1]) + fabs(p[2] - q[2]) < 1e-6)
				continue;
		}
		memcpy(f->v[f->n++], p, sizeof f->v[0]);
	}
	if (f->n >= 3)
		c->nf++;
}

static void cell_box(cell_t *c)
{
	static const int fv[6][4][3] = {   /* the box's faces, by the corner's signs */
		{ { 0, 0, 0 }, { 0, 1, 0 }, { 0, 1, 1 }, { 0, 0, 1 } }, { { 1, 0, 0 }, { 1, 0, 1 }, { 1, 1, 1 }, { 1, 1, 0 } },
		{ { 0, 0, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 1, 0, 0 } }, { { 0, 1, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 0, 1, 1 } },
		{ { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } }, { { 0, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 0, 1 } },
	};
	c->nf = 6;
	for (int i = 0; i < 6; i++) {
		c->f[i].n = 4;
		for (int j = 0; j < 4; j++)
			for (int k = 0; k < 3; k++)
				c->f[i].v[j][k] = fv[i][j][k] ? CELL_BOUND : -CELL_BOUND;
	}
}

/* ------------------------------------------------------------- build -- */

typedef struct {
	const seq_world *w;
	const qw_plane  *planes;
	const uint8_t   *member;             /* [num_faces]: the chunk's records */
	uint32_t         nn, nl, nvis;
	uint8_t         *pvs;                /* [nl]: a leaf some camera leaf sees */
	uint8_t         *block;              /* [nn][2]: the side's block has chunk records */
	uint32_t        *nrec;               /* [nn][2]: those records */
	int8_t          *cls;                /* [nn]: 0 front, 1 back, 2 var, -1 nothing below */
	uint32_t        *n_leaf, *n_block, *n_rec, *n_ent;   /* [nn] the subtree's */
	uint8_t         *vdepth;             /* [nn]: VAR nodes nested in the subtree, itself included */
	uint32_t         anc_vars;           /* statistics: VAR nodes that are a cell's boundary plane */
	int             *up;                 /* [nn]: the parent, -1 */
	int             *leaf_up;            /* [nl]: a leaf's parent node */
	int32_t         *lofs;               /* [nl]: the leaf's word offset (x 28) */
	int32_t         *nofs;               /* [nn]: the node's, from the leaves (negative) */
	int              fast, test_score, prune_min;
	/* the output */
	uint32_t        *words;
	uint32_t         nw, cap;
	int              depth, max_depth, failed;
	seq_stats       *st;
} build_t;

static void put(build_t *b, uint32_t w)
{
	if (b->nw >= b->cap) {
		b->cap = b->cap ? b->cap * 2 : 4096;
		b->words = realloc(b->words, b->cap * sizeof *b->words);
		if (!b->words)
			die("out of memory");
	}
	b->words[b->nw++] = w;
}

/* the leaves a camera leaf's PVS row names, into b->pvs (the row RLE, as
   the engine reads it; none: every leaf) */
static void pvs_union(build_t *b, uint32_t leaf)
{
	const qw_header *H = b->w->h;
	const qw_leaf *L = (const qw_leaf *) (b->w->file + H->ofs_leaves);
	uint32_t rowbytes = (b->nvis + 7) >> 3;
	if (L[leaf].vis == QW_NONE) {
		memset(b->pvs + 1, 1, b->nvis);
		return;
	}
	const uint8_t *in = b->w->file + H->ofs_vis + L[leaf].vis, *end = b->w->file + H->ofs_vis + H->len_vis;
	uint32_t out = 0;
	while (out < rowbytes && in < end) {
		if (*in) {
			for (int bit = 0; bit < 8; bit++)
				if ((*in >> bit & 1) && out * 8 + (uint32_t) bit < b->nvis)
					b->pvs[out * 8 + bit + 1] = 1;
			out++;
			in++;
			continue;
		}
		out += in[1];
		in += 2;
	}
}

/* every node's subtree counts and whether anything below matters; nodes
   are indexed so that children come after their parents in Quake's BSP
   (the cook's node order is Quake's), so a reverse pass sums them */
static void subtree_counts(build_t *b)
{
	const qw_node *N = b->w->nodes;
	for (int n = (int) b->nn - 1; n >= 0; n--) {
		if (!b->w->world[n])
			continue;
		uint32_t nl = 0, nb = b->block[n * 2] + b->block[n * 2 + 1], nr = b->nrec[n * 2] + b->nrec[n * 2 + 1], ne = nb;
		for (int c = 0; c < 2; c++) {
			uint32_t ch = N[n].child[c];
			if (ch & 0x80000000u) {
				uint32_t l = ch & 0x7fffffffu;
				if (l < b->nl && b->pvs[l]) {
					nl++;
					ne++;
				}
			} else {
				if (ch <= (uint32_t) n)
					die("the cooked walk: node %u's child %u is not after it", n, ch);
				nl += b->n_leaf[ch];
				nb += b->n_block[ch];
				nr += b->n_rec[ch];
				ne += b->n_ent[ch];
			}
		}
		b->n_leaf[n] = nl;
		b->n_block[n] = nb;
		b->n_rec[n] = nr;
		b->n_ent[n] = ne;
		int vd = 0;
		for (int c = 0; c < 2; c++) {
			uint32_t ch = N[n].child[c];
			if (!(ch & 0x80000000u) && b->vdepth[ch] > vd)
				vd = b->vdepth[ch];
		}
		b->vdepth[n] = (uint8_t) (vd + (b->cls[n] == 2));
	}
}

static void emit_node(build_t *b, uint32_t n);

static void emit_child(build_t *b, uint32_t ch)
{
	if (ch & 0x80000000u) {
		uint32_t l = ch & 0x7fffffffu;
		if (l < b->nl && b->pvs[l]) {
			put(b, QWSQ_WORD(QWSQ_LEAF, !b->fast, b->lofs[l]));
			b->st->leaves++;
		}
		return;
	}
	emit_node(b, ch);
}

static void frame_in(build_t *b)
{
	if (++b->depth > b->max_depth)
		b->max_depth = b->depth;
}

static void emit_node(build_t *b, uint32_t n)
{
	const qw_node *N = b->w->nodes;
	if (!b->n_ent[n])
		return;                              /* nothing a camera of the zone may draw below */
	int score = (b->fast ? 20 : 100) * (int) b->n_leaf[n] + 10 * (int) b->n_block[n] + 8 * (int) b->n_rec[n];
	int tested = score >= b->test_score && b->depth < QW_SEQ_MAX_DEPTH;
	int gate = tested || ((int) b->n_ent[n] >= b->prune_min && b->cls[n] != 2);   /* (a VAR gates itself) */
	uint32_t at = b->nw;
	if (gate) {
		put(b, QWSQ_WORD(QWSQ_NODE, tested, b->nofs[n]));
		put(b, 0);                           /* the subtree's words, below */
		b->st->nodes++;
		b->st->tested += tested;
		if (tested)
			frame_in(b);
	}
	int cls = b->cls[n];
	int has_block = b->block[n * 2] || b->block[n * 2 + 1];
	uint32_t c0 = N[n].child[0], c1 = N[n].child[1];
	int rel0 = (c0 & 0x80000000u) ? (c0 & 0x7fffffffu) < b->nl && b->pvs[c0 & 0x7fffffffu] : b->n_ent[c0] != 0;
	int rel1 = (c1 & 0x80000000u) ? (c1 & 0x7fffffffu) < b->nl && b->pvs[c1 & 0x7fffffffu] : b->n_ent[c1] != 0;
	if (cls == 2 && !has_block && !(rel0 && rel1))
		cls = rel0 ? 0 : 1;                  /* one subtree, no block: either order is the same */
	if (cls == 2) {
		uint32_t hd = b->nw;
		put(b, QWSQ_WORD(QWSQ_OTHER, 0, b->nofs[n]));   /* VAR */
		put(b, 0);
		b->st->vars++;
		uint32_t f0 = b->nw;
		emit_child(b, c0);
		put(b, QWSQ_ENDF | (b->nw - hd));
		uint32_t lf = b->nw - f0, b0 = b->nw;
		emit_child(b, c1);
		put(b, QWSQ_ENDB | (b->nw - hd));
		uint32_t lb = b->nw - b0;
		if (lf > 0xffff || lb > 0xffff || b->nw - hd > 0xffffff)
			die("the cooked walk: a VAR subtree of %u / %u words", lf, lb);
		b->words[hd + 1] = lf | lb << 16;
	} else {
		uint32_t near = cls ? c1 : c0, far = cls ? c0 : c1;
		emit_child(b, near);
		if (b->block[n * 2 + cls]) {
			put(b, QWSQ_WORD(QWSQ_BLOCK, cls, b->nofs[n]));
			b->st->blocks++;
		}
		emit_child(b, far);
	}
	if (gate) {
		if (tested) {
			put(b, QWSQ_ENDN);
			b->depth--;
		}
		b->words[at + 1] = b->nw - (at + 2);
	}
}

/* ------------------------------------------------------------- check -- */

/* QCOOK_SEQ_CHECK=1: the words run by a C interpreter of the renderer's
   (engine/r_world.s seq_start) for cameras in each cell of the zone (the
   cell's corners pulled a little toward its centre, and the centre), the
   PVS from the camera leaf's row, every box in the frustum; the events
   (the leaves visited, the blocks drawn) against the tree walk's from the
   same camera, step for step. A mismatch is a bug of the cook; the
   camera points are a sample: a plane the sample does not cross keeps
   its class unchecked. */
typedef struct { uint32_t *ev; uint32_t n, cap; } evlist;

static void ev_put(evlist *e, uint32_t v)
{
	if (e->n >= e->cap) {
		e->cap = e->cap ? e->cap * 2 : 1024;
		e->ev = realloc(e->ev, e->cap * sizeof *e->ev);
		if (!e->ev)
			die("out of memory");
	}
	e->ev[e->n++] = v;
}
#define EV_LEAF(l)      ((uint32_t) (l))
#define EV_BLOCK(n, sd) (0x80000000u | (uint32_t) (n) << 1 | (uint32_t) (sd))

static int32_t cam_d(const build_t *b, uint32_t n, const int32_t cam[3])
{
	const qw_plane *pl = (const qw_plane *) ((const int32_t *) b->planes + b->w->nodes[n].plane3);
	int32_t dist = pl->dist >> 1;
	return pl->type < 3 ? cam[pl->type] * 4096 - dist : pl->n[0] * cam[0] + pl->n[1] * cam[1] + pl->n[2] * cam[2] - dist;
}

/* the tree walk's events (the PVS stamps in `vis`: leaves and nodes) */
static void tree_events(const build_t *b, uint32_t n, const uint8_t *vis_leaf, const uint8_t *vis_node, const int32_t cam[3],
                        evlist *e)
{
	if (!vis_node[n])
		return;
	int32_t d = cam_d(b, n, cam);
	int side = d < 0, nodraw = d + BACKFACE_EPS >= 0 && d - BACKFACE_EPS <= 0;
	for (int k = 0; k < 2; k++) {
		uint32_t ch = b->w->nodes[n].child[k ? 1 - side : side];
		if (ch & 0x80000000u) {
			uint32_t l = ch & 0x7fffffffu;
			if (l < b->nl && vis_leaf[l])
				ev_put(e, EV_LEAF(l));
		} else {
			tree_events(b, ch, vis_leaf, vis_node, cam, e);
		}
		if (!k && !nodraw && b->block[n * 2 + side])
			ev_put(e, EV_BLOCK(n, side));
	}
}

/* the words' events: the renderer's interpreter in C (the frames: the
   flags left out, no frustum here) */
static int seq_trace;
#define EV_TRACE(e, i, what, v) do { if (seq_trace && (e)->n < 8) printf("qcook:   ev %u at word %u: %s %08x\n", (e)->n, (unsigned) (i), what, (unsigned) (v)); } while (0)

static int seq_events(const build_t *b, const uint32_t *words, uint32_t nw, const uint8_t *vis_leaf,
                      const uint8_t *vis_node, const int32_t cam[3], evlist *e)
{
	uint32_t stack[QW_SEQ_MAX_DEPTH + 2], sp = 0, i = 0, steps = 0;
	uint32_t node_of_ofs_base = (b->w->skel_nodes) * (uint32_t) sizeof(qw_node);
	/* the skeleton's node at a word's offset: new index -> old */
	uint32_t *old_of = xmalloc(b->w->skel_nodes * sizeof *old_of);
	uint8_t *state = xcalloc(nw, 1);         /* a VAR word's side and no-faces bits (the renderer
	                                            writes them into the word) */
	for (uint32_t n = 0; n < b->nn; n++)
		if (b->w->world[n])
			old_of[b->w->new_of[n]] = n;
#define NODE_OF(ofs) old_of[(uint32_t) ((ofs) + (int32_t) node_of_ofs_base) / sizeof(qw_node)]
	while (i < nw) {
		if (++steps > nw * 4)
			die("the check's interpreter loops");
		uint32_t w = words[i++], t = QWSQ_TYPE(w);
		int32_t ofs = QWSQ_OFS(w);
		if (t == QWSQ_LEAF) {
			uint32_t l = (uint32_t) ofs / sizeof(qw_leaf);
			if (vis_leaf[l]) {
				EV_TRACE(e, i - 1, "leaf", w);
				ev_put(e, EV_LEAF(l));
			}
		} else if (t == QWSQ_BLOCK) {
			uint32_t n = NODE_OF(ofs);
			if (vis_node[n]) {               /* (out of the PVS: no record of it stamped, nothing drawn) */
				EV_TRACE(e, i - 1, "block word", w);
				ev_put(e, EV_BLOCK(n, (w & QWSQ_FLAG) != 0));
			}
		} else if (t == QWSQ_NODE) {
			uint32_t n = NODE_OF(ofs), len = words[i++];
			if (!vis_node[n]) {
				i += len;
			} else if (w & QWSQ_FLAG) {
				if (sp >= QW_SEQ_MAX_DEPTH + 1)
					die("the check: too deep");
				stack[sp++] = 0;
			}
		} else if ((w & ~1u) == QWSQ_ENDN) {
			if (w & 1)
				break;                       /* STOP */
			if (!sp)
				die("the check: an ENDN with no frame");
			sp--;
		} else if ((w >> 28) == (QWSQ_ENDF >> 28) || (w >> 28) == (QWSQ_ENDB >> 28)) {
			uint32_t at = (i - 1) - (w & 0xffffffu), vw = words[at], lens = words[at + 1];
			uint32_t n = NODE_OF(QWSQ_OFS(vw));
			int behind = state[at] & 1, noface = state[at] & 2;
			if ((w >> 28) == (QWSQ_ENDF >> 28)) {
				if (behind) {
					i += lens >> 16;         /* F last: past B */
				} else if (!noface && vis_node[n] && b->block[n * 2]) {
					EV_TRACE(e, at, "var end front", vw);
					ev_put(e, EV_BLOCK(n, 0));
				}
			} else if (behind) {
				i = at + 2;                  /* B done: the back block, then F */
				if (!noface && vis_node[n] && b->block[n * 2 + 1]) {
					EV_TRACE(e, at, "var end back", vw);
					ev_put(e, EV_BLOCK(n, 1));
				}
			}
		} else {                             /* VAR */
			uint32_t n = NODE_OF(ofs);
			uint32_t at = i - 1, lens = words[i++];
			if (!vis_node[n]) {              /* (its own PVS gate: F and B skipped) */
				i += (lens & 0xffff) + (lens >> 16);
				continue;
			}
			int32_t d = cam_d(b, n, cam);
			int side = d < 0, nodraw = d + BACKFACE_EPS >= 0 && d - BACKFACE_EPS <= 0;
			state[at] = (uint8_t) (side | nodraw << 1);
			if (side)
				i += lens & 0xffff;
		}
	}
#undef NODE_OF
	free(old_of);
	free(state);
	(void) stack;
	return 1;
}

static void count_sub(const build_t *b, uint32_t n, uint32_t *leaves, uint32_t *blocks)
{
	*blocks += b->block[n * 2] + b->block[n * 2 + 1];
	for (int c = 0; c < 2; c++) {
		uint32_t ch = b->w->nodes[n].child[c];
		if (ch & 0x80000000u)
			*leaves += (ch & 0x7fffffffu) < b->nl && b->pvs[ch & 0x7fffffffu];
		else
			count_sub(b, ch, leaves, blocks);
	}
}

static void seq_check(build_t *b, const uint32_t *words, uint32_t nw, int zone)
{
	const qw_header *H = b->w->h;
	const qw_node *N = b->w->nodes;
	uint8_t *vis_leaf = xcalloc(b->nl, 1), *vis_node = xcalloc(b->nn, 1), *save = b->pvs;
	evlist et, es;
	memset(&et, 0, sizeof et);
	memset(&es, 0, sizeof es);
	cell_t *cell = xmalloc(sizeof *cell);
	uint32_t cams = 0, bad = 0;
	for (uint32_t l = 1; l <= b->nvis; l++) {
		if (b->w->leaf_zone[l] != zone || b->leaf_up[l] < 0)
			continue;
		/* the PVS of this camera leaf alone: the leaves, their ancestors */
		b->pvs = vis_leaf;
		memset(vis_leaf, 0, b->nl);
		memset(vis_node, 0, b->nn);
		pvs_union(b, l);
		for (uint32_t k = 1; k < b->nl; k++)
			if (vis_leaf[k])
				for (int n = b->leaf_up[k]; n >= 0 && !vis_node[n]; n = b->up[n])
					vis_node[n] = 1;
		/* the cell again, its corners pulled in, and its centre */
		cell_box(cell);
		uint32_t child = 0x80000000u | l;
		for (int n = b->leaf_up[l]; n >= 0; child = (uint32_t) n, n = b->up[n]) {
			const qw_plane *pl = (const qw_plane *) ((const int32_t *) b->planes + N[n].plane3);
			double nrm[3] = { pl->n[0] / 4096.0, pl->n[1] / 4096.0, pl->n[2] / 4096.0 };
			cell_clip(cell, nrm, pl->dist / (4096.0 * QW_CLIP_SUB), N[n].child[0] == child ? 1 : -1);
		}
		double ctr[3] = { 0, 0, 0 };
		int nv = 0;
		for (int i = 0; i < cell->nf; i++)
			for (int j = 0; j < cell->f[i].n; j++, nv++)
				for (int k = 0; k < 3; k++)
					ctr[k] += cell->f[i].v[j][k];
		if (!nv)
			continue;
		for (int k = 0; k < 3; k++)
			ctr[k] /= nv;
		for (int i = -1; i < cell->nf; i++) {
			for (int j = 0; j < (i < 0 ? 1 : cell->f[i].n); j++) {
				int32_t cam[3];
				for (int k = 0; k < 3; k++) {
					double v = i < 0 ? ctr[k] : ctr[k] + (cell->f[i].v[j][k] - ctr[k]) * 0.9;
					cam[k] = (int32_t) (v * QW_VSCALE);   /* (vertex units, whole: the engine's) */
				}
				/* the point must be in this leaf by the engine's descent, else it is skipped */
				uint32_t at = H->root;
				while (!(at & 0x80000000u))
					at = N[at].child[cam_d(b, at, cam) <= 0];
				if ((at & 0x7fffffffu) != l)
					continue;
				cams++;
				et.n = es.n = 0;
				if (!(H->root & 0x80000000u))
					tree_events(b, H->root, vis_leaf, vis_node, cam, &et);
				seq_trace = !bad && env_int("QCOOK_SEQ_DEBUG", 0);
				seq_events(b, words, nw, vis_leaf, vis_node, cam, &es);
				seq_trace = 0;
				uint32_t k = 0;
				while (k < et.n && k < es.n && et.ev[k] == es.ev[k])
					k++;
				if (k != et.n || k != es.n) {
					if (bad++ < 4)
						printf("qcook:   zone %d: the cooked walk differs from the tree's at camera %d %d %d (leaf %u): "
						       "event %u of %u / %u: tree %08x, cooked %08x\n", zone, cam[0], cam[1], cam[2], l, k,
						       et.n, es.n, k < et.n ? et.ev[k] : 0, k < es.n ? es.ev[k] : 0);
					if (bad == 1 && env_int("QCOOK_SEQ_DEBUG", 0)) {
						printf("qcook:   tree:  ");
						for (uint32_t q = 0; q < et.n && q < 14; q++) printf(" %08x", et.ev[q]);
						printf("\nqcook:   cooked:");
						for (uint32_t q = 0; q < es.n && q < 14; q++) printf(" %08x", es.ev[q]);
						printf("\n");
						uint32_t bn = k < es.n && es.ev[k] & 0x80000000u ? (es.ev[k] & 0x7fffffffu) >> 1 : 0;
						printf("qcook:   node %u: cls %d, d %d, children %08x %08x, n_ent %u %u, block %u %u, vis_node %u\n", bn,
						       b->cls[bn], cam_d(b, bn, cam), N[bn].child[0], N[bn].child[1],
						       (N[bn].child[0] & 0x80000000u) ? 0 : b->n_ent[N[bn].child[0]],
						       (N[bn].child[1] & 0x80000000u) ? 0 : b->n_ent[N[bn].child[1]], b->block[bn * 2], b->block[bn * 2 + 1],
						       vis_node[bn]);
						for (uint32_t q = 0; q < 12 && q < nw; q++) printf(" w%u=%08x", q, words[q]);
						printf("\n");
						for (int c = 0; c < 2; c++) {
							uint32_t ch = N[bn].child[c], cl = 0, cb = 0;
							if (ch & 0x80000000u)
								continue;
							b->pvs = save;
							count_sub(b, ch, &cl, &cb);
							uint32_t ul = cl, ub = cb;
							cl = cb = 0;
							b->pvs = vis_leaf;
							count_sub(b, ch, &cl, &cb);
							printf("qcook:   child %u (node %u): union %u leaves %u blocks; this camera's PVS %u leaves %u blocks; "
							       "n_leaf %u n_block %u n_ent %u world %u\n", c, ch, ul, ub, cl, cb, b->n_leaf[ch], b->n_block[ch],
							       b->n_ent[ch], b->w->world[ch]);
						}
					}
				}
			}
		}
	}
	printf("qcook:   zone %d: the cooked walk checked against the tree walk at %u cameras: %u differ\n", zone, cams,
	       bad);
	b->pvs = save;
	free(vis_leaf); free(vis_node); free(cell); free(et.ev); free(es.ev);
}

int seq_build(const seq_world *w, int zone, const uint8_t *member, buf_t *out, seq_stats *st)
{
	const qw_header *H = w->h;
	build_t bb, *b = &bb;
	memset(b, 0, sizeof *b);
	memset(st, 0, sizeof *st);
	b->w = w;
	b->st = st;
	b->member = member;
	b->planes = (const qw_plane *) (w->file + H->ofs_clip + sizeof(qw_clip_header));
	b->nn = H->num_nodes;
	b->nl = H->num_leaves;
	b->nvis = H->num_visleafs < H->num_leaves ? H->num_visleafs : H->num_leaves - 1;
	b->fast = env_int("QCOOK_SEQ_FAST", 0) != 0;
	b->test_score = env_int("QCOOK_SEQ_TEST", 200);
	b->prune_min = env_int("QCOOK_SEQ_PRUNE", 2);
	const qw_node *N = w->nodes;

	b->pvs     = xcalloc(b->nl, 1);
	b->block   = xcalloc(b->nn * 2, 1);
	b->nrec    = xcalloc(b->nn * 2, sizeof *b->nrec);
	b->cls     = xmalloc(b->nn);
	b->n_leaf  = xcalloc(b->nn, sizeof *b->n_leaf);
	b->n_block = xcalloc(b->nn, sizeof *b->n_block);
	b->n_rec   = xcalloc(b->nn, sizeof *b->n_rec);
	b->n_ent   = xcalloc(b->nn, sizeof *b->n_ent);
	b->vdepth  = xcalloc(b->nn, 1);
	b->up      = xmalloc(b->nn * sizeof *b->up);
	b->leaf_up = xmalloc(b->nl * sizeof *b->leaf_up);
	b->lofs    = xmalloc(b->nl * sizeof *b->lofs);
	b->nofs    = xmalloc(b->nn * sizeof *b->nofs);
	for (uint32_t n = 0; n < b->nn; n++) {
		b->up[n] = -1;
		b->cls[n] = -1;
	}
	for (uint32_t l = 0; l < b->nl; l++)
		b->leaf_up[l] = -1;
	for (uint32_t n = 0; n < b->nn; n++) {
		if (!w->world[n])
			continue;
		for (int c = 0; c < 2; c++) {
			uint32_t ch = N[n].child[c];
			if (ch & 0x80000000u) {
				if ((ch & 0x7fffffffu) < b->nl)
					b->leaf_up[ch & 0x7fffffffu] = (int) n;
			} else {
				b->up[ch] = (int) n;
			}
		}
		/* the blocks with chunk records */
		for (int sd = 0; sd < 2; sd++) {
			uint32_t at = N[n].faces[sd], end = sd ? N[n + 1].faces[0] : N[n].faces[1];
			while (at < end) {
				const qw_record *r = (const qw_record *) (w->faces + at);
				if (member[r->face_id]) {
					b->block[n * 2 + sd] = 1;
					b->nrec[n * 2 + sd]++;
				}
				at += r->size;
			}
		}
		b->nofs[n] = (int32_t) ((w->new_of[n] - w->skel_nodes) * (int32_t) sizeof(qw_node));
	}
	for (uint32_t l = 0; l < b->nl; l++)
		b->lofs[l] = (int32_t) (l * sizeof(qw_leaf));

	/* the camera leaves: their PVS union; their cells, and each node
	   plane's distance range over them */
	double *dmin = xmalloc(b->nn * sizeof *dmin), *dmax = xmalloc(b->nn * sizeof *dmax);
	uint8_t *touch_front = xcalloc(b->nn, 1);   /* a cell touches the plane from the front, the
	                                               node not its ancestor */
	uint8_t *anc = xmalloc(b->nn);              /* a cell's ancestors (per cell) */
	uint8_t *anc_any = xcalloc(b->nn, 1);       /* an ancestor of some cell */
	for (uint32_t n = 0; n < b->nn; n++) {
		dmin[n] = 1e30;
		dmax[n] = -1e30;
	}
	cell_t *cell = xmalloc(sizeof *cell);
	uint32_t ncells = 0;
	for (uint32_t l = 1; l <= b->nvis; l++) {
		if (w->leaf_zone[l] != zone || b->leaf_up[l] < 0)
			continue;
		pvs_union(b, l);
		ncells++;
		/* the cell: the box cut by the ancestors' half-spaces */
		cell_box(cell);
		memset(anc, 0, b->nn);
		uint32_t child = 0x80000000u | l;
		for (int n = b->leaf_up[l]; n >= 0; child = (uint32_t) n, n = b->up[n]) {
			const qw_plane *pl = (const qw_plane *) ((const int32_t *) b->planes + N[n].plane3);
			double nrm[3] = { pl->n[0] / 4096.0, pl->n[1] / 4096.0, pl->n[2] / 4096.0 };
			double dd = pl->dist / (4096.0 * QW_CLIP_SUB);
			int side = N[n].child[0] == child ? 0 : 1;
			cell_clip(cell, nrm, dd, side ? -1 : 1);
			anc[n] = 1;
			anc_any[n] = 1;
		}
		/* every world node's plane over the cell's corners: d as the walk
		   computes it (the camera in vertex units, x4096) */
		for (uint32_t n = 0; n < b->nn; n++) {
			if (!w->world[n])
				continue;
			const qw_plane *pl = (const qw_plane *) ((const int32_t *) b->planes + N[n].plane3);
			double lo = 1e30, hi = -1e30, dist = (double) (pl->dist >> 1);
			for (int i = 0; i < cell->nf; i++)
				for (int j = 0; j < cell->f[i].n; j++) {
					const double *v = cell->f[i].v[j];
					double d = (pl->n[0] * v[0] + pl->n[1] * v[1] + pl->n[2] * v[2]) * QW_VSCALE - dist;
					if (d < lo) lo = d;
					if (d > hi) hi = d;
				}
			if (lo < dmin[n]) dmin[n] = lo;
			if (hi > dmax[n]) dmax[n] = hi;
			if (!anc[n] && lo <= SIDE_MARGIN && hi > -SIDE_MARGIN)
				touch_front[n] = 1;
		}
	}
	st->cells = ncells;
	if (!ncells) {
		free(cell); free(dmin); free(dmax); free(touch_front); free(anc); free(anc_any);
		goto done;                           /* (a zone with no camera leaf: no walk) */
	}
	/* the classes. QCOOK_SEQ_ZERO=0 fixes the back side of a cell's own
	   axial boundary plane too: a camera exactly on it (d = 0, the lookup
	   puts it behind, the walk takes the front order and draws no face of
	   the plane) then gets the back order, the two subtrees in the other
	   order: no pixel changes (from a point of the plane, nothing of one
	   side is seen over the other), the list's order of that frame does. */
	int zero_exact = env_int("QCOOK_SEQ_ZERO", 1) != 0;
	for (uint32_t n = 0; n < b->nn; n++) {
		if (!w->world[n])
			continue;
		const qw_plane *pl = (const qw_plane *) ((const int32_t *) b->planes + N[n].plane3);
		int axial_whole = pl->type < 3 && (pl->dist % (4096 * 2)) == 0;   /* dist / 2 a multiple of 4096 */
		if (dmin[n] > SIDE_MARGIN)
			b->cls[n] = 0;
		else if (dmax[n] < -SIDE_MARGIN)
			b->cls[n] = 1;
		else if (dmin[n] > -SIDE_MARGIN && axial_whole && !touch_front[n])
			b->cls[n] = 0;                   /* the cells before it, touching as its own descendants
			                                    (d > 0 there, a multiple of 4096: never within the epsilon) */
		else if (dmax[n] < SIDE_MARGIN && axial_whole && anc_any[n] && !zero_exact)
			b->cls[n] = 1;                   /* (behind it, touching: d = 0 or <= -4096) */
		else
			b->cls[n] = 2;
		if (b->cls[n] == 2) {
			st->var_nodes++;
			if (anc_any[n] && dmax[n] < SIDE_MARGIN)
				st->anc_vars++;              /* behind a boundary of its own */
			else if (anc_any[n])
				st->anc_front_vars++;        /* before one (not axial, or not at a whole unit) */
		}
	}
	free(cell); free(dmin); free(dmax); free(touch_front); free(anc); free(anc_any);

	subtree_counts(b);
	if (H->root & 0x80000000u)
		goto done;
	emit_node(b, H->root);
	put(b, QWSQ_STOP);
	if (b->failed) {
		printf("qcook:   zone %d: the cooked walk nests more than %d frames: none\n", zone, QW_SEQ_MAX_DEPTH);
		goto done;
	}
	st->words = b->nw;
	st->depth = (uint32_t) b->max_depth;
	if (env_int("QCOOK_SEQ_CHECK", 0))
		seq_check(b, b->words, b->nw, zone);
	qw_seq_header hd;
	memset(&hd, 0, sizeof hd);
	hd.num_words = b->nw;
	hd.depth     = (uint16_t) b->max_depth;
	hd.flags     = b->fast ? 0 : QWSQ_EXACT;
	hd.nodes     = (uint16_t) (st->nodes > 0xffff ? 0xffff : st->nodes);
	hd.vars      = (uint16_t) (st->vars > 0xffff ? 0xffff : st->vars);
	hd.leaves    = (uint16_t) (st->leaves > 0xffff ? 0xffff : st->leaves);
	hd.blocks    = (uint16_t) (st->blocks > 0xffff ? 0xffff : st->blocks);
	buf_put(out, &hd, sizeof hd);
	buf_put(out, b->words, b->nw * sizeof *b->words);
done:
	free(b->pvs); free(b->block); free(b->nrec); free(b->cls); free(b->n_leaf); free(b->n_block); free(b->n_rec);
	free(b->n_ent); free(b->vdepth); free(b->up); free(b->leaf_up); free(b->lofs); free(b->nofs); free(b->words);
	return st->words != 0;
}
