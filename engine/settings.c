#include <stdio.h>

#include "dlight.h"
#include "game.h"
#include "hud.h"
#include "render.h"
#include "settings.h"
#include "snd.h"
#include "vid.h"
#include "world.h"

const uint8_t fps_caps[]  = { 60, 45, 30 };
const int num_fps_caps    = (int) sizeof fps_caps;
/* the world's ticks a second (engine/ents.c ents_tick; the player's, the
   movers', the projectiles' are 60): 60, every edict each vblank; 30, each
   one every other vblank, half of them each vblank */
const uint8_t tick_rates[] = { 60, 30 };
const int num_tick_rates   = (int) sizeof tick_rates;

settings_t settings;
int settings_level_mask[QL_AXES] = { QL_TEXTURE_MASK, QL_GEOMETRY_MASK, QL_MODEL_MASK, QL_EFFECT_MASK };

void settings_defaults(void)
{
	for (int a = 0; a < QL_AXES; a++)           /* the disc's (the lowest it has) */
		settings.level[a] = settings_level_step(a, QL_LEVELS - 1, 1);
	settings.level[QL_EFFECTS] = QL_MEDIUM;
	settings.dither  = 1;
	settings.hud     = HUD_ORIGINAL;
	settings.dlights = 1;
	settings.volume  = 7;
	settings.music   = 10;
	settings.fps_cap = 0;                       /* 60: no cap */
	settings.tick    = 1;                       /* the world at 30 ticks a second */
}

void settings_apply(void)
{
#if Q1PSX_PADSCRIPT
	printf("settings: effects %d dither %d hud %d dlights %d vol %d music %d fps %d tick %d\n",
	       settings.level[QL_EFFECTS], settings.dither, settings.hud, settings.dlights, settings.volume,
	       settings.music, fps_caps[settings.fps_cap], tick_rates[settings.tick]);
#endif
	game_set_effects(settings.level[QL_EFFECTS]);
	r_set_warp(R_WARP_DYNAMIC, SETTINGS_WARP_FLOOR);
	vid_set_dither(settings.dither);
	hud_set_style(settings.hud);
	dl_enabled = settings.dlights;
	snd_set_volume(settings.volume);
	snd_set_cd_volume(settings.music);
	vid_set_fps_cap(fps_caps[settings.fps_cap]);
	game_world_hz = tick_rates[settings.tick];
	game_draw_viewmodel = Q1PSX_WEAPON_2D ? 2 : 1;
	r_set_options(Q1PSX_DRAWDIST, R_FLAT_FAR);
}

int settings_level_step(int axis, int level, int d)
{
	for (int i = 0; i < QL_LEVELS; i++) {
		level = (level + QL_LEVELS + d) % QL_LEVELS;
		if (settings_level_mask[axis] & (1 << level))
			return level;
	}
	return level;
}

/* ------------------------------------------------------------- the rows -- */

static int wrap(int v, int d, int n)
{
	return (v + n + d) % n;
}

void settings_step(int row, int d)
{
	switch (row) {
	case SET_EFFECTS:
		settings.level[QL_EFFECTS] = settings_level_step(QL_EFFECTS, settings.level[QL_EFFECTS], d);
		break;
	case SET_DITHER:   settings.dither ^= 1; break;
	case SET_HUD:      settings.hud = wrap(settings.hud, d, HUD_STYLES); break;
	case SET_DLIGHTS:  settings.dlights ^= 1; break;
	case SET_FPS:      settings.fps_cap = wrap(settings.fps_cap, d, num_fps_caps); break;
	default:           settings.tick = wrap(settings.tick, d, num_tick_rates); break;
	}
}

void settings_value(int row, char *buf, int size)
{
	switch (row) {
	case SET_EFFECTS:  snprintf(buf, size, "%s", level_name[settings.level[QL_EFFECTS]]); break;
	case SET_DITHER:   snprintf(buf, size, "%s", settings.dither ? "on" : "off"); break;
	case SET_HUD:      snprintf(buf, size, "%s", hud_style_name[settings.hud]); break;
	case SET_DLIGHTS:  snprintf(buf, size, "%s", settings.dlights ? "on" : "off"); break;
	case SET_FPS:      snprintf(buf, size, "%d fps", fps_caps[settings.fps_cap]); break;
	default:           snprintf(buf, size, "%d Hz", tick_rates[settings.tick]); break;
	}
}

/* What each value gains and costs: the speed first, then the picture. The
   numbers are the port's measures (docs/porting-notes.md §34, §35, §39):
   frame cycles on PCSX-Redux. */
void settings_hint(int row, const char *line[2])
{
	static const char *const none = "";
	const char *a = none, *b = none;
	switch (row) {
	case SET_EFFECTS:
		if (settings.level[QL_EFFECTS] == QL_HIGH) { a = "- under 1% slower in fights";       b = "+ fuller sparks and explosions"; }
		else                                       { a = "The port's lighter particles";      b = "high: fuller sparks, explosions"; }
		break;
	case SET_DITHER:
		if (settings.dither) { a = "Free: the GPU dithers as it draws"; b = "smooth light gradients"; }
		else                 { a = "no speed gained";                   b = "- banding in the light"; }
		break;
	case SET_HUD:
		if (settings.hud == HUD_ORIGINAL) { a = "+ 192-row 3D view: less to fill";     b = "Quake's status and inventory bars"; }
		else                              { a = "- full-screen 3D view: more to fill"; b = "Quake II PSX style, more to see"; }
		break;
	case SET_DLIGHTS:
		if (settings.dlights) { a = "- relights the walls near a flash"; b = "+ shots and explosions light up"; }
		else                  { a = "+ no relighting: faster in fights"; b = "- shots and explosions stay dark"; }
		break;
	case SET_FPS:
		if (settings.fps_cap) { a = "+ steadier pacing, time to spare"; b = "- fewer frames a second"; }
		else                  { a = "A frame every vblank it fits in";  b = "lower: steadier pacing"; }
		break;
	default:
		if (tick_rates[settings.tick] < 60) { a = "+ the world's physics halved";     b = "you, lifts and shots stay at 60"; }
		else                                { a = "- the world every vblank: more CPU"; b = "+ monsters' falls a bit smoother"; }
		break;
	}
	line[0] = a;
	line[1] = b;
}
