/*
 * q1psx — the streamed assets' manager. See engine/stream.h and
 * docs/porting-notes.md §38.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxcd.h>
#include <psxgpu.h>

#include "ents.h"
#include "game.h"
#include "gdata.h"
#include "mdl.h"
#include "mem.h"
#include "music.h"
#include "qsound.h"
#include "qtex.h"
#include "stream.h"
#include "world.h"

stream_stats_t stream_stats;
int            stream_log;
int32_t        stream_origin[3];

#define SLOG(...) do { if (stream_log) printf(__VA_ARGS__); } while (0)

#define MAX_RES       48       /* assets resident (or being read) at once       */
#define MAX_REGIONS   40       /* free VRAM rectangles                          */
#define PASS_FRAMES   8        /* a residency pass every this many frames       */
#define TRIM_FRAMES   2        /* an image's pixels kept until its upload is done */
#define BACKOFF       8        /* passes before an asset no pool took is tried again */
#define MDL_CACHE     (24 * 1024)   /* the expanded frames' cache (engine/mdl.c)  */

enum { A_DISC, A_READING, A_RES };

typedef struct {
	qst_asset d;               /* the directory's entry                         */
	uint32_t  score;           /* this pass's: the least of the wants, ~0 none   */
	uint32_t  last;            /* the last complete pass's                       */
	uint8_t   state;           /* A_*                                           */
	uint8_t   res;             /* its slot in res[] (A_READING, A_RES)          */
	uint8_t   backoff;         /* passes before it is tried again               */
	uint8_t   pad;
} asset_t;

#define VR_T 1                 /* vrect_t flags: the image placed transposed     */

typedef struct {
	uint8_t  page;             /* the texture page (qtex.h numbering)           */
	uint8_t  flags;            /* VR_*                                          */
	uint8_t  u, v;             /* page-relative texels                          */
	uint16_t w, h;             /* texels taken (transposed: the image's h, w)    */
} vrect_t;

typedef struct {
	int16_t  asset;            /* -1: a free slot                              */
	uint8_t  trim;             /* frames until its upload is done: then the
	                              images' pixels go, its blocks may move again */
	uint8_t  nimg;             /* images in VRAM                                */
	uint8_t *ram;              /* its block in the RAM pool                     */
	uint32_t ram_size;         /* the block's bytes (a multiple of 16)          */
	uint8_t *tmp;              /* a transposed image's pixels, until the trim   */
	uint32_t tmp_size;
	vrect_t  img[2];
	qw_tex   tex[2];           /* the images' texture words (the renderer's)    */
	uint8_t  half;             /* the images are the fallback's (VRAM short)    */
} res_t;

static asset_t *assets;
static int      num_assets;
static int32_t  base_lba;      /* STREAM.DAT's first sector on the disc          */

static res_t    res[MAX_RES];

static uint8_t *pool, *pool_end, *pool_top;   /* the RAM pool (bump, compacted) */

static vrect_t  regions[MAX_REGIONS];
static int      num_regions;

static int      reading = -1;  /* the asset being read                           */
static int      waiting = -1;  /* the asset whose read waits for the music's fade */
static uint32_t frame_no;

#define ALIGN16(n) (((n) + 15u) & ~15u)
#define ALIGN64(n) (((n) + 63u) & ~63u)

/* ------------------------------------------------------------ directory -- */

/* a synchronous read of whole sectors (boot, the loading screen) */
static int read_sectors(int32_t lba, int n, void *dst)
{
	CdlLOC loc;
	CdIntToPos(lba, &loc);
	CdControl(CdlSetloc, &loc, 0);
	if (!CdReadRetry(n, (uint32_t *) dst, CdlModeSpeed, 3))
		return 0;
	return CdReadSync(0, 0) >= 0;
}

