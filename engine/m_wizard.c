/*
 * q1psx — the scrag (monster_wizard): wizard.qc, the shareware's 1.06,
 * transcribed state by state: its two spikes, each fired by a helper
 * entity a moment after the cast (engine/game.c's projectiles), its
 * strafing. See engine/monsters.h.
 */

#include <string.h>

#include "game.h"
#include "monsters.h"
#include "q_fixed.h"
#include "render.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: hover1-15, fly1-14, magatt1-13, pain1-4, death1-8 */
enum {
	F_HOVER1 = 0, F_FLY1 = 15, F_MAGATT1 = 29, F_PAIN1 = 42, F_DEATH1 = 46
};

enum {
	WIZ_STAND1 = 0, WIZ_WALK1 = 8, WIZ_SIDE1 = 16, WIZ_RUN1 = 24, WIZ_FAST1 = 38, WIZ_PAIN1 = 48,
	WIZ_DEATH1 = 52, WIZ_COUNT = 60
};

enum { C_WALK1, C_SIDE1, C_RUN1, C_FAST1, C_FAST10, C_DEATH1, C_DEATH3 };

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[WIZ_COUNT] = {
	/* wiz_stand */
	ST(F_HOVER1, WIZ_STAND1 + 1, A_STAND, 0),     ST(F_HOVER1 + 1, WIZ_STAND1 + 2, A_STAND, 0),
	ST(F_HOVER1 + 2, WIZ_STAND1 + 3, A_STAND, 0), ST(F_HOVER1 + 3, WIZ_STAND1 + 4, A_STAND, 0),
	ST(F_HOVER1 + 4, WIZ_STAND1 + 5, A_STAND, 0), ST(F_HOVER1 + 5, WIZ_STAND1 + 6, A_STAND, 0),
	ST(F_HOVER1 + 6, WIZ_STAND1 + 7, A_STAND, 0), ST(F_HOVER1 + 7, WIZ_STAND1, A_STAND, 0),
	/* wiz_walk */
	ST(F_HOVER1, WIZ_WALK1 + 1, A_CUSTOM, C_WALK1),
	ST(F_HOVER1 + 1, WIZ_WALK1 + 2, A_WALK, 8),   ST(F_HOVER1 + 2, WIZ_WALK1 + 3, A_WALK, 8),
	ST(F_HOVER1 + 3, WIZ_WALK1 + 4, A_WALK, 8),   ST(F_HOVER1 + 4, WIZ_WALK1 + 5, A_WALK, 8),
	ST(F_HOVER1 + 5, WIZ_WALK1 + 6, A_WALK, 8),   ST(F_HOVER1 + 6, WIZ_WALK1 + 7, A_WALK, 8),
	ST(F_HOVER1 + 7, WIZ_WALK1, A_WALK, 8),
	/* wiz_side */
	ST(F_HOVER1, WIZ_SIDE1 + 1, A_CUSTOM, C_SIDE1),
	ST(F_HOVER1 + 1, WIZ_SIDE1 + 2, A_RUN, 8),    ST(F_HOVER1 + 2, WIZ_SIDE1 + 3, A_RUN, 8),
	ST(F_HOVER1 + 3, WIZ_SIDE1 + 4, A_RUN, 8),    ST(F_HOVER1 + 4, WIZ_SIDE1 + 5, A_RUN, 8),
	ST(F_HOVER1 + 5, WIZ_SIDE1 + 6, A_RUN, 8),    ST(F_HOVER1 + 6, WIZ_SIDE1 + 7, A_RUN, 8),
	ST(F_HOVER1 + 7, WIZ_SIDE1, A_RUN, 8),
	/* wiz_run */
	ST(F_FLY1, WIZ_RUN1 + 1, A_CUSTOM, C_RUN1),
	ST(F_FLY1 + 1, WIZ_RUN1 + 2, A_RUN, 16),      ST(F_FLY1 + 2, WIZ_RUN1 + 3, A_RUN, 16),
	ST(F_FLY1 + 3, WIZ_RUN1 + 4, A_RUN, 16),      ST(F_FLY1 + 4, WIZ_RUN1 + 5, A_RUN, 16),
	ST(F_FLY1 + 5, WIZ_RUN1 + 6, A_RUN, 16),      ST(F_FLY1 + 6, WIZ_RUN1 + 7, A_RUN, 16),
	ST(F_FLY1 + 7, WIZ_RUN1 + 8, A_RUN, 16),      ST(F_FLY1 + 8, WIZ_RUN1 + 9, A_RUN, 16),
	ST(F_FLY1 + 9, WIZ_RUN1 + 10, A_RUN, 16),     ST(F_FLY1 + 10, WIZ_RUN1 + 11, A_RUN, 16),
	ST(F_FLY1 + 11, WIZ_RUN1 + 12, A_RUN, 16),    ST(F_FLY1 + 12, WIZ_RUN1 + 13, A_RUN, 16),
	ST(F_FLY1 + 13, WIZ_RUN1, A_RUN, 16),
	/* wiz_fast */
	ST(F_MAGATT1, WIZ_FAST1 + 1, A_CUSTOM, C_FAST1),
	ST(F_MAGATT1 + 1, WIZ_FAST1 + 2, A_FACE, 0),  ST(F_MAGATT1 + 2, WIZ_FAST1 + 3, A_FACE, 0),
	ST(F_MAGATT1 + 3, WIZ_FAST1 + 4, A_FACE, 0),  ST(F_MAGATT1 + 4, WIZ_FAST1 + 5, A_FACE, 0),
	ST(F_MAGATT1 + 5, WIZ_FAST1 + 6, A_FACE, 0),  ST(F_MAGATT1 + 4, WIZ_FAST1 + 7, A_FACE, 0),
	ST(F_MAGATT1 + 3, WIZ_FAST1 + 8, A_FACE, 0),  ST(F_MAGATT1 + 2, WIZ_FAST1 + 9, A_FACE, 0),
	ST(F_MAGATT1 + 1, WIZ_RUN1, A_CUSTOM, C_FAST10),
	/* wiz_pain */
	ST(F_PAIN1, WIZ_PAIN1 + 1, A_NONE, 0),        ST(F_PAIN1 + 1, WIZ_PAIN1 + 2, A_NONE, 0),
	ST(F_PAIN1 + 2, WIZ_PAIN1 + 3, A_NONE, 0),    ST(F_PAIN1 + 3, WIZ_RUN1, A_NONE, 0),
	/* wiz_death */
	ST(F_DEATH1, WIZ_DEATH1 + 1, A_CUSTOM, C_DEATH1),
	ST(F_DEATH1 + 1, WIZ_DEATH1 + 2, A_NONE, 0),
	ST(F_DEATH1 + 2, WIZ_DEATH1 + 3, A_CUSTOM, C_DEATH3),
	ST(F_DEATH1 + 3, WIZ_DEATH1 + 4, A_NONE, 0),  ST(F_DEATH1 + 4, WIZ_DEATH1 + 5, A_NONE, 0),
	ST(F_DEATH1 + 5, WIZ_DEATH1 + 6, A_NONE, 0),  ST(F_DEATH1 + 6, WIZ_DEATH1 + 7, A_NONE, 0),
	ST(F_DEATH1 + 7, WIZ_DEATH1 + 7, A_NONE, 0),
};

