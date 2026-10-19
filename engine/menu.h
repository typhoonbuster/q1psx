/*
 * q1psx — Quake's menu (menu.c), the release build's: the main menu, the
 * single player menu, the options and their graphics page, drawn over the
 * paused game (in single player Quake stops the world while a menu is up:
 * host.c "always pause in single player if in console or menus").
 *
 * The pad stands for Quake's keys: the d-pad the arrows, Cross ENTER,
 * Triangle and Circle ESCAPE, START the menu key (togglemenu). What has no
 * PlayStation counterpart (multiplayer, help, quit, loading and saving, the
 * console, the mouse) is shown greyed.
 */

#ifndef Q1PSX_MENU_H
#define Q1PSX_MENU_H

#include "pad.h"

/* what the menu asks of the game (engine/main.c) */
typedef struct {
	void (*new_game)(void);     /* SetNewParms and "map start" */
	int  (*in_game)(void);      /* sv.active: a game the player has begun */
	int  (*reload_due)(void);   /* the settings ask for other files than the
	                               level on screen was loaded from (levels,
	                               texture depth): they wait for a load */
	int  (*no_world)(void);     /* no map is loaded (Quake's forced console:
	                               the menu over its background, not over
	                               the faded game) */
} menu_hooks_t;

void menu_init(const menu_hooks_t *hooks);
int  menu_active(void);
void menu_open(void);                 /* M_Menu_Main_f */
void menu_key(const pad_t *p);        /* the frame's presses */
void menu_draw(void);                 /* over the frame, after the HUD (M_Draw) */

#endif
