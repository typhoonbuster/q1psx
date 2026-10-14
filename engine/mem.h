/*
 * q1psx — fixed memory map (architecture.md §4).
 *
 * No general-purpose heap at runtime. Each region is a static block with one
 * owner and a bump allocator:
 *
 *   level arena   everything a map needs (world, PVS, clip hulls, ...);
 *                 reset on map load. mem_level_mark()/mem_level_release()
 *                 give a transient top-of-arena area (e.g. texture pixels on
 *                 their way to VRAM).
 *
 * The packet arenas live in vid.c; the game arena (QuakeC) arrives in
 * Phase 7 and will take part of the level arena's current size.
 */

#ifndef Q1PSX_MEM_H
#define Q1PSX_MEM_H

#include <stdint.h>

#define MEM_LEVEL_BYTES (1024 * 1024)

void     mem_level_reset(void);
void    *mem_level_alloc(uint32_t bytes);      /* 16-byte aligned; NULL if full */
uint32_t mem_level_mark(void);
void     mem_level_release(uint32_t mark);
uint32_t mem_level_used(void);

#endif
