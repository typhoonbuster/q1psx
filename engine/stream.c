/*
 * q1psx — the streamed assets' manager. See engine/stream.h and
 * docs/porting-notes.md §40.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxcd.h>
#include <psxgpu.h>
#include <psxspu.h>

#include "cd.h"
#include "ents.h"
#include "game.h"
#include "gdata.h"
#include "mdl.h"
#include "mem.h"
#include "music.h"
#include "prof.h"
#include "q_fmt.h"
#include "qsound.h"
#include "qspu.h"
#include "qtex.h"
#include "snd.h"
#include "stream.h"
#include "world.h"
#include "zone.h"

stream_stats_t stream_stats;
int            stream_log;
int32_t        stream_origin[3];

#define SLOG(...) do { if (stream_log) printf(__VA_ARGS__); } while (0)

#define MAX_RES       48       /* assets resident (or being read) at once (96 measured
                                  the same, docs/porting-notes.md §86.3)        */
#define MAX_REGIONS   40       /* free VRAM rectangles                          */
#define PASS_FRAMES   8        /* a residency pass every this many frames       */
#define PASS_JUMP     (256 * QW_CLIP_SUB)   /* and at once when the player moved
                                       farther since the last one (a teleporter:
                                       no read started on the old place's wants) */
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
	uint8_t   backoff;         /* passes before it is tried again (a read error) */
	uint8_t   stuck;           /* ST_*: no room for it, or not readable          */
	uint8_t   absent;          /* not in the level's file (stream_level)         */
	uint8_t   banked;          /* in the level's bank: never the stream's (§86)  */
	uint32_t  ram_ofs;         /* its bytes in the RAM block (QSTF_RAM), ~0 none  */
	uint32_t  stuck_ram;       /* ST_ROOM: what the assets wanted as much held   */
	uint32_t  stuck_vram;      /*   then (RAM bytes, VRAM texels, SPU bytes)     */
	uint32_t  stuck_spu;
	int16_t   group_next;      /* the next asset of its group (the cook's), -1  */
	int16_t   group_first;     /* its group's first                             */
} asset_t;

/* asset_t stuck: no pool had room for it, even with all that is wanted
   less dropped (tried again once what is wanted as much holds less: not
   read again and again for nothing); not a model this engine reads */
enum { ST_NONE, ST_ROOM, ST_BROKEN };

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
	uint8_t  sound;            /* a sound: its samples in the SPU pool          */
	stream_sound_t snd;        /* (its place there, its header)                 */
	uint32_t spu_size;         /* the SPU bytes it holds (a multiple of 64)     */
} res_t;

static asset_t *assets;
static int      num_assets;
static int32_t  base_lba;      /* the assets' file's first sector: the level's
                                  (stream_level), else the pool's                */
static int32_t  pool_lba;      /* STREAM_<n>.DAT's                               */
static uint32_t *pool_sector;  /* [num_assets]: each asset's sector in the pool  */

/* The level's RAM-resident sounds (docs/porting-notes.md §86): the level's
   file flags them QSTF_RAM and lays them first, together; the map's start
   reads them in one go into a block of the level arena, and a wanted one
   goes to the SPU's swap room from there by a DMA, no read. */
static uint8_t *ram_block;
static uint32_t ram_first, ram_sectors;   /* their sectors in the level's file */
static int      ram_sounds;

static res_t    res[MAX_RES];

static uint8_t *pool, *pool_end, *pool_top;   /* the RAM pool (bump, compacted) */

/* The pool's end lent outside the assets: a block read from the disc (the
   2D gun's packets, Q1PSX_WEAPON_2D builds: engine/game.c game_ws_sync).
   Asked for once, it is given at each map's start before the stream's own
   buffers (the 2D gun as sure of its room as when it lived in the game
   arena) and read with the preload. Its bytes stay past pool_end:
   compaction never moves them (its packets' links are absolute). */
enum { L_NONE, L_WANT, L_SHORT, L_ROOM, L_IN };
static int32_t  lend_lba;
static uint32_t lend_bytes;      /* asked for: whole sectors                     */
static uint8_t *lend_at;         /* given: at pool_end, lend_bytes long          */
static uint8_t *pool_end_load;   /* the pool's end while the load overlay sits in
                                    its top (§90.7; 0: none): stream_load_done's */
extern uint8_t  __load_start[];  /* the overlay (engine/q1psx.ld)                 */
static uint8_t  lend_state;      /* L_*; L_SHORT: the pool could not spare it
                                    (asked again at the next map's start)       */
static uint32_t lend_reads;      /* the block's reads: a new one to link again   */

static vrect_t  regions[MAX_REGIONS];
static int      num_regions;

static int      reading = -1;  /* the asset being read                           */
static uint32_t spu_lo, spu_hi;   /* the SPU pool: SPU RAM past the map's bank     */

/* a sound's read: STAGE_SECTORS at a time from the disc into the staging
   buffer, each piece's samples into its place in SPU RAM before the next
   is read */
#define STAGE_SECTORS (MUSIC_REFILL_CHUNKS * 2 * QSPU_CHUNK / QST_SECTOR > 8 ? \
                       MUSIC_REFILL_CHUNKS * 2 * QSPU_CHUNK / QST_SECTOR : 8)   /* (a music
                                     refill's sectors, 8 at least: 8 with refills of 2, the
                                     game's since §90.3; 16 with refills of 4) */
static uint8_t *stage;            /* STAGE_SECTORS x QST_SECTOR, the level arena's */
static struct {
	int      a;                   /* the sound, -1 none                          */
	uint32_t spu;                 /* where its next piece's samples go           */
	uint32_t left;                /* its samples' bytes still to upload          */
	uint16_t sector, sectors;     /* the next piece's (from its first), its count */
	uint8_t  step;                /* SR_*                                        */
} sread = { -1, 0, 0, 0, 0, 0 };
enum { SR_WAIT, SR_READ, SR_UPLOAD };   /* (SR_WAIT: for the music's room) */
static int      waiting = -1;  /* the asset whose read waits for the music's room */
static int      swap_a = -1;   /* the RAM sound whose DMA is in flight (§86), -1 none:
                                  the swaps go on beside the drive's reads (§99) */
static uint32_t frame_no;


#define ALIGN16(n) (((n) + 15u) & ~15u)
#define ALIGN64(n) (((n) + 63u) & ~63u)

/* ------------------------------------------------------------ directory -- */

/* a synchronous read of whole sectors (boot, the loading screen) */
static int read_sectors(int32_t lba, int n, void *dst, int who, int tag)
{
	return cd_read_now(lba, (uint32_t) n, dst, who, tag);
}

static int stream_level_n = -1;   /* the model level whose STREAM_<n>.DAT is read */

/* STREAM_<level>.DAT's directory into assets[] (allocated at the first:
   every level's file holds the same assets in the same order, its models
   reduced at that level, the same sounds), through the level arena's top */
