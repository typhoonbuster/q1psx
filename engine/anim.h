/*
 * q1psx — the animated textures (R_TextureAnimation): each chain's frame
 * of the moment copied into the display tile its faces draw with (a VRAM
 * to VRAM move at the head of the frame's display list, and the 4-bit
 * set's CLUT with it), a frame every 0.2 s of the game's time; the
 * alternate frames while a brush model drawing the chain has its frame set
 * (a pressed button, a toggled wall). formats/qworld.h qw_anim.
 */

#ifndef Q1PSX_ANIM_H
#define Q1PSX_ANIM_H

void anim_reset(void);          /* a map loaded: its display tiles hold frame 0 */
void anim_frame(int tick);      /* this frame's copies, before the scene */
void anim_invalidate(void);     /* every chain copied again at the next frame (the
                                   bench's dumps: pktview sees only the dumped frames) */

#endif
