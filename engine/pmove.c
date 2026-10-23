/*
 * q1psx — player movement, C reference.
 *
 * The PS1 build links the asm port (engine/pmove.s); this file is the
 * specification it follows (see engine/clip.c for how the two are kept
 * identical). 32-bit arithmetic only, plus 32 x 32 -> 64-bit products: every
 * velocity component is capped at PM_MAXVELOCITY, which bounds all of them.
 */

#include <stdint.h>
#include <string.h>
#ifdef PM_DEBUG
#include <stdio.h>
#endif

#include "clip.h"
#include "pmove.h"
#include "q_fixed.h"
#include "render.h"   /* r_sin / r_cos: binary degrees -> x4096 */

/* Quake defaults (sv_*.c cvars + client.qc). A tick lasts dt 60ths of a
   second (pmove_cmd_t): Quake's *host_frametime factors are x dt / PM_HZ. */
#define PM_HZ          60
#define PM_MAXSPEED    320
#define PM_ACCEL       10
#define PM_FRICTION    4
#define PM_STOPSPEED   100
#define PM_GRAVITY     800
#define PM_JUMPSPEED   270
#define PM_STEPSIZE    18
#define PM_MAXVELOCITY 2000    /* sv_maxvelocity, per axis */

#define FLOOR_NZ       2867    /* 0.7 * 4096 — a normal steeper than this walks */
#define STOP_V         2       /* kill sub-2 u/s creep after a clip            */
#define NOCLIP_SPEED   4       /* units/frame in free flight (x4 unless walking) */

#define PM_WATERJUMP_TICKS 120 /* FL_WATERJUMP's teleport_time: 2 s             */
#define PM_WATERJUMP_UP    225 /* CheckWaterJump's velocity_z                   */
#define PM_SINK             60 /* SV_WaterMove: the drift to the bottom, units/s */
/* SV_WaterMove's friction over the tick, 1 - 4 dt / 60 (sv_friction), 0.16,
   rounded (engine/pmove.s pm_water_scale: the same) */
static const int32_t water_scale[PM_DT_MAX + 1] = { 65536, 61167, 56798, 52429, 48060, 43691, 39322 };
/* PlayerPreThink's drag, 1 - 0.8 x waterlevel x dt / 60, 0.16, truncated
   (pm_water_drag) */
static const int32_t water_drag[PM_DT_MAX + 1][4] = {
	{ 65536, 65536, 65536, 65536 }, { 65536, 64662, 63788, 62914 }, { 65536, 63788, 62040, 60293 },
	{ 65536, 62914, 60293, 57671 }, { 65536, 62040, 58545, 55050 }, { 65536, 61166, 56797, 52428 },
	{ 65536, 60293, 55050, 49807 },
};

/* gravity over a tick longer than a 60th: the move takes the tick's average
   speed, not its end speed (the parabola's own chord: a jump tops out at
   ~45 units at every tick rate, not lower and lower as the ticks lengthen),
   g dt^2 / 2 in pos_acc units (1/8 unit x 1/60): 800 x 8 x dt^2 / 120. Not
   at 60 Hz (dt 1: 0), Quake's own step. (engine/pmove.s pm_grav_mid) */
static const int32_t grav_mid[PM_DT_MAX + 1] = { 0, 0, 213, 480, 853, 1333, 1920 };

static int pm_dt;      /* this tick's length (pmove_step), for the routines below */
static int16_t step_n[3];   /* the last wall a slide_move hit: SV_FlyMove's steptrace's normal */

/* floor(sqrt(v)), bit by bit: no division, exact */
static int32_t isqrt32(uint32_t v)
{
	uint32_t res = 0, bit = 1u << 30;
	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= res + bit) {
			v  -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}
	return (int32_t) res;
}

/* --------------------------------------------------------------- vec utils */

static int32_t dot_iq(const int32_t v[3], const int16_t n[3])   /* int . q12 -> q12*unit */
{
	return v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
}

