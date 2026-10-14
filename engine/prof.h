/*
 * q1psx — cycle profiler (architecture.md §9).
 *
 * Root counter 2 runs at sysclock/8 (one tick = 8 CPU cycles, ~4.23 MHz); a
 * wrap IRQ extends it to 32 bits (wraps after ~17 minutes). Read it with
 * prof_now() from C, or from asm through the same routine.
 */

#ifndef Q1PSX_PROF_H
#define Q1PSX_PROF_H

#include <stdint.h>

#define PROF_CYCLES_PER_TICK 8

void     prof_init(void);
uint32_t prof_now(void);          /* ticks (x8 = CPU cycles) */

static inline uint32_t prof_cycles(uint32_t t0, uint32_t t1)
{
	return (t1 - t0) * PROF_CYCLES_PER_TICK;
}

#endif
