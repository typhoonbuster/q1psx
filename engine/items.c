/*
 * q1psx — items.qc and misc.qc's model entities, transcribed: health
 * (rotten, normal, mega and its rot), armor, ammo boxes, weapons, keys,
 * the rune (sigil), the four powerups; the torches' and flames' models,
 * the fireballs, the explosive boxes, the spike shooters. Each item is
 * dropped to the floor 0.2 s after the map starts (PlaceItem); the player
 * picks it up by touching its box (a unit wider, 15 across: FL_ITEM). The
 * boxes (health, ammo, the explosive ones) are the map's own models
 * (formats/qgame.h QIM_*); the alias models are streamed by their distance
 * (engine/stream.h: the weapons, keys and runes always in), drawn with the
 * monsters'; in their leaf with its light, the pickups turning (EF_ROTATE).
 * Strings are id's (the 1.06 QC's, reference/Quake/qw-qc).
 */

#include <stdio.h>
#include <string.h>

#include "dlight.h"
#include "ents.h"
#include "game.h"
#include "hud.h"
#include "level.h"
#include "render.h"
#include "rlist.h"
#include "snd.h"
#include "stream.h"
#include "vid.h"
#include "world.h"

#define DEG(d) ((d) * ANG_TURN / 360)
#define H_ROTTEN   1
#define H_MEGA     2
#define WEAPON_BIG2 1
#define IT_SUPERHEALTH 65536

#define SPAWNFLAG_SUPERSPIKE 1
#define SPAWNFLAG_LASER      2

static const int32_t box_ammo[2][3]  = { { 0, 0, 0 }, { 32 * CLIP_SUB, 32 * CLIP_SUB, 56 * CLIP_SUB } };
static const int32_t box_weap[2][3]  = { { -16 * CLIP_SUB, -16 * CLIP_SUB, 0 }, { 16 * CLIP_SUB, 16 * CLIP_SUB, 56 * CLIP_SUB } };
static const int32_t box_small[2][3] = { { -16 * CLIP_SUB, -16 * CLIP_SUB, -24 * CLIP_SUB },
                                         { 16 * CLIP_SUB, 16 * CLIP_SUB, 32 * CLIP_SUB } };

/* the alias models' streamed assets (tools/qcook/stream.c's names) */
static const char *const qim_stream[QIM_COUNT] = {
	[QIM_G_SHOT] = "progs/g_shot.mdl", [QIM_G_NAIL] = "progs/g_nail.mdl", [QIM_G_NAIL2] = "progs/g_nail2.mdl",
	[QIM_G_ROCK] = "progs/g_rock.mdl", [QIM_G_ROCK2] = "progs/g_rock2.mdl", [QIM_G_LIGHT] = "progs/g_light.mdl",
	[QIM_ARMOR1] = "progs/armor.mdl", [QIM_ARMOR2] = "progs/armor.mdl#1", [QIM_ARMOR3] = "progs/armor.mdl#2",
	[QIM_W_S_KEY] = "progs/w_s_key.mdl", [QIM_W_G_KEY] = "progs/w_g_key.mdl",
	[QIM_M_S_KEY] = "progs/m_s_key.mdl", [QIM_M_G_KEY] = "progs/m_g_key.mdl",
	[QIM_B_S_KEY] = "progs/b_s_key.mdl", [QIM_B_G_KEY] = "progs/b_g_key.mdl",
	[QIM_INVULNER] = "progs/invulner.mdl", [QIM_SUIT] = "progs/suit.mdl", [QIM_INVISIBL] = "progs/invisibl.mdl",
	[QIM_QUADDAMA] = "progs/quaddama.mdl",
	[QIM_END1] = "progs/end1.mdl", [QIM_END2] = "progs/end2.mdl", [QIM_END3] = "progs/end3.mdl",
	[QIM_END4] = "progs/end4.mdl",
	[QIM_FLAME] = "progs/flame.mdl", [QIM_FLAME2] = "progs/flame2.mdl", [QIM_LAVABALL] = "progs/lavaball.mdl",
	[QIM_TELEPORT] = "progs/teleport.mdl",
};

