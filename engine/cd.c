#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <psxapi.h>
#include <psxcd.h>
#include <psxgpu.h>
#include <hwregs_c.h>

#include "cd.h"
#include "mem.h"
#include "prof.h"

#define SECTOR CD_SECTOR

/* The disc's directories: the root and the directories in it, found at the
   first lookup (after music_init: the drive has answered by then), and a
   directory's directories when a path asks for one (the levels' under
   \LVL, docs/porting-notes.md §84: the root's records must fit its first
   sector, the only one the BIOS reads, SYSTEM.CNF and the executable in
   it). A lookup reads its directory's records into the level arena's top,
   mark and release, never into the SDK's heap: psxcd's CdSearchFile and
   CdOpenDir malloc a whole directory from it, and the heap is what the
   executable leaves under the stack (21 KB once the monsters were in),
   less than the 24 KB of a directory of every map's files (386): the read
   went nowhere and the boot found no map. */
#define MAX_DIRS 40                /* the root, LVL and its 32 levels, the music's */

typedef struct {
	char     name[16];       /* its path without the first backslash ("LVL\E1M1"); "" the root */
	uint32_t lba, len;       /* its records: first sector, bytes             */
} cd_dir;

static cd_dir dirs[MAX_DIRS];
static int    num_dirs;

/* a directory record's both-endian field, its little-endian half (records
   are only 2-aligned) */
static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

/* (engine/cd.h: the drive's commands and its interrupt) */
#define CD_SETTLE 2                       /* vblanks idle before a command */
static int32_t busy_vbl;                  /* the last vblank the drive was busy */

void cd_busy(void)
{
	busy_vbl = VSync(-1);
}

/* ------------------------------------------ the bench's journal, the model -- */

#ifdef Q1PSX_BENCH
/* (engine/cd.h; docs/porting-notes.md §83) */
int cd_log   = Q1PSX_BENCH_CDLOG;
int cd_model = Q1PSX_BENCH_CDMODEL;

/* the read in flight */
static struct {
	int      active;
	uint8_t  who;
	int8_t   cut;              /* broken off (cd_read_cut)                      */
	int32_t  tag, lba, from;
	uint32_t n;
	uint32_t start_vbl;        /* VSync(-1) at its command                      */
	uint32_t seek_us;          /* the seek the model gives it                   */
	uint32_t model_t;          /* prof_now() the modeled drive ends it at        */
	uint32_t held;             /* the polls answered "reading" past its real end */
} rd;
static int32_t head;           /* the head's place: the last read's end         */

/* the phase's totals (cd_log_mark) */
static struct {
	uint32_t reads[CDW_N], sectors[CDW_N];
	uint32_t seek_us, held, hops[3], hop_max, vbl0;
} sum;
static char mark_name[16] = "boot";
static const char who_letter[CDW_N] = { 'D', 'L', 'Z', 'M', 'S', 'U' };

/* 256 log2(x), x >= 1: the exponent, the mantissa through a 16-step table */
static uint32_t log2_q8(uint32_t x)
{
	static const uint16_t t[17] = { 0, 22, 44, 63, 82, 100, 118, 134, 150, 165, 179, 193, 207, 220, 232, 244, 256 };
	int k = 31 - __builtin_clz(x);
	uint32_t f = (k >= 12 ? x >> (k - 12) : x << (12 - k)) - 4096;   /* x / 2^k in Q12, less 1 */
	uint32_t i = f >> 8, r = f & 255;
	return (uint32_t) k * 256 + t[i] + ((t[i + 1] - t[i]) * r >> 8);
}

/* the sectors a track holds at lba (the mechanism's table, by minute) */
static uint32_t sectors_per_track(int32_t lba)
{
	uint32_t mm = (uint32_t) lba / 4500;
	return mm == 0 ? 8 : mm <= 4 ? 9 : mm <= 7 ? 10 : mm <= 11 ? 11 : mm <= 16 ? 12 : mm <= 23 ? 13 : mm <= 27 ? 14
	     : mm <= 32 ? 15 : mm <= 39 ? 16 : mm <= 44 ? 17 : mm <= 52 ? 18 : mm <= 60 ? 19 : mm <= 67 ? 20
	     : mm <= 74 ? 21 : 22;
}

