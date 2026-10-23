/*
 * q1psx — doors.qc, plats.qc and buttons.qc, transcribed: doors (linked
 * pairs, the field that opens them, keys, shootable ones, toggles), secret
 * doors, plats and their inside trigger, buttons, trains along their path
 * corners. Their motion is SUB_CalcMove's (engine/ents.c: exact in ticks,
 * pushing the player, blocked when it cannot). The travel is the 1.06
 * QC's: movedir x (|movedir . size| - lip). Strings are id's (qw-qc).
 */

#include <stdio.h>
#include <string.h>

#include "ents.h"
#include "game.h"
#include "mem.h"
#include "render.h"
#include "snd.h"
#include "world.h"

#define DOOR_START_OPEN 1
#define DOOR_DONT_LINK  4
#define DOOR_GOLD_KEY   8
#define DOOR_SILVER_KEY 16
#define DOOR_TOGGLE     32

#define SECRET_OPEN_ONCE 1
#define SECRET_1ST_LEFT  2
#define SECRET_1ST_DOWN  4
#define SECRET_NO_SHOOT  8
#define SECRET_YES_SHOOT 16

#define PLAT_LOW_TRIGGER 1

#define WORLDTYPE_MEDIEVAL 0
#define WORLDTYPE_METAL    1
#define WORLDTYPE_BASE     2

/* pos1 + movedir x (|movedir . size| - lip): the travel of a door / button */
Q_COLD static void travel_end(edict_t *self)
{
	int32_t d = 0;
	for (int k = 0; k < 3; k++)
		d += self->movedir[k] * (self->maxs[k] - self->mins[k]);   /* x4096, 1/8 units (under 2^30) */
	if (d < 0)
		d = -d;
	int32_t dist = d / 4096 - self->lip * CLIP_SUB;
	for (int k = 0; k < 3; k++)
		self->pos2[k] = self->pos1[k] + self->movedir[k] * dist / 4096;
}

static void sound_of(edict_t *self, int16_t *slot, const char *name)
{
	*slot = (int16_t) snd_find(name);
	(void) self;
}

/* ------------------------------------------------------------------ doors -- */

static void door_go_down(edict_t *self);
static void door_go_up(edict_t *self);

static void door_blocked(edict_t *self)
{
	ent_damage(ent_other, self, self, self->dmg);
	/* a door with a negative wait would never come back: it squashes */
	if (self->wait >= 0) {
		if (self->state == STATE_DOWN)
			door_go_up(self);
		else
			door_go_down(self);
	}
}

static void door_hit_top(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, self->noise1, 255, ATTN_NORM);
	self->state = STATE_TOP;
	if (self->spawnflags & DOOR_TOGGLE)
		return;                                   /* don't come down automatically */
	self->think     = door_go_down;
	self->nextthink = self->ltime + TICKS(self->wait);
	ent_wake(self);
}

static void door_hit_bottom(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, self->noise1, 255, ATTN_NORM);
	self->state = STATE_BOTTOM;
}

static void door_go_down(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, self->noise2, 255, ATTN_NORM);
	if (self->max_health) {
		self->takedamage = DAMAGE_YES;
		self->health     = self->max_health;
	}
	self->state = STATE_DOWN;
	sub_calc_move(self, self->pos1, self->speed, door_hit_bottom);
}

void movers_door_go_down(edict_t *self)
{
	door_go_down(self);
}

static void door_go_up(edict_t *self)
{
	if (self->state == STATE_UP)
		return;                                   /* already going up */
	if (self->state == STATE_TOP) {               /* reset top wait time */
		self->nextthink = self->ltime + TICKS(self->wait);
		ent_wake(self);
		return;
	}
	ent_sound(self, CHAN_VOICE, self->noise2, 255, ATTN_NORM);
	self->state = STATE_UP;
	sub_calc_move(self, self->pos2, self->speed, door_hit_top);
	sub_use_targets(self);
}

