/*
 * qcook — textures: palette -> CLUT, one tile per used miptex (one texture
 * repetition at the fidelity's resolution; on an axis that is not a power of
 * two, possibly several), packed into whole 8bpp texture pages, each
 * power-of-two axis aligned for the GPU texture window, written as a QTX3
 * file (formats/qtex.h). Optionally the same pages in 4bpp, each texture
 * quantized to its own 16 colours (QTX4).
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qtex.h"
#include "../../formats/qworld.h"

#define PAGE_TEX 256                     /* texels per page side (8bpp) */

#define CELLS    (PAGE_TEX / 8)          /* packing grid: 8x8-texel cells */

typedef struct { int idx, w, h; } pack_item;

/* biggest first: first fit then packs aligned power-of-two tiles tightly */
static int by_area(const void *a, const void *b)
{
	const pack_item *x = a, *y = b;
	if (x->w * x->h != y->w * y->h) return y->w * y->h - x->w * x->h;
	return y->h - x->h;
}

static int is_pow2(int v)
{
	return v > 0 && !(v & (v - 1));
}

static uint16_t gpu_tpage(int vx, int vy)   /* 8bpp, no semi-transparency */
{
	return (uint16_t) (((vx & 0x3ff) >> 6) | ((vy >> 8) << 4) | (1 << 7));
}

static uint16_t gpu_tpage4(int vx, int vy)  /* 4bpp, no semi-transparency */
{
	return (uint16_t) (((vx & 0x3ff) >> 6) | ((vy >> 8) << 4));
}

static uint16_t gpu_clut(int cx, int cy)
{
	return (uint16_t) ((cy << 6) | (cx >> 4));
}

/* Nearest-neighbour from the closest Quake mip at or above the target size. */
static uint8_t sample(const bsp_miptex *m, int x, int y, int tw, int th)
{
	int k = 0;
	while (k < 3 && m->mip[k + 1] && (m->w >> (k + 1)) >= tw && (m->h >> (k + 1)) >= th)
		k++;
	int sw = m->w >> k, sh = m->h >> k;
	return m->mip[k][(y * sh / th) * sw + (x * sw / tw)];
}

/* ------------------------------------------------------------ 4bpp -- */

/* A colour the texture uses: its palette entry and how many texels. */
typedef struct { int r, g, b; uint32_t w; } qpoint;

/* Perceptual-ish distance (green weighs most, blue least). */
static uint32_t qdist(int dr, int dg, int db)
{
	return 3u * (uint32_t) (dr * dr) + 4u * (uint32_t) (dg * dg) + 2u * (uint32_t) (db * db);
}

static int qchan(const qpoint *p, int c)
{
	return c == 0 ? p->r : c == 1 ? p->g : p->b;
}

static int qsort_chan;
static int by_chan(const void *a, const void *b)
{
	return qchan(a, qsort_chan) - qchan(b, qsort_chan);
}

/* Weighted spread of a box along channel c (sum of w (x - mean)^2, scaled by
   the metric's channel weight). */
static double box_sse(const qpoint *p, int n, int c)
{
	static const double cw[3] = { 3, 4, 2 };
	double sw = 0, s = 0, s2 = 0;
	for (int i = 0; i < n; i++) {
		double x = qchan(&p[i], c);
		sw += p[i].w;
		s  += p[i].w * x;
		s2 += p[i].w * x * x;
	}
	return sw > 0 ? cw[c] * (s2 - s * s / sw) : 0;
}

/* 16 colours for one texture from the histogram of its palette indices:
   median cut on the weighted colours (split the box and channel with the
   largest spread at the weighted median), then Lloyd iterations. map[i] =
   the 4-bit index of palette entry i (for those used), clut = the colours
   in PS1 BGR555, cut to 5 bits as the 8bpp CLUT is (black = 0x8000, 0x0000
   would be transparent). */
