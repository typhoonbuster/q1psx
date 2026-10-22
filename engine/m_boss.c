/*
 * q1psx — Chthon (monster_boss) and E1M7's lightning (event_lightning):
 * boss.qc, the shareware's 1.06, transcribed state by state. The boss
 * rises when used, throws lava balls (engine/game.c's projectiles), and
 * only the lightning between the two doors targeted "lightning" hurts it.
 * See engine/monsters.h.
 */

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "level.h"
#include "monsters.h"
#include "q_fixed.h"
#include "render.h"
#include "snd.h"
#include "world.h"

#define SUB CLIP_SUB

/* $frame: rise1-17, walk1-31, death1-9, attack1-23, shocka1-10, shockb1-6, shockc1-10 */
enum {
	F_RISE1 = 0, F_WALK1 = 17, F_DEATH1 = 48, F_ATTACK1 = 57, F_SHOCKA1 = 80, F_SHOCKB1 = 90,
	F_SHOCKC1 = 96
};

enum {
	BOSS_RISE1 = 0, BOSS_IDLE1 = 17, BOSS_MISSILE1 = 48, BOSS_SHOCKA1 = 71, BOSS_SHOCKB1 = 81,
	BOSS_SHOCKC1 = 91, BOSS_DEATH1 = 101, BOSS_COUNT = 111
};

