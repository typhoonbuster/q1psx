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
int settings_level_mask[QL_AXES] = { QL_TEXTURE_MASK, QL_GEOMETRY_MASK, QL_MODEL_MASK, QL_EFFECT_MASK };

void settings_defaults(void)
{
	for (int a = 0; a < QL_AXES; a++) {
		settings.level[a] = QL_HIGH;     /* high, or the nearest the disc has */
		if (!(settings_level_mask[a] & (1 << QL_HIGH)))
			settings.level[a] = settings_level_step(a, QL_HIGH, 1);
	}
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
	printf("settings: tex %d geo %d models %d effects %d tex4 %d dither %d warp %d t %d budget %d hud %d "
	       "dlights %d vol %d music %d fps %d tick %d weapon %d drawdist %d flat %d\n",
	       settings.level[QL_TEXTURES], settings.level[QL_GEOMETRY], settings.level[QL_MODELS],
	       settings.level[QL_EFFECTS], settings.tex4, settings.dither, settings.warp, settings.t, settings.budget,
	       settings.hud, settings.dlights, settings.volume, settings.music, fps_caps[settings.fps_cap],
	       tick_rates[settings.tick], settings.viewmodel, draw_dists[settings.drawdist], settings.flat);
#endif
	game_set_effects(settings.level[QL_EFFECTS]);
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

int settings_level_step(int axis, int level, int d)
{
	for (int i = 0; i < QL_LEVELS; i++) {
		level = (level + QL_LEVELS + d) % QL_LEVELS;
		if (settings_level_mask[axis] & (1 << level))
			return level;
	}
	return level;
}

int settings_level_choice(int axis)
{
	int m = settings_level_mask[axis];
	return (m & (m - 1)) != 0;
}

/* ------------------------------------------------------------- the rows -- */

static int wrap(int v, int d, int n)
{
	return (v + n + d) % n;
}

/* the quality rows' axes, in the rows' order */
static const uint8_t row_axis[4] = { QL_TEXTURES, QL_GEOMETRY, QL_MODELS, QL_EFFECTS };

int settings_row_shown(int row)
{
	return row > SET_MODELS || settings_level_choice(row_axis[row]);
}

void settings_step(int row, int d)
{
	switch (row) {
	case SET_TEXTURES: case SET_GEOMETRY: case SET_MODELS: case SET_EFFECTS: {
		int a = row_axis[row];
		settings.level[a] = settings_level_step(a, settings.level[a], d);
		break;
	}
	case SET_DEPTH:    settings.tex4 ^= 1; break;
	case SET_DITHER:   settings.dither ^= 1; break;
	case SET_WARP:     settings.warp = wrap(settings.warp, d, R_WARP_MODES); break;
	case SET_T:        settings.t = wrap(settings.t, d, num_warp_t); break;
	case SET_BUDGET:   settings.budget = wrap(settings.budget, d, num_warp_budget); break;
	case SET_HUD:      settings.hud = wrap(settings.hud, d, HUD_STYLES); break;
	case SET_DLIGHTS:  settings.dlights ^= 1; break;
	case SET_WEAPON:   settings.viewmodel = wrap(settings.viewmodel, d, 3); break;
	case SET_DRAWDIST: settings.drawdist = wrap(settings.drawdist, d, num_draw_dists); break;
	case SET_FLAT:     settings.flat = wrap(settings.flat, d, R_FLAT_MODES); break;
	case SET_FPS:      settings.fps_cap = wrap(settings.fps_cap, d, num_fps_caps); break;
	default:           settings.tick = wrap(settings.tick, d, num_tick_rates); break;
	}
}

void settings_value(int row, char *buf, int size)
{
	switch (row) {
	case SET_TEXTURES: case SET_GEOMETRY: case SET_MODELS: case SET_EFFECTS:
		snprintf(buf, size, "%s", level_name[settings.level[row_axis[row]]]); break;
	case SET_DEPTH:    snprintf(buf, size, "%s", settings.tex4 ? "4-bit" : "8-bit"); break;
	case SET_DITHER:   snprintf(buf, size, "%s", settings.dither ? "on" : "off"); break;
	case SET_WARP:     snprintf(buf, size, "%s", warp_mode_name[settings.warp]); break;
	case SET_T:        snprintf(buf, size, "%d px", warp_t_px[settings.t]); break;
	case SET_BUDGET:   snprintf(buf, size, "%d tris", warp_budgets[settings.budget]); break;
	case SET_HUD:      snprintf(buf, size, "%s", hud_style_name[settings.hud]); break;
	case SET_DLIGHTS:  snprintf(buf, size, "%s", settings.dlights ? "on" : "off"); break;
	case SET_WEAPON:   snprintf(buf, size, "%s", viewmodel_name[settings.viewmodel]); break;
	case SET_DRAWDIST:
		if (draw_dists[settings.drawdist])
			snprintf(buf, size, "%d", draw_dists[settings.drawdist]);
		else
			snprintf(buf, size, "off");
		break;
	case SET_FLAT:     snprintf(buf, size, "%s", flat_name[settings.flat]); break;
	case SET_FPS:      snprintf(buf, size, "%d fps", fps_caps[settings.fps_cap]); break;
	default:           snprintf(buf, size, "%d Hz", tick_rates[settings.tick]); break;
	}
}

/* What each value gains and costs: the speed first, then the picture. The
   numbers are the port's measures (docs/porting-notes.md §34, §35, §37,
   §39): frame cycles on PCSX-Redux; the textures' GPU side (the texture
   cache) only shows on the console. */
static const char *const level_hint[4][QL_LEVELS][2] = {
	{ { "+ 1/16 the texels: lighter GPU, loads", "- blurry, quarter-size textures" },
	  { "+ 1/4 the texels: lighter GPU, loads",  "- softer, half-size textures" },
	  { "Quake's textures, up to 128 texels",    "lower: lighter on the GPU, blurrier" },
	  { "- more texels: GPU, VRAM, loading",     "+ doors, arches, signs at full size" } },
	{ { "+ about 2% fewer polygons",             "- hairline cracks may show" },
	  { "", "" },
	  { "Quake's faces, no cracks",              "ultra: finer light, more polygons" },
	  { "- about 40% more polygons, more RAM",   "+ finer light across walls, floors" } },
	{ { "+ about 9% fewer item triangles",       "- coarser items and projectiles" },
	  { "+ about 5% fewer item triangles",       "- slightly coarser items" },
	  { "Quake's own models",                    "lower: fewer item triangles" },
	  { "", "" } },
	{ { "+ a quarter of the particles",          "- sparse sparks, blood, explosions" },
	  { "+ half the particles",                  "- thinner sparks and explosions" },
	  { "The port's particles",                  "ultra: twice as many, lower: fewer" },
	  { "- twice the particles: slower fights",  "+ fuller explosions, sparks, trails" } },
};

void settings_hint(int row, const char *line[2])
{
	static const char *const none = "";
	const char *a = none, *b = none;
	switch (row) {
	case SET_TEXTURES: case SET_GEOMETRY: case SET_MODELS: case SET_EFFECTS:
		a = level_hint[row][settings.level[row_axis[row]]][0];
		b = level_hint[row][settings.level[row_axis[row]]][1];
		break;
	case SET_DEPTH:
		if (settings.tex4) { a = "+ half the VRAM, faster texel reads"; b = "- 16 colours per texture"; }
		else               { a = "256 colours per texture";             b = "4-bit: half the VRAM, faster reads"; }
		break;
	case SET_DITHER:
		if (settings.dither) { a = "Free: the GPU dithers as it draws"; b = "smooth light gradients"; }
		else                 { a = "no speed gained";                   b = "- banding in the light"; }
		break;
	case SET_WARP:
		if (settings.warp == R_WARP_DYNAMIC)     { a = "+ raises the threshold when late";   b = "- more warp in heavy views"; }
		else if (settings.warp == R_WARP_BUDGET) { a = "+ caps the triangles a frame draws"; b = "- more warp past the budget"; }
		else                                     { a = "One threshold, every frame";         b = "dynamic, budget: steadier frames"; }
		break;
	case SET_T:
		if (warp_t_px[settings.t] < 8)      { a = "- more polygons near walls";          b = "+ textures warp less up close"; }
		else if (warp_t_px[settings.t] > 8) { a = "+ fewer near polygons (16 px: -18%)"; b = "- textures warp more up close"; }
		else                                { a = "Polygons split past 8 px of warp";    b = "higher: faster (16 px: -18%)"; }
		break;
	case SET_BUDGET:
		a = "fewer triangles: steadier frames"; b = "- more warp when a view is over";
		break;
	case SET_HUD:
		if (settings.hud == HUD_ORIGINAL) { a = "+ 192-row 3D view: less to fill";     b = "Quake's status and inventory bars"; }
		else                              { a = "- full-screen 3D view: more to fill"; b = "Quake II PSX style, more to see"; }
		break;
	case SET_DLIGHTS:
		if (settings.dlights) { a = "- relights the walls near a flash"; b = "+ shots and explosions light up"; }
		else                  { a = "+ no relighting: faster in fights"; b = "- shots and explosions stay dark"; }
		break;
	case SET_WEAPON:
		if (settings.viewmodel == 2)      { a = "+ about 4% faster (up to 12%)";    b = "- the gun's frames as flat pictures"; }
		else if (settings.viewmodel == 1) { a = "Quake's gun: 5 to 20% of a frame"; b = "2D: about 4% faster, flat frames"; }
		else                              { a = "+ saves 5 to 20% of a frame";      b = "- no gun on screen"; }
		break;
	case SET_DRAWDIST:
		switch (draw_dists[settings.drawdist]) {
		case 0:    a = "Everything in view is drawn";        b = "lower: big rooms faster, black fog"; break;
		case 1024: a = "+ deep views faster";                b = "- black fog past 1024 units"; break;
		case 768:  a = "+ big rooms up to 27% faster";       b = "- black fog past 768 units"; break;
		default:   a = "+ little: few rooms reach that far"; b = "- far walls fade to black"; break;
		}
		break;
	case SET_FLAT:
		if (settings.flat == R_FLAT_ALL)      { a = "+ about 1.5% faster";        b = "- every wall in one shade"; }
		else if (settings.flat == R_FLAT_FAR) { a = "+ under 1%: far walls flat"; b = "- walls past 512 units in one shade"; }
		else                                  { a = "Walls lit per vertex";       b = "far, all: a little faster"; }
		break;
	case SET_FPS:
		if (settings.fps_cap) { a = "+ steadier pacing, time to spare"; b = "- fewer frames a second"; }
		else                  { a = "A frame every vblank it fits in";  b = "lower: steadier pacing"; }
		break;
	default:
		if (tick_rates[settings.tick] < 32)       { a = "+ less physics CPU (10 Hz: -64%)"; b = "- coarser motion and input"; }
		else if (tick_rates[settings.tick] > 32)  { a = "- physics every vblank: most CPU"; b = "+ smoothest motion and input"; }
		else                                      { a = "32 game ticks a second";           b = "60 Hz: smoother, more physics CPU"; }
		break;
	}
	line[0] = a;
	line[1] = b;
}
