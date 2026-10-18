/*
 * q1psx — the level's end (client.qc): trigger_changelevel, the
 * intermission (the camera at an info_intermission, the stats, the
 * episode's finale), the next map. The map change itself is main.c's
 * (between frames: the level arena is reset); the player's inventory
 * goes along (game.c game_save_parms / game_spawn_level).
 */

#ifndef Q1PSX_LEVEL_H
#define Q1PSX_LEVEL_H

#include <stdint.h>

#include "game.h"

extern int         level_intermission;      /* intermission_running: 0, 1 the stats, 2 the finale */
extern int32_t     level_completed_ticks;   /* the level's time when it ended */
extern const char *level_finale_text;       /* the episode's end, or NULL */
extern int32_t     level_finale_tick;       /* when it started coming out */
extern int32_t     level_start_tick;
extern int32_t     level_camera[3];         /* the intermission's eye, 1/8 units */
extern int         level_camera_angles[3];  /* pitch, yaw, roll, binary degrees */
extern int         game_serverflags;        /* the runes taken (items.qc item_sigil) */
extern int         game_skill;              /* 0 easy .. 3 nightmare (trigger_setskill) */

/* a new map started (the entities spawned) */
void level_begin(void);

/* the player's think while the intermission runs (PlayerPreThink's
   IntermissionThink): fire or jump goes on */
void level_think(const game_cmd_t *cmd);

/* main.c, after the frame's ticks: a map change asked for (its name
   copied out, lowercase as the QC has it); 1 once per request */
int  level_change_wanted(char name[16]);

int level_found_secrets(void);
int level_total_secrets(void);
int level_killed_monsters(void);
int level_total_monsters(void);

#endif
