/*
 * q1psx — the near polygons' regular subdivision: engine/r_tess.s's C
 * version (the C engine, CMake Q1PSX_ENGINE_C; docs/porting-notes.md §92).
 * The asm is the base; this follows it decision for decision, packet for
 * packet (docs/porting-notes.md §55): the same corners read from the
 * record's vertex cache, the same level from the same bounds, the same
 * grid (the midpoints' rounding), the GTE's projection, the same cells
 * emitted and the same left out.
 *
 * r_slow (engine/r_slow.c) calls r_tess first: 1 when the polygon was
 * drawn (or given up, the arena full), 0 when it is the slow path's (a
 * corner too near or off the GTE's range, a level past KMAX).
 */

#include <stdint.h>
#include <psxgte.h>
#include <inline_c.h>

#include "qworld.h"
#include "render.h"
#include "vid.h"

#define KMAX     3                        /* the finest level (engine/r_tess.s KMAX) */
#define CAP      4                        /* the cap: levels summing past it, the slow path's */
#define SPAN     500
#define NMAX     8

typedef struct {
	int16_t  x, y, z, pad;
	uint16_t u, v, c, pad2;   /* u, v: texels x 256; c: the light */
	uint32_t gc, uvw;         /* the grey colour word, the packet's uv word */
	uint32_t sxy, sz;         /* the GTE's */
} tp_t;

static tp_t grid[(NMAX + 1) * (NMAX + 1)];

#if Q1PSX_ENGINE_C
volatile uint32_t r_tess_stat[6];         /* the asm's counters (r_tess.s), the C engine's here */
#define TSTAT(k) (r_tess_stat[k]++)
#else
#define TSTAT(k) ((void) 0)
#endif

#define SPAD        0x1F800000u
#define SP_SXY      0x100
#define SP_SZ       0x1a8
#define SP_COL      0x250
#define SP_FULL     0x33c
#define SP_SZCLIP   0x3b0
#define SP_VERTPOOL 0x3c0
#define SP_CLUT16   0x3d4
#define SP_TPAGE16  0x3d8
#define SP_T4       0x3e8
#define SPW(o)      (*(uint32_t *) (SPAD + (o)))

static void mid(const tp_t *a, const tp_t *b, tp_t *m)
{
	m->x = (int16_t) ((a->x + b->x) >> 1);
	m->y = (int16_t) ((a->y + b->y) >> 1);
	m->z = (int16_t) ((a->z + b->z) >> 1);
	m->pad = 0;
	m->u = (uint16_t) ((a->u + b->u) >> 1);
	m->v = (uint16_t) ((a->v + b->v) >> 1);
	m->c = (uint16_t) ((a->c + b->c) >> 1);
	m->uvw = ((uint32_t) (m->u + 128) >> 8) | ((uint32_t) (m->v + 128) & 0xff00);
	m->gc  = (uint32_t) m->c * 0x010101u;
}

/* the points between p[0] and p[n x stride] (n a power of two, at least
   2), level by level: the midpoint of each pair, then of each half (the
   asm's tess_line) */
static void line(tp_t *p, int stride, int n)
{
	for (int half = n / 2; half >= 1; half /= 2)
		for (int at = 0; at + 2 * half <= n; at += 2 * half)
			mid(&p[at * stride], &p[(at + 2 * half) * stride], &p[(at + half) * stride]);
}

/* four screen positions all off the screen on one side (the asm's OFF4) */
static int off4(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t wide, uint32_t rows)
{
	uint32_t all = a & b & c & d;
	if ((int32_t) all < 0 || (all & 0x8000))
		return 1;                                 /* all y < 0, or all x < 0 */
	if ((int32_t) ((a - rows) | (b - rows) | (c - rows) | (d - rows)) >= 0)
		return 1;                                 /* all y >= the rows */
	if ((int32_t) (((a << 16) - wide) | ((b << 16) - wide) | ((c << 16) - wide) | ((d << 16) - wide)) >= 0)
		return 1;                                 /* all x >= the width */
	return 0;
}

