/*
 * q1psx — the enforcer (monster_enforcer): enforcer.qc, the 1.06 progs.dat's
 * (the registered game's; the rerelease's enforcer.qc for the frames'
 * names), transcribed state by state: its laser (LaunchLaser, Laser_Touch:
 * engine/game.c MM_LASER). See engine/monsters.h.
 */

#include "game.h"
#include "monsters.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: stand1-7, walk1-16, run1-8, attack1-10, death1-14, fdeath1-11,
   paina1-4, painb1-5, painc1-8, paind1-19 */
enum {
	F_STAND1 = 0, F_WALK1 = 7, F_RUN1 = 23, F_ATTACK1 = 31, F_DEATH1 = 41, F_FDEATH1 = 55, F_PAINA1 = 66,
	F_PAINB1 = 70, F_PAINC1 = 75, F_PAIND1 = 83
};

enum {
	ENF_STAND1 = 0, ENF_WALK1 = 7, ENF_RUN1 = 23, ENF_ATK1 = 31, ENF_PAINA1 = 45, ENF_PAINB1 = 49, ENF_PAINC1 = 54,
	ENF_PAIND1 = 62, ENF_DIE1 = 81, ENF_FDIE1 = 95, ENF_COUNT = 106
};

enum { C_WALK1, C_RUN1, C_FIRE, C_ATK14, C_DIE3 };

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[ENF_COUNT] = {
	/* enf_stand1-7 =[ $stand1, enf_stand2 ] { ai_stand(); } */
	ST(F_STAND1 + 0, ENF_STAND1 + 1, A_STAND, 0), ST(F_STAND1 + 1, ENF_STAND1 + 2, A_STAND, 0),
	ST(F_STAND1 + 2, ENF_STAND1 + 3, A_STAND, 0), ST(F_STAND1 + 3, ENF_STAND1 + 4, A_STAND, 0),
	ST(F_STAND1 + 4, ENF_STAND1 + 5, A_STAND, 0), ST(F_STAND1 + 5, ENF_STAND1 + 6, A_STAND, 0),
	ST(F_STAND1 + 6, ENF_STAND1 + 0, A_STAND, 0),
	/* enf_walk1-16: the idle sound at random, ai_walk */
	ST(F_WALK1 + 0,  ENF_WALK1 + 1,  A_CUSTOM, C_WALK1),
	ST(F_WALK1 + 1,  ENF_WALK1 + 2,  A_WALK, 4), ST(F_WALK1 + 2,  ENF_WALK1 + 3,  A_WALK, 4),
	ST(F_WALK1 + 3,  ENF_WALK1 + 4,  A_WALK, 3), ST(F_WALK1 + 4,  ENF_WALK1 + 5,  A_WALK, 1),
	ST(F_WALK1 + 5,  ENF_WALK1 + 6,  A_WALK, 2), ST(F_WALK1 + 6,  ENF_WALK1 + 7,  A_WALK, 2),
	ST(F_WALK1 + 7,  ENF_WALK1 + 8,  A_WALK, 1), ST(F_WALK1 + 8,  ENF_WALK1 + 9,  A_WALK, 2),
	ST(F_WALK1 + 9,  ENF_WALK1 + 10, A_WALK, 4), ST(F_WALK1 + 10, ENF_WALK1 + 11, A_WALK, 4),
	ST(F_WALK1 + 11, ENF_WALK1 + 12, A_WALK, 1), ST(F_WALK1 + 12, ENF_WALK1 + 13, A_WALK, 2),
	ST(F_WALK1 + 13, ENF_WALK1 + 14, A_WALK, 3), ST(F_WALK1 + 14, ENF_WALK1 + 15, A_WALK, 4),
	ST(F_WALK1 + 15, ENF_WALK1 + 0,  A_WALK, 2),
	/* enf_run1-8 */
	ST(F_RUN1 + 0, ENF_RUN1 + 1, A_CUSTOM, C_RUN1),
	ST(F_RUN1 + 1, ENF_RUN1 + 2, A_RUN, 14), ST(F_RUN1 + 2, ENF_RUN1 + 3, A_RUN, 7),
	ST(F_RUN1 + 3, ENF_RUN1 + 4, A_RUN, 12), ST(F_RUN1 + 4, ENF_RUN1 + 5, A_RUN, 14),
	ST(F_RUN1 + 5, ENF_RUN1 + 6, A_RUN, 14), ST(F_RUN1 + 6, ENF_RUN1 + 7, A_RUN, 7),
	ST(F_RUN1 + 7, ENF_RUN1 + 0, A_RUN, 11),
	/* enf_atk1-14: aiming, a shot at 6 and at 10 (attack5-8 played
	   twice), the refire at 14 */
	ST(F_ATTACK1 + 0, ENF_ATK1 + 1,  A_FACE, 0), ST(F_ATTACK1 + 1, ENF_ATK1 + 2, A_FACE, 0),
	ST(F_ATTACK1 + 2, ENF_ATK1 + 3,  A_FACE, 0), ST(F_ATTACK1 + 3, ENF_ATK1 + 4, A_FACE, 0),
	ST(F_ATTACK1 + 4, ENF_ATK1 + 5,  A_FACE, 0),
	ST(F_ATTACK1 + 5, ENF_ATK1 + 6,  A_CUSTOM, C_FIRE),
	ST(F_ATTACK1 + 6, ENF_ATK1 + 7,  A_FACE, 0), ST(F_ATTACK1 + 7, ENF_ATK1 + 8, A_FACE, 0),
	ST(F_ATTACK1 + 4, ENF_ATK1 + 9,  A_FACE, 0),
	ST(F_ATTACK1 + 5, ENF_ATK1 + 10, A_CUSTOM, C_FIRE),
	ST(F_ATTACK1 + 6, ENF_ATK1 + 11, A_FACE, 0), ST(F_ATTACK1 + 7, ENF_ATK1 + 12, A_FACE, 0),
	ST(F_ATTACK1 + 8, ENF_ATK1 + 13, A_FACE, 0),
	ST(F_ATTACK1 + 9, ENF_RUN1,      A_CUSTOM, C_ATK14),
	/* enf_paina1-4 */
	ST(F_PAINA1 + 0, ENF_PAINA1 + 1, A_NONE, 0), ST(F_PAINA1 + 1, ENF_PAINA1 + 2, A_NONE, 0),
	ST(F_PAINA1 + 2, ENF_PAINA1 + 3, A_NONE, 0), ST(F_PAINA1 + 3, ENF_RUN1,       A_NONE, 0),
	/* enf_painb1-5 */
	ST(F_PAINB1 + 0, ENF_PAINB1 + 1, A_NONE, 0), ST(F_PAINB1 + 1, ENF_PAINB1 + 2, A_NONE, 0),
	ST(F_PAINB1 + 2, ENF_PAINB1 + 3, A_NONE, 0), ST(F_PAINB1 + 3, ENF_PAINB1 + 4, A_NONE, 0),
	ST(F_PAINB1 + 4, ENF_RUN1,       A_NONE, 0),
	/* enf_painc1-8 */
	ST(F_PAINC1 + 0, ENF_PAINC1 + 1, A_NONE, 0), ST(F_PAINC1 + 1, ENF_PAINC1 + 2, A_NONE, 0),
	ST(F_PAINC1 + 2, ENF_PAINC1 + 3, A_NONE, 0), ST(F_PAINC1 + 3, ENF_PAINC1 + 4, A_NONE, 0),
	ST(F_PAINC1 + 4, ENF_PAINC1 + 5, A_NONE, 0), ST(F_PAINC1 + 5, ENF_PAINC1 + 6, A_NONE, 0),
	ST(F_PAINC1 + 6, ENF_PAINC1 + 7, A_NONE, 0), ST(F_PAINC1 + 7, ENF_RUN1,       A_NONE, 0),
	/* enf_paind1-19 */
	ST(F_PAIND1 + 0,  ENF_PAIND1 + 1,  A_NONE, 0),    ST(F_PAIND1 + 1,  ENF_PAIND1 + 2,  A_NONE, 0),
	ST(F_PAIND1 + 2,  ENF_PAIND1 + 3,  A_NONE, 0),    ST(F_PAIND1 + 3,  ENF_PAIND1 + 4,  A_PAINFWD, 2),
	ST(F_PAIND1 + 4,  ENF_PAIND1 + 5,  A_PAINFWD, 1), ST(F_PAIND1 + 5,  ENF_PAIND1 + 6,  A_NONE, 0),
	ST(F_PAIND1 + 6,  ENF_PAIND1 + 7,  A_NONE, 0),    ST(F_PAIND1 + 7,  ENF_PAIND1 + 8,  A_NONE, 0),
	ST(F_PAIND1 + 8,  ENF_PAIND1 + 9,  A_NONE, 0),    ST(F_PAIND1 + 9,  ENF_PAIND1 + 10, A_NONE, 0),
	ST(F_PAIND1 + 10, ENF_PAIND1 + 11, A_PAINFWD, 1), ST(F_PAIND1 + 11, ENF_PAIND1 + 12, A_PAINFWD, 1),
	ST(F_PAIND1 + 12, ENF_PAIND1 + 13, A_PAINFWD, 1), ST(F_PAIND1 + 13, ENF_PAIND1 + 14, A_NONE, 0),
	ST(F_PAIND1 + 14, ENF_PAIND1 + 15, A_NONE, 0),    ST(F_PAIND1 + 15, ENF_PAIND1 + 16, A_PAIN, 1),
	ST(F_PAIND1 + 16, ENF_PAIND1 + 17, A_PAIN, 1),    ST(F_PAIND1 + 17, ENF_PAIND1 + 18, A_NONE, 0),
	ST(F_PAIND1 + 18, ENF_RUN1,        A_NONE, 0),
	/* enf_die1-14: non-solid at 3, the backpack */
	ST(F_DEATH1 + 0,  ENF_DIE1 + 1,  A_NONE, 0),     ST(F_DEATH1 + 1,  ENF_DIE1 + 2,  A_NONE, 0),
	ST(F_DEATH1 + 2,  ENF_DIE1 + 3,  A_CUSTOM, C_DIE3),
	ST(F_DEATH1 + 3,  ENF_DIE1 + 4,  A_FORWARD, 14), ST(F_DEATH1 + 4,  ENF_DIE1 + 5,  A_FORWARD, 2),
	ST(F_DEATH1 + 5,  ENF_DIE1 + 6,  A_NONE, 0),     ST(F_DEATH1 + 6,  ENF_DIE1 + 7,  A_NONE, 0),
	ST(F_DEATH1 + 7,  ENF_DIE1 + 8,  A_NONE, 0),     ST(F_DEATH1 + 8,  ENF_DIE1 + 9,  A_FORWARD, 3),
	ST(F_DEATH1 + 9,  ENF_DIE1 + 10, A_FORWARD, 5),  ST(F_DEATH1 + 10, ENF_DIE1 + 11, A_FORWARD, 5),
	ST(F_DEATH1 + 11, ENF_DIE1 + 12, A_FORWARD, 5),  ST(F_DEATH1 + 12, ENF_DIE1 + 13, A_NONE, 0),
	ST(F_DEATH1 + 13, ENF_DIE1 + 13, A_NONE, 0),
	/* enf_fdie1-11 */
	ST(F_FDEATH1 + 0,  ENF_FDIE1 + 1,  A_NONE, 0), ST(F_FDEATH1 + 1, ENF_FDIE1 + 2, A_NONE, 0),
	ST(F_FDEATH1 + 2,  ENF_FDIE1 + 3,  A_CUSTOM, C_DIE3),
	ST(F_FDEATH1 + 3,  ENF_FDIE1 + 4,  A_NONE, 0), ST(F_FDEATH1 + 4, ENF_FDIE1 + 5, A_NONE, 0),
	ST(F_FDEATH1 + 5,  ENF_FDIE1 + 6,  A_NONE, 0), ST(F_FDEATH1 + 6, ENF_FDIE1 + 7, A_NONE, 0),
	ST(F_FDEATH1 + 7,  ENF_FDIE1 + 8,  A_NONE, 0), ST(F_FDEATH1 + 8, ENF_FDIE1 + 9, A_NONE, 0),
	ST(F_FDEATH1 + 9,  ENF_FDIE1 + 10, A_NONE, 0), ST(F_FDEATH1 + 10, ENF_FDIE1 + 10, A_NONE, 0),
};

