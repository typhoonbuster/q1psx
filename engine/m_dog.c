/*
 * q1psx — the dog (monster_dog): dog.qc, the shareware's 1.06 (the
 * rerelease's rottweiler.qc, its additions left out), transcribed state by
 * state: the bite, the leap and its touch. See engine/monsters.h.
 */

#include "game.h"
#include "monsters.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: attack1-8, death1-9, deathb1-9, pain1-6, painb1-16, run1-12, leap1-9, stand1-9, walk1-8 */
enum {
	F_ATTACK1 = 0, F_DEATH1 = 8, F_DEATHB1 = 17, F_PAIN1 = 26, F_PAINB1 = 32, F_RUN1 = 48,
	F_LEAP1 = 60, F_STAND1 = 69, F_WALK1 = 78
};

enum {
	DOG_STAND1 = 0, DOG_WALK1 = 9, DOG_RUN1 = 17, DOG_ATTA1 = 29, DOG_LEAP1 = 37, DOG_PAIN1 = 46,
	DOG_PAINB1 = 52, DOG_DIE1 = 68, DOG_DIEB1 = 77, DOG_COUNT = 86
};

enum { C_WALK1, C_RUN1, C_ATTA4, C_LEAP2 };

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[DOG_COUNT] = {
	/* dog_stand */
	ST(F_STAND1, DOG_STAND1 + 1, A_STAND, 0),     ST(F_STAND1 + 1, DOG_STAND1 + 2, A_STAND, 0),
	ST(F_STAND1 + 2, DOG_STAND1 + 3, A_STAND, 0), ST(F_STAND1 + 3, DOG_STAND1 + 4, A_STAND, 0),
	ST(F_STAND1 + 4, DOG_STAND1 + 5, A_STAND, 0), ST(F_STAND1 + 5, DOG_STAND1 + 6, A_STAND, 0),
	ST(F_STAND1 + 6, DOG_STAND1 + 7, A_STAND, 0), ST(F_STAND1 + 7, DOG_STAND1 + 8, A_STAND, 0),
	ST(F_STAND1 + 8, DOG_STAND1, A_STAND, 0),
	/* dog_walk */
	ST(F_WALK1, DOG_WALK1 + 1, A_CUSTOM, C_WALK1),
	ST(F_WALK1 + 1, DOG_WALK1 + 2, A_WALK, 8),    ST(F_WALK1 + 2, DOG_WALK1 + 3, A_WALK, 8),
	ST(F_WALK1 + 3, DOG_WALK1 + 4, A_WALK, 8),    ST(F_WALK1 + 4, DOG_WALK1 + 5, A_WALK, 8),
	ST(F_WALK1 + 5, DOG_WALK1 + 6, A_WALK, 8),    ST(F_WALK1 + 6, DOG_WALK1 + 7, A_WALK, 8),
	ST(F_WALK1 + 7, DOG_WALK1, A_WALK, 8),
	/* dog_run */
	ST(F_RUN1, DOG_RUN1 + 1, A_CUSTOM, C_RUN1),
	ST(F_RUN1 + 1, DOG_RUN1 + 2, A_RUN, 32),      ST(F_RUN1 + 2, DOG_RUN1 + 3, A_RUN, 32),
	ST(F_RUN1 + 3, DOG_RUN1 + 4, A_RUN, 20),      ST(F_RUN1 + 4, DOG_RUN1 + 5, A_RUN, 64),
	ST(F_RUN1 + 5, DOG_RUN1 + 6, A_RUN, 32),      ST(F_RUN1 + 6, DOG_RUN1 + 7, A_RUN, 16),
	ST(F_RUN1 + 7, DOG_RUN1 + 8, A_RUN, 32),      ST(F_RUN1 + 8, DOG_RUN1 + 9, A_RUN, 32),
	ST(F_RUN1 + 9, DOG_RUN1 + 10, A_RUN, 20),     ST(F_RUN1 + 10, DOG_RUN1 + 11, A_RUN, 64),
	ST(F_RUN1 + 11, DOG_RUN1, A_RUN, 32),
	/* dog_atta */
	ST(F_ATTACK1, DOG_ATTA1 + 1, A_CHARGE, 10),   ST(F_ATTACK1 + 1, DOG_ATTA1 + 2, A_CHARGE, 10),
	ST(F_ATTACK1 + 2, DOG_ATTA1 + 3, A_CHARGE, 10),
	ST(F_ATTACK1 + 3, DOG_ATTA1 + 4, A_CUSTOM, C_ATTA4),
	ST(F_ATTACK1 + 4, DOG_ATTA1 + 5, A_CHARGE, 10), ST(F_ATTACK1 + 5, DOG_ATTA1 + 6, A_CHARGE, 10),
	ST(F_ATTACK1 + 6, DOG_ATTA1 + 7, A_CHARGE, 10), ST(F_ATTACK1 + 7, DOG_RUN1, A_CHARGE, 10),
	/* dog_leap */
	ST(F_LEAP1, DOG_LEAP1 + 1, A_FACE, 0),
	ST(F_LEAP1 + 1, DOG_LEAP1 + 2, A_CUSTOM, C_LEAP2),
	ST(F_LEAP1 + 2, DOG_LEAP1 + 3, A_NONE, 0),    ST(F_LEAP1 + 3, DOG_LEAP1 + 4, A_NONE, 0),
	ST(F_LEAP1 + 4, DOG_LEAP1 + 5, A_NONE, 0),    ST(F_LEAP1 + 5, DOG_LEAP1 + 6, A_NONE, 0),
	ST(F_LEAP1 + 6, DOG_LEAP1 + 7, A_NONE, 0),    ST(F_LEAP1 + 7, DOG_LEAP1 + 8, A_NONE, 0),
	ST(F_LEAP1 + 8, DOG_LEAP1 + 8, A_NONE, 0),
	/* dog_pain */
	ST(F_PAIN1, DOG_PAIN1 + 1, A_NONE, 0),        ST(F_PAIN1 + 1, DOG_PAIN1 + 2, A_NONE, 0),
	ST(F_PAIN1 + 2, DOG_PAIN1 + 3, A_NONE, 0),    ST(F_PAIN1 + 3, DOG_PAIN1 + 4, A_NONE, 0),
	ST(F_PAIN1 + 4, DOG_PAIN1 + 5, A_NONE, 0),    ST(F_PAIN1 + 5, DOG_RUN1, A_NONE, 0),
	/* dog_painb */
	ST(F_PAINB1, DOG_PAINB1 + 1, A_NONE, 0),      ST(F_PAINB1 + 1, DOG_PAINB1 + 2, A_NONE, 0),
	ST(F_PAINB1 + 2, DOG_PAINB1 + 3, A_PAIN, 4),  ST(F_PAINB1 + 3, DOG_PAINB1 + 4, A_PAIN, 12),
	ST(F_PAINB1 + 4, DOG_PAINB1 + 5, A_PAIN, 12), ST(F_PAINB1 + 5, DOG_PAINB1 + 6, A_PAIN, 2),
	ST(F_PAINB1 + 6, DOG_PAINB1 + 7, A_NONE, 0),  ST(F_PAINB1 + 7, DOG_PAINB1 + 8, A_PAIN, 4),
	ST(F_PAINB1 + 8, DOG_PAINB1 + 9, A_NONE, 0),  ST(F_PAINB1 + 9, DOG_PAINB1 + 10, A_PAIN, 10),
	ST(F_PAINB1 + 10, DOG_PAINB1 + 11, A_NONE, 0), ST(F_PAINB1 + 11, DOG_PAINB1 + 12, A_NONE, 0),
	ST(F_PAINB1 + 12, DOG_PAINB1 + 13, A_NONE, 0), ST(F_PAINB1 + 13, DOG_PAINB1 + 14, A_NONE, 0),
	ST(F_PAINB1 + 14, DOG_PAINB1 + 15, A_NONE, 0), ST(F_PAINB1 + 15, DOG_RUN1, A_NONE, 0),
	/* dog_die */
	ST(F_DEATH1, DOG_DIE1 + 1, A_NONE, 0),        ST(F_DEATH1 + 1, DOG_DIE1 + 2, A_NONE, 0),
	ST(F_DEATH1 + 2, DOG_DIE1 + 3, A_NONE, 0),    ST(F_DEATH1 + 3, DOG_DIE1 + 4, A_NONE, 0),
	ST(F_DEATH1 + 4, DOG_DIE1 + 5, A_NONE, 0),    ST(F_DEATH1 + 5, DOG_DIE1 + 6, A_NONE, 0),
	ST(F_DEATH1 + 6, DOG_DIE1 + 7, A_NONE, 0),    ST(F_DEATH1 + 7, DOG_DIE1 + 8, A_NONE, 0),
	ST(F_DEATH1 + 8, DOG_DIE1 + 8, A_NONE, 0),
	/* dog_dieb */
	ST(F_DEATHB1, DOG_DIEB1 + 1, A_NONE, 0),      ST(F_DEATHB1 + 1, DOG_DIEB1 + 2, A_NONE, 0),
	ST(F_DEATHB1 + 2, DOG_DIEB1 + 3, A_NONE, 0),  ST(F_DEATHB1 + 3, DOG_DIEB1 + 4, A_NONE, 0),
	ST(F_DEATHB1 + 4, DOG_DIEB1 + 5, A_NONE, 0),  ST(F_DEATHB1 + 5, DOG_DIEB1 + 6, A_NONE, 0),
	ST(F_DEATHB1 + 6, DOG_DIEB1 + 7, A_NONE, 0),  ST(F_DEATHB1 + 7, DOG_DIEB1 + 8, A_NONE, 0),
	ST(F_DEATHB1 + 8, DOG_DIEB1 + 8, A_NONE, 0),
};

