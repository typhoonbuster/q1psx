/*
 * q1psx — collision against the world's clip hulls (Quake hulls 1 and 2).
 *
 * clip_trace() sweeps a hull-sized box (represented, as in Quake, by a point
 * against the pre-expanded hull) from start to end and reports the first
 * impact: against the world, then against the solid brush models the game
 * lists (clip_bmodels: doors, plats, walls), each in its own space, the
 * nearest kept (SV_Move + SV_ClipToLinks). Fixed-point port of
 * SV_RecursiveHullCheck / SV_HullPointContents.
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
	int16_t  ent;          /* what was hit: a brush model's clip_bmodels
	                          index + 1, 0 the world                   */
	int32_t  plane_d;      /* its distance (x4096, 1/8 units; a brush
	                          model's: in its own space); 0 if nothing hit */
} trace_t;

/* engine/p_defs.inc mirrors this layout for the asm port */
_Static_assert(__builtin_offsetof(trace_t, fraction) == 16, "p_defs.inc TR_FRACTION");
_Static_assert(__builtin_offsetof(trace_t, endpos)   == 20, "p_defs.inc TR_ENDPOS");
_Static_assert(__builtin_offsetof(trace_t, plane_n)  == 32, "p_defs.inc TR_PLANE_N");
_Static_assert(__builtin_offsetof(trace_t, ent)      == 38, "p_defs.inc TR_ENT");
_Static_assert(__builtin_offsetof(trace_t, plane_d)  == 40, "p_defs.inc TR_PLANE_D");
_Static_assert(sizeof(trace_t) == 44, "p_defs.inc TR_SIZE");

/* The solid brush models the traces clip against besides the world
   (Quake's SOLID_BSP entities, SV_ClipToLinks): the game keeps the table
   (engine/ents.c), in spawn order. Origins in 1/CLIP_SUB units. An entry
   with no brush model is a SOLID_BBOX entity's box (absmin / absmax its
   exact box: SV_HullForBox grows it by the mover's). */
typedef struct {
	const qw_bmodel *bm;               /* NULL: a box */
	int32_t          origin[3];
	int32_t          absmin[3];        /* origin + the model's bounds: the box */
	int32_t          absmax[3];        /* test against the swept box           */
} clip_bmodel_t;                       /* 40 bytes: p_defs.inc CB_* */
#define CLIP_MAX_BMODELS 128
extern clip_bmodel_t clip_bmodels[CLIP_MAX_BMODELS];
extern int           clip_num_bmodels;

/* hull 0 = a point (projectiles, shots: the world's own BSP), 1 = player
   size, 2 = large. Returns tr->fraction. Does nothing (and reports a free
   move) when the world has no clip hulls. The PS1 build links the asm port
   (engine/clip.s): positions must fit int16 there, i.e. stay within +-4095
   world units (Quake's own map limit). */
q_fixed clip_trace(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr);

/* Quake CONTENTS_* of hull `hull` at p (the world only, like Quake's
   SV_PointContents). */
int clip_contents(int hull, const int32_t p[3]);

#endif