/* the level an edge needs (the asm's EDGE and tess_lvl): -1 past KMAX */
static int level(const uint32_t *c_sxy, const uint32_t *c_sz, uint32_t t4, int a, int b)
{
	int32_t xa = (int16_t) (c_sxy[a] & 0xffff), ya = (int16_t) (c_sxy[a] >> 16);
	int32_t xb = (int16_t) (c_sxy[b] & 0xffff), yb = (int16_t) (c_sxy[b] >> 16);
	int32_t za = (int32_t) c_sz[a], zb = (int32_t) c_sz[b];
	int32_t dx = xa - xb, dy = ya - yb, dz = za - zb;
	if (dx < 0) dx = -dx;
	if (dy < 0) dy = -dy;
	if (dz < 0) dz = -dz;
	int32_t zm = zb < za ? zb : za, mx = dx < dy ? dy : dx, mn = dx < dy ? dx : dy;
	uint32_t ds = (uint32_t) (mx + (3 * mn >> 3)), prod = ds * (uint32_t) dz;
	uint32_t bound = t4 * (uint32_t) zm, tail = (t4 >> 1) * (uint32_t) dz, span = SPAN;
	uint32_t l1 = (uint32_t) (dx + dy);
	int m = 0;
	while (bound + tail < prod || span < l1) {     /* (the nearer half's za + zb: 2 zm + dz / 2^m) */
		if (++m > KMAX)
			return -1;
		bound <<= 2;
		tail <<= 1;
		span <<= 1;
	}
	return m;
}