static int16_t s_dattack1, s_ddeath, s_dpain1, s_dsight, s_idle, s_udeath;

static void dog_map_start(void)
{
	s_dattack1 = (int16_t) snd_find("dog/dattack1.wav");
	s_ddeath   = (int16_t) snd_find("dog/ddeath.wav");
	s_dpain1   = (int16_t) snd_find("dog/dpain1.wav");
	s_dsight   = (int16_t) snd_find("dog/dsight.wav");
	s_idle     = (int16_t) snd_find("dog/idle.wav");
	s_udeath   = (int16_t) snd_find("player/udeath.wav");
}

static void dog_stand(edict_t *self) { monster_state(self, DOG_STAND1); }
static void dog_walk(edict_t *self)  { monster_state(self, DOG_WALK1); }
static void dog_run(edict_t *self)   { monster_state(self, DOG_RUN1); }
static void dog_atta(edict_t *self)  { monster_state(self, DOG_ATTA1); }
static void dog_leap(edict_t *self)  { monster_state(self, DOG_LEAP1); }

/* dog_bite: (random () + random () + random ()) x 8 within 100 units */
static void dog_bite(edict_t *self)
{
	if (!self->enemy)
		return;
	ai_charge(self, 10);
	if (!ai_can_damage(self->enemy, self))
		return;
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	if (ai_vlen2(d) > AI_UNITS2(100))
		return;
	ent_damage_x256(self->enemy, self, self, (int) ((frand() + frand() + frand()) * 8 * 256 >> 16));
}

