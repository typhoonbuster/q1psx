/*
 * q1psx — cooked BSP visibility + collision ("QBS2"), output of `qcook bsp`.
 *
 * The slice of the Quake BSP the runtime needs to (a) cull the world model to
 * what the camera can see — the render node tree, leaves + their surface lists,
 * the RLE-compressed PVS kept exactly as Quake stores it — and (b) collide a
 * box against the world — Quake's pre-expanded clip hulls (`dclipnodes`). Both
 * share the one deduplicated plane table.
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
 * Little-endian; maps straight into PS1 RAM. Every fixed-size section starts
 * 4-byte aligned (qbsp_node is padded to 20 for this; qbsp_clipnode is 6 and
 * only needs 2) so the R3000 can read the structs in place with no bounce copy.
 *
 * Layout:
 *   qbsp_header
 *   qbsp_plane     planes[num_planes]
 *   qbsp_node      nodes[num_nodes]
 *   qbsp_leaf      leaves[num_leaves]       (leaf 0 = the shared CONTENTS_SOLID leaf)
 *   qbsp_clipnode  clipnodes[num_clipnodes] (Quake hulls 1 & 2 share this array)
 *   uint16_t       marksurf[num_marksurf]   (each = QSF1 face index)
 *   uint8_t        vis[vis_len]             (Quake RLE; leaf.visofs is a byte offset in here)
 */

#ifndef Q1PSX_QBSP_H
#define Q1PSX_QBSP_H

#include <stdint.h>

#define QBSP_MAGIC "QBS3"

/* Quake CONTENTS_* — a clipnode child < 0 is one of these, verbatim. */
#define QBSP_CONTENTS_EMPTY  (-1)
#define QBSP_CONTENTS_SOLID  (-2)
#define QBSP_CONTENTS_WATER  (-3)
#define QBSP_CONTENTS_SLIME  (-4)
#define QBSP_CONTENTS_LAVA   (-5)
#define QBSP_CONTENTS_SKY    (-6)

typedef struct {
	char     magic[4];
	uint32_t num_planes;
	uint32_t num_nodes;
	uint32_t num_leaves;    /* including the solid leaf 0                       */
	uint32_t num_visleaves; /* model 0 visleafs — PVS row = (this + 7) >> 3     */
	uint32_t num_marksurf;
	uint32_t num_clipnodes;
	uint32_t vis_len;       /* bytes of RLE vis data; 0 if the map has none     */
	int32_t  root;          /* render head node (always 0 for the world model)  */
	int32_t  hull1_root;    /* clipnode root for the player-size hull           */
	int32_t  hull2_root;    /* clipnode root for the big-monster hull           */
	int16_t  mins[3];       /* world model bounds, for a sane camera start      */
	int16_t  maxs[3];
	int32_t  spawn[3];      /* info_player_start origin (world units)           */
	int32_t  spawn_yaw;     /* info_player_start "angle" (whole degrees)        */
	uint32_t has_spawn;     /* 1 if the entity lump had an info_player_start    */
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

/* Collision-hull node. child >= 0: another clipnode. child < 0: a Quake
   CONTENTS_* value (the leaf's contents) — QBSP_CONTENTS_SOLID stops a trace. */
typedef struct {
	uint16_t plane;
	int16_t  child[2];
} qbsp_clipnode;           /* 6 B */

#endif
