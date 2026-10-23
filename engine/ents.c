/*
 * q1psx — the edicts' runtime: the pool, the tick (touches, pushers,
 * thinks), subs.qc and combat.qc's T_Damage for edicts, the brush models'
 * clip table and render list. See engine/ents.h.
 */

#include <stdio.h>
#include <string.h>

#include "bmfrag.h"
#include "ents.h"
#include "game.h"
#include "hud.h"
#include "level.h"
#include "mem.h"
#include "monsters.h"
#include "pmove.h"
#include "prof.h"
#include "q_fixed.h"
#include "render.h"
#include "rlist.h"
#include "snd.h"
#include "stream.h"
#include "world.h"

clip_bmodel_t clip_bmodels[CLIP_MAX_BMODELS];
int           clip_num_bmodels;
clip_area_t   clip_areas[CLIP_AREAS];
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
static uint32_t pushers[ENT_WORDS];  /* the movers (MOVETYPE_PUSH): run every tick */
static int      lists_dirty;         /* an edict spawned or went: the lists */
static uint16_t touch_list[512];     /* edicts with a touch (the player's pass) */
static int      num_touch;
typedef struct {
	int16_t  box[6];                 /* its box in whole units, a unit wider (the
	                                    touch passes' first test: no edict read
	                                    for one far away) */
	uint8_t  area;                   /* its area node, CLIP_NO_AREA none */
	uint8_t  mon;                    /* in the node's monster list too (touch_relink) */
	uint16_t pad;
} touch_slot_t;
static touch_slot_t *touch_slots;    /* (touch_list's) */
static uint16_t     *touch_next[2];  /* a slot's next in its node's list (+ 1, 0 the end) */
static int           max_touch;
/* the area nodes' trigger lists: [0] every slot, [1] the slots whose touch
   can do something to another edict than the player (no FL_ITEM, no
   FL_TOUCH_PLAYER: the monsters' list); their heads, tails, and the slots
   linked in each node's subtree */
static uint16_t trig_head[2][CLIP_AREAS], trig_tail[2][CLIP_AREAS], trig_count[2][CLIP_AREAS];
static void touch_relink(const edict_t *e);
static uint16_t draw_list[CLIP_MAX_BMODELS + 64];   /* drawn brush models */
uint16_t        ents_model_list[512];               /* drawn alias models (items.c) */
int             ents_num_models;
uint16_t        ents_streamed_list[512];            /* the monsters, the streamed models */
int             ents_num_streamed;
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
		e->touch_slot = -1;
		e->inuse = 1;
		e->class = QE_TEMP;
		e->item_model = -1;
		e->masset = -1;
		e->noise = e->noise1 = e->noise2 = e->noise3 = e->noise4 = SND_NONE;
		if (i >= num_edicts)
			num_edicts = i + 1;
		lists_dirty = 1;
		return e;
	}
	printf("ents: no free edict\n");
	return 0;
}

/* ------------------------------------------------------- the area nodes -- */

static int     num_areas;
static uint8_t area_parent[CLIP_AREAS];
static uint8_t area_solids[CLIP_AREAS];   /* the solid entries linked in each node's subtree */

/* SV_CreateAreaNode: the box halved across its longer side (x or y), the
   upper half first (children[0], numbered next), to AREA_DEPTH */
static int create_area(int depth, const int32_t mins[3], const int32_t maxs[3])
{
	int n = num_areas++;
	clip_area_t *a = &clip_areas[n];
	a->head = a->tail = 0;
	trig_head[0][n] = trig_tail[0][n] = trig_count[0][n] = 0;
	trig_head[1][n] = trig_tail[1][n] = trig_count[1][n] = 0;
	area_solids[n] = 0;
	if (depth == CLIP_AREA_DEPTH) {
		a->flags  = CLIP_AREA_LEAF;
		a->dist   = 0;
		a->child1 = 0;
		return n;
	}
	int axis = maxs[0] - mins[0] > maxs[1] - mins[1] ? 0 : 1;
	a->flags = (uint8_t) axis;
	a->dist  = (int16_t) ((maxs[axis] + mins[axis]) >> 1);
	int32_t mins2[3], maxs1[3];
	__builtin_memcpy(mins2, mins, sizeof mins2);
	__builtin_memcpy(maxs1, maxs, sizeof maxs1);
	maxs1[axis] = mins2[axis] = a->dist;
	area_parent[create_area(depth + 1, mins2, maxs)] = (uint8_t) n;
	a->child1 = (uint8_t) create_area(depth + 1, mins, maxs1);
	area_parent[a->child1] = (uint8_t) n;
	return n;
}

/* a solid entry linked in node n (d 1) or unlinked (d -1): the counts of
   its subtree and its ancestors', a parent's flag when a child's subtree
   gets or loses its first entry */
static void area_count(int n, int d)
{
	for (;;) {
		int was = area_solids[n];
		area_solids[n] = (uint8_t) (was + d);
		if (!n)
			return;
		int p = area_parent[n];
		if (!was != !area_solids[n]) {
			uint8_t bit = n == p + 1 ? CLIP_AREA_C0 : CLIP_AREA_C1;
			if (area_solids[n])
				clip_areas[p].flags |= bit;
			else
				clip_areas[p].flags &= (uint8_t) ~bit;
		}
		n = p;
	}
}

/* SV_LinkEdict's node: the first one whose split the box straddles (or a
   leaf) */
static int area_of(const int32_t absmin[3], const int32_t absmax[3])
{
	int n = 0;
	for (;;) {
		const clip_area_t *a = &clip_areas[n];
		if (a->flags & CLIP_AREA_LEAF)
			return n;
		int axis = a->flags & CLIP_AREA_Y;
		if (absmin[axis] > a->dist)
			n++;                              /* children[0] */
		else if (absmax[axis] < a->dist)
			n = a->child1;
		else
			return n;
	}
}

/* a linked clip entry out of its node's list */
static void clip_list_remove(clip_bmodel_t *cb)
{
	clip_area_t *a = &clip_areas[cb->area];
	if (cb->prev)
		CLIP_ENTRY(cb->prev)->next = cb->next;
	else
		a->head = cb->next;
	if (cb->next)
		CLIP_ENTRY(cb->next)->prev = cb->prev;
	else
		a->tail = cb->prev;
}

