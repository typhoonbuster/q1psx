/*
 * q1psx — the spawn (monster_tarbaby): tarbaby.qc, the 1.06 progs.dat's
 * (the registered game's; the rerelease's spawn.qc for the frames' names),
 * transcribed state by state: its leap (MOVETYPE_BOUNCE, Tar_JumpTouch),
 * the bounce again in the air at its fourth fly frame, its blast when it
 * dies (TE_TAREXPLOSION: engine/game.c game_tar_explosion). See
 * engine/monsters.h.
 */

#include "game.h"
#include "monsters.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: walk1-25, run1-25, jump1-6, fly1-4, exp */
enum { F_WALK1 = 0, F_RUN1 = 25, F_JUMP1 = 50, F_FLY1 = 56, F_EXP = 60 };

enum {
	TB_STAND1 = 0, TB_HANG1 = 1, TB_WALK1 = 2, TB_RUN1 = 27, TB_FLY1 = 52, TB_JUMP1 = 56, TB_DIE1 = 62,
	TB_COUNT = 64
};

enum { C_FLY4, C_JUMP5, C_DIE1, C_DIE2 };

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[TB_COUNT] = {
	/* tbaby_stand1, tbaby_hang1 =[ $walk1, itself ] { ai_stand(); } */
	ST(F_WALK1, TB_STAND1, A_STAND, 0), ST(F_WALK1, TB_HANG1, A_STAND, 0),
	/* tbaby_walk1-25: turning, then ai_walk */
	ST(F_WALK1 + 0,  TB_WALK1 + 1,  A_TURN, 0), ST(F_WALK1 + 1,  TB_WALK1 + 2,  A_TURN, 0),
	ST(F_WALK1 + 2,  TB_WALK1 + 3,  A_TURN, 0), ST(F_WALK1 + 3,  TB_WALK1 + 4,  A_TURN, 0),
	ST(F_WALK1 + 4,  TB_WALK1 + 5,  A_TURN, 0), ST(F_WALK1 + 5,  TB_WALK1 + 6,  A_TURN, 0),
	ST(F_WALK1 + 6,  TB_WALK1 + 7,  A_TURN, 0), ST(F_WALK1 + 7,  TB_WALK1 + 8,  A_TURN, 0),
	ST(F_WALK1 + 8,  TB_WALK1 + 9,  A_TURN, 0), ST(F_WALK1 + 9,  TB_WALK1 + 10, A_TURN, 0),
	ST(F_WALK1 + 10, TB_WALK1 + 11, A_WALK, 2), ST(F_WALK1 + 11, TB_WALK1 + 12, A_WALK, 2),
	ST(F_WALK1 + 12, TB_WALK1 + 13, A_WALK, 2), ST(F_WALK1 + 13, TB_WALK1 + 14, A_WALK, 2),
	ST(F_WALK1 + 14, TB_WALK1 + 15, A_WALK, 2), ST(F_WALK1 + 15, TB_WALK1 + 16, A_WALK, 2),
	ST(F_WALK1 + 16, TB_WALK1 + 17, A_WALK, 2), ST(F_WALK1 + 17, TB_WALK1 + 18, A_WALK, 2),
	ST(F_WALK1 + 18, TB_WALK1 + 19, A_WALK, 2), ST(F_WALK1 + 19, TB_WALK1 + 20, A_WALK, 2),
	ST(F_WALK1 + 20, TB_WALK1 + 21, A_WALK, 2), ST(F_WALK1 + 21, TB_WALK1 + 22, A_WALK, 2),
	ST(F_WALK1 + 22, TB_WALK1 + 23, A_WALK, 2), ST(F_WALK1 + 23, TB_WALK1 + 24, A_WALK, 2),
	ST(F_WALK1 + 24, TB_WALK1 + 0,  A_WALK, 2),
	/* tbaby_run1-25: facing, then ai_run */
	ST(F_RUN1 + 0,  TB_RUN1 + 1,  A_FACE, 0), ST(F_RUN1 + 1,  TB_RUN1 + 2,  A_FACE, 0),
	ST(F_RUN1 + 2,  TB_RUN1 + 3,  A_FACE, 0), ST(F_RUN1 + 3,  TB_RUN1 + 4,  A_FACE, 0),
	ST(F_RUN1 + 4,  TB_RUN1 + 5,  A_FACE, 0), ST(F_RUN1 + 5,  TB_RUN1 + 6,  A_FACE, 0),
	ST(F_RUN1 + 6,  TB_RUN1 + 7,  A_FACE, 0), ST(F_RUN1 + 7,  TB_RUN1 + 8,  A_FACE, 0),
	ST(F_RUN1 + 8,  TB_RUN1 + 9,  A_FACE, 0), ST(F_RUN1 + 9,  TB_RUN1 + 10, A_FACE, 0),
	ST(F_RUN1 + 10, TB_RUN1 + 11, A_RUN, 2),  ST(F_RUN1 + 11, TB_RUN1 + 12, A_RUN, 2),
	ST(F_RUN1 + 12, TB_RUN1 + 13, A_RUN, 2),  ST(F_RUN1 + 13, TB_RUN1 + 14, A_RUN, 2),
	ST(F_RUN1 + 14, TB_RUN1 + 15, A_RUN, 2),  ST(F_RUN1 + 15, TB_RUN1 + 16, A_RUN, 2),
	ST(F_RUN1 + 16, TB_RUN1 + 17, A_RUN, 2),  ST(F_RUN1 + 17, TB_RUN1 + 18, A_RUN, 2),
	ST(F_RUN1 + 18, TB_RUN1 + 19, A_RUN, 2),  ST(F_RUN1 + 19, TB_RUN1 + 20, A_RUN, 2),
	ST(F_RUN1 + 20, TB_RUN1 + 21, A_RUN, 2),  ST(F_RUN1 + 21, TB_RUN1 + 22, A_RUN, 2),
	ST(F_RUN1 + 22, TB_RUN1 + 23, A_RUN, 2),  ST(F_RUN1 + 23, TB_RUN1 + 24, A_RUN, 2),
	ST(F_RUN1 + 24, TB_RUN1 + 0,  A_RUN, 2),
	/* tbaby_fly1-4: in the air till its touch, a new leap at the fourth
	   loop */
	ST(F_FLY1 + 0, TB_FLY1 + 1, A_NONE, 0), ST(F_FLY1 + 1, TB_FLY1 + 2, A_NONE, 0),
	ST(F_FLY1 + 2, TB_FLY1 + 3, A_NONE, 0), ST(F_FLY1 + 3, TB_FLY1 + 0, A_CUSTOM, C_FLY4),
	/* tbaby_jump1-6 (its th_melee and th_missile) */
	ST(F_JUMP1 + 0, TB_JUMP1 + 1, A_FACE, 0), ST(F_JUMP1 + 1, TB_JUMP1 + 2, A_FACE, 0),
	ST(F_JUMP1 + 2, TB_JUMP1 + 3, A_FACE, 0), ST(F_JUMP1 + 3, TB_JUMP1 + 4, A_FACE, 0),
	ST(F_JUMP1 + 4, TB_JUMP1 + 5, A_CUSTOM, C_JUMP5),
	ST(F_JUMP1 + 5, TB_FLY1,      A_NONE, 0),
	/* tbaby_die1-2 */
	ST(F_EXP, TB_DIE1 + 1, A_CUSTOM, C_DIE1), ST(F_EXP, TB_RUN1, A_CUSTOM, C_DIE2),
};

