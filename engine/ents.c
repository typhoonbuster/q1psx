/*
 * q1psx — the edicts' runtime: the pool, the tick (touches, pushers,
 * thinks), subs.qc and combat.qc's T_Damage for edicts, the brush models'
 * clip table and render list. See engine/ents.h.
 */

#include <stdio.h>
#include <string.h>

#include "ents.h"
#include "game.h"
#include "hud.h"
#include "level.h"
#include "mem.h"
#include "pmove.h"
#include "q_fixed.h"
#include "render.h"
#include "rlist.h"
#include "snd.h"
#include "world.h"

clip_bmodel_t clip_bmodels[CLIP_MAX_BMODELS];
int           clip_num_bmodels;
static edict_t *clip_owner[CLIP_MAX_BMODELS];   /* the edict of each clip entry */

edict_t *edicts;
int      num_edicts;
static int max_edicts;

edict_t *ent_other, *ent_activator, *ent_damage_attacker;
int32_t  ent_time;
int      ent_found_secrets, ent_total_secrets;
int      ent_killed_monsters, ent_total_monsters;
int      ent_force_retouch;
int      ents_first_spawn;

#define ENT_SPARE 96         /* edicts beyond the map's: what the QC spawns
                                (door fields, plat triggers, delays, fogs) */
#define ENT_WORDS 32         /* the active bitmap: up to 1024 edicts */

static uint32_t active[ENT_WORDS];   /* edicts with a think or a move pending */
static int      lists_dirty;         /* an edict spawned or went: the lists */
static uint16_t touch_list[512];     /* edicts with a touch (the player's pass) */
static int      num_touch;
static uint16_t draw_list[CLIP_MAX_BMODELS + 64];   /* drawn brush models */
uint16_t        ents_model_list[512];               /* drawn alias models (items.c) */
int             ents_num_models;
static int      num_draw;
static uint16_t vis_list[CLIP_MAX_BMODELS + 64];    /* those the PVS stamp sees */
static int      num_vis;
static uint16_t vis_list_stamp;
static int      ents_relink;         /* a drawn box moved: the visible list is stale */

#ifdef Q1PSX_BENCH
int ents_log;
#define ELOG(...) do { if (ents_log) printf(__VA_ARGS__); } while (0)
#else
#define ELOG(...) ((void) 0)
#endif

/* ------------------------------------------------------------- the pool -- */

static int index_of(const edict_t *e)
{
	return (int) (e - edicts);
}

void ent_wake(edict_t *e)
{
	int i = index_of(e);
	active[i >> 5] |= 1u << (i & 31);
}

edict_t *ent_spawn(void)
{
	for (int i = 2; i < max_edicts; i++) {
		edict_t *e = &edicts[i];
		if (e->inuse)
			continue;
		memset(e, 0, sizeof *e);
		e->inuse = 1;
		e->class = QE_TEMP;
		e->item_model = -1;
		e->noise = e->noise1 = e->noise2 = e->noise3 = e->noise4 = SND_NONE;
		if (i >= num_edicts)
			num_edicts = i + 1;
		lists_dirty = 1;
		return e;
	}
	printf("ents: no free edict\n");
	return 0;
}

/* a clip entry out of the traces: a box no sweep reaches */
static void clip_disable(edict_t *e)
{
	if (!e->clip)
		return;
	clip_bmodel_t *cb = &clip_bmodels[e->clip - 1];
	cb->absmin[0] = cb->absmin[1] = cb->absmin[2] = 0x3fffffff;
	cb->absmax[0] = cb->absmax[1] = cb->absmax[2] = -0x3fffffff;
}

static void clip_update(edict_t *e)
{
	if (!e->clip)
		return;
	if (e->solid != SOLID_BSP && e->solid != SOLID_BBOX) {
		clip_disable(e);
		return;
	}
	clip_bmodel_t *cb = &clip_bmodels[e->clip - 1];
	memcpy(cb->origin, e->origin, sizeof cb->origin);
	if (cb->bm) {
		memcpy(cb->absmin, e->absmin, sizeof cb->absmin);
		memcpy(cb->absmax, e->absmax, sizeof cb->absmax);
	} else {                                  /* a box: its own, exact (clip.h) */
		for (int k = 0; k < 3; k++) {
			cb->absmin[k] = e->origin[k] + e->mins[k];
			cb->absmax[k] = e->origin[k] + e->maxs[k];
		}
	}
}

/* a SOLID_BBOX (or SLIDEBOX) entity without a brush model into the traces:
   SV_ClipToLinks takes its box (SV_HullForBox) */
static void clip_add_box(edict_t *e)
{
	if (clip_num_bmodels >= CLIP_MAX_BMODELS) {
		printf("ents: more than %d solid entities\n", CLIP_MAX_BMODELS);
		return;
	}
	clip_bmodels[clip_num_bmodels].bm = 0;
	clip_owner[clip_num_bmodels] = e;
	e->clip = (uint8_t) ++clip_num_bmodels;
}

