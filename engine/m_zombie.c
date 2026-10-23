/*
 * q1psx — the zombie (monster_zombie): zombie.qc, the shareware's 1.06,
 * transcribed state by state: the thrown flesh (engine/game.c's
 * projectiles), the falls and the getting up, the crucified ones. See
 * engine/monsters.h.
 */

#include "game.h"
#include "monsters.h"
#include "q_fixed.h"
#include "snd.h"

#define SUB CLIP_SUB
#define SPAWN_CRUCIFIED 1

/* $frame: stand1-15, walk1-19, run1-18, atta1-13, attb1-14, attc1-12, paina1-12, painb1-28, painc1-18, paind1-13, paine1-30, cruc_1-6 */
enum {
	F_STAND1 = 0, F_WALK1 = 15, F_RUN1 = 34, F_ATTA1 = 52, F_ATTB1 = 65, F_ATTC1 = 79, F_PAINA1 = 91,
	F_PAINB1 = 103, F_PAINC1 = 131, F_PAIND1 = 149, F_PAINE1 = 162, F_CRUC1 = 192
};

enum {
	ZOMBIE_STAND1 = 0, ZOMBIE_CRUC1 = 15, ZOMBIE_WALK1 = 21, ZOMBIE_RUN1 = 40, ZOMBIE_ATTA1 = 58,
	ZOMBIE_ATTB1 = 71, ZOMBIE_ATTC1 = 85, ZOMBIE_PAINA1 = 97, ZOMBIE_PAINB1 = 109,
	ZOMBIE_PAINC1 = 137, ZOMBIE_PAIND1 = 155, ZOMBIE_PAINE1 = 168, ZOMBIE_COUNT = 198
};