static int load_directory(int level)
{
	char name[24];
	q_snprintf(name, sizeof name, "\\STREAM_%d.DAT;1", level);
	uint32_t lba;
	if (!cd_find(name, &lba, NULL)) {
		printf("stream: no %s on the disc\n", name);
		return 0;
	}
	uint32_t mark = mem_level_mark();
	qst_header *h = mem_level_alloc(QST_SECTOR);
	if (!h || !read_sectors((int32_t) lba, 1, h, CDW_DIR, 0) || memcmp(h->magic, QSTREAM_MAGIC, 4) ||
	    h->dir_sectors < 1 || h->dir_sectors > 16) {
		printf("stream: %s unreadable or of another version\n", name);
		mem_level_release(mark);
		return 0;
	}
	uint32_t n = h->num_assets, dir = h->dir_sectors, ofs = h->ofs_assets;
	if (dir > 1) {
		mem_level_release(mark);
		h = mem_level_alloc(dir * QST_SECTOR);
		if (!h || !read_sectors((int32_t) lba, (int) dir, h, CDW_DIR, 0)) {
			mem_level_release(mark);
			return 0;
		}
	}
	if (ofs + n * sizeof(qst_asset) > dir * QST_SECTOR || (assets && n != (uint32_t) num_assets)) {
		printf("stream: %s's directory does not match\n", name);
		mem_level_release(mark);
		return 0;
	}
	if (!assets)
		assets = mem_game_alloc(n * sizeof *assets);
	const qst_asset *d = (const qst_asset *) ((const uint8_t *) h + ofs);
	for (uint32_t i = 0; i < n; i++) {
		memset(&assets[i], 0, sizeof assets[i]);
		assets[i].d = d[i];
		assets[i].score = assets[i].last = ~0u;
	}
	num_assets = (int) n;
	for (int i = 0; i < num_assets; i++) {      /* the groups' lists, in the file's order */
		int next = -1, first = i;
		for (int j = 0; j < num_assets; j++) {
			if (assets[j].d.group != assets[i].d.group)
				continue;
			if (assets[j].d.sector > assets[i].d.sector &&
			    (next < 0 || assets[j].d.sector < assets[next].d.sector))
				next = j;
			if (assets[j].d.sector < assets[first].d.sector)
				first = j;
		}
		assets[i].group_next  = (int16_t) next;
		assets[i].group_first = (int16_t) first;
	}
	mem_level_release(mark);
	base_lba = pool_lba = (int32_t) lba;
	if (!pool_sector)
		pool_sector = mem_game_alloc(n * sizeof *pool_sector);
	for (uint32_t i = 0; i < n; i++)
		pool_sector[i] = assets[i].d.sector;
	stream_level_n = level;
	printf("stream: %s, %d assets from sector %d\n", name, num_assets, (int) base_lba);
	return 1;
}

/* the level's own file: the pool's directory, each asset's sector in it
   (QST_ABSENT: not in it: the level never wants it; wanted all the same, a
   bench build says so), read through the level arena's top */
Q_COLD void stream_level(const char *map)
{
	for (int i = 0; i < num_assets; i++) {
		assets[i].d.sector = pool_sector[i];
		assets[i].absent   = 0;
		assets[i].banked   = 0;
		assets[i].ram_ofs  = ~0u;
	}
	base_lba = pool_lba;
	ram_first = ram_sectors = 0;
	ram_sounds = 0;
	char name[32];
	q_snprintf(name, sizeof name, "\\LVL\\%s\\%s_S%d.DAT;1", map, map, stream_level_n);
	uint32_t lba, mark = mem_level_mark();
	qst_header *h = num_assets && cd_find(name, &lba, NULL) ? mem_level_alloc(2 * QST_SECTOR) : 0;
	if (!h) {
		printf("stream: no %s: the pool's reads\n", name);
		return;
	}
	int ok = read_sectors((int32_t) lba, 2, h, CDW_DIR, 0) && !memcmp(h->magic, QSTREAM_MAGIC, 4) &&
	         h->num_assets == (uint32_t) num_assets && h->dir_sectors <= 2 &&
	         h->ofs_assets + h->num_assets * sizeof(qst_asset) <= h->dir_sectors * QST_SECTOR;
	const qst_asset *d = (const qst_asset *) ((const uint8_t *) h + h->ofs_assets);
	int in = 0, banked = 0;
	uint32_t rlo = ~0u, rhi = 0;
	for (int i = 0; ok && i < num_assets; i++) {
		if (d[i].hash != assets[i].d.hash)
			ok = 0;
		else if (d[i].sector == QST_BANKED)
			assets[i].absent = assets[i].banked = 1, banked++;
		else if (d[i].sector == QST_ABSENT)
			assets[i].absent = 1;
		else {
			in++;
			if (d[i].flags & QSTF_RAM) {
				uint32_t end = d[i].sector + (d[i].size + QST_SECTOR - 1) / QST_SECTOR;
				if (d[i].sector < rlo) rlo = d[i].sector;
				if (end > rhi)         rhi = end;
			}
		}
	}
	if (!ok) {
		printf("stream: %s is not the pool's directory: the pool's reads\n", name);
		for (int i = 0; i < num_assets; i++)
			assets[i].absent = assets[i].banked = 0;
		mem_level_release(mark);
		return;
	}
	for (int i = 0; i < num_assets; i++)
		if (!assets[i].absent) {
			assets[i].d.sector = d[i].sector;
			if (d[i].flags & QSTF_RAM) {
				assets[i].ram_ofs = (d[i].sector - rlo) * QST_SECTOR;
				ram_sounds++;
			}
		}
	if (rhi > rlo) {
		ram_first   = rlo;
		ram_sectors = rhi - rlo;
	}
	base_lba = (int32_t) lba;
	mem_level_release(mark);
	printf("stream: %s, %d of %d assets, %d sounds in the bank, %d for RAM (%u KB), from sector %d\n", name, in,
	       num_assets, banked, ram_sounds, (unsigned) (ram_sectors * QST_SECTOR / 1024), (int) base_lba);
}

#ifdef Q1PSX_BENCH
/* The bench's look at every model's header, its first sector read at boot:
   the RAM it keeps once its images are in VRAM (trims(): its header, frame
   map, frames and records), the residency report's measure; its frames,
   the monsters' check of their state tables (stream_frame_stored) */
#define BENCH_ASSETS 512
#define BENCH_FMAP   2048
static uint32_t bench_keep[BENCH_ASSETS];
static uint16_t bench_nframes[BENCH_ASSETS], bench_fmap_at[BENCH_ASSETS], bench_fmap[BENCH_FMAP];

static void bench_scan(void)
{
	uint32_t mark = mem_level_mark(), used = 0;
	uint8_t *b = mem_level_alloc(QST_SECTOR);
	for (int a = 0; a < num_assets && a < BENCH_ASSETS; a++) {
		const asset_t *as = &assets[a];
		bench_keep[a] = ALIGN16(as->d.size);
		bench_nframes[a] = 0;
		bench_fmap_at[a] = 0xffff;
		if (as->d.kind != QSTK_MODEL || !b || !read_sectors(base_lba + (int32_t) as->d.sector, 1, b, CDW_DIR, a))
			continue;
		const qst_model *m = (const qst_model *) b;
		bench_keep[a]    = ALIGN16(m->images[0].ofs);
		bench_nframes[a] = m->num_frames;
		if ((m->flags & QSTM_FMAP) && sizeof *m + m->num_frames * 2u <= QST_SECTOR &&
		    used + m->num_frames <= BENCH_FMAP) {
			memcpy(&bench_fmap[used], m + 1, m->num_frames * 2u);
			bench_fmap_at[a] = (uint16_t) used;
			used += m->num_frames;
		}
	}
	mem_level_release(mark);
}

