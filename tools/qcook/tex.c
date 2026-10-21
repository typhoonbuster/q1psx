/*
 * qcook — textures: palette -> CLUT, one tile per used miptex (one texture
 * repetition at the level's resolution), packed into whole 8bpp texture
 * pages, each power-of-two axis aligned for the GPU texture window, written
 * as a QTX6 file (formats/qtex.h) with the map's texture table: the slots'
 * words, how the world's UVs (cooked for the finest level's layout) land on
 * this set's tiles, the animated textures' tiles. Optionally the same pages
 * in 4bpp, each texture quantized to its own 16 colours (QTX7).
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


/* ------------------------------------------------------------- tables -- */

/* a slot's CLUT and tpage words in one of the two sets */
typedef struct { uint16_t clut, tpage; } tex_words;

/* The slot of `ts` that draws slot i of the layout `canon`: itself, or, for
   an animation's display slot at a level that left the animation out, the
   chain's first frame (the display tile is cut from it); -1 if none. */
static int draw_slot(const tex_set *ts, const tex_set *canon, int i)
{
	if (i < ts->num)
		return i;
	for (int c = 0; c < canon->num_anims; c++)
		if (canon->anims[c].display == i)
			return canon->anims[c].src;
	return -1;
}

static int face_miptex(const bsp_t *b, int f)
{
	const bsp_face *bf = &b->faces[f];
	if (bf->texinfo < 0 || bf->texinfo >= b->num_texinfo)
		return -1;
	int mt = b->texinfo[bf->texinfo].miptex;
	return mt >= 0 && mt < b->num_miptex ? mt : -1;
}

/* The set's tables (formats/qtex.h) into `out`: qw_tex[canon->num] (the
   CLUT and tpage of `words`, the texture window), qtex_uvmap[canon->num]
   (the layout's UVs onto this set's tiles), then qw_anim[ts->num_anims]
   (the animated textures' tiles, and the brush models drawing a chain
   with alternates: the engine looks at their frame). */
static void tables_build(buf_t *out, const tex_set *ts, const tex_set *canon, const bsp_t *b,
                         const tex_words *words)
{
	for (int i = 0; i < canon->num; i++) {
		qw_tex t;
		memset(&t, 0, sizeof t);
		int j = draw_slot(ts, canon, i);
		if (j >= 0 && ts->slots[j].present && canon->slots[i].present) {
			t.clut   = words[j].clut;
			t.tpage  = words[j].tpage;
			t.window = ts->slots[j].window;
		}
		buf_put(out, &t, sizeof t);
	}
	for (int i = 0; i < canon->num; i++) {
		qtex_uvmap m;
		memset(&m, 0, sizeof m);
		int j = draw_slot(ts, canon, i);
		const tex_slot *c = &canon->slots[i];
		if (j >= 0 && ts->slots[j].present && c->present) {
			const tex_slot *s = &ts->slots[j];
			long su = lround(32768.0 * s->res_u / c->res_u), sv = lround(32768.0 * s->res_v / c->res_v);
			if (su < 1 || sv < 1 || su > 32768 || sv > 32768)
				die("texture slot %d: scale %ld / %ld (finer than the layout's)", i, su, sv);
			m.su  = (uint16_t) su;
			m.sv  = (uint16_t) sv;
			m.u0  = (uint8_t) (s->win_x ? 0 : s->u0 + s->inner);
			m.v0  = (uint8_t) (s->win_y ? 0 : s->v0 + s->inner);
			m.rnd = s->inner ? 0 : 16384;          /* a skin's texels: divided (game.c) */
		}
		buf_put(out, &m, sizeof m);
	}
	for (int c = 0; c < ts->num_anims; c++) {
		const tex_anim *a = &ts->anims[c];
		qw_anim q;
		memset(&q, 0, sizeof q);
		#define TILE_OF(t, sl) do { const tex_slot *s_ = &ts->slots[sl]; (t).page = (uint8_t) s_->page; \
			(t).slot = (uint16_t) (sl); (t).u0 = (uint16_t) s_->u0; (t).v0 = (uint16_t) s_->v0; \
			(t).w = (uint16_t) s_->tile_w; (t).h = (uint16_t) s_->tile_h; } while (0)
		TILE_OF(q.display, a->display);
		q.num_frames = (uint8_t) a->nframes;
		q.num_alt    = (uint8_t) a->nalt;
		for (int k = 0; k < a->nframes; k++) TILE_OF(q.frames[k], a->frames[k]);
		for (int k = 0; k < a->nalt; k++)    TILE_OF(q.alt[k], a->alt[k]);
		#undef TILE_OF
		for (int m = 1; a->nalt && m < b->num_models && q.num_models < QW_ANIM_MODELS; m++) {
			const bsp_model *bm = &b->models[m];
			int uses = 0;
			for (int fc = bm->firstface; fc < bm->firstface + bm->numfaces && !uses; fc++) {
				int mt = face_miptex(b, fc);
				uses = mt >= 0 && ts->remap[mt] == a->display;
			}
			if (uses)
				q.models[q.num_models++] = (uint16_t) m;
		}
		buf_put(out, &q, sizeof q);
	}
}