static int16_t s_hit, s_wattack, s_wdeath, s_widle1, s_widle2, s_wpain, s_wsight, s_udeath;

static void wiz_map_start(void)
{
	s_hit     = (int16_t) snd_find("wizard/hit.wav");
	s_wattack = (int16_t) snd_find("wizard/wattack.wav");
	s_wdeath  = (int16_t) snd_find("wizard/wdeath.wav");
	s_widle1  = (int16_t) snd_find("wizard/widle1.wav");
	s_widle2  = (int16_t) snd_find("wizard/widle2.wav");
	s_wpain   = (int16_t) snd_find("wizard/wpain.wav");
	s_wsight  = (int16_t) snd_find("wizard/wsight.wav");
	s_udeath  = (int16_t) snd_find("player/udeath.wav");
}

static void wiz_stand(edict_t *self) { monster_state(self, WIZ_STAND1); }
static void wiz_walk(edict_t *self)  { monster_state(self, WIZ_WALK1); }
static void wiz_run(edict_t *self)   { monster_state(self, WIZ_RUN1); }
static void wiz_missile(edict_t *self) { monster_state(self, WIZ_FAST1); }   /* Wiz_Missile */

/* (a QC `wiz_run1 ();` / `wiz_side1 ();` inside its own check) */
static void wiz_go(edict_t *self, int as, int s)
{
	if (self->attack_state != as) {
		self->attack_state = (uint8_t) as;
		monster_state(self, s);
	}
}

