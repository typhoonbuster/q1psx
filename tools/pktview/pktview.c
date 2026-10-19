/*
 * pktview — render a q1psx display-list dump to PNG images (host tool).
 *
 *   pktview <bench.log> <textures.tex|.tx4> <out-prefix> [game.dat [stream.dat]]
 *
 * The bench build prints, for one frame per scenario, "D <name>" (the views'
 * names are "view <n>"), "V <rows>" (the 3D viewport's height) and the
 * frame's packets in GPU draw order ("P <hex words>"), then "E". pktview
 * rebuilds VRAM from the QTX3 or QTX4 texture file (CLUTs + pages), and from
 * the game data's images (GAME.DAT: models' skins, the HUD) when given, and
 * rasterises the packets the way the PS1 GPU does — affine texture mapping
 * in screen space, no depth buffer, painter's order — into
 * <out-prefix><name>.png (320x240, "view 0" -> view0.png). It is how the
 * renderer's output is checked without a screen; it does not emulate GPU
 * timing.
 *
 * Supported: polygons (flat / Gouraud, textured or not), sprites (SPRT),
 * tiles (TILE), the texture page (DR_TPAGE), texture window (E2) and draw
 * area (E3 / E4) commands, VRAM-to-VRAM copies (GP0 80h: they change the
 * VRAM for the dumps after). The debug font is skipped.
 *
 * PKTVIEW_BG=r,g,b sets the clear colour (default black). A colour no
 * texture produces (e.g. 255,0,255) makes every pixel no polygon covers
 * stand out: cracks between polygons. PKTVIEW_DITHER=0 renders as the GPU
 * does with dithering off (the menu's option).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../formats/qtex.h"
#include "../../formats/qgame.h"
#include "../../formats/qstream.h"

#define W 320
#define H 240

static uint16_t vram[512][1024];
static uint8_t  fb[H][W][3];
static uint8_t  bg[3];          /* clear colour (PKTVIEW_BG) */
static int      clip_x0, clip_y0, clip_x1 = W - 1, clip_y1 = H - 1;   /* the draw area */

/* ------------------------------------------------------------------ png -- */

static uint32_t crc_table[256];

static void crc_init(void)
{
	for (uint32_t n = 0; n < 256; n++) {
		uint32_t c = n;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
	c = ~c;
	while (n--)
		c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return ~c;
}

static void put32(FILE *f, uint32_t v)
{
	uint8_t b[4] = { (uint8_t) (v >> 24), (uint8_t) (v >> 16), (uint8_t) (v >> 8), (uint8_t) v };
	fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	put32(f, len);
	fwrite(type, 1, 4, f);
	fwrite(data, 1, len, f);
	uint32_t c = crc(0, (const uint8_t *) type, 4);
	c = crc(c, data, len);
	put32(f, c);
}

/* zlib stream made of stored (uncompressed) deflate blocks */
static void write_png(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "pktview: cannot write %s\n", path); exit(1); }
	static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);

	uint8_t ihdr[13] = { 0, 0, W >> 8, W & 255, 0, 0, H >> 8, H & 255, 8, 2, 0, 0, 0 };
	chunk(f, "IHDR", ihdr, 13);

	size_t raw_len = (size_t) H * (1 + W * 3);
	uint8_t *raw = malloc(raw_len);
	for (int y = 0; y < H; y++) {
		raw[y * (1 + W * 3)] = 0;
		memcpy(raw + y * (1 + W * 3) + 1, fb[y], W * 3);
	}
	size_t nblocks = (raw_len + 65534) / 65535;
	size_t zlen = 2 + nblocks * 5 + raw_len + 4;
	uint8_t *z = malloc(zlen), *p = z;
	*p++ = 0x78; *p++ = 0x01;
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
	for (size_t off = 0; off < raw_len; off += 65535) {
		size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
		*p++ = off + n == raw_len;
		*p++ = (uint8_t) n; *p++ = (uint8_t) (n >> 8);
		*p++ = (uint8_t) ~n; *p++ = (uint8_t) (~n >> 8);
		memcpy(p, raw + off, n);
		p += n;
	}
	uint32_t ad = (b << 16) | a;
	*p++ = (uint8_t) (ad >> 24); *p++ = (uint8_t) (ad >> 16); *p++ = (uint8_t) (ad >> 8); *p++ = (uint8_t) ad;
	chunk(f, "IDAT", z, (uint32_t) (p - z));
	chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw);
	free(z);
}

