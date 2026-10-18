/*
 * q1psx — the monsters (engine/monsters.h): their kinds, their spawn, their
 * streamed assets' wants, their drawing.
 */

#include <stdio.h>
#include <string.h>

#include "dlight.h"
#include "game.h"
#include "monsters.h"
#include "render.h"
#include "rlist.h"
#include "stream.h"
#include "world.h"

typedef struct mdef_s {
	const char *assets[MK_ASSETS];   /* the model first, then what its QC precaches */
	int16_t     mins[3], maxs[3];    /* setsize, world units                        */
	int16_t     health;
	uint8_t     stand;               /* the standing frame                          */
} mdef_t;

static const mdef_t MDEFS[MK_COUNT] = {
	[MK_ARMY]     = { { "progs/soldier.mdl", "progs/h_guard.mdl", "progs/backpack.mdl" },
	                  { -16, -16, -24 }, { 16, 16, 40 }, 30, 0 },
	[MK_DOG]      = { { "progs/dog.mdl", "progs/h_dog.mdl" },
	                  { -32, -32, -24 }, { 32, 32, 40 }, 25, 69 },
	[MK_OGRE]     = { { "progs/ogre.mdl", "progs/h_ogre.mdl" },
	                  { -32, -32, -24 }, { 32, 32, 64 }, 200, 0 },
	[MK_KNIGHT]   = { { "progs/knight.mdl", "progs/h_knight.mdl" },
	                  { -16, -16, -24 }, { 16, 16, 40 }, 75, 0 },
	[MK_ZOMBIE]   = { { "progs/zombie.mdl", "progs/h_zombie.mdl", "progs/zom_gib.mdl" },
	                  { -16, -16, -24 }, { 16, 16, 40 }, 60, 0 },
	[MK_WIZARD]   = { { "progs/wizard.mdl", "progs/h_wizard.mdl", "progs/w_spike.mdl" },
	                  { -16, -16, -24 }, { 16, 16, 40 }, 80, 0 },
	[MK_DEMON]    = { { "progs/demon.mdl", "progs/h_demon.mdl" },
	                  { -32, -32, -24 }, { 32, 32, 64 }, 300, 0 },
	[MK_SHAMBLER] = { { "progs/shambler.mdl", "progs/h_shams.mdl", "progs/s_light.mdl", "progs/bolt.mdl" },
	                  { -32, -32, -24 }, { 32, 32, 64 }, 600, 0 },
	[MK_BOSS]     = { { "progs/boss.mdl", "progs/lavaball.mdl" },
	                  { -128, -128, -24 }, { 128, 128, 256 }, 3, 0 },
};

static int16_t kind_asset[MK_COUNT][MK_ASSETS];   /* the stream's indices, -1 none */
static int16_t gib_asset[3];                      /* gib1..3: every monster's     */
static uint8_t kind_present[MK_COUNT];            /* the map has one              */

void monsters_map_start(void)
{
	static const char *const gibs[3] = { "progs/gib1.mdl", "progs/gib2.mdl", "progs/gib3.mdl" };
	for (int k = 0; k < MK_COUNT; k++) {
		kind_present[k] = 0;
		for (int i = 0; i < MK_ASSETS; i++)
			kind_asset[k][i] = (int16_t) (MDEFS[k].assets[i] ? stream_find(MDEFS[k].assets[i]) : -1);
	}
	for (int i = 0; i < 3; i++)
		gib_asset[i] = (int16_t) stream_find(gibs[i]);
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

void monsters_spawn(edict_t *e, const qw_ent *q)
{
	(void) q;
	int k = kind_of(e->class);
	if (k < 0) {
		ent_remove(e);
		return;
	}
	const mdef_t *d = &MDEFS[k];
	kind_present[k] = 1;
	e->mdef     = d;
	e->solid    = SOLID_SLIDEBOX;
	e->movetype = MOVETYPE_STEP;
	e->ang[1]   = (uint16_t) ((e->angle % 360 + 360) % 360 * 65536 / 360);
	e->frame    = d->stand;
	e->masset   = k == MK_BOSS ? -1 : kind_asset[k][0];   /* (the boss: no model until it rises) */
	e->health   = d->health;
	int32_t mins[3], maxs[3];
	for (int i = 0; i < 3; i++) {
		mins[i] = d->mins[i] * CLIP_SUB;
		maxs[i] = d->maxs[i] * CLIP_SUB;
	}
	ent_setsize(e, mins, maxs);
	ent_total_monsters++;                     /* walkmonster_start's total_monsters */
}

/* the kinds' assets at their nearest monster's distance (Chebyshev, world
   units), the gibs with them */
void monsters_stream_wants(void)
{
	uint32_t near[MK_COUNT];
	for (int k = 0; k < MK_COUNT; k++)
		near[k] = ~0u;
	const int32_t *p = stream_origin;
	for (int i = 0; i < ents_num_streamed; i++) {
		const edict_t *e = &edicts[ents_streamed_list[i]];
		if (!e->inuse || !e->mdef)
			continue;
		int k = (int) ((const mdef_t *) e->mdef - MDEFS);
		uint32_t d = 0;
		for (int j = 0; j < 3; j++) {
			int32_t v = e->origin[j] - p[j];
			uint32_t a = (uint32_t) (v < 0 ? -v : v);
			if (a > d) d = a;
		}
		d /= CLIP_SUB;
		if (d < near[k])
			near[k] = d;
	}
	uint32_t gibs = ~0u;
	for (int k = 0; k < MK_COUNT; k++) {
		if (near[k] == ~0u)
			continue;
		for (int i = 0; i < MK_ASSETS; i++)
			stream_want(kind_asset[k][i], near[k]);
		if (near[k] < gibs)
			gibs = near[k];
	}
	for (int i = 0; i < 3; i++)
		stream_want(gib_asset[i], gibs);
}

void monsters_render(void)
{
	uint16_t vis = (uint16_t) r_state.visframe;
	for (int i = 0; i < ents_num_streamed; i++) {
		edict_t *e = &edicts[ents_streamed_list[i]];
		if (!e->inuse || e->masset < 0)
			continue;
		const qw_leaf *leaf = e->leaves[0];
		if (!leaf || leaf->visframe != vis)
			continue;
		const qst_model *m = stream_model(e->masset);
		if (!m || !ent_in_view(e->origin, m->radius))
			continue;
		int light = leaf->light + dl_add_at(e->origin, game_tick_count) * 3 / 2;
		rl_add_model(leaf, rl_model_stream(e->masset, e->frame, e->origin, e->ang[1] >> 4, e->ang[0] >> 4,
		                                   e->ang[2] >> 4, light));
	}
}
