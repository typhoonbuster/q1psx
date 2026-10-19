/*
 * qcook — game data: the models, sprites and HUD pictures the engine needs
 * on every map, cooked from the pak into one QGM7 file per model level
 * (formats/qgame.h, formats/qlevels.h).
 *
 *   qcook game --pak <pak0.pak> --out <prefix>      (<prefix>_<level>.dat)
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
	int         even;          /* its u must be even (a halfword of 8bpp texels:
	                              the engine copies into it) */
	int         page, u, v;    /* u, v: the image's own origin (past the border) */
} image;

static image images[256];
static int   nimages;

static image *add_image(const char *what, int w, int h, int border)
{
	if (nimages >= 256) die("too many images");
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
	image *order[256];
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
				int ex = im->even ? ((f->x + b) & 1) : 0;   /* (to an even u) */
				if (bw + ex > f->w || bh > f->h) continue;
				int ds = f->w - bw - ex, dl = f->h - bh;
				if (ds > dl) { int t = ds; ds = dl; dl = t; }
				if (ds < bs || (ds == bs && dl < bl)) { bs = ds; bl = dl; bp = p; bi = i; }
			}
		if (bp < 0)
			die("%s (%dx%d): the game's %d VRAM pages are full (%ld K texels placed of %ld K)",
			    im->what, bw, bh, GP_PAGES, area / 1024, total / 1024);
		rect at = { freer[bp][bi].x, freer[bp][bi].y, bw, bh };
		if (im->even)
			at.x += (at.x + b) & 1;
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
	double      scale[3];  /* extra scale per axis (the lightning bolt is drawn
	                          twice as long: one bolt per 60 units, not 30) */
} model_spec;

static const model_spec MODELS[] = {
	{ "progs/v_axe.mdl",    QGM_V_AXE,    1, 0, 2, { 1, 1, 1 } },
	{ "progs/v_shot.mdl",   QGM_V_SHOT,   1, 0, 2, { 1, 1, 1 } },
	{ "progs/v_shot2.mdl",  QGM_V_SHOT2,  1, 0, 2, { 1, 1, 1 } },
	{ "progs/v_nail.mdl",   QGM_V_NAIL,   1, 0, 2, { 1, 1, 1 } },
	{ "progs/v_nail2.mdl",  QGM_V_NAIL2,  1, 0, 2, { 1, 1, 1 } },
	{ "progs/v_rock.mdl",   QGM_V_ROCK,   1, 0, 2, { 1, 1, 1 } },
	{ "progs/v_rock2.mdl",  QGM_V_ROCK2,  1, 0, 2, { 1, 1, 1 } },
	{ "progs/v_light.mdl",  QGM_V_LIGHT,  1, 0, 2, { 1, 1, 1 } },
	{ "progs/spike.mdl",    QGM_SPIKE,    0, 1, 4, { 1, 1, 1 } },
	{ "progs/s_spike.mdl",  QGM_S_SPIKE,  0, 1, 4, { 1, 1, 1 } },
	{ "progs/grenade.mdl",  QGM_GRENADE,  0, 1, 4, { 1, 1, 1 } },
	{ "progs/missile.mdl",  QGM_MISSILE,  0, 0, 4, { 1, 1, 1 } },
	{ "progs/bolt2.mdl",    QGM_BOLT,     0, 1, 4, { 2, 1, 1 } },
	{ "progs/s_explod.spr", QGM_S_EXPLOD, 0, 1, 1, { 1, 1, 1 } },
	{ "progs/s_bubble.spr", QGM_S_BUBBLE, 0, 1, 1, { 1, 1, 1 } },
};
#define NUM_MODELS (int) (sizeof MODELS / sizeof MODELS[0])

typedef struct {
	const model_spec *sp;
	const uint8_t    *data;
	uint32_t          len;
	mdl_t             mdl;      /* a model */
	spr_t             spr;      /* or a sprite */
	image            *im;       /* the skin / the sprite sheet                */
	int               fx[32], fy[32];   /* a sprite's frames in its sheet     */
} model_src;

