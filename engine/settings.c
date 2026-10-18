#include <stdio.h>

#include "dlight.h"
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
}

void settings_apply(void)
{
#if Q1PSX_PADSCRIPT
	printf("settings: fid %d tex4 %d dither %d warp %d t %d budget %d hud %d dlights %d vol %d music %d\n",
	       settings.fidelity, settings.tex4, settings.dither, settings.warp, settings.t, settings.budget,
	       settings.hud, settings.dlights, settings.volume, settings.music);
#endif
	r_set_warp(settings.warp, warp_t_px[settings.t], warp_budgets[settings.budget]);
	vid_set_dither(settings.dither);
	hud_set_style(settings.hud);
	dl_enabled = settings.dlights;
	snd_set_volume(settings.volume);
	snd_set_cd_volume(settings.music);
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
