/*
 * q1psx — the fiend (monster_demon1): demon.qc, the shareware's 1.06,
 * transcribed state by state: its claws, its leap and its touch. See
 * engine/monsters.h.
 */

#include "game.h"
#include "monsters.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: stand1-13, walk1-8, run1-6, leap1-12, pain1-6, death1-9, attacka1-15 */
enum {
	F_STAND1 = 0, F_WALK1 = 13, F_RUN1 = 21, F_LEAP1 = 27, F_PAIN1 = 39, F_DEATH1 = 45,
	F_ATTACKA1 = 54
};

enum {
	DEMON_STAND1 = 0, DEMON_WALK1 = 13, DEMON_RUN1 = 21, DEMON_JUMP1 = 27, DEMON_ATTA1 = 39,
	DEMON_PAIN1 = 54, DEMON_DIE1 = 60, DEMON_COUNT = 69
};

enum { C_WALK1, C_RUN1, C_JUMP4, C_JUMP10, C_ATTA5, C_ATTA11, C_DIE1, C_DIE6 };

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[DEMON_COUNT] = {
	/* demon1_stand */
	ST(F_STAND1, DEMON_STAND1 + 1, A_STAND, 0),   ST(F_STAND1 + 1, DEMON_STAND1 + 2, A_STAND, 0),
	ST(F_STAND1 + 2, DEMON_STAND1 + 3, A_STAND, 0), ST(F_STAND1 + 3, DEMON_STAND1 + 4, A_STAND, 0),
	ST(F_STAND1 + 4, DEMON_STAND1 + 5, A_STAND, 0), ST(F_STAND1 + 5, DEMON_STAND1 + 6, A_STAND, 0),
	ST(F_STAND1 + 6, DEMON_STAND1 + 7, A_STAND, 0), ST(F_STAND1 + 7, DEMON_STAND1 + 8, A_STAND, 0),
	ST(F_STAND1 + 8, DEMON_STAND1 + 9, A_STAND, 0), ST(F_STAND1 + 9, DEMON_STAND1 + 10, A_STAND, 0),
	ST(F_STAND1 + 10, DEMON_STAND1 + 11, A_STAND, 0), ST(F_STAND1 + 11, DEMON_STAND1 + 12, A_STAND, 0),
	ST(F_STAND1 + 12, DEMON_STAND1, A_STAND, 0),
	/* demon1_walk */
	ST(F_WALK1, DEMON_WALK1 + 1, A_CUSTOM, C_WALK1),
	ST(F_WALK1 + 1, DEMON_WALK1 + 2, A_WALK, 6),  ST(F_WALK1 + 2, DEMON_WALK1 + 3, A_WALK, 6),
	ST(F_WALK1 + 3, DEMON_WALK1 + 4, A_WALK, 7),  ST(F_WALK1 + 4, DEMON_WALK1 + 5, A_WALK, 4),
	ST(F_WALK1 + 5, DEMON_WALK1 + 6, A_WALK, 6),  ST(F_WALK1 + 6, DEMON_WALK1 + 7, A_WALK, 10),
	ST(F_WALK1 + 7, DEMON_WALK1, A_WALK, 10),
	/* demon1_run */
	ST(F_RUN1, DEMON_RUN1 + 1, A_CUSTOM, C_RUN1),
	ST(F_RUN1 + 1, DEMON_RUN1 + 2, A_RUN, 15),    ST(F_RUN1 + 2, DEMON_RUN1 + 3, A_RUN, 36),
	ST(F_RUN1 + 3, DEMON_RUN1 + 4, A_RUN, 20),    ST(F_RUN1 + 4, DEMON_RUN1 + 5, A_RUN, 15),
	ST(F_RUN1 + 5, DEMON_RUN1, A_RUN, 36),
	/* demon1_jump */
	ST(F_LEAP1, DEMON_JUMP1 + 1, A_FACE, 0),      ST(F_LEAP1 + 1, DEMON_JUMP1 + 2, A_FACE, 0),
	ST(F_LEAP1 + 2, DEMON_JUMP1 + 3, A_FACE, 0),
	ST(F_LEAP1 + 3, DEMON_JUMP1 + 4, A_CUSTOM, C_JUMP4),
	ST(F_LEAP1 + 4, DEMON_JUMP1 + 5, A_NONE, 0),  ST(F_LEAP1 + 5, DEMON_JUMP1 + 6, A_NONE, 0),
	ST(F_LEAP1 + 6, DEMON_JUMP1 + 7, A_NONE, 0),  ST(F_LEAP1 + 7, DEMON_JUMP1 + 8, A_NONE, 0),
	ST(F_LEAP1 + 8, DEMON_JUMP1 + 9, A_NONE, 0),
	ST(F_LEAP1 + 9, DEMON_JUMP1, A_CUSTOM, C_JUMP10),
	ST(F_LEAP1 + 10, DEMON_JUMP1 + 11, A_NONE, 0), ST(F_LEAP1 + 11, DEMON_RUN1, A_NONE, 0),
	/* demon1_atta */
	ST(F_ATTACKA1, DEMON_ATTA1 + 1, A_CHARGE, 4), ST(F_ATTACKA1 + 1, DEMON_ATTA1 + 2, A_CHARGE, 0),
	ST(F_ATTACKA1 + 2, DEMON_ATTA1 + 3, A_CHARGE, 0), ST(F_ATTACKA1 + 3, DEMON_ATTA1 + 4, A_CHARGE, 1),
	ST(F_ATTACKA1 + 4, DEMON_ATTA1 + 5, A_CUSTOM, C_ATTA5),
	ST(F_ATTACKA1 + 5, DEMON_ATTA1 + 6, A_CHARGE, 1), ST(F_ATTACKA1 + 6, DEMON_ATTA1 + 7, A_CHARGE, 6),
	ST(F_ATTACKA1 + 7, DEMON_ATTA1 + 8, A_CHARGE, 8), ST(F_ATTACKA1 + 8, DEMON_ATTA1 + 9, A_CHARGE, 4),
	ST(F_ATTACKA1 + 9, DEMON_ATTA1 + 10, A_CHARGE, 2),
	ST(F_ATTACKA1 + 10, DEMON_ATTA1 + 11, A_CUSTOM, C_ATTA11),
	ST(F_ATTACKA1 + 11, DEMON_ATTA1 + 12, A_CHARGE, 5), ST(F_ATTACKA1 + 12, DEMON_ATTA1 + 13, A_CHARGE, 8),
	ST(F_ATTACKA1 + 13, DEMON_ATTA1 + 14, A_CHARGE, 4), ST(F_ATTACKA1 + 14, DEMON_RUN1, A_CHARGE, 4),
	/* demon1_pain */
	ST(F_PAIN1, DEMON_PAIN1 + 1, A_NONE, 0),      ST(F_PAIN1 + 1, DEMON_PAIN1 + 2, A_NONE, 0),
	ST(F_PAIN1 + 2, DEMON_PAIN1 + 3, A_NONE, 0),  ST(F_PAIN1 + 3, DEMON_PAIN1 + 4, A_NONE, 0),
	ST(F_PAIN1 + 4, DEMON_PAIN1 + 5, A_NONE, 0),  ST(F_PAIN1 + 5, DEMON_RUN1, A_NONE, 0),
	/* demon1_die */
	ST(F_DEATH1, DEMON_DIE1 + 1, A_CUSTOM, C_DIE1),
	ST(F_DEATH1 + 1, DEMON_DIE1 + 2, A_NONE, 0),  ST(F_DEATH1 + 2, DEMON_DIE1 + 3, A_NONE, 0),
	ST(F_DEATH1 + 3, DEMON_DIE1 + 4, A_NONE, 0),  ST(F_DEATH1 + 4, DEMON_DIE1 + 5, A_NONE, 0),
	ST(F_DEATH1 + 5, DEMON_DIE1 + 6, A_CUSTOM, C_DIE6),
	ST(F_DEATH1 + 6, DEMON_DIE1 + 7, A_NONE, 0),  ST(F_DEATH1 + 7, DEMON_DIE1 + 8, A_NONE, 0),
	ST(F_DEATH1 + 8, DEMON_DIE1 + 8, A_NONE, 0),
};

