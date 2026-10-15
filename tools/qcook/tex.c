/*
 * qcook — textures: palette -> CLUT, one tile per used miptex (one texture
 * repetition at the fidelity's resolution; on an axis that is not a power of
 * two, possibly several), packed into whole 8bpp texture pages, each
 * power-of-two axis aligned for the GPU texture window, written as a QTX3
 * file (formats/qtex.h).
 */

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

void tex_build(tex_set *ts, const bsp_t *b, const uint8_t palette[768],
               const fidelity_t *fid, const char *out_path)
{
	memset(ts, 0, sizeof *ts);
	ts->slots = xcalloc((size_t) b->num_miptex, sizeof *ts->slots);

	/* which miptex do world-model faces use? */
	uint8_t *used = xcalloc((size_t) b->num_miptex, 1);
	const bsp_model *wm = &b->models[0];
	for (int f = wm->firstface; f < wm->firstface + wm->numfaces; f++) {
		int ti = b->faces[f].texinfo;
		if (ti >= 0 && ti < b->num_texinfo) {
			int mt = b->texinfo[ti].miptex;
			if (mt >= 0 && mt < b->num_miptex)
				used[mt] = 1;
		}
	}

	/* tile size per texture */
	pack_item *items = xcalloc((size_t) b->num_miptex, sizeof *items);
	int nitems = 0;
	for (int i = 0; i < b->num_miptex; i++) {
		const bsp_miptex *m = &b->miptex[i];
		tex_slot *s = &ts->slots[i];
		s->index = i;
		if (!strncmp(m->name, "sky", 3))
			s->flags |= QWF_SKY;
		if (m->name[0] == '*')
			s->flags |= QWF_LIQUID;
		if (!used[i] || !m->w || !m->mip[0] || (s->flags & QWF_SKY))
			continue;

		int w = m->w >> fid->tex_shift, h = m->h >> fid->tex_shift;
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

	/* First fit over QTEX_PAGES pages of 256x256 texels, on a grid of 8x8
	   cells. A wrapped axis must start at a multiple of its own size (the
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
		for (int p = 0; p < QTEX_PAGES && page < 0; p++)
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
		if (page < 0)
			die("textures do not fit in %d VRAM pages at fidelity %s",
			    QTEX_PAGES, fid->name);
		for (int yy = 0; yy < ch; yy++)
			for (int xx = 0; xx < cw; xx++)
				cell[page][cy + yy][cx + xx] = 1;

		const bsp_miptex *m = &b->miptex[it->idx];
		int px = QTEX_PAGE_X0 + (page % QTEX_PAGE_COLS) * (PAGE_TEX / 2);
		int py = (page / QTEX_PAGE_COLS) * PAGE_TEX;
		s->present = 1;
		s->u0      = cx * 8;
		s->v0      = cy * 8;
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
				img[(s->v0 + y) * PAGE_TEX + s->u0 + xx] =
					sample(m, xx % s->rep_w, y % s->rep_h, s->rep_w, s->rep_h);

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
	ofs = (ofs + 31u) & ~31u;
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
	static const uint8_t zero[32];
	fwrite(zero, 1, (size_t) (((here + 31) & ~31L) - here), f);
	for (int p = 0; p < ts->pages_used; p++)
		fwrite(pages + (size_t) p * PAGE_TEX * PAGE_TEX, 1,
		       (size_t) page_h[p] * PAGE_TEX, f);
	long total = ftell(f);
	fclose(f);

	printf("qcook: %s — %d tiles in %d pages, %u texel bytes, file %ld B\n",
	       out_path, nitems, ts->pages_used, ts->bytes, total);

	free(pages);
	free(items);
	free(used);
}