void ent_remove(edict_t *e)
{
	if (e == ent_world || e == ent_player || !e->inuse)
		return;
	clip_disable(e);
	e->clip  = 0;
	e->inuse = 0;
	e->draw  = 0;
	e->touch = 0;
	e->think = 0;
	e->nextthink = 0;
	lists_dirty = 1;
	ents_relink = 1;
}

void ent_link(edict_t *e)
{
	for (int k = 0; k < 3; k++) {             /* a unit wider (SV_LinkEdict) */
		e->absmin[k] = e->origin[k] + e->mins[k] - CLIP_SUB;
		e->absmax[k] = e->origin[k] + e->maxs[k] + CLIP_SUB;
	}
	if (e->flags & FL_ITEM) {                 /* items: 15 more across */
		for (int k = 0; k < 2; k++) {
			e->absmin[k] -= 15 * CLIP_SUB;
			e->absmax[k] += 15 * CLIP_SUB;
		}
	}
	if (e->item_model >= 0) {                 /* an alias model: drawn in its centre's leaf */
		int32_t c[3];
		for (int k = 0; k < 3; k++)
			c[k] = (e->origin[k] + (e->mins[k] + e->maxs[k]) / 2) / CLIP_SUB;
		e->leaves[0]  = world_point_leaf(c);
		e->num_leaves = 1;
	} else if (e->draw && e->model) {
		uint16_t idx[ENT_MAX_LEAVES];
		e->num_leaves = (uint8_t) world_box_leaves(e->absmin, e->absmax, idx, ENT_MAX_LEAVES);
		for (int k = 0; k < e->num_leaves; k++)
			e->leaves[k] = &world.leaves[idx[k]];
		e->vis_stamp = 0xffff;
		ents_relink = 1;
	}
	if (!e->clip && !e->model && e != ent_player && (e->solid == SOLID_BBOX || e->solid == SOLID_SLIDEBOX))
		clip_add_box(e);
	clip_update(e);
}

void ent_setorigin(edict_t *e, const int32_t org[3])
{
	memcpy(e->origin, org, sizeof e->origin);
	ent_link(e);
}

void ent_setsize(edict_t *e, const int32_t mins[3], const int32_t maxs[3])
{
	memcpy(e->mins, mins, sizeof e->mins);
	memcpy(e->maxs, maxs, sizeof e->maxs);
	ent_link(e);
}

/* setmodel(): the brush model's box; a solid one (SOLID_BSP, a shot
   trigger's SOLID_BBOX: its brush is its box) into the traces */
void ent_setmodel(edict_t *e, int model)
{
	const qw_bmodel *bm = &world.bmodels[model];
	e->model = (int16_t) model;
	memcpy(e->mins, bm->mins, sizeof e->mins);
	memcpy(e->maxs, bm->maxs, sizeof e->maxs);
	if ((e->solid == SOLID_BSP || e->solid == SOLID_BBOX) && model && !e->clip) {
		if (clip_num_bmodels < CLIP_MAX_BMODELS) {
			clip_bmodels[clip_num_bmodels].bm = bm;
			clip_owner[clip_num_bmodels] = e;
			e->clip = (uint8_t) ++clip_num_bmodels;
		} else {
			printf("ents: more than %d solid brush models\n", CLIP_MAX_BMODELS);
		}
	}
	lists_dirty = 1;
	ent_link(e);
}

edict_t *ent_find(edict_t *from, uint16_t targetname)
{
	if (!targetname)
		return 0;
	for (int i = from ? index_of(from) + 1 : 0; i < num_edicts; i++)
		if (edicts[i].inuse && edicts[i].targetname == targetname)
			return &edicts[i];
	return 0;
}

edict_t *ent_find_class(edict_t *from, int cls)
{
	for (int i = from ? index_of(from) + 1 : 0; i < num_edicts; i++)
		if (edicts[i].inuse && edicts[i].class == cls)
			return &edicts[i];
	return 0;
}

void ent_centre(const edict_t *e, int32_t out[3])
{
	for (int k = 0; k < 3; k++)
		out[k] = e->origin[k] + (e->mins[k] + e->maxs[k]) / 2;
}

/* sound(): from the entity's box centre (SV_StartSound), its own channels */
void ent_sound(edict_t *e, int chan, int s, int vol, int atten)
{
	int32_t org[3];
	if (e == ent_player) {
		snd_start_idx(ENT_PLAYER, chan, s, pl.pm.pos, vol, atten);
		return;
	}
	ent_centre(e, org);
	snd_start_idx(ENT_EDICT + index_of(e), chan, s, org, vol, atten);
}

void ent_centerprint(edict_t *client, const char *s)
{
	if (client == ent_player && s && *s)
		hud_centerprint(s);
}

void sub_null(edict_t *self)
{
	(void) self;
}

void sub_remove(edict_t *self)
{
	ent_remove(self);
}

/* ------------------------------------------------------------- subs.qc -- */