/* ---------------------------------------------------------------- vram -- */

static void load_tex(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "pktview: cannot open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc((size_t) len);
	if (fread(d, 1, (size_t) len, f) != (size_t) len) { fprintf(stderr, "pktview: short read\n"); exit(1); }
	fclose(f);

	uint32_t nrects, first;
	if (!memcmp(d, QTEX_MAGIC, 4)) {             /* 8bpp: the CLUT in the header */
		qtex_header h;
		memcpy(&h, d, sizeof h);
		memcpy(&vram[QTEX_CLUT_Y][QTEX_CLUT_X], h.clut, sizeof h.clut);
		nrects = h.num_rects;
		first  = sizeof h;
	} else if (!memcmp(d, QTEX4_MAGIC, 4)) {     /* 4bpp: CLUTs are a rect */
		qtex4_header h;
		memcpy(&h, d, sizeof h);
		nrects = h.num_rects;
		first  = sizeof h;
	} else {
		fprintf(stderr, "pktview: not a QTX6 / QTX7 file\n");
		exit(1);
	}
	for (uint32_t i = 0; i < nrects; i++) {
		qtex_rect r;
		memcpy(&r, d + first + i * sizeof r, sizeof r);
		const uint16_t *px = (const uint16_t *) (d + r.data_ofs);
		for (int y = 0; y < r.h; y++)
			memcpy(&vram[r.y + y][r.x], px + y * r.w, r.w * 2u);
	}
	free(d);
}

/* the game data's images: its CLUT row and pages */
static void load_game(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "pktview: cannot open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc((size_t) len);
	if (fread(d, 1, (size_t) len, f) != (size_t) len) { fprintf(stderr, "pktview: short read\n"); exit(1); }
	fclose(f);
	qg_header h;
	if ((size_t) len < sizeof h || memcmp(d, QGAME_MAGIC, 4)) {
		fprintf(stderr, "pktview: %s is not a %s file\n", path, QGAME_MAGIC);
		exit(1);
	}
	memcpy(&h, d, sizeof h);
	for (uint32_t i = 0; i < h.num_rects; i++) {
		qtex_rect r;
		memcpy(&r, d + h.ofs_rects + i * sizeof r, sizeof r);
		const uint16_t *px = (const uint16_t *) (d + r.data_ofs);
		for (int y = 0; y < r.h; y++)
			memcpy(&vram[r.y + y][r.x], px + y * r.w, r.w * 2u);
	}
	free(d);
}

/* the streamed assets (STREAM.DAT): the bench's "S hash image page u v
   flags" lines put a model's skin image where the engine placed it
   (flags 1: transposed, 2: the half-resolution fallback) */
static uint8_t *stream_dat;
static long     stream_len;

static void load_stream(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "pktview: cannot open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END);
	stream_len = ftell(f);
	fseek(f, 0, SEEK_SET);
	stream_dat = malloc((size_t) stream_len);
	if (fread(stream_dat, 1, (size_t) stream_len, f) != (size_t) stream_len) {
		fprintf(stderr, "pktview: short read\n");
		exit(1);
	}
	fclose(f);
	if (stream_len < (long) sizeof(qst_header) || memcmp(stream_dat, QSTREAM_MAGIC, 4)) {
		fprintf(stderr, "pktview: %s is not a QST1 file\n", path);
		exit(1);
	}
}