/* setmodel: drawn (ents_model_list or ents_streamed_list, at the lists'
   next rebuild), in the leaf of its box's centre (ent_link): the map's
   own model, or the streamed one */
void ent_set_qim(edict_t *e, int qim)
{
	e->item_model = world.model_of[qim];
	e->masset     = -1;
	if (e->item_model < 0 && qim_stream[qim])
		e->masset = (int16_t) stream_find(qim_stream[qim]);
	ent_link(e);
}

#define set_model ent_set_qim

/* setmodel (self, string_null): not drawn any more */
static void clear_model(edict_t *e)
{
	e->item_model = -1;
	e->masset     = -1;
}

/* droptofloor: down to 256 units with the entity's box, hull 1's point
   where the box's would be (SV_HullForEntity: offset = the hull's mins -
   the box's, the trace from origin - offset); 0 if it fell out of the
   level */
static int drop_to_floor(edict_t *e)
{
	static const int32_t hmin[3] = { -16 * CLIP_SUB, -16 * CLIP_SUB, -24 * CLIP_SUB };
	int32_t off[3], start[3], end[3];
	for (int k = 0; k < 3; k++) {
		off[k]   = hmin[k] - e->mins[k];
		start[k] = e->origin[k] - off[k];
		end[k]   = start[k];
	}
	end[2] -= 256 * CLIP_SUB;
	int solid = e->solid;                         /* SV_Move's passedict: not its own box */
	e->solid = SOLID_NOT;
	ent_link(e);
	trace_t tr;
	clip_trace(1, start, end, &tr);
	e->solid = (uint8_t) solid;
	ent_link(e);
	if (tr.fraction >= Q_FIX_ONE || tr.allsolid)
		return 0;
	int32_t org[3];
	for (int k = 0; k < 3; k++)
		org[k] = tr.endpos[k] + off[k];
	ent_setorigin(e, org);
	e->flags |= 512;                              /* FL_ONGROUND */
	e->groundentity = ent_from_trace(&tr) ? ent_from_trace(&tr) : ent_world;   /* (a lift carries it) */
	return 1;
}

/* PlaceItem: on the floor, touchable */
static void place_item(edict_t *self)
{
	self->flags   |= FL_ITEM;                     /* make extra wide */
	self->solid    = SOLID_TRIGGER;
	self->movetype = MOVETYPE_TOSS;
	memset(self->velocity, 0, sizeof self->velocity);
	self->origin[2] += 6 * CLIP_SUB;
	if (!drop_to_floor(self)) {
		printf("ents: bonus item fell out of level at %d %d %d\n", (int) self->origin[0] / CLIP_SUB,
		       (int) self->origin[1] / CLIP_SUB, (int) self->origin[2] / CLIP_SUB);
		ent_remove(self);
	}
}

/* StartItem: placed 0.2 s in, when the plats and doors have been spawned */
static void start_item(edict_t *self)
{
	self->nextthink = ent_time + TICKS(20);
	self->think     = place_item;
	ent_wake(self);
}

static void bonus(void)                          /* stuffcmd (other, "bf\n") */
{
	game_bonus_flash();
}

/* the item is gone (single player: no respawn) */
static void taken(edict_t *self)
{
	self->solid = SOLID_NOT;
	clear_model(self);
	ent_link(self);
	ent_activator = ent_other;
	sub_use_targets(self);
}

/* ---------------------------------------------------------------- health -- */

/* T_Heal: a living player, up to max_health (or past it when `ignore`),
   250 at most */
static int t_heal(int amount, int ignore)
{
	if (pl.health <= 0)
		return 0;
	if (!ignore && pl.health >= 100)
		return 0;
	pl.health += amount;
	if (!ignore && pl.health >= 100)
		pl.health = 100;
	if (pl.health > 250)
		pl.health = 250;
	return 1;
}

static void item_megahealth_rot(edict_t *self)
{
	if (pl.health > 100) {
		pl.health--;
		self->nextthink = ent_time + 60;
		ent_wake(self);
		return;
	}
	pl.items &= ~IT_SUPERHEALTH;                  /* (then removed in single player) */
	ent_remove(self);
}

