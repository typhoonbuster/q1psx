/*
 * q1psx — the shambler (monster_shambler): shambler.qc, the shareware's
 * 1.06, transcribed state by state: its claws and smash, its lightning (the
 * glow in its hands an entity of its own, the beam engine/game.c's) and
 * fight.qc's ShamCheckAttack. See engine/monsters.h.
 */

#include "game.h"
#include "level.h"
#include "monsters.h"
#include "q_fixed.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: stand1-17, walk1-12, run1-6, smash1-12, swingr1-9, swingl1-9, magic1-12, pain1-6, death1-11 */
enum {
	F_STAND1 = 0, F_WALK1 = 17, F_RUN1 = 29, F_SMASH1 = 35, F_SWINGR1 = 47, F_SWINGL1 = 56,
	F_MAGIC1 = 65, F_PAIN1 = 77, F_DEATH1 = 83
};

enum {
	SHAM_STAND1 = 0, SHAM_WALK1 = 17, SHAM_RUN1 = 29, SHAM_SMASH1 = 35, SHAM_SWINGL1 = 47,
	SHAM_SWINGR1 = 56, SHAM_MAGIC1 = 65, SHAM_PAIN1 = 75, SHAM_DEATH1 = 81, SHAM_COUNT = 92
};

enum {
	C_WALK12, C_RUN6, C_SMASH1, C_SMASH10, C_SWINGL1, C_SWINGL7, C_SWINGL9, C_SWINGR1, C_SWINGR7,
	C_SWINGR9, C_MAGIC1, C_MAGIC3, C_MAGIC4, C_MAGIC5, C_MAGIC6, C_MAGIC9, C_MAGIC10, C_MAGIC11,
	C_DEATH3
};

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[SHAM_COUNT] = {
	/* sham_stand */
	ST(F_STAND1, SHAM_STAND1 + 1, A_STAND, 0),    ST(F_STAND1 + 1, SHAM_STAND1 + 2, A_STAND, 0),
	ST(F_STAND1 + 2, SHAM_STAND1 + 3, A_STAND, 0), ST(F_STAND1 + 3, SHAM_STAND1 + 4, A_STAND, 0),
	ST(F_STAND1 + 4, SHAM_STAND1 + 5, A_STAND, 0), ST(F_STAND1 + 5, SHAM_STAND1 + 6, A_STAND, 0),
	ST(F_STAND1 + 6, SHAM_STAND1 + 7, A_STAND, 0), ST(F_STAND1 + 7, SHAM_STAND1 + 8, A_STAND, 0),
	ST(F_STAND1 + 8, SHAM_STAND1 + 9, A_STAND, 0), ST(F_STAND1 + 9, SHAM_STAND1 + 10, A_STAND, 0),
	ST(F_STAND1 + 10, SHAM_STAND1 + 11, A_STAND, 0), ST(F_STAND1 + 11, SHAM_STAND1 + 12, A_STAND, 0),
	ST(F_STAND1 + 12, SHAM_STAND1 + 13, A_STAND, 0), ST(F_STAND1 + 13, SHAM_STAND1 + 14, A_STAND, 0),
	ST(F_STAND1 + 14, SHAM_STAND1 + 15, A_STAND, 0), ST(F_STAND1 + 15, SHAM_STAND1 + 16, A_STAND, 0),
	ST(F_STAND1 + 16, SHAM_STAND1, A_STAND, 0),
	/* sham_walk */
	ST(F_WALK1, SHAM_WALK1 + 1, A_WALK, 10),      ST(F_WALK1 + 1, SHAM_WALK1 + 2, A_WALK, 9),
	ST(F_WALK1 + 2, SHAM_WALK1 + 3, A_WALK, 9),   ST(F_WALK1 + 3, SHAM_WALK1 + 4, A_WALK, 5),
	ST(F_WALK1 + 4, SHAM_WALK1 + 5, A_WALK, 6),   ST(F_WALK1 + 5, SHAM_WALK1 + 6, A_WALK, 12),
	ST(F_WALK1 + 6, SHAM_WALK1 + 7, A_WALK, 8),   ST(F_WALK1 + 7, SHAM_WALK1 + 8, A_WALK, 3),
	ST(F_WALK1 + 8, SHAM_WALK1 + 9, A_WALK, 13),  ST(F_WALK1 + 9, SHAM_WALK1 + 10, A_WALK, 9),
	ST(F_WALK1 + 10, SHAM_WALK1 + 11, A_WALK, 7),
	ST(F_WALK1 + 11, SHAM_WALK1, A_CUSTOM, C_WALK12),
	/* sham_run */
	ST(F_RUN1, SHAM_RUN1 + 1, A_RUN, 20),         ST(F_RUN1 + 1, SHAM_RUN1 + 2, A_RUN, 24),
	ST(F_RUN1 + 2, SHAM_RUN1 + 3, A_RUN, 20),     ST(F_RUN1 + 3, SHAM_RUN1 + 4, A_RUN, 20),
	ST(F_RUN1 + 4, SHAM_RUN1 + 5, A_RUN, 24),
	ST(F_RUN1 + 5, SHAM_RUN1, A_CUSTOM, C_RUN6),
	/* sham_smash */
	ST(F_SMASH1, SHAM_SMASH1 + 1, A_CUSTOM, C_SMASH1),
	ST(F_SMASH1 + 1, SHAM_SMASH1 + 2, A_CHARGE, 6), ST(F_SMASH1 + 2, SHAM_SMASH1 + 3, A_CHARGE, 6),
	ST(F_SMASH1 + 3, SHAM_SMASH1 + 4, A_CHARGE, 5), ST(F_SMASH1 + 4, SHAM_SMASH1 + 5, A_CHARGE, 4),
	ST(F_SMASH1 + 5, SHAM_SMASH1 + 6, A_CHARGE, 1), ST(F_SMASH1 + 6, SHAM_SMASH1 + 7, A_CHARGE, 0),
	ST(F_SMASH1 + 7, SHAM_SMASH1 + 8, A_CHARGE, 0), ST(F_SMASH1 + 8, SHAM_SMASH1 + 9, A_CHARGE, 0),
	ST(F_SMASH1 + 9, SHAM_SMASH1 + 10, A_CUSTOM, C_SMASH10),
	ST(F_SMASH1 + 10, SHAM_SMASH1 + 11, A_CHARGE, 5), ST(F_SMASH1 + 11, SHAM_RUN1, A_CHARGE, 4),
	/* sham_swingl */
	ST(F_SWINGL1, SHAM_SWINGL1 + 1, A_CUSTOM, C_SWINGL1),
	ST(F_SWINGL1 + 1, SHAM_SWINGL1 + 2, A_CHARGE, 3), ST(F_SWINGL1 + 2, SHAM_SWINGL1 + 3, A_CHARGE, 7),
	ST(F_SWINGL1 + 3, SHAM_SWINGL1 + 4, A_CHARGE, 3), ST(F_SWINGL1 + 4, SHAM_SWINGL1 + 5, A_CHARGE, 7),
	ST(F_SWINGL1 + 5, SHAM_SWINGL1 + 6, A_CHARGE, 9),
	ST(F_SWINGL1 + 6, SHAM_SWINGL1 + 7, A_CUSTOM, C_SWINGL7),
	ST(F_SWINGL1 + 7, SHAM_SWINGL1 + 8, A_CHARGE, 4),
	ST(F_SWINGL1 + 8, SHAM_RUN1, A_CUSTOM, C_SWINGL9),
	/* sham_swingr */
	ST(F_SWINGR1, SHAM_SWINGR1 + 1, A_CUSTOM, C_SWINGR1),
	ST(F_SWINGR1 + 1, SHAM_SWINGR1 + 2, A_CHARGE, 8), ST(F_SWINGR1 + 2, SHAM_SWINGR1 + 3, A_CHARGE, 14),
	ST(F_SWINGR1 + 3, SHAM_SWINGR1 + 4, A_CHARGE, 7), ST(F_SWINGR1 + 4, SHAM_SWINGR1 + 5, A_CHARGE, 3),
	ST(F_SWINGR1 + 5, SHAM_SWINGR1 + 6, A_CHARGE, 6),
	ST(F_SWINGR1 + 6, SHAM_SWINGR1 + 7, A_CUSTOM, C_SWINGR7),
	ST(F_SWINGR1 + 7, SHAM_SWINGR1 + 8, A_CHARGE, 3),
	ST(F_SWINGR1 + 8, SHAM_RUN1, A_CUSTOM, C_SWINGR9),
	/* sham_magic */
	ST(F_MAGIC1, SHAM_MAGIC1 + 1, A_CUSTOM, C_MAGIC1),
	ST(F_MAGIC1 + 1, SHAM_MAGIC1 + 2, A_FACE, 0),
	ST(F_MAGIC1 + 2, SHAM_MAGIC1 + 3, A_CUSTOM, C_MAGIC3),
	ST(F_MAGIC1 + 3, SHAM_MAGIC1 + 4, A_CUSTOM, C_MAGIC4),
	ST(F_MAGIC1 + 4, SHAM_MAGIC1 + 5, A_CUSTOM, C_MAGIC5),
	ST(F_MAGIC1 + 5, SHAM_MAGIC1 + 6, A_CUSTOM, C_MAGIC6),
	ST(F_MAGIC1 + 8, SHAM_MAGIC1 + 7, A_CUSTOM, C_MAGIC9),
	ST(F_MAGIC1 + 9, SHAM_MAGIC1 + 8, A_CUSTOM, C_MAGIC10),
	ST(F_MAGIC1 + 10, SHAM_MAGIC1 + 9, A_CUSTOM, C_MAGIC11),
	ST(F_MAGIC1 + 11, SHAM_RUN1, A_NONE, 0),
	/* sham_pain */
	ST(F_PAIN1, SHAM_PAIN1 + 1, A_NONE, 0),       ST(F_PAIN1 + 1, SHAM_PAIN1 + 2, A_NONE, 0),
	ST(F_PAIN1 + 2, SHAM_PAIN1 + 3, A_NONE, 0),   ST(F_PAIN1 + 3, SHAM_PAIN1 + 4, A_NONE, 0),
	ST(F_PAIN1 + 4, SHAM_PAIN1 + 5, A_NONE, 0),   ST(F_PAIN1 + 5, SHAM_RUN1, A_NONE, 0),
	/* sham_death */
	ST(F_DEATH1, SHAM_DEATH1 + 1, A_NONE, 0),     ST(F_DEATH1 + 1, SHAM_DEATH1 + 2, A_NONE, 0),
	ST(F_DEATH1 + 2, SHAM_DEATH1 + 3, A_CUSTOM, C_DEATH3),
	ST(F_DEATH1 + 3, SHAM_DEATH1 + 4, A_NONE, 0), ST(F_DEATH1 + 4, SHAM_DEATH1 + 5, A_NONE, 0),
	ST(F_DEATH1 + 5, SHAM_DEATH1 + 6, A_NONE, 0), ST(F_DEATH1 + 6, SHAM_DEATH1 + 7, A_NONE, 0),
	ST(F_DEATH1 + 7, SHAM_DEATH1 + 8, A_NONE, 0), ST(F_DEATH1 + 8, SHAM_DEATH1 + 9, A_NONE, 0),
	ST(F_DEATH1 + 9, SHAM_DEATH1 + 10, A_NONE, 0), ST(F_DEATH1 + 10, SHAM_DEATH1 + 10, A_NONE, 0),
};

