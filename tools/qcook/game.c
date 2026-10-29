/*
 * qcook — game data: the models, sprites and HUD pictures the engine needs
 * on every map, cooked from the pak into one QGM7 file per model level
 * (formats/qgame.h, formats/qlevels.h).
 *
 *   qcook game --pak <pak0.pak> [--textures medium|high] --out <prefix>
 *                                                   (<prefix>_<level>.dat)
 *
 * Models (Quake MDL): the triangles are cut into face records exactly like
 * the world's (formats/qworld.h), so the renderer draws them with its own
 * record loop; every frame becomes a vertex pool in vertex units (world x 4),
 * with the vertex's light-normal index in the pool entry's spare byte. A
 * vertex shared by two records is duplicated (each record's indices are its
 * own slots), so records gather adjacent triangles. View models are written
 * nearest-first, the renderer's painter order, as seen from the eye at the
 * model origin. Skins go to VRAM reduced (a view model's to a half, a
 * projectile's to a quarter: 2x2 / 4x4 texels averaged, then the nearest
 * palette colour): the screen never shows them larger. Each skin and
 * sprite sheet has a border of its own edge texels in the page (the GPU
 * strays a texel past a triangle's edge).
 * Sprites: one record per frame, a quad in the view plane. Pictures
 * (gfx.wad): as they are, index 255 transparent.
 *
 * VRAM: the texture pages the maps leave free (qtex.h QTEX_MAP_PAGES and up).
 * Every image (skins, sprite sheets, pictures) is placed in shelves by
 * decreasing height, then one 256-colour CLUT.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "qcook.h"
#include "../../formats/qtex.h"
#include "../../formats/qworld.h"
#include "../../formats/qgame.h"
#include "../../formats/qstream.h"
#include "lod.h"

static const float anorms[QG_NORMALS][3] = {
#include "anorms.h"
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
static int32_t  rdi(const uint8_t *p)  { return (int32_t) rd32(p); }
static float    rdf(const uint8_t *p)  { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

/* ----------------------------------------------------------------- buf -- */

typedef struct { uint8_t *p; size_t len, cap; } gbuf;

static void gput(gbuf *b, const void *d, size_t n)
{
	if (b->len + n > b->cap) {
		b->cap = (b->len + n) * 2 + 1024;
		b->p = realloc(b->p, b->cap);
		if (!b->p) die("out of memory");
	}
	memcpy(b->p + b->len, d, n);
	b->len += n;
}

static void galign(gbuf *b, size_t a)
{
	static const uint8_t z[32];
	while (b->len % a)
		gput(b, z, 1);
}

/* --------------------------------------------------------------- palette -- */

static const uint8_t *palette;   /* 768 bytes */

static int nearest_colour(int r, int g, int b)
{
	int best = 0;
	long bd = 1L << 40;
	for (int i = 0; i < 255; i++) {              /* never 255: transparent */
		long dr = palette[i * 3] - r, dg = palette[i * 3 + 1] - g, db = palette[i * 3 + 2] - b;
		long d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
		if (d < bd) { bd = d; best = i; }
	}
	return best;
}

/* ------------------------------------------------------------------ vram -- */

#define GP_PAGES (QTEX_PAGES - QTEX_MAP_PAGES)
#define PT 256

static uint8_t gpage[GP_PAGES][PT][PT];
static int page_rows[GP_PAGES];

/* an image to place: its pixels, then where it went. A textured polygon's
   image (a skin, a sprite sheet) gets a border of replicated edge texels
   in the page: the GPU's texture coordinates stray a texel past a
   triangle's edge, and a neighbour's texels showed there (blue pixels from
   the bolt's skin on the barrels). Pictures are drawn as sprites, texel
   for texel: no border. */
#define GP_BORDER 2
typedef struct {
	const char *what;
	int         w, h;
	uint8_t    *px;
	int         border;
	int         align;         /* its u a multiple of this (0, 1: any; the 2D
	                              gun's slot: 32 texels, the 16 halfwords of
	                              the GPU's fill) */
	int         page, u, v;    /* u, v: the image's own origin (past the border) */
} image;

#define MAX_IMAGES 512   /* (the stream's every model and its skin: the registered
                           game's monsters past 256) */
static image images[MAX_IMAGES];
static int   nimages;

static image *add_image(const char *what, int w, int h, int border)
{
	if (nimages >= MAX_IMAGES) die("too many images");
	if (w < 1 || h < 1 || w + 2 * border > PT || h + 2 * border > PT)
		die("%s: %dx%d does not fit a texture page", what, w, h);
	image *im = &images[nimages++];
	im->what = what;
	im->w = w;
	im->h = h;
	im->border = border;
	im->px = xcalloc((size_t) w * h, 1);
	im->page = -1;
	return im;
}

static int by_area(const void *a, const void *b)
{
	const image *const *x = a, *const *y = b;
	int xa = ((*x)->w + 2 * (*x)->border) * ((*x)->h + 2 * (*x)->border);
	int ya = ((*y)->w + 2 * (*y)->border) * ((*y)->h + 2 * (*y)->border);
	if (xa != ya) return ya - xa;
	return (*y)->w - (*x)->w;
}

/* The free space of a page as maximal rectangles (MaxRects): placing an
   image cuts every free rectangle it overlaps into the pieces left of,
   right of, above and below it, and the pieces another contains are
   dropped. An image goes where its shorter leftover side is the smallest
   (best short side fit), over all pages; images by decreasing area. */
typedef struct { int x, y, w, h; } rect;
#define MAX_FREE 2048
static rect freer[GP_PAGES][MAX_FREE];
static int  nfree[GP_PAGES];

static int contains(const rect *a, const rect *b)   /* a holds b */
{
	return b->x >= a->x && b->y >= a->y && b->x + b->w <= a->x + a->w && b->y + b->h <= a->y + a->h;
}

static void free_add(int p, rect r)
{
	if (r.w <= 0 || r.h <= 0) return;
	for (int i = 0; i < nfree[p]; i++)
		if (contains(&freer[p][i], &r)) return;
	for (int i = 0; i < nfree[p]; )
		if (contains(&r, &freer[p][i])) freer[p][i] = freer[p][--nfree[p]];
		else i++;
	if (nfree[p] >= MAX_FREE) die("MaxRects: too many free rectangles");
	freer[p][nfree[p]++] = r;
}

static void place_images(void)
{
	image *order[MAX_IMAGES];
	for (int i = 0; i < nimages; i++) order[i] = &images[i];
	qsort(order, (size_t) nimages, sizeof *order, by_area);
	for (int p = 0; p < GP_PAGES; p++) {
		freer[p][0] = (rect) { 0, 0, PT, PT };
		nfree[p] = 1;
	}
	long area = 0, total = 0;
	for (int n = 0; n < nimages; n++) {
		total += (long) (order[n]->w + 2 * order[n]->border) * (order[n]->h + 2 * order[n]->border);
		if (getenv("QCOOK_IMAGES"))
			printf("image %-24s %3dx%3d +%d\n", order[n]->what, order[n]->w, order[n]->h, order[n]->border);
	}
	for (int n = 0; n < nimages; n++) {
		image *im = order[n];
		int b = im->border, bw = im->w + 2 * b, bh = im->h + 2 * b;
		int bp = -1, bi = -1, bs = 1 << 30, bl = 1 << 30;
		for (int p = 0; p < GP_PAGES; p++)
			for (int i = 0; i < nfree[p]; i++) {
				const rect *f = &freer[p][i];
				int ex = im->align > 1 ? (im->align - (f->x + b) % im->align) % im->align : 0;
				if (bw + ex > f->w || bh > f->h) continue;
				int ds = f->w - bw - ex, dl = f->h - bh;
				if (ds > dl) { int t = ds; ds = dl; dl = t; }
				if (ds < bs || (ds == bs && dl < bl)) { bs = ds; bl = dl; bp = p; bi = i; }
			}
		if (bp < 0)
			die("%s (%dx%d): the game's %d VRAM pages are full (%ld K texels placed of %ld K)",
			    im->what, bw, bh, GP_PAGES, area / 1024, total / 1024);
		rect at = { freer[bp][bi].x, freer[bp][bi].y, bw, bh };
		if (im->align > 1)
			at.x += (im->align - (at.x + b) % im->align) % im->align;
		im->page = bp; im->u = at.x + b; im->v = at.y + b;
		area += (long) bw * bh;
		/* split the free rectangles it overlaps */
		rect old[MAX_FREE];
		int nold = nfree[bp];
		memcpy(old, freer[bp], (size_t) nold * sizeof *old);
		nfree[bp] = 0;
		for (int i = 0; i < nold; i++) {
			rect f = old[i];
			if (at.x >= f.x + f.w || at.x + at.w <= f.x || at.y >= f.y + f.h || at.y + at.h <= f.y) {
				free_add(bp, f);
				continue;
			}
			free_add(bp, (rect) { f.x, f.y, at.x - f.x, f.h });                                   /* left */
			free_add(bp, (rect) { at.x + at.w, f.y, f.x + f.w - (at.x + at.w), f.h });             /* right */
			free_add(bp, (rect) { f.x, f.y, f.w, at.y - f.y });                                   /* above */
			free_add(bp, (rect) { f.x, at.y + at.h, f.w, f.y + f.h - (at.y + at.h) });             /* below */
		}
		if (at.y + bh > page_rows[bp])
			page_rows[bp] = at.y + bh;
		for (int y = 0; y < bh; y++)                /* the border: the edge texels again */
			for (int x = 0; x < bw; x++) {
				int sx = x - b, sy = y - b;
				if (sx < 0) sx = 0;
				if (sx >= im->w) sx = im->w - 1;
				if (sy < 0) sy = 0;
				if (sy >= im->h) sy = im->h - 1;
				gpage[bp][at.y + y][at.x + x] = im->px[(size_t) sy * im->w + sx];
			}
	}
	printf("qcook: %d images, %ld K texels of %d K in the game's pages\n", nimages, area / 1024,
	       GP_PAGES * PT * PT / 1024);
}

static uint16_t page_tpage(int p)   /* 8bpp, no semi-transparency */
{
	int gp = QTEX_MAP_PAGES + p;
	int vx = QTEX_PAGE_X0 + (gp % QTEX_PAGE_COLS) * (PT / 2), vy = (gp / QTEX_PAGE_COLS) * PT;
	return (uint16_t) (((vx & 0x3ff) >> 6) | ((vy >> 8) << 4) | (1 << 7));
}

/* ------------------------------------------------------------------- mdl -- */

typedef struct { uint8_t v[3], n; } tvert;

typedef struct {
	float   scale[3], origin[3];
	int     nverts, ntris, nframes, skinw, skinh, flags;
	const uint8_t *skin;
	int   (*st)[3];                 /* onseam, s, t                  */
	int   (*tris)[4];               /* facesfront, v0, v1, v2        */
	tvert **frames;                 /* [nframes][nverts]             */
	double *fine;                   /* a view model the cook cut (tess_build):
	                                   [nverts][nframes][3], Quake's steps, its
	                                   middles' exact; NULL: the frames' bytes */
	double (*stf)[2];               /* and its skin coordinates, exact (s, t) */
} mdl_t;

static int mdl_skin_index;      /* the skin mdl_parse keeps (a map's armor: 0 .. 2) */

static void mdl_parse(mdl_t *m, const uint8_t *d, uint32_t len, const char *name)
{
	if (len < 84 || memcmp(d, "IDPO", 4) || rdi(d + 4) != 6)
		die("%s: not a version 6 MDL", name);
	for (int k = 0; k < 3; k++) {
		m->scale[k]  = rdf(d + 8 + k * 4);
		m->origin[k] = rdf(d + 20 + k * 4);
	}
	int nskins = rdi(d + 48);
	if (mdl_skin_index >= nskins)
		die("%s: no skin %d", name, mdl_skin_index);
	m->fine    = NULL;
	m->stf     = NULL;
	m->skinw   = rdi(d + 52);
	m->skinh   = rdi(d + 56);
	m->nverts  = rdi(d + 60);
	m->ntris   = rdi(d + 64);
	int nfr    = rdi(d + 68);
	m->flags   = rdi(d + 76);
	if (nskins < 1 || m->nverts < 3 || m->ntris < 1 || nfr < 1 || m->skinw < 1 || m->skinh < 1)
		die("%s: bad counts", name);
	const uint8_t *p = d + 84, *end = d + len;
#define NEED(n) do { if ((size_t) (end - p) < (size_t) (n)) die("%s: truncated", name); } while (0)
	size_t sk = (size_t) m->skinw * m->skinh;
	for (int s = 0; s < nskins; s++) {
		NEED(4);
		int type = rdi(p);
		p += 4;
		if (type == 0) {
			NEED(sk);
			if (s == mdl_skin_index) m->skin = p;
			p += sk;
		} else {
			NEED(4);
			int nb = rdi(p);
			p += 4 + 4 * nb;
			NEED(sk * (size_t) nb);
			if (s == mdl_skin_index) m->skin = p;
			p += sk * (size_t) nb;
		}
	}
	NEED(12 * (size_t) m->nverts + 16 * (size_t) m->ntris);
	m->st = xmalloc((size_t) m->nverts * sizeof *m->st);
	for (int i = 0; i < m->nverts; i++, p += 12)
		for (int k = 0; k < 3; k++)
			m->st[i][k] = rdi(p + k * 4);
	m->tris = xmalloc((size_t) m->ntris * sizeof *m->tris);
	for (int i = 0; i < m->ntris; i++, p += 16)
		for (int k = 0; k < 4; k++) {
			m->tris[i][k] = rdi(p + k * 4);
			if (k && (m->tris[i][k] < 0 || m->tris[i][k] >= m->nverts))
				die("%s: triangle vertex out of range", name);
		}
	/* frames: groups are flattened in order */
	int cap = 16;
	m->frames = xmalloc((size_t) cap * sizeof *m->frames);
	m->nframes = 0;
	size_t fv = 4 * (size_t) m->nverts;
	for (int f = 0; f < nfr; f++) {
		NEED(4);
		int type = rdi(p);
		p += 4;
		int nb = 1;
		if (type) {
			NEED(4);
			nb = rdi(p);
			p += 4 + 8 + 4 * nb;    /* count, bboxmin/max, intervals */
		}
		for (int b = 0; b < nb; b++) {
			NEED(24 + fv);
			p += 24;                /* bboxmin, bboxmax, name */
			if (m->nframes == cap) {
				cap *= 2;
				m->frames = realloc(m->frames, (size_t) cap * sizeof *m->frames);
				if (!m->frames) die("out of memory");
			}
			m->frames[m->nframes++] = (tvert *) p;
			p += fv;
		}
	}
#undef NEED
}

/* ------------------------------------------------------------------- spr -- */

typedef struct { int ox, oy, w, h; const uint8_t *px; } sprframe;

typedef struct { int type, w, h, nframes; sprframe frames[32]; } spr_t;

static void spr_parse(spr_t *s, const uint8_t *d, uint32_t len, const char *name)
{
	if (len < 36 || memcmp(d, "IDSP", 4) || rdi(d + 4) != 1)
		die("%s: not a version 1 SPR", name);
	s->type = rdi(d + 8);
	s->w    = rdi(d + 16);
	s->h    = rdi(d + 20);
	int nfr = rdi(d + 24);
	s->nframes = 0;
	const uint8_t *p = d + 36, *end = d + len;
	for (int f = 0; f < nfr; f++) {
		if (end - p < 4) die("%s: truncated", name);
		int type = rdi(p);
		p += 4;
		int nb = 1;
		if (type) {
			nb = rdi(p);
			p += 4 + 4 * nb;
		}
		for (int b = 0; b < nb; b++) {
			if (end - p < 16) die("%s: truncated", name);
			if (s->nframes >= 32) die("%s: too many frames", name);
			sprframe *fr = &s->frames[s->nframes++];
			fr->ox = rdi(p);
			fr->oy = rdi(p + 4);
			fr->w  = rdi(p + 8);
			fr->h  = rdi(p + 12);
			p += 16;
			if (fr->w < 1 || fr->h < 1 || (size_t) (end - p) < (size_t) fr->w * fr->h)
				die("%s: bad frame", name);
			fr->px = p;
			p += (size_t) fr->w * fr->h;
		}
	}
}

/* ------------------------------------------------------------------- wad -- */

static const uint8_t *wad;
static uint32_t       wad_len;

/* a WAD2 lump by name: its data and type; qpic (0x42) data starts with w, h */
static const uint8_t *wad_lump(const char *name, uint32_t *size, int *type)
{
	if (wad_len < 12 || memcmp(wad, "WAD2", 4))
		die("gfx.wad: not a WAD2");
	uint32_t n = rd32(wad + 4), ofs = rd32(wad + 8);
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *e = wad + ofs + i * 32;
		if ((size_t) (e + 32 - wad) > wad_len) die("gfx.wad: bad directory");
		char nm[17];
		memcpy(nm, e + 16, 16);
		nm[16] = 0;
		if (!strcasecmp(nm, name)) {
			uint32_t fpos = rd32(e), dsize = rd32(e + 4);
			if ((uint64_t) fpos + dsize > wad_len) die("gfx.wad: %s out of range", name);
			*size = dsize;
			*type = e[12];
			return wad + fpos;
		}
	}
	die("gfx.wad: %s missing", name);
}

/* ---------------------------------------------------------------- models -- */

typedef struct {
	const char *file;
	int         id;
	int         view;      /* a view model: nearest-first, seen from the origin */
	int         flat;      /* QGF_FLAT */
	int         skin_div;  /* the skin's resolution: 1 / skin_div (a view model
	                          2: the screen never shows it larger; a
	                          projectile 4: small and far) */
	double      warp;      /* a view model: the affine error its triangles are
	                          cut under, px (tess_build; 0: not cut) */
	double      scale[3];  /* extra scale per axis (the lightning bolt is drawn
	                          twice as long: one bolt per 60 units, not 30) */
} model_spec;

static const model_spec MODELS[] = {
	{ "progs/v_axe.mdl",    QGM_V_AXE,    1, 0, 2, 0, { 1, 1, 1 } },
	{ "progs/v_shot.mdl",   QGM_V_SHOT,   1, 0, 2, 8, { 1, 1, 1 } },
	{ "progs/v_shot2.mdl",  QGM_V_SHOT2,  1, 0, 2, 8, { 1, 1, 1 } },
	{ "progs/v_nail.mdl",   QGM_V_NAIL,   1, 0, 2, 8, { 1, 1, 1 } },
	{ "progs/v_nail2.mdl",  QGM_V_NAIL2,  1, 0, 2, 8, { 1, 1, 1 } },
	{ "progs/v_rock.mdl",   QGM_V_ROCK,   1, 0, 2, 8, { 1, 1, 1 } },
	{ "progs/v_rock2.mdl",  QGM_V_ROCK2,  1, 0, 2, 8, { 1, 1, 1 } },
	{ "progs/v_light.mdl",  QGM_V_LIGHT,  1, 0, 2, 8, { 1, 1, 1 } },
	{ "progs/spike.mdl",    QGM_SPIKE,    0, 1, 4, 0, { 1, 1, 1 } },
	{ "progs/s_spike.mdl",  QGM_S_SPIKE,  0, 1, 4, 0, { 1, 1, 1 } },
	{ "progs/grenade.mdl",  QGM_GRENADE,  0, 1, 4, 0, { 1, 1, 1 } },
	{ "progs/missile.mdl",  QGM_MISSILE,  0, 0, 4, 0, { 1, 1, 1 } },
	{ "progs/bolt2.mdl",    QGM_BOLT,     0, 1, 4, 0, { 2, 1, 1 } },
	{ "progs/s_explod.spr", QGM_S_EXPLOD, 0, 1, 1, 0, { 1, 1, 1 } },
	{ "progs/s_bubble.spr", QGM_S_BUBBLE, 0, 1, 1, 0, { 1, 1, 1 } },
};
#define NUM_MODELS (int) (sizeof MODELS / sizeof MODELS[0])

typedef struct {
	const model_spec *sp;
	const uint8_t    *data;
	uint32_t          len;
	mdl_t             mdl;      /* a model */
	spr_t             spr;      /* or a sprite */
	image            *im;       /* the skin / the sprite sheet (a gun's: its atlas) */
	int               fx[32], fy[32];   /* a sprite's frames in its sheet     */
	mdl_t             vmesh;    /* a gun: its triangles as drawn (view_mesh)  */
	int             (*vuv)[3][2];   /* and their corners in its atlas, texels */
	int               vnear, vback, vhidden, vtris;   /* the culling's drops, of vtris */
} model_src;

static int gun_tex = QL_MEDIUM; /* the guns' skins: half (view_atlas; qcook game
                                   --textures high: Quake's where the screen
                                   spreads them, measured in §53, too big for the
                                   game's pages: not on a disc) */

static model_src srcs[QGM_COUNT];

static double mdl_wind(const mdl_t *m);
static void view_mesh(model_src *ms);
static void view_atlas(model_src *ms);
static void skin_reduce(const mdl_t *m, const uint8_t *cover, int d, uint8_t *out);

/* The texels a model's triangles show: whose corner is inside a triangle's
   skin coordinates (a back face's onseam vertices on the right half, as
   the renderer maps them; a cut model's middles exact), or with `edges`
   every one a triangle overlaps at all: the GPU's floor(u) takes one an
   edge crosses (a gun's atlas: the corner's rule left out the rocket
   launcher's muzzle rim, its half texels the beige of its neighbours;
   §53). sw x sh, 1 shown */
static uint8_t *skin_cover(const mdl_t *m, int edges)
{
	int sw = m->skinw, sh = m->skinh;
	uint8_t *cover = xcalloc((size_t) sw * sh, 1);
	for (int t = 0; t < m->ntris; t++) {
		double tx[3], ty[3];
		for (int k = 0; k < 3; k++) {
			int vi = m->tris[t][k + 1];
			tx[k] = (m->stf ? m->stf[vi][0] : m->st[vi][1]) + (!m->tris[t][0] && m->st[vi][0] ? sw / 2 : 0) + 0.5;
			ty[k] = (m->stf ? m->stf[vi][1] : m->st[vi][2]) + 0.5;
		}
		double area = (tx[1] - tx[0]) * (ty[2] - ty[0]) - (tx[2] - tx[0]) * (ty[1] - ty[0]);
		if (area == 0) continue;
		double sgn = area > 0 ? 1 : -1;
		int x0 = (int) floor(fmin(fmin(tx[0], tx[1]), tx[2])) - 1, x1 = (int) ceil(fmax(fmax(tx[0], tx[1]), tx[2])) + 1;
		int y0 = (int) floor(fmin(fmin(ty[0], ty[1]), ty[2])) - 1, y1 = (int) ceil(fmax(fmax(ty[0], ty[1]), ty[2])) + 1;
		for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < sh; y++)
			for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < sw; x++) {
				/* the texel's corner (its centre against the triangle shifted
				   by half a texel), or (edges) its square: its centre against
				   the triangle itself, the half square's reach along each
				   edge's normal added */
				double px = x + (edges ? 1 : 0.5), py = y + (edges ? 1 : 0.5), in = 1;
				for (int e = 0; e < 3 && in; e++) {           /* on the inner side of each edge */
					int a = e, b = (e + 1) % 3;
					double ex = tx[b] - tx[a], ey = ty[b] - ty[a], len = sqrt(ex * ex + ey * ey);
					if (len <= 0) len = 1;
					double dd = sgn * (ex * (py - ty[a]) - ey * (px - tx[a])) / len;
					if (edges ? dd + 0.5 * (fabs(ex) + fabs(ey)) / len < 0.01 : dd < -0.01)
						in = 0;
				}
				if (in) cover[y * sw + x] = 1;
			}
	}
	return cover;
}

/* parse the model and make its image (not placed yet) */
static void model_prepare(model_src *ms)
{
	const model_spec *sp = ms->sp;
	if (strstr(sp->file, ".spr")) {
		spr_t *s = &ms->spr;
		spr_parse(s, ms->data, ms->len, sp->file);
		/* the frames side by side, rows of as many as fit 256 texels */
		int per_row = (PT - 2 * GP_BORDER) / s->frames[0].w;
		if (per_row < 1) per_row = 1;
		int rows = (s->nframes + per_row - 1) / per_row;
		int cw = 0, ch = 0;
		for (int f = 0; f < s->nframes; f++) {
			if (s->frames[f].w > cw) cw = s->frames[f].w;
			if (s->frames[f].h > ch) ch = s->frames[f].h;
		}
		ms->im = add_image(sp->file, cw * (s->nframes < per_row ? s->nframes : per_row), ch * rows, GP_BORDER);
		for (int f = 0; f < s->nframes; f++) {
			ms->fx[f] = (f % per_row) * cw;
			ms->fy[f] = (f / per_row) * ch;
			for (int y = 0; y < s->frames[f].h; y++)
				memcpy(ms->im->px + (size_t) (ms->fy[f] + y) * ms->im->w + ms->fx[f],
				       s->frames[f].px + (size_t) y * s->frames[f].w, (size_t) s->frames[f].w);
		}
		return;
	}
	mdl_t *m = &ms->mdl;
	mdl_parse(m, ms->data, ms->len, sp->file);
	if (sp->view) {                              /* a gun: its triangles as drawn, and
		                                            an atlas of the texels they show */
		view_mesh(ms);
		view_atlas(ms);
		return;
	}
	/* The skin's used texels: the ones whose centre is inside a triangle's
	   texture coordinates (a back face's onseam vertices on the right
	   half, as the renderer maps them); no margin, since the skin's fill
	   begins right at the painted area's edge. The rest of a Quake skin is
	   a flat blue fill that no triangle samples at full resolution, but
	   that the reduction below would average into the edge texels: that
	   is where blue pixels on the barrels came from. */
	int sw = m->skinw, sh = m->skinh;
	uint8_t *cover = skin_cover(m, 0);
	int d = sp->skin_div;
	ms->im = add_image(sp->file, (sw + d - 1) / d, (sh + d - 1) / d, GP_BORDER);
	skin_reduce(m, cover, d, ms->im->px);
	free(cover);
}

/* The skin at 1 / d resolution ((w + d - 1) / d x (h + d - 1) / d into
   out): the used texels (cover) of each block averaged; a block with
   none takes its used neighbours' colour, so the GPU's stray texel next
   to an edge is never the blue */
static void skin_reduce(const mdl_t *m, const uint8_t *cover, int d, uint8_t *out)
{
	int sw = m->skinw, sh = m->skinh, w2 = (sw + d - 1) / d, h2 = (sh + d - 1) / d;
	uint8_t *done = xcalloc((size_t) w2 * h2, 1);
	int (*rgb)[3] = xcalloc((size_t) w2 * h2, sizeof *rgb);
	for (int y = 0; y < h2; y++)
		for (int x = 0; x < w2; x++) {
			int r = 0, g = 0, b = 0, n = 0;
			for (int dy = 0; dy < d; dy++)
				for (int dx = 0; dx < d; dx++) {
					int sx = d * x + dx, sy = d * y + dy;
					if (sx >= sw || sy >= sh || !cover[sy * sw + sx]) continue;
					int c = m->skin[sy * sw + sx];
					r += palette[c * 3]; g += palette[c * 3 + 1]; b += palette[c * 3 + 2];
					n++;
				}
			if (n) {
				rgb[y * w2 + x][0] = r / n; rgb[y * w2 + x][1] = g / n; rgb[y * w2 + x][2] = b / n;
				done[y * w2 + x] = 1;
			}
		}
	for (int pass = 0; pass < 4; pass++) {
		uint8_t *now = xmalloc((size_t) w2 * h2);
		memcpy(now, done, (size_t) w2 * h2);
		for (int y = 0; y < h2; y++)
			for (int x = 0; x < w2; x++) {
				if (now[y * w2 + x]) continue;
				int r = 0, g = 0, b = 0, n = 0;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						int nx = x + dx, ny = y + dy;
						if (nx < 0 || ny < 0 || nx >= w2 || ny >= h2 || !now[ny * w2 + nx]) continue;
						r += rgb[ny * w2 + nx][0]; g += rgb[ny * w2 + nx][1]; b += rgb[ny * w2 + nx][2];
						n++;
					}
				if (n) {
					rgb[y * w2 + x][0] = r / n; rgb[y * w2 + x][1] = g / n; rgb[y * w2 + x][2] = b / n;
					done[y * w2 + x] = 1;
				}
			}
		free(now);
	}
	for (int i = 0; i < w2 * h2; i++)
		out[i] = done[i] ? (uint8_t) nearest_colour(rgb[i][0], rgb[i][1], rgb[i][2])
		                 : (uint8_t) nearest_colour(palette[m->skin[0] * 3], palette[m->skin[0] * 3 + 1],
		                                            palette[m->skin[0] * 3 + 2]);
	free(done);
	free(rgb);
}

#define REC_MAX_TRIS 48          /* 48 x 40 B + 8 < QW_MAX_REC_PKT */

typedef struct {
	int    nv, nt;
	int    mv[QW_MAX_VERTS];     /* slot -> model vertex */
	qw_tri t[REC_MAX_TRIS];
} mrec;

static int   *slots;            /* pool slot -> model vertex */
static int    nslots, slots_cap;
static int   *slot_of;          /* model vertex -> its pool slot, -1: none yet (one
                                   slot a vertex: the records share the pool's
                                   entries, as the world's do) */

static int rec_slot(mrec *r, int vi)
{
	for (int i = 0; i < r->nv; i++)
		if (r->mv[i] == vi)
			return i;
	if (r->nv >= QW_MAX_VERTS)
		return -1;
	r->mv[r->nv] = vi;
	return r->nv++;
}

static int rec_has(const mrec *r, int vi)
{
	for (int i = 0; i < r->nv; i++)
		if (r->mv[i] == vi)
			return 1;
	return 0;
}