static void stream_image(const char *line)
{
	unsigned hash, k, page, u, v, flags;
	if (!stream_dat || sscanf(line + 2, "%x %u %u %u %u %u", &hash, &k, &page, &u, &v, &flags) != 6 || k > 1)
		return;
	qst_header h;
	memcpy(&h, stream_dat, sizeof h);
	for (uint32_t i = 0; i < h.num_assets; i++) {
		qst_asset a;
		memcpy(&a, stream_dat + h.ofs_assets + i * sizeof a, sizeof a);
		if (a.hash != hash || a.kind != QSTK_MODEL)
			continue;
		const uint8_t *base = stream_dat + (size_t) a.sector * QST_SECTOR;
		qst_model m;
		memcpy(&m, base, sizeof m);
		const qst_image *im = flags & 2 ? &m.small[k] : &m.images[k];
		const uint8_t *px = base + im->ofs;
		int vx = QTEX_PAGE_X0 + (int) (page % QTEX_PAGE_COLS) * 128, vy = (int) (page / QTEX_PAGE_COLS) * 256;
		int tw = flags & 1 ? (im->h + 1) & ~1 : im->w, th = flags & 1 ? im->w : im->h;
		for (int y = 0; y < th; y++)
			for (int x = 0; x < tw; x++) {
				int sx = flags & 1 ? y : x, sy = flags & 1 ? (x < im->h ? x : im->h - 1) : y;
				uint8_t c = px[sy * im->w + sx];
				int tx = (int) u + x, ty = (int) v + y;
				uint16_t *hw = &vram[vy + ty][vx + tx / 2];
				*hw = tx & 1 ? (uint16_t) ((*hw & 0x00ff) | c << 8) : (uint16_t) ((*hw & 0xff00) | c);
			}
		return;
	}
}

/* --------------------------------------------------------------- raster -- */

typedef struct { int x, y; int u, v; int r, g, b; } vtx;

/* the GPU's 4x4 dither offsets (GP0(E1h) bit 9, which the engine sets),
   added to 8-bit components before they are cut to 5 bits */
static const int dither[4][4] = {
	{ -4, +0, -3, +1 }, { +2, -2, +3, -1 }, { -3, +1, -4, +0 }, { +3, -1, +2, -2 },
};

static uint32_t twin;   /* current GP0 E2h texture window (0 = none) */
static int dither_on = 1;   /* PKTVIEW_DITHER=0: the engine's dithering off */

static void texel(int tpage, int clut, int u, int v, uint8_t out[3], int *opaque)
{
	int px = (tpage & 15) * 64, py = ((tpage >> 4) & 1) * 256;
	int mode = (tpage >> 7) & 3;
	int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
	uint16_t c;
	u &= 255;
	v &= 255;
	/* texture window: texcoord = (texcoord & ~(mask * 8)) | ((offset & mask) * 8) */
	int mx = twin & 31, my = (twin >> 5) & 31, ox = (twin >> 10) & 31, oy = (twin >> 15) & 31;
	u = (u & ~(mx * 8)) | ((ox & mx) * 8);
	v = (v & ~(my * 8)) | ((oy & my) * 8);
	if (mode == 0) {           /* 4bpp */
		uint16_t w = vram[(py + v) & 511][(px + u / 4) & 1023];
		c = vram[cy][(cx + ((w >> ((u & 3) * 4)) & 15)) & 1023];
	} else if (mode == 1) {    /* 8bpp */
		uint16_t w = vram[(py + v) & 511][(px + u / 2) & 1023];
		c = vram[cy][(cx + ((w >> ((u & 1) * 8)) & 255)) & 1023];
	} else {                   /* 15bpp */
		c = vram[(py + v) & 511][(px + u) & 1023];
	}
	*opaque = c != 0;
	out[0] = (uint8_t) ((c & 31) << 3);
	out[1] = (uint8_t) (((c >> 5) & 31) << 3);
	out[2] = (uint8_t) (((c >> 10) & 31) << 3);
}

static long edge(const vtx *a, const vtx *b, int x, int y)
{
	return (long) (b->x - a->x) * (y - a->y) - (long) (b->y - a->y) * (x - a->x);
}

