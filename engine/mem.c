#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "mem.h"

static uint8_t  level_mem[MEM_LEVEL_BYTES] __attribute__((aligned(16)));
static uint32_t level_top;

void mem_level_reset(void)
{
	level_top = 0;
}

void *mem_level_alloc(uint32_t bytes)
{
	uint32_t size = (bytes + 15u) & ~15u;
	if (size > MEM_LEVEL_BYTES - level_top) {
		printf("mem: level arena full (%u + %u > %u)\n",
		       level_top, size, (uint32_t) MEM_LEVEL_BYTES);
		return NULL;
	}
	void *p = level_mem + level_top;
	level_top += size;
	return p;
}

uint32_t mem_level_mark(void)          { return level_top; }
void     mem_level_release(uint32_t m) { if (m <= level_top) level_top = m; }
uint32_t mem_level_used(void)          { return level_top; }

static uint8_t  game_mem[MEM_GAME_BYTES] __attribute__((aligned(16)));
static uint32_t game_top;

void *mem_game_alloc(uint32_t bytes)
{
	uint32_t size = (bytes + 15u) & ~15u;
	if (size > MEM_GAME_BYTES - game_top) {
		printf("mem: game arena full (%u + %u > %u)\n",
		       game_top, size, (uint32_t) MEM_GAME_BYTES);
		return NULL;
	}
	void *p = game_mem + game_top;
	game_top += size;
	return p;
}

uint32_t mem_game_used(void) { return game_top; }