/* a model vertex of frame f in vertex units */
/* the plane normal of a triangle, (v1 - v0) x (v2 - v0) */
static void tri_normal(int16_t v[3][3], double n[3])
{
	double a[3], b[3];
	for (int k = 0; k < 3; k++) {
		a[k] = v[1][k] - v[0][k];
		b[k] = v[2][k] - v[0][k];
	}
	n[0] = a[1] * b[2] - a[2] * b[1];
	n[1] = a[2] * b[0] - a[0] * b[2];
	n[2] = a[0] * b[1] - a[1] * b[0];
}

static void mdl_vertex(const mdl_t *m, int f, int vi, const double extra[3], int16_t out[3])
{
	const tvert *tv = &m->frames[f][vi];
	for (int k = 0; k < 3; k++) {
		double v = (tv->v[k] * m->scale[k] + m->origin[k]) * extra[k] * QW_VSCALE;
		long l = lround(v);
		if (l < -32768 || l > 32767) die("model vertex out of int16 range");
		out[k] = (int16_t) l;
	}
}

/* vertex vi of frame f in Quake's steps (a cut model's middles exact), and
   in world units, the model's axes */
static double vstep(const mdl_t *m, int f, int vi, int k)
{
	return m->fine ? m->fine[((size_t) vi * m->nframes + f) * 3 + k] : m->frames[f][vi].v[k];
}

static void vpos(const mdl_t *m, int f, int vi, double p[3])
{
	for (int k = 0; k < 3; k++)
		p[k] = vstep(m, f, vi, k) * m->scale[k] + m->origin[k];
}

/* flush record r: header + indices (global slots) + light bytes + tris */
static int view_records;   /* a view model's: their sphere never culls them */

static void mrec_flush(gbuf *geo, mrec *r, const mdl_t *m, const double extra[3], int *nrec)
{
	if (!r->nt)
		return;
	qw_record h;
	memset(&h, 0, sizeof h);
	h.num_verts = (uint8_t) r->nv;
	h.num_tris  = (uint8_t) r->nt;
	int nidx    = (r->nv + 2) / 3 * 3;
	int ibytes  = (nidx * 2 + 3) & ~3;
	int cbytes  = (r->nv + 3) & ~3;
	h.polys     = (uint16_t) (sizeof h + ibytes + cbytes);
	h.size      = (uint16_t) (h.polys + r->nt * sizeof(qw_tri));

	/* bounding sphere over every frame */
	long lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 };
	for (int f = 0; f < m->nframes; f++)
		for (int i = 0; i < r->nv; i++) {
			int16_t v[3];
			mdl_vertex(m, f, r->mv[i], extra, v);
			for (int k = 0; k < 3; k++) {
				if (v[k] < lo[k]) lo[k] = v[k];
				if (v[k] > hi[k]) hi[k] = v[k];
			}
		}
	for (int k = 0; k < 3; k++)
		h.sc[k] = (int16_t) ((lo[k] + hi[k]) / 2);
	double rad = 0;
	for (int f = 0; f < m->nframes; f++)
		for (int i = 0; i < r->nv; i++) {
			int16_t v[3];
			mdl_vertex(m, f, r->mv[i], extra, v);
			double dx = v[0] - h.sc[0], dy = v[1] - h.sc[1], dz = v[2] - h.sc[2];
			double d = sqrt(dx * dx + dy * dy + dz * dz);
			if (d > rad) rad = d;
		}
	h.sr = (uint16_t) ceil(rad + 1);
	if (view_records)
		h.sr = 4000;    /* the renderer scales a view model's x and y by 4 for
		                   the GTE's divide: a sphere this big is never culled
		                   and always takes the exact near test */
	gput(geo, &h, sizeof h);

	uint16_t idx[QW_MAX_VERTS + 3] = { 0 };
	for (int i = 0; i < r->nv; i++) {
		int vi = r->mv[i];
		if (slot_of[vi] < 0) {
			if (nslots == slots_cap) {
				slots_cap = slots_cap ? slots_cap * 2 : 256;
				slots = realloc(slots, (size_t) slots_cap * sizeof *slots);
				if (!slots) die("out of memory");
			}
			slots[nslots] = vi;
			slot_of[vi] = nslots++;
		}
		idx[i] = (uint16_t) slot_of[vi];
	}
	for (int i = r->nv; i < nidx; i++)
		idx[i] = idx[0];
	gput(geo, idx, (size_t) ibytes);
	uint8_t col[QW_MAX_VERTS + 4];
	memset(col, 128, sizeof col);
	gput(geo, col, (size_t) cbytes);
	gput(geo, r->t, (size_t) r->nt * sizeof(qw_tri));
	(*nrec)++;
	r->nv = r->nt = 0;
}

typedef struct { int tri; double d; } tri_order;

static int by_dist(const void *a, const void *b)
{
	const tri_order *x = a, *y = b;
	return x->d < y->d ? -1 : x->d > y->d;
}

/* The model level's triangles (formats/qlevels.h): at low up to half, at
   medium up to a quarter of those kept go (decim.c), as long as no surface
   or texture moves more than 3 units (low) or 1.5 (medium): 4 and 2 px at
   128 units. The view models stay Quake's at every level: seen from the
   eye, they lose nothing at an error of 2 to 4 px on the screen, and at 12
   px, for 1 to 3 % of a frame, their outline broke (the super shotgun's
   barrels) and a few triangles turned from the eye. A model of fewer than
   DECIM_MIN triangles stays as it is too. Returns the reduced model (its
   own triangle list) or NULL. */
#define DECIM_MIN 24
static int decim_tris, decim_left;

static int (*reduce_mdl(const mdl_t *m, const model_spec *sp, const uint8_t *keep, int model_level,
                        mdl_t *out))[4]
{
	int nk = 0;
	for (int t = 0; t < m->ntris; t++)
		nk += keep[t];
	decim_tris = decim_left = nk;
	if (model_level >= QL_HIGH || nk < DECIM_MIN || sp->view)
		return NULL;
	int (*tris)[4] = xmalloc((size_t) nk * sizeof *tris);
	int k = 0;
	for (int t = 0; t < m->ntris; t++)
		if (keep[t])
			memcpy(tris[k++], m->tris[t], sizeof tris[0]);
	double *pos = xmalloc((size_t) m->nframes * m->nverts * 3 * sizeof *pos);
	for (int f = 0; f < m->nframes; f++)
		for (int v = 0; v < m->nverts; v++)
			for (int c = 0; c < 3; c++)
				pos[((size_t) f * m->nverts + v) * 3 + c] =
					(m->frames[f][v].v[c] * m->scale[c] + m->origin[c]) * sp->scale[c];
	double  *uv   = xmalloc((size_t) m->nverts * 2 * sizeof *uv);
	uint8_t *seam = xmalloc((size_t) m->nverts);
	for (int v = 0; v < m->nverts; v++) {
		uv[v * 2]     = m->st[v][1];
		uv[v * 2 + 1] = m->st[v][2];
		seam[v]       = m->st[v][0] != 0;
	}
	dmesh dm = { m->nverts, m->nframes, pos, uv, 0.25, seam, m->skinw };
	int left = decimate(&dm, tris, nk, model_level == QL_LOW ? 0.5 : 0.75, model_level == QL_LOW ? 3.0 : 1.5);
	free(pos);
	free(uv);
	free(seam);
	*out        = *m;
	out->tris   = tris;
	out->ntris  = left;
	decim_left  = left;
	return tris;
}

/* ------------------------------------------------------ the guns by hand -- */

/* The guns rebuilt by hand (docs/porting-notes.md §52: the decimation broke them, so they are rebuilt):
   a gun's triangles written here in place of the file's, between vertices
   of the file kept as they are (their frames: the animation; their skin
   coordinates: the skin), plus the file's own triangles among sets of its
   vertices (the muzzle flash, a cap, a part too small to gain from it).
   A section joins two rings, a
   cross-section's corners in the same order at both ends, by quads; each
   quad is cut along its shorter diagonal. A triangle takes its winding
   and its skin half (facesfront) from the file's triangle in its plane
   that shares an edge with it, else its winding from its vertices'
   normals (the file's convention); its skin half from its vertices off
   the seam. The view model culling then drops what the eye never sees.
   The vertices are the file's, Quake's own model: no level reduces a
   gun. */
typedef struct { const uint8_t *a, *b; int n; } gun_section;   /* ring a to ring b, n corners */
typedef struct { int lo, hi; const uint8_t *v; int n; } gun_keep;   /* the vertices lo .. hi, or the
                                                                       n listed (v): the file's triangles
                                                                       among them kept as they are */
typedef struct {
	const char *file;
	const gun_keep *keep;
	int nkeep;
	const gun_section *sections;
	int nsections;
} gun_hand;

#define GUN_N(a) (int) (sizeof a / sizeof a[0])

/* v_shot.mdl: a hexagonal receiver (x -1.7 .. 5.3), its step to the
   barrel section (x 5.9: a taper from the receiver instead stretched the
   skin past its painted strip), the barrel section to the forearm (x
   11.5), the forearm a hexagon (x 11.8 .. 19.3, the file's 10-gon), the
   barrel's end beyond it (x 19.4 .. 21.3) in one piece; the flash, the
   vertices 0 .. 11, the file's */
static const uint8_t SHOT_R0[]  = { 13, 14, 12, 31, 45, 44 };   /* top left, top right, right, */
static const uint8_t SHOT_R1[]  = { 52, 62, 30, 29, 43, 42 };   /* bottom right, bottom left, left */
static const uint8_t SHOT_B0[]  = { 51, 61, 27, 26, 41, 40 };
static const uint8_t SHOT_R2[]  = { 50, 60, 28, 25, 39, 38 };
static const uint8_t SHOT_F0[]  = { 59, 58, 66, 23, 67, 36 };
static const uint8_t SHOT_F1[]  = { 57, 56, 32, 21, 17, 35 };
static const uint8_t SHOT_M0[]  = { 57, 56, 63, 48 };
static const uint8_t SHOT_M1[]  = { 47, 53, 20, 46 };
static const gun_section SHOT[] = {
	{ SHOT_R0, SHOT_R1, 6 }, { SHOT_R1, SHOT_B0, 6 }, { SHOT_B0, SHOT_R2, 6 }, { SHOT_R2, SHOT_F0, 6 },
	{ SHOT_F0, SHOT_F1, 6 }, { SHOT_M0, SHOT_M1, 4 } };

static const gun_keep SHOT_K[] = { { 0, 11, NULL, 0 } };

/* v_shot2.mdl: the stock's two hexagonal pieces (x 3.2 .. 8.2, 8.1 ..
   13.3) without their middle sections (on their ends' line within 0.15
   units, their skin following within a texel); their caps, the barrels
   and the flash, the file's */
static const uint8_t SHOT2_A0[] = { 29, 23, 24, 30, 36, 32 };
static const uint8_t SHOT2_A1[] = { 35, 34, 26, 21, 19, 20 };
static const uint8_t SHOT2_B0[] = { 47, 41, 42, 48, 54, 50 };
static const uint8_t SHOT2_B1[] = { 53, 52, 44, 39, 37, 38 };
static const gun_section SHOT2[] = { { SHOT2_A0, SHOT2_A1, 6 }, { SHOT2_B0, SHOT2_B1, 6 } };
static const gun_keep SHOT2_K[] = {
	{ 0, 18, NULL, 0 }, { 55, 85, NULL, 0 },             /* the barrels, the flash */
	{ 0, 0, SHOT2_A0, 6 }, { 0, 0, SHOT2_A1, 6 }, { 0, 0, SHOT2_B0, 6 }, { 0, 0, SHOT2_B1, 6 } };

/* v_rock.mdl, the grenade launcher: its hexagonal tube (x 5.4 .. 23.4)
   without its middle section (x 14.8: on its ends' line within 0.2
   units, its skin following); its caps and the flash, the file's */
static const uint8_t ROCK_A[]  = { 17, 14, 1, 0, 2, 15 };
static const uint8_t ROCK_C[]  = { 6, 10, 18, 12, 3, 7 };
static const uint8_t ROCK_CC[] = { 6, 10, 18, 12, 3, 7, 13 };
static const gun_section ROCK[] = { { ROCK_A, ROCK_C, 6 } };
static const gun_keep ROCK_K[] = { { 19, 30, NULL, 0 }, { 0, 0, ROCK_A, 6 }, { 0, 0, ROCK_CC, 7 } };

/* v_light.mdl: its box (x 3.0 .. 21.7) without its two middle sections
   (on its ends' line within 0.2 units, its skin following within a
   texel); its caps and every other piece, the file's */
static const uint8_t LIGHT_A[] = { 93, 98, 102, 103 };
static const uint8_t LIGHT_B[] = { 88, 92, 100, 91 };
static const gun_section LIGHT[] = { { LIGHT_A, LIGHT_B, 4 } };
static const gun_keep LIGHT_K[] = { { 0, 87, NULL, 0 }, { 0, 0, LIGHT_A, 4 }, { 0, 0, LIGHT_B, 4 } };

static const gun_hand GUNS[] = {
	{ "progs/v_shot.mdl",  SHOT_K,  GUN_N(SHOT_K),  SHOT,  GUN_N(SHOT) },
	{ "progs/v_shot2.mdl", SHOT2_K, GUN_N(SHOT2_K), SHOT2, GUN_N(SHOT2) },
	{ "progs/v_rock.mdl",  ROCK_K,  GUN_N(ROCK_K),  ROCK,  GUN_N(ROCK) },
	{ "progs/v_light.mdl", LIGHT_K, GUN_N(LIGHT_K), LIGHT, GUN_N(LIGHT) },
};

/* vertex v in keep set k */
static int gun_in(const gun_keep *k, int v)
{
	if (!k->v)
		return v >= k->lo && v <= k->hi;
	for (int i = 0; i < k->n; i++)
		if (k->v[i] == v)
			return 1;
	return 0;
}

static void unit(double n[3])
{
	double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (l > 0)
		for (int k = 0; k < 3; k++)
			n[k] /= l;
}

/* triangle (a, b, c)'s frame-0 unit normal (0: degenerate) */
static int hand_normal(const mdl_t *m, const model_spec *sp, int a, int b, int c, double n[3])
{
	int16_t v[3][3];
	mdl_vertex(m, 0, a, sp->scale, v[0]);
	mdl_vertex(m, 0, b, sp->scale, v[1]);
	mdl_vertex(m, 0, c, sp->scale, v[2]);
	tri_normal(v, n);
	if (n[0] == 0 && n[1] == 0 && n[2] == 0)
		return 0;
	unit(n);
	return 1;
}

/* the hand triangle (a, b, c) into t: wound and given its skin half */
static int hand_tri(const mdl_t *m, const model_spec *sp, double wind, int a, int b, int c, int t[4])
{
	double n[3];
	if (a == b || b == c || a == c || !hand_normal(m, sp, a, b, c, n))
		return 0;
	int ff = -1, flip = 0;
	double best = 0.5;
	for (int i = 0; i < m->ntris; i++) {           /* the file's triangle in its plane on an edge */
		const int *o = &m->tris[i][1];
		int shared = 0;
		for (int k = 0; k < 3; k++)
			shared += o[k] == a || o[k] == b || o[k] == c;
		double on[3];
		if (shared < 2 || !hand_normal(m, sp, o[0], o[1], o[2], on))
			continue;
		double d = n[0] * on[0] + n[1] * on[1] + n[2] * on[2];
		if (fabs(d) > best) {
			best = fabs(d);
			ff   = m->tris[i][0];
			flip = d < 0;
		}
	}
	if (ff < 0) {                                  /* none: its vertices' normals */
		double vn[3] = { 0, 0, 0 };
		const int vi[3] = { a, b, c };
		for (int k = 0; k < 3; k++) {
			int ni = m->frames[0][vi[k]].n;
			if (ni >= QG_NORMALS) ni = 0;
			for (int q = 0; q < 3; q++) vn[q] += anorms[ni][q];
		}
		double d = n[0] * vn[0] + n[1] * vn[1] + n[2] * vn[2];
		flip = (d < 0) != (wind < 0);
		ff   = 1;
	}
	/* the skin half: the one its vertices off the seam are on (a seam
	   vertex goes with them), so its skin triangle never straddles the
	   halves' gap; all on the seam: the file's triangle's above */
	int front = 0, back = 0;
	const int vs[3] = { a, b, c };
	for (int k = 0; k < 3; k++)
		if (!m->st[vs[k]][0]) {
			if (m->st[vs[k]][1] >= m->skinw / 2) back++;
			else front++;
		}
	if (front || back)
		ff = back > front ? 0 : 1;
	t[0] = ff;
	t[1] = a;
	t[2] = flip ? c : b;
	t[3] = flip ? b : c;
	return 1;
}

/* the file's winding convention, read off the model itself: the side its
   vertex normals point to, by majority (> 0: their side is the triangles'
   (v1 - v0) x (v2 - v0); Quake's files, all 76: < 0, the world's faces'
   the other way) */
static double mdl_wind(const mdl_t *m)
{
	static const double one[3] = { 1, 1, 1 };
	double wind = 0;
	for (int t = 0; t < m->ntris; t++) {
		int16_t v[3][3];
		double n[3] = { 0, 0, 0 }, c[3];
		for (int k = 0; k < 3; k++) {
			int vi = m->tris[t][k + 1];
			mdl_vertex(m, 0, vi, one, v[k]);
			int ni = m->frames[0][vi].n;
			if (ni >= QG_NORMALS) ni = 0;
			for (int a = 0; a < 3; a++) n[a] += anorms[ni][a];
		}
		tri_normal(v, c);
		double l = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		if (l > 0)
			wind += (c[0] * n[0] + c[1] * n[1] + c[2] * n[2]) / l;
	}
	return wind;
}

/* the gun's triangles by hand in place of the file's (m's copy in out): 1,
   or 0 for a model with none */
static int gun_hand_build(const mdl_t *m, const model_spec *sp, double wind, mdl_t *out)
{
	const gun_hand *g = NULL;
	for (size_t i = 0; i < sizeof GUNS / sizeof GUNS[0]; i++)
		if (!strcmp(GUNS[i].file, sp->file))
			g = &GUNS[i];
	if (!g || getenv("QCOOK_NOHAND"))             /* (QCOOK_NOHAND=1: the file's, an A / B) */
		return 0;
	int cap = m->ntris, n = 0, kept = 0;
	for (int s = 0; s < g->nsections; s++)
		cap += 2 * g->sections[s].n;
	int (*tris)[4] = xmalloc((size_t) cap * sizeof *tris);
	for (int i = 0; i < m->ntris; i++) {
		int in = 0;
		for (int s = 0; s < g->nkeep && !in; s++)
			in = gun_in(&g->keep[s], m->tris[i][1]) && gun_in(&g->keep[s], m->tris[i][2]) &&
			     gun_in(&g->keep[s], m->tris[i][3]);
		if (in) {
			memcpy(tris[n++], m->tris[i], sizeof tris[0]);
			kept++;
		}
	}
	for (int s = 0; s < g->nsections; s++) {
		const gun_section *sec = &g->sections[s];
		for (int i = 0; i < sec->n; i++) {
			int j = (i + 1) % sec->n;
			int a0 = sec->a[i], a1 = sec->a[j], b0 = sec->b[i], b1 = sec->b[j];
			for (int k = 0; k < 4; k++)
				if ((k < 2 ? (k ? a1 : a0) : (k == 2 ? b1 : b0)) >= m->nverts)
					die("%s: a hand-made section's vertex out of range", sp->file);
			int16_t p[4][3];                       /* the shorter diagonal */
			mdl_vertex(m, 0, a0, sp->scale, p[0]);
			mdl_vertex(m, 0, a1, sp->scale, p[1]);
			mdl_vertex(m, 0, b1, sp->scale, p[2]);
			mdl_vertex(m, 0, b0, sp->scale, p[3]);
			double d02 = 0, d13 = 0;
			for (int k = 0; k < 3; k++) {
				d02 += (double) (p[0][k] - p[2][k]) * (p[0][k] - p[2][k]);
				d13 += (double) (p[1][k] - p[3][k]) * (p[1][k] - p[3][k]);
			}
			if (d02 <= d13) {
				n += hand_tri(m, sp, wind, a0, a1, b1, tris[n]);
				n += hand_tri(m, sp, wind, a0, b1, b0, tris[n]);
			} else {
				n += hand_tri(m, sp, wind, a0, a1, b0, tris[n]);
				n += hand_tri(m, sp, wind, a1, b1, b0, tris[n]);
			}
		}
	}
	*out = *m;
	out->tris  = tris;
	out->ntris = n;
	printf("qcook: %s rebuilt by hand: %d triangles (%d of them the file's), the file had %d\n", sp->file, n,
	       kept, m->ntris);
	const char *dump = getenv("QCOOK_GUNS");      /* (the triangles, for a look: a directory) */
	if (dump) {
		char path[512];
		const char *base = strrchr(sp->file, '/');
		snprintf(path, sizeof path, "%s/%s.tris", dump, base ? base + 1 : sp->file);
		FILE *f = fopen(path, "w");
		if (f) {
			for (int i = 0; i < n; i++)
				fprintf(f, "%d %d %d %d\n", tris[i][0], tris[i][1], tris[i][2], tris[i][3]);
			fclose(f);
		}
	}
	return 1;
}

/* ------------------------------------------------- a view model's order -- */

/* A view model is drawn with no depth buffer, in its records' order (the
   renderer prepends each polygon to one bucket: the first written is drawn
   last), and only ever from about the eye, so its order is worked out here
   once, from where the eye sees it: in every frame, from every place the
   eye takes (at rest; the bob's forward reach, 0.4 x -2.56 .. 4; the
   full-screen viewport's gun, 2 units lower), every two triangles (clipped
   at the renderer's 5 units) whose pictures overlap must come nearer
   first. Those turned away from the eye too: the renderer draws them (it
   culls no model's triangle by its winding; the culling above dropped
   only those never facing it), so they must go under the ones facing it
   (the super shotgun's barrels' far sides came over their near ones). A
   pair's say is its overlap's area, frame 0 (the gun at rest) four times,
   the eye at rest twice, a tenth between two turned away (hidden behind
   the others but in a hole); the frames that disagree are outvoted. The centroid's distance (the order until
   then) put the nailgun's barrels over its blocks, the super shotgun's
   gap over a barrel, the rocket launcher's outside over its muzzle. */
#define VO_NEAR 5.0          /* the renderer's near clip for the view model     */
#define VO_UP   2.0          /* the status bar's viewport fudge (game.c)        */
#define VO_CX   160.0        /* the projection (render.c): the overlaps' areas */
#define VO_CY   96.0         /* in pixels                                      */
#define VO_PROJ 160.0

typedef struct { double x, y, iz; } vo_pt;

static int vo_project(const double p[3][3], vo_pt out[4])   /* eye-relative: x ahead, y left, z up */
{
	double c[4][3];
	int n = 0;
	for (int i = 0; i < 3; i++) {
		const double *a = p[i], *b = p[(i + 1) % 3];
		int ina = a[0] >= VO_NEAR, inb = b[0] >= VO_NEAR;
		if (ina)
			memcpy(c[n++], a, sizeof c[0]);
		if (ina != inb) {
			double f = (VO_NEAR - a[0]) / (b[0] - a[0]);
			for (int k = 0; k < 3; k++)
				c[n][k] = a[k] + f * (b[k] - a[k]);
			n++;
		}
	}
	for (int i = 0; i < n; i++) {
		out[i].x  = VO_CX - VO_PROJ * c[i][1] / c[i][0];
		out[i].y  = VO_CY - VO_PROJ * c[i][2] / c[i][0];
		out[i].iz = 1 / c[i][0];
	}
	return n;
}

/* convex a clipped by convex b (Sutherland-Hodgman): the overlap, its count */
static int vo_overlap(const vo_pt *a, int na, const vo_pt *b, int nb, vo_pt *out)
{
	vo_pt buf[2][16];
	int n = na, cur = 0;
	memcpy(buf[0], a, (size_t) na * sizeof *a);
	double area = 0;
	for (int i = 0; i < nb; i++)
		area += b[i].x * b[(i + 1) % nb].y - b[(i + 1) % nb].x * b[i].y;
	double s = area > 0 ? 1 : -1;
	for (int e = 0; e < nb && n; e++) {
		const vo_pt *p = &b[e], *q = &b[(e + 1) % nb];
		int m = 0;
		for (int i = 0; i < n; i++) {
			const vo_pt *u = &buf[cur][i], *v = &buf[cur][(i + 1) % n];
			double du = s * ((q->x - p->x) * (u->y - p->y) - (q->y - p->y) * (u->x - p->x));
			double dv = s * ((q->x - p->x) * (v->y - p->y) - (q->y - p->y) * (v->x - p->x));
			if (du >= 0 && m < 15)
				buf[!cur][m++] = *u;
			if ((du >= 0) != (dv >= 0) && m < 15) {
				double f = du / (du - dv);
				vo_pt w = { u->x + f * (v->x - u->x), u->y + f * (v->y - u->y), 0 };
				buf[!cur][m++] = w;
			}
		}
		cur = !cur;
		n = m;
	}
	memcpy(out, buf[cur], (size_t) n * sizeof *out);
	return n;
}

/* 1 / z at (x, y) on the plane of polygon a (n >= 3) */
static double vo_iz(const vo_pt *a, int n, double x, double y)
{
	int i1 = 1, i2 = 2;
	double d = (a[1].x - a[0].x) * (a[2].y - a[0].y) - (a[2].x - a[0].x) * (a[1].y - a[0].y);
	if (n > 3) {
		double d3 = (a[2].x - a[0].x) * (a[3].y - a[0].y) - (a[3].x - a[0].x) * (a[2].y - a[0].y);
		if (fabs(d3) > fabs(d)) { d = d3; i1 = 2; i2 = 3; }
	}
	if (fabs(d) < 1e-12)
		return a[0].iz;
	double w1 = ((x - a[0].x) * (a[i2].y - a[0].y) - (a[i2].x - a[0].x) * (y - a[0].y)) / d;
	double w2 = ((a[i1].x - a[0].x) * (y - a[0].y) - (x - a[0].x) * (a[i1].y - a[0].y)) / d;
	return a[0].iz + w1 * (a[i1].iz - a[0].iz) + w2 * (a[i2].iz - a[0].iz);
}

/* net[i * n + j] > 0: triangle i is written before j (in front of it where
   they overlap), by that much say */
static double *view_order(const mdl_t *m, const model_spec *sp, double wind, const uint8_t *drawn)
{
	static const double eyes[4][3] = { { 0, 0, -VO_UP }, { 1.0, 0, -VO_UP }, { -1.6, 0, -VO_UP }, { 0, 0, 0 } };
	int n = m->ntris;
	double *say = xcalloc((size_t) n * n, sizeof *say);
	vo_pt (*poly)[4] = xmalloc((size_t) n * sizeof *poly);
	int    *np  = xmalloc((size_t) n * sizeof *np);
	uint8_t *away = xmalloc((size_t) n);
	double (*box)[4] = xmalloc((size_t) n * sizeof *box);
	for (int f = 0; f < m->nframes; f++)
		for (int e = 0; e < 4; e++) {
			double w = (f == 0 ? 4 : 1) * (e == 0 ? 2 : 1);
			for (int t = 0; t < n; t++) {
				np[t] = 0;
				if (!drawn[t])
					continue;
				double p[3][3];
				for (int k = 0; k < 3; k++) {
					vpos(m, f, m->tris[t][k + 1], p[k]);
					for (int a = 0; a < 3; a++)
						p[k][a] = p[k][a] * sp->scale[a] - eyes[e][a];
				}
				double a[3], b[3], c[3];
				for (int k = 0; k < 3; k++) { a[k] = p[1][k] - p[0][k]; b[k] = p[2][k] - p[0][k]; }
				c[0] = a[1] * b[2] - a[2] * b[1];
				c[1] = a[2] * b[0] - a[0] * b[2];
				c[2] = a[0] * b[1] - a[1] * b[0];
				double d = -(c[0] * p[0][0] + c[1] * p[0][1] + c[2] * p[0][2]);   /* the eye at 0 */
				if (wind < 0) d = -d;
				away[t] = d <= 0;                      /* its back to the eye (drawn all the same) */
				np[t] = vo_project(p, poly[t]);
				if (np[t] < 3) {
					np[t] = 0;
					continue;
				}
				box[t][0] = box[t][2] = poly[t][0].x;
				box[t][1] = box[t][3] = poly[t][0].y;
				for (int k = 1; k < np[t]; k++) {
					box[t][0] = fmin(box[t][0], poly[t][k].x); box[t][2] = fmax(box[t][2], poly[t][k].x);
					box[t][1] = fmin(box[t][1], poly[t][k].y); box[t][3] = fmax(box[t][3], poly[t][k].y);
				}
			}
			for (int i = 0; i < n; i++) {
				if (!np[i]) continue;
				for (int j = i + 1; j < n; j++) {
					if (!np[j] || box[i][0] >= box[j][2] || box[j][0] >= box[i][2] ||
					    box[i][1] >= box[j][3] || box[j][1] >= box[i][3])
						continue;
					vo_pt o[16];
					int no = vo_overlap(poly[i], np[i], poly[j], np[j], o);
					if (no < 3)
						continue;
					double area = 0, cx = 0, cy = 0;
					for (int k = 0; k < no; k++) {
						area += o[k].x * o[(k + 1) % no].y - o[(k + 1) % no].x * o[k].y;
						cx += o[k].x / no;
						cy += o[k].y / no;
					}
					area = fabs(area) / 2;
					if (area < 0.05)                   /* (neighbours along an edge) */
						continue;
					double zi = vo_iz(poly[i], np[i], cx, cy), zj = vo_iz(poly[j], np[j], cx, cy);
					if (fabs(zi - zj) < 1e-9)
						continue;
					double ws = away[i] && away[j] ? w / 10 : w;
					if (zi > zj) say[(size_t) i * n + j] += ws * area;   /* i nearer */
					else         say[(size_t) j * n + i] += ws * area;
				}
			}
		}
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++) {
			double a = say[(size_t) i * n + j], b = say[(size_t) j * n + i];
			say[(size_t) i * n + j] = a > b ? a - b : 0;
			say[(size_t) j * n + i] = b > a ? b - a : 0;
		}
	free(poly);
	free(np);
	free(away);
	free(box);
	return say;
}

