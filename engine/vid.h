/*
 * q1psx — video core (architecture.md §4-§5).
 *
 * 320x240 NTSC, two framebuffers at (0,0) and (0,240). Each buffer owns an
 * ordering table and a 96 KB packet arena. The CPU builds frame N+1 into one
 * buffer while the GPU DMA draws frame N from the other; vid_flip() waits for
 * the GPU + vblank, shows the finished frame and kicks the next DMA.
 *
 * The renderer (asm) writes packets straight through `vid_frame`: it reads
 * `pkt` / `end`, writes packets, links them into `ot[...]` and stores the
 * advanced `pkt` back. Keep the struct layout in sync with engine/r_defs.inc.
 */

#ifndef Q1PSX_VID_H
#define Q1PSX_VID_H

#include <stdint.h>

#define VID_W 320
#define VID_H 240

/* Ordering table buckets: the highest index is drawn first. The world's
   bucket holds the whole scene in the painter's order the BSP walk gives
   (models and particles included, engine/rlist.h). */
#define VID_OT_LEN     8
#define OT_SKY         7
#define OT_WORLD       4
#define OT_OVERLAY     0

#define VID_ARENA_BYTES (96 * 1024)

typedef struct {
	uint32_t *ot;      /* +0  this frame's ordering table              */
	uint8_t  *pkt;     /* +4  next free byte in this frame's arena     */
	uint8_t  *end;     /* +8  end of the arena                         */
	uint32_t  dropped; /* +12 packets dropped for lack of arena space  */
} vid_frame_t;

typedef struct {
	uint32_t vbl;          /* vblanks the last frame took (1 = 60 fps)    */
	uint32_t sync_cycles;  /* CPU cycles spent waiting for the GPU        */
	uint32_t vsync_cycles; /* CPU cycles spent waiting for the vblank     */
	uint32_t pkt_bytes;    /* arena bytes the last frame used             */
	uint32_t dropped;      /* packets dropped last frame (arena full)     */
	int      gpu_busy;     /* GPU still drawing when the CPU was done     */
} vid_stats_t;

extern vid_frame_t vid_frame;
extern vid_stats_t vid_stats;

void  vid_init(void);
void  vid_set_bg(int r, int g, int b);
void  vid_set_dither(int on);                  /* GP0(E1h) bit 9, from the next frame */
void  vid_set_viewport(int rows);              /* the 3D view's draw area: 240 rows, or
                                                  192 above the original status bar;
                                                  from the next frame */
int   vid_viewport_rows(void);
void *vid_alloc(int bytes);                    /* NULL when the arena is full */
void  vid_link(int bucket, void *packet);
void  vid_text(int x, int y, const char *s);   /* debug font, overlay bucket */

/* The overlay chain: 2D packets drawn in the order they are added, over the
   whole screen (the chain opens with a DR_AREA when the viewport is
   smaller), before the overlay bucket's other packets (the debug text). */
void *vid_ovl_alloc(int bytes);                /* NULL when the arena is full */
void  vid_ovl_append(void *head, void *tail);  /* a ready chain (terminated) at its end */
void  vid_tint(int r, int g, int b);           /* a 50 % blend of a colour over the viewport */

/* The HUD's packets keep across frames (hud.c): the last VID_HUD_BYTES of
   each buffer's arena, past what vid_alloc hands out. A buffer's block is
   read by the GPU only while that buffer's display list draws, so the
   block of the buffer being built is free to write. */
#define VID_HUD_BYTES 1536
void *vid_hud_block(int previous);             /* this buffer's (0) or the other's (1) */

void  vid_flip(void);
void  vid_dump(int hash_only);                  /* display list -> stdout (P lines,
                                                   then its hash; or the hash only) */

#endif
