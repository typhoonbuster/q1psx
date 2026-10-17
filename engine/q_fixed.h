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