/* --------------------------------------------- a view model's affine cut -- */

/* The GPU maps a texture linearly over the screen: a triangle deep along
   the view shows its far end's texels too large and its near end's too
   small (the shotgun's barrel bent, the grenade launcher's band and the
   rocket launcher's rim smeared over their middles). A view model's edges
   are cut at their middles where, seen from the eye at rest in frame 0,
   the texture's displacement there passes the model's warp (px): the slow
   path's measure (engine/r_slow.c), L |dz| / (2 (za + zb)), L the edge's
   length on the screen, of its part in front of the renderer's near plane.
   A triangle is cut as the slow path cuts one (an edge: two, two: three,
   three: four), its pieces again. The decision is the edge's alone, so the
   two triangles on an edge cut it alike: no cracks. A middle is a vertex
   of its own in every frame: the middle of the ends' places (exact in the
   pools, Quake's steps x QG_VIEW_SUB), the normal nearest their normals'
   mean (the light), its skin coordinates the ends' mean (on the seam if
   both ends are; a seam end and one off it: the triangle's half, which
   every triangle on that edge shares). A piece behind the eye in every
   frame (x < 3 units, as the culling's) goes. */
#define TESS_MAX   4096          /* middles a model */
#define TESS_DEEP  12            /* (a bound the measure never reaches) */

typedef struct {
	mdl_t  m;                    /* the model as it is cut: its vertices grow   */
	int    cap;
	double warp;
	int  (*key)[3];              /* an edge (a < b) and its middle              */
	int    nkey;
	int  (*tris)[4];
	int    ntris, tcap;
} tess_t;

/* edge (a, b)'s affine displacement at its middle, frame 0, the eye at rest */
static double tess_err(const mdl_t *m, int a, int b)
{
	double pa[3], pb[3];
	vpos(m, 0, a, pa);
	vpos(m, 0, b, pb);
	pa[2] += VO_UP;                              /* (the eye 2 units under the gun) */
	pb[2] += VO_UP;
	if (pa[0] < VO_NEAR && pb[0] < VO_NEAR)
		return 0;
	if (pa[0] < VO_NEAR || pb[0] < VO_NEAR) {    /* its part in front of the near plane */
		double *n = pa[0] < VO_NEAR ? pa : pb, *f = n == pa ? pb : pa;
		double t = (f[0] - VO_NEAR) / (f[0] - n[0]);
		for (int k = 0; k < 3; k++)
			n[k] = f[k] + t * (n[k] - f[k]);
	}
	double ax = VO_CX - VO_PROJ * pa[1] / pa[0], ay = VO_CY - VO_PROJ * pa[2] / pa[0];
	double bx = VO_CX - VO_PROJ * pb[1] / pb[0], by = VO_CY - VO_PROJ * pb[2] / pb[0];
	double len = fabs(ax - bx) + fabs(ay - by);
	return len * fabs(pa[0] - pb[0]) / (2 * (pa[0] + pb[0]));
}

static int tess_mid(tess_t *c, int a, int b, int ff, const char *file)
{
	if (a > b) { int t = a; a = b; b = t; }
	for (int i = 0; i < c->nkey; i++)
		if (c->key[i][0] == a && c->key[i][1] == b)
			return c->key[i][2];
	mdl_t *o = &c->m;
	if (o->nverts >= c->cap)
		die("%s: more than %d middles", file, TESS_MAX);
	int v = o->nverts++;
	for (int f = 0; f < o->nframes; f++) {
		double nv[3];
		for (int k = 0; k < 3; k++) {
			double x = (vstep(o, f, a, k) + vstep(o, f, b, k)) / 2;
			o->fine[((size_t) v * o->nframes + f) * 3 + k] = x;
			o->frames[f][v].v[k] = (uint8_t) lround(x);
			int na = o->frames[f][a].n < QG_NORMALS ? o->frames[f][a].n : 0;
			int nb = o->frames[f][b].n < QG_NORMALS ? o->frames[f][b].n : 0;
			nv[k] = anorms[na][k] + anorms[nb][k];
		}
		int best = 0;
		double bd = -1e9;
		for (int n = 0; n < QG_NORMALS; n++) {
			double d = anorms[n][0] * nv[0] + anorms[n][1] * nv[1] + anorms[n][2] * nv[2];
			if (d > bd) { bd = d; best = n; }
		}
		o->frames[f][v].n = (uint8_t) best;
	}
	double sa = o->stf[a][0], sb = o->stf[b][0];
	int seam = o->st[a][0] && o->st[b][0];
	if (!seam) {                                 /* a seam end takes the triangle's half */
		if (o->st[a][0] && !ff) sa += o->skinw / 2;
		if (o->st[b][0] && !ff) sb += o->skinw / 2;
	}
	o->stf[v][0] = (sa + sb) / 2;
	o->stf[v][1] = (o->stf[a][1] + o->stf[b][1]) / 2;
	o->st[v][0] = seam;
	o->st[v][1] = (int) lround(o->stf[v][0]);
	o->st[v][2] = (int) lround(o->stf[v][1]);
	if (c->nkey >= TESS_MAX)
		die("%s: more than %d middles", file, TESS_MAX);
	c->key[c->nkey][0] = a;
	c->key[c->nkey][1] = b;
	c->key[c->nkey][2] = v;
	c->nkey++;
	return v;
}

static void tess_tri(tess_t *c, int ff, int v0, int v1, int v2, int depth, const char *file)
{
	int v[3] = { v0, v1, v2 }, cut[3], n = 0;
	if (depth > TESS_DEEP)
		die("%s: an edge cut more than %d times", file, TESS_DEEP);
	for (int i = 0; i < 3; i++) {
		cut[i] = tess_err(&c->m, v[i], v[(i + 1) % 3]) > c->warp;
		n += cut[i];
	}
	if (!n) {
		int behind = 1;                          /* behind the eye in every frame: gone */
		for (int f = 0; f < c->m.nframes && behind; f++)
			for (int k = 0; k < 3; k++) {
				double p[3];
				vpos(&c->m, f, v[k], p);
				if (p[0] >= 3)
					behind = 0;
			}
		if (behind)
			return;
		if (c->ntris == c->tcap) {
			c->tcap *= 2;
			c->tris = realloc(c->tris, (size_t) c->tcap * sizeof *c->tris);
			if (!c->tris) die("out of memory");
		}
		c->tris[c->ntris][0] = ff;
		c->tris[c->ntris][1] = v0;
		c->tris[c->ntris][2] = v1;
		c->tris[c->ntris][3] = v2;
		c->ntris++;
		return;
	}
	int mid[3];
	for (int i = 0; i < 3; i++)
		mid[i] = cut[i] ? tess_mid(c, v[i], v[(i + 1) % 3], ff, file) : -1;
	if (n == 3) {
		tess_tri(c, ff, v[0], mid[0], mid[2], depth + 1, file);
		tess_tri(c, ff, mid[0], v[1], mid[1], depth + 1, file);
		tess_tri(c, ff, mid[2], mid[1], v[2], depth + 1, file);
		tess_tri(c, ff, mid[0], mid[1], mid[2], depth + 1, file);
	} else if (n == 1) {                         /* edge i cut */
		int i = cut[0] ? 0 : cut[1] ? 1 : 2, j = (i + 1) % 3, k = (i + 2) % 3;
		tess_tri(c, ff, v[i], mid[i], v[k], depth + 1, file);
		tess_tri(c, ff, mid[i], v[j], v[k], depth + 1, file);
	} else {                                     /* edge i whole */
		int i = !cut[0] ? 0 : !cut[1] ? 1 : 2, j = (i + 1) % 3, k = (i + 2) % 3;
		tess_tri(c, ff, v[i], v[j], mid[k], depth + 1, file);
		tess_tri(c, ff, v[j], mid[j], mid[k], depth + 1, file);
		tess_tri(c, ff, mid[j], v[k], mid[k], depth + 1, file);
	}
}

/* A flat quad (two triangles (v0 v1 v2), (v0 v2 v3) of one plane and one
   skin half) cut as a tube's section is: two opposite edges cut, by a
   ring between their middles, into two quads, again on each; else its two
   triangles as triangles are. A barrel's long quad along the view, cut as
   two triangles, became six, its diagonal's middle too: the cut put back
   far more than a ring per level (the grenade launcher's tube without its
   middle section came out longer than with it). */
static void tess_quad(tess_t *c, int ff, int v0, int v1, int v2, int v3, int depth, const char *file)
{
	int v[4] = { v0, v1, v2, v3 }, cut[4];
	if (depth > TESS_DEEP)
		die("%s: an edge cut more than %d times", file, TESS_DEEP);
	for (int i = 0; i < 4; i++)
		cut[i] = tess_err(&c->m, v[i], v[(i + 1) % 4]) > c->warp;
	if (cut[0] && cut[2] && !cut[1] && !cut[3]) {
		int m01 = tess_mid(c, v0, v1, ff, file), m23 = tess_mid(c, v2, v3, ff, file);
		tess_quad(c, ff, v0, m01, m23, v3, depth + 1, file);
		tess_quad(c, ff, m01, v1, v2, m23, depth + 1, file);
	} else if (cut[1] && cut[3] && !cut[0] && !cut[2]) {
		int m12 = tess_mid(c, v1, v2, ff, file), m30 = tess_mid(c, v3, v0, ff, file);
		tess_quad(c, ff, v0, v1, m12, m30, depth + 1, file);
		tess_quad(c, ff, m30, m12, v2, v3, depth + 1, file);
	} else {
		tess_tri(c, ff, v0, v1, v2, depth, file);
		tess_tri(c, ff, v0, v2, v3, depth, file);
	}
}

/* triangle t's unit normal, frame 0 (0: degenerate) */
static int tess_normal(const mdl_t *m, int t, double n[3])
{
	double p[3][3], a[3], b[3];
	for (int k = 0; k < 3; k++)
		vpos(m, 0, m->tris[t][k + 1], p[k]);
	for (int k = 0; k < 3; k++) { a[k] = p[1][k] - p[0][k]; b[k] = p[2][k] - p[0][k]; }
	n[0] = a[1] * b[2] - a[2] * b[1];
	n[1] = a[2] * b[0] - a[0] * b[2];
	n[2] = a[0] * b[1] - a[1] * b[0];
	double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (l <= 0)
		return 0;
	for (int k = 0; k < 3; k++)
		n[k] /= l;
	return 1;
}

/* m's triangles kept (keep) cut into out: 1, or 0 when nothing was cut */
static int tess_build(const mdl_t *m, const model_spec *sp, const uint8_t *keep, mdl_t *out)
{
	tess_t c;
	memset(&c, 0, sizeof c);
	c.cap  = m->nverts + TESS_MAX;
	c.warp = sp->warp;
	c.m    = *m;
	c.m.fine   = xmalloc((size_t) c.cap * m->nframes * 3 * sizeof *c.m.fine);
	c.m.stf    = xmalloc((size_t) c.cap * sizeof *c.m.stf);
	c.m.st     = xmalloc((size_t) c.cap * sizeof *c.m.st);
	c.m.frames = xmalloc((size_t) m->nframes * sizeof *c.m.frames);
	for (int f = 0; f < m->nframes; f++) {
		c.m.frames[f] = xmalloc((size_t) c.cap * sizeof **c.m.frames);
		memcpy(c.m.frames[f], m->frames[f], (size_t) m->nverts * sizeof **c.m.frames);
	}
	for (int v = 0; v < m->nverts; v++) {
		for (int f = 0; f < m->nframes; f++)
			for (int k = 0; k < 3; k++)
				c.m.fine[((size_t) v * m->nframes + f) * 3 + k] = vstep(m, f, v, k);
		memcpy(c.m.st[v], m->st[v], sizeof c.m.st[v]);
		c.m.stf[v][0] = m->stf ? m->stf[v][0] : m->st[v][1];
		c.m.stf[v][1] = m->stf ? m->stf[v][1] : m->st[v][2];
	}
	c.key  = xmalloc((size_t) TESS_MAX * sizeof *c.key);
	c.tcap = 2 * m->ntris + 16;
	c.tris = xmalloc((size_t) c.tcap * sizeof *c.tris);
	/* the flat quads: a triangle and its neighbour across its longest
	   edge, in its plane, of its skin half, the edge's ends shared the
	   other way round (one winding) */
	int kept = 0, nquads = 0;
	int *pair = xmalloc((size_t) m->ntris * sizeof *pair);
	for (int t = 0; t < m->ntris; t++)
		pair[t] = -1;
	for (int t = 0; t < m->ntris; t++) {
		if (!keep[t] || pair[t] >= 0)
			continue;
		double nt[3];
		if (!tess_normal(m, t, nt))
			continue;
		int best = -1, bk = -1;
		double blen = 0;
		for (int k = 0; k < 3; k++) {                /* edge (k, k+1) of t */
			int a = m->tris[t][k + 1], b = m->tris[t][(k + 1) % 3 + 1];
			double pa[3], pb[3];
			vpos(m, 0, a, pa);
			vpos(m, 0, b, pb);
			double len = (pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) +
			             (pa[2] - pb[2]) * (pa[2] - pb[2]);
			for (int u = 0; u < m->ntris; u++) {
				if (u == t || !keep[u] || pair[u] >= 0 || m->tris[u][0] != m->tris[t][0])
					continue;
				int has = 0;
				for (int j = 0; j < 3; j++)              /* (b, a) in u */
					has |= m->tris[u][j + 1] == b && m->tris[u][(j + 1) % 3 + 1] == a;
				double nu[3];
				if (!has || !tess_normal(m, u, nu) || nt[0] * nu[0] + nt[1] * nu[1] + nt[2] * nu[2] < 0.995)
					continue;
				if (len > blen) { blen = len; best = u; bk = k; }
			}
		}
		if (best < 0)
			continue;
		pair[t] = best;
		pair[best] = t;
		/* the quad from t's edge bk: t = (a, b, x), the neighbour's third y:
		   (b, x, a, y) wound as t */
		int a = m->tris[t][bk + 1], b = m->tris[t][(bk + 1) % 3 + 1], x = m->tris[t][(bk + 2) % 3 + 1], y = -1;
		for (int j = 0; j < 3; j++)
			if (m->tris[best][j + 1] != a && m->tris[best][j + 1] != b)
				y = m->tris[best][j + 1];
		kept += 2;
		nquads++;
		tess_quad(&c, m->tris[t][0], b, x, a, y, 0, sp->file);
	}
	for (int t = 0; t < m->ntris; t++)
		if (keep[t] && pair[t] < 0) {
			kept++;
			tess_tri(&c, m->tris[t][0], m->tris[t][1], m->tris[t][2], m->tris[t][3], 0, sp->file);
		}
	free(pair);
	free(c.key);
	if (!c.nkey && c.ntris == kept) {
		free(c.tris);
		return 0;
	}
	*out = c.m;
	out->tris  = c.tris;
	out->ntris = c.ntris;
	printf("qcook: %s cut under %.0f px of affine error: %d triangles (%d flat quads) -> %d, %d middles\n",
	       sp->file, sp->warp, kept, nquads, c.ntris, c.nkey);
	return 1;
}

/* ------------------------------------------------- a gun's mesh and atlas -- */

/* A view model is only ever seen from the eye, which sits about the
   model's origin (Quake's view.c: the gun's origin is the view's plus 0.4
   x bob forward and bob up, the view size's 2-unit fudge; the bob runs
   -2.56 .. 4): a triangle behind the near plane in every frame (x < 3
   units: the renderer clips at 5) is never drawn, nor is one whose plane
   keeps every eye position on its back in every frame (Quake's alias
   drawer skips a triangle wound backwards on screen, d_polyse.c
   D_DrawNonSubdiv, zero-area ones included; so does the renderer since
   §53). Both are dropped (keep[t] 0). */
static void view_cull(const mdl_t *m, const model_spec *sp, double wind, uint8_t *keep, int *dnear, int *dback)
{
	static const double eye[8][3] = {
		{ -2.5, -1, -7 }, { 2, -1, -7 }, { -2.5, 1, -7 }, { 2, 1, -7 },
		{ -2.5, -1, 3.5 }, { 2, -1, 3.5 }, { -2.5, 1, 3.5 }, { 2, 1, 3.5 } };
	*dnear = *dback = 0;
	for (int t = 0; t < m->ntris; t++) {
		keep[t] = 1;
		int behind = 1, back = 1;
		for (int f = 0; f < m->nframes && (behind || back); f++) {
			int16_t v[3][3];
			double n[3];
			for (int k = 0; k < 3; k++) {
				mdl_vertex(m, f, m->tris[t][k + 1], sp->scale, v[k]);
				if (v[k][0] >= 3 * QW_VSCALE)
					behind = 0;
			}
			tri_normal(v, n);
			for (int e = 0; e < 8; e++) {
				double d = 0;
				for (int a = 0; a < 3; a++)
					d += n[a] * (eye[e][a] * QW_VSCALE - v[0][a]);
				if (wind < 0) d = -d;
				if (d > 0)                      /* this eye position sees its front */
					back = 0;
			}
		}
		if (behind) { keep[t] = 0; (*dnear)++; }
		else if (back) { keep[t] = 0; (*dback)++; }
	}
}

/* A gun's triangles the eye never sees for others in front of them: in
   every frame, from every place the eye takes (the bob's forward reach in
   five steps, -1.0 .. 1.6 units, under the status bar's viewport and the
   full screen's), at twice the screen's resolution and past its edges, a
   depth buffer of the triangles facing the eye (clipped at the renderer's
   5 units: the near end too) keeps none of their samples. They go
   (keep[t] 0): drawn first, under the others, they cost the renderer and
   showed nothing (Quake's guns hide parts in one another: the nailgun's
   boxes in its body, the axe's handle in its head). Their count. */
#define OC_SS 2
#define OC_X0 (-160)                             /* the buffer reaches past the screen */
#define OC_Y0 (-120)
#define OC_W  (640 * OC_SS)
#define OC_H  (480 * OC_SS)

static float   oc_iz[OC_H][OC_W];
static int16_t oc_id[OC_H][OC_W];

static int view_hidden(const mdl_t *m, const model_spec *sp, double wind, uint8_t *keep)
{
	static const double bob[5] = { -1.0, -0.4, 0.3, 1.0, 1.6 };
	uint8_t *seen = xcalloc((size_t) m->ntris, 1);
	for (int f = 0; f < m->nframes; f++)
		for (int e = 0; e < 10; e++) {
			double eye[3] = { -bob[e % 5], 0, e < 5 ? -VO_UP : 0 };
			for (int y = 0; y < OC_H; y++)
				for (int x = 0; x < OC_W; x++) {
					oc_iz[y][x] = 0;
					oc_id[y][x] = -1;
				}
			for (int t = 0; t < m->ntris; t++) {
				if (!keep[t])
					continue;
				double p[3][3];
				for (int k = 0; k < 3; k++) {
					vpos(m, f, m->tris[t][k + 1], p[k]);
					for (int a = 0; a < 3; a++)
						p[k][a] = p[k][a] * sp->scale[a] - eye[a];
				}
				double a[3], b[3], c[3];
				for (int k = 0; k < 3; k++) { a[k] = p[1][k] - p[0][k]; b[k] = p[2][k] - p[0][k]; }
				c[0] = a[1] * b[2] - a[2] * b[1];
				c[1] = a[2] * b[0] - a[0] * b[2];
				c[2] = a[0] * b[1] - a[1] * b[0];
				double d = -(c[0] * p[0][0] + c[1] * p[0][1] + c[2] * p[0][2]);
				if (wind < 0) d = -d;
				if (d <= 0)
					continue;                    /* turned away: the renderer culls it */
				vo_pt q[4];
				int nq = vo_project(p, q);
				for (int k = 0; k < nq; k++) {
					q[k].x = (q[k].x - OC_X0) * OC_SS;
					q[k].y = (q[k].y - OC_Y0) * OC_SS;
				}
				for (int k = 1; k + 1 < nq; k++) {
					const vo_pt *v0 = &q[0], *v1 = &q[k], *v2 = &q[k + 1];
					double area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
					if (fabs(area) < 1e-9)
						continue;
					int x0 = (int) floor(fmin(v0->x, fmin(v1->x, v2->x))), x1 = (int) ceil(fmax(v0->x, fmax(v1->x, v2->x)));
					int y0 = (int) floor(fmin(v0->y, fmin(v1->y, v2->y))), y1 = (int) ceil(fmax(v0->y, fmax(v1->y, v2->y)));
					if (x0 < 0) x0 = 0;
					if (y0 < 0) y0 = 0;
					if (x1 > OC_W - 1) x1 = OC_W - 1;
					if (y1 > OC_H - 1) y1 = OC_H - 1;
					for (int y = y0; y <= y1; y++)
						for (int x = x0; x <= x1; x++) {
							double px = x + 0.5, py = y + 0.5;
							double w0 = ((v1->x - px) * (v2->y - py) - (v2->x - px) * (v1->y - py)) / area;
							double w1 = ((v2->x - px) * (v0->y - py) - (v0->x - px) * (v2->y - py)) / area;
							double w2 = 1 - w0 - w1;
							if (w0 < 0 || w1 < 0 || w2 < 0)
								continue;
							double iz = w0 * v0->iz + w1 * v1->iz + w2 * v2->iz;
							if (iz > oc_iz[y][x]) {
								oc_iz[y][x] = (float) iz;
								oc_id[y][x] = (int16_t) t;
							}
						}
				}
			}
			for (int y = 0; y < OC_H; y++)
				for (int x = 0; x < OC_W; x++)
					if (oc_id[y][x] >= 0)
						seen[oc_id[y][x]] = 1;
		}
	int hidden = 0;
	for (int t = 0; t < m->ntris; t++)
		if (keep[t] && !seen[t]) {
			keep[t] = 0;
			hidden++;
		}
	free(seen);
	return hidden;
}

/* a gun's triangles as the renderer draws them: rebuilt by hand (if it
   is), culled, the hidden ones dropped, cut against the affine mapping */
static void view_mesh(model_src *ms)
{
	const model_spec *sp = ms->sp;
	mdl_t *m = &ms->mdl, *hand = xmalloc(sizeof *hand);
	double wind = mdl_wind(m);
	if (gun_hand_build(m, sp, wind, hand))
		m = hand;
	uint8_t *keep = xmalloc((size_t) m->ntris);
	view_cull(m, sp, wind, keep, &ms->vnear, &ms->vback);
	ms->vhidden = view_hidden(m, sp, wind, keep);
	ms->vtris = m->ntris;
	if (!(sp->warp > 0 && tess_build(m, sp, keep, &ms->vmesh))) {
		ms->vmesh = *m;                          /* (nothing cut: the kept ones) */
		ms->vmesh.tris  = xmalloc((size_t) m->ntris * sizeof *ms->vmesh.tris);
		ms->vmesh.ntris = 0;
		for (int t = 0; t < m->ntris; t++)
			if (keep[t])
				memcpy(ms->vmesh.tris[ms->vmesh.ntris++], m->tris[t], sizeof m->tris[t]);
	}
	free(keep);
}

/* A gun's skin: the texels its triangles show, in one image, its atlas
   (docs/porting-notes.md §53). At the disc's texture level high, Quake's
   own where the screen spreads them (a triangle showing more than a pixel
   a texel in some frame, or reaching nearer than the near plane: the
   gun's near end), half elsewhere (its far end, where a half texel is no
   bigger than a pixel: the 2D gun's pictures are half the screen
   throughout); at medium, half everywhere. Until §53 the skins were half
   everywhere, Quake's 1-texel lines spread over the near end (the rocket
   launcher's highlight a band over its middle). The triangles sharing an
   edge, its texels the same on both, at one resolution, make a chart; a
   chart's box of texels, with two of the skin around them (the GPU strays
   a texel past an edge), goes into the atlas in shelves, the width
   leaving the least area: Quake's skins are mostly an unused fill, the
   atlas no larger than the half skin was. A texel no triangle shows takes
   its shown neighbours' colour (the fill is a flat blue). */
#define ATLAS_B 2

typedef struct { int div, x0, y0, w, h, ax, ay; } chart_t;

static int uf_find(int *p, int i)
{
	while (p[i] != i)
		i = p[i] = p[p[i]];
	return i;
}

/* corner k of triangle t: Quake's skin coordinates (a back face's seam) */
static void tri_stf(const mdl_t *m, int t, int k, double st[2])
{
	int vi = m->tris[t][k + 1];
	st[0] = (m->stf ? m->stf[vi][0] : m->st[vi][1]) + (!m->tris[t][0] && m->st[vi][0] ? m->skinw / 2 : 0);
	st[1] =  m->stf ? m->stf[vi][1] : m->st[vi][2];
}

static int by_chart_h(const void *a, const void *b)
{
	const chart_t *x = *(const chart_t *const *) a, *y = *(const chart_t *const *) b;
	return x->h != y->h ? y->h - x->h : y->w - x->w;
}

/* the charts in shelves `width` texels wide: the height, their places */
static int atlas_shelves(chart_t **order, int n, int width)
{
	int x = 0, y = 0, row = 0;
	for (int i = 0; i < n; i++) {
		if (x + order[i]->w > width) {
			y += row;
			x = row = 0;
		}
		order[i]->ax = x;
		order[i]->ay = y;
		x += order[i]->w;
		if (order[i]->h > row)
			row = order[i]->h;
	}
	return y + row;
}

static void view_atlas(model_src *ms)
{
	const model_spec *sp = ms->sp;
	const mdl_t *m = &ms->vmesh;
	int n = m->ntris, sw = m->skinw, sh = m->skinh, hw = (sw + 1) / 2, hh = (sh + 1) / 2;

	/* the skin, Quake's and half, the texels shown and their spread */
	uint8_t *cover = skin_cover(m, 1);
	uint8_t *full = xmalloc((size_t) sw * sh), *half = xmalloc((size_t) hw * hh);
	skin_reduce(m, cover, 1, full);
	skin_reduce(m, cover, 2, half);
	free(cover);

	/* each triangle's resolution: Quake's where the screen spreads it */
	uint8_t *res = xmalloc((size_t) n);
	for (int t = 0; t < n; t++) {
		double st[3][2];
		for (int k = 0; k < 3; k++)
			tri_stf(m, t, k, st[k]);
		double ua = fabs((st[1][0] - st[0][0]) * (st[2][1] - st[0][1]) -
		                 (st[2][0] - st[0][0]) * (st[1][1] - st[0][1])) / 2;
		int spread = 0;
		for (int f = 0; f < m->nframes && !spread && gun_tex >= QL_HIGH; f++) {
			double sx[3], sy[3];
			for (int k = 0; k < 3; k++) {
				double p[3];
				vpos(m, f, m->tris[t][k + 1], p);
				if (p[0] < VO_NEAR)
					spread = 1;                  /* the gun's near end */
				sx[k] = VO_CX - VO_PROJ * p[1] / (p[0] > 0.1 ? p[0] : 0.1);
				sy[k] = VO_CY - VO_PROJ * (p[2] + VO_UP) / (p[0] > 0.1 ? p[0] : 0.1);
			}
			double sa = fabs((sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0])) / 2;
			if (sa > ua)
				spread = 1;
		}
		res[t] = spread ? 1 : 2;
	}

	/* charts: triangles of one resolution sharing an edge and its texels */
	int *parent = xmalloc((size_t) n * sizeof *parent);
	for (int t = 0; t < n; t++)
		parent[t] = t;
	for (int a = 0; a < n; a++)
		for (int b = a + 1; b < n; b++) {
			if (res[a] != res[b] || uf_find(parent, a) == uf_find(parent, b))
				continue;
			int same = 0;
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++) {
					if (m->tris[a][i + 1] != m->tris[b][j + 1])
						continue;
					double sa[2], sb[2];
					tri_stf(m, a, i, sa);
					tri_stf(m, b, j, sb);
					same += fabs(sa[0] - sb[0]) < 1e-6 && fabs(sa[1] - sb[1]) < 1e-6;
				}
			if (same >= 2)
				parent[uf_find(parent, a)] = uf_find(parent, b);
		}
	chart_t *charts = xcalloc((size_t) n, sizeof *charts);
	int *chart_of = xmalloc((size_t) n * sizeof *chart_of), nc = 0;
	for (int t = 0; t < n; t++)
		chart_of[t] = -1;
	ms->vuv = xmalloc((size_t) n * sizeof *ms->vuv);
	for (int t = 0; t < n; t++) {
		int r = uf_find(parent, t);
		if (chart_of[r] < 0) {
			chart_of[r] = nc;
			charts[nc].div = res[t];
			charts[nc].x0 = charts[nc].y0 = 1 << 30;
			charts[nc].w = charts[nc].h = -(1 << 30);   /* (the max corner, until boxed) */
			nc++;
		}
		chart_t *c = &charts[chart_of[r]];
		for (int k = 0; k < 3; k++) {            /* its texel at the chart's resolution,
			                                        a quarter down (cook_mdl's rounding) */
			double st[2];
			tri_stf(m, t, k, st);
			int u = (int) floor(st[0] / c->div + 0.25), v = (int) floor(st[1] / c->div + 0.25);
			ms->vuv[t][k][0] = u;
			ms->vuv[t][k][1] = v;
			if (u < c->x0) c->x0 = u;
			if (v < c->y0) c->y0 = v;
			if (u > c->w) c->w = u;
			if (v > c->h) c->h = v;
		}
	}
	long texels[3] = { 0, 0, 0 };
	for (int i = 0; i < nc; i++) {               /* the boxes, the skin around them */
		chart_t *c = &charts[i];
		c->x0 -= ATLAS_B;
		c->y0 -= ATLAS_B;
		c->w = c->w + ATLAS_B - c->x0 + 1;
		c->h = c->h + ATLAS_B - c->y0 + 1;
		texels[c->div] += (long) c->w * c->h;
	}

	/* the shelves' width with the least area */
	chart_t **order = xmalloc((size_t) nc * sizeof *order);
	int wmin = 2;
	for (int i = 0; i < nc; i++) {
		order[i] = &charts[i];
		if (charts[i].w > wmin) wmin = charts[i].w;
	}
	if (wmin > PT)
		die("%s: a chart %d texels wide", sp->file, wmin);
	qsort(order, (size_t) nc, sizeof *order, by_chart_h);
	int bw = -1;
	long barea = 1L << 40;
	for (int w = wmin; w <= PT; w += 2) {
		int h = atlas_shelves(order, nc, w);
		if (h <= PT && (long) w * h < barea) {
			barea = (long) w * h;
			bw = w;
		}
	}
	if (bw < 0)
		die("%s: its atlas does not fit a texture page", sp->file);
	int bh = atlas_shelves(order, nc, bw);

	/* the atlas's texels and each corner's place in it */
	ms->im = add_image(sp->file, bw, bh, 0);
	for (int i = 0; i < nc; i++) {
		const chart_t *c = &charts[i];
		int cw = c->div == 1 ? sw : hw, ch = c->div == 1 ? sh : hh;
		const uint8_t *src = c->div == 1 ? full : half;
		for (int y = 0; y < c->h; y++)
			for (int x = 0; x < c->w; x++) {
				int sx = c->x0 + x, sy = c->y0 + y;
				sx = sx < 0 ? 0 : sx >= cw ? cw - 1 : sx;
				sy = sy < 0 ? 0 : sy >= ch ? ch - 1 : sy;
				ms->im->px[(size_t) (c->ay + y) * bw + c->ax + x] = src[(size_t) sy * cw + sx];
			}
	}
	for (int t = 0; t < n; t++) {
		const chart_t *c = &charts[chart_of[uf_find(parent, t)]];
		for (int k = 0; k < 3; k++) {
			ms->vuv[t][k][0] += c->ax - c->x0;
			ms->vuv[t][k][1] += c->ay - c->y0;
		}
	}
	printf("qcook: %s skin atlas %dx%d (%ld texels; the skin %dx%d): %d charts, %ld texels of Quake's, %ld of half%s\n",
	       sp->file, bw, bh, (long) bw * bh, sw, sh, nc, texels[1], texels[2],
	       gun_tex >= QL_HIGH ? "" : " (textures medium)");
	free(full);
	free(half);
	free(res);
	free(parent);
	free(charts);
	free(chart_of);
	free(order);
}

