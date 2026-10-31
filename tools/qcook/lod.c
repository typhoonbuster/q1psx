/*
 * q1psx — a model's vertices ordered by what the eye would lose without
 * them (tools/qcook/lod.h; docs/porting-notes.md §88). A progressive mesh
 * by half-edge collapses, the cheapest first: a collapse moves a vertex
 * onto a neighbour, its triangles take the neighbour's corner, the edge's
 * own triangles go. Its cost is the larger of two distances, the worst of
 * the frames played: the vertex's move seen along the normals of its
 * triangles and across the rims at it (a border's edge, a fold's: the
 * surface ends there, the rim drawing back as the vertex moves within its
 * plane, which no normal sees), the root of the sum of the squares; and
 * the farthest of the model's own vertices the two stand for (theirs and
 * those collapsed onto them) from the surface around the neighbour after
 * the collapse, what the steps so far lose together (a tube drawn back
 * into its root ring by ring, a tip cut short), taken only for the
 * cheapest by the first (it can only raise a cost); times 3 when the skin
 * would stretch across the model (the two corners' texels more than 24
 * apart on the triangle's side of the skin); a collapse that turns any
 * triangle over in any frame is refused (docs/porting-notes.md §110).
 * The errors are made monotonic along the order; a collapse
 * takes effect beyond the depth where its error projects to the
 * tolerance (error x H / tolerance, H = 160: the view 320 wide, a
 * 90-degree field), so each band of QST_LOD_BAND units from the eye keeps
 * a prefix of the new numbering.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "lod.h"

typedef struct { double x, y, z; } v3;

static v3 pos_at(const lod_mesh *m, int f, int v)
{
	const double *p = m->pos + ((size_t) f * m->nverts + v) * 3;
	return (v3) { p[0], p[1], p[2] };
}

static v3 sub(v3 a, v3 b) { return (v3) { a.x - b.x, a.y - b.y, a.z - b.z }; }
static double dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b) { return (v3) { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }

static v3 unit(v3 n)
{
	double l = sqrt(dot(n, n));
	if (l > 1e-9)
		return (v3) { n.x / l, n.y / l, n.z / l };
	return n;
}

typedef struct {
	const lod_mesh *m;
	int    *parent;        /* -1: alive                                      */
	int    *ahead;         /* the model's vertices a vertex stands for (a    */
	int    *anext;         /* list: itself, those collapsed onto it), its    */
	int    *atail;         /* first, the next, its last                      */
	uint8_t *live;         /* a triangle still of three distinct vertices    */
	int    *tv;            /* a vertex's live triangles (evaluate's scratch) */
	double *best;          /* a vertex's cheapest collapse, < 0: none        */
	int    *best_u;        /* its target                                     */
	int    *seam;          /* its skin's factor (1 or 3)                     */
	double *key;           /* the cost it is chosen by: best, then with the  */
	uint8_t *memo;         /* memory's distance once memo is set             */
	uint8_t *fresh;        /* the cached cost is current                     */
	uint8_t *mark;         /* scratch over the vertices                      */
	int    *ew;            /* cost's scratch: the edges at the vertex (their */
	int    *en;            /* far corners), the triangles holding each, the  */
	int   (*et)[2];        /* first two (their places in tv), the triangles' */
	v3     *tn;            /* unit normals in the frame                      */
	int   (*star)[3];      /* memory's scratch: the triangles after it,      */
	int    *nearv;         /* the vertices whose triangles change (and       */
	uint8_t *near;         /* flagged)                                       */
} state;

static int mapped(const state *s, int v)
{
	while (s->parent[v] >= 0)
		v = s->parent[v];
	return v;
}

/* the triangle's corners through the collapses so far */
static void tri_verts(const state *s, int t, int c[3])
{
	for (int k = 0; k < 3; k++)
		c[k] = mapped(s, s->m->tris[t][k + 1]);
}

/* the texels of vertex v's corner on triangle t's side of the skin */
static void uv_of(const lod_mesh *m, int t, int v, int *su, int *tv)
{
	*su = m->st[v][1] + (!m->tris[t][0] && m->st[v][0] ? m->skinw / 2 : 0);
	*tv = m->st[v][2];
}

/* the cost of collapsing v onto u over its live triangles tv[ntv] (its
   skin's factor in *seamp); < 0: a triangle would turn over */
