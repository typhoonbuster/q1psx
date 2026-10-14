/*
 * pktview — render a q1psx display-list dump to PNG images (host tool).
 *
 *   pktview <bench.log> <textures.tex> <out-prefix>
 *
 * The bench build prints, for one frame per viewpoint, "D view <n>" followed
 * by the frame's packets in GPU draw order ("P <hex words>") and "E".
 * pktview rebuilds VRAM from the QTX3 texture file (CLUT + pages) and
 * rasterises the textured polygons the way the PS1 GPU does — affine texture
 * mapping in screen space, no depth buffer, painter's order — into
 * <out-prefix><n>.png (320x240). It is how the renderer's output is checked
 * without a screen; it does not emulate GPU timing.
 *
 * Supported: POLY_FT3 / POLY_FT4 (raw or blended), flat/gouraud polygons
 * drawn as flat. Everything else (sprites, debug font) is skipped.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../formats/qtex.h"

#define W 320
#define H 240

static uint16_t vram[512][1024];
static uint8_t  fb[H][W][3];

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

	qtex_header h;
	memcpy(&h, d, sizeof h);
	if (memcmp(h.magic, QTEX_MAGIC, 4)) { fprintf(stderr, "pktview: not a QTX3 file\n"); exit(1); }
	memcpy(&vram[QTEX_CLUT_Y][QTEX_CLUT_X], h.clut, sizeof h.clut);
	for (uint32_t i = 0; i < h.num_rects; i++) {
		qtex_rect r;
		memcpy(&r, d + sizeof h + i * sizeof r, sizeof r);
		const uint16_t *px = (const uint16_t *) (d + r.data_ofs);
		for (int y = 0; y < r.h; y++)
			memcpy(&vram[r.y + y][r.x], px + y * r.w, r.w * 2u);
	}
	free(d);
}

/* --------------------------------------------------------------- raster -- */

typedef struct { int x, y; int u, v; } vtx;

static void texel(int tpage, int clut, int u, int v, uint8_t out[3], int *opaque)
{
	int px = (tpage & 15) * 64, py = ((tpage >> 4) & 1) * 256;
	int mode = (tpage >> 7) & 3;
	int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
	uint16_t c;
	u &= 255;
	v &= 255;
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

/* Affine triangle: barycentric interpolation of u,v in screen space. */
static void tri(vtx a, vtx b, vtx c, int textured, int tpage, int clut, const uint8_t rgb[3])
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
	if (minx < 0) minx = 0;
	if (miny < 0) miny = 0;
	if (maxx > W - 1) maxx = W - 1;
	if (maxy > H - 1) maxy = H - 1;

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
			uint8_t col[3] = { rgb[0], rgb[1], rgb[2] };
			if (textured) {
				int u = (int) ((w0 * a.u + w1 * b.u + w2 * c.u) / area);
				int v = (int) ((w0 * a.v + w1 * b.v + w2 * c.v) / area);
				int op;
				texel(tpage, clut, u, v, col, &op);
				if (!op)
					continue;
			}
			memcpy(fb[y][x], col, 3);
		}
}

static vtx mkv(uint32_t xy, uint32_t uv)
{
	vtx v;
	v.x = (int16_t) (xy & 0xffff);
	v.y = (int16_t) (xy >> 16);
	v.u = (int) (uv & 255);
	v.v = (int) ((uv >> 8) & 255);
	return v;
}

static void packet(const uint32_t *w, int n)
{
	uint32_t code = w[0] >> 24;
	uint8_t rgb[3] = { (uint8_t) w[0], (uint8_t) (w[0] >> 8), (uint8_t) (w[0] >> 16) };

	/* flat polygons only (textured or not): 001 g q t s r */
	if ((code & 0xe0) != 0x20 || (code & 0x10))
		return;
	int quad = code & 8, tex = code & 4;
	int nv = quad ? 4 : 3, stride = tex ? 2 : 1;
	if (n < 1 + nv * stride)
		return;

	vtx v[4];
	int tpage = 0, clut = 0;
	for (int i = 0; i < nv; i++) {
		uint32_t xy = w[1 + i * stride];
		uint32_t uv = tex ? w[2 + i * stride] : 0;
		v[i] = mkv(xy, uv);
		if (i == 0) clut  = (int) (uv >> 16);
		if (i == 1) tpage = (int) (uv >> 16);
	}
	polys_drawn++;
	tri(v[0], v[1], v[2], tex, tpage, clut, rgb);
	if (quad)
		tri(v[1], v[2], v[3], tex, tpage, clut, rgb);
}

/* ----------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: pktview <bench.log> <textures.tex> <out-prefix>\n");
		return 2;
	}
	crc_init();
	load_tex(argv[2]);

	FILE *log = fopen(argv[1], "r");
	if (!log) { fprintf(stderr, "pktview: cannot open %s\n", argv[1]); return 1; }

	static char line[1 << 16];
	int view = -1;
	while (fgets(line, sizeof line, log)) {
		if (!strncmp(line, "D view ", 7)) {
			view = atoi(line + 7);
			memset(fb, 0, sizeof fb);
			polys_drawn = polys_huge = 0;
		} else if (view >= 0 && line[0] == 'P' && line[1] == ' ') {
			uint32_t w[16];
			int n = 0;
			char *s = line + 1;
			while (n < 16) {
				char *e;
				unsigned long x = strtoul(s, &e, 16);
				if (e == s) break;
				w[n++] = (uint32_t) x;
				s = e;
			}
			packet(w, n);
		} else if (view >= 0 && line[0] == 'E') {
			char path[512];
			snprintf(path, sizeof path, "%s%d.png", argv[3], view);
			write_png(path);
			printf("pktview: %s — %d polygons (%d too large for the GPU)\n",
			       path, polys_drawn, polys_huge);
			view = -1;
		}
	}
	fclose(log);
	return 0;
}