/* the master door: every door of its chain up (or down, a toggle's) */
static void door_fire(edict_t *self)
{
	if (self->owner != self)
		printf("ents: door_fire: self.owner != self\n");
	if (self->items)                              /* the key's sound */
		ent_sound(self, CHAN_ITEM, self->noise4, 255, ATTN_NORM);
	self->message = 0;                            /* no more message */
	self->msgtext = 0;
	edict_t *starte = self, *e = self;
	if (self->spawnflags & DOOR_TOGGLE) {
		if (self->state == STATE_UP || self->state == STATE_TOP) {
			do {
				door_go_down(e);
				e = e->enemy;
			} while (e && e != starte && e != ent_world);
			return;
		}
	}
	e = self;
	do {
		door_go_up(e);
		e = e->enemy;
	} while (e && e != starte && e != ent_world);
}

static void door_use(edict_t *self)
{
	self->message = 0;                            /* door messages are for touch only */
	if (self->owner) self->owner->message = 0;
	if (self->enemy) self->enemy->message = 0;
	if (self->owner)
		door_fire(self->owner);
}

static void door_trigger_touch(edict_t *self)
{
	if (ent_other->health <= 0)
		return;
	if (ent_time < self->attack_finished)
		return;
	self->attack_finished = ent_time + 60;
	ent_activator = ent_other;
	if (self->owner)
		door_use(self->owner);
}

static void door_killed(edict_t *self)
{
	edict_t *o = self->owner;
	o->health     = o->max_health;
	o->takedamage = DAMAGE_NO;                    /* will be reset upon return */
	door_use(o);
}

/* the messages, and the key doors */
static void door_touch(edict_t *self)
{
	if (ent_other != ent_player)
		return;
	edict_t *o = self->owner;
	if (o->attack_finished > ent_time)
		return;
	o->attack_finished = ent_time + 2 * 60;
	if (o->message) {
		ent_centerprint(ent_other, world_string(o->message));
		ent_sound(ent_other, CHAN_VOICE, snd_find("misc/talk.wav"), 255, ATTN_NORM);
	}
	if (!self->items)
		return;
	if ((self->items & pl.items) != self->items) {
		static const char *const need[2][3] = {
			{ "You need the silver key", "You need the silver runekey", "You need the silver keycard" },
			{ "You need the gold key", "You need the gold runekey", "You need the gold keycard" },
		};
		int wt = world.worldtype <= WORLDTYPE_BASE ? (int) world.worldtype : WORLDTYPE_MEDIEVAL;
		ent_centerprint(ent_other, need[o->items == IT_KEY1 ? 0 : 1][wt]);
		ent_sound(self, CHAN_VOICE, self->noise3, 255, ATTN_NORM);
		return;
	}
	pl.items &= ~self->items;
	self->touch = sub_null;
	if (self->enemy)
		self->enemy->touch = sub_null;            /* get paired door */
	door_use(self);
}

/* the fat trigger around a door chain that opens it */
static edict_t *spawn_field(edict_t *self, const int32_t fmins[3], const int32_t fmaxs[3])
{
	edict_t *trigger = ent_spawn();
	if (!trigger)
		return 0;
	trigger->movetype = MOVETYPE_NONE;
	trigger->solid    = SOLID_TRIGGER;
	trigger->owner    = self;
	trigger->touch    = door_trigger_touch;
	int32_t mn[3], mx[3];
	for (int k = 0; k < 3; k++) {
		int pad = (k == 2 ? 8 : 60) * CLIP_SUB;
		mn[k] = fmins[k] - pad;
		mx[k] = fmaxs[k] + pad;
	}
	ent_setsize(trigger, mn, mx);
	return trigger;
}

static int entities_touching(const edict_t *e1, const edict_t *e2)
{
	for (int k = 0; k < 3; k++) {
		if (e1->mins[k] > e2->maxs[k]) return 0;
		if (e1->maxs[k] < e2->mins[k]) return 0;
	}
	return 1;
}

/* LinkDoors: touching doors move together; the first is the master
   (owner), enemy chains them in a loop; the master gets a field unless it
   is shot, fired or locked */
