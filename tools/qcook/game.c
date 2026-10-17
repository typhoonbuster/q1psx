/*
 * qcook — game data: the models, sprites and HUD pictures the engine needs
 * on every map, cooked from the pak into one QGM1 file (formats/qgame.h).
 *
 *   qcook game --pak <pak0.pak> --out <game.dat>
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
				if (bw > f->w || bh > f->h) continue;
				int ds = f->w - bw, dl = f->h - bh;
				if (ds > dl) { int t = ds; ds = dl; dl = t; }
				if (ds < bs || (ds == bs && dl < bl)) { bs = ds; bl = dl; bp = p; bi = i; }
			}
		if (bp < 0)
			die("%s (%dx%d): the game's %d VRAM pages are full (%ld K texels placed of %ld K)",
			    im->what, bw, bh, GP_PAGES, area / 1024, total / 1024);
		rect at = { freer[bp][bi].x, freer[bp][bi].y, bw, bh };
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

static void mdl_parse(mdl_t *m, const uint8_t *d, uint32_t len, const char *name)
{
	if (len < 84 || memcmp(d, "IDPO", 4) || rdi(d + 4) != 6)
		die("%s: not a version 6 MDL", name);
	for (int k = 0; k < 3; k++) {
		m->scale[k]  = rdf(d + 8 + k * 4);
		m->origin[k] = rdf(d + 20 + k * 4);
	}
	int nskins = rdi(d + 48);
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
			if (s == 0) m->skin = p;
			p += sk;
		} else {
			NEED(4);
			int nb = rdi(p);
			p += 4 + 4 * nb;
			NEED(sk * (size_t) nb);
			if (s == 0) m->skin = p;
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

static void cook_mdl(model_src *ms, gbuf *geo, qg_model *out)
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
	printf("qcook: %-20s %3d frames %4d tris (%d behind the eye, %d back-facing dropped) -> %3d records, %4d slots (%d verts), skin %dx%d, %u B\n",
	       sp->file, m->nframes, m->ntris, dropped_near, dropped_back, nrec, nslots, m->nverts, im->w, im->h,
	       (unsigned) (geo->len - rec_start));
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

/* ------------------------------------------------------------------ main -- */

int cmd_game(int argc, char **argv)
{
	const char *pak_path = NULL, *out_path = NULL;
	for (int i = 2; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--pak")) pak_path = argv[i + 1];
		if (!strcmp(argv[i], "--out")) out_path = argv[i + 1];
	}
	if (!pak_path || !out_path)
		die("usage: qcook game --pak <pak0.pak> --out <game.dat>");

	long pak_len;
	uint8_t *pak = read_file(pak_path, &pak_len);
	uint32_t len;
	palette = pak_find(pak, pak_len, "gfx/palette.lmp", &len);
	if (!palette || len < 768) die("gfx/palette.lmp missing");
	wad = pak_find(pak, pak_len, "gfx.wad", &wad_len);
	if (!wad) die("gfx.wad missing");

	/* every image first, placed together */
	pics_prepare();
	if (NUM_MODELS != QGM_COUNT) die("model list: %d of %d", NUM_MODELS, QGM_COUNT);
	for (int i = 0; i < NUM_MODELS; i++) {
		model_src *ms = &srcs[MODELS[i].id];
		ms->sp = &MODELS[i];
		ms->data = pak_find(pak, pak_len, MODELS[i].file, &ms->len);
		if (!ms->data) die("%s missing in %s", MODELS[i].file, pak_path);
		model_prepare(ms);
	}
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
	gbuf geo;
	memset(&geo, 0, sizeof geo);
	static qg_model models[QGM_COUNT];
	for (int i = 0; i < QGM_COUNT; i++) {
		model_src *ms = &srcs[i];
		qg_model *out = &models[i];
		memset(out, 0, sizeof *out);
		out->id = (uint16_t) i;
		galign(&geo, 4);
		if (strstr(ms->sp->file, ".spr"))
			cook_spr(ms, &geo, out);
		else
			cook_mdl(ms, &geo, out);
		out->tex.clut = (uint16_t) ((QG_CLUT_Y << 6) | (QG_CLUT_X >> 4));
	}
	galign(&geo, 4);

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

	/* assemble: header, rects, tables, geometry, pixels */
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
	h.num_rects = (uint32_t) (1 + npages);
	h.ofs_rects = (uint32_t) f.len;
	size_t rects_at = f.len;
	{ qtex_rect z; memset(&z, 0, sizeof z); for (uint32_t i = 0; i < h.num_rects; i++) gput(&f, &z, sizeof z); }
	h.num_models = QGM_COUNT; h.ofs_models = (uint32_t) f.len; gput(&f, models, sizeof models);
	h.num_pics = QGP_COUNT;   h.ofs_pics = (uint32_t) f.len;   gput(&f, pics, sizeof pics);
	h.ofs_dots = (uint32_t) f.len; gput(&f, dots, sizeof dots); galign(&f, 4);
	h.ofs_rgb  = (uint32_t) f.len; gput(&f, rgb, sizeof rgb);
	h.ofs_geo  = (uint32_t) f.len; h.len_geo = (uint32_t) geo.len; gput(&f, geo.p, geo.len); galign(&f, 32);

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
	memcpy(f.p + rects_at, rects, h.num_rects * sizeof *rects);
	memcpy(f.p, &h, sizeof h);

	FILE *fp = fopen(out_path, "wb");
	if (!fp || fwrite(f.p, 1, f.len, fp) != f.len) die("cannot write %s", out_path);
	fclose(fp);
	uint32_t texels = 0;
	for (int p = 0; p < npages; p++) texels += (uint32_t) page_rows[p] * PT;
	printf("qcook: %s — %u B: %d models (%u B of geometry), %d pictures, %d VRAM pages (%u K texels)\n",
	       out_path, (unsigned) f.len, QGM_COUNT, (unsigned) geo.len, QGP_COUNT, npages, texels / 1024);
	return 0;
}