/* SetMovedir: the map's angle, -1 up, -2 down; the angles zeroed */
void set_movedir(edict_t *self)
{
	if (self->angle == -1) {
		self->movedir[0] = 0; self->movedir[1] = 0; self->movedir[2] = 4096;
	} else if (self->angle == -2) {
		self->movedir[0] = 0; self->movedir[1] = 0; self->movedir[2] = -4096;
	} else {
		int a = self->angle * ANG_TURN / 360;
		self->movedir[0] = r_cos(a);
		self->movedir[1] = r_sin(a);
		self->movedir[2] = 0;
	}
	self->angle = 0;
}

/* InitTrigger: a one-way trigger's direction, its box, unseen */
void init_trigger(edict_t *self)
{
	if (self->angle)
		set_movedir(self);
	self->solid    = SOLID_TRIGGER;
	self->movetype = MOVETYPE_NONE;
	ent_setmodel(self, self->model);
	self->draw = 0;
}

static void delay_think(edict_t *self)
{
	ent_activator = self->enemy;
	sub_use_targets(self);
	ent_remove(self);
}

/* SUB_UseTargets: a delay through a DelayedUse edict; the message to the
   activator; the killtargets removed; the targets' use */
void sub_use_targets(edict_t *self)
{
	if (self->delay) {
		edict_t *t = ent_spawn();
		if (!t)
			return;
		t->nextthink  = ent_time + TICKS(self->delay);
		t->think      = delay_think;
		t->enemy      = ent_activator;
		t->message    = self->message;
		t->msgtext    = self->msgtext;
		t->killtarget = self->killtarget;
		t->target     = self->target;
		ent_wake(t);
		return;
	}
	if (ent_activator == ent_player && (self->message || self->msgtext)) {
		ent_centerprint(ent_activator, self->message ? world_string(self->message) : self->msgtext);
		if (self->noise == SND_NONE)
			ent_sound(ent_activator, CHAN_VOICE, snd_find("misc/talk.wav"), 255, ATTN_NORM);
	}
	if (self->killtarget)
		for (edict_t *t = ent_find(0, self->killtarget); t; t = ent_find(t, self->killtarget))
			ent_remove(t);
	if (self->target) {
		edict_t *act = ent_activator;
		for (edict_t *t = ent_find(0, self->target); t; t = ent_find(t, self->target)) {
			edict_t *otemp = ent_other;
			ent_other = self;
			if (t->use && t->use != sub_null) {
				ELOG("ent: %s uses %s\n", world_string(self->classname), world_string(t->classname));
				t->use(t);
			}
			ent_other = otemp;
			ent_activator = act;
		}
	}
}

static void sub_calc_move_done(edict_t *self)
{
	self->move_ticks = 0;
	ent_setorigin(self, self->finaldest);
	self->velocity[0] = self->velocity[1] = self->velocity[2] = 0;
	self->nextthink = -1;
	if (self->think1)
		self->think1(self);
}

/* SUB_CalcMove, exact in ticks: the travel's ticks from the distance and
   the speed (at least 0.1 s, else a jump at its end), the position each
   tick from the start, the delta and the ticks elapsed */
void sub_calc_move(edict_t *self, const int32_t tdest[3], int speed, ent_fn func)
{
	self->think1 = func;
	memcpy(self->finaldest, tdest, sizeof self->finaldest);
	self->think = sub_calc_move_done;
	self->move_ticks = 0;
	ent_wake(self);
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = tdest[k] - self->origin[k];
	if (!d[0] && !d[1] && !d[2]) {
		self->velocity[0] = self->velocity[1] = self->velocity[2] = 0;
		self->nextthink = self->ltime + TICKS(10);
		return;
	}
	if (speed <= 0)
		speed = 1;
	int32_t len = (int32_t) q_isqrt64((uint64_t) ((int64_t) d[0] * d[0] + (int64_t) d[1] * d[1] +
	                                             (int64_t) d[2] * d[2]));   /* 1/8 units */
	int32_t ticks = (int32_t) (((int64_t) len * 60 + 4 * speed) / (8 * speed));
	if (ticks < TICKS(10)) {
		self->velocity[0] = self->velocity[1] = self->velocity[2] = 0;
		self->nextthink = self->ltime + TICKS(10);
		return;
	}
	self->nextthink = self->ltime + ticks;
	memcpy(self->move_from, self->origin, sizeof self->move_from);
	memcpy(self->move_delta, d, sizeof self->move_delta);
	self->move_t0    = self->ltime;
	self->move_ticks = ticks;
	for (int k = 0; k < 3; k++) {
		self->velocity[k] = d[k] * 60 / (8 * ticks);
		if (!self->velocity[k] && d[k])
			self->velocity[k] = d[k] > 0 ? 1 : -1;
	}
}

/* ------------------------------------------------------------ combat.qc -- */

/* T_Damage on an edict (no armor: only the player has some, the game's);
   Killed: a pusher's or a trigger's th_die */
