/*
 * q1psx — the render list: what the renderer draws besides the world this
 * frame, and where in the painter's order.
 *
 * An item is a particle (one TILE_1) or a model (an r_ent_t: a frame of a
 * cooked model with its own GTE matrix, lit by its leaf; or a brush model,
 * whose BSP subtree the renderer walks from the model-space camera, its
 * faces lit as cooked). Items are chained
 * per BSP leaf: the leaf's record keeps a list id in the top byte of its
 * `vis` pointer, and the walk (engine/r_world.s draw_ents) draws a leaf's
 * list when it visits the leaf, between the faces behind it and the faces in
 * front of it. The view model is drawn first of all, so it is on top.
 *
 * Within a leaf the list's head is drawn last (the walk emits front to back,
 * each packet prepended). With no depth buffer, the order there stands for
 * Quake's z-buffer: a brush model's pieces (engine/bmfrag.c) keep their
 * model's BSP order; an alias model or a particle goes where its point
 * stands in that order (the pieces in front of it drawn over it, those
 * behind it under it); the alias models among themselves by their depth,
 * the nearest last. A frame adds the brush models' pieces first.
 *
 * Positions are vertex units (world x 4); an item's leaf is the caller's
 * (a particle keeps the leaf it was spawned in).
 */

#ifndef Q1PSX_RLIST_H
#define Q1PSX_RLIST_H

#include <stdint.h>

#include "qgame.h"
#include "qworld.h"

#define RL_MAX_ITEMS 400
#define RL_MAX_ENTS  128
#define RL_MAX_LISTS 127   /* leaves with a list a frame (7 bits of the leaf's vis word) */

#ifndef Q1PSX_ORDER_C
#define Q1PSX_ORDER_C 0    /* an item's place among a leaf's pieces (CMake): 0 the asm
                              (engine/rl_order.s), 1 the C reference, 2 the asm checked by it */
#endif

/* engine/r_defs.inc mirrors both layouts (RI_*, RE_*) */
typedef struct {
	uint16_t next;       /* +0  next item of the leaf's list, index + 1; 0 ends  */
	uint8_t  type;       /* +2  0 particle, 1 model, RI_PIECES a brush model's
	                            pieces (negative as a signed byte: r_world.s)  */
	uint8_t  colour;     /* +3  particle: palette index; pieces: their brush model */
	int16_t  x, y, z;    /* +4  particle: vertex units; model: x its depth (2
	                            world units); pieces, once an alias model or a
	                            particle came to their leaf (rlist.c): x their
	                            node in their model's BSP, z the leaf's next
	                            pieces item (index + 1)                        */
	uint16_t pad;        /* +10 pieces: their records' bytes                     */
	uint32_t ent;        /* +12 model: the r_ent_t; pieces: their first record   */
} r_item_t;              /* 16 bytes */

typedef struct {
	int16_t  rot[10];    /* +0  GTE rotation: model -> view, x R_MATRIX_ONE      */
	int32_t  tr[3];      /* +20 GTE translation, vertex units                    */
	uint32_t pool;       /* +32 the frame's vertex pool (qw_vert[])              */
	uint32_t recs;       /* +36 first record, and                                */
	uint32_t recs_end;   /* +40 the end of the records to draw (RE_BMODEL: recs
	                             is the model's root node, recs_end unused)    */
	uint32_t tex;        /* +44 qw_tex entry: the skin (RE_BMODEL: the map's table) */
	uint32_t dots;       /* +48 light table (QG_NORMALS bytes) or 0: flat        */
	uint32_t light;      /* +52 the leaf's light, 0..170 (x1.5 stays in a byte)  */
	uint32_t flags;      /* +56 RE_VIEW: the view model: no warp subdivision (the
	                             cook cut it), the near clip at 5 units;
	                             RE_BMODEL: a brush model: its subtree walked   */
	union {
		int32_t  cam[3];     /* +60 RE_BMODEL: the camera in the model's space,
		                             vertex units (the walk's plane tests)         */
		struct {
			uint32_t lod_band;   /* +60 RE_SORT: the level of detail's band x 2 (its
			                         entry in each record's table, formats/qstream.h
			                         QST_LOD_REC_BYTES; 0: the whole model), the
			                         pool's entries in the upper half (light_pool) */
			int16_t  water_z;    /* +64 RE_WATER: the liquid's surface in the
			                         model's own z (its vertex pool's): the corners
			                         on the side of it the eye is not on hidden    */
			int16_t  water_m;    /* +66 RE_WATER: 1 the part above it shown (the
			                         eye out of the liquid), -1 the part below (so
			                         the word is never 0: SP_WATER's "none")       */
		};
	};
	uint32_t zsf;        /* +72 RE_SORT: the GTE's ZSF3 for its triangles'
	                             buckets (AVSZ3's OTZ / 4: its depth range on
	                             R_SORT_N - 2 of them)                          */
	uint32_t znear;      /* +76 RE_SORT: its nearest bucket x 4 (the table is
	                             a ring: the range starts anywhere on it)       */
} r_ent_t;               /* 80 bytes */

#define RI_PIECES 0x80

