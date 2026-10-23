/*
 * q1psx — the ogre (monster_ogre, monster_ogre_marksman): ogre.qc, the
 * shareware's 1.06, transcribed state by state: the chainsaw, the
 * grenade (engine/game.c's projectiles), the meat spray. See
 * engine/monsters.h.
 */

#include "game.h"
#include "monsters.h"
#include "q_fixed.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: stand1-9, walk1-16, run1-8, swing1-14, smash1-14, shoot1-6, pain1-5, painb1-3, painc1-6, paind1-16, paine1-15, death1-14, bdeath1-10, pull1-11 */
enum {
	F_STAND1 = 0, F_WALK1 = 9, F_RUN1 = 25, F_SWING1 = 33, F_SMASH1 = 47, F_SHOOT1 = 61,
	F_PAIN1 = 67, F_PAINB1 = 72, F_PAINC1 = 75, F_PAIND1 = 81, F_PAINE1 = 97, F_DEATH1 = 112,
	F_BDEATH1 = 126, F_PULL1 = 136
};

enum {
	OGRE_STAND1 = 0, OGRE_WALK1 = 9, OGRE_RUN1 = 25, OGRE_SWING1 = 33, OGRE_SMASH1 = 47,
	OGRE_NAIL1 = 61, OGRE_PAIN1 = 68, OGRE_PAINB1 = 73, OGRE_PAINC1 = 76, OGRE_PAIND1 = 82,
	OGRE_PAINE1 = 98, OGRE_DIE1 = 113, OGRE_BDIE1 = 127, OGRE_COUNT = 137
};

