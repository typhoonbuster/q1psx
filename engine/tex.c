#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "qtx.h"     /* formats/qtx.h */
#include "tex.h"

/*
 * Scratch VRAM layout for phase 4.3 (framebuffers occupy x 0..319, y 0..479):
 *   texture page  at (512, 0)   — one 8bpp tpage, up to 256x256 texels
 *   shared CLUT   at (512, 300) — 256 x 1 words
 * The debug font lives at (960, 0), clear of both.
 */
#define TEX_VRAM_X   512
#define TEX_VRAM_Y   0
#define CLUT_VRAM_X  512
#define CLUT_VRAM_Y  300

static const qtx_header *qtx;
static const qtx_entry  *qtx_entries;

int qtx_open(const void *blob, int len)
{
	if (len < (int) sizeof(qtx_header) || memcmp(blob, QTX_MAGIC, 4) != 0)
		return -1;

	qtx         = (const qtx_header *) blob;
	qtx_entries = (const qtx_entry *) ((const uint8_t *) blob + sizeof(qtx_header));

	RECT clut_rect = { CLUT_VRAM_X, CLUT_VRAM_Y, 256, 1 };
	LoadImage(&clut_rect, (const uint32_t *) qtx->clut);
	DrawSync(0);

	printf("qtx_open: %u textures, CLUT -> VRAM (%d,%d)\n",
	       qtx->num_tex, CLUT_VRAM_X, CLUT_VRAM_Y);
	return (int) qtx->num_tex;
}

int qtx_upload(const char *name, vram_tex_t *out)
{
	if (!qtx)
		return 0;

	const qtx_entry *e = NULL;
	for (uint32_t i = 0; i < qtx->num_tex; i++) {
		if (strncmp(qtx_entries[i].name, name, QTX_NAMELEN) == 0) {
			e = &qtx_entries[i];
			break;
		}
	}
	if (!e) {
		printf("qtx_upload: '%s' not found\n", name);
		return 0;
	}
	if (e->width > 256 || e->height > 256) {
		printf("qtx_upload: '%s' %ux%u too big for one page\n",
		       name, e->width, e->height);
		return 0;
	}

	const uint8_t *pixels = (const uint8_t *) qtx + e->data_ofs;

	/* 8bpp: two texels per 16-bit VRAM word, so the RECT width is w/2 */
	RECT tex_rect = { TEX_VRAM_X, TEX_VRAM_Y, (e->width + 1) / 2, e->height };
	LoadImage(&tex_rect, (const uint32_t *) pixels);
	DrawSync(0);

	out->tpage = getTPage(1 /* 8bpp */, 0 /* no blending */, TEX_VRAM_X, TEX_VRAM_Y);
	out->clut  = getClut(CLUT_VRAM_X, CLUT_VRAM_Y);
	out->w     = e->width;
	out->h     = e->height;

	printf("qtx_upload: '%s' %dx%d -> VRAM (%d,%d) tpage=%04x clut=%04x\n",
	       name, out->w, out->h, TEX_VRAM_X, TEX_VRAM_Y, out->tpage, out->clut);
	return 1;
}