static double cost(const state *s, int v, int u, const int *tv, int ntv, int *seamp)
{
	const lod_mesh *m = s->m;
	double e2max = 0;
	int seam = 1, ne = 0;
	for (int i = 0; i < ntv; i++) {
		int c[3], c2[3];
		tri_verts(s, tv[i], c);
		for (int k = 0; k < 3; k++)
			c2[k] = c[k] == v ? u : c[k];
		int keep = c2[0] != c2[1] && c2[1] != c2[2] && c2[0] != c2[2];
		if (keep) {                               /* the skin: the corner reads u's texels */
			int sv, tvv, su2, tu;
			uv_of(m, tv[i], v, &sv, &tvv);
			uv_of(m, tv[i], u, &su2, &tu);
			if (abs(sv - su2) + abs(tvv - tu) > 24)
				seam = 3;
		}
		for (int k = 0; k < 3; k++) {             /* the edges at v, the triangles on them */
			int w = c[k], j = 0;
			if (w == v)
				continue;
			while (j < ne && s->ew[j] != w)
				j++;
			if (j == ne) {
				s->ew[ne]   = w;
				s->en[ne++] = 0;
			}
			if (s->en[j] < 2)
				s->et[j][s->en[j]] = i;
			s->en[j]++;
		}
	}
	for (int f = 0; f < m->nframes; f++) {
		double e2 = 0;
		v3 mv = sub(pos_at(m, f, u), pos_at(m, f, v));
		for (int i = 0; i < ntv; i++) {
			int c[3], c2[3];
			tri_verts(s, tv[i], c);
			v3 a = pos_at(m, f, c[0]), b = pos_at(m, f, c[1]), cc = pos_at(m, f, c[2]);
			v3 n = unit(cross(sub(b, a), sub(cc, a)));
			s->tn[i] = n;
			double d = dot(n, mv);
			e2 += d * d;
			for (int k = 0; k < 3; k++)
				c2[k] = c[k] == v ? u : c[k];
			if (c2[0] != c2[1] && c2[1] != c2[2] && c2[0] != c2[2]) {
				v3 a2 = pos_at(m, f, c2[0]), b2 = pos_at(m, f, c2[1]), cc2 = pos_at(m, f, c2[2]);
				v3 n2 = cross(sub(b2, a2), sub(cc2, a2));
				if (dot(n, n2) < 0)
					return -1;                    /* turned over */
			}
		}
		/* the rims: an edge of one triangle (a border: a tube's open end
		   inside the part it meets, the ogre's forearm in its hand, which
		   the normals let draw back into the shoulder) or of two turned
		   back on each other (a fold, the normals more than 90 degrees
		   apart: a thin sheet's edge); v's move across it within each
		   triangle's plane, a fold's by the part its other triangle's
		   normal misses (the normals' cosine squared: all of it at 180
		   degrees, none at 90), the two triangles' mean */
		for (int j = 0; j < ne; j++) {
			if (s->en[j] > 2)
				continue;
			double w = 1;
			if (s->en[j] == 2) {
				double cs = dot(s->tn[s->et[j][0]], s->tn[s->et[j][1]]);
				if (cs >= 0)
					continue;
				w = cs * cs / 2;
			}
			v3 e = sub(pos_at(m, f, s->ew[j]), pos_at(m, f, v));
			for (int k = 0; k < s->en[j]; k++) {
				double d = dot(unit(cross(e, s->tn[s->et[j][k]])), mv);
				e2 += d * d * w;
			}
		}
		if (e2 > e2max)
			e2max = e2;
	}
	*seamp = seam;
	return sqrt(e2max) * seam;
}

/* the squared distance from p to a segment */
static double seg2(v3 p, v3 a, v3 b)
{
	v3 ab = sub(b, a), ap = sub(p, a);
	double l = dot(ab, ab), t = l > 0 ? dot(ap, ab) / l : 0;
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	v3 q = sub(ap, (v3) { ab.x * t, ab.y * t, ab.z * t });
	return dot(q, q);
}

/* the squared distance from p to the triangle (a, b, c): its nearest point
   by the regions of its corners, sides and face (C. Ericson, Real-Time
   Collision Detection, 5.1.5) */