/* ------------------------------------------- a gun's texels repainted -- */

/* Quake draws a gun with a depth buffer, so a part may pass through
   another (the rocket launcher's muzzle cap through its tube's top: the
   arch of its hole is their meeting line); drawn in an order, one or the
   other shows whole (the arch a slot). Where the order shows another
   triangle than the depth buffer would (frame 0, the eye at rest, twice
   the screen's resolution), the texel the PS1 shows there (affine, as
   the GPU interpolates) takes the colour Quake shows there (the visible
   triangle's own texel, perspective-correct, unlit: the light stays the
   drawn triangle's); a texel only seen more often wrong than right (a
   texel is shown wherever the atlas's charts use it). The way chosen: the texels concerned repainted, no triangle more. The texels changed, their count. */
#define RP_SS 2
#define RP_W  (320 * RP_SS)
#define RP_H  (192 * RP_SS)

static int16_t rp_zt[RP_H][RP_W], rp_pt[RP_H][RP_W];   /* the triangle seen: depth buffer, order */
static float   rp_iz[RP_H][RP_W];                       /* the depth buffer's 1 / z */
static float   rp_s[RP_H][RP_W], rp_t[RP_H][RP_W];      /* its skin coordinates there (Quake's) */
static float   rp_u[RP_H][RP_W], rp_v[RP_H][RP_W];      /* the drawn one's atlas texel there */

/* triangle t's corners on the screen (x RP_SS) and their depths; 0 if any
   is nearer than the near plane, or it faces away */
static int rp_corners(const mdl_t *m, double wind, int t, double sx[3], double sy[3], double z[3])
{
	double p[3][3];
	for (int k = 0; k < 3; k++) {
		vpos(m, 0, m->tris[t][k + 1], p[k]);
		p[k][2] += VO_UP;                        /* (the eye 2 units under the gun) */
		if (p[k][0] < VO_NEAR)
			return 0;
		z[k]  = p[k][0];
		sx[k] = RP_SS * (VO_CX - VO_PROJ * p[k][1] / p[k][0]);
		sy[k] = RP_SS * (VO_CY - VO_PROJ * p[k][2] / p[k][0]);
	}
	double a[3], b[3], n[3];
	for (int k = 0; k < 3; k++) { a[k] = p[1][k] - p[0][k]; b[k] = p[2][k] - p[0][k]; }
	n[0] = a[1] * b[2] - a[2] * b[1];
	n[1] = a[2] * b[0] - a[0] * b[2];
	n[2] = a[0] * b[1] - a[1] * b[0];
	double d = -(n[0] * p[0][0] + n[1] * p[0][1] + n[2] * p[0][2]);
	if (wind < 0) d = -d;
	return d > 0;                                /* (the renderer culls the others) */
}

/* each sample of triangle t: fn(x, y, barycentric weights) */
#define RP_EACH(sx, sy, body)                                                                     \
	do {                                                                                          \
		double area_ = ((sx)[1] - (sx)[0]) * ((sy)[2] - (sy)[0]) - ((sx)[2] - (sx)[0]) * ((sy)[1] - (sy)[0]); \
		if (fabs(area_) < 1e-9) break;                                                            \
		int x0_ = (int) floor(fmin((sx)[0], fmin((sx)[1], (sx)[2]))), x1_ = (int) ceil(fmax((sx)[0], fmax((sx)[1], (sx)[2]))); \
		int y0_ = (int) floor(fmin((sy)[0], fmin((sy)[1], (sy)[2]))), y1_ = (int) ceil(fmax((sy)[0], fmax((sy)[1], (sy)[2]))); \
		if (x0_ < 0) x0_ = 0;                                                                     \
		if (y0_ < 0) y0_ = 0;                                                                     \
		if (x1_ > RP_W - 1) x1_ = RP_W - 1;                                                       \
		if (y1_ > RP_H - 1) y1_ = RP_H - 1;                                                       \
		for (int y = y0_; y <= y1_; y++)                                                          \
			for (int x = x0_; x <= x1_; x++) {                                                    \
				double px_ = x + 0.5, py_ = y + 0.5;                                              \
				double w0 = (((sx)[1] - px_) * ((sy)[2] - py_) - ((sx)[2] - px_) * ((sy)[1] - py_)) / area_; \
				double w1 = (((sx)[2] - px_) * ((sy)[0] - py_) - ((sx)[0] - px_) * ((sy)[2] - py_)) / area_; \
				double w2 = 1 - w0 - w1;                                                          \
				if (w0 < 0 || w1 < 0 || w2 < 0) continue;                                         \
				body                                                                              \
			}                                                                                     \
	} while (0)

static int view_repaint(model_src *ms, const mdl_t *m, double wind, const int *emitted, int nemit)
{
	image *im = ms->im;
	const uint8_t *skin = m->skin;
	int sw = m->skinw, sh = m->skinh;
	for (int y = 0; y < RP_H; y++)
		for (int x = 0; x < RP_W; x++) {
			rp_zt[y][x] = rp_pt[y][x] = -1;
			rp_iz[y][x] = 0;
		}
	for (int t = 0; t < m->ntris; t++) {         /* Quake's: the depth buffer */
		double sx[3], sy[3], z[3], st[3][2];
		if (!rp_corners(m, wind, t, sx, sy, z))
			continue;
		for (int k = 0; k < 3; k++)
			tri_stf(m, t, k, st[k]);
		RP_EACH(sx, sy, {
			double iz = w0 / z[0] + w1 / z[1] + w2 / z[2];
			if (iz > rp_iz[y][x]) {
				rp_iz[y][x] = (float) iz;
				rp_zt[y][x] = (int16_t) t;
				rp_s[y][x] = (float) ((w0 * st[0][0] / z[0] + w1 * st[1][0] / z[1] + w2 * st[2][0] / z[2]) / iz);
				rp_t[y][x] = (float) ((w0 * st[0][1] / z[0] + w1 * st[1][1] / z[1] + w2 * st[2][1] / z[2]) / iz);
			}
		});
	}
	for (int e = nemit - 1; e >= 0; e--) {       /* the PS1's: the first written drawn last */
		int t = emitted[e];
		double sx[3], sy[3], z[3];
		if (!rp_corners(m, wind, t, sx, sy, z))
			continue;
		RP_EACH(sx, sy, {
			rp_pt[y][x] = (int16_t) t;
			rp_u[y][x] = (float) (w0 * ms->vuv[t][0][0] + w1 * ms->vuv[t][1][0] + w2 * ms->vuv[t][2][0]);
			rp_v[y][x] = (float) (w0 * ms->vuv[t][0][1] + w1 * ms->vuv[t][1][1] + w2 * ms->vuv[t][2][1]);
		});
	}
	/* each atlas texel: its samples right and wrong, the colours Quake shows at the wrong ones */
	int n = im->w * im->h, changed = 0;
	int *right = xcalloc((size_t) n, sizeof *right), *wrong = xcalloc((size_t) n, sizeof *wrong);
	double (*sum)[3] = xcalloc((size_t) n, sizeof *sum);
	for (int y = 0; y < RP_H; y++)
		for (int x = 0; x < RP_W; x++) {
			int pt = rp_pt[y][x], zt = rp_zt[y][x];
			if (pt < 0 || zt < 0)
				continue;
			int u = (int) floor(rp_u[y][x]), v = (int) floor(rp_v[y][x]);
			if (u < 0 || v < 0 || u >= im->w || v >= im->h)
				continue;
			int i = v * im->w + u;
			if (pt == zt) {
				right[i]++;
				continue;
			}
			int ss = (int) floor(rp_s[y][x]), tt = (int) floor(rp_t[y][x]);
			ss = ss < 0 ? 0 : ss >= sw ? sw - 1 : ss;
			tt = tt < 0 ? 0 : tt >= sh ? sh - 1 : tt;
			int c = skin[(size_t) tt * sw + ss];
			for (int k = 0; k < 3; k++)
				sum[i][k] += palette[c * 3 + k];
			wrong[i]++;
		}
	for (int i = 0; i < n; i++)
		if (wrong[i] > right[i]) {
			uint8_t c = (uint8_t) nearest_colour((int) lround(sum[i][0] / wrong[i]),
			                                     (int) lround(sum[i][1] / wrong[i]),
			                                     (int) lround(sum[i][2] / wrong[i]));
			if (c != im->px[i])
				changed++;
			im->px[i] = c;
			gpage[im->page][im->v + i / im->w][im->u + i % im->w] = c;   /* (placed already) */
		}
	free(right);
	free(wrong);
	free(sum);
	return changed;
}

static void cook_mdl(model_src *ms, int model_level, gbuf *geo, qg_model *out)
{
	const model_spec *sp = ms->sp;
	mdl_t *m = &ms->mdl;
	const image *im = ms->im;
	out->tex.tpage  = page_tpage(im->page);
	out->tex.window = 0xE2000000u;

	double wind = mdl_wind(m);                     /* (the records wind as the world: the
	                                                  renderer leaves out a model's
	                                                  triangles turned away, r_world.s) */
	int dropped_near = 0, dropped_back = 0, dropped_hidden = 0, full_tris = m->ntris;
	if (sp->view) {                                /* a gun: as view_mesh made it */
		m = &ms->vmesh;
		dropped_near   = ms->vnear;
		dropped_back   = ms->vback;
		dropped_hidden = ms->vhidden;
		full_tris      = ms->vtris;
	}
	uint8_t *keep = xmalloc((size_t) m->ntris + 1);
	memset(keep, 1, (size_t) m->ntris + 1);

	/* the model level: the triangles kept, reduced (they all stay) */
	mdl_t reduced;
	int (*reduced_tris)[4] = reduce_mdl(m, sp, keep, model_level, &reduced);
	if (reduced_tris) {
		m = &reduced;
		free(keep);
		keep = xmalloc((size_t) m->ntris + 1);
		memset(keep, 1, (size_t) m->ntris + 1);
	}

	/* triangle order: a view model nearest-first (seen from the origin, the
	   eye), by frame 0; else as in the file */
	tri_order *ord = xmalloc((size_t) m->ntris * sizeof *ord);
	for (int t = 0; t < m->ntris; t++) {
		double c[3] = { 0, 0, 0 };
		for (int k = 1; k < 4; k++) {
			int16_t v[3];
			mdl_vertex(m, 0, m->tris[t][k], sp->scale, v);
			for (int a = 0; a < 3; a++) c[a] += v[a] / 3.0;
		}
		ord[t].tri = t;
		ord[t].d   = sp->view ? sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]) : t;
	}
	if (sp->view)
		qsort(ord, (size_t) m->ntris, sizeof *ord, by_dist);

	/* records: from the order, but among the next WINDOW triangles prefer
	   the one sharing the most vertices with the record being built (fewer
	   duplicated slots; the painter order stays coarse, which is enough) */
	enum { WINDOW = 24 };
	uint8_t *done = xcalloc((size_t) m->ntris, 1);
	size_t rec_start = geo->len;
	view_records = sp->view;
	FILE *gd = NULL;                               /* (QCOOK_GUNDUMP=<dir>: a view model's
	                                                  triangles as drawn, for a look) */
	if (sp->view && getenv("QCOOK_GUNDUMP")) {
		char path[512];
		const char *base = strrchr(sp->file, '/');
		snprintf(path, sizeof path, "%s/%s.gun", getenv("QCOOK_GUNDUMP"), base ? base + 1 : sp->file);
		gd = fopen(path, "w");
		if (gd) {
			fprintf(gd, "skin %d %d %d %d div %d full %d %d\n", im->u, im->v, im->w, im->h, sp->skin_div,
			        m->skinw, m->skinh);
			for (int f = 0; f < m->nframes; f++)
				for (int i = 0; i < m->nverts; i++) {
					double p[3];
					vpos(m, f, i, p);
					fprintf(gd, "v %d %d %.4f %.4f %.4f %d\n", f, i, p[0], p[1], p[2], m->frames[f][i].n);
				}
		}
	}
	nslots = 0;
	slot_of = xmalloc((size_t) m->nverts * sizeof *slot_of);
	for (int i = 0; i < m->nverts; i++)
		slot_of[i] = -1;
	int nrec = 0, left = 0, scan = 0, order_cycles = 0, nemit = 0;
	double order_left = 0;
	double *net = sp->view ? view_order(m, sp, wind, keep) : NULL;
	int *emitted = xmalloc((size_t) m->ntris * sizeof *emitted);   /* (the order written) */
	for (int t = 0; t < m->ntris; t++) {
		done[t] = !keep[t];
		left += keep[t];
	}
	free(keep);
	mrec r;
	memset(&r, 0, sizeof r);
	while (left) {
		while (done[ord[scan].tri]) scan++;
		int best = -1, best_shared = -1;
		if (net) {
			/* a view model: of the triangles every one in front of which is
			   written, the one sharing the most vertices with the record (the
			   nearest first); none (the frames' votes make a cycle): the one
			   whose turn breaks the least say */
			int ready = 0, cyc = -1;
			double cyc_cost = 0;
			for (int o = scan; o < m->ntris; o++) {
				int t = ord[o].tri;
				if (done[t]) continue;
				double cost = 0;
				for (int i = 0; i < m->ntris; i++)
					if (!done[i])
						cost += net[(size_t) i * m->ntris + t];
				int shared = 0, newv = 0;
				for (int k = 1; k < 4; k++)
					if (rec_has(&r, m->tris[t][k])) shared++;
					else newv++;
				int fits = r.nv + newv <= QW_MAX_VERTS;
				if (cost > 0) {
					if (fits && (cyc < 0 || cost < cyc_cost)) { cyc = t; cyc_cost = cost; }
					continue;
				}
				ready++;
				if (fits && shared > best_shared) { best_shared = shared; best = t; }
			}
			if (best < 0 && !ready && cyc >= 0) {
				best = cyc;
				order_left += cyc_cost;
				order_cycles++;
			}
		} else
			for (int o = scan, seen = 0; o < m->ntris && seen < WINDOW; o++) {
				int t = ord[o].tri;
				if (done[t]) continue;
				seen++;
				int shared = 0, newv = 0;
				for (int k = 1; k < 4; k++)
					if (rec_has(&r, m->tris[t][k])) shared++;
					else newv++;
				if (r.nv + newv > QW_MAX_VERTS) continue;
				if (shared > best_shared) { best_shared = shared; best = t; }
				if (!r.nt) break;             /* an empty record: take the first */
			}
		if (best < 0 || r.nt >= REC_MAX_TRIS) {
			mrec_flush(geo, &r, m, sp->scale, &nrec);
			continue;
		}
		int t = best;
		done[t] = 1;
		left--;
		emitted[nemit++] = t;
		int s[3];
		for (int k = 0; k < 3; k++)
			s[k] = rec_slot(&r, m->tris[t][k + 1]);
		qw_tri *tri = &r.t[r.nt++];
		memset(tri, 0, sizeof *tri);
		for (int k = 0; k < 3; k++) {
			int vi = m->tris[t][k + 1];
			double ss = m->stf ? m->stf[vi][0] : m->st[vi][1], tt = m->stf ? m->stf[vi][1] : m->st[vi][2];
			if (!m->tris[t][0] && m->st[vi][0])          /* back face: onseam */
				ss += m->skinw / 2;
			/* the reduced skin's texel holding Quake's texel (s, t) (a cut's
			   middle: the nearest, a quarter of a texel down, as the
			   texels' own come: s / skin_div truncated) */
			long u = im->u + (long) floor(ss / sp->skin_div + 0.25),
			     v = im->v + (long) floor(tt / sp->skin_div + 0.25);
			if (sp->view) {                        /* a gun: its corner in its atlas */
				u = im->u + ms->vuv[t][k][0];
				v = im->v + ms->vuv[t][k][1];
			}
			if (u < im->u) u = im->u;
			if (u > im->u + im->w - 1) u = im->u + im->w - 1;
			if (v < im->v) v = im->v;
			if (v > im->v + im->h - 1) v = im->v + im->h - 1;
			int at = wind < 0 && k ? 3 - k : k;        /* (the world's winding) */
			tri->idx[at] = (uint8_t) (s[k] * 4);
			tri->uv[at]  = (uint16_t) (u | v << 8);
		}
		if (gd) {                                  /* (in the order drawn: nearest first) */
			fprintf(gd, "t %d %d", t, m->tris[t][0]);
			for (int k = 0; k < 3; k++) {
				int vi = m->tris[t][k + 1], at = wind < 0 && k ? 3 - k : k;   /* (its record corner) */
				double ss = (m->stf ? m->stf[vi][0] : m->st[vi][1]) + (!m->tris[t][0] && m->st[vi][0] ? m->skinw / 2 : 0);
				double tt = m->stf ? m->stf[vi][1] : m->st[vi][2];
				fprintf(gd, "  %d %d %d %.2f %.2f", vi, tri->uv[at] & 0xff, tri->uv[at] >> 8, ss, tt);
			}
			fprintf(gd, "\n");
		}
	}
	int repainted = sp->view ? view_repaint(ms, m, wind, emitted, nemit) : 0;
	free(emitted);
	if (gd) {                                      /* (the atlas's texels, repainted) */
		for (int y = 0; y < im->h; y++) {
			fprintf(gd, "px");
			for (int x = 0; x < im->w; x++)
				fprintf(gd, " %d", im->px[(size_t) y * im->w + x]);
			fprintf(gd, "\n");
		}
		fclose(gd);
	}
	mrec_flush(geo, &r, m, sp->scale, &nrec);
	free(slot_of);
	slot_of = NULL;
	free(net);
	free(ord);
	free(done);
	out->ofs_records = (uint32_t) rec_start;
	out->len_records = (uint32_t) (geo->len - rec_start);
	out->num_records = (uint16_t) nrec;
	out->num_verts   = (uint16_t) nslots;

	/* frames: the pools (a view model's Quake's own vertex words, its steps
	   and origin before them: formats/qgame.h qg_view) */
	galign(geo, 4);
	if (sp->view) {
		qg_view qv;
		for (int k = 0; k < 3; k++) {
			if (sp->scale[k] != 1)
				die("%s: a view model scaled", sp->file);
			qv.step[k]   = (int32_t) lround(m->scale[k] * QW_VSCALE / QG_VIEW_SUB * 65536.0);
			qv.origin[k] = (int32_t) lround(m->origin[k] * QW_VSCALE * 65536.0);
		}
		gput(geo, &qv, sizeof qv);
	}
	out->ofs_frames  = (uint32_t) geo->len;
	out->num_frames  = (uint16_t) m->nframes;
	double rad = 0;
	for (int f = 0; f < m->nframes; f++)
		for (int s = 0; s < nslots; s++) {
			int16_t p[3];
			mdl_vertex(m, f, slots[s], sp->scale, p);
			int n = m->frames[f][slots[s]].n;
			if (n >= QG_NORMALS) n = 0;
			if (sp->view) {                      /* x | z << 8 | y << 16 | normal << 24 */
				uint32_t w = (uint32_t) n << 24;
				for (int k = 0; k < 3; k++) {
					long b = lround(vstep(m, f, slots[s], k));   /* (a middle: the nearest) */
					w |= (uint32_t) (b < 0 ? 0 : b > 255 ? 255 : b) << (k == 0 ? 0 : k == 1 ? 16 : 8);
				}
				gput(geo, &w, sizeof w);
			} else {
				qw_vert v;
				v.x = p[0]; v.y = p[1]; v.z = p[2];
				v.pad = (int16_t) n;
				gput(geo, &v, sizeof v);
			}
			double d = sqrt((double) p[0] * p[0] + (double) p[1] * p[1] + (double) p[2] * p[2]);
			if (d > rad) rad = d;
		}
	if (sp->view && nslots > QG_VIEW_VERTS)
		die("%s: %d vertices, the engine's pool for a view model holds %d", sp->file, nslots, QG_VIEW_VERTS);
	out->radius = (uint16_t) (rad > 65000 ? 65000 : ceil(rad));
	out->flags  = (uint16_t) ((sp->flat ? QGF_FLAT : 0) | (sp->view ? QGF_VIEW : 0));
	printf("qcook: %-20s %3d frames %4d tris (%d behind the eye, %d back-facing, %d hidden dropped; %d drawn) -> %3d records, %4d slots (%d verts), skin %dx%d, %u B",
	       sp->file, m->nframes, full_tris, dropped_near, dropped_back, dropped_hidden, m->ntris, nrec, nslots,
	       m->nverts, im->w, im->h, (unsigned) (geo->len - rec_start));
	if (reduced_tris)
		printf("; %s: %d of %d tris", level_name[model_level], decim_left, decim_tris);
	if (sp->view)
		printf("; order: %d cycles broken (%.0f px2 of overlap out of order), %d texels repainted",
		       order_cycles, order_left, repainted);
	printf("\n");
	free(reduced_tris);
}

/* a sprite: one record (one quad) per frame; the frame's pool holds the
   quad's corners in the view plane (x right, y down), vertex units */
static void cook_spr(model_src *ms, gbuf *geo, qg_model *out)
{
	const model_spec *sp = ms->sp;
	const spr_t *s = &ms->spr;
	const image *im = ms->im;
	out->tex.tpage  = page_tpage(im->page);
	out->tex.window = 0xE2000000u;

	size_t rec_start = geo->len;
	for (int f = 0; f < s->nframes; f++) {
		const sprframe *fr = &s->frames[f];
		qw_record h;
		memset(&h, 0, sizeof h);
		h.num_verts = 4;
		h.num_quads = 1;
		h.polys     = (uint16_t) (sizeof h + 12 + 4);   /* 6 indices (12 B), 4 light bytes */
		h.size      = (uint16_t) (h.polys + sizeof(qw_quad));
		double r = sqrt((double) fr->w * fr->w + (double) fr->h * fr->h) * QW_VSCALE;
		h.sr = (uint16_t) ceil(r + 1);
		gput(geo, &h, sizeof h);
		uint16_t idx[6] = { 0, 1, 2, 3, 0, 0 };
		gput(geo, idx, sizeof idx);
		uint8_t col[4] = { 128, 128, 128, 128 };
		gput(geo, col, sizeof col);
		qw_quad q;
		int u0 = im->u + ms->fx[f], v0 = im->v + ms->fy[f], u1 = u0 + fr->w - 1, v1 = v0 + fr->h - 1;
		if (u1 > 255 || v1 > 255) die("%s: sprite past its page", sp->file);
		q.idx[0] = 0;  q.uv[0] = (uint16_t) (u0 | v0 << 8);
		q.idx[1] = 4;  q.uv[1] = (uint16_t) (u1 | v0 << 8);
		q.idx[2] = 8;  q.uv[2] = (uint16_t) (u0 | v1 << 8);
		q.idx[3] = 12; q.uv[3] = (uint16_t) (u1 | v1 << 8);
		gput(geo, &q, sizeof q);
	}
	out->ofs_records = (uint32_t) rec_start;
	out->len_records = (uint32_t) (geo->len - rec_start);
	out->num_records = (uint16_t) s->nframes;
	out->num_verts   = 4;
	galign(geo, 4);
	out->ofs_frames  = (uint32_t) geo->len;
	out->num_frames  = (uint16_t) s->nframes;
	double rad = 0;
	for (int f = 0; f < s->nframes; f++) {
		const sprframe *fr = &s->frames[f];
		/* Quake: origin = the upper-left corner, x right, y up */
		int x0 = fr->ox * QW_VSCALE, x1 = (fr->ox + fr->w) * QW_VSCALE;
		int y0 = -fr->oy * QW_VSCALE, y1 = (-fr->oy + fr->h) * QW_VSCALE;
		qw_vert v[4] = { { (int16_t) x0, (int16_t) y0, 0, 0 }, { (int16_t) x1, (int16_t) y0, 0, 0 },
		                 { (int16_t) x0, (int16_t) y1, 0, 0 }, { (int16_t) x1, (int16_t) y1, 0, 0 } };
		gput(geo, v, sizeof v);
		double d = sqrt((double) x1 * x1 + (double) y1 * y1);
		if (d > rad) rad = d;
		d = sqrt((double) x0 * x0 + (double) y0 * y0);
		if (d > rad) rad = d;
	}
	out->radius = (uint16_t) ceil(rad);
	out->flags  = QGF_SPRITE | QGF_FLAT;
	printf("qcook: %-20s %3d frames (%dx%d) -> %u B\n", sp->file, s->nframes, s->w, s->h,
	       (unsigned) (geo->len - rec_start));
}

/* -------------------------------------------------------------- pictures -- */

typedef struct { const char *lump; int id; int x0, w; image *im; } pic_spec;   /* w: crop, 0 = whole */

static pic_spec PICS[QGP_COUNT];
static int      npics;

static void pic(const char *lump, int id, int x0, int w)
{
	if (npics >= QGP_COUNT) die("too many pictures");
	PICS[npics].lump = lump;
	PICS[npics].id   = id;
	PICS[npics].x0   = x0;
	PICS[npics].w    = w;
	npics++;
}

static void pic_list(void)
{
	static const char *const digit[11] = { "num_0", "num_1", "num_2", "num_3", "num_4", "num_5",
	                                       "num_6", "num_7", "num_8", "num_9", "num_minus" };
	static const char *const adigit[11] = { "anum_0", "anum_1", "anum_2", "anum_3", "anum_4", "anum_5",
	                                        "anum_6", "anum_7", "anum_8", "anum_9", "anum_minus" };
	static const char *const weapon[7] = { "shotgun", "sshotgun", "nailgun", "snailgun", "rlaunch",
	                                       "srlaunch", "lightng" };
	static char names[7 * 7][20];
	for (int i = 0; i < 11; i++) pic(digit[i], QGP_NUM_0 + i, 0, 0);
	for (int i = 0; i < 11; i++) pic(adigit[i], QGP_ANUM_0 + i, 0, 0);
	pic("num_colon", QGP_NUM_COLON, 0, 0);
	pic("num_slash", QGP_NUM_SLASH, 0, 0);
	pic("sbar", QGP_SBAR_L, 0, 160);        pic("sbar", QGP_SBAR_R, 160, 160);
	pic("ibar", QGP_IBAR_L, 0, 160);        pic("ibar", QGP_IBAR_R, 160, 160);
	pic("scorebar", QGP_SCOREBAR_L, 0, 160); pic("scorebar", QGP_SCOREBAR_R, 160, 160);
	pic("sb_armor1", QGP_ARMOR1, 0, 0); pic("sb_armor2", QGP_ARMOR2, 0, 0); pic("sb_armor3", QGP_ARMOR3, 0, 0);
	pic("sb_shells", QGP_AMMO_SHELLS, 0, 0); pic("sb_nails", QGP_AMMO_NAILS, 0, 0);
	pic("sb_rocket", QGP_AMMO_ROCKET, 0, 0); pic("sb_cells", QGP_AMMO_CELLS, 0, 0);
	int n = 0;
	for (int w = 0; w < 7; w++) { snprintf(names[n], 20, "inv_%s", weapon[w]);  pic(names[n], QGP_INV_SHOTGUN + w, 0, 0); n++; }
	for (int w = 0; w < 7; w++) { snprintf(names[n], 20, "inv2_%s", weapon[w]); pic(names[n], QGP_INV2_SHOTGUN + w, 0, 0); n++; }
	for (int a = 0; a < 5; a++)
		for (int w = 0; w < 7; w++) {
			snprintf(names[n], 20, "inva%d_%s", a + 1, weapon[w]);
			pic(names[n], QGP_INVA_SHOTGUN + a * 7 + w, 0, 0);
			n++;
		}
	pic("sb_key1", QGP_KEY1, 0, 0); pic("sb_key2", QGP_KEY2, 0, 0);
	pic("sb_invis", QGP_INVIS, 0, 0); pic("sb_invuln", QGP_INVULN, 0, 0);
	pic("sb_suit", QGP_SUIT, 0, 0); pic("sb_quad", QGP_QUAD, 0, 0);
	pic("sb_sigil1", QGP_SIGIL1, 0, 0); pic("sb_sigil2", QGP_SIGIL2, 0, 0);
	pic("sb_sigil3", QGP_SIGIL3, 0, 0); pic("sb_sigil4", QGP_SIGIL4, 0, 0);
	static const char *const face[15] = { "face1", "face2", "face3", "face4", "face5",
	                                      "face_p1", "face_p2", "face_p3", "face_p4", "face_p5",
	                                      "face_invis", "face_invul1", "face_invul2", "face_inv2", "face_quad" };
	for (int i = 0; i < 15; i++) pic(face[i], QGP_FACE1 + i, 0, 0);
	pic("conchars", QGP_SMALLNUM, 0, 0);
	if (npics != QGP_COUNT) die("picture list: %d of %d", npics, QGP_COUNT);
}

