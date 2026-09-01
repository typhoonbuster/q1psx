#include <stdint.h>
#include <psxgpu.h>

#include "vid.h"

#define PRIM_BYTES 16384   /* per-frame primitive scratch */

typedef struct {
	DISPENV  disp;
	DRAWENV  draw;
	uint32_t ot[VID_OT_LEN];
	uint8_t  prims[PRIM_BYTES];
} vid_buffer_t;

static vid_buffer_t fb[2];
static int          back;
static uint8_t     *prim_top;
static uint8_t      bg_r = 40, bg_g = 20, bg_b = 80;

void vid_init(void)
{
	ResetGraph(0);

	SetDefDrawEnv(&fb[0].draw, 0, 0,      VID_W, VID_H);
	SetDefDispEnv(&fb[0].disp, 0, 0,      VID_W, VID_H);
	SetDefDrawEnv(&fb[1].draw, 0, VID_H,  VID_W, VID_H);
	SetDefDispEnv(&fb[1].disp, 0, VID_H,  VID_W, VID_H);

	for (int i = 0; i < 2; i++) {
		setRGB0(&fb[i].draw, bg_r, bg_g, bg_b);
		fb[i].draw.isbg = 1;
	}

	back     = 0;
	prim_top = fb[0].prims;
	ClearOTagR(fb[0].ot, VID_OT_LEN);

	FntLoad(960, 0);
	SetDispMask(1);
}

void vid_set_bg(int r, int g, int b)
{
	bg_r = (uint8_t) r;
	bg_g = (uint8_t) g;
	bg_b = (uint8_t) b;
	for (int i = 0; i < 2; i++)
		setRGB0(&fb[i].draw, bg_r, bg_g, bg_b);
}

void *vid_alloc(int z, int size)
{
	uint8_t *p = prim_top;
	addPrim(&fb[back].ot[z], p);
	prim_top += size;
	return p;
}

void vid_text(int z, int x, int y, const char *s)
{
	prim_top = FntSort(&fb[back].ot[z], prim_top, x, y, s);
}

void vid_flip(void)
{
	DrawSync(0);
	VSync(0);

	vid_buffer_t *drawn = &fb[back];
	vid_buffer_t *shown = &fb[back ^ 1];

	PutDispEnv(&shown->disp);
	DrawOTagEnv(&drawn->ot[VID_OT_LEN - 1], &drawn->draw);

	back     = back ^ 1;
	prim_top = fb[back].prims;
	ClearOTagR(fb[back].ot, VID_OT_LEN);
}
