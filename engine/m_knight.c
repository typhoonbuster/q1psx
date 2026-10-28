/*
 * q1psx — the knight (monster_knight): knight.qc, the shareware's 1.06,
 * transcribed state by state (1.06's attack skips a frame the rerelease
 * put back: its table's frames are 1.06's); fight.qc's knight_attack.
 * See engine/monsters.h.
 */

#include "game.h"
#include "monsters.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: stand1-9, runb1-8, runattack1-11, pain1-3, painb1-11, attackb1-11, walk1-14, kneel1-5, death1-10, deathb1-11 */
enum {
	F_STAND1 = 0, F_RUNB1 = 9, F_RUNATTACK1 = 17, F_PAIN1 = 28, F_PAINB1 = 31, F_ATTACKB1 = 42,
	F_WALK1 = 53, F_KNEEL1 = 67, F_DEATH1 = 76, F_DEATHB1 = 86
};

enum {
	KNIGHT_STAND1 = 0, KNIGHT_WALK1 = 9, KNIGHT_RUN1 = 23, KNIGHT_RUNATK1 = 31, KNIGHT_ATK1 = 42,
	KNIGHT_PAIN1 = 52, KNIGHT_PAINB1 = 55, KNIGHT_BOW1 = 66, KNIGHT_DIE1 = 76, KNIGHT_DIEB1 = 86,
	KNIGHT_COUNT = 97
};

