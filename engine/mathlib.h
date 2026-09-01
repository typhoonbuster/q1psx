/*
 * q1psx — vector math
 *
 * Ported from id-Software/Quake WinQuake/mathlib.{c,h} (GPLv2, (C) 1996-1997 Id
 * Software), converted to 20.12 fixed point (see q_fixed.h). Plain C + stdint,
 * so it also builds and is unit-tested on the host (tools/mathtest).
 *
 * Naming is kept: `vec_t` / `vec3_t` / the Vector* macros, so Quake call sites
 * port unchanged. The one semantic change vs stock: DotProduct is a function
 * (fixed-point needs a wide intermediate and a shift), not a bare macro.
 *
 * Phase 3.2a: scalars + vector ops. Trig / AngleVectors land in 3.2b.
 */

#pragma once

#include "q_fixed.h"

typedef q_fixed vec_t;
typedef vec_t   vec3_t[3];

enum { PITCH = 0, YAW = 1, ROLL = 2 };   /* indices into an angles vec3_t */

extern const vec3_t vec3_origin;

/* --- cheap component-wise ops: no shift needed, stay as macros --- */
#define VectorSubtract(a, b, c) do { (c)[0] = (a)[0] - (b)[0]; \
                                     (c)[1] = (a)[1] - (b)[1]; \
                                     (c)[2] = (a)[2] - (b)[2]; } while (0)
#define VectorAdd(a, b, c)      do { (c)[0] = (a)[0] + (b)[0]; \
                                     (c)[1] = (a)[1] + (b)[1]; \
                                     (c)[2] = (a)[2] + (b)[2]; } while (0)
#define VectorCopy(a, b)        do { (b)[0] = (a)[0]; \
                                     (b)[1] = (a)[1]; \
                                     (b)[2] = (a)[2]; } while (0)
#define VectorClear(a)          do { (a)[0] = (a)[1] = (a)[2] = 0; } while (0)
#define VectorNegate(a, b)      do { (b)[0] = -(a)[0]; \
                                     (b)[1] = -(a)[1]; \
                                     (b)[2] = -(a)[2]; } while (0)

/* --- ops that scale, so they need fixed-point multiply --- */
vec_t DotProduct(const vec3_t v1, const vec3_t v2);
void  CrossProduct(const vec3_t v1, const vec3_t v2, vec3_t cross);
void  VectorScale(const vec3_t in, vec_t scale, vec3_t out);
void  VectorMA(const vec3_t veca, vec_t scale, const vec3_t vecb, vec3_t vecc);

int   VectorCompare(const vec3_t v1, const vec3_t v2);
vec_t Length(const vec3_t v);
vec_t VectorNormalize(vec3_t v);          /* returns the original length */
void  VectorInverse(vec3_t v);