/* the pictures' images (not placed yet) */
static void pics_prepare(void)
{
	pic_list();
	for (int i = 0; i < npics; i++) {
		pic_spec *ps = &PICS[i];
		uint32_t size;
		int type;
		const uint8_t *d = wad_lump(ps->lump, &size, &type);
		if (ps->id == QGP_SMALLNUM) {
			/* conchars: 16 x 16 glyphs of 8 x 8; the yellow digits are
			   characters 18-27 (row 1, columns 2-11); 0 is transparent there */
			if (size < 128 * 128) die("gfx.wad: conchars too small");
			ps->im = add_image(ps->lump, 80, 8, 0);
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 80; x++) {
					uint8_t c = d[(8 + y) * 128 + 16 + x];
					ps->im->px[y * 80 + x] = c ? c : 255;
				}
			continue;
		}
		if (type != 0x42 || size < 8) die("gfx.wad: %s is not a picture", ps->lump);
		int lw = rdi(d), lh = rdi(d + 4);
		if (lw < 1 || lh < 1 || (uint32_t) lw * (uint32_t) lh + 8 > size) die("gfx.wad: %s bad size", ps->lump);
		int w = ps->w ? ps->w : lw;
		if (ps->x0 + w > lw) die("gfx.wad: %s crop past its width", ps->lump);
		ps->im = add_image(ps->lump, w, lh, 0);
		for (int y = 0; y < lh; y++)
			memcpy(ps->im->px + (size_t) y * w, d + 8 + (size_t) y * lw + ps->x0, (size_t) w);
	}
}


/* ------------------------------------------------------------- aux pics -- */

/* The intermission's, the menu's and the console font, 4bpp with their own
   16 colours (formats/qgame.h QGA_*) in the two 4-bit pages at x 960. A
   picture's colours: its own as they are when 16 do (15 and the
   transparent one), else k-means over its pixels' colours. */
typedef struct { const char *src; int id; int x0, w; } aux_spec;   /* "#conchars": gfx.wad's,
                                                                     x0 its first row */
static const aux_spec AUX[] = {
	{ "gfx/complete.lmp", QGA_COMPLETE, 0, 0 },   { "gfx/inter.lmp", QGA_INTER, 0, 0 },
	{ "gfx/finale.lmp", QGA_FINALE_L, 0, 144 },   { "gfx/finale.lmp", QGA_FINALE_R, 144, 144 },
	{ "#conchars", QGA_CONCHARS, 0, 0 },          { "#conchars", QGA_CONCHARS2, 64, 0 },
	{ "gfx/qplaque.lmp", QGA_QPLAQUE, 0, 0 },     { "gfx/ttl_main.lmp", QGA_TTL_MAIN, 0, 0 },
	{ "gfx/mainmenu.lmp", QGA_MAINMENU, 0, 0 },
	{ "gfx/menudot1.lmp", QGA_MENUDOT1 + 0, 0, 0 }, { "gfx/menudot2.lmp", QGA_MENUDOT1 + 1, 0, 0 },
	{ "gfx/menudot3.lmp", QGA_MENUDOT1 + 2, 0, 0 }, { "gfx/menudot4.lmp", QGA_MENUDOT1 + 3, 0, 0 },
	{ "gfx/menudot5.lmp", QGA_MENUDOT1 + 4, 0, 0 }, { "gfx/menudot6.lmp", QGA_MENUDOT1 + 5, 0, 0 },
	{ "gfx/ttl_sgl.lmp", QGA_TTL_SGL, 0, 0 },     { "gfx/sp_menu.lmp", QGA_SP_MENU, 0, 0 },
	{ "gfx/p_option.lmp", QGA_P_OPTION, 0, 0 },
	{ "gfx/box_tl.lmp", QGA_BOX_TL, 0, 0 }, { "gfx/box_ml.lmp", QGA_BOX_ML, 0, 0 },
	{ "gfx/box_bl.lmp", QGA_BOX_BL, 0, 0 }, { "gfx/box_tm.lmp", QGA_BOX_TM, 0, 0 },
	{ "gfx/box_mm.lmp", QGA_BOX_MM, 0, 0 }, { "gfx/box_mm2.lmp", QGA_BOX_MM2, 0, 0 },
	{ "gfx/box_bm.lmp", QGA_BOX_BM, 0, 0 }, { "gfx/box_tr.lmp", QGA_BOX_TR, 0, 0 },
	{ "gfx/box_mr.lmp", QGA_BOX_MR, 0, 0 }, { "gfx/box_br.lmp", QGA_BOX_BR, 0, 0 },
	{ "#disc", QGA_DISC, 0, 0 },
};
#define NUM_AUX (int) (sizeof AUX / sizeof AUX[0])

/* A big menu's 20-row entries the port draws (engine/menu.c), kept one
   under the other (bit k: entry k), cut from the whole picture once its
   colours are chosen (its pixels the same): the main menu's Single Player
   and Options, not its Multiplayer (single player only), Help or Quit */
#define MENU_ROWS 20
static const struct { int id; unsigned bands; } AUX_BANDS[] = {
	{ QGA_MAINMENU, 1u << 0 | 1u << 2 },
};
#define AUX_PAGES 2

typedef struct {
	int      w, h;
	uint8_t *px;            /* 4-bit indices, one per byte              */
	uint16_t clut[16];
	int      page, u, v;
} aux_img;

static aux_img aux[QGA_COUNT];
static uint8_t apage[AUX_PAGES][PT][PT];   /* 4-bit indices, one per byte */
static int     apage_rows[AUX_PAGES];
static rect    afree[AUX_PAGES][MAX_FREE];
static int     nafree[AUX_PAGES];

static void afree_add(int p, rect r)
{
	if (r.w <= 0 || r.h <= 0) return;
	for (int i = 0; i < nafree[p]; i++)
		if (contains(&afree[p][i], &r)) return;
	for (int i = 0; i < nafree[p]; )
		if (contains(&r, &afree[p][i])) afree[p][i] = afree[p][--nafree[p]];
		else i++;
	if (nafree[p] >= MAX_FREE) die("MaxRects: too many free rectangles (aux)");
	afree[p][nafree[p]++] = r;
}