static int polys_drawn, polys_huge;

/* PKTVIEW_OCCL=1: which polygons end up with no pixel of their own in the
   final picture (painted over, or wholly transparent): an ID per pixel */
static int occl_on;
static int idbuf[H][W];
static int cur_id = -1;
#define OCCL_MAX 16384
static int id_written[OCCL_MAX];   /* pixels each polygon wrote */
static int id_class[OCCL_MAX];     /* 1: bbox off the draw area, 2: bbox under 2 x 2 px */
static int id_same[OCCL_MAX];      /* every corner on the same pixel */

/* Affine triangle: barycentric interpolation of u, v and colour in screen
   space. `raw` textures skip the modulation; anything shaded or modulated
   is dithered, as the GPU does. */
static void tri(vtx a, vtx b, vtx c, int textured, int raw, int tpage, int clut)
{
	long area = edge(&a, &b, c.x, c.y);
	if (area == 0)
		return;
	if (area < 0) { vtx t = b; b = c; c = t; area = -area; }

	int minx = a.x, maxx = a.x, miny = a.y, maxy = a.y;
	const vtx *vs[2] = { &b, &c };
	for (int i = 0; i < 2; i++) {
		if (vs[i]->x < minx) minx = vs[i]->x;
		if (vs[i]->x > maxx) maxx = vs[i]->x;
		if (vs[i]->y < miny) miny = vs[i]->y;
		if (vs[i]->y > maxy) maxy = vs[i]->y;
	}
	/* the GPU refuses primitives wider than 1023 or taller than 511 px */
	if (maxx - minx > 1023 || maxy - miny > 511) {
		polys_huge++;
		return;
	}
	if (minx < clip_x0) minx = clip_x0;
	if (miny < clip_y0) miny = clip_y0;
	if (maxx > clip_x1) maxx = clip_x1;
	if (maxy > clip_y1) maxy = clip_y1;

	for (int y = miny; y <= maxy; y++)
		for (int x = minx; x <= maxx; x++) {
			long w0 = edge(&b, &c, x, y), w1 = edge(&c, &a, x, y), w2 = edge(&a, &b, x, y);
			if (w0 < 0 || w1 < 0 || w2 < 0)
				continue;
			/* exclusive right / bottom edges, like the GPU */
			if ((w0 == 0 && (c.y > b.y || (c.y == b.y && c.x < b.x))) ||
			    (w1 == 0 && (a.y > c.y || (a.y == c.y && a.x < c.x))) ||
			    (w2 == 0 && (b.y > a.y || (b.y == a.y && b.x < a.x))))
				continue;
			int shade[3] = {
				(int) ((w0 * a.r + w1 * b.r + w2 * c.r) / area),
				(int) ((w0 * a.g + w1 * b.g + w2 * c.g) / area),
				(int) ((w0 * a.b + w1 * b.b + w2 * c.b) / area),
			};
			uint8_t col[3] = { (uint8_t) shade[0], (uint8_t) shade[1], (uint8_t) shade[2] };
			if (textured) {
				int u = (int) ((w0 * a.u + w1 * b.u + w2 * c.u) / area);
				int v = (int) ((w0 * a.v + w1 * b.v + w2 * c.v) / area);
				int op;
				texel(tpage, clut, u, v, col, &op);
				if (!op)
					continue;
				if (raw) {
					memcpy(fb[y][x], col, 3);
					if (occl_on) { idbuf[y][x] = cur_id; if (cur_id >= 0) id_written[cur_id]++; }
					continue;
				}
				for (int k = 0; k < 3; k++) {       /* texel x colour / 128 */
					int m = col[k] * shade[k] >> 7;
					col[k] = (uint8_t) (m > 255 ? 255 : m);
				}
			}
			for (int k = 0; k < 3; k++) {           /* dither, cut to 5 bits */
				int d = col[k] + (dither_on ? dither[y & 3][x & 3] : 0);
				d = d < 0 ? 0 : d > 255 ? 255 : d;
				col[k] = (uint8_t) (d & 0xf8);
			}
			memcpy(fb[y][x], col, 3);
			if (occl_on) { idbuf[y][x] = cur_id; if (cur_id >= 0) id_written[cur_id]++; }
		}
}

