/*
 * q1psx — the monsters: the map's monster_* entities, native C transcribed
 * from id's QC (monsters.qc, ai.qc, fight.qc, combat.qc and each monster's
 * file; the shareware's progs.dat 1.06 where the rerelease's QC differs)
 * on the edicts (engine/ents.h). Their models are streamed
 * (engine/stream.h): a monster's kind is loaded when the player comes near
 * one, and a monster whose model is not in yet thinks and moves all the
 * same (the QC never looks at a model), it is only not drawn.
 *
 * A monster's animation is QC's frame functions, `name =[ $frame, next ]
 * { code }`: a table of states, one per function (its frame, the state the
 * next think runs, its code as an action and an argument), run by one
 * think (monster_think): the frame set, the next think a tenth of a second
 * on, then the action (ai_stand, ai_run(dist)...), or the kind's own code
 * (A_CUSTOM). Its th_* are the kind's functions.
 *
 * Units as the edicts': positions 1/CLIP_SUB world units, velocities units
 * per second, times ticks (60 a second); a monster's angles are 65536 a
 * turn (Quake's anglemod), its ideal_yaw whole degrees (vectoyaw's).
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
#define MK_ASSETS 5

/* defs.qc */
enum { RANGE_MELEE, RANGE_NEAR, RANGE_MID, RANGE_FAR };
enum { AS_STRAIGHT = 1, AS_SLIDING, AS_MELEE, AS_MISSILE };
enum { DEAD_NO, DEAD_DYING, DEAD_DEAD };
#define FL_FLY            1
#define FL_SWIM           2
#define FL_CLIENT         8
#define FL_INWATER       16
#define FL_MONSTER       32
#define FL_GODMODE       64
#define FL_NOTARGET     128
#define FL_ONGROUND     512
#define FL_PARTIALGROUND 1024
#define EF_MUZZLEFLASH    2

/* a state's action (its QC function's code) */
enum {
	A_NONE, A_STAND, A_WALK, A_RUN, A_FACE, A_CHARGE, A_PAIN, A_PAINFWD, A_FORWARD, A_BACK,
	A_TURN, A_MELEE, A_CHARGESIDE, A_MELEESIDE, A_CUSTOM
};

typedef struct {
	uint8_t frame;           /* the model's frame                             */
	uint8_t next;            /* the state the next think runs                 */
	uint8_t act;             /* A_*                                           */
	int8_t  arg;             /* its distance (world units), or A_CUSTOM's code */
} mstate_t;

typedef struct mdef_s {
	const char     *classname;        /* "monster_army" (T_Damage's same-kind test) */
	const char     *assets[MK_ASSETS];/* the model first, then what its QC precaches */
	int16_t         mins[3], maxs[3]; /* setsize, world units                        */
	int16_t         health;
	uint8_t         kind;             /* MK_*                                        */
	uint8_t         fly;              /* flymonster_start (else walkmonster_start)   */
	const mstate_t *states;
	uint8_t         num_states;
	void (*th_stand)(edict_t *self);
	void (*th_walk)(edict_t *self);
	void (*th_run)(edict_t *self);
	void (*th_missile)(edict_t *self);   /* NULL: none                              */
	void (*th_melee)(edict_t *self);
	uint8_t th_pain;                     /* EF_* (ent_fns.h): attacker: ent_damage_attacker,
	                                        the damage taken: ai_damage_take          */
	uint8_t th_die;
	void (*custom)(edict_t *self, int code);   /* A_CUSTOM's code                    */
	int  (*check_attack)(edict_t *self);       /* CheckAnyAttack's kind's, NULL:
	                                              CheckAttack                       */
	int  (*spawn)(edict_t *self);     /* its spawn function's own end: 1 if it
	                                     started it (no walk/flymonster_start)   */
	void (*map_start)(void);          /* the kind's sounds found in the map's bank  */
	const int16_t  *sight;            /* SightSound's sound (snd_find's)            */
} mdef_t;

/* ---- the AI (engine/ai.c: ai.qc, fight.qc, monsters.qc, sv_move.c) ------- */

extern edict_t *ai_sight_entity;
extern int32_t  ai_sight_entity_time;
extern int      ai_enemy_visible, ai_enemy_infront, ai_enemy_range, ai_enemy_yaw;
extern int      ai_movedist;          /* units */
extern int      ai_damage_take;       /* th_pain's damage */
extern int      ai_trace_inopen, ai_trace_inwater;
extern int32_t  ai_trace_endpos[3];
extern edict_t *ai_trace_ent;
extern int16_t  ai_trace_normal[3];
/* the QC's v_forward, v_right, v_up: the last makevectors' (the monsters',
   and the player's view's from its PlayerPreThink each tick), x4096 */
extern int32_t  ai_v_forward[3], ai_v_right[3], ai_v_up[3];
#ifdef Q1PSX_BENCH
extern uint32_t ai_traces;            /* the monsters' traces so far (the bench's) */
/* the traces' cost by caller (the bench's): count and cycles; the tag is
   the next trace's, set by its caller */
enum { AI_TR_OTHER, AI_TR_VISIBLE, AI_TR_ATTACK, AI_TR_BOTTOM, AI_TR_WALK, AI_TR_FLY, AI_TR_CORNERS,
       AI_TR_TOUCH, AI_TR_TAGS };
extern uint32_t ai_tr_n[AI_TR_TAGS], ai_tr_cyc[AI_TR_TAGS];
extern int      ai_tr_tag;
#endif

/* the map's start: the checks' state cleared */
void ai_map_start(void);

/* QC builtins */
uint32_t frand(void);                 /* random(): 0 .. 65535 (x 1/65536)          */
int  crand(void);                     /* crandom(): -65535 .. 65535                */
int  vectoyaw(const int32_t v[3]);    /* whole degrees, 0..359                     */
void ai_makevectors(const uint16_t ang[3], int32_t f[3], int32_t r[3], int32_t u[3]);   /* x4096,
                                                                                       ai_v_* too */
