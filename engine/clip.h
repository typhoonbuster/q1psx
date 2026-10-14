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

#define CLIP_SUB 8   /* position sub-units per world unit */

typedef struct {
	int      startsolid;   /* the trace began inside solid            */
	int      allsolid;     /* the whole trace was inside solid        */
	int      inopen;
	int      inwater;
	q_fixed  fraction;     /* 0..Q_FIX_ONE; ONE = reached `end`       */
	int32_t  endpos[3];    /* 1/CLIP_SUB world units                 */
	int16_t  plane_n[3];   /* normal of the surface hit (x4096)      */
	int32_t  plane_d;      /* its distance (x4096); 0 if nothing hit */
} trace_t;

/* hull 1 = player size, 2 = large. Returns tr->fraction. Does nothing (and
   reports a free move) when the world has no clip hulls. */
q_fixed clip_trace(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr);

/* Quake CONTENTS_* of hull `hull` at p. */
int clip_contents(int hull, const int32_t p[3]);

#endif
