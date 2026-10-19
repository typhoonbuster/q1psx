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
 *                 models' geometry: engine/gdata.c), filled once at boot
 *                 (the 2D gun's packets are not: engine/game.c
 *                 game_ws_sync).
 *
 * The packet arenas live in vid.c; the QuakeC arena (Phase 7) will take
 * part of the level arena's current size.
 */

#ifndef Q1PSX_MEM_H
#define Q1PSX_MEM_H

#include <stdint.h>

#define MEM_LEVEL_BYTES (1240 * 1024)  /* (1152 until 2026-10-19: with the monsters,
                                          the streaming and the brush models' pieces
                                          the BSS ended past the 2 MB; the stack's
                                          16 KB checked at every link:
                                          scripts/ramcheck.sh; 1136 + the game
                                          arena's 108 KB once the 2D gun's packets
                                          left it, less 2 KB for the code that
                                          lends them: porting-notes §45; less 2 KB
                                          for the music's streams' code, §48) */
#define MEM_LEVEL_WORLD (1136 * 1024)  /* the room a map's world may take while it
                                          loads (below it, a lower geometry level):
                                          the arena's size before §45, so the room
                                          the game arena gave goes to what follows
                                          the world (the stream's pool, the 2D
                                          gun's packets), not to a larger world */
#define MEM_GAME_BYTES  (115 * 1024)   /* (the boot's tables: 114 KB, "mem: game
                                          arena" at boot, the XA music's carousel
                                          header included; 224 KB while it held the 2D
                                          gun's packets, now lent by the stream's pool
                                          when that gun is chosen) */

void     mem_level_reset(void);
void    *mem_level_alloc(uint32_t bytes);      /* 16-byte aligned; NULL if full */
uint32_t mem_level_mark(void);
void     mem_level_release(uint32_t mark);
uint32_t mem_level_used(void);
void     mem_level_cap(uint32_t bytes);        /* allocations past `bytes` refused
                                                  (MEM_LEVEL_BYTES: none) */

void    *mem_game_alloc(uint32_t bytes);       /* 16-byte aligned; full: says so and stops
                                                  (boot-time tables only) */
uint32_t mem_game_used(void);

#endif
