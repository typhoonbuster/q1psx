/*
 * q1psx — cooked BSP visibility + collision (QBS2) in RAM. See formats/qbsp.h.
 *
 * Enough of the world BSP to answer "which leaf is the camera in" and "walk the
 * world front-to-back, skipping what the PVS says is invisible" (render side,
 * r_world), plus Quake's pre-expanded clip hulls for sweeping a box against the
 * world (bsp_hull_trace, used by player movement).
 */

#pragma once

#include <stdint.h>

#include "qbsp.h"     /* formats/qbsp.h */
#include "q_fixed.h"

typedef struct {
	int                   num_planes;
	int                   num_nodes;
	int                   num_leaves;
	int                   num_visleaves;
	int                   num_marksurf;
	int                   num_clipnodes;
	int                   root;
	int                   hull1_root;
	int                   hull2_root;
	uint32_t              vis_len;
	const qbsp_plane     *planes;
	const qbsp_node      *nodes;
	const qbsp_leaf      *leaves;
	const qbsp_clipnode  *clipnodes;
	const uint16_t       *marksurf;
	const uint8_t        *vis;
	int16_t               mins[3], maxs[3];
} bsp_t;

/* Result of sweeping a point through a clip hull. */
typedef struct {
	int      startsolid;   /* the trace began inside solid            */
	int      allsolid;     /* the whole trace was inside solid        */
	int      inopen;
	int      inwater;
	q_fixed  fraction;     /* 0..Q_FIX_ONE; ONE = reached `end`       */
	int32_t  endpos[3];    /* world units                            */
	int16_t  plane_n[3];   /* normal of the surface hit (x4096)      */
	int32_t  plane_d;      /* its distance (x4096); 0 if nothing hit */
} trace_t;

/* Point QBS1 pointers into `blob` (kept alive by the caller). 1 on success. */
int bsp_load(const void *blob, int len, bsp_t *out);

/* Leaf index for a world-space integer point. 0 = solid. */
int bsp_point_leaf(const bsp_t *b, const int32_t p[3]);

/* Which side of a cooked plane a point is on: 0 = front (positive), 1 = back.
   (Shared with the render-side world walk.) */
int bsp_plane_side(const qbsp_plane *pl, const int32_t p[3]);

/* Sweep the point `start` -> `end` (integer world units) through Quake clip
   hull `hull` (1 = player-size box, 2 = big-monster box). Fills *tr and
   returns tr->fraction (Q_FIX_ONE = the move is unobstructed). */
q_fixed bsp_hull_trace(const bsp_t *b, int hull,
                       const int32_t start[3], const int32_t end[3], trace_t *tr);

/* Decompressed PVS bitset for `leaf` (bit i set => leaf i+1 is visible).
   Cached: cheap to call every frame. When `leaf` is solid/no-vis the last good
   set is kept (a debug fly-cam clips into walls constantly). */
const uint8_t *bsp_pvs(const bsp_t *b, int leaf);

/* Bytes per PVS row. */
int bsp_pvs_row(const bsp_t *b);