static model_src srcs[QGM_COUNT];

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
	/* The skin's used texels: the ones whose centre is inside a triangle's
	   texture coordinates (a back face's onseam vertices on the right
	   half, as the renderer maps them); no margin, since the skin's fill
	   begins right at the painted area's edge. The rest of a Quake skin is
	   a flat blue fill that no triangle samples at full resolution, but
	   that the reduction below would average into the edge texels: that
	   is where blue pixels on the barrels came from. */
	int sw = m->skinw, sh = m->skinh;
	uint8_t *cover = xcalloc((size_t) sw * sh, 1);
	for (int t = 0; t < m->ntris; t++) {
		double tx[3], ty[3];
		for (int k = 0; k < 3; k++) {
			int vi = m->tris[t][k + 1];
			tx[k] = m->st[vi][1] + (!m->tris[t][0] && m->st[vi][0] ? sw / 2 : 0) + 0.5;
			ty[k] = m->st[vi][2] + 0.5;
		}
		double area = (tx[1] - tx[0]) * (ty[2] - ty[0]) - (tx[2] - tx[0]) * (ty[1] - ty[0]);
		if (area == 0) continue;
		double sgn = area > 0 ? 1 : -1;
		int x0 = (int) floor(fmin(fmin(tx[0], tx[1]), tx[2])) - 1, x1 = (int) ceil(fmax(fmax(tx[0], tx[1]), tx[2])) + 1;
		int y0 = (int) floor(fmin(fmin(ty[0], ty[1]), ty[2])) - 1, y1 = (int) ceil(fmax(fmax(ty[0], ty[1]), ty[2])) + 1;
		for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < sh; y++)
			for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < sw; x++) {
				double px = x + 0.5, py = y + 0.5, in = 1;
				for (int e = 0; e < 3 && in; e++) {           /* on the inner side of each edge */
					int a = e, b = (e + 1) % 3;
					double ex = tx[b] - tx[a], ey = ty[b] - ty[a], len = sqrt(ex * ex + ey * ey);
					double dd = sgn * (ex * (py - ty[a]) - ey * (px - tx[a])) / (len > 0 ? len : 1);
					if (dd < -0.01) in = 0;
				}
				if (in) cover[y * sw + x] = 1;
			}
	}
	/* the skin at 1 / skin_div resolution: the used texels of each block
	   averaged; a block with none takes its used neighbours' colour, so
	   the GPU's stray texel next to an edge is never the blue */
	int d = sp->skin_div, w2 = (sw + d - 1) / d, h2 = (sh + d - 1) / d;
	ms->im = add_image(sp->file, w2, h2, GP_BORDER);
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
		ms->im->px[i] = done[i] ? (uint8_t) nearest_colour(rgb[i][0], rgb[i][1], rgb[i][2])
		                        : (uint8_t) nearest_colour(palette[m->skin[0] * 3], palette[m->skin[0] * 3 + 1],
		                                                   palette[m->skin[0] * 3 + 2]);
	free(cover);
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
		if (nslots == slots_cap) {
			slots_cap = slots_cap ? slots_cap * 2 : 256;
			slots = realloc(slots, (size_t) slots_cap * sizeof *slots);
			if (!slots) die("out of memory");
		}
		slots[nslots] = r->mv[i];
		idx[i] = (uint16_t) nslots++;
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

