/*
 * q1psx — the game: the player's state, weapons, projectiles and effects.
 *
 * A fixed-point, 60 Hz port of Quake's player side of the game code
 * (weapons.qc, player.qc's weapon animations, combat.qc's T_Damage /
 * T_RadiusDamage, sv_phys.c's missile and bounce physics, view.c's bob and
 * kicks, r_part.c's particles), native C as the permanent home of the
 * player's weapons (docs/porting-notes.md §22). No sounds yet (Phase 9), no
 * dynamic lights (by choice), no monsters: projectiles hit the world, and
 * explosions hurt the player (rocket jumps work).
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

/* the tick being run: its length in 60ths of a second (1 at the 60 Hz tick;
   the tick rate setting: 2 at 30 Hz, 3 at 20, 6 at 10, 1 or 2 at 32). The
   rates (physics, gravity, decays) act over it; the times (nextthink, die,
   attack_finished...) stay in 60ths. */
extern int game_dt;
/* ticks a second (the tick rate setting: 60, 32, 30, 20 or 10; engine/main.c
   runs them), and the camera's offset from the last tick's view while it is
   drawn between the last two (the view model follows it) */
extern int     game_tick_hz;
extern int32_t game_view_lerp[3];
extern int     game_draw_viewmodel;   /* the weapon drawn (r_drawviewmodel) */
extern int     game_view_cut;   /* set by a spawn outside a tick: the scheduler
                                   takes the view afresh, no lerp across it */

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
	int      fire, fire_was;   /* the fire button now / last tick            */
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
	int jump;                  /* edge                                        */
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
void game_view_pos(int32_t pos[3]);
void game_view_angles(int *yaw, int *pitch, int *roll);

/* between rl_begin() and r_frame(): the render list (projectiles, effects,
   particles) and the view model; after the frame: the world's lights back */
void game_render(void);
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
void game_powerup(uint32_t item);                  /* its 30 s */
void game_radius_damage_player(const int32_t org[3], int damage);
void game_explosion(const int32_t org[3]);         /* BecomeExplosion's */
void game_launch_spike(const int32_t org[3], const int32_t dir[3], int speed, int super);

void game_save_parms(void);
void game_new_parms(void);
void game_spawn_level(void);
void game_restart_level(void);

#endif