static int16_t s_death1, s_enfire, s_enfstop, s_idle1, s_pain1, s_pain2, s_sight[4], s_udeath;

static void enf_map_start(void)
{
	s_death1  = (int16_t) snd_find("enforcer/death1.wav");
	s_enfire  = (int16_t) snd_find("enforcer/enfire.wav");
	s_enfstop = (int16_t) snd_find("enforcer/enfstop.wav");
	s_idle1   = (int16_t) snd_find("enforcer/idle1.wav");
	s_pain1   = (int16_t) snd_find("enforcer/pain1.wav");
	s_pain2   = (int16_t) snd_find("enforcer/pain2.wav");
	s_sight[0] = (int16_t) snd_find("enforcer/sight1.wav");
	s_sight[1] = (int16_t) snd_find("enforcer/sight2.wav");
	s_sight[2] = (int16_t) snd_find("enforcer/sight3.wav");
	s_sight[3] = (int16_t) snd_find("enforcer/sight4.wav");
	s_udeath  = (int16_t) snd_find("player/udeath.wav");
}

static void enf_stand(edict_t *self)   { monster_state(self, ENF_STAND1); }
static void enf_walk(edict_t *self)    { monster_state(self, ENF_WALK1); }
static void enf_run(edict_t *self)     { monster_state(self, ENF_RUN1); }
static void enf_missile(edict_t *self) { monster_state(self, ENF_ATK1); }

