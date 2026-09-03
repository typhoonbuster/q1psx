#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "qtx.h"     /* formats/qtx.h */
#include "tex.h"

#define MAX_SLOTS 256

static const qtx_header *qtx;
static const qtx_entry  *entries;
static int               n_slots;
static tex_slot_t        slots[MAX_SLOTS];

static void make_slot(const qtx_entry *e, tex_slot_t *s)
{
	int vx = e->vram_x, vy = e->vram_y;
	int tw = e->width  ? e->width  : 2;
	int th = e->height ? e->height : 2;

	s->tpage = getTPage(1 /* 8bpp */, 0, vx, vy);
	s->clut  = getClut(QTX_CLUT_X, QTX_CLUT_Y);
	s->flags = e->flags;

	/* PS1 texture window: mask so U,V wrap mod (tw,th); offset by the cell's
	   position inside the tpage. All in 8-pixel units.
	     u_off (texels) = (vx & 63) * 2   (8bpp: 2 texels per VRAM word)
	     mask           = (256 - size) / 8 */
	int u_off = (vx & 0x3f) * 2;
	int v_off =  vy & 0xff;
	setRECT(&s->twin, u_off >> 3, v_off >> 3, (256 - tw) >> 3, (256 - th) >> 3);
}

int qtx_open(const void *blob, int len)
{
	if (len < (int) sizeof(qtx_header) || memcmp(blob, QTX_MAGIC, 4) != 0)
		return -1;

	qtx     = (const qtx_header *) blob;
	entries = (const qtx_entry *) ((const uint8_t *) blob + sizeof(qtx_header));
	n_slots = (int) qtx->num_tex;
	if (n_slots > MAX_SLOTS)
		n_slots = MAX_SLOTS;

	RECT clut_rect = { QTX_CLUT_X, QTX_CLUT_Y, 256, 1 };
	LoadImage(&clut_rect, (const uint32_t *) qtx->clut);

	for (int i = 0; i < n_slots; i++) {
		const qtx_entry *e = &entries[i];
		const uint8_t   *px = (const uint8_t *) blob + e->data_ofs;

		RECT r = { e->vram_x, e->vram_y, (e->width + 1) / 2, e->height };
		LoadImage(&r, (const uint32_t *) px);

		make_slot(e, &slots[i]);
	}
	DrawSync(0);

	printf("qtx_open: %u textures -> atlas, CLUT -> VRAM (%d,%d)\n",
	       qtx->num_tex, QTX_CLUT_X, QTX_CLUT_Y);

	/* the caller is free to release the blob now — everything the renderer
	   needs is in `slots[]` and VRAM */
	qtx     = 0;
	entries = 0;
	return n_slots;
}

int qtx_count(void)
{
	return n_slots;
}

int qtx_slot(int idx, tex_slot_t *out)
{
	if (idx < 0 || idx >= n_slots)
		return 0;
	*out = slots[idx];
	return 1;
}

const tex_slot_t *qtx_slot_ptr(int idx)
{
	if (idx < 0 || idx >= n_slots)
		return 0;
	return &slots[idx];
}