static uint16_t rgb15(int r, int g, int b)
{
	uint16_t c = (uint16_t) ((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
	return c ? c : 0x8000;                       /* black, not transparent */
}

/* k-means on the picture's colours (weighted by use) into k clusters */
static void aux_quantize(aux_img *a, const uint8_t *src, int trans_index)
{
	int used[256] = { 0 }, n = a->w * a->h, has_trans = 0;
	for (int i = 0; i < n; i++) {
		if (src[i] == trans_index) has_trans = 1;
		else used[src[i]]++;
	}
	int k = has_trans ? 15 : 16, base = has_trans ? 1 : 0;
	int cols[256], nc = 0;
	for (int i = 0; i < 256; i++)
		if (used[i]) cols[nc++] = i;
	double cr[16], cg[16], cb[16];
	int map[256];
	if (nc <= k) {
		for (int c = 0; c < nc; c++) {
			cr[c] = palette[cols[c] * 3]; cg[c] = palette[cols[c] * 3 + 1]; cb[c] = palette[cols[c] * 3 + 2];
			map[cols[c]] = c;
		}
		k = nc;
	} else {
		/* seeds: the most used colours, spread (farthest-first among them) */
		int seed[16], ns = 0;
		int best = 0;
		for (int c = 0; c < nc; c++) if (used[cols[c]] > used[cols[best]]) best = c;
		seed[ns++] = best;
		while (ns < k) {
			double bd = -1; int bi = 0;
			for (int c = 0; c < nc; c++) {
				double md = 1e30;
				for (int s = 0; s < ns; s++) {
					const uint8_t *p = &palette[cols[c] * 3], *q = &palette[cols[seed[s]] * 3];
					double d = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
					if (d < md) md = d;
				}
				md *= 1 + 0.02 * used[cols[c]];
				if (md > bd) { bd = md; bi = c; }
			}
			seed[ns++] = bi;
		}
		for (int s = 0; s < k; s++) {
			cr[s] = palette[cols[seed[s]] * 3]; cg[s] = palette[cols[seed[s]] * 3 + 1]; cb[s] = palette[cols[seed[s]] * 3 + 2];
		}
		for (int it = 0; it < 40; it++) {
			double sr[16] = { 0 }, sg[16] = { 0 }, sb[16] = { 0 }, sw[16] = { 0 };
			for (int c = 0; c < nc; c++) {
				const uint8_t *p = &palette[cols[c] * 3];
				double bd = 1e30; int bi = 0;
				for (int s = 0; s < k; s++) {
					double d = (p[0] - cr[s]) * (p[0] - cr[s]) + (p[1] - cg[s]) * (p[1] - cg[s]) + (p[2] - cb[s]) * (p[2] - cb[s]);
					if (d < bd) { bd = d; bi = s; }
				}
				map[cols[c]] = bi;
				double w = used[cols[c]];
				sr[bi] += p[0] * w; sg[bi] += p[1] * w; sb[bi] += p[2] * w; sw[bi] += w;
			}
			for (int s = 0; s < k; s++)
				if (sw[s] > 0) { cr[s] = sr[s] / sw[s]; cg[s] = sg[s] / sw[s]; cb[s] = sb[s] / sw[s]; }
		}
	}
	memset(a->clut, 0, sizeof a->clut);              /* (entry 0: transparent when used) */
	for (int s = 0; s < k; s++)
		a->clut[base + s] = rgb15((int) lround(cr[s]), (int) lround(cg[s]), (int) lround(cb[s]));
	a->px = xcalloc((size_t) n, 1);
	for (int i = 0; i < n; i++)
		a->px[i] = src[i] == trans_index ? 0 : (uint8_t) (base + map[src[i]]);
}

static void aux_prepare(const uint8_t *pak, long pak_len)
{
	if (NUM_AUX != QGA_COUNT) die("aux picture list: %d of %d", NUM_AUX, QGA_COUNT);
	for (int i = 0; i < NUM_AUX; i++) {
		const aux_spec *as = &AUX[i];
		aux_img *a = &aux[as->id];
		const uint8_t *d;
		int lw, lh, trans = 255;
		uint32_t size;
		int conchars = !strcmp(as->src, "#conchars");
		if (conchars) {
			int type;
			d  = wad_lump(as->src + 1, &size, &type);
			if (size < 128 * 128) die("gfx.wad: conchars too small");
			d += as->x0 * 128;                        /* the half's first row */
			lw = 128;
			lh = 64;
			trans = 0;                                /* Draw_Character: 0 is transparent */
		} else if (as->src[0] == '#') {               /* a gfx.wad picture */
			int type;
			d = wad_lump(as->src + 1, &size, &type);
			if (type != 0x42 || size < 8) die("gfx.wad: %s is not a picture", as->src + 1);
			lw = rdi(d); lh = rdi(d + 4);
			if (lw < 1 || lh < 1 || (uint32_t) lw * (uint32_t) lh + 8 > size) die("gfx.wad: %s bad size", as->src + 1);
			d += 8;
		} else {
			const uint8_t *f = pak_find(pak, pak_len, as->src, &size);
			if (!f || size < 8) die("%s missing", as->src);
			lw = rdi(f); lh = rdi(f + 4);
			if (lw < 1 || lh < 1 || (uint32_t) lw * (uint32_t) lh + 8 > size) die("%s: bad picture", as->src);
			d = f + 8;
		}
		int w = as->w ? as->w : lw;
		int x0 = conchars ? 0 : as->x0;
		if (x0 + w > lw || w > PT || lh > PT) die("%s: crop past its width", as->src);
		a->w = w;
		a->h = lh;
		uint8_t *tmp = xmalloc((size_t) w * lh);
		for (int y = 0; y < lh; y++)
			memcpy(tmp + (size_t) y * w, d + (size_t) y * lw + x0, (size_t) w);
		aux_quantize(a, tmp, trans);
		free(tmp);
		for (size_t k = 0; k < sizeof AUX_BANDS / sizeof AUX_BANDS[0]; k++) {
			if (AUX_BANDS[k].id != as->id)
				continue;
			int h = 0;                                /* (its bands' rows, moved up) */
			for (int y = 0; y < lh; y++)
				if (AUX_BANDS[k].bands >> (y / MENU_ROWS) & 1)
					memmove(a->px + (size_t) h++ * w, a->px + (size_t) y * w, (size_t) w);
			a->h = h;
		}
	}
	/* MaxRects over the two pages: several orders and fits tried, the
	   first that places everything kept */
	long total = 0;
	int ok = 0;
	for (int attempt = 0; attempt < 12 && !ok; attempt++) {
		int sortkey = attempt % 4, fit = attempt / 4;   /* area, height, width, longer side; 3 fits */
		int order[QGA_COUNT];
		for (int i = 0; i < QGA_COUNT; i++) order[i] = i;
		for (int i = 1; i < QGA_COUNT; i++)
			for (int j = i; j > 0; j--) {
				const aux_img *x = &aux[order[j]], *y = &aux[order[j - 1]];
				int kx = sortkey == 0 ? x->w * x->h : sortkey == 1 ? x->h * 1024 + x->w
				       : sortkey == 2 ? x->w * 1024 + x->h : (x->w > x->h ? x->w : x->h) * 1024 + x->w * x->h;
				int ky = sortkey == 0 ? y->w * y->h : sortkey == 1 ? y->h * 1024 + y->w
				       : sortkey == 2 ? y->w * 1024 + y->h : (y->w > y->h ? y->w : y->h) * 1024 + y->w * y->h;
				if (kx <= ky) break;
				int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
			}
		memset(apage, 0, sizeof apage);
		memset(apage_rows, 0, sizeof apage_rows);
		for (int p = 0; p < AUX_PAGES; p++) {
			afree[p][0] = (rect) { 0, 0, PT, PT };
			nafree[p] = 1;
		}
		total = 0;
		ok = 1;
		for (int n = 0; n < QGA_COUNT && ok; n++) {
			aux_img *a = &aux[order[n]];
			int bp = -1, bi = -1, bs = 1 << 30, bl = 1 << 30;
			for (int p = 0; p < AUX_PAGES; p++)
				for (int i = 0; i < nafree[p]; i++) {
					const rect *f = &afree[p][i];
					if (a->w > f->w || a->h > f->h) continue;
					int ds, dl;
					if (fit == 0) {                        /* best short side */
						ds = f->w - a->w; dl = f->h - a->h;
						if (ds > dl) { int t = ds; ds = dl; dl = t; }
					} else if (fit == 1) {                 /* bottom-left */
						ds = f->y + a->h; dl = f->x;
					} else {                               /* best area */
						ds = f->w * f->h - a->w * a->h; dl = f->w - a->w < f->h - a->h ? f->w - a->w : f->h - a->h;
					}
					if (ds < bs || (ds == bs && dl < bl)) { bs = ds; bl = dl; bp = p; bi = i; }
				}
			if (bp < 0) {
				ok = 0;
				break;
			}
			rect at = { afree[bp][bi].x, afree[bp][bi].y, a->w, a->h };
			a->page = bp; a->u = at.x; a->v = at.y;
			total += (long) a->w * a->h;
			rect old[MAX_FREE];
			int nold = nafree[bp];
			memcpy(old, afree[bp], (size_t) nold * sizeof *old);
			nafree[bp] = 0;
			for (int i = 0; i < nold; i++) {
				rect f = old[i];
				if (at.x >= f.x + f.w || at.x + at.w <= f.x || at.y >= f.y + f.h || at.y + at.h <= f.y) {
					afree_add(bp, f);
					continue;
				}
				afree_add(bp, (rect) { f.x, f.y, at.x - f.x, f.h });
				afree_add(bp, (rect) { at.x + at.w, f.y, f.x + f.w - (at.x + at.w), f.h });
				afree_add(bp, (rect) { f.x, f.y, f.w, at.y - f.y });
				afree_add(bp, (rect) { f.x, at.y + at.h, f.w, f.y + f.h - (at.y + at.h) });
			}
			if (at.y + a->h > apage_rows[bp])
				apage_rows[bp] = at.y + a->h;
			for (int y = 0; y < a->h; y++)
				memcpy(&apage[bp][at.y + y][at.x], a->px + (size_t) y * a->w, (size_t) a->w);
		}
	}
	if (!ok)
		die("aux pictures: the two 4-bit pages are full");
	printf("qcook: %d aux pictures, %ld K texels in two 4-bit pages (%d + %d rows)\n", QGA_COUNT,
	       total / 1024, apage_rows[0], apage_rows[1]);
}

/* ------------------------------------------------------- a map's models -- */

/* An alias model cooked for a map (tools/qcook/items.c): the same
   preparation and records as GAME.DAT's, its skin placed among the map's
   textures instead of the game's pages. */
struct qc_mdl {
	model_spec spec;
	model_src  src;
	char       file[64];
	uint8_t   *bordered;            /* the skin with its border            */
	int        bw, bh;
	int        sort;                /* a monster's body: QSTM_SORT         */
	const struct frame_plan *fplan; /* its frames stored (QSTM_FMAP), NULL: all */
	const lod_plan *lplan;          /* its level of detail (QSTM_LOD), NULL: none */
};

void qc_set_palette(const uint8_t *pal)
{
	palette = pal;
}

qc_mdl *qc_mdl_load(const char *file, const uint8_t *data, uint32_t len, int skin, int skin_div, int flat)
{
	qc_mdl *q = xcalloc(1, sizeof *q);
	snprintf(q->file, sizeof q->file, "%s", file);
	q->spec.file     = q->file;
	q->spec.flat     = flat;
	q->spec.skin_div = skin_div;
	q->spec.scale[0] = q->spec.scale[1] = q->spec.scale[2] = 1;
	q->src.sp   = &q->spec;
	q->src.data = data;
	q->src.len  = len;
	mdl_skin_index = skin;
	model_prepare(&q->src);
	mdl_skin_index = 0;
	const image *im = q->src.im;
	int b = im->border;
	q->bw = im->w + 2 * b;
	q->bh = im->h + 2 * b;
	q->bordered = xmalloc((size_t) q->bw * q->bh);
	for (int y = 0; y < q->bh; y++)                 /* the border: the edge texels again */
		for (int x = 0; x < q->bw; x++) {
			int sx = x - b, sy = y - b;
			if (sx < 0) sx = 0;
			if (sx >= im->w) sx = im->w - 1;
			if (sy < 0) sy = 0;
			if (sy >= im->h) sy = im->h - 1;
			q->bordered[y * q->bw + x] = im->px[(size_t) sy * im->w + sx];
		}
	return q;
}

void qc_mdl_skin(const qc_mdl *q, int *w, int *h, const uint8_t **px)
{
	*w  = q->bw;
	*h  = q->bh;
	*px = q->bordered;
}

/* the model's frame `frame` alone (the finale's player, oldone.qc
   finale_4: a statue of its frame 1) */
void qc_mdl_one_frame(qc_mdl *q, int frame)
{
	mdl_t *m = &q->src.mdl;
	if (frame < 0 || frame >= m->nframes)
		die("%s: no frame %d of %d", q->file, frame, m->nframes);
	m->frames[0] = m->frames[frame];
	m->nframes   = 1;
}

int qc_mdl_flags(const qc_mdl *q)
{
	return q->src.mdl.flags;
}

const int qc_mdl_border = GP_BORDER;

void qc_mdl_cook(qc_mdl *q, int model_level, buf_t *geo, void *qg_model_out)
{
	qg_model *out = qg_model_out;
	image *im = q->src.im;
	im->u    = 0;                /* the layout's UVs: from the skin's corner */
	im->v    = 0;
	im->page = 0;
	cook_mdl(&q->src, model_level, (gbuf *) geo, out);
}

/* ------------------------------------------------- the compact format -- */

/* A streamed model (formats/qstream.h): qst_model, the frames as Quake's
   own vertex words, the records indexing them directly, the skin's images
   (its front and back halves when every triangle keeps to one, each with
   its own border); the whole asset appended to `out`. The model's scale
   and origin are kept for its GTE matrix; a big model's steps are expanded
   with a left shift so that the matrix (the view's 1.5 x the step) stays
   within the GTE's 4.12 (QSTM_MAX_STEP). */
#define QSTM_MAX_STEP 5.0          /* vertex units per expanded step, at most */

static uint32_t mdl_word(const tvert *tv)
{
	int n = tv->n < QG_NORMALS ? tv->n : 0;
	return (uint32_t) tv->v[0] | (uint32_t) tv->v[2] << 8 | (uint32_t) tv->v[1] << 16 | (uint32_t) n << 24;
}

/* a triangle's skin s at vertex k, the back face's onseam shift applied */
static int tri_s(const mdl_t *m, int t, int k)
{
	int vi = m->tris[t][k + 1];
	return m->st[vi][1] + (!m->tris[t][0] && m->st[vi][0] ? m->skinw / 2 : 0);
}

/* an image of the reduced skin: columns x0 .. x1 - 1, with a border of the
   edge texels, its width made even (one more edge column), and its count
   of VRAM halfwords too (two more): LoadImage's DMA sends n / 2 words for
   n halfwords, rounded up to its 16-word unit, so an odd n whose half is a
   multiple of 16 went one word short and the GPU took the next command's
   first word for the last pixels (gib1, dog, h_demon: 10 of the 212
   images) */
static void cut_image(const image *im, int x0, int x1, buf_t *pix, uint16_t *w, uint16_t *h)
{
	int b = im->border, iw = x1 - x0, bw = iw + 2 * b, bh = im->h + 2 * b;
	if (bw & 1) bw++;
	if ((bw / 2) & bh & 1) bw += 2;
	for (int y = 0; y < bh; y++)
		for (int x = 0; x < bw; x++) {
			int sx = x - b, sy = y - b;
			if (sx < 0) sx = 0;
			if (sx >= iw) sx = iw - 1;
			if (sy < 0) sy = 0;
			if (sy >= im->h) sy = im->h - 1;
			buf_put(pix, &im->px[(size_t) sy * im->w + x0 + sx], 1);
		}
	static const uint8_t zero[64];
	buf_put(pix, zero, (64 - (size_t) bw * bh % 64) % 64);   /* LoadImage's DMA unit */
	if (bw > 256 || bh > 256)
		die("%s: a skin image of %dx%d past a texture page", im->what, bw, bh);
	*w = (uint16_t) bw;
	*h = (uint16_t) bh;
}

/* A streamed model's triangles at a model level (formats/qlevels.h): a
   monster's body at every level, high included (2026-10-19: START's nine
   crucified zombies, 347 triangles each, filled the GPU's packet arena and
   cost a frame), half of them at high, a third at medium, a quarter at
   low (bounds of 64, 96, 128: the animation makes every collapse cost, a
   bound of 3 kept 84 % of the zombie's triangles, 12 kept 68 %);
   the rest (heads, gibs, projectiles, the items, the flames) as the map's
   own models (reduce_mdl: Quake's at high). The vertices no triangle uses
   any more leave the frames (the RAM pool's bytes, the expansion's work).
   Returns 1 with *out filled (its own arrays), 0: the model as it is;
   orig (m->nverts entries) receives each kept vertex's index in m. */
static int reduce_finish(const mdl_t *m, int (*tris)[4], int left, mdl_t *out, int *orig);

static int stream_reduce(const mdl_t *m, int model_level, int body, mdl_t *out, int *orig)
{
	double keep, err;
	if (body) {
		keep = model_level >= QL_HIGH ? 0.5 : model_level == QL_MEDIUM ? 0.35 : 0.25;
		err  = model_level >= QL_HIGH ? 64 : model_level == QL_MEDIUM ? 96 : 128;
	} else if (model_level >= QL_HIGH) {
		return 0;
	} else {
		keep = model_level == QL_LOW ? 0.5 : 0.75;
		err  = model_level == QL_LOW ? 3.0 : 1.5;
	}
	if (m->ntris < DECIM_MIN)
		return 0;
	int (*tris)[4] = xmalloc((size_t) m->ntris * sizeof *tris);
	memcpy(tris, m->tris, (size_t) m->ntris * sizeof *tris);
	double *pos = xmalloc((size_t) m->nframes * m->nverts * 3 * sizeof *pos);
	for (int f = 0; f < m->nframes; f++)
		for (int v = 0; v < m->nverts; v++)
			for (int c = 0; c < 3; c++)
				pos[((size_t) f * m->nverts + v) * 3 + c] = m->frames[f][v].v[c] * m->scale[c] + m->origin[c];
	double  *uv   = xmalloc((size_t) m->nverts * 2 * sizeof *uv);
	uint8_t *seam = xmalloc((size_t) m->nverts);
	for (int v = 0; v < m->nverts; v++) {
		uv[v * 2]     = m->st[v][1];
		uv[v * 2 + 1] = m->st[v][2];
		seam[v]       = m->st[v][0] != 0;
	}
	dmesh dm = { m->nverts, m->nframes, pos, uv, 0.25, seam, m->skinw };
	int left = decimate(&dm, tris, m->ntris, keep, err);
	free(pos);
	free(uv);
	free(seam);
	return reduce_finish(m, tris, left, out, orig);
}

/* a reduced model from its triangles: the vertices they still use, renumbered
   in order, the others out of the frames; orig[] each one's index in m */
static int reduce_finish(const mdl_t *m, int (*tris)[4], int left, mdl_t *out, int *orig)
{
	int *map = xmalloc((size_t) m->nverts * sizeof *map), nv = 0;
	for (int v = 0; v < m->nverts; v++)
		map[v] = -1;
	for (int t = 0; t < left; t++)
		for (int k = 1; k < 4; k++) {
			int v = tris[t][k];
			if (map[v] < 0)
				map[v] = nv++;
			tris[t][k] = map[v];
		}
	for (int v = 0; v < m->nverts; v++)
		if (map[v] >= 0)
			orig[map[v]] = v;
	*out        = *m;
	out->nverts = nv;
	out->ntris  = left;
	out->tris   = tris;
	out->st     = xmalloc((size_t) nv * sizeof *out->st);
	out->frames = xmalloc((size_t) m->nframes * sizeof *out->frames);
	tvert *tv   = xmalloc((size_t) m->nframes * nv * sizeof *tv);
	for (int v = 0; v < m->nverts; v++)
		if (map[v] >= 0)
			memcpy(out->st[map[v]], m->st[v], sizeof out->st[0]);
	for (int f = 0; f < m->nframes; f++) {
		out->frames[f] = tv + (size_t) f * nv;
		for (int v = 0; v < m->nverts; v++)
			if (map[v] >= 0)
				out->frames[f][map[v]] = m->frames[f][v];
	}
	free(map);
	return 1;
}

/* ---------------------------------------- the monsters by hand (§62, §64) -- */

/* A monster's body rebuilt by hand from Quake's own model (2026-10-22: simplified by hand from Quake's own models, the guns' way): half-edge
   collapses u -> v (u's triangles take v, the edge's own two go), in the
   table's order, each chosen one at a time against every frame of the
   model (the moved corner within 3 units of the new surface, its skin
   within 4 texels: docs/porting-notes.md §62; the boss 13 units, his
   vertices' grid four times coarser: §64), the parts that carry the look
   kept whole (the head, the hands, the feet, a weapon); the triangles no
   view of any frame shows dropped (from outside the model, zoomed till
   the triangle spans 24 pixels: never 3 of them seen, §64). The same
   model at every level (no decimation). */
typedef struct {
	const char     *file;
	const uint16_t (*cuts)[2];
	int             ncuts;
	const uint16_t *drop;          /* the file's triangles no view shows */
	int             ndrop;
} mob_hand;

/* progs/ogre.mdl: 326 -> 266 triangles, 169 -> 140 vertices; the grenade
   launcher (0-19), the head, the hands and the feet's soles whole */
static const uint16_t OGRE_CUTS[][2] = {
	{ 140, 153 }, { 154, 153 }, { 38, 42 }, { 157, 67 }, { 45, 41 }, { 32, 49 }, { 23, 51 }, { 74, 73 },
	{ 33, 35 }, { 104, 105 }, { 103, 114 }, { 113, 106 }, { 60, 117 }, { 137, 136 }, { 65, 66 }, { 64, 109 },
	{ 85, 110 }, { 123, 122 }, { 41, 40 }, { 148, 168 }, { 59, 129 }, { 69, 68 }, { 57, 56 }, { 161, 101 },
	{ 135, 102 }, { 70, 71 }, { 20, 50 }, { 67, 139 }, { 163, 68 },
};
static const uint16_t OGRE_DROP[] = { 110, 294 };

/* progs/soldier.mdl: 328 -> 270 triangles, 170 -> 141 vertices; the gun
   (9-24) and its muzzle flash (0-8: inside the body but in the shot), the
   head, the hands on the gun and the feet whole */
static const uint16_t SOLDIER_CUTS[][2] = {
	{ 141, 43 }, { 67, 132 }, { 150, 149 }, { 104, 155 }, { 138, 142 }, { 86, 87 }, { 83, 82 },
	{ 106, 45 }, { 140, 30 }, { 31, 76 }, { 98, 36 }, { 130, 128 }, { 105, 168 }, { 91, 90 },
	{ 161, 63 }, { 96, 33 }, { 78, 77 }, { 129, 128 }, { 162, 168 }, { 73, 74 }, { 100, 99 },
	{ 45, 44 }, { 160, 144 }, { 132, 131 }, { 79, 59 }, { 49, 107 }, { 39, 38 }, { 61, 62 },
	{ 133, 116 },
};

/* progs/dog.mdl: 426 -> 377 triangles, 236 -> 216 vertices; the teeth, the
   eyes and the tongue (0-61), the head and the paws whole */
static const uint16_t DOG_CUTS[][2] = {
	{ 91, 102 }, { 93, 92 }, { 106, 117 }, { 175, 90 }, { 139, 77 }, { 92, 117 }, { 118, 90 },
	{ 159, 86 }, { 176, 172 }, { 162, 168 }, { 155, 132 }, { 153, 127 }, { 163, 168 }, { 143, 144 },
	{ 131, 89 }, { 146, 147 }, { 158, 160 }, { 102, 82 }, { 77, 90 }, { 104, 191 },
};
static const uint16_t DOG_DROP[] = { 8, 15, 16, 17, 22, 76, 77, 99, 116 };

/* progs/knight.mdl: 213 -> 189 triangles, 108 -> 96 vertices; the sword,
   the hand on its hilt, the free hand, the head and the feet whole */
static const uint16_t KNIGHT_CUTS[][2] = {
	{ 18, 19 }, { 76, 102 }, { 8, 42 }, { 6, 2 }, { 39, 47 }, { 102, 75 }, { 24, 25 }, { 25, 53 },
	{ 73, 55 }, { 13, 14 }, { 48, 57 }, { 12, 43 },
};

/* progs/zombie.mdl: 347 -> 240 triangles, 177 -> 124 vertices; the flesh in
   its hand (0-11), the head, the hands and the feet whole */
static const uint16_t ZOMBIE_CUTS[][2] = {
	{ 45, 44 }, { 163, 112 }, { 168, 142 }, { 105, 106 }, { 75, 74 }, { 117, 110 }, { 116, 109 },
	{ 157, 90 }, { 113, 35 }, { 32, 97 }, { 107, 124 }, { 30, 148 }, { 176, 160 }, { 33, 65 },
	{ 156, 21 }, { 141, 73 }, { 160, 120 }, { 118, 119 }, { 154, 140 }, { 172, 53 }, { 86, 87 },
	{ 123, 115 }, { 155, 90 }, { 122, 112 }, { 153, 28 }, { 149, 158 }, { 148, 147 }, { 31, 29 },
	{ 24, 36 }, { 71, 138 }, { 28, 158 }, { 136, 137 }, { 108, 126 }, { 18, 66 }, { 110, 109 },
	{ 26, 25 }, { 46, 44 }, { 171, 170 }, { 51, 50 }, { 83, 82 }, { 68, 72 }, { 88, 89 },
	{ 161, 164 }, { 111, 50 }, { 48, 70 }, { 162, 53 }, { 65, 63 }, { 35, 73 }, { 142, 112 },
	{ 42, 44 }, { 120, 164 }, { 114, 119 }, { 15, 131 },
};
static const uint16_t ZOMBIE_DROP[] = { 108 };

/* progs/wizard.mdl: 157 -> 140 triangles, 80 -> 72 vertices; the head, the
   arms' spikes and the tail's tip whole */
static const uint16_t WIZARD_CUTS[][2] = {
	{ 22, 23 }, { 34, 27 }, { 70, 8 }, { 39, 40 }, { 13, 3 }, { 67, 66 }, { 69, 68 }, { 65, 72 },
};
static const uint16_t WIZARD_DROP[] = { 79 };

/* progs/demon.mdl: 275 -> 267 triangles, 143 -> 140 vertices; the horns
   (0-9), the head, the claws, the feet and the tail's tip whole */
static const uint16_t DEMON_CUTS[][2] = {
	{ 106, 98 }, { 35, 34 }, { 110, 120 },
};
static const uint16_t DEMON_DROP[] = { 10, 11 };

/* progs/shambler.mdl: 284 -> 278 triangles, 144 -> 141 vertices; the head,
   the claws and the feet whole */
static const uint16_t SHAMBLER_CUTS[][2] = {
	{ 112, 136 }, { 103, 97 }, { 138, 140 },
};

/* progs/boss.mdl: 555 -> 439 triangles, 325 -> 281 vertices; the head, the
   horns and the spikes (0-170) and the hands whole */
static const uint16_t BOSS_CUTS[][2] = {
	{ 324, 269 }, { 318, 222 }, { 228, 227 }, { 205, 186 }, { 250, 223 }, { 266, 247 }, { 291, 290 },
	{ 295, 275 }, { 229, 233 }, { 246, 247 }, { 322, 323 }, { 245, 315 }, { 259, 234 }, { 317, 306 },
	{ 222, 323 }, { 223, 248 }, { 279, 278 }, { 251, 252 }, { 227, 320 }, { 307, 276 }, { 207, 177 },
	{ 254, 313 }, { 313, 255 }, { 225, 306 }, { 192, 191 }, { 179, 178 }, { 303, 267 }, { 233, 278 },
	{ 184, 206 }, { 224, 314 }, { 310, 239 }, { 311, 235 }, { 171, 176 }, { 260, 320 }, { 238, 309 },
	{ 304, 242 }, { 316, 306 }, { 323, 221 }, { 234, 244 }, { 320, 232 }, { 273, 244 }, { 249, 315 },
	{ 321, 231 }, { 230, 231 },
};
static const uint16_t BOSS_DROP[] = {
	1, 4, 7, 10, 26, 27, 56, 57, 85, 88, 91, 94, 103, 106, 115, 118, 121, 133, 136, 139, 142, 145,
	148, 157, 160, 175, 178, 341,
};

static const mob_hand MOBS[] = {
	{ "progs/ogre.mdl",     OGRE_CUTS,     GUN_N(OGRE_CUTS),     OGRE_DROP,   GUN_N(OGRE_DROP) },
	{ "progs/soldier.mdl",  SOLDIER_CUTS,  GUN_N(SOLDIER_CUTS),  NULL,        0 },
	{ "progs/dog.mdl",      DOG_CUTS,      GUN_N(DOG_CUTS),      DOG_DROP,    GUN_N(DOG_DROP) },
	{ "progs/knight.mdl",   KNIGHT_CUTS,   GUN_N(KNIGHT_CUTS),   NULL,        0 },
	{ "progs/zombie.mdl",   ZOMBIE_CUTS,   GUN_N(ZOMBIE_CUTS),   ZOMBIE_DROP, GUN_N(ZOMBIE_DROP) },
	{ "progs/wizard.mdl",   WIZARD_CUTS,   GUN_N(WIZARD_CUTS),   WIZARD_DROP, GUN_N(WIZARD_DROP) },
	{ "progs/demon.mdl",    DEMON_CUTS,    GUN_N(DEMON_CUTS),    DEMON_DROP,  GUN_N(DEMON_DROP) },
	{ "progs/shambler.mdl", SHAMBLER_CUTS, GUN_N(SHAMBLER_CUTS), NULL,        0 },
	{ "progs/boss.mdl",     BOSS_CUTS,     GUN_N(BOSS_CUTS),     BOSS_DROP,   GUN_N(BOSS_DROP) },
};

/* The frames a body's QC never plays (Quake's frame numbers: the state
   tables of engine/m_*.c play every other one; the bench checks it at a
   map's start, engine/monsters.c): left out of its asset, with every frame
   equal to an earlier one (formats/qstream.h QSTM_FMAP, docs/porting-notes.md
   §67). QCOOK_NOFMAP=1 cooks every frame (an A / B). */
typedef struct {
	const char *file;
	uint8_t     unplayed[2][2];    /* first, count */
} mob_frames;

static const mob_frames MOB_FRAMES[] = {
	{ "progs/soldier.mdl",  { { 29, 11 } } },               /* load1-11 */
	{ "progs/ogre.mdl",     { { 136, 11 } } },              /* pull1-11 */
	{ "progs/knight.mdl",   { { 43, 1 }, { 72, 4 } } },     /* attackb1's second, standing2-5 */
	{ "progs/zombie.mdl",   { { 78, 1 } } },                /* attb14 (zombie_attb14 shows attb13) */
	{ "progs/wizard.mdl",   { { 8, 7 }, { 35, 7 } } },      /* hover9-15, magatt7-13 */
	{ "progs/shambler.mdl", { { 71, 2 } } },                /* magic7-8 */
	{ "progs/oldone.mdl",   { { 58, 1 } } },                /* shake13 */
};

typedef struct frame_plan {
	int       nframes, nstored;
	uint16_t *map;                 /* [nframes]: the frame's place among those
	                                  stored, QST_NOFRAME: never played      */
	int      *src;                 /* [nstored]: the frame each one is      */
} frame_plan;

/* the frames a body plays: played[f] (MOB_FRAMES's left out) */
static void mob_played_frames(const mdl_t *m, const char *file, uint8_t *played)
{
	memset(played, 1, (size_t) m->nframes);
	for (size_t i = 0; i < sizeof MOB_FRAMES / sizeof MOB_FRAMES[0]; i++)
		if (!strcmp(MOB_FRAMES[i].file, file))
			for (int r = 0; r < 2; r++)
				for (int k = 0; k < MOB_FRAMES[i].unplayed[r][1]; k++) {
					int f = MOB_FRAMES[i].unplayed[r][0] + k;
					if (f >= m->nframes)
						die("%s: frame %d never played, of %d", file, f, m->nframes);
					played[f] = 0;
				}
}

/* a body's frames stored: those played, each once (its vertices' words, in
   the shared numbering `order`, the nv the high level keeps: equal there,
   equal at every level); 0 when every frame would be stored */
static int frame_plan_make(const mdl_t *m, const char *file, const int *order, int nv, frame_plan *fp)
{
	if (getenv("QCOOK_NOFMAP"))
		return 0;
	uint8_t *played = xmalloc((size_t) m->nframes);
	mob_played_frames(m, file, played);
	fp->nframes = m->nframes;
	fp->nstored = 0;
	fp->map     = xmalloc((size_t) m->nframes * sizeof *fp->map);
	fp->src     = xmalloc((size_t) m->nframes * sizeof *fp->src);
	int dropped = 0, shared = 0;
	for (int f = 0; f < m->nframes; f++) {
		fp->map[f] = QST_NOFRAME;
		if (!played[f]) {
			dropped++;
			continue;
		}
		int same = -1;
		for (int i = 0; i < fp->nstored && same < 0; i++) {
			int g = fp->src[i], j = 0;
			while (j < nv && mdl_word(&m->frames[f][order[j]]) == mdl_word(&m->frames[g][order[j]]))
				j++;
			if (j == nv)
				same = i;
		}
		if (same >= 0) {
			fp->map[f] = (uint16_t) same;
			shared++;
		} else {
			fp->map[f] = (uint16_t) fp->nstored;
			fp->src[fp->nstored++] = f;
		}
	}
	free(played);
	if (fp->nstored == m->nframes) {
		free(fp->map);
		free(fp->src);
		return 0;
	}
	printf("qcook: %-22s %d of %d frames stored (%d never played, %d equal to an earlier one)\n", file,
	       fp->nstored, m->nframes, dropped, shared);
	return 1;
}

static const mob_hand *mob_hand_find(const char *file)
{
	if (getenv("QCOOK_NOHAND"))                   /* (the decimated one: an A / B) */
		return NULL;
	const char *only = getenv("QCOOK_HAND");      /* (these tables only: "progs/ogre.mdl,...") */
	if (only && !strstr(only, file))
		return NULL;
	for (int i = 0; i < GUN_N(MOBS); i++)
		if (!strcmp(MOBS[i].file, file))
			return &MOBS[i];
	return NULL;
}

/* the model a hand table makes, as stream_reduce gives one */
static int mob_hand_reduce(const mdl_t *m, const mob_hand *h, mdl_t *out, int *orig)
{
	int (*tris)[4] = xmalloc((size_t) m->ntris * sizeof *tris);
	uint8_t *alive = xmalloc((size_t) m->ntris);
	memcpy(tris, m->tris, (size_t) m->ntris * sizeof *tris);
	memset(alive, 1, (size_t) m->ntris);
	for (int i = 0; i < h->ndrop; i++) {
		if (h->drop[i] >= m->ntris)
			die("%s: the hand table drops triangle %d of %d", h->file, h->drop[i], m->ntris);
		alive[h->drop[i]] = 0;
	}
	for (int i = 0; i < h->ncuts; i++) {
		int u = h->cuts[i][0], v = h->cuts[i][1], edge = 0;
		if (u >= m->nverts || v >= m->nverts)
			die("%s: the hand table's cut %d -> %d past its %d vertices", h->file, u, v, m->nverts);
		for (int t = 0; t < m->ntris; t++) {
			if (!alive[t])
				continue;
			int hu = 0, hv = 0;
			for (int k = 1; k < 4; k++) {
				hu |= tris[t][k] == u;
				hv |= tris[t][k] == v;
			}
			if (hu && hv) {
				alive[t] = 0;                     /* the edge's own */
				edge = 1;
			} else if (hu) {
				for (int k = 1; k < 4; k++)
					if (tris[t][k] == u)
						tris[t][k] = v;
			}
		}
		if (!edge)
			die("%s: the hand table's cut %d -> %d is not an edge there", h->file, u, v);
	}
	int left = 0;
	for (int t = 0; t < m->ntris; t++)
		if (alive[t])
			memmove(tris[left++], tris[t], sizeof tris[0]);
	free(alive);
	return reduce_finish(m, tris, left, out, orig);
}

static void stream_reduce_free(mdl_t *r)
{
	free(r->tris);
	free(r->st);
	free(r->frames[0]);
	free(r->frames);
}

/* ------------------------- the level of detail by the vertices' order (§88) -- */

/* A body's plan (tools/qcook/lod.c) on its hand-made model, over the frames
   it plays, at QCOOK_LOD_PX pixels of tolerance (2 by default, the
   choice of §88) up to the draw distance; QCOOK_NOLOD=1 cooks today's order
   (an A / B). 0: none. */
#define LOD_DRAW_DIST (QST_LOD_BANDS * QST_LOD_BAND)   /* 1536: the game's draw distance */

static int lod_plan_body(const mdl_t *m, const char *file, lod_plan *plan)
{
	if (getenv("QCOOK_NOLOD"))
		return 0;
	const char *px = getenv("QCOOK_LOD_PX");
	double tol = px && *px ? atof(px) : 2.0;
	if (!(tol > 0))
		die("QCOOK_LOD_PX=%s: not a tolerance in pixels", px);
	uint8_t *played = xmalloc((size_t) m->nframes);
	mob_played_frames(m, file, played);
	int nf = 0;
	for (int f = 0; f < m->nframes; f++)
		nf += played[f];
	double *pos = xmalloc((size_t) nf * m->nverts * 3 * sizeof *pos);
	int (*st)[3] = xmalloc((size_t) m->nverts * sizeof *st);
	for (int f = 0, i = 0; f < m->nframes; f++) {
		if (!played[f])
			continue;
		for (int v = 0; v < m->nverts; v++)
			for (int k = 0; k < 3; k++)
				pos[((size_t) i * m->nverts + v) * 3 + k] = m->frames[f][v].v[k] * m->scale[k] + m->origin[k];
		i++;
	}
	for (int v = 0; v < m->nverts; v++)
		for (int k = 0; k < 3; k++)
			st[v][k] = m->st[v][k];
	lod_mesh mesh = { m->nverts, m->ntris, nf, pos, (const int (*)[4]) m->tris, (const int (*)[3]) st, m->skinw };
	lod_plan_make(&mesh, tol, LOD_DRAW_DIST, plan);
	free(pos);
	free(st);
	free(played);
	return 1;
}

/* a model's compact asset (formats/qstream.h qst_model) from its triangles
   and vertices as given (a level's, numbered for the shared asset) */
static void cook_compact(qc_mdl *q, const mdl_t *m, buf_t *out)
{
	model_src   *ms = &q->src;
	const image *im = ms->im;
	int div = ms->sp->skin_div;

	/* the expansion's shift: the smallest that keeps a step within bounds */
	double step_max = 0;
	for (int k = 0; k < 3; k++)
		if (m->scale[k] * QW_VSCALE > step_max)
			step_max = m->scale[k] * QW_VSCALE;
	int shift = 0;
	while (step_max / (1 << shift) > QSTM_MAX_STEP)
		shift++;
	if (shift > 7)
		die("%s: scale too large for the compact format", q->file);

	/* the records wind as the world's faces (the renderer leaves out a
	   model's triangles turned away from the eye, r_world.s) */
	int swap = mdl_wind(m) < 0;

	/* the halves: each triangle's s within one (Quake's skins: the front
	   on the left, the back on the right), else one image */
	int half = m->skinw / 2, two = 1;
	uint8_t *side = xcalloc((size_t) m->ntris, 1);
	for (int t = 0; t < m->ntris && two; t++) {
		int lo = 1 << 30, hi = -1;
		for (int k = 0; k < 3; k++) {
			int ss = tri_s(m, t, k);
			if (ss < lo) lo = ss;
			if (ss > hi) hi = ss;
		}
		if (hi < half)       side[t] = 0;
		else if (lo >= half) side[t] = 1;
		else                 two = 0;
	}
	if (!two)
		memset(side, 0, (size_t) m->ntris);
	/* the images' columns of the reduced skin: the front's [0, fx1), the
	   back's [bx0, w) (a texel the split crosses goes to both) */
	int fx1 = two ? (half + div - 1) / div : im->w, bx0 = two ? half / div : 0;
	if (fx1 > im->w) fx1 = im->w;

	/* records: each half's triangles in file order, preferring among the
	   next WINDOW the one sharing the most vertices with the record being
	   built; the indices are the model's own vertices. A sorted body
	   (QSTM_SORT) gets the level of detail's tables (§88): its slots and
	   triangles ordered by the band they last serve (q->lplan's, every
	   band without a plan), a table of their counts per band at its end */
	enum { WINDOW = 24 };
	buf_t recs = { 0 };
	uint8_t *done = xcalloc((size_t) m->ntris, 1);
	int nrec = 0;
	const lod_plan *lp = q->lplan;
	if (lp && (lp->ntris != m->ntris || lp->nverts != m->nverts))
		die("%s: a level of detail of %d triangles, %d vertices for %d, %d", q->file, lp->ntris, lp->nverts,
		    m->ntris, m->nverts);
	int lod_tris[QST_LOD_BANDS] = { 0 }, lod_slots[QST_LOD_BANDS] = { 0 };   /* over the records */
	uint8_t rg[REC_MAX_TRIS];                     /* the record's triangles' bands */
	#define EXP(f, vi, k) ((double) (m->frames[f][vi].v[k] << shift))
	#define VU(f, vi, k)  ((m->frames[f][vi].v[k] * m->scale[k] + m->origin[k]) * QW_VSCALE)
	for (int img = 0; img < (two ? 2 : 1); img++) {
		int left = 0, scan = 0;
		for (int t = 0; t < m->ntris; t++) {
			done[t] = side[t] != img;
			left += !done[t];
		}
		mrec r;
		memset(&r, 0, sizeof r);
		for (;;) {
			int best = -1, best_shared = -1;
			if (left) {
				while (done[scan]) scan++;
				for (int t = scan, seen = 0; t < m->ntris && seen < WINDOW; t++) {
					if (done[t]) continue;
					seen++;
					int shared = 0, newv = 0;
					for (int k = 1; k < 4; k++)
						if (rec_has(&r, m->tris[t][k])) shared++;
						else newv++;
					if (r.nv + newv > QW_MAX_VERTS) continue;
					if (shared > best_shared) { best_shared = shared; best = t; }
					if (!r.nt) break;
				}
			}
			if (best < 0 || r.nt >= REC_MAX_TRIS) {
				if (!r.nt)
					break;
				/* the level of detail's order and table (a sorted body): each
				   slot by the farthest band of its triangles, each triangle by
				   its band, descending (stable); the table's counts per band */
				uint8_t lodtab[QST_LOD_REC_BYTES];
				if (q->sort) {
					int sg[QW_MAX_VERTS], sperm[QW_MAX_VERTS], srank[QW_MAX_VERTS], tperm[REC_MAX_TRIS];
					for (int i = 0; i < r.nv; i++) {
						sg[i] = 0;
						sperm[i] = i;
					}
					for (int i = 0; i < r.nt; i++) {
						tperm[i] = i;
						for (int k = 0; k < 3; k++) {
							int sl = r.t[i].idx[k] / 4;
							if (rg[i] > sg[sl])
								sg[sl] = rg[i];
						}
					}
					for (int i = 1; i < r.nv; i++) {      /* (insertion sorts: stable) */
						int x = sperm[i], j = i;
						while (j > 0 && sg[sperm[j - 1]] < sg[x]) {
							sperm[j] = sperm[j - 1];
							j--;
						}
						sperm[j] = x;
					}
					for (int i = 1; i < r.nt; i++) {
						int x = tperm[i], j = i;
						while (j > 0 && rg[tperm[j - 1]] < rg[x]) {
							tperm[j] = tperm[j - 1];
							j--;
						}
						tperm[j] = x;
					}
					mrec r2 = r;
					uint8_t rg2[REC_MAX_TRIS];
					for (int i = 0; i < r.nv; i++) {
						srank[sperm[i]] = i;
						r2.mv[i] = r.mv[sperm[i]];
					}
					for (int i = 0; i < r.nt; i++) {
						r2.t[i] = r.t[tperm[i]];
						rg2[i]  = rg[tperm[i]];
						for (int k = 0; k < 3; k++)
							r2.t[i].idx[k] = (uint8_t) (srank[r.t[tperm[i]].idx[k] / 4] * 4);
					}
					r = r2;
					memcpy(rg, rg2, sizeof rg);
					for (int b = 0; b < QST_LOD_BANDS; b++) {
						int nv = 0, nt = 0;
						for (int i = 0; i < r.nv; i++)
							nv += sg[sperm[i]] > b;
						for (int i = 0; i < r.nt; i++)
							nt += rg[i] > b;
						lodtab[2 * b]     = (uint8_t) nv;
						lodtab[2 * b + 1] = (uint8_t) nt;
						lod_slots[b] += nv;
						lod_tris[b]  += nt;
					}
				}
				qw_record h;
				memset(&h, 0, sizeof h);
				h.num_verts = (uint8_t) r.nv;
				h.num_tris  = (uint8_t) r.nt;
				h.tex       = (uint16_t) img;
				int nidx    = (r.nv + 2) / 3 * 3;
				int ibytes  = (nidx * 2 + 3) & ~3;
				int cbytes  = (r.nv + 3) & ~3;
				h.polys     = (uint16_t) (sizeof h + ibytes + cbytes);
				h.size      = (uint16_t) (h.polys + r.nt * sizeof(qw_tri) + (q->sort ? QST_LOD_REC_BYTES : 0));
				/* the sphere: the box of its vertices over every frame in the
				   expanded units (its centre), the farthest in vertex units */
				double lo[3] = { 1e9, 1e9, 1e9 }, hi[3] = { -1e9, -1e9, -1e9 };
				for (int f = 0; f < m->nframes; f++)
					for (int i = 0; i < r.nv; i++)
						for (int k = 0; k < 3; k++) {
							double e = EXP(f, r.mv[i], k);
							if (e < lo[k]) lo[k] = e;
							if (e > hi[k]) hi[k] = e;
						}
				double cv[3];
				for (int k = 0; k < 3; k++) {
					h.sc[k] = (int16_t) lround((lo[k] + hi[k]) / 2);
					cv[k] = (h.sc[k] / (double) (1 << shift) * m->scale[k] + m->origin[k]) * QW_VSCALE;
				}
				double rad = 0;
				for (int f = 0; f < m->nframes; f++)
					for (int i = 0; i < r.nv; i++) {
						double d2 = 0;
						for (int k = 0; k < 3; k++) {
							double d = VU(f, r.mv[i], k) - cv[k];
							d2 += d * d;
						}
						if (d2 > rad * rad) rad = sqrt(d2);
					}
				h.sr = (uint16_t) ceil(rad + 1);
				buf_put(&recs, &h, sizeof h);
				uint16_t idx[QW_MAX_VERTS + 3] = { 0 };
				for (int i = 0; i < r.nv; i++)
					idx[i] = (uint16_t) r.mv[i];
				for (int i = r.nv; i < nidx; i++)
					idx[i] = idx[0];
				buf_put(&recs, idx, (size_t) ibytes);
				uint8_t col[QW_MAX_VERTS + 4];
				memset(col, 128, sizeof col);
				buf_put(&recs, col, (size_t) cbytes);
				buf_put(&recs, r.t, (size_t) r.nt * sizeof(qw_tri));
				if (q->sort)
					buf_put(&recs, lodtab, sizeof lodtab);
				nrec++;
				r.nv = r.nt = 0;
				continue;
			}
			int t = best;
			done[t] = 1;
			left--;
			int s[3];
			for (int k = 0; k < 3; k++)
				s[k] = rec_slot(&r, m->tris[t][k + 1]);
			rg[r.nt] = lp ? lp->group[t] : QST_LOD_BANDS;
			qw_tri *tri = &r.t[r.nt++];
			memset(tri, 0, sizeof *tri);
			int x0 = img ? bx0 : 0, x1 = img ? im->w : fx1;
			for (int k = 0; k < 3; k++) {
				/* the reduced skin's texel holding Quake's texel (s, t), from
				   its image's bordered corner */
				long u = tri_s(m, t, k) / div, v = m->st[m->tris[t][k + 1]][2] / div;
				if (u < x0) u = x0;
				if (u > x1 - 1) u = x1 - 1;
				if (v > im->h - 1) v = im->h - 1;
				int at = swap && k ? 3 - k : k;
				tri->idx[at] = (uint8_t) (s[k] * 4);
				tri->uv[at]  = (uint16_t) ((u - x0 + im->border) | (v + im->border) << 8);
			}
		}
	}
	#undef EXP
	#undef VU
	free(done);
	free(side);

	/* the fallback's skin: the same reduction at twice the step */
	model_spec sp2 = *ms->sp;
	sp2.skin_div = div * 2;
	model_src src2 = *ms;
	src2.sp = &sp2;
	model_prepare(&src2);
	const image *im2 = src2.im;
	int fx1q = two ? (half + 2 * div - 1) / (2 * div) : im2->w, bx0q = two ? half / (2 * div) : 0;
	if (fx1q > im2->w) fx1q = im2->w;

	/* the level of detail's block (formats/qstream.h qst_lod, a sorted
	   body): per band the vertices expanded, then the parents of the
	   vertices that collapse within the draw distance (a plan; without
	   one: every vertex at every band, no parent) */
	buf_t lodb = { 0 };
	int nmin = lp ? lp->nv[QST_LOD_BANDS - 1] : m->nverts;
	if (q->sort) {
		qst_lod l;
		memset(&l, 0, sizeof l);
		l.num_min = (uint16_t) nmin;
		for (int b = 0; b < QST_LOD_BANDS; b++)
			l.num_verts[b] = (uint16_t) (lp ? lp->nv[b] : m->nverts);
		buf_put(&lodb, &l, sizeof l);
		for (int v = nmin; v < m->nverts; v++) {
			if (lp->parent[v] >= v)
				die("%s: vertex %d's parent is %d", q->file, v, lp->parent[v]);
			buf_put(&lodb, &lp->parent[v], 2);
		}
		buf_align4(&lodb);
		if (lodb.len != qst_lod_bytes((uint32_t) m->nverts, (uint32_t) nmin))
			die("%s: a level of detail block of %zu bytes, not %u", q->file, lodb.len,
			    (unsigned) qst_lod_bytes((uint32_t) m->nverts, (uint32_t) nmin));
	}

	/* the asset: header, the frame map (QSTM_FMAP), the frames (those
	   stored), records, images */
	const frame_plan *fp = q->fplan;
	if (fp && fp->nframes != m->nframes)
		die("%s: a frame plan of %d frames for %d", q->file, fp->nframes, m->nframes);
	int nstored = fp ? fp->nstored : m->nframes;
	qst_model h;
	memset(&h, 0, sizeof h);
	h.num_frames  = (uint16_t) m->nframes;
	h.num_verts   = (uint16_t) m->nverts;
	h.num_records = (uint16_t) nrec;
	h.flags       = (uint16_t) ((ms->sp->flat ? QSTM_FLAT : 0) | (q->sort ? QSTM_SORT | QSTM_LOD : 0) |
	                            (fp ? QSTM_FMAP : 0) | (m->flags & 0xff) << 8);
	if (q->sort && nstored >= 2048)
		die("%s: %d frames stored, past the frame cache's key with a level of detail", q->file, nstored);
	h.ofs_frames  = sizeof h + qst_map_bytes(h.flags, (uint32_t) m->nframes) + (uint32_t) lodb.len;
	h.ofs_records = h.ofs_frames + (uint32_t) nstored * (uint32_t) m->nverts * 4;
	h.len_records = (uint32_t) recs.len;
	h.shift       = (uint8_t) shift;
	h.num_images  = (uint8_t) (two ? 2 : 1);
	double rad = 0;
	for (int k = 0; k < 3; k++) {
		h.scale[k]  = (int32_t) lround(m->scale[k] * QW_VSCALE * 65536.0 / (1 << shift));
		h.origin[k] = (int32_t) lround(m->origin[k] * QW_VSCALE * 16.0);
	}
	for (int f = 0; f < m->nframes; f++)
		for (int v = 0; v < m->nverts; v++) {
			double d2 = 0;
			for (int k = 0; k < 3; k++) {
				double d = (m->frames[f][v].v[k] * m->scale[k] + m->origin[k]) * QW_VSCALE;
				d2 += d * d;
			}
			if (d2 > rad * rad) rad = sqrt(d2);
		}
	h.radius = (uint16_t) (rad > 65000 ? 65000 : ceil(rad));
	buf_t pix = { 0 };
	uint32_t img_at = h.ofs_records + h.len_records;
	h.images[0].ofs = img_at;
	cut_image(im, 0, fx1, &pix, &h.images[0].w, &h.images[0].h);
	if (two) {
		h.images[1].ofs = img_at + (uint32_t) pix.len;
		h.images[1].x0  = (uint16_t) bx0;
		cut_image(im, bx0, im->w, &pix, &h.images[1].w, &h.images[1].h);
	}
	h.small[0].ofs = img_at + (uint32_t) pix.len;
	cut_image(im2, 0, fx1q, &pix, &h.small[0].w, &h.small[0].h);
	if (two) {
		h.small[1].ofs = img_at + (uint32_t) pix.len;
		h.small[1].x0  = (uint16_t) bx0q;
		cut_image(im2, bx0q, im2->w, &pix, &h.small[1].w, &h.small[1].h);
	}
	if (im->border != QST_BORDER || im2->border != QST_BORDER)
		die("%s: a skin border of %d, the engine's is %d", q->file, im->border, QST_BORDER);
	size_t start = out->len;
	buf_put(out, &h, sizeof h);
	if (fp) {
		static const uint8_t zero[4];
		buf_put(out, fp->map, (size_t) m->nframes * 2);
		buf_put(out, zero, qst_map_bytes(h.flags, (uint32_t) m->nframes) - (size_t) m->nframes * 2);
	}
	if (q->sort) {
		buf_put(out, lodb.p, lodb.len);
		free(lodb.p);
	}
	for (int i = 0; i < nstored; i++) {
		int f = fp ? fp->src[i] : i;
		for (int v = 0; v < m->nverts; v++) {
			uint32_t w = mdl_word(&m->frames[f][v]);
			buf_put(out, &w, 4);
		}
	}
	buf_put(out, recs.p, recs.len);
	buf_put(out, pix.p, pix.len);
	free(recs.p);
	free(pix.p);
	printf("qcook: %-22s %3d frames %4d verts %4d tris -> %3d records, skin %s %dx%d%s%dx%d (or %dx%d), "
	       "shift %d: %6zu B (%zu B of frames)\n", q->file, m->nframes, m->nverts, m->ntris, nrec,
	       two ? "halves" : "whole", h.images[0].w, h.images[0].h, two ? " + " : "", two ? h.images[1].w : 0,
	       two ? h.images[1].h : 0, h.small[0].w, h.small[0].h, shift, out->len - start,
	       (size_t) nstored * m->nverts * 4);
	if (lp) {
		printf("qcook: %-22s lod: %d collapses within %d units (errors to %.2f units), %d vertices with a parent, "
		       "%u + %d x %d B;", q->file, lp->nerr_dist, LOD_DRAW_DIST,
		       lp->nerr_dist ? lp->err[lp->nerr_dist - 1] : 0.0, m->nverts - nmin,
		       (unsigned) qst_lod_bytes((uint32_t) m->nverts, (uint32_t) nmin), nrec, QST_LOD_REC_BYTES);
		for (int b = 0; b < QST_LOD_BANDS; b++)
			printf(" %d/%d/%d", lp->nv[b], lod_slots[b], lod_tris[b]);
		printf(" (verts/slots/tris a band of %d units)\n", QST_LOD_BAND);
	}
}

/* The three model levels of a streamed model (formats/qstream.h
   qst_shared): each level's reduction (stream_reduce), nested (a lower
   level's vertices a subset of the higher's: the collapses go on in the
   same order); the vertices numbered by the lowest level keeping them,
   each level's asset cooked with that numbering (out[level]); the shared
   asset (shared) from them: the high level's frames, the levels' records,
   the images (the same at every level), checked against each level's
   asset through qst_assemble. */
void qc_mdl_cook_levels(qc_mdl *q, int body, buf_t out[3], buf_t *shared)
{
	const mdl_t *m = &q->src.mdl;
	mdl_t red[3];
	int *orig[3], nv[3], reduced[3];
	const mob_hand *hand = body ? mob_hand_find(q->file) : NULL;
	q->sort = body;                               /* (its triangles drawn by depth: QSTM_SORT) */
	for (int l = 0; l < 3; l++) {
		/* (a body without its hand table, QCOOK_NOHAND's A / B or a new
		   monster's first cook: the low level's reduction, the one level
		   on the disc since §52, at every level, as a hand-made one is
		   the same model at every level: the frame map and the level of
		   detail's tables are every level's) */
		orig[l] = xmalloc((size_t) m->nverts * sizeof *orig[l]);
		reduced[l] = hand ? mob_hand_reduce(m, hand, &red[l], orig[l])
		                  : stream_reduce(m, body ? 0 : l, body, &red[l], orig[l]);
		if (!reduced[l]) {                        /* the model as it is */
			red[l] = *m;
			for (int v = 0; v < m->nverts; v++)
				orig[l][v] = v;
		}
		nv[l] = red[l].nverts;
	}
	/* a hand-made body's level of detail (§88): the same model at every
	   level, its vertices numbered by their relevance */
	lod_plan lplan;
	q->lplan = hand && lod_plan_body(&red[0], q->file, &lplan) ? &lplan : NULL;
	/* the shared numbering: low's vertices in low's order, then medium's
	   others in medium's, then high's (a level of detail: its order) */
	int *rank = xmalloc((size_t) m->nverts * sizeof *rank), *order = xmalloc((size_t) m->nverts * sizeof *order);
	int n = 0, prefix[3];
	for (int v = 0; v < m->nverts; v++)
		rank[v] = -1;
	for (int l = 0; l < 3; l++) {
		for (int i = 0; i < nv[l]; i++) {
			int ov = orig[l][q->lplan ? q->lplan->perm[i] : i];
			if (rank[ov] < 0) {
				rank[ov] = n;
				order[n++] = ov;
			}
		}
		prefix[l] = n;
		if (prefix[l] != nv[l])
			die("%s: the model levels' vertices are not nested (level %d: %d of %d)", q->file, l, nv[l],
			    prefix[l]);
	}
	frame_plan plan;                              /* a body's frames stored (QSTM_FMAP) */
	q->fplan = body && frame_plan_make(m, q->file, order, nv[2], &plan) ? &plan : NULL;
	for (int l = 0; l < 3; l++) {
		mdl_t lv = red[l];
		lv.nverts = nv[l];
		lv.st     = xmalloc((size_t) nv[l] * sizeof *lv.st);
		lv.tris   = xmalloc((size_t) red[l].ntris * sizeof *lv.tris);
		lv.frames = xmalloc((size_t) m->nframes * sizeof *lv.frames);
		tvert *tv = xmalloc((size_t) m->nframes * nv[l] * sizeof *tv);
		for (int j = 0; j < nv[l]; j++)
			memcpy(lv.st[j], m->st[order[j]], sizeof lv.st[0]);
		for (int f = 0; f < m->nframes; f++) {
			lv.frames[f] = tv + (size_t) f * nv[l];
			for (int j = 0; j < nv[l]; j++)
				lv.frames[f][j] = m->frames[f][order[j]];
		}
		for (int t = 0; t < red[l].ntris; t++) {
			lv.tris[t][0] = red[l].tris[t][0];
			for (int k = 1; k < 4; k++)
				lv.tris[t][k] = rank[orig[l][red[l].tris[t][k]]];
		}
		memset(&out[l], 0, sizeof out[l]);
		cook_compact(q, &lv, &out[l]);
		if (reduced[l])
			printf("qcook: %-22s %s: %d of %d tris, %d of %d verts%s\n", q->file, level_name[l], lv.ntris,
			       m->ntris, lv.nverts, m->nverts, hand ? " (by hand)" : "");
		free(lv.st);
		free(lv.tris);
		free(tv);
		free(lv.frames);
	}
	/* the shared asset */
	const qst_model *h[3];
	for (int l = 0; l < 3; l++)
		h[l] = (const qst_model *) out[l].p;
	uint32_t img_len = (uint32_t) out[2].len - (h[2]->ofs_records + h[2]->len_records);
	qst_shared sh;
	memset(&sh, 0, sizeof sh);
	sh.num_frames = h[2]->num_frames;
	sh.num_verts  = h[2]->num_verts;
	sh.flags      = h[2]->flags;
	sh.shift      = h[2]->shift;
	sh.num_images = h[2]->num_images;
	memcpy(sh.scale, h[2]->scale, sizeof sh.scale);
	memcpy(sh.origin, h[2]->origin, sizeof sh.origin);
	uint32_t img2 = h[2]->ofs_records + h[2]->len_records;
	for (int k = 0; k < 2; k++) {
		sh.images[k] = h[2]->images[k];
		sh.small[k]  = h[2]->small[k];
		if (k < sh.num_images) {
			sh.images[k].ofs -= img2;
			sh.small[k].ofs  -= img2;
		}
	}
	buf_t sb = { 0 };
	buf_put(&sb, &sh, sizeof sh);
	if (q->fplan) {                               /* the frame map: every level's */
		sh.ofs_map    = (uint32_t) sb.len;
		sh.num_stored = (uint16_t) q->fplan->nstored;
		buf_put(&sb, out[2].p + sizeof(qst_model), qst_map_bytes(sh.flags, sh.num_frames));
	}
	if (sh.flags & QSTM_LOD) {                    /* the level of detail: every level's */
		uint32_t at = (uint32_t) sizeof(qst_model) + qst_map_bytes(sh.flags, sh.num_frames);
		sh.ofs_lod = (uint32_t) sb.len;
		sh.len_lod = h[2]->ofs_frames - at;
		buf_put(&sb, out[2].p + at, sh.len_lod);
	}
	sh.ofs_frames = (uint32_t) sb.len;
	buf_put(&sb, out[2].p + h[2]->ofs_frames, (size_t) qst_stored_frames(h[2]) * h[2]->num_verts * 4);
	for (int l = 0; l < 3; l++) {
		const uint8_t *r = out[l].p + h[l]->ofs_records;
		int same = -1;
		for (int k = 0; k < l && same < 0; k++)
			if (h[k]->len_records == h[l]->len_records &&
			    !memcmp(out[k].p + h[k]->ofs_records, r, h[l]->len_records))
				same = k;
		sh.level[l].num_verts   = h[l]->num_verts;
		sh.level[l].num_records = h[l]->num_records;
		sh.level[l].len_records = h[l]->len_records;
		sh.level[l].radius      = h[l]->radius;
		if (same >= 0) {
			sh.level[l].ofs_records = sh.level[same].ofs_records;
		} else {
			sh.level[l].ofs_records = (uint32_t) sb.len;
			buf_put(&sb, r, h[l]->len_records);
		}
	}
	sh.ofs_images = (uint32_t) sb.len;
	sh.len_images = img_len;
	buf_put(&sb, out[2].p + img2, img_len);
	memcpy(sb.p, &sh, sizeof sh);
	/* every level's asset as the engine will make it from the shared one */
	for (int l = 0; l < 3; l++) {
		uint32_t imgl = h[l]->ofs_records + h[l]->len_records;
		if (out[l].len - imgl != img_len || memcmp(out[l].p + imgl, out[2].p + img2, img_len))
			die("%s: the %s level's images are not the high level's", q->file, level_name[l]);
		qst_model lm;
		qst_layout lay;
		uint32_t size = qst_level_model(&sh, l, &lm);
		qst_layout_of(&sh, l, &lay);
		uint8_t *a = xcalloc(size, 1);
		memcpy(a, &lm, sizeof lm);
		for (uint32_t ofs = 0; ofs < sb.len; ofs += QST_SECTOR) {   /* (sector by sector, as captured) */
			uint32_t piece = sb.len - ofs < QST_SECTOR ? (uint32_t) (sb.len - ofs) : QST_SECTOR;
			qst_assemble(&lay, ofs, sb.p + ofs, piece, a);
		}
		if (size != out[l].len || memcmp(a, out[l].p, size))
			die("%s: the %s level's asset made from the shared one differs", q->file, level_name[l]);
		free(a);
	}
	*shared = sb;
	if (q->fplan) {
		free(plan.map);
		free(plan.src);
		q->fplan = NULL;
	}
	if (q->lplan) {
		lod_plan_free(&lplan);
		q->lplan = NULL;
	}
	for (int l = 0; l < 3; l++) {
		if (reduced[l])
			stream_reduce_free(&red[l]);
		free(orig[l]);
	}
	free(rank);
	free(order);
}

/* ------------------------------------------------------------------ main -- */

/* ------------------------------------------------------------- finales -- */

/* The episodes' end texts are QuakeC strings: client.qc's ExitIntermission
   sends them with SVC_FINALE. They are read from the pak's progs.dat by
   walking that function's statements: each `world.model == "maps/..."`
   test (EQ_S then IFNOT) guards a block, each WriteString argument (a
   constant stored into OFS_PARM1) with a line break in it is a text
   of the innermost block around it; e1m7's block holds two, the shareware
   game's then the registered one's; the text outside every block is the
   four runes' (intermission_running 3). */
#define PR_OP_DONE    0
#define PR_OP_EQ_S    12
#define PR_OP_STORE_F 31
#define PR_OP_STORE_S 33
#define PR_OP_IFNOT   50
#define PR_OFS_PARM1  7

typedef struct { int first, end; char map[12]; int texts; } fin_block;

static void finales_cook(const uint8_t *pak, long pak_len, gbuf *table, gbuf *texts, uint32_t *num)
{
	uint32_t len;
	const uint8_t *d = pak_find(pak, pak_len, "progs.dat", &len);
	*num = 0;
	if (!d || len < 60) {
		printf("qcook: no progs.dat: no episode end texts\n");
		return;
	}
	const int32_t *hd = (const int32_t *) d;
	int32_t ofs_st = hd[2], nst = hd[3], ofs_fn = hd[8], nfn = hd[9], ofs_str = hd[10], nstr = hd[11];
	int32_t ofs_gl = hd[12], ngl = hd[13];
	if (hd[0] != 6 || ofs_st + nst * 8 > (int32_t) len || ofs_fn + nfn * 36 > (int32_t) len ||
	    ofs_str + nstr > (int32_t) len || ofs_gl + ngl * 4 > (int32_t) len)
		die("progs.dat: not a version 6 progs");
	const char *str = (const char *) d + ofs_str;
	const int32_t *gl = (const int32_t *) (d + ofs_gl);
	const uint8_t *st = d + ofs_st;
	int first = -1;
	for (int i = 0; i < nfn; i++) {
		const int32_t *fn = (const int32_t *) (d + ofs_fn + i * 36);
		if (fn[4] >= 0 && fn[4] < nstr && !strcmp(str + fn[4], "ExitIntermission"))
			first = fn[0];
	}
	if (first <= 0) {
		printf("qcook: progs.dat has no ExitIntermission: no episode end texts\n");
		return;
	}
	#define ST_OP(i) (*(const uint16_t *) (st + (i) * 8))
	#define ST_A(i)  (*(const int16_t *) (st + (i) * 8 + 2))
	#define ST_B(i)  (*(const int16_t *) (st + (i) * 8 + 4))
	/* a global's constant string ("" when it is none) */
	#define GSTR(g)  ((g) >= 0 && (g) < ngl && gl[g] > 0 && gl[g] < nstr ? str + gl[g] : "")
	fin_block blocks[16];
	int nb = 0, last = first;
	while (last < nst && ST_OP(last) != PR_OP_DONE)
		last++;
	for (int i = first; i < last; i++) {
		if (ST_OP(i) != PR_OP_EQ_S || ST_OP(i + 1) != PR_OP_IFNOT || nb == 16)
			continue;
		const char *m = GSTR(ST_A(i));
		if (strncmp(m, "maps/", 5))
			m = GSTR(ST_B(i));
		if (strncmp(m, "maps/", 5))
			continue;
		fin_block *b = &blocks[nb++];
		b->first = i + 2;
		b->end   = i + 1 + ST_B(i + 1);
		b->texts = 0;
		int k = 0;
		for (const char *p = m + 5; *p && *p != '.' && k < 11; p++)
			b->map[k++] = (char) (*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
		b->map[k] = 0;
	}
	for (int i = first; i < last; i++) {
		if (ST_OP(i) < PR_OP_STORE_F || ST_OP(i) > PR_OP_STORE_S || ST_B(i) != PR_OFS_PARM1)
			continue;                             /* (qcc copies a parameter with STORE_V) */
		const char *t = GSTR(ST_A(i));
		if (strlen(t) < 40 || !strchr(t, '\n'))
			continue;
		fin_block *in = 0;
		for (int b = 0; b < nb; b++)
			if (i >= blocks[b].first && i < blocks[b].end &&
			    (!in || blocks[b].end - blocks[b].first < in->end - in->first))
				in = &blocks[b];
		qg_finale fz;
		memset(&fz, 0, sizeof fz);
		if (in) {
			memcpy(fz.map, in->map, sizeof fz.map);
			fz.flags = in->texts++ == 0 ? 0 : QGFIN_REGISTERED;
		} else {
			fz.flags = QGFIN_RUNES;
		}
		fz.len = (uint16_t) strlen(t);
		fz.ofs = (uint32_t) texts->len;
		gput(texts, t, strlen(t) + 1);
		gput(table, &fz, sizeof fz);
		(*num)++;
	}
	/* oldone.qc's finale_4: Shub-Niggurath's death's text (SVC_FINALE),
	   END's own */
	int f4 = -1;
	for (int i = 0; i < nfn; i++) {
		const int32_t *fn = (const int32_t *) (d + ofs_fn + i * 36);
		if (fn[4] >= 0 && fn[4] < nstr && !strcmp(str + fn[4], "finale_4"))
			f4 = fn[0];
	}
	for (int i = f4; f4 > 0 && i < nst && ST_OP(i) != PR_OP_DONE; i++) {
		if (ST_OP(i) < PR_OP_STORE_F || ST_OP(i) > PR_OP_STORE_S || ST_B(i) != PR_OFS_PARM1)
			continue;
		const char *t = GSTR(ST_A(i));
		if (strlen(t) < 40 || !strchr(t, '\n'))
			continue;
		qg_finale fz;
		memset(&fz, 0, sizeof fz);
		memcpy(fz.map, "END", 4);
		fz.len = (uint16_t) strlen(t);
		fz.ofs = (uint32_t) texts->len;
		gput(texts, t, strlen(t) + 1);
		gput(table, &fz, sizeof fz);
		(*num)++;
		break;
	}
	/* a block with two texts: its first is the shareware game's */
	for (uint32_t i = 0; i < *num; i++) {
		qg_finale *fz = (qg_finale *) table->p + i;
		if (fz->flags == 0 && i + 1 < *num && !strcmp(fz[1].map, fz->map) && fz[1].flags == QGFIN_REGISTERED)
			fz->flags = QGFIN_SHAREWARE;
	}
	for (uint32_t i = 0; i < *num; i++) {
		const qg_finale *fz = (const qg_finale *) table->p + i;
		printf("qcook: finale %-5s %s%s%s %u chars\n", fz->map[0] ? fz->map : "-",
		       fz->flags & QGFIN_SHAREWARE ? "shareware" : "", fz->flags & QGFIN_REGISTERED ? "registered" : "",
		       fz->flags & QGFIN_RUNES ? "runes" : "", fz->len);
	}
	#undef ST_OP
	#undef ST_A
	#undef ST_B
	#undef GSTR
}

/* -------------------------------------------------------- the 2D weapons -- */

/* The view models as pictures (Graphics: "Draw weapon: 2D", the
   idea: a flat image costs a quad where the model costs its triangles).
   Every frame of every v_*.mdl rendered as the eye sees it in the status
   bar's viewport (the projection's centre (160, 96), H 160: render.c; 240
   rows drawn, the full-screen viewport shows the gun's lower part), the
   gun at rest (at the eye, the viewport's 2 units up: the engine adds the
   bob's forward part, the kick and the lean), the way Quake draws an
   alias model: z-buffered, the triangles wound backwards dropped (the
   model's own winding, as cook_mdl reads it), clipped at 5 units
   (ALIAS_Z_CLIP_PLANE), each vertex lit by the light table of yaw 0 at
   QG_WS_L0 (the engine scales the colour for its own light), the texels
   of the full skin, perspective-correct. Then halved (2 x 2 samples: two
   covered make a texel, their mean its colour; the view models' skins are
   halved too), trimmed to its texels with a transparent margin of one,
   and a weapon's frames quantized together to 255 colours in the GPU's
   15 bits (median cut; index 0 transparent, the others with the STP bit:
   black stays opaque). */

#define WS_W     320
#define WS_H     216          /* (the status bar's viewport is 192 rows: the rows
                                 under it are the full screen's, the gun 24 + px
                                 lower there) */
#define WS_CX    160
#define WS_CY    96
#define WS_PROJ  160.0
#define WS_NEAR  5.0
#define WS_UP    2.0          /* the status bar's viewport fudge (game.c) */
#define WS_MAX   80           /* a frame larger (texels, either side) stays 3D: the
                                 axe's swings, which sweep the screen next to the
                                 eye, would take a slot four times the others' */

typedef struct { double x, y, z, s, t, l; } wsv;   /* view space (x right, y down, z ahead) */

typedef struct {
	int      x, y, w, h;      /* full-res top-left from the centre; half-res size */
	uint16_t *rgb;            /* w x h, 15-bit colour | 0x8000, 0 transparent */
	uint8_t  *px;             /* the indices, after quantizing */
} ws_frame;

typedef struct {
	int       nframes;
	ws_frame *frames;
	uint16_t  clut[256];
	double    anchor;
} ws_weapon;

static ws_weapon wsw[QG_WEAPONS];
static image    *ws_slot;

static float    ws_iz[WS_H][WS_W];          /* 1 / z, 0 empty */
static float    ws_rgb[WS_H][WS_W][3];

static int ws_clip(const wsv *in, int n, wsv *out)
{
	int m = 0;
	for (int i = 0; i < n; i++) {
		const wsv *a = &in[i], *b = &in[(i + 1) % n];
		int ina = a->z >= WS_NEAR, inb = b->z >= WS_NEAR;
		if (ina)
			out[m++] = *a;
		if (ina != inb) {
			double f = (WS_NEAR - a->z) / (b->z - a->z);
			wsv c = { a->x + f * (b->x - a->x), a->y + f * (b->y - a->y), WS_NEAR,
			          a->s + f * (b->s - a->s), a->t + f * (b->t - a->t), a->l + f * (b->l - a->l) };
			out[m++] = c;
		}
	}
	return m;
}

static void ws_tri(const wsv *v0, const wsv *v1, const wsv *v2, const mdl_t *m)
{
	const wsv *v[3] = { v0, v1, v2 };
	double sx[3], sy[3], iz[3], sz[3], tz[3], l[3];
	for (int k = 0; k < 3; k++) {
		iz[k] = 1.0 / v[k]->z;
		sx[k] = WS_CX + WS_PROJ * v[k]->x * iz[k];
		sy[k] = WS_CY + WS_PROJ * v[k]->y * iz[k];
		sz[k] = v[k]->s * iz[k];
		tz[k] = v[k]->t * iz[k];
		l[k]  = v[k]->l;
	}
	double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
	if (fabs(area) < 1e-9)
		return;
	int x0 = (int) floor(fmin(sx[0], fmin(sx[1], sx[2]))), x1 = (int) ceil(fmax(sx[0], fmax(sx[1], sx[2])));
	int y0 = (int) floor(fmin(sy[0], fmin(sy[1], sy[2]))), y1 = (int) ceil(fmax(sy[0], fmax(sy[1], sy[2])));
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > WS_W - 1) x1 = WS_W - 1;
	if (y1 > WS_H - 1) y1 = WS_H - 1;
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			double px = x + 0.5, py = y + 0.5;
			double w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
			double w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
			double w2 = 1 - w0 - w1;
			if (w0 < 0 || w1 < 0 || w2 < 0)
				continue;
			double izp = w0 * iz[0] + w1 * iz[1] + w2 * iz[2];
			if (izp <= ws_iz[y][x])
				continue;
			double s = (w0 * sz[0] + w1 * sz[1] + w2 * sz[2]) / izp;
			double t = (w0 * tz[0] + w1 * tz[1] + w2 * tz[2]) / izp;
			double lt = w0 * l[0] + w1 * l[1] + w2 * l[2];   /* (screen-linear, as the GPU's) */
			int si = (int) floor(s), ti = (int) floor(t);
			si %= m->skinw;
			if (si < 0) si += m->skinw;
			if (ti < 0) ti = 0;
			if (ti >= m->skinh) ti = m->skinh - 1;
			int c = m->skin[(size_t) ti * m->skinw + si];
			ws_iz[y][x] = (float) izp;
			for (int k = 0; k < 3; k++)
				ws_rgb[y][x][k] = (float) (palette[c * 3 + k] * lt / 128);
		}
}

