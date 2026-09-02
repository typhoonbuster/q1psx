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

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fidelity.h"
#include "qtx.h"
#include "qgeo.h"
#include "qsrf.h"

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

static const char *arg_after(int argc, char **argv, const char *key)
{
	for (int i = 1; i < argc - 1; i++)
		if (!strcmp(argv[i], key))
			return argv[i + 1];
	return NULL;
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

static uint16_t rd_u16(const uint8_t *p)
{
	return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

static int32_t rd_i32(const uint8_t *p) { return (int32_t) rd_u32(p); }
static int16_t rd_i16(const uint8_t *p) { return (int16_t) rd_u16(p); }

static float rd_f32(const uint8_t *p) /* host is LE IEEE-754 */
{
	float f;
	uint32_t u = rd_u32(p);
	memcpy(&f, &u, 4);
	return f;
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
	int      w, h;          /* cooked size (<= 64)    */
	int      src_w, src_h;  /* original size          */
	uint8_t  flags;
	int      vram_x, vram_y;/* atlas slot, word coords */
	uint8_t *pixels;        /* w*h, owned             */
} cooked_tex_t;

#define WORLD_TEX_CAP 64   /* atlas cell size; fidelity tunes this later */

/* Extract every miptex slot, index-aligned (gaps become a 2x2 marker) and
   assign each a slot in the VRAM atlas grid. */
static int extract_textures(const uint8_t *bsp, uint32_t bsp_len,
                            const fidelity_spec_t *fs,
                            cooked_tex_t **out, int *out_n)
{
	if (bsp_len < 4 + HEADER_LUMPS * 8)
		die("bsp too small");
	if (rd_u32(bsp) != BSPVERSION)
		die("bsp version %u, expected %d", rd_u32(bsp), BSPVERSION);

	int cap = fs->max_texture < WORLD_TEX_CAP ? fs->max_texture : WORLD_TEX_CAP;

	const uint8_t *lump = bsp + 4 + LUMP_TEXTURES * 8;
	uint32_t tex_ofs = rd_u32(lump), tex_len = rd_u32(lump + 4);
	if (tex_ofs + tex_len > bsp_len || tex_len < 4)
		die("bad texture lump");

	const uint8_t *tl = bsp + tex_ofs;
	int nummiptex = (int) rd_u32(tl);

	if (nummiptex > QTX_ATLAS_COLS * QTX_ATLAS_ROWS)
		die("%d textures — atlas holds %d", nummiptex, QTX_ATLAS_COLS * QTX_ATLAS_ROWS);

	cooked_tex_t *list = xmalloc(sizeof(*list) * (size_t) nummiptex);

	for (int i = 0; i < nummiptex; i++) {
		cooked_tex_t *c = &list[i];
		memset(c, 0, sizeof(*c));
		c->vram_x = QTX_ATLAS_X + (i % QTX_ATLAS_COLS) * QTX_ATLAS_CELL_W;
		c->vram_y = QTX_ATLAS_Y + (i / QTX_ATLAS_COLS) * QTX_ATLAS_CELL_H;

		int32_t mtofs = (int32_t) rd_u32(tl + 4 + i * 4);
		const uint8_t *mt = (mtofs >= 0 && (uint32_t) mtofs + 40 <= tex_len)
		                  ? tl + mtofs : NULL;
		int mw = mt ? (int) rd_u32(mt + 16) : 0;
		int mh = mt ? (int) rd_u32(mt + 20) : 0;
		uint32_t moff0 = mt ? rd_u32(mt + 24) : 0;

		if (!mt || mw <= 0 || mh <= 0 || (mw & 15) || (mh & 15) ||
		    moff0 == 0 || (uint32_t) mtofs + moff0 + (uint32_t) mw * mh > tex_len) {
			/* placeholder: 2x2 bright magenta (Quake palette ~251) */
			c->w = c->h = c->src_w = c->src_h = 2;
			c->flags  = QTX_F_PLACEHOLDER;
			c->pixels = xmalloc(4);
			memset(c->pixels, 251, 4);
			if (mt) { memcpy(c->name, mt, 16); c->name[15] = 0; }
			else    snprintf(c->name, 16, "miptex%d", i);
			continue;
		}

		memcpy(c->name, mt, 16);
		c->name[15] = 0;
		c->src_w = mw;
		c->src_h = mh;
		if (c->name[0] == '{')                 c->flags |= QTX_F_MASKED;
		else if (c->name[0] == '*')            c->flags |= QTX_F_LIQUID;
		else if (!strncmp(c->name, "sky", 3))  c->flags |= QTX_F_SKY;

		/* closest built-in mip not smaller than the cap, then nn-downscale */
		int mip = 0;
		while (mip < 3) {
			if ((mw >> (mip + 1)) < cap && (mh >> (mip + 1)) < cap)
				break;
			mip++;
		}
		int pw = mw >> mip, ph = mh >> mip;
		if (pw < 1) pw = 1;
		if (ph < 1) ph = 1;
		const uint8_t *pix = mt + rd_u32(mt + 24 + mip * 4);

		int tw = pw, th = ph;
		if (tw > cap || th > cap) {
			if (tw >= th) { th = th * cap / tw; tw = cap; }
			else          { tw = tw * cap / th; th = cap; }
			tw &= ~1; th &= ~1;
			if (tw < 2) tw = 2;
			if (th < 2) th = 2;
		}
		c->w = tw;
		c->h = th;
		c->pixels = (tw == pw && th == ph)
		          ? memcpy(xmalloc((size_t) tw * th), pix, (size_t) tw * th)
		          : downscale(pix, pw, ph, tw, th);
	}

	*out = list;
	*out_n = nummiptex;
	return nummiptex;
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
		e.vram_x     = (uint16_t) tex[i].vram_x;
		e.vram_y     = (uint16_t) tex[i].vram_y;
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

/* --------------------------------------------------------------- geometry -- */

#define LUMP_VERTEXES  3
#define LUMP_FACES     7
#define LUMP_EDGES    12
#define LUMP_SURFEDGES 13
#define LUMP_MODELS   14

static const uint8_t *bsp_lump(const uint8_t *bsp, uint32_t bsp_len, int idx, uint32_t *len)
{
	const uint8_t *l = bsp + 4 + idx * 8;
	uint32_t ofs = rd_u32(l), l4 = rd_u32(l + 4);
	if (ofs + l4 > bsp_len)
		die("bad lump %d", idx);
	*len = l4;
	return bsp + ofs;
}

static int cook_geometry(const uint8_t *bsp, uint32_t bsp_len, const char *out)
{
	if (bsp_len < 4 + HEADER_LUMPS * 8 || rd_u32(bsp) != BSPVERSION)
		die("not a v%d BSP", BSPVERSION);

	uint32_t vlen, elen, selen, mlen, flen;
	const uint8_t *vlump  = bsp_lump(bsp, bsp_len, LUMP_VERTEXES,  &vlen);
	const uint8_t *elump  = bsp_lump(bsp, bsp_len, LUMP_EDGES,     &elen);
	const uint8_t *selump = bsp_lump(bsp, bsp_len, LUMP_SURFEDGES, &selen);
	const uint8_t *mlump  = bsp_lump(bsp, bsp_len, LUMP_MODELS,    &mlen);
	const uint8_t *flump  = bsp_lump(bsp, bsp_len, LUMP_FACES,     &flen);

	int nverts    = (int) (vlen / 12);
	int nedges    = (int) (elen / 4);
	int nsurfedge = (int) (selen / 4);
	int nfaces    = (int) (flen / 20);
	if (mlen < 64)
		die("no worldmodel");

	/* SVECTOR verts + bounds */
	int16_t *verts = xmalloc((size_t) nverts * 8);
	int16_t mins[3] = { 32767, 32767, 32767 }, maxs[3] = { -32768, -32768, -32768 };
	for (int i = 0; i < nverts; i++) {
		for (int k = 0; k < 3; k++) {
			long v = lround(rd_f32(vlump + i * 12 + k * 4));
			if (v < -32768) v = -32768;
			if (v >  32767) v =  32767;
			verts[i * 4 + k] = (int16_t) v;
			if (v < mins[k]) mins[k] = (int16_t) v;
			if (v > maxs[k]) maxs[k] = (int16_t) v;
		}
		verts[i * 4 + 3] = 0;
	}

	/* mark the edges used by worldmodel (model 0) faces */
	int firstface = rd_i32(mlump + 56);
	int numfaces  = rd_i32(mlump + 60);
	uint8_t *used = xmalloc((size_t) (nedges > 0 ? nedges : 1));
	memset(used, 0, (size_t) (nedges > 0 ? nedges : 1));

	for (int f = firstface; f < firstface + numfaces && f < nfaces; f++) {
		const uint8_t *face = flump + (long) f * 20;
		int fe = rd_i32(face + 4);
		int ne = rd_i16(face + 8);
		for (int s = fe; s < fe + ne && s < nsurfedge; s++) {
			int se  = rd_i32(selump + (long) s * 4);
			int idx = se < 0 ? -se : se;
			if (idx > 0 && idx < nedges)
				used[idx] = 1;
		}
	}

	int nout = 0;
	for (int i = 0; i < nedges; i++)
		if (used[i])
			nout++;

	uint16_t *edges = xmalloc((size_t) nout * 4);
	int w = 0;
	for (int i = 0; i < nedges; i++) {
		if (!used[i])
			continue;
		edges[w * 2 + 0] = rd_u16(elump + i * 4 + 0);
		edges[w * 2 + 1] = rd_u16(elump + i * 4 + 2);
		w++;
	}

	FILE *fp = fopen(out, "wb");
	if (!fp)
		die("cannot write %s", out);

	qgeo_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QGEO_MAGIC, 4);
	h.num_verts = (uint32_t) nverts;
	h.num_edges = (uint32_t) nout;
	memcpy(h.mins, mins, sizeof mins);
	memcpy(h.maxs, maxs, sizeof maxs);

	fwrite(&h, sizeof h, 1, fp);
	fwrite(verts, 8, (size_t) nverts, fp);
	fwrite(edges, 4, (size_t) nout, fp);
	fclose(fp);

	printf("qcook: %s — %d verts, %d edges (of %d), bounds [%d %d %d]..[%d %d %d]\n",
	       out, nverts, nout, nedges,
	       mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2]);

	free(verts);
	free(used);
	free(edges);
	return 0;
}

static int cmd_geometry(int argc, char **argv)
{
	const char *pak_path = arg_after(argc, argv, "--pak");
	const char *map      = arg_after(argc, argv, "--map");
	const char *out      = arg_after(argc, argv, "--out");
	if (!pak_path || !map || !out)
		die("usage: qcook geometry --pak <pak> --map <name> --out <file.qgeo>");

	pak_t pak;
	pak_load(&pak, pak_path);

	uint32_t bsp_len;
	const uint8_t *bsp = pak_lump(&pak, map, &bsp_len);
	if (!bsp)
		die("%s not found in %s", map, pak_path);

	return cook_geometry(bsp, bsp_len, out);
}

/* --------------------------------------------------------------- surfaces -- */

#define LUMP_TEXINFO 6

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int cook_surfaces(const uint8_t *bsp, uint32_t bsp_len, const char *out)
{
	if (bsp_len < 4 + HEADER_LUMPS * 8 || rd_u32(bsp) != BSPVERSION)
		die("not a v%d BSP", BSPVERSION);

	uint32_t vlen, elen, selen, mlen, flen, tilen;
	const uint8_t *vlump  = bsp_lump(bsp, bsp_len, LUMP_VERTEXES,  &vlen);
	const uint8_t *elump  = bsp_lump(bsp, bsp_len, LUMP_EDGES,     &elen);
	const uint8_t *selump = bsp_lump(bsp, bsp_len, LUMP_SURFEDGES, &selen);
	const uint8_t *mlump  = bsp_lump(bsp, bsp_len, LUMP_MODELS,    &mlen);
	const uint8_t *flump  = bsp_lump(bsp, bsp_len, LUMP_FACES,     &flen);
	const uint8_t *tilump = bsp_lump(bsp, bsp_len, LUMP_TEXINFO,   &tilen);

	int nverts    = (int) (vlen / 12);
	int nedges    = (int) (elen / 4);
	int nsurfedge = (int) (selen / 4);
	int nfaces    = (int) (flen / 20);
	int ntexinfo  = (int) (tilen / 40);
	if (mlen < 64)
		die("no worldmodel");

	/* vert pool + bounds (same as geometry) */
	int16_t *verts = xmalloc((size_t) nverts * 8);
	int16_t mins[3] = { 32767, 32767, 32767 }, maxs[3] = { -32768, -32768, -32768 };
	float  *vf = xmalloc((size_t) nverts * 3 * sizeof(float));
	for (int i = 0; i < nverts; i++) {
		for (int k = 0; k < 3; k++) {
			float f = rd_f32(vlump + i * 12 + k * 4);
			vf[i * 3 + k] = f;
			long v = lround(f);
			if (v < -32768) v = -32768;
			if (v >  32767) v =  32767;
			verts[i * 4 + k] = (int16_t) v;
			if (v < mins[k]) mins[k] = (int16_t) v;
			if (v > maxs[k]) maxs[k] = (int16_t) v;
		}
		verts[i * 4 + 3] = 0;
	}

	int firstface = rd_i32(mlump + 56);
	int numfaces  = rd_i32(mlump + 60);

	/* worst-case sizes */
	int max_faces = numfaces;
	int max_fv    = nsurfedge;   /* one facevert per surfedge, generous */
	qsrf_face     *faces = xmalloc((size_t) max_faces * sizeof(qsrf_face));
	qsrf_facevert *fv    = xmalloc((size_t) max_fv * sizeof(qsrf_facevert));
	int nout_faces = 0, nout_fv = 0;

	for (int f = firstface; f < firstface + numfaces && f < nfaces; f++) {
		const uint8_t *face = flump + (long) f * 20;
		int fe = rd_i32(face + 4);
		int ne = rd_i16(face + 8);
		int ti = rd_i16(face + 10);
		if (ne < 3 || ti < 0 || ti >= ntexinfo)
			continue;

		const uint8_t *tx = tilump + (long) ti * 40;
		float s0[4], s1[4];
		for (int k = 0; k < 4; k++) s0[k] = rd_f32(tx + k * 4);
		for (int k = 0; k < 4; k++) s1[k] = rd_f32(tx + 16 + k * 4);
		int miptex = rd_i32(tx + 32);
		int tflags = rd_i32(tx + 36);

		/* gather the polygon's vertex indices in winding order */
		int vidx[64];
		int nv = 0;
		for (int s = fe; s < fe + ne && s < nsurfedge && nv < 64; s++) {
			int se  = rd_i32(selump + (long) s * 4);
			int eix = se < 0 ? -se : se;
			if (eix < 0 || eix >= nedges)
				continue;
			int vi = se >= 0 ? rd_u16(elump + eix * 4 + 0)
			                 : rd_u16(elump + eix * 4 + 2);
			if (vi >= 0 && vi < nverts)
				vidx[nv++] = vi;
		}
		if (nv < 3)
			continue;

		/* s/t per vertex, then shift so the face-local minimum is 0 */
		double st[64][2], smin = 1e30, tmin = 1e30;
		for (int i = 0; i < nv; i++) {
			const float *p = &vf[vidx[i] * 3];
			double sc = p[0] * s0[0] + p[1] * s0[1] + p[2] * s0[2] + s0[3];
			double tc = p[0] * s1[0] + p[1] * s1[1] + p[2] * s1[2] + s1[3];
			st[i][0] = sc; st[i][1] = tc;
			if (sc < smin) smin = sc;
			if (tc < tmin) tmin = tc;
		}
		smin = floor(smin);
		tmin = floor(tmin);

		qsrf_face *of = &faces[nout_faces++];
		of->tex      = (uint16_t) (miptex < 0 ? 0 : miptex);
		of->first_fv = (uint16_t) nout_fv;
		of->num_fv   = (uint8_t) nv;
		of->flags    = 0;
		of->pad[0] = of->pad[1] = 0;
		if (tflags & 1) {   /* TEX_SPECIAL */
			/* name-based classification is more reliable; leave to the engine */
		}

		for (int i = 0; i < nv; i++) {
			qsrf_facevert *ofv = &fv[nout_fv++];
			ofv->vert = (uint16_t) vidx[i];
			ofv->u = (uint8_t) clampi((int) lround(st[i][0] - smin), 0, 255);
			ofv->v = (uint8_t) clampi((int) lround(st[i][1] - tmin), 0, 255);
		}
	}

	FILE *fp = fopen(out, "wb");
	if (!fp)
		die("cannot write %s", out);
	qsrf_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QSRF_MAGIC, 4);
	h.num_verts     = (uint32_t) nverts;
	h.num_faces     = (uint32_t) nout_faces;
	h.num_faceverts = (uint32_t) nout_fv;
	memcpy(h.mins, mins, sizeof mins);
	memcpy(h.maxs, maxs, sizeof maxs);
	fwrite(&h, sizeof h, 1, fp);
	fwrite(verts, 8, (size_t) nverts, fp);
	fwrite(faces, sizeof(qsrf_face), (size_t) nout_faces, fp);
	fwrite(fv, sizeof(qsrf_facevert), (size_t) nout_fv, fp);
	fclose(fp);

	printf("qcook: %s — %d verts, %d faces, %d faceverts\n",
	       out, nverts, nout_faces, nout_fv);

	free(verts); free(vf); free(faces); free(fv);
	return 0;
}

