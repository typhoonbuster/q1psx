/*
 * q1psx — Shub-Niggurath (monster_oldone): oldone.qc, the 1.06 progs.dat's
 * (the registered game's; the rerelease's shub.qc for the frames' names),
 * transcribed state by state: no AI (its idle loop), its health back to
 * the full at every hurt (nopain: only the teleporter's telefrag kills
 * it), its death the game's end (finale_1-4: the finale's camera, the
 * fog, the thrash pulsing the world's light style 0, the gibs, the text,
 * the player's statue). See engine/monsters.h.
 */

#include <stdio.h>

#include "game.h"
#include "level.h"
#include "monsters.h"
#include "music.h"
#include "snd.h"

#define SUB CLIP_SUB

/* $frame: old1-46, shake1-21 */
enum { F_OLD1 = 0, F_SHAKE1 = 46 };

enum { OLD_IDLE1 = 0, OLD_THRASH1 = 46, OLD_COUNT = 66 };

/* a code from 'a' up: lightstyle (0, that letter) */
enum { C_THRASH15 = 1, C_FINALE4 };

/* its 40000 health, in the edicts' 16 bits: what nopain gives back and the
   telefrag's 50000 alone takes away all the same */
#define OLD_HEALTH 30000

#define ST(f, n, a, d) { (uint8_t) (f), (uint8_t) (n), (uint8_t) (a), (int8_t) (d) }

