/*
 * q1psx — the hell knight (monster_hell_knight): hknight.qc, the 1.06
 * progs.dat's (the registered game's; the rerelease's hellknight.qc for the
 * frames' names), transcribed state by state: its spikes (hknight_shot:
 * engine/game.c MM_KSPIKE), its charge (CheckForCharge,
 * CheckContinueCharge), its three swings in turn (hknight_melee). See
 * engine/monsters.h.
 */

#include "game.h"
#include "monsters.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: stand1-9, walk1-20, run1-8, pain1-5, death1-12, deathb1-9,
   char_a1-16, magica1-14, magicb1-13, char_b1-6, slice1-10, smash1-11,
   w_attack1-22, magicc1-11 */
enum {
	F_STAND1 = 0, F_WALK1 = 9, F_RUN1 = 29, F_PAIN1 = 37, F_DEATH1 = 42, F_DEATHB1 = 54, F_CHAR_A1 = 63,
	F_MAGICA1 = 79, F_MAGICB1 = 93, F_CHAR_B1 = 106, F_SLICE1 = 112, F_SMASH1 = 122, F_WATTACK1 = 133,
	F_MAGICC1 = 155
};

enum {
	HK_STAND1 = 0, HK_WALK1 = 9, HK_RUN1 = 29, HK_PAIN1 = 37, HK_DIE1 = 42, HK_DIEB1 = 54, HK_MAGICA1 = 63,
	HK_MAGICB1 = 77, HK_MAGICC1 = 90, HK_CHARA1 = 101, HK_CHARB1 = 117, HK_SLICE1 = 123, HK_SMASH1 = 133,
	HK_WATK1 = 144, HK_COUNT = 166
};

/* C_SHOT0 + k: hknight_shot (k - 2) */
enum { C_WALK1, C_RUN1, C_PAIN1, C_DIE3, C_DIEB3, C_CHARB1, C_SHOT0 };

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }
#define CM A_CHARGEMELEE