enum {
	C_CRUC1, C_CRUC2, C_CRUC3, C_CRUC4, C_CRUC5, C_CRUC6, C_WALK19, C_RUN1, C_RUN18, C_ATTA13,
	C_ATTB14, C_ATTC12, C_PAINA1, C_PAINB1, C_PAINB9, C_PAINC1, C_PAIND1, C_PAINE1, C_PAINE10,
	C_PAINE11, C_PAINE12
};

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[ZOMBIE_COUNT] = {
	/* zombie_stand */
	ST(F_STAND1, ZOMBIE_STAND1 + 1, A_STAND, 0),  ST(F_STAND1 + 1, ZOMBIE_STAND1 + 2, A_STAND, 0),
	ST(F_STAND1 + 2, ZOMBIE_STAND1 + 3, A_STAND, 0), ST(F_STAND1 + 3, ZOMBIE_STAND1 + 4, A_STAND, 0),
	ST(F_STAND1 + 4, ZOMBIE_STAND1 + 5, A_STAND, 0), ST(F_STAND1 + 5, ZOMBIE_STAND1 + 6, A_STAND, 0),
	ST(F_STAND1 + 6, ZOMBIE_STAND1 + 7, A_STAND, 0), ST(F_STAND1 + 7, ZOMBIE_STAND1 + 8, A_STAND, 0),
	ST(F_STAND1 + 8, ZOMBIE_STAND1 + 9, A_STAND, 0), ST(F_STAND1 + 9, ZOMBIE_STAND1 + 10, A_STAND, 0),
	ST(F_STAND1 + 10, ZOMBIE_STAND1 + 11, A_STAND, 0), ST(F_STAND1 + 11, ZOMBIE_STAND1 + 12, A_STAND, 0),
	ST(F_STAND1 + 12, ZOMBIE_STAND1 + 13, A_STAND, 0), ST(F_STAND1 + 13, ZOMBIE_STAND1 + 14, A_STAND, 0),
	ST(F_STAND1 + 14, ZOMBIE_STAND1, A_STAND, 0),
	/* zombie_cruc */
	ST(F_CRUC1, ZOMBIE_CRUC1 + 1, A_CUSTOM, C_CRUC1),
	ST(F_CRUC1 + 1, ZOMBIE_CRUC1 + 2, A_CUSTOM, C_CRUC2),
	ST(F_CRUC1 + 2, ZOMBIE_CRUC1 + 3, A_CUSTOM, C_CRUC3),
	ST(F_CRUC1 + 3, ZOMBIE_CRUC1 + 4, A_CUSTOM, C_CRUC4),
	ST(F_CRUC1 + 4, ZOMBIE_CRUC1 + 5, A_CUSTOM, C_CRUC5),
	ST(F_CRUC1 + 5, ZOMBIE_CRUC1, A_CUSTOM, C_CRUC6),
	/* zombie_walk */
	ST(F_WALK1, ZOMBIE_WALK1 + 1, A_WALK, 0),     ST(F_WALK1 + 1, ZOMBIE_WALK1 + 2, A_WALK, 2),
	ST(F_WALK1 + 2, ZOMBIE_WALK1 + 3, A_WALK, 3), ST(F_WALK1 + 3, ZOMBIE_WALK1 + 4, A_WALK, 2),
	ST(F_WALK1 + 4, ZOMBIE_WALK1 + 5, A_WALK, 1), ST(F_WALK1 + 5, ZOMBIE_WALK1 + 6, A_WALK, 0),
	ST(F_WALK1 + 6, ZOMBIE_WALK1 + 7, A_WALK, 0), ST(F_WALK1 + 7, ZOMBIE_WALK1 + 8, A_WALK, 0),
	ST(F_WALK1 + 8, ZOMBIE_WALK1 + 9, A_WALK, 0), ST(F_WALK1 + 9, ZOMBIE_WALK1 + 10, A_WALK, 0),
	ST(F_WALK1 + 10, ZOMBIE_WALK1 + 11, A_WALK, 2), ST(F_WALK1 + 11, ZOMBIE_WALK1 + 12, A_WALK, 2),
	ST(F_WALK1 + 12, ZOMBIE_WALK1 + 13, A_WALK, 1), ST(F_WALK1 + 13, ZOMBIE_WALK1 + 14, A_WALK, 0),
	ST(F_WALK1 + 14, ZOMBIE_WALK1 + 15, A_WALK, 0), ST(F_WALK1 + 15, ZOMBIE_WALK1 + 16, A_WALK, 0),
	ST(F_WALK1 + 16, ZOMBIE_WALK1 + 17, A_WALK, 0), ST(F_WALK1 + 17, ZOMBIE_WALK1 + 18, A_WALK, 0),
	ST(F_WALK1 + 18, ZOMBIE_WALK1, A_CUSTOM, C_WALK19),
	/* zombie_run */
	ST(F_RUN1, ZOMBIE_RUN1 + 1, A_CUSTOM, C_RUN1),
	ST(F_RUN1 + 1, ZOMBIE_RUN1 + 2, A_RUN, 1),    ST(F_RUN1 + 2, ZOMBIE_RUN1 + 3, A_RUN, 0),
	ST(F_RUN1 + 3, ZOMBIE_RUN1 + 4, A_RUN, 1),    ST(F_RUN1 + 4, ZOMBIE_RUN1 + 5, A_RUN, 2),
	ST(F_RUN1 + 5, ZOMBIE_RUN1 + 6, A_RUN, 3),    ST(F_RUN1 + 6, ZOMBIE_RUN1 + 7, A_RUN, 4),
	ST(F_RUN1 + 7, ZOMBIE_RUN1 + 8, A_RUN, 4),    ST(F_RUN1 + 8, ZOMBIE_RUN1 + 9, A_RUN, 2),
	ST(F_RUN1 + 9, ZOMBIE_RUN1 + 10, A_RUN, 0),   ST(F_RUN1 + 10, ZOMBIE_RUN1 + 11, A_RUN, 0),
	ST(F_RUN1 + 11, ZOMBIE_RUN1 + 12, A_RUN, 0),  ST(F_RUN1 + 12, ZOMBIE_RUN1 + 13, A_RUN, 2),
	ST(F_RUN1 + 13, ZOMBIE_RUN1 + 14, A_RUN, 4),  ST(F_RUN1 + 14, ZOMBIE_RUN1 + 15, A_RUN, 6),
	ST(F_RUN1 + 15, ZOMBIE_RUN1 + 16, A_RUN, 7),  ST(F_RUN1 + 16, ZOMBIE_RUN1 + 17, A_RUN, 3),
	ST(F_RUN1 + 17, ZOMBIE_RUN1, A_CUSTOM, C_RUN18),
	/* zombie_atta */
	ST(F_ATTA1, ZOMBIE_ATTA1 + 1, A_FACE, 0),     ST(F_ATTA1 + 1, ZOMBIE_ATTA1 + 2, A_FACE, 0),
	ST(F_ATTA1 + 2, ZOMBIE_ATTA1 + 3, A_FACE, 0), ST(F_ATTA1 + 3, ZOMBIE_ATTA1 + 4, A_FACE, 0),
	ST(F_ATTA1 + 4, ZOMBIE_ATTA1 + 5, A_FACE, 0), ST(F_ATTA1 + 5, ZOMBIE_ATTA1 + 6, A_FACE, 0),
	ST(F_ATTA1 + 6, ZOMBIE_ATTA1 + 7, A_FACE, 0), ST(F_ATTA1 + 7, ZOMBIE_ATTA1 + 8, A_FACE, 0),
	ST(F_ATTA1 + 8, ZOMBIE_ATTA1 + 9, A_FACE, 0), ST(F_ATTA1 + 9, ZOMBIE_ATTA1 + 10, A_FACE, 0),
	ST(F_ATTA1 + 10, ZOMBIE_ATTA1 + 11, A_FACE, 0), ST(F_ATTA1 + 11, ZOMBIE_ATTA1 + 12, A_FACE, 0),
	ST(F_ATTA1 + 12, ZOMBIE_RUN1, A_CUSTOM, C_ATTA13),
	/* zombie_attb */
	ST(F_ATTB1, ZOMBIE_ATTB1 + 1, A_FACE, 0),     ST(F_ATTB1 + 1, ZOMBIE_ATTB1 + 2, A_FACE, 0),
	ST(F_ATTB1 + 2, ZOMBIE_ATTB1 + 3, A_FACE, 0), ST(F_ATTB1 + 3, ZOMBIE_ATTB1 + 4, A_FACE, 0),
	ST(F_ATTB1 + 4, ZOMBIE_ATTB1 + 5, A_FACE, 0), ST(F_ATTB1 + 5, ZOMBIE_ATTB1 + 6, A_FACE, 0),
	ST(F_ATTB1 + 6, ZOMBIE_ATTB1 + 7, A_FACE, 0), ST(F_ATTB1 + 7, ZOMBIE_ATTB1 + 8, A_FACE, 0),
	ST(F_ATTB1 + 8, ZOMBIE_ATTB1 + 9, A_FACE, 0), ST(F_ATTB1 + 9, ZOMBIE_ATTB1 + 10, A_FACE, 0),
	ST(F_ATTB1 + 10, ZOMBIE_ATTB1 + 11, A_FACE, 0), ST(F_ATTB1 + 11, ZOMBIE_ATTB1 + 12, A_FACE, 0),
	ST(F_ATTB1 + 12, ZOMBIE_ATTB1 + 13, A_FACE, 0),
	ST(F_ATTB1 + 12, ZOMBIE_RUN1, A_CUSTOM, C_ATTB14),
	/* zombie_attc */
	ST(F_ATTC1, ZOMBIE_ATTC1 + 1, A_FACE, 0),     ST(F_ATTC1 + 1, ZOMBIE_ATTC1 + 2, A_FACE, 0),
	ST(F_ATTC1 + 2, ZOMBIE_ATTC1 + 3, A_FACE, 0), ST(F_ATTC1 + 3, ZOMBIE_ATTC1 + 4, A_FACE, 0),
	ST(F_ATTC1 + 4, ZOMBIE_ATTC1 + 5, A_FACE, 0), ST(F_ATTC1 + 5, ZOMBIE_ATTC1 + 6, A_FACE, 0),
	ST(F_ATTC1 + 6, ZOMBIE_ATTC1 + 7, A_FACE, 0), ST(F_ATTC1 + 7, ZOMBIE_ATTC1 + 8, A_FACE, 0),
	ST(F_ATTC1 + 8, ZOMBIE_ATTC1 + 9, A_FACE, 0), ST(F_ATTC1 + 9, ZOMBIE_ATTC1 + 10, A_FACE, 0),
	ST(F_ATTC1 + 10, ZOMBIE_ATTC1 + 11, A_FACE, 0),
	ST(F_ATTC1 + 11, ZOMBIE_RUN1, A_CUSTOM, C_ATTC12),
	/* zombie_paina */
	ST(F_PAINA1, ZOMBIE_PAINA1 + 1, A_CUSTOM, C_PAINA1),
	ST(F_PAINA1 + 1, ZOMBIE_PAINA1 + 2, A_PAINFWD, 3), ST(F_PAINA1 + 2, ZOMBIE_PAINA1 + 3, A_PAINFWD, 1),
	ST(F_PAINA1 + 3, ZOMBIE_PAINA1 + 4, A_PAIN, 1), ST(F_PAINA1 + 4, ZOMBIE_PAINA1 + 5, A_PAIN, 3),
	ST(F_PAINA1 + 5, ZOMBIE_PAINA1 + 6, A_PAIN, 1), ST(F_PAINA1 + 6, ZOMBIE_PAINA1 + 7, A_NONE, 0),
	ST(F_PAINA1 + 7, ZOMBIE_PAINA1 + 8, A_NONE, 0), ST(F_PAINA1 + 8, ZOMBIE_PAINA1 + 9, A_NONE, 0),
	ST(F_PAINA1 + 9, ZOMBIE_PAINA1 + 10, A_NONE, 0), ST(F_PAINA1 + 10, ZOMBIE_PAINA1 + 11, A_NONE, 0),
	ST(F_PAINA1 + 11, ZOMBIE_RUN1, A_NONE, 0),
	/* zombie_painb */
	ST(F_PAINB1, ZOMBIE_PAINB1 + 1, A_CUSTOM, C_PAINB1),
	ST(F_PAINB1 + 1, ZOMBIE_PAINB1 + 2, A_PAIN, 2), ST(F_PAINB1 + 2, ZOMBIE_PAINB1 + 3, A_PAIN, 8),
	ST(F_PAINB1 + 3, ZOMBIE_PAINB1 + 4, A_PAIN, 6), ST(F_PAINB1 + 4, ZOMBIE_PAINB1 + 5, A_PAIN, 2),
	ST(F_PAINB1 + 5, ZOMBIE_PAINB1 + 6, A_NONE, 0), ST(F_PAINB1 + 6, ZOMBIE_PAINB1 + 7, A_NONE, 0),
	ST(F_PAINB1 + 7, ZOMBIE_PAINB1 + 8, A_NONE, 0),
	ST(F_PAINB1 + 8, ZOMBIE_PAINB1 + 9, A_CUSTOM, C_PAINB9),
	ST(F_PAINB1 + 9, ZOMBIE_PAINB1 + 10, A_NONE, 0), ST(F_PAINB1 + 10, ZOMBIE_PAINB1 + 11, A_NONE, 0),
	ST(F_PAINB1 + 11, ZOMBIE_PAINB1 + 12, A_NONE, 0), ST(F_PAINB1 + 12, ZOMBIE_PAINB1 + 13, A_NONE, 0),
	ST(F_PAINB1 + 13, ZOMBIE_PAINB1 + 14, A_NONE, 0), ST(F_PAINB1 + 14, ZOMBIE_PAINB1 + 15, A_NONE, 0),
	ST(F_PAINB1 + 15, ZOMBIE_PAINB1 + 16, A_NONE, 0), ST(F_PAINB1 + 16, ZOMBIE_PAINB1 + 17, A_NONE, 0),
	ST(F_PAINB1 + 17, ZOMBIE_PAINB1 + 18, A_NONE, 0), ST(F_PAINB1 + 18, ZOMBIE_PAINB1 + 19, A_NONE, 0),
	ST(F_PAINB1 + 19, ZOMBIE_PAINB1 + 20, A_NONE, 0), ST(F_PAINB1 + 20, ZOMBIE_PAINB1 + 21, A_NONE, 0),
	ST(F_PAINB1 + 21, ZOMBIE_PAINB1 + 22, A_NONE, 0), ST(F_PAINB1 + 22, ZOMBIE_PAINB1 + 23, A_NONE, 0),
	ST(F_PAINB1 + 23, ZOMBIE_PAINB1 + 24, A_NONE, 0), ST(F_PAINB1 + 24, ZOMBIE_PAINB1 + 25, A_PAINFWD, 1),
	ST(F_PAINB1 + 25, ZOMBIE_PAINB1 + 26, A_NONE, 0), ST(F_PAINB1 + 26, ZOMBIE_PAINB1 + 27, A_NONE, 0),
	ST(F_PAINB1 + 27, ZOMBIE_RUN1, A_NONE, 0),
	/* zombie_painc */
	ST(F_PAINC1, ZOMBIE_PAINC1 + 1, A_CUSTOM, C_PAINC1),
	ST(F_PAINC1 + 1, ZOMBIE_PAINC1 + 2, A_NONE, 0), ST(F_PAINC1 + 2, ZOMBIE_PAINC1 + 3, A_PAIN, 3),
	ST(F_PAINC1 + 3, ZOMBIE_PAINC1 + 4, A_PAIN, 1), ST(F_PAINC1 + 4, ZOMBIE_PAINC1 + 5, A_NONE, 0),
	ST(F_PAINC1 + 5, ZOMBIE_PAINC1 + 6, A_NONE, 0), ST(F_PAINC1 + 6, ZOMBIE_PAINC1 + 7, A_NONE, 0),
	ST(F_PAINC1 + 7, ZOMBIE_PAINC1 + 8, A_NONE, 0), ST(F_PAINC1 + 8, ZOMBIE_PAINC1 + 9, A_NONE, 0),
	ST(F_PAINC1 + 9, ZOMBIE_PAINC1 + 10, A_NONE, 0), ST(F_PAINC1 + 10, ZOMBIE_PAINC1 + 11, A_PAINFWD, 1),
	ST(F_PAINC1 + 11, ZOMBIE_PAINC1 + 12, A_PAINFWD, 1), ST(F_PAINC1 + 12, ZOMBIE_PAINC1 + 13, A_NONE, 0),
	ST(F_PAINC1 + 13, ZOMBIE_PAINC1 + 14, A_NONE, 0), ST(F_PAINC1 + 14, ZOMBIE_PAINC1 + 15, A_NONE, 0),
	ST(F_PAINC1 + 15, ZOMBIE_PAINC1 + 16, A_NONE, 0), ST(F_PAINC1 + 16, ZOMBIE_PAINC1 + 17, A_NONE, 0),
	ST(F_PAINC1 + 17, ZOMBIE_RUN1, A_NONE, 0),
	/* zombie_paind */
	ST(F_PAIND1, ZOMBIE_PAIND1 + 1, A_CUSTOM, C_PAIND1),
	ST(F_PAIND1 + 1, ZOMBIE_PAIND1 + 2, A_NONE, 0), ST(F_PAIND1 + 2, ZOMBIE_PAIND1 + 3, A_NONE, 0),
	ST(F_PAIND1 + 3, ZOMBIE_PAIND1 + 4, A_NONE, 0), ST(F_PAIND1 + 4, ZOMBIE_PAIND1 + 5, A_NONE, 0),
	ST(F_PAIND1 + 5, ZOMBIE_PAIND1 + 6, A_NONE, 0), ST(F_PAIND1 + 6, ZOMBIE_PAIND1 + 7, A_NONE, 0),
	ST(F_PAIND1 + 7, ZOMBIE_PAIND1 + 8, A_NONE, 0), ST(F_PAIND1 + 8, ZOMBIE_PAIND1 + 9, A_PAIN, 1),
	ST(F_PAIND1 + 9, ZOMBIE_PAIND1 + 10, A_NONE, 0), ST(F_PAIND1 + 10, ZOMBIE_PAIND1 + 11, A_NONE, 0),
	ST(F_PAIND1 + 11, ZOMBIE_PAIND1 + 12, A_NONE, 0), ST(F_PAIND1 + 12, ZOMBIE_RUN1, A_NONE, 0),
	/* zombie_paine */
	ST(F_PAINE1, ZOMBIE_PAINE1 + 1, A_CUSTOM, C_PAINE1),
	ST(F_PAINE1 + 1, ZOMBIE_PAINE1 + 2, A_PAIN, 8), ST(F_PAINE1 + 2, ZOMBIE_PAINE1 + 3, A_PAIN, 5),
	ST(F_PAINE1 + 3, ZOMBIE_PAINE1 + 4, A_PAIN, 3), ST(F_PAINE1 + 4, ZOMBIE_PAINE1 + 5, A_PAIN, 1),
	ST(F_PAINE1 + 5, ZOMBIE_PAINE1 + 6, A_PAIN, 2), ST(F_PAINE1 + 6, ZOMBIE_PAINE1 + 7, A_PAIN, 1),
	ST(F_PAINE1 + 7, ZOMBIE_PAINE1 + 8, A_PAIN, 1), ST(F_PAINE1 + 8, ZOMBIE_PAINE1 + 9, A_PAIN, 2),
	ST(F_PAINE1 + 9, ZOMBIE_PAINE1 + 10, A_CUSTOM, C_PAINE10),
	ST(F_PAINE1 + 10, ZOMBIE_PAINE1 + 11, A_CUSTOM, C_PAINE11),
	ST(F_PAINE1 + 11, ZOMBIE_PAINE1 + 12, A_CUSTOM, C_PAINE12),
	ST(F_PAINE1 + 12, ZOMBIE_PAINE1 + 13, A_NONE, 0), ST(F_PAINE1 + 13, ZOMBIE_PAINE1 + 14, A_NONE, 0),
	ST(F_PAINE1 + 14, ZOMBIE_PAINE1 + 15, A_NONE, 0), ST(F_PAINE1 + 15, ZOMBIE_PAINE1 + 16, A_NONE, 0),
	ST(F_PAINE1 + 16, ZOMBIE_PAINE1 + 17, A_NONE, 0), ST(F_PAINE1 + 17, ZOMBIE_PAINE1 + 18, A_NONE, 0),
	ST(F_PAINE1 + 18, ZOMBIE_PAINE1 + 19, A_NONE, 0), ST(F_PAINE1 + 19, ZOMBIE_PAINE1 + 20, A_NONE, 0),
	ST(F_PAINE1 + 20, ZOMBIE_PAINE1 + 21, A_NONE, 0), ST(F_PAINE1 + 21, ZOMBIE_PAINE1 + 22, A_NONE, 0),
	ST(F_PAINE1 + 22, ZOMBIE_PAINE1 + 23, A_NONE, 0), ST(F_PAINE1 + 23, ZOMBIE_PAINE1 + 24, A_NONE, 0),
	ST(F_PAINE1 + 24, ZOMBIE_PAINE1 + 25, A_PAINFWD, 5), ST(F_PAINE1 + 25, ZOMBIE_PAINE1 + 26, A_PAINFWD, 3),
	ST(F_PAINE1 + 26, ZOMBIE_PAINE1 + 27, A_PAINFWD, 1), ST(F_PAINE1 + 27, ZOMBIE_PAINE1 + 28, A_PAIN, 1),
	ST(F_PAINE1 + 28, ZOMBIE_PAINE1 + 29, A_NONE, 0), ST(F_PAINE1 + 29, ZOMBIE_RUN1, A_NONE, 0),
};

