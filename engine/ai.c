/*
 * q1psx — the monsters' AI and movement: ai.qc, fight.qc and monsters.qc's
 * shared code, sv_move.c (SV_movestep, SV_StepDirection, SV_NewChaseDir,
 * SV_MoveToGoal, SV_CheckBottom), the builtins they call (walkmove,
 * droptofloor, changeyaw, checkclient, traceline, vectoyaw) and sv_phys.c's
 * SV_Physics_Step / SV_Physics_Toss for the monsters and what they throw.
 * Transcribed line by line; where the rerelease's QC differs from the
 * shareware's progs.dat (1.06), the latter's (its statements read back).
 * See engine/monsters.h.
 */

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "level.h"
#include "monsters.h"
#include "q_fixed.h"
#include "render.h"
#include "snd.h"
#include "world.h"

edict_t *ai_sight_entity;
int32_t  ai_sight_entity_time;
int      ai_enemy_visible, ai_enemy_infront, ai_enemy_range, ai_enemy_yaw;
int      ai_movedist;
int      ai_damage_take;
int      ai_trace_inopen, ai_trace_inwater;
int32_t  ai_trace_endpos[3];
edict_t *ai_trace_ent;
int16_t  ai_trace_normal[3];

#define SUB      CLIP_SUB
#define STEPSIZE (18 * SUB)
#define NEVER    0x3fffffff         /* pausetime 99999999 (a time never reached) */

/* -------------------------------------------------------------- numbers -- */

uint32_t frand(void)
{
	return q_rand() & 0xffff;
}

int crand(void)
{
	return (int) frand() * 2 - 65535;
}

/* atan2 in degrees x 256, (-180, 180]: an octant reduction, then atan(t)
   for t in 0..1 from a 257-entry table and a linear step (within 1/256 of
   a degree: vectoyaw truncates whole degrees as Quake's float does) */
static int32_t atan2_d256(int32_t y, int32_t x)
{
	static uint16_t atab[257];          /* atan(i / 256), degrees x 256 */
	if (!atab[256]) {
		/* built once: atan(t) by its series on t <= 1/2, then atan(t) =
		   pi/4 - atan((1 - t)/(1 + t)) above (no floats on the PS1:
		   fixed point 2.30) */
		for (int i = 0; i <= 256; i++) {
			int64_t t = ((int64_t) i << 30) / 256, a;
			int flip = t > (1 << 29);
			if (flip)
				t = (((int64_t) 1 << 30) - t << 30) / (((int64_t) 1 << 30) + t);
			int64_t t2 = t * t >> 30, term = t, sum = 0;
			for (int k = 1; k < 40; k += 2) {
				sum += ((k >> 1) & 1) ? -term / k : term / k;
				term = term * t2 >> 30;
			}
			a = flip ? 843314857 - sum : sum;          /* pi/4 x 2^30 */
			atab[i] = (uint16_t) ((a * 180 * 256 / 3373259426LL + 0) );   /* pi x 2^30 */
		}
	}
	if (!x && !y)
		return 0;
	int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, a;
	int swap = ay > ax;
	int32_t num = swap ? ax : ay, den = swap ? ay : ax;
	while (den >= (1 << 15)) {
		num >>= 1;
		den >>= 1;
	}
	if (!den)
		den = 1;
	uint32_t t = ((uint32_t) num << 16) / (uint32_t) den;   /* 0 .. 65536 */
	uint32_t i = t >> 8, f = t & 255;
	a = i >= 256 ? atab[256] : (int32_t) (atab[i] + (((atab[i + 1] - atab[i]) * f + 128) >> 8));
	if (swap) a = 90 * 256 - a;
	if (x < 0) a = 180 * 256 - a;
	return y < 0 ? -a : a;
}

int vectoyaw(const int32_t v[3])
{
	if (!v[0] && !v[1])
		return 0;
	int yaw = atan2_d256(v[1], v[0]) / 256;   /* (int) truncates toward 0 */
	if (yaw < 0)
		yaw += 360;
	return yaw;
}

/* makevectors(angles): Quake's AngleVectors, x4096 (the angles 65536 a
   turn, the renderer's sines 4096 a turn) */
void ai_makevectors(const uint16_t ang[3], int32_t f[3], int32_t r[3], int32_t u[3])
{
	int p = ang[0] >> 4, y = ang[1] >> 4, ro = ang[2] >> 4;
	int32_t sy = r_sin(y), cy = r_cos(y), sp = r_sin(p), cp = r_cos(p), sr = r_sin(ro), cr = r_cos(ro);
	f[0] = cp * cy >> 12;
	f[1] = cp * sy >> 12;
	f[2] = -sp;
	r[0] = ((-sr * sp >> 12) * cy >> 12) + (cr * sy >> 12);
	r[1] = ((-sr * sp >> 12) * sy >> 12) - (cr * cy >> 12);
	r[2] = -sr * cp >> 12;
	u[0] = ((cr * sp >> 12) * cy >> 12) + (sr * sy >> 12);
	u[1] = ((cr * sp >> 12) * sy >> 12) - (sr * cy >> 12);
	u[2] = cr * cp >> 12;
}

int ai_vlen(const int32_t v[3])
{
	return (int) (q_isqrt64((uint64_t) ((int64_t) v[0] * v[0] + (int64_t) v[1] * v[1] +
	                                    (int64_t) v[2] * v[2])) / SUB);
}

/* degrees -> the angles' 65536 a turn, as anglemod truncates */
static uint16_t deg_bam(int deg)
{
	return (uint16_t) (deg * 65536 / 360);
}

/* --------------------------------------------------------------- traces -- */

/* the world's hull of an entity's size (SV_HullForEntity on the world) */
static int hull_of(const edict_t *e)
{
	int32_t sx = e->maxs[0] - e->mins[0];
	if (sx < 3 * SUB) return 0;
	if (sx <= 32 * SUB) return 1;
	return 2;
}