Q_COLD static void link_doors(edict_t *self)
{
	if (self->enemy)
		return;                                   /* already linked by another door */
	if (self->spawnflags & DOOR_DONT_LINK) {
		self->owner = self->enemy = self;
		return;
	}
	int32_t cmins[3], cmaxs[3];
	__builtin_memcpy(cmins, self->mins, sizeof cmins);
	__builtin_memcpy(cmaxs, self->maxs, sizeof cmaxs);
	edict_t *starte = self, *t = self;
	for (;;) {
		self->owner = starte;                     /* master door */
		if (self->health) starte->health = self->health;
		if (self->targetname) starte->targetname = self->targetname;
		if (self->message) starte->message = self->message;
		t = ent_find_class(t, QE_FUNC_DOOR);
		if (!t) {
			self->enemy = starte;                 /* make the chain a loop */
			self = self->owner;
			if (self->health || self->targetname || self->items)
				return;                           /* shot, fired or key doors: no field */
			self->owner->trigger_field = spawn_field(self, cmins, cmaxs);
			return;
		}
		if (entities_touching(self, t)) {
			if (t->enemy)
				printf("ents: cross connected doors\n");
			self->enemy = t;
			self = t;
			for (int k = 0; k < 3; k++) {
				if (t->mins[k] < cmins[k]) cmins[k] = t->mins[k];
				if (t->maxs[k] > cmaxs[k]) cmaxs[k] = t->maxs[k];
			}
		}
	}
}

static void func_door(edict_t *self)
{
	if (world.worldtype == WORLDTYPE_MEDIEVAL) {
		sound_of(self, &self->noise3, "doors/medtry.wav");
		sound_of(self, &self->noise4, "doors/meduse.wav");
	} else if (world.worldtype == WORLDTYPE_METAL) {
		sound_of(self, &self->noise3, "doors/runetry.wav");
		sound_of(self, &self->noise4, "doors/runeuse.wav");
	} else if (world.worldtype == WORLDTYPE_BASE) {
		sound_of(self, &self->noise3, "doors/basetry.wav");
		sound_of(self, &self->noise4, "doors/baseuse.wav");
	}
	switch (self->sounds) {
	case 0: self->noise1 = self->noise2 = SND_NULL; break;
	case 1: sound_of(self, &self->noise1, "doors/drclos4.wav"); sound_of(self, &self->noise2, "doors/doormv1.wav"); break;
	case 2: sound_of(self, &self->noise2, "doors/hydro1.wav");  sound_of(self, &self->noise1, "doors/hydro2.wav"); break;
	case 3: sound_of(self, &self->noise2, "doors/stndr1.wav");  sound_of(self, &self->noise1, "doors/stndr2.wav"); break;
	case 4: sound_of(self, &self->noise1, "doors/ddoor2.wav");  sound_of(self, &self->noise2, "doors/ddoor1.wav"); break;
	}
	set_movedir(self);
	self->max_health = self->health;
	self->solid      = SOLID_BSP;
	self->movetype   = MOVETYPE_PUSH;
	self->draw       = 1;
	ent_setmodel(self, self->model);
	self->blocked = door_blocked;
	self->use     = door_use;
	if (self->spawnflags & DOOR_SILVER_KEY) self->items = IT_KEY1;
	if (self->spawnflags & DOOR_GOLD_KEY)   self->items = IT_KEY2;
	if (!self->speed) self->speed = 100;
	if (!self->wait)  self->wait = 300;
	if (!self->lip)   self->lip = 8;
	if (!self->dmg)   self->dmg = 2;
	__builtin_memcpy(self->pos1, self->origin, sizeof self->pos1);
	travel_end(self);
	/* DOOR_START_OPEN: lit closed, spawned open, working the other way */
	if (self->spawnflags & DOOR_START_OPEN) {
		ent_setorigin(self, self->pos2);
		__builtin_memcpy(self->pos2, self->pos1, sizeof self->pos2);
		__builtin_memcpy(self->pos1, self->origin, sizeof self->pos1);
	}
	self->state = STATE_BOTTOM;
	if (self->health) {
		self->takedamage = DAMAGE_YES;
		self->th_die     = door_killed;
	}
	if (self->items)
		self->wait = -100;
	self->touch = door_touch;
	/* LinkDoors can't be done until all of the doors have been spawned */
	self->think     = link_doors;
	self->nextthink = self->ltime + TICKS(10);
	ent_wake(self);
}

/* ------------------------------------------------------------ secret doors -- */

static void fd_secret_move1(edict_t *self);
static void fd_secret_move2(edict_t *self);
static void fd_secret_move3(edict_t *self);
static void fd_secret_move4(edict_t *self);
static void fd_secret_move5(edict_t *self);
static void fd_secret_move6(edict_t *self);
static void fd_secret_done(edict_t *self);

