/*
 * q1psx — the monsters (engine/monsters.h): their kinds, their states'
 * runner, monsters.qc (the starts, monster_use, monster_death_use),
 * player.qc's ThrowGib / ThrowHead, items.qc's DropBackpack, their
 * streamed assets' wants, their drawing.
 */

#include <stdio.h>
#include <string.h>

#include "dlight.h"
#include "game.h"
#include "hud.h"
#include "monsters.h"
#include "render.h"
#include "rlist.h"
#include "snd.h"
#include "stream.h"
#include "world.h"

#define SUB   CLIP_SUB
#define NEVER 0x3fffffff         /* pausetime 99999999: a time never reached */

/* the kinds (engine/m_*.c) */
extern const mdef_t mdef_army, mdef_dog, mdef_ogre, mdef_ogre_marksman, mdef_knight, mdef_zombie, mdef_wizard,
                    mdef_demon, mdef_shambler, mdef_boss;

static const mdef_t *const KINDS[MK_COUNT] = {
	[MK_ARMY]     = &mdef_army,
	[MK_DOG]      = &mdef_dog,
	[MK_OGRE]     = &mdef_ogre,
	[MK_KNIGHT]   = &mdef_knight,
	[MK_ZOMBIE]   = &mdef_zombie,
	[MK_WIZARD]   = &mdef_wizard,
	[MK_DEMON]    = &mdef_demon,
	[MK_SHAMBLER] = &mdef_shambler,
	[MK_BOSS]     = &mdef_boss,
};

static int16_t kind_asset[MK_COUNT][MK_ASSETS];   /* the stream's indices, -1 none */
static int16_t gib_asset[4];                      /* gib1..3, zom_gib              */
static int16_t backpack_asset;


void monsters_map_start(void)
{
	static const char *const gibs[4] = { "progs/gib1.mdl", "progs/gib2.mdl", "progs/gib3.mdl",
	                                     "progs/zom_gib.mdl" };
	for (int k = 0; k < MK_COUNT; k++) {
		for (int i = 0; i < MK_ASSETS; i++) {
			const char *n = KINDS[k]->assets[i];
			kind_asset[k][i] = (int16_t) (n ? stream_find(n) : -1);
		}
		if (KINDS[k]->map_start)
			KINDS[k]->map_start();
		for (int s = 0; s < KINDS[k]->num_states; s++)   /* (a table's rows miscounted: caught here) */
			if (KINDS[k]->states[s].next >= KINDS[k]->num_states ||
			    (KINDS[k]->states[s].act == A_CUSTOM && !KINDS[k]->custom))
				printf("monsters: %s state %d is wrong\n", KINDS[k]->classname, s);
	}
	for (int i = 0; i < 4; i++)
		gib_asset[i] = (int16_t) stream_find(gibs[i]);
	backpack_asset = (int16_t) stream_find("progs/backpack.mdl");
	ai_map_start();
}

static int kind_of(int cls)
{
	switch (cls) {
	case QE_MONSTER_ARMY:     return MK_ARMY;
	case QE_MONSTER_DOG:      return MK_DOG;
	case QE_MONSTER_OGRE:
	case QE_MONSTER_OGRE_MARKSMAN: return MK_OGRE;
	case QE_MONSTER_KNIGHT:   return MK_KNIGHT;
	case QE_MONSTER_ZOMBIE:   return MK_ZOMBIE;
	case QE_MONSTER_WIZARD:   return MK_WIZARD;
	case QE_MONSTER_DEMON1:   return MK_DEMON;
	case QE_MONSTER_SHAMBLER: return MK_SHAMBLER;
	case QE_MONSTER_BOSS:     return MK_BOSS;
	default:                  return -1;
	}
}

/* ------------------------------------------------------------- states -- */