/* SV_Move for entity e (its box, itself out of the traces: its clip entry,
   or a missile's owner's) */
static q_fixed ent_trace(edict_t *e, const int32_t start[3], const int32_t end[3], int nomonsters,
                         trace_t *tr)
{
	for (int k = 0; k < 3; k++) {
		clip_mover.box[k]     = e->mins[k];
		clip_mover.box[k + 3] = e->maxs[k];
	}
	clip_mover.pass       = e->clip ? e->clip : e->owner ? e->owner->clip : 0;
	clip_mover.nomonsters = nomonsters;
	return clip_trace_ent(hull_of(e), start, end, tr);
}

q_fixed ai_traceline(const int32_t a[3], const int32_t b[3], int nomonsters, edict_t *pass)
{
	trace_t tr;
	memset(clip_mover.box, 0, sizeof clip_mover.box);
	clip_mover.pass       = pass ? pass->clip : 0;
	clip_mover.nomonsters = nomonsters;
	clip_trace_ent(0, a, b, &tr);
	ai_trace_inopen  = tr.inopen;
	ai_trace_inwater = tr.inwater;
	memcpy(ai_trace_endpos, tr.endpos, sizeof ai_trace_endpos);
	memcpy(ai_trace_normal, tr.plane_n, sizeof ai_trace_normal);
	ai_trace_ent = tr.fraction < Q_FIX_ONE ? (ent_from_trace(&tr) ? ent_from_trace(&tr) : ent_world) : 0;
	return tr.fraction;
}

void ai_view_org(const edict_t *e, int32_t out[3])
{
	memcpy(out, e->origin, 3 * sizeof *out);
	out[2] += e == ent_player ? pl.viewheight : e->view_ofs_z;
}

/* ----------------------------------------------------------- checkclient -- */

/* a leaf's index in world.leaves: world_point_leaf's pointers are the
   walk's KUSEG ones (the children's), the table KSEG0's */
static int leaf_index(const qw_leaf *l)
{
	return (int) ((const qw_leaf *) ((uintptr_t) l | 0x80000000u) - world.leaves);
}

/* PF_checkclient: the player, if the monster's eye is in the leaves the
   player's eye sees (its PVS row, taken again every tenth of a second);
   not when dead or not a target */
static uint8_t check_pvs[1024];
static int32_t check_time = -1000;
static int     check_ok;

void ai_map_start(void)
{
	check_time = -1000;
	check_ok = 0;
	ai_sight_entity = 0;
	ai_sight_entity_time = -1000;
}

static void new_check(void)
{
	check_ok = 0;
	if (pl.dead || pl.health <= 0)
		return;
	int32_t eye[3];
	ai_view_org(ent_player, eye);
	for (int k = 0; k < 3; k++)
		eye[k] /= SUB;
	const qw_leaf *leaf = world_point_leaf(eye);
	if (leaf)
		leaf = (const qw_leaf *) ((uintptr_t) leaf | 0x80000000u);
	uint32_t row = world.vis_rowbytes;
	if (row > sizeof check_pvs)
		row = sizeof check_pvs;
	const uint8_t *in = leaf && leaf->vis ? (const uint8_t *) ((leaf->vis & 0x00ffffffu) | 0x80000000u) : 0;
	if (!in) {
		memset(check_pvs, 0xff, row);
	} else {
		uint32_t o = 0;                       /* Mod_DecompressVis */
		while (o < row) {
			if (*in) {
				check_pvs[o++] = *in++;
				continue;
			}
			int c = in[1];
			in += 2;
			while (c-- > 0 && o < row)
				check_pvs[o++] = 0;
		}
	}
	check_ok = 1;
}

edict_t *ai_checkclient(edict_t *self)
{
	if (ent_time - check_time >= TICKS(10)) {
		new_check();
		check_time = ent_time;
	}
	if (!check_ok || pl.dead || pl.health <= 0)
		return 0;
	int32_t view[3];
	ai_view_org(self, view);
	for (int k = 0; k < 3; k++)
		view[k] /= SUB;
	const qw_leaf *leaf = world_point_leaf(view);
	int l = leaf ? leaf_index(leaf) - 1 : -1;
	if (l < 0 || (uint32_t) (l >> 3) >= world.vis_rowbytes || !(check_pvs[l >> 3] & (1 << (l & 7))))
		return 0;
	return ent_player;
}

/* ------------------------------------------------------------ sv_move.c -- */

/* SV_CheckBottom: false if a part of the box's bottom is off an edge that
   is not a staircase */
int ai_check_bottom(edict_t *ent)
{
	int32_t mins[3], maxs[3], start[3], stop[3];
	for (int k = 0; k < 3; k++) {
		mins[k] = ent->origin[k] + ent->mins[k];
		maxs[k] = ent->origin[k] + ent->maxs[k];
	}
	/* every point under the corners solid world: done */
	start[2] = mins[2] - SUB;
	for (int x = 0; x <= 1; x++)
		for (int y = 0; y <= 1; y++) {
			start[0] = x ? maxs[0] : mins[0];
			start[1] = y ? maxs[1] : mins[1];
			if (clip_contents(0, start) != QW_CONTENTS_SOLID)
				goto realcheck;
		}
	return 1;
realcheck:
	/* the midpoint must be within 16 of the bottom */
	start[2] = mins[2];
	start[0] = stop[0] = (mins[0] + maxs[0]) / 2;
	start[1] = stop[1] = (mins[1] + maxs[1]) / 2;
	stop[2] = start[2] - 2 * STEPSIZE;
	if (ai_traceline(start, stop, 1, ent) >= Q_FIX_ONE)
		return 0;
	int32_t mid, bottom;
	mid = bottom = ai_trace_endpos[2];
	/* the corners must be within 16 of the midpoint */
	for (int x = 0; x <= 1; x++)
		for (int y = 0; y <= 1; y++) {
			start[0] = stop[0] = x ? maxs[0] : mins[0];
			start[1] = stop[1] = y ? maxs[1] : mins[1];
			q_fixed f = ai_traceline(start, stop, 1, ent);
			if (f < Q_FIX_ONE && ai_trace_endpos[2] > bottom)
				bottom = ai_trace_endpos[2];
			if (f >= Q_FIX_ONE || mid - ai_trace_endpos[2] > STEPSIZE)
				return 0;
		}
	return 1;
}