static int16_t s_ddeath, s_dhit2, s_djump, s_dpain1, s_idle1, s_sight2, s_udeath;

static void demon_map_start(void)
{
	s_ddeath = (int16_t) snd_find("demon/ddeath.wav");
	s_dhit2  = (int16_t) snd_find("demon/dhit2.wav");
	s_djump  = (int16_t) snd_find("demon/djump.wav");
	s_dpain1 = (int16_t) snd_find("demon/dpain1.wav");
	s_idle1  = (int16_t) snd_find("demon/idle1.wav");
	s_sight2 = (int16_t) snd_find("demon/sight2.wav");
	s_udeath = (int16_t) snd_find("player/udeath.wav");
}

static void demon_stand(edict_t *self) { monster_state(self, DEMON_STAND1); }
static void demon_walk(edict_t *self)  { monster_state(self, DEMON_WALK1); }
static void demon_run(edict_t *self)   { monster_state(self, DEMON_RUN1); }
static void demon_jump(edict_t *self)  { monster_state(self, DEMON_JUMP1); }
static void demon_melee_attack(edict_t *self) { monster_state(self, DEMON_ATTA1); }   /* Demon_MeleeAttack */

/* Demon_Melee: a step in, then a claw within 100 units (10 + 5 x random),
   the meat thrown aside */