static void cook_mdl(model_src *ms, int model_level, gbuf *geo, qg_model *out)
{
	const model_spec *sp = ms->sp;
	mdl_t *m = &ms->mdl;
	const image *im = ms->im;
	out->tex.tpage  = page_tpage(im->page);
	out->tex.window = 0xE2000000u;

	/* A view model is only ever seen from the eye, which sits about the
	   model's origin (Quake's view.c: the gun's origin is the view's plus
	   0.4 x bob forward and bob up, the view size's 2-unit fudge; the bob
	   runs -2.56 .. 4): a triangle behind the near plane in every frame
	   (x < 3 units: the renderer clips at 5) is never drawn, nor is one
	   whose plane keeps every eye position on its back in every frame
	   (Quake's alias drawer skips a triangle wound backwards on screen,
	   d_polyse.c D_DrawNonSubdiv, zero-area ones included). Both are
	   dropped. The file's winding convention is read off the model
	   itself: the side its vertex normals point to, by majority. */
	static const double eye[8][3] = {
		{ -2.5, -1, -7 }, { 2, -1, -7 }, { -2.5, 1, -7 }, { 2, 1, -7 },
		{ -2.5, -1, 3.5 }, { 2, -1, 3.5 }, { -2.5, 1, 3.5 }, { 2, 1, 3.5 } };
	uint8_t *keep = xmalloc((size_t) m->ntris);
	int dropped_near = 0, dropped_back = 0;
	double wind = 0;
	for (int t = 0; t < m->ntris && sp->view; t++) {
		int16_t v[3][3];
		double n[3] = { 0, 0, 0 }, c[3];
		for (int k = 0; k < 3; k++) {
			int vi = m->tris[t][k + 1];
			mdl_vertex(m, 0, vi, sp->scale, v[k]);
			int ni = m->frames[0][vi].n;
			if (ni >= QG_NORMALS) ni = 0;
			for (int a = 0; a < 3; a++) n[a] += anorms[ni][a];
		}
		tri_normal(v, c);
		double l = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		if (l > 0)
			wind += (c[0] * n[0] + c[1] * n[1] + c[2] * n[2]) / l;
	}
	for (int t = 0; t < m->ntris; t++) {
		keep[t] = 1;
		if (!sp->view)
			continue;
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
				if (d > 0)                  /* this eye position sees its front */
					back = 0;
			}
		}
		if (behind) { keep[t] = 0; dropped_near++; }
		else if (back) { keep[t] = 0; dropped_back++; }
	}

	/* the model level: the triangles kept, reduced (they all stay) */
	mdl_t reduced;
	int (*reduced_tris)[4] = reduce_mdl(m, sp, keep, model_level, &reduced);
	int full_tris = m->ntris;
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
	nslots = 0;
	int nrec = 0, left = 0, scan = 0;
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
		int s[3];
		for (int k = 0; k < 3; k++)
			s[k] = rec_slot(&r, m->tris[t][k + 1]);
		qw_tri *tri = &r.t[r.nt++];
		memset(tri, 0, sizeof *tri);
		for (int k = 0; k < 3; k++) {
			int vi = m->tris[t][k + 1];
			int ss = m->st[vi][1], tt = m->st[vi][2];
			if (!m->tris[t][0] && m->st[vi][0])          /* back face: onseam */
				ss += m->skinw / 2;
			/* the reduced skin's texel holding Quake's texel (s, t) */
			long u = im->u + ss / sp->skin_div, v = im->v + tt / sp->skin_div;
			if (u < im->u) u = im->u;
			if (u > im->u + im->w - 1) u = im->u + im->w - 1;
			if (v < im->v) v = im->v;
			if (v > im->v + im->h - 1) v = im->v + im->h - 1;
			tri->idx[k] = (uint8_t) (s[k] * 4);
			tri->uv[k]  = (uint16_t) (u | v << 8);
		}
	}
	mrec_flush(geo, &r, m, sp->scale, &nrec);
	free(ord);
	free(done);
	out->ofs_records = (uint32_t) rec_start;
	out->len_records = (uint32_t) (geo->len - rec_start);
	out->num_records = (uint16_t) nrec;
	out->num_verts   = (uint16_t) nslots;

	/* frames: the pools */
	galign(geo, 4);
	out->ofs_frames  = (uint32_t) geo->len;
	out->num_frames  = (uint16_t) m->nframes;
	double rad = 0;
	for (int f = 0; f < m->nframes; f++)
		for (int s = 0; s < nslots; s++) {
			qw_vert v;
			int16_t p[3];
			mdl_vertex(m, f, slots[s], sp->scale, p);
			v.x = p[0]; v.y = p[1]; v.z = p[2];
			int n = m->frames[f][slots[s]].n;
			if (n >= QG_NORMALS) n = 0;
			v.pad = (int16_t) n;
			gput(geo, &v, sizeof v);
			double d = sqrt((double) p[0] * p[0] + (double) p[1] * p[1] + (double) p[2] * p[2]);
			if (d > rad) rad = d;
		}
	out->radius = (uint16_t) (rad > 65000 ? 65000 : ceil(rad));
	out->flags  = (uint16_t) (sp->flat ? QGF_FLAT : 0);
	printf("qcook: %-20s %3d frames %4d tris (%d behind the eye, %d back-facing dropped) -> %3d records, %4d slots (%d verts), skin %dx%d, %u B",
	       sp->file, m->nframes, full_tris, dropped_near, dropped_back, nrec, nslots, m->nverts, im->w, im->h,
	       (unsigned) (geo->len - rec_start));
	if (reduced_tris)
		printf("; %s: %d of %d tris", level_name[model_level], decim_left, decim_tris);
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

