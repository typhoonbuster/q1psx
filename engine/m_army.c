/*
 * q1psx — the soldier (monster_army): soldier.qc, the shareware's 1.06
 * (the rerelease's grunt.qc, its additions left out), transcribed state by
 * state; weapons.qc's FireBullets and TraceAttack for its shotgun. See
 * engine/monsters.h.
 */

#include <string.h>

#include "game.h"
#include "monsters.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: stand1-8, death1-10, deathc1-11, load1-11, pain1-6, painb1-14,
   painc1-13, run1-8, shoot1-9, prowl_1-24 */
enum {
	F_STAND1 = 0, F_DEATH1 = 8, F_DEATHC1 = 18, F_LOAD1 = 29, F_PAIN1 = 40, F_PAINB1 = 46,
	F_PAINC1 = 60, F_RUN1 = 73, F_SHOOT1 = 81, F_PROWL1 = 90
};

enum {
	ARMY_STAND1 = 0, ARMY_WALK1 = 8, ARMY_RUN1 = 32, ARMY_ATK1 = 40, ARMY_PAIN1 = 49, ARMY_PAINB1 = 55, ARMY_PAINC1 = 69,
	ARMY_DIE1 = 82, ARMY_CDIE1 = 92, ARMY_COUNT = 103
};

enum { C_WALK1, C_RUN1, C_ATK5, C_ATK7, C_DIE3, C_CDIE3 };

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[ARMY_COUNT] = {
	/* army_stand1-8 =[ $stand1, army_stand2 ] { ai_stand(); } */
	ST(F_STAND1 + 0, ARMY_STAND1 + 1, A_STAND, 0), ST(F_STAND1 + 1, ARMY_STAND1 + 2, A_STAND, 0),
	ST(F_STAND1 + 2, ARMY_STAND1 + 3, A_STAND, 0), ST(F_STAND1 + 3, ARMY_STAND1 + 4, A_STAND, 0),
	ST(F_STAND1 + 4, ARMY_STAND1 + 5, A_STAND, 0), ST(F_STAND1 + 5, ARMY_STAND1 + 6, A_STAND, 0),
	ST(F_STAND1 + 6, ARMY_STAND1 + 7, A_STAND, 0), ST(F_STAND1 + 7, ARMY_STAND1 + 0, A_STAND, 0),
	/* army_walk1-24: the idle sound at random, ai_walk */
	ST(F_PROWL1 + 0,  ARMY_WALK1 + 1,  A_CUSTOM, C_WALK1),
	ST(F_PROWL1 + 1,  ARMY_WALK1 + 2,  A_WALK, 1), ST(F_PROWL1 + 2,  ARMY_WALK1 + 3,  A_WALK, 1),
	ST(F_PROWL1 + 3,  ARMY_WALK1 + 4,  A_WALK, 1), ST(F_PROWL1 + 4,  ARMY_WALK1 + 5,  A_WALK, 2),
	ST(F_PROWL1 + 5,  ARMY_WALK1 + 6,  A_WALK, 3), ST(F_PROWL1 + 6,  ARMY_WALK1 + 7,  A_WALK, 4),
	ST(F_PROWL1 + 7,  ARMY_WALK1 + 8,  A_WALK, 4), ST(F_PROWL1 + 8,  ARMY_WALK1 + 9,  A_WALK, 2),
	ST(F_PROWL1 + 9,  ARMY_WALK1 + 10, A_WALK, 2), ST(F_PROWL1 + 10, ARMY_WALK1 + 11, A_WALK, 2),
	ST(F_PROWL1 + 11, ARMY_WALK1 + 12, A_WALK, 1), ST(F_PROWL1 + 12, ARMY_WALK1 + 13, A_WALK, 0),
	ST(F_PROWL1 + 13, ARMY_WALK1 + 14, A_WALK, 1), ST(F_PROWL1 + 14, ARMY_WALK1 + 15, A_WALK, 1),
	ST(F_PROWL1 + 15, ARMY_WALK1 + 16, A_WALK, 1), ST(F_PROWL1 + 16, ARMY_WALK1 + 17, A_WALK, 3),
	ST(F_PROWL1 + 17, ARMY_WALK1 + 18, A_WALK, 3), ST(F_PROWL1 + 18, ARMY_WALK1 + 19, A_WALK, 3),
	ST(F_PROWL1 + 19, ARMY_WALK1 + 20, A_WALK, 3), ST(F_PROWL1 + 20, ARMY_WALK1 + 21, A_WALK, 2),
	ST(F_PROWL1 + 21, ARMY_WALK1 + 22, A_WALK, 1), ST(F_PROWL1 + 22, ARMY_WALK1 + 23, A_WALK, 1),
	ST(F_PROWL1 + 23, ARMY_WALK1 + 0,  A_WALK, 1),
	/* army_run1-8 */
	ST(F_RUN1 + 0, ARMY_RUN1 + 1, A_CUSTOM, C_RUN1),
	ST(F_RUN1 + 1, ARMY_RUN1 + 2, A_RUN, 15), ST(F_RUN1 + 2, ARMY_RUN1 + 3, A_RUN, 10),
	ST(F_RUN1 + 3, ARMY_RUN1 + 4, A_RUN, 10), ST(F_RUN1 + 4, ARMY_RUN1 + 5, A_RUN, 8),
	ST(F_RUN1 + 5, ARMY_RUN1 + 6, A_RUN, 15), ST(F_RUN1 + 6, ARMY_RUN1 + 7, A_RUN, 10),
	ST(F_RUN1 + 7, ARMY_RUN1 + 0, A_RUN, 8),
	/* army_atk1-9: aiming, the shot at 5, the nightmare refire at 7 */
	ST(F_SHOOT1 + 0, ARMY_ATK1 + 1, A_FACE, 0), ST(F_SHOOT1 + 1, ARMY_ATK1 + 2, A_FACE, 0),
	ST(F_SHOOT1 + 2, ARMY_ATK1 + 3, A_FACE, 0), ST(F_SHOOT1 + 3, ARMY_ATK1 + 4, A_FACE, 0),
	ST(F_SHOOT1 + 4, ARMY_ATK1 + 5, A_CUSTOM, C_ATK5),
	ST(F_SHOOT1 + 5, ARMY_ATK1 + 6, A_FACE, 0),
	ST(F_SHOOT1 + 6, ARMY_ATK1 + 7, A_CUSTOM, C_ATK7),
	ST(F_SHOOT1 + 7, ARMY_ATK1 + 8, A_FACE, 0), ST(F_SHOOT1 + 8, ARMY_RUN1, A_FACE, 0),
	/* army_pain1-6 */
	ST(F_PAIN1 + 0, ARMY_PAIN1 + 1, A_NONE, 0), ST(F_PAIN1 + 1, ARMY_PAIN1 + 2, A_NONE, 0),
	ST(F_PAIN1 + 2, ARMY_PAIN1 + 3, A_NONE, 0), ST(F_PAIN1 + 3, ARMY_PAIN1 + 4, A_NONE, 0),
	ST(F_PAIN1 + 4, ARMY_PAIN1 + 5, A_NONE, 0), ST(F_PAIN1 + 5, ARMY_RUN1, A_PAIN, 1),
	/* army_painb1-14 */
	ST(F_PAINB1 + 0,  ARMY_PAINB1 + 1,  A_NONE, 0),     ST(F_PAINB1 + 1,  ARMY_PAINB1 + 2,  A_PAINFWD, 13),
	ST(F_PAINB1 + 2,  ARMY_PAINB1 + 3,  A_PAINFWD, 9),  ST(F_PAINB1 + 3,  ARMY_PAINB1 + 4,  A_NONE, 0),
	ST(F_PAINB1 + 4,  ARMY_PAINB1 + 5,  A_NONE, 0),     ST(F_PAINB1 + 5,  ARMY_PAINB1 + 6,  A_NONE, 0),
	ST(F_PAINB1 + 6,  ARMY_PAINB1 + 7,  A_NONE, 0),     ST(F_PAINB1 + 7,  ARMY_PAINB1 + 8,  A_NONE, 0),
	ST(F_PAINB1 + 8,  ARMY_PAINB1 + 9,  A_NONE, 0),     ST(F_PAINB1 + 9,  ARMY_PAINB1 + 10, A_NONE, 0),
	ST(F_PAINB1 + 10, ARMY_PAINB1 + 11, A_NONE, 0),     ST(F_PAINB1 + 11, ARMY_PAINB1 + 12, A_PAIN, 2),
	ST(F_PAINB1 + 12, ARMY_PAINB1 + 13, A_NONE, 0),     ST(F_PAINB1 + 13, ARMY_RUN1,        A_NONE, 0),
	/* army_painc1-13 */
	ST(F_PAINC1 + 0,  ARMY_PAINC1 + 1,  A_NONE, 0),     ST(F_PAINC1 + 1,  ARMY_PAINC1 + 2,  A_PAIN, 1),
	ST(F_PAINC1 + 2,  ARMY_PAINC1 + 3,  A_NONE, 0),     ST(F_PAINC1 + 3,  ARMY_PAINC1 + 4,  A_NONE, 0),
	ST(F_PAINC1 + 4,  ARMY_PAINC1 + 5,  A_PAINFWD, 1),  ST(F_PAINC1 + 5,  ARMY_PAINC1 + 6,  A_PAINFWD, 1),
	ST(F_PAINC1 + 6,  ARMY_PAINC1 + 7,  A_NONE, 0),     ST(F_PAINC1 + 7,  ARMY_PAINC1 + 8,  A_PAIN, 1),
	ST(F_PAINC1 + 8,  ARMY_PAINC1 + 9,  A_PAINFWD, 4),  ST(F_PAINC1 + 9,  ARMY_PAINC1 + 10, A_PAINFWD, 3),
	ST(F_PAINC1 + 10, ARMY_PAINC1 + 11, A_PAINFWD, 6),  ST(F_PAINC1 + 11, ARMY_PAINC1 + 12, A_PAINFWD, 8),
	ST(F_PAINC1 + 12, ARMY_RUN1,        A_NONE, 0),
	/* army_die1-10: non-solid at 3, the backpack */
	ST(F_DEATH1 + 0, ARMY_DIE1 + 1, A_NONE, 0), ST(F_DEATH1 + 1, ARMY_DIE1 + 2, A_NONE, 0),
	ST(F_DEATH1 + 2, ARMY_DIE1 + 3, A_CUSTOM, C_DIE3),
	ST(F_DEATH1 + 3, ARMY_DIE1 + 4, A_NONE, 0), ST(F_DEATH1 + 4, ARMY_DIE1 + 5, A_NONE, 0),
	ST(F_DEATH1 + 5, ARMY_DIE1 + 6, A_NONE, 0), ST(F_DEATH1 + 6, ARMY_DIE1 + 7, A_NONE, 0),
	ST(F_DEATH1 + 7, ARMY_DIE1 + 8, A_NONE, 0), ST(F_DEATH1 + 8, ARMY_DIE1 + 9, A_NONE, 0),
	ST(F_DEATH1 + 9, ARMY_DIE1 + 9, A_NONE, 0),
	/* army_cdie1-11 */
	ST(F_DEATHC1 + 0,  ARMY_CDIE1 + 1,  A_NONE, 0), ST(F_DEATHC1 + 1, ARMY_CDIE1 + 2, A_BACK, 5),
	ST(F_DEATHC1 + 2,  ARMY_CDIE1 + 3,  A_CUSTOM, C_CDIE3),
	ST(F_DEATHC1 + 3,  ARMY_CDIE1 + 4,  A_BACK, 13), ST(F_DEATHC1 + 4, ARMY_CDIE1 + 5, A_BACK, 3),
	ST(F_DEATHC1 + 5,  ARMY_CDIE1 + 6,  A_BACK, 4),  ST(F_DEATHC1 + 6, ARMY_CDIE1 + 7, A_NONE, 0),
	ST(F_DEATHC1 + 7,  ARMY_CDIE1 + 8,  A_NONE, 0),  ST(F_DEATHC1 + 8, ARMY_CDIE1 + 9, A_NONE, 0),
	ST(F_DEATHC1 + 9,  ARMY_CDIE1 + 10, A_NONE, 0),  ST(F_DEATHC1 + 10, ARMY_CDIE1 + 10, A_NONE, 0),
};