/* makevectors (the secret door's angles: its yaw): forward, right, up */
static void secret_vectors(const edict_t *self, int32_t f[3], int32_t r[3], int32_t u[3])
{
	int y = self->mangle[1] * ANG_TURN / 360;
	int32_t sy = r_sin(y), cy = r_cos(y);
	f[0] = cy; f[1] = sy; f[2] = 0;
	r[0] = sy; r[1] = -cy; r[2] = 0;
	u[0] = 0;  u[1] = 0;   u[2] = 4096;
}

static int32_t fabs_dot_size(const int32_t v[3], const edict_t *self)   /* 1/8 units */
{
	int32_t d = 0;
	for (int k = 0; k < 3; k++)
		d += v[k] * (self->maxs[k] - self->mins[k]);   /* (under 2^30) */
	return (d < 0 ? -d : d) / 4096;
}

/* fd_secret_use's two moves' ends: aside (or down), then back */
static void secret_dests(const edict_t *self, int32_t dest1[3], int32_t dest2[3])
{
	int temp = self->spawnflags & SECRET_1ST_LEFT ? -1 : 1;
	int32_t f[3], r[3], u[3];
	secret_vectors(self, f, r, u);
	int32_t width  = self->t_width * CLIP_SUB, length = self->t_length * CLIP_SUB;
	if (!width)
		width = fabs_dot_size(self->spawnflags & SECRET_1ST_DOWN ? u : r, self);
	if (!length)
		length = fabs_dot_size(f, self);
	for (int k = 0; k < 3; k++) {
		if (self->spawnflags & SECRET_1ST_DOWN)
			dest1[k] = self->origin[k] - u[k] * width / 4096;
		else
			dest1[k] = self->origin[k] + r[k] * width * temp / 4096;
		dest2[k] = dest1[k] + f[k] * length / 4096;
	}
}

static void fd_secret_use(edict_t *self)
{
	self->health = 10000;
	if (memcmp(self->origin, self->oldorigin, sizeof self->origin))
		return;                                   /* still moving around */
	self->message = 0;
	sub_use_targets(self);                        /* fire all targets / killtargets */
	if (!(self->spawnflags & SECRET_NO_SHOOT)) {
		self->th_pain    = sub_null;
		self->takedamage = DAMAGE_NO;
	}
	self->velocity[0] = self->velocity[1] = self->velocity[2] = 0;
	ent_sound(self, CHAN_VOICE, self->noise1, 255, ATTN_NORM);   /* a sound, a little wait */
	self->nextthink = self->ltime + TICKS(10);
	secret_dests(self, self->dest1, self->dest2);
	sub_calc_move(self, self->dest1, self->speed, fd_secret_move1);
	ent_sound(self, CHAN_VOICE, self->noise2, 255, ATTN_NORM);
}

static void fd_secret_move1(edict_t *self)     /* wait after the first move */
{
	self->nextthink = self->ltime + 60;
	self->think     = fd_secret_move2;
	ent_wake(self);
	ent_sound(self, CHAN_VOICE, self->noise3, 255, ATTN_NORM);
}

static void fd_secret_move2(edict_t *self)     /* sideways */
{
	ent_sound(self, CHAN_VOICE, self->noise2, 255, ATTN_NORM);
	sub_calc_move(self, self->dest2, self->speed, fd_secret_move3);
}

static void fd_secret_move3(edict_t *self)     /* wait there until time to go back */
{
	ent_sound(self, CHAN_VOICE, self->noise3, 255, ATTN_NORM);
	if (!(self->spawnflags & SECRET_OPEN_ONCE)) {
		self->nextthink = self->ltime + TICKS(self->wait);
		self->think     = fd_secret_move4;
		ent_wake(self);
	}
}

static void fd_secret_move4(edict_t *self)     /* backward */
{
	ent_sound(self, CHAN_VOICE, self->noise2, 255, ATTN_NORM);
	sub_calc_move(self, self->dest1, self->speed, fd_secret_move5);
}

static void fd_secret_move5(edict_t *self)     /* wait a second */
{
	self->nextthink = self->ltime + 60;
	self->think     = fd_secret_move6;
	ent_wake(self);
	ent_sound(self, CHAN_VOICE, self->noise3, 255, ATTN_NORM);
}

