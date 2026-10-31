/*
 * qcook — an alias model's triangles reduced for a model level below high
 * (formats/qlevels.h).
 *
 * Half-edge collapses: a vertex u goes onto a neighbour v, the triangles on
 * the edge vanish and u's other triangles take v. No vertex moves, so every
 * animation frame stays Quake's own and the skin coordinates stay the
 * vertices'. The cost of u -> v is the squared distance from v to the
 * planes of the triangles u stood on, in five dimensions: the position and
 * the skin coordinates (scaled to units), so that a collapse that slides
 * the texture costs as one that bends the surface (Garland and Heckbert's
 * quadrics, with Hoppe's attributes), averaged over every frame (a
 * collapse must hold in all of them). Kept: a seam vertex goes
 * only onto another (a back face shifts both by half the skin), a vertex
 * of an open edge only along it (the outline, held by planes across those
 * edges), a vertex where the skin's
 * halves meet off the seam or the outline touches itself stays; no
 * triangle turns over or flattens in any frame, no edge gets three
 * triangles. Cheapest first, until `keep` of the triangles are left or the
 * next costs more than max_err squared.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"

#define D 5                                   /* x y z, s t */
#define QN (D * (D + 1) / 2 + D + 1)          /* A (symmetric), b, c */

typedef struct { double q[QN]; } quadric;

/* the quadric of the plane through three 5D points: the squared distance
   from x to it, |x - p0|^2 minus its parts along the plane's two axes */
static void quadric_tri(quadric *Q, const double p[3][D], double w)
{
	double e1[D], e2[D], l = 0, d = 0;
	for (int k = 0; k < D; k++) { e1[k] = p[1][k] - p[0][k]; l += e1[k] * e1[k]; }
	if (l < 1e-12) return;
	l = sqrt(l);
	for (int k = 0; k < D; k++) { e1[k] /= l; e2[k] = p[2][k] - p[0][k]; d += e1[k] * e2[k]; }
	l = 0;
	for (int k = 0; k < D; k++) { e2[k] -= d * e1[k]; l += e2[k] * e2[k]; }
	if (l < 1e-12) return;
	l = sqrt(l);
	for (int k = 0; k < D; k++) e2[k] /= l;
	double A[D][D];
	for (int i = 0; i < D; i++)
		for (int j = 0; j < D; j++)
			A[i][j] = (i == j) - e1[i] * e1[j] - e2[i] * e2[j];
	int at = 0;
	for (int i = 0; i < D; i++)
		for (int j = i; j < D; j++)
			Q->q[at++] += w * A[i][j];
	double c = 0;
	for (int i = 0; i < D; i++) {
		double Ap = 0;
		for (int j = 0; j < D; j++)
			Ap += A[i][j] * p[0][j];
		Q->q[at++] += -w * Ap;                    /* b = -A p0 */
		c += p[0][i] * Ap;
	}
	Q->q[at] += w * c;                            /* c = p0' A p0 */
}

/* a 3D plane's quadric in the 5D space (the skin coordinates free): an
   open edge's, through it and across its triangle */
static void quadric_plane3(quadric *Q, const double n[3], double d, double w)
{
	int at = 0;
	for (int i = 0; i < D; i++)
		for (int j = i; j < D; j++)
			Q->q[at++] += i < 3 && j < 3 ? w * n[i] * n[j] : 0;
	for (int i = 0; i < D; i++)
		Q->q[at++] += i < 3 ? w * d * n[i] : 0;
	Q->q[at] += w * d * d;
}

static double quadric_eval(const quadric *Q, const double x[D])
{
	double s = 0;
	int at = 0;
	for (int i = 0; i < D; i++)
		for (int j = i; j < D; j++, at++)
			s += (i == j ? 1 : 2) * Q->q[at] * x[i] * x[j];
	for (int i = 0; i < D; i++)
		s += 2 * Q->q[at++] * x[i];
	return s + Q->q[at];
}

static const double *P(const dmesh *m, int f, int v)
{
	return m->pos + ((size_t) f * m->nverts + (size_t) v) * 3;
}

/* vertex v's 5D point in frame f, its skin coordinates as a triangle
   facing front (or not) maps them */
static void point5(const dmesh *m, int f, int v, int front, double x[D])
{
	const double *p = P(m, f, v);
	x[0] = p[0]; x[1] = p[1]; x[2] = p[2];
	x[3] = (m->uv[v * 2] + (!front && m->seam && m->seam[v] ? m->skin_w / 2 : 0)) * m->uv_scale;
	x[4] = m->uv[v * 2 + 1] * m->uv_scale;
}

