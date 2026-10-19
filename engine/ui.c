#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "cd.h"
#include "gdata.h"
#include "mem.h"
#include "qmenu.h"
#include "ui.h"
#include "vid.h"

static int cur_tpage = -1;

void ui_begin(void)
{
	cur_tpage = -1;
}

static void set_tpage(int tpage)
{
	if (tpage == cur_tpage)
		return;
	DR_TPAGE *t = vid_ovl_alloc(sizeof *t);
	if (!t)
		return;
	setDrawTPage(t, 0, 0, tpage);
	cur_tpage = tpage;
}

/* a SPRT, its words written whole (setSprt, setXY0, setUV0, setWH, setRGB0
   and the clut, byte for byte: the halfword and byte stores cost more) */
static void sprite(const qg_aux *a, int x, int y, int u, int v, int w, int h, int shade)
{
	set_tpage(a->tpage);
	uint32_t *s = vid_ovl_alloc(sizeof (SPRT));
	if (!s)
		return;
	s[1] = 0x64000000u | (uint32_t) shade * 0x010101u;
	s[2] = (uint32_t) y << 16 | ((uint32_t) x & 0xffffu);
	s[3] = (uint32_t) a->clut << 16 | (uint32_t) v << 8 | (uint32_t) u;
	s[4] = (uint32_t) h << 16 | ((uint32_t) w & 0xffffu);
}

void ui_aux_shaded(int id, int x, int y, int shade)
{
	if (!gdata.aux || id < 0 || id >= QGA_COUNT)
		return;
	const qg_aux *a = &gdata.aux[id];
	sprite(a, x, y, a->u, a->v, a->w, a->h, shade);
}

void ui_aux(int id, int x, int y)
{
	ui_aux_shaded(id, x, y, 128);
}

void ui_aux_rows(int id, int x, int y, int sy, int h, int shade)
{
	if (!gdata.aux || id < 0 || id >= QGA_COUNT)
		return;
	const qg_aux *a = &gdata.aux[id];
	if (sy < 0) { h += sy; y -= sy; sy = 0; }
	if (sy + h > a->h) h = a->h - sy;
	if (h > 0)
		sprite(a, x, y, a->u, a->v + sy, a->w, h, shade);
}

void ui_char(int x, int y, int c, int shade)
{
	c &= 255;
	if ((c & 127) == 32 || !gdata.aux)
		return;
	const qg_aux *a = &gdata.aux[c & 128 ? QGA_CONCHARS2 : QGA_CONCHARS];
	sprite(a, x, y, a->u + (c & 15) * 8, a->v + ((c >> 4) & 7) * 8, 8, 8, shade);
}

/* n characters of s from (x, y), 8 px apart: ui_char's packets, the same
   bytes, built in one run when they share a page (a tpage change: ui_char
   one by one) */
void ui_chars(int x, int y, const char *s, int n, int add, int shade)
{
	if (!gdata.aux || n <= 0)
		return;
	int count = 0, tp = -1;
	for (int j = 0; j < n; j++) {
		int c = ((uint8_t) s[j] + add) & 255;
		if ((c & 127) == 32)
			continue;
		int t = gdata.aux[c & 128 ? QGA_CONCHARS2 : QGA_CONCHARS].tpage;
		if (tp >= 0 && t != tp) {                 /* two pages: one by one */
			for (j = 0; j < n; j++, x += 8)
				ui_char(x, y, (uint8_t) s[j] + add, shade);
			return;
		}
		tp = t;
		count++;
	}
	if (!count)
		return;
	set_tpage(tp);
	uint32_t *p = vid_ovl_alloc_run(sizeof (SPRT), count);
	if (!p)
		return;
	uint32_t rgb = 0x64000000u | (uint32_t) shade * 0x010101u, yy = (uint32_t) y << 16;
	for (int j = 0; j < n; j++, x += 8) {
		int c = ((uint8_t) s[j] + add) & 255;
		if ((c & 127) == 32)
			continue;
		const qg_aux *a = &gdata.aux[c & 128 ? QGA_CONCHARS2 : QGA_CONCHARS];
		p[1] = rgb;
		p[2] = yy | ((uint32_t) x & 0xffffu);
		p[3] = (uint32_t) a->clut << 16 | (uint32_t) (a->v + ((c >> 4) & 7) * 8) << 8 |
		       (uint32_t) (a->u + (c & 15) * 8);
		p[4] = 8u << 16 | 8u;
		p += sizeof (SPRT) / 4;
	}
}

void ui_text_shaded(int x, int y, const char *s, int add, int shade)
{
	for (;;) {                                    /* a line at a time */
		int n = 0;
		while (s[n] && s[n] != '\n')
			n++;
		ui_chars(x, y, s, n, add, shade);
		if (!s[n])
			return;
		s += n + 1;
		y += 8;
	}
}

void ui_text(int x, int y, const char *s)
{
	ui_text_shaded(x, y, s, 0, 128);
}

