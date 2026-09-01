/*
 * q1psx — cooked texture (QTX1) loading and VRAM upload.
 *
 * Phase 4.3: load the QTX blob, upload its shared CLUT, and push individual
 * 8bpp textures into a scratch VRAM page so they can be drawn. This is a
 * proof-of-life path; the real texture-cache management arrives with the BSP
 * renderer (Phase 5).
 */

#pragma once

#include <stdint.h>

typedef struct {
	uint16_t tpage;   /* getTPage() result (for POLY_FT4 / SPRT) */
	uint16_t clut;    /* getClut() result                        */
	int      w, h;    /* texel dimensions                        */
} vram_tex_t;

/* Point the loader at a QTX1 blob already in RAM and upload its 256-entry CLUT
   to VRAM. Returns the texture count, or -1 if the blob is not QTX1. */
int qtx_open(const void *blob, int len);

/* Upload texture `name` into the scratch VRAM page and fill *out.
   Returns 1 on success, 0 if not found / too big for the page. */
int qtx_upload(const char *name, vram_tex_t *out);
