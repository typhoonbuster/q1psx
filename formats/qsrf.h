/*
 * q1psx — cooked map surfaces ("QSF1"), output of `qcook surfaces`.
 *
 * Per world-model BSP face: a convex polygon of shared vertices plus per-vertex
 * texel coordinates and the texture index (== QTX entry index for the same
 * map). The renderer triangulates each face as a fan and draws it textured.
 *
 * Quake guarantees a non-special face's texture footprint is <= 256 texels in
 * each axis, so face-local UVs (texel - floor(min texel)) fit a byte-ish range;
 * they are stored offset so the min is 0 and clamped to 255.
 *
 * Little-endian; maps straight into PS1 RAM.
 *
 * Layout:
 *   qsrf_header
 *   int16_t       verts[num_verts][4]      (SVECTOR each: x,y,z,0 — Quake units)
 *   qsrf_face     faces[num_faces]
 *   qsrf_facevert fv[num_faceverts]
 */

#ifndef Q1PSX_QSRF_H
#define Q1PSX_QSRF_H

#include <stdint.h>

#define QSRF_MAGIC "QSF2"

typedef struct {
	char     magic[4];
	uint32_t num_verts;
	uint32_t num_faces;
	uint32_t num_faceverts;
	int16_t  mins[3], maxs[3];
} qsrf_header;

typedef struct {
	int32_t  dist;       /* plane dist x4096, oriented so the camera sees the
	                        front iff  n[0]*px + n[1]*py + n[2]*pz - dist > 0  */
	int16_t  n[3];       /* outward face normal x4096                          */
	uint16_t tex;        /* miptex / QTX entry index      */
	uint16_t first_fv;   /* index into the facevert array */
	uint8_t  num_fv;     /* polygon vertex count (>= 3)   */
	uint8_t  flags;      /* QSRF_F_*                      */
} qsrf_face;             /* 16 B */

typedef struct {
	uint16_t vert;       /* index into the vertex pool */
	uint8_t  u, v;       /* face-local texel coords    */
} qsrf_facevert;

#define QSRF_F_SKY    (1 << 0)
#define QSRF_F_LIQUID (1 << 1)
#define QSRF_F_MASKED (1 << 2)

#endif
