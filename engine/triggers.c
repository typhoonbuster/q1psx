/*
 * q1psx — triggers.qc and misc.qc's map entities, transcribed: the multi
 * triggers (once, multiple, secret), relay, counter, teleporters and their
 * destinations, push, hurt, setskill, onlyregistered, monsterjump (its box
 * only: the monsters' stage), func_wall / illusionary / episodegate /
 * bossgate, air_bubbles, the spots (info_*, path_corner) and the lights.
 * Strings are id's (the 1.06 QC's, reference/Quake/qw-qc).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ents.h"
#include "game.h"
#include "level.h"
#include "mem.h"
#include "monsters.h"
#include "render.h"
#include "snd.h"
#include "world.h"

#define SPAWNFLAG_NOMESSAGE 1
#define SPAWNFLAG_NOTOUCH   1
#define PLAYER_ONLY         1
#define SILENT              2
#define PUSH_ONCE           1
#define DEG(d) ((d) * ANG_TURN / 360)

/* makevectors (angles in degrees): v_forward x4096 */
static void forward_of(int pitch_deg, int yaw_deg, int32_t f[3])
{
	int p = DEG(pitch_deg), y = DEG(yaw_deg);
	int32_t cp = r_cos(p), sp = r_sin(p);
	f[0] = cp * r_cos(y) >> 12;
	f[1] = cp * r_sin(y) >> 12;
	f[2] = -sp;
}

/* ------------------------------------------------------- multi triggers -- */

/* the wait time has passed, so set back up for another activation */
static void multi_wait(edict_t *self)
{
	if (self->max_health) {
		self->health     = self->max_health;
		self->takedamage = DAMAGE_YES;
		self->solid      = SOLID_BBOX;
		ent_link(self);
	}
}

/* the trigger was just touched / killed / used: self->enemy the activator */
static void multi_trigger(edict_t *self)
{
	if (self->nextthink > ent_time)
		return;                                   /* already been triggered */
	if (self->class == QE_TRIGGER_SECRET) {
		if (self->enemy != ent_player)
			return;
		ent_found_secrets++;                      /* SVC_FOUNDSECRET */
	}
	if (self->noise != SND_NONE)
		ent_sound(self, CHAN_VOICE, self->noise, 255, ATTN_NORM);
	self->takedamage = DAMAGE_NO;                 /* don't trigger again until reset */
	ent_activator = self->enemy;
	sub_use_targets(self);
	if (self->wait > 0) {
		self->think     = multi_wait;
		self->nextthink = ent_time + TICKS(self->wait);
	} else {                                      /* a touch: removed a little later */
		self->touch     = sub_null;
		self->nextthink = ent_time + TICKS(10);
		self->think     = sub_remove;
	}
	ent_wake(self);
}

static void multi_killed(edict_t *self)
{
	self->enemy = ent_damage_attacker;
	multi_trigger(self);
}

static void multi_use(edict_t *self)
{
	self->enemy = ent_activator;
	multi_trigger(self);
}

static void multi_touch(edict_t *self)
{
	if (ent_other != ent_player)
		return;
	/* a one-way trigger: the player must face its direction (the player
	   entity's angles: the view's yaw, a third of its pitch, negated) */
	if (self->movedir[0] | self->movedir[1] | self->movedir[2]) {
		int32_t f[3];
		int p = -pl.pm.pitch / 3, y = pl.pm.yaw;
		int32_t cp = r_cos(p);
		f[0] = cp * r_cos(y) >> 12;
		f[1] = cp * r_sin(y) >> 12;
		f[2] = -r_sin(p);
		if (f[0] * self->movedir[0] + f[1] * self->movedir[1] + f[2] * self->movedir[2] < 0)
			return;                               /* not facing the right way */
	}
	self->enemy = ent_other;
	multi_trigger(self);
}

static void trigger_multiple(edict_t *self)
{
	if (self->sounds == 1)      self->noise = (int16_t) snd_find("misc/secret.wav");
	else if (self->sounds == 2) self->noise = (int16_t) snd_find("misc/talk.wav");
	else if (self->sounds == 3) self->noise = (int16_t) snd_find("misc/trigger1.wav");
	if (!self->wait)
		self->wait = 20;                          /* 0.2 s */
	self->use = multi_use;
	init_trigger(self);
	if (self->health) {
		if (self->spawnflags & SPAWNFLAG_NOTOUCH)
			printf("ents: trigger with health and notouch\n");
		self->max_health = self->health;
		self->th_die     = multi_killed;
		self->takedamage = DAMAGE_YES;
		self->solid      = SOLID_BBOX;            /* shot, not touched (a box in the traces) */
		ent_setmodel(self, self->model);
	} else if (!(self->spawnflags & SPAWNFLAG_NOTOUCH)) {
		self->touch  = multi_touch;
		self->flags |= FL_TOUCH_PLAYER;
	}
}

