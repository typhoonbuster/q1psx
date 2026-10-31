/*
 * q1psx — Quake's menu (menu.c), the release build's: the main menu, the
 * single player menu, the options and their graphics page, drawn over the
 * paused game (in single player Quake stops the world while a menu is up:
 * host.c "always pause in single player if in console or menus").
 *
 * The pad stands for Quake's keys: the d-pad the arrows, Cross ENTER,
 * Triangle and Circle ESCAPE, START the menu key (togglemenu); the menus
 * take the pad alone, a mouse or not. What the port does not do
 * (multiplayer: single player only; help, quit, the console) is left out
 * (engine/menu.c). Loading and saving are the memory card's
 * (engine/card.h).
 */

#ifndef Q1PSX_MENU_H
#define Q1PSX_MENU_H

#include "pad.h"

/* what the menu asks of the game (engine/main.c) */
typedef struct {
	void (*begin_game)(int map, int skill);   /* SetNewParms, the skill, "map <map>" (a disc map) */
	void (*load_game)(int slot);     /* a memory card slot's level from its start (card_save) */
	void (*save_game)(int slot);     /* the level's start into a slot, onto the card */
	int  (*in_game)(void);           /* sv.active: a game the player has begun */
	int  (*can_save)(void);          /* a game, no intermission, the player alive */
	int  (*no_world)(void);          /* no map is loaded (Quake's forced console:
	                                    the menu over its background, not over
	                                    the faded game) */
} menu_hooks_t;

void menu_init(const menu_hooks_t *hooks);
int  menu_active(void);
void menu_open(void);                 /* M_Menu_Main_f */
void menu_key(const pad_t *p);        /* the frame's presses */
void menu_draw(void);                 /* over the frame, after the HUD (M_Draw) */

/* a map's name in Quake's menu ("e1m3": "The Necropolis"), NULL if none */
const char *menu_level_name(const char *map);

#endif