static void health_touch(edict_t *self)
{
	if (ent_other != ent_player)
		return;
	if (self->item_extra == 2) {                  /* megahealth: past the max */
		if (pl.health >= 250)
			return;
		if (!t_heal(self->aflag, 1))
			return;
	} else if (!t_heal(self->aflag, 0)) {
		return;
	}
	char s[32];
	snprintf(s, sizeof s, "You receive %d health", (int) self->aflag);
	hud_notify(s);
	ent_sound(ent_player, CHAN_ITEM, self->noise, 255, ATTN_NORM);
	bonus();
	self->solid = SOLID_NOT;
	clear_model(self);
	ent_link(self);
	if (self->item_extra == 2) {                  /* rot down the player's super health */
		pl.items |= IT_SUPERHEALTH;
		self->nextthink = ent_time + 5 * 60;
		self->think     = item_megahealth_rot;
		ent_wake(self);
	}
	ent_activator = ent_other;
	sub_use_targets(self);
}

static void item_health(edict_t *self)
{
	self->touch = health_touch;
	if (self->spawnflags & H_ROTTEN) {
		set_model(self, QIM_B_BH10);
		self->noise      = (int16_t) snd_find("items/r_item1.wav");
		self->aflag      = 15;
		self->item_extra = 0;
	} else if (self->spawnflags & H_MEGA) {
		set_model(self, QIM_B_BH100);
		self->noise      = (int16_t) snd_find("items/r_item2.wav");
		self->aflag      = 100;
		self->item_extra = 2;
	} else {
		set_model(self, QIM_B_BH25);
		self->noise      = (int16_t) snd_find("items/health1.wav");
		self->aflag      = 25;
		self->item_extra = 1;
	}
	ent_setsize(self, box_ammo[0], box_ammo[1]);
	start_item(self);
}

/* ----------------------------------------------------------------- armor -- */

static void armor_touch(edict_t *self)
{
	if (ent_other != ent_player || pl.health <= 0)
		return;
	int type, value;
	uint32_t bit;
	if (self->class == QE_ITEM_ARMOR1)      { type = 30; value = 100; bit = IT_ARMOR1; }
	else if (self->class == QE_ITEM_ARMOR2) { type = 60; value = 150; bit = IT_ARMOR2; }
	else                                    { type = 80; value = 200; bit = IT_ARMOR3; }
	if (pl.armortype * pl.armor >= type * value)
		return;
	pl.armortype = type;
	pl.armor     = value;
	pl.items     = (pl.items & ~(IT_ARMOR1 | IT_ARMOR2 | IT_ARMOR3)) | bit;
	self->solid  = SOLID_NOT;
	clear_model(self);
	ent_link(self);
	hud_notify("You got armor");
	snd_start(ENT_PLAYER, CHAN_ITEM, S_ARMOR1, pl.pm.pos, 255, ATTN_NORM);
	bonus();
	ent_activator = ent_other;
	sub_use_targets(self);
}

static void item_armor(edict_t *self, int qim)
{
	self->touch = armor_touch;
	set_model(self, qim);
	ent_setsize(self, box_weap[0], box_weap[1]);
	start_item(self);
}

/* ---------------------------------------------------------------- weapons -- */

static void bound_ammo(void)                    /* bound_other_ammo */
{
	if (pl.ammo[AMMO_SHELLS] > 100)  pl.ammo[AMMO_SHELLS] = 100;
	if (pl.ammo[AMMO_NAILS] > 200)   pl.ammo[AMMO_NAILS] = 200;
	if (pl.ammo[AMMO_ROCKETS] > 100) pl.ammo[AMMO_ROCKETS] = 100;
	if (pl.ammo[AMMO_CELLS] > 100)   pl.ammo[AMMO_CELLS] = 100;
}

