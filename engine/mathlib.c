/*
 * q1psx — vector math (20.12 fixed point)
 *
 * Ported from id-Software/Quake WinQuake/mathlib.c. See mathlib.h.
 */

#include <stdint.h>

#include "mathlib.h"

const vec3_t vec3_origin = { 0, 0, 0 };

vec_t DotProduct(const vec3_t v1, const vec3_t v2)
{
	int64_t acc = (int64_t)v1[0] * v2[0]
	            + (int64_t)v1[1] * v2[1]
	            + (int64_t)v1[2] * v2[2];
	return (vec_t)(acc >> Q_FIX_SHIFT);
}

void CrossProduct(const vec3_t v1, const vec3_t v2, vec3_t cross)
{
	cross[0] = (vec_t)(((int64_t)v1[1] * v2[2] - (int64_t)v1[2] * v2[1]) >> Q_FIX_SHIFT);
	cross[1] = (vec_t)(((int64_t)v1[2] * v2[0] - (int64_t)v1[0] * v2[2]) >> Q_FIX_SHIFT);
	cross[2] = (vec_t)(((int64_t)v1[0] * v2[1] - (int64_t)v1[1] * v2[0]) >> Q_FIX_SHIFT);
}

void VectorScale(const vec3_t in, vec_t scale, vec3_t out)
{
	out[0] = q_mul(in[0], scale);
	out[1] = q_mul(in[1], scale);
	out[2] = q_mul(in[2], scale);
}

void VectorMA(const vec3_t veca, vec_t scale, const vec3_t vecb, vec3_t vecc)
{
	vecc[0] = veca[0] + q_mul(scale, vecb[0]);
	vecc[1] = veca[1] + q_mul(scale, vecb[1]);
	vecc[2] = veca[2] + q_mul(scale, vecb[2]);
}

int VectorCompare(const vec3_t v1, const vec3_t v2)
{
	for (int i = 0; i < 3; i++)
		if (v1[i] != v2[i])
			return 0;
	return 1;
}

/* squared length as a 20.12 value, wide (may exceed q_fixed range) */
static uint64_t length_sq(const vec3_t v)
{
	int64_t sq = (int64_t)v[0] * v[0]
	           + (int64_t)v[1] * v[1]
	           + (int64_t)v[2] * v[2];
	return (uint64_t)(sq >> Q_FIX_SHIFT);
}

vec_t Length(const vec3_t v)
{
	return q_sqrt_big(length_sq(v));
}

vec_t VectorNormalize(vec3_t v)
{
	vec_t length = q_sqrt_big(length_sq(v));

	if (length) {
		/*
		 * Stock Quake does ilength = 1/length then multiplies, but in fixed
		 * point a sub-1.0 reciprocal is quantised to 1/4096 and normalisation
		 * comes out several percent short for long vectors. Dividing each
		 * component directly keeps operands the same magnitude and is exact
		 * to a couple of units.
		 */
		v[0] = q_div(v[0], length);
		v[1] = q_div(v[1], length);
		v[2] = q_div(v[2], length);
	}

	return length;
}

void VectorInverse(vec3_t v)
{
	v[0] = -v[0];
	v[1] = -v[1];
	v[2] = -v[2];
}