static int32_t dot_ii(const int32_t a[3], const int32_t b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Make v . n >= 0 with the smallest change, on n's dominant axis. Integer
   rounding leaves a slide a hair INTO the plane it should run along; started
   within DIST_EPS of that plane, the next trace then stops dead at fraction 0
   every bump (the player stuck on ramps). */
static void push_out(int32_t v[3], const int16_t n[3])
{
	int32_t d = dot_iq(v, n);
	if (d >= 0)
		return;
	int m = 0;
	for (int k = 1; k < 3; k++)
		if ((n[k] < 0 ? -n[k] : n[k]) > (n[m] < 0 ? -n[m] : n[m]))
			m = k;
	int32_t a = n[m] < 0 ? -n[m] : n[m];
	int32_t s = (-d + a - 1) / a;
	v[m] += n[m] < 0 ? -s : s;
}

/* Slide `v` (units/s) along plane normal `n` (x4096). overbounce fixed at 1. */
static void clip_velocity(const int32_t v[3], const int16_t n[3], int32_t out[3])
{
	int32_t backoff = dot_iq(v, n) >> Q_FIX_SHIFT;
	for (int i = 0; i < 3; i++) {
		int32_t change = (n[i] * backoff) >> Q_FIX_SHIFT;
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
			disp[k] = (rem[k] * time_left + Q_FIX_ONE / 2) >> Q_FIX_SHIFT;
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
		if (tr.ent)                      /* a brush model run into (SV_Impact) */
			pm->touched = tr.ent;

		if (tr.plane_n[2] > FLOOR_NZ) {
			blocked |= 1;
			pm->onground = 1 + tr.ent;
		}
		if (!tr.plane_n[2]) {               /* a wall: Quake's "step", a vertical plane only */
			blocked |= 2;
			step_n[0] = tr.plane_n[0];
			step_n[1] = tr.plane_n[1];
		}

		time_left -= (time_left * tr.fraction) >> Q_FIX_SHIFT;

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
			   tick. Project onto the crease properly: speed along the unit
			   crease sp = dir.vel / |dir|, then vel = dir * sp / |dir|. */
			int32_t dir[3];
			dir[0] = (planes[0][1] * planes[1][2] - planes[0][2] * planes[1][1]) >> Q_FIX_SHIFT;
			dir[1] = (planes[0][2] * planes[1][0] - planes[0][0] * planes[1][2]) >> Q_FIX_SHIFT;
			dir[2] = (planes[0][0] * planes[1][1] - planes[0][1] * planes[1][0]) >> Q_FIX_SHIFT;
			int32_t len = isqrt32((uint32_t) dot_ii(dir, dir));
			if (!len) {                   /* parallel planes: wedged */
				vel[0] = vel[1] = vel[2] = 0;
				break;
			}
			int32_t sp = dot_ii(dir, vel) / len;
			vel[0] = dir[0] * sp / len;
			vel[1] = dir[1] * sp / len;
			vel[2] = dir[2] * sp / len;
		}

		/* stop dead if the result reverses the original push (sloping corners) */
		if (dot_ii(vel, primal) <= 0) {
			vel[0] = vel[1] = vel[2] = 0;
			break;
		}

		/* round to nearest, not toward zero: a truncated component can tip
		   a move that should run along a slope slightly INTO it, and the
		   trace then stops dead every bump (stuck at the foot of ramps) */
		for (int k = 0; k < 3; k++) {
			int32_t num = vel[k] * CLIP_SUB * pm_dt;
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
		pm->onground = 1 + tr.ent;
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

/* SV_UserFriction: the speed in the plane (Quake's leaves z out); the
   friction doubled (sv_edgefriction 2) when the leading edge is over a
   dropoff: 16 units ahead along the velocity at the feet, nothing within
   34 units down (a point; the world and the brush models only, as a line
   of sight asks: its fraction 1 or not). A start inside solid is Quake's
   fraction 1 too. */
static void ground_friction(pmove_t *pm)
{
	int32_t speed = isqrt32((uint32_t) (pm->vel[0] * pm->vel[0] + pm->vel[1] * pm->vel[1]));
	if (speed < 1)
		return;

	int32_t start[3], stop[3];
	start[0] = stop[0] = pm->pos[0] + pm->vel[0] * 16 * CLIP_SUB / speed;
	start[1] = stop[1] = pm->pos[1] + pm->vel[1] * 16 * CLIP_SUB / speed;
	start[2] = pm->pos[2] + PM_MINS_Z * CLIP_SUB;
	stop[2]  = start[2] - 34 * CLIP_SUB;
	memset(&clip_mover, 0, sizeof clip_mover);
	clip_mover.pass       = 1;                  /* (the player's own entry) */
	clip_mover.nomonsters = 1;
	clip_mover.sight      = 1;
	trace_t tr;
	int32_t friction = clip_trace_ent(0, start, stop, &tr) >= Q_FIX_ONE ? PM_FRICTION * 2 : PM_FRICTION;

	int32_t control  = speed < PM_STOPSPEED ? PM_STOPSPEED : speed;
	int32_t drop     = control * friction * pm_dt / PM_HZ;
	int32_t newspeed = speed - drop;
	if (newspeed < 0)
		newspeed = 0;

	/* one division: scale = newspeed / speed in 0.16 fixed point */
	int32_t scale = (int32_t) (((uint32_t) newspeed << 16) / (uint32_t) speed);
	for (int k = 0; k < 3; k++)
		pm->vel[k] = (pm->vel[k] * scale + (1 << 15)) >> 16;
}

/* `cap` limits how fast we may end up going along wishdir this step; `rate` is
   the speed the accel rate scales with. Quake's air move passes cap=30 but
   rate=the full wished speed (SV_AirAccelerate), which is what makes air
   strafing possible while keeping straight-line air control weak. */
static void accelerate(pmove_t *pm, const int32_t wishdir[3], int32_t cap, int32_t rate)
{
	int32_t cur = dot_ii(pm->vel, wishdir) >> Q_FIX_SHIFT;   /* speed along wishdir */

	int32_t add = cap - cur;
	if (add <= 0)
		return;

	int32_t accel = PM_ACCEL * rate * pm_dt / PM_HZ;
	if (accel > add)
		accel = add;

	for (int k = 0; k < 3; k++)
		pm->vel[k] += (wishdir[k] * accel) >> Q_FIX_SHIFT;
}

/* --------------------------------------------------------------------- water */

/* SV_CheckWater: the contents at the feet (mins_z + 1), then the waist, then
   the eyes; water, slime and lava count (Quake's `cont <= CONTENTS_WATER`,
   the sky too). Its return is Quake's, `waterlevel > 1`: 1 when the waist is
   under, gravity's switch; the feet alone in the water keep it. */
static int check_water(pmove_t *pm)
{
	int32_t p[3] = { pm->pos[0], pm->pos[1], pm->pos[2] + (PM_MINS_Z + 1) * CLIP_SUB };
	pm->waterlevel = 0;
	pm->watertype  = QW_CONTENTS_EMPTY;
	int c = clip_contents(0, p);
	if (c > QW_CONTENTS_WATER)
		return 0;
	pm->watertype  = c;
	pm->waterlevel = 1;
	p[2] = pm->pos[2] + (PM_MINS_Z + PM_MAXS_Z) / 2 * CLIP_SUB;
	if (clip_contents(0, p) > QW_CONTENTS_WATER)
		return 0;
	pm->waterlevel = 2;
	p[2] = pm->pos[2] + PM_EYE_HEIGHT * CLIP_SUB;
	if (clip_contents(0, p) <= QW_CONTENTS_WATER)
		pm->waterlevel = 3;
	return 1;
}

/* SV_WaterMove: the wish along the view, its pitch too (fwd3), the stick's
   full deflection sv_maxspeed; sinking at 60 units/s when idle; |wishvel|
   capped at sv_maxspeed, then x 0.7; the friction 1 - 4/60 on the whole
   velocity (Quake scales it by newspeed / speed, which is that); then
   the speed gained toward the wish, 10 x wishspeed / 60 a tick, never past
   wishspeed less the speed. wishvel is kept x64 (1/64 units/s). */
static void water_move(pmove_t *pm, const pmove_cmd_t *cmd, const int32_t fwd3[3], const int32_t right[3])
{
	int32_t wv[3];
	for (int k = 0; k < 3; k++)
		wv[k] = ((fwd3[k] * cmd->fwd + right[k] * cmd->side) * PM_MAXSPEED) >> 13;   /* x4096 x128 -> x64 */
	if (!cmd->fwd && !cmd->side && !cmd->up)
		wv[2] -= PM_SINK * 64;
	else
		wv[2] += cmd->up * 64;
	int32_t len = isqrt32((uint32_t) dot_ii(wv, wv));   /* |wishvel| x64 */
	int32_t wishspeed = len > PM_MAXSPEED * 64 ? PM_MAXSPEED * 64 : len;
	wishspeed = wishspeed * 7 / 10;

	for (int k = 0; k < 3; k++)
		pm->vel[k] = pm->vel[k] * water_scale[pm_dt] / 65536;
	int32_t newspeed = isqrt32((uint32_t) dot_ii(pm->vel, pm->vel));

	if (!wishspeed)
		return;
	int32_t addspeed = wishspeed - newspeed * 64;
	if (addspeed <= 0)
		return;
	int32_t accel = PM_ACCEL * wishspeed * pm_dt / PM_HZ;
	if (accel > addspeed)
		accel = addspeed;
	for (int k = 0; k < 3; k++)
		pm->vel[k] += ((wv[k] << 12) / len * accel) >> 18;   /* the wish normalized x4096 */
}

/* SV_WaterJump: CheckWaterJump's push drives the horizontal velocity while
   FL_WATERJUMP holds: up to 2 s, or until out of the water */
static void water_jump(pmove_t *pm)
{
	if (pm->waterjump <= pm_dt || !pm->waterlevel)
		pm->waterjump = 0;
	else
		pm->waterjump -= pm_dt;
	pm->vel[0] = pm->movedir[0];
	pm->vel[1] = pm->movedir[1];
}

/* ----------------------------------------------------------------- step-up */

/* SV_WallFriction: the step's move stopped by a wall the view faces within
   60 degrees of head-on (the forward of the view's angles, its pitch too,
   . the wall's normal < -0.5): the speed along the wall, x (1 + that dot
   + 0.5). The wall vertical: its normal's z 0 */
static void wall_friction(pmove_t *pm)
{
	int32_t sy = r_sin(pm->yaw), cy = r_cos(pm->yaw), cp = r_cos(pm->pitch);
	int32_t fx = (cp * cy) >> 12, fy = (cp * sy) >> 12;
	int32_t d  = ((step_n[0] * fx + step_n[1] * fy) >> 12) + 2048;   /* x4096 */
	if (d >= 0)
		return;
	int32_t i = (pm->vel[0] * step_n[0] + pm->vel[1] * step_n[1]) >> 12;   /* n . vel */
	for (int k = 0; k < 2; k++) {
		int32_t side = pm->vel[k] - ((step_n[k] * i) >> 12);
		pm->vel[k] = (side * (4096 + d)) >> 12;
	}
}

/* --------------------------------------------------------------- pmove_step */

void pmove_step(pmove_t *pm, const pmove_cmd_t *cmd)
{
	pm_dt = cmd->dt;
	pm->touched = 0;
	int32_t sy = r_sin(pm->yaw), cy = r_cos(pm->yaw);
	int32_t right[3] = { sy, -cy, 0 };            /* Quake AngleVectors, roll 0 */

	if (pm->noclip) {
		int32_t sp = r_sin(pm->pitch), cp = r_cos(pm->pitch);
		int32_t fwd[3] = { (cp * cy) >> 12, (cp * sy) >> 12, -sp };
		int s = NOCLIP_SPEED * CLIP_SUB * (cmd->walk ? 1 : 4) * pm_dt;
		int32_t fs = (cmd->fwd * s) >> 7, ss = (cmd->side * s) >> 7;   /* / PM_AXIS */
		for (int k = 0; k < 3; k++)
			pm->pos[k] += (fwd[k] * fs + right[k] * ss) >> Q_FIX_SHIFT;
		pm->pos[2] += cmd->up * s;
		pm->vel[0] = pm->vel[1] = pm->vel[2] = 0;
		return;
	}

	/* aim vectors are horizontal — the player walks flat regardless of pitch */
	int32_t fwd[3] = { cy, sy, 0 };

	categorize_position(pm);

	/* wish direction and speed, as Quake (wishspeed = |wishvel|, capped at
	   sv_maxspeed): forward and right are orthogonal unit vectors (x4096),
	   so |wishvel| = |(fwd, side)|; the direction is normalized by it, the
	   speed follows it up to PM_AXIS (a d-pad direction: exactly full) */
	int32_t wishdir[3] = { 0, 0, 0 }, wishspeed = 0;
	int32_t len = isqrt32((uint32_t) (cmd->fwd * cmd->fwd + cmd->side * cmd->side));
	if (len) {
		wishdir[0] = (fwd[0] * cmd->fwd + right[0] * cmd->side) / len;
		wishdir[1] = (fwd[1] * cmd->fwd + right[1] * cmd->side) / len;
		int32_t amount = len < PM_AXIS ? len : PM_AXIS;
		wishspeed = (cmd->walk ? PM_MAXSPEED / 2 : PM_MAXSPEED) * amount >> 7;
	}

	/* ---- SV_ClientThink: the intent on the velocity (the water level is
	   the last tick's, as Quake's think runs before its physics) ---- */
	if (pm->waterjump) {
		water_jump(pm);
	} else if (pm->waterlevel >= 2) {
		int32_t sp = r_sin(pm->pitch), cp = r_cos(pm->pitch);
		int32_t fwd3[3] = { (cp * cy) >> 12, (cp * sy) >> 12, -sp };
		water_move(pm, cmd, fwd3, right);
	} else if (pm->onground) {
		ground_friction(pm);                     /* friction on the ground, then acceleration */
		accelerate(pm, wishdir, wishspeed, wishspeed);
	} else {
		int32_t cap = wishspeed > 30 ? 30 : wishspeed;
		accelerate(pm, wishdir, cap, wishspeed);
	}

	/* ---- PlayerPreThink (client.qc): WaterMove's drag, CheckWaterJump's
	   push (the game found the ledge), PlayerJump ---- */
	if (pm->waterlevel && !pm->waterjump)
		for (int k = 0; k < 3; k++)
			pm->vel[k] = pm->vel[k] * water_drag[pm_dt][pm->waterlevel] / 65536;
	if (cmd->waterjump) {
		pm->waterjump    = PM_WATERJUMP_TICKS;
		pm->vel[2]       = PM_WATERJUMP_UP;
		pm->jumpreleased = 0;
	}
	if (cmd->jump) {
		if (!pm->waterjump) {
			if (pm->waterlevel >= 2) {
				pm->vel[2] = pm->watertype == QW_CONTENTS_WATER ? 100 : pm->watertype == QW_CONTENTS_SLIME ? 80 : 50;
			} else if (pm->onground && pm->jumpreleased) {
				pm->jumpreleased = 0;
				pm->onground     = 0;            /* don't stairwalk */
				pm->vel[2]      += PM_JUMPSPEED;
			}
		}
	} else {
		pm->jumpreleased = 1;
	}

	/* ---- SV_Physics_Client: the water, gravity unless swimming, the move ---- */
	int swimming = check_water(pm);
	int grav     = !pm->onground && !swimming && !pm->waterjump;
	if (grav) {
		pm->grav_acc += PM_GRAVITY * pm_dt;
		pm->vel[2] -= pm->grav_acc / PM_HZ;
		pm->grav_acc %= PM_HZ;
	} else {
		pm->grav_acc = 0;
	}
	for (int k = 0; k < 3; k++) {
		if (pm->vel[k] >  PM_MAXVELOCITY) pm->vel[k] =  PM_MAXVELOCITY;
		if (pm->vel[k] < -PM_MAXVELOCITY) pm->vel[k] = -PM_MAXVELOCITY;
	}

	/* this frame's displacement, with sub-unit carry so slow walking moves */
	if (grav)
		pm->pos_acc[2] += grav_mid[pm_dt];
	int32_t move[3];
	for (int k = 0; k < 3; k++) {
		pm->pos_acc[k] += pm->vel[k] * CLIP_SUB * pm_dt;
		move[k] = pm->pos_acc[k] / PM_HZ;
		pm->pos_acc[k] -= move[k] * PM_HZ;
	}

	/* plain slide */
	int32_t flat_pos[3]  = { pm->pos[0], pm->pos[1], pm->pos[2] };
	int32_t flat_vel[3]  = { pm->vel[0], pm->vel[1], pm->vel[2] };
	int blocked = slide_move(pm, move);
	int32_t after_flat[3] = { pm->pos[0], pm->pos[1], pm->pos[2] };
	int32_t flat_out_vel[3] = { pm->vel[0], pm->vel[1], pm->vel[2] };

#ifdef PM_DEBUG
	printf(" flat: blocked %d pos %d %d %d c%d\n", blocked, pm->pos[0], pm->pos[1], pm->pos[2], clip_contents(1, pm->pos));
#endif
	/* SV_WalkMove: a wall stopped the move on the ground (or in the water,
	   never while jumping out of it): the move again from 18 units up,
	   level (velocity_z 0), slowed by a wall it meets (SV_WallFriction),
	   then down 18 units less the move's own z (2 more: the ground is met
	   without Quake's gravity on it). Kept when that lands on a floor (its
	   velocity too), else the plain slide's. Quake's SV_TryUnstick (nudges
	   when the step made no progress: a float's remedy) is left out. */
	if ((blocked & 2) && (pm->onground || pm->waterlevel) && !pm->waterjump) {
		pm->pos[0] = flat_pos[0];
		pm->pos[1] = flat_pos[1];
		pm->pos[2] = flat_pos[2];

		int32_t up_from[3] = { pm->pos[0], pm->pos[1], pm->pos[2] };
		int32_t up_to[3]   = { pm->pos[0], pm->pos[1], pm->pos[2] + PM_STEPSIZE * CLIP_SUB };
		trace_t tr;
		clip_trace(1, up_from, up_to, &tr);
		pm->pos[0] = tr.endpos[0];
		pm->pos[1] = tr.endpos[1];
		pm->pos[2] = tr.endpos[2];

		pm->vel[0] = flat_vel[0];
		pm->vel[1] = flat_vel[1];
		pm->vel[2] = 0;
		int32_t level[3] = { move[0], move[1], 0 };
		if (slide_move(pm, level) & 2)
			wall_friction(pm);

		int32_t dn_from[3] = { pm->pos[0], pm->pos[1], pm->pos[2] };
		int32_t dn_to[3]   = { pm->pos[0], pm->pos[1], pm->pos[2] - (PM_STEPSIZE + 2) * CLIP_SUB + move[2] };
		clip_trace(1, dn_from, dn_to, &tr);
#ifdef PM_DEBUG
		printf(" step: down frac %d n %d %d %d end %d %d %d c%d\n", tr.fraction, tr.plane_n[0],
		       tr.plane_n[1], tr.plane_n[2], tr.endpos[0], tr.endpos[1], tr.endpos[2],
		       clip_contents(1, tr.endpos));
#endif
		if (tr.fraction < Q_FIX_ONE && tr.plane_n[2] > FLOOR_NZ) {
			pm->pos[0] = tr.endpos[0];            /* on a floor: the step kept */
			pm->pos[1] = tr.endpos[1];
			pm->pos[2] = tr.endpos[2];
		} else {
			pm->pos[0] = after_flat[0];
			pm->pos[1] = after_flat[1];
			pm->pos[2] = after_flat[2];
			pm->vel[0] = flat_out_vel[0];
			pm->vel[1] = flat_out_vel[1];
			pm->vel[2] = flat_out_vel[2];
		}
	}

	categorize_position(pm);
}