void ent_damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, int damage)
{
	if (!targ || !targ->takedamage)
		return;
	ent_damage_attacker = attacker;
	if (targ == ent_player) {
		int32_t c[3];
		if (inflictor && inflictor != ent_world) {
			for (int k = 0; k < 3; k++)
				c[k] = (inflictor->absmin[k] + inflictor->absmax[k]) / 2;
			game_damage_player(damage, c);
		} else {
			game_damage_player(damage, 0);
		}
		return;
	}
	if (attacker == ent_player && game_quad_active())
		damage *= 4;
	targ->health -= damage;
	ELOG("ent: %s takes %d (health %d)\n", world_string(targ->classname), damage, targ->health);
	if (targ->health <= 0) {
		if (targ->health < -99)
			targ->health = -99;
		if ((targ->movetype == MOVETYPE_PUSH || targ->movetype == MOVETYPE_NONE) && targ->th_die)
			targ->th_die(targ);
		return;
	}
	if (targ->th_pain)
		targ->th_pain(targ);
}

edict_t *ent_from_trace(const trace_t *tr)
{
	if (!tr->ent || tr->ent > clip_num_bmodels)
		return 0;
	return clip_owner[tr->ent - 1];
}

void ents_impact(int ent)
{
	if (!edicts || ent <= 0 || ent > clip_num_bmodels)
		return;
	edict_t *e = clip_owner[ent - 1];
	if (!e || !e->inuse || !e->touch || e->solid == SOLID_NOT)
		return;
	ent_time  = game_tick_count;
	ent_other = ent_player;
	e->touch(e);
}

/* CanDamage: a pusher by its box centre (the line may end on the pusher),
   anything else by its origin and four points around it */
static int can_damage(const edict_t *targ, const int32_t org[3])
{
	trace_t tr;
	if (targ->movetype == MOVETYPE_PUSH) {
		int32_t c[3];
		for (int k = 0; k < 3; k++)
			c[k] = (targ->absmin[k] + targ->absmax[k]) / 2;
		clip_trace(0, org, c, &tr);
		return tr.fraction >= Q_FIX_ONE || ent_from_trace(&tr) == targ;
	}
	static const int8_t off[5][2] = { { 0, 0 }, { 15, 15 }, { -15, -15 }, { -15, 15 }, { 15, -15 } };
	for (int i = 0; i < 5; i++) {
		int32_t p[3] = { targ->origin[0] + off[i][0] * CLIP_SUB, targ->origin[1] + off[i][1] * CLIP_SUB,
		                 targ->origin[2] };
		clip_trace(0, org, p, &tr);
		if (tr.fraction >= Q_FIX_ONE)
			return 1;
	}
	return 0;
}

void ents_radius_damage(const int32_t org[3], int damage, edict_t *attacker, edict_t *ignore)
{
	int32_t rad = (damage + 40) * CLIP_SUB;
	for (int i = 2; i < num_edicts; i++) {
		edict_t *e = &edicts[i];
		if (!e->inuse || e == ignore || !e->takedamage || e->solid == SOLID_NOT)
			continue;
		int32_t d[3];
		for (int k = 0; k < 3; k++)
			d[k] = org[k] - (e->origin[k] + (e->mins[k] + e->maxs[k]) / 2);
		int32_t len = (int32_t) q_isqrt64((uint64_t) ((int64_t) d[0] * d[0] + (int64_t) d[1] * d[1] +
		                                             (int64_t) d[2] * d[2]));
		if (len > rad)
			continue;
		int points = damage - len / (2 * CLIP_SUB);
		if (e == attacker)
			points /= 2;
		if (points > 0 && can_damage(e, org))
			ent_damage(e, attacker, attacker, points);
	}
}

/* -------------------------------------------------------------- physics -- */

static const int32_t player_mins[3] = { -16 * CLIP_SUB, -16 * CLIP_SUB, -24 * CLIP_SUB };
static const int32_t player_maxs[3] = {  16 * CLIP_SUB,  16 * CLIP_SUB,  32 * CLIP_SUB };

/* the player's mirror: its box, health, items, from the game's player */
static void sync_player(void)
{
	edict_t *p = ent_player;
	memcpy(p->origin, pl.pm.pos, sizeof p->origin);
	for (int k = 0; k < 3; k++) {
		p->absmin[k] = p->origin[k] + player_mins[k] - CLIP_SUB;
		p->absmax[k] = p->origin[k] + player_maxs[k] + CLIP_SUB;
	}
	p->health     = (int16_t) pl.health;
	p->items      = pl.items;
	p->takedamage = pl.dead || game_intermission() ? DAMAGE_NO : DAMAGE_AIM;
	p->solid      = pl.dead || game_intermission() ? SOLID_NOT : SOLID_SLIDEBOX;
	p->movetype   = pl.pm.noclip ? MOVETYPE_NOCLIP : MOVETYPE_WALK;
}

/* SV_TestEntityPosition for the player: inside anything solid? */
static int player_stuck(void)
{
	trace_t tr;
	clip_trace(1, pl.pm.pos, pl.pm.pos, &tr);
	return tr.startsolid || tr.allsolid;
}

/* the edict the player stands on: pmove's onground is 1 + the ground
   trace's ent (0 the world, a clip entry + 1) */
static edict_t *player_ground(void)
{
	int g = pl.pm.onground;
	if (!g)
		return 0;
	if (g == 1 || g - 2 >= clip_num_bmodels)
		return ent_world;
	return clip_owner[g - 2];
}