static int16_t s_sattck1, s_sboom, s_sdeath, s_shurt2, s_sidle, s_ssight, s_melee1, s_melee2, s_smack,
               s_udeath;

static void sham_map_start(void)
{
	s_sattck1 = (int16_t) snd_find("shambler/sattck1.wav");
	s_sboom   = (int16_t) snd_find("shambler/sboom.wav");
	s_sdeath  = (int16_t) snd_find("shambler/sdeath.wav");
	s_shurt2  = (int16_t) snd_find("shambler/shurt2.wav");
	s_sidle   = (int16_t) snd_find("shambler/sidle.wav");
	s_ssight  = (int16_t) snd_find("shambler/ssight.wav");
	s_melee1  = (int16_t) snd_find("shambler/melee1.wav");
	s_melee2  = (int16_t) snd_find("shambler/melee2.wav");
	s_smack   = (int16_t) snd_find("shambler/smack.wav");
	s_udeath  = (int16_t) snd_find("player/udeath.wav");
}

static void sham_stand(edict_t *self) { monster_state(self, SHAM_STAND1); }
static void sham_walk(edict_t *self)  { monster_state(self, SHAM_WALK1); }
static void sham_run(edict_t *self)   { monster_state(self, SHAM_RUN1); }
static void sham_magic(edict_t *self) { monster_state(self, SHAM_MAGIC1); }

