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

#define GAME_HZ 60

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
	int      muzzleflash;      /* EF_MUZZLEFLASH set this tick (a dlight)    */
	int      dead, death_tick;
	int      viewheight;       /* 1/R_POS_SUB units above the box origin     */
	int      fire, fire_was;   /* the fire button now / last tick            */
	int      weapon_switch_tick;   /* when the weapon last changed (the HUD) */
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
void game_tick(const game_cmd_t *cmd);         /* one 60 Hz tick              */

/* the camera for this frame (1/R_POS_SUB units, binary degrees) */
void game_view(int32_t pos[3], int *yaw, int *pitch, int *roll);

/* before r_frame(): the render list (projectiles, effects, particles) and
   the view model; after it: rl_end() */
void game_render(void);
void game_render_done(void);

/* the current weapon's ammo count, and its ammo type (-1: none) */
int game_current_ammo(void);
int game_ammo_type(void);

/* a Quake-style random number: 0 .. 65535 */
uint32_t q_rand(void);

#endif