static void quantize16(const uint32_t hist[256], const uint8_t pal[768],
                       uint8_t map[256], uint16_t clut[16])
{
	qpoint pts[256];
	int    idx[256], n = 0;
	for (int i = 0; i < 256; i++)
		if (hist[i]) {
			pts[n].r = pal[i * 3];
			pts[n].g = pal[i * 3 + 1];
			pts[n].b = pal[i * 3 + 2];
			pts[n].w = hist[i];
			idx[n]   = i;
			n++;
		}

	double cr[16], cg[16], cb[16];
	int    k = 0;
	if (n <= 16) {
		for (; k < n; k++) {
			cr[k] = pts[k].r;
			cg[k] = pts[k].g;
			cb[k] = pts[k].b;
		}
	} else {
		/* the points keep their palette index through the sorts */
		qpoint *q = pts;
		int    *qi = idx;
		struct { int start, len; } box[16];
		box[0].start = 0;
		box[0].len   = n;
		int nb = 1;
		while (nb < 16) {
			int bb = -1, bc = 0;
			double best = 0;
			for (int i = 0; i < nb; i++)
				if (box[i].len > 1)
					for (int c = 0; c < 3; c++) {
						double e = box_sse(q + box[i].start, box[i].len, c);
						if (e > best) { best = e; bb = i; bc = c; }
					}
			if (bb < 0)
				break;                         /* every box is one colour */
			int st = box[bb].start, len = box[bb].len;
			/* sort the box's points (and their indices) by channel bc */
			struct { qpoint p; int i; } tmp[256];
			for (int i = 0; i < len; i++) { tmp[i].p = q[st + i]; tmp[i].i = qi[st + i]; }
			qsort_chan = bc;
			qsort(tmp, (size_t) len, sizeof tmp[0], by_chan);   /* qpoint first */
			for (int i = 0; i < len; i++) { q[st + i] = tmp[i].p; qi[st + i] = tmp[i].i; }
			double tw = 0, acc = 0;
			for (int i = 0; i < len; i++) tw += q[st + i].w;
			int m = 1;
			for (int i = 0; i < len - 1; i++) {
				acc += q[st + i].w;
				m = i + 1;
				if (acc >= tw / 2) break;
			}
			box[nb].start = st + m;
			box[nb].len   = len - m;
			box[bb].len   = m;
			nb++;
		}
		for (k = 0; k < nb; k++) {
			double sw = 0, r = 0, g = 0, b = 0;
			for (int i = 0; i < box[k].len; i++) {
				const qpoint *p = &q[box[k].start + i];
				sw += p->w; r += p->w * p->r; g += p->w * p->g; b += p->w * p->b;
			}
			cr[k] = r / sw; cg[k] = g / sw; cb[k] = b / sw;
		}
		/* Lloyd: assign to the nearest, move to the weighted mean */
		for (int it = 0; it < 12; it++) {
			double sr[16] = { 0 }, sg[16] = { 0 }, sb[16] = { 0 }, sw[16] = { 0 };
			for (int i = 0; i < n; i++) {
				int bj = 0;
				uint32_t bd = UINT32_MAX;
				for (int j = 0; j < k; j++) {
					uint32_t d = qdist(pts[i].r - (int) (cr[j] + 0.5), pts[i].g - (int) (cg[j] + 0.5),
					                   pts[i].b - (int) (cb[j] + 0.5));
					if (d < bd) { bd = d; bj = j; }
				}
				sw[bj] += pts[i].w;
				sr[bj] += pts[i].w * pts[i].r;
				sg[bj] += pts[i].w * pts[i].g;
				sb[bj] += pts[i].w * pts[i].b;
			}
			for (int j = 0; j < k; j++)
				if (sw[j] > 0) {
					cr[j] = sr[j] / sw[j];
					cg[j] = sg[j] / sw[j];
					cb[j] = sb[j] / sw[j];
				}
		}
	}

	for (int j = 0; j < 16; j++)
		clut[j] = 0x8000;
	for (int j = 0; j < k; j++) {
		int r = (int) (cr[j] + 0.5), g = (int) (cg[j] + 0.5), b = (int) (cb[j] + 0.5);
		uint16_t c = (uint16_t) ((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
		clut[j] = c ? c : 0x8000;
	}
	memset(map, 0, 256);
	for (int i = 0; i < n; i++) {
		int bj = 0;
		uint32_t bd = UINT32_MAX;
		for (int j = 0; j < k; j++) {
			uint32_t d = qdist(pts[i].r - (int) (cr[j] + 0.5), pts[i].g - (int) (cg[j] + 0.5),
			                   pts[i].b - (int) (cb[j] + 0.5));
			if (d < bd) { bd = d; bj = j; }
		}
		map[idx[i]] = (uint8_t) bj;
	}
}

/* The QTX4 file: the pages of `pages` (8bpp indices) re-coded per tile with
   that tile's own 16 colours. */
static void write_tex4(const tex_set *ts, int num_miptex, const uint8_t *pages,
                       const int page_h[], const uint8_t palette[768], const char *path)
{
	int ncl = num_miptex;
	int cl_rows = (ncl + QTEX4_CLUTS_PER_ROW - 1) / QTEX4_CLUTS_PER_ROW;
	if (cl_rows > QTEX4_CLUT_ROWS)
		die("%d textures: more 4-bit CLUTs than the CLUT area holds (%d)", ncl,
		    QTEX4_CLUT_ROWS * QTEX4_CLUTS_PER_ROW);
	int cl_w = 16 * (ncl < QTEX4_CLUTS_PER_ROW ? ncl : QTEX4_CLUTS_PER_ROW);
	uint16_t  *clut = xcalloc((size_t) (cl_w * cl_rows) + 16, 2);
	uint8_t   *nib  = xcalloc(QTEX_PAGES, PAGE_TEX * PAGE_TEX);
	qtex4_tex *tab  = xcalloc((size_t) num_miptex + 1, sizeof *tab);
	double err = 0, npx = 0;

	for (int i = 0; i < num_miptex; i++) {
		const tex_slot *s = &ts->slots[i];
		if (!s->present)
			continue;
		const uint8_t *img = pages + (size_t) s->page * PAGE_TEX * PAGE_TEX;
		uint32_t hist[256] = { 0 };
		for (int y = 0; y < s->tile_h; y++)
			for (int x = 0; x < s->tile_w; x++)
				hist[img[(s->v0 + y) * PAGE_TEX + s->u0 + x]]++;
		uint8_t  map[256];
		uint16_t c16[16];
		quantize16(hist, palette, map, c16);
		uint8_t *n4 = nib + (size_t) s->page * PAGE_TEX * PAGE_TEX;
		for (int y = 0; y < s->tile_h; y++)
			for (int x = 0; x < s->tile_w; x++) {
				int o = (s->v0 + y) * PAGE_TEX + s->u0 + x, pi = img[o];
				n4[o] = map[pi];
				uint16_t c = c16[map[pi]];
				int dr = (palette[pi * 3] >> 3) - (c & 31);
				int dg = (palette[pi * 3 + 1] >> 3) - ((c >> 5) & 31);
				int db = (palette[pi * 3 + 2] >> 3) - ((c >> 10) & 31);
				err += dr * dr + dg * dg + db * db;
				npx++;
			}
		int cx = 16 * (i % QTEX4_CLUTS_PER_ROW), cy = i / QTEX4_CLUTS_PER_ROW;
		memcpy(&clut[cy * cl_w + cx], c16, sizeof c16);
		tab[i].clut  = gpu_clut(cx, QTEX4_CLUT_Y + cy);
		tab[i].tpage = gpu_tpage4(QTEX4_PAGE_X0 + s->page * (PAGE_TEX / 4), 0);
	}

	FILE *f = fopen(path, "wb");
	if (!f)
		die("cannot write %s", path);
	int nrects = (ncl ? 1 : 0) + ts->pages_used;
	qtex4_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QTEX4_MAGIC, 4);
	h.num_rects = (uint32_t) nrects;
	h.num_tex   = (uint32_t) num_miptex;
	h.ofs_tex   = (uint32_t) (sizeof h + (size_t) nrects * sizeof(qtex_rect));
	fwrite(&h, sizeof h, 1, f);

	uint32_t ofs = h.ofs_tex + (uint32_t) num_miptex * (uint32_t) sizeof(qtex4_tex);
	ofs = (ofs + 63u) & ~63u;                            /* (64: the GPU DMA's 16-word unit; zeros follow each rectangle) */
	if (ncl) {
		qtex_rect r = { 0, QTEX4_CLUT_Y, (uint16_t) cl_w, (uint16_t) cl_rows, ofs };
		fwrite(&r, sizeof r, 1, f);
		ofs += (uint32_t) (cl_w * cl_rows * 2);          /* 32 B per CLUT */
		ofs  = (ofs + 63u) & ~63u;
	}
	for (int p = 0; p < ts->pages_used; p++) {
		qtex_rect r;
		r.x = (uint16_t) (QTEX4_PAGE_X0 + p * (PAGE_TEX / 4));
		r.y = 0;
		r.w = PAGE_TEX / 4;
		r.h = (uint16_t) page_h[p];
		r.data_ofs = ofs;
		fwrite(&r, sizeof r, 1, f);
		ofs += (uint32_t) r.w * r.h * 2;                 /* 128 * h bytes */
	}
	fwrite(tab, sizeof *tab, (size_t) num_miptex, f);
	long here = ftell(f);
	static const uint8_t zero[64];
	fwrite(zero, 1, (size_t) (((here + 63) & ~63L) - here), f);
	if (ncl) {
		fwrite(clut, 2, (size_t) (cl_w * cl_rows), f);
		here = ftell(f);
		fwrite(zero, 1, (size_t) (((here + 63) & ~63L) - here), f);
	}
	for (int p = 0; p < ts->pages_used; p++) {
		const uint8_t *n4 = nib + (size_t) p * PAGE_TEX * PAGE_TEX;
		for (int y = 0; y < page_h[p]; y++)
			for (int x = 0; x < PAGE_TEX; x += 4) {
				const uint8_t *t = &n4[y * PAGE_TEX + x];
				uint16_t w = (uint16_t) (t[0] | t[1] << 4 | t[2] << 8 | t[3] << 12);
				fwrite(&w, 2, 1, f);
			}
	}
	long file_len = ftell(f);
	fclose(f);
	printf("qcook: %s — 4bpp, %d CLUTs, file %ld B, rms error %.2f / 31 per channel\n",
	       path, ncl, file_len, npx > 0 ? sqrt(err / npx / 3) : 0.0);
	free(clut);
	free(nib);
	free(tab);
}

/* ------------------------------------------------------------- build -- */

int tex_build(tex_set *ts, const bsp_t *b, const tex_extra *extra, int nextra,
              const uint8_t palette[768], const fidelity_t *fid, const char *out_path,
              const char *out4_path, int anims, int may_fail)
{
	memset(ts, 0, sizeof *ts);

	/* which miptex do the drawn models' faces use? (the world's and the
	   brush models' but the triggers'); the extras are */
	uint8_t *mt_used = xcalloc((size_t) b->num_miptex + 1, 1);
	uint8_t *drawn = xcalloc((size_t) b->num_models, 1);
	ents_drawn_models(b, drawn);
	for (int m = 0; m < b->num_models; m++) {
		if (!drawn[m])
			continue;
		const bsp_model *wm = &b->models[m];
		for (int f = wm->firstface; f < wm->firstface + wm->numfaces; f++) {
			int ti = f >= 0 && f < b->num_faces ? b->faces[f].texinfo : -1;
			if (ti >= 0 && ti < b->num_texinfo) {
				int mt = b->texinfo[ti].miptex;
				if (mt >= 0 && mt < b->num_miptex)
					mt_used[mt] = 1;
			}
		}
	}
	free(drawn);

	/* Quake's animated textures (Mod_LoadTextures, R_TextureAnimation): a
	   chain a drawn face uses gets every frame packed (frames 0 .. n - 1 and
	   the alternates, all of one size; a chain with a missing frame, which
	   Quake refuses, is left still) and a display slot of its own, cut from
	   its first frame, that every face of the chain draws with */
	ts->remap = xcalloc((size_t) b->num_miptex + 1, sizeof *ts->remap);
	for (int i = 0; i < b->num_miptex; i++)
		ts->remap[i] = i;
	ts->anims = xcalloc((size_t) b->num_miptex + 1, sizeof *ts->anims);
	uint8_t *in_chain = xcalloc((size_t) b->num_miptex + 1, 1);
	for (int i = 0; i < b->num_miptex && anims; i++) {
		const bsp_miptex *mi = &b->miptex[i];
		if (in_chain[i] || mi->name[0] != '+' || !mi->name[1] || !mi->w)
			continue;
		tex_anim a;
		memset(&a, 0, sizeof a);
		int frame_of[TEX_ANIM_FRAMES], alt_of[TEX_ANIM_FRAMES], any_used = 0, ok = 1;
		for (int k = 0; k < TEX_ANIM_FRAMES; k++)
			frame_of[k] = alt_of[k] = -1;
		for (int j = i; j < b->num_miptex; j++) {
			const bsp_miptex *mj = &b->miptex[j];
			if (mj->name[0] != '+' || strcmp(mj->name + 2, mi->name + 2))
				continue;
			int c = mj->name[1];
			if (c >= 'A' && c <= 'J') c += 'a' - 'A';
			if (c >= '0' && c <= '9')      frame_of[c - '0'] = j;
			else if (c >= 'a' && c <= 'j') alt_of[c - 'a'] = j;
			else continue;
			in_chain[j] = 1;
			any_used |= mt_used[j];
			if (mj->w != mi->w || mj->h != mi->h || !mj->mip[0])
				ok = 0;
		}
		for (int k = 0; k < TEX_ANIM_FRAMES; k++) {
			if (frame_of[k] >= 0) { if (a.nframes != k) ok = 0; else a.frames[a.nframes++] = frame_of[k]; }
			if (alt_of[k] >= 0)   { if (a.nalt != k) ok = 0; else a.alt[a.nalt++] = alt_of[k]; }
		}
		if (!any_used || !ok || a.nframes + a.nalt < 2)
			continue;
		a.src = a.nframes ? a.frames[0] : a.alt[0];
		ts->anims[ts->num_anims++] = a;
		printf("qcook: animated %s: %d frames, %d alternates, %dx%d\n", mi->name + 2, a.nframes, a.nalt,
		       mi->w, mi->h);
	}
	free(in_chain);

	int total = b->num_miptex + nextra + ts->num_anims;
	ts->num   = total;
	ts->slots = xcalloc((size_t) total + 1, sizeof *ts->slots);
	uint8_t *used = xcalloc((size_t) total + 1, 1);
	memcpy(used, mt_used, (size_t) b->num_miptex);
	free(mt_used);
	for (int i = b->num_miptex; i < total; i++)
		used[i] = 1;
	for (int c = 0; c < ts->num_anims; c++) {
		tex_anim *a = &ts->anims[c];
		a->display = b->num_miptex + nextra + c;
		for (int k = 0; k < a->nframes; k++) { used[a->frames[k]] = 1; ts->remap[a->frames[k]] = a->display; }
		for (int k = 0; k < a->nalt; k++)    { used[a->alt[k]] = 1;    ts->remap[a->alt[k]] = a->display; }
	}
	#define SLOT_MIPTEX(i) ((i) < b->num_miptex ? &b->miptex[i] \
	                       : (i) < b->num_miptex + nextra ? &extra[(i) - b->num_miptex].mt \
	                       : &b->miptex[ts->anims[(i) - b->num_miptex - nextra].src])
	#define SLOT_SKIN(i)   ((i) >= b->num_miptex && (i) < b->num_miptex + nextra && extra[(i) - b->num_miptex].skin)

	/* tile size per texture */
	pack_item *items = xcalloc((size_t) total + 1, sizeof *items);
	int nitems = 0;
	for (int i = 0; i < total; i++) {
		const bsp_miptex *m = SLOT_MIPTEX(i);
		int skin = SLOT_SKIN(i);
		tex_slot *s = &ts->slots[i];
		s->index = i;
		if (!strncmp(m->name, "sky", 3))
			s->flags |= QWF_SKY;
		if (m->name[0] == '*')
			s->flags |= QWF_LIQUID;
		if (!used[i] || !m->w || !m->mip[0] || (s->flags & QWF_SKY))
			continue;
		if (skin) {                             /* a model's skin: as it is */
			s->rep_w  = (m->w + 1) & ~1;        /* (halfword-aligned in 8bpp) */
			s->rep_h  = m->h;
			s->reps_x = s->reps_y = 1;
			items[nitems].idx = i;
			items[nitems].w   = s->rep_w;
			items[nitems].h   = s->rep_h;
			nitems++;
			continue;
		}

		int shift = fid->tex_shift + (i >= b->num_miptex && i < b->num_miptex + nextra
		                              ? extra[i - b->num_miptex].shift : 0);
		int w = m->w >> shift, h = m->h >> shift;
		while (w > fid->tex_max || h > fid->tex_max) {
			w >>= 1;
			h >>= 1;
		}
		/* A power-of-two axis wraps in hardware (texture window, whose
		   granularity is 8 texels: smaller ones are resampled up to 8). */
		s->win_x = is_pow2(m->w);
		s->win_y = is_pow2(m->h);
		if (s->win_x && w < 8) w = 8;
		if (s->win_y && h < 8) h = 8;
		if (w < 2) w = 2;
		if (h < 2) h = 2;
		w = (w + 1) & ~1;               /* halfword-aligned in 8bpp */
		s->rep_w = w;
		s->rep_h = h;

		/* an axis that cannot wrap: lay small repetitions side by side so
		   a polygon can span several (at most 128 texels: UVs in a byte) */
		s->reps_x = s->reps_y = 1;
		while (!s->win_x && w * s->reps_x < fid->tile_min && w * s->reps_x * 2 <= 128)
			s->reps_x *= 2;
		while (!s->win_y && h * s->reps_y < fid->tile_min && h * s->reps_y * 2 <= 128)
			s->reps_y *= 2;

		items[nitems].idx = i;
		items[nitems].w   = w * s->reps_x;
		items[nitems].h   = h * s->reps_y;
		nitems++;
	}
	qsort(items, (size_t) nitems, sizeof *items, by_area);

	/* First fit over the map's QTEX_MAP_PAGES pages of 256x256 texels, on a
	   grid of 8x8 cells. A wrapped axis must start at a multiple of its own size (the
	   texture window ORs its offset into the low bits). */
	uint8_t *pages = xcalloc(QTEX_PAGES, PAGE_TEX * PAGE_TEX);
	static uint8_t cell[QTEX_PAGES][CELLS][CELLS];   /* 1 = taken */
	memset(cell, 0, sizeof cell);
	int page_h[QTEX_PAGES] = { 0 };   /* used rows per page          */
	int pages_used = 0;
	for (int n = 0; n < nitems; n++) {
		pack_item *it = &items[n];
		tex_slot  *s  = &ts->slots[it->idx];
		int cw = (it->w + 7) / 8, ch = (it->h + 7) / 8;
		int ax = s->win_x ? cw : 1, ay = s->win_y ? ch : 1;   /* alignment, cells */
		int page = -1, cx = 0, cy = 0;
		for (int p = 0; p < QTEX_MAP_PAGES && page < 0; p++)
			for (int y = 0; y + ch <= CELLS && page < 0; y += ay)
				for (int x = 0; x + cw <= CELLS && page < 0; x += ax) {
					int ok = 1;
					for (int yy = 0; yy < ch && ok; yy++)
						for (int xx = 0; xx < cw && ok; xx++)
							ok = !cell[p][y + yy][x + xx];
					if (ok) {
						page = p;
						cx = x;
						cy = y;
					}
				}
		if (page < 0) {
			long need = 0, extras = 0;
			for (int k = 0; k < nitems; k++) {
				long ar = (long) ((items[k].w + 7) & ~7) * ((items[k].h + 7) & ~7);
				need += ar;
				if (items[k].idx >= b->num_miptex && items[k].idx < b->num_miptex + nextra)
					extras += ar;
			}
			if (may_fail) {
				printf("qcook: %ld K texels (%ld K the models') overflow %d pages at %s\n",
				       need / 1024, extras / 1024, QTEX_MAP_PAGES, fid->name);
				free(pages);
				free(items);
				free(used);
				return 0;
			}
			die("textures do not fit in %d VRAM pages at fidelity %s (%ld K texels, %ld K of them "
			    "the models', item %d of %d: %dx%d)", QTEX_MAP_PAGES, fid->name, need / 1024,
			    extras / 1024, n, nitems, it->w, it->h);
		}
		for (int yy = 0; yy < ch; yy++)
			for (int xx = 0; xx < cw; xx++)
				cell[page][cy + yy][cx + xx] = 1;

		const bsp_miptex *m = SLOT_MIPTEX(it->idx);
		int skin = SLOT_SKIN(it->idx);
		int px = QTEX_PAGE_X0 + (page % QTEX_PAGE_COLS) * (PAGE_TEX / 2);
		int py = (page / QTEX_PAGE_COLS) * PAGE_TEX;
		s->present = 1;
		s->u0      = cx * 8;
		s->v0      = cy * 8;
		s->page    = page;
		s->tile_w  = it->w;
		s->tile_h  = it->h;
		s->tpage   = gpu_tpage(px, py);
		s->clut    = gpu_clut(QTEX_CLUT_X, QTEX_CLUT_Y);
		/* GP0 E2h: texcoord = (texcoord & ~(mask * 8)) | ((offset & mask) * 8) */
		uint32_t mx = s->win_x ? ((uint32_t) (256 - s->rep_w) >> 3) & 31 : 0;
		uint32_t my = s->win_y ? ((uint32_t) (256 - s->rep_h) >> 3) & 31 : 0;
		uint32_t ox = s->win_x ? ((uint32_t) s->u0 >> 3) & 31 : 0;
		uint32_t oy = s->win_y ? ((uint32_t) s->v0 >> 3) & 31 : 0;
		s->window  = 0xE2000000u | mx | my << 5 | ox << 10 | oy << 15;

		uint8_t *img = pages + (size_t) page * PAGE_TEX * PAGE_TEX;
		for (int y = 0; y < it->h; y++)
			for (int xx = 0; xx < it->w; xx++)
				img[(s->v0 + y) * PAGE_TEX + s->u0 + xx] = skin
					? m->mip[0][y * m->w + (xx < m->w ? xx : m->w - 1)]   /* (the pad: the last column) */
					: sample(m, xx % s->rep_w, y % s->rep_h, s->rep_w, s->rep_h);

		ts->bytes += (uint32_t) (it->w * it->h);
		if (s->v0 + it->h > page_h[page])
			page_h[page] = s->v0 + it->h;
		if (page + 1 > pages_used)
			pages_used = page + 1;
	}
	ts->pages_used = pages_used;

	/* QTX3: header + one rect per used page (only the used rows) */
	FILE *f = fopen(out_path, "wb");
	if (!f)
		die("cannot write %s", out_path);

	qtex_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QTEX_MAGIC, 4);
	h.num_rects = (uint32_t) ts->pages_used;
	for (int i = 0; i < 256; i++) {
		int r = palette[i * 3], g = palette[i * 3 + 1], bl = palette[i * 3 + 2];
		uint16_t c = (uint16_t) ((r >> 3) | ((g >> 3) << 5) | ((bl >> 3) << 10));
		h.clut[i] = c ? c : 0x8000;     /* 0x0000 would be transparent */
	}
	fwrite(&h, sizeof h, 1, f);

	uint32_t ofs = (uint32_t) (sizeof h + (size_t) ts->pages_used * sizeof(qtex_rect));
	ofs = (ofs + 63u) & ~63u;                            /* (64: the GPU DMA's 16-word unit) */
	for (int p = 0; p < ts->pages_used; p++) {
		qtex_rect r;
		r.x = (uint16_t) (QTEX_PAGE_X0 + (p % QTEX_PAGE_COLS) * (PAGE_TEX / 2));
		r.y = (uint16_t) ((p / QTEX_PAGE_COLS) * PAGE_TEX);
		r.w = PAGE_TEX / 2;
		r.h = (uint16_t) page_h[p];
		r.data_ofs = ofs;
		fwrite(&r, sizeof r, 1, f);
		ofs += (uint32_t) r.w * r.h * 2;       /* 256 * h bytes: 32-multiple */
	}
	long here = ftell(f);
	static const uint8_t zero[64];
	fwrite(zero, 1, (size_t) (((here + 63) & ~63L) - here), f);
	for (int p = 0; p < ts->pages_used; p++)
		fwrite(pages + (size_t) p * PAGE_TEX * PAGE_TEX, 1,
		       (size_t) page_h[p] * PAGE_TEX, f);
	long file_len = ftell(f);
	fclose(f);

	printf("qcook: %s — %d tiles in %d pages (%d animated textures), %u texel bytes, file %ld B\n",
	       out_path, nitems, ts->pages_used, ts->num_anims, ts->bytes, file_len);
	#undef SLOT_MIPTEX
	#undef SLOT_SKIN

	if (out4_path)
		write_tex4(ts, total, pages, page_h, palette, out4_path);

	free(pages);
	free(items);
	free(used);
	return 1;
}