/* one frame: the full-res render, then halved and trimmed */
static void ws_render(const mdl_t *m, int f, double wind, const uint8_t *dots0, ws_frame *out)
{
	memset(ws_iz, 0, sizeof ws_iz);
	wsv *vv = xmalloc((size_t) m->nverts * sizeof *vv);
	double (*mp)[3] = xmalloc((size_t) m->nverts * sizeof *mp);
	for (int i = 0; i < m->nverts; i++) {
		const tvert *tv = &m->frames[f][i];
		for (int k = 0; k < 3; k++)
			mp[i][k] = tv->v[k] * m->scale[k] + m->origin[k];
		mp[i][2] += WS_UP;
		vv[i].x = -mp[i][1];                     /* model x ahead, y left, z up */
		vv[i].y = -mp[i][2];
		vv[i].z =  mp[i][0];
		int ni = tv->n < QG_NORMALS ? tv->n : 0;
		vv[i].l = (dots0[ni] * QG_WS_L0) >> 7;   /* (light_recs' integer light) */
	}
	for (int t = 0; t < m->ntris; t++) {
		const int *tr = m->tris[t];
		double a[3], b[3], n[3];
		for (int k = 0; k < 3; k++) {
			a[k] = mp[tr[2]][k] - mp[tr[1]][k];
			b[k] = mp[tr[3]][k] - mp[tr[1]][k];
		}
		n[0] = a[1] * b[2] - a[2] * b[1];
		n[1] = a[2] * b[0] - a[0] * b[2];
		n[2] = a[0] * b[1] - a[1] * b[0];
		double d = -(n[0] * mp[tr[1]][0] + n[1] * mp[tr[1]][1] + n[2] * mp[tr[1]][2]);   /* eye at 0 */
		if (wind < 0) d = -d;
		if (d <= 0)
			continue;                            /* wound backwards: Quake skips it */
		wsv in[3], cl[4];
		for (int k = 0; k < 3; k++) {
			int vi = tr[k + 1];
			in[k] = vv[vi];
			in[k].s = m->st[vi][1] + (m->st[vi][0] && !tr[0] ? m->skinw / 2 : 0);   /* onseam */
			in[k].t = m->st[vi][2];
		}
		int nc = ws_clip(in, 3, cl);
		for (int k = 1; k + 1 < nc; k++)
			ws_tri(&cl[0], &cl[k], &cl[k + 1], m);
	}
	free(vv);
	free(mp);
	if (getenv("QCOOK_WSDUMP")) {                /* (debug: the full-res render, PPM) */
		char path[512];
		static int nd;
		snprintf(path, sizeof path, "%s/ws%02d.ppm", getenv("QCOOK_WSDUMP"), nd++);
		FILE *fp = fopen(path, "wb");
		if (fp) {
			fprintf(fp, "P6 %d %d 255\n", WS_W, WS_H);
			for (int y = 0; y < WS_H; y++)
				for (int x = 0; x < WS_W; x++) {
					uint8_t c[3];
					for (int k = 0; k < 3; k++)
						c[k] = ws_iz[y][x] > 0 ? (uint8_t) fmin(255, ws_rgb[y][x][k]) : (uint8_t) (k == 2 ? 90 : 40);
					fwrite(c, 1, 3, fp);
				}
			fclose(fp);
		}
	}

	/* halved: a texel from its 2 x 2 samples, two covered at least */
	static uint16_t half[WS_H / 2][WS_W / 2];
	int bx0 = WS_W, by0 = WS_H, bx1 = -1, by1 = -1;
	for (int y = 0; y < WS_H / 2; y++)
		for (int x = 0; x < WS_W / 2; x++) {
			double c[3] = { 0, 0, 0 };
			int n = 0;
			for (int dy = 0; dy < 2; dy++)
				for (int dx = 0; dx < 2; dx++)
					if (ws_iz[2 * y + dy][2 * x + dx] > 0) {
						for (int k = 0; k < 3; k++) c[k] += ws_rgb[2 * y + dy][2 * x + dx][k];
						n++;
					}
			half[y][x] = 0;
			if (n < 2)
				continue;
			int r = (int) lround(c[0] / n), g = (int) lround(c[1] / n), bl = (int) lround(c[2] / n);
			if (r > 255) r = 255;
			if (g > 255) g = 255;
			if (bl > 255) bl = 255;
			half[y][x] = (uint16_t) (0x8000 | (r >> 3) | (g >> 3) << 5 | (bl >> 3) << 10);
			if (x < bx0) bx0 = x;
			if (x > bx1) bx1 = x;
			if (y < by0) by0 = y;
			if (y > by1) by1 = y;
		}
	if (bx1 < 0) {                               /* nothing in front of the eye */
		out->x = out->y = 0;
		out->w = 2;
		out->h = 1;
		out->rgb = xcalloc(2, sizeof *out->rgb);
		return;
	}
	bx0--; by0--; bx1++; by1++;                  /* the transparent margin */
	int w = bx1 - bx0 + 1, h = by1 - by0 + 1;
	w += w & 1;                                  /* (whole halfwords) */
	out->x = 2 * bx0 - WS_CX;
	out->y = 2 * by0 - WS_CY;
	out->w = w;
	out->h = h;
	out->rgb = xcalloc((size_t) w * h, sizeof *out->rgb);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int sx = bx0 + x, sy = by0 + y;
			if (sx >= 0 && sx < WS_W / 2 && sy >= 0 && sy < WS_H / 2)
				out->rgb[y * w + x] = half[sy][sx];
		}
}