enum { C_WALK1, C_RUN1, C_RUNATK1, C_ATK1, C_ATK6, C_ATK7, C_ATK8, C_DIE3, C_DIEB3 };

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[KNIGHT_COUNT] = {
	/* knight_stand */
	ST(F_STAND1, KNIGHT_STAND1 + 1, A_STAND, 0),  ST(F_STAND1 + 1, KNIGHT_STAND1 + 2, A_STAND, 0),
	ST(F_STAND1 + 2, KNIGHT_STAND1 + 3, A_STAND, 0), ST(F_STAND1 + 3, KNIGHT_STAND1 + 4, A_STAND, 0),
	ST(F_STAND1 + 4, KNIGHT_STAND1 + 5, A_STAND, 0), ST(F_STAND1 + 5, KNIGHT_STAND1 + 6, A_STAND, 0),
	ST(F_STAND1 + 6, KNIGHT_STAND1 + 7, A_STAND, 0), ST(F_STAND1 + 7, KNIGHT_STAND1 + 8, A_STAND, 0),
	ST(F_STAND1 + 8, KNIGHT_STAND1, A_STAND, 0),
	/* knight_walk */
	ST(F_WALK1, KNIGHT_WALK1 + 1, A_CUSTOM, C_WALK1),
	ST(F_WALK1 + 1, KNIGHT_WALK1 + 2, A_WALK, 2), ST(F_WALK1 + 2, KNIGHT_WALK1 + 3, A_WALK, 3),
	ST(F_WALK1 + 3, KNIGHT_WALK1 + 4, A_WALK, 4), ST(F_WALK1 + 4, KNIGHT_WALK1 + 5, A_WALK, 3),
	ST(F_WALK1 + 5, KNIGHT_WALK1 + 6, A_WALK, 3), ST(F_WALK1 + 6, KNIGHT_WALK1 + 7, A_WALK, 3),
	ST(F_WALK1 + 7, KNIGHT_WALK1 + 8, A_WALK, 4), ST(F_WALK1 + 8, KNIGHT_WALK1 + 9, A_WALK, 3),
	ST(F_WALK1 + 9, KNIGHT_WALK1 + 10, A_WALK, 3), ST(F_WALK1 + 10, KNIGHT_WALK1 + 11, A_WALK, 2),
	ST(F_WALK1 + 11, KNIGHT_WALK1 + 12, A_WALK, 3), ST(F_WALK1 + 12, KNIGHT_WALK1 + 13, A_WALK, 4),
	ST(F_WALK1 + 13, KNIGHT_WALK1, A_WALK, 3),
	/* knight_run */
	ST(F_RUNB1, KNIGHT_RUN1 + 1, A_CUSTOM, C_RUN1),
	ST(F_RUNB1 + 1, KNIGHT_RUN1 + 2, A_RUN, 20),  ST(F_RUNB1 + 2, KNIGHT_RUN1 + 3, A_RUN, 13),
	ST(F_RUNB1 + 3, KNIGHT_RUN1 + 4, A_RUN, 7),   ST(F_RUNB1 + 4, KNIGHT_RUN1 + 5, A_RUN, 16),
	ST(F_RUNB1 + 5, KNIGHT_RUN1 + 6, A_RUN, 20),  ST(F_RUNB1 + 6, KNIGHT_RUN1 + 7, A_RUN, 14),
	ST(F_RUNB1 + 7, KNIGHT_RUN1, A_RUN, 6),
	/* knight_runatk */
	ST(F_RUNATTACK1, KNIGHT_RUNATK1 + 1, A_CUSTOM, C_RUNATK1),
	ST(F_RUNATTACK1 + 1, KNIGHT_RUNATK1 + 2, A_CHARGESIDE, 0),
	ST(F_RUNATTACK1 + 2, KNIGHT_RUNATK1 + 3, A_CHARGESIDE, 0),
	ST(F_RUNATTACK1 + 3, KNIGHT_RUNATK1 + 4, A_CHARGESIDE, 0),
	ST(F_RUNATTACK1 + 4, KNIGHT_RUNATK1 + 5, A_MELEESIDE, 0),
	ST(F_RUNATTACK1 + 5, KNIGHT_RUNATK1 + 6, A_MELEESIDE, 0),
	ST(F_RUNATTACK1 + 6, KNIGHT_RUNATK1 + 7, A_MELEESIDE, 0),
	ST(F_RUNATTACK1 + 7, KNIGHT_RUNATK1 + 8, A_MELEESIDE, 0),
	ST(F_RUNATTACK1 + 8, KNIGHT_RUNATK1 + 9, A_MELEESIDE, 0),
	ST(F_RUNATTACK1 + 9, KNIGHT_RUNATK1 + 10, A_CHARGESIDE, 0), ST(F_RUNATTACK1 + 10, KNIGHT_RUN1, A_CHARGE, 10),
	/* knight_atk */
	ST(F_ATTACKB1, KNIGHT_ATK1 + 1, A_CUSTOM, C_ATK1),
	ST(F_ATTACKB1 + 2, KNIGHT_ATK1 + 2, A_CHARGE, 7), ST(F_ATTACKB1 + 3, KNIGHT_ATK1 + 3, A_CHARGE, 4),
	ST(F_ATTACKB1 + 4, KNIGHT_ATK1 + 4, A_CHARGE, 0), ST(F_ATTACKB1 + 5, KNIGHT_ATK1 + 5, A_CHARGE, 3),
	ST(F_ATTACKB1 + 6, KNIGHT_ATK1 + 6, A_CUSTOM, C_ATK6),
	ST(F_ATTACKB1 + 7, KNIGHT_ATK1 + 7, A_CUSTOM, C_ATK7),
	ST(F_ATTACKB1 + 8, KNIGHT_ATK1 + 8, A_CUSTOM, C_ATK8),
	ST(F_ATTACKB1 + 9, KNIGHT_ATK1 + 9, A_CHARGE, 1), ST(F_ATTACKB1 + 10, KNIGHT_RUN1, A_CHARGE, 5),
	/* knight_pain */
	ST(F_PAIN1, KNIGHT_PAIN1 + 1, A_NONE, 0),     ST(F_PAIN1 + 1, KNIGHT_PAIN1 + 2, A_NONE, 0),
	ST(F_PAIN1 + 2, KNIGHT_RUN1, A_NONE, 0),
	/* knight_painb */
	ST(F_PAINB1, KNIGHT_PAINB1 + 1, A_PAINFWD, 0), ST(F_PAINB1 + 1, KNIGHT_PAINB1 + 2, A_PAINFWD, 3),
	ST(F_PAINB1 + 2, KNIGHT_PAINB1 + 3, A_NONE, 0), ST(F_PAINB1 + 3, KNIGHT_PAINB1 + 4, A_NONE, 0),
	ST(F_PAINB1 + 4, KNIGHT_PAINB1 + 5, A_PAINFWD, 2), ST(F_PAINB1 + 5, KNIGHT_PAINB1 + 6, A_PAINFWD, 4),
	ST(F_PAINB1 + 6, KNIGHT_PAINB1 + 7, A_PAINFWD, 2), ST(F_PAINB1 + 7, KNIGHT_PAINB1 + 8, A_PAINFWD, 5),
	ST(F_PAINB1 + 8, KNIGHT_PAINB1 + 9, A_PAINFWD, 5), ST(F_PAINB1 + 9, KNIGHT_PAINB1 + 10, A_PAINFWD, 0),
	ST(F_PAINB1 + 10, KNIGHT_RUN1, A_NONE, 0),
	/* knight_bow */
	ST(F_KNEEL1, KNIGHT_BOW1 + 1, A_TURN, 0),     ST(F_KNEEL1 + 1, KNIGHT_BOW1 + 2, A_TURN, 0),
	ST(F_KNEEL1 + 2, KNIGHT_BOW1 + 3, A_TURN, 0), ST(F_KNEEL1 + 3, KNIGHT_BOW1 + 4, A_TURN, 0),
	ST(F_KNEEL1 + 4, KNIGHT_BOW1 + 4, A_TURN, 0), ST(F_KNEEL1 + 3, KNIGHT_BOW1 + 6, A_TURN, 0),
	ST(F_KNEEL1 + 2, KNIGHT_BOW1 + 7, A_TURN, 0), ST(F_KNEEL1 + 1, KNIGHT_BOW1 + 8, A_TURN, 0),
	ST(F_KNEEL1, KNIGHT_BOW1 + 9, A_TURN, 0),     ST(F_WALK1, KNIGHT_WALK1, A_TURN, 0),
	/* knight_die */
	ST(F_DEATH1, KNIGHT_DIE1 + 1, A_NONE, 0),     ST(F_DEATH1 + 1, KNIGHT_DIE1 + 2, A_NONE, 0),
	ST(F_DEATH1 + 2, KNIGHT_DIE1 + 3, A_CUSTOM, C_DIE3),
	ST(F_DEATH1 + 3, KNIGHT_DIE1 + 4, A_NONE, 0), ST(F_DEATH1 + 4, KNIGHT_DIE1 + 5, A_NONE, 0),
	ST(F_DEATH1 + 5, KNIGHT_DIE1 + 6, A_NONE, 0), ST(F_DEATH1 + 6, KNIGHT_DIE1 + 7, A_NONE, 0),
	ST(F_DEATH1 + 7, KNIGHT_DIE1 + 8, A_NONE, 0), ST(F_DEATH1 + 8, KNIGHT_DIE1 + 9, A_NONE, 0),
	ST(F_DEATH1 + 9, KNIGHT_DIE1 + 9, A_NONE, 0),
	/* knight_dieb */
	ST(F_DEATHB1, KNIGHT_DIEB1 + 1, A_NONE, 0),   ST(F_DEATHB1 + 1, KNIGHT_DIEB1 + 2, A_NONE, 0),
	ST(F_DEATHB1 + 2, KNIGHT_DIEB1 + 3, A_CUSTOM, C_DIEB3),
	ST(F_DEATHB1 + 3, KNIGHT_DIEB1 + 4, A_NONE, 0), ST(F_DEATHB1 + 4, KNIGHT_DIEB1 + 5, A_NONE, 0),
	ST(F_DEATHB1 + 5, KNIGHT_DIEB1 + 6, A_NONE, 0), ST(F_DEATHB1 + 6, KNIGHT_DIEB1 + 7, A_NONE, 0),
	ST(F_DEATHB1 + 7, KNIGHT_DIEB1 + 8, A_NONE, 0), ST(F_DEATHB1 + 8, KNIGHT_DIEB1 + 9, A_NONE, 0),
	ST(F_DEATHB1 + 9, KNIGHT_DIEB1 + 10, A_NONE, 0), ST(F_DEATHB1 + 10, KNIGHT_DIEB1 + 10, A_NONE, 0),
};