static int16_t s_idle_w2, s_z_idle, s_z_idle1, s_z_shot1, s_z_gib, s_z_pain, s_z_pain1, s_z_fall, s_z_miss,
               s_z_hit;

static void zombie_map_start(void)
{
	s_idle_w2 = (int16_t) snd_find("zombie/idle_w2.wav");
	s_z_idle  = (int16_t) snd_find("zombie/z_idle.wav");
	s_z_idle1 = (int16_t) snd_find("zombie/z_idle1.wav");
	s_z_shot1 = (int16_t) snd_find("zombie/z_shot1.wav");
	s_z_gib   = (int16_t) snd_find("zombie/z_gib.wav");
	s_z_pain  = (int16_t) snd_find("zombie/z_pain.wav");
	s_z_pain1 = (int16_t) snd_find("zombie/z_pain1.wav");
	s_z_fall  = (int16_t) snd_find("zombie/z_fall.wav");
	s_z_miss  = (int16_t) snd_find("zombie/z_miss.wav");
	s_z_hit   = (int16_t) snd_find("zombie/z_hit.wav");
}

static void zombie_stand(edict_t *self) { monster_state(self, ZOMBIE_STAND1); }
static void zombie_walk(edict_t *self)  { monster_state(self, ZOMBIE_WALK1); }
static void zombie_run(edict_t *self)   { monster_state(self, ZOMBIE_RUN1); }

