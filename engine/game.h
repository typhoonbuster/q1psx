/*
 * q1psx — the game: the player's state, weapons, projectiles and effects.
 *
 * A fixed-point, 60 Hz port of Quake's player side of the game code
 * (weapons.qc, player.qc's weapon animations, combat.qc's T_Damage /
 * T_RadiusDamage, sv_phys.c's missile and bounce physics, view.c's bob and
 * kicks, r_part.c's particles), native C as the permanent home of the
 * player's weapons (docs/porting-notes.md §22). The projectiles are the
 * player's, the traps' and the monsters' (an owner: its box passed, the
 * damage its); they hit the world, the brush models and the boxes (the
 * player's, the monsters' grown for a missile), explosions hurt everything
 * in reach.
 *
 * Units: positions 1/R_POS_SUB (1/8) world units, velocities world units
 * per second, angles binary degrees (4096 per turn), times in ticks.
 */

#ifndef Q1PSX_GAME_H
#define Q1PSX_GAME_H

#include <stdint.h>

#include "pmove.h"
#include "qworld.h"

#define GAME_HZ 60      /* the game's time unit: 60ths of a second (game_tick_count) */

/* the tick being run: its length in 60ths of a second. A tick a vblank:
   the player's part (the move, the weapons, the view), the movers', the
   projectiles' and the effects' are 1; the other edicts' (the monsters,
   what they throw, the items, the triggers' thinks) run at the world's
   rate: at 30 Hz, each one every other tick, over 2 (engine/ents.c
   ents_tick). The rates (physics, gravity, decays) act over it; the times
   (nextthink, die, attack_finished...) stay in 60ths. */
extern int game_dt;
/* the world's ticks a second (the tick rate setting: 60 or 30); the
   player's are 60 */
extern int     game_world_hz;
extern int     game_draw_viewmodel;   /* the weapon drawn (r_drawviewmodel): 0 none,
                                         1 the 3D gun, 2 the 2D one */
void game_ws_invalidate(void);           /* the 2D gun's slot filled again (a dump) */
void game_ws_sync(void);                 /* the 2D gun's packets asked for while it is the
                                            gun drawn, given back when not (a map's start,
                                            then each frame outside the menu) */

enum { W_AXE, W_SHOTGUN, W_SSHOTGUN, W_NAILGUN, W_SNAILGUN, W_GLAUNCHER, W_RLAUNCHER,
       W_LIGHTNING, W_COUNT };
enum { AMMO_SHELLS, AMMO_NAILS, AMMO_ROCKETS, AMMO_CELLS, AMMO_COUNT };

/* Quake's item bits (defs.qc) */
#define IT_SHOTGUN          1
#define IT_SUPER_SHOTGUN    2
#define IT_NAILGUN          4
#define IT_SUPER_NAILGUN    8
#define IT_GRENADE_LAUNCHER 16
#define IT_ROCKET_LAUNCHER  32
#define IT_LIGHTNING        64
#define IT_AXE              4096
#define IT_ARMOR1           8192
#define IT_ARMOR2           16384
#define IT_ARMOR3           32768
#define IT_KEY1             131072
#define IT_KEY2             262144
#define IT_INVISIBILITY     524288
#define IT_INVULNERABILITY  1048576
#define IT_SUIT             2097152
#define IT_QUAD             4194304
#define IT_SIGIL1           (1 << 28)

