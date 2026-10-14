/*
 * q1psx — collision against the world's clip hulls (Quake hulls 1 and 2).
 *
 * clip_trace() sweeps a hull-sized box (represented, as in Quake, by a point
 * against the pre-expanded hull) from start to end and reports the first
 * impact. Fixed-point port of SV_RecursiveHullCheck / SV_HullPointContents.
 * Positions are in 1/CLIP_SUB world units (sub-unit precision matters:
 * with whole units, sliding along a gentle ramp rounded every bump to zero
 * and the player stuck at its foot). Plane normals / distances are x4096.
 */

#ifndef Q1PSX_CLIP_H
#define Q1PSX_CLIP_H

#include <stdint.h>

#include "q_fixed.h"
#include "qworld.h"

#define CLIP_SUB QW_CLIP_SUB   /* position sub-units per world unit (8) */

typedef struct {
	int      startsolid;   /* the trace began inside solid            */
	int      allsolid;     /* the whole trace was inside solid        */
	int      inopen;
	int      inwater;
	q_fixed  fraction;     /* 0..Q_FIX_ONE; ONE = reached `end`       */
	int32_t  endpos[3];    /* 1/CLIP_SUB world units                 */
	int16_t  plane_n[3];   /* normal of the surface hit (x4096)      */
	int32_t  plane_d;      /* its distance (x4096, 1/8 units); 0 if nothing hit */
} trace_t;

/* engine/p_defs.inc mirrors this layout for the asm port */
_Static_assert(__builtin_offsetof(trace_t, fraction) == 16, "p_defs.inc TR_FRACTION");
_Static_assert(__builtin_offsetof(trace_t, endpos)   == 20, "p_defs.inc TR_ENDPOS");
_Static_assert(__builtin_offsetof(trace_t, plane_n)  == 32, "p_defs.inc TR_PLANE_N");
_Static_assert(__builtin_offsetof(trace_t, plane_d)  == 40, "p_defs.inc TR_PLANE_D");
_Static_assert(sizeof(trace_t) == 44, "p_defs.inc TR_SIZE");

/* hull 1 = player size, 2 = large. Returns tr->fraction. Does nothing (and
   reports a free move) when the world has no clip hulls. The PS1 build links
   the asm port (engine/clip.s): positions must fit int16 there, i.e. stay
   within +-4095 world units (Quake's own map limit). */
q_fixed clip_trace(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr);

/* Quake CONTENTS_* of hull `hull` at p. */
int clip_contents(int hull, const int32_t p[3]);

#endif