void stream_init(void)
{
	num_assets = 0;
	for (int i = 0; i < MAX_RES; i++)
		res[i].asset = -1;
	CdlFILE file;
	if (!CdSearchFile(&file, "\\STREAM.DAT;1")) {
		printf("stream: no STREAM.DAT on the disc\n");
		return;
	}
	base_lba = CdPosToInt(&file.pos);
	uint32_t mark = mem_level_mark();
	qst_header *h = mem_level_alloc(QST_SECTOR);
	if (!h || !read_sectors(base_lba, 1, h) || memcmp(h->magic, QSTREAM_MAGIC, 4) ||
	    h->dir_sectors < 1 || h->dir_sectors > 16) {
		printf("stream: STREAM.DAT unreadable or of another version\n");
		mem_level_release(mark);
		return;
	}
	uint32_t n = h->num_assets, dir = h->dir_sectors, ofs = h->ofs_assets;
	if (dir > 1) {
		mem_level_release(mark);
		h = mem_level_alloc(dir * QST_SECTOR);
		if (!h || !read_sectors(base_lba, (int) dir, h)) {
			mem_level_release(mark);
			return;
		}
	}
	if (ofs + n * sizeof(qst_asset) > dir * QST_SECTOR) {
		mem_level_release(mark);
		return;
	}
	assets = mem_game_alloc(n * sizeof *assets);
	if (!assets) {
		mem_level_release(mark);
		return;
	}
	const qst_asset *d = (const qst_asset *) ((const uint8_t *) h + ofs);
	for (uint32_t i = 0; i < n; i++) {
		memset(&assets[i], 0, sizeof assets[i]);
		assets[i].d = d[i];
		assets[i].score = assets[i].last = ~0u;
	}
	num_assets = (int) n;
	mem_level_release(mark);
	printf("stream: STREAM.DAT, %d assets from sector %d\n", num_assets, (int) base_lba);
}

int stream_find(const char *name)
{
	uint32_t hh = qs_hash(name);
	int lo = 0, hi = num_assets - 1;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		uint32_t m = assets[mid].d.hash;
		if (m == hh)
			return mid;
		if (m < hh) lo = mid + 1;
		else        hi = mid - 1;
	}
	return -1;
}

/* ------------------------------------------------------------- RAM pool -- */

typedef struct { uint8_t **p; uint32_t size; int pinned; } block_t;

/* every block slid down against the previous one, but a block whose
   upload the GPU has yet to take (its slot's trim): the blocks after it
   slide down to its end. Not while a read is in flight. */
static void ram_compact(void)
{
	block_t b[2 * MAX_RES];
	int n = 0;
	for (int i = 0; i < MAX_RES; i++) {
		res_t *r = &res[i];
		if (r->asset < 0)
			continue;
		if (r->ram)
			b[n++] = (block_t) { &r->ram, r->ram_size, r->trim != 0 };
		if (r->tmp)
			b[n++] = (block_t) { &r->tmp, r->tmp_size, r->trim != 0 };
	}
	for (int i = 1; i < n; i++)               /* (a few dozen: insertion sort) */
		for (int j = i; j > 0 && *b[j - 1].p > *b[j].p; j--) {
			block_t t = b[j];
			b[j] = b[j - 1];
			b[j - 1] = t;
		}
	uint8_t *at = pool;
	for (int i = 0; i < n; i++) {
		if (b[i].pinned) {
			at = *b[i].p + b[i].size;
			continue;
		}
		if (*b[i].p != at) {
			memmove(at, *b[i].p, b[i].size);
			*b[i].p = at;
		}
		at += b[i].size;
	}
	pool_top = at;
}

static uint32_t ram_used(void)
{
	uint32_t u = 0;
	for (int i = 0; i < MAX_RES; i++)
		if (res[i].asset >= 0)
			u += res[i].ram_size + res[i].tmp_size;
	return u;
}

/* n bytes at the pool's top, the pool compacted if they do not fit there
   (pinned blocks may leave it short); NULL if they do not fit */
static uint8_t *ram_alloc(uint32_t n)
{
	n = ALIGN16(n);
	if ((uint32_t) (pool_end - pool_top) < n) {
		if ((uint32_t) (pool_end - pool) - ram_used() < n)
			return 0;
		ram_compact();
		if ((uint32_t) (pool_end - pool_top) < n)
			return 0;
	}
	uint8_t *p = pool_top;
	pool_top += n;
	return p;
}

/* ------------------------------------------------------------ VRAM pool -- */

static void region_add(int page, int u, int v, int w, int h)
{
	if (u & 1) { u++; w--; }                  /* a halfword holds two texels */
	w &= ~1;
	if (w < 8 || h < 8 || num_regions >= MAX_REGIONS)
		return;
	vrect_t *r = &regions[num_regions++];
	r->page = (uint8_t) page;
	r->flags = 0;
	r->u = (uint8_t) u;  r->v = (uint8_t) v;
	r->w = (uint16_t) w; r->h = (uint16_t) h;
	stream_stats.vram_texels += (uint32_t) (w * h);
	SLOG("stream: region page %d u %d v %d %dx%d\n", page, u, v, w, h);
}