/* SV_LinkEdict(ent, true): its box linked, the triggers it reaches touched */
static void link_touch(edict_t *ent)
{
	ent_link(ent);
	ents_touch_links(ent);
}

/* SV_movestep: the move adjusted for slopes and stairs, or none (0) */
static int movestep(edict_t *ent, const int32_t move[3], int relink)
{
	int32_t oldorg[3], neworg[3], end[3];
	trace_t tr;
	memcpy(oldorg, ent->origin, sizeof oldorg);
	for (int k = 0; k < 3; k++)
		neworg[k] = ent->origin[k] + move[k];

	/* flying monsters don't step up: one move with vertical motion, then
	   one without */
	if (ent->flags & (FL_SWIM | FL_FLY)) {
		for (int i = 0; i < 2; i++) {
			for (int k = 0; k < 3; k++)
				neworg[k] = ent->origin[k] + move[k];
			edict_t *enemy = ent->enemy;
			if (i == 0 && enemy) {
				int32_t dz = ent->origin[2] - enemy->origin[2];
				if (dz > 40 * SUB) neworg[2] -= 8 * SUB;
				if (dz < 30 * SUB) neworg[2] += 8 * SUB;
			}
			ent_trace(ent, ent->origin, neworg, 0, &tr);
			if (tr.fraction >= Q_FIX_ONE) {
				if ((ent->flags & FL_SWIM) && clip_contents(0, tr.endpos) == QW_CONTENTS_EMPTY)
					return 0;            /* swim monster left water */
				memcpy(ent->origin, tr.endpos, sizeof ent->origin);
				if (relink)
					link_touch(ent);
				return 1;
			}
			if (!enemy)
				break;
		}
		return 0;
	}

	/* push down from a step height above the wished position */
	neworg[2] += STEPSIZE;
	memcpy(end, neworg, sizeof end);
	end[2] -= 2 * STEPSIZE;
	ent_trace(ent, neworg, end, 0, &tr);
	if (tr.allsolid)
		return 0;
	if (tr.startsolid) {
		neworg[2] -= STEPSIZE;
		ent_trace(ent, neworg, end, 0, &tr);
		if (tr.allsolid || tr.startsolid)
			return 0;
	}
	if (tr.fraction >= Q_FIX_ONE) {
		/* the monster had the ground pulled out: it falls */
		if (ent->flags & FL_PARTIALGROUND) {
			for (int k = 0; k < 3; k++)
				ent->origin[k] += move[k];
			if (relink)
				link_touch(ent);
			ent->flags &= ~FL_ONGROUND;
			return 1;
		}
		return 0;                         /* walked off an edge */
	}
	/* check point traces down for dangling corners */
	memcpy(ent->origin, tr.endpos, sizeof ent->origin);
	if (!ai_check_bottom(ent)) {
		if (ent->flags & FL_PARTIALGROUND) {
			/* the floor mostly pulled out from under it, it is trying to
			   correct */
			if (relink)
				link_touch(ent);
			return 1;
		}
		memcpy(ent->origin, oldorg, sizeof ent->origin);
		return 0;
	}
	if (ent->flags & FL_PARTIALGROUND)
		ent->flags &= ~FL_PARTIALGROUND;
	/* (groundentity: not kept) */
	if (relink)
		link_touch(ent);
	return 1;
}

/* PF_changeyaw: toward ideal_yaw by yaw_speed at most, in degrees x 65536
   (exact: the angle is a multiple of 360/65536, anglemod's) */
void ai_changeyaw(edict_t *ent)
{
	int32_t cur = (int32_t) ent->ang[1] * 360, ideal = (int32_t) ent->ideal_yaw * 65536;
	if (cur == ideal)
		return;
	int32_t move = ideal - cur, speed = (int32_t) ent->yaw_speed * 65536;
	if (ideal > cur) {
		if (move >= 180 * 65536)
			move -= 360 * 65536;
	} else if (move <= -180 * 65536) {
		move += 360 * 65536;
	}
	if (move > 0) {
		if (move > speed) move = speed;
	} else if (move < -speed) {
		move = -speed;
	}
	ent->ang[1] = (uint16_t) ((cur + move) / 360);   /* (int) truncates, & 65535 */
}

/* yaw (degrees) and dist (units) as a move, 1/8 units */
static void yaw_move(int yaw, int dist, int32_t move[3])
{
	int a = yaw * ANG_TURN / 360;
	move[0] = r_cos(a) * dist * SUB / 4096;
	move[1] = r_sin(a) * dist * SUB / 4096;
	move[2] = 0;
}

int ai_walkmove(edict_t *ent, int yaw, int dist)
{
	if (!(ent->flags & (FL_ONGROUND | FL_FLY | FL_SWIM)))
		return 0;
	int32_t move[3];
	yaw_move(yaw, dist, move);
	return movestep(ent, move, 1);
}

/* SV_StepDirection: turn to the move's direction, walk the distance if
   facing it */