static void weapon_touch(edict_t *self)
{
	if (ent_other != ent_player)
		return;
	int w = 0;
	const char *name = "";
	switch (self->class) {
	case QE_WEAPON_NAILGUN:         w = W_NAILGUN;   pl.ammo[AMMO_NAILS] += 30;  name = "nailgun"; break;
	case QE_WEAPON_SUPERNAILGUN:    w = W_SNAILGUN;  pl.ammo[AMMO_NAILS] += 30;  name = "Super Nailgun"; break;
	case QE_WEAPON_SUPERSHOTGUN:    w = W_SSHOTGUN;  pl.ammo[AMMO_SHELLS] += 5;  name = "Super Shotgun"; break;
	case QE_WEAPON_ROCKETLAUNCHER:  w = W_RLAUNCHER; pl.ammo[AMMO_ROCKETS] += 5; name = "Rocket Launcher"; break;
	case QE_WEAPON_GRENADELAUNCHER: w = W_GLAUNCHER; pl.ammo[AMMO_ROCKETS] += 5; name = "Grenade Launcher"; break;
	case QE_WEAPON_LIGHTNING:       w = W_LIGHTNING; pl.ammo[AMMO_CELLS] += 15;  name = "Thunderbolt"; break;
	default: return;
	}
	char s[48];
	snprintf(s, sizeof s, "You got the %s", name);
	hud_notify(s);
	snd_start(ENT_PLAYER, CHAN_ITEM, S_PKUP, pl.pm.pos, 255, ATTN_NORM);
	bonus();
	bound_ammo();
	game_give_weapon(w);                          /* (single player: to the new one) */
	taken(self);
}

static void weapon_item(edict_t *self, int qim)
{
	self->touch = weapon_touch;
	set_model(self, qim);
	ent_setsize(self, box_weap[0], box_weap[1]);
	start_item(self);
}

/* ------------------------------------------------------------------- ammo -- */

static void ammo_touch(edict_t *self)
{
	if (ent_other != ent_player || pl.health <= 0)
		return;
	int best = game_best_weapon();                /* the player was using it: change up */
	int a = (int) self->item_extra;
	static const int max[AMMO_COUNT] = { 100, 200, 100, 100 };
	if (pl.ammo[a] >= max[a])
		return;
	pl.ammo[a] += self->aflag;
	bound_ammo();
	static const char *const name[AMMO_COUNT] = { "shells", "nails", "rockets", "cells" };
	char s[32];
	snprintf(s, sizeof s, "You got the %s", name[a]);
	hud_notify(s);
	snd_start(ENT_PLAYER, CHAN_ITEM, S_LOCK4, pl.pm.pos, 255, ATTN_NORM);
	bonus();
	if (pl.weapon == best)                        /* change to a better weapon if appropriate */
		game_select_best();
	taken(self);
}

static void ammo_item(edict_t *self, int ammo, int small_qim, int big_qim, int small_n, int big_n)
{
	self->touch = ammo_touch;
	int big = self->spawnflags & WEAPON_BIG2;
	set_model(self, big ? big_qim : small_qim);
	self->aflag      = big ? big_n : small_n;
	self->item_extra = ammo;
	ent_setsize(self, box_ammo[0], box_ammo[1]);
	start_item(self);
}

/* ------------------------------------------------------------------- keys -- */

static void key_touch(edict_t *self)
{
	if (ent_other != ent_player || pl.health <= 0)
		return;
	if (pl.items & self->items)
		return;
	char s[48];
	snprintf(s, sizeof s, "You got the %s", self->netname);
	hud_notify(s);
	ent_sound(ent_player, CHAN_ITEM, self->noise, 255, ATTN_NORM);
	bonus();
	pl.items |= self->items;
	taken(self);
}

static void item_key(edict_t *self, int gold)
{
	static const char *const names[2][3] = {
		{ "silver key", "silver runekey", "silver keycard" },
		{ "gold key", "gold runekey", "gold keycard" },
	};
	static const char *const noise[3] = { "misc/medkey.wav", "misc/runekey.wav", "misc/basekey.wav" };
	static const int qims[2][3] = { { QIM_W_S_KEY, QIM_M_S_KEY, QIM_B_S_KEY },
	                                { QIM_W_G_KEY, QIM_M_G_KEY, QIM_B_G_KEY } };
	int wt = world.worldtype <= 2 ? (int) world.worldtype : 0;
	self->netname = names[gold][wt];
	self->noise   = (int16_t) snd_find(noise[wt]);
	set_model(self, qims[gold][wt]);
	self->items = gold ? IT_KEY2 : IT_KEY1;
	self->touch = key_touch;
	ent_setsize(self, box_small[0], box_small[1]);
	start_item(self);
}

