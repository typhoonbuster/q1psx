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
void hud_invalidate(void);       /* something drew over the bars' rows: every part
                                    drawn again into both framebuffers */
int  hud_style(void);
void hud_draw(void);             /* this frame's HUD, from the player's state;
                                    the intermission's overlay instead during it,
                                    the centerprint over it */

/* SCR_CenterPrint: a message centred on the screen for 2 s (lines split
   at '\n') */
void hud_centerprint(const char *s);
void hud_centerprint_clear(void);

/* Con_Printf's lines in the notify area (Con_DrawNotify): the last 4 at the
   top of the screen, 3 s each (sprint: "You got the Shotgun") */
void hud_notify(const char *s);

/* a level's start (the client's state cleared: every item counts as just
   picked up, the inventory's weapons flash; no message left) */
void hud_level_start(void);

#endif