/* zombie_missile: one of the three throws */
static void zombie_missile(edict_t *self)
{
	uint32_t r = frand();
	if (r < 19661)                       /* 0.3 */
		monster_state(self, ZOMBIE_ATTA1);
	else if (r < 39322)                  /* 0.6 */
		monster_state(self, ZOMBIE_ATTB1);
	else
		monster_state(self, ZOMBIE_ATTC1);
}

/* ZombieFireGrenade (st): a piece of flesh at the enemy, 600 across and 200
   up, from st (forward, right, up - 24) off the origin along the last
   makevectors' vectors (1.06 takes them before its own: the player's
   view's, or a monster's that thought last) */
static void zombie_fire_grenade(edict_t *self, int sf, int sr, int su)
{
	ent_sound(self, CHAN_WEAPON, s_z_shot1, 255, ATTN_NORM);
	int32_t org[3], f[3], r[3], u[3], d[3], v[3];
	for (int k = 0; k < 3; k++)
		org[k] = self->origin[k] + (sf * ai_v_forward[k] + sr * ai_v_right[k] + (su - 24) * ai_v_up[k]) * SUB / 4096;
	ai_makevectors(self->ang, f, r, u);
	for (int k = 0; k < 3; k++)
		d[k] = self->enemy->origin[k] - org[k];
	int32_t len = (int32_t) q_isqrt64((uint64_t) ai_vlen2(d));
	for (int k = 0; k < 3; k++)
		v[k] = len ? d[k] * 600 / len : 0;
	v[2] = 200;
	game_monster_missile(MM_ZOMGIB, self, org, v, monster_asset(self, 2), s_z_hit, s_z_miss);
}

