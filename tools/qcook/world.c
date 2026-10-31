/*
 * qcook — world: QWVG file (formats/qworld.h), one per geometry level.
 *
 * Faces are attached to their BSP node (front block / back block), cut on
 * texture-repeat boundaries so every piece's UVs stay inside one VRAM tile
 * (the finest texture level's layout: every texture level maps the UVs onto
 * its own tiles, formats/qtex.h qtex_uvmap), at the ultra level on a light
 * grid as well, triangulated into PS1 quads (+ a triangle when odd) and
 * grouped into face records of <= 63 shared vertices.
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
   folds it onto the texture); another axis its position in the tile (the
   loader adds the tile's: formats/qtex.h qtex_uvmap). */
static uint16_t uvword(double u, double v)
{
	long uu = lround(u), vv = lround(v);
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
   (T-junction vertices, its own): clip the corner
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

/* A brush model's polygon turned the alias models' way round: the model
   loop (r_world.s RECLOOP de: draw_model, a brush model's blocks, its cut
   pieces) leaves out a triangle whose NCLIP is >= 0 (§54, §93), where
   Quake's winding (the world's faces, which the walk takes by their
   planes) gives a front face's a positive one: corners 1 and 2 swapped, a
   triangle's and a quad's alike (the quad keeps its diagonal: the GPU's two
   halves the same triangles), so the cut (engine/bmfrag.c) keeps the turn
   in every piece it makes of them (docs/porting-notes.md §109) */
static void bmodel_turn(uint8_t *idx, uint16_t *uv)
{
	uint8_t i = idx[1];
	idx[1] = idx[2];
	idx[2] = i;
	uint16_t u = uv[1];
	uv[1] = uv[2];
	uv[2] = u;
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
				q->uv[k]  = uvword(uv[pairs[e][k]][0], uv[pairs[e][k]][1]);
			}
			if (o->bmodel)
				bmodel_turn(q->idx, q->uv);
		}
		for (int e = 0; e < nsingle; e++) {
			qw_tri *t = &r->t[r->nt++];
			for (int q = 0; q < 3; q++) {
				t->idx[q] = (uint8_t) (idx[ears[single[e]][q]] * 4);
				t->uv[q]  = uvword(uv[ears[single[e]][q]][0], uv[ears[single[e]][q]][1]);
			}
			if (o->bmodel)
				bmodel_turn(t->idx, t->uv);
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
			q->uv[k]  = uvword(uv[c[k]][0], uv[c[k]][1]);
		}
		if (o->bmodel)
			bmodel_turn(q->idx, q->uv);
	}
	if (i + 1 < pc->n) {                       /* one triangle left */
		qw_tri *t = &r->t[r->nt++];
		int c[3] = { 0, i, i + 1 };
		for (int k = 0; k < 3; k++) {
			t->idx[k] = (uint8_t) (idx[c[k]] * 4);
			t->uv[k]  = uvword(uv[c[k]][0], uv[c[k]][1]);
		}
		if (o->bmodel)
			bmodel_turn(t->idx, t->uv);
		t->pad = 0;
		t->pad2 = 0;
	}
	o->pieces++;
}

/* ---------------------------------------------------------------- light -- */

double light_gain = 1.66;
const char *world_fmap;

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
   without a lightmap in a lit map is black, as in Quake; the liquids, which
   Quake draws unlit (D_DrawTurbulent8), take 128 instead (the caller). */
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

/* A brush model's face whose four corner lights, spread by the GPU's
   Gouraud, would miss its lightmap by more than QL_BM_LIGHT_ERR at some
   luxel (a shadow at a corner, a light in the middle): cut on the light
   grid (docs/porting-notes.md §96). The Gouraud is taken as the bilinear
   of the samples at the face's (s, t) extents, over every luxel of its
   box (a few outside an L-shaped face: a cut for nothing, harmless). */
static int bm_face_needs_grid(const bsp_t *b, int f, const poly_t *pl, double smin, double smax, double tmin, double tmax)
{
	facelight fl;
	face_light_setup(b, f, pl, &fl);
	if (!fl.nmaps || smax - smin < 16 || tmax - tmin < 16)
		return 0;
	double c00 = face_light_at(&fl, smin, tmin), c10 = face_light_at(&fl, smax, tmin);
	double c01 = face_light_at(&fl, smin, tmax), c11 = face_light_at(&fl, smax, tmax);
	for (int j = 0; j < fl.h; j++)
		for (int i = 0; i < fl.w; i++) {
			double sl = fl.smin + 16 * i, tl = fl.tmin + 16 * j;
			if (sl < smin || sl > smax || tl < tmin || tl > tmax)
				continue;
			double fs = (sl - smin) / (smax - smin), ft = (tl - tmin) / (tmax - tmin);
			double est = (c00 * (1 - fs) + c10 * fs) * (1 - ft) + (c01 * (1 - fs) + c11 * fs) * ft;
			if (fabs(est - face_light_at(&fl, sl, tl)) > QL_BM_LIGHT_ERR)
				return 1;
		}
	return 0;
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
   for the whole map, before records are built. A vertex lying on another
   piece's edge (a cut of one face along its neighbour's edge, qbsp's own
   leftovers: a T-junction) is left as it is: a crack may show there. */
typedef struct {
	int    face;
	int    model;           /* the Quake model the face belongs to        */
	double s_off, t_off;    /* the cell's corner, Quake texels            */
	poly_t pl;
	int    dead;            /* merged into another piece (QCOOK_MERGE)    */
	long   sub[2];          /* its sub-cell on the light grid (0 0: none) */
	int    mfirst, mn;      /* merged: its faces in pc_mem (mn 0: `face`) */
} piece_t;

static piece_t *pc_list;
static int     *face_model;              /* [num_faces]: the model of each face */
static int      pc_num, pc_cap;
static int     *pc_first, *pc_count;   /* per face */

static int bm_grid_faces;                /* the brush models' faces cut on their light grid (§96) */

/* the coplanar faces merged (QCOOK_MERGE, docs/porting-notes.md §113) */
static int     merge_on;
static int    *face_cl;                  /* [num_faces]: the face's cluster, -1 none (its own cells) */
static double *face_s0, *face_t0;        /* a cluster's cell origin, Quake texels */
static int    *pc_mem;                   /* the merged pieces' faces */
static int     pc_nmem, pc_memcap;
static uint32_t mg_pieces, mg_into, mg_dropped, mg_dead;

/* a face's polygon, its texture coordinates (Quake texels) at each vertex */
static void face_poly(const bsp_t *b, int f, poly_t *pl)
{
	const bsp_face    *bf = &b->faces[f];
	const bsp_texinfo *ti = &b->texinfo[bf->texinfo];
	pl->n = 0;
	for (int e = 0; e < bf->numedges && pl->n < MAXPV; e++) {
		int se = b->surfedges[bf->firstedge + e];
		int vi = se >= 0 ? b->edges[se * 2] : b->edges[-se * 2 + 1];
		pvert *v = &pl->v[pl->n++];
		for (int k = 0; k < 3; k++)
			v->p[k] = b->verts[vi * 3 + k];
		v->s = v->p[0] * ti->vecs[0][0] + v->p[1] * ti->vecs[0][1] + v->p[2] * ti->vecs[0][2] + ti->vecs[0][3];
		v->t = v->p[0] * ti->vecs[1][0] + v->p[1] * ti->vecs[1][1] + v->p[2] * ti->vecs[1][2] + ti->vecs[1][3];
	}
}

static void poly_st_box(const poly_t *pl, double *smin, double *smax, double *tmin, double *tmax)
{
	*smin = 1e30; *smax = -1e30; *tmin = 1e30; *tmax = -1e30;
	for (int i = 0; i < pl->n; i++) {
		if (pl->v[i].s < *smin) *smin = pl->v[i].s;
		if (pl->v[i].s > *smax) *smax = pl->v[i].s;
		if (pl->v[i].t < *tmin) *tmin = pl->v[i].t;
		if (pl->v[i].t > *tmax) *tmax = pl->v[i].t;
	}
}

/* the slot a drawn face takes, or NULL (no texture, sky, missing) */
static const tex_slot *face_drawn_slot(const bsp_t *b, const tex_set *ts, int f)
{
	const bsp_face *bf = &b->faces[f];
	if (bf->numedges < 3 || bf->texinfo < 0 || bf->texinfo >= b->num_texinfo)
		return NULL;
	const bsp_texinfo *ti = &b->texinfo[bf->texinfo];
	if (ti->miptex < 0 || ti->miptex >= b->num_miptex)
		return NULL;
	const tex_slot *s = face_slot(ts, ti->miptex);
	return s->present ? s : NULL;           /* (sky or missing: not drawn) */
}

static void face_pieces(const bsp_t *b, const tex_set *ts, int f)
{
	pc_first[f] = pc_num;
	pc_count[f] = 0;
	const tex_slot *s = face_drawn_slot(b, ts, f);
	if (!s)
		return;
	const bsp_texinfo *ti = &b->texinfo[b->faces[f].texinfo];
	const bsp_miptex  *m  = &b->miptex[ti->miptex];

	poly_t pl;
	face_poly(b, f, &pl);

	double smin, smax, tmin, tmax;
	poly_st_box(&pl, &smin, &smax, &tmin, &tmax);
	int bm_grid = face_model[f] && bm_face_needs_grid(b, f, &pl, smin, smax, tmin, tmax);
	if (bm_grid)
		bm_grid_faces++;

	/* Cells, in Quake texels. A wrapped axis: 256 cooked texels (a byte of
	   UV), from an origin on a repetition boundary so the virtual
	   coordinate keeps the texture's phase; the face's own origin, so it
	   is cut as little as possible. Another axis: the VRAM tile (one
	   repetition) on the global grid. A brush model's face whose corner
	   lights miss its lightmap (§96) is cut again every QL_BM_LIGHT_GRID
	   texels from the cell's corner (luxel boundaries: a texture's size is
	   a multiple of 16), the UVs still from the cell's corner, so the
	   vertex light follows the lightmap closer.
	   A face of a merged cluster (QCOOK_MERGE) takes the cluster's origin,
	   so that its faces' pieces in a cell can be merged. */
	const double W  = s->win_x ? 256.0 * m->w / s->rep_w : (double) m->w;
	const double H  = s->win_y ? 256.0 * m->h / s->rep_h : (double) m->h;
	double s0 = s->win_x ? floor(smin / m->w) * m->w : 0;
	double t0 = s->win_y ? floor(tmin / m->h) * m->h : 0;
	if (face_cl && face_cl[f] >= 0) {
		s0 = face_s0[f];
		t0 = face_t0[f];
	}
	long i0 = (long) floor((smin - s0) / W), i1 = (long) floor((smax - 1e-4 - s0) / W);
	long j0 = (long) floor((tmin - t0) / H), j1 = (long) floor((tmax - 1e-4 - t0) / H);

	for (long i = i0; i <= i1; i++) {
		poly_t a, c;
		clip_poly(&pl, &a, 0, s0 + i * W, +1);
		clip_poly(&a, &c, 0, s0 + (i + 1) * W, -1);
		for (long j = j0; j <= j1; j++) {
			poly_t d, cell;
			clip_poly(&c, &d, 1, t0 + j * H, +1);
			clip_poly(&d, &cell, 1, t0 + (j + 1) * H, -1);
			/* the light grid's lines inside the cell (none: the cell whole) */
			double cs0 = s0 + i * W, cs1 = cs0 + W, ct0 = t0 + j * H, ct1 = ct0 + H;
			double gs0 = cs0, gs1 = cs1, gt0 = ct0, gt1 = ct1;
			double G = face_model[f] && bm_grid ? QL_BM_LIGHT_GRID : 0;   /* (a brush model's face the
			                                                                 vertex lights misrepresent) */
			if (G > 0) {                 /* (the cell's sub-cells the face reaches) */
				gs0 = cs0 + floor(((smin > cs0 ? smin : cs0) - cs0) / G) * G;
				gt0 = ct0 + floor(((tmin > ct0 ? tmin : ct0) - ct0) / G) * G;
				gs1 = smax < cs1 ? smax : cs1;
				gt1 = tmax < ct1 ? tmax : ct1;
			} else {
				G = 1e30;
			}
			for (double gs = gs0; gs < gs1 - 1e-4; gs += G)
				for (double gt = gt0; gt < gt1 - 1e-4; gt += G) {
					poly_t e, g, h2, pc;
					clip_poly(&cell, &e, 0, gs > cs0 ? gs : cs0, +1);
					clip_poly(&e, &g, 0, gs + G < cs1 ? gs + G : cs1, -1);
					clip_poly(&g, &h2, 1, gt > ct0 ? gt : ct0, +1);
					clip_poly(&h2, &pc, 1, gt + G < ct1 ? gt + G : ct1, -1);
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
					p->s_off = cs0;
					p->t_off = ct0;
					p->pl    = pc;
					p->dead  = 0;
					p->sub[0] = G < 1e29 ? lround((gs - cs0) / G) : 0;
					p->sub[1] = G < 1e29 ? lround((gt - ct0) / G) : 0;
					p->mfirst = p->mn = 0;
					pc_count[f]++;
				}
		}
	}
}

