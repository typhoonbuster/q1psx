#include <stdio.h>

#include "dlight.h"
#include "game.h"
#include "hud.h"
#include "render.h"
#include "settings.h"
#include "snd.h"
#include "vid.h"
#include "world.h"

const uint8_t  warp_t_px[]    = { 1, 2, 4, 6, 8, 10, 12, 14, 16 };
const uint16_t warp_budgets[] = { 1000, 1500, 2000, 2500, 3000, 4000 };
const char *const warp_mode_name[R_WARP_MODES] = { "fixed", "dynamic", "budget" };
const int num_warp_t      = (int) sizeof warp_t_px;
const int num_warp_budget = (int) (sizeof warp_budgets / sizeof warp_budgets[0]);
const uint8_t fps_caps[]  = { 60, 45, 30, 15 };
const int num_fps_caps    = (int) sizeof fps_caps;
/* the game's ticks a second (engine/main.c player_update): 60 a tick per
   vblank; fewer ticks, less work a second (the physics' traces, the thinks,
   the touches), the camera drawn between two ticks */
const uint8_t tick_rates[] = { 10, 20, 30, 32, 60 };
const int num_tick_rates   = (int) sizeof tick_rates;
/* the graphics options that change the picture for speed (render.h
   r_set_options), off by default: the draw distance (the fog black at
   it), the flat shading */
const uint16_t draw_dists[]  = { 0, 2048, 1536, 1024, 768 };
const int num_draw_dists     = (int) (sizeof draw_dists / sizeof draw_dists[0]);
const char *const flat_name[R_FLAT_MODES] = { "off", "far", "all" };
/* the gun: none, the model, or its frames as pictures (formats/qgame.h:
   a quad instead of its triangles) */
const char *const viewmodel_name[3] = { "off", "3D", "2D" };

settings_t settings;
int settings_fid_mask = (1 << FIDELITY_LEVELS) - 1;

void settings_defaults(void)
{
	settings.fidelity = 2;               /* high, or the nearest the disc has */
	if (!(settings_fid_mask & (1 << settings.fidelity)))
		settings.fidelity = settings_fid_step(settings.fidelity, 1);
	settings.tex4    = 0;                /* 8-bit textures */
	settings.dither  = 1;
	settings.warp    = R_WARP_FIXED;
	settings.t       = 4;                /* 8 px */
	settings.budget  = 2;                /* 2000 triangles */
	settings.hud     = HUD_ORIGINAL;
	settings.dlights = 1;
	settings.volume  = 7;
	settings.music   = 10;
	settings.fps_cap = 0;                /* 60: no cap */
	settings.tick    = 3;                /* 32 ticks a second */
	settings.viewmodel = 1;
	settings.drawdist  = 0;              /* off */
	settings.flat      = R_FLAT_OFF;
}

void settings_apply(void)
{
#if Q1PSX_PADSCRIPT
	printf("settings: fid %d tex4 %d dither %d warp %d t %d budget %d hud %d dlights %d vol %d music %d "
	       "fps %d tick %d weapon %d drawdist %d flat %d\n",
	       settings.fidelity, settings.tex4, settings.dither, settings.warp, settings.t, settings.budget,
	       settings.hud, settings.dlights, settings.volume, settings.music, fps_caps[settings.fps_cap],
	       tick_rates[settings.tick], settings.viewmodel, draw_dists[settings.drawdist], settings.flat);
#endif
	r_set_warp(settings.warp, warp_t_px[settings.t], warp_budgets[settings.budget]);
	vid_set_dither(settings.dither);
	hud_set_style(settings.hud);
	dl_enabled = settings.dlights;
	snd_set_volume(settings.volume);
	snd_set_cd_volume(settings.music);
	vid_set_fps_cap(fps_caps[settings.fps_cap]);
	game_tick_hz = tick_rates[settings.tick];
	game_draw_viewmodel = settings.viewmodel;
	r_set_options(draw_dists[settings.drawdist], settings.flat);
}

int settings_fid_step(int fid, int d)
{
	for (int i = 0; i < FIDELITY_LEVELS; i++) {
		fid = (fid + FIDELITY_LEVELS + d) % FIDELITY_LEVELS;
		if (settings_fid_mask & (1 << fid))
			return fid;
	}
	return fid;
}