/* -------------------------------------------------------- relay, counter -- */

static void counter_use(edict_t *self)
{
	self->count--;
	if (self->count < 0)
		return;
	if (self->count != 0) {
		if (ent_activator == ent_player && !(self->spawnflags & SPAWNFLAG_NOMESSAGE)) {
			if (self->count >= 4)      ent_centerprint(ent_activator, "There are more to go...");
			else if (self->count == 3) ent_centerprint(ent_activator, "Only 3 more to go...");
			else if (self->count == 2) ent_centerprint(ent_activator, "Only 2 more to go...");
			else                       ent_centerprint(ent_activator, "Only 1 more to go...");
		}
		return;
	}
	if (ent_activator == ent_player && !(self->spawnflags & SPAWNFLAG_NOMESSAGE))
		ent_centerprint(ent_activator, "Sequence completed!");
	self->enemy = ent_activator;
	multi_trigger(self);
}

/* ----------------------------------------------------------- teleports -- */

static void play_teleport(edict_t *self)
{
	int v = (int) ((q_rand() & 0x7fff) * 5 / 0x8000);   /* random () * 5 */
	int32_t org[3];
	__builtin_memcpy(org, self->origin, sizeof org);
	snd_start(ENT_EDICT + (int) (self - edicts), CHAN_VOICE, S_R_TELE1 + v, org, 255, ATTN_NORM);
	ent_remove(self);
}

/* the fog: the flash's particles now, its sound 0.2 s later (TE_TELEPORT) */
static void spawn_tfog(const int32_t org[3])
{
	edict_t *s = ent_spawn();
	if (s) {
		__builtin_memcpy(s->origin, org, sizeof s->origin);
		s->nextthink = ent_time + TICKS(20);
		s->think     = play_teleport;
		ent_wake(s);
	}
	game_teleport_splash(org);
}

/* tdeath_touch: what stands where the teleported one arrives dies (the
   player there kills a monster arriving). The QC's multiplayer cases are
   gone: a player arriving on the player (the owner is never the player
   when the player touches it: the test above), and the classname
   teledeath2 for an invincible one (read by the obituaries and a player's
   gib sound only) */
static void tdeath_touch(edict_t *self)
{
	edict_t *other = ent_other;
	if (other == self->owner)
		return;
	if (other == ent_player) {                    /* other monsters explode themselves */
		ent_damage(self->owner, self, self, 50000);
		return;
	}
	if (other->health)
		ent_damage(other, self, self, 50000);
}

/* spawn_tdeath: a trigger of the arriving one's size (a unit wider) at
   its destination for 0.2 s, everything made to touch it again */
static void spawn_tdeath(const int32_t org[3], edict_t *death_owner)
{
	edict_t *death = ent_spawn();
	if (!death)
		return;
	death->movetype = MOVETYPE_NONE;
	death->solid    = SOLID_TRIGGER;
	int32_t mins[3], maxs[3];
	for (int k = 0; k < 3; k++) {
		mins[k] = death_owner->mins[k] - CLIP_SUB;
		maxs[k] = death_owner->maxs[k] + CLIP_SUB;
	}
	ent_setsize(death, mins, maxs);
	ent_setorigin(death, org);
	death->touch     = tdeath_touch;
	death->nextthink = ent_time + TICKS(20);
	death->think     = sub_remove;
	death->owner     = death_owner;
	ent_wake(death);
	ent_force_retouch = 2;                        /* make sure even still objects get hit */
}

/* teleport_touch: a living creature (the player, a monster) to the
   target, a fog at both ends, a telefrag at the arrival */