static int16_t s_idle, s_sattck1, s_pain1, s_pain2, s_death1, s_sight1, s_udeath;

static void army_map_start(void)
{
	s_idle    = (int16_t) snd_find("soldier/idle.wav");
	s_sattck1 = (int16_t) snd_find("soldier/sattck1.wav");
	s_pain1   = (int16_t) snd_find("soldier/pain1.wav");
	s_pain2   = (int16_t) snd_find("soldier/pain2.wav");
	s_death1  = (int16_t) snd_find("soldier/death1.wav");
	s_sight1  = (int16_t) snd_find("soldier/sight1.wav");
	s_udeath  = (int16_t) snd_find("player/udeath.wav");
}

static void army_stand(edict_t *self)   { monster_state(self, ARMY_STAND1); }
static void army_walk(edict_t *self)    { monster_state(self, ARMY_WALK1); }
static void army_run(edict_t *self)     { monster_state(self, ARMY_RUN1); }
static void army_missile(edict_t *self) { monster_state(self, ARMY_ATK1); }

/* FireBullets for a monster: makevectors (self.v_angle), a monster's is 0
   (forward +x, right -y, up +z: Quake's own), the source 10 forward at 0.7
   of the box's height; each pellet's TraceAttack: blood and the damage
   gathered on what it hit (AddMultiDamage), or the wall's puff */
