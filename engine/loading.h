/*
 * q1psx — the loading screens (docs/porting-notes.md §120).
 *
 * A map's load is Quake's SCR_BeginLoadingPlaque: the last frame stays on
 * the screen (the intermission's stats, the world, the console's
 * background) with the loading plaque over it (SCR_DrawLoading: centred
 * above the status bar's 48 rows), the disc in the top right corner
 * (Draw_BeginDisc: the drive reading), and, the port's own, a bar under
 * the plaque (the console font's slider track, filled with its separator
 * line) and the level's name (Quake's menu names) under the bar. The frame
 * carrying them is built and flipped as any other (loading_overlay); the
 * load then draws the bar's progress over the frame on display
 * (vid_draw_over), from the drive's meter (engine/cd.h cd_meter): the
 * sectors read against the load's estimate, which each phase refines
 * (loading_rest: the bar keeps its place and fills the rest at the new
 * pace).
 *
 * The boot's screen is the console's background with the plaque and the
 * bar (from the background's boot strip until the game data is in VRAM:
 * formats/qmenu.h), redrawn and flipped as the boot reads.
 */

#ifndef Q1PSX_LOADING_H
#define Q1PSX_LOADING_H

#include <stdint.h>

/* over the frame being built: the plaque, the empty bar, `title` (NULL:
   none) and the disc; the caller flips the frame */
void loading_overlay(const char *title);

/* the load starts (the frame with the overlay flipped): the meter on, the
   whole load estimated at `sectors` */
void loading_begin(uint32_t sectors);

/* a phase of the load begins: what is left of it estimated anew */
void loading_rest(uint32_t sectors);

/* the load is over: the bar full, the meter off, the frame times taken
   from now (engine/vid.h vid_resync) */
void loading_end(void);

/* the boot: the console's background, the plaque and the bar, a frame
   flipped; the meter on, the boot's reads estimated at `sectors`, the bar
   redrawn as they come (loading_end ends it) */
void loading_boot(uint32_t sectors);

#endif