static int step_direction(edict_t *ent, int yaw, int dist)
{
	ent->ideal_yaw = (int16_t) yaw;
	ai_changeyaw(ent);
	int32_t move[3], oldorigin[3];
	yaw_move(yaw, dist, move);
	memcpy(oldorigin, ent->origin, sizeof oldorigin);
	if (movestep(ent, move, 0)) {
		/* delta = angles[YAW] - ideal_yaw, as Quake's floats compare it */
		int32_t delta = (int32_t) ent->ang[1] * 360 - (int32_t) ent->ideal_yaw * 65536;
		if (delta > 45 * 65536 && delta < 315 * 65536)
			memcpy(ent->origin, oldorigin, sizeof ent->origin);   /* not turned far enough */
		link_touch(ent);
		return 1;
	}
	link_touch(ent);
	return 0;
}

#define DI_NODIR (-1)

static int anglemod_deg(int a)
{
	a %= 360;
	return a < 0 ? a + 360 : a;
}

/* SV_NewChaseDir */
static void new_chase_dir(edict_t *actor, edict_t *enemy, int dist)
{
	int olddir = anglemod_deg((actor->ideal_yaw / 45) * 45);
	int turnaround = anglemod_deg(olddir - 180);
	int32_t deltax = enemy->origin[0] - actor->origin[0];
	int32_t deltay = enemy->origin[1] - actor->origin[1];
	int d1, d2, tdir;
	if (deltax > 10 * SUB)       d1 = 0;
	else if (deltax < -10 * SUB) d1 = 180;
	else                         d1 = DI_NODIR;
	if (deltay < -10 * SUB)      d2 = 270;
	else if (deltay > 10 * SUB)  d2 = 90;
	else                         d2 = DI_NODIR;

	/* try direct route */
	if (d1 != DI_NODIR && d2 != DI_NODIR) {
		if (d1 == 0)
			tdir = d2 == 90 ? 45 : 315;
		else
			tdir = d2 == 90 ? 135 : 215;
		if (tdir != turnaround && step_direction(actor, tdir, dist))
			return;
	}
	/* try other directions (Quake's abs on the floats' int parts) */
	int32_t ady = deltay / SUB, adx = deltax / SUB;
	if (ady < 0) ady = -ady;
	if (adx < 0) adx = -adx;
	if (((q_rand() & 3) & 1) || ady > adx) {
		tdir = d1;
		d1 = d2;
		d2 = tdir;
	}
	if (d1 != DI_NODIR && d1 != turnaround && step_direction(actor, d1, dist))
		return;
	if (d2 != DI_NODIR && d2 != turnaround && step_direction(actor, d2, dist))
		return;
	/* there is no direct path to the player, so pick another direction */
	if (olddir != DI_NODIR && step_direction(actor, olddir, dist))
		return;
	if (q_rand() & 1) {                   /* randomly determine direction of search */
		for (tdir = 0; tdir <= 315; tdir += 45)
			if (tdir != turnaround && step_direction(actor, tdir, dist))
				return;
	} else {
		for (tdir = 315; tdir >= 0; tdir -= 45)
			if (tdir != turnaround && step_direction(actor, tdir, dist))
				return;
	}
	if (turnaround != DI_NODIR && step_direction(actor, turnaround, dist))
		return;
	actor->ideal_yaw = (int16_t) olddir;  /* can't move */
	/* if a bridge was pulled out from underneath a monster, it may not have
	   a valid standing position at all */
	if (!ai_check_bottom(actor))
		actor->flags |= FL_PARTIALGROUND;  /* SV_FixCheckBottom */
}

/* SV_CloseEnough */
static int close_enough(const edict_t *ent, const edict_t *goal, int dist)
{
	int32_t d = dist * SUB;
	for (int i = 0; i < 3; i++) {
		if (goal->absmin[i] > ent->absmax[i] + d)
			return 0;
		if (goal->absmax[i] < ent->absmin[i] - d)
			return 0;
	}
	return 1;
}

/* SV_MoveToGoal */
void ai_movetogoal(edict_t *ent, int dist)
{
	edict_t *goal = ent->goalentity;
	if (!(ent->flags & (FL_ONGROUND | FL_FLY | FL_SWIM)))
		return;
	if (!goal)
		goal = ent_world;
	/* if the next step hits the enemy, return immediately */
	if (ent->enemy && close_enough(ent, goal, dist))
		return;
	/* bump around... */
	if ((q_rand() & 3) == 1 || !step_direction(ent, ent->ideal_yaw, dist))
		new_chase_dir(ent, goal, dist);
}

/* PF_droptofloor */
int ai_droptofloor(edict_t *ent)
{
	int32_t end[3];
	trace_t tr;
	memcpy(end, ent->origin, sizeof end);
	end[2] -= 256 * SUB;
	ent_trace(ent, ent->origin, end, 0, &tr);
	if (tr.fraction >= Q_FIX_ONE || tr.allsolid)
		return 0;
	memcpy(ent->origin, tr.endpos, sizeof ent->origin);
	ent_link(ent);
	ent->flags |= FL_ONGROUND;
	return 1;
}

/* --------------------------------------------------------------- ai.qc -- */

/* range: 0 melee (120), 1 near (500), 2 mid (1000), 3 far */
int ai_range(edict_t *self, edict_t *targ)
{
	int32_t a[3], b[3], d[3];
	ai_view_org(self, a);
	ai_view_org(targ, b);
	for (int k = 0; k < 3; k++)
		d[k] = a[k] - b[k];
	int r = ai_vlen(d);
	if (r < 120) return RANGE_MELEE;
	if (r < 500) return RANGE_NEAR;
	if (r < 1000) return RANGE_MID;
	return RANGE_FAR;
}

/* visible: seen even from behind (other monsters seen through) */
int ai_visible(edict_t *self, edict_t *targ)
{
	int32_t a[3], b[3];
	ai_view_org(self, a);
	ai_view_org(targ, b);
	q_fixed f = ai_traceline(a, b, 1, self);
	if (ai_trace_inopen && ai_trace_inwater)
		return 0;                         /* sight line crossed contents */
	return f >= Q_FIX_ONE;
}

