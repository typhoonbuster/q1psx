/*
 * q1psx — the rotfish (monster_fish): fish.qc, the 1.06 progs.dat's (the
 * registered game's; the rerelease's rotfish.qc for the frames' names),
 * transcribed state by state: a swimmer (swimmonster_start: engine/
 * monsters.c), its bite (fish_melee). See engine/monsters.h.
 */

#include "game.h"
#include "monsters.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: attack1-18, death1-21, swim1-18, pain1-9 */
enum { F_ATTACK1 = 0, F_DEATH1 = 18, F_SWIM1 = 39, F_PAIN1 = 57 };

enum {
	FISH_STAND1 = 0, FISH_WALK1 = 18, FISH_RUN1 = 36, FISH_ATTACK1 = 45, FISH_DEATH1 = 63, FISH_PAIN1 = 84,
	FISH_COUNT = 93
};

enum { C_RUN1, C_MELEE, C_DEATH1, C_DEATH21 };

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[FISH_COUNT] = {
	/* f_stand1-18 */
	ST(F_SWIM1 + 0,  FISH_STAND1 + 1,  A_STAND, 0), ST(F_SWIM1 + 1,  FISH_STAND1 + 2,  A_STAND, 0),
	ST(F_SWIM1 + 2,  FISH_STAND1 + 3,  A_STAND, 0), ST(F_SWIM1 + 3,  FISH_STAND1 + 4,  A_STAND, 0),
	ST(F_SWIM1 + 4,  FISH_STAND1 + 5,  A_STAND, 0), ST(F_SWIM1 + 5,  FISH_STAND1 + 6,  A_STAND, 0),
	ST(F_SWIM1 + 6,  FISH_STAND1 + 7,  A_STAND, 0), ST(F_SWIM1 + 7,  FISH_STAND1 + 8,  A_STAND, 0),
	ST(F_SWIM1 + 8,  FISH_STAND1 + 9,  A_STAND, 0), ST(F_SWIM1 + 9,  FISH_STAND1 + 10, A_STAND, 0),
	ST(F_SWIM1 + 10, FISH_STAND1 + 11, A_STAND, 0), ST(F_SWIM1 + 11, FISH_STAND1 + 12, A_STAND, 0),
	ST(F_SWIM1 + 12, FISH_STAND1 + 13, A_STAND, 0), ST(F_SWIM1 + 13, FISH_STAND1 + 14, A_STAND, 0),
	ST(F_SWIM1 + 14, FISH_STAND1 + 15, A_STAND, 0), ST(F_SWIM1 + 15, FISH_STAND1 + 16, A_STAND, 0),
	ST(F_SWIM1 + 16, FISH_STAND1 + 17, A_STAND, 0), ST(F_SWIM1 + 17, FISH_STAND1 + 0,  A_STAND, 0),
	/* f_walk1-18 */
	ST(F_SWIM1 + 0,  FISH_WALK1 + 1,  A_WALK, 8), ST(F_SWIM1 + 1,  FISH_WALK1 + 2,  A_WALK, 8),
	ST(F_SWIM1 + 2,  FISH_WALK1 + 3,  A_WALK, 8), ST(F_SWIM1 + 3,  FISH_WALK1 + 4,  A_WALK, 8),
	ST(F_SWIM1 + 4,  FISH_WALK1 + 5,  A_WALK, 8), ST(F_SWIM1 + 5,  FISH_WALK1 + 6,  A_WALK, 8),
	ST(F_SWIM1 + 6,  FISH_WALK1 + 7,  A_WALK, 8), ST(F_SWIM1 + 7,  FISH_WALK1 + 8,  A_WALK, 8),
	ST(F_SWIM1 + 8,  FISH_WALK1 + 9,  A_WALK, 8), ST(F_SWIM1 + 9,  FISH_WALK1 + 10, A_WALK, 8),
	ST(F_SWIM1 + 10, FISH_WALK1 + 11, A_WALK, 8), ST(F_SWIM1 + 11, FISH_WALK1 + 12, A_WALK, 8),
	ST(F_SWIM1 + 12, FISH_WALK1 + 13, A_WALK, 8), ST(F_SWIM1 + 13, FISH_WALK1 + 14, A_WALK, 8),
	ST(F_SWIM1 + 14, FISH_WALK1 + 15, A_WALK, 8), ST(F_SWIM1 + 15, FISH_WALK1 + 16, A_WALK, 8),
	ST(F_SWIM1 + 16, FISH_WALK1 + 17, A_WALK, 8), ST(F_SWIM1 + 17, FISH_WALK1 + 0,  A_WALK, 8),
	/* f_run1-9: every other swim frame */
	ST(F_SWIM1 + 0,  FISH_RUN1 + 1, A_CUSTOM, C_RUN1),
	ST(F_SWIM1 + 2,  FISH_RUN1 + 2, A_RUN, 12), ST(F_SWIM1 + 4,  FISH_RUN1 + 3, A_RUN, 12),
	ST(F_SWIM1 + 6,  FISH_RUN1 + 4, A_RUN, 12), ST(F_SWIM1 + 8,  FISH_RUN1 + 5, A_RUN, 12),
	ST(F_SWIM1 + 10, FISH_RUN1 + 6, A_RUN, 12), ST(F_SWIM1 + 12, FISH_RUN1 + 7, A_RUN, 12),
	ST(F_SWIM1 + 14, FISH_RUN1 + 8, A_RUN, 12), ST(F_SWIM1 + 16, FISH_RUN1 + 0, A_RUN, 12),
	/* f_attack1-18 (its th_melee): bites at 3, 9 and 15 */
	ST(F_ATTACK1 + 0,  FISH_ATTACK1 + 1,  A_CHARGE, 10), ST(F_ATTACK1 + 1,  FISH_ATTACK1 + 2,  A_CHARGE, 10),
	ST(F_ATTACK1 + 2,  FISH_ATTACK1 + 3,  A_CUSTOM, C_MELEE),
	ST(F_ATTACK1 + 3,  FISH_ATTACK1 + 4,  A_CHARGE, 10), ST(F_ATTACK1 + 4,  FISH_ATTACK1 + 5,  A_CHARGE, 10),
	ST(F_ATTACK1 + 5,  FISH_ATTACK1 + 6,  A_CHARGE, 10), ST(F_ATTACK1 + 6,  FISH_ATTACK1 + 7,  A_CHARGE, 10),
	ST(F_ATTACK1 + 7,  FISH_ATTACK1 + 8,  A_CHARGE, 10),
	ST(F_ATTACK1 + 8,  FISH_ATTACK1 + 9,  A_CUSTOM, C_MELEE),
	ST(F_ATTACK1 + 9,  FISH_ATTACK1 + 10, A_CHARGE, 10), ST(F_ATTACK1 + 10, FISH_ATTACK1 + 11, A_CHARGE, 10),
	ST(F_ATTACK1 + 11, FISH_ATTACK1 + 12, A_CHARGE, 10), ST(F_ATTACK1 + 12, FISH_ATTACK1 + 13, A_CHARGE, 10),
	ST(F_ATTACK1 + 13, FISH_ATTACK1 + 14, A_CHARGE, 10),
	ST(F_ATTACK1 + 14, FISH_ATTACK1 + 15, A_CUSTOM, C_MELEE),
	ST(F_ATTACK1 + 15, FISH_ATTACK1 + 16, A_CHARGE, 10), ST(F_ATTACK1 + 16, FISH_ATTACK1 + 17, A_CHARGE, 10),
	ST(F_ATTACK1 + 17, FISH_RUN1,         A_CHARGE, 10),
	/* f_death1-21: non-solid at the last */
	ST(F_DEATH1 + 0,  FISH_DEATH1 + 1,  A_CUSTOM, C_DEATH1),
	ST(F_DEATH1 + 1,  FISH_DEATH1 + 2,  A_NONE, 0), ST(F_DEATH1 + 2,  FISH_DEATH1 + 3,  A_NONE, 0),
	ST(F_DEATH1 + 3,  FISH_DEATH1 + 4,  A_NONE, 0), ST(F_DEATH1 + 4,  FISH_DEATH1 + 5,  A_NONE, 0),
	ST(F_DEATH1 + 5,  FISH_DEATH1 + 6,  A_NONE, 0), ST(F_DEATH1 + 6,  FISH_DEATH1 + 7,  A_NONE, 0),
	ST(F_DEATH1 + 7,  FISH_DEATH1 + 8,  A_NONE, 0), ST(F_DEATH1 + 8,  FISH_DEATH1 + 9,  A_NONE, 0),
	ST(F_DEATH1 + 9,  FISH_DEATH1 + 10, A_NONE, 0), ST(F_DEATH1 + 10, FISH_DEATH1 + 11, A_NONE, 0),
	ST(F_DEATH1 + 11, FISH_DEATH1 + 12, A_NONE, 0), ST(F_DEATH1 + 12, FISH_DEATH1 + 13, A_NONE, 0),
	ST(F_DEATH1 + 13, FISH_DEATH1 + 14, A_NONE, 0), ST(F_DEATH1 + 14, FISH_DEATH1 + 15, A_NONE, 0),
	ST(F_DEATH1 + 15, FISH_DEATH1 + 16, A_NONE, 0), ST(F_DEATH1 + 16, FISH_DEATH1 + 17, A_NONE, 0),
	ST(F_DEATH1 + 17, FISH_DEATH1 + 18, A_NONE, 0), ST(F_DEATH1 + 18, FISH_DEATH1 + 19, A_NONE, 0),
	ST(F_DEATH1 + 19, FISH_DEATH1 + 20, A_NONE, 0),
	ST(F_DEATH1 + 20, FISH_DEATH1 + 20, A_CUSTOM, C_DEATH21),
	/* f_pain1-9 */
	ST(F_PAIN1 + 0, FISH_PAIN1 + 1, A_NONE, 0),  ST(F_PAIN1 + 1, FISH_PAIN1 + 2, A_PAIN, 6),
	ST(F_PAIN1 + 2, FISH_PAIN1 + 3, A_PAIN, 6),  ST(F_PAIN1 + 3, FISH_PAIN1 + 4, A_PAIN, 6),
	ST(F_PAIN1 + 4, FISH_PAIN1 + 5, A_PAIN, 6),  ST(F_PAIN1 + 5, FISH_PAIN1 + 6, A_PAIN, 6),
	ST(F_PAIN1 + 6, FISH_PAIN1 + 7, A_PAIN, 6),  ST(F_PAIN1 + 7, FISH_PAIN1 + 8, A_PAIN, 6),
	ST(F_PAIN1 + 8, FISH_RUN1,      A_PAIN, 6),
};