/* a vertex position quantized to the pool's units (QW_VSCALE) */
static int32_t vert_quant(double v)
{
	return (int32_t) lround(v * QW_VSCALE);
}

/* ------------------------------------------------- coplanar faces merged -- */

/* QCOOK_MERGE (1 by default; 0 the cook as before; docs/porting-notes.md
   §113). The faces of a node's side with one texture mapping, marked by the
   same leaves (always drawn together: the records' own rule) and connected
   form a cluster, cut on one cell grid (its origin where the cluster covers
   the fewest cells), and its faces' pieces in a cell are merged while their
   union stays convex (qbsp cut them for its lightmaps, 240 texels, which
   the PS1 does not use). Then every vertex lying on a straight run of each
   piece that holds it is dropped from all of them: the inner edges' ends a
   merge left on the outline, and the vertices qbsp put into the walls'
   edges for them. A vertex that any piece keeps as a corner stays in every
   piece, so no edge loses a vertex its neighbour keeps (no new crack).
   The cost is the light: the vertex lights were sampled at the dropped
   points, and the common grid moves the cuts. QCOOK_MERGE_ERR (MG_ERR, 16
   by default) bounds it at each of Quake's luxels: a merge, a dropped
   vertex or a cluster's grid that would light one worse by more than that
   is not made (merge_check, merge_drop). A brush model's face cut on its
   light grid (§96) keeps its own cells. */
#define MG_TOL   (1.0 / 16)              /* world units: on a line, the same point */
#define MG_MAXV  32                      /* a merged piece's vertices at most */
#define MG_AREA  0.25                    /* square units: a union's hull no bigger than its pieces */

static int merge_face_ok(const bsp_t *b, const tex_set *ts, int f)
{
	if (face_model[f] < 0 || !face_drawn_slot(b, ts, f))
		return 0;
	if (face_model[f] > 0) {
		poly_t pl;
		double smin, smax, tmin, tmax;
		face_poly(b, f, &pl);
		poly_st_box(&pl, &smin, &smax, &tmin, &tmax);
		if (bm_face_needs_grid(b, f, &pl, smin, smax, tmin, tmax))
			return 0;
	}
	return 1;
}

/* one plane, one side, one texture mapping */
static int same_mapping(const bsp_t *b, int f, int g)
{
	const bsp_face *a = &b->faces[f], *c = &b->faces[g];
	if (a->side != c->side || a->planenum != c->planenum)
		return 0;
	if (a->texinfo == c->texinfo)
		return 1;
	const bsp_texinfo *x = &b->texinfo[a->texinfo], *y = &b->texinfo[c->texinfo];
	return x->miptex == y->miptex && x->flags == y->flags && !memcmp(x->vecs, y->vecs, sizeof x->vecs);
}

static int cmp_long2(const void *x, const void *y)
{
	const long *a = x, *c = y;
	return a[0] != c[0] ? (a[0] < c[0] ? -1 : 1) : a[1] != c[1] ? (a[1] < c[1] ? -1 : 1) : 0;
}

/* the clusters (face_cl: the cluster's first face; a face alone: -1) and
   their cell origins (face_s0, face_t0), before the faces are cut */
static void merge_clusters(const bsp_t *b, const tex_set *ts, const int *mfirst, const int *mleaf)
{
	face_cl = xmalloc((size_t) b->num_faces * sizeof *face_cl);
	face_s0 = xmalloc((size_t) b->num_faces * sizeof *face_s0);
	face_t0 = xmalloc((size_t) b->num_faces * sizeof *face_t0);
	int     *up = xmalloc((size_t) b->num_faces * sizeof *up);
	uint8_t *ok = xcalloc((size_t) b->num_faces, 1);
	int     *edge_face = xmalloc((size_t) b->num_edges * sizeof *edge_face);
	for (int f = 0; f < b->num_faces; f++) {
		face_cl[f] = -1;
		up[f] = f;
		ok[f] = (uint8_t) merge_face_ok(b, ts, f);
	}
	for (int e = 0; e < b->num_edges; e++)
		edge_face[e] = -1;
	for (int n = 0; n < b->num_nodes; n++) {
		int f0 = b->nodes[n].firstface, f1 = f0 + b->nodes[n].numfaces;
		for (int f = f0; f < f1; f++) {
			if (!ok[f])
				continue;
			for (int e = 0; e < b->faces[f].numedges; e++) {
				int se = b->surfedges[b->faces[f].firstedge + e];
				int ed = se >= 0 ? se : -se, g = edge_face[ed];
				if (g >= f0 && g < f && same_mapping(b, g, f) && same_marks(mfirst, mleaf, g, f))
					up[uf_find(up, f)] = uf_find(up, g);
				edge_face[ed] = f;
			}
		}
	}

	/* each cluster's cells: on a wrapped axis, every origin on a
	   repetition within one cell's reach of its lowest face's, the one
	   whose cells the faces' boxes reach fewest of (the lowest on a tie) */
	int *memb = xmalloc((size_t) b->num_faces * sizeof *memb), clusters = 0, faces = 0;
	long (*cells)[2] = NULL;
	size_t ccap = 0;
	for (int f = 0; f < b->num_faces; f++) {
		if (ok[f] != 1)
			continue;                                  /* (not merged, or in a cluster already) */
		int root = uf_find(up, f), nm = 0;
		for (int g = f; g < b->num_faces; g++)        /* (f its first face: the cluster's id) */
			if (ok[g] == 1 && uf_find(up, g) == root) {
				memb[nm++] = g;
				ok[g] = 2;
			}
		if (nm < 2)
			continue;
		const tex_slot   *s = face_drawn_slot(b, ts, f);
		const bsp_miptex *m = &b->miptex[b->texinfo[b->faces[f].texinfo].miptex];
		const double W = s->win_x ? 256.0 * m->w / s->rep_w : (double) m->w;
		const double H = s->win_y ? 256.0 * m->h / s->rep_h : (double) m->h;
		double (*box)[4] = xmalloc((size_t) nm * sizeof *box), smin = 1e30, tmin = 1e30;
		for (int k = 0; k < nm; k++) {
			poly_t pl;
			face_poly(b, memb[k], &pl);
			poly_st_box(&pl, &box[k][0], &box[k][1], &box[k][2], &box[k][3]);
			if (box[k][0] < smin) smin = box[k][0];
			if (box[k][2] < tmin) tmin = box[k][2];
		}
		double bs = s->win_x ? floor(smin / m->w) * m->w : 0, bt = s->win_y ? floor(tmin / m->h) * m->h : 0;
		int ns = s->win_x ? (int) lround(W / m->w) : 1, nt = s->win_y ? (int) lround(H / m->h) : 1;
		double best_s = bs, best_t = bt;
		size_t best = (size_t) -1;
		for (int ks = 0; ks < ns; ks++)
			for (int kt = 0; kt < nt; kt++) {
				double s0 = bs - ks * m->w, t0 = bt - kt * m->h;
				size_t nc = 0;
				for (int k = 0; k < nm; k++) {
					long i0 = (long) floor((box[k][0] - s0) / W), i1 = (long) floor((box[k][1] - 1e-4 - s0) / W);
					long j0 = (long) floor((box[k][2] - t0) / H), j1 = (long) floor((box[k][3] - 1e-4 - t0) / H);
					for (long i = i0; i <= i1; i++)
						for (long j = j0; j <= j1; j++) {
							if (nc == ccap) {
								ccap = ccap ? 2 * ccap : 1024;
								cells = realloc(cells, ccap * sizeof *cells);
								if (!cells)
									die("out of memory (merge)");
							}
							cells[nc][0] = i;
							cells[nc][1] = j;
							nc++;
						}
				}
				qsort(cells, nc, sizeof *cells, cmp_long2);
				size_t u = nc ? 1 : 0;
				for (size_t c = 1; c < nc; c++)
					u += cmp_long2(cells[c], cells[c - 1]) != 0;
				if (u < best) {
					best = u;
					best_s = s0;
					best_t = t0;
				}
			}
		for (int k = 0; k < nm; k++) {
			face_cl[memb[k]] = f;
			face_s0[memb[k]] = best_s;
			face_t0[memb[k]] = best_t;
		}
		free(box);
		clusters++;
		faces += nm;
	}
	printf("qcook:   merge: %d clusters of %d faces on one cell grid each\n", clusters, faces);
	free(cells); free(memb); free(edge_face); free(ok); free(up);
}

/* 2D: the plane's two axes other than its normal's largest */
typedef struct { double x, y; } pt2;

static pt2 to2(const double p[3], int ax)
{
	pt2 r = { p[ax == 0 ? 1 : 0], p[ax == 2 ? 1 : 2] };
	return r;
}

static double cross3(pt2 o, pt2 a, pt2 c)
{
	return (a.x - o.x) * (c.y - o.y) - (a.y - o.y) * (c.x - o.x);
}

static double area2(const poly_t *pl, int ax)        /* signed, twice */
{
	double a = 0;
	for (int i = 0; i < pl->n; i++) {
		pt2 p = to2(pl->v[i].p, ax), q = to2(pl->v[(i + 1) % pl->n].p, ax);
		a += p.x * q.y - p.y * q.x;
	}
	return a;
}

