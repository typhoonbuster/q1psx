/*
 * q1psx — asset cooker (host tool).
 *
 * Phase 4.1: extract Quake's global palette + the textures embedded in a BSP
 * and write a cooked texture pack (qtx.h). Fidelity controls the max texture
 * size. Reads straight from a .pak — see fidelity.h for the level table.
 *
 *   qcook textures --pak <PAK0.PAK> --map maps/start.bsp \
 *                  [--fidelity high] --out cooked/start.qtx
 *   qcook info <file.qtx>
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fidelity.h"
#include "qtx.h"

/* ------------------------------------------------------------------- util -- */

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "qcook: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		die("out of memory (%zu bytes)", n);
	return p;
}

static uint8_t *read_file(const char *path, long *out_len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		die("cannot open %s", path);
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = xmalloc((size_t) len);
	if (fread(buf, 1, (size_t) len, f) != (size_t) len)
		die("short read on %s", path);
	fclose(f);
	*out_len = len;
	return buf;
}

static uint32_t rd_u32(const uint8_t *p) /* little-endian */
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* -------------------------------------------------------------------- pak -- */

typedef struct {
	const uint8_t *base;
	long           size;
	int            numfiles;
	const uint8_t *dir;   /* -> directory entries, 64 bytes each */
} pak_t;

static void pak_load(pak_t *pak, const char *path)
{
	long len;
	pak->base = read_file(path, &len);
	pak->size = len;

	if (len < 12 || memcmp(pak->base, "PACK", 4) != 0)
		die("%s is not a PACK file", path);

	uint32_t dirofs = rd_u32(pak->base + 4);
	uint32_t dirlen = rd_u32(pak->base + 8);
	if (dirofs + dirlen > (uint32_t) len)
		die("%s: directory out of range", path);

	pak->numfiles = (int) (dirlen / 64);
	pak->dir      = pak->base + dirofs;
}

static const uint8_t *pak_lump(const pak_t *pak, const char *name, uint32_t *out_len)
{
	for (int i = 0; i < pak->numfiles; i++) {
		const uint8_t *e = pak->dir + (long) i * 64;
		if (strncmp((const char *) e, name, 56) == 0) {
			uint32_t pos = rd_u32(e + 56);
			uint32_t l   = rd_u32(e + 60);
			if (pos + l > (uint32_t) pak->size)
				die("lump %s out of range", name);
			if (out_len)
				*out_len = l;
			return pak->base + pos;
		}
	}
	return NULL;
}

/* ---------------------------------------------------------------- palette -- */

static void build_clut(uint16_t clut[256], const uint8_t *pal /* 768 bytes RGB */)
{
	for (int i = 0; i < 256; i++) {
		int r = pal[i * 3 + 0] >> 3;
		int g = pal[i * 3 + 1] >> 3;
		int b = pal[i * 3 + 2] >> 3;
		clut[i] = (uint16_t) (r | (g << 5) | (b << 10)); /* PS1 BGR555 */
	}
}

/* ------------------------------------------------------------------- bsp -- */

#define BSPVERSION    29
#define LUMP_TEXTURES 2
#define HEADER_LUMPS  15

/* nearest-neighbour downscale of an 8bpp image to dw x dh */
static uint8_t *downscale(const uint8_t *src, int sw, int sh, int dw, int dh)
{
	uint8_t *dst = xmalloc((size_t) dw * dh);
	for (int y = 0; y < dh; y++) {
		int sy = y * sh / dh;
		for (int x = 0; x < dw; x++) {
			int sx = x * sw / dw;
			dst[y * dw + x] = src[sy * sw + sx];
		}
	}
	return dst;
}

typedef struct {
	char     name[16];
	int      w, h;          /* cooked size            */
	int      src_w, src_h;  /* original size          */
	uint8_t  flags;
	uint8_t *pixels;        /* w*h, owned             */
} cooked_tex_t;