static const mstate_t states[OLD_COUNT] = {
	/* old_idle1-46 =[ $old1, old_idle2 ] {} */
	ST(F_OLD1 + 0,  OLD_IDLE1 + 1,  A_NONE, 0), ST(F_OLD1 + 1,  OLD_IDLE1 + 2,  A_NONE, 0),
	ST(F_OLD1 + 2,  OLD_IDLE1 + 3,  A_NONE, 0), ST(F_OLD1 + 3,  OLD_IDLE1 + 4,  A_NONE, 0),
	ST(F_OLD1 + 4,  OLD_IDLE1 + 5,  A_NONE, 0), ST(F_OLD1 + 5,  OLD_IDLE1 + 6,  A_NONE, 0),
	ST(F_OLD1 + 6,  OLD_IDLE1 + 7,  A_NONE, 0), ST(F_OLD1 + 7,  OLD_IDLE1 + 8,  A_NONE, 0),
	ST(F_OLD1 + 8,  OLD_IDLE1 + 9,  A_NONE, 0), ST(F_OLD1 + 9,  OLD_IDLE1 + 10, A_NONE, 0),
	ST(F_OLD1 + 10, OLD_IDLE1 + 11, A_NONE, 0), ST(F_OLD1 + 11, OLD_IDLE1 + 12, A_NONE, 0),
	ST(F_OLD1 + 12, OLD_IDLE1 + 13, A_NONE, 0), ST(F_OLD1 + 13, OLD_IDLE1 + 14, A_NONE, 0),
	ST(F_OLD1 + 14, OLD_IDLE1 + 15, A_NONE, 0), ST(F_OLD1 + 15, OLD_IDLE1 + 16, A_NONE, 0),
	ST(F_OLD1 + 16, OLD_IDLE1 + 17, A_NONE, 0), ST(F_OLD1 + 17, OLD_IDLE1 + 18, A_NONE, 0),
	ST(F_OLD1 + 18, OLD_IDLE1 + 19, A_NONE, 0), ST(F_OLD1 + 19, OLD_IDLE1 + 20, A_NONE, 0),
	ST(F_OLD1 + 20, OLD_IDLE1 + 21, A_NONE, 0), ST(F_OLD1 + 21, OLD_IDLE1 + 22, A_NONE, 0),
	ST(F_OLD1 + 22, OLD_IDLE1 + 23, A_NONE, 0), ST(F_OLD1 + 23, OLD_IDLE1 + 24, A_NONE, 0),
	ST(F_OLD1 + 24, OLD_IDLE1 + 25, A_NONE, 0), ST(F_OLD1 + 25, OLD_IDLE1 + 26, A_NONE, 0),
	ST(F_OLD1 + 26, OLD_IDLE1 + 27, A_NONE, 0), ST(F_OLD1 + 27, OLD_IDLE1 + 28, A_NONE, 0),
	ST(F_OLD1 + 28, OLD_IDLE1 + 29, A_NONE, 0), ST(F_OLD1 + 29, OLD_IDLE1 + 30, A_NONE, 0),
	ST(F_OLD1 + 30, OLD_IDLE1 + 31, A_NONE, 0), ST(F_OLD1 + 31, OLD_IDLE1 + 32, A_NONE, 0),
	ST(F_OLD1 + 32, OLD_IDLE1 + 33, A_NONE, 0), ST(F_OLD1 + 33, OLD_IDLE1 + 34, A_NONE, 0),
	ST(F_OLD1 + 34, OLD_IDLE1 + 35, A_NONE, 0), ST(F_OLD1 + 35, OLD_IDLE1 + 36, A_NONE, 0),
	ST(F_OLD1 + 36, OLD_IDLE1 + 37, A_NONE, 0), ST(F_OLD1 + 37, OLD_IDLE1 + 38, A_NONE, 0),
	ST(F_OLD1 + 38, OLD_IDLE1 + 39, A_NONE, 0), ST(F_OLD1 + 39, OLD_IDLE1 + 40, A_NONE, 0),
	ST(F_OLD1 + 40, OLD_IDLE1 + 41, A_NONE, 0), ST(F_OLD1 + 41, OLD_IDLE1 + 42, A_NONE, 0),
	ST(F_OLD1 + 42, OLD_IDLE1 + 43, A_NONE, 0), ST(F_OLD1 + 43, OLD_IDLE1 + 44, A_NONE, 0),
	ST(F_OLD1 + 44, OLD_IDLE1 + 45, A_NONE, 0), ST(F_OLD1 + 45, OLD_IDLE1 + 0,  A_NONE, 0),
	/* old_thrash1-20: the world's light style 0 a letter a frame, 1-15
	   three times (shake13 skipped), then finale_4 */
	ST(F_SHAKE1 + 0,  OLD_THRASH1 + 1,  A_CUSTOM, 'm'), ST(F_SHAKE1 + 1,  OLD_THRASH1 + 2,  A_CUSTOM, 'k'),
	ST(F_SHAKE1 + 2,  OLD_THRASH1 + 3,  A_CUSTOM, 'k'), ST(F_SHAKE1 + 3,  OLD_THRASH1 + 4,  A_CUSTOM, 'i'),
	ST(F_SHAKE1 + 4,  OLD_THRASH1 + 5,  A_CUSTOM, 'g'), ST(F_SHAKE1 + 5,  OLD_THRASH1 + 6,  A_CUSTOM, 'e'),
	ST(F_SHAKE1 + 6,  OLD_THRASH1 + 7,  A_CUSTOM, 'c'), ST(F_SHAKE1 + 7,  OLD_THRASH1 + 8,  A_CUSTOM, 'a'),
	ST(F_SHAKE1 + 8,  OLD_THRASH1 + 9,  A_CUSTOM, 'c'), ST(F_SHAKE1 + 9,  OLD_THRASH1 + 10, A_CUSTOM, 'e'),
	ST(F_SHAKE1 + 10, OLD_THRASH1 + 11, A_CUSTOM, 'g'), ST(F_SHAKE1 + 11, OLD_THRASH1 + 12, A_CUSTOM, 'i'),
	ST(F_SHAKE1 + 13, OLD_THRASH1 + 13, A_CUSTOM, 'k'), ST(F_SHAKE1 + 14, OLD_THRASH1 + 14, A_CUSTOM, 'm'),
	ST(F_SHAKE1 + 15, OLD_THRASH1 + 15, A_CUSTOM, C_THRASH15),
	ST(F_SHAKE1 + 16, OLD_THRASH1 + 16, A_CUSTOM, 'g'), ST(F_SHAKE1 + 17, OLD_THRASH1 + 17, A_CUSTOM, 'c'),
	ST(F_SHAKE1 + 18, OLD_THRASH1 + 18, A_CUSTOM, 'b'), ST(F_SHAKE1 + 19, OLD_THRASH1 + 19, A_CUSTOM, 'a'),
	ST(F_SHAKE1 + 20, OLD_THRASH1 + 19, A_CUSTOM, C_FINALE4),
};

static int16_t  s_death, s_pop2, s_tele1;
static edict_t *shub;                     /* the QC's global */

static void old_map_start(void)
{
	s_death = (int16_t) snd_find("boss2/death.wav");
	s_pop2  = (int16_t) snd_find("boss2/pop2.wav");
	s_tele1 = (int16_t) snd_find("misc/r_tele1.wav");
	shub    = 0;
}

static void old_idle(edict_t *self) { monster_state(self, OLD_IDLE1); }

/* finale_4: the pop, fifty gibs from its body, the text, the player's
   statue where the camera sees it, the music, the light back */