/* Dog_JumpTouch: what the leap meets hurt if it flew fast, then run on
   once on the ground (leap again if stuck on an edge) */
void dog_jump_touch(edict_t *self)
{
	if (self->health <= 0)
		return;
	edict_t *other = ent_other;
	if (other && other->takedamage) {
		const int32_t *v = self->velocity;
		if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > 300 * 300)   /* (capped at 2000: 32 bits) */
			ent_damage_x256(other, self, self, 10 * 256 + (int) (frand() * 10 * 256 >> 16));
	}
	if (!ai_check_bottom(self)) {
		if (self->flags & FL_ONGROUND) {  /* jump randomly to not get hung up */
			self->touch = EF_sub_null;
			monster_think_at(self, DOG_LEAP1, TICKS(10));
		}
		return;                           /* not on ground yet */
	}
	self->touch = EF_sub_null;
	monster_think_at(self, DOG_RUN1, TICKS(10));
}

static void dog_custom(edict_t *self, int code)
{
	switch (code) {
	case C_WALK1:
		if (frand() < 13107)             /* random () < 0.2 */
			ent_sound(self, CHAN_VOICE, s_idle, 255, ATTN_IDLE);
		ai_walk(self, 8);
		break;
	case C_RUN1:
		if (frand() < 13107)
			ent_sound(self, CHAN_VOICE, s_idle, 255, ATTN_IDLE);
		ai_run(self, 16);
		break;
	case C_ATTA4:
		ent_sound(self, CHAN_VOICE, s_dattack1, 255, ATTN_NORM);
		dog_bite(self);
		break;
	case C_LEAP2: {
		/* (1.06: makevectors with the angles as they are) */
		int32_t f[3], r[3], u[3];
		ai_face(self);
		self->touch = EF_dog_jump_touch;
		ai_makevectors(self->ang, f, r, u);
		self->origin[2] += SUB;
		for (int k = 0; k < 3; k++)
			self->velocity[k] = f[k] * 300 / 4096;
		self->velocity[2] += 200;
		self->flags &= ~FL_ONGROUND;
		break;
	}
	}
}

