/*
 * q1psx — the world's zones. See engine/zone.h, formats/qworld.h "zones"
 * and docs/porting-notes.md §57.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxcd.h>

#include "cd.h"
#include "mem.h"
#include "music.h"
#include "prof.h"
#include "render.h"
#include "stream.h"
#include "world.h"
#include "zone.h"

zone_rt_t    zone_rt __attribute__((aligned(16)));   /* (zone.s: one %hi for its words) */
zone_stats_t zone_stats;

#define SECTOR    CD_SECTOR
#define MAX_RES   6        /* chunks in the arena at once (in or being read) */
#define SET_NONE  0x100u   /* zone_rt.set before any call: no byte equals it */
#define BACKOFF   30       /* frames before a read that failed is tried again */

typedef struct {
	int16_t  zone;         /* -1: a free entry                       */
	uint8_t  ready;        /* read and checked                       */
	uint32_t at, size;     /* its place in the arena; whole sectors  */
} slot_t;

static const qw_zone_header *zh;   /* NULL: the map is whole */
static const qw_chunk_dir   *zdir;
static const qw_zone_set    *zsets;
static uint8_t  *arena;
static int32_t   zon_lba;
static uint32_t  skel_bytes;
static slot_t    slots[MAX_RES];
static int       cur_slot = -1;           /* the chunk drawn                     */
static int       cur_zone = -1;
static uint32_t  want_set = SET_NONE;     /* the camera leaf's set               */
static int       reading  = -1;           /* the slot being read                 */
static int       placed   = -1;           /* a slot placed, waiting for the drive */
static uint32_t  frame_no, read_frame, backoff_until;
#ifdef Q1PSX_BENCH
int zone_sync;
#endif

#if Q1PSX_ENGINE_C
/* the C engine's (engine/zone.s's asm, followed) */
void zone_patch(qw_node *nd, const uint32_t *w, uint32_t n, uint8_t *p)
{
	for (; n; n--, nd++) {
		uint32_t pw = *w++;
		for (uint32_t skip = QZP_SKIP(pw); skip; skip--, nd++)
			nd->faces[0] = nd->faces[1] = (uint32_t) p;
		nd->faces[0] = (uint32_t) p;
		p += QZP_FRONT(pw);
		nd->faces[1] = (uint32_t) p;
		p += QZP_BACK(pw);
	}
}

static void zone_move(uint8_t *dst, const uint8_t *src, uint32_t bytes)
{
	uint32_t *d = (uint32_t *) dst;
	const uint32_t *s = (const uint32_t *) src;
	for (bytes >>= 2; bytes; bytes--)
		*d++ = *s++;
}
#else
void zone_move(uint8_t *dst, const uint8_t *src, uint32_t bytes);   /* zone.s: down, 16-byte steps */
#endif

/* ---------------------------------------------------------- the drive -- */

static int read_now(int32_t lba, uint32_t sectors, void *dst)
{
	return cd_read_now(lba, sectors, dst);
}

int zone_reading(void)
{
	return reading >= 0;
}

int zone_drive_wanted(void)
{
	return reading >= 0 || placed >= 0;
}

/* ---------------------------------------------------------- the arena -- */

static int slot_of(int z)
{
	for (int i = 0; i < MAX_RES; i++)
		if (slots[i].zone == z)
			return i;
	return -1;
}

static int is_wanted(int z, const uint8_t want[3], int upto)
{
	for (int i = 0; i < upto; i++)
		if (want[i] == z)
			return 1;
	return 0;
}

static void evict(int s)
{
	slots[s].zone  = -1;
	slots[s].ready = 0;
	zone_stats.evictions++;
}

/* the lowest gap of `need` bytes the chunks leave: 1, its place in *at */
static int gap(uint32_t need, uint32_t *at)
{
	uint32_t from = 0;
	for (;;) {
		uint32_t next = zh->arena_bytes;            /* the nearest chunk at or past `from` */
		int hit = -1;
		for (int i = 0; i < MAX_RES; i++)
			if (slots[i].zone >= 0 && slots[i].at + slots[i].size > from && slots[i].at < next)
				next = slots[i].at, hit = i;
		if (next >= from && next - from >= need) {
			*at = from;
			return 1;
		}
		if (hit < 0)
			return 0;
		from = slots[hit].at + slots[hit].size;
	}
}

