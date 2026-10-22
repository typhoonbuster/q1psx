#include "q_fixed.h"

int64_t q_div64_32_long(int64_t n, int32_t d)
{
	uint64_t un = n < 0 ? 0 - (uint64_t) n : (uint64_t) n;
	uint32_t ud = d < 0 ? 0u - (uint32_t) d : (uint32_t) d;
	uint32_t hi = (uint32_t) (un >> 32), lo = (uint32_t) un, qh = 0, ql;
	if (!hi) {
		ql = lo / ud;
	} else {                                   /* restoring division, 64 steps */
		uint32_t r = 0;
		ql = 0;
		for (int i = 63; i >= 0; i--) {
			/* (r < ud <= 2^31: the shift never carries out) */
			r  = r << 1 | ((i >= 32 ? hi >> (i - 32) : lo >> i) & 1);
			qh = qh << 1 | ql >> 31;
			ql <<= 1;
			if (r >= ud) {
				r -= ud;
				ql |= 1;
			}
		}
	}
	uint64_t q = (uint64_t) qh << 32 | ql;
	return (n < 0) != (d < 0) ? -(int64_t) q : (int64_t) q;
}
