/*
 * q1psx — collision against the world's clip hulls (Quake hulls 1 and 2).
 *
 * clip_trace() sweeps a hull-sized box (represented, as in Quake, by a point
 * against the pre-expanded hull) from start to end and reports the first
 * impact: against the world, then against the solid brush models the game
 * lists (clip_bmodels: doors, plats, walls), each in its own space, the
 * nearest kept (SV_Move + SV_ClipToLinks). Fixed-point port of
 * SV_RecursiveHullCheck / SV_HullPointContents.
 * Positions are in 1/CLIP_SUB world units (sub-unit precision matters:
 * with whole units, sliding along a gentle ramp rounded every bump to zero
 * and the player stuck at its foot). Plane normals / distances are x4096.
 */

#ifndef Q1PSX_CLIP_H
#define Q1PSX_CLIP_H

#include <stdint.h>

#include "q_fixed.h"
#include "qworld.h"

#define CLIP_SUB QW_CLIP_SUB   /* position sub-units per world unit (8) */

typedef struct {
	int      startsolid;   /* the trace began inside solid            */
	int      allsolid;     /* the whole trace was inside solid        */
	int      inopen;
	int      inwater;
	q_fixed  fraction;     /* 0..Q_FIX_ONE; ONE = reached `end`       */
	int32_t  endpos[3];    /* 1/CLIP_SUB world units                 */
	int16_t  plane_n[3];   /* normal of the surface hit (x4096)      */
	int16_t  ent;          /* what was hit: a brush model's clip_bmodels
	                          index + 1, 0 the world                   */
	int32_t  plane_d;      /* its distance (x4096, 1/8 units; a brush
	                          model's: in its own space); 0 if nothing hit */
} trace_t;

/* engine/p_defs.inc mirrors this layout for the asm port */
_Static_assert(__builtin_offsetof(trace_t, fraction) == 16, "p_defs.inc TR_FRACTION");
_Static_assert(__builtin_offsetof(trace_t, endpos)   == 20, "p_defs.inc TR_ENDPOS");
_Static_assert(__builtin_offsetof(trace_t, plane_n)  == 32, "p_defs.inc TR_PLANE_N");
_Static_assert(__builtin_offsetof(trace_t, ent)      == 38, "p_defs.inc TR_ENT");
_Static_assert(__builtin_offsetof(trace_t, plane_d)  == 40, "p_defs.inc TR_PLANE_D");
_Static_assert(sizeof(trace_t) == 44, "p_defs.inc TR_SIZE");

/* The solid brush models the traces clip against besides the world
   (Quake's SOLID_BSP entities, SV_ClipToLinks): the game keeps the table
   (engine/ents.c), in spawn order. Origins in 1/CLIP_SUB units. An entry
   with no brush model is a SOLID_BBOX entity's box (absmin / absmax its
   exact box: SV_HullForBox grows it by the mover's). Each entry is linked
   in an area node's list (below): the traces try the lists of the nodes
   the swept box reaches, in Quake's order. */
typedef struct {
	const qw_bmodel *bm;               /* NULL: a box, CLIP_BM_MONSTER: a monster's */
	int32_t          origin[3];
	int32_t          absmin[3];        /* origin + the model's bounds: the box */
	int32_t          absmax[3];        /* test against the swept box           */
	uint16_t         next;             /* its area's list: the next entry, CLIP_LINK, 0 the end */
	uint16_t         prev;             /* the one before, 0 none (the C side's) */
	uint8_t          area;             /* the area node it is linked in, CLIP_NO_AREA none */
	uint8_t          self;             /* its index + 1 (what a trace hits) */
	uint16_t         pad;
} clip_bmodel_t;                       /* 48 bytes: p_defs.inc CB_* */
#define CLIP_BM_MONSTER  ((const qw_bmodel *) 1)   /* an FL_MONSTER's box: MOVE_MISSILE grows it */
#define CLIP_MAX_BMODELS 128
extern clip_bmodel_t clip_bmodels[CLIP_MAX_BMODELS];
extern int           clip_num_bmodels;
/* Entry 0 is the player's box (engine/ents.c keeps it): the player's own
   traces (clip_trace: pmove, its shots) pass it, an entity's
   (clip_trace_ent) meet it. */

/* a list link: entry i as (i + 1) x its size, the byte offset from the
   entry before the table (the asm adds it to that address); 0 none */
#define CLIP_LINK(i)    ((uint16_t) (((i) + 1) * sizeof (clip_bmodel_t)))
#define CLIP_ENTRY(l)   (&clip_bmodels[(l) / sizeof (clip_bmodel_t) - 1])
#define CLIP_NO_AREA    0xff

/* Quake's area nodes (SV_CreateAreaNode, AREA_DEPTH 4): the world's box
   halved four times, each time across its longer side (x or y), 31 nodes.
   Node i's children are i + 1 (its upper half, Quake's children[0]) and
   child1 (the lower one): numbered in the order of Quake's depth-first
   walks (SV_ClipToLinks, SV_TouchLinks), a walk is the nodes it reaches in
   index order. An entity is linked (SV_LinkEdict, appended) in the
   deepest node whose split its box does not straddle. */