/* the chunk drawn: the world nodes' blocks into it, its pool the renderer's */
static void make_current(int s)
{
	uint32_t t0 = prof_now();
	qw_chunk *c = (qw_chunk *) (arena + slots[s].at);
	zone_patch(world.nodes, (const uint32_t *) (c + 1), c->num_patch, (uint8_t *) c + c->ofs_records);
	r_state.vertpool = (uint32_t) ((uint8_t *) c + c->ofs_verts);
	cur_slot = s;
	cur_zone = slots[s].zone;
	uint32_t cyc = prof_cycles(t0, prof_now());
	zone_stats.switches++;
	zone_stats.switch_cycles += cyc;
	if (cyc > zone_stats.switch_max)
		zone_stats.switch_max = cyc;
}

/* the chunks slid down against each other (the one drawn too, its nodes
   patched again at its new place); no read in flight */
static void compact(void)
{
	uint32_t t0 = prof_now(), top = 0;
	for (;;) {
		int lo = -1;                              /* the lowest chunk at or past top */
		for (int i = 0; i < MAX_RES; i++)
			if (slots[i].zone >= 0 && slots[i].at >= top && (lo < 0 || slots[i].at < slots[lo].at))
				lo = i;
		if (lo < 0)
			break;
		if (slots[lo].at != top) {
			zone_move(arena + top, arena + slots[lo].at, slots[lo].size);
			zone_stats.compact_bytes += slots[lo].size;
			slots[lo].at = top;
			if (lo == cur_slot)
				make_current(lo);
		}
		top += slots[lo].size;
	}
	zone_stats.compactions++;
	zone_stats.compact_cycles += prof_cycles(t0, prof_now());
}

/* room for zone z's chunk, wanted at rank `rank` of want[] (0: the camera's
   zone): the chunks no set wants dropped, the biggest first, then those
   wanted less than it, then the rest compacted. Its slot (not ready), or -1
   (no room without dropping what is wanted more). No read in flight. */
static int place(int z, const uint8_t want[3], int rank)
{
	uint32_t need = (zdir[z].bytes + SECTOR - 1) / SECTOR * SECTOR, at;
	int e = -1;
	for (int i = 0; i < MAX_RES && e < 0; i++)
		if (slots[i].zone < 0)
			e = i;
	for (int step = 0; ; step++) {
		if (e >= 0 && gap(need, &at)) {
			slots[e].zone  = (int16_t) z;
			slots[e].ready = 0;
			slots[e].at    = at;
			slots[e].size  = need;
			return e;
		}
		int v = -1;                               /* a chunk to drop: none wanted first */
		for (int i = 0; i < MAX_RES; i++)
			if (slots[i].zone >= 0 && i != cur_slot && !is_wanted(slots[i].zone, want, 3) &&
			    (v < 0 || slots[i].size > slots[v].size))
				v = i;
		for (int r = 2; v < 0 && r > rank; r--)   /* then the least wanted, below z */
			if (want[r] != QZ_NONE && (v = slot_of(want[r])) == cur_slot)
				v = -1;
		if (v >= 0) {
			evict(v);
			if (e < 0)
				e = v;
			continue;
		}
		if (e < 0 || step > 2 * MAX_RES)
			return -1;
		uint32_t used = 0;
		for (int i = 0; i < MAX_RES; i++)
			if (slots[i].zone >= 0)
				used += slots[i].size;
		if (zh->arena_bytes - used < need)
			return -1;                            /* (the room is not there, in pieces or not) */
		compact();
		if (!gap(need, &at))
			return -1;
	}
}

