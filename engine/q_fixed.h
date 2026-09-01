/*
 * q1psx — fixed-point scalar core (20.12)
 *
 * The PS1 R3000A has no FPU. Quake's float math is ported to 20.12 fixed point
 * (1.0 == 4096), matching the GTE and PSn00bSDK's libpsn00b so GTE-accelerated
 * paths need no conversion (decision recorded in docs/porting-notes.md §2.1).
 *
 * This header is plain C with only <stdint.h>: it also compiles on the host,
 * which is how the math layer is unit-tested (tools/mathtest) before it ever
 * runs on hardware.
 *
 * Representable integer range ~ +/-524287; keep world/model coordinates well
 * inside that (Quake maps are +/-4096). Products go through int64 internally.
 */

#pragma once

#include <stdint.h>

typedef int32_t q_fixed;

#define Q_FIX_SHIFT  12
#define Q_FIX_ONE    (1 << Q_FIX_SHIFT)          /* 4096 == 1.0 */
#define Q_FIX_HALF   (Q_FIX_ONE >> 1)
#define Q_FIX_MASK   (Q_FIX_ONE - 1)

/* integer <-> fixed */
#define Q_FIX(n)         ((q_fixed)((int32_t)(n) * Q_FIX_ONE))
#define Q_FIX_INT(a)     ((int32_t)((a) >> Q_FIX_SHIFT))          /* arithmetic: floor toward -inf */
#define Q_FIX_FRAC(a)    ((q_fixed)((a) & Q_FIX_MASK))
#define Q_FIX_ROUND(a)   ((int32_t)(((a) + Q_FIX_HALF) >> Q_FIX_SHIFT))

/*
 * Float conversions — for the HOST only (asset cooking, unit tests). Never
 * emitted for the MIPS target; guarded so a stray use on-device fails to build.
 */
#if !defined(__mips__)
#define Q_FIX_FROM_FLOAT(f)  ((q_fixed)((double)(f) * (double)Q_FIX_ONE))
#define Q_FIX_TO_FLOAT(a)    ((double)(a) / (double)Q_FIX_ONE)
#endif

static inline q_fixed q_mul(q_fixed a, q_fixed b)
{
	return (q_fixed)(((int64_t)a * (int64_t)b) >> Q_FIX_SHIFT);
}

static inline q_fixed q_div(q_fixed a, q_fixed b)
{
	return (q_fixed)((((int64_t)a) << Q_FIX_SHIFT) / (int64_t)b);
}

static inline q_fixed q_abs(q_fixed a)
{
	return a < 0 ? -a : a;
}

/*
 * Fixed-point square root, digit-by-digit binary method (no division, no
 * float). On-device the GTE's SquareRoot12() can replace this on hot paths.
 *
 * q_sqrt_big() takes a 20.12 value that may be wider than q_fixed (e.g. a
 * squared vector length) and returns its root as 20.12. The input must be
 * < 2^52 so the internal `<< 12` does not overflow uint64 — squared lengths of
 * Quake-scale vectors are far below that.
 */
static inline q_fixed q_sqrt_big(uint64_t x_fixed)
{
	uint64_t n   = x_fixed << Q_FIX_SHIFT;   /* so the root comes out << 6*2 -> << 12 */
	uint64_t res = 0;
	uint64_t bit = (uint64_t)1 << 62;

	if (!x_fixed)
		return 0;

	while (bit > n)
		bit >>= 2;

	while (bit) {
		if (n >= res + bit) {
			n   -= res + bit;
			res  = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}
	return (q_fixed)res;
}

static inline q_fixed q_sqrt(q_fixed x)
{
	return x <= 0 ? 0 : q_sqrt_big((uint64_t)(uint32_t)x);
}
