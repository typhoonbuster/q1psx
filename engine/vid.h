/*
 * q1psx — video: two VRAM framebuffers, an ordering table per frame, a linear
 * per-frame primitive buffer. Swapped during vblank. Everything that draws goes
 * through here.
 *
 * NTSC 320x240 for now; PAL / other resolutions come later.
 */

#pragma once

#include <stdint.h>

#define VID_W       320
#define VID_H       240
#define VID_OT_LEN  16     /* ordering-table buckets; bucket 0 = frontmost */

void vid_init(void);

/* Reserve `size` bytes in the current frame's primitive buffer and link the
   primitive into ordering-table bucket `z` (0 = drawn last / in front). */
void *vid_alloc(int z, int size);

/* Append text via the debug font into bucket `z`, at pixel x,y. */
void vid_text(int z, int x, int y, const char *s);

/* Finish the frame: wait for GPU + vblank, show it, start drawing the next. */
void vid_flip(void);

/* Background clear colour applied on the next flip (both buffers). */
void vid_set_bg(int r, int g, int b);