enum {
	C_STAND5, C_WALK3, C_WALK6, C_RUN1, C_SWING1, C_SWING5, C_SWING6, C_SWING7, C_SWING8, C_SWING9,
	C_SWING10, C_SWING11, C_SMASH1, C_SMASH6, C_SMASH7, C_SMASH8, C_SMASH9, C_SMASH10, C_SMASH11,
	C_NAIL4, C_DIE3, C_BDIE3
};

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[OGRE_COUNT] = {
	/* ogre_stand */
	ST(F_STAND1, OGRE_STAND1 + 1, A_STAND, 0),    ST(F_STAND1 + 1, OGRE_STAND1 + 2, A_STAND, 0),
	ST(F_STAND1 + 2, OGRE_STAND1 + 3, A_STAND, 0), ST(F_STAND1 + 3, OGRE_STAND1 + 4, A_STAND, 0),
	ST(F_STAND1 + 4, OGRE_STAND1 + 5, A_CUSTOM, C_STAND5),
	ST(F_STAND1 + 5, OGRE_STAND1 + 6, A_STAND, 0), ST(F_STAND1 + 6, OGRE_STAND1 + 7, A_STAND, 0),
	ST(F_STAND1 + 7, OGRE_STAND1 + 8, A_STAND, 0), ST(F_STAND1 + 8, OGRE_STAND1, A_STAND, 0),
	/* ogre_walk */
	ST(F_WALK1, OGRE_WALK1 + 1, A_WALK, 3),       ST(F_WALK1 + 1, OGRE_WALK1 + 2, A_WALK, 2),
	ST(F_WALK1 + 2, OGRE_WALK1 + 3, A_CUSTOM, C_WALK3),
	ST(F_WALK1 + 3, OGRE_WALK1 + 4, A_WALK, 2),   ST(F_WALK1 + 4, OGRE_WALK1 + 5, A_WALK, 2),
	ST(F_WALK1 + 5, OGRE_WALK1 + 6, A_CUSTOM, C_WALK6),
	ST(F_WALK1 + 6, OGRE_WALK1 + 7, A_WALK, 3),   ST(F_WALK1 + 7, OGRE_WALK1 + 8, A_WALK, 2),
	ST(F_WALK1 + 8, OGRE_WALK1 + 9, A_WALK, 3),   ST(F_WALK1 + 9, OGRE_WALK1 + 10, A_WALK, 1),
	ST(F_WALK1 + 10, OGRE_WALK1 + 11, A_WALK, 2), ST(F_WALK1 + 11, OGRE_WALK1 + 12, A_WALK, 3),
	ST(F_WALK1 + 12, OGRE_WALK1 + 13, A_WALK, 3), ST(F_WALK1 + 13, OGRE_WALK1 + 14, A_WALK, 3),
	ST(F_WALK1 + 14, OGRE_WALK1 + 15, A_WALK, 3), ST(F_WALK1 + 15, OGRE_WALK1, A_WALK, 4),
	/* ogre_run */
	ST(F_RUN1, OGRE_RUN1 + 1, A_CUSTOM, C_RUN1),
	ST(F_RUN1 + 1, OGRE_RUN1 + 2, A_RUN, 12),     ST(F_RUN1 + 2, OGRE_RUN1 + 3, A_RUN, 8),
	ST(F_RUN1 + 3, OGRE_RUN1 + 4, A_RUN, 22),     ST(F_RUN1 + 4, OGRE_RUN1 + 5, A_RUN, 16),
	ST(F_RUN1 + 5, OGRE_RUN1 + 6, A_RUN, 4),      ST(F_RUN1 + 6, OGRE_RUN1 + 7, A_RUN, 13),
	ST(F_RUN1 + 7, OGRE_RUN1, A_RUN, 24),
	/* ogre_swing */
	ST(F_SWING1, OGRE_SWING1 + 1, A_CUSTOM, C_SWING1),
	ST(F_SWING1 + 1, OGRE_SWING1 + 2, A_CHARGE, 1), ST(F_SWING1 + 2, OGRE_SWING1 + 3, A_CHARGE, 4),
	ST(F_SWING1 + 3, OGRE_SWING1 + 4, A_CHARGE, 13),
	ST(F_SWING1 + 4, OGRE_SWING1 + 5, A_CUSTOM, C_SWING5),
	ST(F_SWING1 + 5, OGRE_SWING1 + 6, A_CUSTOM, C_SWING6),
	ST(F_SWING1 + 6, OGRE_SWING1 + 7, A_CUSTOM, C_SWING7),
	ST(F_SWING1 + 7, OGRE_SWING1 + 8, A_CUSTOM, C_SWING8),
	ST(F_SWING1 + 8, OGRE_SWING1 + 9, A_CUSTOM, C_SWING9),
	ST(F_SWING1 + 9, OGRE_SWING1 + 10, A_CUSTOM, C_SWING10),
	ST(F_SWING1 + 10, OGRE_SWING1 + 11, A_CUSTOM, C_SWING11),
	ST(F_SWING1 + 11, OGRE_SWING1 + 12, A_CHARGE, 3), ST(F_SWING1 + 12, OGRE_SWING1 + 13, A_CHARGE, 8),
	ST(F_SWING1 + 13, OGRE_RUN1, A_CHARGE, 9),
	/* ogre_smash */
	ST(F_SMASH1, OGRE_SMASH1 + 1, A_CUSTOM, C_SMASH1),
	ST(F_SMASH1 + 1, OGRE_SMASH1 + 2, A_CHARGE, 0), ST(F_SMASH1 + 2, OGRE_SMASH1 + 3, A_CHARGE, 0),
	ST(F_SMASH1 + 3, OGRE_SMASH1 + 4, A_CHARGE, 1), ST(F_SMASH1 + 4, OGRE_SMASH1 + 5, A_CHARGE, 4),
	ST(F_SMASH1 + 5, OGRE_SMASH1 + 6, A_CUSTOM, C_SMASH6),
	ST(F_SMASH1 + 6, OGRE_SMASH1 + 7, A_CUSTOM, C_SMASH7),
	ST(F_SMASH1 + 7, OGRE_SMASH1 + 8, A_CUSTOM, C_SMASH8),
	ST(F_SMASH1 + 8, OGRE_SMASH1 + 9, A_CUSTOM, C_SMASH9),
	ST(F_SMASH1 + 9, OGRE_SMASH1 + 10, A_CUSTOM, C_SMASH10),
	ST(F_SMASH1 + 10, OGRE_SMASH1 + 11, A_CUSTOM, C_SMASH11),
	ST(F_SMASH1 + 11, OGRE_SMASH1 + 12, A_CHARGE, 0), ST(F_SMASH1 + 12, OGRE_SMASH1 + 13, A_CHARGE, 4),
	ST(F_SMASH1 + 13, OGRE_RUN1, A_CHARGE, 12),
	/* ogre_nail */
	ST(F_SHOOT1, OGRE_NAIL1 + 1, A_FACE, 0),      ST(F_SHOOT1 + 1, OGRE_NAIL1 + 2, A_FACE, 0),
	ST(F_SHOOT1 + 1, OGRE_NAIL1 + 3, A_FACE, 0),
	ST(F_SHOOT1 + 2, OGRE_NAIL1 + 4, A_CUSTOM, C_NAIL4),
	ST(F_SHOOT1 + 3, OGRE_NAIL1 + 5, A_FACE, 0),  ST(F_SHOOT1 + 4, OGRE_NAIL1 + 6, A_FACE, 0),
	ST(F_SHOOT1 + 5, OGRE_RUN1, A_FACE, 0),
	/* ogre_pain */
	ST(F_PAIN1, OGRE_PAIN1 + 1, A_NONE, 0),       ST(F_PAIN1 + 1, OGRE_PAIN1 + 2, A_NONE, 0),
	ST(F_PAIN1 + 2, OGRE_PAIN1 + 3, A_NONE, 0),   ST(F_PAIN1 + 3, OGRE_PAIN1 + 4, A_NONE, 0),
	ST(F_PAIN1 + 4, OGRE_RUN1, A_NONE, 0),
	/* ogre_painb */
	ST(F_PAINB1, OGRE_PAINB1 + 1, A_NONE, 0),     ST(F_PAINB1 + 1, OGRE_PAINB1 + 2, A_NONE, 0),
	ST(F_PAINB1 + 2, OGRE_RUN1, A_NONE, 0),
	/* ogre_painc */
	ST(F_PAINC1, OGRE_PAINC1 + 1, A_NONE, 0),     ST(F_PAINC1 + 1, OGRE_PAINC1 + 2, A_NONE, 0),
	ST(F_PAINC1 + 2, OGRE_PAINC1 + 3, A_NONE, 0), ST(F_PAINC1 + 3, OGRE_PAINC1 + 4, A_NONE, 0),
	ST(F_PAINC1 + 4, OGRE_PAINC1 + 5, A_NONE, 0), ST(F_PAINC1 + 5, OGRE_RUN1, A_NONE, 0),
	/* ogre_paind */
	ST(F_PAIND1, OGRE_PAIND1 + 1, A_NONE, 0),     ST(F_PAIND1 + 1, OGRE_PAIND1 + 2, A_PAIN, 10),
	ST(F_PAIND1 + 2, OGRE_PAIND1 + 3, A_PAIN, 9), ST(F_PAIND1 + 3, OGRE_PAIND1 + 4, A_PAIN, 4),
	ST(F_PAIND1 + 4, OGRE_PAIND1 + 5, A_NONE, 0), ST(F_PAIND1 + 5, OGRE_PAIND1 + 6, A_NONE, 0),
	ST(F_PAIND1 + 6, OGRE_PAIND1 + 7, A_NONE, 0), ST(F_PAIND1 + 7, OGRE_PAIND1 + 8, A_NONE, 0),
	ST(F_PAIND1 + 8, OGRE_PAIND1 + 9, A_NONE, 0), ST(F_PAIND1 + 9, OGRE_PAIND1 + 10, A_NONE, 0),
	ST(F_PAIND1 + 10, OGRE_PAIND1 + 11, A_NONE, 0), ST(F_PAIND1 + 11, OGRE_PAIND1 + 12, A_NONE, 0),
	ST(F_PAIND1 + 12, OGRE_PAIND1 + 13, A_NONE, 0), ST(F_PAIND1 + 13, OGRE_PAIND1 + 14, A_NONE, 0),
	ST(F_PAIND1 + 14, OGRE_PAIND1 + 15, A_NONE, 0), ST(F_PAIND1 + 15, OGRE_RUN1, A_NONE, 0),
	/* ogre_paine */
	ST(F_PAINE1, OGRE_PAINE1 + 1, A_NONE, 0),     ST(F_PAINE1 + 1, OGRE_PAINE1 + 2, A_PAIN, 10),
	ST(F_PAINE1 + 2, OGRE_PAINE1 + 3, A_PAIN, 9), ST(F_PAINE1 + 3, OGRE_PAINE1 + 4, A_PAIN, 4),
	ST(F_PAINE1 + 4, OGRE_PAINE1 + 5, A_NONE, 0), ST(F_PAINE1 + 5, OGRE_PAINE1 + 6, A_NONE, 0),
	ST(F_PAINE1 + 6, OGRE_PAINE1 + 7, A_NONE, 0), ST(F_PAINE1 + 7, OGRE_PAINE1 + 8, A_NONE, 0),
	ST(F_PAINE1 + 8, OGRE_PAINE1 + 9, A_NONE, 0), ST(F_PAINE1 + 9, OGRE_PAINE1 + 10, A_NONE, 0),
	ST(F_PAINE1 + 10, OGRE_PAINE1 + 11, A_NONE, 0), ST(F_PAINE1 + 11, OGRE_PAINE1 + 12, A_NONE, 0),
	ST(F_PAINE1 + 12, OGRE_PAINE1 + 13, A_NONE, 0), ST(F_PAINE1 + 13, OGRE_PAINE1 + 14, A_NONE, 0),
	ST(F_PAINE1 + 14, OGRE_RUN1, A_NONE, 0),
	/* ogre_die */
	ST(F_DEATH1, OGRE_DIE1 + 1, A_NONE, 0),       ST(F_DEATH1 + 1, OGRE_DIE1 + 2, A_NONE, 0),
	ST(F_DEATH1 + 2, OGRE_DIE1 + 3, A_CUSTOM, C_DIE3),
	ST(F_DEATH1 + 3, OGRE_DIE1 + 4, A_NONE, 0),   ST(F_DEATH1 + 4, OGRE_DIE1 + 5, A_NONE, 0),
	ST(F_DEATH1 + 5, OGRE_DIE1 + 6, A_NONE, 0),   ST(F_DEATH1 + 6, OGRE_DIE1 + 7, A_NONE, 0),
	ST(F_DEATH1 + 7, OGRE_DIE1 + 8, A_NONE, 0),   ST(F_DEATH1 + 8, OGRE_DIE1 + 9, A_NONE, 0),
	ST(F_DEATH1 + 9, OGRE_DIE1 + 10, A_NONE, 0),  ST(F_DEATH1 + 10, OGRE_DIE1 + 11, A_NONE, 0),
	ST(F_DEATH1 + 11, OGRE_DIE1 + 12, A_NONE, 0), ST(F_DEATH1 + 12, OGRE_DIE1 + 13, A_NONE, 0),
	ST(F_DEATH1 + 13, OGRE_DIE1 + 13, A_NONE, 0),
	/* ogre_bdie */
	ST(F_BDEATH1, OGRE_BDIE1 + 1, A_NONE, 0),     ST(F_BDEATH1 + 1, OGRE_BDIE1 + 2, A_FORWARD, 5),
	ST(F_BDEATH1 + 2, OGRE_BDIE1 + 3, A_CUSTOM, C_BDIE3),
	ST(F_BDEATH1 + 3, OGRE_BDIE1 + 4, A_FORWARD, 1), ST(F_BDEATH1 + 4, OGRE_BDIE1 + 5, A_FORWARD, 3),
	ST(F_BDEATH1 + 5, OGRE_BDIE1 + 6, A_FORWARD, 7), ST(F_BDEATH1 + 6, OGRE_BDIE1 + 7, A_FORWARD, 25),
	ST(F_BDEATH1 + 7, OGRE_BDIE1 + 8, A_NONE, 0), ST(F_BDEATH1 + 8, OGRE_BDIE1 + 9, A_NONE, 0),
	ST(F_BDEATH1 + 9, OGRE_BDIE1 + 9, A_NONE, 0),
};

