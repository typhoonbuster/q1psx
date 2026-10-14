#include <stdint.h>
#include <string.h>
#ifdef PM_DEBUG
#include <stdio.h>
#endif

#include "clip.h"
#include "pmove.h"
#include "q_fixed.h"
#include "render.h"   /* r_sin / r_cos: binary degrees -> x4096 */

/* Quake defaults (sv_*.c cvars + client.qc). Frame-rate is locked to 60 Hz, so
   the *host_frametime factors are baked into the /PM_HZ divisions below. */
#define PM_HZ          60
#define PM_MAXSPEED    320
#define PM_ACCEL       10
#define PM_FRICTION    4
#define PM_STOPSPEED   100
#define PM_GRAVITY     800
#define PM_JUMPSPEED   270
#define PM_STEPSIZE    18

#define FLOOR_NZ       2867    /* 0.7 * 4096 — a normal steeper than this walks */
#define STOP_V         2       /* kill sub-2 u/s creep after a clip            */
#define NOCLIP_SPEED   4       /* units/frame in free flight (x4 unless walking) */

static int32_t isqrt32(uint64_t v)
{
	return (int32_t) q_isqrt64(v);
}

/* --------------------------------------------------------------- vec utils */

static int64_t dot_iq(const int32_t v[3], const int16_t n[3])   /* int . q12 -> q12*unit */
{
	return (int64_t) v[0] * n[0] + (int64_t) v[1] * n[1] + (int64_t) v[2] * n[2];
}

/* Make v . n >= 0 with the smallest change, on n's dominant axis. Integer
   rounding leaves a slide a hair INTO the plane it should run along; started
   within DIST_EPS of that plane, the next trace then stops dead at fraction 0
   every bump (the player stuck on ramps). */
static void push_out(int32_t v[3], const int16_t n[3])
{
	int64_t d = dot_iq(v, n);
	if (d >= 0)
		return;
	int m = 0;
	for (int k = 1; k < 3; k++)
		if ((n[k] < 0 ? -n[k] : n[k]) > (n[m] < 0 ? -n[m] : n[m]))
			m = k;
	int32_t a = n[m] < 0 ? -n[m] : n[m];
	int32_t s = (int32_t) ((-d + a - 1) / a);
	v[m] += n[m] < 0 ? -s : s;
}

/* Slide `v` (units/s) along plane normal `n` (x4096). overbounce fixed at 1. */
static void clip_velocity(const int32_t v[3], const int16_t n[3], int32_t out[3])
{
	int32_t backoff = (int32_t) (dot_iq(v, n) >> Q_FIX_SHIFT);
	for (int i = 0; i < 3; i++) {
		int32_t change = (int32_t) (((int64_t) n[i] * backoff) >> Q_FIX_SHIFT);
		out[i] = v[i] - change;
		if (out[i] > -STOP_V && out[i] < STOP_V)
			out[i] = 0;
	}
	push_out(out, n);
}

/* ---------------------------------------------------------- SV_FlyMove port */

/* Slide pm->pos by displacement `move` (world units) for this frame, clipping
   pm->vel off every surface hit. Sets pm->onground if a floor is touched.
   Returns Quake's blocked flags (1 = floor, 2 = wall). */
