/*
 * q1psx — player movement (Phase 6.2 / 6.3).
 *
 * A fixed-point, 60 Hz port of Quake's player physics: friction + acceleration
 * toward a wished direction, gravity, jumping, sliding along walls
 * (SV_FlyMove / ClipVelocity), and step-up over ledges <= 18 units. Collision
 * is bsp_hull_trace against clip hull 1.
 *
 * No QuakeC, no entities yet — this drives the debug camera. Velocities are
 * plain integer units/second; positions are integer world units with a
 * per-axis sub-unit accumulator so slow walking still moves.
 */

#pragma once

#include <stdint.h>

#include "mathlib.h"
#include "bsp.h"

typedef struct {
	int32_t pos[3];       /* world units                         */
	int32_t vel[3];       /* units per second                    */
	int32_t yaw, pitch;   /* q1psx degrees, 20.12 (aim)          */
	int     onground;     /* 1 = standing on a walkable floor    */
	int     noclip;       /* 1 = free flight, ignore the world   */

	int32_t pos_acc[3];   /* sub-unit displacement carry         */
	int32_t grav_acc;     /* sub-unit gravity carry              */
	int32_t home[3];      /* respawn point                       */
	int32_t home_yaw;     /* respawn facing                      */
} pmove_t;

/* One frame of intent. fwd/side/up are -1, 0 or +1. */
typedef struct {
	int fwd, side, up;    /* up used only in noclip           */
	int jump;             /* edge-triggered                   */
	int walk;             /* held: move at half speed         */
} pmove_cmd_t;

/* Advance the player one 60 Hz tick. `b` may be NULL (behaves as noclip). */
void pmove_step(pmove_t *pm, const pmove_cmd_t *cmd, const bsp_t *b);
