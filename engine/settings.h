/*
 * q1psx — the player's settings: the renderer's (fidelity, texture depth,
 * dithering, the affine-warp control, the dynamic lights), the HUD's, the
 * volumes, the frame rate cap and the game's tick rate. The debug build's
 * map menu and the release build's Options / Graphics menu (engine/menu.c)
 * change them; the fidelity and the texture depth take effect at a map's
 * load, the rest at once (settings_apply).
 */

#ifndef Q1PSX_SETTINGS_H
#define Q1PSX_SETTINGS_H

#include <stdint.h>

typedef struct {
	int fidelity;    /* 0 low .. 3 ultra (FIDELITY_LEVELS), the next load's */
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

/* the fidelity levels the disc carries (bit n: level n; world_scan_maps) */
extern int settings_fid_mask;

void settings_defaults(void);
void settings_apply(void);          /* all but the fidelity and the texture depth */
int  settings_fid_step(int fid, int d);   /* the next level on the disc */

#endif
