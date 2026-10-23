/*
 * q1psx — loaded world (formats/qworld.h + qtex.h + qgame.h).
 *
 * world_load() uploads the map's texture pages to VRAM (the QTX6 / QTX7 file
 * is transient but for its tables: the texture words, the UV maps, the
 * animated textures), then loads the QWVF world and the map's own models
 * (QMM1) into the level arena, maps their UVs onto the loaded set's tiles
 * and rewrites the world's link fields into absolute pointers for the asm
 * renderer: the nodes' children and face blocks, the leaves' marks and PVS
 * rows, the brush models' roots. Each of the three files comes at the level
 * of its own quality axis (formats/qlevels.h).
 */

#ifndef Q1PSX_WORLD_H
#define Q1PSX_WORLD_H

#include <stdint.h>

#include "qgame.h"
#include "qlevels.h"
#include "qtex.h"
#include "qworld.h"

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
	uint32_t       vis_leaves;      /* PVS rows: leaves 1..vis_leaves  */
	const uint8_t *ambient;         /* [num_leaves][2]: water, sky levels */
	qw_bmodel     *bmodels;         /* [num_bmodels]: 0 = the world (unused);
	                                   roots are KUSEG node pointers (bit 0:
	                                   a leaf), like the nodes' children     */
	uint32_t       num_bmodels;
	const uint8_t *ents;            /* the cooked entity lump, num_ents in its
	                                   compact table (qw_ent_decode)       */
	const uint16_t *ent_names;      /* [QE_COUNT]: each class's classname   */
	uint32_t       num_ents;
	const char    *strings;         /* the entities' strings (offsets)      */
	uint32_t       worldtype;       /* 0 medieval, 1 metal, 2 base          */
	const char    *title;           /* the map's name (worldspawn message)  */
	const char    *map_name;        /* the disc's name: "START", "E1M1"...  */
	qg_model      *models;          /* [num_models]: the map's own (QGF_MAP) */
	uint32_t       num_models;
	const qw_tex **model_tex;       /* [num_models]: each one's texture table */
	uint8_t       *mgeo;            /* their geometry block                  */
	int16_t        model_of[QIM_COUNT];   /* QIM_* -> models index, -1 none  */
	const qw_anim *anims;           /* [num_anims]: the animated textures (anim.c) */
	uint32_t       num_anims;
	uint32_t       max_bm_depth;
	int8_t         level[QL_AXES];  /* the levels loaded: textures, geometry, the
	                                   map's models (QL_*; effects: -1)       */
	/* the VRAM the map's textures take (the streamed skins get the rest,
	   engine/stream.c): the 4-bit set's rows of each 4-bit page (x 320 +
	   64 p, y 0) */
	uint16_t       page4_rows[QTEX_MAP_PAGES];
} world_t;

#ifdef __mips__   /* engine/p_defs.inc mirrors the clip-hull fields (PS1 pointers) */
_Static_assert(__builtin_offsetof(world_t, root)       == 8,  "p_defs.inc W_ROOT");
_Static_assert(__builtin_offsetof(world_t, planes)     == 76, "p_defs.inc W_PLANES");
_Static_assert(__builtin_offsetof(world_t, clipnodes)  == 80, "p_defs.inc W_CLIPNODES");
_Static_assert(__builtin_offsetof(world_t, hull1_root) == 84, "p_defs.inc W_HULL1");
_Static_assert(__builtin_offsetof(world_t, hull2_root) == 88, "p_defs.inc W_HULL2");
#endif

extern world_t world;
extern const char *const level_name[QL_LEVELS];   /* "low" .. "ultra" */

/* Find which Quake maps the disc carries (<MAP>_G<n>.WLD, with its
   <MAP>_T<n>.TX4 and <MAP>_M<n>.MDL); returns how many. Call once after
   cd_init. */
int         world_scan_maps(void);
int         world_num_maps(void);
int         world_level_mask(int axis);   /* QL_TEXTURES .. QL_MODELS: bit n, the
                                             disc's maps come at level n of it */
const char *world_map_name(int map);   /* "START", "E1M1", ... */
int         world_find_map(const char *name);   /* by name, any case; -1 if absent */
int         world_registered(void);    /* the disc carries the registered episodes */

/* Load progress: `done` of `total` bytes (the texture, world and models
   files). Called about once per frame while the CD reads, and once per step
   in between; it may draw and flip a frame (a loading bar). */
typedef void (*world_progress_fn)(uint32_t done, uint32_t total);

/* Load map `map` (0 .. world_num_maps() - 1): its textures at level `tex`
   (the 4-bit set, <MAP>_T<n>.TX4), its world at geometry level `geo`, its
   own models at model level `models`; resets the level arena. 1 on
   success, 0 if missing or too big for the arena. `progress` may be NULL
   (the reads then block). */
int world_load(int map, int tex, int geo, int models, world_progress_fn progress);

/* Leaf containing a point (world units). */
const qw_leaf *world_point_leaf(const int32_t pos[3]);
/* The same descent from a node (or a tagged leaf) world_box_topnode gave,
   for a point inside its box (engine/w_leaf.s; the bench's C:
   world_point_leaf_from_c). */
const qw_leaf *world_point_leaf_from(uint32_t at, const int32_t pos[3]);
#if defined(Q1PSX_BENCH) && !Q1PSX_ENGINE_C
const qw_leaf *world_point_leaf_from_c(uint32_t at, const int32_t pos[3]);
#endif
/* The deepest node (or tagged leaf) holding the whole box (world units,
   inclusive) on its side of every plane above it. */
uint32_t world_box_topnode(const int32_t mins[3], const int32_t maxs[3]);

/* The leaves (indices) a box touches, solid ones left out, up to `max` of
   them (Quake's SV_FindTouchedLeafs); the box in 1/QW_CLIP_SUB units. */
int world_box_leaves(const int32_t mins[3], const int32_t maxs[3], uint16_t *out, int max);

/* an entity's string (its offset in the pool; 0 = "") */
static inline const char *world_string(uint32_t ofs)
{
	return world.strings + ofs;
}

/* a texture rectangle (formats/qtex.h) into VRAM, through a zero-padded
   copy when LoadImage's DMA would send bytes past its data (world.c) */
void vram_upload(const uint8_t *data, const qtex_rect *r);

#endif