/* A seek's time on the console's drive, from the command to the first
   sector, in microseconds: DuckStation's model (src/core/cdrom.cpp
   GetTicksForSeek, from measures of the mechanism), at double speed. A
   target just ahead within a track is read up to (the sectors' time); one
   just behind, a track jump back and the sectors up to it; under 7200
   sectors away, 50 ms (very near: under 60 to 330 sectors, by the place on
   the disc) or 100 ms; farther, the sled: 50 ms plus 340 ms x ln d / ln
   324000 plus 510 ms x d / 324000 (200 ms at 7200 sectors, 900 ms across a
   74-minute disc). */
#define US_SECTOR 6667                        /* a sector at double speed */
static uint32_t seek_us(int32_t from, int32_t to)
{
	uint32_t d = to > from ? (uint32_t) (to - from) : (uint32_t) (from - to);
	int32_t spt = (int32_t) sectors_per_track(from), tjump = from >= spt ? from - spt : 0;
	if (from < to && d <= (uint32_t) spt)
		return US_SECTOR * (d < 2 ? 2 : d);
	if (from >= to && tjump <= to)
		return US_SECTOR * (to - tjump < 1 ? 1 : (uint32_t) (to - tjump));
	if (d < 7200) {
		uint32_t at = (uint32_t) from < 4500 ? 4500 : (uint32_t) from > 324000 ? 324000 : (uint32_t) from;
		uint32_t sw = 330 - (log2_q8(at) - log2_q8(4500)) * 43761 / 256000;   /* 330 - 63.1333 ln (minutes) */
		return d < sw ? 50000 : 100000;
	}
	return 50000 + 340000 * log2_q8(d) / 4686 + d * 85 / 54;   /* (ln 324000 = 18.3056 log2 units) */
}

static uint32_t us_ticks(uint32_t us)         /* prof_now's 4.2336 MHz */
{
	return us * 4 + us / 100 * 2336 / 100;
}

static int model_busy(void)                   /* the modeled drive still on the read in flight */
{
	return rd.active && cd_model && (int32_t) (prof_now() - rd.model_t) < 0;
}

static void note_start(int32_t lba, uint32_t n, int who, int tag)
{
	rd.active    = 1;
	rd.who       = (uint8_t) who;
	rd.cut       = 0;
	rd.tag       = tag;
	rd.lba       = lba;
	rd.n         = n;
	rd.from      = head;
	rd.start_vbl = (uint32_t) VSync(-1);
	rd.seek_us   = seek_us(head, lba);
	rd.model_t   = prof_now() + us_ticks(rd.seek_us) + n * 28224;   /* (a sector: 28224 ticks) */
	rd.held      = 0;
}

/* the read in flight is over: its line, the phase's totals, the head moved */
static void end_read(int ok)
{
	uint32_t d = rd.lba > rd.from ? (uint32_t) (rd.lba - rd.from) : (uint32_t) (rd.from - rd.lba);
	head = rd.lba + (int32_t) rd.n;
	sum.reads[rd.who]++;
	sum.sectors[rd.who] += rd.n;
	sum.seek_us += rd.seek_us;
	sum.held    += rd.held;
	sum.hops[d < 330 ? 0 : d < 7200 ? 1 : 2]++;
	if (d > sum.hop_max)
		sum.hop_max = d;
	if (cd_log)
		printf("cd %c %d %d %u %d %u %u %u %u %c\n", who_letter[rd.who], (int) rd.tag, (int) rd.lba, (unsigned) rd.n,
		       (int) rd.from, (unsigned) rd.start_vbl, (unsigned) VSync(-1), (unsigned) rd.seek_us,
		       (unsigned) rd.held, rd.cut ? 'x' : ok ? '.' : 'e');
	rd.active = 0;
}

/* the real read ended with `left` (0: done; < 0: an error): held while the
   modeled drive would still be on it (1: "still reading"), then logged */
static int read_over(int left)
{
	if (left == 0 && model_busy()) {
		rd.held++;
		cd_busy();
		return 1;
	}
	end_read(left >= 0);
	return left;
}

