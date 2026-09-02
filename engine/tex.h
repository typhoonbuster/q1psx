/*
 * q1psx — cooked texture pack (QTX2): load, upload the whole atlas + shared
 * CLUT to VRAM, and hand out per-texture GPU state (tpage / clut / texture
 * window) for the surface renderer.
 */

#pragma once

#include <stdint.h>
#include <psxgpu.h>

typedef struct {
	uint16_t tpage;   /* getTPage() */
	uint16_t clut;    /* getClut()  */
	RECT     twin;    /* setTexWindow() — tiles the 64x64 cell */
	uint8_t  flags;   /* QTX_F_* */
} tex_slot_t;

/* Parse a QTX2 blob in RAM, upload every texture to its atlas slot and the
   CLUT to VRAM. Returns the texture count, or -1. */
int qtx_open(const void *blob, int len);

int  qtx_count(void);
int  qtx_slot(int idx, tex_slot_t *out);   /* 1 on success */