void qc_mdl_cook_compact(qc_mdl *q, buf_t *out)
{
	model_src   *ms = &q->src;
	const mdl_t *m  = &ms->mdl;
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
	   built; the indices are the model's own vertices */
	enum { WINDOW = 24 };
	buf_t recs = { 0 };
	uint8_t *done = xcalloc((size_t) m->ntris, 1);
	int nrec = 0;
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
				qw_record h;
				memset(&h, 0, sizeof h);
				h.num_verts = (uint8_t) r.nv;
				h.num_tris  = (uint8_t) r.nt;
				h.tex       = (uint16_t) img;
				int nidx    = (r.nv + 2) / 3 * 3;
				int ibytes  = (nidx * 2 + 3) & ~3;
				int cbytes  = (r.nv + 3) & ~3;
				h.polys     = (uint16_t) (sizeof h + ibytes + cbytes);
				h.size      = (uint16_t) (h.polys + r.nt * sizeof(qw_tri));
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
				tri->idx[k] = (uint8_t) (s[k] * 4);
				tri->uv[k]  = (uint16_t) ((u - x0 + im->border) | (v + im->border) << 8);
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

	/* the asset: header, frames, records, images */
	qst_model h;
	memset(&h, 0, sizeof h);
	h.num_frames  = (uint16_t) m->nframes;
	h.num_verts   = (uint16_t) m->nverts;
	h.num_records = (uint16_t) nrec;
	h.flags       = (uint16_t) ((ms->sp->flat ? QSTM_FLAT : 0) | (m->flags & 0xff) << 8);
	h.ofs_frames  = sizeof h;
	h.ofs_records = h.ofs_frames + (uint32_t) m->nframes * (uint32_t) m->nverts * 4;
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
	for (int f = 0; f < m->nframes; f++)
		for (int v = 0; v < m->nverts; v++) {
			uint32_t w = mdl_word(&m->frames[f][v]);
			buf_put(out, &w, 4);
		}
	buf_put(out, recs.p, recs.len);
	buf_put(out, pix.p, pix.len);
	free(recs.p);
	free(pix.p);
	printf("qcook: %-22s %3d frames %4d verts %4d tris -> %3d records, skin %s %dx%d%s%dx%d (or %dx%d), "
	       "shift %d: %6zu B (%zu B of frames)\n", q->file, m->nframes, m->nverts, m->ntris, nrec,
	       two ? "halves" : "whole", h.images[0].w, h.images[0].h, two ? " + " : "", two ? h.images[1].w : 0,
	       two ? h.images[1].h : 0, h.small[0].w, h.small[0].h, shift, out->len - start,
	       (size_t) m->nframes * m->nverts * 4);
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
	ws_slot = add_image("2D weapon slot", mw, mh, 0);
	ws_slot->even = 1;
	printf("qcook: 2D weapons, %ld K texels in 8 weapons' frames, a slot of %dx%d\n", texels / 1024, mw, mh);
}

int cmd_game(int argc, char **argv)
{
	const char *pak_path = NULL, *out_path = NULL;
	for (int i = 2; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--pak")) pak_path = argv[i + 1];
		if (!strcmp(argv[i], "--out")) out_path = argv[i + 1];
	}
	if (!pak_path || !out_path)
		die("usage: qcook game --pak <pak0.pak> --out <prefix>   (<prefix>_<level>.dat)");

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
		   weapon's CLUT, a frame's texels (GP0(A0h) to the slot, then
		   GP0(01h)); a tag's link the next packet's offset in the block,
		   the chain's last 0xffffff */
		static qg_wsprite wt[QG_WEAPONS];
		gbuf pix;
		memset(&pix, 0, sizeof pix);
		int nfr = 0;
		for (int w = 0; w < QG_WEAPONS; w++)
			nfr += wsw[w].nframes;
		qg_wframe *fr = xcalloc((size_t) nfr, sizeof *fr);
		int gp = QTEX_MAP_PAGES + ws_slot->page;
		int sx = QTEX_PAGE_X0 + (gp % QTEX_PAGE_COLS) * (PT / 2) + ws_slot->u / 2;   /* (u even) */
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
				fr[k].ofs = (uint32_t) pix.len;
				size_t bytes = (size_t) f->w * f->h, words = (bytes + 3) / 4;
				uint32_t *data = xcalloc(words, 4);
				memcpy(data, f->px, bytes);
				size_t done = 0;
				int first = 1;
				while (done < words || first) {
					size_t room = first ? 252 : 255, n = words - done < room ? words - done : room;
					size_t len = n + (first ? 3 : 0);
					uint32_t tag = (uint32_t) len << 24 | (uint32_t) (pix.len + 4 * (1 + len));   /* (the next's) */
					gput(&pix, &tag, 4);
					if (first) {
						uint32_t a0[3] = { 0xA0000000u, (uint32_t) sy << 16 | (uint32_t) sx,
						                   (uint32_t) f->h << 16 | (uint32_t) (f->w / 2) };
						gput(&pix, a0, sizeof a0);
						first = 0;
					}
					gput(&pix, data + done, 4 * n);
					done += n;
				}
				free(data);
				fr[k].ofs_last = (uint32_t) pix.len;
				uint32_t cc[2] = { 1u << 24 | 0xffffffu, 0x01000000u };   /* the texture cache cleared */
				gput(&pix, cc, sizeof cc);
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
		printf("qcook: 2D weapons: %d frames, %u B of packets, the slot at page %d (%d, %d), VRAM (%d, %d)\n",
		       nfr, (unsigned) pix.len, ws_slot->page, ws_slot->u, ws_slot->v, sx, sy);
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
