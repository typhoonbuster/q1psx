/*
 * q1psx — the loading screens. See engine/loading.h.
 */

#include <stdint.h>
#include <string.h>

#include "cd.h"
#include "loading.h"
#include "mem.h"
#include "gdata.h"
#include "ui.h"
#include "vid.h"

/* SCR_DrawLoading's place: centred, above the status bar's 48 rows */
#define PLAQUE_W 144
#define PLAQUE_X ((VID_W - PLAQUE_W) / 2)
#define PLAQUE_Y ((VID_H - 48 - 24) / 2)
#define BAR_CELLS 18                       /* the plaque's width in glyphs */
#define BAR_W   (BAR_CELLS * 8)
#define BAR_X   PLAQUE_X
#define BAR_Y   (PLAQUE_Y + 24 + 4)
#define TITLE_Y (BAR_Y + 8 + 4)

static uint32_t base;          /* cd_meter when the estimate was made */
static uint32_t rest;          /* the sectors it gave the rest of the load */
static uint32_t at256;         /* the bar's place then, 1/256 px */
static int      shown;         /* the bar's width drawn, px */
static int      boot;          /* the boot's screen: redrawn whole and flipped */

/* the slider's track (conchars 128, 129 x 16, 130) */
static void bar_track(void)
{
	for (int i = 0; i < BAR_CELLS; i++)
		ui_bar_glyph(BAR_X + i * 8, BAR_Y, i == 0 ? 128 : i == BAR_CELLS - 1 ? 130 : 129, 8, 128);
}

/* the console's separator line (29, 30 ..., 31 at the end) over the
   track's first px pixels, half as bright again (shade 192: the track's
   own brown under it otherwise) */
static void bar_fill(int px)
{
	for (int x = 0; x < px; x += 8)
		ui_bar_glyph(BAR_X + x, BAR_Y, x == 0 ? 29 : x + 8 >= BAR_W ? 31 : 30, px - x < 8 ? px - x : 8, 192);
}

static uint32_t place256(uint32_t meter)
{
	uint32_t done = meter - base, left = BAR_W * 256 - at256;
	if (done >= rest)
		return BAR_W * 256;
	return at256 + left / 64 * done / rest * 64;   /* (36864 / 64 x a load's ~2000 sectors:
	                                                 32 bits, no 64-bit division) */
}

static void draw(int px)
{
	ui_begin();
	if (boot) {
		ui_conback();
		ui_loading_plaque(PLAQUE_X, PLAQUE_Y);
		bar_track();
		bar_fill(px);
		vid_flip();
	} else {
		bar_fill(px);
		vid_draw_over();
	}
}

/* the drive's meter (cd.h): the bar drawn again when it grows a pixel */
static void meter(uint32_t sectors)
{
	int px = (int) (place256(sectors) >> 8);
	if (px <= shown)
		return;
	shown = px;
	draw(px);
}

void loading_overlay(const char *title)
{
	ui_begin();
	ui_loading_plaque(PLAQUE_X, PLAQUE_Y);
	bar_track();
	if (title)
		ui_text_shaded((VID_W - 8 * (int) strlen(title)) / 2, TITLE_Y, title, 128, 128);   /* (bronze) */
	if (gdata.aux)
		ui_aux(QGA_DISC, VID_W - 24, 0);
}

static void start(uint32_t sectors)
{
	cd_meter = 0;
	base     = 0;
	rest     = sectors ? sectors : 1;
	at256    = 0;
	shown    = 0;
	cd_meter_hook = meter;
}

void loading_begin(uint32_t sectors)
{
	boot = 0;
	start(sectors);
}

void loading_rest(uint32_t sectors)
{
	if (!cd_meter_hook)
		return;
	at256 = place256(cd_meter);
	base  = cd_meter;
	rest  = sectors ? sectors : 1;
}

void loading_end(void)
{
	if (!cd_meter_hook)
		return;
	if (shown < BAR_W)
		draw(BAR_W);
	cd_meter_hook = 0;
	boot = 0;
	vid_resync();
}

void loading_boot(uint32_t sectors)
{
	boot = 1;
	start(sectors);
	draw(0);
}