typedef struct {
	pmove_t  pm;               /* position, velocity, angles (physics)       */
	int      health;
	int      armor;            /* armorvalue                                 */
	int      armortype;        /* 0, 30, 60, 80: the percent absorbed        */
	uint32_t items;
	int      ammo[AMMO_COUNT];
	int      weapon;           /* W_*                                        */
	int      weaponframe;      /* the view model's frame                     */
	int      attack_finished;  /* tick                                       */
	int      anim, anim_next;  /* the weapon animation chain (game.c)        */
	int      punch;            /* punchangle pitch, binary degrees (< 0: up) */
	int      dmg_time, dmg_roll, dmg_pitch;   /* the damage kick            */
	int      pain_until;       /* tick: the HUD face is in pain until then   */
	int      pain_finished;    /* tick: no pain sound before (PainSound)     */
	int      t_width;          /* tick: the next lightning hit sound         */
	int      jump_flag;        /* this tick's starting z velocity (landing)  */
	int      air_finished;     /* tick: the air runs out (WaterMove)         */
	int      dmgtime;          /* tick: the next lava / slime burn           */
	int      dmg;              /* the drowning damage, 2 .. 15               */
	int      inwater;          /* FL_INWATER: the entry sound played         */
	int      swim_flag;        /* tick: the next swim sound                  */
	int      muzzleflash;      /* EF_MUZZLEFLASH set this tick (a dlight)    */
	int      dead, death_tick;
	int      viewheight;       /* 1/R_POS_SUB units above the box origin     */
	int32_t  step_oldz;        /* V_CalcRefdef's oldz: the view's z rising up a
	                              step, 1/R_POS_SUB units                    */
	int      dead_bounce;      /* dead: MOVETYPE_BOUNCE (a gib's head), else TOSS */
	int      fire, fire_was;   /* the fire button now / last tick            */
	int      jump_held;        /* the jump button last tick (a respawn's press) */
	int      weapon_switch_tick;   /* when the weapon last changed (the HUD) */
	int      teleport_time;    /* tick: a teleport's 0.7 s without backing up */
	int      fly_sound;        /* tick: the next trigger_push wind sound      */
	int      invisible_finished, invisible_time, invisible_sound;   /* the powerups' */
	int      invincible_finished, invincible_time, invincible_sound;  /* ticks (0 none) */
	int      super_damage_finished, super_time, super_sound;
	int      radsuit_finished, rad_time;
	int      bonus_pct;        /* the bonus flash, its percent x256 (V_UpdatePalette) */
	int      damage_pct;       /* the damage flash, x256, and its colour */
	uint8_t  damage_rgb[3];
	int      dmg_take, dmg_save;   /* this tick's damage, sent at its end (svc_damage) */
	int      dimlight;         /* EF_DIMLIGHT: the quad's, the pentagram's glow */
	int32_t  dmg_from[3];      /* its last inflictor's centre (the world's: 0)  */
} player_t;

typedef struct {
	int fwd, side;             /* -PM_AXIS .. PM_AXIS                        */
	int jump;                  /* held                                        */
	int walk;                  /* held                                        */
	int fire;                  /* held                                        */
	int weapon_next, weapon_prev;   /* edges: cycle                           */
	int weapon_group;          /* -1, or 0..3: the d-pad's direct choice      */
	int centre;                /* edge: level the view                        */
} game_cmd_t;

extern player_t pl;
extern int      game_tick_count;

void game_init(void);                          /* once at boot                */
void game_spawn(void);                         /* at the map's spawn, all given */
void game_tick(const game_cmd_t *cmd);         /* one tick, game_dt 60ths long */

/* after r_frame: V_SetContentsColor, the view's leaf's water / slime / lava
   over the view (vid_tint) */
void game_view_tint(void);

/* the camera for this frame (1/R_POS_SUB units, binary degrees): its
   position (the last tick's) and its angles (now) */
void game_view(int32_t pos[3], int *yaw, int *pitch, int *roll);

/* between rl_begin() and r_frame(): the render list (projectiles, effects,
   particles) and the view model; after the frame: the world's lights back */
void game_render(void);
void game_render_particles(void);
void game_render_done(void);

/* the current weapon's ammo count, and its ammo type (-1: none) */
int game_current_ammo(void);
int game_ammo_type(void);

/* a Quake-style random number: 0 .. 65535 */
uint32_t q_rand(void);

/* ---- the map's entities' calls (engine/ents.c, triggers.c, level.c) ---- */
void game_teleport(const int32_t org[3], const int16_t mangle[3]);   /* the player, there */
void game_teleport_splash(const int32_t org[3]);   /* R_TeleportSplash */
void game_spawn_bubble(const int32_t org[3]);      /* misc.qc make_bubbles' bubble */
int  game_quad_active(void);
int  game_intermission(void);
void game_damage_player(int damage, const int32_t inflictor[3]);
const char *game_finale_text(const char *map);     /* an episode's end, or NULL */

