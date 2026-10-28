#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "mem.h"

/* The level arena: every byte from the BSS's end to the stack's room
   (docs/porting-notes.md §81.7) */
extern uint8_t  _end[];
static uint8_t *level_mem;
static uint32_t level_bytes, level_top, level_cap;

/* The stack's room: painted at boot, from its lowest word up to the caller's
   frame; mem_stack_untouched measures what was never reached, mem_stack_ok
   whether the guard (its lowest MEM_STACK_GUARD bytes) still holds paint */
#define STACK_PAINT  0x57ac57acu
#define STACK_BOTTOM ((uint32_t *) (MEM_STACK_TOP - MEM_STACK_BYTES))

Q_COLD void mem_init(void)
{
	level_mem   = (uint8_t *) (((uint32_t) _end + 15u) & ~15u);
	level_bytes = ((MEM_STACK_TOP - MEM_STACK_BYTES) & ~15u) - (uint32_t) level_mem;
#if Q1PSX_LEVEL_CAP
	if (level_bytes > Q1PSX_LEVEL_CAP)
		level_bytes = Q1PSX_LEVEL_CAP;        /* (a bench's: the release's size, an A / B) */
#endif
	level_cap   = level_bytes;
	memset(level_mem, 0, level_bytes);        /* (as the BSS array was) */
	uint32_t *p = STACK_BOTTOM, *top = (uint32_t *) __builtin_frame_address(0) - 64;
	while (p < top)
		*p++ = STACK_PAINT;
}

uint32_t mem_stack_untouched(void)
{
	const uint32_t *p = STACK_BOTTOM, *q = p;
	while (*q == STACK_PAINT)
		q++;
	return (uint32_t) ((const uint8_t *) q - (const uint8_t *) p);
}

int mem_stack_ok(void)
{
	static uint32_t part;                       /* (a sixteenth a call: all of it in 16 frames) */
	const uint32_t *p = STACK_BOTTOM + (part++ & 15) * (MEM_STACK_GUARD / 64);
	uint32_t bad = 0;
	for (int i = 0; i < MEM_STACK_GUARD / 64; i += 4)
		bad |= (p[i] ^ STACK_PAINT) | (p[i + 1] ^ STACK_PAINT) | (p[i + 2] ^ STACK_PAINT) |
		       (p[i + 3] ^ STACK_PAINT);
	return !bad;
}

uint32_t mem_level_bytes(void)
{
	return level_bytes;
}

void mem_level_reset(void)
{
	level_top = 0;
}

void mem_level_cap(uint32_t bytes)
{
	level_cap = bytes < level_bytes ? bytes : level_bytes;
}

#if Q1PSX_BENCH_MEMLOG
/* the arena's log (MEMLOG=1 scripts/bench.sh): every block taken or given
   back, with its caller's return address (addr2line on the host): a map's
   RAM block by block, docs/porting-notes.md §90 */
#define MEMLOG(...) printf(__VA_ARGS__)
#else
#define MEMLOG(...) ((void) 0)
#endif

void *mem_level_alloc(uint32_t bytes)
{
	uint32_t size = (bytes + 15u) & ~15u;
	if (level_top > level_cap || size > level_cap - level_top) {
		printf("mem: level arena full (%u + %u > %u)\n",
		       level_top, size, (unsigned) level_cap);
		MEMLOG("memlog alloc %u REFUSED top %u cap %u from %08x\n", bytes, level_top, (unsigned) level_cap,
		       (unsigned) __builtin_return_address(0));
		return NULL;
	}
	void *p = level_mem + level_top;
	level_top += size;
	MEMLOG("memlog alloc %u at %08x top %u from %08x\n", bytes, (unsigned) p, level_top,
	       (unsigned) __builtin_return_address(0));
	return p;
}

uint32_t mem_level_mark(void)          { return level_top; }
void     mem_level_release(uint32_t m)
{
	if (m <= level_top) {
		MEMLOG("memlog release %u -> %u from %08x\n", level_top, m, (unsigned) __builtin_return_address(0));
		level_top = m;
	}
}
uint32_t mem_level_used(void)          { return level_top; }

static uint8_t  game_mem[MEM_GAME_BYTES] __attribute__((aligned(16)));
static uint32_t game_top;

void *mem_game_alloc(uint32_t bytes)
{
	uint32_t size = (bytes + 15u) & ~15u;
	if (size > MEM_GAME_BYTES - game_top) {    /* (boot-time tables: none may go
		                                              missing, a NULL would be written
		                                              through: stop here, said) */
		printf("mem: game arena full (%u + %u > %u): MEM_GAME_BYTES too small\n",
		       game_top, size, (uint32_t) MEM_GAME_BYTES);
		for (;;)
			;
	}
	void *p = game_mem + game_top;
	game_top += size;
	return p;
}

uint32_t mem_game_used(void) { return game_top; }
