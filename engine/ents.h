/*
 * q1psx — the map's entities: Quake's edicts, native (docs/porting-notes.md
 * §28-§29). The cooked entity table (formats/qworld.h qw_ent) is spawned at
 * the map's load into a pool of edicts in the level arena, class by class as
 * the QC's spawn functions do; their think / touch / use / blocked / th_die
 * functions are the QC's, transcribed line by line (engine/triggers.c,
 * engine/movers.c, engine/level.c, engine/items.c). The world is edicts[0],
 * the player's mirror edicts[1] (its box, health and items follow the
 * game's player each tick).
 *
 * Each tick (ents_tick, after the player moved, Quake's order): the
 * triggers the player's box overlaps are touched (SV_TouchLinks), then the
 * edicts with something to do run their physics in edict order
 * (SV_Physics_Pusher for MOVETYPE_PUSH, SV_RunThink for the rest). The
 * brush models among them are drawn by the renderer at their origin (each
 * in the visible leaf of its box nearest the camera, in the painter's
 * order) and the solid ones clip the traces (engine/clip.h clip_bmodels).
 *
 * Units: positions 1/CLIP_SUB world units, velocities units per second,
 * directions x4096, angles binary degrees, times in ticks (the cooked wait
 * and delay are hundredths of a second: TICKS()).
 */

#ifndef Q1PSX_ENTS_H
#define Q1PSX_ENTS_H

#include <stdint.h>

#include "clip.h"
#include "qworld.h"

/* Quake's solid, movetype, mover states and flags (defs.qc) */
enum { SOLID_NOT, SOLID_TRIGGER, SOLID_BBOX, SOLID_SLIDEBOX, SOLID_BSP };
enum { MOVETYPE_NONE, MOVETYPE_WALK = 3, MOVETYPE_STEP, MOVETYPE_FLY, MOVETYPE_TOSS,
       MOVETYPE_PUSH, MOVETYPE_NOCLIP, MOVETYPE_FLYMISSILE, MOVETYPE_BOUNCE };
enum { STATE_TOP, STATE_BOTTOM, STATE_UP, STATE_DOWN };
enum { DAMAGE_NO, DAMAGE_YES, DAMAGE_AIM };
#define FL_ITEM 256

#define QE_WORLD  254        /* edicts[0]'s class */
#define QE_PLAYER 255        /* the player's mirror edict's class */
#define QE_TEMP   253        /* an edict the QC spawns (DelayedUse, a field, a fog) */
#define ENT_MAX_LEAVES 8     /* leaves a box touches (the render list's choice) */

#ifndef TICKS
#define TICKS(sec100) ((sec100) * 60 / 100)   /* hundredths of a second -> ticks */
#endif

typedef struct edict_s edict_t;
typedef void (*ent_fn)(edict_t *self);   /* think / use / touch / blocked / th_die:
                                            `other` and `activator` are globals,
                                            as in QC (ent_other, ent_activator) */