/* ---------------------------------------------------------------- 4bpp -- */

/* The QTX7 file: the pages of `pages` (8bpp indices) re-coded per tile with
   that tile's own 16 colours. */
static void write_tex4(const tex_set *ts, const tex_set *canon, const bsp_t *b, const uint8_t *pages,
                       const int page_h[], const uint8_t palette[768], const char *path)
{
	int ncl = ts->num;
	int cl_rows = (ncl + QTEX4_CLUTS_PER_ROW - 1) / QTEX4_CLUTS_PER_ROW;
	if (cl_rows > QTEX4_CLUT_ROWS)
		die("%d textures: more 4-bit CLUTs than the CLUT area holds (%d)", ncl,
		    QTEX4_CLUT_ROWS * QTEX4_CLUTS_PER_ROW);
	int cl_w = 16 * (ncl < QTEX4_CLUTS_PER_ROW ? ncl : QTEX4_CLUTS_PER_ROW);
	uint16_t  *clut = xcalloc((size_t) (cl_w * cl_rows) + 16, 2);
	uint8_t   *nib  = xcalloc(QTEX_PAGES, PAGE_TEX * PAGE_TEX);
	tex_words *tab  = xcalloc((size_t) ts->num + 1, sizeof *tab);
	double err = 0, npx = 0;

	for (int i = 0; i < ts->num; i++) {
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

	buf_t tables;
	memset(&tables, 0, sizeof tables);
	tables_build(&tables, ts, canon, b, tab);

	FILE *f = fopen(path, "wb");
	if (!f)
		die("cannot write %s", path);
	int nrects = (ncl ? 1 : 0) + ts->pages_used;
	qtex4_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QTEX4_MAGIC, 4);
	h.num_rects = (uint32_t) nrects;
	h.num_slots = (uint32_t) canon->num;
	h.ofs_slots = (uint32_t) (sizeof h + (size_t) nrects * sizeof(qtex_rect));
	h.num_anims = (uint32_t) ts->num_anims;
	h.ofs_anims = h.ofs_slots + (uint32_t) canon->num * (uint32_t) (sizeof(qw_tex) + sizeof(qtex_uvmap));
	fwrite(&h, sizeof h, 1, f);

	uint32_t ofs = h.ofs_slots + (uint32_t) tables.len;
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
	fwrite(tables.p, 1, tables.len, f);
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
	free(tables.p);
	free(clut);
	free(nib);
	free(tab);
}

/* ------------------------------------------------------------- build -- */