static void teleport_touch(edict_t *self)
{
	edict_t *other = ent_other;
	if (self->targetname && self->nextthink < ent_time)
		return;                                   /* not fired yet */
	if ((self->spawnflags & PLAYER_ONLY) && other != ent_player)
		return;
	if (other->health <= 0 || other->solid != SOLID_SLIDEBOX)
		return;                                   /* only living creatures */
	sub_use_targets(self);
	spawn_tfog(other->origin);                    /* a fog where the player was */
	edict_t *t = ent_find(0, self->target);
	if (!t) {
		printf("ents: teleporter: couldn't find target %s\n", world_string(self->target));
		return;
	}
	int32_t f[3], org[3];
	forward_of(t->mangle[0], t->mangle[1], f);
	for (int k = 0; k < 3; k++)
		org[k] = t->origin[k] + 32 * f[k] * CLIP_SUB / 4096;
	spawn_tfog(org);                              /* a fog in front of the destination */
	spawn_tdeath(t->origin, other);
	if (other == ent_player) {                    /* move the player and lock him down */
		game_teleport(t->origin, t->mangle);
		return;
	}
	ent_setorigin(other, t->origin);
	for (int k = 0; k < 3; k++)                   /* other.angles = t.mangle */
		other->ang[k] = (uint16_t) ((t->mangle[k] % 360 + 360) % 360 * 65536 / 360);
	other->flags &= ~FL_ONGROUND;
}

/* trigger_monsterjump_touch: a walking monster thrown over a gap */
static void trigger_monsterjump_touch(edict_t *self)
{
	edict_t *o = ent_other;
	if ((o->flags & (FL_MONSTER | FL_FLY | FL_SWIM)) != FL_MONSTER)
		return;
	/* set XY even if not on ground, so the jump will clear lips */
	o->velocity[0] = self->movedir[0] * self->speed / 4096;
	o->velocity[1] = self->movedir[1] * self->speed / 4096;
	if (!(o->flags & FL_ONGROUND))
		return;
	o->flags &= ~FL_ONGROUND;
	o->velocity[2] = self->height;
}

static void teleport_use(edict_t *self)
{
	self->nextthink = ent_time + TICKS(20);
	ent_force_retouch = 2;                        /* make sure even still objects get hit */
	self->think = sub_null;
	ent_wake(self);
}

/* ---------------------------------------------------------------- misc -- */

static void trigger_skill_touch(edict_t *self)
{
	if (ent_other != ent_player)
		return;
	const char *m = world_string(self->message);   /* cvar_set ("skill", message) */
	int v = 0;
	while (*m >= '0' && *m <= '9')
		v = v * 10 + (*m++ - '0');
	game_skill = v;
}

static void trigger_onlyregistered_touch(edict_t *self)
{
	if (ent_other != ent_player)
		return;
	if (self->attack_finished > ent_time)
		return;
	self->attack_finished = ent_time + 2 * 60;
	if (world_registered()) {
		self->message = 0;
		sub_use_targets(self);
		ent_remove(self);
	} else if (self->message) {
		ent_centerprint(ent_other, world_string(self->message));
		ent_sound(ent_other, CHAN_BODY, snd_find("misc/talk.wav"), 255, ATTN_NORM);
	}
}

static void hurt_on(edict_t *self)
{
	self->solid     = SOLID_TRIGGER;
	self->nextthink = -1;
}

static void hurt_touch(edict_t *self)
{
	if (ent_other->takedamage) {
		self->solid = SOLID_NOT;
		ent_damage(ent_other, self, self, self->dmg);
		self->think     = hurt_on;
		self->nextthink = ent_time + 60;
		ent_wake(self);
	}
}

/* trigger_push_touch: a living creature thrown (the QC's grenades too:
   the player's are the game's projectiles, which touch no trigger) */
static void trigger_push_touch(edict_t *self)
{
	if (ent_other->health > 0 && ent_other == ent_player) {
		for (int k = 0; k < 3; k++)
			pl.pm.vel[k] = self->speed * self->movedir[k] * 10 / 4096;
		pl.pm.onground = 0;
		if (pl.fly_sound < game_tick_count) {
			pl.fly_sound = game_tick_count + TICKS(150);
			snd_start(ENT_PLAYER, CHAN_AUTO, S_WINDFLY, pl.pm.pos, 255, ATTN_NORM);
		}
	} else if (ent_other->health > 0) {
		for (int k = 0; k < 3; k++)
			ent_other->velocity[k] = self->speed * self->movedir[k] * 10 / 4096;
	}
	if (self->spawnflags & PUSH_ONCE)
		ent_remove(self);
}

static void func_wall_use(edict_t *self)
{
	self->frame = (uint8_t) (1 - self->frame);   /* the alternate textures */
}

static void make_bubbles(edict_t *self)
{
	game_spawn_bubble(self->origin);
	self->nextthink = ent_time + 30 + (int) ((q_rand() & 0x7fff) * 60 / 0x8000);   /* random () + 0.5 */
	self->think     = make_bubbles;
	ent_wake(self);
}