static int extract_textures(const uint8_t *bsp, uint32_t bsp_len,
                            const fidelity_spec_t *fs,
                            cooked_tex_t **out, int *out_n)
{
	if (bsp_len < 4 + HEADER_LUMPS * 8)
		die("bsp too small");
	if (rd_u32(bsp) != BSPVERSION)
		die("bsp version %u, expected %d", rd_u32(bsp), BSPVERSION);

	const uint8_t *lump = bsp + 4 + LUMP_TEXTURES * 8;
	uint32_t tex_ofs = rd_u32(lump);
	uint32_t tex_len = rd_u32(lump + 4);
	if (tex_ofs + tex_len > bsp_len || tex_len < 4)
		die("bad texture lump");

	const uint8_t *tl = bsp + tex_ofs;
	int nummiptex = (int) rd_u32(tl);

	cooked_tex_t *list = xmalloc(sizeof(*list) * (size_t) nummiptex);
	int n = 0;

	for (int i = 0; i < nummiptex; i++) {
		int32_t mtofs = (int32_t) rd_u32(tl + 4 + i * 4);
		if (mtofs < 0 || (uint32_t) mtofs + 40 > tex_len)
			continue; /* missing / referenced-from-wad */

		const uint8_t *mt = tl + mtofs;
		int mw = (int) rd_u32(mt + 16);
		int mh = (int) rd_u32(mt + 20);
		uint32_t moff[4] = { rd_u32(mt + 24), rd_u32(mt + 28),
		                     rd_u32(mt + 32), rd_u32(mt + 36) };
		/* Quake sizes are multiples of 16 (so the >>mip below is exact) but not
		   necessarily powers of two — e.g. "quake" is 288x64. Keep them; the
		   renderer subdivides surfaces per texture tile anyway. */
		if (mw <= 0 || mh <= 0 || mw > 1024 || mh > 1024 || (mw & 15) || (mh & 15))
			continue;

		/* Pick the built-in (box-filtered) mip closest to the cap without
		   undershooting it, then nearest-neighbour down to the exact size. */
		int cap = fs->max_texture;
		int mip = 0;
		while (mip < 3) {
			int nw = mw >> (mip + 1), nh = mh >> (mip + 1);
			if (nw < cap && nh < cap)
				break;
			mip++;
		}
		int pw = mw >> mip, ph = mh >> mip;
		if (pw < 1) pw = 1;
		if (ph < 1) ph = 1;
		if (moff[mip] == 0 || (uint32_t) mtofs + moff[mip] + (uint32_t) pw * ph > tex_len)
			continue;

		const uint8_t *pix = mt + moff[mip];

		/* fit the cap, preserving aspect; keep dims even for the GPU */
		int tw = pw, th = ph;
		if (tw > cap || th > cap) {
			if (tw >= th) { th = th * cap / tw; tw = cap; }
			else          { tw = tw * cap / th; th = cap; }
			tw &= ~1; th &= ~1;
			if (tw < 2) tw = 2;
			if (th < 2) th = 2;
		}

		cooked_tex_t *c = &list[n++];
		memset(c, 0, sizeof(*c));
		memcpy(c->name, mt, 16);
		c->name[15] = 0;
		c->src_w = mw;
		c->src_h = mh;
		c->w = tw;
		c->h = th;
		if (c->name[0] == '{') c->flags |= QTX_F_MASKED;
		else if (c->name[0] == '*') c->flags |= QTX_F_LIQUID;
		else if (!strncmp(c->name, "sky", 3)) c->flags |= QTX_F_SKY;

		c->pixels = (tw == pw && th == ph)
		          ? memcpy(xmalloc((size_t) tw * th), pix, (size_t) tw * th)
		          : downscale(pix, pw, ph, tw, th);
	}

	*out = list;
	*out_n = n;
	return n;
}

/* ------------------------------------------------------------------ write -- */

static void write_qtx(const char *path, const uint16_t clut[256],
                      cooked_tex_t *tex, int n)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		die("cannot write %s", path);

	qtx_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QTX_MAGIC, 4);
	memcpy(h.clut, clut, sizeof h.clut);
	h.num_tex = (uint32_t) n;

	uint32_t data_ofs = (uint32_t) (sizeof(qtx_header) + (size_t) n * sizeof(qtx_entry));

	fwrite(&h, sizeof h, 1, f);

	uint32_t cur = data_ofs;
	for (int i = 0; i < n; i++) {
		qtx_entry e;
		memset(&e, 0, sizeof e);
		memcpy(e.name, tex[i].name, QTX_NAMELEN);
		e.width      = (uint16_t) tex[i].w;
		e.height     = (uint16_t) tex[i].h;
		e.src_width  = (uint16_t) tex[i].src_w;
		e.src_height = (uint16_t) tex[i].src_h;
		e.flags      = tex[i].flags;
		e.data_ofs   = cur;
		e.data_len   = (uint32_t) (tex[i].w * tex[i].h);
		fwrite(&e, sizeof e, 1, f);
		cur += (e.data_len + 3u) & ~3u;
	}

	for (int i = 0; i < n; i++) {
		uint32_t len = (uint32_t) (tex[i].w * tex[i].h);
		fwrite(tex[i].pixels, 1, len, f);
		static const uint8_t z[4] = { 0 };
		if (len & 3)
			fwrite(z, 1, 4 - (len & 3), f);
	}

	fclose(f);
	printf("qcook: wrote %s — %d textures, %u bytes\n", path, n, cur);
}