static int same_point(const pvert *a, const pvert *c)
{
	for (int k = 0; k < 3; k++)
		if (vert_quant(a->p[k]) != vert_quant(c->p[k]))
			return 0;
	return 1;
}

/* The light a set of faces gives a point: the lightmap of the face that
   holds it (the deepest inside, in texture space: one mapping for all).
   The merge keeps Quake's light within QCOOK_MERGE_ERR (MG_ERR, 0..255 at
   the cook's gain; negative: no limit) of what the vertex lights spread
   by the GPU's Gouraud give there: a merge whose dropped points the merged
   piece's corners light worse is not made, a vertex whose neighbours on
   its run light it worse is kept. */
#define MG_MAXF 64
#define MG_ERR  16
static double mg_err = MG_ERR;

typedef struct {
	const bsp_t *b;
	int       nf, liquid;
	int       face[MG_MAXF];
	poly_t    st[MG_MAXF];
	facelight fl[MG_MAXF];
	double    sign[MG_MAXF];
} mg_light;

static int mg_light_set(mg_light *L, const bsp_t *b, const tex_set *ts, const int *faces, int nf)
{
	if (nf > MG_MAXF)
		return 0;
	L->b = b;
	L->nf = nf;
	L->liquid = (face_slot(ts, b->texinfo[b->faces[faces[0]].texinfo].miptex)->flags & QWF_LIQUID) != 0;
	for (int k = 0; k < nf; k++) {
		L->face[k] = faces[k];
		face_poly(b, faces[k], &L->st[k]);
		face_light_setup(b, faces[k], &L->st[k], &L->fl[k]);
		double a = 0;
		const poly_t *pl = &L->st[k];
		for (int i = 0; i < pl->n; i++)
			a += pl->v[i].s * pl->v[(i + 1) % pl->n].t - pl->v[i].t * pl->v[(i + 1) % pl->n].s;
		L->sign[k] = a < 0 ? -1 : 1;
	}
	return 1;
}

static uint8_t mg_light_at(const mg_light *L, const pvert *v, int *src)
{
	double best = -1e30;
	int    at = 0;
	for (int k = 0; k < L->nf && L->nf > 1; k++) {
		const poly_t *pl = &L->st[k];
		double in = 1e30;
		for (int i = 0; i < pl->n; i++) {
			const pvert *e0 = &pl->v[i], *e1 = &pl->v[(i + 1) % pl->n];
			double ds = e1->s - e0->s, dt = e1->t - e0->t, len = sqrt(ds * ds + dt * dt);
			if (len < 1e-6)
				continue;
			double c = L->sign[k] * (ds * (v->t - e0->t) - dt * (v->s - e0->s)) / len;
			if (c < in)
				in = c;
		}
		if (in > best) {
			best = in;
			at = k;
		}
	}
	if (src)
		*src = L->face[at];
	return face_light_at(&L->fl[at], v->s, v->t);
}

/* the light the GPU gives point q of convex polygon u, its corners lit ul:
   Gouraud over the fan from corner 0 (rec_add_piece's), in the triangle q
   is the deepest in */
static double fan_light(const poly_t *u, const double *ul, int ax, pt2 q)
{
	double best = -1e30, val = ul[0];
	pt2 p0 = to2(u->v[0].p, ax);
	for (int i = 1; i + 1 < u->n; i++) {
		pt2 p1 = to2(u->v[i].p, ax), p2 = to2(u->v[i + 1].p, ax);
		double d = cross3(p0, p1, p2);
		if (fabs(d) < 1e-9)
			continue;
		double l1 = cross3(q, p1, p2) / d, l2 = cross3(p0, q, p2) / d, l3 = 1 - l1 - l2;
		double m = l1 < l2 ? (l1 < l3 ? l1 : l3) : (l2 < l3 ? l2 : l3);
		if (m > best) {
			best = m;
			val = l1 * ul[0] + l2 * ul[i] + l3 * ul[i + 1];
		}
	}
	return val;
}

static int    g_ax;
static pvert *g_pts;
static int cmp_pt(const void *x, const void *y)
{
	pt2 a = to2(g_pts[*(const int *) x].p, g_ax), c = to2(g_pts[*(const int *) y].p, g_ax);
	return a.x != c.x ? (a.x < c.x ? -1 : 1) : a.y != c.y ? (a.y < c.y ? -1 : 1) : 0;
}

/* a and c merged into u if their union is convex (its hull's area theirs):
   the hull's corners, and every vertex of either on the hull's outline, in
   a's winding */
static int try_union(const poly_t *a, const poly_t *c, int ax, const mg_light *L, poly_t *u)
{
	static pvert pts[2 * MAXPV];
	int n = 0;
	for (int i = 0; i < a->n + c->n; i++) {
		const pvert *v = i < a->n ? &a->v[i] : &c->v[i - a->n];
		int seen = 0;
		for (int j = 0; j < n && !seen; j++)
			seen = same_point(&pts[j], v);
		if (!seen)
			pts[n++] = *v;
	}
	int idx[2 * MAXPV], hull[4 * MAXPV], nh = 0;
	for (int i = 0; i < n; i++)
		idx[i] = i;
	g_ax = ax;
	g_pts = pts;
	qsort(idx, (size_t) n, sizeof *idx, cmp_pt);
	for (int i = 0; i < n; i++) {                      /* Andrew's monotone chain, CCW */
		while (nh >= 2 && cross3(to2(pts[hull[nh - 2]].p, ax), to2(pts[hull[nh - 1]].p, ax), to2(pts[idx[i]].p, ax)) <= 1e-9)
			nh--;
		hull[nh++] = idx[i];
	}
	for (int i = n - 2, lo = nh + 1; i >= 0; i--) {
		while (nh >= lo && cross3(to2(pts[hull[nh - 2]].p, ax), to2(pts[hull[nh - 1]].p, ax), to2(pts[idx[i]].p, ax)) <= 1e-9)
			nh--;
		hull[nh++] = idx[i];
	}
	nh--;                                              /* (the first again) */
	if (nh < 3)
		return 0;
	double ha = 0;
	for (int i = 0; i < nh; i++) {
		pt2 p = to2(pts[hull[i]].p, ax), q = to2(pts[hull[(i + 1) % nh]].p, ax);
		ha += p.x * q.y - p.y * q.x;
	}
	if (fabs(ha / 2 - (fabs(area2(a, ax)) + fabs(area2(c, ax))) / 2) > MG_AREA)
		return 0;                                      /* not convex (or apart) */
	u->n = 0;
	for (int i = 0; i < nh; i++) {
		const pvert *h0 = &pts[hull[i]], *h1 = &pts[hull[(i + 1) % nh]];
		if (u->n >= MG_MAXV)
			return 0;
		u->v[u->n++] = *h0;
		pt2 p = to2(h0->p, ax), q = to2(h1->p, ax);
		double dx = q.x - p.x, dy = q.y - p.y, L2 = dx * dx + dy * dy, L = sqrt(L2);
		struct { double t; int k; } on[2 * MAXPV];
		int non = 0;
		for (int k = 0; k < n; k++) {                  /* the vertices on this edge, in order */
			pt2 r = to2(pts[k].p, ax);
			double t = ((r.x - p.x) * dx + (r.y - p.y) * dy) / L2;
			if (t * L < MG_TOL || (1 - t) * L < MG_TOL || fabs(cross3(p, q, r)) / L > MG_TOL)
				continue;
			int at = non++;
			for (; at > 0 && on[at - 1].t > t; at--)
				on[at] = on[at - 1];
			on[at].t = t;
			on[at].k = k;
		}
		for (int k = 0; k < non; k++) {
			if (u->n >= MG_MAXV)
				return 0;
			u->v[u->n++] = pts[on[k].k];
		}
	}
	if ((area2(a, ax) < 0) != (area2(u, ax) < 0))      /* a's winding */
		for (int i = 0, j = u->n - 1; i < j; i++, j--) {
			pvert t = u->v[i];
			u->v[i] = u->v[j];
			u->v[j] = t;
		}
	if (mg_err >= 0 && !L->liquid) {                   /* the points it drops: lit as before? */
		double ul[MAXPV];
		for (int i = 0; i < u->n; i++)
			ul[i] = mg_light_at(L, &u->v[i], NULL);
		for (int k = 0; k < n; k++) {
			int kept = 0;
			for (int i = 0; i < u->n && !kept; i++)
				kept = same_point(&u->v[i], &pts[k]);
			if (!kept && fabs(fan_light(u, ul, ax, to2(pts[k].p, ax)) - mg_light_at(L, &pts[k], NULL)) > mg_err)
				return 0;
		}
	}
	return 1;
}

static int cmp_cell(const void *x, const void *y)
{
	const piece_t *a = &pc_list[*(const int *) x], *c = &pc_list[*(const int *) y];
	if (face_cl[a->face] != face_cl[c->face])
		return face_cl[a->face] < face_cl[c->face] ? -1 : 1;
	if (a->s_off != c->s_off)
		return a->s_off < c->s_off ? -1 : 1;
	if (a->t_off != c->t_off)
		return a->t_off < c->t_off ? -1 : 1;
	for (int k = 0; k < 2; k++)
		if (a->sub[k] != c->sub[k])
			return a->sub[k] < c->sub[k] ? -1 : 1;
	return *(const int *) x < *(const int *) y ? -1 : 1;
}

/* a cluster's pieces in one cell merged while their union is convex: the
   merged piece in its first piece's place, the others dead */
static void merge_pieces(const bsp_t *b, const tex_set *ts)
{
	static mg_light L;
	int *ids = xmalloc((size_t) (pc_num + 1) * sizeof *ids), n = 0;
	for (int i = 0; i < pc_num; i++)
		if (face_cl[pc_list[i].face] >= 0)
			ids[n++] = i;
	qsort(ids, (size_t) n, sizeof *ids, cmp_cell);
	poly_t *cur = NULL;
	int    *owner = NULL, cap = 0;
	for (int r0 = 0, r1; r0 < n; r0 = r1) {
		for (r1 = r0 + 1; r1 < n; r1++) {
			const piece_t *a = &pc_list[ids[r0]], *c = &pc_list[ids[r1]];
			if (face_cl[a->face] != face_cl[c->face] || a->s_off != c->s_off || a->t_off != c->t_off ||
			    a->sub[0] != c->sub[0] || a->sub[1] != c->sub[1])
				break;
		}
		int k = r1 - r0;
		if (k < 2)
			continue;
		if (k > cap) {
			cap   = k;
			cur   = realloc(cur, (size_t) cap * sizeof *cur);
			owner = realloc(owner, (size_t) cap * sizeof *owner);
			if (!cur || !owner)
				die("out of memory (merge)");
		}
		const float *nrm = b->planes[b->faces[pc_list[ids[r0]].face].planenum].n;
		int ax = fabs(nrm[0]) >= fabs(nrm[1]) && fabs(nrm[0]) >= fabs(nrm[2]) ? 0 : fabs(nrm[1]) >= fabs(nrm[2]) ? 1 : 2;
		int rf[MG_MAXF + 1];
		if (k > MG_MAXF)
			continue;                                  /* (too many faces in one cell) */
		for (int x = 0; x < k; x++)
			rf[x] = pc_list[ids[r0 + x]].face;
		mg_light_set(&L, b, ts, rf, k);
		for (int x = 0; x < k; x++) {
			cur[x]   = pc_list[ids[r0 + x]].pl;
			owner[x] = x;                              /* (merged into owner[x]) */
		}
		for (int changed = 1; changed; ) {
			changed = 0;
			for (int x = 0; x < k; x++)
				for (int y = x + 1; y < k && owner[x] == x; y++) {
					poly_t u;
					if (owner[y] != y || !try_union(&cur[x], &cur[y], ax, &L, &u))
						continue;
					cur[x] = u;
					for (int z = 0; z < k; z++)
						if (owner[z] == y)
							owner[z] = x;
					changed = 1;
				}
		}
		for (int x = 0; x < k; x++) {
			if (owner[x] != x)
				continue;
			int nf = 0;
			for (int z = 0; z < k; z++)
				nf += owner[z] == x;
			if (nf < 2)
				continue;
			piece_t *lead = &pc_list[ids[r0 + x]];
			lead->pl = cur[x];
			lead->mfirst = pc_nmem;
			lead->mn = nf;
			for (int z = 0; z < k; z++) {
				if (owner[z] != x)
					continue;
				if (pc_nmem == pc_memcap) {
					pc_memcap = pc_memcap ? 2 * pc_memcap : 1024;
					pc_mem = realloc(pc_mem, (size_t) pc_memcap * sizeof *pc_mem);
					if (!pc_mem)
						die("out of memory (merge)");
				}
				pc_mem[pc_nmem++] = pc_list[ids[r0 + z]].face;
				if (z != x)
					pc_list[ids[r0 + z]].dead = 1;
			}
		}
	}
	free(cur); free(owner); free(ids);
}