/* CheckDogJump: the enemy's height within the dog's reach, 80 to 150
   units away across */
static int dog_check_jump(edict_t *self)
{
	const edict_t *en = self->enemy;
	int32_t size = en->maxs[2] - en->mins[2];
	if (self->origin[2] + self->mins[2] > en->origin[2] + en->mins[2] + size * 3 / 4)
		return 0;
	if (self->origin[2] + self->maxs[2] < en->origin[2] + en->mins[2] + size / 4)
		return 0;
	int32_t d[3] = { en->origin[0] - self->origin[0], en->origin[1] - self->origin[1], 0 };
	int64_t d2 = ai_vlen2(d);
	if (d2 < AI_UNITS2(80))
		return 0;
	if (d2 > AI_UNITS2(150))
		return 0;
	return 1;
}

/* DogCheckAttack: a bite if close enough for slashing (CheckDogMelee),
   else a leap */
static int dog_check_attack(edict_t *self)
{
	if (ai_enemy_range == RANGE_MELEE) {
		self->attack_state = AS_MELEE;
		return 1;
	}
	if (dog_check_jump(self)) {
		self->attack_state = AS_MISSILE;
		return 1;
	}
	return 0;
}

void dog_pain(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, s_dpain1, 255, ATTN_NORM);
	if (frand() > 32768)                 /* random () > 0.5 */
		monster_state(self, DOG_PAIN1);
	else
		monster_state(self, DOG_PAINB1);
}

void dog_die(edict_t *self)
{
	if (self->health < -35) {            /* check for gib */
		ent_sound(self, CHAN_VOICE, s_udeath, 255, ATTN_NORM);
		throw_gib(self, 2, self->health);
		throw_gib(self, 2, self->health);
		throw_gib(self, 2, self->health);
		throw_head(self, self->health);
		return;
	}
	/* regular death */
	ent_sound(self, CHAN_VOICE, s_ddeath, 255, ATTN_NORM);
	self->solid = SOLID_NOT;
	ent_link(self);
	if (frand() > 32768)
		monster_state(self, DOG_DIE1);
	else
		monster_state(self, DOG_DIEB1);
}

const mdef_t mdef_dog = {
	.classname = "monster_dog",
	.assets = { "progs/dog.mdl", "progs/h_dog.mdl" },
	.mins = { -32, -32, -24 }, .maxs = { 32, 32, 40 },
	.health = 25,
	.kind = MK_DOG,
	.states = states, .num_states = DOG_COUNT,
	.th_stand = dog_stand, .th_walk = dog_walk, .th_run = dog_run,
	.th_missile = dog_leap, .th_melee = dog_atta,
	.th_pain = EF_dog_pain, .th_die = EF_dog_die,
	.custom = dog_custom,
	.check_attack = dog_check_attack,
	.map_start = dog_map_start,
	.sight = &s_dsight,
};
