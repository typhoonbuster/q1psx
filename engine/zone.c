/*
 * q1psx — memory allocation
 *
 * Ported from id-Software/Quake WinQuake/zone.c (GPLv2, (C) 1996-1997 Id
 * Software). Zone and Hunk logic kept as close to the original as possible;
 * the Cache_??? subsystem and the "-zone" command line option are omitted
 * (see zone.h / docs/porting-notes.md §3).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "zone.h"

typedef uint8_t byte;

#define DYNAMIC_SIZE  0xc000        /* 48 KiB zone, as in stock Quake */

#define ZONEID        0x1d4a11
#define MINFRAGMENT   64

typedef struct memblock_s {
	int              size;         /* including the header and possibly tiny fragments */
	int              tag;          /* a tag of 0 is a free block */
	int              id;           /* should be ZONEID */
	struct memblock_s *next, *prev;
	int              pad;          /* pad to 64 bit boundary */
} memblock_t;

typedef struct {
	int        size;               /* total bytes malloced, including header */
	memblock_t blocklist;          /* start / end cap for linked list */
	memblock_t *rover;
} memzone_t;

/*
==============================================================================
                        ZONE MEMORY ALLOCATION

There is never any space between memblocks, and there will never be two
contiguous free memblocks. The rover can be left pointing at a non-empty block.
The zone calls are pretty much only used for small strings and structures, all
big things are allocated on the hunk.
==============================================================================
*/

static memzone_t *mainzone;

static void Z_ClearZone(memzone_t *zone, int size)
{
	memblock_t *block;

	/* set the entire zone to one free block */
	zone->blocklist.next = zone->blocklist.prev = block =
		(memblock_t *) ((byte *) zone + sizeof(memzone_t));
	zone->blocklist.tag  = 1;      /* in use block */
	zone->blocklist.id   = 0;
	zone->blocklist.size = 0;
	zone->rover = block;

	block->prev = block->next = &zone->blocklist;
	block->tag  = 0;              /* free block */
	block->id   = ZONEID;
	block->size = size - sizeof(memzone_t);
}

void Z_Free(void *ptr)
{
	memblock_t *block, *other;

	if (!ptr)
		Sys_Error("Z_Free: NULL pointer");

	block = (memblock_t *) ((byte *) ptr - sizeof(memblock_t));
	if (block->id != ZONEID)
		Sys_Error("Z_Free: freed a pointer without ZONEID");
	if (block->tag == 0)
		Sys_Error("Z_Free: freed a freed pointer");

	block->tag = 0;               /* mark as free */

	other = block->prev;
	if (!other->tag) {            /* merge with previous free block */
		other->size += block->size;
		other->next = block->next;
		other->next->prev = other;
		if (block == mainzone->rover)
			mainzone->rover = other;
		block = other;
	}

	other = block->next;
	if (!other->tag) {            /* merge the next free block onto the end */
		block->size += other->size;
		block->next = other->next;
		block->next->prev = block;
		if (other == mainzone->rover)
			mainzone->rover = block;
	}
}

void *Z_TagMalloc(int size, int tag)
{
	int        extra;
	memblock_t *start, *rover, *new, *base;

	if (!tag)
		Sys_Error("Z_TagMalloc: tried to use a 0 tag");

	/* scan through the block list looking for the first free block of
	   sufficient size */
	size += sizeof(memblock_t);   /* account for size of block header */
	size += 4;                    /* space for memory trash tester */
	size = (size + 7) & ~7;       /* align to 8-byte boundary */

	base = rover = mainzone->rover;
	start = base->prev;

	do {
		if (rover == start)      /* scaned all the way around the list */
			return NULL;
		if (rover->tag)
			base = rover = rover->next;
		else
			rover = rover->next;
	} while (base->tag || base->size < size);

	/* found a block big enough */
	extra = base->size - size;
	if (extra > MINFRAGMENT) {
		/* there will be a free fragment after the allocated block */
		new = (memblock_t *) ((byte *) base + size);
		new->size = extra;
		new->tag  = 0;           /* free block */
		new->prev = base;
		new->id   = ZONEID;
		new->next = base->next;
		new->next->prev = new;
		base->next = new;
		base->size = size;
	}

	base->tag = tag;              /* no longer a free block */
	mainzone->rover = base->next; /* next allocation will start looking here */
	base->id = ZONEID;

	/* marker for memory trash testing */
	*(int *) ((byte *) base + base->size - 4) = ZONEID;

	return (void *) ((byte *) base + sizeof(memblock_t));
}

void *Z_Malloc(int size)
{
	void *buf;

	Z_CheckHeap();               /* DEBUG */
	buf = Z_TagMalloc(size, 1);
	if (!buf)
		Sys_Error("Z_Malloc: failed on allocation of %i bytes", size);
	memset(buf, 0, size);

	return buf;
}

void Z_CheckHeap(void)
{
	memblock_t *block;

	for (block = mainzone->blocklist.next; ; block = block->next) {
		if (block->next == &mainzone->blocklist)
			break;               /* all blocks have been hit */
		if ((byte *) block + block->size != (byte *) block->next)
			Sys_Error("Z_CheckHeap: block size does not touch the next block");
		if (block->next->prev != block)
			Sys_Error("Z_CheckHeap: next block doesn't have proper back link");
		if (!block->tag && !block->next->tag)
			Sys_Error("Z_CheckHeap: two consecutive free blocks");
	}
}

int Z_FreeMemory(void)
{
	memblock_t *block;
	int         free = 0;

	for (block = mainzone->blocklist.next;
	     block != &mainzone->blocklist;
	     block = block->next) {
		if (!block->tag)
			free += block->size;
	}
	return free;
}