void cd_log_mark(const char *name)
{
	if (cd_log || cd_model) {
		uint32_t n = 0, sec = 0;
		for (int w = 0; w < CDW_N; w++) {
			n   += sum.reads[w];
			sec += sum.sectors[w];
		}
		printf("cd sum %s: %u reads, %u sectors, %u vblanks; D %u/%u L %u/%u Z %u/%u M %u/%u S %u/%u U %u/%u; "
		       "seeks %u ms, transfer %u ms, held %u polls; hops %u near, %u mid, %u sled, the longest %u\n",
		       mark_name, (unsigned) n, (unsigned) sec, (unsigned) ((uint32_t) VSync(-1) - sum.vbl0),
		       (unsigned) sum.reads[CDW_DIR], (unsigned) sum.sectors[CDW_DIR], (unsigned) sum.reads[CDW_FILE],
		       (unsigned) sum.sectors[CDW_FILE], (unsigned) sum.reads[CDW_ZONE], (unsigned) sum.sectors[CDW_ZONE],
		       (unsigned) sum.reads[CDW_MODEL], (unsigned) sum.sectors[CDW_MODEL], (unsigned) sum.reads[CDW_SOUND],
		       (unsigned) sum.sectors[CDW_SOUND], (unsigned) sum.reads[CDW_MUSIC], (unsigned) sum.sectors[CDW_MUSIC],
		       (unsigned) (sum.seek_us / 1000), (unsigned) (sec * US_SECTOR / 1000), (unsigned) sum.held,
		       (unsigned) sum.hops[0], (unsigned) sum.hops[1], (unsigned) sum.hops[2], (unsigned) sum.hop_max);
		if (cd_log)
			printf("cd # %s\n", name);
	}
	memset(&sum, 0, sizeof sum);
	sum.vbl0 = (uint32_t) VSync(-1);
	strncpy(mark_name, name, sizeof mark_name - 1);
}
#else
#define model_busy() 0
#endif

/* ------------------------------------------------------------ the drive -- */

int cd_ready(void)
{
	if (CdReadSync(1, 0) > 0 || CdSync(1, 0) == CdlNoIntr) {   /* (a read, a Pause in progress) */
		cd_busy();
		return 0;
	}
	if (model_busy())                         /* (the bench's drive model: still reading) */
		return 0;
	return VSync(-1) - busy_vbl >= CD_SETTLE;
}

int cd_poll(void)
{
	int left = CdReadSync(1, 0);              /* (also runs CdReadRetry's retries) */
	if (left > 0 || CdSync(1, 0) == CdlNoIntr)
		cd_busy();
#ifdef Q1PSX_BENCH
	if (rd.active && left <= 0)
		left = read_over(left);
#endif
	return left;
}

int cd_read_sync(void)
{
	int r = CdReadSync(0, 0);
	cd_busy();                                /* (it just ended: settling from now) */
#ifdef Q1PSX_BENCH
	if (rd.active) {
		if (r >= 0)
			while (model_busy())              /* (the modeled drive still on it) */
				;
		cd_busy();
		end_read(r >= 0);
	}
#endif
	return r;
}

void cd_read_cut(void)
{
#ifdef Q1PSX_BENCH
	rd.cut = 1;
#endif
	CdReadBreak();
}

/* the drive's interrupts back as CdInit leaves them: every pending one
   acknowledged, all enabled, bank 0 */
static void cd_repair(void)
{
	FastEnterCriticalSection();
	CD_REG(0) = 1;
	CD_REG(3) = 0x1f;
	CD_REG(2) = 0x1f;
	CD_REG(0) = 0;
	FastExitCriticalSection();
}

int cd_seek(int32_t lba)
{
	CdlLOC loc;
	CdIntToPos(lba, &loc);
	for (int tries = 0; tries < 4; tries++) {
		CdSync(0, 0);                         /* (a blocking command done: a read's Pause) */
		while (!cd_ready())                   /* (settled: its stragglers in) */
			;
		int ok = CdControl(CdlSetloc, &loc, 0);
		cd_busy();
		if (ok && CdSync(1, 0) != CdlDiskError)
			return 1;                         /* (acknowledged, not refused) */
		printf("cd: the drive refused a seek to %d: its interrupts enabled again\n", (int) lba);
		cd_repair();
	}
	return 0;
}

int cd_read_start(int32_t lba, uint32_t sectors, void *dst, int who, int tag)
{
	(void) who;
	(void) tag;
	if (!cd_seek(lba) || !CdReadRetry((int) sectors, (uint32_t *) dst, CdlModeSpeed, 3))
		return 0;
#ifdef Q1PSX_BENCH
	if (rd.active)                            /* (an owner's read ended, not polled yet) */
		end_read(1);
	note_start(lba, sectors, who, tag);
#endif
	return 1;
}

