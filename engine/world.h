/*
 * q1psx — loaded world (formats/qworld.h + qtex.h).
 *
 * world_load() uploads the map's texture pages to VRAM (the QTX3 / QTX4 file
 * is only transient), then loads the QWV2 world into the level arena and rewrites its
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
	const qw_tex  *textures;        /* [num_tex]: CLUT, tpage, window */
	uint32_t       num_tex;
	const qw_vert *verts;           /* [num_verts]: the vertex pool   */
	uint32_t       num_verts;
	int            tex4;            /* the 4-bit textures are loaded  */
	uint32_t       vis_leaves;      /* PVS rows: leaves 1..vis_leaves  */
} world_t;

#ifdef __mips__   /* engine/p_defs.inc mirrors the clip-hull fields (PS1 pointers) */
_Static_assert(__builtin_offsetof(world_t, root)       == 8,  "p_defs.inc W_ROOT");
_Static_assert(__builtin_offsetof(world_t, planes)     == 76, "p_defs.inc W_PLANES");
_Static_assert(__builtin_offsetof(world_t, clipnodes)  == 80, "p_defs.inc W_CLIPNODES");
_Static_assert(__builtin_offsetof(world_t, hull1_root) == 84, "p_defs.inc W_HULL1");
_Static_assert(__builtin_offsetof(world_t, hull2_root) == 88, "p_defs.inc W_HULL2");
#endif

extern world_t world;
extern const char *const fidelity_name[FIDELITY_LEVELS];

/* Find which Quake maps the disc carries (<MAP>_<fidelity>.TEX / .WLD);
   returns how many. Call once after cd_init. */
int         world_scan_maps(void);
int         world_num_maps(void);
const char *world_map_name(int map);   /* "START", "E1M1", ... */

/* Load progress: `done` of `total` bytes (texture file + world file). Called
   about once per frame while the CD reads, and once per step in between; it
   may draw and flip a frame (a loading bar). */
typedef void (*world_progress_fn)(uint32_t done, uint32_t total);

/* Load map `map` (0 .. world_num_maps() - 1) at a fidelity level, with
   its 4-bit textures (<MAP>_<N>.TX4) if `tex4` and the disc has them, else
   the 8-bit ones; resets the level arena. 1 on success, 0 if missing or too
   big for the arena. `progress` may be NULL (the reads then block). */
int world_load(int map, int fidelity, int tex4, world_progress_fn progress);

/* Leaf containing a point (world units). */
const qw_leaf *world_point_leaf(const int32_t pos[3]);

#endif
