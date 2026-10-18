/*
 * q1psx — player movement (R4).
 *
 * A fixed-point, 60 Hz port of Quake's player physics:
 *   - friction and acceleration toward a wished direction;
 *   - gravity and jumping;
 *   - sliding along walls (SV_FlyMove / ClipVelocity);
 *   - stepping up ledges of 18 units or less;
 *   - the water: SV_CheckWater's level and type, SV_WaterMove's swim,
 *     SV_WaterJump's push out, PlayerPreThink's drag and PlayerJump's
 *     kicks (the QC's decisions, CheckWaterJump and the sounds, are the
 *     game's: engine/game.c).
 * Collision is clip_trace() against hull 1, the water clip_contents() on
 * hull 0.
 *
 * Velocities are integer units/second. Positions are 1/CLIP_SUB (1/8) world
 * units — whole units made slopes stick — with a per-axis accumulator for
 * the remainder. Angles are binary degrees (4096 per turn), like the
 * renderer's.
 *
 * The same algorithms as phase 6 (docs/porting-notes.md §9), validated on
 * screen.
 */

#ifndef Q1PSX_PMOVE_H
#define Q1PSX_PMOVE_H

#include <stdint.h>

typedef struct {
	int32_t pos[3];       /* 1/CLIP_SUB world units (box origin)       */
	int32_t vel[3];       /* units per second                          */
	int     yaw, pitch;   /* binary degrees; pitch is aim only         */
	int     onground;     /* standing on a walkable floor: 1 + the floor's
	                         trace ent (1 the world, 2 + a clip_bmodels
	                         entry: the plat it rides, engine/ents.c);
	                         0 in the air                               */
	int     noclip;       /* 1 = free flight, ignore the world         */

	int32_t pos_acc[3];   /* sub-unit displacement carry               */
	int32_t grav_acc;     /* sub-unit gravity carry                    */
	int     waterlevel;   /* 0 out, 1 the feet, 2 the waist, 3 the eyes (SV_CheckWater) */
	int     watertype;    /* QW_CONTENTS_* at the feet, EMPTY when out  */
	int     waterjump;    /* ticks of FL_WATERJUMP left (teleport_time); 0 none */
	int     jumpreleased; /* FL_JUMPRELEASED: no pogo while jump is held */
	int32_t movedir[2];   /* the water jump's push, units/s (CheckWaterJump) */
	int32_t touched;      /* this tick: the last brush model a slide ran into,
	                         its trace ent (0 none): the game calls its touch
	                         (SV_Impact: buttons, doors) */
} pmove_t;

/* One tick of intent. fwd / side are analog, -PM_AXIS .. PM_AXIS (a stick;
   a d-pad direction is +-PM_AXIS): the wished speed follows |(fwd, side)|,
   capped at full speed, as Quake's wishspeed = |wishvel|. up is -1, 0, +1
   (noclip, and the swim's upmove). */
#define PM_AXIS 128       /* full deflection (2^7)                       */

typedef struct {
	int fwd, side, up;
	int jump;             /* held (PlayerJump: the pogo is stopped by jumpreleased) */
	int walk;             /* held: half speed                          */
	int waterjump;        /* this tick: CheckWaterJump found the ledge (the game) */
} pmove_cmd_t;

/* engine/p_defs.inc mirrors both layouts for the asm port (engine/pmove.s) */
_Static_assert(__builtin_offsetof(pmove_t, vel) == 12 && __builtin_offsetof(pmove_t, yaw) == 24 &&
               __builtin_offsetof(pmove_t, onground) == 32 && __builtin_offsetof(pmove_t, pos_acc) == 40 &&
               __builtin_offsetof(pmove_t, grav_acc) == 52 && __builtin_offsetof(pmove_t, waterlevel) == 56 &&
               __builtin_offsetof(pmove_t, waterjump) == 64 && __builtin_offsetof(pmove_t, movedir) == 72 &&
               __builtin_offsetof(pmove_t, touched) == 80 && sizeof(pmove_t) == 84, "p_defs.inc PM_*");
_Static_assert(__builtin_offsetof(pmove_cmd_t, up) == 8 && __builtin_offsetof(pmove_cmd_t, walk) == 16 &&
               __builtin_offsetof(pmove_cmd_t, waterjump) == 20 && sizeof(pmove_cmd_t) == 24, "p_defs.inc CMD_*");

#define PM_EYE_HEIGHT 22  /* view origin above the box origin (Quake), units */
#define PM_MINS_Z    (-24) /* the box (hull 1, 32 x 32 x 56): SV_CheckWater's feet */
#define PM_MAXS_Z      32

/* Advance the player one 60 Hz tick. The PS1 build links the asm port
   (engine/pmove.s); engine/pmove.c is its C reference. */
void pmove_step(pmove_t *pm, const pmove_cmd_t *cmd);

#endif
