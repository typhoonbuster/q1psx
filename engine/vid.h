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

/* Split version: reserve without linking, then link explicitly. `vid_link`
   prepends, so to run prims A,B,C in that order, link C, then B, then A.
   Used where several prims must stay adjacent (a DR_TWIN and its polys). */
void *vid_alloc_raw(int size);
void  vid_link(int z, void *prim);

/* Append text via the debug font into bucket `z`, at pixel x,y. */
void vid_text(int z, int x, int y, const char *s);

/* Finish the frame: wait for GPU + vblank, show it, start drawing the next. */
void vid_flip(void);

/* Background clear colour applied on the next flip (both buffers). */
void vid_set_bg(int r, int g, int b);

/* 1 if a vid_alloc since the last flip hit the per-frame buffer limit. */
int vid_prim_overflowed(void);

/* Rough frame timing, updated each vid_flip():
   - vid_frame_vbl():  vblanks the last frame took (1 == 60 fps, 2 == 30, ...).
   - vid_gpu_busy():   1 if the GPU was still drawing when the CPU came back
                       (i.e. we are GPU-bound).
   - vid_prim_used():  bytes of the per-frame primitive buffer used last frame. */
int vid_frame_vbl(void);
int vid_gpu_busy(void);
int vid_prim_used(void);

/* Raw vblank counter (increments in the vblank IRQ). Bracket a section with two
   reads to see roughly how many 16.7 ms vblank periods it spanned. */
uint32_t vid_vbl_now(void);

/* Free-running Hblank timer, 16-bit. `((t1 - t0) & 0xffff) * 64` ~= elapsed
   microseconds (wraps ~every 4 s). Fine-grained profiling stopwatch. */
uint32_t vid_ticks(void);
