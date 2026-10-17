/*
 * q1psx — the HUD, two styles:
 *   HUD_ORIGINAL  Quake's status bar and inventory bar (sbar.c) in the
 *                 bottom 48 rows; the 3D view takes the 192 rows above
 *                 (Quake's viewsize 100);
 *   HUD_Q2PSX     a full-screen view with the numbers in the corners, the
 *                 way Quake II on the PlayStation does it.
 * Both draw with the pictures of gfx.wad (formats/qgame.h) into the overlay
 * chain (vid.h): one DR_TPAGE per texture page change, one SPRT per picture.
 */

#ifndef Q1PSX_HUD_H
#define Q1PSX_HUD_H

enum { HUD_ORIGINAL, HUD_Q2PSX, HUD_STYLES };

extern const char *const hud_style_name[HUD_STYLES];

void hud_set_style(int style);   /* sets the renderer's and the GPU's viewport */
int  hud_style(void);
void hud_draw(void);             /* this frame's HUD, from the player's state */

#endif