static const mstate_t states[HK_COUNT] = {
	/* hknight_stand1-9 */
	ST(F_STAND1 + 0, HK_STAND1 + 1, A_STAND, 0), ST(F_STAND1 + 1, HK_STAND1 + 2, A_STAND, 0),
	ST(F_STAND1 + 2, HK_STAND1 + 3, A_STAND, 0), ST(F_STAND1 + 3, HK_STAND1 + 4, A_STAND, 0),
	ST(F_STAND1 + 4, HK_STAND1 + 5, A_STAND, 0), ST(F_STAND1 + 5, HK_STAND1 + 6, A_STAND, 0),
	ST(F_STAND1 + 6, HK_STAND1 + 7, A_STAND, 0), ST(F_STAND1 + 7, HK_STAND1 + 8, A_STAND, 0),
	ST(F_STAND1 + 8, HK_STAND1 + 0, A_STAND, 0),
	/* hknight_walk1-20: hk_idle_sound, ai_walk */
	ST(F_WALK1 + 0,  HK_WALK1 + 1,  A_CUSTOM, C_WALK1),
	ST(F_WALK1 + 1,  HK_WALK1 + 2,  A_WALK, 5), ST(F_WALK1 + 2,  HK_WALK1 + 3,  A_WALK, 5),
	ST(F_WALK1 + 3,  HK_WALK1 + 4,  A_WALK, 4), ST(F_WALK1 + 4,  HK_WALK1 + 5,  A_WALK, 4),
	ST(F_WALK1 + 5,  HK_WALK1 + 6,  A_WALK, 2), ST(F_WALK1 + 6,  HK_WALK1 + 7,  A_WALK, 2),
	ST(F_WALK1 + 7,  HK_WALK1 + 8,  A_WALK, 3), ST(F_WALK1 + 8,  HK_WALK1 + 9,  A_WALK, 3),
	ST(F_WALK1 + 9,  HK_WALK1 + 10, A_WALK, 4), ST(F_WALK1 + 10, HK_WALK1 + 11, A_WALK, 3),
	ST(F_WALK1 + 11, HK_WALK1 + 12, A_WALK, 4), ST(F_WALK1 + 12, HK_WALK1 + 13, A_WALK, 6),
	ST(F_WALK1 + 13, HK_WALK1 + 14, A_WALK, 2), ST(F_WALK1 + 14, HK_WALK1 + 15, A_WALK, 2),
	ST(F_WALK1 + 15, HK_WALK1 + 16, A_WALK, 4), ST(F_WALK1 + 16, HK_WALK1 + 17, A_WALK, 3),
	ST(F_WALK1 + 17, HK_WALK1 + 18, A_WALK, 3), ST(F_WALK1 + 18, HK_WALK1 + 19, A_WALK, 3),
	ST(F_WALK1 + 19, HK_WALK1 + 0,  A_WALK, 2),
	/* hknight_run1-8: hk_idle_sound, ai_run, CheckForCharge */
	ST(F_RUN1 + 0, HK_RUN1 + 1, A_CUSTOM, C_RUN1),
	ST(F_RUN1 + 1, HK_RUN1 + 2, A_RUN, 25), ST(F_RUN1 + 2, HK_RUN1 + 3, A_RUN, 18),
	ST(F_RUN1 + 3, HK_RUN1 + 4, A_RUN, 16), ST(F_RUN1 + 4, HK_RUN1 + 5, A_RUN, 14),
	ST(F_RUN1 + 5, HK_RUN1 + 6, A_RUN, 25), ST(F_RUN1 + 6, HK_RUN1 + 7, A_RUN, 21),
	ST(F_RUN1 + 7, HK_RUN1 + 0, A_RUN, 13),
	/* hknight_pain1-5 */
	ST(F_PAIN1 + 0, HK_PAIN1 + 1, A_CUSTOM, C_PAIN1), ST(F_PAIN1 + 1, HK_PAIN1 + 2, A_NONE, 0),
	ST(F_PAIN1 + 2, HK_PAIN1 + 3, A_NONE, 0),         ST(F_PAIN1 + 3, HK_PAIN1 + 4, A_NONE, 0),
	ST(F_PAIN1 + 4, HK_RUN1,      A_NONE, 0),
	/* hknight_die1-12: non-solid at 3 */
	ST(F_DEATH1 + 0,  HK_DIE1 + 1,  A_FORWARD, 10), ST(F_DEATH1 + 1,  HK_DIE1 + 2,  A_FORWARD, 8),
	ST(F_DEATH1 + 2,  HK_DIE1 + 3,  A_CUSTOM, C_DIE3),
	ST(F_DEATH1 + 3,  HK_DIE1 + 4,  A_NONE, 0),     ST(F_DEATH1 + 4,  HK_DIE1 + 5,  A_NONE, 0),
	ST(F_DEATH1 + 5,  HK_DIE1 + 6,  A_NONE, 0),     ST(F_DEATH1 + 6,  HK_DIE1 + 7,  A_NONE, 0),
	ST(F_DEATH1 + 7,  HK_DIE1 + 8,  A_FORWARD, 10), ST(F_DEATH1 + 8,  HK_DIE1 + 9,  A_FORWARD, 11),
	ST(F_DEATH1 + 9,  HK_DIE1 + 10, A_NONE, 0),     ST(F_DEATH1 + 10, HK_DIE1 + 11, A_NONE, 0),
	ST(F_DEATH1 + 11, HK_DIE1 + 11, A_NONE, 0),
	/* hknight_dieb1-9 */
	ST(F_DEATHB1 + 0, HK_DIEB1 + 1, A_NONE, 0), ST(F_DEATHB1 + 1, HK_DIEB1 + 2, A_NONE, 0),
	ST(F_DEATHB1 + 2, HK_DIEB1 + 3, A_CUSTOM, C_DIEB3),
	ST(F_DEATHB1 + 3, HK_DIEB1 + 4, A_NONE, 0), ST(F_DEATHB1 + 4, HK_DIEB1 + 5, A_NONE, 0),
	ST(F_DEATHB1 + 5, HK_DIEB1 + 6, A_NONE, 0), ST(F_DEATHB1 + 6, HK_DIEB1 + 7, A_NONE, 0),
	ST(F_DEATHB1 + 7, HK_DIEB1 + 8, A_NONE, 0), ST(F_DEATHB1 + 8, HK_DIEB1 + 8, A_NONE, 0),
	/* hknight_magica1-14: aiming, six spikes from -2 to 3 */
	ST(F_MAGICA1 + 0,  HK_MAGICA1 + 1,  A_FACE, 0), ST(F_MAGICA1 + 1, HK_MAGICA1 + 2, A_FACE, 0),
	ST(F_MAGICA1 + 2,  HK_MAGICA1 + 3,  A_FACE, 0), ST(F_MAGICA1 + 3, HK_MAGICA1 + 4, A_FACE, 0),
	ST(F_MAGICA1 + 4,  HK_MAGICA1 + 5,  A_FACE, 0), ST(F_MAGICA1 + 5, HK_MAGICA1 + 6, A_FACE, 0),
	ST(F_MAGICA1 + 6,  HK_MAGICA1 + 7,  A_CUSTOM, C_SHOT0 + 0),
	ST(F_MAGICA1 + 7,  HK_MAGICA1 + 8,  A_CUSTOM, C_SHOT0 + 1),
	ST(F_MAGICA1 + 8,  HK_MAGICA1 + 9,  A_CUSTOM, C_SHOT0 + 2),
	ST(F_MAGICA1 + 9,  HK_MAGICA1 + 10, A_CUSTOM, C_SHOT0 + 3),
	ST(F_MAGICA1 + 10, HK_MAGICA1 + 11, A_CUSTOM, C_SHOT0 + 4),
	ST(F_MAGICA1 + 11, HK_MAGICA1 + 12, A_CUSTOM, C_SHOT0 + 5),
	ST(F_MAGICA1 + 12, HK_MAGICA1 + 13, A_FACE, 0), ST(F_MAGICA1 + 13, HK_RUN1, A_FACE, 0),
	/* hknight_magicb1-13 */
	ST(F_MAGICB1 + 0,  HK_MAGICB1 + 1,  A_FACE, 0), ST(F_MAGICB1 + 1, HK_MAGICB1 + 2, A_FACE, 0),
	ST(F_MAGICB1 + 2,  HK_MAGICB1 + 3,  A_FACE, 0), ST(F_MAGICB1 + 3, HK_MAGICB1 + 4, A_FACE, 0),
	ST(F_MAGICB1 + 4,  HK_MAGICB1 + 5,  A_FACE, 0), ST(F_MAGICB1 + 5, HK_MAGICB1 + 6, A_FACE, 0),
	ST(F_MAGICB1 + 6,  HK_MAGICB1 + 7,  A_CUSTOM, C_SHOT0 + 0),
	ST(F_MAGICB1 + 7,  HK_MAGICB1 + 8,  A_CUSTOM, C_SHOT0 + 1),
	ST(F_MAGICB1 + 8,  HK_MAGICB1 + 9,  A_CUSTOM, C_SHOT0 + 2),
	ST(F_MAGICB1 + 9,  HK_MAGICB1 + 10, A_CUSTOM, C_SHOT0 + 3),
	ST(F_MAGICB1 + 10, HK_MAGICB1 + 11, A_CUSTOM, C_SHOT0 + 4),
	ST(F_MAGICB1 + 11, HK_MAGICB1 + 12, A_CUSTOM, C_SHOT0 + 5),
	ST(F_MAGICB1 + 12, HK_RUN1,         A_FACE, 0),
	/* hknight_magicc1-11 (its th_missile) */
	ST(F_MAGICC1 + 0,  HK_MAGICC1 + 1,  A_FACE, 0), ST(F_MAGICC1 + 1, HK_MAGICC1 + 2, A_FACE, 0),
	ST(F_MAGICC1 + 2,  HK_MAGICC1 + 3,  A_FACE, 0), ST(F_MAGICC1 + 3, HK_MAGICC1 + 4, A_FACE, 0),
	ST(F_MAGICC1 + 4,  HK_MAGICC1 + 5,  A_FACE, 0),
	ST(F_MAGICC1 + 5,  HK_MAGICC1 + 6,  A_CUSTOM, C_SHOT0 + 0),
	ST(F_MAGICC1 + 6,  HK_MAGICC1 + 7,  A_CUSTOM, C_SHOT0 + 1),
	ST(F_MAGICC1 + 7,  HK_MAGICC1 + 8,  A_CUSTOM, C_SHOT0 + 2),
	ST(F_MAGICC1 + 8,  HK_MAGICC1 + 9,  A_CUSTOM, C_SHOT0 + 3),
	ST(F_MAGICC1 + 9,  HK_MAGICC1 + 10, A_CUSTOM, C_SHOT0 + 4),
	ST(F_MAGICC1 + 10, HK_RUN1,         A_CUSTOM, C_SHOT0 + 5),
	/* hknight_char_a1-16: the charge, swinging from 6 to 11 */
	ST(F_CHAR_A1 + 0,  HK_CHARA1 + 1,  A_CHARGE, 20), ST(F_CHAR_A1 + 1,  HK_CHARA1 + 2,  A_CHARGE, 25),
	ST(F_CHAR_A1 + 2,  HK_CHARA1 + 3,  A_CHARGE, 18), ST(F_CHAR_A1 + 3,  HK_CHARA1 + 4,  A_CHARGE, 16),
	ST(F_CHAR_A1 + 4,  HK_CHARA1 + 5,  A_CHARGE, 14), ST(F_CHAR_A1 + 5,  HK_CHARA1 + 6,  CM, 20),
	ST(F_CHAR_A1 + 6,  HK_CHARA1 + 7,  CM, 21),       ST(F_CHAR_A1 + 7,  HK_CHARA1 + 8,  CM, 13),
	ST(F_CHAR_A1 + 8,  HK_CHARA1 + 9,  CM, 20),       ST(F_CHAR_A1 + 9,  HK_CHARA1 + 10, CM, 20),
	ST(F_CHAR_A1 + 10, HK_CHARA1 + 11, CM, 18),       ST(F_CHAR_A1 + 11, HK_CHARA1 + 12, A_CHARGE, 16),
	ST(F_CHAR_A1 + 12, HK_CHARA1 + 13, A_CHARGE, 14), ST(F_CHAR_A1 + 13, HK_CHARA1 + 14, A_CHARGE, 25),
	ST(F_CHAR_A1 + 14, HK_CHARA1 + 15, A_CHARGE, 21), ST(F_CHAR_A1 + 15, HK_RUN1,        A_CHARGE, 13),
	/* hknight_char_b1-6: the charge's loop (CheckContinueCharge) */
	ST(F_CHAR_B1 + 0, HK_CHARB1 + 1, A_CUSTOM, C_CHARB1), ST(F_CHAR_B1 + 1, HK_CHARB1 + 2, CM, 17),
	ST(F_CHAR_B1 + 2, HK_CHARB1 + 3, CM, 12),             ST(F_CHAR_B1 + 3, HK_CHARB1 + 4, CM, 22),
	ST(F_CHAR_B1 + 4, HK_CHARB1 + 5, CM, 18),             ST(F_CHAR_B1 + 5, HK_CHARB1 + 0, CM, 8),
	/* hknight_slice1-10 */
	ST(F_SLICE1 + 0, HK_SLICE1 + 1, A_CHARGE, 9),  ST(F_SLICE1 + 1, HK_SLICE1 + 2, A_CHARGE, 6),
	ST(F_SLICE1 + 2, HK_SLICE1 + 3, A_CHARGE, 13), ST(F_SLICE1 + 3, HK_SLICE1 + 4, A_CHARGE, 4),
	ST(F_SLICE1 + 4, HK_SLICE1 + 5, CM, 7),        ST(F_SLICE1 + 5, HK_SLICE1 + 6, CM, 15),
	ST(F_SLICE1 + 6, HK_SLICE1 + 7, CM, 8),        ST(F_SLICE1 + 7, HK_SLICE1 + 8, CM, 2),
	ST(F_SLICE1 + 8, HK_SLICE1 + 9, A_MELEE, 0),   ST(F_SLICE1 + 9, HK_RUN1,       A_CHARGE, 3),
	/* hknight_smash1-11 */
	ST(F_SMASH1 + 0,  HK_SMASH1 + 1,  A_CHARGE, 1),  ST(F_SMASH1 + 1, HK_SMASH1 + 2, A_CHARGE, 13),
	ST(F_SMASH1 + 2,  HK_SMASH1 + 3,  A_CHARGE, 9),  ST(F_SMASH1 + 3, HK_SMASH1 + 4, A_CHARGE, 11),
	ST(F_SMASH1 + 4,  HK_SMASH1 + 5,  CM, 10),       ST(F_SMASH1 + 5, HK_SMASH1 + 6, CM, 7),
	ST(F_SMASH1 + 6,  HK_SMASH1 + 7,  CM, 12),       ST(F_SMASH1 + 7, HK_SMASH1 + 8, CM, 2),
	ST(F_SMASH1 + 8,  HK_SMASH1 + 9,  CM, 3),        ST(F_SMASH1 + 9, HK_SMASH1 + 10, A_CHARGE, 0),
	ST(F_SMASH1 + 10, HK_RUN1,        A_CHARGE, 0),
	/* hknight_watk1-22 */
	ST(F_WATTACK1 + 0,  HK_WATK1 + 1,  A_CHARGE, 2), ST(F_WATTACK1 + 1,  HK_WATK1 + 2,  A_CHARGE, 0),
	ST(F_WATTACK1 + 2,  HK_WATK1 + 3,  A_CHARGE, 0), ST(F_WATTACK1 + 3,  HK_WATK1 + 4,  A_MELEE, 0),
	ST(F_WATTACK1 + 4,  HK_WATK1 + 5,  A_MELEE, 0),  ST(F_WATTACK1 + 5,  HK_WATK1 + 6,  A_MELEE, 0),
	ST(F_WATTACK1 + 6,  HK_WATK1 + 7,  A_CHARGE, 1), ST(F_WATTACK1 + 7,  HK_WATK1 + 8,  A_CHARGE, 4),
	ST(F_WATTACK1 + 8,  HK_WATK1 + 9,  A_CHARGE, 5), ST(F_WATTACK1 + 9,  HK_WATK1 + 10, CM, 3),
	ST(F_WATTACK1 + 10, HK_WATK1 + 11, CM, 2),       ST(F_WATTACK1 + 11, HK_WATK1 + 12, CM, 2),
	ST(F_WATTACK1 + 12, HK_WATK1 + 13, A_CHARGE, 0), ST(F_WATTACK1 + 13, HK_WATK1 + 14, A_CHARGE, 0),
	ST(F_WATTACK1 + 14, HK_WATK1 + 15, A_CHARGE, 0), ST(F_WATTACK1 + 15, HK_WATK1 + 16, A_CHARGE, 1),
	ST(F_WATTACK1 + 16, HK_WATK1 + 17, CM, 1),       ST(F_WATTACK1 + 17, HK_WATK1 + 18, CM, 3),
	ST(F_WATTACK1 + 18, HK_WATK1 + 19, CM, 4),       ST(F_WATTACK1 + 19, HK_WATK1 + 20, A_CHARGE, 6),
	ST(F_WATTACK1 + 20, HK_WATK1 + 21, A_CHARGE, 7), ST(F_WATTACK1 + 21, HK_RUN1,       A_CHARGE, 3),
};