/* ------------------------------------------------------------------- rune -- */

static void sigil_touch(edict_t *self)
{
	if (ent_other != ent_player || pl.health <= 0)
		return;
	ent_centerprint(ent_other, "You got the rune!");
	ent_sound(ent_player, CHAN_ITEM, self->noise, 255, ATTN_NORM);
	bonus();
	self->solid = SOLID_NOT;
	clear_model(self);
	ent_link(self);
	game_serverflags |= self->spawnflags & 15;
	self->class = QE_TEMP;                        /* so rune doors won't find it */
	ent_activator = ent_other;
	sub_use_targets(self);
}

static void item_sigil(edict_t *self)
{
	self->noise = (int16_t) snd_find("misc/runekey.wav");
	int sf = self->spawnflags;
	set_model(self, sf & 2 ? QIM_END2 : sf & 4 ? QIM_END3 : sf & 8 ? QIM_END4 : QIM_END1);
	self->touch = sigil_touch;
	ent_setsize(self, box_small[0], box_small[1]);
	start_item(self);
}

/* --------------------------------------------------------------- powerups -- */

static void powerup_touch(edict_t *self)
{
	if (ent_other != ent_player || pl.health <= 0)
		return;
	char s[48];
	snprintf(s, sizeof s, "You got the %s", self->netname);
	hud_notify(s);
	ent_sound(ent_player, CHAN_VOICE, self->noise, 255, ATTN_NORM);
	bonus();
	self->solid = SOLID_NOT;
	pl.items |= self->items;
	clear_model(self);
	ent_link(self);
	game_powerup(self->items);                    /* the 30 s */
	ent_activator = ent_other;
	sub_use_targets(self);
}

static void powerup_item(edict_t *self, int qim, uint32_t it, const char *name, const char *noise)
{
	self->touch   = powerup_touch;
	self->items   = it;
	self->netname = name;
	self->noise   = (int16_t) snd_find(noise);
	set_model(self, qim);
	ent_setsize(self, box_small[0], box_small[1]);
	start_item(self);
}

/* ---------------------------------------------------- flames, fireballs -- */

static void fire_touch(edict_t *self)
{
	ent_damage(ent_other, self, self, 20);
	ent_remove(self);
}

static void fire_fly(edict_t *self)
{
	edict_t *f = ent_spawn();
	if (f) {
		f->solid    = SOLID_TRIGGER;
		f->movetype = MOVETYPE_TOSS;
		f->velocity[0] = (int32_t) ((q_rand() & 0x7fff) * 100 / 0x8000) - 50;
		f->velocity[1] = (int32_t) ((q_rand() & 0x7fff) * 100 / 0x8000) - 50;
		f->velocity[2] = self->speed + (int32_t) ((q_rand() & 0x7fff) * 200 / 0x8000);
		f->class = QE_TEMP;
		set_model(f, QIM_LAVABALL);
		ent_setorigin(f, self->origin);
		f->nextthink = ent_time + 5 * 60;
		f->think     = sub_remove;
		f->touch     = fire_touch;
		ent_wake(f);
	}
	self->nextthink = ent_time + (int32_t) ((q_rand() & 0x7fff) * 5 * 60 / 0x8000) + 3 * 60;
	self->think     = fire_fly;
	ent_wake(self);
}

static void barrel_explode(edict_t *self)
{
	self->takedamage = DAMAGE_NO;
	ents_radius_damage(self->origin, 160, self, 0);
	game_radius_damage_player(self->origin, 160);
	ent_sound(self, CHAN_VOICE, snd_find("weapons/r_exp3.wav"), 255, ATTN_NORM);
	int32_t org[3] = { self->origin[0], self->origin[1], self->origin[2] + 32 * CLIP_SUB };
	game_explosion(org);                          /* BecomeExplosion */
	ent_remove(self);
}