int tex_build(tex_set *ts, const tex_set *canon, const bsp_t *b, const tex_extra *extra,
              int nextra, const uint8_t palette[768], int level, const char *out_path,
              const char *out4_path, int anims, int may_fail)
{
	const tex_level_t *lv = &tex_levels[level];
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
		if (out_path || out4_path)
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
	/* a level that leaves the animations out still draws the chains' first
	   frames: the stand-ins for their display slots (draw_slot) */
	for (int c = 0; canon && !anims && c < canon->num_anims; c++)
		used[canon->anims[c].src] = 1;
	#define SLOT_MIPTEX(i) ((i) < b->num_miptex ? &b->miptex[i] \
	                       : (i) < b->num_miptex + nextra ? &extra[(i) - b->num_miptex].mt \
	                       : &b->miptex[ts->anims[(i) - b->num_miptex - nextra].src])
	#define SLOT_EXTRA(i)  ((i) >= b->num_miptex && (i) < b->num_miptex + nextra ? &extra[(i) - b->num_miptex] : 0)

	/* tile size per texture: one repetition at the level's resolution */
	pack_item *items = xcalloc((size_t) total + 1, sizeof *items);
	int nitems = 0;
	for (int i = 0; i < total; i++) {
		const bsp_miptex *m = SLOT_MIPTEX(i);
		const tex_extra  *x = SLOT_EXTRA(i);
		tex_slot *s = &ts->slots[i];
		s->index = i;
		if (!strncmp(m->name, "sky", 3))
			s->flags |= QWF_SKY;
		if (m->name[0] == '*')
			s->flags |= QWF_LIQUID;
		if (!used[i] || !m->w || !m->mip[0] || (s->flags & QWF_SKY))
			continue;
		if (x && x->skin) {                     /* a model's skin: as it is */
			s->rep_w  = (m->w + 1) & ~1;        /* (halfword-aligned in 8bpp) */
			s->rep_h  = m->h;
			s->res_u  = s->res_v = 1.0 / x->div;
			s->inner  = x->border;
			items[nitems].idx = i;
			items[nitems].w   = s->rep_w;
			items[nitems].h   = s->rep_h;
			nitems++;
			continue;
		}

		int shift = lv->tex_shift + (x ? x->shift : 0);
		int w = m->w >> shift, h = m->h >> shift;
		while (w > lv->tex_max || h > lv->tex_max) {
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
		s->res_u = (double) w / m->w;
		s->res_v = (double) h / m->h;

		items[nitems].idx = i;
		items[nitems].w   = w;
		items[nitems].h   = h;
		nitems++;
	}
	if (!out_path && !out4_path) {          /* the layout alone */
		for (int n = 0; n < nitems; n++)
			ts->slots[items[n].idx].present = 1;
		free(items);
		free(used);
		return 1;
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
				       need / 1024, extras / 1024, QTEX_MAP_PAGES, level_name[level]);
				free(pages);
				free(items);
				free(used);
				return 0;
			}
			die("textures do not fit in %d VRAM pages at %s (%ld K texels, %ld K of them "
			    "the models', item %d of %d: %dx%d)", QTEX_MAP_PAGES, level_name[level], need / 1024,
			    extras / 1024, n, nitems, it->w, it->h);
		}
		for (int yy = 0; yy < ch; yy++)
			for (int xx = 0; xx < cw; xx++)
				cell[page][cy + yy][cx + xx] = 1;

		const bsp_miptex *m = SLOT_MIPTEX(it->idx);
		const tex_extra  *x = SLOT_EXTRA(it->idx);
		int skin = x && x->skin;
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
					: sample(m, xx, y, s->rep_w, s->rep_h);

		ts->bytes += (uint32_t) (it->w * it->h);
		if (s->v0 + it->h > page_h[page])
			page_h[page] = s->v0 + it->h;
		if (page + 1 > pages_used)
			pages_used = page + 1;
	}
	ts->pages_used = pages_used;

	/* QTX6, the 8-bit set (no disc carries it since docs/porting-notes.md
	   §52: written only when asked): header, one rect per used page (only
	   the used rows), the tables, the pixels */
	if (out_path) {
		tex_words *words = xcalloc((size_t) total + 1, sizeof *words);
		for (int i = 0; i < total; i++) {
			words[i].clut  = ts->slots[i].clut;
			words[i].tpage = ts->slots[i].tpage;
		}
		buf_t tables;
		memset(&tables, 0, sizeof tables);
		tables_build(&tables, ts, canon, b, words);
		free(words);

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
		h.num_slots = (uint32_t) canon->num;
		h.ofs_slots = (uint32_t) (sizeof h + (size_t) ts->pages_used * sizeof(qtex_rect));
		h.num_anims = (uint32_t) ts->num_anims;
		h.ofs_anims = h.ofs_slots + (uint32_t) canon->num * (uint32_t) (sizeof(qw_tex) + sizeof(qtex_uvmap));
		fwrite(&h, sizeof h, 1, f);

		uint32_t ofs = h.ofs_slots + (uint32_t) tables.len;
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
		fwrite(tables.p, 1, tables.len, f);
		free(tables.p);
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
	}
	#undef SLOT_MIPTEX
	#undef SLOT_EXTRA

	if (out4_path)
		write_tex4(ts, canon, b, pages, page_h, palette, out4_path);

	free(pages);
	free(items);
	free(used);
	return 1;
}
