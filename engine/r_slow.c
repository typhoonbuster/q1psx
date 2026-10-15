/*
 * q1psx — renderer slow path (R3.3). C reference version: it will be ported
 * to asm once validated, like the rest of the renderer (architecture.md §8).
 *
 * engine/r_world.s calls r_slow() for a polygon the fast path cannot draw:
 * one of its vertices is nearer than sz_fast. There the GTE projection is
 * inaccurate (its divide saturates below 13 world units), the polygon can
 * be bigger than the GPU accepts (1023 x 511 px), and affine texturing
 * warps visibly.
 *
 * Work happens in view space (x right, y down, z forward, world x 6):
 *   1. transform the polygon's vertices with the current view matrix,
 *   2. clip against the near plane z = ZNEAR (8 world units: the player's
 *      collision box keeps walls >= 16 units away),
 *   3. fan-triangulate, then split each triangle on its longest screen edge
 *      (3D midpoint, UVs interpolated) until it is small on screen and its
 *      depth range is narrow enough for affine texturing to look right,
 *   4. project with integer division and emit POLY_GT3 packets (texture x
 *      the interpolated vertex light),
 *      prepended to the world OT bucket exactly like the asm does.
 */

#include <stdint.h>

#include "prof.h"
#include "qworld.h"
#include "render.h"

#define VU       6            /* view units per world unit                  */
#define ZNEAR    (8 * VU)     /* near clip plane                            */
#define SPAN     96           /* split edges longer than this on screen (px)  */
#define MAXD     5            /* max split depth                            */
#define OFX      160
#define OFY      120

typedef struct {
	int32_t x, y, z;          /* view space                                  */
	int32_t u, v;             /* texels x256 (page-relative)                 */
	int32_t c;                /* light 0..255                                */
	int32_t sx, sy;           /* projected                                   */
} sv_t;

typedef struct { sv_t v[3]; int depth; } tri_t;

static tri_t    stack[3 * MAXD + 2];         /* depth-first, <= 3 left per level */
static uint32_t clut16, tpage16;


static void project(sv_t *p)
{
	/* one division per vertex: after frustum clipping |x|,|y| <~ z, so the
	   products stay far below 2^31 */
	int32_t inv = (R_SCREEN_H << 16) / p->z;
	p->sx = OFX + ((p->x * inv) >> 16);
	p->sy = OFY + ((p->y * inv) >> 16);
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
}

/* Signed distance to clip plane `plane` (inside >= 0). The frustum planes
   get a 2-unit guard so rounding never pulls a vertex inside the screen. */
static int32_t plane_d(const sv_t *p, int plane)
{
	switch (plane) {
	case 0:  return p->z - ZNEAR;                  /* near            */
	case 1:  return p->z + p->x + 2 * VU;          /* left:   x >= -z */
	case 2:  return p->z - p->x + 2 * VU;          /* right:  x <=  z */
	case 3:  return 3 * p->z + 4 * p->y + 8 * VU;  /* top:    y >= -3z/4 */
	default: return 3 * p->z - 4 * p->y + 8 * VU;  /* bottom: y <=  3z/4 */
	}
}

/* Sutherland-Hodgman against one plane; returns the output vertex count. */
static int clip_plane(const sv_t *in, int n, sv_t *out, int plane)
{
	int m = 0;
	for (int i = 0; i < n; i++) {
		const sv_t *a = &in[i], *b = &in[(i + 1) % n];
		int32_t da = plane_d(a, plane), db = plane_d(b, plane);
		if (da >= 0)
			out[m++] = *a;
		if ((da >= 0) != (db >= 0))
			lerp(a, b, da, da - db, &out[m++]);
	}
	return m;
}

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

static void emit(r_state_t *s, const sv_t *a, const sv_t *b, const sv_t *c)
{
	if (s->pkt + 40 > s->pkt_end) {
		s->stat_skipped++;
		return;
	}
	uint32_t *p = (uint32_t *) s->pkt;
	p[0] = (9u << 24) | s->ot_head;
	p[1] = 0x34000000u | grey(a->c);           /* POLY_GT3: texture x light */
	p[2] = ((uint32_t) a->sx & 0xffff) | ((uint32_t) a->sy << 16);
	p[3] = uvword(a) | clut16;
	p[4] = grey(b->c);
	p[5] = ((uint32_t) b->sx & 0xffff) | ((uint32_t) b->sy << 16);
	p[6] = uvword(b) | tpage16;
	p[7] = grey(c->c);
	p[8] = ((uint32_t) c->sx & 0xffff) | ((uint32_t) c->sy << 16);
	p[9] = uvword(c);
	s->ot_head = (uint32_t) p & 0xffffff;
	s->pkt += 40;
	s->stat_slow++;
}