static int16_t s_idle, s_sword1, s_sword2, s_khurt, s_kdeath, s_ksight, s_udeath;

static void knight_map_start(void)
{
	s_idle   = (int16_t) snd_find("knight/idle.wav");
	s_sword1 = (int16_t) snd_find("knight/sword1.wav");
	s_sword2 = (int16_t) snd_find("knight/sword2.wav");
	s_khurt  = (int16_t) snd_find("knight/khurt.wav");
	s_kdeath = (int16_t) snd_find("knight/kdeath.wav");
	s_ksight = (int16_t) snd_find("knight/ksight.wav");
	s_udeath = (int16_t) snd_find("player/udeath.wav");
}

static void knight_stand(edict_t *self) { monster_state(self, KNIGHT_STAND1); }
static void knight_walk(edict_t *self)  { monster_state(self, KNIGHT_WALK1); }
static void knight_run(edict_t *self)   { monster_state(self, KNIGHT_RUN1); }

/* knight_attack (fight.qc, CheckAttack's for a knight in place of its
   th_melee, knight_atk1): the slash when close, else the run attack */
static void knight_attack(edict_t *self)
{
	int32_t a[3], b[3], d[3];
	ai_view_org(self->enemy, a);
	ai_view_org(self, b);
	for (int k = 0; k < 3; k++)
		d[k] = a[k] - b[k];
	if (ai_vlen2(d) < AI_UNITS2(80))
		monster_state(self, KNIGHT_ATK1);
	else
		monster_state(self, KNIGHT_RUNATK1);
}