static int slide_move(pmove_t *pm, const int32_t move[3])
{
	int32_t vel[3]    = { pm->vel[0], pm->vel[1], pm->vel[2] };
	int32_t primal[3] = { vel[0], vel[1], vel[2] };
	int32_t cur[3]    = { pm->pos[0], pm->pos[1], pm->pos[2] };
	int32_t rem[3]    = { move[0], move[1], move[2] };

	int16_t planes[5][3];
	int     nplanes = 0;
	int     blocked = 0;
	q_fixed time_left = Q_FIX_ONE;

	for (int bump = 0; bump < 4; bump++) {
		if (!rem[0] && !rem[1] && !rem[2])
			break;

		/* this bump's displacement, kept off every plane already hit (two
		   passes: fixing one plane of a crease can tip it into the other) */
		int32_t disp[3], end[3];
		for (int k = 0; k < 3; k++)
			disp[k] = (int32_t) (((int64_t) rem[k] * time_left + Q_FIX_ONE / 2) >> Q_FIX_SHIFT);
		for (int pass = 0; pass < 2; pass++)
			for (int i = 0; i < nplanes; i++)
				push_out(disp, planes[i]);
		for (int k = 0; k < 3; k++)
			end[k] = cur[k] + disp[k];

		trace_t tr;
		clip_trace(1, cur, end, &tr);
#ifdef PM_DEBUG
		printf("  bump %d: %d %d %d -> %d %d %d frac %d n %d %d %d all %d start %d end %d %d %d c%d\n", bump,
		       cur[0], cur[1], cur[2], end[0], end[1], end[2], tr.fraction, tr.plane_n[0], tr.plane_n[1],
		       tr.plane_n[2], tr.allsolid, tr.startsolid, tr.endpos[0], tr.endpos[1], tr.endpos[2],
		       clip_contents(1, tr.endpos));
#endif

		if (tr.allsolid) {
			vel[0] = vel[1] = vel[2] = 0;
			blocked |= 4;
			break;
		}
		/* advanced? (a short impact can round back onto the start: that is
		   no advance, and must not forget the planes already hit) */
		if (tr.endpos[0] != cur[0] || tr.endpos[1] != cur[1] || tr.endpos[2] != cur[2]) {
			cur[0] = tr.endpos[0];
			cur[1] = tr.endpos[1];
			cur[2] = tr.endpos[2];
			nplanes = 0;
		}
		if (tr.fraction >= Q_FIX_ONE)
			break;

		if (tr.plane_n[2] > FLOOR_NZ) {
			blocked |= 1;
			pm->onground = 1;
		} else {
			blocked |= 2;
		}

		time_left -= (q_fixed) (((int64_t) time_left * tr.fraction) >> Q_FIX_SHIFT);

		if (nplanes >= 5) {
			vel[0] = vel[1] = vel[2] = 0;
			break;
		}
		planes[nplanes][0] = tr.plane_n[0];
		planes[nplanes][1] = tr.plane_n[1];
		planes[nplanes][2] = tr.plane_n[2];
		nplanes++;

		/* find a velocity that runs parallel to every plane hit since the
		   last real advance (Quake's inner loop) */
		int     i;
		int32_t newvel[3] = { 0, 0, 0 };
		for (i = 0; i < nplanes; i++) {
			clip_velocity(primal, planes[i], newvel);
			int j;
			for (j = 0; j < nplanes; j++)
				if (j != i && dot_iq(newvel, planes[j]) < 0)
					break;
			if (j == nplanes)
				break;
		}
		if (i != nplanes) {
			vel[0] = newvel[0];
			vel[1] = newvel[1];
			vel[2] = newvel[2];
		} else {
			if (nplanes != 2) {
				vel[0] = vel[1] = vel[2] = 0;
				break;
			}
			/* slide along the crease of the two planes. Quake scales by the
			   unnormalised cross product, i.e. by sin^2 of the planes'
			   angle: falling into a 45-degree wedge halved the speed every
			   tick. Project onto the crease properly. */
			int32_t dir[3];
			dir[0] = (int32_t) (((int64_t) planes[0][1] * planes[1][2]
			                   - (int64_t) planes[0][2] * planes[1][1]) >> Q_FIX_SHIFT);
			dir[1] = (int32_t) (((int64_t) planes[0][2] * planes[1][0]
			                   - (int64_t) planes[0][0] * planes[1][2]) >> Q_FIX_SHIFT);
			dir[2] = (int32_t) (((int64_t) planes[0][0] * planes[1][1]
			                   - (int64_t) planes[0][1] * planes[1][0]) >> Q_FIX_SHIFT);
			int64_t len2 = (int64_t) dir[0] * dir[0] + (int64_t) dir[1] * dir[1]
			             + (int64_t) dir[2] * dir[2];
			if (!len2) {                  /* parallel planes: wedged */
				vel[0] = vel[1] = vel[2] = 0;
				break;
			}
			int64_t d = (int64_t) dir[0] * vel[0] + (int64_t) dir[1] * vel[1]
			          + (int64_t) dir[2] * vel[2];
			vel[0] = (int32_t) (dir[0] * d / len2);
			vel[1] = (int32_t) (dir[1] * d / len2);
			vel[2] = (int32_t) (dir[2] * d / len2);
		}

		/* stop dead if the result reverses the original push (sloping corners) */
		if ((int64_t) vel[0] * primal[0] + (int64_t) vel[1] * primal[1]
		  + (int64_t) vel[2] * primal[2] <= 0) {
			vel[0] = vel[1] = vel[2] = 0;
			break;
		}

		/* round to nearest, not toward zero: a truncated component can tip
		   a move that should run along a slope slightly INTO it, and the
		   trace then stops dead every bump (stuck at the foot of ramps) */
		for (int k = 0; k < 3; k++) {
			int32_t num = vel[k] * CLIP_SUB;
			rem[k] = (num + (num >= 0 ? PM_HZ / 2 : -PM_HZ / 2)) / PM_HZ;
		}
	}

	pm->pos[0] = cur[0];
	pm->pos[1] = cur[1];
	pm->pos[2] = cur[2];
	pm->vel[0] = vel[0];
	pm->vel[1] = vel[1];
	pm->vel[2] = vel[2];
	return blocked;
}