/* is vertex k of pl on the straight line between its neighbours? */
static int on_run(const poly_t *pl, int k)
{
	const double *p = pl->v[(k + pl->n - 1) % pl->n].p, *v = pl->v[k].p, *q = pl->v[(k + 1) % pl->n].p;
	double d[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] }, w[3] = { v[0] - p[0], v[1] - p[1], v[2] - p[2] };
	double L2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	if (L2 < MG_TOL * MG_TOL)
		return 0;
	double t = (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / L2, L = sqrt(L2);
	double cx = w[1] * d[2] - w[2] * d[1], cy = w[2] * d[0] - w[0] * d[2], cz = w[0] * d[1] - w[1] * d[0];
	return t * L >= MG_TOL && (1 - t) * L >= MG_TOL && (cx * cx + cy * cy + cz * cz) / L2 <= MG_TOL * MG_TOL;
}

/* A set of pieces' light at (s, t), as the GPU spreads their vertex lights:
   the piece holding the point (the deepest inside, in texture space), the
   fan from its corner 0 */
typedef struct { const poly_t *pl; double l[MAXPV]; double sign; } mg_lit;

static double lit_at(const mg_lit *ps, int n, double s, double t)
{
	double best = -1e30;
	int    at = -1;
	for (int i = 0; i < n; i++) {
		const poly_t *pl = ps[i].pl;
		double in = 1e30;
		for (int k = 0; k < pl->n; k++) {
			const pvert *e0 = &pl->v[k], *e1 = &pl->v[(k + 1) % pl->n];
			double ds = e1->s - e0->s, dt = e1->t - e0->t, len = sqrt(ds * ds + dt * dt);
			if (len < 1e-6)
				continue;
			double c = ps[i].sign * (ds * (t - e0->t) - dt * (s - e0->s)) / len;
			if (c < in)
				in = c;
		}
		if (in > best) {
			best = in;
			at = i;
		}
	}
	if (at < 0)
		return 0;
	const poly_t *pl = ps[at].pl;
	const double *l = ps[at].l;
	double bv = -1e30, val = l[0];
	for (int i = 1; i + 1 < pl->n; i++) {
		const pvert *p0 = &pl->v[0], *p1 = &pl->v[i], *p2 = &pl->v[i + 1];
		double d = (p1->s - p0->s) * (p2->t - p0->t) - (p1->t - p0->t) * (p2->s - p0->s);
		if (fabs(d) < 1e-9)
			continue;
		double l1 = ((p1->s - s) * (p2->t - t) - (p1->t - t) * (p2->s - s)) / d;
		double l2 = ((s - p0->s) * (p2->t - p0->t) - (t - p0->t) * (p2->s - p0->s)) / d;
		double l3 = 1 - l1 - l2, m = l1 < l2 ? (l1 < l3 ? l1 : l3) : (l2 < l3 ? l2 : l3);
		if (m > bv) {
			bv = m;
			val = l1 * l[0] + l2 * l[i] + l3 * l[i + 1];
		}
	}
	return val;
}

static void lit_set(mg_lit *x, const bsp_t *b, const tex_set *ts, const piece_t *p)
{
	static mg_light L;
	mg_light_set(&L, b, ts, p->mn ? &pc_mem[p->mfirst] : &p->face, p->mn ? p->mn : 1);
	x->pl = &p->pl;
	double a = 0;
	for (int k = 0; k < p->pl.n; k++) {
		x->l[k] = mg_light_at(&L, &p->pl.v[k], NULL);
		a += p->pl.v[k].s * p->pl.v[(k + 1) % p->pl.n].t - p->pl.v[k].t * p->pl.v[(k + 1) % p->pl.n].s;
	}
	x->sign = a < 0 ? -1 : 1;
}

/* A cluster whose cut on its common grid lights a luxel of Quake's
   lightmap worse by more than MG_ERR than its faces' own cuts did (the cut
   lines moved: the vertex lights sampled elsewhere, merges or not) takes
   its faces' own cuts back, unmerged. Before merge_drop (each dropped
   vertex is checked there). */
static void merge_check(const bsp_t *b, const tex_set *ts)
{
	if (mg_err < 0)
		return;
	int *memb = xmalloc((size_t) b->num_faces * sizeof *memb), back = 0, kept = 0;
	mg_lit *now = NULL, *was = NULL;
	int ncap = 0, wcap = 0;
	piece_t *own = NULL;
	int ocap = 0;
	for (int c = 0; c < b->num_faces; c++) {
		if (face_cl[c] != c)
			continue;
		int nm = 0;
		for (int f = c; f < b->num_faces; f++)
			if (face_cl[f] == c)
				memb[nm++] = f;
		/* the faces' own cuts (face_pieces with no cluster: appended, then
		   taken out of the list) */
		int base = pc_num, no = 0;
		for (int k = 0; k < nm; k++) {
			int f = memb[k], f0 = pc_first[f], fn = pc_count[f];
			face_cl[f] = -1;
			face_pieces(b, ts, f);
			face_cl[f] = c;
			pc_first[f] = f0;
			pc_count[f] = fn;
		}
		no = pc_num - base;
		if (no > ocap) {
			ocap = no;
			own = realloc(own, (size_t) ocap * sizeof *own);
			if (!own) die("out of memory (merge)");
		}
		memcpy(own, &pc_list[base], (size_t) no * sizeof *own);
		pc_num = base;
		if (no > wcap) {
			wcap = no;
			was = realloc(was, (size_t) wcap * sizeof *was);
			if (!was) die("out of memory (merge)");
		}
		/* the pieces now (their faces' ranges hold the merged ones; after
		   the own cuts, whose appending may move the list) */
		int nn = 0;
		for (int k = 0; k < nm; k++)
			for (int i = 0; i < pc_count[memb[k]]; i++) {
				const piece_t *p = &pc_list[pc_first[memb[k]] + i];
				if (p->dead)
					continue;
				if (nn == ncap) {
					ncap = ncap ? 2 * ncap : 64;
					now = realloc(now, (size_t) ncap * sizeof *now);
					if (!now) die("out of memory (merge)");
				}
				lit_set(&now[nn++], b, ts, p);
			}
		for (int i = 0; i < no; i++)
			lit_set(&was[i], b, ts, &own[i]);
		/* every luxel of every face: no worse by more than MG_ERR */
		int worse = 0;
		for (int k = 0; k < nm && !worse; k++) {
			poly_t pl;
			facelight fl;
			face_poly(b, memb[k], &pl);
			face_light_setup(b, memb[k], &pl, &fl);
			if (!fl.nmaps)
				continue;
			mg_lit self;
			piece_t fp = { 0 };
			fp.face = memb[k];
			fp.pl = pl;
			lit_set(&self, b, ts, &fp);              /* (its polygon: the inside test) */
			for (int j = 0; j < fl.h && !worse; j++)
				for (int i = 0; i < fl.w && !worse; i++) {
					double sl = fl.smin + 16 * i, tl = fl.tmin + 16 * j, in = 1e30;
					for (int e = 0; e < pl.n; e++) {
						const pvert *e0 = &pl.v[e], *e1 = &pl.v[(e + 1) % pl.n];
						double ds = e1->s - e0->s, dt = e1->t - e0->t, len = sqrt(ds * ds + dt * dt);
						if (len >= 1e-6) {
							double cc = self.sign * (ds * (tl - e0->t) - dt * (sl - e0->s)) / len;
							if (cc < in) in = cc;
						}
					}
					if (in < 0)
						continue;                    /* (a luxel outside the face) */
					double truth = face_light_at(&fl, sl, tl);
					double e_now = fabs(lit_at(now, nn, sl, tl) - truth);
					double e_was = fabs(lit_at(was, no, sl, tl) - truth);
					worse = e_now > e_was + mg_err;
				}
		}
		if (!worse) {
			kept++;
			continue;
		}
		back++;                                      /* the faces' own cuts back */
		for (int k = 0; k < nm; k++) {
			int f = memb[k];
			for (int i = 0; i < pc_count[f]; i++)
				pc_list[pc_first[f] + i].dead = 1;
			pc_first[f] = pc_num;
			pc_count[f] = 0;
		}
		for (int i = 0; i < no; i++) {
			if (pc_num == pc_cap) {
				pc_cap = pc_cap ? 2 * pc_cap : 4096;
				pc_list = realloc(pc_list, (size_t) pc_cap * sizeof *pc_list);
				if (!pc_list) die("out of memory (pieces)");
			}
			int f = own[i].face;
			if (!pc_count[f])
				pc_first[f] = pc_num;
			pc_list[pc_num++] = own[i];
			pc_count[f]++;
		}
		for (int k = 0; k < nm; k++)
			face_cl[memb[k]] = -1;
	}
	printf("qcook:   merge: %d clusters on their grid, %d back to their faces' own cuts (the light)\n", kept, back);
	free(memb); free(now); free(was); free(own);
}

/* every vertex on a straight run of every live piece holding it (within its
   model) dropped from all of them, unless the light along the run would
   miss Quake's there by more than MG_ERR in one of them */
typedef struct { int32_t q[3]; int model, users, runs, keep; } mg_vert;

static mg_vert *mg_find(mg_vert *h, uint32_t mask, const pvert *v, int model)
{
	int32_t q[3] = { vert_quant(v->p[0]), vert_quant(v->p[1]), vert_quant(v->p[2]) };
	uint32_t at = ((uint32_t) q[0] * 73856093u ^ (uint32_t) q[1] * 19349663u ^ (uint32_t) q[2] * 83492791u ^
	               (uint32_t) model * 2654435761u) & mask;
	for (;; at = (at + 1) & mask) {
		mg_vert *e = &h[at];
		if (!e->users) {
			memcpy(e->q, q, sizeof q);
			e->model = model;
			return e;
		}
		if (e->model == model && !memcmp(e->q, q, sizeof q))
			return e;
	}
}