/* sham_melee: the smash (always at full health), else a swing */
static void sham_melee(edict_t *self)
{
	uint32_t chance = frand();
	if (chance > 39322 || self->health == 600)   /* 0.6 */
		monster_state(self, SHAM_SMASH1);
	else if (chance > 19661)                     /* 0.3 */
		monster_state(self, SHAM_SWINGR1);
	else
		monster_state(self, SHAM_SWINGL1);
}

static void muzzle_flash(edict_t *self)          /* effects |= EF_MUZZLEFLASH */
{
	game_muzzle_flash(ENT_EDICT + (int) (self - edicts), self->origin, self->ang[1] >> 4);
}

/* ShamClaw: (random () + random () + random ()) x 20 within 100 units, the
   meat thrown aside */
static void sham_claw(edict_t *self, int side)
{
	if (!self->enemy)
		return;
	ai_charge(self, 10);
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	if (ai_vlen2(d) > AI_UNITS2(100))
		return;
	ent_damage_x256(self->enemy, self, self, (int) ((frand() + frand() + frand()) * 20 * 256 >> 16));
	ent_sound(self, CHAN_VOICE, s_smack, 255, ATTN_NORM);
	if (side) {
		int32_t f[3], r[3], u[3], org[3], vel[3];
		ai_makevectors(self->ang, f, r, u);
		for (int k = 0; k < 3; k++) {
			org[k] = self->origin[k] + 16 * SUB * f[k] / 4096;
			vel[k] = side * r[k] / 4096;
		}
		spawn_meat_spray(self, org, vel);
	}
}

