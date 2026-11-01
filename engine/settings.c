#include <string.h>

#include "card.h"
#include "dlight.h"
#include "gamma.h"
#include "game.h"
#include "hud.h"
#include "mem.h"
#include "pad.h"
#include "q_fmt.h"
#include "render.h"
#include "rumble.h"
#include "settings.h"
#include "snd.h"
#include "vid.h"

const uint8_t fps_caps[]  = { 60, 45, 30 };
const int num_fps_caps    = (int) sizeof fps_caps;
/* the world's ticks a second (engine/ents.c ents_tick; the player's, the
   movers', the projectiles' are 60): 60, every edict each vblank; 30, each
   one every other vblank, half of them each vblank */
const uint8_t tick_rates[] = { 60, 30 };
const int num_tick_rates   = (int) sizeof tick_rates;

/* The anti-warping's levels (docs/porting-notes.md §117): the dynamic warp
   control's floor and cap, px (engine/render.h r_set_warp); default is the
   control as it was (§79.2). Low lets the textures warp more for speed,
   medium, high and ultra less at a cost. */
enum { WARP_LOW, WARP_DEFAULT, WARP_MEDIUM, WARP_HIGH, WARP_ULTRA, WARP_LEVELS };
static const uint8_t warp_levels[WARP_LEVELS][2] = {
	{ 16, 48 }, { SETTINGS_WARP_FLOOR, 32 }, { 4, 16 }, { 4, 8 }, { 1, 4 },
};
static const char *const warp_name[WARP_LEVELS] = { "low", "default", "medium", "high", "ultra" };

settings_t settings;

void settings_defaults(void)
{
	settings.hud     = HUD_ORIGINAL;
	settings.dlights = 1;
	settings.volume  = 7;
	settings.music   = 10;
	settings.fps_cap = 0;                       /* 60: no cap */
	settings.tick    = 1;                       /* the world at 30 ticks a second */
	settings.warp    = WARP_DEFAULT;
	settings.brightness = 0;                    /* Quake's v_gamma 1 */
	settings.mouse      = 0;
	settings.mouse_speed = 6;                   /* sensitivity 3 (default.cfg) */
	settings.invert_y   = 0;
	settings.swap_mouse = 0;
	settings.vibration  = 1;
}

/* the order the card keeps them in, and each one's values (0 .. max) */
static int *const packed[] = {
	&settings.hud, &settings.dlights, &settings.volume, &settings.music, &settings.fps_cap, &settings.tick,
	&settings.warp, &settings.brightness, &settings.mouse, &settings.mouse_speed, &settings.invert_y,
	&settings.swap_mouse, &settings.vibration,
};
#define PACKED (int) (sizeof packed / sizeof packed[0])

Q_COLD void settings_pack(uint8_t *b)
{
	memset(b, 0xff, CARD_SETTINGS);
	for (int i = 0; i < PACKED; i++)
		b[i] = (uint8_t) *packed[i];
}

Q_COLD void settings_unpack(const uint8_t *b)
{
	const int max[PACKED] = { HUD_STYLES - 1, 1, 10, 10, num_fps_caps - 1, num_tick_rates - 1, WARP_LEVELS - 1,
	                          GAMMA_LEVELS - 1, 1, 22, 1, 1, 1 };
	for (int i = 0; i < PACKED; i++)
		if (b[i] <= max[i])
			*packed[i] = b[i];
	if (settings.mouse_speed < 2)
		settings.mouse_speed = 2;
}

void settings_apply(void)
{
	r_set_warp(R_WARP_DYNAMIC, warp_levels[settings.warp][0], warp_levels[settings.warp][1]);
	hud_set_style(settings.hud);
	dl_enabled = settings.dlights;
	snd_set_volume(settings.volume);
	snd_set_cd_volume(settings.music);
	vid_set_fps_cap(fps_caps[settings.fps_cap]);
	game_world_hz = tick_rates[settings.tick];
	gamma_set(settings.brightness);
	pad_port2 = settings.mouse;
	if (!settings.vibration)
		rumble_stop();
}

/* ------------------------------------------------------------- the rows -- */

static int wrap(int v, int d, int n)
{
	return (v + n + d) % n;
}

void settings_step(int row, int d)
{
	switch (row) {
	case SET_HUD:      settings.hud = wrap(settings.hud, d, HUD_STYLES); break;
	case SET_WARP:     settings.warp = wrap(settings.warp, d, WARP_LEVELS); break;
	case SET_DLIGHTS:  settings.dlights ^= 1; break;
	case SET_FPS:      settings.fps_cap = wrap(settings.fps_cap, d, num_fps_caps); break;
	default:           settings.tick = wrap(settings.tick, d, num_tick_rates); break;
	}
}

void settings_value(int row, char *buf, int size)
{
	switch (row) {
	case SET_HUD:      q_snprintf(buf, size, "%s", hud_style_name[settings.hud]); break;
	case SET_WARP:     q_snprintf(buf, size, "%s", warp_name[settings.warp]); break;
	case SET_DLIGHTS:  q_snprintf(buf, size, "%s", settings.dlights ? "on" : "off"); break;
	case SET_FPS:      q_snprintf(buf, size, "%d fps", fps_caps[settings.fps_cap]); break;
	default:           q_snprintf(buf, size, "%d Hz", tick_rates[settings.tick]); break;
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
	case SET_HUD:
		if (settings.hud == HUD_ORIGINAL) { a = "+ 192-row 3D view: less to fill";     b = "Quake's status and inventory bars"; }
		else                              { a = "- full-screen 3D view: more to fill"; b = "Quake II PSX style, more to see"; }
		break;
	case SET_WARP: {                            /* (the effects alone, no pixel figures: §117) */
		static const char *const wh[WARP_LEVELS][2] = {
			{ "+ faster: fewer polygons split",       "- more texture warping up close" },
			{ "The port's balance of speed and warp", "low: faster; higher: less warping" },
			{ "- a little slower: more splits",       "+ less texture warping up close" },
			{ "- slower: more polygons split",        "+ much less texture warping" },
			{ "- slowest: the most polygons split",   "+ the least texture warping" },
		};
		a = wh[settings.warp][0];
		b = wh[settings.warp][1];
		break;
	}
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