static void fire_bullets(edict_t *self, int shotcount, const int32_t dir[3], int spread)
{
	int32_t src[3];
	src[0] = self->origin[0] + 10 * SUB;
	src[1] = self->origin[1];
	src[2] = self->absmin[2] + (self->absmax[2] - self->absmin[2]) * 7 / 10;
	edict_t *multi_ent = 0;              /* ClearMultiDamage */
	int      multi_damage = 0;
	while (shotcount-- > 0) {
		int32_t d[3], end[3], org[3];
		int32_t cx = (int32_t) ((int64_t) crand() * spread >> 16);   /* x4096 */
		int32_t cy = (int32_t) ((int64_t) crand() * spread >> 16);
		d[0] = dir[0];
		d[1] = dir[1] - cx;              /* + crandom () x spread_x x v_right (0 -1 0) */
		d[2] = dir[2] + cy;              /* + crandom () x spread_y x v_up (0 0 1) */
		for (int k = 0; k < 3; k++)
			end[k] = src[k] + (int32_t) ((int64_t) d[k] * 2048 * SUB / 4096);
		if (ai_traceline(src, end, 0, self) >= Q_FIX_ONE)
			continue;
		for (int k = 0; k < 3; k++)     /* TraceAttack: org = trace_endpos - dir x 4 */
			org[k] = ai_trace_endpos[k] - 4 * SUB * d[k] / 4096;
		edict_t *hit = ai_trace_ent;
		if (hit && hit->takedamage) {    /* SpawnBlood (damage 4), AddMultiDamage */
			game_particles(org, 73, 8);
			if (hit != multi_ent) {
				if (multi_ent)
					ent_damage(multi_ent, self, self, multi_damage);
				multi_ent    = hit;
				multi_damage = 0;
			}
			multi_damage += 4;
		} else {
			game_particles(org, 0, 20);  /* TE_GUNSHOT */
		}
	}
	if (multi_ent)                       /* ApplyMultiDamage */
		ent_damage(multi_ent, self, self, multi_damage);
}