/* the mover's position after `t` ticks of its travel */
static void move_at(const edict_t *e, int32_t t, int32_t out[3])
{
	if (t >= e->move_ticks) {
		for (int k = 0; k < 3; k++)
			out[k] = e->move_from[k] + e->move_delta[k];
		return;
	}
	for (int k = 0; k < 3; k++)
		out[k] = e->move_from[k] + (int32_t) ((int64_t) e->move_delta[k] * t / e->move_ticks);
}

/* SV_PushMove: the pusher to its next position; the player in its way, or
   riding it, moved along (SV_PushEntity, the pusher out of the traces);
   still inside something: everything back, the pusher blocked */
static void push_move(edict_t *pusher, int movetime)
{
	if (!pusher->move_ticks) {
		pusher->ltime += movetime;
		return;
	}
	int32_t to[3], move[3], mins[3], maxs[3], pushorig[3];
	move_at(pusher, pusher->ltime + movetime - pusher->move_t0, to);
	for (int k = 0; k < 3; k++) {
		move[k] = to[k] - pusher->origin[k];
		mins[k] = pusher->absmin[k] + move[k];
		maxs[k] = pusher->absmax[k] + move[k];
	}
	memcpy(pushorig, pusher->origin, sizeof pushorig);
	pusher->ltime += movetime;
	ent_setorigin(pusher, to);

	edict_t *p = ent_player;
	if (p->movetype == MOVETYPE_NOCLIP || pl.dead || game_intermission())
		return;
	if (player_ground() != pusher) {
		if (p->absmin[0] >= maxs[0] || p->absmin[1] >= maxs[1] || p->absmin[2] >= maxs[2] ||
		    p->absmax[0] <= mins[0] || p->absmax[1] <= mins[1] || p->absmax[2] <= mins[2])
			return;
		if (!player_stuck())
			return;
	}
	int32_t entorig[3], end[3];
	memcpy(entorig, pl.pm.pos, sizeof entorig);
	for (int k = 0; k < 3; k++)
		end[k] = pl.pm.pos[k] + move[k];
	uint8_t solid = pusher->solid;
	pusher->solid = SOLID_NOT;                 /* (out of the traces) */
	clip_update(pusher);
	trace_t tr;
	clip_trace(1, pl.pm.pos, end, &tr);
	memcpy(pl.pm.pos, tr.endpos, sizeof pl.pm.pos);
	pusher->solid = solid;
	clip_update(pusher);
	sync_player();
	if (player_stuck()) {                      /* fail the move */
		memcpy(pl.pm.pos, entorig, sizeof pl.pm.pos);
		sync_player();
		pusher->ltime -= movetime;
		ent_setorigin(pusher, pushorig);
		if (pusher->blocked) {
			ent_other = p;
			ELOG("ent: %s blocked\n", world_string(pusher->classname));
			pusher->blocked(pusher);
		}
	}
}

/* SV_Physics_Pusher, in ticks: the move up to the think's time, the think */
static void physics_pusher(edict_t *e)
{
	int32_t oldltime  = e->ltime;
	int32_t thinktime = e->nextthink;
	int     movetime;
	if (thinktime < e->ltime + 1) {
		movetime = thinktime - e->ltime;
		if (movetime < 0)
			movetime = 0;
	} else {
		movetime = 1;
	}
	if (movetime)
		push_move(e, movetime);
	if (thinktime > oldltime && thinktime <= e->ltime) {
		e->nextthink = 0;
		ent_other = ent_world;
		if (e->think)
			e->think(e);
	}
}

/* SV_RunThink */
static void run_think(edict_t *e)
{
	if (e->nextthink <= 0 || e->nextthink > ent_time)
		return;
	e->nextthink = 0;
	ent_other = ent_world;
	if (e->think)
		e->think(e);
}

static void rebuild_lists(void)
{
	num_touch = num_draw = ents_num_models = 0;
	for (int i = 2; i < num_edicts; i++) {
		edict_t *e = &edicts[i];
		if (!e->inuse)
			continue;
		if (e->item_model >= 0 && ents_num_models < (int) (sizeof ents_model_list / sizeof ents_model_list[0]))
			ents_model_list[ents_num_models++] = (uint16_t) i;
		if (e->touch && num_touch < (int) (sizeof touch_list / sizeof touch_list[0]))
			touch_list[num_touch++] = (uint16_t) i;
		if (e->draw && e->model && e->num_leaves &&
		    num_draw < (int) (sizeof draw_list / sizeof draw_list[0]))
			draw_list[num_draw++] = (uint16_t) i;
	}
	lists_dirty = 0;
	ents_relink = 1;
}

/* SV_TouchLinks for the player: the SOLID_TRIGGER edicts its box overlaps */
static void touch_player(void)
{
	edict_t *p = ent_player;
	if (p->solid == SOLID_NOT)
		return;
	for (int i = 0; i < num_touch; i++) {
		edict_t *t = &edicts[touch_list[i]];
		if (!t->inuse || !t->touch || t->solid != SOLID_TRIGGER)
			continue;
		if (p->absmin[0] > t->absmax[0] || p->absmin[1] > t->absmax[1] || p->absmin[2] > t->absmax[2] ||
		    p->absmax[0] < t->absmin[0] || p->absmax[1] < t->absmin[1] || p->absmax[2] < t->absmin[2])
			continue;
		ent_other = p;
		t->touch(t);
		if (lists_dirty)                       /* (the touch spawned or removed) */
			break;
	}
}

