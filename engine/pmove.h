/*
 * q1psx — player movement (R4).
 *
 * A fixed-point, 60 Hz port of Quake's player physics:
 *   - friction and acceleration toward a wished direction;
 *   - gravity and jumping;
 *   - sliding along walls (SV_FlyMove / ClipVelocity);
 *   - stepping up ledges of 18 units or less.
 * Collision is clip_trace() against hull 1.
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
	int     onground;     /* 1 = standing on a walkable floor          */
	int     noclip;       /* 1 = free flight, ignore the world         */

	int32_t pos_acc[3];   /* sub-unit displacement carry               */
	int32_t grav_acc;     /* sub-unit gravity carry                    */
} pmove_t;

/* One tick of intent. fwd / side are analog, -PM_AXIS .. PM_AXIS (a stick;
   a d-pad direction is +-PM_AXIS): the wished speed follows |(fwd, side)|,
   capped at full speed, as Quake's wishspeed = |wishvel|. up is -1, 0, +1. */
#define PM_AXIS 128       /* full deflection (2^7)                       */

typedef struct {
	int fwd, side, up;    /* up: noclip only                           */
	int jump;             /* edge-triggered                            */
	int walk;             /* held: half speed                          */
} pmove_cmd_t;

/* engine/p_defs.inc mirrors both layouts for the asm port (engine/pmove.s) */
_Static_assert(__builtin_offsetof(pmove_t, vel) == 12 && __builtin_offsetof(pmove_t, yaw) == 24 &&
               __builtin_offsetof(pmove_t, onground) == 32 && __builtin_offsetof(pmove_t, pos_acc) == 40 &&
               __builtin_offsetof(pmove_t, grav_acc) == 52 && sizeof(pmove_t) == 56, "p_defs.inc PM_*");
_Static_assert(__builtin_offsetof(pmove_cmd_t, up) == 8 && __builtin_offsetof(pmove_cmd_t, walk) == 16 &&
               sizeof(pmove_cmd_t) == 20, "p_defs.inc CMD_*");

#define PM_EYE_HEIGHT 22  /* view origin above the box origin (Quake), units */

/* Advance the player one 60 Hz tick. The PS1 build links the asm port
   (engine/pmove.s); engine/pmove.c is its C reference. */
void pmove_step(pmove_t *pm, const pmove_cmd_t *cmd);

#endif
