#include <stdio.h>
#include <psxgpu.h>

#include "vid.h"
#include "loading.h"

#define BAR_X   40
#define BAR_Y   132
#define BAR_W   (VID_W - 2 * BAR_X)   /* 240 */
#define BAR_H   14
#define BORDER  2

static int done_steps;
static int total_steps = 1;

static void draw(const char *label)
{
	int filled = (BAR_W - 2 * BORDER) * done_steps / total_steps;
	int pct    = 100 * done_steps / total_steps;

	/* bar outline */
	TILE *frame = vid_alloc(2, sizeof(TILE));
	setTile(frame);
	setXY0(frame, BAR_X, BAR_Y);
	setWH(frame, BAR_W, BAR_H);
	setRGB0(frame, 90, 90, 100);

	/* bar trough */
	TILE *trough = vid_alloc(1, sizeof(TILE));
	setTile(trough);
	setXY0(trough, BAR_X + BORDER, BAR_Y + BORDER);
	setWH(trough, BAR_W - 2 * BORDER, BAR_H - 2 * BORDER);
	setRGB0(trough, 20, 20, 28);

	/* fill */
	if (filled > 0) {
		TILE *fill = vid_alloc(0, sizeof(TILE));
		setTile(fill);
		setXY0(fill, BAR_X + BORDER, BAR_Y + BORDER);
		setWH(fill, filled, BAR_H - 2 * BORDER);
		setRGB0(fill, 220, 170, 40);
	}

	char pctbuf[8];
	snprintf(pctbuf, sizeof pctbuf, "%d%%", pct);

	vid_text(0, VID_W / 2 - 40, BAR_Y - 40, "q1psx  loading");
	vid_text(0, BAR_X, BAR_Y + BAR_H + 8, label ? label : "");
	vid_text(0, VID_W - BAR_X - 24, BAR_Y - 12, pctbuf);

	vid_flip();
}

void load_begin(int total)
{
	total_steps = total > 0 ? total : 1;
	done_steps  = 0;
	vid_set_bg(16, 12, 24);
	draw("starting");
}

void load_step(const char *label)
{
	if (done_steps < total_steps)
		done_steps++;
	printf("[load %d/%d] %s\n", done_steps, total_steps, label ? label : "");
	draw(label);
}

void load_note(const char *label)
{
	draw(label);
}

void load_done(void)
{
	done_steps = total_steps;
	draw("ready");
}
