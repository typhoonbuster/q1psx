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

/* One tick of intent. fwd/side/up are -1, 0 or +1. */
typedef struct {
	int fwd, side, up;    /* up: noclip only                           */
	int jump;             /* edge-triggered                            */
	int walk;             /* held: half speed                          */
} pmove_cmd_t;

#define PM_EYE_HEIGHT 22  /* view origin above the box origin (Quake), units */

/* Advance the player one 60 Hz tick. */
void pmove_step(pmove_t *pm, const pmove_cmd_t *cmd);

#endif
