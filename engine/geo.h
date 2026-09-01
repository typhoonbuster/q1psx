/*
 * q1psx — cooked map geometry (QGE1) in RAM.
 *
 * Phase 5.1b: just vertices + edges for the wireframe. Grows into the real
 * BSP (surfaces, texinfo, nodes, PVS) as the renderer does.
 */

#pragma once

#include <stdint.h>
#include <psxgte.h>

typedef struct {
	int             num_verts;
	int             num_edges;
	const SVECTOR  *verts;    /* into the loaded blob */
	const uint16_t *edges;    /* 2 * num_edges indices */
	int16_t         mins[3], maxs[3];
} geo_t;

/* Parse a QGE1 blob already in RAM. Returns 1 on success. */
int geo_load(const void *blob, int len, geo_t *out);