static double pt_tri2(v3 p, v3 a, v3 b, v3 c)
{
	v3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
	double d1 = dot(ab, ap), d2 = dot(ac, ap);
	if (d1 <= 0 && d2 <= 0)
		return dot(ap, ap);
	v3 bp = sub(p, b);
	double d3 = dot(ab, bp), d4 = dot(ac, bp);
	if (d3 >= 0 && d4 <= d3)
		return dot(bp, bp);
	double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0)
		return seg2(p, a, b);
	v3 cp = sub(p, c);
	double d5 = dot(ab, cp), d6 = dot(ac, cp);
	if (d6 >= 0 && d5 <= d6)
		return dot(cp, cp);
	double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0)
		return seg2(p, a, c);
	double va = d3 * d6 - d5 * d4;
	if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0)
		return seg2(p, b, c);
	double den = va + vb + vc;
	if (!(den > 1e-12)) {                          /* (flat in this frame) */
		double e = seg2(p, a, b), e2 = seg2(p, b, c), e3 = seg2(p, a, c);
		return e < e2 ? (e < e3 ? e : e3) : (e2 < e3 ? e2 : e3);
	}
	double bv = vb / den, bw = vc / den;
	v3 q = sub(ap, (v3) { ab.x * bv + ac.x * bw, ab.y * bv + ac.y * bw, ab.z * bv + ac.z * bw });
	return dot(q, q);
}

/* the memory of collapsing v onto u: the farthest of the model's vertices
   that v and the corners of its triangles stand for (those whose triangles
   change: a corner whose last triangles go, a blade's tip, takes its part
   with it) from the triangles after it with a corner among them (the
   whole model's left when none stays: a part going), the worst frame;
   from `from` (a distance the cost has already: a vertex nearer changes
   nothing, its search stops at the first triangle that near) */
static double memory(state *s, int v, int u, double from)
{
	const lod_mesh *m = s->m;
	int nn = 0, n = 0;
	for (int t = 0; t < m->ntris; t++) {
		if (!s->live[t])
			continue;
		int c[3];
		tri_verts(s, t, c);
		if (c[0] == v || c[1] == v || c[2] == v)
			for (int k = 0; k < 3; k++)
				if (!s->near[c[k]]) {
					s->near[c[k]]  = 1;
					s->nearv[nn++] = c[k];
				}
	}
	for (int all = 0; all < 2 && !n; all++)
		for (int t = 0; t < m->ntris; t++) {
			if (!s->live[t])
				continue;
			int c[3], on = 0;
			tri_verts(s, t, c);
			for (int k = 0; k < 3; k++) {
				on |= s->near[c[k]];
				if (c[k] == v)
					c[k] = u;
			}
			if ((on || all) && c[0] != c[1] && c[1] != c[2] && c[0] != c[2])
				memcpy(s->star[n++], c, sizeof c);
		}
	for (int i = 0; i < nn; i++)
		s->near[s->nearv[i]] = 0;
	double w2 = from * from;
	if (!n)
		return INFINITY;
	for (int f = 0; f < m->nframes; f++)
		for (int i = 0; i < nn; i++)
			for (int x = s->ahead[s->nearv[i]]; x >= 0; x = s->anext[x]) {
				v3 p = pos_at(m, f, x);
				double d2 = INFINITY;
				for (int j = 0; j < n && d2 > w2; j++) {
					double e = pt_tri2(p, pos_at(m, f, s->star[j][0]), pos_at(m, f, s->star[j][1]),
					                   pos_at(m, f, s->star[j][2]));
					if (e < d2)
						d2 = e;
				}
				if (d2 > w2)
					w2 = d2;
			}
	return sqrt(w2);
}

/* v's cheapest collapse onto a neighbour: best[v], best_u[v] (-1: none) */
static void evaluate(state *s, int v)
{
	const lod_mesh *m = s->m;
	int ntv = 0;
	for (int t = 0; t < m->ntris; t++) {
		if (!s->live[t])
			continue;
		int c[3];
		tri_verts(s, t, c);
		if (c[0] == v || c[1] == v || c[2] == v)
			s->tv[ntv++] = t;
	}
	s->fresh[v]  = 1;
	s->best_u[v] = -1;
	s->best[v]   = -1;
	s->memo[v]   = 0;
	if (!ntv)
		return;
	memset(s->mark, 0, (size_t) m->nverts);
	for (int i = 0; i < ntv; i++) {
		int c[3];
		tri_verts(s, s->tv[i], c);
		for (int k = 0; k < 3; k++)
			s->mark[c[k]] = 1;
	}
	s->mark[v] = 0;
	for (int u = 0; u < m->nverts; u++) {          /* (increasing: a tie goes to the lowest) */
		if (!s->mark[u])
			continue;
		int seam;
		double c = cost(s, v, u, s->tv, ntv, &seam);
		if (c >= 0 && (s->best_u[v] < 0 || c < s->best[v])) {
			s->best[v]   = c;
			s->best_u[v] = u;
			s->seam[v]   = seam;
		}
	}
	s->key[v] = s->best[v];
}

