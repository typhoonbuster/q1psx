/*
 * q1psx — renderer subdivision paths. C reference version (R_SLOW_C = 1 in
 * engine/r_defs.inc): the specification the asm port follows.
 *
 * engine/r_world.s calls r_slow() for a polygon of a face record that the
 * fast path cannot simply emit: the record's bounding sphere says one of its
 * vertices may be too near (the GTE's projection breaks down), or one of its
 * polygons may be too big for the GPU, or its affine texture error may exceed
 * the threshold (docs/porting-notes.md §19, §19.4). Per polygon:
 *
 *   - a vertex nearer than SZ_CLIP, or projected beyond the GTE's range:
 *     the clip path. Transform to view space, clip against the near plane
 *     and the frustum (with guard bands), project the new vertices;
 *   - otherwise: straight from the record's vertex cache (the fast path's
 *     RTPT results: SXY, SZ, light).
 *
 * Then both subdivide in screen space. An edge splits when its affine error
 * exceeds the threshold, or when it is too long for the GPU:
 *     (|dx| + |dy|) * |za - zb| > T2 * (za + zb),   T2 = 2 x threshold (px)
 * (the midpoint's screen displacement between affine and perspective is
 * |ds| * |za - zb| / (2 (za + zb)), and |dx| + |dy| bounds |ds|). The
 * decision and the midpoint depend on the edge's two ends only, and every
 * vertex an edge can share with another polygon comes from the same cache
 * value, so neighbours split shared edges identically: no T-junction, no
 * crack. The midpoint is the projected 3D midpoint, from screen positions
 * and depths alone:
 *     s = (sa za + sb zb) / (za + zb),   z = (za + zb) / 2,
 *     u, v, light = averages (affine in 3D).
 */

#include <stdint.h>

#include "prof.h"
#include "qworld.h"
#include "render.h"

#define VU       6            /* view units per world unit                  */
#define ZNEAR    (8 * VU)     /* near clip plane                            */
#define SZ_CLIP  96           /* SZ below this: the GTE's projection is off */
#define SPAN     500          /* split edges with |dx| + |dy| above this: a
                                 triangle's bbox then fits the GPU's 1023 x
                                 511 limit                                  */
#define KOFS     8192         /* rdiv's offset: |sx|, |sy| < 8192 (corners
                                 within +-1023, clip-path projections of a
                                 polygon at most 1536 view units wide at
                                 z >= ZNEAR within +-5300)                  */
#define MAXD     8            /* max split depth (the asm also stops
                                 splitting when its 512-entry vertex pool is
                                 nearly full; no bench view comes close)    */

/* the record's vertex cache in the scratchpad (engine/r_defs.inc) */
#define SPAD     0x1F800000u
#define SP_SXY   0x100
#define SP_SZ    0x1a8

typedef struct {
	int32_t x, y, z;          /* view space (clip path)                      */
	int32_t u, v;             /* texels x256 (page-relative)                 */
	int32_t c;                /* light 0..255                                */
	int32_t sx, sy;           /* projected                                   */
	int32_t cached;           /* sx, sy, z are the cache's (do not project)  */
} sv_t;

typedef struct { sv_t v[3]; int depth; } tri_t;

static tri_t    stack[3 * MAXD + 2];         /* depth-first, <= 3 left per level */
static uint32_t clut16, tpage16;

/* ------------------------------------------------------------- projection */

static void project(sv_t *p)
{
	/* past the near plane only: x, y may be far off screen, so divide each
	   (|x| H < 2^31); subdivision culls or splits what lies outside */
	p->sx = 160 + p->x * R_SCREEN_H / p->z;
	p->sy = r_state.ofy + p->y * R_SCREEN_H / p->z;
}

/* Clip-path vertex order: crossings are always interpolated from the same
   end, so two polygons sharing a crossed edge make the same point. */
static int before(const sv_t *a, const sv_t *b)
{
	if (a->z != b->z) return a->z < b->z;
	if (a->x != b->x) return a->x < b->x;
	return a->y < b->y;
}