/* the level changes (client.qc): the inventory kept leaving a level
   (SetChangeParms), the player put in the new one with it
   (PutClientInServer, DecodeLevelParms; info_player_start2 back in START
   with a rune); a new game's (SetNewParms); the level started over after
   a death (its entities spawned again, the parms it was entered with) */
void game_bonus_flash(void);                       /* stuffcmd "bf" */
void game_give_weapon(int w);                      /* a weapon picked up: to it */
int  game_best_weapon(void);
void game_select_best(void);
void game_set_current_ammo(void);                  /* W_SetCurrentAmmo: out of a firing state */
void game_powerup(uint32_t item);                  /* its 30 s */
void game_radius_damage_player(const int32_t org[3], int damage);
void game_explosion(const int32_t org[3]);         /* BecomeExplosion's */
void game_tar_explosion(const int32_t org[3]);     /* TE_TAREXPLOSION + BecomeExplosion */
struct edict_s;
void game_launch_spike(struct edict_s *owner, const int32_t org[3], const int32_t dir[3], int speed, int super);
/* a monster's missile (the projectiles': its owner's box passed, the
   damage its), from org at vel: MM_GRENADE (OgreFireGrenade's), MM_ZOMGIB
   (ZombieFireGrenade's: s1 its hit sound, s2 its miss), MM_WIZSPIKE (s1
   TE_WIZSPIKE's sound), MM_LAVABALL, MM_LASER (the enforcer's LaunchLaser:
   s1 its stop sound), MM_KSPIKE (the hell knight's knightspike: s1
   TE_KNIGHTSPIKE's sound); masset its streamed model */
enum { MM_GRENADE, MM_ZOMGIB, MM_WIZSPIKE, MM_LAVABALL, MM_LASER, MM_KSPIKE };
void game_monster_missile(int kind, struct edict_s *owner, const int32_t org[3], const int32_t vel[3], int masset,
                          int s1, int s2);
/* the shalrath's ShalMissile: homing on its owner's enemy (ShalHome every
   0.2 s from `ticks` on), masset v_spike.mdl */
void game_vore_missile(struct edict_s *owner, const int32_t org[3], const int32_t vel[3], int masset, int ticks);

/* a shambler's lightning: TE_LIGHTNING1 (its beam, masset bolt.mdl) and
   LightningDamage from `from` along p1-p2 */
void game_monster_beam(struct edict_s *owner, const int32_t org[3], const int32_t end[3], int masset);
void game_lightning_damage(struct edict_s *from, const int32_t p1[3], const int32_t p2[3], int damage);

void game_lava_splash(const int32_t org[3]);   /* TE_LAVASPLASH */
void game_stream_wants(void);                   /* a running powerup's sounds */
#ifdef Q1PSX_BENCH
extern uint32_t game_ents_cycles, game_ents_max, game_ents_ticks;   /* ents_tick's cost */
#endif

/* R_RocketTrail's particles at org (one a tick), flying at vel */
enum { TR_ROCKET, TR_SMOKE, TR_BLOOD, TR_TRACER, TR_SLIGHT_BLOOD, TR_TRACER2, TR_VOOR };
void game_trail(const int32_t org[3], int type, const int32_t vel[3]);

/* R_RunParticleEffect at org, no velocity (TE_GUNSHOT: colour 0, 20;
   SpawnBlood: 73, the damage x 2) */
void game_particles(const int32_t org[3], int colour, int count);
/* EF_MUZZLEFLASH's light (CL_RelinkEntities) for an entity at org (1/8
   units) facing yaw (binary degrees), its dlight key */
void game_muzzle_flash(int key, const int32_t org[3], int yaw);

void game_save_parms(void);
void game_new_parms(void);
void game_spawn_level(void);
void game_restart_level(void);

#endif
