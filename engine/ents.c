#include <stdio.h>
#include <string.h>

#include "ents.h"
#include "mem.h"
#include "render.h"
#include "rlist.h"
#include "world.h"

clip_bmodel_t clip_bmodels[CLIP_MAX_BMODELS];
int           clip_num_bmodels;

edict_t *edicts;
int      num_edicts;
static int max_edicts;
static uint16_t draw_list[CLIP_MAX_BMODELS + 64];   /* the edicts with a model drawn */
static int      num_draw;

#define ENT_SPARE 64         /* edicts beyond the map's, for what spawns during play */

/* SV_LinkEdict: the box in the world, the leaves it touches */
static void ent_link(edict_t *e)
{
	for (int k = 0; k < 3; k++) {
		e->absmin[k] = e->origin[k] + e->mins[k];
		e->absmax[k] = e->origin[k] + e->maxs[k];
	}
	e->num_leaves = (uint8_t) world_box_leaves(e->absmin, e->absmax, e->leaves, ENT_MAX_LEAVES);
}

/* setmodel(): the brush model's box */
static void ent_setmodel(edict_t *e, int model)
{
	const qw_bmodel *bm = &world.bmodels[model];
	e->model = (int16_t) model;
	memcpy(e->mins, bm->mins, sizeof e->mins);
	memcpy(e->maxs, bm->maxs, sizeof e->maxs);
	ent_link(e);
}

/* a SOLID_BSP model: into the traces' table */
static void ent_clip_add(edict_t *e)
{
	if (clip_num_bmodels >= CLIP_MAX_BMODELS) {
		printf("ents: more than %d solid brush models\n", CLIP_MAX_BMODELS);
		return;
	}
	clip_bmodel_t *cb = &clip_bmodels[clip_num_bmodels++];
	cb->bm = &world.bmodels[e->model];
	memcpy(cb->origin, e->origin, sizeof cb->origin);
	memcpy(cb->absmin, e->absmin, sizeof cb->absmin);
	memcpy(cb->absmax, e->absmax, sizeof cb->absmax);
	e->clip = (uint8_t) clip_num_bmodels;
}

void ents_load(void)
{
	num_edicts = 0;
	max_edicts = (int) world.num_ents + ENT_SPARE;
	edicts = mem_level_alloc((uint32_t) max_edicts * sizeof *edicts);
	if (!edicts) {
		printf("ents: no room for %d edicts\n", max_edicts);
		max_edicts = 0;
	}
}

/* the edict for a cooked entity: the fields every class takes (ED_ParseEdict) */
static edict_t *ent_new(const qw_ent *q)
{
	if (num_edicts >= max_edicts)
		return 0;
	edict_t *e = &edicts[num_edicts++];
	memset(e, 0, sizeof *e);
	e->class      = q->class;
	e->spawnflags = q->spawnflags;
	memcpy(e->origin, q->origin, sizeof e->origin);
	e->angle      = q->angle;
	e->angles[1]  = q->angle * ANG_TURN / 360;
	memcpy(e->mangle, q->mangle, sizeof e->mangle);
	e->speed  = q->speed;  e->wait   = q->wait;   e->delay  = q->delay;
	e->lip    = q->lip;    e->height = q->height; e->dmg    = q->dmg;
	e->health = q->health; e->count  = q->count;  e->sounds = q->sounds;
	e->style  = q->style;
	e->target = q->target; e->targetname = q->targetname; e->killtarget = q->killtarget;
	e->message = q->message; e->map = q->map;
	return e;
}