static void read_done(int ok)
{
	slot_t *s = &slots[reading];
	const qw_chunk *c = (const qw_chunk *) (arena + s->at);
	if (ok && c->zone == (uint16_t) s->zone && c->stamp == zh->stamp) {
		s->ready = 1;
		zone_stats.loads++;
		zone_stats.load_bytes += zdir[s->zone].bytes;
		zone_stats.load_frames += frame_no - read_frame;
	} else {
		printf("zone: chunk %d unreadable\n", s->zone);
		s->zone = -1;
		zone_stats.read_errors++;
		backoff_until = frame_no + BACKOFF;
	}
	reading = -1;
}

/* a zone's chunk read at once into a place for it (the map's start, the
   bench): its slot, or -1. Any read in flight is waited for first. */
static int load_now(int z, const uint8_t want[3], int rank)
{
	if (reading >= 0)
		read_done(cd_read_sync() >= 0);
	cd_read_sync();                               /* (a stream's or the music's read: done,
	                                                 its owner sees it at its next poll) */
	if (placed >= 0 && slots[placed].zone != z)
		slots[placed].zone = -1;
	int s = placed >= 0 ? placed : place(z, want, rank);
	placed = -1;
	if (s < 0)
		return -1;
	if (!read_now(zon_lba + (int32_t) zdir[z].sector, slots[s].size / SECTOR, arena + slots[s].at)) {
		slots[s].zone = -1;
		zone_stats.read_errors++;
		return -1;
	}
	const qw_chunk *c = (const qw_chunk *) (arena + slots[s].at);
	if (c->zone != (uint16_t) z || c->stamp != zh->stamp) {
		printf("zone: chunk %d unreadable\n", z);
		slots[s].zone = -1;
		return -1;
	}
	slots[s].ready = 1;
	return s;
}

/* the wanted zones of a set, by rank: its own, then its two nearest */
static void wanted(uint32_t set, uint8_t want[3])
{
	const qw_zone_set *st = &zsets[set];
	want[0] = st->zone;
	want[1] = st->near[0];
	want[2] = st->near[1];
}

/* the next chunk to read, placed; its read started when the drive is free */
static void next_read(void)
{
	if (want_set >= zh->num_sets)
		return;
	uint8_t want[3];
	wanted(want_set, want);
	if (placed >= 0 && !is_wanted(slots[placed].zone, want, 3)) {
		slots[placed].zone = -1;                  /* (the camera went elsewhere first) */
		placed = -1;
	}
	for (int i = 0; i < 3 && placed < 0; i++) {
		if (want[i] == QZ_NONE || slot_of(want[i]) >= 0)
			continue;
		if (frame_no < backoff_until)
			return;
		placed = place(want[i], want, i);
		if (placed < 0)
			return;                               /* (no room for it: none for those after) */
	}
	if (placed < 0)
		return;
	uint32_t sectors = slots[placed].size / SECTOR;
	if (stream_reading() || !music_read_ok((int) sectors) || !cd_ready()) {
		zone_stats.waits++;                       /* (the stream's read first; the music's
		                                             ring full enough for this one; the last
		                                             read's Pause done: engine/cd.h) */
		return;
	}
	int s = placed;
	placed = -1;
	if (!cd_seek(zon_lba + (int32_t) zdir[slots[s].zone].sector) ||
	    !CdReadRetry((int) sectors, (uint32_t *) (arena + slots[s].at), CdlModeSpeed, 3)) {
		slots[s].zone = -1;
		zone_stats.read_errors++;
		backoff_until = frame_no + BACKOFF;
		return;
	}
	reading = s;
	read_frame = frame_no;
}

/* ---------------------------------------------------------- the map -- */

void zone_abort(void)
{
	if (reading >= 0) {
		CdReadBreak();                            /* (the drive paused, waited for, as */
		cd_read_sync();                           /*  stream_abort does)                */
		CdControlB(CdlPause, 0, 0);
		cd_busy();
	}
	reading = placed = -1;
	zh = 0;
	arena = 0;
	cur_slot = cur_zone = -1;
	want_set = SET_NONE;
	zone_rt.set = SET_NONE;
	for (int i = 0; i < MAX_RES; i++)
		slots[i].zone = -1;
}

