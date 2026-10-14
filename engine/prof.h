/*
 * q1psx — cycle profiler (architecture.md §9).
 *
 * Root counter 2 runs at sysclock/8 (one tick = 8 CPU cycles, ~4.23 MHz); a
 * wrap IRQ extends it to 32 bits (wraps after ~17 minutes). Read it with
 * prof_now() from C. Asm stages read the raw 16-bit counter at 0x1F801120 and
 * add `(t1 - t0) & 0xffff` ticks — fine for any stage shorter than ~520 k
 * cycles, i.e. anything that fits in a frame.
 *
 * Per-frame accumulators: prof_stage[] (cycles) and prof_count[] (events) are
 * zeroed by prof_frame_reset() at the start of each frame. Keep the indices in
 * sync with engine/r_defs.inc.
 */

#ifndef Q1PSX_PROF_H
#define Q1PSX_PROF_H

#include <stdint.h>

#define PROF_CYCLES_PER_TICK 8

enum {
	PROF_FRAME = 0,  /* all CPU work of the frame (waits excluded)     */
	PROF_SIM,        /* simulation tick(s)                             */
	PROF_R_SETUP,    /* renderer: view setup, leaf lookup, PVS marking */
	PROF_R_WALK,     /* renderer: BSP walk, blocks excluded            */
	PROF_R_BLOCK,    /* renderer: block transform + fast-path emit     */
	PROF_R_SLOW,     /* renderer: near clip / subdivision              */
	PROF_OVERLAY,    /* debug text                                     */
	PROF_STAGES
};

enum {
	PROF_N_NODES = 0, /* BSP nodes visited                   */
	PROF_N_BLOCKS,    /* face blocks drawn                   */
	PROF_N_VERTS,     /* vertices transformed                */
	PROF_N_POLYS,     /* polygons emitted                    */
	PROF_N_SLOW,      /* polygons that took the slow path    */
	PROF_COUNTERS
};

extern uint32_t prof_stage[PROF_STAGES];
extern uint32_t prof_count[PROF_COUNTERS];
extern const char *const prof_stage_name[PROF_STAGES];
extern const char *const prof_count_name[PROF_COUNTERS];

void     prof_init(void);
uint32_t prof_now(void);          /* ticks (x8 = CPU cycles) */
void     prof_frame_reset(void);

static inline uint32_t prof_cycles(uint32_t t0, uint32_t t1)
{
	return (t1 - t0) * PROF_CYCLES_PER_TICK;
}

static inline void prof_add(int stage, uint32_t t0, uint32_t t1)
{
	prof_stage[stage] += prof_cycles(t0, t1);
}

#endif