/* the map's pages' free rows (its 8-bit set: each page's rows below what it
   took; its 4-bit set: each 4-bit page's, the 8-bit pages it leaves whole),
   the game's pages' free rectangles */
static void regions_build(void)
{
	num_regions = 0;
	stream_stats.vram_texels = 0;
	if (world.tex4) {
		for (int p = 0; p < QTEX_MAP_PAGES; p++)
			if (world.page4_rows[p] < 256)        /* 4-bit page p: half of 8-bit page p / 2 */
				region_add(p / 2, (p & 1) * 128, world.page4_rows[p], 128, 256 - world.page4_rows[p]);
		for (int p = QTEX_MAP_PAGES / 2; p < QTEX_MAP_PAGES; p++)
			region_add(p, 0, 0, 256, 256);
	} else {
		for (int p = 0; p < QTEX_MAP_PAGES; p++)
			if (world.page_rows[p] < 256)
				region_add(p, 0, world.page_rows[p], 256, 256 - world.page_rows[p]);
	}
	for (uint32_t i = 0; i < gdata.num_free; i++) {
		const qg_free *f = &gdata.free[i];
		region_add(f->page, f->u, f->v, f->w, f->h);
	}
}

/* a w x h place at (u, v) of region rg overlapping no image */
static int vram_fits(int u, int v, int w, int h, const vrect_t *rg)
{
	if (u < rg->u || v < rg->v || u + w > rg->u + rg->w || v + h > rg->v + rg->h)
		return 0;
	for (int r = 0; r < MAX_RES; r++)
		if (res[r].asset >= 0)
			for (int k = 0; k < res[r].nimg; k++) {
				const vrect_t *a = &res[r].img[k];
				if (a->page == rg->page && u < a->u + a->w && a->u < u + w && v < a->v + a->h &&
				    a->v < v + h)
					return 0;
			}
	return 1;
}

/* The place for a w x h image (or, transposed, h' x w with h' the even
   width h makes), overlapping no other: in each region the corner and the
   corners the images in its page leave (right of them, under them), the
   best by the region's fit (its shorter side's leftover: a small image
   goes to small room, the big room kept), then the lowest, the leftmost.
   0 if none. */
static int vram_place(int w, int h, vrect_t *out)
{
	uint32_t bkey = ~0u;
	for (int t = 0; t < 2; t++) {
		int pw = t ? (h + 1) & ~1 : w, ph = t ? w : h;
		for (int g = 0; g < num_regions; g++) {
			const vrect_t *rg = &regions[g];
			if (pw > rg->w || ph > rg->h)
				continue;
			int ds = rg->w - pw, dl = rg->h - ph;
			uint32_t fit = (uint32_t) (ds < dl ? ds : dl);
			int cu[1 + 2 * 2 * MAX_RES], cv[1 + 2 * 2 * MAX_RES], nc = 0;
			cu[nc] = rg->u; cv[nc++] = rg->v;
			for (int r = 0; r < MAX_RES; r++)
				if (res[r].asset >= 0)
					for (int k = 0; k < res[r].nimg; k++) {
						const vrect_t *a = &res[r].img[k];
						if (a->page != rg->page)
							continue;
						cu[nc] = a->u + a->w; cv[nc++] = a->v > rg->v ? a->v : rg->v;
						cu[nc] = a->u > rg->u ? a->u : rg->u; cv[nc++] = a->v + a->h;
					}
			for (int c = 0; c < nc; c++) {
				int u = (cu[c] + 1) & ~1, v = cv[c];
				uint32_t key = fit << 18 | (uint32_t) v << 9 | (uint32_t) u;
				if (key >= bkey || !vram_fits(u, v, pw, ph, rg))
					continue;
				bkey = key;
				out->page = rg->page;
				out->flags = (uint8_t) (t ? VR_T : 0);
				out->u = (uint8_t) u; out->v = (uint8_t) v;
				out->w = (uint16_t) pw; out->h = (uint16_t) ph;
			}
		}
	}
	return bkey != ~0u;
}