static int16_t s_death1, s_hit1, s_land1, s_sight1;

static void tbaby_map_start(void)
{
	s_death1 = (int16_t) snd_find("blob/death1.wav");
	s_hit1   = (int16_t) snd_find("blob/hit1.wav");
	s_land1  = (int16_t) snd_find("blob/land1.wav");
	s_sight1 = (int16_t) snd_find("blob/sight1.wav");
}

static void tbaby_stand(edict_t *self) { monster_state(self, TB_STAND1); }
static void tbaby_walk(edict_t *self)  { monster_state(self, TB_WALK1); }
static void tbaby_run(edict_t *self)   { monster_state(self, TB_RUN1); }
static void tbaby_jump(edict_t *self)  { monster_state(self, TB_JUMP1); }

/* Tar_JumpTouch: what it hits fast hurt (not another spawn), then on the
   ground the next leap, off it a run (a ledge's edge) */
void tar_jump_touch(edict_t *self)
{
	edict_t *other = ent_other;
	if (other->takedamage && !(other->mdef && other->mdef->kind == MK_TARBABY)) {
		if (ai_vlen2(self->velocity) > (int64_t) 400 * 400) {
			ent_damage_x256(other, self, self, 10 * 256 + (int) (10 * 256 * frand() >> 16));   /* 10 +
			                                                                   10 x random () */
			ent_sound(self, CHAN_WEAPON, s_hit1, 255, ATTN_NORM);
		}
	} else {
		ent_sound(self, CHAN_WEAPON, s_land1, 255, ATTN_NORM);
	}
	if (!ai_check_bottom(self)) {
		if (self->flags & FL_ONGROUND) { /* jump randomly to not get hung up */
			self->touch    = EF_sub_null;
			self->movetype = MOVETYPE_STEP;
			monster_think_at(self, TB_RUN1, TICKS(10));
		}
		return;                          /* not on ground yet */
	}
	self->touch = EF_sub_null;
	monster_think_at(self, TB_JUMP1, TICKS(10));
}