static int16_t s_attack1, s_death1, s_pain1, s_sight1, s_hit, s_slash1, s_idle, s_sword1, s_sword2, s_udeath;
static int     hknight_type;              /* the QC's global: the next swing (every hell knight's) */

static void hknight_map_start(void)
{
	s_attack1 = (int16_t) snd_find("hknight/attack1.wav");
	s_death1  = (int16_t) snd_find("hknight/death1.wav");
	s_pain1   = (int16_t) snd_find("hknight/pain1.wav");
	s_sight1  = (int16_t) snd_find("hknight/sight1.wav");
	s_hit     = (int16_t) snd_find("hknight/hit.wav");
	s_slash1  = (int16_t) snd_find("hknight/slash1.wav");
	s_idle    = (int16_t) snd_find("hknight/idle.wav");
	s_sword1  = (int16_t) snd_find("knight/sword1.wav");
	s_sword2  = (int16_t) snd_find("knight/sword2.wav");
	s_udeath  = (int16_t) snd_find("player/udeath.wav");
	hknight_type = 0;                         /* (the progs' globals: each map's load) */
}

static void hknight_stand(edict_t *self)   { monster_state(self, HK_STAND1); }
static void hknight_walk(edict_t *self)    { monster_state(self, HK_WALK1); }
static void hknight_run(edict_t *self)     { monster_state(self, HK_RUN1); }
static void hknight_missile(edict_t *self) { monster_state(self, HK_MAGICC1); }