int r_tess(r_state_t *s, const uint8_t *rec, const uint8_t *poly, int nv)
{
	const uint32_t *c_sxy = (const uint32_t *) (SPAD + SP_SXY);
	const uint32_t *c_sz  = (const uint32_t *) (SPAD + SP_SZ);
	const uint32_t *c_col = (const uint32_t *) (SPAD + SP_COL);
	int32_t szclip = (int32_t) SPW(SP_SZCLIP);
	int slot[4];
	for (int j = 0; j < 4; j++)
		slot[j] = poly[j] >> 2;
	if (nv == 3)
		slot[3] = slot[2];                        /* a triangle: its third corner is the grid's fourth */

	/* the corners' depths and positions: one too near or off the GTE's
	   range is the slow path's */
	for (int j = 0; j < 4; j++)
		if ((int32_t) c_sz[slot[j]] < szclip) {
			TSTAT(2);
			return 0;
		}
	for (int j = 0; j < 4; j++) {
		int32_t x = (int16_t) (c_sxy[slot[j]] & 0xffff), y = (int16_t) (c_sxy[slot[j]] >> 16);
		if ((uint32_t) (x + 1023) >= 2046 || (uint32_t) (y + 1023) >= 2046) {
			TSTAT(2);
			return 0;
		}
	}

	/* the levels: along the rows the top and bottom edges' greatest (ku),
	   down the columns the left and right ones' (kv); the diagonal the GPU
	   draws, corners 1-2, raises the larger of the two to its own */
	uint32_t t4 = SPW(SP_T4);
	int ku, kv, k;
	if ((ku = level(c_sxy, c_sz, t4, slot[0], slot[1])) < 0 || (k = level(c_sxy, c_sz, t4, slot[2], slot[3])) < 0) {
		TSTAT(3);
		return 0;
	}
	if (k > ku)
		ku = k;
	if ((kv = level(c_sxy, c_sz, t4, slot[0], slot[2])) < 0 || (k = level(c_sxy, c_sz, t4, slot[1], slot[3])) < 0) {
		TSTAT(3);
		return 0;
	}
	if (k > kv)
		kv = k;
	if ((k = level(c_sxy, c_sz, t4, slot[1], slot[2])) < 0) {
		TSTAT(3);
		return 0;
	}
	if (ku < kv) {
		if (kv < k)
			kv = k;
	} else if (ku < k)
		ku = k;
	if (ku + kv > CAP) {
		TSTAT(5);                                 /* past the cap: the slow path's */
		return 0;
	}
	uint32_t clut16 = SPW(SP_CLUT16), tpage16 = SPW(SP_TPAGE16);
	uint32_t wide = (uint32_t) VID_W << 16, rows = (uint32_t) s->vh2 << 16;
	const uint16_t *uvs  = (const uint16_t *) (poly + 4);
	if (!ku && !kv) {
		/* the five edges pass: the polygon unsplit, one packet from the
		   cache (the slow path's two triangles) */
		if (((s->pkt + 12) & 0x1fffffff) > (s->pkt_end & 0x1fffffff)) {
			SPW(SP_FULL) = 1;
			return 1;
		}
		TSTAT(4);
		uint32_t a = c_sxy[slot[0]], b = c_sxy[slot[1]], c = c_sxy[slot[2]], d = c_sxy[slot[3]];
		if (off4(a, b, c, d, wide, rows))
			return 1;
		uint32_t *p = (uint32_t *) s->pkt;
		p[1] = (nv == 3 ? 0x34000000u : 0x3c000000u) | c_col[slot[0]];
		p[2] = a;
		p[3] = uvs[0] | clut16;
		p[4] = c_col[slot[1]];
		p[5] = b;
		p[6] = uvs[1] | tpage16;
		p[7] = c_col[slot[2]];
		p[8] = c;
		p[9] = uvs[2];
		if (nv == 3) {
			p[0] = (9u << 24) | s->ot_head;
			s->ot_head = (uint32_t) p & 0xffffff;
			s->pkt += 40;
		} else {
			p[10] = c_col[slot[3]];
			p[11] = d;
			p[12] = uvs[3];
			p[0] = (12u << 24) | s->ot_head;
			s->ot_head = (uint32_t) p & 0xffffff;
			s->pkt += 52;
		}
		return 1;
	}
	int nu = 1 << ku, nvr = 1 << kv, rs = nu + 1;
	TSTAT(0);
#if Q1PSX_ENGINE_C
	r_tess_stat[1] += (uint32_t) (nu * nvr);
#endif

	/* the corners into the grid */
	const uint16_t *vidx = (const uint16_t *) (rec + sizeof(qw_record));
	const qw_vert  *pool = (const qw_vert *) SPW(SP_VERTPOOL);
	tp_t *g[4] = { &grid[0], &grid[nu], &grid[nvr * rs], &grid[nvr * rs + nu] };
	for (int j = 0; j < 4; j++) {
		const qw_vert *v = &pool[vidx[slot[j]]];
		uint32_t uvw = uvs[nv == 3 && j == 3 ? 2 : j];
		tp_t *p = g[j];
		p->x = v->x; p->y = v->y; p->z = v->z; p->pad = 0;
		p->uvw = uvw;
		p->u = (uint16_t) ((uvw & 0xff) << 8);
		p->v = (uint16_t) (uvw & 0xff00);
		p->gc = c_col[slot[j]];
		p->c  = (uint16_t) (p->gc & 0xff);
	}
	if (nu > 1) {
		line(&grid[0], 1, nu);                    /* the top row */
		line(&grid[nvr * rs], 1, nu);             /* the bottom row */
	}
	if (nvr > 1) {
		line(&grid[0], rs, nvr);                  /* the left column */
		line(&grid[nu], rs, nvr);                 /* the right column */
		if (nu > 1)
			for (int j = 1; j < nvr; j++)
				line(&grid[j * rs], 1, nu);       /* the rows between */
	}

	/* the grid through the GTE, three at a time (the last three again
	   when the count is not a multiple of three) */
	int cnt = rs * (nvr + 1);
	for (int i = 0;; i += 3) {
		if (i + 3 > cnt)
			i = cnt - 3;
		tp_t *p = &grid[i];
		gte_ldv0(&p[0]);
		gte_ldv1(&p[1]);
		gte_ldv2(&p[2]);
		gte_rtpt();
		gte_stsxy3(&p[0].sxy, &p[1].sxy, &p[2].sxy);
		gte_stsz3(&p[0].sz, &p[1].sz, &p[2].sz);     /* (SZ1, SZ2, SZ3: the three points') */
		if (i + 3 >= cnt)
			break;
	}

	/* the strips, then their cells (the asm makes a strip's rows only
	   when it is not off the screen: the same cells either way) */
	int trirow = nv == 3 ? nvr - 1 : -1;          /* the row drawn as triangles */
	for (int j = 0; j < nvr; j++) {
		if (off4(grid[j * rs].sxy, grid[j * rs + nu].sxy, grid[(j + 1) * rs].sxy, grid[(j + 1) * rs + nu].sxy, wide, rows))
			continue;                                 /* the strip off the screen on one side */
		for (int i = 0; i < nu; i++) {
			if (((s->pkt + 12) & 0x1fffffff) > (s->pkt_end & 0x1fffffff)) {   /* (at most 40 bytes
				SPW(SP_FULL) = 1;                                               past the bound, as
				return 1;                                                       the slow path's) */
			}
			const tp_t *tl = &grid[j * rs + i], *tr = tl + 1, *bl = tl + rs, *br = bl + 1;
			uint32_t a = tl->sxy, b = tr->sxy, c = bl->sxy, d = br->sxy;
			if (off4(a, b, c, d, wide, rows))
				continue;
			uint32_t *p = (uint32_t *) s->pkt;
			int tri = j == trirow;
			p[1] = (tri ? 0x34000000u : 0x3c000000u) | tl->gc;
			p[2] = a;
			p[3] = tl->uvw | clut16;
			p[4] = tr->gc;
			p[5] = b;
			p[6] = tr->uvw | tpage16;
			p[7] = bl->gc;
			p[8] = c;
			p[9] = bl->uvw;
			if (tri) {
				p[0] = (9u << 24) | s->ot_head;
				s->ot_head = (uint32_t) p & 0xffffff;
				s->pkt += 40;
			} else {
				p[10] = br->gc;
				p[11] = d;
				p[12] = br->uvw;
				p[0] = (12u << 24) | s->ot_head;
				s->ot_head = (uint32_t) p & 0xffffff;
				s->pkt += 52;
			}
		}
	}
	return 1;
}