static int16_t s_death, s_bite, s_idle;

static void fish_map_start(void)
{
	s_death = (int16_t) snd_find("fish/death.wav");
	s_bite  = (int16_t) snd_find("fish/bite.wav");
	s_idle  = (int16_t) snd_find("fish/idle.wav");
}

static void fish_stand(edict_t *self) { monster_state(self, FISH_STAND1); }
static void fish_walk(edict_t *self)  { monster_state(self, FISH_WALK1); }
static void fish_run(edict_t *self)   { monster_state(self, FISH_RUN1); }
static void fish_attack(edict_t *self) { monster_state(self, FISH_ATTACK1); }

/* fish_melee: a bite within 60 units, (random () + random ()) x 3 */
static void fish_melee(edict_t *self)
{
	if (!self->enemy)
		return;                          /* removed before stroke */
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	if (ai_vlen2(d) > AI_UNITS2(60))
		return;
	ent_sound(self, CHAN_VOICE, s_bite, 255, ATTN_NORM);
	uint32_t r1 = frand(), r2 = frand();
	ent_damage_x256(self->enemy, self, self, (int) ((r1 + r2) * 3 * 256 >> 16));
}

static void fish_custom(edict_t *self, int code)
{
	switch (code) {
	case C_RUN1:
		ai_run(self, 12);
		if (frand() < 32768)             /* random () < 0.5 */
			ent_sound(self, CHAN_VOICE, s_idle, 255, ATTN_NORM);
		break;
	case C_MELEE:
		fish_melee(self);
		break;
	case C_DEATH1:
		ent_sound(self, CHAN_VOICE, s_death, 255, ATTN_NORM);
		break;
	case C_DEATH21:
		self->solid = SOLID_NOT;
		ent_link(self);
		break;
	}
}

void fish_pain(edict_t *self)
{
	monster_state(self, FISH_PAIN1);
}

void fish_die(edict_t *self)
{
	monster_state(self, FISH_DEATH1);
}

const mdef_t mdef_fish = {
	.classname = "monster_fish",
	.assets = { "progs/fish.mdl" },
	.mins = { -16, -16, -24 }, .maxs = { 16, 16, 24 },
	.health = 25,
	.kind = MK_FISH,
	.fly = MS_SWIM,
	.states = states, .num_states = FISH_COUNT,
	.th_stand = fish_stand, .th_walk = fish_walk, .th_run = fish_run,
	.th_missile = 0, .th_melee = fish_attack,
	.th_pain = EF_fish_pain, .th_die = EF_fish_die,
	.custom = fish_custom,
	.check_attack = 0,
	.map_start = fish_map_start,
	.sight = 0,
};
