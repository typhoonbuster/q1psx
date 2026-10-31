/*
 * q1psx — fixed memory map (architecture.md §4).
 *
 * No general-purpose heap at runtime. Each region is a block with one owner
 * and a bump allocator:
 *
 *   level arena   everything a map needs (world, PVS, clip hulls, ...): every
 *                 byte between the BSS and the stack's room (mem_init, §81.7);
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

/* The RAM's layout (docs/porting-notes.md §81.7): the program and its BSS
   from 0x80010000, then the level arena, every byte up to the stack's room,
   then the stack, MEM_STACK_BYTES under the kernel's stack top. Until §81.7
   the arena was a BSS array of 1243 KB and the stack had what was left (the
   RAM every pass freed: 89.7 KB, of which ~2.5 KB ever used); now what the
   program frees goes to the arena by itself. */
#define MEM_STACK_TOP   0x801ffff0u    /* the EXE header's stack: the kernel sets sp there */
#ifndef Q1PSX_STACK_BYTES
#define Q1PSX_STACK_BYTES 8192         /* (CMakeLists.txt Q1PSX_STACK_BYTES) */
#endif
#define MEM_STACK_BYTES Q1PSX_STACK_BYTES   /* the stack's room: 8 KB (§90.4; 12 KB from
                                          §81.7, the choice then: every bench
                                          reaches 2.0-2.6 KB, §61's worst was 4.3 KB; the
                                          guard below is a KB of it) */
#define MEM_STACK_GUARD 1024           /* its lowest bytes: painted at boot, a sixteenth
                                          checked a frame (mem_stack_ok, all of it in 16
                                          frames): reached, the game stops on a red
                                          screen (vid_fatal) */
#define MEM_LEVEL_MIN   (1243 * 1024)  /* the least level arena the link accepts
                                          (scripts/ramcheck.sh): its size until §81.7 */
#ifndef Q1PSX_LEVEL_CAP
#define Q1PSX_LEVEL_CAP 0              /* the arena capped at that many bytes (0: none;
                                          CMakeLists.txt, bench.sh LEVEL_CAP: a bench
                                          whose code leaves more than the release's) */
#endif
#define MEM_LEVEL_WORLD (1136 * 1024)  /* the room a map's world may take while it
                                          loads (past it, the map fails to load):
                                          the arena's size before §45, so the room
                                          the game arena gave goes to what follows
                                          the world (the stream's pool), not to a
                                          larger world */
#define MEM_GAME_BYTES  (92 * 1024)    /* (90 KB until the registered game's six
                                          monsters: the stream's directory 162 ->
                                          224 assets, 56 B each in the engine's
                                          table; the boot's tables: 85 KB, "mem: game
                                          arena" at boot; 115 KB until §53, the guns' frames
                                          then Quake's own vertex words, 100 KB until
                                          §54, their hidden triangles gone: 25 KB left
                                          the arena, kept as the stack's margin (for
                                          now); 224 KB
                                          while it held the 2D gun's packets, until
                                          §45; that gun gone since §119) */

/* Code that runs once a level or at a menu's pace (the loads, the spawns,
   a level's start and end): compiled for size, out of the hot code's way
   (GCC's cold: -Os for the function, its own text section). Every KB of
   code is a KB of RAM (docs/porting-notes.md §60). */
#define Q_COLD __attribute__((cold))

void     mem_level_reset(void);
void    *mem_level_alloc(uint32_t bytes);      /* 16-byte aligned; NULL if full */
uint32_t mem_level_mark(void);
void     mem_level_release(uint32_t mark);
uint32_t mem_level_used(void);
void     mem_level_cap(uint32_t bytes);        /* allocations past `bytes` refused
                                                  (mem_level_bytes(): none) */
uint32_t mem_level_bytes(void);                /* the arena's size: the RAM between the
                                                  BSS and the stack's room */
void     mem_init(void);                       /* first thing at boot: the arena zeroed,
                                                  the stack's room painted */
uint32_t mem_stack_untouched(void);            /* the stack's room never reached, bytes */
int      mem_stack_ok(void);                   /* 0 once the stack has reached its guard
                                                  (a sixteenth of it checked a call) */

void    *mem_game_alloc(uint32_t bytes);       /* 16-byte aligned; full: says so and stops
                                                  (boot-time tables only) */
uint32_t mem_game_used(void);

#endif