static void demon_melee(edict_t *self, int side)
{
	ai_face(self);
	ai_walkmove(self, self->ideal_yaw, 12);   /* allow a little closing */
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	if (ai_vlen2(d) > AI_UNITS2(100))
		return;
	if (!ai_can_damage(self->enemy, self))
		return;
	ent_sound(self, CHAN_WEAPON, s_dhit2, 255, ATTN_NORM);
	ent_damage_x256(self->enemy, self, self, 10 * 256 + (int) (frand() * 5 * 256 >> 16));
	int32_t f[3], r[3], u[3], org[3], vel[3];
	ai_makevectors(self->ang, f, r, u);
	for (int k = 0; k < 3; k++) {
		org[k] = self->origin[k] + 16 * SUB * f[k] / 4096;
		vel[k] = side * r[k] / 4096;
	}
	spawn_meat_spray(self, org, vel);
}

/* Demon_JumpTouch: what the leap meets hurt if it flew fast (40 + 10 x
   random), then run on once on the ground (leap again if stuck on an
   edge) */
void demon_jump_touch(edict_t *self)
{
	if (self->health <= 0)
		return;
	edict_t *other = ent_other;
	if (other && other->takedamage) {
		const int32_t *v = self->velocity;
		if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > 400 * 400)   /* (capped at 2000: 32 bits) */
			ent_damage_x256(other, self, self, 40 * 256 + (int) (frand() * 10 * 256 >> 16));
	}
	if (!ai_check_bottom(self)) {
		if (self->flags & FL_ONGROUND) {  /* jump randomly to not get hung up */
			self->touch = EF_sub_null;
			monster_think_at(self, DEMON_JUMP1, TICKS(10));
		}
		return;                           /* not on ground yet */
	}
	self->touch = EF_sub_null;
	monster_think_at(self, DEMON_JUMP1 + 10, TICKS(10));   /* demon1_jump11 */
}

