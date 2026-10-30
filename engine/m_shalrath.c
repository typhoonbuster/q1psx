/*
 * q1psx — the vore (monster_shalrath): shalrath.qc, the 1.06 progs.dat's
 * (the registered game's; the rerelease's vore.qc for the frames' names),
 * transcribed state by state: its homing ball (ShalMissile, ShalHome,
 * ShalMissileTouch: engine/game.c game_vore_missile), its start a tenth of
 * a second after the others'. See engine/monsters.h.
 */

#include "game.h"
#include "monsters.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: attack1-11, pain1-5, death1-7, walk1-12 */
enum { F_ATTACK1 = 0, F_PAIN1 = 11, F_DEATH1 = 16, F_WALK1 = 23 };

enum {
	SHAL_STAND = 0, SHAL_WALK1 = 1, SHAL_RUN1 = 13, SHAL_ATTACK1 = 25, SHAL_PAIN1 = 36, SHAL_DEATH1 = 41,
	SHAL_COUNT = 48
};

enum { C_WALK1, C_RUN1, C_ATTACK1, C_ATTACK9 };

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[SHAL_COUNT] = {
	/* shal_stand =[ $walk1, shal_stand ] { ai_stand(); } */
	ST(F_WALK1, SHAL_STAND, A_STAND, 0),
	/* shal_walk1-12: the idle sound at random, ai_walk */
	ST(F_WALK1 + 1,  SHAL_WALK1 + 1,  A_CUSTOM, C_WALK1),
	ST(F_WALK1 + 2,  SHAL_WALK1 + 2,  A_WALK, 4), ST(F_WALK1 + 3,  SHAL_WALK1 + 3,  A_WALK, 0),
	ST(F_WALK1 + 4,  SHAL_WALK1 + 4,  A_WALK, 0), ST(F_WALK1 + 5,  SHAL_WALK1 + 5,  A_WALK, 0),
	ST(F_WALK1 + 6,  SHAL_WALK1 + 6,  A_WALK, 0), ST(F_WALK1 + 7,  SHAL_WALK1 + 7,  A_WALK, 5),
	ST(F_WALK1 + 8,  SHAL_WALK1 + 8,  A_WALK, 6), ST(F_WALK1 + 9,  SHAL_WALK1 + 9,  A_WALK, 5),
	ST(F_WALK1 + 10, SHAL_WALK1 + 10, A_WALK, 0), ST(F_WALK1 + 11, SHAL_WALK1 + 11, A_WALK, 4),
	ST(F_WALK1 + 0,  SHAL_WALK1 + 0,  A_WALK, 5),
	/* shal_run1-12 */
	ST(F_WALK1 + 1,  SHAL_RUN1 + 1,  A_CUSTOM, C_RUN1),
	ST(F_WALK1 + 2,  SHAL_RUN1 + 2,  A_RUN, 4), ST(F_WALK1 + 3,  SHAL_RUN1 + 3,  A_RUN, 0),
	ST(F_WALK1 + 4,  SHAL_RUN1 + 4,  A_RUN, 0), ST(F_WALK1 + 5,  SHAL_RUN1 + 5,  A_RUN, 0),
	ST(F_WALK1 + 6,  SHAL_RUN1 + 6,  A_RUN, 0), ST(F_WALK1 + 7,  SHAL_RUN1 + 7,  A_RUN, 5),
	ST(F_WALK1 + 8,  SHAL_RUN1 + 8,  A_RUN, 6), ST(F_WALK1 + 9,  SHAL_RUN1 + 9,  A_RUN, 5),
	ST(F_WALK1 + 10, SHAL_RUN1 + 10, A_RUN, 0), ST(F_WALK1 + 11, SHAL_RUN1 + 11, A_RUN, 4),
	ST(F_WALK1 + 0,  SHAL_RUN1 + 0,  A_RUN, 5),
	/* shal_attack1-11: its sound, aiming, the ball at 9 */
	ST(F_ATTACK1 + 0,  SHAL_ATTACK1 + 1,  A_CUSTOM, C_ATTACK1),
	ST(F_ATTACK1 + 1,  SHAL_ATTACK1 + 2,  A_FACE, 0), ST(F_ATTACK1 + 2, SHAL_ATTACK1 + 3, A_FACE, 0),
	ST(F_ATTACK1 + 3,  SHAL_ATTACK1 + 4,  A_FACE, 0), ST(F_ATTACK1 + 4, SHAL_ATTACK1 + 5, A_FACE, 0),
	ST(F_ATTACK1 + 5,  SHAL_ATTACK1 + 6,  A_FACE, 0), ST(F_ATTACK1 + 6, SHAL_ATTACK1 + 7, A_FACE, 0),
	ST(F_ATTACK1 + 7,  SHAL_ATTACK1 + 8,  A_FACE, 0),
	ST(F_ATTACK1 + 8,  SHAL_ATTACK1 + 9,  A_CUSTOM, C_ATTACK9),
	ST(F_ATTACK1 + 9,  SHAL_ATTACK1 + 10, A_FACE, 0), ST(F_ATTACK1 + 10, SHAL_RUN1, A_NONE, 0),
	/* shal_pain1-5 */
	ST(F_PAIN1 + 0, SHAL_PAIN1 + 1, A_NONE, 0), ST(F_PAIN1 + 1, SHAL_PAIN1 + 2, A_NONE, 0),
	ST(F_PAIN1 + 2, SHAL_PAIN1 + 3, A_NONE, 0), ST(F_PAIN1 + 3, SHAL_PAIN1 + 4, A_NONE, 0),
	ST(F_PAIN1 + 4, SHAL_RUN1,      A_NONE, 0),
	/* shal_death1-7 */
	ST(F_DEATH1 + 0, SHAL_DEATH1 + 1, A_NONE, 0), ST(F_DEATH1 + 1, SHAL_DEATH1 + 2, A_NONE, 0),
	ST(F_DEATH1 + 2, SHAL_DEATH1 + 3, A_NONE, 0), ST(F_DEATH1 + 3, SHAL_DEATH1 + 4, A_NONE, 0),
	ST(F_DEATH1 + 4, SHAL_DEATH1 + 5, A_NONE, 0), ST(F_DEATH1 + 5, SHAL_DEATH1 + 6, A_NONE, 0),
	ST(F_DEATH1 + 6, SHAL_DEATH1 + 6, A_NONE, 0),
};