void monster_state(edict_t *self, int s)
{
	const mdef_t *m = self->mdef;
	if (!m || s >= m->num_states)
		return;
	const mstate_t *st = &m->states[s];
	self->frame     = st->frame;          /* the QC's `state`: frame, think in 0.1 s */
	self->nextthink = ent_time + TICKS(10);
	self->think     = monster_think;
	self->mstate    = st->next;
	ent_wake(self);
	switch (st->act) {
	case A_STAND:   ai_stand(self); break;
	case A_WALK:    ai_walk(self, st->arg); break;
	case A_RUN:     ai_run(self, st->arg); break;
	case A_FACE:    ai_face(self); break;
	case A_CHARGE:  ai_charge(self, st->arg); break;
	case A_PAIN:    ai_pain(self, st->arg); break;
	case A_PAINFWD: ai_painforward(self, st->arg); break;
	case A_FORWARD: ai_forward(self, st->arg); break;
	case A_BACK:    ai_back(self, st->arg); break;
	case A_TURN:    ai_turn(self); break;
	case A_MELEE:   ai_melee(self); break;
	case A_CHARGESIDE: ai_charge_side(self); break;
	case A_MELEESIDE:  ai_melee_side(self); break;
	case A_CUSTOM:  m->custom(self, st->arg); break;
	default:        break;
	}
}

void monster_think(edict_t *self)
{
	monster_state(self, self->mstate);
}

void monster_think_at(edict_t *self, int s, int ticks)
{
	self->mstate    = (uint16_t) s;
	self->think     = monster_think;
	self->nextthink = ent_time + ticks;
	ent_wake(self);
}

/* -------------------------------------------------------- monsters.qc -- */

/* monster_use: using a monster makes it angry at the current activator */
void monster_use(edict_t *self)
{
	if (self->enemy)
		return;
	if (self->health <= 0)
		return;
	if (ent_activator->items & IT_INVISIBILITY)
		return;
	if (ent_activator->flags & FL_NOTARGET)
		return;
	if (ent_activator != ent_player)
		return;
	/* delay reaction so if the monster is teleported, its sound is still heard */
	self->enemy     = ent_activator;
	self->nextthink = ent_time + TICKS(10);
	self->think     = ai_found_target;
	ent_wake(self);
}

/* monster_death_use: a dying monster fires its targets, its enemy the
   activator */
void monster_death_use(edict_t *self)
{
	if (self->flags & FL_FLY)             /* fall to ground */
		self->flags &= ~FL_FLY;
	if (self->flags & FL_SWIM)
		self->flags &= ~FL_SWIM;
	if (!self->target)
		return;
	ent_activator = self->enemy;
	sub_use_targets(self);
}

/* walkmonster_start_go: on the floor, its path or standing */
static void walkmonster_start_go(edict_t *self)
{
	const mdef_t *m = self->mdef;
	self->origin[2] += SUB;               /* raise off floor a bit */
	ai_droptofloor(self);
	if (!ai_walkmove(self, 0, 0))
		printf("walkmonster in wall at: %d %d %d\n", (int) self->origin[0] / SUB, (int) self->origin[1] / SUB,
		       (int) self->origin[2] / SUB);
	self->takedamage = DAMAGE_AIM;
	self->ideal_yaw  = (int16_t) ((int32_t) self->ang[1] * 360 / 65536);   /* angles * '0 1 0' */
	if (!self->yaw_speed)
		self->yaw_speed = 20;
	self->view_ofs_z = 25 * SUB;
	self->use = monster_use;
	self->flags |= FL_MONSTER;
	ent_link(self);                       /* (its clip entry marked a monster's: clip.h) */
	if (self->target) {
		self->goalentity = self->movetarget = ent_find(0, self->target);
		if (self->goalentity) {
			int32_t d[3];
			for (int k = 0; k < 3; k++)
				d[k] = self->goalentity->origin[k] - self->origin[k];
			self->ideal_yaw = (int16_t) vectoyaw(d);
		}
		if (!self->movetarget)
			printf("Monster can't find target at %d %d %d\n", (int) self->origin[0] / SUB,
			       (int) self->origin[1] / SUB, (int) self->origin[2] / SUB);
		/* (1.06: th_walk for a path_corner, then th_stand whatever: the
		   else's braces missing) */
		if (self->movetarget && self->movetarget->class == QE_PATH_CORNER)
			m->th_walk(self);
		else
			self->pausetime = NEVER;
		m->th_stand(self);
	} else {
		self->pausetime = NEVER;
		m->th_stand(self);
	}
	/* spread think times so they don't all happen at same time */
	self->nextthink += (int32_t) (frand() * TICKS(50) >> 16);
}