/* WizardCheckAttack: a spike by chance at its range, else strafe (near)
   or close in (mid, or no clear shot) */
static int wiz_check_attack(edict_t *self)
{
	if (ent_time < self->attack_finished)
		return 0;
	if (!ai_enemy_visible)
		return 0;
	if (ai_enemy_range == RANGE_FAR) {
		wiz_go(self, AS_STRAIGHT, WIZ_RUN1);
		return 0;
	}
	edict_t *targ = self->enemy;
	int32_t spot1[3], spot2[3];
	ai_view_org(self, spot1);
	ai_view_org(targ, spot2);
	ai_traceline(spot1, spot2, 0, self);
	if (ai_trace_ent != targ) {          /* don't have a clear shot, so move to a side */
		wiz_go(self, AS_STRAIGHT, WIZ_RUN1);
		return 0;
	}
	uint32_t chance = ai_enemy_range == RANGE_MELEE ? 58982     /* 0.9 */
	                : ai_enemy_range == RANGE_NEAR  ? 39322     /* 0.6 */
	                : ai_enemy_range == RANGE_MID   ? 13107 : 0;   /* 0.2 */
	if (frand() < chance) {
		self->attack_state = AS_MISSILE;
		return 1;
	}
	if (ai_enemy_range == RANGE_MID)
		wiz_go(self, AS_STRAIGHT, WIZ_RUN1);
	else
		wiz_go(self, AS_SLIDING, WIZ_SIDE1);
	return 0;
}

/* WizardAttackFinished: strafe after the cast when close and in sight,
   else close in (the think the state set, its next one) */
static void wiz_attack_finished(edict_t *self)
{
	if (ai_enemy_range >= RANGE_MID || !ai_enemy_visible) {
		self->attack_state = AS_STRAIGHT;
		self->mstate = WIZ_RUN1;
	} else {
		self->attack_state = AS_SLIDING;
		self->mstate = WIZ_SIDE1;
	}
	self->think = EF_monster_think;
}

/* makevectors (an entity's angles): a monster's, or the player's (its
   v_angle's yaw, a third of its pitch the other way: SV_ClientThink's) */
static void makevectors_of(const edict_t *e)
{
	int32_t f[3], r[3], u[3];
	if (e == ent_player) {
		uint16_t ang[3] = { (uint16_t) (-pl.pm.pitch * (65536 / ANG_TURN) / 3), (uint16_t) (pl.pm.yaw * (65536 / ANG_TURN)), 0 };
		ai_makevectors(ang, f, r, u);
	} else {
		ai_makevectors(e->ang, f, r, u);
	}
}

/* Wiz_FastFire (self: the helper): the spike from its place at the enemy,
   13 units to its side (its movedir), if the scrag still lives */
void wiz_fast_fire(edict_t *self)
{
	edict_t *owner = self->owner;
	if (owner && owner->health > 0) {
		game_muzzle_flash(ENT_EDICT + (int) (owner - edicts), owner->origin, owner->ang[1] >> 4);   /* EF_MUZZLEFLASH */
		makevectors_of(self->enemy);     /* (its vectors unused) */
		int32_t dst[3], d[3], vec[3];
		for (int k = 0; k < 3; k++) {
			dst[k] = self->enemy->origin[k] - 13 * SUB * self->movedir[k] / 4096;
			d[k] = dst[k] - self->origin[k];
		}
		int32_t len = (int32_t) q_isqrt64((uint64_t) ai_vlen2(d));
		for (int k = 0; k < 3; k++)
			vec[k] = len ? d[k] * 600 / len : 0;
		ent_sound(self, CHAN_WEAPON, s_wattack, 255, ATTN_NORM);
		game_monster_missile(MM_WIZSPIKE, owner, self->origin, vec, monster_asset(owner, 2), s_hit, SND_NONE);
	}
	ent_remove(self);
}

/* Wiz_StartFast: two helpers 30 up, 14 ahead and 14 to each side, firing
   0.8 and 0.3 s on */