void lod_plan_make(const lod_mesh *m, double tol_px, int draw_dist, lod_plan *out)
{
	int V = m->nverts, T = m->ntris;
	state s;
	s.m      = m;
	s.parent = xmalloc((size_t) V * sizeof *s.parent);
	s.ahead  = xmalloc((size_t) V * sizeof *s.ahead);
	s.anext  = xmalloc((size_t) V * sizeof *s.anext);
	s.atail  = xmalloc((size_t) V * sizeof *s.atail);
	s.live   = xmalloc((size_t) T);
	s.tv     = xmalloc((size_t) T * sizeof *s.tv);
	s.best   = xmalloc((size_t) V * sizeof *s.best);
	s.best_u = xmalloc((size_t) V * sizeof *s.best_u);
	s.seam   = xmalloc((size_t) V * sizeof *s.seam);
	s.key    = xmalloc((size_t) V * sizeof *s.key);
	s.memo   = xcalloc((size_t) V, 1);
	s.fresh  = xcalloc((size_t) V, 1);
	s.mark   = xmalloc((size_t) V);
	s.ew     = xmalloc((size_t) 2 * T * sizeof *s.ew);
	s.en     = xmalloc((size_t) 2 * T * sizeof *s.en);
	s.et     = xmalloc((size_t) 2 * T * sizeof *s.et);
	s.tn     = xmalloc((size_t) T * sizeof *s.tn);
	s.star   = xmalloc((size_t) T * sizeof *s.star);
	s.nearv  = xmalloc((size_t) V * sizeof *s.nearv);
	s.near   = xcalloc((size_t) V, 1);
	for (int v = 0; v < V; v++) {
		s.parent[v] = -1;
		s.ahead[v]  = s.atail[v] = v;
		s.anext[v]  = -1;
	}
	memset(s.live, 1, (size_t) T);
	int nlive = T;
	for (int t = 0; t < T; t++) {                  /* (a degenerate triangle to start with: out) */
		const int *c = &m->tris[t][1];
		if (c[0] == c[1] || c[1] == c[2] || c[0] == c[2]) {
			s.live[t] = 0;
			nlive--;
		}
	}
	int   *cv   = xmalloc((size_t) V * sizeof *cv);    /* the collapses: vertex, */
	int   *cu   = xmalloc((size_t) V * sizeof *cu);    /* its target (alive then), */
	double *ce  = xmalloc((size_t) V * sizeof *ce);    /* its cost */
	int   *step = xmalloc((size_t) T * sizeof *step);  /* a triangle's vanishing collapse */
	for (int t = 0; t < T; t++)
		step[t] = -1;
	int K = 0;
	while (nlive > 4) {
		for (int v = 0; v < V; v++)
			if (s.parent[v] < 0 && (!s.fresh[v] || (s.best_u[v] >= 0 && s.parent[s.best_u[v]] >= 0)))
				evaluate(&s, v);
		/* the cheapest by its key; one without the memory yet takes it
		   (which can only raise it), and the choice is made again */
		int bv, bu;
		double bc;
		for (;;) {
			bv = bu = -1;
			bc = 0;
			for (int v = 0; v < V; v++) {
				if (s.parent[v] >= 0 || s.best_u[v] < 0)
					continue;
				if (bv < 0 || s.key[v] < bc || (s.key[v] == bc && (v < bv || (v == bv && s.best_u[v] < bu)))) {
					bv = v;
					bu = s.best_u[v];
					bc = s.key[v];
				}
			}
			if (bv < 0 || s.memo[bv])
				break;
			s.key[bv]  = memory(&s, bv, bu, s.best[bv] / s.seam[bv]) * s.seam[bv];
			s.memo[bv] = 1;
		}
		if (bv < 0)
			break;
		/* the vertices whose costs change: those of the live triangles around
		   the vertex and its target (before the collapse), the target */
		memset(s.mark, 0, (size_t) V);
		s.mark[bu] = 1;
		for (int t = 0; t < T; t++) {
			if (!s.live[t])
				continue;
			int c[3];
			tri_verts(&s, t, c);
			if (c[0] == bv || c[1] == bv || c[2] == bv || c[0] == bu || c[1] == bu || c[2] == bu)
				for (int k = 0; k < 3; k++)
					s.mark[c[k]] = 1;
		}
		s.parent[bv] = bu;
		s.anext[s.atail[bu]] = s.ahead[bv];       /* bu stands for bv's too */
		s.atail[bu] = s.atail[bv];
		for (int t = 0; t < T; t++) {
			if (!s.live[t])
				continue;
			int c[3];
			tri_verts(&s, t, c);
			if (c[0] == c[1] || c[1] == c[2] || c[0] == c[2]) {
				s.live[t] = 0;
				step[t]   = K;
				nlive--;
			}
		}
		/* (a vertex next to one whose triangles changed: its memory
		   again when wanted; near is all 0 between the uses) */
		for (int t = 0; t < T; t++) {
			if (!s.live[t])
				continue;
			int c[3];
			tri_verts(&s, t, c);
			if (s.mark[c[0]] || s.mark[c[1]] || s.mark[c[2]])
				s.near[c[0]] = s.near[c[1]] = s.near[c[2]] = 1;
		}
		for (int v = 0; v < V; v++) {
			if (s.mark[v])
				s.fresh[v] = 0;
			else if (s.near[v]) {
				s.key[v]  = s.best[v];
				s.memo[v] = 0;
			}
		}
		memset(s.near, 0, (size_t) V);
		cv[K] = bv;
		cu[K] = bu;
		ce[K] = bc;
		K++;
	}

	/* the errors monotonic; each collapse's depth */
	out->nverts    = V;
	out->ntris     = T;
	out->ncollapse = K;
	out->err       = xmalloc((size_t) (K ? K : 1) * sizeof *out->err);
	double emax = 0, h = 160.0;
	int kdist = 0;
	for (int i = 0; i < K; i++) {
		if (ce[i] > emax)
			emax = ce[i];
		out->err[i] = emax;
		if (emax * h / tol_px < draw_dist)
			kdist = i + 1;
	}
	out->nerr_dist = kdist;
	/* the bands: band b (its depths from b x QST_LOD_BAND) takes the
	   collapses whose depth is under its start; band 0 none */
	int kb[QST_LOD_BANDS];
	for (int b = 0; b < QST_LOD_BANDS; b++) {
		double d0 = (double) b * QST_LOD_BAND;
		int k = 0;
		if (b)
			while (k < K && out->err[k] * h / tol_px < d0)
				k++;
		kb[b] = k;
		out->nv[b] = (uint16_t) (V - k);
	}
	/* the numbering: the vertices never collapsed first (in their order),
	   then the collapses from the last to the first */
	out->perm = xmalloc((size_t) V * sizeof *out->perm);
	out->rank = xmalloc((size_t) V * sizeof *out->rank);
	int n = 0;
	for (int v = 0; v < V; v++)
		if (s.parent[v] < 0)
			out->perm[n++] = v;
	for (int i = K - 1; i >= 0; i--)
		out->perm[n++] = cv[i];
	if (n != V)
		die("lod: %d of %d vertices numbered", n, V);
	for (int i = 0; i < V; i++)
		out->rank[out->perm[i]] = i;
	out->parent = xmalloc((size_t) V * sizeof *out->parent);
	for (int v = 0; v < V; v++)
		out->parent[v] = (uint16_t) QST_LOD_NONE;
	for (int i = 0; i < K; i++) {
		int p = out->rank[cu[i]], c = out->rank[cv[i]];
		if (p >= c)
			die("lod: the parent of vertex %d is %d, not a lower index", c, p);
		out->parent[c] = (uint16_t) p;
	}
	/* a triangle's band: the first whose collapses take it; QST_LOD_BANDS
	   when none within the draw distance */
	out->group = xmalloc((size_t) T);
	for (int t = 0; t < T; t++) {
		int g = QST_LOD_BANDS;
		if (step[t] >= 0)
			for (int b = 1; b < QST_LOD_BANDS; b++)
				if (kb[b] > step[t]) {
					g = b;
					break;
				}
		out->group[t] = (uint8_t) g;
	}
	free(cv);
	free(cu);
	free(ce);
	free(step);
	free(s.parent);
	free(s.ahead);
	free(s.anext);
	free(s.atail);
	free(s.live);
	free(s.tv);
	free(s.best);
	free(s.best_u);
	free(s.seam);
	free(s.key);
	free(s.memo);
	free(s.fresh);
	free(s.mark);
	free(s.ew);
	free(s.en);
	free(s.et);
	free(s.tn);
	free(s.star);
	free(s.nearv);
	free(s.near);
}

void lod_plan_free(lod_plan *p)
{
	free(p->perm);
	free(p->rank);
	free(p->group);
	free(p->parent);
	free(p->err);
	memset(p, 0, sizeof *p);
}