static void spikeshooter_use(edict_t *self)
{
	if (self->spawnflags & SPAWNFLAG_LASER)       /* (the laser: a spike drawn, the enforcer's sound) */
		ent_sound(self, CHAN_VOICE, snd_find("enforcer/enfire.wav"), 255, ATTN_NORM);
	else
		ent_sound(self, CHAN_VOICE, snd_find("weapons/spike2.wav"), 255, ATTN_NORM);
	game_launch_spike(self, self->origin, self->movedir, 500, self->spawnflags & SPAWNFLAG_SUPERSPIKE);
}

static void shooter_think(edict_t *self)
{
	spikeshooter_use(self);
	self->nextthink = ent_time + TICKS(self->wait);
	ent_wake(self);
}

/* SV_Physics_Toss's move for what the QC spawns with MOVETYPE_TOSS (the
   fireballs, SOLID_TRIGGER points): gravity (800 a second: 13, 13, 14 a
   tick), the push (SV_PushEntity: a trigger clips against the brush models
   only, MOVE_NOMONSTERS), SV_Impact on what it hits. The player meets it
   through its own touch pass (SV_LinkEdict's SV_TouchLinks). */
static void toss_tick(edict_t *e)
{
	if (e->flags & 512)                           /* FL_ONGROUND: no move */
		return;
	for (int t = ent_time - game_dt + 1; t <= ent_time; t++)   /* each 60th of the tick */
		e->velocity[2] -= t % 3 == 2 ? 14 : 13;
	int32_t end[3];
	for (int k = 0; k < 3; k++)
		end[k] = e->origin[k] + e->velocity[k] * CLIP_SUB * game_dt / 60;
	trace_t tr;
	clip_trace(0, e->origin, end, &tr);
	ent_setorigin(e, tr.endpos);
	if (tr.fraction >= Q_FIX_ONE)
		return;
	ent_other = tr.ent ? ent_from_trace(&tr) : ent_world;
	if (!ent_other)
		ent_other = ent_world;
	if (e->touch)
		e->touch(e);                              /* (the fireball's: removed) */
	if (!e->inuse)
		return;
	if (tr.plane_n[2] > 4096 * 7 / 10) {          /* on the ground: stops */
		e->flags |= 512;
		e->groundentity = ent_other;
		e->velocity[0] = e->velocity[1] = e->velocity[2] = 0;
	} else {                                      /* ClipVelocity, backoff 1 */
		int32_t back = (e->velocity[0] * tr.plane_n[0] + e->velocity[1] * tr.plane_n[1] +
		                e->velocity[2] * tr.plane_n[2]) >> 12;
		for (int k = 0; k < 3; k++)
			e->velocity[k] -= tr.plane_n[k] * back >> 12;
	}
}

/* ----------------------------------------------------------------- spawn -- */