/* hknight_melee: slice, smash, then the wide attack, in turn */
static void hknight_melee(edict_t *self)
{
	hknight_type++;
	ent_sound(self, CHAN_WEAPON, s_slash1, 255, ATTN_NORM);
	if (hknight_type == 1) {
		monster_state(self, HK_SLICE1);
	} else if (hknight_type == 2) {
		monster_state(self, HK_SMASH1);
	} else if (hknight_type == 3) {
		monster_state(self, HK_WATK1);
		hknight_type = 0;
	}
}

static void hk_idle_sound(edict_t *self)
{
	if (frand() < 13107)                 /* random () < 0.2 */
		ent_sound(self, CHAN_VOICE, s_idle, 255, ATTN_NORM);
}

/* hknight_shot: a spike towards the enemy, its yaw `offset` x 6 degrees
   off (vectoangles' whole degrees), from 20 forward of the box's centre,
   at 300 a second, its height flipped back (makevectors' pitch) and
   jittered */
static void hknight_shot(edict_t *self, int offset)
{
	int32_t d[3], h[3], f[3], r[3], u[3], org[3], vec[3];
	const edict_t *en = self->enemy ? self->enemy : ent_world;
	for (int k = 0; k < 3; k++)
		d[k] = en->origin[k] - self->origin[k];
	h[0] = (int32_t) q_isqrt64((uint64_t) ((int64_t) d[0] * d[0] + (int64_t) d[1] * d[1]));
	h[1] = d[2];
	h[2] = 0;
	int yaw = vectoyaw(d) + offset * 6, pitch = vectoyaw(h);   /* (vectoangles: atan2 (z, forward)) */
	uint16_t ang[3] = { (uint16_t) (pitch * 65536 / 360), (uint16_t) ((yaw + 360) % 360 * 65536 / 360), 0 };
	ai_makevectors(ang, f, r, u);
	for (int k = 0; k < 3; k++)
		org[k] = self->origin[k] + (self->mins[k] + self->maxs[k]) / 2 + 20 * SUB * f[k] / 4096;
	vec[0] = f[0];
	vec[1] = f[1];
	vec[2] = -f[2] + (int32_t) (((int32_t) frand() - 32768) * 4096 / 655360);   /* + (random () - 0.5) x 0.1 */
	int32_t vel[3];
	for (int k = 0; k < 3; k++)
		vel[k] = vec[k] * 300 / 4096;
	game_monster_missile(MM_KSPIKE, self, org, vel, monster_asset(self, 2), s_hit, SND_NONE);
	ent_sound(self, CHAN_WEAPON, s_attack1, 255, ATTN_NORM);
}