int zone_on(void)
{
	return zh != 0;
}

int zone_map_start(const uint8_t *f, const char *map, int tex)
{
	zone_abort();
	const qw_zone_header *h = (const qw_zone_header *) (f + sizeof(qw_header));
	if (!h->num_zones || h->num_zones >= QZ_NONE || !h->num_sets || h->num_sets > QZ_NONE ||
	    !h->arena_bytes || (h->arena_bytes & (SECTOR - 1))) {
		printf("zone: %s: bad zone tables\n", map);
		return 0;
	}
	char name[24];
	uint32_t lba, size;
	snprintf(name, sizeof name, "\\MAPS\\%s_Z%d.ZON;1", map, tex);
	if (!cd_find(name, &lba, &size)) {
		printf("zone: no %s on the disc\n", name);
		return 0;
	}
	uint8_t *a = mem_level_alloc(h->arena_bytes);
	if (!a)
		return 0;
	const qz_file *fh = (const qz_file *) a;      /* (its header through the arena, empty yet) */
	if (!read_now((int32_t) lba, 1, a) || memcmp(fh->magic, QZONES_MAGIC, 4) || fh->stamp != h->stamp ||
	    fh->num_zones != h->num_zones) {
		printf("zone: %s is not this skeleton's (another cook?)\n", name);
		return 0;
	}
	zh      = h;
	zdir    = (const qw_chunk_dir *) (f + h->ofs_chunks);
	zsets   = (const qw_zone_set *) (f + h->ofs_sets);
	arena   = a;
	zon_lba = (int32_t) lba;
	skel_bytes = h->ofs_sets + h->num_sets * (uint32_t) sizeof(qw_zone_set);
	zone_rt.leaves   = (uint32_t) world.leaves & 0x00ffffffu;
	zone_rt.leaf_set = f + h->ofs_leaf_set;
	zone_rt.set      = SET_NONE;
	memset(&zone_stats, 0, sizeof zone_stats);
	frame_no = backoff_until = 0;
	printf("zone: %s %u zones, the skeleton %u KB, the chunk arena %u KB (the biggest chunk %u KB): "
	       "%u KB resident against the whole world's %u KB\n", name, h->num_zones, skel_bytes / 1024,
	       h->arena_bytes / 1024, h->max_chunk / 1024, (skel_bytes + h->arena_bytes) / 1024,
	       h->whole_bytes / 1024);
	return 1;
}

/* the leaf the renderer finds for a camera at `pos` (1/8 units): r_set_view's
   arithmetic, r_render's descent (r_world.s fl_loop, plane_dist: d <= 0 is
   the back child) */
static uint32_t leaf_at(const int32_t pos[3])
{
	int32_t c[3];
	for (int k = 0; k < 3; k++)
		c[k] = pos[k] * QW_VSCALE / R_POS_SUB;
	uint32_t at = (uint32_t) world.root;
	while (!(at & 1)) {
		const qw_node  *n  = (const qw_node *) at;
		const qw_plane *pl = (const qw_plane *) ((const int32_t *) world.planes + n->plane3);
		int32_t dist = pl->dist >> 1;
		int32_t d = pl->type < 3 ? c[pl->type] * 4096 - dist
		          : pl->n[0] * c[0] + pl->n[1] * c[1] + pl->n[2] * c[2] - dist;
		at = n->child[d <= 0];
	}
	return (((at & ~1u) & 0x00ffffffu) - ((uint32_t) world.leaves & 0x00ffffffu)) / sizeof(qw_leaf);
}

void zone_preload(const int32_t eye[3])
{
	if (!zh)
		return;
	uint32_t set = zone_rt.leaf_set[leaf_at(eye)];
	if (set >= zh->num_sets)
		set = 0;                                  /* (the eye in solid) */
	uint8_t want[3];
	wanted(set, want);
	for (int i = 0; i < 3; i++)
		if (want[i] != QZ_NONE && slot_of(want[i]) < 0)
			load_now(want[i], want, i);
	int s = slot_of(want[0]);
	if (s >= 0 && slots[s].ready)
		make_current(s);
	want_set = set;                               /* (the renderer's first call checks the
	                                                 camera's own leaf) */
	memset(&zone_stats, 0, sizeof zone_stats);
}