/*
==============================================================================
                        HUNK MEMORY ALLOCATION
==============================================================================
*/

#define HUNK_SENTINAL 0x1df001ed

typedef struct {
	int  sentinal;
	int  size;                    /* including sizeof(hunk_t), -1 = not allocated */
	char name[8];
} hunk_t;

static byte *hunk_base;
static int   hunk_size;

static int   hunk_low_used;
static int   hunk_high_used;

static int   hunk_tempactive;
static int   hunk_tempmark;

void Hunk_Check(void)
{
	hunk_t *h;

	for (h = (hunk_t *) hunk_base; (byte *) h != hunk_base + hunk_low_used; ) {
		if (h->sentinal != HUNK_SENTINAL)
			Sys_Error("Hunk_Check: trashed sentinal");
		if (h->size < 16 || h->size + (byte *) h - hunk_base > hunk_size)
			Sys_Error("Hunk_Check: bad size");
		h = (hunk_t *) ((byte *) h + h->size);
	}
}

void *Hunk_AllocName(int size, const char *name)
{
	hunk_t *h;

	if (size < 0)
		Sys_Error("Hunk_Alloc: bad size: %i", size);

	size = sizeof(hunk_t) + ((size + 15) & ~15);

	if (hunk_size - hunk_low_used - hunk_high_used < size)
		Sys_Error("Hunk_Alloc: failed on %i bytes", size);

	h = (hunk_t *) (hunk_base + hunk_low_used);
	hunk_low_used += size;

	memset(h, 0, size);

	h->size     = size;
	h->sentinal = HUNK_SENTINAL;
	strncpy(h->name, name, 8);

	return (void *) (h + 1);
}

void *Hunk_Alloc(int size)
{
	return Hunk_AllocName(size, "unknown");
}

int Hunk_LowMark(void)
{
	return hunk_low_used;
}

void Hunk_FreeToLowMark(int mark)
{
	if (mark < 0 || mark > hunk_low_used)
		Sys_Error("Hunk_FreeToLowMark: bad mark %i", mark);
	memset(hunk_base + mark, 0, hunk_low_used - mark);
	hunk_low_used = mark;
}

int Hunk_HighMark(void)
{
	if (hunk_tempactive) {
		hunk_tempactive = 0;
		Hunk_FreeToHighMark(hunk_tempmark);
	}

	return hunk_high_used;
}

void Hunk_FreeToHighMark(int mark)
{
	if (hunk_tempactive) {
		hunk_tempactive = 0;
		Hunk_FreeToHighMark(hunk_tempmark);
	}
	if (mark < 0 || mark > hunk_high_used)
		Sys_Error("Hunk_FreeToHighMark: bad mark %i", mark);
	memset(hunk_base + hunk_size - hunk_high_used, 0, hunk_high_used - mark);
	hunk_high_used = mark;
}

void *Hunk_HighAllocName(int size, const char *name)
{
	hunk_t *h;

	if (size < 0)
		Sys_Error("Hunk_HighAllocName: bad size: %i", size);

	if (hunk_tempactive) {
		Hunk_FreeToHighMark(hunk_tempmark);
		hunk_tempactive = 0;
	}

	size = sizeof(hunk_t) + ((size + 15) & ~15);

	if (hunk_size - hunk_low_used - hunk_high_used < size) {
		printf("Hunk_HighAlloc: failed on %i bytes\n", size);
		return NULL;
	}

	hunk_high_used += size;

	h = (hunk_t *) (hunk_base + hunk_size - hunk_high_used);

	memset(h, 0, size);
	h->size     = size;
	h->sentinal = HUNK_SENTINAL;
	strncpy(h->name, name, 8);

	return (void *) (h + 1);
}

/* Return space from the top of the hunk. */
void *Hunk_TempAlloc(int size)
{
	void *buf;

	size = (size + 15) & ~15;

	if (hunk_tempactive) {
		Hunk_FreeToHighMark(hunk_tempmark);
		hunk_tempactive = 0;
	}

	hunk_tempmark = Hunk_HighMark();

	buf = Hunk_HighAllocName(size, "temp");

	hunk_tempactive = 1;

	return buf;
}

/*
==============================================================================
                        q1psx diagnostics
==============================================================================
*/

int Hunk_LowUsed(void)   { return hunk_low_used; }
int Hunk_HighUsed(void)  { return hunk_high_used; }
int Hunk_TotalSize(void) { return hunk_size; }

void Mem_Report(void)
{
	printf("q1psx mem: hunk %d/%d used (low %d, high %d), free %d; zone free %d\n",
	       hunk_low_used + hunk_high_used, hunk_size,
	       hunk_low_used, hunk_high_used,
	       hunk_size - hunk_low_used - hunk_high_used,
	       Z_FreeMemory());
}

/*
==============================================================================
                        Memory_Init
==============================================================================
*/

void Memory_Init(void *buf, int size)
{
	int zonesize = DYNAMIC_SIZE;

	/*
	 * Stock Quake trusts malloc() to hand back a well-aligned membase. On the
	 * PS1 the libc heap only guarantees 8-byte alignment, so nudge the base up
	 * to 16 and trim the size to keep both ends of the hunk 16-aligned.
	 */
	uintptr_t base = ((uintptr_t) buf + 15) & ~(uintptr_t) 15;
	size -= (int) (base - (uintptr_t) buf);
	size &= ~15;

	hunk_base       = (byte *) base;
	hunk_size       = size;
	hunk_low_used   = 0;
	hunk_high_used  = 0;
	hunk_tempactive = 0;
	hunk_tempmark   = 0;

	mainzone = Hunk_AllocName(zonesize, "zone");
	Z_ClearZone(mainzone, zonesize);
}