/* sham_smash10: (random () + random () + random ()) x 40, two sprays of
   meat (1.06: along the last makevectors' vectors) */
static void sham_smash(edict_t *self)
{
	if (!self->enemy)
		return;
	ai_charge(self, 0);
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	if (ai_vlen2(d) > AI_UNITS2(100))
		return;
	if (!ai_can_damage(self->enemy, self))
		return;
	ent_damage_x256(self->enemy, self, self, (int) ((frand() + frand() + frand()) * 40 * 256 >> 16));
	ent_sound(self, CHAN_VOICE, s_smack, 255, ATTN_NORM);
	for (int n = 0; n < 2; n++) {
		int32_t org[3], vel[3], c = crand();   /* crandom () x 100 x v_right */
		for (int k = 0; k < 3; k++) {
			org[k] = self->origin[k] + 16 * SUB * ai_v_forward[k] / 4096;
			vel[k] = (int32_t) ((int64_t) c * 100 * ai_v_right[k] >> 28);
		}
		spawn_meat_spray(self, org, vel);
	}
}

/* CastLightning: from 40 up toward the enemy's 16 up, 600 units on from
   its origin, through the monsters: the beam, then LightningDamage 10 */
static void cast_lightning(edict_t *self)
{
	muzzle_flash(self);
	ai_face(self);
	int32_t org[3], d[3], end[3];
	for (int k = 0; k < 3; k++) {
		org[k] = self->origin[k] + (k == 2 ? 40 * SUB : 0);
		d[k] = self->enemy->origin[k] + (k == 2 ? 16 * SUB : 0) - org[k];
	}
	int32_t len = (int32_t) q_isqrt64((uint64_t) ai_vlen2(d));
	for (int k = 0; k < 3; k++)
		end[k] = self->origin[k] + (len ? d[k] * 600 / len : 0);
	ai_traceline(org, end, 1, self);
	int32_t hit[3] = { ai_trace_endpos[0], ai_trace_endpos[1], ai_trace_endpos[2] };
	game_monster_beam(self, org, hit, monster_asset(self, 3));   /* TE_LIGHTNING1 */
	game_lightning_damage(self, org, hit, 10);
}