static void fd_secret_move6(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, self->noise2, 255, ATTN_NORM);
	sub_calc_move(self, self->oldorigin, self->speed, fd_secret_done);
}

static void fd_secret_done(edict_t *self)
{
	if (!self->targetname || (self->spawnflags & SECRET_YES_SHOOT)) {
		self->health     = 10000;
		self->takedamage = DAMAGE_YES;
		self->th_pain    = fd_secret_use;
		self->th_die     = fd_secret_use;
	}
	ent_sound(self, CHAN_VOICE, self->noise3, 255, ATTN_NORM);
}

static void secret_blocked(edict_t *self)
{
	if (ent_time < self->attack_finished)
		return;
	self->attack_finished = ent_time + 30;
	ent_damage(ent_other, self, self, self->dmg);
}

static void secret_touch(edict_t *self)
{
	if (ent_other != ent_player)
		return;
	if (self->attack_finished > ent_time)
		return;
	self->attack_finished = ent_time + 2 * 60;
	if (self->message) {
		ent_centerprint(ent_other, world_string(self->message));
		ent_sound(ent_other, CHAN_BODY, snd_find("misc/talk.wav"), 255, ATTN_NORM);
	}
}

static void func_door_secret(edict_t *self)
{
	if (!self->sounds)
		self->sounds = 3;
	if (self->sounds == 1) {
		sound_of(self, &self->noise1, "doors/latch2.wav");
		sound_of(self, &self->noise2, "doors/winch2.wav");
		sound_of(self, &self->noise3, "doors/drclos4.wav");
	} else if (self->sounds == 2) {
		sound_of(self, &self->noise2, "doors/airdoor1.wav");
		sound_of(self, &self->noise1, "doors/airdoor2.wav");
		sound_of(self, &self->noise3, "doors/airdoor2.wav");
	} else if (self->sounds == 3) {
		sound_of(self, &self->noise2, "doors/basesec1.wav");
		sound_of(self, &self->noise1, "doors/basesec2.wav");
		sound_of(self, &self->noise3, "doors/basesec2.wav");
	}
	if (!self->dmg)
		self->dmg = 2;
	self->mangle[0] = 0; self->mangle[1] = self->angle; self->mangle[2] = 0;   /* mangle = angles */
	self->angle    = 0;
	self->solid    = SOLID_BSP;
	self->movetype = MOVETYPE_PUSH;
	self->draw     = 1;
	ent_setmodel(self, self->model);
	self->touch   = secret_touch;
	self->blocked = secret_blocked;
	self->speed   = 50;
	self->use     = fd_secret_use;
	if (!self->targetname || (self->spawnflags & SECRET_YES_SHOOT)) {
		self->health     = 10000;
		self->takedamage = DAMAGE_YES;
		self->th_pain    = fd_secret_use;
		self->th_die     = fd_secret_use;
	}
	__builtin_memcpy(self->oldorigin, self->origin, sizeof self->oldorigin);
	if (!self->wait)
		self->wait = 500;                         /* 5 seconds before closing */
}

/* ------------------------------------------------------------------ plats -- */

static void plat_go_down(edict_t *self);
static void plat_go_up(edict_t *self);

static void plat_center_touch(edict_t *self)
{
	if (ent_other != ent_player || ent_other->health <= 0)
		return;
	edict_t *p = self->enemy;
	if (p->state == STATE_BOTTOM)
		plat_go_up(p);
	else if (p->state == STATE_TOP) {
		p->nextthink = p->ltime + 60;             /* delay going down */
		ent_wake(p);
	}
}