static void demon_custom(edict_t *self, int code)
{
	switch (code) {
	case C_WALK1:
		if (frand() < 13107)             /* random () < 0.2 */
			ent_sound(self, CHAN_VOICE, s_idle1, 255, ATTN_IDLE);
		ai_walk(self, 8);
		break;
	case C_RUN1:
		if (frand() < 13107)
			ent_sound(self, CHAN_VOICE, s_idle1, 255, ATTN_IDLE);
		ai_run(self, 20);
		break;
	case C_JUMP4: {
		int32_t f[3], r[3], u[3];
		ai_face(self);
		self->touch = EF_demon_jump_touch;
		ai_makevectors(self->ang, f, r, u);
		self->origin[2] += SUB;
		for (int k = 0; k < 3; k++)
			self->velocity[k] = f[k] * 600 / 4096;
		self->velocity[2] += 250;
		self->flags &= ~FL_ONGROUND;
		break;
	}
	case C_JUMP10:                       /* (still flying: once more in 3 s) */
		self->nextthink = ent_time + TICKS(300);
		break;
	case C_ATTA5:
		ai_charge(self, 2);
		demon_melee(self, 200);
		break;
	case C_ATTA11:
		demon_melee(self, -200);
		break;
	case C_DIE1:
		ent_sound(self, CHAN_VOICE, s_ddeath, 255, ATTN_NORM);
		break;
	case C_DIE6:
		self->solid = SOLID_NOT;
		ent_link(self);
		break;
	}
}

/* CheckDemonJump: the enemy's height within reach, 100 to 200 units away
   across (beyond 200 one time in ten) */
static int demon_check_jump(edict_t *self)
{
	const edict_t *en = self->enemy;
	int32_t size = en->maxs[2] - en->mins[2];
	if (self->origin[2] + self->mins[2] > en->origin[2] + en->mins[2] + size * 3 / 4)
		return 0;
	if (self->origin[2] + self->maxs[2] < en->origin[2] + en->mins[2] + size / 4)
		return 0;
	int32_t d[3] = { en->origin[0] - self->origin[0], en->origin[1] - self->origin[1], 0 };
	int64_t d2 = ai_vlen2(d);
	if (d2 < AI_UNITS2(100))
		return 0;
	if (d2 > AI_UNITS2(200) && frand() < 58982)   /* random () < 0.9 */
		return 0;
	return 1;
}

/* DemonCheckAttack: the claws when close (CheckDemonMelee), else the leap */
static int demon_check_attack(edict_t *self)
{
	if (ai_enemy_range == RANGE_MELEE) {
		self->attack_state = AS_MELEE;
		return 1;
	}
	if (demon_check_jump(self)) {
		self->attack_state = AS_MISSILE;
		ent_sound(self, CHAN_VOICE, s_djump, 255, ATTN_NORM);
		return 1;
	}
	return 0;
}

void demon_pain(edict_t *self)
{
	if (self->touch == EF_demon_jump_touch)
		return;
	if (self->pain_finished > ent_time)
		return;
	self->pain_finished = ent_time + TICKS(100);
	ent_sound(self, CHAN_VOICE, s_dpain1, 255, ATTN_NORM);
	if (frand() * 200 > (uint32_t) ai_damage_take << 16)   /* random () x 200 > damage */
		return;                          /* didn't flinch */
	monster_state(self, DEMON_PAIN1);
}

void demon_die(edict_t *self)
{
	if (self->health < -80) {            /* check for gib */
		ent_sound(self, CHAN_VOICE, s_udeath, 255, ATTN_NORM);
		throw_head(self, self->health);
		throw_gib(self, 0, self->health);
		throw_gib(self, 0, self->health);
		throw_gib(self, 0, self->health);
		return;
	}
	monster_state(self, DEMON_DIE1);
}

const mdef_t mdef_demon = {
	.classname = "monster_demon1",
	.assets = { "progs/demon.mdl", "progs/h_demon.mdl", "progs/zom_gib.mdl" },
	.mins = { -32, -32, -24 }, .maxs = { 32, 32, 64 },
	.health = 300,
	.kind = MK_DEMON,
	.states = states, .num_states = DEMON_COUNT,
	.th_stand = demon_stand, .th_walk = demon_walk, .th_run = demon_run,
	.th_missile = demon_jump, .th_melee = demon_melee_attack,
	.th_pain = EF_demon_pain, .th_die = EF_demon_die,
	.custom = demon_custom,
	.check_attack = demon_check_attack,
	.map_start = demon_map_start,
	.sight = &s_sight2,
};
