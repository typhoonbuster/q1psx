/*
 * q1psx — cooked BSP visibility (QBS1) in RAM. See formats/qbsp.h.
 *
 * Just enough of the world BSP to answer "which leaf is the camera in" and
 * "walk the world front-to-back, skipping what the PVS says is invisible".
 * The render side (r_world, engine/render.c) drives the walk.
 */

#pragma once

#include <stdint.h>

#include "qbsp.h"   /* formats/qbsp.h */

typedef struct {
	int                num_planes;
	int                num_nodes;
	int                num_leaves;
	int                num_visleaves;
	int                num_marksurf;
	int                root;
	uint32_t           vis_len;
	const qbsp_plane  *planes;
	const qbsp_node   *nodes;
	const qbsp_leaf   *leaves;
	const uint16_t    *marksurf;
	const uint8_t     *vis;
	int16_t            mins[3], maxs[3];
} bsp_t;

/* Point QBS1 pointers into `blob` (kept alive by the caller). 1 on success. */
int bsp_load(const void *blob, int len, bsp_t *out);

/* Leaf index for a world-space integer point. 0 = solid. */
int bsp_point_leaf(const bsp_t *b, const int32_t p[3]);

/* Which side of a cooked plane a point is on: 0 = front (positive), 1 = back.
   (Shared with the render-side world walk.) */
int bsp_plane_side(const qbsp_plane *pl, const int32_t p[3]);

/* Decompressed PVS bitset for `leaf` (bit i set => leaf i+1 is visible).
   Cached: cheap to call every frame. When `leaf` is solid/no-vis the last good
   set is kept (a debug fly-cam clips into walls constantly). */
const uint8_t *bsp_pvs(const bsp_t *b, int leaf);

/* Bytes per PVS row. */
int bsp_pvs_row(const bsp_t *b);