static void zombie_custom(edict_t *self, int code)
{
	switch (code) {
	case C_CRUC1:
		if (frand() < 6554)              /* random () < 0.1 */
			ent_sound(self, CHAN_VOICE, s_idle_w2, 255, ATTN_STATIC);
		break;
	case C_CRUC2:
	case C_CRUC3:
	case C_CRUC4:
	case C_CRUC5:
	case C_CRUC6:                        /* nextthink = time + 0.1 + random () x 0.1 */
		self->nextthink = ent_time + TICKS(10) + (int32_t) (frand() * TICKS(10) >> 16);
		break;
	case C_WALK19:
		ai_walk(self, 0);
		if (frand() < 13107)             /* 0.2 */
			ent_sound(self, CHAN_VOICE, s_z_idle, 255, ATTN_IDLE);
		break;
	case C_RUN1:
		ai_run(self, 1);
		self->inpain = 0;
		break;
	case C_RUN18:
		ai_run(self, 8);
		if (frand() < 13107)
			ent_sound(self, CHAN_VOICE, s_z_idle, 255, ATTN_IDLE);
		if (frand() > 52429)             /* 0.8 */
			ent_sound(self, CHAN_VOICE, s_z_idle1, 255, ATTN_IDLE);
		break;
	case C_ATTA13:
		ai_face(self);
		zombie_fire_grenade(self, -10, -22, 30);
		break;
	case C_ATTB14:
		ai_face(self);
		zombie_fire_grenade(self, -10, -24, 29);
		break;
	case C_ATTC12:
		ai_face(self);
		zombie_fire_grenade(self, -12, -19, 29);
		break;
	case C_PAINA1:
	case C_PAIND1:
		ent_sound(self, CHAN_VOICE, s_z_pain, 255, ATTN_NORM);
		break;
	case C_PAINB1:
	case C_PAINC1:
		ent_sound(self, CHAN_VOICE, s_z_pain1, 255, ATTN_NORM);
		break;
	case C_PAINB9:
		ent_sound(self, CHAN_BODY, s_z_fall, 255, ATTN_NORM);
		break;
	case C_PAINE1:
		ent_sound(self, CHAN_VOICE, s_z_pain, 255, ATTN_NORM);
		self->health = 60;
		break;
	case C_PAINE10:                      /* down */
		ent_sound(self, CHAN_BODY, s_z_fall, 255, ATTN_NORM);
		self->solid = SOLID_NOT;
		ent_link(self);
		break;
	case C_PAINE11:                      /* five seconds on the floor */
		self->nextthink += TICKS(500);
		self->health = 60;
		break;
	case C_PAINE12:                      /* up again, if there is room */
		self->health = 60;
		ent_sound(self, CHAN_VOICE, s_z_idle, 255, ATTN_IDLE);
		self->solid = SOLID_SLIDEBOX;
		if (!ai_walkmove(self, 0, 0)) {
			monster_think_at(self, ZOMBIE_PAINE1 + 10, TICKS(10));   /* zombie_paine11 */
			self->solid = SOLID_NOT;
			ent_link(self);
			return;
		}
		break;
	}
}