/* Trace straight down a couple of units; on a walkable floor set onground,
   settle onto it (gravity stops while grounded, so without the snap a landing
   left the player hovering up to 2 units above the floor) and kill downward
   velocity. */
static void categorize_position(pmove_t *pm)
{
	if (pm->vel[2] > 180) {           /* clearly rising — jumped this frame */
		pm->onground = 0;
		return;
	}
	int32_t down[3] = { pm->pos[0], pm->pos[1], pm->pos[2] - 2 * CLIP_SUB };
	trace_t tr;
	clip_trace(1, pm->pos, down, &tr);

	if (tr.fraction >= Q_FIX_ONE || tr.plane_n[2] < FLOOR_NZ) {
		pm->onground = 0;
	} else {
		pm->onground = 1;
		if (!tr.startsolid && !tr.allsolid) {
			pm->pos[0] = tr.endpos[0];
			pm->pos[1] = tr.endpos[1];
			pm->pos[2] = tr.endpos[2];
		}
		if (pm->vel[2] < 0)
			pm->vel[2] = 0;
	}
}

/* ------------------------------------------------------- friction & accel */

static void ground_friction(pmove_t *pm)
{
	int32_t speed = isqrt32((uint64_t) ((int64_t) pm->vel[0] * pm->vel[0]
	                                  + (int64_t) pm->vel[1] * pm->vel[1]
	                                  + (int64_t) pm->vel[2] * pm->vel[2]));
	if (speed < 1)
		return;

	int32_t control  = speed < PM_STOPSPEED ? PM_STOPSPEED : speed;
	int32_t drop     = control * PM_FRICTION / PM_HZ;
	int32_t newspeed = speed - drop;
	if (newspeed < 0)
		newspeed = 0;

	for (int k = 0; k < 3; k++)
		pm->vel[k] = (int32_t) ((int64_t) pm->vel[k] * newspeed / speed);
}

/* `cap` limits how fast we may end up going along wishdir this step; `rate` is
   the speed the accel rate scales with. Quake's air move passes cap=30 but
   rate=the full wished speed (SV_AirAccelerate), which is what makes air
   strafing possible while keeping straight-line air control weak. */
