/*
 * q1psx — the monsters: the map's monster_* entities, native C transcribed
 * from id's QC (monsters.qc, ai.qc, fight.qc and each monster's file), on
 * the edicts (engine/ents.h). Their models are streamed (engine/stream.h):
 * a monster's kind is loaded when the player comes near one, and a monster
 * whose model is not in yet thinks and moves all the same (the QC never
 * looks at a model), it is only not drawn.
 */

#ifndef Q1PSX_MONSTERS_H
#define Q1PSX_MONSTERS_H

#include "ents.h"

/* the monsters' kinds (the shareware's, then the registered game's) */
enum {
	MK_ARMY, MK_DOG, MK_OGRE, MK_KNIGHT, MK_ZOMBIE, MK_WIZARD, MK_DEMON, MK_SHAMBLER, MK_BOSS,
	MK_COUNT
};

/* a kind's assets: its model, its head, its projectiles (the stream's
   names), wanted together */
#define MK_ASSETS 4

/* the map's start, before its entities spawn: the kinds' assets found in
   the stream's directory */
void monsters_map_start(void);

/* ents_spawn's: a monster_* entity */
void monsters_spawn(edict_t *e, const qw_ent *q);

/* the residency pass (engine/stream.c, through ents_stream_wants): what the
   monsters' kinds need, by their distance from the player */
void monsters_stream_wants(void);

/* per frame, after the brush models: the streamed models into the render
   list (the monsters, their heads and gibs) */
void monsters_render(void);

#endif