static uint16_t page_tpage(int p)   /* 8bpp, no semi-transparency (game.c's) */
{
	int vx = QTEX_PAGE_X0 + (p % QTEX_PAGE_COLS) * 128, vy = (p / QTEX_PAGE_COLS) * 256;
	return (uint16_t) (((vx & 0x3ff) >> 6) | ((vy >> 8) << 4) | (1 << 7));
}

/* --------------------------------------------------------------- assets -- */

static void slot_free(res_t *r)
{
	r->asset = -1;
	r->ram   = r->tmp = 0;
	r->ram_size = r->tmp_size = 0;
	r->nimg  = 0;
	r->trim  = 0;
}

static void evict(int a)
{
	asset_t *as = &assets[a];
	if (as->state != A_RES)
		return;
	res_t *r = &res[as->res];
	SLOG("stream: out %08x (%u B, last %u)\n", (unsigned) as->d.hash, (unsigned) r->ram_size,
	     (unsigned) as->last);
	if (r->trim)                              /* (its upload not taken yet) */
		DrawSync(0);
	mdl_forget(a);
	slot_free(r);
	as->state = A_DISC;
	stream_stats.evictions++;
}

/* the resident asset wanted least, if it is wanted less than `score` (and
   is not pinned): -1 if none; `vram`: one with images only */
static int least_wanted(uint32_t score, int vram)
{
	int worst = -1;
	uint32_t wl = 0;
	for (int i = 0; i < MAX_RES; i++) {
		int a = res[i].asset;
		if (a < 0 || assets[a].state != A_RES || (vram && !res[i].nimg))
			continue;
		uint32_t l = assets[a].last;
		if (l == STREAM_PIN || l <= score)
			continue;
		if (worst < 0 || l > wl) {
			worst = a;
			wl = l;
		}
	}
	return worst;
}

static int free_slot(void)
{
	for (int i = 0; i < MAX_RES; i++)
		if (res[i].asset < 0)
			return i;
	return -1;
}

/* the images of one resolution placed in VRAM (room made by dropping
   what is wanted less than `score`): 1, or 0 with none taken */
static int place_images(res_t *r, const qst_image *img, int nimg, uint32_t score, vrect_t at[2])
{
	r->nimg = 0;
	for (int k = 0; k < nimg; k++) {
		while (!vram_place(img[k].w, img[k].h, &at[k])) {
			int v = least_wanted(score, 1);
			if (v < 0) {
				r->nimg = 0;
				return 0;
			}
			evict(v);
		}
		r->img[k] = at[k];                    /* (taken: the next image's place avoids it) */
		r->nimg = (uint8_t) (k + 1);
	}
	return 1;
}

/* a model asset just read: its images to VRAM (at the skin's resolution,
   or its fallback at half when no room is left for that; an image may go
   transposed), their UVs into its records, its texture words */