static void flymonster_start_go(edict_t *self)
{
	const mdef_t *m = self->mdef;
	self->takedamage = DAMAGE_AIM;
	self->ideal_yaw  = (int16_t) ((int32_t) self->ang[1] * 360 / 65536);
	if (!self->yaw_speed)
		self->yaw_speed = 10;
	self->view_ofs_z = 25 * SUB;
	self->use = monster_use;
	self->flags |= FL_FLY;
	self->flags |= FL_MONSTER;
	ent_link(self);                       /* (its clip entry marked a monster's: clip.h) */
	if (!ai_walkmove(self, 0, 0))
		printf("flymonster in wall at: %d %d %d\n", (int) self->origin[0] / SUB, (int) self->origin[1] / SUB,
		       (int) self->origin[2] / SUB);
	if (self->target) {
		self->goalentity = self->movetarget = ent_find(0, self->target);
		if (!self->movetarget)
			printf("Monster can't find target at %d %d %d\n", (int) self->origin[0] / SUB,
			       (int) self->origin[1] / SUB, (int) self->origin[2] / SUB);
		if (self->movetarget && self->movetarget->class == QE_PATH_CORNER)
			m->th_walk(self);
		else
			self->pausetime = NEVER;
		m->th_stand(self);
	} else {
		self->pausetime = NEVER;
		m->th_stand(self);
	}
}

/* walkmonster_start / flymonster_start: delayed (the doors spawned first),
   the thinks spread, counted */
static void monster_start(edict_t *self)
{
	self->nextthink += (int32_t) (frand() * TICKS(50) >> 16);
	self->think = self->mdef->fly ? flymonster_start_go : walkmonster_start_go;
	ent_wake(self);
	ent_total_monsters++;
}

void monsters_spawn(edict_t *e, const qw_ent *q)
{
	(void) q;
	int k = kind_of(e->class);
	if (k < 0) {
		ent_remove(e);
		return;
	}
	e->ang[1] = (uint16_t) ((e->angle % 360 + 360) % 360 * 65536 / 360);
	const mdef_t *m = KINDS[k];
	int32_t mins[3], maxs[3];
	if (e->class == QE_MONSTER_OGRE_MARKSMAN)
		m = &mdef_ogre_marksman;
	/* the kind's spawn function's part every monster shares */
	e->mdef     = m;
	e->solid    = SOLID_SLIDEBOX;
	e->movetype = MOVETYPE_STEP;
	e->masset   = kind_asset[k][0];
	e->health   = e->max_health = m->health;
	for (int i = 0; i < 3; i++) {
		mins[i] = m->mins[i] * SUB;
		maxs[i] = m->maxs[i] * SUB;
	}
	ent_setsize(e, mins, maxs);
	e->th_pain = m->th_pain;
	e->th_die  = m->th_die;
	if (m->spawn && m->spawn(e))
		return;
	monster_start(e);
}

int monster_asset(const edict_t *self, int i)
{
	return self->mdef ? kind_asset[self->mdef->kind][i] : -1;
}

int monsters_kind_asset(int kind, int i)
{
	return kind_asset[kind][i];
}

/* ----------------------------------------------------- gibs, backpack -- */

/* VelocityForDamage: the gib's flight by the health it was hit down to */
static void velocity_for_damage(int dm, int32_t v[3])
{
	v[0] = 100 * crand() >> 16;
	v[1] = 100 * crand() >> 16;
	v[2] = 200 + (int32_t) (100 * frand() >> 16);
	int f = dm > -50 ? 7 : dm > -200 ? 20 : 100;   /* x0.7, x2, x10 (/10) */
	for (int k = 0; k < 3; k++)
		v[k] = v[k] * f / 10;
}

