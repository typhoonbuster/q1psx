/*
 * q1psx — a model's vertices ordered by what the eye would lose without
 * them (tools/qcook/lod.h; docs/porting-notes.md §88). A progressive mesh
 * by half-edge collapses, the cheapest first: a collapse moves a vertex
 * onto a neighbour, its triangles take the neighbour's corner, the edge's
 * own triangles go. Its cost is the vertex's move seen along the normals
 * of its triangles (the root of the sum of the squares), the worst of the
 * frames played, plus the error the vertex already carries from earlier
 * collapses onto it, times 3 when the skin would stretch across the model
 * (the two corners' texels more than 24 apart on the triangle's side of
 * the skin); a collapse that turns any triangle over in any frame is
 * refused. The errors are made monotonic along the order; a collapse
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
	double *acc;           /* the error a vertex carries                     */
	uint8_t *live;         /* a triangle still of three distinct vertices    */
	int    *tv;            /* a vertex's live triangles (evaluate's scratch) */
	double *best;          /* a vertex's cheapest collapse, < 0: none        */
	int    *best_u;        /* its target                                     */
	uint8_t *fresh;        /* the cached cost is current                     */
	uint8_t *mark;         /* scratch over the vertices                      */
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

/* the cost of collapsing v onto u over its live triangles tv[ntv]; < 0: a
   triangle would turn over */
static double cost(const state *s, int v, int u, const int *tv, int ntv)
{
	const lod_mesh *m = s->m;
	double e2max = 0;
	int seam = 1;
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
	}
	for (int f = 0; f < m->nframes; f++) {
		double e2 = 0;
		v3 mv = sub(pos_at(m, f, u), pos_at(m, f, v));
		for (int i = 0; i < ntv; i++) {
			int c[3], c2[3];
			tri_verts(s, tv[i], c);
			v3 a = pos_at(m, f, c[0]), b = pos_at(m, f, c[1]), cc = pos_at(m, f, c[2]);
			v3 n = unit(cross(sub(b, a), sub(cc, a)));
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
		if (e2 > e2max)
			e2max = e2;
	}
	return (sqrt(e2max) + s->acc[v]) * seam;
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
		double c = cost(s, v, u, s->tv, ntv);
		if (c >= 0 && (s->best_u[v] < 0 || c < s->best[v])) {
			s->best[v]   = c;
			s->best_u[v] = u;
		}
	}
}

void lod_plan_make(const lod_mesh *m, double tol_px, int draw_dist, lod_plan *out)
{
	int V = m->nverts, T = m->ntris;
	state s;
	s.m      = m;
	s.parent = xmalloc((size_t) V * sizeof *s.parent);
	s.acc    = xcalloc((size_t) V, sizeof *s.acc);
	s.live   = xmalloc((size_t) T);
	s.tv     = xmalloc((size_t) T * sizeof *s.tv);
	s.best   = xmalloc((size_t) V * sizeof *s.best);
	s.best_u = xmalloc((size_t) V * sizeof *s.best_u);
	s.fresh  = xcalloc((size_t) V, 1);
	s.mark   = xmalloc((size_t) V);
	for (int v = 0; v < V; v++)
		s.parent[v] = -1;
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
		int bv = -1, bu = -1;
		double bc = 0;
		for (int v = 0; v < V; v++) {
			if (s.parent[v] >= 0 || s.best_u[v] < 0)
				continue;
			if (bv < 0 || s.best[v] < bc || (s.best[v] == bc && (v < bv || (v == bv && s.best_u[v] < bu)))) {
				bv = v;
				bu = s.best_u[v];
				bc = s.best[v];
			}
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
		if (bc > s.acc[bu])
			s.acc[bu] = bc;
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
		for (int v = 0; v < V; v++)
			if (s.mark[v])
				s.fresh[v] = 0;
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
	free(s.acc);
	free(s.live);
	free(s.tv);
	free(s.best);
	free(s.best_u);
	free(s.fresh);
	free(s.mark);
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