void ents_spawn(void)
{
	clip_num_bmodels = 0;
	num_edicts = 0;
	num_draw = 0;
	int unknown = 0, skipped = 0, noleaf = 0, drawn = 0;
	for (uint32_t i = 0; i < world.num_ents && edicts; i++) {
		const qw_ent *q = &world.ents[i];
		int cls = q->class;
		if (cls >= QE_MONSTER_ARMY && cls <= QE_MONSTER_OLDONE) {   /* (Phase 8.1) */
			skipped++;
			continue;
		}
		if (cls == QE_UNKNOWN) {
			unknown++;
			continue;
		}
		if (cls == QE_FUNC_EPISODEGATE)         /* shown once its episode is done
		                                           (serverflags): never yet */
			continue;
		edict_t *e = ent_new(q);
		if (!e)
			break;
		switch (cls) {
		case QE_FUNC_WALL:                      /* misc.qc func_wall */
		case QE_FUNC_BOSSGATE:                  /* (all four runes: never yet) */
		case QE_FUNC_DOOR:                      /* doors.qc, plats.qc, buttons.qc: */
		case QE_FUNC_DOOR_SECRET:               /* the movers, at their map position */
		case QE_FUNC_BUTTON:                    /* until they move */
		case QE_FUNC_PLAT:
		case QE_FUNC_TRAIN:
			e->movetype = MOVETYPE_PUSH;
			e->solid    = SOLID_BSP;
			if (q->model) {
				ent_setmodel(e, q->model);
				ent_clip_add(e);
				e->draw = 1;
			}
			break;
		case QE_FUNC_ILLUSIONARY:               /* misc.qc: drawn, not solid */
			e->movetype = MOVETYPE_NONE;
			e->solid    = SOLID_NOT;
			if (q->model) {
				ent_setmodel(e, q->model);
				e->draw = 1;
			}
			break;
		case QE_TRIGGER_ONCE: case QE_TRIGGER_MULTIPLE: case QE_TRIGGER_SECRET:
		case QE_TRIGGER_COUNTER: case QE_TRIGGER_TELEPORT: case QE_TRIGGER_CHANGELEVEL:
		case QE_TRIGGER_SETSKILL: case QE_TRIGGER_ONLYREGISTERED: case QE_TRIGGER_PUSH:
		case QE_TRIGGER_HURT: case QE_TRIGGER_MONSTERJUMP:
			e->movetype = MOVETYPE_NONE;        /* subs.qc InitTrigger: the box, unseen */
			e->solid    = SOLID_TRIGGER;
			if (q->model)
				ent_setmodel(e, q->model);
			break;
		default:                                /* point entities: as placed */
			e->movetype = MOVETYPE_NONE;
			e->solid    = SOLID_NOT;
			ent_link(e);
			break;
		}
		if (e->draw) {
			drawn++;
			if (!e->num_leaves)
				noleaf++;
			else if (num_draw < (int) (sizeof draw_list / sizeof draw_list[0]))
				draw_list[num_draw++] = (uint16_t) (e - edicts);
		}
	}
	printf("ents: %d edicts (%d monsters left out, %d unknown), %d solid brush models, "
	       "%d drawn (%d touch no leaf)\n", num_edicts, skipped, unknown, clip_num_bmodels,
	       drawn, noleaf);
}

/* A leaf's box in front of the eye (its depth along the view's forward
   row, x1.5: R_MATRIX_ONE; the extents' sum covers the box); the depth
   out. The walk's frustum test decides the rest. */
static int leaf_in_front(const qw_leaf *l, int32_t *depth)
{
	int32_t z = ((r_state.rot[6] * l->c[0] + r_state.rot[7] * l->c[1] +
	              r_state.rot[8] * l->c[2]) >> 12) + r_state.tr[2];
	int32_t rad = (l->h[0] + l->h[1] + l->h[2]) * 3 / 2;
	if (z + rad <= 0)
		return 0;
	*depth = z;
	return 1;
}

int ents_stat[4];   /* the last frame: models drawn, PVS-visible, leaves in view, candidates */
int ents_debug;     /* the bench: print each model drawn this frame */

void ents_render(void)
{
	if (!edicts)
		return;
	uint16_t vis = (uint16_t) r_state.visframe;
	ents_stat[0] = ents_stat[1] = ents_stat[2] = 0;
	ents_stat[3] = num_draw;
	for (int i = 0; i < num_draw; i++) {
		edict_t *e = &edicts[draw_list[i]];
		/* the leaves of its box the PVS sees: found again when the stamp
		   changes (the camera's leaf), a bit each meanwhile */
		if (e->vis_stamp != vis) {
			e->vis_stamp = vis;
			e->vismask   = 0;
			for (int j = 0; j < e->num_leaves; j++)
				if (world.leaves[e->leaves[j]].visframe == vis)
					e->vismask |= (uint8_t) (1 << j);
		}
		if (!e->vismask)
			continue;
		ents_stat[1]++;
		/* among them, the one in front of the eye nearest it: drawn there,
		   over what the walk draws after it (a single one: as it is) */
		const qw_leaf *best = 0;
		int32_t bestz = 0;
		if (!(e->vismask & (e->vismask - 1))) {
			for (int j = 0; j < e->num_leaves; j++)
				if (e->vismask & (1 << j))
					best = &world.leaves[e->leaves[j]];
			ents_stat[2]++;
		} else {
			for (int j = 0; j < e->num_leaves; j++) {
				const qw_leaf *l = &world.leaves[e->leaves[j]];
				int32_t z;
				if (!(e->vismask & (1 << j)) || !leaf_in_front(l, &z))
					continue;
				ents_stat[2]++;
				if (!best || z < bestz) {
					best  = l;
					bestz = z;
				}
			}
		}
		if (!best)
			continue;
		ents_stat[0]++;
		if (ents_debug)
			printf("  bm *%d leaf %d (vis %u / %u, c %d %d %d, list %08x) mask %02x of %d leaves\n",
			       e->model, (int) (best - world.leaves), best->visframe, vis, best->c[0], best->c[1],
			       best->c[2], (unsigned) best->vis, e->vismask, e->num_leaves);
		rl_add_model(best, rl_bmodel(&world.bmodels[e->model], e->origin));
	}
}