static void plat_spawn_inside_trigger(edict_t *self)
{
	edict_t *trigger = ent_spawn();
	if (!trigger)
		return;
	trigger->touch    = plat_center_touch;
	trigger->flags   |= FL_TOUCH_PLAYER;
	trigger->movetype = MOVETYPE_NONE;
	trigger->solid    = SOLID_TRIGGER;
	trigger->enemy    = self;
	int32_t tmin[3], tmax[3];
	tmin[0] = self->mins[0] + 25 * CLIP_SUB; tmin[1] = self->mins[1] + 25 * CLIP_SUB; tmin[2] = self->mins[2];
	tmax[0] = self->maxs[0] - 25 * CLIP_SUB; tmax[1] = self->maxs[1] - 25 * CLIP_SUB; tmax[2] = self->maxs[2] + 8 * CLIP_SUB;
	tmin[2] = tmax[2] - (self->pos1[2] - self->pos2[2] + 8 * CLIP_SUB);
	if (self->spawnflags & PLAT_LOW_TRIGGER)
		tmax[2] = tmin[2] + 8 * CLIP_SUB;
	if (self->maxs[0] - self->mins[0] <= 50 * CLIP_SUB) {
		tmin[0] = (self->mins[0] + self->maxs[0]) / 2;
		tmax[0] = tmin[0] + CLIP_SUB;
	}
	if (self->maxs[1] - self->mins[1] <= 50 * CLIP_SUB) {
		tmin[1] = (self->mins[1] + self->maxs[1]) / 2;
		tmax[1] = tmin[1] + CLIP_SUB;
	}
	ent_setsize(trigger, tmin, tmax);
}

static void plat_hit_top(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, self->noise1, 255, ATTN_NORM);
	self->state     = STATE_TOP;
	self->think     = plat_go_down;
	self->nextthink = self->ltime + 3 * 60;
	ent_wake(self);
}

static void plat_hit_bottom(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, self->noise1, 255, ATTN_NORM);
	self->state = STATE_BOTTOM;
}

static void plat_go_down(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, self->noise, 255, ATTN_NORM);
	self->state = STATE_DOWN;
	sub_calc_move(self, self->pos2, self->speed, plat_hit_bottom);
}

static void plat_go_up(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, self->noise, 255, ATTN_NORM);
	self->state = STATE_UP;
	sub_calc_move(self, self->pos1, self->speed, plat_hit_top);
}

static void plat_trigger_use(edict_t *self)
{
	if (self->think)
		return;                                   /* already activated */
	plat_go_down(self);
}

static void plat_crush(edict_t *self)
{
	ent_damage(ent_other, self, self, 1);
	if (self->state == STATE_UP)
		plat_go_down(self);
	else if (self->state == STATE_DOWN)
		plat_go_up(self);
	else
		printf("ents: plat_crush: bad self.state\n");
}

static void plat_use(edict_t *self)
{
	self->use = sub_null;
	if (self->state != STATE_UP)
		printf("ents: plat_use: not in up state\n");
	plat_go_down(self);
}

static void func_plat(edict_t *self)
{
	if (!self->t_length) self->t_length = 80;
	if (!self->t_width)  self->t_width = 10;
	if (!self->sounds)   self->sounds = 2;
	if (self->sounds == 1) {
		sound_of(self, &self->noise, "plats/plat1.wav");
		sound_of(self, &self->noise1, "plats/plat2.wav");
	}
	if (self->sounds == 2) {
		sound_of(self, &self->noise, "plats/medplat1.wav");
		sound_of(self, &self->noise1, "plats/medplat2.wav");
	}
	self->mangle[1] = self->angle;
	self->angle     = 0;
	self->solid     = SOLID_BSP;
	self->movetype  = MOVETYPE_PUSH;
	self->draw      = 1;
	ent_setmodel(self, self->model);
	self->blocked = plat_crush;
	if (!self->speed)
		self->speed = 150;
	/* pos1 is the top position, pos2 the bottom */
	__builtin_memcpy(self->pos1, self->origin, sizeof self->pos1);
	__builtin_memcpy(self->pos2, self->origin, sizeof self->pos2);
	if (self->height)
		self->pos2[2] = self->origin[2] - self->height * CLIP_SUB;
	else
		self->pos2[2] = self->origin[2] - (self->maxs[2] - self->mins[2]) + 8 * CLIP_SUB;
	self->use = plat_trigger_use;
	plat_spawn_inside_trigger(self);              /* the "start moving" trigger */
	if (self->targetname) {
		self->state = STATE_UP;
		self->use   = plat_use;
	} else {
		ent_setorigin(self, self->pos2);
		self->state = STATE_BOTTOM;
	}
}

/* ----------------------------------------------------------------- trains -- */

static void train_next(edict_t *self);
static void func_train_find(edict_t *self);