static int16_t s_ogdrag, s_ogdth, s_ogidle, s_ogidle2, s_ogpain1, s_ogsawatk, s_ogwake, s_grenade, s_udeath;

static void ogre_map_start(void)
{
	s_ogdrag   = (int16_t) snd_find("ogre/ogdrag.wav");
	s_ogdth    = (int16_t) snd_find("ogre/ogdth.wav");
	s_ogidle   = (int16_t) snd_find("ogre/ogidle.wav");
	s_ogidle2  = (int16_t) snd_find("ogre/ogidle2.wav");
	s_ogpain1  = (int16_t) snd_find("ogre/ogpain1.wav");
	s_ogsawatk = (int16_t) snd_find("ogre/ogsawatk.wav");
	s_ogwake   = (int16_t) snd_find("ogre/ogwake.wav");
	s_grenade  = (int16_t) snd_find("weapons/grenade.wav");
	s_udeath   = (int16_t) snd_find("player/udeath.wav");
}

static void ogre_stand(edict_t *self) { monster_state(self, OGRE_STAND1); }
static void ogre_walk(edict_t *self)  { monster_state(self, OGRE_WALK1); }
static void ogre_run(edict_t *self)   { monster_state(self, OGRE_RUN1); }
static void ogre_nail(edict_t *self)  { monster_state(self, OGRE_NAIL1); }