/* SightSound's enforcer: rint (random () x 3), 1 sight1, 2 sight2, 0
   sight3, else sight4 */
static void enf_sight(edict_t *self)
{
	static const uint8_t pick[4] = { 2, 0, 1, 3 };
	int rsnd = (int) ((frand() * 3 + 32768) >> 16);
	ent_sound(self, CHAN_VOICE, s_sight[pick[rsnd]], 255, ATTN_NORM);
}

/* enforcer_fire: the muzzle flash, LaunchLaser from 30 forward, 8.5 right
   and 16 up towards the enemy's origin (its sound, a laser at 600 a
   second, 5 s to live, EF_DIMLIGHT) */
static void enforcer_fire(edict_t *self)
{
	game_muzzle_flash(ENT_EDICT + (int) (self - edicts), self->origin, self->ang[1] >> 4);   /* EF_MUZZLEFLASH */
	int32_t f[3], r[3], u[3], org[3], d[3], dir[3], vel[3];
	ai_makevectors(self->ang, f, r, u);
	for (int k = 0; k < 3; k++)
		org[k] = self->origin[k] + (f[k] * 30 * SUB + r[k] * 17 * SUB / 2) / 4096;
	org[2] += 16 * SUB;
	edict_t *en = self->enemy;
	ent_sound(self, CHAN_WEAPON, s_enfire, 255, ATTN_NORM);
	for (int k = 0; k < 3; k++)
		d[k] = en->origin[k] - self->origin[k];
	int32_t len = (int32_t) q_isqrt64((uint64_t) ai_vlen2(d));
	for (int k = 0; k < 3; k++)
		dir[k] = len ? (int32_t) q_div64_32((int64_t) d[k] * 4096, len) : 0;
	for (int k = 0; k < 3; k++)
		vel[k] = dir[k] * 600 / 4096;
	game_monster_missile(MM_LASER, self, org, vel, monster_asset(self, 3), s_enfstop, SND_NONE);
}