static void lerp(const sv_t *a, const sv_t *b, int32_t num, int32_t den, sv_t *o)
{
	/* one division per edge. Within one polygon (a texture repetition, at
	   most 256 world units) plane distances stay < 2^16 and deltas < 2^16,
	   so a 14-bit factor keeps every product below 2^31. */
	int32_t t = (num << 14) / den;
	o->x = a->x + (((b->x - a->x) * t) >> 14);
	o->y = a->y + (((b->y - a->y) * t) >> 14);
	o->z = a->z + (((b->z - a->z) * t) >> 14);
	o->u = a->u + (((b->u - a->u) * t) >> 14);
	o->v = a->v + (((b->v - a->v) * t) >> 14);
	o->c = a->c + (((b->c - a->c) * t) >> 14);
	o->cached = 0;
}

/* Sutherland-Hodgman against the near plane; returns the vertex count.
   (No frustum planes: a crossing there, rounded to the pixel, would sit
   off the neighbour's unclipped edge; subdivision handles what lies off
   screen.) */
static int clip_near(const sv_t *in, int n, sv_t *out)
{
	int m = 0;
	for (int i = 0; i < n; i++) {
		const sv_t *a = &in[i], *b = &in[(i + 1) % n];
		int32_t da = a->z - ZNEAR, db = b->z - ZNEAR;
		if (da >= 0)
			out[m++] = *a;
		if ((da >= 0) != (db >= 0)) {
			if (before(a, b))
				lerp(a, b, da, da - db, &out[m++]);
			else
				lerp(b, a, db, db - da, &out[m++]);
		}
	}
	return m;
}

/* ---------------------------------------------------------------- packets */

static uint32_t uvword(const sv_t *p)
{
	int32_t u = (p->u + 128) >> 8, v = (p->v + 128) >> 8;
	if (u < 0) u = 0;
	if (u > 255) u = 255;
	if (v < 0) v = 0;
	if (v > 255) v = 255;
	return (uint32_t) (u | (v << 8));
}

/* light 0..255 -> GPU colour 0x00cccccc */
static uint32_t grey(int32_t c)
{
	return (uint32_t) c * 0x010101u;
}

static uint32_t sxy(const sv_t *p)
{
	return ((uint32_t) p->sx & 0xffff) | ((uint32_t) p->sy << 16);
}

static void emit3(r_state_t *s, const sv_t *a, const sv_t *b, const sv_t *c)
{
	if (s->pkt + 40 > s->pkt_end) {
		s->stat_skipped++;
		return;
	}
	uint32_t *p = (uint32_t *) s->pkt;
	p[0] = (9u << 24) | s->ot_head;
	p[1] = 0x34000000u | grey(a->c);           /* POLY_GT3: texture x light */
	p[2] = sxy(a);
	p[3] = uvword(a) | clut16;
	p[4] = grey(b->c);
	p[5] = sxy(b);
	p[6] = uvword(b) | tpage16;
	p[7] = grey(c->c);
	p[8] = sxy(c);
	p[9] = uvword(c);
	s->ot_head = (uint32_t) p & 0xffffff;
	s->pkt += 40;
}

/* ------------------------------------------------------------ subdivision */

static int32_t iabs(int32_t v)
{
	return v < 0 ? -v : v;
}

/* Split decision for one edge, from that edge alone (see the header). */
static int split_edge(const r_state_t *s, const sv_t *p, const sv_t *q)
{
	int32_t l = iabs(p->sx - q->sx) + iabs(p->sy - q->sy);
	if (l > SPAN)
		return 1;
	return l * iabs(p->z - q->z) > (int32_t) s->warp_t2 * (p->z + q->z);
}

/* round(n / d) half up, d > 0: n + KOFS d >= 0 for every n here (|s| <
   KOFS), so the division is unsigned, as in the asm */
static int32_t rdiv(int32_t n, int32_t d)
{
	return (int32_t) (((uint32_t) n + (uint32_t) d * KOFS + ((uint32_t) d >> 1)) / (uint32_t) d) - KOFS;
}