static int install_model(int a)
{
	asset_t *as = &assets[a];
	res_t   *r  = &res[as->res];
	uint32_t size = as->d.size;
	const qst_model *m = (const qst_model *) r->ram;
	if (m->num_images < 1 || m->num_images > 2 || m->ofs_records + m->len_records > size ||
	    m->ofs_frames + (uint32_t) m->num_frames * m->num_verts * 4 > size) {
		printf("stream: asset %08x is not a model this engine reads\n", (unsigned) as->d.hash);
		return 0;
	}
	int nimg = m->num_images;
	qst_image full[2], img[2];
	memcpy(full, m->images, sizeof full);
	memcpy(img, m->images, sizeof img);
	for (int k = 0; k < nimg; k++)
		if (m->images[k].ofs + (uint32_t) m->images[k].w * m->images[k].h > size ||
		    m->small[k].ofs + (uint32_t) m->small[k].w * m->small[k].h > size) {
			printf("stream: asset %08x: image out of bounds\n", (unsigned) as->d.hash);
			return 0;
		}
	vrect_t at[2];
	int half = 0;
	if (!place_images(r, img, nimg, as->last, at)) {
		memcpy(img, m->small, sizeof img);
		if (!place_images(r, img, nimg, as->last, at)) {
			printf("stream: no VRAM for %08x's %dx%d images\n", (unsigned) as->d.hash, img[0].w, img[0].h);
			return 0;
		}
		half = 1;
		stream_stats.halved++;
	}
	r->half = (uint8_t) half;
	uint32_t tbytes = 0;
	for (int k = 0; k < nimg; k++)
		if (at[k].flags & VR_T)
			tbytes += ALIGN64((uint32_t) at[k].w * at[k].h);
	/* the transposed images' pixels (the read's block may move here) */
	if (tbytes) {
		r->tmp = ram_alloc(tbytes);
		if (!r->tmp) {
			printf("stream: no RAM to turn %08x's images\n", (unsigned) as->d.hash);
			return 0;
		}
		r->tmp_size = ALIGN16(tbytes);
	}
	m = (const qst_model *) r->ram;
	uint8_t *tp = r->tmp;
	for (int k = 0; k < nimg; k++) {
		const uint8_t *src = r->ram + img[k].ofs;
		if (at[k].flags & VR_T) {                 /* row y of the image: column y here */
			int tw = at[k].w;
			for (int y = 0; y < tw; y++) {
				int sy = y < img[k].h ? y : img[k].h - 1;   /* (the even width's edge column) */
				for (int x = 0; x < img[k].w; x++)
					tp[x * tw + y] = src[sy * img[k].w + x];
			}
			uint32_t n = (uint32_t) at[k].w * at[k].h;
			memset(tp + n, 0, ALIGN64(n) - n);
			src = tp;
			tp += ALIGN64(n);
		}
		RECT rc = { (int16_t) (QTEX_PAGE_X0 + (at[k].page % QTEX_PAGE_COLS) * 128 + at[k].u / 2),
		            (int16_t) ((at[k].page / QTEX_PAGE_COLS) * 256 + at[k].v),
		            (int16_t) (at[k].w / 2), (int16_t) at[k].h };
		LoadImage(&rc, (const uint32_t *) src);   /* (64-byte padded: the cook's, or above) */
		r->tex[k].clut   = gdata.clut;
		r->tex[k].tpage  = page_tpage(at[k].page);
		r->tex[k].window = 0xE2000000u;
		SLOG("stream: %08x image %d %dx%d at page %d u %d v %d%s%s\n", (unsigned) as->d.hash, k, img[k].w,
		     img[k].h, at[k].page, at[k].u, at[k].v, at[k].flags & VR_T ? " turned" : "",
		     half ? " (half)" : "");
	}
	/* the records' UVs: from their image's corner to its place (the half
	   resolution's: the texel holding the full one's; turned: u and v
	   swapped) */
	uint8_t *p = r->ram + m->ofs_records, *end = p + m->len_records;
	while (p < end) {
		qw_record *rec = (qw_record *) p;
		if (!rec->size)
			break;
		int k = rec->tex < nimg ? rec->tex : 0;
		uint16_t add = (uint16_t) (at[k].u | at[k].v << 8);
		int x0f = full[k].x0, x0h = img[k].x0;
		qw_tri *t = (qw_tri *) (p + rec->polys + rec->num_quads * sizeof(qw_quad));
		for (int i = 0; i < rec->num_tris; i++)
			for (int c = 0; c < 3; c++) {
				uint16_t uv = t[i].uv[c];
				if (half) {
					int u = (((uv & 0xff) - QST_BORDER + x0f) >> 1) - x0h + QST_BORDER;
					int v = (((uv >> 8) - QST_BORDER) >> 1) + QST_BORDER;
					uv = (uint16_t) (u | v << 8);
				}
				if (at[k].flags & VR_T)
					uv = (uint16_t) (uv >> 8 | uv << 8);
				t[i].uv[c] = (uint16_t) (uv + add);
			}
		p += rec->size;
	}
	r->ram_size = ALIGN16(size);              /* the read's sector padding back */
	r->trim     = TRIM_FRAMES;                /* then the pixels, once in VRAM */
	return 1;
}

/* the read of asset a is done: install it, or put it back on the disc */
static void read_done(int a, int ok)
{
	asset_t *as = &assets[a];
	reading = -1;
	if (ok && as->d.kind == QSTK_MODEL && install_model(a)) {
		as->state = A_RES;
		stream_stats.reads++;
		stream_stats.read_bytes += as->d.size;
		SLOG("stream: in %08x (%u B, last %u)\n", (unsigned) as->d.hash, (unsigned) as->d.size,
		     (unsigned) as->last);
		return;
	}
	slot_free(&res[as->res]);
	as->state   = A_DISC;
	as->backoff = BACKOFF;
	stream_stats.failures++;
}