void ui_text_alt(int x, int y, const char *s)
{
	ui_text_shaded(x, y, s, 128, 128);
}

void ui_fill(int x, int y, int w, int h, int r, int g, int b)
{
	TILE *t = vid_ovl_alloc(sizeof *t);
	if (!t)
		return;
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
}

/* Draw_FadeScreen: a semi-transparent black over everything (B - F, mode
   2: the screen at a quarter of its brightness... Quake's fade keeps about
   a fifth of it in a dither; B/2 + F/2 of black twice keeps a quarter) */
void ui_fade(void)
{
	for (int k = 0; k < 2; k++) {
		DR_TPAGE *tp = vid_ovl_alloc(sizeof *tp);
		if (!tp)
			return;
		setDrawTPage(tp, 0, 0, getTPage(0, 0, 0, 0));
		TILE *t = vid_ovl_alloc(sizeof *t);
		if (!t)
			return;
		setTile(t);
		setSemiTrans(t, 1);
		setXY0(t, 0, 0);
		setWH(t, VID_W, VID_H);
		setRGB0(t, 0, 0, 0);
	}
	cur_tpage = -1;
}

/* M_DrawTextBox: corners, edges and the middle in 8 x 8 / 16 x 8 pieces */
void ui_text_box(int x, int y, int width, int lines)
{
	int cx = x, cy = y;
	ui_aux(QGA_BOX_TL, cx, cy);
	for (int n = 0; n < lines; n++) { cy += 8; ui_aux(QGA_BOX_ML, cx, cy); }
	ui_aux(QGA_BOX_BL, cx, cy + 8);
	cx += 8;
	while (width > 0) {
		cy = y;
		ui_aux(QGA_BOX_TM, cx, cy);
		for (int n = 0; n < lines; n++) {
			cy += 8;
			ui_aux(n == 1 ? QGA_BOX_MM2 : QGA_BOX_MM, cx, cy);
		}
		ui_aux(QGA_BOX_BM, cx, cy + 8);
		width -= 2;
		cx += 16;
	}
	cy = y;
	ui_aux(QGA_BOX_TR, cx, cy);
	for (int n = 0; n < lines; n++) { cy += 8; ui_aux(QGA_BOX_MR, cx, cy); }
	ui_aux(QGA_BOX_BR, cx, cy + 8);
}

/* ------------------------------------------- the console's background -- */

static int conback_in;           /* in VRAM: no map's textures over it yet */

int ui_conback_load(void)
{
	conback_in = 0;
	mem_level_reset();
	uint32_t size;
	const uint8_t *f = cd_load("\\CONBACK.DAT;1", &size, NULL);
	const qcb_header *h = (const qcb_header *) f;
	if (!f || size < sizeof *h || memcmp(h->magic, QCB_MAGIC, 4) || h->w != 320 || h->h != 200 ||
	    h->ofs_clut + 512 > size || h->ofs_pixels + (uint32_t) h->w * h->h > size) {
		printf("ui: no console background on the disc\n");
		mem_level_reset();
		return 0;
	}
	RECT clut = { QCB_CLUT_X, QCB_CLUT_Y, 256, 1 };
	LoadImage(&clut, (const uint32_t *) (f + h->ofs_clut));
	RECT pix = { QCB_VRAM_X, QCB_VRAM_Y, (int16_t) (h->w / 2), (int16_t) h->h };
	LoadImage(&pix, (const uint32_t *) (f + h->ofs_pixels));   /* (64-byte units: no stray words) */
	DrawSync(0);
	mem_level_reset();
	conback_in = 1;
	return 1;
}

void ui_conback_forget(void)
{
	conback_in = 0;
}

/* Draw_ConsoleBackground (vid.height): its 200 rows stretched over the
   screen's 240 (Quake's v = y * 200 / conheight), the columns as they are;
   in three strips, a texture page being 256 texels wide and a UV at most
   255: texels 0-127, 128-255 and 256-319, each from a page whose first
   column is the strip's. Raw texels (no shading, so no dithering). (Quake
   also writes its version number into the picture's corner: not drawn.) */
void ui_conback(void)
{
	if (!conback_in)
		return;
	uint16_t clut = (uint16_t) getClut(QCB_CLUT_X, QCB_CLUT_Y);
	for (int s = 0; s < 3; s++) {
		int x0 = s * 128, x1 = s < 2 ? x0 + 128 : 320;
		POLY_FT4 *p = vid_ovl_alloc(sizeof *p);
		if (!p)
			return;
		setPolyFT4(p);
		setShadeTex(p, 1);
		setXY4(p, x0, 0, x1, 0, x0, VID_H, x1, VID_H);
		setUV4(p, 0, 0, x1 - x0, 0, 0, 200, x1 - x0, 200);
		p->clut  = clut;
		p->tpage = (uint16_t) getTPage(1, 0, QCB_VRAM_X + s * 64, QCB_VRAM_Y);
	}
	cur_tpage = -1;                       /* (the strips' pages are the GPU's now) */
}