int cd_read_now(int32_t lba, uint32_t sectors, void *dst, int who, int tag)
{
	if (!cd_read_start(lba, sectors, dst, who, tag))
		return 0;
	return cd_read_sync() >= 0;
}

static int read_sectors(uint32_t lba, uint32_t n, void *dst)
{
	return cd_read_now((int32_t) lba, n, dst, CDW_DIR, 0);
}

/* a directory's records into the level arena (the caller releases *mark),
   NULL if there is no room or the read fails */
static const uint8_t *dir_read(const cd_dir *d, uint32_t *mark)
{
	*mark = mem_level_mark();
	uint32_t n = (d->len + SECTOR - 1) / SECTOR;
	uint8_t *buf = mem_level_alloc(n * SECTOR);
	if (!buf || !read_sectors(d->lba, n, buf)) {
		mem_level_release(*mark);
		return NULL;
	}
	return buf;
}

/* each record of a directory but "." and "..": fn(name, its length, first
   sector, bytes, is a directory, arg), until fn returns nonzero */
typedef int (*rec_fn)(const char *name, int nlen, uint32_t lba, uint32_t size, int dir, void *arg);

static int dir_walk(const uint8_t *buf, uint32_t len, rec_fn fn, void *arg)
{
	for (uint32_t s = 0; s < len; s += SECTOR)
		for (uint32_t o = 0; o < SECTOR; ) {
			const uint8_t *r = buf + s + o;
			uint32_t rl = r[0];
			if (rl < 34 || o + rl > SECTOR)
				break;                       /* (0: the rest of the sector is padding) */
			int nl = r[32];
			if (33u + nl <= rl && !(nl == 1 && r[33] <= 1) &&
			    fn((const char *) r + 33, nl, le32(r + 2), le32(r + 10), r[25] & 2, arg))
				return 1;
			o += rl;
		}
	return 0;
}

static int up(int c)
{
	return c >= 'a' && c <= 'z' ? c - 32 : c;
}

/* a record's name against a wanted one, case aside, the versions (";1")
   left out */
static int name_eq(const char *rec, int nlen, const char *want)
{
	int i = 0;
	for (; i < nlen && rec[i] != ';'; i++)
		if (up(rec[i]) != up(want[i]))
			return 0;
	return want[i] == 0 || want[i] == ';';
}

/* a directory's record noted under its path: arg is the parent's path
   ("" the root's), the new one "<parent>\<name>" */
static int note_dir(const char *name, int nlen, uint32_t lba, uint32_t size, int dir, void *arg)
{
	const char *parent = arg;
	int plen = (int) strlen(parent);
	if (!dir || num_dirs >= MAX_DIRS || plen + (plen > 0) + nlen >= (int) sizeof dirs[0].name)
		return 0;
	char *s = dirs[num_dirs].name;
	memcpy(s, parent, (size_t) plen);
	if (plen)
		s[plen++] = '\\';
	memcpy(s + plen, name, (size_t) nlen);
	s[plen + nlen] = 0;
	dirs[num_dirs].lba = lba;
	dirs[num_dirs].len = size;
	num_dirs++;
	return 0;
}

