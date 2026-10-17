/*
 * q1psx — the render list: what the renderer draws besides the world this
 * frame, and where in the painter's order.
 *
 * An item is a particle (one TILE_1) or a model (an r_ent_t: a frame of a
 * cooked model with its own GTE matrix, lit by its leaf). Items are chained
 * per BSP leaf: the leaf's record keeps a list id in the top byte of its
 * `vis` pointer, and the walk (engine/r_world.s draw_ents) draws a leaf's
 * list when it visits the leaf, between the faces behind it and the faces in
 * front of it. The view model is drawn first of all, so it is on top.
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
#define RL_MAX_ENTS   64
#define RL_MAX_LISTS 255

/* engine/r_defs.inc mirrors both layouts (RI_*, RE_*) */
typedef struct {
	uint16_t next;       /* +0  next item of the leaf's list, index + 1; 0 ends  */
	uint8_t  type;       /* +2  0 particle, 1 model                              */
	uint8_t  colour;     /* +3  particle: palette index                          */
	int16_t  x, y, z;    /* +4  particle: vertex units                           */
	uint16_t pad;        /* +10                                                  */
	uint32_t ent;        /* +12 model: the r_ent_t                               */
} r_item_t;              /* 16 bytes */

typedef struct {
	int16_t  rot[10];    /* +0  GTE rotation: model -> view, x R_MATRIX_ONE      */
	int32_t  tr[3];      /* +20 GTE translation, vertex units                    */
	uint32_t pool;       /* +32 the frame's vertex pool (qw_vert[])              */
	uint32_t recs;       /* +36 first record, and                                */
	uint32_t recs_end;   /* +40 the end of the records to draw                   */
	uint32_t tex;        /* +44 qw_tex entry: the skin                           */
	uint32_t dots;       /* +48 light table (QG_NORMALS bytes) or 0: flat        */
	uint32_t light;      /* +52 the leaf's light, 0..170 (x1.5 stays in a byte)  */
	uint32_t flags;      /* +56 RE_VIEW: the view model: no warp subdivision (its
	                             polygons are small), the near clip at 8 units */
	uint32_t pad;
} r_ent_t;               /* 64 bytes */

#define RE_VIEW 1

_Static_assert(sizeof(r_item_t) == 16, "r_defs.inc RI_*");
_Static_assert(sizeof(r_ent_t) == 64, "r_defs.inc RE_*");

extern r_item_t r_items[RL_MAX_ITEMS];
extern uint16_t r_lists[256];           /* list id -> first item + 1 */

/* per frame: before anything is added */
void rl_begin(void);

/* an entity for a model's frame, positioned (1/R_POS_SUB world units) and
   oriented (binary degrees, Quake's entity angles: pitch, yaw, roll), lit
   by `light` (a leaf's); NULL when the pool is full. `records` draws the
   whole model, or with QGF_SPRITE the frame's own record. */
r_ent_t *rl_model(const qg_model *m, int frame, const int32_t org[3], int yaw, int pitch,
                  int roll, int light);

/* the view model: its x and y (screen) rows and translation x4, the
   renderer's H / 4 (RE_VIEW: the GTE then projects down to 5 units) */
void rl_view_model(r_ent_t *e);

/* chain an entity / a particle into a leaf's list (nothing if the leaf is
   NULL or the item pool is full) */
void rl_add_model(const qw_leaf *leaf, r_ent_t *e);
void rl_add_particle(const qw_leaf *leaf, const int32_t pos[3], int colour);   /* 1/R_POS_SUB units */

/* after the frame is rendered: clears the leaves' list ids */
void rl_end(void);

int rl_num_items(void);

#endif