/* RAM for asset a's read (its sectors whole), room made by dropping what
   is wanted less: its slot, or -1 */
static int reserve(int a)
{
	asset_t *as = &assets[a];
	int slot = free_slot();
	if (slot < 0)
		return -1;
	uint32_t bytes = (as->d.size + QST_SECTOR - 1) / QST_SECTOR * QST_SECTOR;
	uint8_t *dst;
	while (!(dst = ram_alloc(bytes))) {
		int v = least_wanted(as->last, 0);
		if (v < 0)
			return -1;
		evict(v);
	}
	res_t *r = &res[slot];
	slot_free(r);
	r->asset    = (int16_t) a;
	r->ram      = dst;
	r->ram_size = ALIGN16(bytes);
	as->res     = (uint8_t) slot;
	as->state   = A_READING;
	return slot;
}

static void start_read(int a)
{
	asset_t *as = &assets[a];
	res_t *r = &res[as->res];
	CdlLOC loc;
	CdIntToPos(base_lba + (int32_t) as->d.sector, &loc);
	CdControl(CdlSetloc, &loc, 0);
	int n = (int) ((as->d.size + QST_SECTOR - 1) / QST_SECTOR);
	if (!CdReadRetry(n, (uint32_t *) r->ram, CdlModeSpeed, 3)) {
		read_done(a, 0);
		return;
	}
	reading = a;
	SLOG("stream: reading %08x, %d sectors (score %u)\n", (unsigned) as->d.hash, n, (unsigned) as->last);
}

/* the wanted asset to read next: the most wanted on the disc within its
   reach (the biggest of equals first: its room found before the small
   ones break it up), -1 if none */
static int next_wanted(void)
{
	int best = -1;
	for (int a = 0; a < num_assets; a++) {
		asset_t *as = &assets[a];
		if (as->state != A_DISC || as->backoff || as->last > STREAM_REACH)
			continue;
		if (best < 0 || as->last < assets[best].last ||
		    (as->last == assets[best].last && as->d.size > assets[best].d.size))
			best = a;
	}
	return best;
}

/* -------------------------------------------------------------- passes -- */

void stream_want(int a, uint32_t score)
{
	if (a < 0 || a >= num_assets)
		return;
	if (score < assets[a].score)
		assets[a].score = score;
}

static void pass(void)
{
	for (int a = 0; a < num_assets; a++)
		assets[a].score = ~0u;
	ents_stream_wants();
	int resident = 0, wanted = 0;
	for (int a = 0; a < num_assets; a++) {
		asset_t *as = &assets[a];
		as->last = as->score;
		if (as->backoff)
			as->backoff--;
		resident += as->state == A_RES;
		wanted   += as->last <= STREAM_REACH;
	}
	stream_stats.resident = (uint16_t) resident;
	stream_stats.wanted   = (uint16_t) wanted;
	stream_stats.ram_used = ram_used();
	stream_stats.vram_used = 0;
	for (int i = 0; i < MAX_RES; i++)
		if (res[i].asset >= 0)
			for (int k = 0; k < res[i].nimg; k++)
				stream_stats.vram_used += (uint32_t) (res[i].img[k].w * res[i].img[k].h);
}

void stream_abort(void)
{
	if (reading >= 0) {
		CdReadBreak();
		CdReadSync(0, 0);                     /* (the drive idle before the next command) */
		reading = -1;
	}
	waiting = -1;
	DrawSync(0);                              /* (no upload left reading the pool) */
	pool = pool_top = pool_end = 0;           /* (the level arena is about to be reset) */
	for (int a = 0; a < num_assets; a++)
		assets[a].state = A_DISC;
	for (int i = 0; i < MAX_RES; i++)
		slot_free(&res[i]);
}

void stream_map_start(uint32_t keep)
{
	stream_abort();
	for (int a = 0; a < num_assets; a++) {
		assets[a].score   = assets[a].last = ~0u;
		assets[a].backoff = 0;
	}
	memset(&stream_stats, 0, sizeof stream_stats);
	mdl_reset(mem_level_alloc(MDL_CACHE), MDL_CACHE);
	uint32_t left = MEM_LEVEL_BYTES - mem_level_used();
	left = left > keep + 64 ? (left - keep - 64) & ~15u : 0;
	pool = left ? mem_level_alloc(left) : 0;
	pool_top = pool;
	pool_end = pool ? pool + left : 0;
	stream_stats.ram_size = left;
	regions_build();
	printf("stream: pools %u KB of RAM, %u K texels of VRAM in %d regions\n", (unsigned) left / 1024,
	       (unsigned) stream_stats.vram_texels / 1024, num_regions);
}