void ents_tick(void)
{
	if (!edicts)
		return;
	ent_time = game_tick_count;
	if (lists_dirty)
		rebuild_lists();
	sync_player();
	touch_player();
	if (ent_force_retouch > 0)
		ent_force_retouch--;
	for (int w = 0; w < ENT_WORDS; w++) {
		uint32_t bits = active[w];
		while (bits) {
			int b = __builtin_ctz(bits);
			bits &= bits - 1;
			edict_t *e = &edicts[w * 32 + b];
			if (e->inuse) {
				if (e->movetype == MOVETYPE_PUSH) {
					physics_pusher(e);
				} else {
					run_think(e);                 /* SV_Physics_Toss: the think, then the move */
					if (e->inuse && e->movetype == MOVETYPE_TOSS)
						items_physics(e);
				}
			}
			if (!e->inuse || (e->nextthink <= 0 && !e->move_ticks))
				active[w] &= ~(1u << b);
		}
	}
	sync_player();
}

/* ---------------------------------------------------------------- spawn -- */

void ents_load(void)
{
	num_edicts = 0;
	max_edicts = (int) world.num_ents + 2 + ENT_SPARE;
	if (max_edicts > ENT_WORDS * 32)
		max_edicts = ENT_WORDS * 32;
	edicts = mem_level_alloc((uint32_t) max_edicts * sizeof *edicts);
	if (!edicts) {
		printf("ents: no room for %d edicts\n", max_edicts);
		max_edicts = 0;
	}
}

/* the edict for a cooked entity: the fields every class takes (ED_ParseEdict) */
static edict_t *ent_new(const qw_ent *q)
{
	if (num_edicts >= max_edicts)
		return 0;
	edict_t *e = &edicts[num_edicts++];
	memset(e, 0, sizeof *e);
	e->inuse      = 1;
	e->class      = q->class;
	e->classname  = q->classname;
	e->spawnflags = q->spawnflags;
	memcpy(e->origin, q->origin, sizeof e->origin);
	e->angle      = q->angle;
	memcpy(e->mangle, q->mangle, sizeof e->mangle);
	e->model  = q->model;
	e->speed  = q->speed;  e->wait   = q->wait;   e->delay  = q->delay;
	e->lip    = q->lip;    e->height = q->height; e->dmg    = q->dmg;
	e->health = q->health; e->count  = q->count;  e->sounds = q->sounds;
	e->style  = q->style;
	e->target = q->target; e->targetname = q->targetname; e->killtarget = q->killtarget;
	e->message = q->message; e->map = q->map;
	e->item_model = -1;
	e->noise = e->noise1 = e->noise2 = e->noise3 = e->noise4 = SND_NONE;
	return e;
}