/* army_fire: somewhat behind the player, so a dodging player is harder
   to hit */
static void army_fire(edict_t *self)
{
	ai_face(self);
	ent_sound(self, CHAN_WEAPON, s_sattck1, 255, ATTN_NORM);
	edict_t *en = self->enemy;
	if (!en)
		return;
	int32_t d[3], dir[3];
	for (int k = 0; k < 3; k++)
		d[k] = en->origin[k] - en->velocity[k] * SUB / 5 - self->origin[k];
	int32_t len = (int32_t) q_isqrt64((uint64_t) ((int64_t) d[0] * d[0] + (int64_t) d[1] * d[1] +
	                                             (int64_t) d[2] * d[2]));
	for (int k = 0; k < 3; k++)
		dir[k] = len ? (int32_t) ((int64_t) d[k] * 4096 / len) : 0;
	fire_bullets(self, 4, dir, 410);     /* '0.1 0.1 0' x4096 */
}

static void army_custom(edict_t *self, int code)
{
	switch (code) {
	case C_WALK1:
		if (frand() < 13107)             /* random () < 0.2 */
			ent_sound(self, CHAN_VOICE, s_idle, 255, ATTN_IDLE);
		ai_walk(self, 1);
		break;
	case C_RUN1:
		if (frand() < 13107)
			ent_sound(self, CHAN_VOICE, s_idle, 255, ATTN_IDLE);
		ai_run(self, 11);
		break;
	case C_ATK5:
		ai_face(self);
		army_fire(self);
		game_muzzle_flash(ENT_EDICT + (int) (self - edicts), self->origin, self->ang[1] >> 4);   /* EF_MUZZLEFLASH */
		break;
	case C_ATK7:
		ai_face(self);
		sub_check_refire(self, ARMY_ATK1);
		break;
	case C_DIE3:
	case C_CDIE3:
		self->solid = SOLID_NOT;
		ent_link(self);
		self->ammo_shells = 5;
		drop_backpack(self);
		if (code == C_CDIE3)
			ai_back(self, 4);
		break;
	}
}