/* ogre_melee: the smash or the swing */
static void ogre_melee(edict_t *self)
{
	if (frand() > 32768)                 /* random () > 0.5 */
		monster_state(self, OGRE_SMASH1);
	else
		monster_state(self, OGRE_SWING1);
}

/* OgreFireGrenade: at the enemy, 600 across, 200 up (the muzzle flash) */
static void ogre_fire_grenade(edict_t *self)
{
	game_muzzle_flash(ENT_EDICT + (int) (self - edicts), self->origin, self->ang[1] >> 4);   /* EF_MUZZLEFLASH */
	ent_sound(self, CHAN_WEAPON, s_grenade, 255, ATTN_NORM);
	int32_t d[3], v[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	int32_t len = (int32_t) q_isqrt64((uint64_t) ai_vlen2(d));
	for (int k = 0; k < 3; k++)
		v[k] = len ? d[k] * 600 / len : 0;
	v[2] = 200;
	game_monster_missile(MM_GRENADE, self, self->origin, v, -1, SND_NONE, SND_NONE);
}

/* chainsaw: (random () + random () + random ()) x 4 within 100 units; a
   side swing sprays meat (side 1: at random) */
static void chainsaw(edict_t *self, int side)
{
	if (!self->enemy)
		return;
	if (!ai_can_damage(self->enemy, self))
		return;
	ai_charge(self, 10);
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	if (ai_vlen2(d) > AI_UNITS2(100))
		return;
	ent_damage_x256(self->enemy, self, self, (int) ((frand() + frand() + frand()) * 4 * 256 >> 16));
	if (side) {
		int32_t f[3], r[3], u[3], org[3], vel[3];
		ai_makevectors(self->ang, f, r, u);
		for (int k = 0; k < 3; k++)
			org[k] = self->origin[k] + 16 * SUB * f[k] / 4096;
		if (side == 1) {
			int32_t c = crand();              /* crandom () x 100 x v_right */
			for (int k = 0; k < 3; k++)
				vel[k] = (int32_t) ((int64_t) c * 100 * r[k] >> 28);
		} else {
			for (int k = 0; k < 3; k++)
				vel[k] = side * r[k] / 4096;
		}
		spawn_meat_spray(self, org, vel);
	}
}

/* the swing's turn: angles_y + random () x 25 */
static void swing_turn(edict_t *self)
{
	self->ang[1] = (uint16_t) (self->ang[1] + frand() * 25 / 360);
}

static void ogre_custom(edict_t *self, int code)
{
	switch (code) {
	case C_STAND5:
		if (frand() < 13107)             /* random () < 0.2 */
			ent_sound(self, CHAN_VOICE, s_ogidle, 255, ATTN_IDLE);
		ai_stand(self);
		break;
	case C_WALK3:
		ai_walk(self, 2);
		if (frand() < 13107)
			ent_sound(self, CHAN_VOICE, s_ogidle, 255, ATTN_IDLE);
		break;
	case C_WALK6:
		ai_walk(self, 5);
		if (frand() < 6554)              /* 0.1 */
			ent_sound(self, CHAN_VOICE, s_ogdrag, 255, ATTN_IDLE);
		break;
	case C_RUN1:
		ai_run(self, 9);
		if (frand() < 13107)
			ent_sound(self, CHAN_VOICE, s_ogidle2, 255, ATTN_IDLE);
		break;
	case C_SWING1:
		ai_charge(self, 11);
		ent_sound(self, CHAN_WEAPON, s_ogsawatk, 255, ATTN_NORM);
		break;
	case C_SWING5:
		ai_charge(self, 9);
		chainsaw(self, 0);
		swing_turn(self);
		break;
	case C_SWING6:
		chainsaw(self, 200);
		swing_turn(self);
		break;
	case C_SWING7:
	case C_SWING8:
	case C_SWING9:
	case C_SWING11:
		chainsaw(self, 0);
		swing_turn(self);
		break;
	case C_SWING10:
		chainsaw(self, -200);
		swing_turn(self);
		break;
	case C_SMASH1:
		ai_charge(self, 6);
		ent_sound(self, CHAN_WEAPON, s_ogsawatk, 255, ATTN_NORM);
		break;
	case C_SMASH6:
	case C_SMASH7:
		ai_charge(self, 4);
		chainsaw(self, 0);
		break;
	case C_SMASH8:
		ai_charge(self, 10);
		chainsaw(self, 0);
		break;
	case C_SMASH9:
		ai_charge(self, 13);
		chainsaw(self, 0);
		break;
	case C_SMASH10:
		chainsaw(self, 1);
		break;
	case C_SMASH11:
		ai_charge(self, 2);
		chainsaw(self, 0);
		self->nextthink += (int32_t) (frand() * TICKS(20) >> 16);   /* + random () x 0.2 */
		break;
	case C_NAIL4:
		ai_face(self);
		ogre_fire_grenade(self);
		break;
	case C_DIE3:
	case C_BDIE3:
		self->solid = SOLID_NOT;
		ent_link(self);
		self->ammo_rockets = 2;
		drop_backpack(self);
		break;
	}
}

/* OgreCheckAttack (1.06: its chance is worked out, then not used: a
   clear line out of the far range always throws) */
static int ogre_check_attack(edict_t *self)
{
	if (ai_enemy_range == RANGE_MELEE && ai_can_damage(self->enemy, self)) {
		self->attack_state = AS_MELEE;
		return 1;
	}
	if (ent_time < self->attack_finished)
		return 0;
	if (!ai_enemy_visible)
		return 0;
	edict_t *targ = self->enemy;
	int32_t spot1[3], spot2[3];
	ai_view_org(self, spot1);
	ai_view_org(targ, spot2);
	ai_traceline(spot1, spot2, 0, self);
	if (ai_trace_inopen && ai_trace_inwater)
		return 0;
	if (ai_trace_ent != targ)
		return 0;
	if (ent_time < self->attack_finished)
		return 0;
	if (ai_enemy_range == RANGE_FAR)
		return 0;
	self->attack_state = AS_MISSILE;
	sub_attack_finished(self, TICKS(100) + (int) (frand() * TICKS(200) >> 16));   /* 1 + 2 x random () */
	return 1;
}

static void ogre_pain(edict_t *self)
{
	if (self->pain_finished > ent_time)
		return;
	ent_sound(self, CHAN_VOICE, s_ogpain1, 255, ATTN_NORM);
	uint32_t r = frand();
	if (r < 16384) {                     /* 0.25 */
		monster_state(self, OGRE_PAIN1);
		self->pain_finished = ent_time + TICKS(100);
	} else if (r < 32768) {              /* 0.5 */
		monster_state(self, OGRE_PAINB1);
		self->pain_finished = ent_time + TICKS(100);
	} else if (r < 49152) {              /* 0.75 */
		monster_state(self, OGRE_PAINC1);
		self->pain_finished = ent_time + TICKS(100);
	} else if (r < 57672) {              /* 0.88 */
		monster_state(self, OGRE_PAIND1);
		self->pain_finished = ent_time + TICKS(200);
	} else {
		monster_state(self, OGRE_PAINE1);
		self->pain_finished = ent_time + TICKS(200);
	}
}

static void ogre_die(edict_t *self)
{
	if (self->health < -80) {            /* check for gib */
		ent_sound(self, CHAN_VOICE, s_udeath, 255, ATTN_NORM);
		throw_head(self, self->health);
		throw_gib(self, 2, self->health);
		throw_gib(self, 2, self->health);
		throw_gib(self, 2, self->health);
		return;
	}
	ent_sound(self, CHAN_VOICE, s_ogdth, 255, ATTN_NORM);
	if (frand() < 32768)
		monster_state(self, OGRE_DIE1);
	else
		monster_state(self, OGRE_BDIE1);
}

const mdef_t mdef_ogre = {
	.classname = "monster_ogre",
	.assets = { "progs/ogre.mdl", "progs/h_ogre.mdl", "progs/zom_gib.mdl", "progs/backpack.mdl" },
	.mins = { -32, -32, -24 }, .maxs = { 32, 32, 64 },
	.health = 200,
	.kind = MK_OGRE,
	.states = states, .num_states = OGRE_COUNT,
	.th_stand = ogre_stand, .th_walk = ogre_walk, .th_run = ogre_run,
	.th_missile = ogre_nail, .th_melee = ogre_melee,
	.th_pain = ogre_pain, .th_die = ogre_die,
	.custom = ogre_custom,
	.check_attack = ogre_check_attack,
	.map_start = ogre_map_start,
	.sight = &s_ogwake,
};

/* monster_ogre_marksman (1.06: monster_ogre's spawn under its own
   classname: CheckAnyAttack's generic CheckAttack, no sight sound, a
   kind apart for T_Damage's anger) */
const mdef_t mdef_ogre_marksman = {
	.classname = "monster_ogre_marksman",
	.assets = { "progs/ogre.mdl", "progs/h_ogre.mdl", "progs/zom_gib.mdl", "progs/backpack.mdl" },
	.mins = { -32, -32, -24 }, .maxs = { 32, 32, 64 },
	.health = 200,
	.kind = MK_OGRE,
	.states = states, .num_states = OGRE_COUNT,
	.th_stand = ogre_stand, .th_walk = ogre_walk, .th_run = ogre_run,
	.th_missile = ogre_nail, .th_melee = ogre_melee,
	.th_pain = ogre_pain, .th_die = ogre_die,
	.custom = ogre_custom,
	.map_start = 0,
};