static void train_blocked(edict_t *self)
{
	if (ent_time < self->attack_finished)
		return;
	self->attack_finished = ent_time + 30;
	ent_damage(ent_other, self, self, self->dmg);
}

static void train_use(edict_t *self)
{
	if (self->think != func_train_find)
		return;                                   /* already activated */
	train_next(self);
}

static void train_wait(edict_t *self)
{
	if (self->wait) {
		self->nextthink = self->ltime + TICKS(self->wait);
		ent_sound(self, CHAN_VOICE, self->noise, 255, ATTN_NORM);
	} else {
		self->nextthink = self->ltime + TICKS(10);
	}
	self->think = train_next;
	ent_wake(self);
}

static void train_next(edict_t *self)
{
	edict_t *targ = ent_find(0, self->target);
	if (!targ) {
		printf("ents: train_next: no next target\n");
		return;
	}
	self->target = targ->target;
	if (!self->target)
		printf("ents: train_next: no next target\n");
	self->wait = targ->wait ? targ->wait : 0;
	ent_sound(self, CHAN_VOICE, self->noise1, 255, ATTN_NORM);
	int32_t dest[3];
	for (int k = 0; k < 3; k++)
		dest[k] = targ->origin[k] - self->mins[k];
	sub_calc_move(self, dest, self->speed, train_wait);
}

Q_COLD static void func_train_find(edict_t *self)
{
	edict_t *targ = ent_find(0, self->target);
	if (!targ) {
		printf("ents: train without its first corner\n");
		return;
	}
	self->target = targ->target;
	int32_t org[3];
	for (int k = 0; k < 3; k++)
		org[k] = targ->origin[k] - self->mins[k];
	ent_setorigin(self, org);
	if (!self->targetname) {                      /* not triggered: start at once */
		self->nextthink = self->ltime + TICKS(10);
		self->think     = train_next;
		ent_wake(self);
	}
}

#ifdef Q1PSX_BENCH
/* The places a train's or a secret door's model goes through, for the
   check of its pieces' room (engine/bmfrag.c bmf_check, BMFCHK=1; the
   room is the cook's: tools/qcook/bmf.c follows the same travel) and the
   movers' scenario's spot (engine/main.c bench_movers, MOVERS=1): a
   train's corners in its path's order (func_train_find's and train_next's:
   a corner's origin less the model's mins), up to the first corner it
   comes back to (that one again: the way back to it), a secret door's
   place and its two moves' ends. Room in out for max + 1 places, in seen
   (the corners on the way) for max. The count, 0 for anything else, -1
   for a path of more than max corners. */
int movers_path(const edict_t *e, int32_t (*out)[3], const edict_t **seen, int max)
{
	if (e->class == QE_FUNC_DOOR_SECRET) {
		if (max < 3)
			return 0;
		memcpy(out[0], e->origin, sizeof out[0]);
		secret_dests(e, out[1], out[2]);
		return 3;
	}
	if (e->class != QE_FUNC_TRAIN)
		return 0;
	int n = 0;
	for (uint16_t name = e->target; name; ) {
		const edict_t *targ = ent_find(0, name);
		if (!targ)
			break;
		int back = 0;
		for (int j = 0; j < n && !back; j++)
			back = seen[j] == targ;
		if (!back && n == max)
			return -1;
		for (int k = 0; k < 3; k++)
			out[n][k] = targ->origin[k] - e->mins[k];
		if (back)                                 /* looped: its way back to that corner */
			return n + 1;
		seen[n++] = targ;
		name = targ->target;
	}
	return n;
}
#endif