/* a brush entity drawn as it is: the model, solid or not */
static void brush_static(edict_t *self, int solid)
{
	self->movetype = solid ? MOVETYPE_PUSH : MOVETYPE_NONE;
	self->solid    = solid ? SOLID_BSP : SOLID_NOT;
	self->draw     = 1;
	if (self->model)
		ent_setmodel(self, self->model);
}

Q_COLD void triggers_spawn(edict_t *e, const qw_ent *q)
{
	(void) q;
	switch (e->class) {
	case QE_TRIGGER_MULTIPLE:
		trigger_multiple(e);
		break;
	case QE_TRIGGER_ONCE:
		e->wait = -100;
		trigger_multiple(e);
		break;
	case QE_TRIGGER_SECRET:
		ent_total_secrets++;
		e->wait = -100;
		if (!e->message)
			e->msgtext = "You found a secret area!";
		if (!e->sounds)
			e->sounds = 1;
		trigger_multiple(e);
		break;
	case QE_TRIGGER_RELAY:
		e->use = sub_use_targets;
		break;
	case QE_TRIGGER_COUNTER:
		e->wait = -100;
		if (!e->count)
			e->count = 2;
		e->use = counter_use;
		break;
	case QE_TRIGGER_TELEPORT:
		init_trigger(e);
		e->touch = teleport_touch;
		if (e->spawnflags & PLAYER_ONLY)
			e->flags |= FL_TOUCH_PLAYER;
		if (!e->target)
			printf("ents: teleporter without a target\n");
		e->use = teleport_use;
		if (!(e->spawnflags & SILENT) && ents_first_spawn) {
			int32_t o[3];
			for (int k = 0; k < 3; k++)
				o[k] = (e->mins[k] + e->maxs[k]) / 2;
			snd_add_static(o, snd_find("ambience/hum1.wav"), 127, 192);   /* 0.5, ATTN_STATIC */
		}
		break;
	case QE_INFO_TELEPORT_DESTINATION:
		e->mangle[0] = 0; e->mangle[1] = e->angle; e->mangle[2] = 0;   /* mangle = angles */
		e->origin[2] += 27 * CLIP_SUB;
		if (!e->targetname)
			printf("ents: teleport destination without a targetname\n");
		break;
	case QE_TRIGGER_SETSKILL:
		init_trigger(e);
		e->touch  = trigger_skill_touch;
		e->flags |= FL_TOUCH_PLAYER;
		break;
	case QE_TRIGGER_ONLYREGISTERED:
		init_trigger(e);
		e->touch  = trigger_onlyregistered_touch;
		e->flags |= FL_TOUCH_PLAYER;
		break;
	case QE_TRIGGER_HURT:
		init_trigger(e);
		e->touch = hurt_touch;
		if (!e->dmg)
			e->dmg = 5;
		break;
	case QE_TRIGGER_PUSH:
		init_trigger(e);
		e->noise = (int16_t) snd_find("ambience/windfly.wav");   /* (its precache: the residency pass's) */
		e->touch = trigger_push_touch;
		if (!e->speed)
			e->speed = 1000;
		break;
	case QE_TRIGGER_MONSTERJUMP:
		if (!e->speed)  e->speed = 200;
		if (!e->height) e->height = 200;
		if (!e->angle)  e->angle = 360;
		init_trigger(e);
		e->touch = trigger_monsterjump_touch;
		break;
	case QE_FUNC_WALL:
		brush_static(e, 1);
		e->use = func_wall_use;
		break;
	case QE_FUNC_ILLUSIONARY:
		brush_static(e, 0);
		break;
	case QE_FUNC_EPISODEGATE:
		if (!(game_serverflags & e->spawnflags))
			break;                                /* can still enter episode */
		brush_static(e, 1);
		e->use = func_wall_use;
		break;
	case QE_FUNC_BOSSGATE:
		if ((game_serverflags & 15) == 15)
			break;                                /* all episodes completed */
		brush_static(e, 1);
		e->use = func_wall_use;
		break;
	case QE_AIR_BUBBLES:
		e->nextthink = ent_time + 60;
		e->think     = make_bubbles;
		ent_wake(e);
		break;
	default:                                      /* spots, lights, path corners */
		e->solid    = SOLID_NOT;
		e->movetype = MOVETYPE_NONE;
		break;
	}
}