enum { C_RISE1, C_RISE2, C_FACE, C_MISSILE9, C_MISSILE20, C_DEATH1, C_DEATH9, C_DEATH10 };

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[BOSS_COUNT] = {
	/* boss_rise */
	ST(F_RISE1, BOSS_RISE1 + 1, A_CUSTOM, C_RISE1),
	ST(F_RISE1 + 1, BOSS_RISE1 + 2, A_CUSTOM, C_RISE2),
	ST(F_RISE1 + 2, BOSS_RISE1 + 3, A_NONE, 0),   ST(F_RISE1 + 3, BOSS_RISE1 + 4, A_NONE, 0),
	ST(F_RISE1 + 4, BOSS_RISE1 + 5, A_NONE, 0),   ST(F_RISE1 + 5, BOSS_RISE1 + 6, A_NONE, 0),
	ST(F_RISE1 + 6, BOSS_RISE1 + 7, A_NONE, 0),   ST(F_RISE1 + 7, BOSS_RISE1 + 8, A_NONE, 0),
	ST(F_RISE1 + 8, BOSS_RISE1 + 9, A_NONE, 0),   ST(F_RISE1 + 9, BOSS_RISE1 + 10, A_NONE, 0),
	ST(F_RISE1 + 10, BOSS_RISE1 + 11, A_NONE, 0), ST(F_RISE1 + 11, BOSS_RISE1 + 12, A_NONE, 0),
	ST(F_RISE1 + 12, BOSS_RISE1 + 13, A_NONE, 0), ST(F_RISE1 + 13, BOSS_RISE1 + 14, A_NONE, 0),
	ST(F_RISE1 + 14, BOSS_RISE1 + 15, A_NONE, 0), ST(F_RISE1 + 15, BOSS_RISE1 + 16, A_NONE, 0),
	ST(F_RISE1 + 16, BOSS_MISSILE1, A_NONE, 0),
	/* boss_idle */
	ST(F_WALK1, BOSS_IDLE1 + 1, A_NONE, 0),
	ST(F_WALK1 + 1, BOSS_IDLE1 + 2, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 2, BOSS_IDLE1 + 3, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 3, BOSS_IDLE1 + 4, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 4, BOSS_IDLE1 + 5, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 5, BOSS_IDLE1 + 6, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 6, BOSS_IDLE1 + 7, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 7, BOSS_IDLE1 + 8, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 8, BOSS_IDLE1 + 9, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 9, BOSS_IDLE1 + 10, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 10, BOSS_IDLE1 + 11, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 11, BOSS_IDLE1 + 12, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 12, BOSS_IDLE1 + 13, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 13, BOSS_IDLE1 + 14, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 14, BOSS_IDLE1 + 15, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 15, BOSS_IDLE1 + 16, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 16, BOSS_IDLE1 + 17, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 17, BOSS_IDLE1 + 18, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 18, BOSS_IDLE1 + 19, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 19, BOSS_IDLE1 + 20, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 20, BOSS_IDLE1 + 21, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 21, BOSS_IDLE1 + 22, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 22, BOSS_IDLE1 + 23, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 23, BOSS_IDLE1 + 24, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 24, BOSS_IDLE1 + 25, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 25, BOSS_IDLE1 + 26, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 26, BOSS_IDLE1 + 27, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 27, BOSS_IDLE1 + 28, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 28, BOSS_IDLE1 + 29, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 29, BOSS_IDLE1 + 30, A_CUSTOM, C_FACE),
	ST(F_WALK1 + 30, BOSS_IDLE1, A_CUSTOM, C_FACE),
	/* boss_missile */
	ST(F_ATTACK1, BOSS_MISSILE1 + 1, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 1, BOSS_MISSILE1 + 2, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 2, BOSS_MISSILE1 + 3, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 3, BOSS_MISSILE1 + 4, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 4, BOSS_MISSILE1 + 5, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 5, BOSS_MISSILE1 + 6, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 6, BOSS_MISSILE1 + 7, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 7, BOSS_MISSILE1 + 8, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 8, BOSS_MISSILE1 + 9, A_CUSTOM, C_MISSILE9),
	ST(F_ATTACK1 + 9, BOSS_MISSILE1 + 10, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 10, BOSS_MISSILE1 + 11, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 11, BOSS_MISSILE1 + 12, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 12, BOSS_MISSILE1 + 13, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 13, BOSS_MISSILE1 + 14, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 14, BOSS_MISSILE1 + 15, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 15, BOSS_MISSILE1 + 16, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 16, BOSS_MISSILE1 + 17, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 17, BOSS_MISSILE1 + 18, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 18, BOSS_MISSILE1 + 19, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 19, BOSS_MISSILE1 + 20, A_CUSTOM, C_MISSILE20),
	ST(F_ATTACK1 + 20, BOSS_MISSILE1 + 21, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 21, BOSS_MISSILE1 + 22, A_CUSTOM, C_FACE),
	ST(F_ATTACK1 + 22, BOSS_MISSILE1, A_CUSTOM, C_FACE),
	/* boss_shocka */
	ST(F_SHOCKA1, BOSS_SHOCKA1 + 1, A_NONE, 0),   ST(F_SHOCKA1 + 1, BOSS_SHOCKA1 + 2, A_NONE, 0),
	ST(F_SHOCKA1 + 2, BOSS_SHOCKA1 + 3, A_NONE, 0), ST(F_SHOCKA1 + 3, BOSS_SHOCKA1 + 4, A_NONE, 0),
	ST(F_SHOCKA1 + 4, BOSS_SHOCKA1 + 5, A_NONE, 0), ST(F_SHOCKA1 + 5, BOSS_SHOCKA1 + 6, A_NONE, 0),
	ST(F_SHOCKA1 + 6, BOSS_SHOCKA1 + 7, A_NONE, 0), ST(F_SHOCKA1 + 7, BOSS_SHOCKA1 + 8, A_NONE, 0),
	ST(F_SHOCKA1 + 8, BOSS_SHOCKA1 + 9, A_NONE, 0), ST(F_SHOCKA1 + 9, BOSS_MISSILE1, A_NONE, 0),
	/* boss_shockb */
	ST(F_SHOCKB1, BOSS_SHOCKB1 + 1, A_NONE, 0),   ST(F_SHOCKB1 + 1, BOSS_SHOCKB1 + 2, A_NONE, 0),
	ST(F_SHOCKB1 + 2, BOSS_SHOCKB1 + 3, A_NONE, 0), ST(F_SHOCKB1 + 3, BOSS_SHOCKB1 + 4, A_NONE, 0),
	ST(F_SHOCKB1 + 4, BOSS_SHOCKB1 + 5, A_NONE, 0), ST(F_SHOCKB1 + 5, BOSS_SHOCKB1 + 6, A_NONE, 0),
	ST(F_SHOCKB1, BOSS_SHOCKB1 + 7, A_NONE, 0),   ST(F_SHOCKB1 + 1, BOSS_SHOCKB1 + 8, A_NONE, 0),
	ST(F_SHOCKB1 + 2, BOSS_SHOCKB1 + 9, A_NONE, 0), ST(F_SHOCKB1 + 3, BOSS_MISSILE1, A_NONE, 0),
	/* boss_shockc */
	ST(F_SHOCKC1, BOSS_SHOCKC1 + 1, A_NONE, 0),   ST(F_SHOCKC1 + 1, BOSS_SHOCKC1 + 2, A_NONE, 0),
	ST(F_SHOCKC1 + 2, BOSS_SHOCKC1 + 3, A_NONE, 0), ST(F_SHOCKC1 + 3, BOSS_SHOCKC1 + 4, A_NONE, 0),
	ST(F_SHOCKC1 + 4, BOSS_SHOCKC1 + 5, A_NONE, 0), ST(F_SHOCKC1 + 5, BOSS_SHOCKC1 + 6, A_NONE, 0),
	ST(F_SHOCKC1 + 6, BOSS_SHOCKC1 + 7, A_NONE, 0), ST(F_SHOCKC1 + 7, BOSS_SHOCKC1 + 8, A_NONE, 0),
	ST(F_SHOCKC1 + 8, BOSS_SHOCKC1 + 9, A_NONE, 0), ST(F_SHOCKC1 + 9, BOSS_DEATH1, A_NONE, 0),
	/* boss_death */
	ST(F_DEATH1, BOSS_DEATH1 + 1, A_CUSTOM, C_DEATH1),
	ST(F_DEATH1 + 1, BOSS_DEATH1 + 2, A_NONE, 0), ST(F_DEATH1 + 2, BOSS_DEATH1 + 3, A_NONE, 0),
	ST(F_DEATH1 + 3, BOSS_DEATH1 + 4, A_NONE, 0), ST(F_DEATH1 + 4, BOSS_DEATH1 + 5, A_NONE, 0),
	ST(F_DEATH1 + 5, BOSS_DEATH1 + 6, A_NONE, 0), ST(F_DEATH1 + 6, BOSS_DEATH1 + 7, A_NONE, 0),
	ST(F_DEATH1 + 7, BOSS_DEATH1 + 8, A_NONE, 0),
	ST(F_DEATH1 + 8, BOSS_DEATH1 + 9, A_CUSTOM, C_DEATH9),
	ST(F_DEATH1 + 8, BOSS_DEATH1 + 9, A_CUSTOM, C_DEATH10),
};