static void func_train(edict_t *self, int teleporttrain)
{
	if (!self->speed)
		self->speed = 100;
	if (!self->target)
		printf("ents: func_train without a target\n");
	if (!self->dmg)
		self->dmg = 2;
	if (self->sounds == 1 && !teleporttrain) {
		sound_of(self, &self->noise, "plats/train2.wav");    /* stop sound */
		sound_of(self, &self->noise1, "plats/train1.wav");   /* move sound */
	} else {
		self->noise = self->noise1 = SND_NULL;
	}
	self->cnt      = 1;
	self->solid    = teleporttrain ? SOLID_NOT : SOLID_BSP;
	self->movetype = MOVETYPE_PUSH;
	self->blocked  = train_blocked;
	self->use      = train_use;
	if (!teleporttrain) {
		self->draw = 1;
		ent_setmodel(self, self->model);
	} else {                                      /* setmodel (progs/teleport.mdl): an alias
		                                                 model's +-16 box (Mod_LoadAliasModel); its
		                                                 avelocity turns nothing under SV_PushMove */
		static const int32_t box[3] = { -16 * CLIP_SUB, -16 * CLIP_SUB, -16 * CLIP_SUB };
		static const int32_t box2[3] = { 16 * CLIP_SUB, 16 * CLIP_SUB, 16 * CLIP_SUB };
		ent_setsize(self, box, box2);
		ent_set_qim(self, QIM_TELEPORT);
	}
	/* trains start on the second frame: their targets must have spawned */
	self->nextthink = self->ltime + TICKS(10);
	self->think     = func_train_find;
	ent_wake(self);
}

/* ----------------------------------------------------------------- buttons -- */

static void button_return(edict_t *self);

static void button_wait(edict_t *self)
{
	self->state     = STATE_TOP;
	self->nextthink = self->ltime + TICKS(self->wait);
	self->think     = button_return;
	ent_wake(self);
	ent_activator   = self->enemy;
	sub_use_targets(self);
	self->frame     = 1;                          /* use alternate textures */
}

static void button_done(edict_t *self)
{
	self->state = STATE_BOTTOM;
}

static void button_return(edict_t *self)
{
	self->state = STATE_DOWN;
	sub_calc_move(self, self->pos1, self->speed, button_done);
	self->frame = 0;                              /* use normal textures */
	if (self->health)
		self->takedamage = DAMAGE_YES;            /* can be shot again */
}

static void button_blocked(edict_t *self)
{
	(void) self;                                  /* just don't come all the way back out */
}

static void button_fire(edict_t *self)
{
	if (self->state == STATE_UP || self->state == STATE_TOP)
		return;
	ent_sound(self, CHAN_VOICE, self->noise, 255, ATTN_NORM);
	self->state = STATE_UP;
	sub_calc_move(self, self->pos2, self->speed, button_wait);
}

static void button_use(edict_t *self)
{
	self->enemy = ent_activator;
	button_fire(self);
}

static void button_touch(edict_t *self)
{
	if (ent_other != ent_player)
		return;
	self->enemy = ent_other;
	button_fire(self);
}

static void button_killed(edict_t *self)
{
	self->enemy      = ent_damage_attacker;
	self->health     = self->max_health;
	self->takedamage = DAMAGE_NO;                 /* will be reset upon return */
	button_fire(self);
}

static void func_button(edict_t *self)
{
	static const char *const noise[4] = { "buttons/airbut1.wav", "buttons/switch21.wav",
	                                      "buttons/switch02.wav", "buttons/switch04.wav" };
	if (self->sounds <= 3)
		sound_of(self, &self->noise, noise[self->sounds]);
	set_movedir(self);
	self->movetype = MOVETYPE_PUSH;
	self->solid    = SOLID_BSP;
	self->draw     = 1;
	ent_setmodel(self, self->model);
	self->blocked = button_blocked;
	self->use     = button_use;
	if (self->health) {
		self->max_health = self->health;
		self->th_die     = button_killed;
		self->takedamage = DAMAGE_YES;
	} else {
		self->touch = button_touch;
	}
	if (!self->speed) self->speed = 40;
	if (!self->wait)  self->wait = 100;
	if (!self->lip)   self->lip = 4;
	self->state = STATE_BOTTOM;
	__builtin_memcpy(self->pos1, self->origin, sizeof self->pos1);
	travel_end(self);
}

Q_COLD void movers_spawn(edict_t *e, const qw_ent *q)
{
	(void) q;
	switch (e->class) {
	case QE_FUNC_DOOR:          func_door(e); break;
	case QE_FUNC_DOOR_SECRET:   func_door_secret(e); break;
	case QE_FUNC_PLAT:          func_plat(e); break;
	case QE_FUNC_TRAIN:         func_train(e, 0); break;
	case QE_MISC_TELEPORTTRAIN: func_train(e, 1); break;
	default:                    func_button(e); break;
	}
}