struct edict_s {
	uint8_t  inuse;          /* a live edict (ED_Alloc'd, not freed)           */
	uint8_t  class;          /* QE_*                                           */
	uint8_t  solid;          /* SOLID_*                                        */
	uint8_t  movetype;       /* MOVETYPE_*                                     */
	uint8_t  state;          /* STATE_*                                        */
	uint8_t  draw;           /* the brush model is drawn                       */
	uint8_t  clip;           /* clip_bmodels index + 1 while SOLID_BSP, 0 = not */
	uint8_t  num_leaves;     /* the leaves the box touches                     */
	uint8_t  takedamage;     /* DAMAGE_*                                       */
	uint8_t  vismask;        /* which leaves the PVS stamp vis_stamp sees      */
	uint8_t  sounds;
	uint8_t  frame;          /* the texture frame (a button's +a, func_wall)   */
	uint16_t spawnflags;
	uint16_t vis_stamp;
	int16_t  model;          /* brush model index (world.bmodels), 0 = none    */
	int16_t  angle;          /* the map's `angle`, degrees (-1 up, -2 down)    */
	int16_t  mangle[3];      /* degrees: pitch, yaw, roll                      */
	int16_t  pad0;
	const qw_leaf *leaves[ENT_MAX_LEAVES];
	uint32_t topnode;        /* a drawn brush model's: world_box_topnode of the
	                            points camera_side_leaf may take (ents.c)     */
	int32_t  origin[3];      /* 1/CLIP_SUB units                               */
	int32_t  oldorigin[3];
	int32_t  mins[3], maxs[3];       /* the box, local                          */
	int32_t  absmin[3], absmax[3];   /* in the world, a unit wider (SV_LinkEdict) */
	int32_t  velocity[3];    /* units per second (a pusher's: its travel's)    */
	int32_t  movedir[3];     /* x4096                                          */
	const struct mdef_s *mdef;   /* a monster's kind (monsters.c), its body's and
	                                head's too; 0 for every other edict. Not in
	                                the union below: a mover's words there read
	                                as a pointer (an opened secret door's stop,
	                                E1M3: a bus error, DuckStation frozen) */
	union {
		struct {             /* a mover's (movers.c, subs.qc's SUB_CalcMove)   */
			int32_t pos1[3], pos2[3];        /* the mover's ends                */
			int32_t dest1[3], dest2[3];      /* the secret door's stops         */
			int32_t finaldest[3];
			int32_t move_from[3];  /* SUB_CalcMove, exact in ticks: the position at  */
			int32_t move_delta[3]; /* ltime move_t0 plus delta x (ltime - t0) / ticks */
		};
		struct {             /* a monster's (monsters.c, ai.c), a gib's, a head's */
			edict_t *oldenemy, *goalentity, *movetarget;
			int32_t  pausetime, search_time, show_hostile, pain_finished;   /* ticks */
			uint16_t ang[3];       /* pitch, yaw, roll: 65536 a turn (anglemod's) */
			int16_t  avel[3];      /* MOVETYPE_BOUNCE's avelocity, degrees a second */
			int16_t  ideal_yaw;    /* degrees (vectoyaw's whole ones)              */
			int16_t  yaw_speed;    /* degrees a think                             */
			int16_t  view_ofs_z;   /* 1/CLIP_SUB units                            */
			uint16_t mstate;       /* the next state of its kind's table          */
			uint8_t  lefty, attack_state, deadflag, waterlevel;
			int8_t   watertype;
			uint8_t  ammo_shells, ammo_nails, ammo_rockets, ammo_cells;
			int32_t  waitmin;      /* the wizard's next idle sound (ticks)        */
			int32_t  water_at[3];  /* where its water level was last taken (the
			                          same place: the same contents, no check) */
			uint8_t  inpain;       /* the zombie's (zombie.qc)                    */
			uint8_t  trail;        /* a thrown model's trail (game_trail's type + 1) */
			uint8_t  mpad;
		};
	};
	int32_t  move_t0, move_ticks;
	int32_t  nextthink;      /* tick (a pusher's: its ltime), <= 0 = none      */
	int32_t  ltime;          /* a pusher's local time, ticks                   */
	int32_t  attack_finished;
	ent_fn   think, think1, use, touch, blocked, th_die, th_pain;
	edict_t *owner, *enemy, *trigger_field;
	edict_t *groundentity;   /* what it stands on (a monster, a tossed thing):
	                            a pusher carries it along                 */
	int16_t  speed, wait, delay, lip, height, dmg, health, max_health;
	int16_t  count, style, t_length, t_width, cnt;
	int16_t  touch_slot;     /* its place in the touch list (engine/ents.c), -1 */
	uint16_t target, targetname, killtarget, message, map;   /* strings (world_string) */
	uint16_t classname;
	const char *msgtext;     /* a message the QC sets (the map's: `message`)   */
	const char *netname;     /* an item's name ("You got the ...")             */
	int16_t  noise, noise1, noise2, noise3, noise4;   /* bank sounds (snd_find) */
	int16_t  masset;         /* a streamed model (engine/stream.h), -1 none    */
	uint32_t items;          /* a key door's key (IT_KEY1 / IT_KEY2), an item's */
	uint32_t flags;
	int32_t  aflag;          /* an item's amount (ammo, health, armor value)   */
	int32_t  item_extra;     /* an item's second value (healtype, armortype x100, weapon) */
	int32_t  item_model;     /* an item's model (formats/qgame.h QGM_*), -1 none */
};

_Static_assert(__builtin_offsetof(edict_t, maxs) == __builtin_offsetof(edict_t, mins) + 12,
               "the box's 6 words (rlist.h rl_add_model)");

extern edict_t *edicts;      /* the pool, in spawn order: [0] the world, [1] the player */
extern int      num_edicts;  /* the slots in use (the highest + 1)          */
#define ent_world  (&edicts[0])
#define ent_player (&edicts[1])

/* the QC's globals */
extern edict_t *ent_other, *ent_activator, *ent_damage_attacker;
extern int32_t  ent_time;            /* = game_tick_count during the tick */
extern int      ent_found_secrets, ent_total_secrets;
extern int      ent_killed_monsters, ent_total_monsters;
extern int      ent_force_retouch;
extern int      ents_first_spawn;    /* the map's first spawn (not a restart):
                                        its static sounds get added */

#ifdef Q1PSX_BENCH
int             ents_check(const char *where);   /* every live edict's fields in range */
extern int      ents_stat[4];   /* the last frame: models drawn, PVS-visible,
                                   leaves in view, drawable */
extern int      ents_debug;     /* print each model drawn */
extern int      ents_log;       /* print the triggers' and movers' events */
#endif

/* after world_load (the pool in the level arena) */
void ents_load(void);

/* spawn the map's entities from the cooked table (the pool emptied first);
   `first`: the map was just loaded (else the level restarts) */
void ents_spawn(int first);

/* one tick, after the player's physics: the player's touches, then the
   edicts' physics and thinks (SV_Physics's order) */
void ents_tick(void);

/* per frame, after the camera is set and before r_frame: the brush models
   into the render list (first of all: the alias models and the particles
   take their places among the pieces, engine/rlist.h), then the items'
   and the monsters' models */