int  ai_vlen(const int32_t v[3]);     /* 1/CLIP_SUB units in, units out            */
/* vlen (v) compared with a distance exactly and without a root: v's
   square (1/CLIP_SUB units) against AI_UNITS2 (units) */
static inline int64_t ai_vlen2(const int32_t v[3])
{
	return (int64_t) v[0] * v[0] + (int64_t) v[1] * v[1] + (int64_t) v[2] * v[2];
}
#define AI_UNITS2(u) ((int64_t) ((u) * CLIP_SUB) * ((u) * CLIP_SUB))
q_fixed ai_traceline(const int32_t a[3], const int32_t b[3], int nomonsters, edict_t *pass);
int  ai_walkmove(edict_t *self, int yaw, int dist);   /* degrees, units: 1 if moved */
void ai_movetogoal(edict_t *self, int dist);
void ai_changeyaw(edict_t *self);
int  ai_droptofloor(edict_t *self);
int  ai_check_bottom(edict_t *self);
edict_t *ai_checkclient(edict_t *self);
void ai_view_org(const edict_t *e, int32_t out[3]);   /* origin + view_ofs         */

/* ai.qc / fight.qc */
int  ai_range(edict_t *self, edict_t *targ);
int  ai_visible(edict_t *self, edict_t *targ);
int  ai_infront(edict_t *self, edict_t *targ);
void ai_stand(edict_t *self);
void ai_walk(edict_t *self, int dist);
void ai_run(edict_t *self, int dist);
void ai_turn(edict_t *self);
void ai_face(edict_t *self);
void ai_charge(edict_t *self, int dist);
void ai_charge_side(edict_t *self);
void ai_melee(edict_t *self);
void ai_melee_side(edict_t *self);
void ai_pain(edict_t *self, int dist);
void ai_painforward(edict_t *self, int dist);
void ai_forward(edict_t *self, int dist);
void ai_back(edict_t *self, int dist);
int  ai_find_target(edict_t *self);
void ai_found_target(edict_t *self);
void ai_hunt_target(edict_t *self);
int  ai_check_attack(edict_t *self);
int  ai_facing_ideal(edict_t *self);
void sub_attack_finished(edict_t *self, int ticks);
void sub_check_refire(edict_t *self, int state);
int  ai_can_damage(edict_t *targ, edict_t *inflictor);
int  ai_can_damage_at(edict_t *targ, const int32_t org[3], edict_t *pass);   /* from a point */

/* the monsters' physics (SV_Physics_Step; SV_Physics_Toss for what they
   throw), from ents_tick; SV_TestEntityPosition and SV_PushEntity (the
   pushers', engine/ents.c) */
void ai_physics_step(edict_t *e);
void ai_physics_toss(edict_t *e);
int  ai_test_position(edict_t *e);
void ai_push_entity(edict_t *e, const int32_t push[3], trace_t *tr);

/* ---- monsters.c ---------------------------------------------------------- */

/* the map's start, before its entities spawn: the kinds' assets found in
   the stream's directory, their sounds in the bank */
void monsters_map_start(void);

/* ents_spawn's: a monster_* entity; event_lightning (boss.qc: E1M7's
   lightning that hurts Chthon) */
void monsters_spawn(edict_t *e, const qw_ent *q);
void monsters_event_lightning(edict_t *e);

/* the stream's index of asset i of the monster's kind (its mdef's), -1 */
int  monster_asset(const edict_t *self, int i);
int  monsters_kind_asset(int kind, int i);

/* run state `s` of the monster's kind now (a QC frame function's call) */
void monster_state(edict_t *self, int s);

/* the think every state sets: the state the last one named */
void monster_think(edict_t *self);

/* `self.think = <state>; self.nextthink = time + t` (a touch's, a use's):
   state `s` runs `ticks` on */
void monster_think_at(edict_t *self, int s, int ticks);

/* monsters.qc */
void monster_use(edict_t *self);
void monster_death_use(edict_t *self);

/* A dead monster's body (or its head) is removed CORPSE_TICKS after its
   death (combat.qc Killed: pain_finished, unused once dead, holds the
   time), its last CORPSE_FADE darkened to black (2026-10-19:
   the dead not kept before the living); a monster whose model is not in
   (engine/stream.c) is paused (ents_tick). */
#define CORPSE_TICKS TICKS(600)
#define CORPSE_FADE  TICKS(100)

/* the residency pass (engine/stream.c, through ents_stream_wants): what the
   monsters' kinds need, by their living members' distance from the player,
   those the player's PVS holds first, the heads and gibs a tier under the
   models; the dead's remains under them all (STREAM_DEAD) */
void monsters_stream_wants(void);

/* per frame, after the brush models: the streamed models into the render
   list (the monsters, their heads and gibs, what they drop, the items'
   alias models: the flames animated and fullbright, the pickups turning) */
void monsters_render(void);
#ifdef Q1PSX_BENCH
extern int monsters_drawn;            /* the last frame's streamed models drawn */
#endif

/* player.qc's ThrowGib / ThrowHead, items.qc's DropBackpack: `dm` the
   health below 0 that threw them */
void velocity_for_damage(int dm, int32_t v[3]);   /* VelocityForDamage */
void throw_gib(edict_t *self, int gib, int dm);   /* gib 0..2: gib1..3, 3: zom_gib */
void throw_head(edict_t *self, int dm);           /* the kind's head */
void drop_backpack(edict_t *self);
void spawn_meat_spray(edict_t *self, const int32_t org[3], const int32_t vel[3]);   /* zom_gib.mdl */

#endif
