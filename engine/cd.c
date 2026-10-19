#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <psxcd.h>

#include "cd.h"
#include "mem.h"

#define SECTOR 2048

/* The disc's directories: the root and the directories in it, found at the
   first lookup (after music_init: the drive has answered by then). A
   lookup reads its directory's records into the level arena's top, mark
   and release, never into the SDK's heap: psxcd's CdSearchFile and
   CdOpenDir malloc a whole directory from it, and the heap is what the
   executable leaves under the stack (21 KB once the monsters were in),
   less than the 24 KB of \MAPS on a disc with every map (386 files): the
   read went nowhere and the boot found no map. */
#define MAX_DIRS 8

typedef struct {
	char     name[12];       /* "" for the root                              */
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

static int read_sectors(uint32_t lba, uint32_t n, void *dst)
{
	CdlLOC loc;
	CdIntToPos((int) lba, &loc);
	if (!CdControl(CdlSetloc, &loc, 0) || !CdReadRetry((int) n, (uint32_t *) dst, CdlModeSpeed, 3))
		return 0;
	return CdReadSync(0, 0) >= 0;
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

static int note_dir(const char *name, int nlen, uint32_t lba, uint32_t size, int dir, void *arg)
{
	(void) arg;
	if (!dir || num_dirs >= MAX_DIRS || nlen >= (int) sizeof dirs[0].name)
		return 0;
	memcpy(dirs[num_dirs].name, name, (size_t) nlen);
	dirs[num_dirs].name[nlen] = 0;
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
	dir_walk(buf, dirs[0].len, note_dir, 0);
	mem_level_release(mark);
	return 1;
}

/* a directory by its path ("\\MAPS"; "" or "\\" the root), NULL if none */
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
	return NULL;
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
	const cd_dir *d = find_dir(name, file > name ? (int) (file - name - 1) : 0);
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

	CdlLOC loc;
	CdIntToPos((int) lba, &loc);
	CdControl(CdlSetloc, &loc, NULL);
	int left = -1;
	if (CdReadRetry((int) sectors, buf, CdlModeSpeed, 3)) {
		if (poll) {
			/* CdReadSync(1) returns the sectors still to come, and must be
			   called regularly for CdReadRetry to retry a failed read */
			while ((left = CdReadSync(1, NULL)) > 0)
				poll((sectors - (uint32_t) left) * SECTOR);
		} else {
			left = CdReadSync(0, NULL);
		}
	}
	if (left < 0) {
		printf("cd: read error on %s\n", name);
		return NULL;
	}

	*size = bytes;
	return buf;
}