#define CLIP_AREA_DEPTH 4
#define CLIP_AREAS      31
typedef struct {
	int16_t  dist;         /* the split, 1/CLIP_SUB units (int16: a map within
	                          +-4095 units, as the traces need) */
	uint8_t  flags;        /* CLIP_AREA_*: its axis, a leaf, its children's
	                          subtrees holding solid entries (the walks skip
	                          the empty ones) */
	uint8_t  child1;       /* children[1] (children[0] is the next node) */
	uint16_t head;         /* its solid entries' list: the first one, CLIP_LINK, 0 none */
	uint16_t tail;         /* the last one */
} clip_area_t;             /* 8 bytes, its first word read at once: p_defs.inc CA_* */
#define CLIP_AREA_Y     0x01   /* the split's axis: y (else x) */
#define CLIP_AREA_C0    0x02   /* children[0]'s subtree holds solid entries */
#define CLIP_AREA_C1    0x04   /* children[1]'s */
#define CLIP_AREA_LEAF  0x80   /* no split, no children */
extern clip_area_t clip_areas[CLIP_AREAS];

/* The entity clip_trace_ent moves (SV_Move's mins, maxs, passedict and
   type): its own box, the entry it never hits (itself, or its owner for a
   missile), MOVE_NOMONSTERS (the boxes left out: only the world and the
   brush models clip) and MOVE_MISSILE (the monsters' boxes grown by 15
   units each way instead of by the mover's box, SV_Move's mins2 / maxs2,
   the sweep's bounds too). */
typedef struct {
	int32_t  box[6];       /* mins[3], maxs[3], 1/CLIP_SUB units          */
	int32_t  pass;         /* the clip_bmodels index + 1 it skips, 0 none  */
	int32_t  nomonsters;
	int32_t  missile;
	uint32_t root0;        /* the next hull-0 sweep's walk of the world
	                          starts at this node (clip_box_node0's, the
	                          sweep inside its box), 0 the root; the sweep
	                          clears it */
	int32_t  sight;        /* the next sweep only asks whether it is clear
	                          (visible's: its fraction 1, the contents it
	                          crossed): an impact is left where found (no
	                          backing out of residual solid), and while the
	                          sweep is blocked only the entries holding its
	                          start are tried (one that starts it in solid
	                          can clear it again: SV_ClipToLinks takes a
	                          trace starting inside, whatever its fraction);
	                          the sweep clears it */
} clip_mover_t;            /* engine/p_defs.inc MV_* */
_Static_assert(__builtin_offsetof(clip_mover_t, pass) == 24, "p_defs.inc MV_PASS");
_Static_assert(__builtin_offsetof(clip_mover_t, nomonsters) == 28, "p_defs.inc MV_NOMON");
_Static_assert(__builtin_offsetof(clip_mover_t, missile) == 32, "p_defs.inc MV_MISSILE");
_Static_assert(__builtin_offsetof(clip_mover_t, root0) == 36, "p_defs.inc MV_ROOT0");
_Static_assert(__builtin_offsetof(clip_mover_t, sight) == 40, "p_defs.inc MV_SIGHT");
_Static_assert(sizeof(clip_mover_t) == 44, "clip.s clip_mover's .space");
_Static_assert(sizeof(clip_bmodel_t) == 48, "p_defs.inc CB_SIZE");
_Static_assert(__builtin_offsetof(clip_bmodel_t, next) == 40, "p_defs.inc CB_NEXT");
_Static_assert(__builtin_offsetof(clip_bmodel_t, self) == 45, "p_defs.inc CB_SELF");
_Static_assert(sizeof(clip_area_t) == 8, "p_defs.inc CA_SIZE");
_Static_assert(__builtin_offsetof(clip_area_t, flags) == 2, "p_defs.inc CA_SIZE");
_Static_assert(__builtin_offsetof(clip_area_t, head) == 4, "p_defs.inc CA_HEAD");
extern clip_mover_t clip_mover;

/* hull 0 = a point (projectiles, shots: the world's own BSP), 1 = player
   size, 2 = large. Returns tr->fraction. Does nothing (and reports a free
   move) when the world has no clip hulls. The PS1 build links the asm port
   (engine/clip.s): positions must fit int16 there, i.e. stay within +-4095
   world units (Quake's own map limit). */
q_fixed clip_trace(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr);

/* SV_Move for clip_mover: the world's hull `hull` (the one of its size:
   SV_HullForEntity's), the brush models' too, every box (the player's
   first) grown by its own box (a monster's by 15 for a missile), but its
   pass entry; the boxes left out with nomonsters. */
q_fixed clip_trace_ent(int hull, const int32_t start[3], const int32_t end[3], trace_t *tr);

/* Quake CONTENTS_* of hull `hull` at p (the world only, like Quake's
   SV_PointContents). */
int clip_contents(int hull, const int32_t p[3]);

/* Hull-0 walks from a node instead of the root, for points and sweeps
   within one box (SV_CheckBottom's corners and its traces down): one walk
   from the root to the deepest node whose region holds the whole box
   (clip_box_node0: every plane above it leaves the box on one side, by
   the walks' own test, n.p - dist >= 0 the front), theirs from there: the
   same leaves, the same results. The node as the walks hold it (a KUSEG
   pointer, bit 0 set a leaf), 0 without a world. The box must fit int16
   (1/CLIP_SUB units), its extents too. */
uint32_t clip_box_node0(const int32_t mins[3], const int32_t maxs[3]);
/* clip_contents(0, p) for a point p of that box */
int clip_contents_node0(uint32_t node, const int32_t p[3]);

#endif