static vtx mkv(uint32_t xy, uint32_t uv, uint32_t rgb)
{
	vtx v;
	v.x = (int16_t) (xy & 0xffff);
	v.y = (int16_t) (xy >> 16);
	v.u = (int) (uv & 255);
	v.v = (int) ((uv >> 8) & 255);
	v.r = (int) (rgb & 255);
	v.g = (int) ((rgb >> 8) & 255);
	v.b = (int) ((rgb >> 16) & 255);
	return v;
}

static int cur_tpage;   /* DR_TPAGE: the page sprites and tiles use */

/* one pixel, dithered and cut to 5 bits like the GPU */
static void put_pixel(int x, int y, const uint8_t col_in[3])
{
	if (x < clip_x0 || x > clip_x1 || y < clip_y0 || y > clip_y1)
		return;
	uint8_t col[3];
	for (int k = 0; k < 3; k++) {
		int d = col_in[k] + (dither_on ? dither[y & 3][x & 3] : 0);
		d = d < 0 ? 0 : d > 255 ? 255 : d;
		col[k] = (uint8_t) (d & 0xf8);
	}
	memcpy(fb[y][x], col, 3);
}

/* a sprite: xy, uv | clut, [wh]; textured from the current page, modulated
   by its colour unless raw */
static void sprite(const uint32_t *w, int n, int sw, int sh, int raw)
{
	if (n < (sw ? 3 : 4))
		return;
	int x0 = (int16_t) (w[1] & 0xffff), y0 = (int16_t) (w[1] >> 16);
	int u0 = (int) (w[2] & 255), v0 = (int) ((w[2] >> 8) & 255), clut = (int) (w[2] >> 16);
	if (!sw) { sw = (int) (w[3] & 0xffff); sh = (int) (w[3] >> 16); }
	int rgb[3] = { (int) (w[0] & 255), (int) ((w[0] >> 8) & 255), (int) ((w[0] >> 16) & 255) };
	for (int y = 0; y < sh; y++)
		for (int x = 0; x < sw; x++) {
			uint8_t col[3];
			int op;
			texel(cur_tpage, clut, u0 + x, v0 + y, col, &op);
			if (!op)
				continue;
			if (raw) {
				if (x0 + x >= clip_x0 && x0 + x <= clip_x1 && y0 + y >= clip_y0 && y0 + y <= clip_y1)
					memcpy(fb[y0 + y][x0 + x], col, 3);
				continue;
			}
			for (int k = 0; k < 3; k++) {
				int m = col[k] * rgb[k] >> 7;
				col[k] = (uint8_t) (m > 255 ? 255 : m);
			}
			put_pixel(x0 + x, y0 + y, col);
		}
	polys_drawn++;
}

/* the GPU's semi-transparency (the primitive's bit 1) by the page's blend
   mode (DR_TPAGE bits 5-6): 0 B/2 + F/2, 1 B + F, 2 B - F, 3 B + F/4 */
static void blend_pixel(int x, int y, const uint8_t f[3])
{
	if (x < clip_x0 || x > clip_x1 || y < clip_y0 || y > clip_y1)
		return;
	int mode = (cur_tpage >> 5) & 3;
	for (int k = 0; k < 3; k++) {
		int b = fb[y][x][k], v;
		switch (mode) {
		case 0:  v = (b + f[k]) / 2; break;
		case 1:  v = b + f[k];       break;
		case 2:  v = b - f[k];       break;
		default: v = b + f[k] / 4;   break;
		}
		fb[y][x][k] = (uint8_t) ((v < 0 ? 0 : v > 255 ? 255 : v) & 0xf8);
	}
}

