/*
 * q1psx — renderer subdivision paths in C: engine/r_slow.s's C version
 * (R_SLOW_C = 1 in engine/r_defs.inc; the C engine, CMake Q1PSX_ENGINE_C).
 * It was the asm's specification; the asm is the base now, and this follows
 * it (docs/porting-notes.md §55): the same decisions in the same order, the
 * same packets, from the scratchpad words the asm reads, the world's or a
 * model's (its bounds, its vertex pool, its texture, the GTE's matrix as
 * r_world.s loaded it).
 *
 * engine/r_world.s calls r_slow() for a polygon of a face record that the
 * fast path cannot simply emit: the record's bounding sphere says one of its
 * vertices may be too near (the GTE's projection breaks down), or one of its
 * polygons may be too big for the GPU, or its affine texture error may exceed
 * the threshold (docs/porting-notes.md §19, §19.4). Per polygon:
 *
 *   - a vertex nearer than SZ_CLIP, or projected beyond the GTE's range:
 *     the clip path. Transform to view space, clip against the near plane,
 *     project the new vertices;
 *   - otherwise: straight from the record's vertex cache (the fast path's
 *     RTPT results: SXY, SZ, light).
 *
 * Then both subdivide in screen space. An edge splits when its affine error
 * exceeds the threshold, or when it is too long for the GPU:
 *     |ds| * |za - zb| > T2 * (za + zb),   T2 = 2 x threshold (px)
 *     |ds| = max(|dx|, |dy|) + 3/8 min(|dx|, |dy|)
 * (the midpoint's screen displacement between affine and perspective is
 * |ds| * |za - zb| / (2 (za + zb)); the octagon's |ds| is within 3 % under
 * and 7 % over the true length, |dx| + |dy| up to 41 % over), or
 * |dx| + |dy| > SPAN. The
 * decision and the midpoint depend on the edge's two ends only, and every
 * vertex an edge can share with another polygon comes from the same cache
 * value, so neighbours split shared edges identically: no T-junction, no
 * crack. The midpoint is the projected 3D midpoint, from screen positions
 * and depths alone:
 *     s = (sa za + sb zb) / (za + zb),   z = (za + zb) / 2,
 *     u, v, light = averages (affine in 3D).
 */

#include <stdint.h>
#include <psxgte.h>
#include <inline_c.h>

#include "prof.h"
#include "qworld.h"
#include "render.h"

#define VU       6            /* view units per world unit                  */
#define ZNEAR    (8 * VU)     /* the near clip plane, the world's and the
                                 models' (SP_ZNEAR: the view model's 120)   */
