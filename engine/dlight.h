/*
 * q1psx — dynamic lights: Quake's cl_dlights (cl_main.c CL_AllocDlight,
 * CL_DecayLights), R_PushDlights / R_MarkLights (r_light.c) and
 * R_AddDynamicLights (r_surf.c), on the world's vertex lighting.
 *
 * Each frame the renderer's walk, as it comes to a node's face block, adds
 * the live lights to the vertices' light bytes of the records it is about
 * to draw (R_MarkLights marks the faces on the planes a light's sphere
 * straddles: the same faces survive the distance test, but a plane runs
 * through the whole map, and here the walk has the drawn records and their
 * plane at hand), as R_AddDynamicLights adds light to a surface's lightmap
 * texels: `rad = radius - |plane distance|`, nothing
 * under the light's minlight, then `rad - dist` at each vertex, `dist`
 * Quake's octagonal approximation of the distance to the light's impact
 * point in the plane. The bytes are put back after the frame (dl_restore).
 * Entities add `radius - distance` of each light to their light
 * (R_DrawEntitiesOnList, R_DrawViewModel): dl_add_at.
 *
 * Positions are the game's (1/R_POS_SUB world units); radii and minlight
 * world units; `die` a game tick; decay world units per second.
 */

#ifndef Q1PSX_DLIGHT_H
#define Q1PSX_DLIGHT_H

#include <stdint.h>

#include "qworld.h"

#define MAX_DLIGHTS 8
#define DL_PRUNE    64           /* vertex units: a plane farther than a light's
                                    reach + this holds none of its lit vertices
                                    on its far side (engine/r_defs.inc DL_PRUNE) */
#ifndef Q1PSX_DLIGHT_MINLIGHT
#define Q1PSX_DLIGHT_MINLIGHT 32 /* (CMake) the world's lights' least minlight, world
                                    units: Quake's minlight, which the muzzle flash
                                    alone sets (32), for every light on the world's
                                    faces; their faintest edge left unlit, their
                                    reach shorter. 0: each light's own (Quake's look;
                                    docs/porting-notes.md §113) */
#endif

typedef struct {
	int32_t origin[3];
	int     radius;
	int     die;
	int     decay;
	int     minlight;
	int     key;
} dlight_t;

extern dlight_t dlights[MAX_DLIGHTS];
extern int      dl_enabled;                /* the menu's dynamic lights: 0 = none pushed,
                                              none added to the models (the game still
                                              allocates and decays them) */
extern uint32_t dl_save_n;                 /* records saved this frame */
extern uint16_t dl_sqrt[1024];           /* sqrt(i << 11): the lights, snd.c */

void      dl_init(void);
void      dl_clear(void);                  /* a new map */
dlight_t *dl_alloc(int key, int now);      /* CL_AllocDlight: the same key's, else a dead one, else the first */
void      dl_decay(int now, int dt);       /* CL_DecayLights over a tick of dt 60ths (now: its end) */
void      dl_push(int now);                /* R_PushDlights: the live lights for r_frame */
void      dl_stamp_nodes(void);            /* r_render, after mark_pvs: the nodes the lights reach */
#ifndef Q1PSX_DLIGHT_C
#define Q1PSX_DLIGHT_C 0                   /* (CMake: 0 the asm, 1 the C reference, 2 both) */
#endif
#if Q1PSX_DLIGHT_C == 1
void      dl_restore(void);                /* after r_frame: the records' light bytes back */
#else
void      dl_restore_asm(void);            /* (engine/r_world.s: the same, no C in between) */
#define   dl_restore dl_restore_asm
#endif
/* the renderer's record hook (engine/r_world.s dr_light_rec), C reference
   version: a stamped record of a node a light reaches, about to be
   drawn, lit in place */
void      dl_light_record(qw_record *rec, const qw_node *node);
void      dl_check_record(qw_record *rec, const qw_node *node);   /* (R_DLIGHT_C = 2) */
int       dl_add_at(const int32_t org[3], int now);   /* Σ (radius - distance), world units */
/* a dynamic light's share at the cook's light gain (scripts/cook.sh
   LIGHT_GAIN 1.66): x 53 / 32 = 1.656, the world's vertices' 53 / 128 x 4 */
#define DL_GAIN(x) ((x) * 53 / 32)

#endif
