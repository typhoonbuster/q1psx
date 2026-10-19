/*
 * q1psx — the brush models cut by the world's BSP, as Quake's software
 * renderer draws them (r_bsp.c R_DrawSolidClippedSubmodelPolygons and
 * R_RecursiveClipBPoly): every polygon of a door, a plat, a wall model is
 * cut by the planes of the world's nodes under the model's top node; a
 * piece in a solid leaf is dropped (a door slid into its wall, a plat's
 * sides under the floor), the others are drawn in their own leaf, where
 * the walk's painter's order puts them among the world's faces (a slab
 * over a pit: each piece over the part of the pit behind it). Quake's
 * span sorter needed no more; with no depth buffer, the port draws a
 * leaf's pieces with the record loop in that leaf, and a model's faces
 * facing away from the eye are left out as Quake does.
 *
 * The pieces are kept per model (records in the world's format, their
 * vertices in a pool of their own) and cut again only when the model has
 * moved (a door, a plat while they move).
 */

#ifndef Q1PSX_BMFRAG_H
#define Q1PSX_BMFRAG_H

#include <stdint.h>

#include "ents.h"

/* At a level's load: bmf_level first (this level's caches to make), then,
   once the map's entities are spawned and before the stream manager takes
   the level arena's rest, bmf_reserve (room for the pieces of every model
   the spawned edicts draw, as the cook measured it: the most their cuts
   take where a model stands, all along a mover's travel; a model without
   room keeps the old way: drawn whole in one leaf). After each spawn (the
   load's, a death's): bmf_load, the drawn models cut where they stand. */
void bmf_level(void);
void bmf_reserve(void);
void bmf_load(void);

/* This level's caches by brush model (NULL before the load, or without
   room): a model with one has pieces. Not in the edict, whose size stays
   384 bytes (an index is two shifts). */
typedef struct bmf_cache bmf_cache;
extern bmf_cache **bmf_models;

/* At a new PVS stamp `vis`: is a leaf of the model's pieces in the PVS?
   (exact, where the box's kept leaves are a superset, cut short past 8;
   a model too cut up answers `whole`, its box's leaves' answer) */
int bmf_seen(const bmf_cache *c, uint16_t vis, int whole);

/* Each frame, a drawn brush model with pieces (bmf_models) in the PVS `vis`:
   its pieces facing the eye in their leaves' render lists, cut again if it
   moved. 0: none (too cut up where it stands: the caller draws it whole). */
int bmf_draw(edict_t *e, uint16_t vis);

/* Within a leaf, Quake's z-buffer between an alias model and a door: the
   node of brush model `model`'s BSP whose pieces an rl_add_pieces item
   holds (`recs`: its records; a model bmf_draw drew this frame), and are
   that node's pieces in front of point p (1/8 units), drawn over it (1),
   or behind it, drawn before (0)? */
int bmf_pieces_node(int model, const uint8_t *recs);
int bmf_node_over(int model, int node, const int32_t p[3]);
/* 0: the model cannot hide point p (the segment from the eye to p misses
   its box, grown by 32 units): its pieces all under p */
int bmf_may_hide(int model, const int32_t p[3]);

#ifdef Q1PSX_BENCH
/* this level: models with pieces, pieces made, cuts, cuts given up, the
   cuts' cycles, the dearest cut's */
extern uint32_t bmf_stats[6];
#endif

#endif