static void midpoint(const sv_t *p, const sv_t *q, sv_t *m)
{
	/* symmetric in p, q: both neighbours compute the same point */
	int32_t zs = p->z + q->z;
	m->sx = rdiv(p->sx * p->z + q->sx * q->z, zs);
	m->sy = rdiv(p->sy * p->z + q->sy * q->z, zs);
	m->z  = zs >> 1;
	m->u  = (p->u + q->u) >> 1;
	m->v  = (p->v + q->v) >> 1;
	m->c  = (p->c + q->c) >> 1;
	m->cached = 1;
}

static void push(int *sp, const sv_t *a, const sv_t *b, const sv_t *c, int depth)
{
	tri_t *t = &stack[(*sp)++];
	t->v[0] = *a;
	t->v[1] = *b;
	t->v[2] = *c;
	t->depth = depth;
}

static void subdivide(r_state_t *s, const sv_t *a, const sv_t *b, const sv_t *c)
{
	int sp = 0;
	push(&sp, a, b, c, 0);

	while (sp) {
		tri_t t = stack[--sp];
		const sv_t *p0 = &t.v[0], *p1 = &t.v[1], *p2 = &t.v[2];

		int32_t minx = p0->sx, maxx = minx, miny = p0->sy, maxy = miny;
		for (int i = 1; i < 3; i++) {
			if (t.v[i].sx < minx) minx = t.v[i].sx;
			if (t.v[i].sx > maxx) maxx = t.v[i].sx;
			if (t.v[i].sy < miny) miny = t.v[i].sy;
			if (t.v[i].sy > maxy) maxy = t.v[i].sy;
		}
		if (maxx < 0 || minx >= 320 || maxy < 0 || miny >= 240)
			continue;                              /* off screen */

		int s01 = 0, s12 = 0, s20 = 0;
		if (t.depth < MAXD) {
			s01 = split_edge(s, p0, p1);
			s12 = split_edge(s, p1, p2);
			s20 = split_edge(s, p2, p0);
		}
		int nsplit = s01 + s12 + s20;
		if (!nsplit) {
			if (maxx - minx <= 1023 && maxy - miny <= 511)
				emit3(s, p0, p1, p2);
			else
				s->stat_skipped++;
			continue;
		}

		int d = t.depth + 1;
		sv_t m01, m12, m20;
		if (s01) midpoint(p0, p1, &m01);
		if (s12) midpoint(p1, p2, &m12);
		if (s20) midpoint(p2, p0, &m20);

		if (nsplit == 3) {                         /* 4 triangles */
			push(&sp, p0, &m01, &m20, d);
			push(&sp, &m01, p1, &m12, d);
			push(&sp, &m20, &m12, p2, d);
			push(&sp, &m01, &m12, &m20, d);
		} else if (nsplit == 2) {                  /* 3 triangles */
			/* rotate so the unsplit edge is (r2, r0): splits on r0-r1, r1-r2 */
			const sv_t *r0, *r1, *r2, *ma, *mb;
			if (!s20)      { r0 = p0; r1 = p1; r2 = p2; ma = &m01; mb = &m12; }
			else if (!s01) { r0 = p1; r1 = p2; r2 = p0; ma = &m12; mb = &m20; }
			else           { r0 = p2; r1 = p0; r2 = p1; ma = &m20; mb = &m01; }
			push(&sp, r0, ma, r2, d);
			push(&sp, ma, r1, mb, d);
			push(&sp, ma, mb, r2, d);
		} else {                                   /* 2 triangles */
			if (s01)      { push(&sp, p0, &m01, p2, d); push(&sp, &m01, p1, p2, d); }
			else if (s12) { push(&sp, p1, &m12, p0, d); push(&sp, &m12, p2, p0, d); }
			else          { push(&sp, p2, &m20, p1, d); push(&sp, &m20, p0, p1, d); }
		}
	}
}

/* -------------------------------------------------------------- the entry */

/* A cache value the GTE could not project well: too near, or saturated. */
static int off_range(uint32_t sxy_w, uint32_t sz)
{
	int32_t x = (int16_t) (sxy_w & 0xffff), y = (int16_t) (sxy_w >> 16);
	return sz < SZ_CLIP || x <= -1024 || x >= 1023 || y <= -1024 || y >= 1023;
}