/* SV_UnlinkEdict for a clip entry */
static void clip_unlink(clip_bmodel_t *cb)
{
	if (cb->area == CLIP_NO_AREA)
		return;
	clip_list_remove(cb);
	area_count(cb->area, -1);
	cb->next = cb->prev = 0;
	cb->area = CLIP_NO_AREA;
}

/* SV_LinkEdict for a clip entry: unlinked, then appended to its node's
   list (InsertLinkBefore the list's head: at its end) */
static void clip_link(clip_bmodel_t *cb, const int32_t absmin[3], const int32_t absmax[3])
{
	int n = area_of(absmin, absmax);
	if (cb->area == n) {                      /* the same node: */
		if (!cb->next)
			return;                           /* already its last */
		clip_list_remove(cb);
	} else {
		clip_unlink(cb);
		area_count(n, 1);
	}
	clip_area_t *a = &clip_areas[n];
	uint16_t self = CLIP_LINK(cb - clip_bmodels);
	if (a->tail)
		CLIP_ENTRY(a->tail)->next = self;
	else
		a->head = self;
	cb->prev = a->tail;
	cb->next = 0;
	a->tail  = self;
	cb->area = (uint8_t) n;
}

/* a new clip entry for e: not linked yet */
static int clip_new(edict_t *e, const qw_bmodel *bm)
{
	if (clip_num_bmodels >= CLIP_MAX_BMODELS)
		return 0;
	clip_bmodel_t *cb = &clip_bmodels[clip_num_bmodels];
	cb->bm   = bm;
	cb->next = cb->prev = 0;
	cb->area = CLIP_NO_AREA;
	cb->self = (uint8_t) (clip_num_bmodels + 1);
	clip_owner[clip_num_bmodels] = e;
	e->clip = (uint8_t) ++clip_num_bmodels;
	return 1;
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

static int clip_solid(const edict_t *e)
{
	return e->solid == SOLID_BSP || e->solid == SOLID_BBOX || e->solid == SOLID_SLIDEBOX;
}

/* the entry's box from its edict's (a solid one), in place */
static void clip_box(edict_t *e)
{
	clip_bmodel_t *cb = &clip_bmodels[e->clip - 1];
	__builtin_memcpy(cb->origin, e->origin, sizeof cb->origin);
	if ((uintptr_t) cb->bm > 1) {
		__builtin_memcpy(cb->absmin, e->absmin, sizeof cb->absmin);
		__builtin_memcpy(cb->absmax, e->absmax, sizeof cb->absmax);
	} else {                                  /* a box: its own, exact (clip.h), a */
		cb->bm = (e->flags & FL_MONSTER) ? CLIP_BM_MONSTER : 0;   /* monster's marked */
		for (int k = 0; k < 3; k++) {
			cb->absmin[k] = e->origin[k] + e->mins[k];
			cb->absmax[k] = e->origin[k] + e->maxs[k];
		}
	}
}

/* the entry after its edict's solid changed, the edict not relinked (as a
   pusher out of the traces and back, SV_PushMove's solid flips): its box
   in place, it stays in its list (SV_ClipToLinks skips SOLID_NOT) */
static void clip_update(edict_t *e)
{
	if (!e->clip)
		return;
	if (!clip_solid(e)) {
		clip_disable(e);
		return;
	}
	clip_box(e);
	if (clip_bmodels[e->clip - 1].area == CLIP_NO_AREA)
		clip_link(&clip_bmodels[e->clip - 1], e->absmin, e->absmax);
}

/* SV_LinkEdict for the entry: its box, relinked (a solid edict) or
   unlinked */
static void clip_relink(edict_t *e)
{
	if (!e->clip)
		return;
	clip_bmodel_t *cb = &clip_bmodels[e->clip - 1];
	if (!clip_solid(e)) {
		clip_disable(e);
		clip_unlink(cb);
		return;
	}
	clip_box(e);
	clip_link(cb, e->absmin, e->absmax);
}

/* a SOLID_BBOX (or SLIDEBOX) entity without a brush model into the traces:
   SV_ClipToLinks takes its box (SV_HullForBox) */
static void clip_add_box(edict_t *e)
{
	if (!clip_new(e, 0))
		printf("ents: more than %d solid entities\n", CLIP_MAX_BMODELS);
}

void ent_remove(edict_t *e)
{
	if (e == ent_world || e == ent_player || !e->inuse)
		return;
	clip_disable(e);
	if (e->clip)
		clip_unlink(&clip_bmodels[e->clip - 1]);
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
	if (e->item_model >= 0 || e->masset >= 0) {   /* an alias model: drawn in its centre's leaf, */
		e->leaves[0]  = 0;                        /* found when it is drawn (ent_model_leaf) */
		e->num_leaves = 1;
	} else if (e->draw && e->model) {
		uint16_t idx[ENT_MAX_LEAVES];
		e->num_leaves = (uint8_t) world_box_leaves(e->absmin, e->absmax, idx, ENT_MAX_LEAVES);
		for (int k = 0; k < e->num_leaves; k++)
			e->leaves[k] = &world.leaves[idx[k]];
		int32_t lo[3], hi[3];                 /* camera_side_leaf's points: the box, */
		for (int k = 0; k < 3; k++) {         /* 2 units beyond it, truncated to units */
			lo[k] = (e->absmin[k] >> 3) - 2;  /* (CLIP_SUB 8), with a unit to spare */
			hi[k] = (e->absmax[k] >> 3) + 3;
		}
		e->topnode = world_box_topnode(lo, hi);
		e->vis_stamp = 0xffff;
		ents_relink = 1;
	}
	if (!e->clip && !e->model && e != ent_player && (e->solid == SOLID_BBOX || e->solid == SOLID_SLIDEBOX))
		clip_add_box(e);
	clip_relink(e);
	if (e->touch_slot >= 0 && touch_slots)
		touch_relink(e);
}

const qw_leaf *ent_model_leaf(edict_t *e)
{
	if (!e->leaves[0]) {
		int32_t c[3];
		for (int k = 0; k < 3; k++)
			c[k] = (e->origin[k] + (e->mins[k] + e->maxs[k]) / 2) / CLIP_SUB;
		e->leaves[0] = world_point_leaf(c);
	}
	return e->leaves[0];
}

void ent_setorigin(edict_t *e, const int32_t org[3])
{
	__builtin_memcpy(e->origin, org, sizeof e->origin);
	ent_link(e);
}

void ent_setsize(edict_t *e, const int32_t mins[3], const int32_t maxs[3])
{
	__builtin_memcpy(e->mins, mins, sizeof e->mins);
	__builtin_memcpy(e->maxs, maxs, sizeof e->maxs);
	ent_link(e);
}

/* setmodel(): the brush model's box; a solid one (SOLID_BSP, a shot
   trigger's SOLID_BBOX: its brush is its box) into the traces */
void ent_setmodel(edict_t *e, int model)
{
	const qw_bmodel *bm = &world.bmodels[model];
	e->model = (int16_t) model;
	__builtin_memcpy(e->mins, bm->mins, sizeof e->mins);
	__builtin_memcpy(e->maxs, bm->maxs, sizeof e->maxs);
	if ((e->solid == SOLID_BSP || e->solid == SOLID_BBOX) && model && !e->clip && !clip_new(e, bm))
		printf("ents: more than %d solid brush models\n", CLIP_MAX_BMODELS);
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
	__builtin_memcpy(self->finaldest, tdest, sizeof self->finaldest);
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
	int32_t ticks = (int32_t) q_div64_32((int64_t) len * 60 + 4 * speed, 8 * speed);
	if (ticks < TICKS(10)) {
		self->velocity[0] = self->velocity[1] = self->velocity[2] = 0;
		self->nextthink = self->ltime + TICKS(10);
		return;
	}
	self->nextthink = self->ltime + ticks;
	__builtin_memcpy(self->move_from, self->origin, sizeof self->move_from);
	__builtin_memcpy(self->move_delta, d, sizeof self->move_delta);
	self->move_t0    = self->ltime;
	self->move_ticks = ticks;
	for (int k = 0; k < 3; k++) {
		self->velocity[k] = d[k] * 60 / (8 * ticks);
		if (!self->velocity[k] && d[k])
			self->velocity[k] = d[k] > 0 ? 1 : -1;
	}
}

/* ------------------------------------------------------------ combat.qc -- */

/* combat.qc Killed: a pusher's or a trigger's th_die as it is; a
   monster's: angry at its killer, counted, no more damage or touch, its
   targets fired (monster_death_use), its th_die */
static void killed(edict_t *targ, edict_t *attacker)
{
	if (targ->health < -99)
		targ->health = -99;               /* don't let sbar look bad if a player */
	if (targ->movetype == MOVETYPE_PUSH || targ->movetype == MOVETYPE_NONE) {
		if (targ->th_die)
			targ->th_die(targ);           /* doors, triggers, etc */
		return;
	}
	targ->enemy = attacker;
	if (targ->flags & FL_MONSTER)         /* bump the monster counter */
		ent_killed_monsters++;
	targ->takedamage = DAMAGE_NO;
	targ->touch = sub_null;
	if (targ->mdef) {
		targ->pain_finished = ent_time + CORPSE_TICKS;   /* its body's end (monsters.h) */
		monster_death_use(targ);
	}
	if (targ->th_die)
		targ->th_die(targ);
}

/* combat.qc T_Damage, the damage x256 (the QC's damage is a float: a
   melee's sum of randoms, a blast's falloff): the attacker's quad, the
   armour (the player's, the game's), the health less the damage rounded
   up, Killed, or the monster's anger (not at its own kind, but the
   soldiers') and its pain */
void ent_damage_x256(edict_t *targ, edict_t *inflictor, edict_t *attacker, int damage)
{
	if (!targ || !targ->takedamage)
		return;
	ent_damage_attacker = attacker;       /* used by buttons and triggers to set activator */
	if (attacker == ent_player && game_quad_active())
		damage *= 4;
	if (targ == ent_player) {
		int32_t c[3];
		if (inflictor && inflictor != ent_world) {
			for (int k = 0; k < 3; k++)
				c[k] = (inflictor->absmin[k] + inflictor->absmax[k]) / 2;
			game_damage_player_x256(damage, c);
		} else {
			game_damage_player_x256(damage, 0);
		}
		return;
	}
	int take = (damage + 255) >> 8;       /* ceil (no armour: only the player's) */
	targ->health = (int16_t) (targ->health - take);
	ELOG("ent: %s takes %d (health %d)\n", world_string(targ->classname), take, targ->health);
	if (targ->health <= 0) {
		killed(targ, attacker);
		return;
	}
	/* react to the damage */
	if ((targ->flags & FL_MONSTER) && attacker && attacker != ent_world) {
		/* get mad unless of the same class (except for soldiers) */
		if (targ != attacker && attacker != targ->enemy &&
		    (attacker->mdef != targ->mdef || (targ->mdef && targ->mdef->kind == MK_ARMY))) {
			if (targ->enemy == ent_player)
				targ->oldenemy = targ->enemy;
			targ->enemy = attacker;
			ai_found_target(targ);
		}
	}
	if (targ->th_pain) {
		ai_damage_take = take;
		targ->th_pain(targ);
		if (game_skill == 3 && targ->mdef)   /* nightmare mode monsters don't go into pain frames often
		                                        (a monster's field: not a shootable door's stops) */
			targ->pain_finished = ent_time + TICKS(500);
	}
}

void ent_damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, int damage)
{
	ent_damage_x256(targ, inflictor, attacker, damage * 256);
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

void ents_radius_damage(const int32_t org[3], int damage, edict_t *attacker, edict_t *ignore)
{
	/* findradius's chain: the last edict found first */
	int32_t rad = (damage + 40) * CLIP_SUB;
	for (int i = num_edicts - 1; i >= 2; i--) {
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
		int points = damage * 256 - len * (256 / (2 * CLIP_SUB));   /* damage - vlen / 2, x256 */
		if (e == attacker)
			points /= 2;
		if (points > 0 && ai_can_damage_at(e, org, 0)) {
			if (e->mdef && e->mdef->kind == MK_SHAMBLER)
				points /= 2;
			ent_damage_x256(e, attacker, attacker, points);
		}
	}
}

/* -------------------------------------------------------------- physics -- */

static const int32_t player_mins[3] = { -16 * CLIP_SUB, -16 * CLIP_SUB, -24 * CLIP_SUB };
static const int32_t player_maxs[3] = {  16 * CLIP_SUB,  16 * CLIP_SUB,  32 * CLIP_SUB };

/* the player's mirror: its box, health, items, from the game's player;
   its box in the traces' entry 0 (clip.h: the entities' traces meet it,
   its own begin past it) */
static void sync_player(void)
{
	edict_t *p = ent_player;
	__builtin_memcpy(p->origin, pl.pm.pos, sizeof p->origin);
	__builtin_memcpy(p->velocity, pl.pm.vel, sizeof p->velocity);
	for (int k = 0; k < 3; k++) {
		p->absmin[k] = p->origin[k] + player_mins[k] - CLIP_SUB;
		p->absmax[k] = p->origin[k] + player_maxs[k] + CLIP_SUB;
	}
	p->health     = (int16_t) pl.health;
	p->items      = pl.items;
	p->takedamage = pl.dead || game_intermission() ? DAMAGE_NO : DAMAGE_AIM;
	p->solid      = pl.dead || game_intermission() || pl.pm.noclip ? SOLID_NOT : SOLID_SLIDEBOX;
	p->movetype   = pl.pm.noclip ? MOVETYPE_NOCLIP : MOVETYPE_WALK;
	clip_relink(p);                           /* (SV_LinkEdict, its every move) */
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
		out[k] = e->move_from[k] + (int32_t) q_div64_32((int64_t) e->move_delta[k] * t, e->move_ticks);
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
	__builtin_memcpy(pushorig, pusher->origin, sizeof pushorig);
	pusher->ltime += movetime;
	ent_setorigin(pusher, to);

	/* the player (edict 1, SV_PushMove's first) */
	edict_t *p = ent_player;
	int32_t plorig[3];
	int player_moved = 0;
	__builtin_memcpy(plorig, pl.pm.pos, sizeof plorig);
	if (p->movetype != MOVETYPE_NOCLIP && !pl.dead && !game_intermission() &&
	    (player_ground() == pusher ||
	     (!(p->absmin[0] >= maxs[0] || p->absmin[1] >= maxs[1] || p->absmin[2] >= maxs[2] ||
	        p->absmax[0] <= mins[0] || p->absmax[1] <= mins[1] || p->absmax[2] <= mins[2]) && player_stuck()))) {
		int32_t end[3];
		for (int k = 0; k < 3; k++)
			end[k] = pl.pm.pos[k] + move[k];
		uint8_t solid = pusher->solid;
		pusher->solid = SOLID_NOT;             /* (out of the traces) */
		clip_update(pusher);
		trace_t tr;
		clip_trace(1, pl.pm.pos, end, &tr);
		__builtin_memcpy(pl.pm.pos, tr.endpos, sizeof pl.pm.pos);
		pusher->solid = solid;
		clip_update(pusher);
		sync_player();
		player_moved = 1;
		if (player_stuck()) {                  /* fail the move */
			__builtin_memcpy(pl.pm.pos, plorig, sizeof pl.pm.pos);
			sync_player();
			pusher->ltime -= movetime;
			ent_setorigin(pusher, pushorig);
			if (pusher->blocked) {
				ent_other = p;
				ELOG("ent: %s blocked\n", world_string(pusher->classname));
				pusher->blocked(pusher);
			}
			return;
		}
	}

	/* the other edicts: a monster, a dropped pack, a gib riding it or in
	   its way, pushed along; one stuck blocks it (every one moved put
	   back), a corpse or a trigger in the way flattened, a point left */
	static edict_t *moved[256];
	static int32_t  moved_from[256][3];
	int nmoved = 0;
	for (int i = 2; i < num_edicts; i++) {
		edict_t *c = &edicts[i];
		if (!c->inuse || c == pusher || c->movetype == MOVETYPE_PUSH || c->movetype == MOVETYPE_NONE ||
		    c->movetype == MOVETYPE_NOCLIP)
			continue;
		/* if the entity is standing on the pusher, it will definitely be moved */
		if (!((c->flags & FL_ONGROUND) && c->groundentity == pusher)) {
			if (c->absmin[0] >= maxs[0] || c->absmin[1] >= maxs[1] || c->absmin[2] >= maxs[2] ||
			    c->absmax[0] <= mins[0] || c->absmax[1] <= mins[1] || c->absmax[2] <= mins[2])
				continue;
			if (!ai_test_position(c))      /* not in the pusher's final position */
				continue;
		}
		c->flags &= ~FL_ONGROUND;              /* (not a player: it walks no more) */
		if (nmoved < 256) {
			moved[nmoved] = c;
			__builtin_memcpy(moved_from[nmoved++], c->origin, sizeof moved_from[0]);
		}
		int32_t entorig[3];
		__builtin_memcpy(entorig, c->origin, sizeof entorig);
		uint8_t solid = pusher->solid;         /* try moving the contacted entity */
		pusher->solid = SOLID_NOT;
		clip_update(pusher);
		trace_t tr;
		ai_push_entity(c, move, &tr);
		pusher->solid = solid;
		clip_update(pusher);
		if (!c->inuse || !ai_test_position(c))
			continue;
		/* still inside the pusher: blocked */
		if (c->mins[0] == c->maxs[0])          /* a point: left in it */
			continue;
		if (c->solid == SOLID_NOT || c->solid == SOLID_TRIGGER) {   /* a corpse: flattened */
			c->mins[0] = c->mins[1] = 0;
			__builtin_memcpy(c->maxs, c->mins, sizeof c->maxs);
			continue;
		}
		__builtin_memcpy(c->origin, entorig, sizeof c->origin);
		ent_link(c);
		pusher->ltime -= movetime;
		ent_setorigin(pusher, pushorig);
		/* if the pusher has a "blocked" function, call it, otherwise just
		   stay in place until the obstacle is gone */
		if (pusher->blocked) {
			ent_other = c;
			ELOG("ent: %s blocked by %s\n", world_string(pusher->classname), world_string(c->classname));
			pusher->blocked(pusher);
		}
		/* move back any entities we already moved */
		for (int j = 0; j < nmoved; j++)
			if (moved[j]->inuse) {
				__builtin_memcpy(moved[j]->origin, moved_from[j], sizeof moved_from[0]);
				ent_link(moved[j]);
			}
		if (player_moved) {
			__builtin_memcpy(pl.pm.pos, plorig, sizeof pl.pm.pos);
			sync_player();
		}
		return;
	}
}

/* SV_Physics_Pusher, in 60ths: the tick's move (game_dt, host_frametime)
   up to the think's time, the think */
static void physics_pusher(edict_t *e)
{
	int32_t oldltime  = e->ltime;
	int32_t thinktime = e->nextthink;
	int     movetime;
	if (thinktime < e->ltime + game_dt) {
		movetime = thinktime - e->ltime;
		if (movetime < 0)
			movetime = 0;
	} else {
		movetime = game_dt;
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

/* a box in whole units, the touch passes' (floor of the mins, ceiling of
   the maxs: it holds the exact one) */
static void box16(const edict_t *e, int16_t b[6])
{
	for (int k = 0; k < 3; k++) {
		b[k]     = (int16_t) (e->absmin[k] >> 3);
		b[k + 3] = (int16_t) ((e->absmax[k] + 7) >> 3);
	}
}

/* SV_LinkEdict for a trigger: its slot's box, the slot unlinked and
   appended to its node's list */
/* a trigger slot linked in node n (d 1) or unlinked (d -1): the subtree
   counts of n and its ancestors */
static void trig_count_add(int n, int d, int mon)
{
	for (;;) {
		trig_count[0][n] = (uint16_t) (trig_count[0][n] + d);
		if (mon)
			trig_count[1][n] = (uint16_t) (trig_count[1][n] + d);
		if (!n)
			return;
		n = area_parent[n];
	}
}

static void list_unlink(int list, int a, int s)
{
	uint16_t *next = touch_next[list], self = (uint16_t) (s + 1), prev = 0;
	for (uint16_t l = trig_head[list][a]; l; prev = l, l = next[l - 1])
		if (l == self) {
			if (prev)
				next[prev - 1] = next[s];
			else
				trig_head[list][a] = next[s];
			if (trig_tail[list][a] == self)
				trig_tail[list][a] = prev;
			break;
		}
	next[s] = 0;
}
static void list_append(int list, int a, int s)
{
	uint16_t *next = touch_next[list];
	if (trig_tail[list][a])
		next[trig_tail[list][a] - 1] = (uint16_t) (s + 1);
	else
		trig_head[list][a] = (uint16_t) (s + 1);
	trig_tail[list][a] = (uint16_t) (s + 1);
	next[s] = 0;
}

static void touch_unlink(int s)
{
	touch_slot_t *t = &touch_slots[s];
	if (t->area == CLIP_NO_AREA)
		return;
	trig_count_add(t->area, -1, t->mon);
	list_unlink(0, t->area, s);
	if (t->mon)
		list_unlink(1, t->area, s);
	t->area = CLIP_NO_AREA;
}
static void touch_relink(const edict_t *e)
{
	int s = e->touch_slot;
	touch_slot_t *t = &touch_slots[s];
	box16(e, t->box);
	touch_unlink(s);
	int n = area_of(e->absmin, e->absmax);
	t->area = (uint8_t) n;
	/* the monsters' list: the slots whose touch can do something to
	   another edict than the player (id's QC: an item's touch, a
	   trigger_multiple's, a changelevel's... return at once for all but
	   the player) */
	t->mon = !(e->flags & (FL_ITEM | FL_TOUCH_PLAYER));
	list_append(0, n, s);
	if (t->mon)
		list_append(1, n, s);
	trig_count_add(n, 1, t->mon);
}

static void rebuild_lists(void)
{
	num_touch = num_draw = ents_num_models = ents_num_streamed = 0;
	memset(trig_head, 0, sizeof trig_head);
	memset(trig_tail, 0, sizeof trig_tail);
	memset(trig_count, 0, sizeof trig_count);
	memset(pushers, 0, sizeof pushers);
	for (int i = 2; i < num_edicts; i++) {
		edict_t *e = &edicts[i];
		if (!e->inuse)
			continue;
		if (e->movetype == MOVETYPE_PUSH)
			pushers[i >> 5] |= 1u << (i & 31);
		if (e->item_model >= 0 && ents_num_models < (int) (sizeof ents_model_list / sizeof ents_model_list[0]))
			ents_model_list[ents_num_models++] = (uint16_t) i;
		if ((e->masset >= 0 || e->mdef) &&
		    ents_num_streamed < (int) (sizeof ents_streamed_list / sizeof ents_streamed_list[0]))
			ents_streamed_list[ents_num_streamed++] = (uint16_t) i;
		e->touch_slot = -1;
		if (e->touch && (e->solid == SOLID_TRIGGER || e->solid == SOLID_NOT) && !e->mdef &&
		    num_touch < max_touch) {
			e->touch_slot = (int16_t) num_touch;
			touch_slots[num_touch].area = CLIP_NO_AREA;
			touch_list[num_touch++] = (uint16_t) i;
			touch_relink(e);
		}
		if (e->draw && e->model && e->num_leaves &&
		    num_draw < (int) (sizeof draw_list / sizeof draw_list[0]))
			draw_list[num_draw++] = (uint16_t) i;
	}
	lists_dirty = 0;
	ents_relink = 1;
}

#ifdef Q1PSX_BENCH
int ents_num_touch(void)
{
	return num_touch;
}

#ifdef Q1PSX_BENCH
int ents_in_use(int *max)
{
	int n = 0;
	for (int i = 0; i < num_edicts; i++)
		n += edicts[i].inuse;
	*max = max_edicts;
	return n;
}
#endif

/* the area nodes' lists: solid entries and trigger slots in each node */
void ents_area_stats(void)
{
	if (lists_dirty)
		rebuild_lists();
	printf("areas:");
	for (int a = 0; a < num_areas; a++) {
		int ns = 0, nt = 0;
		for (uint16_t l = clip_areas[a].head; l; l = CLIP_ENTRY(l)->next)
			ns++;
		for (uint16_t l = trig_head[0][a]; l; l = touch_next[0][l - 1])
			nt++;
		printf(" %d:%s%d/%d", a, clip_areas[a].flags & CLIP_AREA_LEAF ? "L" : (clip_areas[a].flags & 1) ? "y" : "x",
		       ns, nt);
	}
	printf("\n");
}
#endif

/* SV_TouchLinks' walk: the trigger slots whose box the box of e reaches,
   area node by area node (Quake's order: clip.h), their edicts; touched
   after (the touches spawn, remove and move edicts: each one is tested
   again when its turn comes). List 0: every slot (the player's walk); 1:
   the monsters' list (another edict's: the slots whose touch can do
   something to it, in the same order, a node with none of them pruned) */
#define TOUCH_MAX 64
static int touch_gather(const edict_t *e, uint16_t out[TOUCH_MAX], int list)
{
	const uint16_t *count = trig_count[list], *head = trig_head[list], *next = touch_next[list];
	int16_t eb[6];
	box16(e, eb);
	int n = 0;
	uint8_t stack[CLIP_AREA_DEPTH + 2];          /* the nodes to walk (depth first, */
	int     sp = 0;                              /* children[0] on top) */
	if (count[0])
		stack[sp++] = 0;
	while (sp) {
		int a = stack[--sp];
		const clip_area_t *an = &clip_areas[a];
		if (!(an->flags & CLIP_AREA_LEAF)) {
			int axis = an->flags & CLIP_AREA_Y;
			if (e->absmin[axis] < an->dist && count[an->child1])
				stack[sp++] = an->child1;
			if (e->absmax[axis] > an->dist && count[a + 1])
				stack[sp++] = (uint8_t) (a + 1);     /* children[0]: the next node */
		}
		for (uint16_t l = head[a]; l; l = next[l - 1]) {
			const int16_t *b = touch_slots[l - 1].box;
#ifdef Q1PSX_BENCH
			ai_tr_n[AI_TR_OTHER]++;                   /* (the bench: the slots tested) */
#endif
			if (eb[0] > b[3] || eb[1] > b[4] || eb[2] > b[5] || eb[3] < b[0] || eb[4] < b[1] || eb[5] < b[2])
				continue;
			if (n < TOUCH_MAX)
				out[n++] = touch_list[l - 1];
		}
	}
	return n;
}

/* SV_TouchLinks: the SOLID_TRIGGER edicts the box of e overlaps, touched
   by it (the player each tick; a monster when it moves) */
void ents_touch_links(edict_t *e)
{
	if (e->solid == SOLID_NOT || !edicts)
		return;
	if (lists_dirty)
		rebuild_lists();
#ifdef Q1PSX_BENCH
	uint32_t t0 = prof_now();
#endif
	uint16_t hit[TOUCH_MAX];
	int n = touch_slots ? touch_gather(e, hit, e != ent_player) : 0;   /* (another edict: the
	                                                                      monsters' list) */
	for (int i = 0; i < n; i++) {
		edict_t *t = &edicts[hit[i]];
		if (t == e || !t->inuse || !t->touch || t->solid != SOLID_TRIGGER)
			continue;
		if (e->absmin[0] > t->absmax[0] || e->absmin[1] > t->absmax[1] || e->absmin[2] > t->absmax[2] ||
		    e->absmax[0] < t->absmin[0] || e->absmax[1] < t->absmin[1] || e->absmax[2] < t->absmin[2])
			continue;
		edict_t *other = ent_other;
		ent_other = e;
		t->touch(t);
		ent_other = other;
		if (!e->inuse)
			break;
	}
#ifdef Q1PSX_BENCH
	ai_tr_cyc[AI_TR_TOUCH] += prof_cycles(t0, prof_now());
	ai_tr_n[AI_TR_TOUCH]++;
#endif
}

/* SV_TouchLinks for the player: the SOLID_TRIGGER edicts its box overlaps */
static void touch_player(void)
{
	edict_t *p = ent_player;
	if (p->solid == SOLID_NOT || !touch_slots)
		return;
	uint16_t hit[TOUCH_MAX];
	int n = touch_gather(p, hit, 0);
	for (int i = 0; i < n; i++) {
		edict_t *t = &edicts[hit[i]];
		if (!t->inuse || !t->touch || t->solid != SOLID_TRIGGER)
			continue;
		if (p->absmin[0] > t->absmax[0] || p->absmin[1] > t->absmax[1] || p->absmin[2] > t->absmax[2] ||
		    p->absmax[0] < t->absmin[0] || p->absmax[1] < t->absmin[1] || p->absmax[2] < t->absmin[2])
			continue;
		ent_other = p;
		t->touch(t);
	}
}

/* the lowest set bit's index by a de Bruijn sequence (the edicts' active
   words): __builtin_ctz is libgcc's __ctzsi2 and its 256-byte table here */
static const uint8_t ctz_tab[32] = {
	0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
	31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
};

/* SV_Physics, a tick: the player's touches, then the edicts in their order.
   The world's rate (game_world_hz): at 60, every edict each tick; at 30,
   the movers each tick (they carry the player: game_dt 1), the others in
   two halves by their index's parity, a half each tick, over 2 60ths
   (game_dt 2): a 60 Hz world's load spread over both ticks, each edict's
   thinks on its half's ticks */
void ents_tick(void)
{
	if (!edicts)
		return;
	ent_time = game_tick_count;
	if (lists_dirty)
		rebuild_lists();
	sync_player();
	touch_player();
	int      wdt  = game_world_hz >= 60 ? 1 : 2;
	uint32_t half = wdt == 1 ? 0xffffffffu : (ent_time & 1) ? 0xaaaaaaaau : 0x55555555u;
	if (ent_force_retouch > 0 && (wdt == 1 || !(ent_time & 1)))
		ent_force_retouch--;                  /* (at 30: a count of both halves' ticks) */
	game_dt = wdt;
	for (int w = 0; w < ENT_WORDS; w++) {
		uint32_t bits = active[w] & (half | pushers[w]);
		while (bits) {
			int b = ctz_tab[((bits & -bits) * 0x077cb531u) >> 27];   /* (not libgcc's __ctzsi2) */
			bits &= bits - 1;
			edict_t *e = &edicts[w * 32 + b];
			if (e->inuse && ent_force_retouch && e->movetype != MOVETYPE_PUSH)
				ents_touch_links(e);           /* force retouch even for stationary */
			if (e->inuse) {
				if (e->movetype == MOVETYPE_PUSH) {
					game_dt = 1;
					physics_pusher(e);
					game_dt = wdt;
				} else if (e->mdef || e->masset >= 0) {   /* a monster, what it throws */
					if (e->mdef && e->health <= 0 && ent_time - e->pain_finished >= 0)
						ent_remove(e);                /* a body's end (monsters.h) */
					else if (e->mdef && e->masset >= 0 && !stream_model(e->masset))
						;                             /* its model not in: paused, nothing it
						                                 does unseen (2026-10-19) */
					else if (e->movetype == MOVETYPE_STEP)
						ai_physics_step(e);
					else if (e->movetype >= MOVETYPE_FLY && e->movetype != MOVETYPE_PUSH &&
					         e->movetype != MOVETYPE_NOCLIP)
						ai_physics_toss(e);
					else
						run_think(e);
				} else {
					run_think(e);                 /* SV_Physics_Toss: the think, then the move */
					if (e->inuse && e->movetype == MOVETYPE_TOSS)
						items_physics(e);
				}
			}
			/* asleep: nothing to think, nothing moving (a monster or what
			   it throws keeps falling while off the ground) */
			if (!e->inuse || (e->nextthink <= 0 && !e->move_ticks &&
			                  !((e->mdef || e->masset >= 0) && e->movetype >= MOVETYPE_STEP &&
			                    e->movetype != MOVETYPE_PUSH && !(e->flags & FL_ONGROUND)) &&
			                  !(e->mdef && e->health <= 0)))   /* (a body: till its end) */
				active[w] &= ~(1u << b);
		}
	}
	game_dt = 1;
	sync_player();
}

/* Where an edict is drawn, its angles: at the 30 Hz world, one that moves
   by its velocity (a fall, a throw, a gib's spin) and whose half was not
   run this tick is drawn a 60th on, where the 60 Hz world would have it
   (its next move's start); else where it is. */
const int32_t *ents_draw_pose(const edict_t *e, int32_t tmp[3], uint16_t ang[3])
{
	ang[0] = e->ang[0];
	ang[1] = e->ang[1];
	ang[2] = e->ang[2];
	if (game_world_hz >= 60 || !((index_of(e) ^ ent_time) & 1))
		return e->origin;
	int mt = e->movetype;
	if (mt == MOVETYPE_STEP ? (e->flags & (FL_ONGROUND | FL_FLY | FL_SWIM)) != 0
	                        : mt < MOVETYPE_FLY || mt == MOVETYPE_PUSH || (e->flags & FL_ONGROUND))
		return e->origin;
	for (int k = 0; k < 3; k++) {
		tmp[k] = e->origin[k] + e->velocity[k] * CLIP_SUB / GAME_HZ;
		if (mt != MOVETYPE_STEP)              /* (a monster's turns: its thinks') */
			ang[k] = (uint16_t) (ang[k] + e->avel[k] * 65536 / 360 / GAME_HZ);
	}
	return tmp;
}

/* ---------------------------------------------------------------- spawn -- */

Q_COLD void ents_load(void)
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
	max_touch = max_edicts < (int) (sizeof touch_list / sizeof touch_list[0]) ? max_edicts
	          : (int) (sizeof touch_list / sizeof touch_list[0]);
	touch_slots   = max_touch ? mem_level_alloc((uint32_t) max_touch * sizeof *touch_slots) : 0;
	touch_next[0] = touch_slots ? mem_level_alloc((uint32_t) max_touch * 2 * sizeof (uint16_t)) : 0;
	touch_next[1] = touch_next[0] ? touch_next[0] + max_touch : 0;
	if (!touch_slots || !touch_next[0])
		max_touch = 0;
}

/* the edict for a cooked entity: the fields every class takes (ED_ParseEdict) */
static edict_t *ent_new(const qw_ent *q)
{
	if (num_edicts >= max_edicts)
		return 0;
	edict_t *e = &edicts[num_edicts++];
	memset(e, 0, sizeof *e);
	e->touch_slot = -1;
	e->inuse      = 1;
	e->class      = q->class;
	e->classname  = q->classname;
	e->spawnflags = q->spawnflags;
	__builtin_memcpy(e->origin, q->origin, sizeof e->origin);
	e->angle      = q->angle;
	__builtin_memcpy(e->mangle, q->mangle, sizeof e->mangle);
	e->model  = q->model;
	e->speed  = q->speed;  e->wait   = q->wait;   e->delay  = q->delay;
	e->lip    = q->lip;    e->height = q->height; e->dmg    = q->dmg;
	e->health = q->health; e->count  = q->count;  e->sounds = q->sounds;
	e->style  = q->style;
	e->target = q->target; e->targetname = q->targetname; e->killtarget = q->killtarget;
	e->message = q->message; e->map = q->map;
	e->item_model = -1;
	e->masset = -1;
	e->noise = e->noise1 = e->noise2 = e->noise3 = e->noise4 = SND_NONE;
	return e;
}

Q_COLD void ents_spawn(int first)
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
	__builtin_memcpy(edicts[1].mins, player_mins, sizeof player_mins);
	__builtin_memcpy(edicts[1].maxs, player_maxs, sizeof player_maxs);
	edicts[0].noise = edicts[0].noise1 = edicts[0].noise2 = edicts[0].noise3 = edicts[0].noise4 = SND_NONE;
	edicts[1].noise = edicts[1].noise1 = edicts[1].noise2 = edicts[1].noise3 = edicts[1].noise4 = SND_NONE;
	{                                         /* SV_ClearWorld: the area nodes, over */
		int32_t mins[3] = { 0, 0, 0 }, maxs[3] = { 0, 0, 0 };   /* the world's box (its */
		const qw_node *r = world.root;                         /* BSP root's, in vertex */
		for (int k = 0; r && k < 3; k++) {                     /* units: x 2 for 1/8; */
			mins[k] = (r->c[k] - r->h[k]) * (CLIP_SUB / QW_VSCALE);   /* within int16, */
			maxs[k] = (r->c[k] + r->h[k]) * (CLIP_SUB / QW_VSCALE);   /* the splits too) */
			mins[k] = mins[k] < -32768 ? -32768 : mins[k];
			maxs[k] = maxs[k] > 32767 ? 32767 : maxs[k];
		}
		num_areas = 0;
		create_area(r ? 0 : CLIP_AREA_DEPTH, mins, maxs);     /* (no world: one leaf) */
	}
	clip_new(ent_player, 0);                  /* the traces' entry 0: the player's box */
	clip_disable(ent_player);
	edicts[0].item_model = edicts[1].item_model = -1;     /* (no model of the map's) */
	edicts[0].masset = edicts[1].masset = -1;
	edicts[0].touch_slot = edicts[1].touch_slot = -1;
	num_edicts = 2;

	int unknown = 0, inhibit = 0;
	const uint8_t *at = world.ents;         /* the compact table, decoded one by one: */
	for (uint32_t i = 0; i < world.num_ents; i++) {   /* the spawn functions read q */
		qw_ent ent;                           /* during their call only */
		at = qw_ent_decode(at, world.ent_names, &ent);
		const qw_ent *q = &ent;
		int cls = q->class;
		/* ED_LoadFromFile: not on this skill (not easy 256, not medium
		   512, not hard 1024) */
		if ((game_skill == 0 && (q->spawnflags & 256)) || (game_skill == 1 && (q->spawnflags & 512)) ||
		    (game_skill >= 2 && (q->spawnflags & 1024))) {
			inhibit++;
			continue;
		}

		if (cls == QE_UNKNOWN) {
			unknown++;
			continue;
		}
		edict_t *e = ent_new(q);
		if (!e)
			break;

		if (cls >= QE_MONSTER_ARMY && cls <= QE_MONSTER_OLDONE) {
			monsters_spawn(e, q);
			continue;
		}
		switch (cls) {
		case QE_FUNC_DOOR: case QE_FUNC_DOOR_SECRET: case QE_FUNC_BUTTON:
		case QE_FUNC_PLAT: case QE_FUNC_TRAIN: case QE_MISC_TELEPORTTRAIN:
			movers_spawn(e, q);
			break;
		case QE_TRIGGER_CHANGELEVEL: case QE_INFO_INTERMISSION:
			level_spawn(e, q);
			break;
		case QE_EVENT_LIGHTNING:
			monsters_event_lightning(e);
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
	printf("ents: %d edicts (%d monsters, %d unknown, %d not on skill %d), %d solid brush "
	       "models, %d drawn, %d models, %d touch, %d secrets\n", num_edicts, ent_total_monsters, unknown, inhibit,
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
		extern int monsters_mdef_ok(const struct mdef_s *m);
		if (e->num_leaves > ENT_MAX_LEAVES || e->item_model < -1 || e->item_model >= (int) world.num_models ||
		    e->model < 0 || e->model >= (int) world.num_bmodels || e->class > QE_TEMP || !monsters_mdef_ok(e->mdef)) {
			printf("ents: CORRUPT at %s: edict %d of %d at %08x (size %u, pool %08x..%08x) class %d model %d "
			       "item %d leaves %d mdef %08x\n", where, i, num_edicts, (unsigned) (uintptr_t) e, (unsigned) sizeof *e,
			       (unsigned) (uintptr_t) edicts, (unsigned) (uintptr_t) (edicts + max_edicts), e->class,
			       e->model, (int) e->item_model, e->num_leaves, (unsigned) (uintptr_t) e->mdef);
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

/* a model's box not wholly behind the eye (leaf_in_front's test) */
static int box_in_front(const edict_t *e)
{
	int32_t c[3], h = 0;
	for (int k = 0; k < 3; k++) {                 /* 1/8 units -> vertex units */
		c[k] = (e->absmin[k] + e->absmax[k]) >> 2;
		h   += (e->absmax[k] - e->absmin[k]) >> 2;
	}
	int32_t z = ((r_state.rot[6] * c[0] + r_state.rot[7] * c[1] + r_state.rot[8] * c[2]) >> 12) +
	            r_state.tr[2];
	return z + h * 3 / 2 > 0;
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
   the PVS sees. A model with pieces (bmfrag.c) is seen when the PVS sees
   one of its pieces' leaves: exact, where its box's leaves are a superset
   that stops at 8 (the slab over START's pit touches more: from some
   places none of its 8 were in the PVS, and it went with them). */
static void ents_visible(uint16_t vis)
{
	bmf_cache *const *bm = bmf_models;
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
		int seen = e->vismask != 0;
		if (bm && bm[e->model] && (seen || e->num_leaves == ENT_MAX_LEAVES))
			seen = bmf_seen(bm[e->model], e, vis, seen);
		if (seen)
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
	const qw_leaf *l = world_point_leaf_from(e->topnode, q);
	if (!l || l->contents == QW_CONTENTS_SOLID || l->visframe != vis)
		return 0;
	return l;
}

void ents_render_bmodels(void)
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
	bmf_cache *const *bm = bmf_models;
	for (int i = 0; i < num_vis; i++) {
		edict_t *e = &edicts[vis_list[i]];
		STAT(1);
		if (bm && bm[e->model]) {                /* cut by the world's BSP, as Quake:
		                                            its pieces in their own leaves */
			if (!box_in_front(e))
				continue;
			if (bmf_draw(e, vis)) {
				STAT(0);
				continue;
			}
			if (!e->vismask)                     /* (too cut up: drawn whole, its
			                                        leaves' way) */
				continue;
		}
		/* drawn whole: among the leaves the PVS sees, the one in front of the eye
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
		rl_add_model(best, rl_bmodel(&world.bmodels[e->model], e->origin), 0, 0);
	}
	rl_pieces_done();
}

void ents_render(void)
{
	if (!edicts)
		return;
	items_render();
	monsters_render();
}

void ents_stream_wants(void)
{
	if (!edicts)
		return;
	if (lists_dirty)
		rebuild_lists();
	monsters_stream_wants();
	/* the entities' streamed sounds (a door's, a plat's, a trigger's, an
	   item's), by their box centre's distance (Chebyshev, world units) */
	for (int i = 2; i < num_edicts; i++) {
		const edict_t *e = &edicts[i];
		if (!e->inuse || (e->noise < SND_STREAM && e->noise1 < SND_STREAM && e->noise2 < SND_STREAM &&
		                  e->noise3 < SND_STREAM && e->noise4 < SND_STREAM))
			continue;
		int32_t c[3];
		ent_centre(e, c);
		uint32_t d = 0;
		for (int k = 0; k < 3; k++) {
			int32_t v = c[k] - stream_origin[k];
			uint32_t x = (uint32_t) (v < 0 ? -v : v);
			if (x > d) d = x;
		}
		d /= CLIP_SUB;
		const int16_t n[5] = { e->noise, e->noise1, e->noise2, e->noise3, e->noise4 };
		for (int k = 0; k < 5; k++)
			if (n[k] >= SND_STREAM)
				stream_want(n[k] - SND_STREAM, d);
	}
	game_stream_wants();
}
