/*
 * q1psx — cooked map geometry ("QGE1"), output of `qcook geometry`.
 *
 * Phase 5.1b: just what a wireframe needs — the BSP's world vertices (as PS1
 * SVECTORs, integer Quake world units: X fwd, Y left, Z up) and the unique
 * edges used by the world model's faces. Surfaces, texinfo, lightmaps, PVS and
 * the node/leaf tree come as the renderer grows.
 *
 * Little-endian; maps straight into RAM on the PS1 (also LE).
 *
 * Layout:
 *   qgeo_header
 *   int16_t verts[num_verts][4]   (x, y, z, 0 — an SVECTOR each)
 *   uint16_t edges[num_edges][2]  (vertex indices)
 */

#ifndef Q1PSX_QGEO_H
#define Q1PSX_QGEO_H

#include <stdint.h>

#define QGEO_MAGIC "QGE1"

typedef struct {
	char     magic[4];
	uint32_t num_verts;
	uint32_t num_edges;
	int16_t  mins[3];      /* world model bounds, for a sane camera start */
	int16_t  maxs[3];
} qgeo_header;

#endif