void ents_render_bmodels(void);
void ents_render(void);

/* ---- the QC's builtins and subs.qc, for the class files ----------------- */

edict_t *ent_spawn(void);                       /* spawn(): a cleared edict    */
void     ent_remove(edict_t *e);                /* remove()                    */
void     ent_link(edict_t *e);                  /* SV_LinkEdict: setorigin / setsize */
void     ent_setmodel(edict_t *e, int model);   /* setmodel: the brush model's box */
void     ent_setsize(edict_t *e, const int32_t mins[3], const int32_t maxs[3]);
void     ent_setorigin(edict_t *e, const int32_t org[3]);
const qw_leaf *ent_model_leaf(edict_t *e);      /* an alias model's: its centre's leaf (NULL:
                                                   outside), found once it is drawn */
void     ent_wake(edict_t *e);                  /* on the tick's list (a think or a move set) */
edict_t *ent_find(edict_t *from, uint16_t targetname);   /* find(from, targetname, s) */
edict_t *ent_find_class(edict_t *from, int cls);         /* find(from, classname, s)  */
void     ent_sound(edict_t *e, int chan, int s, int vol, int atten);   /* sound() */
void     ent_centre(const edict_t *e, int32_t out[3]);   /* origin + (mins + maxs) / 2 */
void     ent_centerprint(edict_t *client, const char *s);
void     sub_use_targets(edict_t *self);
void     sub_calc_move(edict_t *self, const int32_t tdest[3], int speed, ent_fn func);
void     set_movedir(edict_t *self);
void     init_trigger(edict_t *self);
void     sub_null(edict_t *self);
void     sub_remove(edict_t *self);

/* combat.qc T_Damage on any edict (the player's goes to the game): the
   damage whole, or x256 (the QC's float damage, rounded up at the end) */
void     ent_damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, int damage);
void     ent_damage_x256(edict_t *targ, edict_t *inflictor, edict_t *attacker, int damage);

/* SV_TouchLinks for any edict: the triggers its box overlaps touched
   (other = it) */
void     ents_touch_links(edict_t *e);

/* the edict a trace hit (its `ent`), NULL for the world */
edict_t *ent_from_trace(const trace_t *tr);

/* SV_Impact: the player ran into the brush model of trace ent `ent`: its
   touch, other = the player */
void     ents_impact(int ent);

/* T_RadiusDamage on the edicts (the player's is the game's, after them:
   findradius's chain runs from the last edict to the player): from `org`
   (1/8 units), damage - half the distance to each box centre (x256), half
   on the attacker and on a shambler, CanDamage's clear line */
void     ents_radius_damage(const int32_t org[3], int damage, edict_t *attacker, edict_t *ignore);

/* the game's damage on the player (engine/game.c t_damage): inflictor's
   box centre for the kick and the momentum, or NULL; whole, or x256 */
void game_damage_player(int damage, const int32_t inflictor[3]);
void game_damage_player_x256(int damage, const int32_t inflictor[3]);

/* the class files' spawn functions, the cooked entity in hand */
void triggers_spawn(edict_t *e, const qw_ent *q);
void movers_spawn(edict_t *e, const qw_ent *q);
int  movers_path(const edict_t *e, int32_t (*out)[3], const edict_t **seen,
                 int max);                       /* a train's corners, a secret
                                                    door's moves' ends (bench
                                                    builds only) */
void movers_door_go_down(edict_t *self);         /* (boss.qc's lightning_fire) */
void level_spawn(edict_t *e, const qw_ent *q);
void items_spawn(edict_t *e, const qw_ent *q);   /* items, flames, fireballs, boxes, traps */
void items_physics(edict_t *e);                  /* MOVETYPE_TOSS (the fireballs) */
void items_render(void);                         /* the models into the render list */
void ent_set_qim(edict_t *e, int qim);           /* setmodel: a map's own model (QIM_*),
                                                    or its streamed one */
int  ents_model_frame(int model);                /* the frame of the drawn edict with brush model
                                                    `model` (its textures' alternates), 0 none */

/* the live edicts with a model of the map's own (an item, a flame, a
   fireball: item_model >= 0), rebuilt with the other lists */
extern uint16_t ents_model_list[];
extern int      ents_num_models;

/* the live edicts with a streamed model or a monster's kind (masset >= 0,
   mdef: engine/monsters.c) */
extern uint16_t ents_streamed_list[];
extern int      ents_num_streamed;

/* the residency pass (engine/stream.c): every asset the edicts use, wanted
   by their distance from the player */
void ents_stream_wants(void);

/* a model about `org` (1/CLIP_SUB units) of radius `radius_vu` (vertex
   units) may be in the view's frustum (items.c) */
int  ent_in_view(const int32_t org[3], int radius_vu);

#ifdef Q1PSX_BENCH
int  ents_num_touch(void);             /* the touch list's length (the bench's) */
#ifdef Q1PSX_BENCH
int  ents_in_use(int *max);            /* the edicts in use, *max the pool's */
#endif
void ents_area_stats(void);            /* the area nodes' lists (the bench's) */
#endif

#endif