/* a tile: a flat rectangle, blended when semi-transparent (the water's tint) */
static void tile(const uint32_t *w, int n, int tw, int th, int semi)
{
	if (n < (tw ? 2 : 3))
		return;
	int x0 = (int16_t) (w[1] & 0xffff), y0 = (int16_t) (w[1] >> 16);
	if (!tw) { tw = (int) (w[2] & 0xffff); th = (int) (w[2] >> 16); }
	uint8_t col[3] = { (uint8_t) (w[0] & 255), (uint8_t) ((w[0] >> 8) & 255), (uint8_t) ((w[0] >> 16) & 255) };
	for (int y = 0; y < th; y++)
		for (int x = 0; x < tw; x++) {
			if (semi) {
				blend_pixel(x0 + x, y0 + y, col);
				continue;
			}
			if (x0 + x >= clip_x0 && x0 + x <= clip_x1 && y0 + y >= clip_y0 && y0 + y <= clip_y1)
				memcpy(fb[y0 + y][x0 + x], col, 3);
		}
	polys_drawn++;
}

/* a CPU to VRAM copy in progress (GP0(A0h), its texels in this packet and
   the next ones: the 2D gun's slot, formats/qgame.h): where the next
   halfword goes, how many are left */
static int cpu_x, cpu_y, cpu_x0, cpu_w, cpu_left;

static void cpu_words(const uint32_t *w, int n)
{
	for (int i = 0; i < n && cpu_left > 0; i++)
		for (int k = 0; k < 2 && cpu_left > 0; k++, cpu_left--) {
			vram[cpu_y & 511][cpu_x & 1023] = (uint16_t) (w[i] >> (16 * k));
			if (++cpu_x == cpu_x0 + cpu_w) {
				cpu_x = cpu_x0;
				cpu_y++;
			}
		}
}