static void merge_drop(const bsp_t *b, const tex_set *ts)
{
	static mg_light L;
	size_t nv = 0;
	for (int i = 0; i < pc_num; i++)
		if (!pc_list[i].dead)
			nv += (size_t) pc_list[i].pl.n;
	uint32_t size = 1024;
	while (size < 2 * nv)
		size *= 2;
	mg_vert **key = xmalloc((nv + 1) * sizeof *key);    /* each live piece's vertices' */
	double   *lit = xmalloc((nv + 1) * sizeof *lit);    /* ... and Quake's light there */
	int      *off = xmalloc((size_t) (pc_num + 1) * sizeof *off);
	mg_vert  *h = xcalloc(size, sizeof *h);
	size_t at = 0;
	for (int i = 0; i < pc_num; i++) {
		const piece_t *p = &pc_list[i];
		off[i] = (int) at;
		if (p->dead)
			continue;
		mg_light_set(&L, b, ts, p->mn ? &pc_mem[p->mfirst] : &p->face, p->mn ? p->mn : 1);
		for (int k = 0; k < p->pl.n; k++, at++) {
			key[at] = mg_find(h, size - 1, &p->pl.v[k], p->model);
			key[at]->users++;
			key[at]->runs += on_run(&p->pl, k);
			lit[at] = L.liquid ? 0 : mg_light_at(&L, &p->pl.v[k], NULL);
		}
	}
#define DROPS(e) ((e)->runs == (e)->users && !(e)->keep)
	for (int round = 0, vetoes = 1; vetoes && mg_err >= 0 && round < 16; round++) {
		vetoes = 0;                                 /* (a kept vertex shortens its neighbours' runs) */
		for (int i = 0; i < pc_num; i++) {
			const piece_t *p = &pc_list[i];
			int n = p->pl.n;
			for (int k = 0; k < n && !p->dead; k++) {
				mg_vert **K = &key[off[i]];
				if (!DROPS(K[k]))
					continue;
				int a = k, c = k;
				do a = (a + n - 1) % n; while (a != k && DROPS(K[a]));
				do c = (c + 1) % n; while (c != k && DROPS(K[c]));
				if (a == k || c == k)
					continue;
				const double *A = p->pl.v[a].p, *C = p->pl.v[c].p, *V = p->pl.v[k].p;
				double d[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
				double L2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
				double t = L2 > 0 ? ((V[0] - A[0]) * d[0] + (V[1] - A[1]) * d[1] + (V[2] - A[2]) * d[2]) / L2 : 0;
				const double *l = &lit[off[i]];
				if (fabs(l[a] + t * (l[c] - l[a]) - l[k]) > mg_err) {
					K[k]->keep = 1;
					vetoes++;
				}
			}
		}
	}
	for (int i = 0; i < pc_num; i++) {
		piece_t *p = &pc_list[i];
		if (p->dead)
			continue;
		poly_t out;
		out.n = 0;
		for (int k = 0; k < p->pl.n; k++) {
			if (DROPS(key[off[i] + k]))
				mg_dropped++;
			else
				out.v[out.n++] = p->pl.v[k];
		}
		if (out.n < 3) {                           /* (a sliver: no pixel) */
			p->dead = 1;
			mg_dead++;
		} else {
			p->pl = out;
		}
	}
#undef DROPS
	free(h); free(off); free(lit); free(key);
}

/* a merged piece's vertex light: from the lightmap of its face that holds
   the point */
static uint8_t merged_light_at(const bsp_t *b, const tex_set *ts, const piece_t *p, const pvert *v, int *src)
{
	static mg_light L;
	static const piece_t *last;
	if (p != last) {
		mg_light_set(&L, b, ts, &pc_mem[p->mfirst], p->mn);
		last = p;
	}
	return mg_light_at(&L, v, src);
}

/* the box of a face's live pieces (what it emits, merged pieces whole):
   their count */
static int pieces_bbox(int f, double mn[3], double mx[3])
{
	int n = 0;
	for (int k = 0; k < 3; k++) { mn[k] = 1e30; mx[k] = -1e30; }
	for (int i = 0; i < pc_count[f]; i++) {
		const piece_t *p = &pc_list[pc_first[f] + i];
		if (p->dead)
			continue;
		n++;
		for (int v = 0; v < p->pl.n; v++)
			for (int k = 0; k < 3; k++) {
				if (p->pl.v[v].p[k] < mn[k]) mn[k] = p->pl.v[v].p[k];
				if (p->pl.v[v].p[k] > mx[k]) mx[k] = p->pl.v[v].p[k];
			}
	}
	return n;
}

/* each face's vertex light, summed over its cooked vertices (the leaf light) */
static uint32_t *face_lsum;
static uint32_t *face_lcnt;

/* Add a face's (repaired) pieces to record r (flushed whenever it fills
   up): UVs from the cell's corner, light from the face's lightmap (a
   merged piece's: each vertex from its face's, QCOOK_MERGE). */
static void emit_face(out_t *o, const bsp_t *b, const tex_set *ts, int f, rec_t *r)
{
	if (!pc_count[f])
		return;
	const bsp_face    *bf = &b->faces[f];
	const bsp_texinfo *ti = &b->texinfo[bf->texinfo];
	const tex_slot    *s  = face_slot(ts, ti->miptex);
	const bsp_miptex  *m  = &b->miptex[ti->miptex];

	poly_t pl;                          /* the whole face, for its lightmap */
	face_poly(b, f, &pl);
	facelight fl;
	face_light_setup(b, f, &pl, &fl);
	if (pc_count[f] > 1)
		o->faces_split++;

	for (int i = 0; i < pc_count[f]; i++) {
		const piece_t *p = &pc_list[pc_first[f] + i];
		if (p->dead)
			continue;
		double  uv[MAXPV][2];            /* cooked texels from the cell's corner */
		uint8_t light[MAXPV];
		for (int k = 0; k < p->pl.n; k++) {
			uv[k][0] = (p->pl.v[k].s - p->s_off) * s->rep_w / m->w;
			uv[k][1] = (p->pl.v[k].t - p->t_off) * s->rep_h / m->h;
			if (s->flags & QWF_LIQUID) {
				/* a liquid (Quake's turbulent "*" textures, TEX_SPECIAL:
				   no lightmap) is drawn unlit, its texture as it is
				   (D_DrawTurbulent8): 128 at the cook's gain, as a wall
				   at that light. Not in the leaf light: R_LightPoint
				   skips it (SURF_DRAWTILED) */
				long l = lround(128 * light_gain);
				light[k] = (uint8_t) (l > 255 ? 255 : l);
				continue;
			}
			int src = f;
			light[k] = p->mn ? merged_light_at(b, ts, p, &p->pl.v[k], &src)
			                 : face_light_at(&fl, p->pl.v[k].s, p->pl.v[k].t);
			face_lsum[src] += light[k];
			face_lcnt[src]++;
		}
		rec_add_piece(o, r, &p->pl, (const double (*)[2]) uv, light, s, f);
		for (int k = 0; k < p->mn; k++) {      /* (the record draws its other faces too) */
			int g = pc_mem[p->mfirst + k], known = 0;
			for (int j = 0; j < r->nfaces && !known; j++)
				known = r->faces[j] == g;
			if (!known) {
				if (r->nfaces >= 256)
					die("a record of more than 256 faces");
				r->faces[r->nfaces++] = g;
			}
		}
	}
}

/* ------------------------------------------------------------- entities -- */

/* Quake's RLE of a PVS row (a zero byte, then the count of zero bytes):
   decoded into row[rowbytes] (short rows padded with zeros), encoded
   (rle_put: into a buffer; rle_len: its bytes), and the cooked rows
   (qw_vis_decode: formats/qworld.h qw_leaf vis, a plain row or the XOR
   against an earlier leaf's; docs/porting-notes.md §90.9). */
static void rle_decode(const uint8_t *in, const uint8_t *end, uint8_t *row, uint32_t rowbytes)
{
	uint32_t out = 0;
	while (out < rowbytes && (!end || in < end)) {
		if (*in) {
			row[out++] = *in++;
			continue;
		}
		uint32_t c = in[1];
		in += 2;
		while (c-- && out < rowbytes)
			row[out++] = 0;
	}
	while (out < rowbytes)
		row[out++] = 0;
}

static void rle_xor(const uint8_t *in, uint8_t *row, uint32_t rowbytes)
{
	uint32_t out = 0;
	while (out < rowbytes) {
		if (*in) {
			row[out++] ^= *in++;
			continue;
		}
		out += in[1];
		in += 2;
	}
}

static uint32_t rle_len(const uint8_t *row, uint32_t rowbytes)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < rowbytes; ) {
		if (row[i]) {
			n++;
			i++;
			continue;
		}
		uint32_t j = i;
		while (j < rowbytes && !row[j] && j - i < 255)
			j++;
		n += 2;
		i = j;
	}
	return n;
}

static void rle_put(buf_t *b, const uint8_t *row, uint32_t rowbytes)
{
	for (uint32_t i = 0; i < rowbytes; ) {
		if (row[i]) {
			buf_put(b, &row[i], 1);
			i++;
			continue;
		}
		uint32_t j = i;
		while (j < rowbytes && !row[j] && j - i < 255)
			j++;
		uint8_t run[2] = { 0, (uint8_t) (j - i) };
		buf_put(b, run, 2);
		i = j;
	}
}

void qw_vis_decode(const uint8_t *vis, const void *leaves_v, uint32_t leaf, uint8_t *row, uint32_t rowbytes)
{
	const qw_leaf *leaves = leaves_v;
	const uint8_t *in = vis + leaves[leaf].vis;
	uint32_t back = *in++;
	if (back) {
		rle_decode(vis + leaves[leaf - back].vis + 1, NULL, row, rowbytes);
		rle_xor(in, row, rowbytes);
	} else {
		rle_decode(in, NULL, row, rowbytes);
	}
}

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

/* The light a model in a leaf takes: Quake's R_LightPoint (r_light.c), the
   lightmap of the first lit face a line from a point straight down 2048
   units meets (RecursiveLightPoint: the front side first, the node's faces
   at the crossing, then the back side), its styles summed at their normal
   brightness as the faces' vertices take them (face_light_at), averaged
   over the points of a 3 x 3 x 3 grid inside the leaf's box that are in
   the leaf; -1: none met. The faces' extents are
   Quake's CalcSurfaceExtents (model.c). */
typedef struct { int tmin[2], ext[2]; } face_ext;
static face_ext *fext;