void ents_spawn(int first)
{
	ents_first_spawn = first;
	clip_num_bmodels = 0;
	num_edicts = 0;
	num_draw = num_touch = 0;
	memset(active, 0, sizeof active);
	ent_found_secrets = ent_total_secrets = 0;
	ent_killed_monsters = ent_total_monsters = 0;
	ent_force_retouch = 0;
	ent_time = game_tick_count;
	ent_activator = ent_other = ent_damage_attacker = 0;
	if (!edicts || max_edicts < 2)
		return;
	/* the pool cleared (ent_spawn takes the first slot not in use, past the
	   map's entities too: the arena's old bytes there were live edicts),
	   the world and the player */
	memset(edicts, 0, (size_t) max_edicts * sizeof *edicts);
	edicts[0].inuse = 1; edicts[0].class = QE_WORLD; edicts[0].solid = SOLID_BSP;
	edicts[1].inuse = 1; edicts[1].class = QE_PLAYER; edicts[1].solid = SOLID_SLIDEBOX;
	memcpy(edicts[1].mins, player_mins, sizeof player_mins);
	memcpy(edicts[1].maxs, player_maxs, sizeof player_maxs);
	edicts[0].noise = edicts[0].noise1 = edicts[0].noise2 = edicts[0].noise3 = edicts[0].noise4 = SND_NONE;
	edicts[1].noise = edicts[1].noise1 = edicts[1].noise2 = edicts[1].noise3 = edicts[1].noise4 = SND_NONE;
	edicts[0].item_model = edicts[1].item_model = -1;     /* (no model of the map's) */
	num_edicts = 2;

	int unknown = 0, skipped = 0, inhibit = 0;
	for (uint32_t i = 0; i < world.num_ents; i++) {
		const qw_ent *q = &world.ents[i];
		int cls = q->class;
		/* ED_LoadFromFile: not on this skill (not easy 256, not medium
		   512, not hard 1024) */
		if ((game_skill == 0 && (q->spawnflags & 256)) || (game_skill == 1 && (q->spawnflags & 512)) ||
		    (game_skill >= 2 && (q->spawnflags & 1024))) {
			inhibit++;
			continue;
		}
		if (cls >= QE_MONSTER_ARMY && cls <= QE_MONSTER_OLDONE) {   /* (Phase 8.1) */
			skipped++;
			continue;
		}
		if (cls == QE_UNKNOWN) {
			unknown++;
			continue;
		}
		edict_t *e = ent_new(q);
		if (!e)
			break;

		switch (cls) {
		case QE_FUNC_DOOR: case QE_FUNC_DOOR_SECRET: case QE_FUNC_BUTTON:
		case QE_FUNC_PLAT: case QE_FUNC_TRAIN: case QE_MISC_TELEPORTTRAIN:
			movers_spawn(e, q);
			break;
		case QE_TRIGGER_CHANGELEVEL: case QE_INFO_INTERMISSION:
			level_spawn(e, q);
			break;
		case QE_LIGHT_TORCH_SMALL_WALLTORCH: case QE_LIGHT_FLAME_LARGE_YELLOW:
		case QE_LIGHT_FLAME_SMALL_YELLOW: case QE_LIGHT_FLAME_SMALL_WHITE:
		case QE_MISC_FIREBALL: case QE_MISC_EXPLOBOX: case QE_MISC_EXPLOBOX2:
		case QE_TRAP_SPIKESHOOTER: case QE_TRAP_SHOOTER:
			items_spawn(e, q);
			break;
		default:
			if (cls >= QE_ITEM_HEALTH && cls <= QE_WEAPON_LIGHTNING)
				items_spawn(e, q);
			else
				triggers_spawn(e, q);
			break;
		}
	}
	rebuild_lists();
	int drawn = 0;
	for (int i = 0; i < num_edicts; i++)
		drawn += edicts[i].inuse && edicts[i].draw;
	printf("ents: %d edicts (%d monsters left out, %d unknown, %d not on skill %d), %d solid brush "
	       "models, %d drawn, %d models, %d touch, %d secrets\n", num_edicts, skipped, unknown, inhibit,
	       game_skill, clip_num_bmodels, drawn, ents_num_models, num_touch, ent_total_secrets);
}

#ifdef Q1PSX_BENCH
/* the bench's check: every live edict's fields in range (a corruption's
   first sight, by where it is called) */
int ents_check(const char *where)
{
	for (int i = 2; i < num_edicts; i++) {
		const edict_t *e = &edicts[i];
		if (!e->inuse)
			continue;
		if (e->num_leaves > ENT_MAX_LEAVES || e->item_model < -1 || e->item_model >= (int) world.num_models ||
		    e->model < 0 || e->model >= (int) world.num_bmodels || e->class > QE_TEMP) {
			printf("ents: CORRUPT at %s: edict %d of %d at %08x (size %u, pool %08x..%08x) class %d model %d "
			       "item %d leaves %d\n", where, i, num_edicts, (unsigned) (uintptr_t) e, (unsigned) sizeof *e,
			       (unsigned) (uintptr_t) edicts, (unsigned) (uintptr_t) (edicts + max_edicts), e->class,
			       e->model, (int) e->item_model, e->num_leaves);
			const uint32_t *w = (const uint32_t *) e;
			for (unsigned k = 0; k + 8 <= sizeof *e / 4; k += 8)
				printf("  +%03x %08x %08x %08x %08x %08x %08x %08x %08x\n", k * 4, (unsigned) w[k], (unsigned) w[k + 1],
				       (unsigned) w[k + 2], (unsigned) w[k + 3], (unsigned) w[k + 4], (unsigned) w[k + 5],
				       (unsigned) w[k + 6], (unsigned) w[k + 7]);
			return 0;
		}
	}
	return 1;
}
#endif

int ents_model_frame(int model)
{
	for (int i = 0; i < num_draw; i++) {
		const edict_t *e = &edicts[draw_list[i]];
		if (e->model == model && e->inuse && e->draw)
			return e->frame;
	}
	return 0;
}

/* ---------------------------------------------------------------- render -- */

/* A leaf's box in front of the eye (its depth along the view's forward
   row, x1.5: R_MATRIX_ONE; the extents' sum covers the box); the depth
   out. The walk's frustum test decides the rest. */
static int leaf_in_front(const qw_leaf *l, int32_t *depth)
{
	int32_t z = ((r_state.rot[6] * l->c[0] + r_state.rot[7] * l->c[1] +
	              r_state.rot[8] * l->c[2]) >> 12) + r_state.tr[2];
	int32_t rad = (l->h[0] + l->h[1] + l->h[2]) * 3 / 2;
	if (z + rad <= 0)
		return 0;
	*depth = z;
	return 1;
}

#ifdef Q1PSX_BENCH
int ents_stat[4];   /* the last frame: models drawn, PVS-visible, leaves in view, candidates */
int ents_debug;     /* the bench: print each model drawn this frame */
#define STAT(i)   (ents_stat[i]++)
#else
#define STAT(i)   ((void) 0)
#endif

