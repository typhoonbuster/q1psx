/*
 * q1psx — the player's settings (docs/porting-notes.md §52): the HUD's
 * style, the anti-warping (§117), the dynamic lights, the volumes, the
 * frame rate cap and the game's tick rate. The debug build's map menu and
 * the release build's Options / Graphics menu (engine/menu.c) change them,
 * at once (settings_apply).
 *
 * The rest is every build's, never a menu's: the disc's one set of
 * textures, 4-bit, at high; the geometry and the models low (the guns
 * rebuilt by hand: tools/qcook), the gun its 3D model; the 3D view at
 * 320 x 240, the draw distance 1536 world units
 * (render.h R_DRAW_DIST), the walls past 512 units flat-shaded
 * (R_FLAT_FAR_UNITS), the effects' particles the port's high counts
 * (engine/game.c), the dithering on (engine/vid.c).
 */

#ifndef Q1PSX_SETTINGS_H
#define Q1PSX_SETTINGS_H

#include <stdint.h>

#define SETTINGS_WARP_FLOOR 4    /* px: the dynamic warp control's least threshold at the
                                    default anti-warping (the bench's T) */

typedef struct {
	int hud;         /* HUD_*                                                */
	int dlights;     /* the dynamic lights                                   */
	int volume;      /* the sound effects', tenths (Quake's volume 0.7)      */
	int music;       /* the music's, tenths (bgmvolume 1)                    */
	int fps_cap;     /* fps_caps[] index: 60 (none), 45, 30                  */
	int tick;        /* tick_rates[] index: the world's 60, 30 ticks a second */
	int warp;        /* the anti-warping: low, default, medium, high, ultra
	                    (the dynamic warp control's floor and cap, settings.c) */
	int brightness;  /* the Options' Brightness, 0-10: Quake's v_gamma 1 down
	                    to 0.5 on the CLUTs (engine/gamma.c)                 */
	int mouse;       /* the PlayStation Mouse played with when it answers (port 2
	                    polled: engine/pad.h pad_port2)                      */
	int mouse_speed; /* Quake's sensitivity x 2: 2 .. 22 (1 .. 11, 3 by default) */
	int invert_y;    /* the mouse's pitch negated (m_pitch < 0), and the right
	                    stick's: up looks up (engine/main.c player_cmd)      */
	int swap_mouse;  /* the buttons swapped: the right one fires (the mouse in the
	                    left hand)                                            */
	int vibration;   /* the DualShock's motors (engine/rumble.c)             */
} settings_t;

extern settings_t settings;

extern const uint8_t fps_caps[];
extern const int num_fps_caps;
extern const uint8_t tick_rates[];
extern const int num_tick_rates;

void settings_defaults(void);
void settings_apply(void);

/* the settings as the memory card keeps them (engine/card.h CARD_SETTINGS
   bytes), and back (each value checked: one out of its range stays as it
   is); not applied */
void settings_pack(uint8_t *b);
void settings_unpack(const uint8_t *b);

/* The rows both menus show (engine/menu.c's Graphics page, the debug map
   menu), in their order: */
enum { SET_HUD, SET_WARP, SET_DLIGHTS, SET_FPS, SET_TICK, SET_ROWS };

void settings_step(int row, int d);   /* its value one step on (wrapping); not applied */
void settings_value(int row, char *buf, int size);   /* its value as the menus write it */

/* What the row's value does: line[0] its effect on the speed, line[1] on
   the picture ("+" a gain, "-" a cost, neither for the default), at most
   SETTINGS_HINT_CHARS characters each, "" when there is nothing to say. */
#define SETTINGS_HINT_CHARS 38
void settings_hint(int row, const char *line[2]);

#endif