static void enf_custom(edict_t *self, int code)
{
	switch (code) {
	case C_WALK1:
		if (frand() < 13107)             /* random () < 0.2 */
			ent_sound(self, CHAN_VOICE, s_idle1, 255, ATTN_IDLE);
		ai_walk(self, 2);
		break;
	case C_RUN1:
		if (frand() < 13107)
			ent_sound(self, CHAN_VOICE, s_idle1, 255, ATTN_IDLE);
		ai_run(self, 18);
		break;
	case C_FIRE:
		enforcer_fire(self);
		break;
	case C_ATK14:
		ai_face(self);
		sub_check_refire(self, ENF_ATK1);
		break;
	case C_DIE3:
		self->solid = SOLID_NOT;
		ent_link(self);
		self->ammo_cells = 5;
		drop_backpack(self);
		break;
	}
}

void enf_pain(edict_t *self)
{
	uint32_t r = frand();                /* (drawn before the test) */
	if (self->pain_finished > ent_time)
		return;
	ent_sound(self, CHAN_VOICE, r < 32768 ? s_pain1 : s_pain2, 255, ATTN_NORM);
	if (r < 13107) {                     /* 0.2 */
		self->pain_finished = ent_time + TICKS(100);
		monster_state(self, ENF_PAINA1);
	} else if (r < 26214) {              /* 0.4 */
		self->pain_finished = ent_time + TICKS(100);
		monster_state(self, ENF_PAINB1);
	} else if (r < 45875) {              /* 0.7 */
		self->pain_finished = ent_time + TICKS(100);
		monster_state(self, ENF_PAINC1);
	} else {
		self->pain_finished = ent_time + TICKS(200);
		monster_state(self, ENF_PAIND1);
	}
}

void enf_die(edict_t *self)
{
	if (self->health < -35) {            /* check for gib */
		ent_sound(self, CHAN_VOICE, s_udeath, 255, ATTN_NORM);
		throw_head(self, self->health);
		throw_gib(self, 0, self->health);
		throw_gib(self, 1, self->health);
		throw_gib(self, 2, self->health);
		return;
	}
	/* regular death */
	ent_sound(self, CHAN_VOICE, s_death1, 255, ATTN_NORM);
	if (frand() > 32768)
		monster_state(self, ENF_DIE1);
	else
		monster_state(self, ENF_FDIE1);
}

const mdef_t mdef_enforcer = {
	.classname = "monster_enforcer",
	.assets = { "progs/enforcer.mdl", "progs/h_mega.mdl", "progs/backpack.mdl", "progs/laser.mdl" },
	.mins = { -16, -16, -24 }, .maxs = { 16, 16, 40 },
	.health = 80,
	.kind = MK_ENFORCER,
	.states = states, .num_states = ENF_COUNT,
	.th_stand = enf_stand, .th_walk = enf_walk, .th_run = enf_run,
	.th_missile = enf_missile, .th_melee = 0,
	.th_pain = EF_enf_pain, .th_die = EF_enf_die,
	.custom = enf_custom,
	.check_attack = 0,
	.map_start = enf_map_start,
	.sight_fn = enf_sight,
};