/* SoldierCheckAttack (fight.qc): a clear line to the player, then a shot
   by chance at its range */
static int army_check_attack(edict_t *self)
{
	edict_t *targ = self->enemy;
	int32_t spot1[3], spot2[3];
	ai_view_org(self, spot1);
	ai_view_org(targ, spot2);
	ai_traceline(spot1, spot2, 0, self);
	if (ai_trace_inopen && ai_trace_inwater)
		return 0;                        /* sight line crossed contents */
	if (ai_trace_ent != targ)
		return 0;                        /* don't have a clear shot */
	if (ent_time < self->attack_finished)
		return 0;
	if (ai_enemy_range == RANGE_FAR)
		return 0;
	uint32_t chance = ai_enemy_range == RANGE_MELEE ? 58982     /* 0.9  */
	                : ai_enemy_range == RANGE_NEAR  ? 26214     /* 0.4  */
	                : ai_enemy_range == RANGE_MID   ? 3277 : 0; /* 0.05 */
	if (frand() < chance) {
		army_missile(self);
		sub_attack_finished(self, TICKS(100) + (int) (frand() * TICKS(100) >> 16));
		if (frand() < 19661)             /* 0.3 */
			self->lefty = (uint8_t) !self->lefty;
		return 1;
	}
	return 0;
}

static void army_pain(edict_t *self)
{
	if (self->pain_finished > ent_time)
		return;
	uint32_t r = frand();
	if (r < 13107) {                     /* 0.2 */
		self->pain_finished = ent_time + TICKS(60);
		monster_state(self, ARMY_PAIN1);
		ent_sound(self, CHAN_VOICE, s_pain1, 255, ATTN_NORM);
	} else if (r < 39322) {              /* 0.6 */
		self->pain_finished = ent_time + TICKS(110);
		monster_state(self, ARMY_PAINB1);
		ent_sound(self, CHAN_VOICE, s_pain2, 255, ATTN_NORM);
	} else {
		self->pain_finished = ent_time + TICKS(110);
		monster_state(self, ARMY_PAINC1);
		ent_sound(self, CHAN_VOICE, s_pain2, 255, ATTN_NORM);
	}
}

static void army_die(edict_t *self)
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
	if (frand() < 32768)
		monster_state(self, ARMY_DIE1);
	else
		monster_state(self, ARMY_CDIE1);
}

const mdef_t mdef_army = {
	.classname = "monster_army",
	.assets = { "progs/soldier.mdl", "progs/h_guard.mdl", "progs/backpack.mdl" },
	.mins = { -16, -16, -24 }, .maxs = { 16, 16, 40 },
	.health = 30,
	.kind = MK_ARMY,
	.states = states, .num_states = ARMY_COUNT,
	.th_stand = army_stand, .th_walk = army_walk, .th_run = army_run,
	.th_missile = army_missile, .th_melee = 0,
	.th_pain = army_pain, .th_die = army_die,
	.custom = army_custom,
	.check_attack = army_check_attack,
	.map_start = army_map_start,
	.sight = &s_sight1,
};