/* CheckForCharge: a mad charge, the enemy in sight on its own level and
   80 units off or more */
static void check_for_charge(edict_t *self)
{
	if (!ai_enemy_visible)
		return;
	if (ent_time < self->attack_finished)
		return;
	const edict_t *en = self->enemy ? self->enemy : ent_world;
	int32_t dz = self->origin[2] - en->origin[2];
	if (dz > 20 * SUB || dz < -20 * SUB)
		return;                          /* too much height change */
	int32_t d[3];
	for (int k = 0; k < 3; k++)
		d[k] = self->origin[k] - en->origin[k];
	if (ai_vlen2(d) < AI_UNITS2(80))
		return;                          /* use regular attack */
	sub_attack_finished(self, TICKS(200));   /* charge */
	monster_state(self, HK_CHARA1);
}

/* CheckContinueCharge: the charge over, or its swords' sound */
static void check_continue_charge(edict_t *self)
{
	if (ent_time > self->attack_finished) {
		sub_attack_finished(self, TICKS(300));
		monster_state(self, HK_RUN1);
		return;                          /* done charging */
	}
	if (frand() > 32768)                 /* random () > 0.5 */
		ent_sound(self, CHAN_WEAPON, s_sword2, 255, ATTN_NORM);
	else
		ent_sound(self, CHAN_WEAPON, s_sword1, 255, ATTN_NORM);
}

