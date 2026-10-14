/*
 * q1psx — loaded world (formats/qworld.h + qtex.h).
 *
 * world_load() uploads the map's texture pages to VRAM (the QTX3 file is only
 * transient), then loads the QWV2 world into the level arena and rewrites its
 * link fields into absolute pointers for the asm renderer.
 */

#ifndef Q1PSX_WORLD_H
#define Q1PSX_WORLD_H

#include <stdint.h>

#include "qworld.h"

#define FIDELITY_LEVELS 4   /* 0 low, 1 medium, 2 high, 3 ultra */

typedef struct {
	qw_node       *nodes;
	qw_leaf       *leaves;
	qw_node       *root;
	uint16_t      *marks;
	uint16_t      *face_visframe;   /* [num_faces], runtime stamps    */
	const uint8_t *faces;           /* face record stream             */
	const qw_view *views;
	uint32_t       num_nodes, num_leaves, num_faces, num_views;
	uint32_t       num_records, num_pieces;
	uint32_t       vis_rowbytes;
	int32_t        spawn[3], spawn_yaw;
	int            has_spawn;
	const qw_plane    *planes;      /* clip hulls (NULL if absent) */
	const qw_clipnode *clipnodes;
	int32_t        hull1_root, hull2_root;
} world_t;

#ifdef __mips__   /* engine/p_defs.inc mirrors the clip-hull fields (PS1 pointers) */
_Static_assert(__builtin_offsetof(world_t, planes)     == 76, "p_defs.inc W_PLANES");
_Static_assert(__builtin_offsetof(world_t, clipnodes)  == 80, "p_defs.inc W_CLIPNODES");
_Static_assert(__builtin_offsetof(world_t, hull1_root) == 84, "p_defs.inc W_HULL1");
_Static_assert(__builtin_offsetof(world_t, hull2_root) == 88, "p_defs.inc W_HULL2");
#endif

extern world_t world;
extern const char *const fidelity_name[FIDELITY_LEVELS];

/* Load START<fidelity>.TEX + .WLD; resets the level arena. 1 on success. */
int world_load(int fidelity);

/* Leaf containing a point (world units). */
const qw_leaf *world_point_leaf(const int32_t pos[3]);

#endif