/* zombie_pain: the health back to 60 every hit; a small one does nothing,
   a big one (25) or one while hurting (within 3 s of the last) knocks it
   down */
static void zombie_pain(edict_t *self)
{
	int take = ai_damage_take;
	self->health = 60;                   /* allways reset health */
	if (take < 9)
		return;                          /* totally ignore */
	if (self->inpain == 2)
		return;                          /* down on ground, so don't reset any counters */
	/* go down immediately if a big enough hit */
	if (take >= 25) {
		self->inpain = 2;
		monster_state(self, ZOMBIE_PAINE1);
		return;
	}
	if (self->inpain) {
		/* if hit again in next gre seconds while not in pain frames,
		   definately drop */
		self->pain_finished = ent_time + TICKS(300);
		return;                          /* currently going through an animation, don't change */
	}
	if (self->pain_finished > ent_time) {
		/* hit again, so drop down */
		self->inpain = 2;
		monster_state(self, ZOMBIE_PAINE1);
		return;
	}
	/* go into one of the fast pain animations */
	self->inpain = 1;
	uint32_t r = frand();
	if (r < 16384)
		monster_state(self, ZOMBIE_PAINA1);
	else if (r < 32768)
		monster_state(self, ZOMBIE_PAINB1);
	else if (r < 49152)
		monster_state(self, ZOMBIE_PAINC1);
	else
		monster_state(self, ZOMBIE_PAIND1);
}

