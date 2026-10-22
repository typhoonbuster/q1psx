/*
 * q1psx — 20.12 fixed point (project-wide convention: ONE = 4096, the GTE's
 * own scale) and an integer square root.
 */

#ifndef Q1PSX_Q_FIXED_H
#define Q1PSX_Q_FIXED_H

#include <stdint.h>

typedef int32_t q_fixed;

#define Q_FIX_SHIFT 12
#define Q_FIX_ONE   (1 << Q_FIX_SHIFT)

/* n / d exactly as C divides an int64 by an int32 (truncated toward zero),
   d != 0, without libgcc's __divdi3 (1.5 KB, a call of a hundred cycles
   and more): one div when n fits 32 bits (what the game divides: lengths,
   ticks, 1/8 units), else a restoring division on the 64 bits
   (engine/q_fixed.c) */
int64_t q_div64_32_long(int64_t n, int32_t d);

static inline int64_t q_div64_32(int64_t n, int32_t d)
{
	if (n == (int32_t) n && d != -1)
		return (int32_t) n / d;
	return q_div64_32_long(n, d);
}

/* floor(sqrt(v)) by the bit-by-bit method: no division, exact */
static inline uint32_t q_isqrt32(uint32_t v)
{
	uint32_t res = 0, bit = 1u << 30;
	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= res + bit) {
			v  -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}
	return res;
}

static inline uint32_t q_isqrt64(uint64_t v)
{
	if (v < ((uint64_t) 1 << 32))        /* (the usual case: a quarter of the work) */
		return q_isqrt32((uint32_t) v);
	uint64_t res = 0, bit = (uint64_t) 1 << 62;
	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= res + bit) {
			v  -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t) res;
}

#endif