static void face_extents(const bsp_t *b)
{
	fext = xcalloc((size_t) b->num_faces, sizeof *fext);
	for (int f = 0; f < b->num_faces; f++) {
		const bsp_face *bf = &b->faces[f];
		const bsp_texinfo *tx = &b->texinfo[bf->texinfo];
		double mn[2] = { 1e30, 1e30 }, mx[2] = { -1e30, -1e30 };
		for (int i = 0; i < bf->numedges; i++) {
			int e = b->surfedges[bf->firstedge + i];
			const float *v = b->verts + 3 * (e >= 0 ? b->edges[2 * e] : b->edges[2 * -e + 1]);
			for (int k = 0; k < 2; k++) {
				double val = v[0] * tx->vecs[k][0] + v[1] * tx->vecs[k][1] + v[2] * tx->vecs[k][2] + tx->vecs[k][3];
				if (val < mn[k]) mn[k] = val;
				if (val > mx[k]) mx[k] = val;
			}
		}
		for (int k = 0; k < 2; k++) {
			int b0 = (int) floor(mn[k] / 16), b1 = (int) ceil(mx[k] / 16);
			fext[f].tmin[k] = b0 * 16;
			fext[f].ext[k]  = (b1 - b0) * 16;
		}
	}
}

static int light_point_rec(const bsp_t *b, int node, const double s[3], const double e[3])
{
	if (node < 0)
		return -1;                                /* a leaf: nothing met */
	const bsp_node  *n  = &b->nodes[node];
	const bsp_plane *pl = &b->planes[n->planenum];
	double front = pl->n[0] * s[0] + pl->n[1] * s[1] + pl->n[2] * s[2] - pl->dist;
	double back  = pl->n[0] * e[0] + pl->n[1] * e[1] + pl->n[2] * e[2] - pl->dist;
	int side = front < 0;
	if ((back < 0) == side)
		return light_point_rec(b, n->child[side], s, e);
	double frac = front / (front - back), mid[3];
	for (int k = 0; k < 3; k++)
		mid[k] = s[k] + (e[k] - s[k]) * frac;
	int r = light_point_rec(b, n->child[side], s, mid);
	if (r >= 0)
		return r;
	for (int i = 0; i < n->numfaces; i++) {
		int f = n->firstface + i;
		const bsp_face    *bf = &b->faces[f];
		const bsp_texinfo *tx = &b->texinfo[bf->texinfo];
		if (tx->flags & 1)                         /* (TEX_SPECIAL: Quake's SURF_DRAWTILED) */
			continue;
		double ss = mid[0] * tx->vecs[0][0] + mid[1] * tx->vecs[0][1] + mid[2] * tx->vecs[0][2] + tx->vecs[0][3];
		double tt = mid[0] * tx->vecs[1][0] + mid[1] * tx->vecs[1][1] + mid[2] * tx->vecs[1][2] + tx->vecs[1][3];
		if (ss < fext[f].tmin[0] || tt < fext[f].tmin[1])
			continue;
		int ds = (int) (ss - fext[f].tmin[0]), dt = (int) (tt - fext[f].tmin[1]);
		if (ds > fext[f].ext[0] || dt > fext[f].ext[1])
			continue;
		if (bf->lightofs < 0 || !b->lighting)
			return 0;
		int w = (fext[f].ext[0] >> 4) + 1, h = (fext[f].ext[1] >> 4) + 1, sum = 0;
		size_t at = (size_t) bf->lightofs + (size_t) (dt >> 4) * w + (ds >> 4);
		for (int k = 0; k < 4 && bf->styles[k] != 255; k++, at += (size_t) w * h)
			if (at < b->light_len)
				sum += b->lighting[at];
		return sum;
	}
	return light_point_rec(b, n->child[!side], mid, e);
}

static int point_leaf(const bsp_t *b, const double p[3])
{
	int node = 0;
	while (node >= 0) {
		const bsp_node  *n  = &b->nodes[node];
		const bsp_plane *pl = &b->planes[n->planenum];
		node = n->child[pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - pl->dist < 0];
	}
	return -1 - node;
}