static void accelerate(pmove_t *pm, const int32_t wishdir[3], int32_t cap, int32_t rate)
{
	int64_t d = (int64_t) pm->vel[0] * wishdir[0] + (int64_t) pm->vel[1] * wishdir[1]
	          + (int64_t) pm->vel[2] * wishdir[2];
	int32_t cur = (int32_t) (d >> Q_FIX_SHIFT);   /* speed along wishdir */

	int32_t add = cap - cur;
	if (add <= 0)
		return;

	int32_t accel = PM_ACCEL * rate / PM_HZ;
	if (accel > add)
		accel = add;

	for (int k = 0; k < 3; k++)
		pm->vel[k] += (int32_t) (((int64_t) wishdir[k] * accel) >> Q_FIX_SHIFT);
}

/* ----------------------------------------------------------------- step-up */

static int32_t horiz_dist2(const int32_t a[3], const int32_t b[3])
{
	int32_t dx = a[0] - b[0], dy = a[1] - b[1];
	return dx * dx + dy * dy;
}

/* --------------------------------------------------------------- pmove_step */

void pmove_step(pmove_t *pm, const pmove_cmd_t *cmd)
{
	int32_t sy = r_sin(pm->yaw), cy = r_cos(pm->yaw);
	int32_t right[3] = { sy, -cy, 0 };            /* Quake AngleVectors, roll 0 */

	if (pm->noclip) {
		int32_t sp = r_sin(pm->pitch), cp = r_cos(pm->pitch);
		int32_t fwd[3] = { (cp * cy) >> 12, (cp * sy) >> 12, -sp };
		int s = NOCLIP_SPEED * CLIP_SUB * (cmd->walk ? 1 : 4);
		for (int k = 0; k < 3; k++)
			pm->pos[k] += (fwd[k] * (cmd->fwd * s) + right[k] * (cmd->side * s)) >> Q_FIX_SHIFT;
		pm->pos[2] += cmd->up * s;
		pm->vel[0] = pm->vel[1] = pm->vel[2] = 0;
		return;
	}

	/* aim vectors are horizontal — the player walks flat regardless of pitch */
	int32_t fwd[3] = { cy, sy, 0 };

	categorize_position(pm);

	/* wish direction + speed */
	int32_t wishdir[3] = {
		fwd[0] * cmd->fwd + right[0] * cmd->side,
		fwd[1] * cmd->fwd + right[1] * cmd->side,
		fwd[2] * cmd->fwd + right[2] * cmd->side,
	};
	int32_t wishspeed = 0;
	if (cmd->fwd || cmd->side) {
		int32_t len = isqrt32((uint64_t) ((int64_t) wishdir[0] * wishdir[0]
		                                + (int64_t) wishdir[1] * wishdir[1]
		                                + (int64_t) wishdir[2] * wishdir[2]));
		if (len > 0)
			for (int k = 0; k < 3; k++)
				wishdir[k] = (int32_t) (((int64_t) wishdir[k] << Q_FIX_SHIFT) / len);
		wishspeed = cmd->walk ? PM_MAXSPEED / 2 : PM_MAXSPEED;
	}

	/* jump */
	if (cmd->jump && pm->onground) {
		pm->vel[2] = PM_JUMPSPEED;
		pm->onground = 0;
	}

	/* friction on the ground, then acceleration */
	if (pm->onground) {
		ground_friction(pm);
		accelerate(pm, wishdir, wishspeed, wishspeed);
	} else {
		int32_t cap = wishspeed > 30 ? 30 : wishspeed;
		accelerate(pm, wishdir, cap, wishspeed);
	}

	/* gravity */
	if (!pm->onground) {
		pm->grav_acc += PM_GRAVITY;
		pm->vel[2] -= pm->grav_acc / PM_HZ;
		pm->grav_acc %= PM_HZ;
	} else {
		pm->grav_acc = 0;
	}

	/* this frame's displacement, with sub-unit carry so slow walking moves */
	int32_t move[3];
	for (int k = 0; k < 3; k++) {
		pm->pos_acc[k] += pm->vel[k] * CLIP_SUB;
		move[k] = pm->pos_acc[k] / PM_HZ;
		pm->pos_acc[k] -= move[k] * PM_HZ;
	}

	/* plain slide */
	int32_t flat_pos[3]  = { pm->pos[0], pm->pos[1], pm->pos[2] };
	int32_t flat_vel[3]  = { pm->vel[0], pm->vel[1], pm->vel[2] };
	int32_t entry_pos[3] = { pm->pos[0], pm->pos[1], pm->pos[2] };
	int blocked = slide_move(pm, move);
	int32_t after_flat[3] = { pm->pos[0], pm->pos[1], pm->pos[2] };
	int32_t flat_out_vel[3] = { pm->vel[0], pm->vel[1], pm->vel[2] };

#ifdef PM_DEBUG
	printf(" flat: blocked %d pos %d %d %d c%d\n", blocked, pm->pos[0], pm->pos[1], pm->pos[2], clip_contents(1, pm->pos));
#endif
	/* step-up: if a wall stopped us and we're grounded, retry over an
	   18-unit lip (Quake's SV_WalkMove) */
	if ((blocked & 2) && pm->onground) {
		pm->pos[0] = flat_pos[0];
		pm->pos[1] = flat_pos[1];
		pm->pos[2] = flat_pos[2];
		pm->vel[0] = flat_vel[0];
		pm->vel[1] = flat_vel[1];
		pm->vel[2] = flat_vel[2];

		int32_t up_from[3] = { pm->pos[0], pm->pos[1], pm->pos[2] };
		int32_t up_to[3]   = { pm->pos[0], pm->pos[1], pm->pos[2] + PM_STEPSIZE * CLIP_SUB };
		trace_t tr;
		clip_trace(1, up_from, up_to, &tr);
		pm->pos[0] = tr.endpos[0];
		pm->pos[1] = tr.endpos[1];
		pm->pos[2] = tr.endpos[2];

		slide_move(pm, move);

		int32_t dn_from[3] = { pm->pos[0], pm->pos[1], pm->pos[2] };
		int32_t dn_to[3]   = { pm->pos[0], pm->pos[1], pm->pos[2] - (PM_STEPSIZE + 2) * CLIP_SUB };
		clip_trace(1, dn_from, dn_to, &tr);

		int stepped_ok = (tr.fraction < Q_FIX_ONE && tr.plane_n[2] > FLOOR_NZ);
#ifdef PM_DEBUG
		printf(" step: down frac %d n %d %d %d end %d %d %d c%d ok %d\n", tr.fraction, tr.plane_n[0],
		       tr.plane_n[1], tr.plane_n[2], tr.endpos[0], tr.endpos[1], tr.endpos[2],
		       clip_contents(1, tr.endpos), stepped_ok);
#endif
		if (stepped_ok) {
			pm->pos[0] = tr.endpos[0];
			pm->pos[1] = tr.endpos[1];
			pm->pos[2] = tr.endpos[2];
		}
		if (!stepped_ok
		 || horiz_dist2(pm->pos, entry_pos) <= horiz_dist2(after_flat, entry_pos)) {
			/* the step didn't help — keep the flat result */
			pm->pos[0] = after_flat[0];
			pm->pos[1] = after_flat[1];
			pm->pos[2] = after_flat[2];
			pm->vel[0] = flat_out_vel[0];
			pm->vel[1] = flat_out_vel[1];
			pm->vel[2] = flat_out_vel[2];
		} else {
			/* keep horizontal velocity, drop the tiny vertical from the climb */
			pm->vel[2] = flat_out_vel[2];
		}
	}

	categorize_position(pm);
}