static void packet(const uint32_t *w, int n)
{
	if (cpu_left > 0) {                  /* (the copy's texels go on) */
		cpu_words(w, n);
		return;
	}
	uint32_t code = w[0] >> 24;
	if (code == 0xa0 && n >= 3) {        /* CPU to VRAM */
		cpu_x = cpu_x0 = (int) (w[1] & 1023);
		cpu_y = (int) ((w[1] >> 16) & 511);
		cpu_w = (int) (w[2] & 0xffff);
		cpu_left = cpu_w * (int) (w[2] >> 16);
		cpu_words(w + 3, n - 3);
		return;
	}

	if ((code & 0xf8) == 0xe0) {         /* environment commands, one per word */
		for (int i = 0; i < n; i++) {
			uint32_t c = w[i] >> 24;
			if (c == 0xe2) {             /* texture window */
				twin = w[i] & 0xfffff;
			} else if (c == 0xe1) {      /* draw mode: the texture page */
				cur_tpage = (int) (w[i] & 0x1ff);
			} else if (c == 0xe3) {      /* draw area, top left */
				clip_x0 = (int) (w[i] & 1023);
				clip_y0 = (int) ((w[i] >> 10) & 511) % H;   /* (the second buffer: y 240..479) */
			} else if (c == 0xe4) {      /* draw area, bottom right */
				clip_x1 = (int) (w[i] & 1023);
				clip_y1 = (int) ((w[i] >> 10) & 511) % H;
				if (clip_x1 > W - 1) clip_x1 = W - 1;
				if (clip_y1 > H - 1) clip_y1 = H - 1;
			}
		}
		return;
	}
	if (code == 0x02 && n >= 3) {        /* a fill (the 2D gun's slot): 16-halfword units */
		int fx = (int) (w[1] & 0x3f0), fy = (int) ((w[1] >> 16) & 511);
		int fw = (int) (((w[2] & 0x3ff) + 15) & ~15u), fh = (int) ((w[2] >> 16) & 511);
		uint32_t c = w[0];
		uint16_t v = (uint16_t) ((c >> 3 & 31) | (c >> 11 & 31) << 5 | (c >> 19 & 31) << 10);
		for (int y = 0; y < fh; y++)
			for (int x = 0; x < fw; x++)
				vram[(fy + y) & 511][(fx + x) & 1023] = v;
		return;
	}
	if (code == 0x80 && n >= 4) {        /* VRAM to VRAM (the animated textures' frames) */
		int sx = (int) (w[1] & 1023), sy = (int) ((w[1] >> 16) & 511);
		int dx = (int) (w[2] & 1023), dy = (int) ((w[2] >> 16) & 511);
		int cw = (int) (w[3] & 0xffff), ch = (int) (w[3] >> 16);
		for (int y = 0; y < ch; y++)
			for (int x = 0; x < cw; x++)
				vram[(dy + y) & 511][(dx + x) & 1023] = vram[(sy + y) & 511][(sx + x) & 1023];
		return;
	}
	if ((code & 0xe0) == 0x60) {         /* rectangles: 011 ss t a r */
		int size = (code >> 3) & 3, tex = code & 4, raw = code & 1;
		int fixed = size == 1 ? 1 : size == 2 ? 8 : size == 3 ? 16 : 0;
		if (tex)
			sprite(w, n, fixed, fixed, raw);
		else
			tile(w, n, fixed, fixed, code & 2);
		return;
	}

	/* polygons (001 g q t s r): flat or Gouraud, textured or not. Per
	   vertex: [colour (Gouraud, after the first)] xy [uv (textured)]. */
	if ((code & 0xe0) != 0x20)
		return;
	int gour = code & 0x10, quad = code & 8, tex = code & 4, raw = code & 1;
	int nv = quad ? 4 : 3, stride = 1 + (tex ? 1 : 0) + (gour ? 1 : 0);
	if (n < nv * stride + (gour ? 0 : 1))
		return;

	vtx v[4];
	int tpage = 0, clut = 0;
	for (int i = 0; i < nv; i++) {
		const uint32_t *pv = gour ? &w[i * stride] : &w[1 + i * stride - 1];
		uint32_t rgb = gour ? pv[0] : w[0];
		uint32_t xy  = pv[1];
		uint32_t uv  = tex ? pv[2] : 0;
		v[i] = mkv(xy, uv, rgb);
		if (i == 0) clut  = (int) (uv >> 16);
		if (i == 1) tpage = (int) (uv >> 16);
	}
	cur_id = polys_drawn < OCCL_MAX ? polys_drawn : -1;
	if (occl_on && cur_id >= 0) {
		int x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
		for (int i = 1; i < nv; i++) {
			if (v[i].x < x0) x0 = v[i].x;
			if (v[i].x > x1) x1 = v[i].x;
			if (v[i].y < y0) y0 = v[i].y;
			if (v[i].y > y1) y1 = v[i].y;
		}
		id_class[cur_id] = x1 < clip_x0 || x0 > clip_x1 || y1 < clip_y0 || y0 > clip_y1 ? 1
		                 : (x1 - x0 < 2 && y1 - y0 < 2) ? 2 : 0;
		id_same[cur_id] = x0 == x1 && y0 == y1;
	}
	polys_drawn++;
	tri(v[0], v[1], v[2], tex, raw, tpage, clut);
	if (quad)
		tri(v[1], v[2], v[3], tex, raw, tpage, clut);
}