static int16_t s_out1, s_sight1, s_power, s_throw, s_pain, s_death;
static edict_t *le1, *le2;               /* the lightning's two doors */
static int32_t  lightning_end;

static void boss_map_start(void)
{
	s_out1   = (int16_t) snd_find("boss1/out1.wav");
	s_sight1 = (int16_t) snd_find("boss1/sight1.wav");
	s_power  = (int16_t) snd_find("misc/power.wav");
	s_throw  = (int16_t) snd_find("boss1/throw.wav");
	s_pain   = (int16_t) snd_find("boss1/pain.wav");
	s_death  = (int16_t) snd_find("boss1/death.wav");
	le1 = le2 = 0;
	lightning_end = 0;
}

/* boss_face: the player again now and then (the QC's find of the next
   player, then from the start: in a single player game, the player),
   then face it */
static void boss_face(edict_t *self)
{
	int dead = self->enemy->health <= 0;
	if (dead | (frand() < 1311))         /* random () < 0.02 (the QC's || runs both) */
		self->enemy = ent_player;
	ai_face(self);
}

/* boss_missile (p): a lava ball from p (forward, right, up) off its origin
   along its line to the enemy, at the enemy (where it will be, on hard),
   300 a second */
static void boss_missile(edict_t *self, int pf, int pr, int pu)
{
	int32_t d[3], f[3], r[3], u[3], org[3], at[3], vec[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - self->origin[k];
	/* vectoangles (its pitch up positive) */
	int32_t fwd = (int32_t) q_isqrt64((uint64_t) ((int64_t) d[0] * d[0] + (int64_t) d[1] * d[1]));
	uint16_t offang[3] = { (uint16_t) (r_atan2(d[2], fwd) * (65536 / ANG_TURN)),
	                       (uint16_t) (r_atan2(d[1], d[0]) * (65536 / ANG_TURN)), 0 };
	ai_makevectors(offang, f, r, u);
	for (int k = 0; k < 3; k++)
		org[k] = self->origin[k] + (pf * f[k] + pr * r[k]) * SUB / 4096 + (k == 2 ? pu * SUB : 0);
	if (game_skill > 1) {                /* lead the enemy */
		int32_t e[3];
		for (int k = 0; k < 3; k++)
			e[k] = self->enemy->origin[k] - org[k];
		int32_t t = (int32_t) q_isqrt64((uint64_t) ai_vlen2(e));   /* x300: 1/8 units */
		for (int k = 0; k < 3; k++)
			at[k] = self->enemy->origin[k] + (k == 2 ? 0 : (int32_t) ((int64_t) self->enemy->velocity[k] * t / 300));
	} else {
		__builtin_memcpy(at, self->enemy->origin, sizeof at);
	}
	for (int k = 0; k < 3; k++)
		d[k] = at[k] - org[k];
	int32_t len = (int32_t) q_isqrt64((uint64_t) ai_vlen2(d));
	for (int k = 0; k < 3; k++)
		vec[k] = len ? (int32_t) q_div64_32((int64_t) d[k] * 300, len) : 0;
	game_monster_missile(MM_LAVABALL, self, org, vec, monster_asset(self, 1), SND_NONE, SND_NONE);
	ent_sound(self, CHAN_WEAPON, s_throw, 255, ATTN_NORM);
	if (self->enemy->health <= 0)        /* check for dead enemy */
		monster_state(self, BOSS_IDLE1);
}

static void boss_custom(edict_t *self, int code)
{
	switch (code) {
	case C_RISE1:
		ent_sound(self, CHAN_WEAPON, s_out1, 255, ATTN_NORM);
		break;
	case C_RISE2:
		ent_sound(self, CHAN_VOICE, s_sight1, 255, ATTN_NORM);
		break;
	case C_FACE:
		boss_face(self);
		break;
	case C_MISSILE9:
		boss_missile(self, 100, 100, 200);
		break;
	case C_MISSILE20:
		boss_missile(self, 100, -100, 200);
		break;
	case C_DEATH1:
		ent_sound(self, CHAN_VOICE, s_death, 255, ATTN_NORM);
		break;
	case C_DEATH9:
		ent_sound(self, CHAN_BODY, s_out1, 255, ATTN_NORM);
		game_lava_splash(self->origin);
		break;
	case C_DEATH10:
		ent_killed_monsters++;           /* (SVC_KILLEDMONSTER) */
		sub_use_targets(self);
		ent_remove(self);
		break;
	}
}

/* boss_awake (its use): it rises from the lava */
static void boss_awake(edict_t *self)
{
	self->solid      = SOLID_SLIDEBOX;
	self->movetype   = MOVETYPE_STEP;
	self->takedamage = DAMAGE_NO;
	self->masset     = (int16_t) monster_asset(self, 0);
	static const int32_t mins[3] = { -128 * SUB, -128 * SUB, -24 * SUB }, maxs[3] = { 128 * SUB, 128 * SUB, 256 * SUB };
	ent_setsize(self, mins, maxs);
	self->health = game_skill == 0 ? 1 : 3;
	self->enemy  = ent_activator;
	game_lava_splash(self->origin);
	self->yaw_speed = 20;
	monster_state(self, BOSS_RISE1);
}

/* monster_boss: unseen and not solid until used */
static int boss_spawn(edict_t *self)
{
	self->solid    = SOLID_NOT;
	self->movetype = MOVETYPE_NONE;
	self->masset   = -1;
	ent_link(self);
	ent_total_monsters++;
	self->use = boss_awake;
	return 1;
}

/* lightning_fire: the beam between the doors' bottoms, every tenth of a
   second until its second is over, then the doors down */
static void lightning_fire(edict_t *self)
{
	if (ent_time >= lightning_end) {     /* done here, put the terminals back up */
		movers_door_go_down(le1);
		movers_door_go_down(le2);
		return;
	}
	int32_t p1[3], p2[3], d[3], n[3];
	for (int k = 0; k < 3; k++) {
		p1[k] = (le1->mins[k] + le1->maxs[k]) / 2;
		p2[k] = (le2->mins[k] + le2->maxs[k]) / 2;
	}
	p1[2] = le1->absmin[2] - 16 * SUB;
	p2[2] = le2->absmin[2] - 16 * SUB;
	for (int k = 0; k < 3; k++)
		d[k] = p2[k] - p1[k];
	int32_t len = (int32_t) q_isqrt64((uint64_t) ai_vlen2(d));
	for (int k = 0; k < 3; k++) {
		n[k] = len ? (int32_t) q_div64_32((int64_t) d[k] * 100 * SUB, len) : 0;
		p2[k] -= n[k];
	}
	self->nextthink = ent_time + TICKS(10);
	self->think     = lightning_fire;
	ent_wake(self);
	game_monster_beam(ent_world, p1, p2, monsters_kind_asset(MK_BOSS, 2));   /* TE_LIGHTNING3 (bolt3.mdl) */
}

/* the first entity after `from` whose target is "lightning" */
static edict_t *find_lightning_target(edict_t *from)
{
	for (int i = from ? (int) (from - edicts) + 1 : 1; i < num_edicts; i++) {
		edict_t *e = &edicts[i];
		if (e->inuse && e->target && !strcmp(world_string(e->target), "lightning"))
			return e;
	}
	return 0;
}

/* lightning_use: when both doors rest at the same end, the lightning for a
   second; with the doors up (their bottoms down to the beam), Chthon
   hurt */
static void lightning_use(edict_t *self)
{
	if (lightning_end >= ent_time + TICKS(100))
		return;
	le1 = find_lightning_target(0);
	le2 = le1 ? find_lightning_target(le1) : 0;
	if (!le1 || !le2) {
		printf("missing lightning targets\n");
		return;
	}
	if ((le1->state != STATE_TOP && le1->state != STATE_BOTTOM) ||
	    (le2->state != STATE_TOP && le2->state != STATE_BOTTOM) || le1->state != le2->state)
		return;
	/* don't let the electrodes go back up until the bolt is done */
	le1->nextthink = -1;
	le2->nextthink = -1;
	lightning_end = ent_time + TICKS(100);
	ent_sound(self, CHAN_VOICE, s_power, 255, ATTN_NORM);
	lightning_fire(self);
	/* advance the boss pain if down */
	edict_t *boss = ent_find_class(0, QE_MONSTER_BOSS);
	if (!boss)
		return;
	boss->enemy = ent_activator;
	if (le1->state == STATE_TOP && boss->health > 0) {
		ent_sound(boss, CHAN_VOICE, s_pain, 255, ATTN_NORM);
		boss->health--;
		if (boss->health >= 2)
			monster_state(boss, BOSS_SHOCKA1);
		else if (boss->health == 1)
			monster_state(boss, BOSS_SHOCKB1);
		else if (boss->health == 0)
			monster_state(boss, BOSS_SHOCKC1);
	}
}

void monsters_event_lightning(edict_t *e)
{
	e->use = lightning_use;
}

const mdef_t mdef_boss = {
	.classname = "monster_boss",
	.assets = { "progs/boss.mdl", "progs/lavaball.mdl", "progs/bolt3.mdl" },
	.mins = { -128, -128, -24 }, .maxs = { 128, 128, 256 },
	.health = 3,
	.kind = MK_BOSS,
	.states = states, .num_states = BOSS_COUNT,
	.custom = boss_custom,
	.spawn = boss_spawn,
	.map_start = boss_map_start,
};
