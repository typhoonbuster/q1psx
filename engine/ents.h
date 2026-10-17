/*
 * q1psx — the map's entities: Quake's edicts, native (docs/porting-notes.md
 * §28). The cooked entity table (formats/qworld.h qw_ent) is spawned at the
 * map's load into a pool of edicts in the level arena, class by class as
 * the QC's spawn functions do. The brush models among them are drawn by
 * the renderer at their origin (each in the visible leaf of its box
 * nearest the camera, in the painter's order) and the solid ones clip the
 * traces (engine/clip.h clip_bmodels).
 *
 * Units: positions 1/CLIP_SUB world units, angles binary degrees, times in
 * ticks (wait and delay in hundredths of a second, as cooked).
 */

#ifndef Q1PSX_ENTS_H
#define Q1PSX_ENTS_H

#include <stdint.h>

#include "clip.h"
#include "qworld.h"

/* Quake's solid, movetype and mover states (defs.qc) */
enum { SOLID_NOT, SOLID_TRIGGER, SOLID_BBOX, SOLID_SLIDEBOX, SOLID_BSP };
enum { MOVETYPE_NONE, MOVETYPE_WALK = 3, MOVETYPE_STEP, MOVETYPE_FLY, MOVETYPE_TOSS,
       MOVETYPE_PUSH, MOVETYPE_NOCLIP, MOVETYPE_FLYMISSILE, MOVETYPE_BOUNCE };
enum { STATE_TOP, STATE_BOTTOM, STATE_UP, STATE_DOWN };

#define QE_PLAYER 255        /* the player's mirror edict's class */
#define ENT_MAX_LEAVES 8     /* leaves a brush model's box touches (the render list's choice) */

typedef struct edict_s edict_t;
typedef void (*ent_fn)(edict_t *self);   /* think / use / touch / blocked: `other`
                                            and `activator` are globals, as in QC */

struct edict_s {
	uint8_t  class;          /* QE_*, QE_PLAYER                                */
	uint8_t  solid;          /* SOLID_*                                        */
	uint8_t  movetype;       /* MOVETYPE_*                                     */
	uint8_t  state;          /* STATE_*                                        */
	uint16_t spawnflags;
	int16_t  model;          /* brush model index (world.bmodels), 0 = none    */
	uint8_t  draw;           /* the model is drawn                             */
	uint8_t  clip;           /* clip_bmodels index + 1 while solid, 0 = not    */
	uint8_t  num_leaves;     /* the leaves the box touches                     */
	uint8_t  takedamage;
	uint8_t  vismask;        /* which of them the PVS stamp vis_stamp sees     */
	uint8_t  pad;
	uint16_t vis_stamp;
	uint16_t leaves[ENT_MAX_LEAVES];
	int32_t  origin[3];      /* 1/CLIP_SUB units                               */
	int32_t  mins[3], maxs[3];       /* the model's box, local                  */
	int32_t  absmin[3], absmax[3];   /* in the world                            */
	int32_t  velocity[3];    /* world units per second                         */
	int32_t  angles[3];      /* binary degrees (pitch, yaw, roll)              */
	int16_t  angle;          /* the map's `angle`, degrees (-1 up, -2 down)    */
	int16_t  mangle[3];      /* the map's `mangle`, degrees                    */
	int32_t  movedir[3];     /* x4096                                          */
	int32_t  pos1[3], pos2[3];       /* the mover's ends                        */
	int32_t  finaldest[3];
	int32_t  nextthink;      /* tick, 0 = none                                 */
	int32_t  ltime;          /* a pusher's local time, ticks                   */
	ent_fn   think, think1, use, touch, blocked;
	edict_t *owner, *enemy, *goalentity, *groundentity;
	int16_t  speed, wait, delay, lip, height, dmg, health, max_health, count, sounds, style;
	uint16_t target, targetname, killtarget, message, map;   /* strings (world_string) */
	uint32_t noise, noise1, noise2, noise3;   /* sound names' hashes (qs_hash)  */
	int32_t  items;          /* the keys a door needs                          */
	uint32_t flags;
};

extern edict_t *edicts;      /* the pool, in spawn order */
extern int      num_edicts;
extern int      ents_stat[4];   /* the last frame: models drawn, PVS-visible,
                                   leaves in view, drawable (the bench) */
extern int      ents_debug;     /* the bench: print each model drawn */

/* after world_load (the pool in the level arena) */
void ents_load(void);

/* spawn the map's entities from the cooked table (the pool emptied first) */
void ents_spawn(void);

/* per frame, after the camera is set and before r_frame: the brush models
   into the render list */
void ents_render(void);

#endif