static void hknight_custom(edict_t *self, int code)
{
	switch (code) {
	case C_WALK1:
		hk_idle_sound(self);
		ai_walk(self, 2);
		break;
	case C_RUN1:
		hk_idle_sound(self);
		ai_run(self, 20);
		check_for_charge(self);
		break;
	case C_PAIN1:
		ent_sound(self, CHAN_VOICE, s_pain1, 255, ATTN_NORM);
		break;
	case C_DIE3:
		self->solid = SOLID_NOT;
		ent_link(self);
		ai_forward(self, 7);
		break;
	case C_DIEB3:
		self->solid = SOLID_NOT;
		ent_link(self);
		break;
	case C_CHARB1:
		check_continue_charge(self);
		ai_charge(self, 23);
		ai_melee(self);
		break;
	default:                             /* C_SHOT0 + k */
		hknight_shot(self, code - C_SHOT0 - 2);
		break;
	}
}

void hknight_pain(edict_t *self)
{
	if (self->pain_finished > ent_time)
		return;
	ent_sound(self, CHAN_VOICE, s_pain1, 255, ATTN_NORM);
	if (ent_time - self->pain_finished > TICKS(500)) {   /* always go into pain frame if it has
	                                                        been a while */
		monster_state(self, HK_PAIN1);
		self->pain_finished = ent_time + TICKS(100);
		return;
	}
	if (frand() * 30 > (uint32_t) ai_damage_take << 16)   /* random () x 30 > damage: */
		return;                                           /* didn't flinch */
	self->pain_finished = ent_time + TICKS(100);
	monster_state(self, HK_PAIN1);
}

void hknight_die(edict_t *self)
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
	ent_sound(self, CHAN_VOICE, s_death1, 255, ATTN_NORM);
	if (frand() > 32768)
		monster_state(self, HK_DIE1);
	else
		monster_state(self, HK_DIEB1);
}

const mdef_t mdef_hknight = {
	.classname = "monster_hell_knight",
	.assets = { "progs/hknight.mdl", "progs/h_hellkn.mdl", "progs/k_spike.mdl" },
	.mins = { -16, -16, -24 }, .maxs = { 16, 16, 40 },
	.health = 250,
	.kind = MK_HKNIGHT,
	.states = states, .num_states = HK_COUNT,
	.th_stand = hknight_stand, .th_walk = hknight_walk, .th_run = hknight_run,
	.th_missile = hknight_missile, .th_melee = hknight_melee,
	.th_pain = EF_hknight_pain, .th_die = EF_hknight_die,
	.custom = hknight_custom,
	.check_attack = 0,
	.map_start = hknight_map_start,
	.sight = &s_sight1,
};