static int leaf_light_point(const bsp_t *b, int leaf)
{
	const bsp_leaf *bl = &b->leaves[leaf];
	int sum = 0, n = 0;
	for (int i = 1; i <= 3; i++)                  /* (a 3 x 3 x 3 grid inside its box) */
		for (int j = 1; j <= 3; j++)
			for (int k = 1; k <= 3; k++) {
				double p[3] = { bl->mins[0] + (bl->maxs[0] - bl->mins[0]) * i / 4.0,
				                bl->mins[1] + (bl->maxs[1] - bl->mins[1]) * j / 4.0,
				                bl->mins[2] + (bl->maxs[2] - bl->mins[2]) * k / 4.0 };
				double e[3] = { p[0], p[1], p[2] - 2048 };
				if (point_leaf(b, p) != leaf)
					continue;
				int r = light_point_rec(b, 0, p, e);
				if (r >= 0) {
					sum += r;
					n++;
				}
			}
	return n ? sum / n : -1;
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

/* The clip hulls with their identical subtrees shared: two clipnodes on
   the same plane whose children are the same contents or shared nodes are
   one. A trace or a contents walk takes the same planes and reaches the
   same contents through either, so every result is the same, bit for bit;
   qbsp's hulls repeat 1.5-14.7 KB of subtrees a map (docs/porting-notes.md
   §58). The roots (`roots`: the world's hull 1, the brush models', then the
   hulls 2; a contents root stays as it is) are numbered depth first, so
   hull 1's nodes come first, as in the BSP. `out` takes the table (room
   for b->num_clipnodes), `roots` the new indices; returns the count. */
typedef struct { int plane, child[2]; } share_key;

static int share_canon(const bsp_t *b, int n, int *canon, share_key *keys, int *nkeys, int *hash, int hmask)
{
	if (canon[n] >= 0)
		return canon[n];
	share_key k;
	k.plane = b->clipnodes[n].planenum;
	for (int c = 0; c < 2; c++) {
		int ch = b->clipnodes[n].child[c];
		k.child[c] = ch < 0 ? ch : share_canon(b, ch, canon, keys, nkeys, hash, hmask);
	}
	uint32_t hv = (uint32_t) k.plane * 2654435761u ^ (uint32_t) k.child[0] * 40503u ^ (uint32_t) k.child[1] * 2246822519u;
	for (uint32_t i = hv & (uint32_t) hmask;; i = (i + 1) & (uint32_t) hmask) {
		int id = hash[i];
		if (id < 0) {
			id = (*nkeys)++;
			keys[id] = k;
			hash[i] = id;
			return canon[n] = id;
		}
		if (keys[id].plane == k.plane && keys[id].child[0] == k.child[0] && keys[id].child[1] == k.child[1])
			return canon[n] = id;
	}
}

static int clip_share(const bsp_t *b, int *roots, int nroots, qw_clipnode *out)
{
	int n = b->num_clipnodes, hmask = 1, nkeys = 0, count = 0;
	while (hmask < 2 * n + 2)
		hmask <<= 1;
	int *canon = xmalloc((size_t) n * sizeof *canon), *hash = xmalloc((size_t) hmask * sizeof *hash);
	int *num = xmalloc((size_t) n * sizeof *num), *stack = xmalloc((size_t) (n + nroots + 2) * sizeof *stack);
	share_key *keys = xmalloc((size_t) n * sizeof *keys);
	for (int i = 0; i < n; i++)
		canon[i] = num[i] = -1;
	for (int i = 0; i < hmask; i++)
		hash[i] = -1;
	hmask--;
	for (int r = 0; r < nroots; r++)
		if (roots[r] >= 0)
			roots[r] = share_canon(b, roots[r], canon, keys, &nkeys, hash, hmask);
	for (int r = 0; r < nroots; r++) {              /* depth first, child 0 first */
		if (roots[r] < 0)
			continue;
		int sp = 0;
		stack[sp++] = roots[r];
		while (sp) {
			int id = stack[--sp];
			if (num[id] >= 0)
				continue;
			num[id] = count++;
			for (int c = 1; c >= 0; c--)
				if (keys[id].child[c] >= 0)
					stack[sp++] = keys[id].child[c];
		}
	}
	for (int id = 0; id < nkeys; id++) {
		if (num[id] < 0)
			continue;                                  /* (no root reaches it) */
		qw_clipnode *q = &out[num[id]];
		q->plane3 = (uint16_t) (keys[id].plane * 3);
		for (int c = 0; c < 2; c++)
			q->child[c] = (int16_t) (keys[id].child[c] < 0 ? keys[id].child[c] : num[keys[id].child[c]]);
	}
	for (int r = 0; r < nroots; r++)
		if (roots[r] >= 0)
			roots[r] = num[roots[r]];
	free(canon); free(hash); free(num); free(stack); free(keys);
	return count;
}

void world_build(const bsp_t *b, const tex_set *ts, const char *out_path)
{
	const bsp_model *wm = &b->models[0];
	bm_grid_faces = 0;
	if (b->num_models > 254)
		die("%d models: more than 254", b->num_models);

	ent_list ents;
	ents_parse(b, &ents);

	/* every drawn face cut into pieces (the world's and the brush models') */
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
	/* the coplanar faces merged (QCOOK_MERGE, 1 by default): their clusters'
	   cells before the cut, their pieces after */
	const char *mg = getenv("QCOOK_MERGE"), *me = getenv("QCOOK_MERGE_ERR");
	merge_on = !(mg && *mg && !atoi(mg));
	mg_err = me && *me ? atof(me) : MG_ERR;
	if (merge_on)
		merge_clusters(b, ts, face_mfirst, face_mleaf);
	for (int f = 0; f < b->num_faces; f++) {
		pc_count[f] = 0;
		if (face_model[f] >= 0)
			face_pieces(b, ts, f);
	}
	if (merge_on) {
		merge_pieces(b, ts);
		merge_check(b, ts);
		merge_drop(b, ts);
	}
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
	int *memb  = xmalloc((size_t) b->num_faces * sizeof *memb);    /* a group's faces */
	int *order = xmalloc((size_t) b->num_faces * sizeof *order);   /* ... breadth first */

	out_t o;
	memset(&o, 0, sizeof o);
	if ((long) b->num_nodes * QW_NODE_WORDS >= QW_NOPARENT)
		die("%d nodes: parent offsets do not fit 16 bits", b->num_nodes);
	qw_node *nodes = xcalloc((size_t) b->num_nodes + 1, sizeof *nodes);   /* + sentinel */
	for (int i = 0; i < b->num_nodes; i++) {
		const bsp_node *bn = &b->nodes[i];
		const bsp_plane *pl = &b->planes[bn->planenum];
		qw_node *n = &nodes[i];

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
						if (!merge_on)
							face_bbox(b, order[q], fmn, fmx);
						else if (!pieces_bbox(order[q], fmn, fmx))
							continue;            /* (its pieces merged into others') */
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
	}
	o.bmodel = 0;

	/* marksurfaces: faces -> the records drawing them, deduplicated per
	   leaf; records are the visibility ids */
	int *pair_first = xmalloc((size_t) b->num_faces * sizeof *pair_first);
	if (world_fmap) {                          /* (the zones' tables' records -> these, zones.c) */
		FILE *fp = fopen(world_fmap, "w");
		if (!fp)
			die("cannot write %s", world_fmap);
		fprintf(fp, "fmap %u %d\n", o.wrecords, o.npairs);
		for (int p = 0; p < o.npairs; p++)
			fprintf(fp, "%d %d\n", o.pair_rec[p], o.pair_face[p]);
		fclose(fp);
	}
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

	/* leaves. Their light (models in the leaf are lit with it): Quake's
	   R_LightPoint in the leaf at the cook's gain (leaf_light_point: the
	   floor's light under it, as Quake lights a model); where no line meets
	   a lit face, the average vertex light of the leaf's faces, or for a
	   leaf without lit faces (open air) its nearest ancestor's subtree
	   average. */
	face_extents(b);
	long lp_sum = 0, lp_old = 0;
	int lp_n = 0, lp_empty = 0, lp_empty_n = 0;
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
		int lp = i ? leaf_light_point(b, i) : -1;
		if (bl->contents == QW_CONTENTS_EMPTY) {
			lp_empty++;
			lp_empty_n += lp >= 0;
		}
		if (lp >= 0) {
			long g = lround(lp * light_gain);
			lp_old += l->light;
			l->light = (uint8_t) (g > 255 ? 255 : g);
			lp_sum += l->light;
			lp_n++;
		}
	}
	printf("qcook: leaf light: %d of %d leaves (%d of the %d empty) by Quake's R_LightPoint, their mean %ld "
	       "(the faces' average %ld)\n", lp_n, b->num_leaves, lp_empty_n, lp_empty, lp_n ? lp_sum / lp_n : 0,
	       lp_n ? lp_old / lp_n : 0);
	free(fext);
	fext = NULL;
	free(lsum); free(lcnt); free(nsum); free(ncnt);

	/* The brush models' leaves (Quake numbers them after the world's, whose
	   PVS rows index leaves 1..visleafs): no marks, no PVS row, nothing the
	   engine reads but their contents (the renderer stops at a brush
	   model's leaf, the point hull's traces read its contents byte). They
	   become one leaf per contents, after the world's (docs/porting-notes.md
	   §58): 210-953 leaves a map, 28 bytes each and their ambient bytes.
	   A map whose leaves past the world's held anything else would keep
	   them all (none does). */
	int num_leaves = b->num_leaves, own_leaves = b->num_leaves;   /* (own: the leaves kept as they are) */
	int *leaf_new = NULL;                     /* [b->num_leaves]: a leaf's index in the file */
	{
		int merge = wm->visleafs + 1 < b->num_leaves;
		for (int i = wm->visleafs + 1; i < b->num_leaves && merge; i++)
			if (gcount[i] || leaves[i].vis != QW_NONE ||
			    (leaf_parent[i] != QW_NONE && reach[leaf_parent[i]] == 1))   /* (a world leaf) */
				merge = 0;
		if (merge) {
			int8_t shared[8];
			int nshared = 0;
			leaf_new   = xmalloc((size_t) b->num_leaves * sizeof *leaf_new);
			own_leaves = wm->visleafs + 1;
			for (int i = 0; i < b->num_leaves; i++) {
				leaf_new[i] = i;
				if (i < own_leaves)
					continue;
				int k = 0;
				while (k < nshared && shared[k] != leaves[i].contents)
					k++;
				if (k == nshared) {
					if (nshared == (int) sizeof shared)
						die("brush models' leaves: more than %d contents", (int) sizeof shared);
					shared[nshared++] = leaves[i].contents;
				}
				leaf_new[i] = own_leaves + k;
			}
			for (int k = 0; k < nshared; k++) {
				qw_leaf *l = &leaves[own_leaves + k];
				memset(l, 0, sizeof *l);
				l->vis      = QW_NONE;
				l->parent   = QW_NOPARENT;
				l->contents = shared[k];
				l->light    = 128;
			}
			for (int i = 0; i < b->num_nodes; i++)
				for (int c = 0; c < 2; c++)
					if (nodes[i].child[c] & 0x80000000u)
						nodes[i].child[c] = 0x80000000u | (uint32_t) leaf_new[nodes[i].child[c] & 0x7fffffffu];
			num_leaves = own_leaves + nshared;
			printf("qcook:   the brush models' %d leaves -> %d shared\n", b->num_leaves - own_leaves, nshared);
		}
	}

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
			rle_decode(b->vis + bl->visofs, b->vis + b->vis_len, row, (uint32_t) rowbytes);
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

	/* The brush models no entity uses (in the id1 maps, the models of the
	   entities single player never spawns: ents_parse left them out) keep
	   their record, not their trees: no face of theirs is drawn
	   (ents_drawn_models), no edict walks or clips them. Their render
	   nodes leave the file, the others numbered down in order (a parent
	   still before its children, the sentinel last); each one's root
	   becomes a leaf of its tree, its clip roots the world's (clip_share
	   then leaves their clipnodes out). */
	uint8_t *used = xcalloc((size_t) b->num_models, 1);
	ents_used_models(&ents, b->num_models, used);
	int *node_new = xmalloc(((size_t) b->num_nodes + 1) * sizeof *node_new);
	int kept_nodes = 0;
	for (int i = 0; i < b->num_nodes; i++)
		node_new[i] = !reach[i] || used[reach[i] - 1] ? kept_nodes++ : -1;
	node_new[b->num_nodes] = kept_nodes;
	if (kept_nodes < b->num_nodes) {
		for (int i = 0; i <= b->num_nodes; i++) {   /* (in place: node_new[i] <= i) */
			if (node_new[i] < 0) {
				if (nodes[i].faces[0] != nodes[i].faces[1] || nodes[i].faces[1] != nodes[i + 1].faces[0])
					die("node %d of a model no entity uses has faces", i);   /* (its blocks: empty) */
				continue;
			}
			qw_node *n = &nodes[node_new[i]];
			*n = nodes[i];
			if (i == b->num_nodes)
				break;                               /* (the sentinel: its faces[0]) */
			for (int c = 0; c < 2; c++)
				if (!(n->child[c] & 0x80000000u))
					n->child[c] = (uint32_t) node_new[n->child[c]];   /* (its tree's: kept) */
			if (n->parent != QW_NOPARENT)
				n->parent = (uint16_t) (node_new[n->parent / QW_NODE_WORDS] * QW_NODE_WORDS);
		}
		for (int i = 0; i < num_leaves; i++)
			if (leaves[i].parent != QW_NOPARENT) {
				int p = node_new[leaves[i].parent / QW_NODE_WORDS];
				leaves[i].parent = p < 0 ? QW_NOPARENT : (uint16_t) (p * QW_NODE_WORDS);
			}
		int unused = 0;
		for (int m = 1; m < b->num_models; m++)
			unused += !used[m];
		printf("qcook:   %d brush models no entity uses: %d render nodes -> %d\n", unused, b->num_nodes,
		       kept_nodes);
	}

	/* assemble */
	qw_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QWORLD_MAGIC, 4);
	h.num_nodes   = (uint32_t) kept_nodes;
	h.num_leaves  = (uint32_t) num_leaves;
	h.num_marks   = (uint32_t) ngmarks;
	h.num_faces   = o.wrecords;                /* visibility ids: the world's records */
	h.num_records = o.records;
	h.num_pieces  = o.quads + o.tris;
	h.num_views   = (uint32_t) nviews;
	h.has_spawn   = (uint32_t) has_spawn;
	for (int k = 0; k < 3; k++)
		h.spawn[k] = (int32_t) lround(so[k]);
	h.spawn_yaw = (int32_t) lround(sa);
	h.root      = (uint32_t) node_new[root];
	h.max_depth = (uint32_t) depth_of(b, root);
	h.num_visleafs = (uint32_t) wm->visleafs;
	if (h.max_depth > QW_MAX_DEPTH)
		die("BSP depth %u exceeds the renderer stack (%d)", h.max_depth, QW_MAX_DEPTH);

	/* the brush models: bounds spread by a unit (Mod_LoadSubmodels), the
	   hull-0 root (its subtree is walked by the renderer), the clip roots;
	   their pieces' rooms once the file is whole (bmf_rooms) */
	qw_bmodel *bmodels = xcalloc((size_t) b->num_models, sizeof *bmodels);
	h.max_bm_depth = 0;
	for (int m = 1; m < b->num_models; m++) {
		const bsp_model *bm = &b->models[m];
		qw_bmodel *q = &bmodels[m];
		for (int k = 0; k < 3; k++) {
			q->mins[k] = (int32_t) lround((bm->mins[k] - 1) * QW_CLIP_SUB);
			q->maxs[k] = (int32_t) lround((bm->maxs[k] + 1) * QW_CLIP_SUB);
		}
		int r = bm->headnode[0];
		while (!used[m] && r >= 0)                  /* (a model no entity uses: a leaf of its tree) */
			r = b->nodes[r].child[0];
		q->root  = r >= 0 ? (uint32_t) node_new[r]
		         : 0x80000000u | (uint32_t) (leaf_new ? leaf_new[-1 - r] : -1 - r);
		if (bm->headnode[1] < 0 || bm->headnode[1] > 32767 || bm->headnode[2] < 0 || bm->headnode[2] > 32767)
			die("brush model %d: clip roots %d, %d", m, bm->headnode[1], bm->headnode[2]);
		q->hull1 = (int16_t) bm->headnode[1];
		q->hull2 = (int16_t) bm->headnode[2];
		int d = depth_of(b, r);
		if (d > QW_MAX_BM_DEPTH)
			die("brush model %d: depth %d > %d", m, d, QW_MAX_BM_DEPTH);
		q->depth = (uint16_t) d;
		if ((uint32_t) d > h.max_bm_depth)
			h.max_bm_depth = (uint32_t) d;
	}

	/* the entities */
	buf_t etab, estr;
	memset(&etab, 0, sizeof etab);
	memset(&estr, 0, sizeof estr);
	int unknown = 0;
	ents_cook(&ents, b->num_models, &etab, &estr, &h.worldtype, &h.title, &unknown);

	/* The PVS rows re-encoded (formats/qworld.h qw_leaf vis, docs/porting-
	   notes.md §90.9): a byte `back`, then Quake's RLE of the row (back
	   0) or of its XOR against the plain row of the leaf `back` before it,
	   whichever RLEs shorter; the anchors the last VIS_ANCHORS plain rows,
	   within 255 leaves. A marking decodes two rows at most. */
	buf_t vis;
	memset(&vis, 0, sizeof vis);
	uint32_t vis_rows = 0, vis_deltas = 0;
	{
		enum { VIS_ANCHORS = 4 };
		uint32_t rowbytes = ((uint32_t) wm->visleafs + 7) >> 3;
		if (rowbytes > 256)
			die("PVS rows of %u bytes: the engine expands a row into 256 (r_vis_row)", rowbytes);
		uint8_t *cur = xmalloc(rowbytes), *best = xmalloc(rowbytes), *x = xmalloc(rowbytes);
		uint8_t *anchor = xmalloc((size_t) VIS_ANCHORS * rowbytes);
		int anchor_leaf[VIS_ANCHORS], nanch = 0;
		for (int i = 1; i <= wm->visleafs && i < num_leaves; i++) {
			qw_leaf *l = &leaves[i];
			if (l->vis == QW_NONE)
				continue;
			rle_decode(b->vis + l->vis, b->vis + b->vis_len, cur, rowbytes);
			vis_rows++;
			uint32_t bestlen = rle_len(cur, rowbytes);
			int bestk = -1;
			for (int k = 0; k < nanch; k++) {
				if (i - anchor_leaf[k] > 255)
					continue;
				for (uint32_t j = 0; j < rowbytes; j++)
					x[j] = cur[j] ^ anchor[k * rowbytes + j];
				uint32_t n = rle_len(x, rowbytes);
				if (n < bestlen) {
					bestlen = n;
					bestk = k;
					memcpy(best, x, rowbytes);
				}
			}
			l->vis = (uint32_t) vis.len;
			uint8_t back = (uint8_t) (bestk < 0 ? 0 : i - anchor_leaf[bestk]);
			buf_put(&vis, &back, 1);
			rle_put(&vis, bestk < 0 ? cur : best, rowbytes);
			if (bestk < 0) {                              /* a new anchor, the newest first */
				if (nanch < VIS_ANCHORS)
					nanch++;
				for (int k = nanch - 1; k > 0; k--) {
					memcpy(anchor + k * rowbytes, anchor + (k - 1) * rowbytes, rowbytes);
					anchor_leaf[k] = anchor_leaf[k - 1];
				}
				memcpy(anchor, cur, rowbytes);
				anchor_leaf[0] = i;
			} else {
				vis_deltas++;
			}
		}
		/* decoded back: the rows as they came */
		for (int i = 1; i <= wm->visleafs && i < num_leaves; i++) {
			if (leaves[i].vis == QW_NONE)
				continue;
			rle_decode(b->vis + b->leaves[i].visofs, b->vis + b->vis_len, cur, rowbytes);
			qw_vis_decode(vis.p, leaves, (uint32_t) i, x, rowbytes);
			if (memcmp(cur, x, rowbytes))
				die("leaf %d: its PVS row decodes wrong", i);
		}
		free(cur); free(best); free(x); free(anchor);
	}

	buf_t f;
	memset(&f, 0, sizeof f);
	buf_put(&f, &h, sizeof h);
	h.ofs_nodes = (uint32_t) f.len;  buf_put(&f, nodes, (size_t) (kept_nodes + 1) * sizeof *nodes);
	h.ofs_leaves = (uint32_t) f.len; buf_put(&f, leaves, (size_t) num_leaves * sizeof *leaves);
	h.ofs_marks = (uint32_t) f.len;  buf_put(&f, gmarks, (size_t) ngmarks * 2); buf_align4(&f);
	h.ofs_vis = (uint32_t) f.len;    buf_put(&f, vis.p, vis.len); buf_align4(&f);
	h.len_vis = (uint32_t) vis.len;
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

	h.ofs_ambient = (uint32_t) f.len;           /* the leaves' water / sky levels */
	for (int i = 0; i < num_leaves; i++) {
		static const uint8_t none[2];             /* (the brush models' shared leaves) */
		buf_put(&f, i < own_leaves ? b->leaves[i].ambient : none, 2);
	}
	buf_align4(&f);
	h.num_tex = (uint32_t) ts->num;              /* (the texture set's table) */

	/* clip hulls 1 and 2 (all BSP planes; the clipnodes with their
	   identical subtrees shared: clip_share) */
	{
		for (int i = 0; i < b->num_clipnodes; i++) {
			const bsp_clipnode *cn = &b->clipnodes[i];
			if (cn->planenum < 0 || cn->planenum >= b->num_planes || cn->planenum * 3 > 0xffff)
				die("clipnode %d: bad plane (or more than 21845 planes)", i);
			for (int c = 0; c < 2; c++)
				if (cn->child[c] >= b->num_clipnodes)
					die("clipnode %d: child out of range", i);
		}
		int nm = b->num_models;
		int *roots = xmalloc((size_t) 2 * nm * sizeof *roots);
		for (int m = 0; m < nm; m++) {               /* (a model no entity uses: the world's) */
			roots[m]      = b->models[used[m] ? m : 0].headnode[1];
			roots[nm + m] = b->models[used[m] ? m : 0].headnode[2];
		}
		qw_clipnode *cnodes = xcalloc((size_t) b->num_clipnodes + 1, sizeof *cnodes);
		int ncn = clip_share(b, roots, 2 * nm, cnodes);
		for (int m = 1; m < nm; m++) {
			bmodels[m].hull1 = (int16_t) roots[m];
			bmodels[m].hull2 = (int16_t) roots[nm + m];
		}
		printf("qcook:   clip hulls: %d clipnodes -> %d, the identical subtrees shared\n", b->num_clipnodes, ncn);

		h.ofs_clip = (uint32_t) f.len;
		qw_clip_header ch;
		ch.num_planes    = (uint32_t) b->num_planes;
		ch.num_clipnodes = (uint32_t) ncn;
		ch.hull1_root    = roots[0];
		ch.hull2_root    = roots[nm];
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
		buf_put(&f, cnodes, (size_t) ncn * sizeof *cnodes);
		free(cnodes);
		free(roots);
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
	ents_pack((const qw_ent *) etab.p, h.num_ents, &f);   /* (compact: formats/qworld.h) */
	buf_align4(&f);
	h.ofs_strings = (uint32_t) f.len;
	h.len_strings = (uint32_t) estr.len;
	buf_put(&f, estr.p, estr.len);
	memcpy(f.p, &h, sizeof h);
	bmf_rooms(f.p, f.len);                 /* (on the file as the engine reads it) */

	FILE *fp = fopen(out_path, "wb");
	if (!fp)
		die("cannot write %s", out_path);
	fwrite(f.p, 1, f.len, fp);
	fclose(fp);

	printf("qcook: %s — %u B: nodes %zu, leaves %zu, marks %u, vis %u, faces %u\n",
	       out_path, (uint32_t) f.len, kept_nodes * sizeof *nodes,
	       num_leaves * sizeof *leaves, ngmarks * 2, h.len_vis, h.len_faces);
	printf("qcook:   PVS rows: %u B -> %u B, %u of %u rows the XOR against an earlier plain row\n",
	       b->vis_len, h.len_vis, vis_deltas, vis_rows);
	printf("qcook:   %u records, %u pieces -> %u quads + %u tris, %u vertex refs "
	       "(%.1f/record) into a pool of %d; %u faces split; depth %u; %d bench views\n",
	       o.records, o.pieces, o.quads, o.tris, o.verts,
	       o.records ? (double) o.verts / o.records : 0.0, o.npool, o.faces_split,
	       h.max_depth, nviews);
	printf("qcook:   %u pieces ear-clipped\n", o.pieces_eared);
	if (merge_on) {
		mg_pieces = mg_into = 0;
		for (int i = 0; i < pc_num; i++)
			if (!pc_list[i].dead && pc_list[i].mn) {
				mg_into++;
				mg_pieces += (uint32_t) pc_list[i].mn;
			}
		printf("qcook:   merge: %u pieces merged into %u; %u vertices dropped from the straight runs, "
		       "%u slivers left out\n", mg_pieces, mg_into, mg_dropped, mg_dead);
	}
	if (bm_grid_faces)
		printf("qcook:   brush models' light grid: %d faces cut every %d texels\n", bm_grid_faces, QL_BM_LIGHT_GRID);
	if (has_spawn)
		printf("qcook:   spawn %d %d %d yaw %d\n", h.spawn[0], h.spawn[1], h.spawn[2], h.spawn_yaw);
	{
		uint32_t brecs = o.records - o.wrecords;
		printf("qcook:   %d brush models, %u records, depth <= %u; %u entities (%d unknown, %d single "
		       "player never spawns left out), %u B of strings; worldtype %u\n", b->num_models - 1, brecs,
		       h.max_bm_depth, h.num_ents, unknown, ents.sp_never, h.len_strings, h.worldtype);
	}

	free(f.p); free(o.stream.p); free(nodes); free(leaves); free(bmodels);
	free(etab.p); free(estr.p); ents_free(&ents);
	free(node_parent); free(leaf_parent); free(reach); free(leaf_new); free(used); free(node_new);
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
	SECTION(h.ofs_verts, h.num_verts * sizeof(qw_vert));
	SECTION(h.ofs_ambient, h.num_leaves * 2);
	SECTION(h.ofs_bmodels, h.num_bmodels * sizeof(qw_bmodel));
	if (h.ofs_strings < h.ofs_ents + QE_COUNT * 2)
		FAIL("the entities' section (%u..%u) holds no class names", h.ofs_ents, h.ofs_strings);
	SECTION(h.ofs_ents, h.ofs_strings - h.ofs_ents);
	SECTION(h.ofs_strings, h.len_strings);
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
		if (!bm[i].room_verts != !bm[i].room_recs || !bm[i].room_verts != !bm[i].room_groups ||
		    bm[i].room_recs * 4u < bm[i].room_groups * (uint32_t) sizeof(qw_record))
			FAIL("bmodel %u: a room of %u B, %u vertices, %u groups", i, bm[i].room_recs * 4u,
			     bm[i].room_verts, bm[i].room_groups);
	}
	const uint16_t *names = (const uint16_t *) (d + h.ofs_ents);
	const char *strs = (const char *) (d + h.ofs_strings);
	if (h.len_strings < 1 || strs[0] || strs[h.len_strings - 1])
		FAIL("string pool not NUL-ended");
	for (int c = 0; c < QE_COUNT; c++)
		if (names[c] >= h.len_strings || (!c && names[c]))
			FAIL("class %d: name at %u", c, names[c]);
	/* the compact entities, decoded from a copy padded past the section's
	   end (a wrong head reads at most 4 + 40 + 2 bytes) */
	uint32_t elen = h.ofs_strings - h.ofs_ents - QE_COUNT * 2;
	uint8_t *ecopy = xcalloc(elen + 64, 1);
	memcpy(ecopy, d + h.ofs_ents + QE_COUNT * 2, elen);
	const uint8_t *ep = ecopy;
	for (uint32_t i = 0; i < h.num_ents; i++) {
		if (ep[3] & 0x80)
			FAIL("entity %u: head bit 31 set", i);
		qw_ent ent;
		ep = qw_ent_decode(ep, names, &ent);
		if ((uint32_t) (ep - ecopy) > elen)
			FAIL("entity %u: past the section's end", i);
		const qw_ent *e = &ent;
		if (e->class >= QE_COUNT) FAIL("entity %u: class %u", i, e->class);
		if (e->model < 0 || (uint32_t) e->model >= h.num_bmodels) FAIL("entity %u: model %d", i, e->model);
		uint16_t so[6] = { e->target, e->targetname, e->killtarget, e->message, e->map, e->classname };
		for (int k = 0; k < 6; k++)
			if (so[k] >= h.len_strings) FAIL("entity %u: string offset %u", i, so[k]);
		if (!e->classname) FAIL("entity %u: no classname", i);
	}
	if (elen - (uint32_t) (ep - ecopy) > 3)
		FAIL("%u bytes after the entities", elen - (uint32_t) (ep - ecopy));
	for (; ep < ecopy + elen; ep++)
		if (*ep) FAIL("the entities' padding is not zero");
	free(ecopy);
	if (h.title >= h.len_strings) FAIL("title string out of range");

	printf("check: %s OK — %u nodes, %u leaves, %u records (%u the world's), %u polys, depth %u; "
	       "%u brush models, %u entities\n",
	       path, h.num_nodes, h.num_leaves, recs, h.num_faces, polys, h.max_depth,
	       h.num_bmodels ? h.num_bmodels - 1 : 0, h.num_ents);
	free(d);
	return 0;
}