/* the volume descriptor's root, then the directories in it */
static int scan(void)
{
	if (num_dirs)
		return 1;
	uint32_t mark = mem_level_mark();
	uint8_t *pvd = mem_level_alloc(SECTOR);
	if (!pvd || !read_sectors(16, 1, pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) {
		printf("cd: no ISO9660 volume on the disc\n");
		mem_level_release(mark);
		return 0;
	}
	dirs[0].name[0] = 0;
	dirs[0].lba = le32(pvd + 156 + 2);         /* the root's directory record */
	dirs[0].len = le32(pvd + 156 + 10);
	mem_level_release(mark);
	num_dirs = 1;
	const uint8_t *buf = dir_read(&dirs[0], &mark);
	if (!buf) {
		num_dirs = 0;
		return 0;
	}
	dir_walk(buf, dirs[0].len, note_dir, "");
	mem_level_release(mark);
	return 1;
}

/* a directory by its path ("\\LVL\\E1M1"; "" or "\\" the root), NULL if
   none: the root's are known from the scan, a deeper one is looked up in
   its parent's records (read once) and noted */
static const cd_dir *find_dir(const char *path, int len)
{
	if (len && *path == '\\')
		path++, len--;
	if (!scan())
		return NULL;
	for (int i = 0; i < num_dirs; i++) {
		int k = 0;
		while (k < len && dirs[i].name[k] && up(dirs[i].name[k]) == up(path[k]))
			k++;
		if (k == len && !dirs[i].name[k])
			return &dirs[i];
	}
	int cut = len;
	while (cut > 0 && path[cut - 1] != '\\')
		cut--;
	if (!cut)
		return NULL;                              /* (a root's directory the scan did not list) */
	const cd_dir *parent = find_dir(path, cut - 1);
	uint32_t mark;
	const uint8_t *buf = parent ? dir_read(parent, &mark) : NULL;
	if (!buf)
		return NULL;
	int before = num_dirs;
	char pname[sizeof parent->name];
	strcpy(pname, parent->name);              /* (note_dir may add to dirs[]: the parent's name copied) */
	dir_walk(buf, parent->len, note_dir, pname);
	mem_level_release(mark);
	for (int i = before; i < num_dirs; i++) {
		int k = 0;
		while (k < len && dirs[i].name[k] && up(dirs[i].name[k]) == up(path[k]))
			k++;
		if (k == len && !dirs[i].name[k])
			return &dirs[i];
	}
	return NULL;
}

/* The level's directory kept (cd_cache_dir): its files by name, the
   lookups of a load answered without a read each (seven directory reads
   a load before, each a seek: docs/porting-notes.md §83.6). */
#define CACHE_MAX 12
static struct {
	char dir[16];              /* its path without the first backslash; "" none */
	int  n;                    /* its files (CACHE_MAX: too many, not kept) */
	struct {
		char     name[14];
		uint32_t lba, size;
	} e[CACHE_MAX];
} cache;

static int cache_file(const char *name, int nlen, uint32_t lba, uint32_t size, int dir, void *arg)
{
	(void) arg;
	if (dir)
		return 0;
	if (cache.n >= CACHE_MAX || nlen >= (int) sizeof cache.e[0].name) {
		cache.n = CACHE_MAX;                  /* (not all of them: not trusted) */
		return 1;
	}
	memcpy(cache.e[cache.n].name, name, (size_t) nlen);
	cache.e[cache.n].name[nlen] = 0;
	cache.e[cache.n].lba  = lba;
	cache.e[cache.n].size = size;
	cache.n++;
	return 0;
}

int cd_cache_dir(const char *path)
{
	cache.n = 0;
	cache.dir[0] = 0;
	const char *p = *path == '\\' ? path + 1 : path;
	if (strlen(p) >= sizeof cache.dir)
		return 0;
	const cd_dir *d = find_dir(path, (int) strlen(path));
	uint32_t mark;
	const uint8_t *buf = d ? dir_read(d, &mark) : NULL;
	if (!buf)
		return 0;
	dir_walk(buf, d->len, cache_file, 0);
	mem_level_release(mark);
	if (cache.n >= CACHE_MAX) {
		cache.n = 0;
		return 0;
	}
	strcpy(cache.dir, p);
	return cache.n;
}

/* the cached directory holds the path's: its file's place, or none there
   (1 answered, 0 not this directory) */
static int cache_find(const char *dir, int len, const char *file, uint32_t *lba, uint32_t *size)
{
	if (!cache.dir[0])
		return 0;
	int k = 0;
	while (k < len && cache.dir[k] && up(cache.dir[k]) == up(dir[k]))
		k++;
	if (k != len || cache.dir[k])
		return 0;
	for (int i = 0; i < cache.n; i++)
		if (name_eq(cache.e[i].name, (int) strlen(cache.e[i].name), file)) {
			if (lba)
				*lba = cache.e[i].lba;
			if (size)
				*size = cache.e[i].size;
			return 1;
		}
	if (lba)
		*lba = 0;
	if (size)
		*size = 0;
	return 1;
}

typedef struct {
	const char *want;
	uint32_t    lba, size;
	int         found;
} find_t;

static int find_file(const char *name, int nlen, uint32_t lba, uint32_t size, int dir, void *arg)
{
	find_t *f = arg;
	if (dir || !name_eq(name, nlen, f->want))
		return 0;
	f->lba   = lba;
	f->size  = size;
	f->found = 1;
	return 1;
}

int cd_find(const char *name, uint32_t *lba, uint32_t *size)
{
	const char *file = strrchr(name, '\\');
	file = file ? file + 1 : name;
	int dlen = file > name ? (int) (file - name - 1) : 0;
	const char *dir = *name == '\\' ? name + 1 : name;
	uint32_t at = 0;
	if (dlen && cache_find(dir, dlen - (dir - name), file, &at, size)) {
		if (lba)                              /* (the level's directory, read at its load) */
			*lba = at;
		return at != 0;
	}
	const cd_dir *d = find_dir(name, dlen);
	uint32_t mark;
	const uint8_t *buf = d ? dir_read(d, &mark) : NULL;
	if (!buf)
		return 0;
	find_t f = { file, 0, 0, 0 };
	dir_walk(buf, d->len, find_file, &f);
	mem_level_release(mark);
	if (f.found) {
		if (lba)
			*lba = f.lba;
		if (size)
			*size = f.size;
	}
	return f.found;
}

typedef struct {
	cd_list_fn fn;
	int        n;
} list_t;

static int list_file(const char *name, int nlen, uint32_t lba, uint32_t size, int dir, void *arg)
{
	list_t *l = arg;
	if (dir)
		return 0;
	char s[40];
	if (nlen > (int) sizeof s - 1)
		nlen = (int) sizeof s - 1;
	memcpy(s, name, (size_t) nlen);
	s[nlen] = 0;
	l->fn(s, lba, size);
	l->n++;
	return 0;
}

typedef struct {
	cd_dir_fn fn;
	int       n;
} dlist_t;

static int list_subdir(const char *name, int nlen, uint32_t lba, uint32_t size, int dir, void *arg)
{
	(void) lba;
	(void) size;
	dlist_t *l = arg;
	if (!dir)
		return 0;
	char s[16];
	if (nlen > (int) sizeof s - 1)
		nlen = (int) sizeof s - 1;
	memcpy(s, name, (size_t) nlen);
	s[nlen] = 0;
	l->fn(s);
	l->n++;
	return 0;
}

int cd_list_dirs(const char *path, cd_dir_fn fn)
{
	const cd_dir *d = find_dir(path, (int) strlen(path));
	uint32_t mark;
	const uint8_t *buf = d ? dir_read(d, &mark) : NULL;
	if (!buf)
		return 0;
	dlist_t l = { fn, 0 };
	dir_walk(buf, d->len, list_subdir, &l);
	mem_level_release(mark);
	return l.n;
}

int cd_list_dir(const char *path, cd_list_fn fn)
{
	const cd_dir *d = find_dir(path, (int) strlen(path));
	uint32_t mark;
	const uint8_t *buf = d ? dir_read(d, &mark) : NULL;
	if (!buf)
		return 0;
	list_t l = { fn, 0 };
	dir_walk(buf, d->len, list_file, &l);
	mem_level_release(mark);
	return l.n;
}

void cd_init(void)
{
	CdInit();
}

int cd_exists(const char *name, uint32_t *size)
{
	return cd_find(name, NULL, size);
}

void *cd_load(const char *name, uint32_t *size, cd_poll_fn poll)
{
	uint32_t lba, bytes;
	if (!cd_find(name, &lba, &bytes)) {
		printf("cd: %s not found\n", name);
		return NULL;
	}

	uint32_t sectors = (bytes + SECTOR - 1) / SECTOR;
	uint32_t *buf = mem_level_alloc(sectors * SECTOR);
	if (!buf)
		return NULL;

	int left = -1;
	if (cd_read_start((int32_t) lba, sectors, buf, CDW_FILE, 0)) {
		if (poll) {
			/* CdReadSync(1) returns the sectors still to come, and must be
			   called regularly for CdReadRetry to retry a failed read */
			while ((left = cd_poll()) > 0)
				poll((sectors - (uint32_t) left) * SECTOR);
		}
		int done = cd_read_sync();           /* (the Pause after it too) */
		if (!poll)
			left = done;
	}
	if (left < 0) {
		printf("cd: read error on %s\n", name);
		return NULL;
	}

	*size = bytes;
	return buf;
}