static void zombie_die(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, s_z_gib, 255, ATTN_NORM);
	throw_head(self, self->health);
	throw_gib(self, 0, self->health);
	throw_gib(self, 1, self->health);
	throw_gib(self, 2, self->health);
}

/* monster_zombie's end: a crucified one hangs there (no start: never
   counted, never hurt), the others start as walking monsters */
static int zombie_spawn(edict_t *self)
{
	if (!(self->spawnflags & SPAWN_CRUCIFIED))
		return 0;
	self->movetype = MOVETYPE_NONE;
	monster_state(self, ZOMBIE_CRUC1);
	return 1;
}

const mdef_t mdef_zombie = {
	.classname = "monster_zombie",
	.assets = { "progs/zombie.mdl", "progs/h_zombie.mdl", "progs/zom_gib.mdl" },
	.mins = { -16, -16, -24 }, .maxs = { 16, 16, 40 },
	.health = 60,
	.kind = MK_ZOMBIE,
	.states = states, .num_states = ZOMBIE_COUNT,
	.th_stand = zombie_stand, .th_walk = zombie_walk, .th_run = zombie_run,
	.th_missile = zombie_missile, .th_melee = 0,
	.th_pain = zombie_pain, .th_die = zombie_die,
	.custom = zombie_custom,
	.spawn = zombie_spawn,
	.map_start = zombie_map_start,
	.sight = &s_z_idle,
};