static void knight_custom(edict_t *self, int code)
{
	switch (code) {
	case C_WALK1:
		if (frand() < 13107)             /* random () < 0.2 */
			ent_sound(self, CHAN_VOICE, s_idle, 255, ATTN_IDLE);
		ai_walk(self, 3);
		break;
	case C_RUN1:
		if (frand() < 13107)
			ent_sound(self, CHAN_VOICE, s_idle, 255, ATTN_IDLE);
		ai_run(self, 16);
		break;
	case C_RUNATK1:
		if (frand() > 32768)             /* random () > 0.5 */
			ent_sound(self, CHAN_WEAPON, s_sword2, 255, ATTN_NORM);
		else
			ent_sound(self, CHAN_WEAPON, s_sword1, 255, ATTN_NORM);
		ai_charge(self, 20);
		break;
	case C_ATK1:
		ent_sound(self, CHAN_WEAPON, s_sword1, 255, ATTN_NORM);
		ai_charge(self, 0);
		break;
	case C_ATK6:
		ai_charge(self, 4);
		ai_melee(self);
		break;
	case C_ATK7:
		ai_charge(self, 1);
		ai_melee(self);
		break;
	case C_ATK8:
		ai_charge(self, 3);
		ai_melee(self);
		break;
	case C_DIE3:
	case C_DIEB3:
		self->solid = SOLID_NOT;
		ent_link(self);
		break;
	}
}

void knight_pain(edict_t *self)
{
	if (self->pain_finished > ent_time)
		return;
	uint32_t r = frand();
	ent_sound(self, CHAN_VOICE, s_khurt, 255, ATTN_NORM);
	if (r < 55706) {                     /* 0.85 */
		monster_state(self, KNIGHT_PAIN1);
		self->pain_finished = ent_time + TICKS(100);
	} else {
		monster_state(self, KNIGHT_PAINB1);
		self->pain_finished = ent_time + TICKS(100);
	}
}

void knight_die(edict_t *self)
{
	if (self->health < -40) {            /* check for gib */
		ent_sound(self, CHAN_VOICE, s_udeath, 255, ATTN_NORM);
		throw_head(self, self->health);
		throw_gib(self, 0, self->health);
		throw_gib(self, 1, self->health);
		throw_gib(self, 2, self->health);
		return;
	}
	/* regular death */
	ent_sound(self, CHAN_VOICE, s_kdeath, 255, ATTN_NORM);
	if (frand() < 32768)
		monster_state(self, KNIGHT_DIE1);
	else
		monster_state(self, KNIGHT_DIEB1);
}

const mdef_t mdef_knight = {
	.classname = "monster_knight",
	.assets = { "progs/knight.mdl", "progs/h_knight.mdl" },
	.mins = { -16, -16, -24 }, .maxs = { 16, 16, 40 },
	.health = 75,
	.kind = MK_KNIGHT,
	.states = states, .num_states = KNIGHT_COUNT,
	.th_stand = knight_stand, .th_walk = knight_walk, .th_run = knight_run,
	.th_missile = 0, .th_melee = knight_attack,
	.th_pain = EF_knight_pain, .th_die = EF_knight_die,
	.custom = knight_custom,
	.map_start = knight_map_start,
	.sight = &s_ksight,
};
