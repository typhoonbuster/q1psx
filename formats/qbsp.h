/*
 * q1psx — cooked BSP visibility ("QBS1"), output of `qcook bsp`.
 *
 * The slice of the Quake BSP the runtime needs to cull the world model to what
 * the camera can see: the node tree (for "which leaf am I in" + front-to-back
 * traversal), the leaves (bounds + their surface lists), the RLE-compressed PVS
 * kept exactly as Quake stores it (decompressed one row at a time on the PS1),
 * and a deduplicated plane table.
 *
 * Only the world model (model 0). Its faces are 1:1 with the QSF1 `faces[]`
 * array for the same map, so `marksurf[]` values index straight into it.
 *
 * Fixed-point: plane normals are unit vectors x4096 (int16, 20.12-compatible),
 * plane distances are world units x4096 (int32). A point `p` (integer world
 * units) is on the positive side of a plane when
 *   n[0]*p[0] + n[1]*p[1] + n[2]*p[2] - dist >= 0
 * (accumulate in int64).
 *
 * Little-endian; maps straight into PS1 RAM. Every section starts 4-byte
 * aligned (qbsp_node is padded to 20 for this) so the R3000 can read the
 * structs in place with no bounce copy.
 *
 * Layout:
 *   qbsp_header
 *   qbsp_plane  planes[num_planes]
 *   qbsp_node   nodes[num_nodes]
 *   qbsp_leaf   leaves[num_leaves]      (leaf 0 = the shared CONTENTS_SOLID leaf)
 *   uint16_t    marksurf[num_marksurf]  (each = QSF1 face index)
 *   uint8_t     vis[vis_len]            (Quake RLE; leaf.visofs is a byte offset in here)
 */

#ifndef Q1PSX_QBSP_H
#define Q1PSX_QBSP_H

#include <stdint.h>

#define QBSP_MAGIC "QBS1"

typedef struct {
	char     magic[4];
	uint32_t num_planes;
	uint32_t num_nodes;
	uint32_t num_leaves;    /* including the solid leaf 0                       */
	uint32_t num_visleaves; /* model 0 visleafs — PVS row = (this + 7) >> 3     */
	uint32_t num_marksurf;
	uint32_t vis_len;       /* bytes of RLE vis data; 0 if the map has none     */
	int32_t  root;          /* head node index (always 0 for the world model)   */
	int16_t  mins[3];       /* world model bounds, for a sane camera start      */
	int16_t  maxs[3];
} qbsp_header;

/* type: 0/1/2 = axial (X/Y/Z), 3 = non-axial — axial lets the side test skip
   the dot product and stay exact. */
typedef struct {
	int16_t  n[3];          /* unit normal x4096 */
	uint8_t  type;
	uint8_t  pad;
	int32_t  dist;          /* world units x4096 */
} qbsp_plane;               /* 12 B */

typedef struct {
	int16_t  child[2];      /* >= 0: node index;  < 0: leaf index = -(child) - 1 */
	uint16_t plane;
	int16_t  mins[3];       /* int16 world units — frustum culling */
	int16_t  maxs[3];
	int16_t  pad;           /* keep the following section 4-byte aligned */
} qbsp_node;                /* 20 B */

typedef struct {
	int32_t  visofs;        /* byte offset into vis[]; -1 = no vis info      */
	int16_t  mins[3];
	int16_t  maxs[3];
	uint16_t first_marksurf;
	uint16_t num_marksurf;
	int32_t  contents;      /* Quake CONTENTS_* (negative)                   */
} qbsp_leaf;               /* 24 B */

#endif