/* tbaby_jump5: thrown forward at 600 a second, 200 to 350 up */
static void tbaby_leap(edict_t *self)
{
	int32_t f[3], r[3], u[3];
	self->movetype = MOVETYPE_BOUNCE;
	self->touch    = EF_tar_jump_touch;
	ai_makevectors(self->ang, f, r, u);
	self->origin[2] += SUB;                  /* (no setorigin: linked by its next move) */
	for (int k = 0; k < 3; k++)
		self->velocity[k] = f[k] * 600 / 4096;
	self->velocity[2] += 200;
	self->velocity[2] += (int32_t) (frand() * 150 >> 16);
	if (self->flags & FL_ONGROUND)
		self->flags &= ~FL_ONGROUND;
	self->cnt = 0;
}

static void tbaby_custom(edict_t *self, int code)
{
	switch (code) {
	case C_FLY4:
		self->cnt++;
		if (self->cnt == 4)
			monster_state(self, TB_JUMP1 + 4);   /* tbaby_jump5 () */
		break;
	case C_JUMP5:
		tbaby_leap(self);
		break;
	case C_DIE1:
		self->takedamage = DAMAGE_NO;
		break;
	case C_DIE2: {
		ents_radius_damage(self->origin, 120, self, ent_world);
		game_radius_damage_player(self->origin, 120);
		ent_sound(self, CHAN_VOICE, s_death1, 255, ATTN_NORM);
		if (self->velocity[0] || self->velocity[1] || self->velocity[2]) {   /* - 8 x normalize (velocity) */
			int32_t len = (int32_t) q_isqrt64((uint64_t) ai_vlen2(self->velocity));
			for (int k = 0; k < 3; k++)
				self->origin[k] -= (int32_t) q_div64_32((int64_t) self->velocity[k] * 8 * SUB, len);
		}
		game_tar_explosion(self->origin);   /* TE_TAREXPLOSION, BecomeExplosion */
		ent_remove(self);
		break;
	}
	}
}

void tbaby_die(edict_t *self)
{
	monster_state(self, TB_DIE1);
}

const mdef_t mdef_tarbaby = {
	.classname = "monster_tarbaby",
	.assets = { "progs/tarbaby.mdl" },
	.mins = { -16, -16, -24 }, .maxs = { 16, 16, 40 },
	.health = 80,
	.kind = MK_TARBABY,
	.states = states, .num_states = TB_COUNT,
	.th_stand = tbaby_stand, .th_walk = tbaby_walk, .th_run = tbaby_run,
	.th_missile = tbaby_jump, .th_melee = tbaby_jump,
	.th_pain = EF_NONE, .th_die = EF_tbaby_die,
	.custom = tbaby_custom,
	.check_attack = 0,
	.map_start = tbaby_map_start,
	.sight = &s_sight1,
};