void throw_gib(edict_t *self, int gib, int dm)
{
	edict_t *n = ent_spawn();
	if (!n)
		return;
	__builtin_memcpy(n->origin, self->origin, sizeof n->origin);
	n->masset = gib_asset[gib];
	static const int32_t zero[3] = { 0, 0, 0 };
	ent_setsize(n, zero, zero);
	velocity_for_damage(dm, n->velocity);
	n->movetype = MOVETYPE_BOUNCE;
	n->solid    = SOLID_NOT;
	for (int k = 0; k < 3; k++)
		n->avel[k] = (int16_t) (600 * frand() >> 16);
	n->think     = sub_remove;
	n->ltime     = ent_time;
	n->nextthink = ent_time + TICKS(1000) + (int32_t) (frand() * TICKS(1000) >> 16);
	n->frame     = 0;
	n->flags     = 0;
	n->trail     = gib < 3 ? TR_BLOOD + 1 : TR_SLIGHT_BLOOD + 1;   /* EF_GIB, EF_ZOMGIB */
	ent_wake(n);
}

void throw_head(edict_t *self, int dm)
{
	int k = self->mdef ? self->mdef->kind : 0;
	self->masset     = kind_asset[k][1];   /* setmodel (self, gibname) */
	self->frame      = 0;
	self->nextthink  = -1;
	self->movetype   = MOVETYPE_BOUNCE;
	self->takedamage = DAMAGE_NO;
	self->solid      = SOLID_NOT;
	self->view_ofs_z = 8 * SUB;
	static const int32_t hmins[3] = { -16 * SUB, -16 * SUB, 0 }, hmaxs[3] = { 16 * SUB, 16 * SUB, 56 * SUB };
	ent_setsize(self, hmins, hmaxs);
	velocity_for_damage(dm, self->velocity);
	self->origin[2] -= 24 * SUB;
	self->flags &= ~FL_ONGROUND;
	self->avel[0] = self->avel[2] = 0;
	self->avel[1] = (int16_t) (600 * crand() >> 16);
	self->trail   = TR_BLOOD + 1;                /* the heads: EF_GIB */
	ent_link(self);
	ent_wake(self);
}

/* weapons.qc SpawnMeatSpray: a bit of flesh thrown from org (zom_gib.mdl,
   bouncing, gone in a second) */
void spawn_meat_spray(edict_t *self, const int32_t org[3], const int32_t vel[3])
{
	edict_t *n = ent_spawn();
	if (!n)
		return;
	n->owner    = self;
	n->movetype = MOVETYPE_BOUNCE;
	n->solid    = SOLID_NOT;
	for (int k = 0; k < 3; k++)
		n->velocity[k] = vel[k];
	n->velocity[2] += 250 + (int32_t) (50 * frand() >> 16);
	n->avel[0] = 3000;
	n->avel[1] = 1000;
	n->avel[2] = 2000;
	n->nextthink = ent_time + TICKS(100);
	n->think     = sub_remove;
	n->masset    = gib_asset[3];
	n->trail     = TR_SLIGHT_BLOOD + 1;          /* EF_ZOMGIB */
	static const int32_t zero[3] = { 0, 0, 0 };
	ent_setsize(n, zero, zero);
	ent_setorigin(n, org);
	ent_wake(n);
}