/* ----------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
	if (argc < 4 || argc > 6) {
		fprintf(stderr, "usage: pktview <bench.log> <textures.tex> <out-prefix> [game.dat [stream.dat]]\n");
		return 2;
	}
	crc_init();
	if (argc >= 5)                       /* the console's order: the game data at boot, */
		load_game(argv[4]);              /* then the map over it                         */
	if (argc == 6)
		load_stream(argv[5]);
	load_tex(argv[2]);
	const char *dis = getenv("PKTVIEW_DITHER");
	if (dis && dis[0] == '0')
		dither_on = 0;
	const char *occ = getenv("PKTVIEW_OCCL");
	occl_on = occ && occ[0] == '1';
	const char *bgs = getenv("PKTVIEW_BG");
	unsigned br, bgg, bb;
	if (bgs && sscanf(bgs, "%u,%u,%u", &br, &bgg, &bb) == 3) {
		bg[0] = (uint8_t) br;
		bg[1] = (uint8_t) bgg;
		bg[2] = (uint8_t) bb;
	}

	FILE *log = fopen(argv[1], "r");
	if (!log) { fprintf(stderr, "pktview: cannot open %s\n", argv[1]); return 1; }

	static char line[1 << 16];
	static char name[64];
	int view = 0;
	while (fgets(line, sizeof line, log)) {
		if (!strncmp(line, "D ", 2)) {
			/* the frame's name, without spaces ("view 0" -> "view0") */
			int k = 0;
			for (const char *p = line + 2; *p && *p != '\n' && k < 63; p++)
				if (*p != ' ')
					name[k++] = *p;
			name[k] = 0;
			view = 1;
			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++)
					memcpy(fb[y][x], bg, 3);
			polys_drawn = polys_huge = 0;
			memset(idbuf, 0xff, sizeof idbuf);
			memset(id_written, 0, sizeof id_written);
			memset(id_class, 0, sizeof id_class);
			twin = 0;
			cur_tpage = 0;
			clip_x0 = clip_y0 = 0;
			clip_x1 = W - 1;
			clip_y1 = H - 1;
		} else if (!strncmp(line, "S ", 2)) {
			stream_image(line);               /* (VRAM: kept for the dumps after) */
		} else if (view && !strncmp(line, "V ", 2)) {
			int rows = atoi(line + 2);        /* the 3D viewport: the draw area */
			if (rows > 0 && rows <= H)
				clip_y1 = rows - 1;
		} else if (view && line[0] == 'P' && line[1] == ' ') {
			uint32_t w[256];                  /* (a linked packet's most: 255 words,
			                                     the 2D gun's texel copies) */
			int n = 0;
			char *s = line + 1;
			while (n < 256) {
				char *e;
				unsigned long x = strtoul(s, &e, 16);
				if (e == s) break;
				w[n++] = (uint32_t) x;
				s = e;
			}
			packet(w, n);
		} else if (view && line[0] == 'E') {
			char path[512];
			snprintf(path, sizeof path, "%s%s.png", argv[3], name);
			write_png(path);
			printf("pktview: %s — %d primitives (%d too large for the GPU)\n",
			       path, polys_drawn, polys_huge);
			if (occl_on) {
				static char seen[OCCL_MAX];
				memset(seen, 0, sizeof seen);
				long fill = 0, shown_px = 0;
				for (int y = 0; y < H; y++)
					for (int x = 0; x < W; x++)
						if (idbuf[y][x] >= 0) { seen[idbuf[y][x]] = 1; shown_px++; }
				int n = polys_drawn < OCCL_MAX ? polys_drawn : OCCL_MAX, wrote = 0, shown = 0;
				int offs = 0, tiny = 0, other = 0, same = 0, same_shown = 0;
				for (int i = 0; i < n; i++) {
					fill += id_written[i];
					wrote += id_written[i] > 0;
					shown += seen[i];
					same += id_same[i];
					same_shown += id_same[i] && id_written[i];
					if (!id_written[i]) {
						if (id_class[i] == 1) offs++;
						else if (id_class[i] == 2) tiny++;
						else other++;
					}
				}
				printf("occl: %s no pixel: off the draw area %d, under 2 x 2 px %d, other %d; all corners on one pixel %d (drawing %d)\n",
				       name, offs, tiny, other, same, same_shown);
				printf("occl: %s polygons %d, wrote a pixel %d, seen at the end %d (hidden %d = %.1f%%), "
				       "pixels written %ld for %ld shown (overdraw %.2f)\n", name, n, wrote, shown,
				       wrote - shown, 100.0 * (wrote - shown) / (wrote ? wrote : 1), fill, shown_px,
				       shown_px ? (double) fill / shown_px : 0.0);
			}
			view = 0;
		}
	}
	fclose(log);
	return 0;
}