/* the uploads the GPU took: the images' pixels go, the blocks may move */
static void trims(void)
{
	for (int i = 0; i < MAX_RES; i++) {
		res_t *r = &res[i];
		if (r->asset < 0 || !r->trim || --r->trim)
			continue;
		const qst_model *m = (const qst_model *) r->ram;
		r->ram_size = ALIGN16(m->images[0].ofs);
		r->tmp = 0;
		r->tmp_size = 0;
	}
}

void stream_frame(void)
{
	if (!num_assets || !pool)
		return;
	frame_no++;
	memcpy(stream_origin, pl.pm.pos, sizeof stream_origin);
	trims();
	if (reading >= 0) {
		int left = CdReadSync(1, 0);          /* (also runs CdReadRetry's retries) */
		if (left > 0)
			return;
		read_done(reading, left == 0);
	}
	if (frame_no % PASS_FRAMES == 0)
		pass();
	if (waiting < 0) {
		int a = next_wanted();
		if (a < 0) {
			if (music_suspended())
				music_resume();
			return;
		}
		if (reserve(a) < 0) {
			assets[a].backoff = BACKOFF;
			return;
		}
		waiting = a;
		if (!music_suspended() && music_track())
			stream_stats.music_pauses++;
	}
	if (!music_suspend())                     /* (fading: the read waits) */
		return;
	int a = waiting;
	waiting = -1;
	start_read(a);
}

void stream_preload(void)
{
	if (!num_assets || !pool)
		return;
	if (world.has_spawn)                      /* (the player is put there after the load) */
		for (int k = 0; k < 3; k++)
			stream_origin[k] = world.spawn[k] * CLIP_SUB;
	else
		memcpy(stream_origin, pl.pm.pos, sizeof stream_origin);
	pass();
	for (;;) {
		int a = next_wanted();
		if (a < 0)
			break;
		if (reserve(a) < 0) {
			assets[a].backoff = BACKOFF;
			continue;
		}
		int n = (int) ((assets[a].d.size + QST_SECTOR - 1) / QST_SECTOR);
		int ok = read_sectors(base_lba + (int32_t) assets[a].d.sector, n, res[assets[a].res].ram);
		read_done(a, ok);
		DrawSync(0);                          /* the images uploaded: their pixels go */
		for (int i = 0; i < MAX_RES; i++)
			if (res[i].asset >= 0 && res[i].trim)
				res[i].trim = 1;
		trims();
	}
	pass();
	printf("stream: preloaded %d assets, %u KB of RAM, %u K texels of VRAM\n", stream_stats.resident,
	       (unsigned) stream_stats.ram_used / 1024, (unsigned) stream_stats.vram_used / 1024);
}

#ifdef Q1PSX_BENCH
/* the bench's frame dumps: where the resident images are (tools/pktview
   puts them in its VRAM from STREAM.DAT): "S hash image page u v flags",
   flags 1 turned, 2 the fallback's */
void stream_dump(void)
{
	for (int i = 0; i < MAX_RES; i++) {
		const res_t *r = &res[i];
		if (r->asset < 0 || assets[r->asset].state != A_RES)
			continue;
		for (int k = 0; k < r->nimg; k++)
			printf("S %08x %d %d %d %d %d\n", (unsigned) assets[r->asset].d.hash, k, r->img[k].page,
			       r->img[k].u, r->img[k].v, (r->img[k].flags & VR_T ? 1 : 0) | (r->half ? 2 : 0));
	}
}
#endif

const qst_model *stream_model(int a)
{
	if (a < 0 || a >= num_assets || assets[a].state != A_RES)
		return 0;
	return (const qst_model *) res[assets[a].res].ram;
}

const qw_tex *stream_model_tex(int a)
{
	if (a < 0 || a >= num_assets || assets[a].state != A_RES)
		return 0;
	return res[assets[a].res].tex;
}