/* the 3D normal of (a, b, c) in frame f; 0 if degenerate */
static int tri_normal(const dmesh *m, int a, int b, int c, int f, double n[3])
{
	const double *pa = P(m, f, a), *pb = P(m, f, b), *pc = P(m, f, c);
	double u[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
	double w[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
	n[0] = u[1] * w[2] - u[2] * w[1];
	n[1] = u[2] * w[0] - u[0] * w[2];
	n[2] = u[0] * w[1] - u[1] * w[0];
	double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (l < 1e-9)
		return 0;
	for (int k = 0; k < 3; k++)
		n[k] /= l;
	return 1;
}

typedef struct { double cost; int u, v; } cand;

static int by_cost(const void *a, const void *b)
{
	const cand *x = a, *y = b;
	return x->cost < y->cost ? -1 : x->cost > y->cost;
}

/* the triangles each vertex is in (alive ones), rebuilt after a collapse */
typedef struct { int *first, *count, *list; } adjacency;

static void adj_build(adjacency *A, const int (*tri)[4], const uint8_t *alive, int n, int nv)
{
	memset(A->count, 0, (size_t) nv * sizeof *A->count);
	for (int t = 0; t < n; t++)
		if (alive[t])
			for (int k = 1; k < 4; k++)
				A->count[tri[t][k]]++;
	int at = 0;
	for (int v = 0; v < nv; v++) {
		A->first[v] = at;
		at += A->count[v];
		A->count[v] = 0;
	}
	for (int t = 0; t < n; t++)
		if (alive[t])
			for (int k = 1; k < 4; k++) {
				int v = tri[t][k];
				A->list[A->first[v] + A->count[v]++] = t;
			}
}

static int has(const int t[4], int v)
{
	return t[1] == v || t[2] == v || t[3] == v;
}

/* how many alive triangles use the edge (a, b) */
static int edge_uses(const int (*tri)[4], const adjacency *A, int a, int b)
{
	int uses = 0;
	for (int i = 0; i < A->count[a]; i++)
		uses += has(tri[A->list[A->first[a] + i]], b);
	return uses;
}

/* may u go onto v? the link condition and no triangle turned over */
static int collapse_ok(const dmesh *m, const int (*tri)[4], const adjacency *A, int u, int v)
{
	int onedge = 0;
	for (int i = 0; i < A->count[u]; i++) {
		const int *t = tri[A->list[A->first[u] + i]];
		if (has(t, v)) {
			onedge++;
			continue;
		}
		int nt[3] = { t[1], t[2], t[3] };
		for (int k = 0; k < 3; k++)
			if (nt[k] == u)
				nt[k] = v;
		for (int f = 0; f < m->nframes; f++) {
			double n0[3], n1[3];
			if (!tri_normal(m, t[1], t[2], t[3], f, n0))
				continue;
			if (!tri_normal(m, nt[0], nt[1], nt[2], f, n1) ||
			    n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] < 0.2)
				return 0;
		}
	}
	if (!onedge)
		return 0;
	/* the vertices next to both u and v: exactly the edge's triangles' third ones */
	int common = 0;
	for (int i = 0; i < A->count[u]; i++) {
		const int *t = tri[A->list[A->first[u] + i]];
		for (int k = 1; k < 4; k++) {
			int w = t[k], seen = 0;
			if (w == u || w == v)
				continue;
			for (int j = 0; j < i && !seen; j++)          /* (counted once) */
				seen = has(tri[A->list[A->first[u] + j]], w);
			for (int k2 = 1; k2 < k && !seen; k2++)
				seen = t[k2] == w;
			if (seen)
				continue;
			for (int j = 0; j < A->count[v]; j++)
				if (has(tri[A->list[A->first[v] + j]], w)) {
					common++;
					break;
				}
		}
	}
	return common == onedge;
}

int decimate(const dmesh *m, int (*tri)[4], int n, double keep, double max_err)
{
	int target = (int) ceil(n * keep);
	if (n <= target)
		return n;
	int nv = m->nverts, nf = m->nframes;
	uint8_t *alive = xcalloc((size_t) n, 1);
	uint8_t *lock  = xcalloc((size_t) nv, 1);
	int8_t  *front = xcalloc((size_t) nv, 1);        /* a vertex's triangles: 1 front, 0 back */
	quadric *Q     = xcalloc((size_t) nv * nf, sizeof *Q);
	adjacency A    = { xcalloc((size_t) nv, sizeof(int)), xcalloc((size_t) nv, sizeof(int)),
	                   xcalloc((size_t) n * 3 + 1, sizeof(int)) };
	cand *cands    = xcalloc((size_t) n * 6 + 1, sizeof *cands);
	int left = 0;
	for (int t = 0; t < n; t++) {
		alive[t] = tri[t][1] != tri[t][2] && tri[t][2] != tri[t][3] && tri[t][1] != tri[t][3];
		left += alive[t];
	}
	adj_build(&A, (const int (*)[4]) tri, alive, n, nv);

	/* kept in place: a vertex whose triangles face both ways without being
	   on the seam (the skin's halves meeting off it), a vertex on more than
	   two open edges (a corner of the outline) */
	uint8_t *border = xcalloc((size_t) nv, 1);   /* open edges at the vertex */
	for (int v = 0; v < nv; v++) {
		int f = 0, b = 0;
		for (int i = 0; i < A.count[v]; i++) {
			if (tri[A.list[A.first[v] + i]][0]) f = 1;
			else b = 1;
		}
		front[v] = (int8_t) f;
		if (f && b && !(m->seam && m->seam[v]))
			lock[v] = 1;
	}

	/* every vertex's quadric per frame: its triangles' 5D planes, by area;
	   an open edge's plane across its triangle at both ends (the outline
	   held) */
	for (int t = 0; t < n; t++) {
		if (!alive[t])
			continue;
		for (int f = 0; f < nf; f++) {
			double p[3][D], nrm[3];
			for (int k = 0; k < 3; k++)
				point5(m, f, tri[t][1 + k], tri[t][0], p[k]);
			if (!tri_normal(m, tri[t][1], tri[t][2], tri[t][3], f, nrm))
				continue;
			const double *a = P(m, f, tri[t][1]), *b = P(m, f, tri[t][2]), *c = P(m, f, tri[t][3]);
			double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, w[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			double cx = u[1] * w[2] - u[2] * w[1], cy = u[2] * w[0] - u[0] * w[2], cz = u[0] * w[1] - u[1] * w[0];
			double area = sqrt(cx * cx + cy * cy + cz * cz) / 2;
			for (int k = 1; k < 4; k++)
				quadric_tri(&Q[(size_t) tri[t][k] * nf + f], (const double (*)[D]) p, area);
			for (int e = 0; e < 3; e++) {
				int ea = tri[t][1 + e], eb = tri[t][1 + (e + 1) % 3];
				if (edge_uses((const int (*)[4]) tri, &A, ea, eb) != 1)
					continue;
				if (!f) {
					border[ea]++;
					border[eb]++;
				}
				const double *pa = P(m, f, ea), *pb = P(m, f, eb);
				double ed[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] }, bn[3];
				bn[0] = ed[1] * nrm[2] - ed[2] * nrm[1];
				bn[1] = ed[2] * nrm[0] - ed[0] * nrm[2];
				bn[2] = ed[0] * nrm[1] - ed[1] * nrm[0];
				double l = sqrt(bn[0] * bn[0] + bn[1] * bn[1] + bn[2] * bn[2]);
				if (l < 1e-9)
					continue;
				for (int k = 0; k < 3; k++)
					bn[k] /= l;
				double el2 = ed[0] * ed[0] + ed[1] * ed[1] + ed[2] * ed[2];
				double d = -(bn[0] * pa[0] + bn[1] * pa[1] + bn[2] * pa[2]);
				quadric_plane3(&Q[(size_t) ea * nf + f], bn, d, 10 * el2);
				quadric_plane3(&Q[(size_t) eb * nf + f], bn, d, 10 * el2);
			}
		}
	}
	for (int v = 0; v < nv; v++)
		if (border[v] > 2)
			lock[v] = 1;

	while (left > target) {
		/* every directed edge's cost, cheapest first, the first allowed */
		int nc = 0;
		for (int t = 0; t < n; t++) {
			if (!alive[t])
				continue;
			for (int e = 0; e < 6; e++) {
				int u = tri[t][1 + e % 3], v = tri[t][1 + (e % 3 + 1 + e / 3) % 3];
				if (lock[u] || u == v)
					continue;
				if (m->seam && m->seam[u] && !m->seam[v])
					continue;              /* a seam vertex: along the seam only */
				if (border[u] && edge_uses((const int (*)[4]) tri, &A, u, v) != 1)
					continue;              /* an outline vertex: along the outline only */
				double c = 0, x[D];
				for (int f = 0; f < nf; f++) {
					point5(m, f, v, front[u], x);
					c += quadric_eval(&Q[(size_t) u * nf + f], x);
				}
				c /= nf;
				cands[nc].cost = c;
				cands[nc].u = u;
				cands[nc].v = v;
				nc++;
			}
		}
		qsort(cands, (size_t) nc, sizeof *cands, by_cost);
		int pick = -1;
		for (int i = 0; i < nc && pick < 0 && cands[i].cost <= max_err * max_err; i++)
			if (collapse_ok(m, (const int (*)[4]) tri, &A, cands[i].u, cands[i].v))
				pick = i;
		if (pick < 0)
			break;
		int bu = cands[pick].u, bv = cands[pick].v;
		/* u onto v: the edge's triangles go, u's others take v */
		for (int i = 0; i < A.count[bu]; i++) {
			int t = A.list[A.first[bu] + i];
			if (has(tri[t], bv)) {
				alive[t] = 0;
				left--;
				continue;
			}
			for (int k = 1; k < 4; k++)
				if (tri[t][k] == bu)
					tri[t][k] = bv;
		}
		for (int f = 0; f < nf; f++)
			for (int k = 0; k < QN; k++)
				Q[(size_t) bv * nf + f].q[k] += Q[(size_t) bu * nf + f].q[k];
		adj_build(&A, (const int (*)[4]) tri, alive, n, nv);
	}

	int out = 0;
	for (int t = 0; t < n; t++)
		if (alive[t]) {
			memmove(tri[out], tri[t], sizeof tri[0]);
			out++;
		}
	free(alive);
	free(lock);
	free(border);
	free(front);
	free(Q);
	free(A.first);
	free(A.count);
	free(A.list);
	free(cands);
	return out;
}
