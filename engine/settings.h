/*
 * q1psx — the player's settings (docs/porting-notes.md §52): the effects'
 * level (medium or high), the dithering, the HUD's style, the dynamic
 * lights, the volumes, the frame rate cap and the game's tick rate. The
 * debug build's map menu and the release build's Options / Graphics menu
 * (engine/menu.c) change them, at once (settings_apply).
 *
 * The rest is the build's (CMakeLists.txt), never a menu's:
 *   the textures   Q1PSX_TEXTURES, high or medium: the disc's one set, 4-bit
 *   the geometry   low
 *   the models     low (the guns rebuilt by hand: tools/qcook)
 *   the gun        Q1PSX_WEAPON, 3d or 2d (Q1PSX_WEAPON_2D)
 *   the draw       Q1PSX_DRAWDIST, 1536 or 1024 world units, the walls past
 *   distance       512 units flat-shaded (R_FLAT_FAR)
 *   the warp       dynamic, the threshold from 4 px up (SETTINGS_WARP_FLOOR)
 */

#ifndef Q1PSX_SETTINGS_H
#define Q1PSX_SETTINGS_H

#include <stdint.h>

#include "qlevels.h"

#ifndef Q1PSX_DRAWDIST
#define Q1PSX_DRAWDIST 1536      /* the build's draw distance, world units */
#endif
#ifndef Q1PSX_WEAPON_2D
#define Q1PSX_WEAPON_2D 0        /* 1: the gun's frames as pictures (formats/qgame.h) */
#endif
#define SETTINGS_WARP_FLOOR 4    /* px: the dynamic warp control's least threshold */

typedef struct {
	int level[QL_AXES];   /* the disc's textures, geometry and models (one
	                         level each: the build's); the effects' QL_MEDIUM
	                         or QL_HIGH */
	int dither;
	int hud;         /* HUD_*                                                */
	int dlights;     /* the dynamic lights                                   */
	int volume;      /* the sound effects', tenths (Quake's volume 0.7)      */
	int music;       /* the music's, tenths (bgmvolume 1)                    */
	int fps_cap;     /* fps_caps[] index: 60 (none), 45, 30                  */
	int tick;        /* tick_rates[] index: 60, 30 ticks a second            */
} settings_t;

extern settings_t settings;

extern const uint8_t fps_caps[];
extern const int num_fps_caps;
extern const uint8_t tick_rates[];
extern const int num_tick_rates;

/* each axis's levels the disc carries (bit n: level n; world_level_mask);
   the effects' are the engine's own */
extern int settings_level_mask[QL_AXES];

void settings_defaults(void);
void settings_apply(void);
int  settings_level_step(int axis, int level, int d);   /* the next level the disc has */

/* The rows both menus show (engine/menu.c's Graphics page, the debug map
   menu), in their order: */
enum { SET_EFFECTS, SET_DITHER, SET_HUD, SET_DLIGHTS, SET_FPS, SET_TICK, SET_ROWS };

void settings_step(int row, int d);   /* its value one step on (wrapping); not applied */
void settings_value(int row, char *buf, int size);   /* its value as the menus write it */

/* What the row's value does: line[0] its effect on the speed, line[1] on
   the picture ("+" a gain, "-" a cost, neither for the default), at most
   SETTINGS_HINT_CHARS characters each, "" when there is nothing to say. */
#define SETTINGS_HINT_CHARS 38
void settings_hint(int row, const char *line[2]);

#endif
