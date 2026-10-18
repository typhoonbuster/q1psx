/*
 * qcook — world: QWV2 file (formats/qworld.h).
 *
 * Faces are attached to their BSP node (front block / back block), cut on
 * texture-repeat boundaries so every piece's UVs stay inside one VRAM tile,
 * triangulated into PS1 quads (+ a triangle when odd) and grouped into face
 * records of <= 63 shared vertices.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qworld.h"

/* -------------------------------------------------------------- buffers -- */

static int16_t clamp16(long v)
{
	return (int16_t) (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* ------------------------------------------------------- face splitting -- */

#define MAXPV 64

typedef struct { double p[3]; double s, t; } pvert;
typedef struct { int n; pvert v[MAXPV]; } poly_t;

/* Keep the part of `in` where (sign * attr) >= sign * value. attr: 0 = s, 1 = t */
static void clip_poly(const poly_t *in, poly_t *out, int attr, double value, int sign)
{
	out->n = 0;
	for (int i = 0; i < in->n; i++) {
		const pvert *a = &in->v[i], *b = &in->v[(i + 1) % in->n];
		double da = sign * ((attr ? a->t : a->s) - value);
		double db = sign * ((attr ? b->t : b->s) - value);
		if (da >= 0 && out->n < MAXPV)
			out->v[out->n++] = *a;
		if ((da >= 0) != (db >= 0) && out->n < MAXPV) {
			double f = da / (da - db);
			pvert *o = &out->v[out->n++];
			for (int k = 0; k < 3; k++)
				o->p[k] = a->p[k] + f * (b->p[k] - a->p[k]);
			o->s = a->s + f * (b->s - a->s);
			o->t = a->t + f * (b->t - a->t);
			if (attr) o->t = value; else o->s = value;   /* exact on the line */
		}
	}
}

static double poly_area(const poly_t *pl)
{
	double area = 0;
	for (int i = 1; i + 1 < pl->n; i++) {
		double a[3], b[3];
		for (int k = 0; k < 3; k++) {
			a[k] = pl->v[i].p[k] - pl->v[0].p[k];
			b[k] = pl->v[i + 1].p[k] - pl->v[0].p[k];
		}
		double cx = a[1] * b[2] - a[2] * b[1];
		double cy = a[2] * b[0] - a[0] * b[2];
		double cz = a[0] * b[1] - a[1] * b[0];
		area += 0.5 * sqrt(cx * cx + cy * cy + cz * cz);
	}
	return area;
}

/* Drop consecutive vertices closer than 1/16 unit. */
static void poly_weld(poly_t *pl)
{
	int n = 0;
	for (int i = 0; i < pl->n; i++) {
		const pvert *a = &pl->v[i];
		const pvert *b = n ? &pl->v[n - 1] : NULL;
		if (b && fabs(a->p[0] - b->p[0]) < 1.0 / 16 && fabs(a->p[1] - b->p[1]) < 1.0 / 16 &&
		    fabs(a->p[2] - b->p[2]) < 1.0 / 16)
			continue;
		pl->v[n++] = *a;
	}
	while (n > 1 && fabs(pl->v[0].p[0] - pl->v[n - 1].p[0]) < 1.0 / 16 &&
	       fabs(pl->v[0].p[1] - pl->v[n - 1].p[1]) < 1.0 / 16 &&
	       fabs(pl->v[0].p[2] - pl->v[n - 1].p[2]) < 1.0 / 16)
		n--;
	pl->n = n;
}

/* ------------------------------------------------------- record builder -- */

typedef struct { int16_t x, y, z; uint8_t c; } vkey;   /* c: vertex light */

typedef struct {
	int     nv;
	vkey    v[QW_MAX_VERTS];
	int     nq, nt;
	qw_quad q[256];
	qw_tri  t[256];
	int     faces[256];     /* Quake faces with pieces in this record */
	int     nfaces;
	double  mn[3], mx[3];   /* bbox of those faces (world units)      */
} rec_t;

/* A record holding several faces stays about one qbsp face in size: its
   bounding sphere is the renderer's frustum test, and a bigger one lets
   off-screen neighbours through. */
#define REC_MAX_RADIUS 160.0

static void face_bbox(const bsp_t *b, int f, double mn[3], double mx[3])
{
	const bsp_face *bf = &b->faces[f];
	for (int k = 0; k < 3; k++) { mn[k] = 1e30; mx[k] = -1e30; }
	for (int e = 0; e < bf->numedges; e++) {
		int se = b->surfedges[bf->firstedge + e];
		int vi = se >= 0 ? b->edges[se * 2] : b->edges[-se * 2 + 1];
		for (int k = 0; k < 3; k++) {
			double v = b->verts[vi * 3 + k];
			if (v < mn[k]) mn[k] = v;
			if (v > mx[k]) mx[k] = v;
		}
	}
}

typedef struct {
	buf_t    stream;
	uint32_t records, quads, tris, verts, pieces, faces_split, pieces_eared;
	vkey    *pool;          /* the map's distinct vertices          */
	int      npool, cap;
	int     *hash;          /* open addressing: pool index + 1      */
	int      hsize;         /* power of two                         */
	int     *pair_face;     /* (face, record) pairs: which records  */
	int     *pair_rec;      /* draw a piece of which face           */
	int      npairs, pcap;
	uint32_t wrecords;      /* the world's records: visibility ids  */
	int      bmodel;        /* the brush model being emitted, 0 = world */
} out_t;

/* index of vertex k in the map's pool, adding it if new */
static int pool_index(out_t *o, vkey k)
{
	if (2 * (o->npool + 1) > o->hsize) {               /* grow + rehash */
		int nh = o->hsize ? o->hsize * 2 : 1 << 14;
		int *h = xcalloc((size_t) nh, sizeof *h);
		for (int i = 0; i < o->npool; i++) {
			uint32_t x = ((uint32_t) (uint16_t) o->pool[i].x * 73856093u) ^
			             ((uint32_t) (uint16_t) o->pool[i].y * 19349663u) ^
			             ((uint32_t) (uint16_t) o->pool[i].z * 83492791u);
			uint32_t at = x & (uint32_t) (nh - 1);
			while (h[at]) at = (at + 1) & (uint32_t) (nh - 1);
			h[at] = i + 1;
		}
		free(o->hash);
		o->hash  = h;
		o->hsize = nh;
	}
	uint32_t x = ((uint32_t) (uint16_t) k.x * 73856093u) ^ ((uint32_t) (uint16_t) k.y * 19349663u) ^
	             ((uint32_t) (uint16_t) k.z * 83492791u);
	uint32_t at = x & (uint32_t) (o->hsize - 1);
	while (o->hash[at]) {
		const vkey *p = &o->pool[o->hash[at] - 1];
		if (p->x == k.x && p->y == k.y && p->z == k.z)
			return o->hash[at] - 1;
		at = (at + 1) & (uint32_t) (o->hsize - 1);
	}
	if (o->npool >= 65535)
		die("more than 65535 distinct vertices");
	if (o->npool == o->cap) {
		o->cap  = o->cap ? o->cap * 2 : 4096;
		o->pool = realloc(o->pool, (size_t) o->cap * sizeof *o->pool);
		if (!o->pool) die("out of memory");
	}
	o->pool[o->npool] = k;
	o->hash[at] = o->npool + 1;
	return o->npool++;
}

static int rec_find_or_add(rec_t *r, vkey k)
{
	for (int i = 0; i < r->nv; i++)
		if (r->v[i].x == k.x && r->v[i].y == k.y && r->v[i].z == k.z && r->v[i].c == k.c)
			return i;
	if (r->nv >= QW_MAX_VERTS)
		return -1;
	r->v[r->nv] = k;
	return r->nv++;
}

/* A record's visibility id is its own index: the walk stamps it when any
   leaf marking one of its faces is visible (marks are rewritten from faces
   to records). */
static void rec_flush(out_t *o, rec_t *r, const tex_slot *s)
{
	if (!r->nq && !r->nt) {
		r->nv = 0;
		return;
	}

	qw_record h;
	if (o->records >= 65535)
		die("more than 65535 face records");
	/* a brush model's records have no visibility slot: face_id 0 (the
	   renderer's stamp for a model's records), no leaf pairs */
	h.face_id = o->bmodel ? 0 : (uint16_t) o->wrecords++;
	for (int i = 0; i < r->nfaces && !o->bmodel; i++) {
		if (o->npairs == o->pcap) {
			o->pcap      = o->pcap ? o->pcap * 2 : 4096;
			o->pair_face = realloc(o->pair_face, (size_t) o->pcap * sizeof *o->pair_face);
			o->pair_rec  = realloc(o->pair_rec, (size_t) o->pcap * sizeof *o->pair_rec);
			if (!o->pair_face || !o->pair_rec) die("out of memory");
		}
		o->pair_face[o->npairs] = r->faces[i];
		o->pair_rec[o->npairs]  = (int) o->records;
		o->npairs++;
	}
	r->nfaces = 0;
	for (int k = 0; k < 3; k++) { r->mn[k] = 1e30; r->mx[k] = -1e30; }
	h.num_verts = (uint8_t) r->nv;
	h.num_quads = (uint8_t) r->nq;
	h.num_tris  = (uint8_t) r->nt;
	h.flags     = s->flags;
	h.tex       = (uint16_t) s->index;
	int nidx    = (r->nv + 2) / 3 * 3;                /* padded index count */
	int ibytes  = (nidx * 2 + 3) & ~3;
	int cbytes  = (r->nv + 3) & ~3;                  /* light bytes: polys - this */
	h.polys     = (uint16_t) (sizeof h + ibytes + cbytes);
	h.size      = (uint16_t) (h.polys + r->nq * sizeof(qw_quad) + r->nt * sizeof(qw_tri));

	/* bounding sphere: bbox centre, radius = farthest vertex (rounded up) */
	long lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 };
	for (int i = 0; i < r->nv; i++) {
		long p[3] = { r->v[i].x, r->v[i].y, r->v[i].z };
		for (int k = 0; k < 3; k++) {
			if (p[k] < lo[k]) lo[k] = p[k];
			if (p[k] > hi[k]) hi[k] = p[k];
		}
	}
	for (int k = 0; k < 3; k++)
		h.sc[k] = (int16_t) ((lo[k] + hi[k]) / 2);
	double rad = 0;
	for (int i = 0; i < r->nv; i++) {
		double dx = r->v[i].x - h.sc[0], dy = r->v[i].y - h.sc[1], dz = r->v[i].z - h.sc[2];
		double d = sqrt(dx * dx + dy * dy + dz * dz);
		if (d > rad) rad = d;
	}
	if (rad > 65000) die("record radius %.0f too large", rad);
	h.sr = (uint16_t) ceil(rad + 1);
	buf_put(&o->stream, &h, sizeof h);
	uint16_t idx[QW_MAX_VERTS + 3] = { 0 };
	for (int i = 0; i < r->nv; i++)
		idx[i] = (uint16_t) pool_index(o, r->v[i]);
	for (int i = r->nv; i < nidx; i++)
		idx[i] = idx[0];
	buf_put(&o->stream, idx, (size_t) ibytes);
	uint8_t col[QW_MAX_VERTS + 4] = { 0 };       /* vertex light, per slot */
	for (int i = 0; i < r->nv; i++)
		col[i] = r->v[i].c;
	buf_put(&o->stream, col, (size_t) cbytes);
	buf_put(&o->stream, r->q, (size_t) r->nq * sizeof(qw_quad));
	buf_put(&o->stream, r->t, (size_t) r->nt * sizeof(qw_tri));

	o->records++;
	o->quads += (uint32_t) r->nq;
	o->tris  += (uint32_t) r->nt;
	o->verts += (uint32_t) r->nv;
	r->nv = r->nq = r->nt = 0;
}

/* A wrapped axis gets its virtual 0..256 coordinate (the texture window
   folds it onto the texture); another axis its page position. */
static uint16_t uvword(const tex_slot *s, double u, double v)
{
	long uu = (s->win_x ? 0 : s->u0) + lround(u), vv = (s->win_y ? 0 : s->v0) + lround(v);
	if (uu < 0) uu = 0;
	if (uu > 255) uu = 255;
	if (vv < 0) vv = 0;
	if (vv > 255) vv = 255;
	return (uint16_t) (uu | (vv << 8));
}

/* twice the area of triangle (a, b, c), squared, in pool units */
static double tri_area2(const vkey *a, const vkey *b, const vkey *c)
{
	double u[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
	double v[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
	double cx = u[1] * v[2] - u[2] * v[1], cy = u[2] * v[0] - u[0] * v[2], cz = u[0] * v[1] - u[1] * v[0];
	return cx * cx + cy * cy + cz * cz;
}

/* Ear clipping of a convex piece that has collinear vertices on its boundary
   (T-junction vertices, its own or inserted by tj_repair): clip the corner
   with the largest triangle each time. Every boundary vertex stays on the
   outline, so a neighbour that has it meets this piece edge for edge. A fan
   would pair up collinear corners into zero-area triangles and draw the
   boundary as one segment over them. Returns the triangle count. */
static int ear_clip(const vkey *k, int n, int (*tri)[3])
{
	int ring[MAXPV], nt = 0;
	for (int i = 0; i < n; i++)
		ring[i] = i;
	while (n > 3) {
		int best = -1;
		double best_a = 0.25;
		for (int i = 0; i < n; i++) {
			double a = tri_area2(&k[ring[(i + n - 1) % n]], &k[ring[i]], &k[ring[(i + 1) % n]]);
			if (a <= best_a)
				continue;
			/* not if the rest would be flat (it would lose its vertices) */
			int flat = 1;
			for (int j = 0; j < n && flat; j++) {
				if (j == i)
					continue;
				int jp = (j + n - 1) % n, jn = (j + 1) % n;
				if (jp == i) jp = (jp + n - 1) % n;
				if (jn == i) jn = (jn + 1) % n;
				flat = tri_area2(&k[ring[jp]], &k[ring[j]], &k[ring[jn]]) <= 0.25;
			}
			if (flat)
				continue;
			best_a = a;
			best = i;
		}
		if (best < 0)
			return nt;                       /* the rest is flat: no pixel */
		tri[nt][0] = ring[(best + n - 1) % n];
		tri[nt][1] = ring[best];
		tri[nt][2] = ring[(best + 1) % n];
		nt++;
		for (int i = best; i + 1 < n; i++)
			ring[i] = ring[i + 1];
		n--;
	}
	if (tri_area2(&k[ring[0]], &k[ring[1]], &k[ring[2]]) > 0.25) {
		tri[nt][0] = ring[0]; tri[nt][1] = ring[1]; tri[nt][2] = ring[2];
		nt++;
	}
	return nt;
}

/* Would the fan of quads from vertex 0 draw a zero-area triangle? (Then a
   boundary vertex is skipped: see ear_clip.) */
static int quad_fan_degenerate(const vkey *k, int n)
{
	int i = 1;
	for (; i + 2 < n; i += 2)                /* GPU halves (0, i, i+2), (i, i+2, i+1) */
		if (tri_area2(&k[0], &k[i], &k[i + 2]) <= 0.25 || tri_area2(&k[i], &k[i + 2], &k[i + 1]) <= 0.25)
			return 1;
	return i + 1 < n && tri_area2(&k[0], &k[i], &k[i + 1]) <= 0.25;
}

/* Add one convex piece (UVs already local to its tile) to the record,
   flushing first if its vertices would not fit: a fan of quads from vertex 0
   (+ a triangle when odd), or ear-clipped triangles when the piece has
   collinear vertices on its boundary. */
static void rec_add_piece(out_t *o, rec_t *r, const poly_t *pc, const double (*uv)[2],
                          const uint8_t *light, const tex_slot *s, int face)
{
	vkey keys[MAXPV];
	for (int i = 0; i < pc->n; i++) {
		keys[i].x = clamp16(lround(pc->v[i].p[0] * QW_VSCALE));
		keys[i].y = clamp16(lround(pc->v[i].p[1] * QW_VSCALE));
		keys[i].z = clamp16(lround(pc->v[i].p[2] * QW_VSCALE));
		keys[i].c = light[i];
	}

	/* will it fit? count new vertices */
	int fresh = 0;
	for (int i = 0; i < pc->n; i++) {
		int seen = 0;
		for (int j = 0; j < r->nv && !seen; j++)
			seen = r->v[j].x == keys[i].x && r->v[j].y == keys[i].y && r->v[j].z == keys[i].z &&
			       r->v[j].c == keys[i].c;
		for (int j = 0; j < i && !seen; j++)
			seen = keys[j].x == keys[i].x && keys[j].y == keys[i].y && keys[j].z == keys[i].z &&
			       keys[j].c == keys[i].c;
		fresh += !seen;
	}
	int tris_needed = pc->n - 2;
	int ears[MAXPV][3], nears = 0, use_ears = quad_fan_degenerate(keys, pc->n);
	int pairs[MAXPV][4], npairs = 0, single[MAXPV], nsingle = 0;
	if (use_ears) {
		/* then two triangles sharing an edge go out as one quad: the GPU
		   draws a quad (a, b, c, d) as (a, b, c) + (b, c, d) */
		nears = ear_clip(keys, pc->n, ears);
		int used[MAXPV] = { 0 };
		for (int x = 0; x < nears; x++) {
			if (used[x])
				continue;
			for (int y = x + 1; y < nears && !used[x]; y++) {
				if (used[y])
					continue;
				int sh[2], ns = 0, ax = -1, dy = -1;
				for (int u = 0; u < 3; u++) {
					int in = 0;
					for (int v = 0; v < 3; v++)
						in |= ears[x][u] == ears[y][v];
					if (in && ns < 2) sh[ns++] = ears[x][u];
					else if (!in) ax = ears[x][u];
				}
				if (ns != 2)
					continue;
				for (int v = 0; v < 3; v++)
					if (ears[y][v] != sh[0] && ears[y][v] != sh[1])
						dy = ears[y][v];
				pairs[npairs][0] = ax; pairs[npairs][1] = sh[0];
				pairs[npairs][2] = sh[1]; pairs[npairs][3] = dy;
				npairs++;
				used[x] = used[y] = 1;
			}
			if (!used[x])
				single[nsingle++] = x;
		}
		o->pieces_eared++;
	}
	int add_q = use_ears ? npairs : tris_needed / 2, add_t = use_ears ? nsingle : (tris_needed & 1);
	int pkt = 8 + (r->nq + add_q) * 52 + (r->nt + add_t) * 40;
	if (r->nv + fresh > QW_MAX_VERTS || r->nq + add_q > 255 || r->nt + add_t > 255 ||
	    pkt > QW_MAX_REC_PKT)
		rec_flush(o, r, s);

	int known = 0;
	for (int i = 0; i < r->nfaces && !known; i++)
		known = r->faces[i] == face;
	if (!known)
		r->faces[r->nfaces++] = face;

	int idx[MAXPV];
	for (int i = 0; i < pc->n; i++) {
		idx[i] = rec_find_or_add(r, keys[i]);
		if (idx[i] < 0)
			die("piece with %d vertices does not fit a record", pc->n);
	}

	if (use_ears) {
		for (int e = 0; e < npairs; e++) {   /* PS1 corner order: (a, b, c, d) */
			qw_quad *q = &r->q[r->nq++];
			for (int k = 0; k < 4; k++) {
				q->idx[k] = (uint8_t) (idx[pairs[e][k]] * 4);
				q->uv[k]  = uvword(s, uv[pairs[e][k]][0], uv[pairs[e][k]][1]);
			}
		}
		for (int e = 0; e < nsingle; e++) {
			qw_tri *t = &r->t[r->nt++];
			for (int q = 0; q < 3; q++) {
				t->idx[q] = (uint8_t) (idx[ears[single[e]][q]] * 4);
				t->uv[q]  = uvword(s, uv[ears[single[e]][q]][0], uv[ears[single[e]][q]][1]);
			}
			t->pad = 0;
			t->pad2 = 0;
		}
		o->pieces++;
		return;
	}

	/* fan of quads (p0, pi, pi+1, pi+2) -> PS1 order (p0, pi, pi+2, pi+1) */
	int i = 1;
	for (; i + 2 < pc->n; i += 2) {
		qw_quad *q = &r->q[r->nq++];
		int c[4] = { 0, i, i + 2, i + 1 };
		for (int k = 0; k < 4; k++) {
			q->idx[k] = (uint8_t) (idx[c[k]] * 4);
			q->uv[k]  = uvword(s, uv[c[k]][0], uv[c[k]][1]);
		}
	}
	if (i + 1 < pc->n) {                       /* one triangle left */
		qw_tri *t = &r->t[r->nt++];
		int c[3] = { 0, i, i + 1 };
		for (int k = 0; k < 3; k++) {
			t->idx[k] = (uint8_t) (idx[c[k]] * 4);
			t->uv[k]  = uvword(s, uv[c[k]][0], uv[c[k]][1]);
		}
		t->pad = 0;
		t->pad2 = 0;
	}
	o->pieces++;
}

/* ---------------------------------------------------------------- light -- */

double light_gain = 1.5;

/* A face's lightmap as Quake lays it out (CalcSurfaceExtents): one luxel
   per 16 texels, from texturemins = floor(min / 16) * 16, up to 4 styles. */
typedef struct {
	double         smin, tmin;
	int            w, h;           /* luxels                      */
	const uint8_t *maps[4];
	int            nmaps;
} facelight;

static void face_light_setup(const bsp_t *b, int f, const poly_t *pl, facelight *fl)
{
	const bsp_face *bf = &b->faces[f];
	double mn[2] = { 1e30, 1e30 }, mx[2] = { -1e30, -1e30 };
	for (int i = 0; i < pl->n; i++) {
		double st[2] = { pl->v[i].s, pl->v[i].t };
		for (int k = 0; k < 2; k++) {
			if (st[k] < mn[k]) mn[k] = st[k];
			if (st[k] > mx[k]) mx[k] = st[k];
		}
	}
	int b0 = (int) floor(mn[0] / 16), b1 = (int) ceil(mx[0] / 16);
	int c0 = (int) floor(mn[1] / 16), c1 = (int) ceil(mx[1] / 16);
	fl->smin = b0 * 16.0;
	fl->tmin = c0 * 16.0;
	fl->w    = b1 - b0 + 1;
	fl->h    = c1 - c0 + 1;
	fl->nmaps = 0;
	if (bf->lightofs < 0 || !b->lighting)
		return;
	for (int k = 0; k < 4 && bf->styles[k] != 255; k++) {
		size_t at = (size_t) bf->lightofs + (size_t) k * fl->w * fl->h;
		if (at + (size_t) fl->w * fl->h > b->light_len)
			break;
		fl->maps[fl->nmaps++] = b->lighting + at;
	}
}

/* Light at texture coordinates (s, t): bilinear over the luxels, summed over
   the styles (each at its normal brightness), 0..255. It modulates the
   texture as the GPU does: 128 leaves the texel as is, 255 about doubles it
   - the same scale as Quake's software colormap (light 127 = row 32). A face
   without a lightmap in a lit map is black, as in Quake. */
static uint8_t face_light_at(const facelight *fl, double s, double t)
{
	if (!fl->nmaps)
		return 0;
	double u = (s - fl->smin) / 16, v = (t - fl->tmin) / 16;
	if (u < 0) u = 0;
	if (v < 0) v = 0;
	if (u > fl->w - 1) u = fl->w - 1;
	if (v > fl->h - 1) v = fl->h - 1;
	int u0 = (int) u, v0 = (int) v;
	int u1 = u0 + 1 < fl->w ? u0 + 1 : u0, v1 = v0 + 1 < fl->h ? v0 + 1 : v0;
	double fu = u - u0, fv = v - v0, sum = 0;
	for (int k = 0; k < fl->nmaps; k++) {
		const uint8_t *m = fl->maps[k];
		sum += (m[v0 * fl->w + u0] * (1 - fu) + m[v0 * fl->w + u1] * fu) * (1 - fv)
		     + (m[v1 * fl->w + u0] * (1 - fu) + m[v1 * fl->w + u1] * fu) * fv;
	}
	long l = lround(sum * light_gain);
	return (uint8_t) (l > 255 ? 255 : l);
}

/* union-find over faces */
static int uf_find(int *up, int x)
{
	while (up[x] != x) {
		up[x] = up[up[x]];
		x = up[x];
	}
	return x;
}

/* Are two faces marked by exactly the same leaves? (lists sorted) Then
   they are always visible together, and a record holding both draws
   nothing a per-face record would not. */
static int same_marks(const int *first, const int *leaf, int f, int g)
{
	int a = first[f], ae = first[f + 1], c = first[g], ce = first[g + 1];
	if (ae - a != ce - c)
		return 0;
	for (; a < ae; a++, c++)
		if (leaf[a] != leaf[c])
			return 0;
	return 1;
}

/* do two faces share an edge? */
static int faces_adjacent(const bsp_t *b, int f, int g)
{
	const bsp_face *a = &b->faces[f], *c = &b->faces[g];
	for (int i = 0; i < a->numedges; i++) {
		int ea = abs(b->surfedges[a->firstedge + i]);
		for (int j = 0; j < c->numedges; j++)
			if (abs(b->surfedges[c->firstedge + j]) == ea)
				return 1;
	}
	return 0;
}

/* the miptex a face uses, or -1 */
/* the slot a face of miptex mt draws with (an animated texture's: its
   chain's display slot, tex_build) */
static const tex_slot *face_slot(const tex_set *ts, int mt)
{
	return &ts->slots[ts->remap ? ts->remap[mt] : mt];
}

static int face_miptex(const bsp_t *b, int f)
{
	const bsp_face *bf = &b->faces[f];
	if (bf->texinfo < 0 || bf->texinfo >= b->num_texinfo)
		return -1;
	int mt = b->texinfo[bf->texinfo].miptex;
	return mt >= 0 && mt < b->num_miptex ? mt : -1;
}

/* ------------------------------------------------- pieces + T-junctions -- */

/* Every drawn face is cut on its texture-repeat grid first (face_pieces),
   for the whole map, so that T-junctions can be repaired before records are
   built: a vertex lying on another piece's edge (a cut of one face along its
   neighbour's edge, qbsp's own leftovers) leaves a crack on screen, because
   the two sides of that edge are not drawn between the same vertices.
   tj_repair inserts such vertices into the edges they lie on. */
typedef struct {
	int    face;
	int    model;           /* the Quake model the face belongs to        */
	int    fixed;           /* T-junction vertices inserted               */
	double s_off, t_off;    /* the cell's corner, Quake texels            */
	poly_t pl;
} piece_t;

static piece_t *pc_list;
static int     *face_model;              /* [num_faces]: the model of each face */
static int      pc_num, pc_cap;
static int     *pc_first, *pc_count;   /* per face */
static uint32_t tj_inserted, tj_pieces;

static void face_pieces(const bsp_t *b, const tex_set *ts, int f)
{
	pc_first[f] = pc_num;
	pc_count[f] = 0;
	const bsp_face *bf = &b->faces[f];
	if (bf->numedges < 3 || bf->texinfo < 0 || bf->texinfo >= b->num_texinfo)
		return;
	const bsp_texinfo *ti = &b->texinfo[bf->texinfo];
	if (ti->miptex < 0 || ti->miptex >= b->num_miptex)
		return;
	const tex_slot   *s = face_slot(ts, ti->miptex);
	const bsp_miptex *m = &b->miptex[ti->miptex];
	if (!s->present)                    /* sky or missing: not drawn (yet) */
		return;

	poly_t pl;
	pl.n = 0;
	for (int e = 0; e < bf->numedges && pl.n < MAXPV; e++) {
		int se = b->surfedges[bf->firstedge + e];
		int vi = se >= 0 ? b->edges[se * 2] : b->edges[-se * 2 + 1];
		pvert *v = &pl.v[pl.n++];
		for (int k = 0; k < 3; k++)
			v->p[k] = b->verts[vi * 3 + k];
		v->s = v->p[0] * ti->vecs[0][0] + v->p[1] * ti->vecs[0][1] + v->p[2] * ti->vecs[0][2] + ti->vecs[0][3];
		v->t = v->p[0] * ti->vecs[1][0] + v->p[1] * ti->vecs[1][1] + v->p[2] * ti->vecs[1][2] + ti->vecs[1][3];
	}

	double smin = 1e30, smax = -1e30, tmin = 1e30, tmax = -1e30;
	for (int i = 0; i < pl.n; i++) {
		if (pl.v[i].s < smin) smin = pl.v[i].s;
		if (pl.v[i].s > smax) smax = pl.v[i].s;
		if (pl.v[i].t < tmin) tmin = pl.v[i].t;
		if (pl.v[i].t > tmax) tmax = pl.v[i].t;
	}

	/* Cells, in Quake texels. A wrapped axis: 256 cooked texels (a byte of
	   UV), from an origin on a repetition boundary so the virtual
	   coordinate keeps the texture's phase; the face's own origin, so it
	   is cut as little as possible. Another axis: the VRAM tile (reps
	   repetitions) on the global grid. */
	const double W  = s->win_x ? 256.0 * m->w / s->rep_w : (double) m->w * s->reps_x;
	const double H  = s->win_y ? 256.0 * m->h / s->rep_h : (double) m->h * s->reps_y;
	const double s0 = s->win_x ? floor(smin / m->w) * m->w : 0;
	const double t0 = s->win_y ? floor(tmin / m->h) * m->h : 0;
	long i0 = (long) floor((smin - s0) / W), i1 = (long) floor((smax - 1e-4 - s0) / W);
	long j0 = (long) floor((tmin - t0) / H), j1 = (long) floor((tmax - 1e-4 - t0) / H);

	for (long i = i0; i <= i1; i++) {
		poly_t a, c;
		clip_poly(&pl, &a, 0, s0 + i * W, +1);
		clip_poly(&a, &c, 0, s0 + (i + 1) * W, -1);
		for (long j = j0; j <= j1; j++) {
			poly_t d, pc;
			clip_poly(&c, &d, 1, t0 + j * H, +1);
			clip_poly(&d, &pc, 1, t0 + (j + 1) * H, -1);
			poly_weld(&pc);
			if (pc.n < 3 || poly_area(&pc) < 1.0 / 64)
				continue;
			if (pc_num == pc_cap) {
				pc_cap = pc_cap ? 2 * pc_cap : 4096;
				pc_list = realloc(pc_list, (size_t) pc_cap * sizeof *pc_list);
				if (!pc_list)
					die("out of memory (pieces)");
			}
			piece_t *p = &pc_list[pc_num++];
			p->face  = f;
			p->model = face_model[f];
			p->fixed = 0;
			p->s_off = s0 + i * W;
			p->t_off = t0 + j * H;
			p->pl    = pc;
			pc_count[f]++;
		}
	}
}

/* quantized vertex positions (the pool's: QW_VSCALE units) on a grid, per
   model: a brush model's edges only take its own vertices (it moves) */
typedef struct { int32_t q[3]; int model; } qv_t;
#define TJ_CELL  256                     /* grid cell, quantized units (64 world units) */
#define TJ_HSIZE (1 << 16)

static qv_t *tj_v;
static int   tj_nv, tj_cap;
static int  *tj_head, *tj_next;           /* hash of cells -> vertex chains */

static uint32_t tj_cell_hash(int32_t cx, int32_t cy, int32_t cz)
{
	uint32_t h = (uint32_t) cx * 73856093u ^ (uint32_t) cy * 19349663u ^ (uint32_t) cz * 83492791u;
	return h & (TJ_HSIZE - 1);
}

static int32_t tj_quant(double v)
{
	return (int32_t) lround(v * QW_VSCALE);
}

static int32_t tj_floordiv(int32_t a, int32_t b)
{
	return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* add a quantized vertex once */
static void tj_add(const double p[3], int model)
{
	int32_t q[3] = { tj_quant(p[0]), tj_quant(p[1]), tj_quant(p[2]) };
	uint32_t h = tj_cell_hash(tj_floordiv(q[0], TJ_CELL), tj_floordiv(q[1], TJ_CELL), tj_floordiv(q[2], TJ_CELL));
	for (int i = tj_head[h]; i >= 0; i = tj_next[i])
		if (tj_v[i].q[0] == q[0] && tj_v[i].q[1] == q[1] && tj_v[i].q[2] == q[2] &&
		    tj_v[i].model == model)
			return;
	if (tj_nv == tj_cap) {
		tj_cap  = tj_cap ? 2 * tj_cap : 16384;
		tj_v    = realloc(tj_v, (size_t) tj_cap * sizeof *tj_v);
		tj_next = realloc(tj_next, (size_t) tj_cap * sizeof *tj_next);
		if (!tj_v || !tj_next)
			die("out of memory (T-junctions)");
	}
	memcpy(tj_v[tj_nv].q, q, sizeof q);
	tj_v[tj_nv].model = model;
	tj_next[tj_nv] = tj_head[h];
	tj_head[h] = tj_nv++;
}

/* Insert into every piece edge the map's vertices lying on it (within half
   a world unit of the segment, strictly between its ends), in order. Their
   texture coordinates come from the face's own mapping at their quantized
   position, so the pool shares them with the neighbour. */
static void tj_repair(const bsp_t *b)
{
	tj_head = xmalloc(TJ_HSIZE * sizeof *tj_head);
	for (int i = 0; i < TJ_HSIZE; i++)
		tj_head[i] = -1;
	for (int i = 0; i < pc_num; i++)
		for (int k = 0; k < pc_list[i].pl.n; k++)
			tj_add(pc_list[i].pl.v[k].p, pc_list[i].model);

	int *stamp = xmalloc((size_t) tj_nv * sizeof *stamp);
	for (int i = 0; i < tj_nv; i++)
		stamp[i] = -1;
	int edge_id = 0;

	for (int pi = 0; pi < pc_num; pi++) {
		piece_t *pc = &pc_list[pi];
		const bsp_texinfo *ti = &b->texinfo[b->faces[pc->face].texinfo];
		poly_t out;
		out.n = 0;
		for (int e = 0; e < pc->pl.n; e++) {
			const pvert *va = &pc->pl.v[e], *vb = &pc->pl.v[(e + 1) % pc->pl.n];
			if (out.n < MAXPV)
				out.v[out.n++] = *va;
			int32_t qa[3] = { tj_quant(va->p[0]), tj_quant(va->p[1]), tj_quant(va->p[2]) };
			int32_t qb[3] = { tj_quant(vb->p[0]), tj_quant(vb->p[1]), tj_quant(vb->p[2]) };
			double D[3] = { qb[0] - qa[0], qb[1] - qa[1], qb[2] - qa[2] };
			double L2 = D[0] * D[0] + D[1] * D[1] + D[2] * D[2];
			if (L2 < 16)                   /* shorter than a world unit */
				continue;
			double L = sqrt(L2);

			/* candidates: the cells along the segment and their neighbours */
			struct { double t; int v; } hit[MAXPV];
			int nhit = 0;
			edge_id++;
			int steps = (int) (L / (TJ_CELL / 2)) + 1;
			for (int st = 0; st <= steps; st++) {
				double f = (double) st / steps;
				int32_t c0 = tj_floordiv((int32_t) lround(qa[0] + f * D[0]), TJ_CELL);
				int32_t c1 = tj_floordiv((int32_t) lround(qa[1] + f * D[1]), TJ_CELL);
				int32_t c2 = tj_floordiv((int32_t) lround(qa[2] + f * D[2]), TJ_CELL);
				for (int dx = -1; dx <= 1; dx++)
				for (int dy = -1; dy <= 1; dy++)
				for (int dz = -1; dz <= 1; dz++)
					for (int v = tj_head[tj_cell_hash(c0 + dx, c1 + dy, c2 + dz)]; v >= 0; v = tj_next[v]) {
						if (stamp[v] == edge_id || tj_v[v].model != pc->model)
							continue;
						stamp[v] = edge_id;
						const int32_t *q = tj_v[v].q;
						double d[3] = { q[0] - qa[0], q[1] - qa[1], q[2] - qa[2] };
						double t = (d[0] * D[0] + d[1] * D[1] + d[2] * D[2]) / L2;
						if (t * L < 2 || (1 - t) * L < 2)
							continue;          /* an end (or beyond) */
						double cx = d[1] * D[2] - d[2] * D[1], cy = d[2] * D[0] - d[0] * D[2],
						       cz = d[0] * D[1] - d[1] * D[0];
						if ((cx * cx + cy * cy + cz * cz) / L2 > 4.0)
							continue;          /* > half a world unit off the line */
						if (nhit < MAXPV) {
							hit[nhit].t = t;
							hit[nhit].v = v;
							nhit++;
						}
					}
			}
			for (int x = 1; x < nhit; x++)       /* in order along the edge */
				for (int y = x; y > 0 && hit[y].t < hit[y - 1].t; y--) {
					double tt = hit[y].t; int vv = hit[y].v;
					hit[y].t = hit[y - 1].t; hit[y].v = hit[y - 1].v;
					hit[y - 1].t = tt; hit[y - 1].v = vv;
				}
			for (int x = 0; x < nhit && out.n < MAXPV; x++) {
				pvert *nv = &out.v[out.n++];
				for (int k = 0; k < 3; k++)
					nv->p[k] = tj_v[hit[x].v].q[k] / (double) QW_VSCALE;
				nv->s = nv->p[0] * ti->vecs[0][0] + nv->p[1] * ti->vecs[0][1] + nv->p[2] * ti->vecs[0][2] + ti->vecs[0][3];
				nv->t = nv->p[0] * ti->vecs[1][0] + nv->p[1] * ti->vecs[1][1] + nv->p[2] * ti->vecs[1][2] + ti->vecs[1][3];
				tj_inserted++;
			}
			if (nhit)
				pc->fixed = 1;
		}
		if (pc->fixed) {
			pc->pl = out;
			tj_pieces++;
		}
	}
	free(stamp);
	free(tj_head);
}

/* each face's vertex light, summed over its cooked vertices (the leaf light) */
static uint32_t *face_lsum;
static uint32_t *face_lcnt;

/* Add a face's (repaired) pieces to record r (flushed whenever it fills
   up): UVs from the cell's corner, light from the face's lightmap. */
static void emit_face(out_t *o, const bsp_t *b, const tex_set *ts, int f, rec_t *r)
{
	if (!pc_count[f])
		return;
	const bsp_face    *bf = &b->faces[f];
	const bsp_texinfo *ti = &b->texinfo[bf->texinfo];
	const tex_slot    *s  = face_slot(ts, ti->miptex);
	const bsp_miptex  *m  = &b->miptex[ti->miptex];

	poly_t pl;                          /* the whole face, for its lightmap */
	pl.n = 0;
	for (int e = 0; e < bf->numedges && pl.n < MAXPV; e++) {
		int se = b->surfedges[bf->firstedge + e];
		int vi = se >= 0 ? b->edges[se * 2] : b->edges[-se * 2 + 1];
		pvert *v = &pl.v[pl.n++];
		for (int k = 0; k < 3; k++)
			v->p[k] = b->verts[vi * 3 + k];
		v->s = v->p[0] * ti->vecs[0][0] + v->p[1] * ti->vecs[0][1] + v->p[2] * ti->vecs[0][2] + ti->vecs[0][3];
		v->t = v->p[0] * ti->vecs[1][0] + v->p[1] * ti->vecs[1][1] + v->p[2] * ti->vecs[1][2] + ti->vecs[1][3];
	}
	facelight fl;
	face_light_setup(b, f, &pl, &fl);
	if (pc_count[f] > 1)
		o->faces_split++;

	for (int i = 0; i < pc_count[f]; i++) {
		const piece_t *p = &pc_list[pc_first[f] + i];
		double  uv[MAXPV][2];            /* cooked texels from the cell's corner */
		uint8_t light[MAXPV];
		for (int k = 0; k < p->pl.n; k++) {
			uv[k][0] = (p->pl.v[k].s - p->s_off) * s->rep_w / m->w;
			uv[k][1] = (p->pl.v[k].t - p->t_off) * s->rep_h / m->h;
			light[k] = face_light_at(&fl, p->pl.v[k].s, p->pl.v[k].t);
			face_lsum[f] += light[k];
			face_lcnt[f]++;
		}
		rec_add_piece(o, r, &p->pl, (const double (*)[2]) uv, light, s, f);
	}
}

/* ------------------------------------------------------------- entities -- */

static int find_player_start(const ent_list *l, double origin[3], double *angle)
{
	for (int i = 0; i < l->num; i++) {
		const ent_rec *e = &l->ents[i];
		if (strcmp(e->classname, "info_player_start") || !ent_vector(l, e, "origin", origin))
			continue;
		*angle = ent_float(l, e, "angle", 0);
		return 1;
	}
	return 0;
}

/* ---------------------------------------------------------------- world -- */

static int depth_of(const bsp_t *b, int node)
{
	if (node < 0)
		return 0;
	int a = depth_of(b, b->nodes[node].child[0]);
	int c = depth_of(b, b->nodes[node].child[1]);
	return 1 + (a > c ? a : c);
}

/* the nodes of a model's hull-0 tree: reach[node] = model + 1 */
static void mark_reachable(const bsp_t *b, int node, uint8_t *reach, int model)
{
	if (node < 0)
		return;
	if (reach[node])
		die("node %d is in two models' trees", node);
	reach[node] = (uint8_t) (model + 1);
	mark_reachable(b, b->nodes[node].child[0], reach, model);
	mark_reachable(b, b->nodes[node].child[1], reach, model);
}

/* Quake's rendering-hull point test: contents of the leaf holding p. */
static int point_contents(const bsp_t *b, int node, const double p[3])
{
	while (node >= 0) {
		const bsp_node  *n  = &b->nodes[node];
		const bsp_plane *pl = &b->planes[n->planenum];
		double d = pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - pl->dist;
		node = n->child[d < 0];
	}
	return b->leaves[-1 - node].contents;
}

static void bbox(const int16_t mins[3], const int16_t maxs[3], int16_t c[3], int16_t h[3])
{
	for (int k = 0; k < 3; k++) {
		long lo = (long) mins[k] * QW_VSCALE, hi = (long) maxs[k] * QW_VSCALE;
		c[k] = clamp16((lo + hi) / 2);
		h[k] = clamp16((hi - lo) / 2 + QW_VSCALE);   /* +1 world unit margin */
	}
}

/* depth of a clip hull (engine/clip.c keeps at most QW_MAX_SPLIT pending
   splits, one per node on the current path) */
static int clip_depth(const bsp_t *b, int n)
{
	if (n < 0)
		return 0;
	int a = clip_depth(b, b->clipnodes[n].child[0]);
	int c = clip_depth(b, b->clipnodes[n].child[1]);
	return 1 + (a > c ? a : c);
}

void world_build(const bsp_t *b, const tex_set *ts, const fidelity_t *fid,
                 const char *out_path)
{
	(void) fid;
	const bsp_model *wm = &b->models[0];
	if (b->num_models > 254)
		die("%d models: more than 254", b->num_models);

	ent_list ents;
	ents_parse(b, &ents);

	/* every drawn face cut into pieces (the world's and the brush models'),
	   T-junctions repaired within each model */
	pc_first = xmalloc((size_t) b->num_faces * sizeof *pc_first);
	pc_count = xmalloc((size_t) b->num_faces * sizeof *pc_count);
	face_model = xmalloc((size_t) b->num_faces * sizeof *face_model);
	face_lsum = xcalloc((size_t) b->num_faces, sizeof *face_lsum);
	face_lcnt = xcalloc((size_t) b->num_faces, sizeof *face_lcnt);
	for (int f = 0; f < b->num_faces; f++)
		face_model[f] = -1;
	uint8_t *drawn = xcalloc((size_t) b->num_models, 1);   /* (not the triggers') */
	ents_drawn_models(b, drawn);
	for (int m = 0; m < b->num_models; m++)
		for (int f = b->models[m].firstface; f < b->models[m].firstface + b->models[m].numfaces; f++)
			if (f >= 0 && f < b->num_faces && drawn[m])
				face_model[f] = m;
	free(drawn);

	/* the faces of the brush models that never move (func_wall,
	   func_illusionary, the episode and boss gates) buried in the world:
	   the space in front of every corner and of the centre is solid. Quake
	   never shows them (its z-sorted spans hide them); the PS1's painter's
	   order would paint them over the world (the boss gate's dark edges
	   over START's floor) */
	{
		uint8_t *still = xcalloc((size_t) b->num_models, 1);
		static const char *const still_classes[] = { "func_wall", "func_illusionary", "func_episodegate",
		                                             "func_bossgate" };
		for (int i = 0; i < ents.num; i++) {
			const char *mdl = ent_value(&ents, &ents.ents[i], "model");
			if (!mdl || mdl[0] != '*')
				continue;
			for (unsigned c = 0; c < sizeof still_classes / sizeof still_classes[0]; c++)
				if (!strcmp(ents.ents[i].classname, still_classes[c])) {
					int idx = atoi(mdl + 1);
					if (idx > 0 && idx < b->num_models)
						still[idx] = 1;
				}
		}
		int buried = 0, wroot = b->models[0].headnode[0];
		for (int f = 0; f < b->num_faces; f++) {
			if (face_model[f] <= 0 || !still[face_model[f]])
				continue;
			const bsp_face  *bf = &b->faces[f];
			const bsp_plane *pl = &b->planes[bf->planenum];
			double n[3], c[3] = { 0, 0, 0 }, v[64][3];
			int nv = 0;
			for (int k = 0; k < 3; k++)
				n[k] = bf->side ? -pl->n[k] : pl->n[k];
			for (int e = 0; e < bf->numedges && nv < 64; e++, nv++) {
				int se = b->surfedges[bf->firstedge + e];
				int vi = se >= 0 ? b->edges[se * 2] : b->edges[-se * 2 + 1];
				for (int k = 0; k < 3; k++) {
					v[nv][k] = b->verts[vi * 3 + k];
					c[k] += v[nv][k];
				}
			}
			if (nv < 3)
				continue;
			for (int k = 0; k < 3; k++)
				c[k] /= nv;
			int open = 0;
			for (int i = 0; i <= nv && !open; i++) {
				double q[3];
				for (int k = 0; k < 3; k++)     /* 2 units out, a tenth of the way in */
					q[k] = (i < nv ? v[i][k] + (c[k] - v[i][k]) * 0.1 : c[k]) + n[k] * 2;
				open = point_contents(b, wroot, q) != -2;
			}
			if (!open) {
				face_model[f] = -1;
				buried++;
			}
		}
		if (buried)
			printf("qcook: %d buried faces of still brush models left out\n", buried);
		free(still);
	}
	for (int f = 0; f < b->num_faces; f++) {
		pc_count[f] = 0;
		if (face_model[f] >= 0)
			face_pieces(b, ts, f);
	}
	tj_repair(b);
	int root = wm->headnode[0];

	/* each model's hull-0 tree (reach[n] = model + 1): the world's is
	   walked by the renderer with the PVS, a brush model's from its root */
	uint8_t *reach = xcalloc((size_t) b->num_nodes, 1);
	for (int m = 0; m < b->num_models; m++)
		mark_reachable(b, b->models[m].headnode[0], reach, m);

	/* parents */
	uint32_t *node_parent = xmalloc((size_t) b->num_nodes * 4);
	uint32_t *leaf_parent = xmalloc((size_t) b->num_leaves * 4);
	for (int i = 0; i < b->num_nodes; i++) node_parent[i] = QW_NONE;
	for (int i = 0; i < b->num_leaves; i++) leaf_parent[i] = QW_NONE;
	for (int i = 0; i < b->num_nodes; i++) {
		if (!reach[i])
			continue;
		for (int c = 0; c < 2; c++) {
			int ch = b->nodes[i].child[c];
			if (ch >= 0) node_parent[ch] = (uint32_t) i;
			else         leaf_parent[-1 - ch] = (uint32_t) i;
		}
	}

	/* face stream, node by node: front block then back block */
	int *uf_up     = xmalloc((size_t) b->num_faces * sizeof *uf_up);
	int *edge_face = xmalloc((size_t) b->num_edges * sizeof *edge_face);   /* last face seen on an edge */
	for (int e = 0; e < b->num_edges; e++)
		edge_face[e] = -1;
	int *face_group = xmalloc((size_t) b->num_faces * sizeof *face_group);
	for (int f = 0; f < b->num_faces; f++)
		face_group[f] = -1;
	/* per face, the sorted list of leaves that mark it */
	int *face_mfirst = xcalloc((size_t) b->num_faces + 1, sizeof *face_mfirst);
	int *face_mleaf  = xmalloc((size_t) b->num_marks * sizeof *face_mleaf + 4);
	for (int i = 0; i < b->num_leaves; i++)
		for (int k = 0; k < b->leaves[i].nummarks; k++) {
			int mk = b->leaves[i].firstmark + k;
			if (mk >= 0 && mk < b->num_marks && b->marks[mk] < b->num_faces)
				face_mfirst[b->marks[mk] + 1]++;
		}
	for (int f = 0; f < b->num_faces; f++)
		face_mfirst[f + 1] += face_mfirst[f];
	{
		int *fill = xmalloc((size_t) b->num_faces * sizeof *fill);
		memcpy(fill, face_mfirst, (size_t) b->num_faces * sizeof *fill);
		for (int i = 0; i < b->num_leaves; i++)          /* leaves ascending: lists sorted */
			for (int k = 0; k < b->leaves[i].nummarks; k++) {
				int mk = b->leaves[i].firstmark + k;
				if (mk >= 0 && mk < b->num_marks && b->marks[mk] < b->num_faces)
					face_mleaf[fill[b->marks[mk]]++] = i;
			}
		free(fill);
	}
	int *memb  = xmalloc((size_t) b->num_faces * sizeof *memb);    /* a group's faces */
	int *order = xmalloc((size_t) b->num_faces * sizeof *order);   /* ... breadth first */

	out_t o;
	memset(&o, 0, sizeof o);
	if ((long) b->num_nodes * QW_NODE_WORDS >= QW_NOPARENT)
		die("%d nodes: parent offsets do not fit 16 bits", b->num_nodes);
	qw_node *nodes = xcalloc((size_t) b->num_nodes + 1, sizeof *nodes);   /* + sentinel */
	uint32_t *bm_records = xcalloc((size_t) b->num_models, sizeof *bm_records);
	for (int i = 0; i < b->num_nodes; i++) {
		const bsp_node *bn = &b->nodes[i];
		const bsp_plane *pl = &b->planes[bn->planenum];
		qw_node *n = &nodes[i];
		uint32_t recs_before = o.records;

		bbox(bn->mins, bn->maxs, n->c, n->h);
		(void) pl;
		n->plane3 = (uint16_t) (bn->planenum * 3);    /* the clip section's table */
		for (int c = 0; c < 2; c++) {
			int ch = bn->child[c];
			n->child[c] = ch >= 0 ? (uint32_t) ch : (0x80000000u | (uint32_t) (-1 - ch));
		}
		n->parent = node_parent[i] == QW_NONE ? QW_NOPARENT
		          : (uint16_t) (node_parent[i] * QW_NODE_WORDS);

		/* Records hold several faces: those of a node side with the same
		   texture that are connected (share an edge) - typically one surface
		   qbsp cut for lightmaps, which the PS1 does not use. They share the
		   record header and vertices. Visibility is per record (drawn when
		   any leaf marking one of its faces is visible): records are filled
		   breadth first, so a record is a compact patch and drawing it whole
		   costs little. Unconnected faces stay apart. */
		for (int f = bn->firstface; f < bn->firstface + bn->numfaces; f++)
			uf_up[f] = f;
		for (int f = bn->firstface; f < bn->firstface + bn->numfaces; f++)
			for (int e = 0; e < b->faces[f].numedges; e++) {
				int se = b->surfedges[b->faces[f].firstedge + e];
				int ed = se >= 0 ? se : -se;
				if (edge_face[ed] >= bn->firstface && edge_face[ed] < f &&
				    b->faces[edge_face[ed]].side == b->faces[f].side &&
				    face_miptex(b, edge_face[ed]) == face_miptex(b, f) &&
				    same_marks(face_mfirst, face_mleaf, edge_face[ed], f))
					uf_up[uf_find(uf_up, f)] = uf_find(uf_up, edge_face[ed]);
				edge_face[ed] = f;
			}
		for (int side = 0; side < 2; side++) {
			n->faces[side] = (uint32_t) o.stream.len;
			for (int f = bn->firstface; f < bn->firstface + bn->numfaces; f++) {
				if (b->faces[f].side != side || face_group[f] >= 0)
					continue;
				/* the group's faces, breadth first over shared edges, so each
				   record holds a compact patch */
				int root = uf_find(uf_up, f), nm = 0, nq = 0;
				for (int g = f; g < bn->firstface + bn->numfaces; g++)
					if (face_group[g] < 0 && uf_find(uf_up, g) == root)
						memb[nm++] = g;
				order[nq++] = memb[0];
				face_group[memb[0]] = 1;
				for (int q = 0; q < nq; q++)
					for (int k = 0; k < nm; k++)
						if (face_group[memb[k]] < 0 && faces_adjacent(b, order[q], memb[k])) {
							face_group[memb[k]] = 1;
							order[nq++] = memb[k];
						}
				for (int k = 0; k < nm; k++)           /* (connected: none left) */
					if (face_group[memb[k]] < 0) {
						face_group[memb[k]] = 1;
						order[nq++] = memb[k];
					}
				rec_t *r = xcalloc(1, sizeof *r);
				for (int k = 0; k < 3; k++) { r->mn[k] = 1e30; r->mx[k] = -1e30; }
				o.bmodel = reach[i] - 1;
				if (reach[i])
					for (int q = 0; q < nq; q++) {
						/* keep the record compact: flush before a face that
						   would grow it past REC_MAX_RADIUS */
						double fmn[3], fmx[3], h2 = 0;
						face_bbox(b, order[q], fmn, fmx);
						for (int k = 0; k < 3; k++) {
							double lo = fmn[k] < r->mn[k] ? fmn[k] : r->mn[k];
							double hi = fmx[k] > r->mx[k] ? fmx[k] : r->mx[k];
							h2 += (hi - lo) * (hi - lo) / 4;
						}
						if ((r->nq || r->nt) && h2 > REC_MAX_RADIUS * REC_MAX_RADIUS)
							rec_flush(&o, r, face_slot(ts, face_miptex(b, f)));
						for (int k = 0; k < 3; k++) {
							if (fmn[k] < r->mn[k]) r->mn[k] = fmn[k];
							if (fmx[k] > r->mx[k]) r->mx[k] = fmx[k];
						}
						emit_face(&o, b, ts, order[q], r);
					}
				if (r->nq || r->nt)
					rec_flush(&o, r, face_slot(ts, face_miptex(b, f)));
				free(r);
			}
		}
		if (reach[i] > 1)
			bm_records[reach[i] - 1] += o.records - recs_before;
	}
	o.bmodel = 0;

	/* marksurfaces: faces -> the records drawing them, deduplicated per
	   leaf; records are the visibility ids */
	int *pair_first = xmalloc((size_t) b->num_faces * sizeof *pair_first);
	int *pair_next  = xmalloc((size_t) (o.npairs + 1) * sizeof *pair_next);
	for (int f = 0; f < b->num_faces; f++)
		pair_first[f] = -1;
	for (int p = o.npairs - 1; p >= 0; p--) {
		pair_next[p] = pair_first[o.pair_face[p]];
		pair_first[o.pair_face[p]] = p;
	}
	size_t mark_bound = 1;                     /* before deduplication */
	for (int mk = 0; mk < b->num_marks; mk++)
		if (b->marks[mk] < b->num_faces)
			for (int p = pair_first[b->marks[mk]]; p >= 0; p = pair_next[p])
				mark_bound++;
	uint16_t *gmarks = xmalloc(mark_bound * 2);
	int      *gfirst = xmalloc((size_t) b->num_leaves * sizeof *gfirst);
	int      *gcount = xmalloc((size_t) b->num_leaves * sizeof *gcount);
	int       ngmarks = 0;
	for (int i = 0; i < b->num_leaves; i++) {
		const bsp_leaf *bl = &b->leaves[i];
		gfirst[i] = ngmarks;
		for (int k = 0; k < bl->nummarks; k++) {
			int mk = bl->firstmark + k;
			if (mk < 0 || mk >= b->num_marks || b->marks[mk] >= b->num_faces)
				continue;
			for (int p = pair_first[b->marks[mk]]; p >= 0; p = pair_next[p]) {
				uint16_t g = (uint16_t) o.pair_rec[p];
				int seen = 0;
				for (int j = gfirst[i]; j < ngmarks && !seen; j++)
					seen = gmarks[j] == g;
				if (!seen)
					gmarks[ngmarks++] = g;
			}
		}
		gcount[i] = ngmarks - gfirst[i];
	}
	free(pair_first);
	free(pair_next);
	nodes[b->num_nodes].faces[0] = (uint32_t) o.stream.len;   /* sentinel: end of the stream */

	/* leaves. Their light (models in the leaf are lit with it): the average
	   vertex light of the leaf's faces; a leaf without lit faces (open air)
	   takes its nearest ancestor's subtree average. */
	qw_leaf *leaves = xcalloc((size_t) b->num_leaves, sizeof *leaves);
	uint32_t *lsum = xcalloc((size_t) b->num_leaves, 4), *lcnt = xcalloc((size_t) b->num_leaves, 4);
	uint32_t *nsum = xcalloc((size_t) b->num_nodes, 4), *ncnt = xcalloc((size_t) b->num_nodes, 4);
	for (int i = 0; i < b->num_leaves; i++) {
		const bsp_leaf *bl = &b->leaves[i];
		for (int m = 0; m < bl->nummarks; m++) {
			int f = b->marks[bl->firstmark + m];
			if (f >= 0 && f < b->num_faces && face_lcnt[f]) {
				lsum[i] += face_lsum[f] / face_lcnt[f];
				lcnt[i]++;
			}
		}
	}
	for (int i = b->num_nodes - 1; i >= 0; i--)        /* children have higher indices */
		for (int c = 0; c < 2; c++) {
			int ch = b->nodes[i].child[c];
			if (ch >= 0) { nsum[i] += nsum[ch]; ncnt[i] += ncnt[ch]; }
			else         { nsum[i] += lsum[-1 - ch]; ncnt[i] += lcnt[-1 - ch]; }
		}
	for (int i = 0; i < b->num_leaves; i++) {
		const bsp_leaf *bl = &b->leaves[i];
		qw_leaf *l = &leaves[i];
		bbox(bl->mins, bl->maxs, l->c, l->h);
		l->num_marks = (uint16_t) gcount[i];
		l->marks     = (uint32_t) gfirst[i];
		l->parent    = leaf_parent[i] == QW_NONE ? QW_NOPARENT
		             : (uint16_t) (leaf_parent[i] * QW_NODE_WORDS);
		l->vis       = bl->visofs >= 0 && (uint32_t) bl->visofs < b->vis_len
		             ? (uint32_t) bl->visofs : QW_NONE;
		l->contents  = (int8_t) bl->contents;
		uint32_t sum = lsum[i], cnt = lcnt[i];
		for (uint32_t n = leaf_parent[i]; !cnt && n != QW_NONE; n = node_parent[n]) {
			sum = nsum[n];
			cnt = ncnt[n];
		}
		l->light = (uint8_t) (cnt ? sum / cnt : 128);
	}
	free(lsum); free(lcnt); free(nsum); free(ncnt);

	/* bench views: spawn x4 yaws, then the 3 leaves that see the most faces */
	qw_view views[16];
	int nviews = 0;
	double so[3] = { 0, 0, 0 }, sa = 0;
	int has_spawn = find_player_start(&ents, so, &sa);
	if (has_spawn)
		for (int k = 0; k < 4; k++) {
			qw_view *v = &views[nviews++];
			v->pos[0] = (int32_t) lround(so[0]);
			v->pos[1] = (int32_t) lround(so[1]);
			v->pos[2] = (int32_t) lround(so[2]) + 22;    /* eye height */
			v->yaw    = (int16_t) (((long) lround(sa) + 90 * k) % 360);
			v->pitch  = 0;
		}
	{
		int rowbytes = (wm->visleafs + 7) >> 3;
		long best[3] = { -1, -1, -1 };
		int  besti[3] = { 0, 0, 0 };
		uint8_t *row = xmalloc((size_t) rowbytes + 1);
		for (int i = 1; i <= wm->visleafs && i < b->num_leaves; i++) {
			const bsp_leaf *bl = &b->leaves[i];
			if (bl->visofs < 0 || bl->contents != -1)
				continue;
			/* decompress Quake RLE row */
			const uint8_t *in = b->vis + bl->visofs;
			int out = 0;
			while (out < rowbytes && in < b->vis + b->vis_len) {
				if (*in) { row[out++] = *in++; continue; }
				int c = in[1];
				in += 2;
				while (c-- && out < rowbytes) row[out++] = 0;
			}
			long load = 0;
			for (int j = 0; j < wm->visleafs; j++)
				if (row[j >> 3] & (1 << (j & 7)))
					load += b->leaves[j + 1].nummarks;
			for (int k = 0; k < 3; k++)
				if (load > best[k]) {
					for (int m = 2; m > k; m--) { best[m] = best[m - 1]; besti[m] = besti[m - 1]; }
					best[k] = load;
					besti[k] = i;
					break;
				}
		}
		free(row);
		for (int k = 0; k < 3; k++) {
			if (best[k] < 0)
				continue;
			const bsp_leaf *bl = &b->leaves[besti[k]];
			for (int yaw = 0; yaw < 360; yaw += 180) {
				qw_view *v = &views[nviews++];
				for (int c = 0; c < 3; c++)
					v->pos[c] = (bl->mins[c] + bl->maxs[c]) / 2;
				v->yaw = (int16_t) yaw;
				v->pitch = 0;
			}
		}
	}
	if (has_spawn) {
		/* near-geometry views for the slow path: the floor right under the
		   eye, and a wall at contact distance (12 units from it, about the
		   player's radius) */
		qw_view *v = &views[nviews++];
		*v = views[0];
		v->pitch = 60;

		/* the first of the 4 axial directions from the spawn that meets a
		   SOLID wall within 512 units (liquids are not walls) */
		for (int k = 0; k < 4; k++) {
			double yaw = (sa + 90.0 * k) * M_PI / 180.0, p[3] = { so[0], so[1], so[2] + 22 };
			double dx = cos(yaw), dy = sin(yaw);
			int hit = 0;
			for (int step = 0; step < 128 && !hit; step++) {
				double q[3] = { p[0] + dx * 4, p[1] + dy * 4, p[2] };
				int c = point_contents(b, root, q);
				if (c == -2)
					hit = 1;                         /* CONTENTS_SOLID */
				else if (c != -1)
					break;                           /* liquid: try another way */
				else
					memcpy(p, q, sizeof p);
			}
			if (!hit)
				continue;
			v = &views[nviews++];
			v->pos[0] = (int32_t) lround(p[0] - dx * 12);
			v->pos[1] = (int32_t) lround(p[1] - dy * 12);
			v->pos[2] = (int32_t) lround(p[2]);
			v->yaw    = (int16_t) (lround(sa + 90.0 * k) % 360);
			v->pitch  = 0;
			break;
		}
	}

	/* assemble */
	qw_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QWORLD_MAGIC, 4);
	h.num_nodes   = (uint32_t) b->num_nodes;
	h.num_leaves  = (uint32_t) b->num_leaves;
	h.num_marks   = (uint32_t) ngmarks;
	h.num_faces   = o.wrecords;                /* visibility ids: the world's records */
	h.num_records = o.records;
	h.num_pieces  = o.quads + o.tris;
	h.num_views   = (uint32_t) nviews;
	h.has_spawn   = (uint32_t) has_spawn;
	for (int k = 0; k < 3; k++)
		h.spawn[k] = (int32_t) lround(so[k]);
	h.spawn_yaw = (int32_t) lround(sa);
	h.root      = (uint32_t) root;
	h.max_depth = (uint32_t) depth_of(b, root);
	h.num_visleafs = (uint32_t) wm->visleafs;
	if (h.max_depth > QW_MAX_DEPTH)
		die("BSP depth %u exceeds the renderer stack (%d)", h.max_depth, QW_MAX_DEPTH);

	/* the brush models: bounds spread by a unit (Mod_LoadSubmodels), the
	   hull-0 root (its subtree is walked by the renderer), the clip roots */
	qw_bmodel *bmodels = xcalloc((size_t) b->num_models, sizeof *bmodels);
	h.max_bm_depth = 0;
	for (int m = 1; m < b->num_models; m++) {
		const bsp_model *bm = &b->models[m];
		qw_bmodel *q = &bmodels[m];
		for (int k = 0; k < 3; k++) {
			q->mins[k] = (int32_t) lround((bm->mins[k] - 1) * QW_CLIP_SUB);
			q->maxs[k] = (int32_t) lround((bm->maxs[k] + 1) * QW_CLIP_SUB);
		}
		q->root  = bm->headnode[0] >= 0 ? (uint32_t) bm->headnode[0]
		         : 0x80000000u | (uint32_t) (-1 - bm->headnode[0]);
		q->hull1 = bm->headnode[1];
		q->hull2 = bm->headnode[2];
		int d = depth_of(b, bm->headnode[0]);
		if (d > QW_MAX_BM_DEPTH)
			die("brush model %d: depth %d > %d", m, d, QW_MAX_BM_DEPTH);
		q->depth       = (uint16_t) d;
		q->num_records = (uint16_t) bm_records[m];
		if ((uint32_t) d > h.max_bm_depth)
			h.max_bm_depth = (uint32_t) d;
	}

	/* the entities */
	buf_t etab, estr;
	memset(&etab, 0, sizeof etab);
	memset(&estr, 0, sizeof estr);
	int unknown = 0;
	ents_cook(&ents, b->num_models, &etab, &estr, &h.worldtype, &h.title, &unknown);

	buf_t f;
	memset(&f, 0, sizeof f);
	buf_put(&f, &h, sizeof h);
	h.ofs_nodes = (uint32_t) f.len;  buf_put(&f, nodes, (size_t) (b->num_nodes + 1) * sizeof *nodes);
	h.ofs_leaves = (uint32_t) f.len; buf_put(&f, leaves, (size_t) b->num_leaves * sizeof *leaves);
	h.ofs_marks = (uint32_t) f.len;  buf_put(&f, gmarks, (size_t) ngmarks * 2); buf_align4(&f);
	h.ofs_vis = (uint32_t) f.len;    buf_put(&f, b->vis, b->vis_len); buf_align4(&f);
	h.len_vis = b->vis_len;
	h.ofs_faces = (uint32_t) f.len;  buf_put(&f, o.stream.p, o.stream.len);
	h.len_faces = (uint32_t) o.stream.len;
	{ static const uint8_t tail[QW_STREAM_TAIL]; buf_put(&f, tail, sizeof tail); }
	h.ofs_views = (uint32_t) f.len;  buf_put(&f, views, (size_t) nviews * sizeof *views);
	buf_align4(&f);

	/* the vertex pool every record indexes */
	h.ofs_verts = (uint32_t) f.len;
	h.num_verts = (uint32_t) o.npool;
	for (int i = 0; i < o.npool; i++) {
		qw_vert v = { o.pool[i].x, o.pool[i].y, o.pool[i].z, 0 };
		buf_put(&f, &v, sizeof v);
	}

	/* texture table: CLUT, tpage and texture window per miptex */
	h.ofs_ambient = (uint32_t) f.len;           /* the leaves' water / sky levels */
	for (int i = 0; i < b->num_leaves; i++)
		buf_put(&f, b->leaves[i].ambient, 2);
	buf_align4(&f);
	h.ofs_tex = (uint32_t) f.len;
	h.num_tex = (uint32_t) ts->num;
	for (int i = 0; i < ts->num; i++) {
		qw_tex t;
		memset(&t, 0, sizeof t);
		if (ts->slots[i].present) {
			t.clut   = ts->slots[i].clut;
			t.tpage  = ts->slots[i].tpage;
			t.window = ts->slots[i].window;
		}
		buf_put(&f, &t, sizeof t);
	}

	/* clip hulls 1 and 2 (all BSP planes, all clipnodes) */
	{
		h.ofs_clip = (uint32_t) f.len;
		qw_clip_header ch;
		ch.num_planes    = (uint32_t) b->num_planes;
		ch.num_clipnodes = (uint32_t) b->num_clipnodes;
		ch.hull1_root    = wm->headnode[1];
		ch.hull2_root    = wm->headnode[2];
		buf_put(&f, &ch, sizeof ch);
		for (int i = 0; i < b->num_planes; i++) {
			const bsp_plane *pl = &b->planes[i];
			qw_plane qp;
			for (int k = 0; k < 3; k++)
				qp.n[k] = clamp16(lround(pl->n[k] * 4096.0));
			qp.type = (uint8_t) (pl->type < 3 ? pl->type : 3);
			qp.pad  = 0;
			qp.dist = (int32_t) lround((double) pl->dist * 4096.0 * QW_CLIP_SUB);
			buf_put(&f, &qp, sizeof qp);
		}
		for (int i = 0; i < b->num_clipnodes; i++) {
			const bsp_clipnode *cn = &b->clipnodes[i];
			qw_clipnode qc;
			if (cn->planenum < 0 || cn->planenum >= b->num_planes || cn->planenum * 3 > 0xffff)
				die("clipnode %d: bad plane (or more than 21845 planes)", i);
			for (int c = 0; c < 2; c++)
				if (cn->child[c] >= b->num_clipnodes)
					die("clipnode %d: child out of range", i);
			qc.plane3   = (uint16_t) (cn->planenum * 3);
			qc.child[0] = (int16_t) cn->child[0];
			qc.child[1] = (int16_t) cn->child[1];
			buf_put(&f, &qc, sizeof qc);
		}
		h.len_clip = (uint32_t) f.len - h.ofs_clip;
		for (int m = 0; m < b->num_models; m++)
			for (int hull = 1; hull <= 2; hull++) {
				int d = clip_depth(b, b->models[m].headnode[hull]);
				if (d > QW_MAX_SPLIT)
					die("model %d clip hull %d depth %d > %d", m, hull, d, QW_MAX_SPLIT);
			}
	}

	/* the brush models, the entities and their strings */
	buf_align4(&f);                        /* (the clipnodes are 6 bytes) */
	h.ofs_bmodels = (uint32_t) f.len;
	h.num_bmodels = (uint32_t) b->num_models;
	buf_put(&f, bmodels, (size_t) b->num_models * sizeof *bmodels);
	h.ofs_ents = (uint32_t) f.len;
	h.num_ents = (uint32_t) (etab.len / sizeof(qw_ent));
	buf_put(&f, etab.p, etab.len);
	h.ofs_strings = (uint32_t) f.len;
	h.len_strings = (uint32_t) estr.len;
	buf_put(&f, estr.p, estr.len);
	buf_align4(&f);
	items_write(&f, &h.ofs_models, &h.num_models, &h.ofs_mgeo, &h.len_mgeo);

	/* the animated textures: their tiles, and the brush models drawing a
	   chain with alternates (the engine looks at their frame) */
	buf_align4(&f);
	h.ofs_anims = (uint32_t) f.len;
	h.num_anims = (uint32_t) ts->num_anims;
	for (int c = 0; c < ts->num_anims; c++) {
		const tex_anim *a = &ts->anims[c];
		qw_anim q;
		memset(&q, 0, sizeof q);
		#define TILE_OF(t, sl) do { const tex_slot *s_ = &ts->slots[sl]; (t).page = (uint8_t) s_->page; \
			(t).slot = (uint16_t) (sl); (t).u0 = (uint16_t) s_->u0; (t).v0 = (uint16_t) s_->v0; \
			(t).w = (uint16_t) s_->tile_w; (t).h = (uint16_t) s_->tile_h; } while (0)
		TILE_OF(q.display, a->display);
		q.num_frames = (uint8_t) a->nframes;
		q.num_alt    = (uint8_t) a->nalt;
		for (int k = 0; k < a->nframes; k++) TILE_OF(q.frames[k], a->frames[k]);
		for (int k = 0; k < a->nalt; k++)    TILE_OF(q.alt[k], a->alt[k]);
		#undef TILE_OF
		for (int m = 1; a->nalt && m < b->num_models && q.num_models < QW_ANIM_MODELS; m++) {
			const bsp_model *bm = &b->models[m];
			int uses = 0;
			for (int fc = bm->firstface; fc < bm->firstface + bm->numfaces && !uses; fc++) {
				int mt = face_miptex(b, fc);
				uses = mt >= 0 && ts->remap[mt] == a->display;
			}
			if (uses)
				q.models[q.num_models++] = (uint16_t) m;
		}
		buf_put(&f, &q, sizeof q);
	}
	memcpy(f.p, &h, sizeof h);

	FILE *fp = fopen(out_path, "wb");
	if (!fp)
		die("cannot write %s", out_path);
	fwrite(f.p, 1, f.len, fp);
	fclose(fp);

	printf("qcook: %s — %u B: nodes %zu, leaves %zu, marks %u, vis %u, faces %u\n",
	       out_path, (uint32_t) f.len, b->num_nodes * sizeof *nodes,
	       b->num_leaves * sizeof *leaves, ngmarks * 2, b->vis_len, h.len_faces);
	printf("qcook:   %u records, %u pieces -> %u quads + %u tris, %u vertex refs "
	       "(%.1f/record) into a pool of %d; %u faces split; depth %u; %d bench views\n",
	       o.records, o.pieces, o.quads, o.tris, o.verts,
	       o.records ? (double) o.verts / o.records : 0.0, o.npool, o.faces_split,
	       h.max_depth, nviews);
	printf("qcook:   T-junctions: %u vertices inserted into %u pieces; %u pieces ear-clipped\n",
	       tj_inserted, tj_pieces, o.pieces_eared);
	if (has_spawn)
		printf("qcook:   spawn %d %d %d yaw %d\n", h.spawn[0], h.spawn[1], h.spawn[2], h.spawn_yaw);
	{
		uint32_t brecs = o.records - o.wrecords;
		printf("qcook:   %d brush models, %u records, depth <= %u; %u entities (%d unknown), "
		       "%u B of strings; worldtype %u\n", b->num_models - 1, brecs, h.max_bm_depth,
		       h.num_ents, unknown, h.len_strings, h.worldtype);
	}

	free(f.p); free(o.stream.p); free(nodes); free(leaves); free(bmodels);
	free(etab.p); free(estr.p); ents_free(&ents);
	free(node_parent); free(leaf_parent); free(reach); free(bm_records);
	free(face_group); free(gmarks); free(gfirst); free(gcount); free(uf_up); free(edge_face);
	free(memb); free(order); free(o.pair_face); free(o.pair_rec);
	free(face_mfirst); free(face_mleaf); free(face_model);
}

/* ---------------------------------------------------------------- check -- */

#define FAIL(...) do { fprintf(stderr, "check: " __VA_ARGS__); fputc('\n', stderr); return 1; } while (0)

int world_check(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		FAIL("cannot open %s", path);
	fseek(fp, 0, SEEK_END);
	long len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	uint8_t *d = xmalloc((size_t) len);
	if (fread(d, 1, (size_t) len, fp) != (size_t) len)
		FAIL("short read");
	fclose(fp);

	qw_header h;
	if ((size_t) len < sizeof h)
		FAIL("too small");
	memcpy(&h, d, sizeof h);
	if (memcmp(h.magic, QWORLD_MAGIC, 4))
		FAIL("bad magic");

#define SECTION(ofs, n) do { if ((ofs) & 3) FAIL("unaligned section at %u", (ofs)); \
	if ((uint64_t) (ofs) + (n) > (uint64_t) len) FAIL("section at %u (+%u) past EOF", (ofs), (uint32_t) (n)); } while (0)
	SECTION(h.ofs_nodes, (h.num_nodes + 1) * sizeof(qw_node));
	SECTION(h.ofs_leaves, h.num_leaves * sizeof(qw_leaf));
	SECTION(h.ofs_marks, h.num_marks * 2);
	SECTION(h.ofs_vis, h.len_vis);
	SECTION(h.ofs_faces, h.len_faces);
	SECTION(h.ofs_views, h.num_views * sizeof(qw_view));
	SECTION(h.ofs_tex, h.num_tex * sizeof(qw_tex));
	SECTION(h.ofs_verts, h.num_verts * sizeof(qw_vert));
	SECTION(h.ofs_ambient, h.num_leaves * 2);
	SECTION(h.ofs_bmodels, h.num_bmodels * sizeof(qw_bmodel));
	SECTION(h.ofs_ents, h.num_ents * sizeof(qw_ent));
	SECTION(h.ofs_strings, h.len_strings);
	SECTION(h.ofs_anims, h.num_anims * sizeof(qw_anim));
	if (h.max_depth > QW_MAX_DEPTH)
		FAIL("depth %u > %d", h.max_depth, QW_MAX_DEPTH);
	if (h.max_bm_depth > QW_MAX_BM_DEPTH)
		FAIL("brush model depth %u > %d", h.max_bm_depth, QW_MAX_BM_DEPTH);
	if (h.num_faces > h.num_records)
		FAIL("more visibility ids than records");
	if (h.root >= h.num_nodes)
		FAIL("root %u out of range", h.root);

	const qw_node *nodes = (const qw_node *) (d + h.ofs_nodes);
	const qw_leaf *leaves = (const qw_leaf *) (d + h.ofs_leaves);
	if (!h.ofs_clip || h.len_clip < sizeof(qw_clip_header))
		FAIL("no clip section (the nodes' plane table)");
	uint32_t num_planes = ((const qw_clip_header *) (d + h.ofs_clip))->num_planes;
	const uint8_t *fs = d + h.ofs_faces;
	uint32_t recs = 0, polys = 0;

	for (uint32_t i = 0; i < h.num_nodes; i++) {
		const qw_node *n = &nodes[i];
		for (int c = 0; c < 2; c++) {
			uint32_t ch = n->child[c];
			if (ch & 0x80000000u) { if ((ch & 0x7fffffffu) >= h.num_leaves) FAIL("node %u: leaf child out of range", i); }
			else if (ch >= h.num_nodes) FAIL("node %u: child out of range", i);
		}
		if (n->parent != QW_NOPARENT && (n->parent % QW_NODE_WORDS || n->parent / QW_NODE_WORDS >= h.num_nodes))
			FAIL("node %u: parent out of range", i);
		if (n->plane3 % 3 || n->plane3 / 3 >= num_planes)
			FAIL("node %u: plane out of range", i);
		if (!(n->faces[0] <= n->faces[1] && n->faces[1] <= n[1].faces[0] && n[1].faces[0] <= h.len_faces))
			FAIL("node %u: face block offsets out of order", i);

		/* every record of both blocks must decode exactly to the block end */
		for (int side = 0; side < 2; side++) {
			uint32_t at = n->faces[side], end = qw_face_end(n, side);
			while (at < end) {
				qw_record r;
				if (at + sizeof r > end) FAIL("node %u: truncated record", i);
				memcpy(&r, fs + at, sizeof r);
				uint32_t nidx = (r.num_verts + 2u) / 3u * 3u;
				uint32_t pofs = (uint32_t) sizeof r + ((nidx * 2u + 3u) & ~3u) + ((r.num_verts + 3u) & ~3u);
				uint32_t sz = pofs + r.num_quads * (uint32_t) sizeof(qw_quad) +
				              r.num_tris * (uint32_t) sizeof(qw_tri);
				if (r.polys != pofs) FAIL("record at %u: polys at %u != %u", at, r.polys, pofs);
				if (r.size != sz) FAIL("record at %u: size %u != %u", at, r.size, sz);
				if (!r.num_verts || r.num_verts > QW_MAX_VERTS)
					FAIL("record at %u: %u verts", at, r.num_verts);
				if (r.face_id >= h.num_faces) FAIL("record at %u: face id %u", at, r.face_id);
				if (8u + r.num_quads * 52u + r.num_tris * 40u > QW_MAX_REC_PKT)
					FAIL("record at %u: %u quads + %u tris exceed QW_MAX_REC_PKT", at,
					     r.num_quads, r.num_tris);
				if (r.tex >= h.num_tex) FAIL("record at %u: texture %u", at, r.tex);
				const uint16_t *vi = (const uint16_t *) (fs + at + sizeof r);
				for (uint32_t k = 0; k < nidx; k++)
					if (vi[k] >= h.num_verts) FAIL("record at %u: vertex %u out of the pool", at, vi[k]);
				const uint8_t *q = fs + at + r.polys;
				for (int k = 0; k < r.num_quads; k++, q += sizeof(qw_quad))
					for (int c = 0; c < 4; c++)
						if (q[c] & 3 || q[c] / 4 >= r.num_verts) FAIL("record at %u: quad index", at);
				for (int k = 0; k < r.num_tris; k++, q += sizeof(qw_tri)) {
					for (int c = 0; c < 3; c++)
						if (q[c] & 3 || q[c] / 4 >= r.num_verts) FAIL("record at %u: tri index", at);
					if (q[10] | q[11])   /* the renderer stores uv2's word as is */
						FAIL("record at %u: tri pad2 not zero", at);
				}
				recs++;
				polys += r.num_quads + r.num_tris;
				at += r.size;
			}
			if (at != end) FAIL("node %u: block overrun", i);
		}
	}
	if (recs != h.num_records || polys != h.num_pieces)
		FAIL("record/poly count mismatch (%u/%u vs %u/%u)", recs, polys, h.num_records, h.num_pieces);

	for (uint32_t i = 0; i < h.num_leaves; i++) {
		const qw_leaf *l = &leaves[i];
		if (l->marks + l->num_marks > h.num_marks) FAIL("leaf %u: marks out of range", i);
		if (l->vis != QW_NONE && l->vis >= h.len_vis) FAIL("leaf %u: vis out of range", i);
		if (l->parent != QW_NOPARENT && (l->parent % QW_NODE_WORDS || l->parent / QW_NODE_WORDS >= h.num_nodes))
			FAIL("leaf %u: parent out of range", i);
	}
	const uint16_t *marks = (const uint16_t *) (d + h.ofs_marks);
	for (uint32_t i = 0; i < h.num_marks; i++)
		if (marks[i] >= h.num_faces) FAIL("mark %u out of range", i);

	/* brush models and entities */
	const qw_bmodel *bm = (const qw_bmodel *) (d + h.ofs_bmodels);
	uint32_t num_clipnodes = ((const qw_clip_header *) (d + h.ofs_clip))->num_clipnodes;
	for (uint32_t i = 1; i < h.num_bmodels; i++) {
		uint32_t r = bm[i].root;
		if (r & 0x80000000u) { if ((r & 0x7fffffffu) >= h.num_leaves) FAIL("bmodel %u: root leaf out of range", i); }
		else if (r >= h.num_nodes) FAIL("bmodel %u: root out of range", i);
		if (bm[i].hull1 < 0 || (uint32_t) bm[i].hull1 >= num_clipnodes ||
		    bm[i].hull2 < 0 || (uint32_t) bm[i].hull2 >= num_clipnodes)
			FAIL("bmodel %u: clip roots out of range", i);
		for (int k = 0; k < 3; k++)
			if (bm[i].mins[k] > bm[i].maxs[k]) FAIL("bmodel %u: bounds", i);
		if (bm[i].depth > h.max_bm_depth) FAIL("bmodel %u: depth %u", i, bm[i].depth);
	}
	const qw_ent *ents = (const qw_ent *) (d + h.ofs_ents);
	const char *strs = (const char *) (d + h.ofs_strings);
	if (h.len_strings < 1 || strs[0] || strs[h.len_strings - 1])
		FAIL("string pool not NUL-ended");
	for (uint32_t i = 0; i < h.num_ents; i++) {
		const qw_ent *e = &ents[i];
		if (e->class >= QE_COUNT) FAIL("entity %u: class %u", i, e->class);
		if (e->model < 0 || (uint32_t) e->model >= h.num_bmodels) FAIL("entity %u: model %d", i, e->model);
		uint16_t so[6] = { e->target, e->targetname, e->killtarget, e->message, e->map, e->classname };
		for (int k = 0; k < 6; k++)
			if (so[k] >= h.len_strings) FAIL("entity %u: string offset %u", i, so[k]);
		if (!e->classname) FAIL("entity %u: no classname", i);
	}
	if (h.title >= h.len_strings) FAIL("title string out of range");

	printf("check: %s OK — %u nodes, %u leaves, %u records (%u the world's), %u polys, depth %u; "
	       "%u brush models, %u entities\n",
	       path, h.num_nodes, h.num_leaves, recs, h.num_faces, polys, h.max_depth,
	       h.num_bmodels ? h.num_bmodels - 1 : 0, h.num_ents);
	free(d);
	return 0;
}