static void finale_4(edict_t *self)
{
	ent_sound(self, CHAN_VOICE, s_pop2, 255, ATTN_NORM);
	int32_t oldo[3] = { self->origin[0], self->origin[1], self->origin[2] };
	for (int z = 16; z <= 144; z += 96)
		for (int x = -64; x <= 64; x += 32)
			for (int y = -64; y <= 64; y += 32) {
				self->origin[0] = oldo[0] + x * SUB;
				self->origin[1] = oldo[1] + y * SUB;
				self->origin[2] = oldo[2] + z * SUB;
				uint32_t r = frand();
				throw_gib(self, r < 19661 ? 0 : r < 39322 ? 1 : 2, -999);   /* 0.3, 0.6 */
			}
	level_finale_say(game_finale_text("END"));   /* SVC_FINALE */
	edict_t *n = ent_spawn();
	if (n) {
		n->masset = (int16_t) monster_asset(self, 2);   /* progs/player.mdl: its frame 1 alone */
		int32_t org[3] = { oldo[0] - 32 * SUB, oldo[1] - 264 * SUB, oldo[2] };
		ent_setorigin(n, org);
		n->ang[1] = (uint16_t) (290 * 65536 / 360);
		n->frame  = 0;                       /* (frame 1: the asset's one) */
		ent_wake(n);
	}
	ent_remove(self);
	music_play(3);                           /* SVC_CDTRACK 3 */
	level_lightstyle0("m");
}

static void old_custom(edict_t *self, int code)
{
	if (code >= 'a') {
		char s[2] = { (char) code, 0 };
		level_lightstyle0(s);
		return;
	}
	switch (code) {
	case C_THRASH15:
		level_lightstyle0("m");
		self->cnt++;
		if (self->cnt != 3)
			self->mstate = OLD_THRASH1;      /* self.think = old_thrash1 */
		break;
	case C_FINALE4:
		finale_4(self);
		break;
	}
}

/* nopain: back to the full */
void old_nopain(edict_t *self)
{
	self->health = OLD_HEALTH;
}

/* finale_1 (its th_die): the finale at the intermission's spot, the
   teleporter's train gone, the next part in a second */
void finale_1(edict_t *self)
{
	edict_t *pos = ent_find_class(0, QE_INFO_INTERMISSION);
	if (!pos) {
		printf("no info_intermission\n");    /* (the QC's error ()) */
		return;
	}
	edict_t *train = ent_find_class(0, QE_MISC_TELEPORTTRAIN);
	if (!train) {
		printf("no teleporttrain\n");
		return;
	}
	ent_remove(train);
	level_finale_begin(pos);                 /* (the player there: unseen, untouchable) */
	self->pain_finished = 0x3fffffff;        /* (its end is finale_4's, not a body's) */
	edict_t *timer = ent_spawn();
	if (timer) {
		timer->nextthink = ent_time + TICKS(100);
		timer->think     = EF_finale_2;
		ent_wake(timer);
	}
}

/* finale_2: the fog 100 units before it, then the thrash in 2 s */
void finale_2(edict_t *self)
{
	int32_t o[3] = { shub->origin[0], shub->origin[1] - 100 * SUB, shub->origin[2] };
	game_teleport_splash(o);                 /* TE_TELEPORT */
	ent_sound(shub, CHAN_VOICE, s_tele1, 255, ATTN_NORM);
	self->nextthink = ent_time + TICKS(200);
	self->think     = EF_finale_3;
	ent_wake(self);
}

/* finale_3: Shub thrashing at its next think, its death cry, the light
   style 0 pulsing */
void finale_3(edict_t *self)
{
	(void) self;
	shub->mstate = OLD_THRASH1;              /* shub.think = old_thrash1 */
	shub->think  = EF_monster_think;
	ent_sound(shub, CHAN_VOICE, s_death, 255, ATTN_NORM);
	level_lightstyle0("abcdefghijklmlkjihgfedcb");
}

/* monster_oldone's end: no walkmonster_start, its idle in 0.1 s, hurt by
   anything (DAMAGE_YES), counted */
static int old_spawn(edict_t *self)
{
	self->health    = OLD_HEALTH;
	self->mstate    = OLD_IDLE1;
	self->think     = EF_monster_think;
	self->nextthink = ent_time + TICKS(10);
	self->takedamage = DAMAGE_YES;
	shub = self;
	ent_total_monsters++;
	ent_wake(self);
	return 1;
}

const mdef_t mdef_oldone = {
	.classname = "monster_oldone",
	.assets = { "progs/oldone.mdl", 0, "progs/player.mdl" },
	.mins = { -160, -128, -24 }, .maxs = { 160, 128, 256 },
	.health = OLD_HEALTH,
	.kind = MK_OLDONE,
	.states = states, .num_states = OLD_COUNT,
	.th_stand = old_idle, .th_walk = old_idle, .th_run = old_idle,
	.th_missile = 0, .th_melee = 0,
	.th_pain = EF_old_nopain, .th_die = EF_finale_1,
	.custom = old_custom,
	.check_attack = 0,
	.spawn = old_spawn,
	.map_start = old_map_start,
	.sight = 0,
};