/* BackpackTouch (1.06): the ammo to the player, its weapon kept */
static void backpack_touch(edict_t *self)
{
	if (ent_other != ent_player || pl.health <= 0)
		return;
	char s[80];
	int n = snprintf(s, sizeof s, "You get ");
	int acount = 0;
	static const char *const names[AMMO_COUNT] = { " shells", " nails", " rockets", " cells" };
	const int amounts[AMMO_COUNT] = { self->ammo_shells, self->ammo_nails, self->ammo_rockets, self->ammo_cells };
	for (int a = 0; a < AMMO_COUNT; a++)
		pl.ammo[a] += amounts[a];
	if (pl.ammo[AMMO_SHELLS] > 100)  pl.ammo[AMMO_SHELLS] = 100;   /* bound_other_ammo */
	if (pl.ammo[AMMO_NAILS] > 200)   pl.ammo[AMMO_NAILS] = 200;
	if (pl.ammo[AMMO_ROCKETS] > 100) pl.ammo[AMMO_ROCKETS] = 100;
	if (pl.ammo[AMMO_CELLS] > 100)   pl.ammo[AMMO_CELLS] = 100;
	for (int a = 0; a < AMMO_COUNT; a++)
		if (amounts[a] && n < (int) sizeof s)
			n += snprintf(s + n, sizeof s - (size_t) n, "%s%d%s", acount++ ? ", " : "", amounts[a], names[a]);
	hud_notify(s);
	snd_start(ENT_PLAYER, CHAN_ITEM, S_LOCK4, pl.pm.pos, 255, ATTN_NORM);
	game_bonus_flash();
	ent_remove(self);
}

/* DropBackpack: the monster's ammo in a pack thrown up */
void drop_backpack(edict_t *self)
{
	if (!(self->ammo_shells + self->ammo_nails + self->ammo_rockets + self->ammo_cells))
		return;
	edict_t *item = ent_spawn();
	if (!item)
		return;
	__builtin_memcpy(item->origin, self->origin, sizeof item->origin);
	item->origin[2] -= 24 * SUB;
	item->ammo_shells  = self->ammo_shells;
	item->ammo_nails   = self->ammo_nails;
	item->ammo_rockets = self->ammo_rockets;
	item->ammo_cells   = self->ammo_cells;
	item->velocity[2] = 300;
	item->velocity[0] = -100 + (int32_t) (200 * frand() >> 16);
	item->velocity[1] = -100 + (int32_t) (200 * frand() >> 16);
	item->flags    = FL_ITEM;
	item->solid    = SOLID_TRIGGER;
	item->movetype = MOVETYPE_TOSS;
	item->masset   = backpack_asset;
	static const int32_t bmins[3] = { -16 * SUB, -16 * SUB, 0 }, bmaxs[3] = { 16 * SUB, 16 * SUB, 56 * SUB };
	ent_setsize(item, bmins, bmaxs);
	item->touch     = backpack_touch;
	item->nextthink = ent_time + TICKS(12000);   /* remove after 2 minutes */
	item->think     = sub_remove;
	ent_wake(item);
}

/* ------------------------------------------------------ assets, render -- */

static uint32_t cheb(const int32_t a[3], const int32_t b[3])
{
	uint32_t d = 0;
	for (int j = 0; j < 3; j++) {
		int32_t v = a[j] - b[j];
		uint32_t x = (uint32_t) (v < 0 ? -v : v);
		if (x > d) d = x;
	}
	return d / SUB;
}

/* the items the game must always have (the keys and the
   weapons first, whatever else is in): the keys, the runes, the weapons */
static int pinned(const edict_t *e)
{
	return e->class == QE_ITEM_KEY1 || e->class == QE_ITEM_KEY2 || e->class == QE_ITEM_SIGIL ||
	       (e->class >= QE_WEAPON_SUPERSHOTGUN && e->class <= QE_WEAPON_LIGHTNING);
}

/* the kinds' assets at their nearest living monster's distance (Chebyshev,
   world units), the gibs with the nearest of all; a streamed model of
   another edict's (a pack, an item) at its own, the keys' and the weapons'
   always; a dead monster's body or head, a gib, under every living want
   (STREAM_DEAD) */