/* median cut over a weapon's frames' 15-bit colours */
typedef struct { uint16_t c; uint32_t n; } ws_hist;
static int ws_axis;
static int ws_by_axis(const void *a, const void *b)
{
	int ca = (((const ws_hist *) a)->c >> (5 * ws_axis)) & 31, cb = (((const ws_hist *) b)->c >> (5 * ws_axis)) & 31;
	return ca - cb;
}

static void ws_quantize(ws_weapon *w)
{
	static uint32_t cnt[32768];
	memset(cnt, 0, sizeof cnt);
	for (int f = 0; f < w->nframes; f++)              /* (a 3D frame: w 0) */
		for (int i = 0; i < w->frames[f].w * w->frames[f].h; i++)
			if (w->frames[f].rgb[i])
				cnt[w->frames[f].rgb[i] & 0x7fff]++;
	ws_hist *hs = xmalloc(32768 * sizeof *hs);
	int nh = 0;
	for (int c = 0; c < 32768; c++)
		if (cnt[c]) { hs[nh].c = (uint16_t) c; hs[nh].n = cnt[c]; nh++; }
	typedef struct { int a, b; } box;            /* hs[a .. b) */
	box boxes[255];
	int nb = 1;
	boxes[0] = (box) { 0, nh };
	while (nb < 255) {
		int best = -1, bax = 0;
		double bscore = 0;
		for (int i = 0; i < nb; i++) {
			if (boxes[i].b - boxes[i].a < 2) continue;
			int lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
			uint32_t pop = 0;
			for (int j = boxes[i].a; j < boxes[i].b; j++) {
				for (int k = 0; k < 3; k++) {
					int v = (hs[j].c >> (5 * k)) & 31;
					if (v < lo[k]) lo[k] = v;
					if (v > hi[k]) hi[k] = v;
				}
				pop += hs[j].n;
			}
			int ax = 0;
			for (int k = 1; k < 3; k++)
				if (hi[k] - lo[k] > hi[ax] - lo[ax]) ax = k;
			double score = (double) (hi[ax] - lo[ax]) * sqrt((double) pop);
			if (hi[ax] > lo[ax] && score > bscore) { bscore = score; best = i; bax = ax; }
		}
		if (best < 0) break;
		box *bx = &boxes[best];
		ws_axis = bax;
		qsort(hs + bx->a, (size_t) (bx->b - bx->a), sizeof *hs, ws_by_axis);
		uint32_t pop = 0, half = 0;
		for (int j = bx->a; j < bx->b; j++) pop += hs[j].n;
		int cut = bx->a + 1;
		for (int j = bx->a; j < bx->b - 1; j++) {
			half += hs[j].n;
			cut = j + 1;
			if (2 * half >= pop) break;
		}
		boxes[nb++] = (box) { cut, bx->b };
		bx->b = cut;
	}
	static int16_t idx[32768];
	memset(w->clut, 0, sizeof w->clut);
	for (int i = 0; i < nb; i++) {
		double s[3] = { 0, 0, 0 }, n = 0;
		for (int j = boxes[i].a; j < boxes[i].b; j++) {
			for (int k = 0; k < 3; k++) s[k] += (double) ((hs[j].c >> (5 * k)) & 31) * hs[j].n;
			n += hs[j].n;
			idx[hs[j].c] = (int16_t) (i + 1);
		}
		int c[3];
		for (int k = 0; k < 3; k++) c[k] = (int) lround(s[k] / n);
		w->clut[i + 1] = (uint16_t) (0x8000 | c[0] | c[1] << 5 | c[2] << 10);
	}
	for (int f = 0; f < w->nframes; f++) {
		ws_frame *fr = &w->frames[f];
		fr->px = xcalloc((size_t) fr->w * fr->h, 1);
		for (int i = 0; i < fr->w * fr->h; i++)
			fr->px[i] = fr->rgb[i] ? (uint8_t) idx[fr->rgb[i] & 0x7fff] : 0;
	}
	free(hs);
}

/* every weapon's frames, and the slot the largest needs among the images */
static void ws_prepare(void)
{
	uint8_t dots0[QG_NORMALS];
	for (int n = 0; n < QG_NORMALS; n++) {       /* (the light table of yaw 0, below) */
		double nx = anorms[n][0];
		dots0[n] = (uint8_t) lround((1 + 0.5 * (nx > 0 ? nx : 0)) * 128);
	}
	int mw = 2, mh = 1;
	long texels = 0;
	for (int wpn = 0; wpn < QG_WEAPONS; wpn++) {
		const mdl_t *m = &srcs[QGM_V_AXE + wpn].mdl;
		double wind = 0;                         /* the file's winding (cook_mdl's) */
		for (int t = 0; t < m->ntris; t++) {
			double v[3][3], n[3] = { 0, 0, 0 };
			for (int k = 0; k < 3; k++) {
				const tvert *tv = &m->frames[0][m->tris[t][k + 1]];
				for (int a = 0; a < 3; a++) v[k][a] = tv->v[a] * m->scale[a] + m->origin[a];
				int ni = tv->n < QG_NORMALS ? tv->n : 0;
				for (int a = 0; a < 3; a++) n[a] += anorms[ni][a];
			}
			double a[3], b[3], c[3];
			for (int k = 0; k < 3; k++) { a[k] = v[1][k] - v[0][k]; b[k] = v[2][k] - v[0][k]; }
			c[0] = a[1] * b[2] - a[2] * b[1];
			c[1] = a[2] * b[0] - a[0] * b[2];
			c[2] = a[0] * b[1] - a[1] * b[0];
			double l = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
			if (l > 0) wind += (c[0] * n[0] + c[1] * n[1] + c[2] * n[2]) / l;
		}
		ws_weapon *w = &wsw[wpn];
		w->nframes = m->nframes;
		w->frames = xcalloc((size_t) m->nframes, sizeof *w->frames);
		double zs = 0;
		int zn = 0;
		for (int i = 0; i < m->nverts; i++) {
			double x = m->frames[0][i].v[0] * m->scale[0] + m->origin[0];
			if (x >= WS_NEAR) { zs += x; zn++; }
		}
		w->anchor = zn ? zs / zn : 20;
		for (int f = 0; f < m->nframes; f++) {
			ws_frame *fr = &w->frames[f];
			ws_render(m, f, wind, dots0, fr);
			if (fr->w > WS_MAX || fr->h > WS_MAX) {      /* drawn 3D: no picture */
				fr->w = fr->h = 0;
				continue;
			}
			if (fr->w > mw) mw = fr->w;
			if (fr->h > mh) mh = fr->h;
			texels += (long) fr->w * fr->h;
		}
		ws_quantize(w);
		if (getenv("QCOOK_WS"))
			for (int f = 0; f < m->nframes; f++)
				printf("ws %s frame %d at %d,%d %dx%d\n", MODELS[QGM_V_AXE + wpn].file, f,
				       w->frames[f].x, w->frames[f].y, w->frames[f].w, w->frames[f].h);
	}
	if (mw > 255 || mh > 255) die("2D weapons: a frame of %dx%d texels", mw, mh);
	/* the slot starts on 32 texels: a frame's rows may be filled with 0
	   before its spans come (GP0(02h) takes x down to 16 halfwords) */
	ws_slot = add_image("2D weapon slot", mw, mh, 0);
	ws_slot->align = 32;
	printf("qcook: 2D weapons, %ld K texels in 8 weapons' frames, a slot of %dx%d\n", texels / 1024, mw, mh);
}

/* the frame of the eight weapons' k-th */
static const ws_frame *ws_of(int k)
{
	for (int w = 0; w < QG_WEAPONS; k -= wsw[w].nframes, w++)
		if (k < wsw[w].nframes)
			return &wsw[w].frames[k];
	return NULL;
}

/* GP0(A0h) of rows i .. j-1, texels l .. r of the frame (r - l + 1 even)
   to the slot at VRAM (sx, sy): its words across packets of 255 */
static void ws_copy(gbuf *pix, const ws_frame *f, int sx, int sy, int i, int j, int l, int r)
{
	size_t rw = (size_t) (r - l + 1), bytes = rw * (size_t) (j - i), words = (bytes + 3) / 4;
	uint8_t *data = xcalloc(words, 4);
	for (int y = i; y < j; y++)
		memcpy(data + (size_t) (y - i) * rw, f->px + (size_t) y * f->w + l, rw);
	size_t done = 0;
	for (int first = 1; done < words; first = 0) {
		size_t room = first ? 252 : 255, cnt = words - done < room ? words - done : room;
		size_t len = cnt + (first ? 3 : 0);
		uint32_t tag = (uint32_t) len << 24 | (uint32_t) (pix->len + 4 * (1 + len));   /* (the next's) */
		gput(pix, &tag, 4);
		if (first) {
			uint32_t a0[3] = { 0xA0000000u, (uint32_t) (sy + i) << 16 | (uint32_t) (sx + l / 2),
			                   (uint32_t) (j - i) << 16 | (uint32_t) (rw / 2) };
			gput(pix, a0, sizeof a0);
		}
		gput(pix, data + 4 * done, 4 * cnt);
		done += cnt;
	}
	free(data);
}

static long ws_copy_words(long halfwords)        /* its packets' words */
{
	long words = (halfwords + 1) / 2;
	return 3 + words + 1 + (words > 252 ? (words - 252 + 254) / 255 : 0);
}

/* A frame's chain into the slot at VRAM (sx, sy), slot_w texels wide, the
   fewer words of:
   - GP0(A0h): the whole frame copied;
   - GP0(02h): its rows of the slot filled with 0, the transparent index
     (the GPU fills whole units of 16 halfwords: the slot starts on one;
     a frame whose unit would pass the slot's end is copied whole), then
     GP0(A0h): its opaque texels, a rectangle per run of rows over the
     run's spans (a halfword on each side), the runs chosen for the fewest
     words (a copy's three words and its packets' tags against the
     transparent texels a wider span carries).
   Then GP0(01h): the texture cache cleared. Either leaves the slot with
   the frame's texels, to the texel. Its first and last packets' offsets. */
static void ws_chain(gbuf *pix, const ws_frame *f, int sx, int sy, int slot_w, uint32_t *ofs, uint32_t *ofs_last)
{
	int w = f->w, h = f->h;
	int lo[256], hi[256];                        /* each row's span; hi < 0: none */
	for (int r = 0; r < h; r++) {
		lo[r] = w;
		hi[r] = -1;
		for (int c = 0; c < w; c++)
			if (f->px[r * w + c]) {
				if (c < lo[r]) lo[r] = c;
				hi[r] = c;
			}
		if (hi[r] >= 0) {
			lo[r] &= ~1;
			hi[r] |= 1;                          /* (w is even) */
		}
	}
	/* cost[j]: the fewest words for rows 0 .. j-1; run[j]: where the
	   rectangle ending there starts, -1 for row j-1 left to the fill */
	long cost[257];
	int run[257];
	cost[0] = 0;
	for (int j = 1; j <= h; j++) {
		cost[j] = hi[j - 1] < 0 ? cost[j - 1] : 1L << 40;
		run[j] = -1;
		int l = w, r = -1;
		for (int i = j - 1; i >= 0; i--) {
			if (hi[i] < 0)
				continue;                        /* (a rectangle starts on an opaque row) */
			if (lo[i] < l) l = lo[i];
			if (hi[i] > r) r = hi[i];
			long c = ws_copy_words((long) (r - l + 1) / 2 * (j - i));
			if (cost[i] + c < cost[j]) {
				cost[j] = cost[i] + c;
				run[j] = i;
			}
		}
	}
	int starts[256], ends[256], nrun = 0;
	for (int j = h; j > 0; )
		if (run[j] < 0) {
			j--;
		} else {
			starts[nrun] = run[j];
			ends[nrun++] = j;
			j = run[j];
		}

	*ofs = (uint32_t) pix->len;
	if (((w / 2 + 15) & ~15) > slot_w / 2 || 4 + cost[h] >= ws_copy_words((long) w / 2 * h)) {
		ws_copy(pix, f, sx, sy, 0, h, 0, w - 1);     /* (whole) */
	} else {
		uint32_t fill[4] = { 3u << 24 | (uint32_t) (pix->len + 16), 0x02000000u,
		                     (uint32_t) sy << 16 | (uint32_t) sx, (uint32_t) h << 16 | (uint32_t) (w / 2) };
		gput(pix, fill, sizeof fill);
		for (int n = nrun - 1; n >= 0; n--) {    /* (top to bottom) */
			int i = starts[n], j = ends[n], l = w, r = -1;
			for (int y = i; y < j; y++)
				if (hi[y] >= 0) {
					if (lo[y] < l) l = lo[y];
					if (hi[y] > r) r = hi[y];
				}
			ws_copy(pix, f, sx, sy, i, j, l, r);
		}
	}
	*ofs_last = (uint32_t) pix->len;
	uint32_t cc[2] = { 1u << 24 | 0xffffffu, 0x01000000u };   /* the texture cache cleared */
	gput(pix, cc, sizeof cc);
}

int cmd_game(int argc, char **argv)
{
	const char *pak_path = NULL, *out_path = NULL;
	for (int i = 2; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--pak")) pak_path = argv[i + 1];
		if (!strcmp(argv[i], "--out")) out_path = argv[i + 1];
		if (!strcmp(argv[i], "--textures"))      /* the guns' skins (view_atlas) */
			gun_tex = level_find(argv[i + 1], QL_TEXTURE_MASK, "textures");
	}
	if (!pak_path || !out_path)
		die("usage: qcook game --pak <pak0.pak> [--textures medium|high] --out <prefix>   (<prefix>_<level>.dat)");

	long pak_len;
	uint8_t *pak = read_file(pak_path, &pak_len);
	uint32_t len;
	palette = pak_find(pak, pak_len, "gfx/palette.lmp", &len);
	if (!palette || len < 768) die("gfx/palette.lmp missing");
	wad = pak_find(pak, pak_len, "gfx.wad", &wad_len);
	if (!wad) die("gfx.wad missing");

	/* every image first, placed together */
	pics_prepare();
	aux_prepare(pak, pak_len);
	if (NUM_MODELS != QGM_COUNT) die("model list: %d of %d", NUM_MODELS, QGM_COUNT);
	for (int i = 0; i < NUM_MODELS; i++) {
		model_src *ms = &srcs[MODELS[i].id];
		ms->sp = &MODELS[i];
		ms->data = pak_find(pak, pak_len, MODELS[i].file, &ms->len);
		if (!ms->data) die("%s missing in %s", MODELS[i].file, pak_path);
		model_prepare(ms);
	}
	ws_prepare();
	place_images();

	/* then the geometry and the tables */
	static qg_pic pics[QGP_COUNT];
	for (int i = 0; i < npics; i++) {
		const pic_spec *ps = &PICS[i];
		qg_pic *p = &pics[ps->id];
		p->tpage = page_tpage(ps->im->page);
		p->u = (uint8_t) ps->im->u;
		p->v = (uint8_t) ps->im->v;
		p->w = (uint16_t) ps->im->w;
		p->h = (uint16_t) ps->im->h;
	}
	/* the models at every model level (their triangles; the pictures and
	   skins are the same): one file each */
	static gbuf geos[QL_LEVELS];
	static qg_model level_models[QL_LEVELS][QGM_COUNT];
	uint32_t len_geo_max = 0;
	for (int lv = 0; lv < QL_LEVELS; lv++) {
		if (!(QL_MODEL_MASK & 1 << lv))
			continue;
		printf("qcook: models at %s\n", level_name[lv]);
		gbuf *geo = &geos[lv];
		memset(geo, 0, sizeof *geo);
		for (int i = 0; i < QGM_COUNT; i++) {
			model_src *ms = &srcs[i];
			qg_model *out = &level_models[lv][i];
			memset(out, 0, sizeof *out);
			out->id = (uint16_t) i;
			galign(geo, 4);
			if (strstr(ms->sp->file, ".spr"))
				cook_spr(ms, geo, out);
			else
				cook_mdl(ms, lv, geo, out);
			out->tex.clut = (uint16_t) ((QG_CLUT_Y << 6) | (QG_CLUT_X >> 4));
		}
		galign(geo, 4);
		if (geo->len > len_geo_max)
			len_geo_max = (uint32_t) geo->len;
	}

	/* light tables: a vertex's factor (x128) by its normal, for a model
	   yawed by k x 22.5 degrees; Quake's fixed light comes from -x: a
	   vertex facing +x in the world is lit up to 1.5 x */
	static uint8_t dots[QG_LIGHT_YAWS][QG_NORMALS];
	for (int q = 0; q < QG_LIGHT_YAWS; q++) {
		double a = q * 2 * M_PI / QG_LIGHT_YAWS, ca = cos(a), sa = sin(a);
		for (int n = 0; n < QG_NORMALS; n++) {
			double nx = anorms[n][0] * ca - anorms[n][1] * sa;
			double f = 1 + 0.5 * (nx > 0 ? nx : 0);
			dots[q][n] = (uint8_t) lround(f * 128);
		}
	}
	static uint32_t rgb[256];
	for (int i = 0; i < 256; i++)
		rgb[i] = (uint32_t) palette[i * 3] | (uint32_t) palette[i * 3 + 1] << 8 | (uint32_t) palette[i * 3 + 2] << 16;

	/* assemble each level's file: header, rects, tables, geometry, pixels */
	for (int lv = 0; lv < QL_LEVELS; lv++) {
	if (!(QL_MODEL_MASK & 1 << lv))
		continue;
	gbuf geo = geos[lv];
	qg_model *models = level_models[lv];
	int npages = 0;
	for (int p = 0; p < GP_PAGES; p++)
		if (page_rows[p]) npages = p + 1;
	gbuf f;
	memset(&f, 0, sizeof f);
	qg_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QGAME_MAGIC, 4);
	h.clut = (uint16_t) ((QG_CLUT_Y << 6) | (QG_CLUT_X >> 4));
	gput(&f, &h, sizeof h);
	h.num_rects = (uint32_t) (1 + npages + 1 + AUX_PAGES);
	h.ofs_rects = (uint32_t) f.len;
	size_t rects_at = f.len;
	{ qtex_rect z; memset(&z, 0, sizeof z); for (uint32_t i = 0; i < h.num_rects; i++) gput(&f, &z, sizeof z); }
	h.num_models = QGM_COUNT; h.ofs_models = (uint32_t) f.len; gput(&f, models, QGM_COUNT * sizeof *models);
	h.level = (uint32_t) lv;
	h.len_geo_max = len_geo_max;
	h.num_pics = QGP_COUNT;   h.ofs_pics = (uint32_t) f.len;   gput(&f, pics, sizeof pics);
	h.ofs_dots = (uint32_t) f.len; gput(&f, dots, sizeof dots); galign(&f, 4);
	h.ofs_rgb  = (uint32_t) f.len; gput(&f, rgb, sizeof rgb);
	h.ofs_geo  = (uint32_t) f.len; h.len_geo = (uint32_t) geo.len; gput(&f, geo.p, geo.len); galign(&f, 4);
	{
		static qg_aux at[QGA_COUNT];
		for (int i = 0; i < QGA_COUNT; i++) {
			const aux_img *a = &aux[i];
			int vy = a->page * PT;
			at[i].tpage = (uint16_t) (((QG_AUX_X & 0x3ff) >> 6) | ((vy >> 8) << 4));   /* 4bpp */
			at[i].clut  = (uint16_t) (((QG_AUX_CLUT_Y + i / QG_AUX_CLUTS_PER_ROW) << 6) |
			                          ((QG_AUX_CLUT_X + 16 * (i % QG_AUX_CLUTS_PER_ROW)) >> 4));
			at[i].u = (uint8_t) a->u; at[i].v = (uint8_t) a->v;
			at[i].w = (uint16_t) a->w; at[i].h = (uint16_t) a->h;
		}
		h.num_aux = QGA_COUNT; h.ofs_aux = (uint32_t) f.len; gput(&f, at, sizeof at);
	}
	{
		gbuf ftab, ftxt;
		memset(&ftab, 0, sizeof ftab);
		memset(&ftxt, 0, sizeof ftxt);
		finales_cook(pak, pak_len, &ftab, &ftxt, &h.num_finales);
		galign(&ftxt, 4);
		uint32_t tab_len = (uint32_t) ftab.len;
		for (uint32_t i = 0; i < h.num_finales; i++)       /* offsets from the block's start */
			((qg_finale *) ftab.p)[i].ofs += tab_len;
		h.ofs_finales = (uint32_t) f.len;
		h.len_finales = (uint32_t) (ftab.len + ftxt.len);
		if (ftab.len) gput(&f, ftab.p, ftab.len);
		if (ftxt.len) gput(&f, ftxt.p, ftxt.len);
		galign(&f, 4);
	}
	{	/* the 2D weapons: their table, their frames', then their packets: a
		   weapon's CLUT, a frame's chain (ws_chain: its rows of the slot
		   filled with 0, its opaque spans copied, GP0(01h)); a tag's link
		   the next packet's offset in the block, the chain's last 0xffffff */
		static qg_wsprite wt[QG_WEAPONS];
		gbuf pix;
		memset(&pix, 0, sizeof pix);
		int nfr = 0;
		for (int w = 0; w < QG_WEAPONS; w++)
			nfr += wsw[w].nframes;
		qg_wframe *fr = xcalloc((size_t) nfr, sizeof *fr);
		int shared = 0;
		int gp = QTEX_MAP_PAGES + ws_slot->page;
		int sx = QTEX_PAGE_X0 + (gp % QTEX_PAGE_COLS) * (PT / 2) + ws_slot->u / 2;   /* (u: 32 texels) */
		int sy = (gp / QTEX_PAGE_COLS) * PT + ws_slot->v;
		int k = 0;
		for (int w = 0; w < QG_WEAPONS; w++) {
			wt[w].first  = (uint16_t) k;
			wt[w].count  = (uint16_t) wsw[w].nframes;
			wt[w].anchor = (uint16_t) lround(wsw[w].anchor * 16);
			wt[w].ofs_clut = (uint32_t) pix.len;
			uint32_t pk[4] = { 131u << 24 | 0xffffffu, 0xA0000000u,
			                   (uint32_t) QG_WCLUT_Y << 16 | QG_WCLUT_X, 1u << 16 | 256 };
			gput(&pix, pk, sizeof pk);
			gput(&pix, wsw[w].clut, sizeof wsw[w].clut);
			for (int i = 0; i < wsw[w].nframes; i++, k++) {
				const ws_frame *f = &wsw[w].frames[i];
				fr[k].x = (int16_t) f->x;
				fr[k].y = (int16_t) f->y;
				fr[k].w = (uint8_t) f->w;
				fr[k].h = (uint8_t) f->h;
				if (!f->w)                  /* (drawn 3D) */
					continue;
				int same = -1;              /* an earlier frame of the same texels: its chain */
				for (int j = 0; j < k && same < 0; j++) {
					const ws_frame *g = ws_of(j);
					if (fr[j].w && g->w == f->w && g->h == f->h && !memcmp(g->px, f->px, (size_t) f->w * f->h))
						same = j;
				}
				if (same >= 0) {
					fr[k].ofs      = fr[same].ofs;
					fr[k].ofs_last = fr[same].ofs_last;
					shared++;
					continue;
				}
				ws_chain(&pix, f, sx, sy, ws_slot->w, &fr[k].ofs, &fr[k].ofs_last);
			}
		}
		h.ofs_wsprites = (uint32_t) f.len; gput(&f, wt, sizeof wt);
		h.num_wframes  = (uint32_t) nfr;
		h.ofs_wframes  = (uint32_t) f.len; gput(&f, fr, (size_t) nfr * sizeof *fr);
		h.ofs_wpix     = (uint32_t) f.len; h.len_wpix = (uint32_t) pix.len;
		gput(&f, pix.p, pix.len);
		galign(&f, 4);
		h.wslot.tpage = page_tpage(ws_slot->page);
		h.wslot.u = (uint8_t) ws_slot->u;
		h.wslot.v = (uint8_t) ws_slot->v;
		h.wslot.w = (uint16_t) ws_slot->w;
		h.wslot.h = (uint16_t) ws_slot->h;
		printf("qcook: 2D weapons: %d frames (%d sharing another's texels), %u B of packets, the slot at "
		       "page %d (%d, %d), VRAM (%d, %d)\n", nfr, shared, (unsigned) pix.len, ws_slot->page, ws_slot->u,
		       ws_slot->v, sx, sy);
	}

	{	/* the pages' free rectangles (the streamed skins' room) */
		h.ofs_free = (uint32_t) f.len;
		uint32_t texels = 0;
		for (int p = 0; p < GP_PAGES; p++)
			for (int i = 0; i < nfree[p]; i++) {
				const rect *r = &freer[p][i];
				if (r->w < 8 || r->h < 8)
					continue;
				qg_free q;
				q.page = (uint8_t) (QTEX_MAP_PAGES + p);
				q.pad  = 0;
				q.u = (uint8_t) r->x; q.v = (uint8_t) r->y;
				q.w = (uint16_t) r->w; q.h = (uint16_t) r->h;
				gput(&f, &q, sizeof q);
				h.num_free++;
				texels += (uint32_t) (r->w * r->h);
			}
		printf("qcook: %u free rectangles in the game's pages (%u K texels, overlapping)\n", h.num_free,
		       texels / 1024);
	}
	galign(&f, 32);

	qtex_rect *rects = xcalloc(h.num_rects, sizeof *rects);
	rects[0].x = QG_CLUT_X; rects[0].y = QG_CLUT_Y; rects[0].w = 256; rects[0].h = 1;
	rects[0].data_ofs = (uint32_t) f.len;
	for (int i = 0; i < 256; i++) {
		int r = palette[i * 3], g = palette[i * 3 + 1], b = palette[i * 3 + 2];
		uint16_t c = (uint16_t) ((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
		if (i == 255) c = 0;                  /* transparent */
		else if (!c) c = 0x8000;              /* black, not transparent */
		gput(&f, &c, 2);
	}
	galign(&f, 32);
	for (int p = 0; p < npages; p++) {
		qtex_rect *r = &rects[1 + p];
		int gp = QTEX_MAP_PAGES + p;
		r->x = (uint16_t) (QTEX_PAGE_X0 + (gp % QTEX_PAGE_COLS) * (PT / 2));
		r->y = (uint16_t) ((gp / QTEX_PAGE_COLS) * PT);
		r->w = PT / 2;
		r->h = (uint16_t) page_rows[p];
		r->data_ofs = (uint32_t) f.len;
		gput(&f, &gpage[p][0][0], (size_t) page_rows[p] * PT);   /* 2 texels per halfword */
		galign(&f, 32);
	}
	{	/* the aux CLUTs (20 a row), then the aux pages' used rows, 4 texels a halfword */
		qtex_rect *r = &rects[1 + npages];
		int crows = (QGA_COUNT + QG_AUX_CLUTS_PER_ROW - 1) / QG_AUX_CLUTS_PER_ROW;
		r->x = QG_AUX_CLUT_X; r->y = QG_AUX_CLUT_Y;
		r->w = (uint16_t) (16 * QG_AUX_CLUTS_PER_ROW); r->h = (uint16_t) crows;
		r->data_ofs = (uint32_t) f.len;
		static const uint16_t none[16];
		for (int i = 0; i < crows * QG_AUX_CLUTS_PER_ROW; i++)
			gput(&f, i < QGA_COUNT ? aux[i].clut : none, sizeof none);
		galign(&f, 32);
		for (int p = 0; p < AUX_PAGES; p++) {
			r = &rects[1 + npages + 1 + p];
			int rows = apage_rows[p] ? apage_rows[p] : 1;
			r->x = QG_AUX_X; r->y = (uint16_t) (p * PT); r->w = PT / 4; r->h = (uint16_t) rows;
			r->data_ofs = (uint32_t) f.len;
			for (int y = 0; y < rows; y++)
				for (int x = 0; x < PT; x += 2) {
					uint8_t b = (uint8_t) ((apage[p][y][x] & 15) | (apage[p][y][x + 1] & 15) << 4);
					gput(&f, &b, 1);
				}
			galign(&f, 32);
		}
	}
	memcpy(f.p + rects_at, rects, h.num_rects * sizeof *rects);
	memcpy(f.p, &h, sizeof h);

	char path[512];
	snprintf(path, sizeof path, "%s_%d.dat", out_path, lv);
	FILE *fp = fopen(path, "wb");
	if (!fp || fwrite(f.p, 1, f.len, fp) != f.len) die("cannot write %s", path);
	fclose(fp);
	uint32_t texels = 0;
	for (int p = 0; p < npages; p++) texels += (uint32_t) page_rows[p] * PT;
	printf("qcook: %s — %u B: %d models (%u B of geometry), %d pictures, %d VRAM pages (%u K texels)\n",
	       path, (unsigned) f.len, QGM_COUNT, (unsigned) geo.len, QGP_COUNT, npages, texels / 1024);
	free(rects);
	free(f.p);
	}
	return 0;
}
