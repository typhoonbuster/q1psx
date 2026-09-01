/*
 * q1psx — vector math (20.12 fixed point)
 *
 * Ported from id-Software/Quake WinQuake/mathlib.c. See mathlib.h.
 */

#include <stdint.h>

#include "mathlib.h"
#include "q_trig_table.h"

const vec3_t vec3_origin = { 0, 0, 0 };

/* ============================================================ trig ======= */

/*
 * Table lookup with linear interpolation. `degrees` is 20.12; convert to a
 * table position carrying 8 fractional bits, wrap to one turn (bitwise AND —
 * works for negative positions in two's complement), then lerp. The divide by
 * 360 runs a few times per frame at most, never per vertex.
 */
#define TRIG_FRAC_BITS  8
#define TRIG_POS_ONE    (Q_SINTAB_SIZE << TRIG_FRAC_BITS)

vec_t q_sin(vec_t degrees)
{
	int64_t p = ((int64_t)degrees * TRIG_POS_ONE) / (360 * Q_FIX_ONE);
	int32_t pos = (int32_t)(p & (TRIG_POS_ONE - 1));

	int idx  = pos >> TRIG_FRAC_BITS;
	int frac = pos & ((1 << TRIG_FRAC_BITS) - 1);

	vec_t a = q_sintab[idx];
	vec_t b = q_sintab[idx + 1];
	return a + (((b - a) * frac) >> TRIG_FRAC_BITS);
}

vec_t q_cos(vec_t degrees)
{
	return q_sin(degrees + Q_DEG(90));
}

vec_t anglemod(vec_t a)
{
	const vec_t full = Q_DEG(360);   /* 1474560 in 20.12 — not a power of two */

	a %= full;
	if (a < 0)
		a += full;
	return a;
}

/* ===================================================== AngleVectors ===== */

void AngleVectors(const vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
{
	vec_t sy = q_sin(angles[YAW]),   cy = q_cos(angles[YAW]);
	vec_t sp = q_sin(angles[PITCH]), cp = q_cos(angles[PITCH]);
	vec_t sr = q_sin(angles[ROLL]),  cr = q_cos(angles[ROLL]);

	if (forward) {
		forward[0] = q_mul(cp, cy);
		forward[1] = q_mul(cp, sy);
		forward[2] = -sp;
	}
	if (right) {
		right[0] = q_mul(-q_mul(sr, sp), cy) + q_mul(cr, sy);
		right[1] = q_mul(-q_mul(sr, sp), sy) - q_mul(cr, cy);
		right[2] = -q_mul(sr, cp);
	}
	if (up) {
		up[0] = q_mul(q_mul(cr, sp), cy) + q_mul(sr, sy);
		up[1] = q_mul(q_mul(cr, sp), sy) - q_mul(sr, cy);
		up[2] = q_mul(cr, cp);
	}
}

/* ================================================= matrix concat ======= */

static vec_t dot3(vec_t a, vec_t b, vec_t c, vec_t x, vec_t y, vec_t z)
{
	int64_t acc = (int64_t)a * x + (int64_t)b * y + (int64_t)c * z;
	return (vec_t)(acc >> Q_FIX_SHIFT);
}

void R_ConcatRotations(const vec_t in1[3][3], const vec_t in2[3][3], vec_t out[3][3])
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			out[i][j] = dot3(in1[i][0], in1[i][1], in1[i][2],
			                 in2[0][j], in2[1][j], in2[2][j]);
}

void R_ConcatTransforms(const vec_t in1[3][4], const vec_t in2[3][4], vec_t out[3][4])
{
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++)
			out[i][j] = dot3(in1[i][0], in1[i][1], in1[i][2],
			                 in2[0][j], in2[1][j], in2[2][j]);
		out[i][3] = dot3(in1[i][0], in1[i][1], in1[i][2],
		                 in2[0][3], in2[1][3], in2[2][3]) + in1[i][3];
	}
}

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