/* infront: within the 0.3 cone of the facing (1.06: makevectors with the
   angles as they are) */
int ai_infront(edict_t *self, edict_t *targ)
{
	int32_t f[3], r[3], u[3], v[3];
	ai_makevectors(self->ang, f, r, u);
	for (int k = 0; k < 3; k++)
		v[k] = targ->origin[k] - self->origin[k];
	int64_t dot = (int64_t) v[0] * f[0] + (int64_t) v[1] * f[1] + (int64_t) v[2] * f[2];
	int64_t len = (int64_t) q_isqrt64((uint64_t) ((int64_t) v[0] * v[0] + (int64_t) v[1] * v[1] +
	                                              (int64_t) v[2] * v[2]));
	return dot * 10 > len * 4096 * 3;     /* normalize (vec) . v_forward > 0.3 */
}

void sub_attack_finished(edict_t *self, int ticks)
{
	self->cnt = 0;                        /* refire count for nightmare */
	if (game_skill != 3)
		self->attack_finished = ent_time + ticks;
}

/* SUB_CheckRefire: nightmare's second shot */
void sub_check_refire(edict_t *self, int state)
{
	if (game_skill != 3)
		return;
	if (self->cnt == 1)
		return;
	if (!ai_visible(self, self->enemy))
		return;
	self->cnt = 1;
	self->mstate = (uint16_t) state;
	self->think  = monster_think;
}

void ai_hunt_target(edict_t *self)
{
	int32_t d[3];
	self->goalentity = self->enemy;
	self->think = monster_think;
	const mdef_t *m = self->mdef;
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	self->ideal_yaw = (int16_t) vectoyaw(d);
	self->nextthink = ent_time + TICKS(10);
	sub_attack_finished(self, TICKS(100));   /* wait a while before first attack */
	m->th_run(self);
	self->nextthink = ent_time + TICKS(10);   /* (the think the QC sets: th_run's state at the next think) */
}

/* SightSound (1.06: by the classname, here the kind's sound) */
static void sight_sound(edict_t *self)
{
	const mdef_t *m = self->mdef;
	if (m->sight)
		ent_sound(self, CHAN_VOICE, *m->sight, 255, ATTN_NORM);
}

void ai_found_target(edict_t *self)
{
	if (self->enemy == ent_player) {      /* let other monsters see this monster for a while */
		ai_sight_entity      = self;
		ai_sight_entity_time = ent_time;
	}
	self->show_hostile = ent_time + TICKS(100);   /* wake up other monsters */
	sight_sound(self);
	ai_hunt_target(self);
}

/* FindTarget: the player sighted (or another monster's anger seen) */
int ai_find_target(edict_t *self)
{
	edict_t *client;
	/* spawnflags & 3: the zombie's crucified flag came first, ambush second */
	if (ai_sight_entity_time >= ent_time - TICKS(10) && !(self->spawnflags & 3)) {
		client = ai_sight_entity;
		if (client && client->enemy == self->enemy)
			return 1;
	} else {
		client = ai_checkclient(self);
		if (!client)
			return 0;                     /* current check entity isn't in PVS */
	}
	if (!client || client == self->enemy)
		return 0;
	if (client->flags & FL_NOTARGET)
		return 0;
	if (client->items & IT_INVISIBILITY)
		return 0;
	int r = ai_range(self, client);
	if (r == RANGE_FAR)
		return 0;
	if (!ai_visible(self, client))
		return 0;
	if (r == RANGE_NEAR) {
		if (client->show_hostile < ent_time && !ai_infront(self, client))
			return 0;
	} else if (r == RANGE_MID) {
		if (!ai_infront(self, client))
			return 0;
	}
	/* got one */
	self->enemy = client;
	if (self->enemy != ent_player) {
		self->enemy = self->enemy->enemy;
		if (self->enemy != ent_player) {
			self->enemy = 0;
			return 0;
		}
	}
	ai_found_target(self);
	return 1;
}

void ai_forward(edict_t *self, int dist)
{
	ai_walkmove(self, self->ang[1] * 360 / 65536, dist);
}

void ai_back(edict_t *self, int dist)
{
	ai_walkmove(self, self->ang[1] * 360 / 65536 + 180, dist);
}

void ai_pain(edict_t *self, int dist)
{
	ai_back(self, dist);
}

void ai_painforward(edict_t *self, int dist)
{
	ai_walkmove(self, self->ideal_yaw, dist);
}

void ai_walk(edict_t *self, int dist)
{
	ai_movedist = dist;
	if (ai_find_target(self))             /* check for noticing a player */
		return;
	ai_movetogoal(self, dist);
}

void ai_stand(edict_t *self)
{
	if (ai_find_target(self))
		return;
	if (ent_time > self->pausetime) {
		self->mdef->th_walk(self);
		return;
	}
}

void ai_turn(edict_t *self)
{
	if (ai_find_target(self))
		return;
	ai_changeyaw(self);
}

int ai_facing_ideal(edict_t *self)
{
	int delta = anglemod_deg((int) ((int32_t) self->ang[1] * 360 / 65536) - self->ideal_yaw);
	return !(delta > 45 && delta < 315);
}

/* CheckAnyAttack: the kind's own CheckAttack, or the generic one */
static int check_any_attack(edict_t *self)
{
	if (!ai_enemy_visible)
		return 0;
	if (self->mdef->check_attack)
		return self->mdef->check_attack(self);
	return ai_check_attack(self);
}

static void ai_run_melee(edict_t *self)
{
	self->ideal_yaw = (int16_t) ai_enemy_yaw;
	ai_changeyaw(self);
	if (ai_facing_ideal(self)) {
		self->mdef->th_melee(self);
		self->attack_state = AS_STRAIGHT;
	}
}