static void wiz_start_fast(edict_t *self)
{
	ent_sound(self, CHAN_WEAPON, s_wattack, 255, ATTN_NORM);
	int32_t f[3], r[3], u[3];
	ai_makevectors(self->ang, f, r, u);
	for (int side = 1; side >= -1; side -= 2) {
		edict_t *m = ent_spawn();
		if (!m)
			return;
		m->owner = self;
		static const int32_t zero[3] = { 0, 0, 0 };
		ent_setsize(m, zero, zero);
		int32_t org[3];
		for (int k = 0; k < 3; k++)
			org[k] = self->origin[k] + (k == 2 ? 30 * SUB : 0) + (14 * f[k] + side * 14 * r[k]) * SUB / 4096;
		ent_setorigin(m, org);
		m->enemy = self->enemy;
		m->nextthink = ent_time + (side > 0 ? TICKS(80) : TICKS(30));
		m->think     = EF_wiz_fast_fire;
		for (int k = 0; k < 3; k++)
			m->movedir[k] = side * r[k];
		ent_wake(m);
	}
}

/* Wiz_idlesound: at most every 2 s, now and then */
static void wiz_idle_sound(edict_t *self)
{
	uint32_t wr = frand() * 5;           /* random () x 5, x65536 */
	if (self->waitmin < ent_time) {
		self->waitmin = ent_time + TICKS(200);
		if (wr > 294912)                 /* 4.5 */
			ent_sound(self, CHAN_VOICE, s_widle1, 255, ATTN_IDLE);
		if (wr < 98304)                  /* 1.5 */
			ent_sound(self, CHAN_VOICE, s_widle2, 255, ATTN_IDLE);
	}
}

static void wiz_custom(edict_t *self, int code)
{
	switch (code) {
	case C_WALK1:
		ai_walk(self, 8);
		wiz_idle_sound(self);
		break;
	case C_SIDE1:
		ai_run(self, 8);
		wiz_idle_sound(self);
		break;
	case C_RUN1:
		ai_run(self, 16);
		wiz_idle_sound(self);
		break;
	case C_FAST1:
		ai_face(self);
		wiz_start_fast(self);
		break;
	case C_FAST10:
		ai_face(self);
		sub_attack_finished(self, TICKS(200));
		wiz_attack_finished(self);
		break;
	case C_DEATH1:
		self->velocity[0] = -200 + (int32_t) (400 * frand() >> 16);
		self->velocity[1] = -200 + (int32_t) (400 * frand() >> 16);
		self->velocity[2] = 100 + (int32_t) (100 * frand() >> 16);
		self->flags &= ~FL_ONGROUND;
		ent_sound(self, CHAN_VOICE, s_wdeath, 255, ATTN_NORM);
		break;
	case C_DEATH3:
		self->solid = SOLID_NOT;
		ent_link(self);
		break;
	}
}

void wiz_pain(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, s_wpain, 255, ATTN_NORM);
	if (frand() * 70 > (uint32_t) ai_damage_take << 16)   /* random () x 70 > damage */
		return;
	monster_state(self, WIZ_PAIN1);
}

void wiz_die(edict_t *self)
{
	if (self->health < -40) {            /* check for gib */
		ent_sound(self, CHAN_VOICE, s_udeath, 255, ATTN_NORM);
		throw_head(self, self->health);
		throw_gib(self, 1, self->health);
		throw_gib(self, 1, self->health);
		throw_gib(self, 1, self->health);
		return;
	}
	monster_state(self, WIZ_DEATH1);
}

const mdef_t mdef_wizard = {
	.classname = "monster_wizard",
	.assets = { "progs/wizard.mdl", "progs/h_wizard.mdl", "progs/w_spike.mdl" },
	.mins = { -16, -16, -24 }, .maxs = { 16, 16, 40 },
	.health = 80,
	.kind = MK_WIZARD,
	.fly = 1,
	.states = states, .num_states = WIZ_COUNT,
	.th_stand = wiz_stand, .th_walk = wiz_walk, .th_run = wiz_run,
	.th_missile = wiz_missile, .th_melee = 0,
	.th_pain = EF_wiz_pain, .th_die = EF_wiz_die,
	.custom = wiz_custom,
	.check_attack = wiz_check_attack,
	.map_start = wiz_map_start,
	.sight = &s_wsight,
};