static int16_t s_attack, s_attack2, s_death, s_idle, s_pain, s_sight, s_udeath;

static void shal_map_start(void)
{
	s_attack  = (int16_t) snd_find("shalrath/attack.wav");
	s_attack2 = (int16_t) snd_find("shalrath/attack2.wav");
	s_death   = (int16_t) snd_find("shalrath/death.wav");
	s_idle    = (int16_t) snd_find("shalrath/idle.wav");
	s_pain    = (int16_t) snd_find("shalrath/pain.wav");
	s_sight   = (int16_t) snd_find("shalrath/sight.wav");
	s_udeath  = (int16_t) snd_find("player/udeath.wav");
}

static void shal_stand(edict_t *self)   { monster_state(self, SHAL_STAND); }
static void shal_walk(edict_t *self)    { monster_state(self, SHAL_WALK1); }
static void shal_run(edict_t *self)     { monster_state(self, SHAL_RUN1); }
static void shal_missile(edict_t *self) { monster_state(self, SHAL_ATTACK1); }

/* ShalMissile: the ball from 10 over its origin towards the enemy's
   (10 over too) at 400 a second, homing from a fly time of its distance
   x 0.002 s (0.1 at least) */
static void shal_missile_fire(edict_t *self)
{
	const edict_t *en = self->enemy ? self->enemy : ent_world;
	int32_t d[3], dir[3], vel[3], org[3];
	for (int k = 0; k < 3; k++)
		d[k] = en->origin[k] - self->origin[k];
	int32_t dist = (int32_t) q_isqrt64((uint64_t) ai_vlen2(d));   /* 1/SUB units */
	d[2] += 10 * SUB;
	int32_t len = (int32_t) q_isqrt64((uint64_t) ai_vlen2(d));
	for (int k = 0; k < 3; k++)
		dir[k] = len ? (int32_t) q_div64_32((int64_t) d[k] * 4096, len) : 0;
	int flytime = dist * 3 / (25 * SUB);    /* x 0.002 s: 0.12 ticks a unit */
	if (flytime < TICKS(10))
		flytime = TICKS(10);
	game_muzzle_flash(ENT_EDICT + (int) (self - edicts), self->origin, self->ang[1] >> 4);   /* EF_MUZZLEFLASH */
	ent_sound(self, CHAN_WEAPON, s_attack2, 255, ATTN_NORM);
	for (int k = 0; k < 3; k++) {
		org[k] = self->origin[k];
		vel[k] = dir[k] * 400 / 4096;
	}
	org[2] += 10 * SUB;
	game_vore_missile(self, org, vel, monster_asset(self, 2), flytime);
}