void items_spawn(edict_t *e, const qw_ent *q)
{
	(void) q;
	switch (e->class) {
	case QE_ITEM_HEALTH:  item_health(e); break;
	case QE_ITEM_ARMOR1:  item_armor(e, QIM_ARMOR1); break;
	case QE_ITEM_ARMOR2:  item_armor(e, QIM_ARMOR2); break;
	case QE_ITEM_ARMORINV: item_armor(e, QIM_ARMOR3); break;
	case QE_ITEM_SHELLS:  ammo_item(e, AMMO_SHELLS, QIM_B_SHELL0, QIM_B_SHELL1, 20, 40); break;
	case QE_ITEM_SPIKES:  ammo_item(e, AMMO_NAILS, QIM_B_NAIL0, QIM_B_NAIL1, 25, 50); break;
	case QE_ITEM_ROCKETS: ammo_item(e, AMMO_ROCKETS, QIM_B_ROCK0, QIM_B_ROCK1, 5, 10); break;
	case QE_ITEM_CELLS:   ammo_item(e, AMMO_CELLS, QIM_B_BATT0, QIM_B_BATT1, 6, 12); break;
	case QE_ITEM_WEAPON:                          /* the old ammo item */
		if (e->spawnflags & 1)      ammo_item(e, AMMO_SHELLS, QIM_B_SHELL0, QIM_B_SHELL1, 20, 40);
		else if (e->spawnflags & 4) ammo_item(e, AMMO_NAILS, QIM_B_NAIL0, QIM_B_NAIL1, 25, 50);
		else if (e->spawnflags & 2) ammo_item(e, AMMO_ROCKETS, QIM_B_ROCK0, QIM_B_ROCK1, 5, 10);
		break;
	case QE_ITEM_KEY1:    item_key(e, 0); break;
	case QE_ITEM_KEY2:    item_key(e, 1); break;
	case QE_ITEM_SIGIL:   item_sigil(e); break;
	case QE_ITEM_ARTIFACT_INVULNERABILITY:
		powerup_item(e, QIM_INVULNER, IT_INVULNERABILITY, "Pentagram of Protection", "items/protect.wav");
		break;
	case QE_ITEM_ARTIFACT_ENVIROSUIT:
		powerup_item(e, QIM_SUIT, IT_SUIT, "Biosuit", "items/suit.wav");
		break;
	case QE_ITEM_ARTIFACT_INVISIBILITY:
		powerup_item(e, QIM_INVISIBL, IT_INVISIBILITY, "Ring of Shadows", "items/inv1.wav");
		break;
	case QE_ITEM_ARTIFACT_SUPER_DAMAGE:
		powerup_item(e, QIM_QUADDAMA, IT_QUAD, "Quad Damage", "items/damage.wav");
		break;
	case QE_WEAPON_SUPERSHOTGUN:    weapon_item(e, QIM_G_SHOT); break;
	case QE_WEAPON_NAILGUN:         weapon_item(e, QIM_G_NAIL); break;
	case QE_WEAPON_SUPERNAILGUN:    weapon_item(e, QIM_G_NAIL2); break;
	case QE_WEAPON_GRENADELAUNCHER: weapon_item(e, QIM_G_ROCK); break;
	case QE_WEAPON_ROCKETLAUNCHER:  weapon_item(e, QIM_G_ROCK2); break;
	case QE_WEAPON_LIGHTNING:       weapon_item(e, QIM_G_LIGHT); break;
	case QE_LIGHT_TORCH_SMALL_WALLTORCH:          /* the static flames: makestatic */
		set_model(e, QIM_FLAME);
		e->frame = 0; e->cnt = 6;                 /* frames 0 .. 5, 10 a second */
		break;
	case QE_LIGHT_FLAME_LARGE_YELLOW:
		set_model(e, QIM_FLAME2);
		e->frame = 6; e->cnt = 11;                /* flame2's second group */
		break;
	case QE_LIGHT_FLAME_SMALL_YELLOW:
	case QE_LIGHT_FLAME_SMALL_WHITE:
		set_model(e, QIM_FLAME2);
		e->frame = 0; e->cnt = 6;
		break;
	case QE_MISC_FIREBALL:
		e->nextthink = ent_time + (int32_t) ((q_rand() & 0x7fff) * 5 * 60 / 0x8000);
		e->think     = fire_fly;
		ent_wake(e);
		/* (if (!self.speed) self.speed == 1000;: the QC's ==, the speed stays 0) */
		break;
	case QE_MISC_EXPLOBOX:
	case QE_MISC_EXPLOBOX2:
		e->solid    = SOLID_BBOX;
		e->movetype = MOVETYPE_NONE;
		set_model(e, e->class == QE_MISC_EXPLOBOX ? QIM_B_EXPLOB : QIM_B_EXBOX2);
		{
			int32_t mx[3] = { 32 * CLIP_SUB, 32 * CLIP_SUB, (e->class == QE_MISC_EXPLOBOX ? 64 : 32) * CLIP_SUB };
			static const int32_t mn[3] = { 0, 0, 0 };
			ent_setsize(e, mn, mx);
		}
		e->health     = 20;
		e->th_die     = barrel_explode;
		e->takedamage = DAMAGE_AIM;
		e->origin[2] += 2 * CLIP_SUB;
		{
			int32_t oldz = e->origin[2];
			if (!drop_to_floor(e) || oldz - e->origin[2] > 250 * CLIP_SUB) {
				printf("ents: item fell out of level\n");
				ent_remove(e);
			}
		}
		break;
	case QE_TRAP_SPIKESHOOTER:
	case QE_TRAP_SHOOTER:
		set_movedir(e);
		e->use = spikeshooter_use;
		if (e->class == QE_TRAP_SHOOTER) {
			if (!e->wait)
				e->wait = 100;
			e->nextthink = e->nextthink + TICKS(e->wait) + ent_time;
			e->think     = shooter_think;
			ent_wake(e);
		}
		break;
	default:
		e->solid    = SOLID_NOT;
		e->movetype = MOVETYPE_NONE;
		break;
	}
}

