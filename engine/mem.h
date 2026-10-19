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
 *   game arena    the game data every map needs (the weapon and projectile
 *                 models' geometry: engine/gdata.c), filled once at boot.
 *
 * The packet arenas live in vid.c; the QuakeC arena (Phase 7) will take
 * part of the level arena's current size.
 */

#ifndef Q1PSX_MEM_H
#define Q1PSX_MEM_H

#include <stdint.h>

#define MEM_LEVEL_BYTES (1136 * 1024)  /* (1152 until 2026-10-19: with the monsters,
                                          the streaming and the brush models' pieces
                                          the BSS ended past the 2 MB; the stack's
                                          16 KB checked at every link:
                                          scripts/ramcheck.sh) */
#define MEM_GAME_BYTES  (224 * 1024)   /* (+ the 2D weapons' 102 KB since 2026-10-18;
                                          the dynamic lights' and the sounds' tables follow) */

void     mem_level_reset(void);
void    *mem_level_alloc(uint32_t bytes);      /* 16-byte aligned; NULL if full */
uint32_t mem_level_mark(void);
void     mem_level_release(uint32_t mark);
uint32_t mem_level_used(void);

void    *mem_game_alloc(uint32_t bytes);       /* 16-byte aligned; full: says so and stops
                                                  (boot-time tables only) */
uint32_t mem_game_used(void);

#endif