/* Called from asm (draw_records): rec = face record, poly = its quad / tri,
   nv = 4 or 3. Uses r_state.pkt / ot_head, which the asm syncs around the
   call. */
void r_slow(r_state_t *s, const uint8_t *rec, const uint8_t *poly, int nv)
{
	const qw_record *r     = (const qw_record *) rec;
	const uint8_t   *light = rec + r->polys - ((r->num_verts + 3) & ~3);
	const uint16_t  *uvs   = (const uint16_t *) (poly + 4);
	const uint32_t  *c_sxy = (const uint32_t *) (SPAD + SP_SXY);
	const uint32_t  *c_sz  = (const uint32_t *) (SPAD + SP_SZ);

	const qw_tex *tex = (const qw_tex *) r_state.textures + r->tex;
	clut16  = (uint32_t) tex->clut << 16;
	tpage16 = (uint32_t) tex->tpage << 16;

	/* the corners, in PS1 order, from the cache */
	sv_t q[4];
	int clip = 0;
	for (int j = 0; j < nv; j++) {
		int slot = poly[j] >> 2;
		sv_t *o = &q[j];
		o->sx = (int16_t) (c_sxy[slot] & 0xffff);
		o->sy = (int16_t) (c_sxy[slot] >> 16);
		o->z  = (int32_t) (c_sz[slot] & 0xffff);
		o->u  = (int32_t) (uvs[j] & 0xff) << 8;
		o->v  = (int32_t) (uvs[j] >> 8) << 8;
		o->c  = light[slot];
		o->cached = !off_range(c_sxy[slot], c_sz[slot]);
		clip |= !o->cached;
	}

	if (!clip) {
		/* subdivision straight from the cache: a quad as the GPU's two
		   halves (draw_records' test is close to exact: a quad none of
		   whose edges splits rarely gets here) */
		subdivide(s, &q[0], &q[1], &q[2]);
		if (nv == 4)
			subdivide(s, &q[1], &q[3], &q[2]);
		return;
	}

	/* the clip path: view space (only clipping needs it) */
	const uint16_t *vidx = (const uint16_t *) (rec + sizeof(qw_record));   /* into the pool */
	const qw_vert  *pool = (const qw_vert *) s->vertpool;
	static const int quad_ring[4] = { 0, 1, 3, 2 };   /* PS1 order -> ring */
	sv_t in[4];
	const int16_t *m = s->rot;
	for (int k = 0; k < nv; k++) {
		int j = nv == 4 ? quad_ring[k] : k;
		const qw_vert *p = &pool[vidx[poly[j] >> 2]];
		sv_t *o = &in[k];
		*o = q[j];
		/* as the GTE's MVMVA (sf = 1): (TR << 12 + R V) >> 12 */
		o->x = (int32_t) ((((int64_t) s->tr[0] << 12) + m[0] * p->x + m[1] * p->y + m[2] * p->z) >> 12);
		o->y = (int32_t) ((((int64_t) s->tr[1] << 12) + m[3] * p->x + m[4] * p->y + m[5] * p->z) >> 12);
		if (!o->cached)
			o->z = (int32_t) ((((int64_t) s->tr[2] << 12) + m[6] * p->x + m[7] * p->y + m[8] * p->z) >> 12);
	}

	/* clip against the near plane (when a vertex is behind it) */
	sv_t buf[2][4 + 1];
	int n = nv, behind = 0;
	for (int k = 0; k < nv; k++) {
		buf[0][k] = in[k];
		behind += in[k].z < ZNEAR;
	}
	if (behind == nv)
		return;
	sv_t *out = buf[0];
	if (behind) {
		n = clip_near(buf[0], nv, buf[1]);
		out = buf[1];
	}
	for (int i = 0; i < n; i++) {
		if (out[i].cached)
			continue;                              /* the cache's own values */
		if (out[i].z < ZNEAR)
			out[i].z = ZNEAR;                      /* rounding of the lerp */
		project(&out[i]);
	}
	for (int i = 1; i + 1 < n; i++)
		subdivide(s, &out[0], &out[i], &out[i + 1]);
}