static void ai_run_missile(edict_t *self)
{
	self->ideal_yaw = (int16_t) ai_enemy_yaw;
	ai_changeyaw(self);
	if (ai_facing_ideal(self)) {
		self->mdef->th_missile(self);
		self->attack_state = AS_STRAIGHT;
	}
}

static void ai_run_slide(edict_t *self)
{
	self->ideal_yaw = (int16_t) ai_enemy_yaw;
	ai_changeyaw(self);
	int ofs = self->lefty ? 90 : -90;
	if (ai_walkmove(self, self->ideal_yaw + ofs, ai_movedist))
		return;
	self->lefty = (uint8_t) (1 - self->lefty);
	ai_walkmove(self, self->ideal_yaw - ofs, ai_movedist);
}

void ai_run(edict_t *self, int dist)
{
	ai_movedist = dist;
	/* see if the enemy is dead */
	if (!self->enemy || self->enemy->health <= 0) {
		self->enemy = 0;
		if (self->oldenemy && self->oldenemy->health > 0) {
			self->enemy = self->oldenemy;
			ai_hunt_target(self);
		} else {
			if (self->movetarget)
				self->mdef->th_walk(self);
			else
				self->mdef->th_stand(self);
			return;
		}
	}
	self->show_hostile = ent_time + TICKS(100);   /* wake up other monsters */
	/* check knowledge of enemy */
	ai_enemy_visible = ai_visible(self, self->enemy);
	if (ai_enemy_visible)
		self->search_time = ent_time + TICKS(500);
	ai_enemy_infront = ai_infront(self, self->enemy);
	ai_enemy_range   = ai_range(self, self->enemy);
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	ai_enemy_yaw = vectoyaw(d);
	if (self->attack_state == AS_MISSILE) {
		ai_run_missile(self);
		return;
	}
	if (self->attack_state == AS_MELEE) {
		ai_run_melee(self);
		return;
	}
	if (check_any_attack(self))
		return;                           /* beginning an attack */
	if (self->attack_state == AS_SLIDING) {
		ai_run_slide(self);
		return;
	}
	ai_movetogoal(self, dist);
}

/* ------------------------------------------------------------ fight.qc -- */

/* CheckAttack: the player in view, move or attack? (0: move on) */
int ai_check_attack(edict_t *self)
{
	edict_t *targ = self->enemy;
	int32_t spot1[3], spot2[3];
	ai_view_org(self, spot1);
	ai_view_org(targ, spot2);
	ai_traceline(spot1, spot2, 0, self);
	if (ai_trace_ent != targ)
		return 0;                         /* don't have a clear shot */
	if (ai_trace_inopen && ai_trace_inwater)
		return 0;                         /* sight line crossed contents */
	const mdef_t *m = self->mdef;
	if (ai_enemy_range == RANGE_MELEE && m->th_melee) {
		m->th_melee(self);                /* (the knight's: knight_attack, its th_melee) */
		return 1;
	}
	/* missile attack */
	if (!m->th_missile)
		return 0;
	if (ent_time < self->attack_finished)
		return 0;
	if (ai_enemy_range == RANGE_FAR)
		return 0;
	uint32_t chance;                      /* x65536 */
	if (ai_enemy_range == RANGE_MELEE) {
		chance = 58982;                   /* 0.9 */
		self->attack_finished = 0;
	} else if (ai_enemy_range == RANGE_NEAR) {
		chance = m->th_melee ? 13107 : 26214;   /* 0.2, 0.4 */
	} else if (ai_enemy_range == RANGE_MID) {
		chance = m->th_melee ? 3277 : 6554;     /* 0.05, 0.1 */
	} else {
		chance = 0;
	}
	if (frand() < chance) {
		m->th_missile(self);
		sub_attack_finished(self, (int) (frand() * 2 * TICKS(100) >> 16));
		return 1;
	}
	return 0;
}

void ai_face(edict_t *self)
{
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	self->ideal_yaw = (int16_t) vectoyaw(d);
	ai_changeyaw(self);
}

void ai_charge(edict_t *self, int dist)
{
	ai_face(self);
	ai_movetogoal(self, dist);
}

/* ai_charge_side: aim to the left of the enemy for a flyby (1.06:
   makevectors with the angles as they are) */