static void sham_custom(edict_t *self, int code)
{
	switch (code) {
	case C_WALK12:
		ai_walk(self, 7);
		if (frand() > 52429)             /* random () > 0.8 */
			ent_sound(self, CHAN_VOICE, s_sidle, 255, ATTN_IDLE);
		break;
	case C_RUN6:
		ai_run(self, 20);
		if (frand() > 52429)
			ent_sound(self, CHAN_VOICE, s_sidle, 255, ATTN_IDLE);
		break;
	case C_SMASH1:
		ent_sound(self, CHAN_VOICE, s_melee1, 255, ATTN_NORM);
		ai_charge(self, 2);
		break;
	case C_SMASH10:
		sham_smash(self);
		break;
	case C_SWINGL1:
		ent_sound(self, CHAN_VOICE, s_melee2, 255, ATTN_NORM);
		ai_charge(self, 5);
		break;
	case C_SWINGL7:
		ai_charge(self, 5);
		sham_claw(self, 250);
		break;
	case C_SWINGL9:
		ai_charge(self, 8);
		if (frand() < 32768)             /* the other claw */
			self->mstate = SHAM_SWINGR1;
		break;
	case C_SWINGR1:
		ent_sound(self, CHAN_VOICE, s_melee1, 255, ATTN_NORM);
		ai_charge(self, 1);
		break;
	case C_SWINGR7:
		ai_charge(self, 6);
		sham_claw(self, -250);
		break;
	case C_SWINGR9:
		ai_charge(self, 1);
		ai_charge(self, 10);
		if (frand() < 32768)
			self->mstate = SHAM_SWINGL1;
		break;
	case C_MAGIC1:
		ai_face(self);
		ent_sound(self, CHAN_WEAPON, s_sattck1, 255, ATTN_NORM);
		break;
	case C_MAGIC3: {                     /* the glow in its hands */
		ai_face(self);
		self->nextthink += TICKS(20);
		muzzle_flash(self);
		ai_face(self);
		edict_t *o = ent_spawn();
		self->owner = o;
		if (!o)
			break;
		o->masset = (int16_t) monster_asset(self, 2);   /* progs/s_light.mdl */
		ent_setorigin(o, self->origin);
		o->ang[0] = self->ang[0];
		o->ang[1] = self->ang[1];
		o->ang[2] = self->ang[2];
		o->nextthink = ent_time + TICKS(70);
		o->think     = sub_remove;
		ent_wake(o);
		break;
	}
	case C_MAGIC4:
	case C_MAGIC5:
		muzzle_flash(self);
		if (self->owner)
			self->owner->frame = (uint8_t) (code == C_MAGIC4 ? 1 : 2);
		break;
	case C_MAGIC6:
		if (self->owner)
			ent_remove(self->owner);
		cast_lightning(self);
		ent_sound(self, CHAN_WEAPON, s_sboom, 255, ATTN_NORM);
		break;
	case C_MAGIC9:
	case C_MAGIC10:
		cast_lightning(self);
		break;
	case C_MAGIC11:
		if (game_skill == 3)
			cast_lightning(self);
		break;
	case C_DEATH3:
		self->solid = SOLID_NOT;
		ent_link(self);
		break;
	}
}

/* ShamCheckAttack: the claws when close, else the lightning within 600
   units on a clear line */
static int sham_check_attack(edict_t *self)
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
	int32_t spot1[3], spot2[3], d[3];
	ai_view_org(self, spot1);
	ai_view_org(targ, spot2);
	for (int k = 0; k < 3; k++)
		d[k] = spot1[k] - spot2[k];
	if (ai_vlen2(d) > AI_UNITS2(600))
		return 0;
	ai_traceline(spot1, spot2, 0, self);
	if (ai_trace_inopen && ai_trace_inwater)
		return 0;
	if (ai_trace_ent != targ)
		return 0;                        /* don't have a clear shot */
	if (ai_enemy_range == RANGE_FAR)
		return 0;
	self->attack_state = AS_MISSILE;
	sub_attack_finished(self, TICKS(200) + (int) (frand() * TICKS(200) >> 16));   /* 2 + 2 x random () */
	return 1;
}

static void sham_pain(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, s_shurt2, 255, ATTN_NORM);
	if (self->health <= 0)
		return;                          /* allready dying, don't go into pain frame */
	if (frand() * 400 > (uint32_t) ai_damage_take << 16)   /* random () x 400 > damage */
		return;                          /* didn't flinch */
	if (self->pain_finished > ent_time)
		return;
	self->pain_finished = ent_time + TICKS(200);
	monster_state(self, SHAM_PAIN1);
}

static void sham_die(edict_t *self)
{
	if (self->health < -60) {            /* check for gib */
		ent_sound(self, CHAN_VOICE, s_udeath, 255, ATTN_NORM);
		throw_head(self, self->health);
		throw_gib(self, 0, self->health);
		throw_gib(self, 1, self->health);
		throw_gib(self, 2, self->health);
		return;
	}
	/* regular death */
	ent_sound(self, CHAN_VOICE, s_sdeath, 255, ATTN_NORM);
	monster_state(self, SHAM_DEATH1);
}

const mdef_t mdef_shambler = {
	.classname = "monster_shambler",
	.assets = { "progs/shambler.mdl", "progs/h_shams.mdl", "progs/s_light.mdl", "progs/bolt.mdl",
	            "progs/zom_gib.mdl" },
	.mins = { -32, -32, -24 }, .maxs = { 32, 32, 64 },
	.health = 600,
	.kind = MK_SHAMBLER,
	.states = states, .num_states = SHAM_COUNT,
	.th_stand = sham_stand, .th_walk = sham_walk, .th_run = sham_run,
	.th_missile = sham_magic, .th_melee = sham_melee,
	.th_pain = sham_pain, .th_die = sham_die,
	.custom = sham_custom,
	.check_attack = sham_check_attack,
	.map_start = sham_map_start,
	.sight = &s_ssight,
};