static int cmd_surfaces(int argc, char **argv)
{
	const char *pak_path = arg_after(argc, argv, "--pak");
	const char *map      = arg_after(argc, argv, "--map");
	const char *out      = arg_after(argc, argv, "--out");
	if (!pak_path || !map || !out)
		die("usage: qcook surfaces --pak <pak> --map <name> --out <file.qsrf>");

	pak_t pak;
	pak_load(&pak, pak_path);
	uint32_t bsp_len;
	const uint8_t *bsp = pak_lump(&pak, map, &bsp_len);
	if (!bsp)
		die("%s not found in %s", map, pak_path);
	return cook_surfaces(bsp, bsp_len, out);
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
		printf("  %2u %-16s %3ux%-3u (src %3ux%-3u) f%u  vram(%u,%u)  %uB\n",
		       i, nm, e[i].width, e[i].height, e[i].src_width, e[i].src_height,
		       e[i].flags, e[i].vram_x, e[i].vram_y, e[i].data_len);
		total += e[i].data_len;
	}
	printf("  %u bytes of pixel data\n", total);
	free(buf);
	return 0;
}

/* ------------------------------------------------------------------- main -- */

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
	if (argc >= 2 && !strcmp(argv[1], "geometry"))
		return cmd_geometry(argc, argv);
	if (argc >= 2 && !strcmp(argv[1], "surfaces"))
		return cmd_surfaces(argc, argv);
	if (argc == 3 && !strcmp(argv[1], "info"))
		return cmd_info(argv[2]);

	fprintf(stderr,
	        "usage:\n"
	        "  qcook textures --pak <pak> --map <name> --out <file.qtx> [--fidelity low|medium|high|ultra]\n"
	        "  qcook geometry --pak <pak> --map <name> --out <file.qgeo>\n"
	        "  qcook surfaces --pak <pak> --map <name> --out <file.qsrf>\n"
	        "  qcook info <file.qtx>\n");
	return 2;
}
