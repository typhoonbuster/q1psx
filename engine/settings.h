/*
 * q1psx — the player's settings: the four quality levels (textures, map
 * geometry, models, effects: formats/qlevels.h), the renderer's (texture
 * depth, dithering, the affine-warp control, the dynamic lights, the draw
 * distance, the flat shading), the HUD's, the volumes, the frame rate cap
 * and the game's tick rate. The debug build's map menu and the release
 * build's Options / Graphics menu (engine/menu.c) change them; the
 * textures, the geometry, the models and the texture depth take effect at
 * a map's load, the rest at once (settings_apply).
 */

#ifndef Q1PSX_SETTINGS_H
#define Q1PSX_SETTINGS_H

#include <stdint.h>

#include "qlevels.h"

typedef struct {
	int level[QL_AXES];   /* QL_LOW .. QL_ULTRA per axis (QL_TEXTURES ..
	                         QL_EFFECTS): the next load's but the effects' */
	int tex4;        /* 4-bit textures, the next load's                      */
	int dither;
	int warp;        /* R_WARP_*                                             */
	int t;           /* warp_t_px[] index: the threshold (fixed), the floor
	                    (dynamic, budget)                                    */
	int budget;      /* warp_budgets[] index (budget)                        */
	int hud;         /* HUD_*                                                */
	int dlights;     /* the dynamic lights                                   */
	int volume;      /* the sound effects', tenths (Quake's volume 0.7)      */
	int music;       /* the CD music's, tenths (bgmvolume 1)                 */
	int fps_cap;     /* fps_caps[] index: 60 (none), 45, 30, 15              */
	int tick;        /* tick_rates[] index: the game's ticks a second        */
	int viewmodel;   /* the weapon drawn (Quake's r_drawviewmodel)           */
	int drawdist;    /* draw_dists[] index: the draw distance (0: off)       */
	int flat;        /* R_FLAT_*: the flat shading                           */
} settings_t;

extern settings_t settings;

extern const uint8_t  warp_t_px[];
extern const uint16_t warp_budgets[];
extern const char *const warp_mode_name[];
extern const int num_warp_t, num_warp_budget;
extern const uint8_t fps_caps[];
extern const int num_fps_caps;
extern const uint8_t tick_rates[];
extern const int num_tick_rates;
extern const uint16_t draw_dists[];       /* world units, 0: off */
extern const int num_draw_dists;
extern const char *const flat_name[];

/* each axis's levels the disc carries (bit n: level n; world_level_mask);
   the effects' are the engine's own */
extern int settings_level_mask[QL_AXES];

void settings_defaults(void);
void settings_apply(void);          /* all but what waits for a load */
int  settings_level_step(int axis, int level, int d);   /* the next level the disc has */
int  settings_level_choice(int axis);   /* the disc has more than one level of it */

/* The rows both menus show (engine/menu.c's Graphics page, the debug map
   menu), in their order: */
enum { SET_TEXTURES, SET_GEOMETRY, SET_MODELS, SET_EFFECTS, SET_DEPTH, SET_DITHER, SET_WARP, SET_T,
       SET_BUDGET, SET_HUD, SET_DLIGHTS, SET_WEAPON, SET_DRAWDIST, SET_FLAT, SET_FPS, SET_TICK,
       SET_ROWS };

int  settings_row_shown(int row);     /* a quality row: only when the disc has a choice */
void settings_step(int row, int d);   /* its value one step on (wrapping); not applied */
void settings_value(int row, char *buf, int size);   /* its value as the menus write it */

/* What the row's value does: line[0] its effect on the speed, line[1] on
   the picture ("+" a gain, "-" a cost, neither for the default), at most
   SETTINGS_HINT_CHARS characters each, "" when there is nothing to say. */
#define SETTINGS_HINT_CHARS 38
void settings_hint(int row, const char *line[2]);

#endif