#define RE_VIEW   1
#define RE_BMODEL 2
#define RE_SORT   4      /* a monster (QSTM_SORT): its triangles drawn far to near
                            through its own ordering table (r_world.s draw_model) */
#define R_SORT_N  16     /* that table's buckets (r_defs.inc R_SORT_N): a power of two,
                            4 or more (dm_sort sets the ring four nodes a round) */
#define RE_WATER  8      /* a monster through a liquid's surface (RE_SORT too): its
                            triangles on the side the eye is not on left out, those
                            through the surface cut by it (water_z, water_m;
                            r_world.s ds_water, the slow path's clip) */

_Static_assert(sizeof(r_item_t) == 16, "r_defs.inc RI_*");
_Static_assert(sizeof(r_ent_t) == 80 && __builtin_offsetof(r_ent_t, cam) == 60 &&
               __builtin_offsetof(r_ent_t, lod_band) == 60 && __builtin_offsetof(r_ent_t, water_z) == 64 &&
               __builtin_offsetof(r_ent_t, water_m) == 66 && __builtin_offsetof(r_ent_t, zsf) == 72,
               "r_defs.inc RE_*");

extern r_item_t r_items[RL_MAX_ITEMS];
extern uint16_t r_lists[256];           /* list id -> first item + 1; the upper
                                           half rlist.c's (its pieces' heads) */

/* per frame: before anything is added */
void rl_begin(void);

#ifdef Q1PSX_BENCH
extern int rl_lod_cut;      /* this frame's streamed models cut to a band past 0 */
extern int rl_lod_band_hi;  /* the farthest band among them */
#endif
extern int rl_lod_level;    /* the bodies' shared level this frame (Q1PSX_LOD_BUDGET) */
extern int rl_lod_raised;   /* the bodies it raised past their depth's band this frame */

/* an entity for a model's frame, positioned (1/R_POS_SUB world units) and
   oriented (binary degrees, Quake's entity angles: pitch, yaw, roll), lit
   by `light` (a leaf's); NULL when the pool is full. `records` draws the
   whole model, or with QGF_SPRITE the frame's own record. */
r_ent_t *rl_model(const qg_model *m, int frame, const int32_t org[3], int yaw, int pitch,
                  int roll, int light);

/* a streamed model's frame (engine/stream.h: asset a, resident), the same
   way; NULL when it is not resident or the pool is full */
r_ent_t *rl_model_stream(int a, int frame, const int32_t org[3], int yaw, int pitch, int roll, int light);

/* a monster's (rl_model_stream's, RE_SORT, no pitch nor roll) through a
   liquid's surface at height h (1/R_POS_SUB units): cut by it, the part on
   the eye's side shown (above: the eye out of the liquid) */
void rl_water(r_ent_t *e, int a, const int32_t org[3], int32_t h, int above);

/* the view model (QGF_VIEW) at frame `frame`, the gun's angles (rl_model's
   yaw and pitch), its origin `rel` from the camera's (world axes, vertex
   units x 65536), lit by `light`; NULL when the pool is full */
r_ent_t *rl_view_gun(const qg_model *m, int frame, int yaw, int pitch, const int32_t rel[3], int light);

/* a brush model at `org` (1/R_POS_SUB units; its brushes are in world
   space, so a static one is at 0), unrotated; NULL when the pool is full */
r_ent_t *rl_bmodel(const qw_bmodel *bm, const int32_t org[3]);

/* chain an entity / a particle into a leaf's list (nothing if the leaf is
   NULL or the item pool is full): an alias model at `org` (1/R_POS_SUB
   units; `box`, when given: its mins then maxs, 6 words, an edict's: the
   box's centre then) among the leaf's other models by its depth (its
   matrix's) and among its brush models' pieces by that point; a brush
   model drawn whole with `org` NULL */
void rl_add_model(const qw_leaf *leaf, r_ent_t *e, const int32_t org[3], const int32_t *box);
void rl_add_particle(const qw_leaf *leaf, const int32_t pos[3], int colour);   /* 1/R_POS_SUB units */
/* a brush model's pieces in that leaf (bmfrag.c): `len` bytes of records,
   their vertices in r_state.fragpool, drawn in the world's space; `tag`
   RI_PIECES | their brush model << 8 (a brush model under 256) */
void rl_add_pieces(const qw_leaf *leaf, const uint8_t *recs, uint32_t len, uint32_t tag);
/* the brush models are in (all of them: before any other item) */
void rl_pieces_done(void);

/* after the frame is rendered: clears the leaves' list ids */
void rl_end(void);

int rl_num_items(void);
int rl_num_lists(void);     /* (the last frame's leaves with a list, of RL_MAX_LISTS) */

#ifdef Q1PSX_BENCH
/* the last debug frame's (ents_debug) places taken among pieces: an "order"
   line, their count and cycles; ORDER_C=2, an "ordasm" line: the asm's
   places checked against the C's, and the mismatches */
void rl_order_print(const char *name);
#if Q1PSX_ORDER_C == 2
/* (a debug frame, its items all in) made-up places asked of every list
   with pieces, each checked and put back: the frame unchanged */
void rl_order_probe(void);
#endif
#endif

#endif