int stream_frame_stored(int a, int frame)
{
	if (a < 0 || a >= num_assets || a >= BENCH_ASSETS || frame < 0 || frame >= bench_nframes[a])
		return 0;
	return bench_fmap_at[a] == 0xffff || bench_fmap[bench_fmap_at[a] + frame] != QST_NOFRAME;
}

uint32_t stream_kept(int a)
{
	return a >= 0 && a < num_assets && a < BENCH_ASSETS ? bench_keep[a] : 0;
}
#endif

Q_COLD void stream_init(int level)
{
	num_assets = 0;
	for (int i = 0; i < MAX_RES; i++)
		res[i].asset = -1;
	load_directory(level);
#ifdef Q1PSX_BENCH
	bench_scan();
#endif
}

int stream_find(const char *name)
{
	return stream_find_hash(qs_hash(name));
}

uint32_t stream_hash(int a)
{
	return a >= 0 && a < num_assets ? assets[a].d.hash : 0;
}

int stream_find_hash(uint32_t hh)
{
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

/* the map's 4-bit pages' free rows (each 4-bit page's below what it took,
   the 8-bit pages it leaves whole), the game's pages' free rectangles */
static void regions_build(void)
{
	num_regions = 0;
	stream_stats.vram_texels = 0;
	for (int p = 0; p < QTEX_MAP_PAGES; p++)
		if (world.page4_rows[p] < 256)            /* 4-bit page p: half of 8-bit page p / 2 */
			region_add(p / 2, (p & 1) * 128, world.page4_rows[p], 128, 256 - world.page4_rows[p]);
	for (int p = QTEX_MAP_PAGES / 2; p < QTEX_MAP_PAGES; p++)
		region_add(p, 0, 0, 256, 256);
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
static int vram_place(int w, int h, vrect_t *out, int upright)
{
	uint32_t bkey = ~0u;
	for (int t = 0; t < (upright ? 1 : 2); t++) {
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

/* next_wanted found nothing to read and nothing it reads changed since:
   not asked again every idle frame (a pass, a slot freed, an asset in or
   stuck clear it); a bit per kind it looks at (W_*) */
static uint8_t none_wanted;
enum { W_DRIVE = 1, W_SWAP = 2, W_ANY = 3 };   /* the drive's reads, the RAM sounds'
                                                 swaps (§99), both (the preload) */

static void slot_free(res_t *r)
{
	none_wanted = 0;
	r->asset = -1;
	r->ram   = r->tmp = 0;
	r->ram_size = r->tmp_size = 0;
	r->nimg  = 0;
	r->trim  = 0;
	r->sound = 0;
	r->spu_size = 0;
}

static void evict(int a)
{
	asset_t *as = &assets[a];
	if (as->state != A_RES)
		return;
	res_t *r = &res[as->res];
	SLOG("stream: out %08x (%u B, last %u)\n", (unsigned) as->d.hash, (unsigned) r->ram_size,
	     (unsigned) as->last);
	if (r->sound)
		snd_forget(SND_STREAM + a);           /* (the voices playing it stopped) */
	else if (r->trim)                         /* (its upload not taken yet) */
		DrawSync(0);
	if (!r->sound)
		mdl_forget(a);
	slot_free(r);
	as->state = A_DISC;
	stream_stats.evictions++;
}

/* what the resident assets wanted at least as much as `score` hold: RAM
   bytes, VRAM texels */
static void held_by_more(uint32_t score, uint32_t *ram, uint32_t *vram, uint32_t *spu)
{
	uint32_t rb = 0, vt = 0, sb = 0;
	for (int i = 0; i < MAX_RES; i++) {
		const res_t *r = &res[i];
		if (r->asset < 0 || assets[r->asset].state != A_RES || assets[r->asset].last > score)
			continue;
		rb += r->ram_size + r->tmp_size;
		for (int k = 0; k < r->nimg; k++)
			vt += (uint32_t) (r->img[k].w * r->img[k].h);
		sb += r->spu_size;
	}
	*ram  = rb;
	*vram = vt;
	*spu  = sb;
}

/* The resident assets sorted by their score (the most wanted first), with
   running totals of what they hold: what the assets wanted at least as
   much as a score hold is then a binary search. next_wanted asks it of
   every asset stuck for room, every idle frame: walking the slots and
   their images each time was quadratic (E1M3's crowd: 29 % of its ticks,
   porting-notes §72). Made once a next_wanted that meets a stuck asset,
   on its stack (768 bytes for a moment, not for good). */
typedef struct { uint32_t last, ram, vram, spu; } held_t;

static int held_sort(held_t *held)
{
	int num_held = 0;
	for (int i = 0; i < MAX_RES; i++) {
		const res_t *r = &res[i];
		if (r->asset < 0 || assets[r->asset].state != A_RES)
			continue;
		uint32_t vt = 0;
		for (int k = 0; k < r->nimg; k++)
			vt += (uint32_t) (r->img[k].w * r->img[k].h);
		uint32_t last = assets[r->asset].last;
		int j = num_held++;
		while (j > 0 && held[j - 1].last > last) {     /* (insertion: 48 at most) */
			held[j] = held[j - 1];
			j--;
		}
		held[j].last = last;
		held[j].ram  = r->ram_size + r->tmp_size;
		held[j].vram = vt;
		held[j].spu  = r->spu_size;
	}
	for (int j = 1; j < num_held; j++) {
		held[j].ram  += held[j - 1].ram;
		held[j].vram += held[j - 1].vram;
		held[j].spu  += held[j - 1].spu;
	}
	return num_held;
}

/* held_by_more's sums from the sorted table (held_sort's): the assets whose
   score is at most `score` */
static void held_upto(const held_t *held, int num_held, uint32_t score, uint32_t *ram, uint32_t *vram,
                      uint32_t *spu)
{
	int lo = 0, hi = num_held;
	while (lo < hi) {
		int m = (lo + hi) >> 1;
		if (held[m].last <= score)
			lo = m + 1;
		else
			hi = m;
	}
	*ram  = lo ? held[lo - 1].ram : 0;
	*vram = lo ? held[lo - 1].vram : 0;
	*spu  = lo ? held[lo - 1].spu : 0;
}

/* asset a found no room: not tried again until the assets wanted as much
   hold less */
static void set_stuck(int a)
{
	asset_t *as = &assets[a];
	none_wanted = 0;
	as->stuck = ST_ROOM;
	held_by_more(as->last, &as->stuck_ram, &as->stuck_vram, &as->stuck_spu);
	SLOG("stream: no room for %08x (score %u; the assets wanted as much hold %u B, %u texels, %u B of SPU)\n",
	     (unsigned) as->d.hash, (unsigned) as->last, (unsigned) as->stuck_ram, (unsigned) as->stuck_vram,
	     (unsigned) as->stuck_spu);
}

/* the resident asset wanted least, if it is wanted less than `score` (and
   is not pinned): -1 if none; `need`: one holding RAM, VRAM or SPU RAM, or
   any (its slot of res[]) */
enum { NEED_RAM, NEED_VRAM, NEED_SPU, NEED_SLOT };

static int least_wanted(uint32_t score, int need)
{
	int worst = -1;
	uint32_t wl = 0;
	for (int i = 0; i < MAX_RES; i++) {
		int a = res[i].asset;
		if (a < 0 || assets[a].state != A_RES)
			continue;
		if (need == NEED_RAM ? !res[i].ram_size : need == NEED_VRAM ? !res[i].nimg :
		    need == NEED_SPU ? !res[i].spu_size : 0)
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

/* a slot for an asset wanted at `score`: a free one, else the one of the
   resident asset wanted least, dropped if it is wanted less (the sounds and
   the models share the MAX_RES slots: a map's own sounds could hold them
   all against a monster in sight, E2M4's shambler: docs/porting-notes.md
   §67); -1 if none */
static int slot_for(uint32_t score)
{
	int slot;
	while ((slot = free_slot()) < 0) {
		int v = least_wanted(score, NEED_SLOT);
		if (v < 0)
			return -1;
		evict(v);
	}
	return slot;
}

/* the images of one resolution placed in VRAM (room made by dropping
   what is wanted less than `score`; `upright`: none turned): 1, or 0 with
   none taken */
static int place_images(res_t *r, const qst_image *img, int nimg, uint32_t score, vrect_t at[2], int upright)
{
	r->nimg = 0;
	for (int k = 0; k < nimg; k++) {
		while (!vram_place(img[k].w, img[k].h, &at[k], upright)) {
			int v = least_wanted(score, NEED_VRAM);
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

/* a model's frames within its `size` bytes: those stored, and its frame
   map's entries among them (formats/qstream.h QSTM_FMAP) */
Q_COLD static int frames_ok(const qst_model *m, uint32_t size)
{
	uint32_t stored = qst_stored_frames(m);
	if (!stored || sizeof *m + qst_map_bytes(m->flags, m->num_frames) > m->ofs_frames ||
	    m->ofs_frames + stored * m->num_verts * 4 > size)
		return 0;
	if (m->flags & QSTM_FMAP)
		for (int f = 0; f < m->num_frames; f++) {
			uint32_t i = ((const uint16_t *) (m + 1))[f];
			if (i >= stored && i != QST_NOFRAME)
				return 0;
		}
	if ((m->flags & QSTM_SORT) && !(m->flags & QSTM_LOD))   /* (a sorted body's records carry */
		return 0;                                             /*  their tables: ds_records) */
	if (m->flags & QSTM_LOD) {                    /* its level of detail (§88): the block */
		uint32_t at = sizeof *m + qst_map_bytes(m->flags, m->num_frames);
		if (at + sizeof(qst_lod) > m->ofs_frames)
			return 0;
		const qst_lod *l = qst_lod_of(m);
		if (l->num_min > m->num_verts || at + qst_lod_bytes(m->num_verts, l->num_min) > m->ofs_frames ||
		    stored >= 2048)
			return 0;
		for (int b = 0; b < QST_LOD_BANDS; b++)
			if (l->num_verts[b] > m->num_verts || l->num_verts[b] < l->num_min ||
			    (b && l->num_verts[b] > l->num_verts[b - 1]))
				return 0;
		const uint16_t *par = qst_lod_parents(l);
		for (uint32_t v = l->num_min; v < m->num_verts; v++)
			if (par[v - l->num_min] >= v)
				return 0;
	}
	return 1;
}

/* a model asset just read: its images to VRAM (at the skin's resolution,
   or its fallback at half when no room is left for that; an image may go
   transposed), their UVs into its records, its texture words. 1 done, 0
   no room (RAM or VRAM, with what is wanted less dropped), -1 not a model
   this engine reads */
static int install_model(int a)
{
	asset_t *as = &assets[a];
	res_t   *r  = &res[as->res];
	uint32_t size = as->d.size;
	const qst_model *m = (const qst_model *) r->ram;
	if (m->num_images < 1 || m->num_images > 2 || m->ofs_records + m->len_records > size ||
	    !frames_ok(m, size)) {
		printf("stream: asset %08x is not a model this engine reads\n", (unsigned) as->d.hash);
		return -1;
	}
	int nimg = m->num_images;
	qst_image full[2], img[2];
	memcpy(full, m->images, sizeof full);
	memcpy(img, m->images, sizeof img);
	for (int k = 0; k < nimg; k++)
		if (m->images[k].ofs + (uint32_t) m->images[k].w * m->images[k].h > size ||
		    m->small[k].ofs + (uint32_t) m->small[k].w * m->small[k].h > size) {
			printf("stream: asset %08x: image out of bounds\n", (unsigned) as->d.hash);
			return -1;
		}
	/* the skin's resolution, then its fallback's; an image turned where it
	   fits better so, upright when no RAM is left for its turned pixels
	   (room made by dropping what is wanted less; the read's block may
	   move then) */
	vrect_t at[2];
	int half = 0, placed = 0;
	for (int tries = 0; tries < 4 && !placed; tries++) {
		half = tries >> 1;
		int upright = tries & 1;
		memcpy(img, half ? m->small : m->images, sizeof img);
		if (!place_images(r, img, nimg, as->last, at, upright)) {
			tries |= 1;                           /* (upright cannot fit either) */
			continue;
		}
		uint32_t tbytes = 0;
		for (int k = 0; k < nimg; k++)
			if (at[k].flags & VR_T)
				tbytes += ALIGN64((uint32_t) at[k].w * at[k].h);
		placed = 1;
		if (!tbytes)
			break;
		while (!(r->tmp = ram_alloc(tbytes))) {
			int v = least_wanted(as->last, NEED_RAM);
			if (v < 0) {
				r->nimg = 0;                  /* (its places given back) */
				placed = 0;
				break;
			}
			evict(v);
		}
		if (placed)
			r->tmp_size = ALIGN16(tbytes);
		m = (const qst_model *) r->ram;
	}
	if (!placed) {
		printf("stream: no room for %08x's %dx%d images\n", (unsigned) as->d.hash, full[0].w, full[0].h);
		return 0;
	}
	if (half)
		stream_stats.halved++;
	r->half = (uint8_t) half;
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
		if ((m->flags & QSTM_LOD) &&              /* (its table at its end: ds_records reads it) */
		    rec->size < rec->polys + rec->num_tris * sizeof(qw_tri) + QST_LOD_REC_BYTES) {
			printf("stream: asset %08x: a record without its level-of-detail table\n", (unsigned) as->d.hash);
			return -1;
		}
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

/* ------------------------------------------------------------ SPU pool -- */

/* the sounds' blocks in the SPU pool, by address: their bounds and slots */
static int spu_blocks(uint32_t lo[MAX_RES], uint32_t hi[MAX_RES], int slot[MAX_RES])
{
	int nb = 0;
	for (int i = 0; i < MAX_RES; i++) {
		if (res[i].asset < 0 || !res[i].spu_size)
			continue;
		int j = nb++;
		for (; j > 0 && lo[j - 1] > res[i].snd.spu; j--) {
			lo[j] = lo[j - 1];
			hi[j] = hi[j - 1];
			slot[j] = slot[j - 1];
		}
		lo[j] = res[i].snd.spu;
		hi[j] = res[i].snd.spu + res[i].spu_size;
		slot[j] = i;
	}
	return nb;
}

/* a place for n bytes (a multiple of 64): the smallest gap the resident
   sounds leave in the pool that holds it */
static int spu_place(uint32_t n, uint32_t *at)
{
	uint32_t lo[MAX_RES], hi[MAX_RES];
	int slot[MAX_RES], nb = spu_blocks(lo, hi, slot);
	uint32_t best = ~0u, prev = spu_lo;
	for (int i = 0; i <= nb; i++) {
		uint32_t end = i < nb ? lo[i] : spu_hi;
		if (end > prev && end - prev >= n && end - prev < best) {
			best = end - prev;
			*at = prev;
		}
		if (i < nb && hi[i] > prev)
			prev = hi[i];
	}
	return best != ~0u;
}

/* the sound to drop for n bytes of the SPU pool, wanted less than `score`:
   the least wanted whose block with the gaps around it would hold them,
   else the least wanted (one more gap merged); -1 if none */
static int spu_victim(uint32_t n, uint32_t score)
{
	uint32_t lo[MAX_RES], hi[MAX_RES];
	int slot[MAX_RES], nb = spu_blocks(lo, hi, slot), best = -1;
	uint32_t wl = 0;
	for (int i = 0; i < nb; i++) {
		int a = res[slot[i]].asset;
		uint32_t l = assets[a].last;
		if (assets[a].state != A_RES || l == STREAM_PIN || l <= score)
			continue;
		uint32_t from = i ? hi[i - 1] : spu_lo, to = i + 1 < nb ? lo[i + 1] : spu_hi;
		if (to - from >= n && (best < 0 || l > wl)) {
			best = a;
			wl = l;
		}
	}
	return best >= 0 ? best : least_wanted(score, NEED_SPU);
}

/* ---------------------------------------------------------- sound reads -- */

/* a place in the SPU pool for sound a (room made by dropping the sounds
   wanted less), its slot taken: its read can start (0: no room; none
   without the staging buffer either: a level arena too full for it, as
   E3M4's and E4M7's were, read the sounds to address 0, each found "not a
   sound") */
static int sound_room(int a)
{
	asset_t *as = &assets[a];
	if (!stage || as->d.size <= sizeof(qst_sound))
		return 0;
	int slot = slot_for(as->last);
	if (slot < 0)
		return 0;
	uint32_t n = ALIGN64(as->d.size - sizeof(qst_sound)), at = 0;
	while (!spu_place(n, &at)) {
		int v = spu_victim(n, as->last);
		if (v < 0)
			return 0;
		evict(v);
	}
	res_t *r = &res[slot];
	slot_free(r);
	r->asset    = (int16_t) a;
	r->sound    = 1;
	r->spu_size = n;
	r->snd.spu  = at;
	as->res     = (uint8_t) slot;
	as->state   = A_READING;
	return 1;
}

static void sound_in(int a, int ok, int broken);

static int sound_start(int a)
{
	if (!sound_room(a))
		return 0;
	uint32_t at = res[assets[a].res].snd.spu, n = res[assets[a].res].spu_size;
	sread.a      = a;
	sread.spu    = at;
	sread.left   = n;
	sread.sector = 0;
	sread.step   = SR_WAIT;
	return 1;
}

/* a RAM-resident sound (§86): its place in the SPU's swap room taken as a
   disc read's, its samples sent from the block by one DMA, no read; the
   SPU's DMA free (0: no room) */
static int ram_start(int a)
{
	if (!sound_room(a))
		return 0;
	asset_t *as = &assets[a];
	res_t *r = &res[as->res];
	const qst_sound *h = (const qst_sound *) (ram_block + as->ram_ofs);
	if (sizeof *h + h->len > as->d.size || !h->len || (h->len & 15)) {
		sound_in(a, 0, 1);                    /* (not a sound this engine reads: never again) */
		return 1;
	}
	r->snd.len   = h->len;
	r->snd.pitch = h->pitch;
	r->snd.flags = h->flags;
	r->snd.ticks = h->ticks;
	uint32_t n = h->len < r->spu_size ? ALIGN64(h->len) : r->spu_size;
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(r->snd.spu);
	SpuWrite((const uint32_t *) (h + 1), n);
	swap_a = a;
	stream_stats.ram_swaps++;
	return 1;
}

/* the read's next piece: its sectors into the staging buffer (0: the
   drive refused) */
static int sound_read_piece(void)
{
	const asset_t *as = &assets[sread.a];
	int total = (int) ((as->d.size + QST_SECTOR - 1) / QST_SECTOR), n = total - sread.sector;
	if (n > STAGE_SECTORS)
		n = STAGE_SECTORS;
	sread.sectors = (uint16_t) n;
	sread.step    = SR_READ;
	return cd_read_start(base_lba + (int32_t) as->d.sector + sread.sector, (uint32_t) n, stage, CDW_SOUND, sread.a);
}

/* the piece read: its samples to their place (the SPU's DMA: whole 64-byte
   units, the last one's from the sector's padding); the first piece's
   header kept (0: not a sound this engine reads) */
static int sound_upload_piece(void)
{
	asset_t *as = &assets[sread.a];
	res_t *r = &res[as->res];
	const uint8_t *src = stage;
	uint32_t bytes = (uint32_t) sread.sectors * QST_SECTOR;
	if (!sread.sector) {
		const qst_sound *h = (const qst_sound *) stage;
		if (sizeof *h + h->len > as->d.size || !h->len || (h->len & 15))
			return 0;
		r->snd.len   = h->len;
		r->snd.pitch = h->pitch;
		r->snd.flags = h->flags;
		r->snd.ticks = h->ticks;
		src   += sizeof *h;
		bytes -= sizeof *h;
	}
	if (bytes > sread.left)
		bytes = sread.left;
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);   /* (a RAM sound's swap in flight: the
	                                               loading screen's back-to-back frames) */
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(sread.spu);
	SpuWrite((const uint32_t *) src, bytes);
	sread.spu    += bytes;
	sread.left   -= bytes;
	sread.sector += sread.sectors;
	sread.step    = SR_UPLOAD;
	return 1;
}

/* the sound's read is over: in, or back on the disc (a read error: tried
   again after a while; not a sound: never) */
static void sound_in(int a, int ok, int broken)
{
	asset_t *as = &assets[a];
	none_wanted = 0;
	if (ok) {
		as->state = A_RES;
		if (!ram_block || as->ram_ofs == ~0u) {   /* (a read, not a swap) */
			stream_stats.reads++;
			stream_stats.read_bytes += as->d.size;
		}
		SLOG("stream: in %08x (sound, %u B at SPU %05x, last %u)\n", (unsigned) as->d.hash,
		     (unsigned) res[as->res].snd.len, (unsigned) res[as->res].snd.spu, (unsigned) as->last);
		return;
	}
	slot_free(&res[as->res]);
	as->state = A_DISC;
	stream_stats.failures++;
	if (broken) {
		printf("stream: asset %08x is not a sound this engine reads\n", (unsigned) as->d.hash);
		as->stuck = ST_BROKEN;
	} else {
		as->backoff = BACKOFF;
	}
}

static void sound_done(int ok, int broken)
{
	int a = sread.a;
	sread.a = -1;
	sound_in(a, ok, broken);
}

/* the sound read's progress: 1 once it is over */
static int sound_step(void)
{
	if (sread.step == SR_WAIT)
		return 0;
	if (sread.step == SR_READ) {
		int left = cd_poll();                 /* (also runs CdReadRetry's retries) */
		if (left > 0)
			return 0;
		if (left < 0) {
			sound_done(0, 0);
			return 1;
		}
		if (!sound_upload_piece()) {
			sound_done(0, 1);
			return 1;
		}
		return 0;
	}
	if (!SpuIsTransferCompleted(SPU_TRANSFER_PEEK))
		return 0;
	if (!sread.left) {
		sound_done(1, 0);
		return 1;
	}
	if (!music_read_ok(STAGE_SECTORS) || zone_drive_wanted() || !cd_ready())
		return 0;                            /* (the SPU music's refill first, a world
		                                        chunk's read: engine/zone.c; the last
		                                        read's Pause done: engine/cd.h) */
	if (!sound_read_piece()) {
		sound_done(0, 0);
		return 1;
	}
	return 0;
}

/* the same, at once (the loading screen) */
static void sound_read_sync(void)
{
	const asset_t *as = &assets[sread.a];
	while (sread.a >= 0) {
		int total = (int) ((as->d.size + QST_SECTOR - 1) / QST_SECTOR), n = total - sread.sector;
		if (n > STAGE_SECTORS)
			n = STAGE_SECTORS;
		sread.sectors = (uint16_t) n;
		if (!read_sectors(base_lba + (int32_t) as->d.sector + sread.sector, n, stage, CDW_SOUND, sread.a)) {
			sound_done(0, 0);
			return;
		}
		if (!sound_upload_piece()) {
			sound_done(0, 1);
			return;
		}
		SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
		if (!sread.left)
			sound_done(1, 0);
	}
}

/* the read of asset a is done: install it, or put it back on the disc
   (a read error: tried again after a while; no room: once there may be) */
static void model_in(int a, int ok)
{
	asset_t *as = &assets[a];
	int in = !ok ? 0 : as->d.kind == QSTK_MODEL ? install_model(a) : -1;
	none_wanted = 0;
	if (in > 0) {
		as->state = A_RES;
		stream_stats.reads++;
		stream_stats.read_bytes += as->d.size;
		SLOG("stream: in %08x (%u B, last %u)\n", (unsigned) as->d.hash, (unsigned) as->d.size,
		     (unsigned) as->last);
		return;
	}
	slot_free(&res[as->res]);
	as->state = A_DISC;
	stream_stats.failures++;
	if (!ok)
		as->backoff = BACKOFF;
	else if (in < 0)
		as->stuck = ST_BROKEN;
	else
		set_stuck(a);
}

static void read_done(int a, int ok)
{
	reading = -1;
	model_in(a, ok);
}

/* RAM for asset a's read (its sectors whole), room made by dropping what
   is wanted less: its slot, or -1 */
static int reserve(int a)
{
	asset_t *as = &assets[a];
	int slot = slot_for(as->last);
	if (slot < 0)
		return -1;
	uint32_t bytes = (as->d.size + QST_SECTOR - 1) / QST_SECTOR * QST_SECTOR;
	uint8_t *dst;
	while (!(dst = ram_alloc(bytes))) {
		int v = least_wanted(as->last, NEED_RAM);
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
	int n = (int) ((as->d.size + QST_SECTOR - 1) / QST_SECTOR);
	if (!cd_read_start(base_lba + (int32_t) as->d.sector, (uint32_t) n, r->ram, CDW_MODEL, a)) {
		read_done(a, 0);
		return;
	}
	reading = a;
	SLOG("stream: reading %08x, %d sectors (score %u)\n", (unsigned) as->d.hash, n, (unsigned) as->last);
}

/* the wanted asset to read next: the most wanted on the disc within its
   reach (the biggest of equals first: its room found before the small
   ones break it up), -1 if none; `which`: the drive's reads, the RAM
   sounds' swaps, or both (W_*) */
static int next_wanted(int which)
{
	if ((none_wanted & which) == which)
		return -1;
	int best = -1, num_held = -1;                /* (-1: not sorted yet) */
	int swaps = ram_block != 0;                  /* (no block: the RAM sounds read) */
	held_t held[MAX_RES];
	for (int a = 0; a < num_assets; a++) {
		asset_t *as = &assets[a];
		if (as->last >= STREAM_DEAD || as->state != A_DISC || as->backoff || as->stuck == ST_BROKEN)
			continue;                         /* (the unwanted first: most of them) */
		if (!(which & (swaps && as->ram_ofs != ~0u ? W_SWAP : W_DRIVE)))
			continue;
		uint32_t reach = as->d.kind == QSTK_SOUND ? STREAM_SOUND_REACH : STREAM_REACH;
		if ((as->last & STREAM_DIST) > reach)
			continue;
		if (as->absent) {                     /* (in the level's bank, or not in its file: the
		                                         cook's list missed it; never read) */
#ifdef Q1PSX_BENCH
			if (!as->backoff && !as->banked) {
				printf("stream: asset %08x wanted, not in the level's file\n", (unsigned) as->d.hash);
				as->backoff = 255;
			}
#endif
			continue;
		}
		if (as->stuck == ST_ROOM) {
			uint32_t ram, vram, spu;
			if (num_held < 0)
				num_held = held_sort(held);
			held_upto(held, num_held, as->last, &ram, &vram, &spu);
			if (ram >= as->stuck_ram && vram >= as->stuck_vram && spu >= as->stuck_spu)
				continue;                     /* (no more room than when it failed) */
			as->stuck = ST_NONE;
		}
		if (best < 0 || as->last < assets[best].last ||
		    (as->last == assets[best].last && as->d.size > assets[best].d.size))
			best = a;
	}
	if (best < 0)
		none_wanted |= (uint8_t) which;
	return best;
}

/* ---------------------------------------------------------- the drive -- */

int stream_reading(void)
{
	return reading >= 0 || (sread.a >= 0 && (sread.step == SR_READ ||
	                        (sread.step == SR_UPLOAD && !SpuIsTransferCompleted(SPU_TRANSFER_PEEK)))) ||
	       zone_reading();                   /* (a world chunk's read: the drive's too; a RAM
	                                             sound's swap is not) */
}

uint8_t *stream_stage(void)
{
	return stage;
}

/* -------------------------------------------------------------- passes -- */

void stream_want(int a, uint32_t score)
{
	if (a < 0 || a >= num_assets)
		return;
	if (score < assets[a].score)
		assets[a].score = score;
}

static int32_t pass_origin[3];             /* stream_origin at the last pass */

static void pass(void)
{
	none_wanted = 0;
	memcpy(pass_origin, stream_origin, sizeof pass_origin);
	for (int a = 0; a < num_assets; a++)
		assets[a].score = ~0u;
	ents_stream_wants();
	snd_stream_wants();
	int resident = 0, wanted = 0;
	for (int a = 0; a < num_assets; a++) {
		asset_t *as = &assets[a];
		as->last = as->score;
		if (as->backoff)
			as->backoff--;
		resident += as->state == A_RES;
		wanted   += (as->last & STREAM_DIST) <= STREAM_REACH && as->last < STREAM_DEAD;
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

/* the load's end: the load overlay's room (the pool's top) is the pool's */
Q_COLD void stream_load_done(void)
{
	if (pool_end_load) {
		pool_end      = pool_end_load;
		pool_end_load = 0;
	}
}

void stream_abort(void)
{
	if (reading >= 0 || (sread.a >= 0 && sread.step == SR_READ)) {
		cd_read_cut();                        /* (the drive paused, waited for, before the */
		cd_read_sync();                       /*  next command: music.c music_stop's)       */
		CdControlB(CdlPause, 0, 0);
		cd_busy();
		reading = -1;
	}
	lend_at = 0;                              /* (asked for again at the map's start) */
	if (lend_state != L_NONE)
		lend_state = L_WANT;
	if (sread.a >= 0 || swap_a >= 0) {
		SpuIsTransferCompleted(SPU_TRANSFER_WAIT);   /* (no DMA left reading the stage, the block) */
		sread.a = swap_a = -1;
	}
	waiting = -1;
	DrawSync(0);                              /* (no upload left reading the pool) */
	pool = pool_top = pool_end = 0;           /* (the level arena is about to be reset) */
	pool_end_load = 0;
	ram_block = 0;
	for (int a = 0; a < num_assets; a++)
		assets[a].state = A_DISC;
	for (int i = 0; i < MAX_RES; i++)
		slot_free(&res[i]);
}

Q_COLD void stream_map_start(uint32_t keep)
{
	stream_abort();
	for (int a = 0; a < num_assets; a++) {
		assets[a].score   = assets[a].last = ~0u;
		assets[a].backoff = 0;
		assets[a].stuck   = ST_NONE;
	}
	memset(&stream_stats, 0, sizeof stream_stats);
	/* a lent block first, then the expanded frames' cache, the staging
	   buffer, the pool; the lent block after the pool, at its end */
	ram_block = 0;
	if (ram_sectors) {                        /* the level's RAM sounds, one read (§86) */
		uint32_t mark = mem_level_mark();
		uint8_t *b = mem_level_alloc(ram_sectors * QST_SECTOR);
		if (b && read_sectors(base_lba + (int32_t) ram_first, (int) ram_sectors, b, CDW_FILE, 0))
			ram_block = b;
		else {
			printf("stream: the RAM sounds' block (%u KB) did not fit or read: those sounds stream from the disc\n",
			       (unsigned) (ram_sectors * QST_SECTOR / 1024));
			mem_level_release(mark);
		}
	}
	uint32_t room = mem_level_bytes() - mem_level_used(), lend = 0;
	if (lend_state == L_WANT) {
		lend = room >= lend_bytes + keep + 64 ? lend_bytes : 0;
		lend_state = lend ? L_ROOM : L_SHORT;
	}
	uint8_t *cache = room - lend >= MDL_CACHE ? mem_level_alloc(MDL_CACHE) : 0;
	if (cache)
		room -= MDL_CACHE;
	mdl_reset(cache, MDL_CACHE);
	stage = room - lend >= STAGE_SECTORS * QST_SECTOR ? mem_level_alloc(STAGE_SECTORS * QST_SECTOR) : 0;
	if (stage)
		room -= STAGE_SECTORS * QST_SECTOR;
	uint32_t left = room - lend > keep + 64 ? (room - lend - keep - 64) & ~15u : 0;
	pool = left + lend ? mem_level_alloc(left + lend) : 0;
	pool_top = pool;
	pool_end = pool ? pool + left : 0;
	lend_at  = lend && pool ? pool_end : 0;
	/* the load overlay sits in the pool's top until the load's end: the
	   preload's reads stop under it (stream_load_done gives the rest) */
	pool_end_load = 0;
	if (pool && pool < __load_start && pool_end > __load_start) {
		pool_end_load = pool_end;
		pool_end      = __load_start;
	}
#ifdef Q1PSX_BENCH
	else if (pool && pool >= __load_start)
		printf("stream: the pool starts past the load overlay (%u B of level): the overlay overrun\n",
		       (unsigned) mem_level_used());
#endif
	stream_stats.ram_size = left;
	regions_build();
	printf("stream: pools %u KB of RAM, %u K texels of VRAM in %d regions; %u KB lent; %d sounds in RAM (%u KB)\n",
	       (unsigned) left / 1024, (unsigned) stream_stats.vram_texels / 1024, num_regions,
	       (unsigned) (lend_at ? lend : 0) / 1024, ram_block ? ram_sounds : 0,
	       (unsigned) (ram_block ? ram_sectors * QST_SECTOR / 1024 : 0));
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

void stream_lend(int32_t lba, int sectors)
{
	lend_lba   = lba;
	lend_bytes = sectors > 0 ? (uint32_t) sectors * QST_SECTOR : 0;
	lend_state = lend_bytes ? L_WANT : L_NONE;
}

uint8_t *stream_lent(uint32_t *reads)
{
	*reads = lend_reads;
	return lend_state == L_IN ? lend_at : 0;
}

/* the RAM sounds' swaps (§86) beside the drive's reads, a world chunk's,
   a model's, a sound's (§99: after a teleport the chunks' reads held them
   back, the ring's pickup 367 units away unheard): the one in flight in
   once its DMA is done, the next started while the SPU's DMA is free */
static void swap_step(void)
{
	if (!SpuIsTransferCompleted(SPU_TRANSFER_PEEK))
		return;                               /* (the swap's, or a disc sound's piece) */
	if (swap_a >= 0) {
		int a = swap_a;
		swap_a = -1;
		sound_in(a, 1, 0);
	}
	int a = next_wanted(W_SWAP);
	if (a >= 0 && !ram_start(a))
		set_stuck(a);
}

/* the drive's reads: the one in flight, the next */
static void drive_step(void)
{
	if (reading >= 0) {                       /* a model's read */
		int left = cd_poll();                 /* (also runs CdReadRetry's retries) */
		if (left > 0)
			return;
		read_done(reading, left == 0);
	}
	if (sread.a >= 0 && sread.step != SR_WAIT && !sound_step())   /* a sound's, piece by piece */
		return;
	if (zone_drive_wanted())                 /* (a world chunk's read goes first: engine/zone.c) */
		return;
	if (waiting < 0) {
		int a = next_wanted(W_DRIVE);
		if (a < 0)
			return;
		if (assets[a].d.kind == QSTK_SOUND ? !sound_start(a) : reserve(a) < 0) {
			set_stuck(a);
			return;
		}
		waiting = a;
	}
	if (!music_read_ok(assets[waiting].d.kind == QSTK_SOUND ? STAGE_SECTORS
	                   : (int) ((assets[waiting].d.size + QST_SECTOR - 1) / QST_SECTOR)) || !cd_ready())
		return;                               /* (the music's ring too low: its refill first;
		                                         the last read's Pause not done: cd.h) */
	int a = waiting;
	waiting = -1;
	if (assets[a].d.kind != QSTK_SOUND)
		start_read(a);
	else if (!sound_read_piece())
		sound_done(0, 0);
}

void stream_frame(void)
{
	if (!num_assets || !pool)
		return;
	frame_no++;
	memcpy(stream_origin, pl.pm.pos, sizeof stream_origin);
	trims();
	int jump = 0;
	for (int k = 0; k < 3; k++) {
		int32_t d = stream_origin[k] - pass_origin[k];
		jump |= d > PASS_JUMP || d < -PASS_JUMP;
	}
	if (frame_no % PASS_FRAMES == 0 || jump)
		pass();
	drive_step();
	if (ram_block)                            /* (after: a piece's upload the drive started
	                                             goes first) */
		swap_step();
}

#ifdef Q1PSX_BENCH
uint32_t stream_score(int a)
{
	return a >= 0 && a < num_assets ? assets[a].last : ~0u;
}

static uint8_t sim_in[512];

uint32_t stream_size(int a)
{
	return a >= 0 && a < num_assets ? assets[a].d.size : 0;
}

int stream_sim_in(int a)
{
	return a >= 0 && a < num_assets && a < 512 && sim_in[a];
}


void stream_simulate(const int32_t org[3], uint32_t reach, uint32_t *wanted, uint32_t *wanted_bytes,
                     uint32_t *in, uint32_t *in_bytes)
{
	memcpy(stream_origin, org, sizeof stream_origin);
	pass();
	memset(sim_in, 0, sizeof sim_in);
	*wanted = *wanted_bytes = *in = *in_bytes = 0;
	uint32_t ram = stream_stats.ram_size, spu = spu_hi > spu_lo ? spu_hi - spu_lo : 0;
	for (int a = 0; a < num_assets && a < 512; a++) {
		const asset_t *as = &assets[a];
		if ((as->last & STREAM_DIST) <= (as->d.kind == QSTK_SOUND ? STREAM_SOUND_REACH : reach) &&
		    as->last < STREAM_DEAD) {
			(*wanted)++;
			*wanted_bytes += as->d.size;
		}
	}
	for (;;) {                                    /* (next_wanted's order) */
		int best = -1;
		for (int a = 0; a < num_assets && a < 512; a++) {
			const asset_t *as = &assets[a];
			if (sim_in[a] || as->stuck == ST_BROKEN || as->last >= STREAM_DEAD ||
			    (as->last & STREAM_DIST) > (as->d.kind == QSTK_SOUND ? STREAM_SOUND_REACH : reach))
				continue;
			if (best < 0 || as->last < assets[best].last ||
			    (as->last == assets[best].last && as->d.size > assets[best].d.size))
				best = a;
		}
		if (best < 0)
			break;
		sim_in[best] = 2;                         /* (looked at) */
		/* a sound: its samples; a model: read whole (its sectors), then
		   what it keeps once its images are in VRAM (trims()) */
		int sound = assets[best].d.kind == QSTK_SOUND;
		uint32_t n = sound ? ALIGN64(assets[best].d.size - sizeof(qst_sound))
		                   : (assets[best].d.size + QST_SECTOR - 1) / QST_SECTOR * QST_SECTOR;
		uint32_t keep = sound ? n : best < BENCH_ASSETS ? bench_keep[best] : ALIGN16(assets[best].d.size);
		uint32_t *pool_left = sound ? &spu : &ram;
		if (n > *pool_left)
			continue;                             /* (no room: the next ones may fit) */
		*pool_left -= keep;
		sim_in[best] = 1;
		(*in)++;
		*in_bytes += assets[best].d.size;
	}
	for (int a = 0; a < 512; a++)
		sim_in[a] = sim_in[a] == 1;
}

static const int32_t *preload_at;          /* stream_preload_at's origin */

void stream_preload_at(const int32_t org[3])
{
	preload_at = org;
	stream_preload();
	preload_at = 0;
}
#endif

Q_COLD void stream_preload(void)
{
	if (lend_state == L_ROOM &&               /* the lent block (the map's start gave it room) */
	    read_sectors(lend_lba, (int) (lend_bytes / QST_SECTOR), lend_at, CDW_FILE, 0)) {
		lend_state = L_IN;
		lend_reads++;
	}
	if (!num_assets || !pool)
		return;
#ifdef Q1PSX_BENCH
	if (preload_at)
		memcpy(stream_origin, preload_at, sizeof stream_origin);
	else
#endif
	if (world.has_spawn)                      /* (the player is put there after the load) */
		for (int k = 0; k < 3; k++)
			stream_origin[k] = world.spawn[k] * CLIP_SUB;
	else
		memcpy(stream_origin, pl.pm.pos, sizeof stream_origin);
	pass();
	for (;;) {
		int a = next_wanted(W_ANY);
		if (a < 0)
			break;
		if (assets[a].d.kind == QSTK_SOUND) {
			if (ram_block && assets[a].ram_ofs != ~0u) {
				if (!ram_start(a))
					set_stuck(a);
				else if (swap_a >= 0) {
					SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
					swap_a = -1;
					sound_in(a, 1, 0);
				}
			} else if (!sound_start(a)) {
				set_stuck(a);
			} else {
				sound_read_sync();
			}
			continue;
		}
		if (reserve(a) < 0) {
			set_stuck(a);
			continue;
		}
		int n = (int) ((assets[a].d.size + QST_SECTOR - 1) / QST_SECTOR);
		int ok = read_sectors(base_lba + (int32_t) assets[a].d.sector, n, res[assets[a].res].ram, CDW_MODEL, a);
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
	if (a < 0 || a >= num_assets || assets[a].state != A_RES || assets[a].d.kind != QSTK_MODEL)
		return 0;
	return (const qst_model *) res[assets[a].res].ram;
}

const stream_sound_t *stream_sound(int a)
{
	if (a < 0 || a >= num_assets || assets[a].state != A_RES || assets[a].d.kind != QSTK_SOUND)
		return 0;
	return &res[assets[a].res].snd;
}

int stream_is_sound(int a)
{
	return a >= 0 && a < num_assets && assets[a].d.kind == QSTK_SOUND;
}

void stream_spu_pool(uint32_t lo, uint32_t hi)
{
	spu_lo = ALIGN64(lo);
	spu_hi = hi & ~63u;
}

void stream_want_group(int a, uint32_t score, int spare, uint32_t spare_score)
{
	if (a < 0 || a >= num_assets)
		return;
	for (int i = assets[a].group_first; i >= 0; i = assets[i].group_next)
		stream_want(i, i == spare ? spare_score : score);
}

const qw_tex *stream_model_tex(int a)
{
	if (a < 0 || a >= num_assets || assets[a].state != A_RES)
		return 0;
	return res[assets[a].res].tex;
}