/* The drawable models the PVS sees: found again when the stamp changes
   (the camera's leaf) or a box moved, each with the bits of its leaves
   the PVS sees. */
static void ents_visible(uint16_t vis)
{
	num_vis = 0;
	for (int i = 0; i < num_draw; i++) {
		edict_t *e = &edicts[draw_list[i]];
		if (!e->inuse || !e->draw)
			continue;
#ifdef Q1PSX_BENCH
		for (int j = 0; j < e->num_leaves; j++)
			if (e->leaves[j] < world.leaves || e->leaves[j] >= world.leaves + world.num_leaves) {
				printf("ents: BAD LEAF edict %d class %d model %d item %d draw %d leaves %d [%d] %08x "
				       "(num_edicts %d, list %d of %d)\n", (int) draw_list[i], e->class, e->model,
				       (int) e->item_model, e->draw, e->num_leaves, j, (unsigned) (uintptr_t) e->leaves[j],
				       num_edicts, i, num_draw);
				e->num_leaves = 0;
				break;
			}
#endif
		e->vis_stamp = vis;
		e->vismask   = 0;
		for (int j = 0; j < e->num_leaves; j++)
			if (e->leaves[j]->visframe == vis)
				e->vismask |= (uint8_t) (1 << j);
		if (e->vismask)
			vis_list[num_vis++] = draw_list[i];
	}
	vis_list_stamp = vis;
	ents_relink    = 0;
}

/* The leaf a brush model is drawn in, when its box spans several: the one
   holding the point of its box nearest the eye, moved 2 units further
   toward the eye. The walk draws a leaf's models after what lies behind
   that leaf and before what stands nearer: the model then comes after the
   world beyond its nearest face (a slab over a pit: after the pit, not
   among its walls) and under what is between it and the eye. NULL when
   that leaf is solid or out of the PVS (the eye's own side hidden: the
   nearest leaf in front is taken instead). */
static const qw_leaf *camera_side_leaf(const edict_t *e, uint16_t vis)
{
	int32_t cam[3], p[3], d[3], m = 0;
	for (int k = 0; k < 3; k++) {
		cam[k] = r_state.cam[k] * CLIP_SUB / QW_VSCALE;        /* vertex units -> 1/8 */
		int32_t lo = e->absmin[k] + CLIP_SUB, hi = e->absmax[k] - CLIP_SUB;   /* (the link's spread off) */
		p[k] = cam[k] < lo ? lo : cam[k] > hi ? hi : cam[k];
		d[k] = cam[k] - p[k];
		int32_t a = d[k] < 0 ? -d[k] : d[k];
		if (a > m) m = a;
	}
	if (!m)
		return 0;                                 /* the eye inside the box */
	int32_t q[3];
	for (int k = 0; k < 3; k++)                   /* 2 units toward the eye (max norm) */
		q[k] = (p[k] + d[k] * (2 * CLIP_SUB) / m) / CLIP_SUB;
	const qw_leaf *l = world_point_leaf(q);
	if (!l || l->contents == QW_CONTENTS_SOLID || l->visframe != vis)
		return 0;
	return l;
}

void ents_render(void)
{
	if (!edicts)
		return;
	if (lists_dirty)
		rebuild_lists();
	uint16_t vis = (uint16_t) r_state.visframe;
#ifdef Q1PSX_BENCH
	ents_stat[0] = ents_stat[1] = ents_stat[2] = 0;
	ents_stat[3] = num_draw;
#endif
	if (vis_list_stamp != vis || ents_relink)
		ents_visible(vis);
	for (int i = 0; i < num_vis; i++) {
		edict_t *e = &edicts[vis_list[i]];
		STAT(1);
		/* among the leaves the PVS sees, the one in front of the eye
		   nearest it: drawn there, over what the walk draws after it (a
		   single one: as it is) */
		const qw_leaf *best = 0;
		int32_t bestz = 0;
		unsigned mask = e->vismask;
		if (e->num_leaves > 1 && (best = camera_side_leaf(e, vis)) != 0) {
			STAT(2);
		} else if (!(mask & (mask - 1))) {
			int j = 0;
			while (!(mask & 1)) { mask >>= 1; j++; }
			best = e->leaves[j];
			STAT(2);
		} else {
			for (int j = 0; mask; j++, mask >>= 1) {
				int32_t z;
				if (!(mask & 1) || !leaf_in_front(e->leaves[j], &z))
					continue;
				STAT(2);
				if (!best || z < bestz) {
					best  = e->leaves[j];
					bestz = z;
				}
			}
		}
		if (!best)
			continue;
		STAT(0);
#ifdef Q1PSX_BENCH
		if (ents_debug)
			printf("  bm *%d leaf %d (vis %u / %u, c %d %d %d, list %08x) mask %02x of %d leaves\n",
			       e->model, (int) (best - world.leaves), best->visframe, vis, best->c[0], best->c[1],
			       best->c[2], (unsigned) best->vis, e->vismask, e->num_leaves);
#endif
		rl_add_model(best, rl_bmodel(&world.bmodels[e->model], e->origin));
	}
	items_render();
}