/* Split decision for one edge, from that edge alone — so the two triangles
   sharing an edge (inside a polygon, or across records: the cook dedups
   vertex positions exactly) always cut it the same way and no T-junction
   cracks appear. Split when the edge is long on screen, or when its depth
   ratio is high enough for affine texturing to warp visibly. */
static int split_edge(const sv_t *p, const sv_t *q)
{
	int32_t dx = p->sx - q->sx, dy = p->sy - q->sy;
	if ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) > SPAN)
		return 1;
	int32_t zlo = p->z < q->z ? p->z : q->z, zhi = p->z ^ q->z ^ zlo;
	return 3 * zhi > 4 * zlo;                    /* depth ratio > 4/3 */
}

static void midpoint(const sv_t *p, const sv_t *q, sv_t *m)
{
	/* symmetric in p, q: both neighbours compute the same point */
	m->x = (p->x + q->x) >> 1;
	m->y = (p->y + q->y) >> 1;
	m->z = (p->z + q->z) >> 1;
	m->u = (p->u + q->u) >> 1;
	m->v = (p->v + q->v) >> 1;
	m->c = (p->c + q->c) >> 1;
	project(m);
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
			s01 = split_edge(p0, p1);
			s12 = split_edge(p1, p2);
			s20 = split_edge(p2, p0);
		}
		int nsplit = s01 + s12 + s20;
		if (!nsplit) {
			if (maxx - minx <= 1023 && maxy - miny <= 511)
				emit(s, p0, p1, p2);
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

/* Called from asm (draw_records): rec = face record, poly = its quad / tri,
   nv = 4 or 3. Uses r_state.pkt / ot_head, which the asm syncs around the
   call. */
void r_slow(r_state_t *s, const uint8_t *rec, const uint8_t *poly, int nv)
{
	const qw_record *r     = (const qw_record *) rec;
	const uint16_t  *vidx  = (const uint16_t *) (rec + sizeof(qw_record));   /* into the pool */
	const uint8_t   *light = rec + r->polys - ((r->num_verts + 3) & ~3);
	const qw_vert   *pool  = (const qw_vert *) s->vertpool;
	const uint16_t  *uvs   = (const uint16_t *) (poly + 4);
	static const int quad_cycle[4] = { 0, 1, 3, 2 };   /* PS1 order -> ring */

	const qw_tex *tex = (const qw_tex *) r_state.textures + r->tex;
	clut16  = (uint32_t) tex->clut << 16;
	tpage16 = (uint32_t) tex->tpage << 16;

	sv_t in[4];
	const int16_t *m = s->rot;
	for (int k = 0; k < nv; k++) {
		int j = nv == 4 ? quad_cycle[k] : k;
		const qw_vert *p = &pool[vidx[poly[j] >> 2]];
		sv_t *o = &in[k];
		o->x = ((m[0] * p->x + m[1] * p->y + m[2] * p->z) >> 12) + s->tr[0];
		o->y = ((m[3] * p->x + m[4] * p->y + m[5] * p->z) >> 12) + s->tr[1];
		o->z = ((m[6] * p->x + m[7] * p->y + m[8] * p->z) >> 12) + s->tr[2];
		o->u = (int32_t) (uvs[j] & 0xff) << 8;
		o->v = (int32_t) (uvs[j] >> 8) << 8;
		o->c = light[poly[j] >> 2];
	}

	/* Clip in view space against the near plane AND the four frustum planes
	   (idea from the GBA Quake prototype's renderer): nothing off-screen is
	   ever generated, so no subdivision is wasted on it, and every projected
	   coordinate stays inside the GPU's range. */
	sv_t buf[2][4 + 5 + 1];
	int n = nv, cur = 0;
	for (int k = 0; k < nv; k++)
		buf[0][k] = in[k];

	/* clip codes (one bit per plane): all vertices outside one plane ->
	   nothing to draw; a plane no vertex crosses -> no clipping against it */
	uint32_t codes_and = 0x1f, codes_or = 0;
	for (int k = 0; k < nv; k++) {
		uint32_t c = 0;
		for (int plane = 0; plane < 5; plane++)
			if (plane_d(&in[k], plane) < 0)
				c |= 1u << plane;
		codes_and &= c;
		codes_or  |= c;
	}
	if (codes_and) {
		return;
	}
	for (int plane = 0; plane < 5 && n >= 3; plane++)
		if (codes_or & (1u << plane)) {
			n = clip_plane(buf[cur], n, buf[cur ^ 1], plane);
			cur ^= 1;
		}
	sv_t *out = buf[cur];
	if (n >= 3) {
		for (int i = 0; i < n; i++) {
			if (out[i].z < ZNEAR)
				out[i].z = ZNEAR;                  /* rounding of the lerp */
			project(&out[i]);
		}
		for (int i = 1; i + 1 < n; i++)
			subdivide(s, &out[0], &out[i], &out[i + 1]);
	}
}