static void shal_custom(edict_t *self, int code)
{
	switch (code) {
	case C_WALK1:
		if (frand() < 13107)             /* random () < 0.2 */
			ent_sound(self, CHAN_VOICE, s_idle, 255, ATTN_IDLE);
		ai_walk(self, 6);
		break;
	case C_RUN1:
		if (frand() < 13107)
			ent_sound(self, CHAN_VOICE, s_idle, 255, ATTN_IDLE);
		ai_run(self, 6);
		break;
	case C_ATTACK1:
		ent_sound(self, CHAN_VOICE, s_attack, 255, ATTN_NORM);
		ai_face(self);
		break;
	case C_ATTACK9:
		shal_missile_fire(self);
		break;
	}
}

void shalrath_pain(edict_t *self)
{
	if (self->pain_finished > ent_time)
		return;
	ent_sound(self, CHAN_VOICE, s_pain, 255, ATTN_NORM);
	monster_state(self, SHAL_PAIN1);
	self->pain_finished = ent_time + TICKS(300);
}

void shalrath_die(edict_t *self)
{
	if (self->health < -90) {            /* check for gib */
		ent_sound(self, CHAN_VOICE, s_udeath, 255, ATTN_NORM);
		throw_head(self, self->health);
		throw_gib(self, 0, self->health);
		throw_gib(self, 1, self->health);
		throw_gib(self, 2, self->health);
		return;
	}
	ent_sound(self, CHAN_VOICE, s_death, 255, ATTN_NORM);
	monster_state(self, SHAL_DEATH1);
	self->solid = SOLID_NOT;
	ent_link(self);
}

/* monster_shalrath's end: walkmonster_start as a think 0.1 + random () x
   0.1 s on */
static int shal_spawn(edict_t *self)
{
	self->think     = EF_monster_start;
	self->nextthink = ent_time + TICKS(10) + (int32_t) (frand() * TICKS(10) >> 16);
	ent_wake(self);
	return 1;
}

const mdef_t mdef_shalrath = {
	.classname = "monster_shalrath",
	.assets = { "progs/shalrath.mdl", "progs/h_shal.mdl", "progs/v_spike.mdl" },
	.mins = { -32, -32, -24 }, .maxs = { 32, 32, 64 },
	.health = 400,
	.kind = MK_SHALRATH,
	.states = states, .num_states = SHAL_COUNT,
	.th_stand = shal_stand, .th_walk = shal_walk, .th_run = shal_run,
	.th_missile = shal_missile, .th_melee = 0,
	.th_pain = EF_shalrath_pain, .th_die = EF_shalrath_die,
	.custom = shal_custom,
	.check_attack = 0,
	.spawn = shal_spawn,
	.map_start = shal_map_start,
	.sight = &s_sight,
};