/* ------------------------------------------------------- the frame -- */

void zone_enter(uint32_t set)
{
	zone_rt.set = set;
	if (!zh || set >= zh->num_sets)
		return;                                   /* (QZ_NONE: the camera in solid) */
	want_set = set;
	int z = zsets[set].zone;
	if (z == cur_zone)
		return;
	int s = slot_of(z);
	if (s >= 0 && slots[s].ready) {
		make_current(s);
		return;
	}
#ifdef Q1PSX_BENCH
	if (zone_sync) {
		uint8_t want[3];
		wanted(set, want);
		if ((s = load_now(z, want, 0)) >= 0) {
			zone_stats.sync_loads++;
			make_current(s);
			return;
		}
	}
#endif
	zone_stats.late_events++;
}

void zone_frame(void)
{
	if (!zh)
		return;
	frame_no++;
	if (reading >= 0) {
		int left = cd_poll();                     /* (also runs CdReadRetry's retries) */
		if (left > 0)
			return;
		read_done(left == 0);
	}
	if (want_set < zh->num_sets && zsets[want_set].zone != cur_zone) {
		zone_stats.late_frames++;                 /* (the frame just drawn) */
		int s = slot_of(zsets[want_set].zone);
		if (s >= 0 && slots[s].ready)
			make_current(s);
	}
	next_read();
}

/* ------------------------------------------------------- the bench -- */

#ifdef Q1PSX_BENCH
uint32_t zone_bench_leaf(const int32_t pos[3])
{
	return world.root ? leaf_at(pos) : 0;
}

void zone_bench_frame(const int32_t pos[3])
{
	if (!zh || !zone_sync)
		return;
	uint32_t set = zone_rt.leaf_set[leaf_at(pos)];
	if (set >= zh->num_sets)
		return;
	uint8_t want[3];
	wanted(set, want);
	int s = slot_of(want[0]);
	if (s < 0 || !slots[s].ready) {
		if ((s = load_now(want[0], want, 0)) < 0)
			return;
		zone_stats.sync_loads++;
	}
	if (zone_rt.set == set && cur_zone != want[0])   /* (the renderer calls zone_enter only
	                                                    for another set) */
		make_current(s);
}

int zone_of_leaf(uint32_t leaf)
{
	if (!zh || leaf >= world.num_leaves)
		return QZ_NONE;
	uint32_t set = zone_rt.leaf_set[leaf];
	return set < zh->num_sets ? zsets[set].zone : (int) QZ_NONE;
}

int zone_current(void)            { return cur_zone; }
int zone_count(void)              { return zh ? (int) zh->num_zones : 0; }
uint32_t zone_arena_bytes(void)   { return zh ? zh->arena_bytes : 0; }
uint32_t zone_skeleton_bytes(void) { return zh ? skel_bytes : 0; }

int zone_check_patch(void)
{
	if (cur_slot < 0)
		return 0;
	const qw_chunk *c = (const qw_chunk *) (arena + slots[cur_slot].at);
	const uint32_t *w = (const uint32_t *) (c + 1);
	uint32_t p = (uint32_t) c + c->ofs_records;
	const qw_node *nd = world.nodes;
	int bad = 0;
	for (uint32_t n = c->num_patch; n; n--, nd++) {
		uint32_t pw = *w++;
		for (uint32_t skip = QZP_SKIP(pw); skip; skip--, nd++)
			if (nd->faces[0] != p || nd->faces[1] != p)
				bad++;
		bad += nd->faces[0] != p;
		p += QZP_FRONT(pw);
		bad += nd->faces[1] != p;
		p += QZP_BACK(pw);
	}
	if (nd - world.nodes != (int) zh->world_nodes + 1 || p != (uint32_t) c + c->ofs_records + c->len_records)
		bad++;
	return bad;
}
#endif