void ai_charge_side(edict_t *self)
{
	int32_t d[3], f[3], r[3], u[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	self->ideal_yaw = (int16_t) vectoyaw(d);
	ai_changeyaw(self);
	ai_makevectors(self->ang, f, r, u);
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - 30 * SUB * r[k] / 4096 - self->origin[k];
	ai_walkmove(self, vectoyaw(d), 20);
}

/* the melee's damage: (random() + random() + random()) x 3, x256 */
static int melee_damage(void)
{
	return (int) (((frand() + frand() + frand()) * 3 * 256) >> 16);
}

void ai_melee(edict_t *self)
{
	if (!self->enemy)
		return;                           /* removed before stroke */
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	if (ai_vlen(d) > 60)
		return;
	ent_damage_x256(self->enemy, self, self, melee_damage());
}

void ai_melee_side(edict_t *self)
{
	if (!self->enemy)
		return;                           /* removed before stroke */
	ai_charge_side(self);
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	if (ai_vlen(d) > 60)
		return;
	if (!ai_can_damage(self->enemy, self))
		return;
	ent_damage_x256(self->enemy, self, self, melee_damage());
}

/* combat.qc CanDamage: a clear line to the target (a pusher: to its box's
   centre), or to one of four points around it */
int ai_can_damage(edict_t *targ, edict_t *inflictor)
{
	if (targ->movetype == MOVETYPE_PUSH) {
		int32_t c[3];
		for (int k = 0; k < 3; k++)
			c[k] = (targ->absmin[k] + targ->absmax[k]) / 2;
		q_fixed f = ai_traceline(inflictor->origin, c, 1, inflictor);
		return f >= Q_FIX_ONE || ai_trace_ent == targ;
	}
	static const int8_t off[5][2] = { { 0, 0 }, { 15, 15 }, { -15, -15 }, { -15, 15 }, { 15, -15 } };
	for (int i = 0; i < 5; i++) {
		int32_t p[3] = { targ->origin[0] + off[i][0] * SUB, targ->origin[1] + off[i][1] * SUB, targ->origin[2] };
		if (ai_traceline(inflictor->origin, p, 1, inflictor) >= Q_FIX_ONE)
			return 1;
	}
	return 0;
}

/* -------------------------------------------------------------- physics -- */

/* SV_CheckWaterTransition */
static void check_water_transition(edict_t *ent)
{
	int cont = clip_contents(0, ent->origin);
	if (!ent->watertype) {                /* just spawned here */
		ent->watertype  = (int8_t) cont;
		ent->waterlevel = 1;
		return;
	}
	if (cont <= QW_CONTENTS_WATER) {
		if (ent->watertype == QW_CONTENTS_EMPTY)   /* just crossed into water */
			ent_sound(ent, CHAN_AUTO, snd_find("misc/h2ohit1.wav"), 255, ATTN_NORM);
		ent->watertype  = (int8_t) cont;
		ent->waterlevel = 1;
	} else {
		if (ent->watertype != QW_CONTENTS_EMPTY)   /* just crossed out of water */
			ent_sound(ent, CHAN_AUTO, snd_find("misc/h2ohit1.wav"), 255, ATTN_NORM);
		ent->watertype  = QW_CONTENTS_EMPTY;
		ent->waterlevel = (uint8_t) cont;          /* (Quake's: the contents) */
	}
}

/* ClipVelocity, units per second, the normal x4096; STOP_EPSILON 0.1 */
static int clip_velocity(const int32_t in[3], const int16_t n[3], int32_t out[3], int over2)
{
	int blocked = 0;
	if (n[2] > 0)
		blocked |= 1;                     /* floor */
	if (!n[2])
		blocked |= 2;                     /* step */
	int32_t backoff = (int32_t) (((int64_t) in[0] * n[0] + (int64_t) in[1] * n[1] +
	                              (int64_t) in[2] * n[2]) * over2 / 2 >> 12);
	for (int i = 0; i < 3; i++) {
		out[i] = in[i] - (n[i] * backoff >> 12);
		if (out[i] > -1 && out[i] < 1)
			out[i] = 0;
	}
	return blocked;
}

/* SV_Impact: both touches */
static void impact(edict_t *e1, edict_t *e2)
{
	edict_t *other = ent_other;
	if (e1->touch && e1->solid != SOLID_NOT) {
		ent_other = e2;
		e1->touch(e1);
	}
	if (e2 && e2->touch && e2->solid != SOLID_NOT && e2->inuse) {
		ent_other = e1;
		e2->touch(e2);
	}
	ent_other = other;
}

/* the tick's move of v units a second (game_dt 60ths), 1/8 units, its
   sub-unit carried in acc */
static int32_t tick_move(int32_t v, int32_t *acc)
{
	int32_t t = v * SUB * game_dt + *acc;
	int32_t m = t / GAME_HZ;
	*acc = t - m * GAME_HZ;
	return m;
}

/* SV_FlyMove: the box slides along the planes it hits (4 bumps) */
static int fly_move(edict_t *ent)
{
	int32_t primal[3], original[3], planes[5][3], end[3];
	int numplanes = 0, blocked = 0;
	memcpy(original, ent->velocity, sizeof original);
	memcpy(primal, ent->velocity, sizeof primal);
	int32_t time_left = Q_FIX_ONE;        /* of the tick */
	int32_t acc[3] = { 0, 0, 0 };
	for (int bump = 0; bump < 4; bump++) {
		if (!ent->velocity[0] && !ent->velocity[1] && !ent->velocity[2])
			break;
		for (int i = 0; i < 3; i++) {
			int32_t m = tick_move(ent->velocity[i], &acc[i]);
			end[i] = ent->origin[i] + (int32_t) ((int64_t) m * time_left >> 12);
		}
		trace_t tr;
		ent_trace(ent, ent->origin, end, 0, &tr);
		if (tr.allsolid) {                /* trapped in another solid */
			memset(ent->velocity, 0, sizeof ent->velocity);
			return 3;
		}
		if (tr.fraction > 0) {            /* actually covered some distance */
			memcpy(ent->origin, tr.endpos, sizeof ent->origin);
			memcpy(original, ent->velocity, sizeof original);
			numplanes = 0;
		}
		if (tr.fraction >= Q_FIX_ONE)
			break;                        /* moved the entire distance */
		edict_t *hit = ent_from_trace(&tr);
		if (tr.plane_n[2] > 2867) {       /* 0.7: floor */
			blocked |= 1;
			if (!hit || hit->solid == SOLID_BSP)
				ent->flags |= FL_ONGROUND;
		}
		if (!tr.plane_n[2])
			blocked |= 2;                 /* step */
		impact(ent, hit ? hit : ent_world);   /* run the impact function */
		if (!ent->inuse)
			break;                        /* removed by the impact function */
		time_left -= (int32_t) ((int64_t) time_left * tr.fraction >> 12);
		if (numplanes >= 5) {             /* this shouldn't really happen */
			memset(ent->velocity, 0, sizeof ent->velocity);
			return 3;
		}
		for (int k = 0; k < 3; k++)
			planes[numplanes][k] = tr.plane_n[k];
		numplanes++;
		/* modify original_velocity so it parallels all of the clip planes */
		int i, j;
		int32_t nv[3];
		for (i = 0; i < numplanes; i++) {
			int16_t n[3] = { (int16_t) planes[i][0], (int16_t) planes[i][1], (int16_t) planes[i][2] };
			clip_velocity(original, n, nv, 2);
			for (j = 0; j < numplanes; j++)
				if (j != i && (int64_t) nv[0] * planes[j][0] + (int64_t) nv[1] * planes[j][1] +
				              (int64_t) nv[2] * planes[j][2] < 0)
					break;                /* not ok */
			if (j == numplanes)
				break;
		}
		if (i != numplanes) {             /* go along this plane */
			memcpy(ent->velocity, nv, sizeof nv);
		} else {                          /* go along the crease */
			if (numplanes != 2) {
				memset(ent->velocity, 0, sizeof ent->velocity);
				return 7;
			}
			int32_t dir[3] = {
				(planes[0][1] * planes[1][2] - planes[0][2] * planes[1][1]) >> 12,
				(planes[0][2] * planes[1][0] - planes[0][0] * planes[1][2]) >> 12,
				(planes[0][0] * planes[1][1] - planes[0][1] * planes[1][0]) >> 12 };
			int32_t d = (int32_t) (((int64_t) dir[0] * ent->velocity[0] + (int64_t) dir[1] * ent->velocity[1] +
			                        (int64_t) dir[2] * ent->velocity[2]) >> 12);
			for (int k = 0; k < 3; k++)
				ent->velocity[k] = dir[k] * d >> 12;
		}
		/* if velocity is against the original velocity, stop dead to avoid
		   tiny occilations in sloping corners */
		if ((int64_t) ent->velocity[0] * primal[0] + (int64_t) ent->velocity[1] * primal[1] +
		    (int64_t) ent->velocity[2] * primal[2] <= 0) {
			memset(ent->velocity, 0, sizeof ent->velocity);
			return blocked;
		}
	}
	return blocked;
}

/* SV_AddGravity: sv_gravity 800 over the tick, its remainder carried */
static void add_gravity(edict_t *ent)
{
	int32_t g = 800 * game_dt + ent->item_extra;      /* (item_extra: a free carry for monsters) */
	ent->velocity[2] -= g / GAME_HZ;
	ent->item_extra = g % GAME_HZ;
}

/* SV_CheckVelocity: sv_maxvelocity 2000 */
static void check_velocity(edict_t *ent)
{
	for (int i = 0; i < 3; i++) {
		if (ent->velocity[i] > 2000) ent->velocity[i] = 2000;
		else if (ent->velocity[i] < -2000) ent->velocity[i] = -2000;
	}
}

static void run_think(edict_t *e)
{
	if (e->nextthink <= 0 || e->nextthink > ent_time)
		return;
	e->nextthink = 0;
	ent_other = ent_world;
	if (e->think)
		e->think(e);
}

void ai_physics_step(edict_t *ent)
{
	/* freefall if not onground */
	if (!(ent->flags & (FL_ONGROUND | FL_FLY | FL_SWIM))) {
		int hitsound = ent->velocity[2] < -80;     /* sv_gravity x -0.1 */
		add_gravity(ent);
		check_velocity(ent);
		fly_move(ent);
		link_touch(ent);
		if ((ent->flags & FL_ONGROUND) && hitsound)   /* just hit ground */
			ent_sound(ent, CHAN_AUTO, snd_find("demon/dland2.wav"), 255, ATTN_NORM);
	}
	run_think(ent);                       /* regular thinking */
	if (ent->inuse)
		check_water_transition(ent);
}

/* SV_PushEntity: moved by `push` (1/8 units), stopped by what it hits
   (only the brush models for a trigger or a non-solid), touched */
static void push_entity(edict_t *ent, const int32_t push[3], trace_t *tr)
{
	int32_t end[3];
	for (int k = 0; k < 3; k++)
		end[k] = ent->origin[k] + push[k];
	ent_trace(ent, ent->origin, end, ent->solid == SOLID_TRIGGER || ent->solid == SOLID_NOT, tr);
	memcpy(ent->origin, tr->endpos, sizeof ent->origin);
	link_touch(ent);
	if (tr->fraction < Q_FIX_ONE && ent->inuse) {
		edict_t *hit = ent_from_trace(tr);
		impact(ent, hit ? hit : ent_world);
	}
}

/* SV_Physics_Toss: toss, bounce and fly; on the ground, nothing */
void ai_physics_toss(edict_t *ent)
{
	run_think(ent);                       /* regular thinking */
	if (!ent->inuse)
		return;
	if (ent->flags & FL_ONGROUND)         /* onground: no move */
		return;
	check_velocity(ent);
	if (ent->movetype != MOVETYPE_FLY && ent->movetype != MOVETYPE_FLYMISSILE)
		add_gravity(ent);
	/* move angles: avelocity degrees a second */
	for (int k = 0; k < 3; k++)
		ent->ang[k] = (uint16_t) (ent->ang[k] + ent->avel[k] * 65536 / 360 * game_dt / GAME_HZ);
	int32_t move[3];
	for (int k = 0; k < 3; k++)
		move[k] = (int32_t) ((int64_t) ent->velocity[k] * SUB * game_dt / GAME_HZ);
	trace_t tr;
	push_entity(ent, move, &tr);
	if (tr.fraction >= Q_FIX_ONE || !ent->inuse)
		return;
	clip_velocity(ent->velocity, tr.plane_n, ent->velocity, ent->movetype == MOVETYPE_BOUNCE ? 3 : 2);
	/* stop if on ground */
	if (tr.plane_n[2] > 2867 && (ent->velocity[2] < 60 || ent->movetype != MOVETYPE_BOUNCE)) {
		ent->flags |= FL_ONGROUND;
		memset(ent->velocity, 0, sizeof ent->velocity);
		memset(ent->avel, 0, sizeof ent->avel);
	}
	check_water_transition(ent);          /* check for in water */
}