/* the tick of the edicts with a toss move (ents.c) */
void items_physics(edict_t *e)
{
	if (e->movetype == MOVETYPE_TOSS)
		toss_tick(e);
}

/* R_AliasCheckBBox's trivial reject, on the model's sphere: its origin in
   the view's space (world x 6: the walk's matrix), wholly behind the eye or
   outside a side of the 90 degree frustum (|x| <= z; |y| <= z x the
   viewport's rows / 320); the distances to the slanted planes taken
   conservatively (x sqrt 2 as 3 / 2, the vertical's 5 / 4) */
int ent_in_view(const int32_t org[3], int radius_vu)
{
	int32_t v[3] = { org[0] * QW_VSCALE / CLIP_SUB, org[1] * QW_VSCALE / CLIP_SUB, org[2] * QW_VSCALE / CLIP_SUB };
	const int16_t *r = r_state.rot;
	int32_t z = ((r[6] * v[0] + r[7] * v[1] + r[8] * v[2]) >> 12) + r_state.tr[2];
	int32_t rad = radius_vu * 3 / 2;                  /* vertex units -> the view's */
	if (z + rad <= 0)
		return 0;
	int32_t x = ((r[0] * v[0] + r[1] * v[1] + r[2] * v[2]) >> 12) + r_state.tr[0];
	if (x - z > rad * 3 / 2 || -x - z > rad * 3 / 2)
		return 0;
	int32_t y = ((r[3] * v[0] + r[4] * v[1] + r[5] * v[2]) >> 12) + r_state.tr[1];
	int32_t zy = z * (int32_t) r_state.vh2 / VID_W;   /* half the rows over 160 */
	if (y - zy > rad * 5 / 4 || -y - zy > rad * 5 / 4)
		return 0;
	return 1;
}

/* the items' models into the render list: in their leaf if the PVS sees
   it and the frustum may, lit by it (and the dynamic lights), the pickups
   turning */
void items_render(void)
{
	uint16_t vis = (uint16_t) r_state.visframe;
	int yaw = (game_tick_count % 216) * ANG_TURN / 216;   /* EF_ROTATE: 100 degrees a second (3.6 s a turn) */
	for (int i = 0; i < ents_num_models; i++) {
		edict_t *e = &edicts[ents_model_list[i]];
		if (!e->inuse || e->item_model < 0)
			continue;
		const qw_leaf *leaf = ent_model_leaf(e);
		if (!leaf || leaf->visframe != vis)
			continue;
		const qg_model *m = &world.models[e->item_model];
		if (!ent_in_view(e->origin, m->radius))
			continue;
		int frame = 0, flame = e->cnt && e->class != QE_MISC_TELEPORTTRAIN;   /* (a train's cnt: its own) */
		if (flame)                                /* an animated group, 10 a second, its phase */
			frame = e->frame + (int) ((game_tick_count / 6 + (e - edicts)) % e->cnt);
		int light = (m->flags & QGF_FLAT) && flame ? 128
		          : leaf->light + dl_add_at(e->origin, game_tick_count) * 3 / 2;
		rl_add_model(leaf, rl_model(m, frame, e->origin, (m->flags & QGF_ROTATE) ? yaw : 0, 0, 0, light), e->origin,
		             e->mins);
	}
}