#define SZ_CLIP  96           /* SZ below this: the GTE's projection is off
                                 (SP_SZCLIP: the view model's 124)          */
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

/* the scratchpad's words the asm reads (engine/r_defs.inc): the record's
   vertex cache, what r_world.s set for the records it draws */
#define SPAD        0x1F800000u
#define SP_SXY      0x100     /* projected XY per record vertex             */
#define SP_SZ       0x1a8     /* SZ per record vertex                       */
#define SP_COL      0x250     /* light per record vertex, 0x00cccccc        */
#define SP_FULL     0x33c     /* non-zero once the packet arena is full     */
#define SP_OFY      0x3a8     /* the viewport's vertical centre             */
#define SP_SZCLIP   0x3b0     /* SZ_CLIP, or the view model's               */
#define SP_ZNEAR    0x3b4     /* ZNEAR, or the view model's                 */
#define SP_HSHIFT   0x3b8     /* H = 5 << it                                */
#define SP_VERTPOOL 0x3c0     /* the vertex pool: the map's, a model's      */
#define SP_CLUT16   0x3d4     /* the record's texture: its clut << 16       */
#define SP_TPAGE16  0x3d8     /* its tpage << 16                            */
#define SP_T2       0x3e4     /* T2 (the view model's 0x7fff: no split)     */
#define SPW(o)      (*(uint32_t *) (SPAD + (o)))

typedef struct {
	int32_t  x, y, z;         /* view space (clip path)                      */
	int32_t  u, v;            /* texels x256 (page-relative)                 */
	int32_t  c;               /* light 0..255                                */
	uint32_t gc;              /* its colour word: a corner's the cache's, the
	                             others' grey c                              */
	int32_t  sx, sy;          /* projected, 16 bits (the asm's pool's)       */
	int32_t  cached;          /* sx, sy, z are the cache's (do not project)  */
} sv_t;

typedef struct { sv_t v[3]; int depth; } tri_t;

static tri_t    stack[3 * MAXD + 2];         /* depth-first, <= 3 left per level */
static uint32_t clut16, tpage16;
static int32_t  t2, h, ofx, ofy, znear;      /* the call's: T2, H, the centre, the plane */

/* ------------------------------------------------------------- projection */

static void project(sv_t *p)
{
	/* past the near plane only: x, y may be far off screen, so divide each
	   (x H as the asm's 5 x << shift); subdivision culls or splits what lies
	   outside */
	p->sx = (int16_t) (ofx + p->x * h / p->z);
	p->sy = (int16_t) (ofy + p->y * h / p->z);
}

/* Clip-path vertex order: crossings are always interpolated from the same
   end, so two polygons sharing a crossed edge make the same point. */
static int before(const sv_t *a, const sv_t *b)
{
	if (a->z != b->z) return a->z < b->z;
	if (a->x != b->x) return a->x < b->x;
	return a->y < b->y;
}

/* light 0..255 -> GPU colour 0x00cccccc */
static uint32_t grey(int32_t c)
{
	return (uint32_t) c * 0x010101u;
}

static void lerp(const sv_t *a, const sv_t *b, int32_t num, int32_t den, sv_t *o)
{
	/* one division per edge. Within one polygon (a texture repetition, at
	   most 256 world units) plane distances stay < 2^16 and deltas < 2^16,
	   so a 14-bit factor keeps every product below 2^31. */
	int32_t t = (num << 14) / den;
	o->x  = a->x + (((b->x - a->x) * t) >> 14);
	o->y  = a->y + (((b->y - a->y) * t) >> 14);
	o->z  = a->z + (((b->z - a->z) * t) >> 14);
	o->u  = a->u + (((b->u - a->u) * t) >> 14);
	o->v  = a->v + (((b->v - a->v) * t) >> 14);
	o->c  = a->c + (((b->c - a->c) * t) >> 14);
	o->gc = grey(o->c);
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
		int32_t da = a->z - znear, db = b->z - znear;
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

/* the rounded texels, as the asm's: a corner's, its record's uv halfword */
static uint32_t uvword(const sv_t *p)
{
	return (uint32_t) (p->u + 128) >> 8 | ((uint32_t) (p->v + 128) & 0xff00);
}

static uint32_t sxy(const sv_t *p)
{
	return ((uint32_t) p->sx & 0xffff) | ((uint32_t) p->sy << 16);
}

/* one POLY_GT3, prepended to the world bucket; 0: the arena was full (the
   asm's packet pointer past its bound: SP_FULL set, the polygon given up) */
static int emit3(r_state_t *s, const sv_t *a, const sv_t *b, const sv_t *c)
{
	if ((s->pkt & 0x1fffffff) > (s->pkt_end & 0x1fffffff)) {
		SPW(SP_FULL) = 1;
		return 0;
	}
	uint32_t *p = (uint32_t *) s->pkt;
	p[0] = (9u << 24) | s->ot_head;
	p[1] = 0x34000000u | a->gc;                /* POLY_GT3: texture x light */
	p[2] = sxy(a);
	p[3] = uvword(a) | clut16;
	p[4] = b->gc;
	p[5] = sxy(b);
	p[6] = uvword(b) | tpage16;
	p[7] = c->gc;
	p[8] = sxy(c);
	p[9] = uvword(c);
	s->ot_head = (uint32_t) p & 0xffffff;
	s->pkt += 40;
	return 1;
}

/* ------------------------------------------------------------ subdivision */

static int32_t iabs(int32_t v)
{
	return v < 0 ? -v : v;
}

/* Split decision for one edge, from that edge alone (see the header): its
   screen length as max + 3/8 min of |dx|, |dy| (the asm's NOSPLIT tests
   |dx| + |dy| first, then REFINE the marked edges: the same split, as
   that length never exceeds |dx| + |dy|); the products compare unsigned,
   as the asm's */
static int split_edge(const sv_t *p, const sv_t *q)
{
	int32_t ax = iabs(p->sx - q->sx), ay = iabs(p->sy - q->sy);
	if (ax + ay > SPAN)
		return 1;
	int32_t mx = ax < ay ? ay : ax, mn = ax < ay ? ax : ay;
	int32_t l = mx + (3 * mn >> 3);
	return (uint32_t) (l * iabs(p->z - q->z)) > (uint32_t) t2 * (uint32_t) p->z + (uint32_t) t2 * (uint32_t) q->z;
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
	m->sx = (int16_t) rdiv(p->sx * p->z + q->sx * q->z, zs);
	m->sy = (int16_t) rdiv(p->sy * p->z + q->sy * q->z, zs);
	m->z  = zs >> 1;
	m->u  = (p->u + q->u) >> 1;
	m->v  = (p->v + q->v) >> 1;
	m->c  = (p->c + q->c) >> 1;
	m->gc = grey(m->c);
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

/* 0: the arena got full (the polygon given up) */
static int subdivide(r_state_t *s, const sv_t *a, const sv_t *b, const sv_t *c)
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
		if (maxx < 0 || minx >= r_res_w || maxy < 0 || miny >= r_res_h)
			continue;                              /* off screen */

		int s01 = 0, s12 = 0, s20 = 0;
		if (t.depth < MAXD) {
			s01 = split_edge(p0, p1);
			s12 = split_edge(p1, p2);
			s20 = split_edge(p2, p0);
		}
		int nsplit = s01 + s12 + s20;
		if (!nsplit) {
			/* (unsplit edges are within SPAN: only a leaf at the depth
			   limit can be too big for the GPU, and it is dropped) */
			if (maxx - minx <= 1023 && maxy - miny <= 511 && !emit3(s, p0, p1, p2))
				return 0;
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
	return 1;
}

/* -------------------------------------------------------------- the entry */

/* Called from asm (draw_records): rec = face record, poly = its quad / tri,
   nv = 4 or 3. Uses r_state.pkt / ot_head, which the asm syncs around the
   call. */
int r_tess(r_state_t *s, const uint8_t *rec, const uint8_t *poly, int nv);   /* (engine/r_tess.c) */

void r_slow(r_state_t *s, const uint8_t *rec, const uint8_t *poly, int nv)
{
	if (r_tess(s, rec, poly, nv))                      /* the regular subdivision first (§92) */
		return;
	const uint16_t *uvs   = (const uint16_t *) (poly + 4);
	const uint32_t *c_sxy = (const uint32_t *) (SPAD + SP_SXY);
	const uint32_t *c_sz  = (const uint32_t *) (SPAD + SP_SZ);
	const uint32_t *c_col = (const uint32_t *) (SPAD + SP_COL);
	int32_t szclip = (int32_t) SPW(SP_SZCLIP);

	clut16  = SPW(SP_CLUT16);
	tpage16 = SPW(SP_TPAGE16);
	t2      = (int32_t) SPW(SP_T2);
	h       = r_res_table[r_res].mul << SPW(SP_HSHIFT);   /* (the target's: W / 2) */
	ofx     = r_res_w / 2;
	ofy     = (int32_t) SPW(SP_OFY);
	znear   = (int32_t) SPW(SP_ZNEAR);

	/* the corners, in PS1 order, from the cache; one the GTE could not
	   project well (too near, or saturated) sends the polygon to the clip
	   path */
	sv_t q[4];
	int clip = 0;
	for (int j = 0; j < nv; j++) {
		int slot = poly[j] >> 2;
		sv_t *o = &q[j];
		o->sx = (int16_t) (c_sxy[slot] & 0xffff);
		o->sy = (int16_t) (c_sxy[slot] >> 16);
		o->z  = (int32_t) (c_sz[slot] & 0xffff);
		o->u  = (int32_t) (uvs[j] & 0xff) << 8;
		o->v  = (int32_t) (uvs[j] & 0xff00);
		o->gc = c_col[slot];
		o->c  = (int32_t) (o->gc & 0xff);
		o->cached = !(o->z < szclip || o->sx <= -1024 || o->sx >= 1023 || o->sy <= -1024 || o->sy >= 1023);
		clip |= !o->cached;
	}

	if (!clip) {
		/* subdivision straight from the cache: a quad as the GPU's two
		   halves (draw_records' test is close to exact: a quad none of
		   whose edges splits rarely gets here) */
		if (subdivide(s, &q[0], &q[1], &q[2]) && nv == 4)
			subdivide(s, &q[1], &q[3], &q[2]);
		return;
	}

	/* the clip path: view space (only clipping needs it), through the
	   GTE's matrix as the asm left it, the view's or the entity's */
	const uint16_t *vidx = (const uint16_t *) (rec + sizeof(qw_record));   /* into the pool */
	const qw_vert  *pool = (const qw_vert *) SPW(SP_VERTPOOL);
	static const int quad_ring[4] = { 0, 1, 3, 2 };   /* PS1 order -> ring */
	sv_t in[4];
	for (int k = 0; k < nv; k++) {
		int j = nv == 4 ? quad_ring[k] : k;
		int32_t mac[3];
		gte_ldv0(&pool[vidx[poly[j] >> 2]]);
		gte_rtv0tr();                              /* MAC = (TR << 12 + R V) >> 12 */
		gte_stlvnl(mac);
		sv_t *o = &in[k];
		*o = q[j];
		o->x = mac[0];
		o->y = mac[1];
		if (!o->cached)
			o->z = mac[2];
	}

	/* clip against the near plane (when a vertex is behind it) */
	sv_t buf[2][4 + 1];
	int n = nv, behind = 0;
	for (int k = 0; k < nv; k++) {
		buf[0][k] = in[k];
		behind += in[k].z < znear;
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
		if (out[i].z < znear)
			out[i].z = znear;                      /* rounding of the lerp */
		project(&out[i]);
	}
	for (int i = 1; i + 1 < n; i++)
		if (!subdivide(s, &out[0], &out[i], &out[i + 1]))
			return;
}