void monsters_stream_wants(void)
{
	uint32_t near[MK_COUNT];
	for (int k = 0; k < MK_COUNT; k++)
		near[k] = ~0u;
	const int32_t *p = stream_origin;
	for (int i = 0; i < ents_num_streamed; i++) {
		const edict_t *e = &edicts[ents_streamed_list[i]];
		if (!e->inuse)
			continue;
		uint32_t d = cheb(e->origin, p);
#ifdef Q1PSX_BENCH
		if ((stream_rank_mode == 1 || stream_rank_mode == 3) && e->mdef) {   /* (the residency
		                                                                          bench: the PVS first) */
			const qw_leaf *l = ent_model_leaf((edict_t *) e);
			if (!l || l->visframe != (uint16_t) r_state.visframe)
				d += STREAM_REACH;
		}
#endif
		int k = e->mdef ? e->mdef->kind : -1;
		if (k < 0 && e->class >= QE_MONSTER_ARMY && e->class <= QE_MONSTER_OLDONE)
			k = kind_of(e->class);
		if (k >= 0 && (!e->mdef || e->health > 0)) {
			if (d < near[k])
				near[k] = d;
		} else if (e->masset >= 0) {
			stream_want(e->masset, pinned(e) ? STREAM_PIN
			                       : k >= 0 || e->masset == gib_asset[0] || e->masset == gib_asset[1] ||
			                         e->masset == gib_asset[2] ? STREAM_DEAD + d : d);
		}
	}
	uint32_t gibs = ~0u;
	for (int k = 0; k < MK_COUNT; k++) {
		if (near[k] == ~0u)
			continue;
#ifdef Q1PSX_BENCH
		if (stream_rank_mode == 3) {              /* (the bench: the head under the models) */
			stream_want_group_but(kind_asset[k][0], near[k], kind_asset[k][1]);
			for (int i = 0; i < MK_ASSETS; i++)
				stream_want(kind_asset[k][i], i == 1 ? near[k] + STREAM_REACH : near[k]);
			if (near[k] < gibs)
				gibs = near[k];
			continue;
		}
#endif
		stream_want_group(kind_asset[k][0], near[k]);   /* its model, head, sounds */
		for (int i = 0; i < MK_ASSETS; i++)
			stream_want(kind_asset[k][i], near[k]);
		if (near[k] < gibs)
			gibs = near[k];
	}
	for (int i = 0; i < 3; i++)
#ifdef Q1PSX_BENCH
		stream_want(gib_asset[i], stream_rank_mode == 3 && gibs != ~0u ? gibs + STREAM_REACH : gibs);
#else
		stream_want(gib_asset[i], gibs);
#endif
}

#ifdef Q1PSX_BENCH
int monsters_drawn;
#endif

void monsters_render(void)
{
	uint16_t vis = (uint16_t) r_state.visframe;
#ifdef Q1PSX_BENCH
	monsters_drawn = 0;
#endif
	for (int i = 0; i < ents_num_streamed; i++) {
		edict_t *e = &edicts[ents_streamed_list[i]];
		if (!e->inuse || e->masset < 0)
			continue;
		const qw_leaf *leaf = ent_model_leaf(e);
		if (!leaf || leaf->visframe != vis)
			continue;
		const qst_model *m = stream_model(e->masset);
		if (!m || !ent_in_view(e->origin, m->radius))
			continue;
#ifdef Q1PSX_BENCH
		monsters_drawn++;
#endif
		/* an item's flame: an animated group, 10 a second, its phase; lit
		   by itself (fullbright) */
		int frame = e->frame, flame = !e->mdef && e->cnt && e->class != QE_MISC_TELEPORTTRAIN;
		if (flame)
			frame = e->frame + (int) ((game_tick_count / 6 + (e - edicts)) % e->cnt);
		int light = (m->flags & QSTM_FLAT) && flame ? 128
		          : leaf->light + dl_add_at(e->origin, game_tick_count) * 3 / 2;
		if (e->mdef && e->health <= 0) {          /* a body's last second: darkened */
			int32_t left = e->pain_finished - game_tick_count;
			if (left < CORPSE_FADE)
				light = left > 0 ? light * left / CORPSE_FADE : 0;
		}
		int yaw = QSTM_EF(m->flags) & QSTM_EF_ROTATE ? (game_tick_count % 216) * ANG_TURN / 216   /* 100 a second */
		                                             : e->ang[1] >> 4;
		rl_add_model(leaf, rl_model_stream(e->masset, frame, e->origin, yaw, e->ang[0] >> 4, e->ang[2] >> 4,
		                                   light), e->origin, e->mins);
	}
}