/* ------------------------------------------------------------------- info -- */

static int cmd_info(const char *path)
{
	long len;
	uint8_t *buf = read_file(path, &len);
	if (len < (long) sizeof(qtx_header) || memcmp(buf, QTX_MAGIC, 4) != 0)
		die("%s is not a QTX1 file", path);

	const qtx_header *h = (const qtx_header *) buf;
	printf("%s: %u textures\n", path, h->num_tex);

	int nz = 0;
	for (int i = 0; i < 256; i++)
		if (h->clut[i])
			nz++;
	printf("  clut: %d/256 non-zero entries\n", nz);

	const qtx_entry *e = (const qtx_entry *) (buf + sizeof(qtx_header));
	uint32_t total = 0;
	for (uint32_t i = 0; i < h->num_tex; i++) {
		char nm[QTX_NAMELEN + 1];
		memcpy(nm, e[i].name, QTX_NAMELEN);
		nm[QTX_NAMELEN] = 0;
		printf("  %-16s %3ux%-3u (src %3ux%-3u) flags %u  %u B @ %u\n",
		       nm, e[i].width, e[i].height, e[i].src_width, e[i].src_height,
		       e[i].flags, e[i].data_len, e[i].data_ofs);
		total += e[i].data_len;
	}
	printf("  %u bytes of pixel data\n", total);
	free(buf);
	return 0;
}

/* ------------------------------------------------------------------- main -- */

static const char *arg_after(int argc, char **argv, const char *key)
{
	for (int i = 1; i < argc - 1; i++)
		if (!strcmp(argv[i], key))
			return argv[i + 1];
	return NULL;
}

static int cmd_textures(int argc, char **argv)
{
	const char *pak_path = arg_after(argc, argv, "--pak");
	const char *map      = arg_after(argc, argv, "--map");
	const char *out      = arg_after(argc, argv, "--out");
	const char *fid_name = arg_after(argc, argv, "--fidelity");
	if (!pak_path || !map || !out)
		die("usage: qcook textures --pak <pak> --map <name> --out <file.qtx> [--fidelity high]");

	const fidelity_spec_t *fs = &FIDELITY[FIDELITY_HIGH];
	if (fid_name) {
		fs = NULL;
		for (int i = 0; i < FIDELITY_COUNT; i++)
			if (!strcmp(fid_name, FIDELITY[i].name))
				fs = &FIDELITY[i];
		if (!fs)
			die("unknown fidelity '%s'", fid_name);
	}

	pak_t pak;
	pak_load(&pak, pak_path);

	uint32_t pal_len;
	const uint8_t *pal = pak_lump(&pak, "gfx/palette.lmp", &pal_len);
	if (!pal || pal_len < 768)
		die("gfx/palette.lmp missing or short in %s", pak_path);
	uint16_t clut[256];
	build_clut(clut, pal);

	uint32_t bsp_len;
	const uint8_t *bsp = pak_lump(&pak, map, &bsp_len);
	if (!bsp)
		die("%s not found in %s", map, pak_path);

	cooked_tex_t *tex;
	int n;
	extract_textures(bsp, bsp_len, fs, &tex, &n);

	printf("qcook: %s @ fidelity '%s' (max_texture %d) -> %d textures\n",
	       map, fs->name, fs->max_texture, n);

	write_qtx(out, clut, tex, n);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "textures"))
		return cmd_textures(argc, argv);
	if (argc == 3 && !strcmp(argv[1], "info"))
		return cmd_info(argv[2]);

	fprintf(stderr,
	        "usage:\n"
	        "  qcook textures --pak <pak> --map <name> --out <file.qtx> [--fidelity low|medium|high|ultra]\n"
	        "  qcook info <file.qtx>\n");
	return 2;
}
