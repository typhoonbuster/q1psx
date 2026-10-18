#include <stdint.h>
#include <psxgpu.h>

#include "gdata.h"
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

static void sprite(const qg_aux *a, int x, int y, int u, int v, int w, int h, int shade)
{
	set_tpage(a->tpage);
	SPRT *s = vid_ovl_alloc(sizeof *s);
	if (!s)
		return;
	setSprt(s);
	setXY0(s, x, y);
	setUV0(s, u, v);
	setWH(s, w, h);
	setRGB0(s, shade, shade, shade);
	s->clut = a->clut;
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

void ui_text_shaded(int x, int y, const char *s, int add, int shade)
{
	int x0 = x;
	for (; *s; s++) {
		if (*s == '\n') {
			x = x0;
			y += 8;
			continue;
		}
		ui_char(x, y, (uint8_t) *s + add, shade);
		x += 8;
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
