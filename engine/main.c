/*
 * q1psx — Quake for the PlayStation 1
 *
 * Phase 2.1: minimal boot with double buffering.
 *
 * Two framebuffers stacked vertically in VRAM. Each frame is drawn into the
 * back buffer while the front buffer is scanned out, then the two are swapped
 * during vblank (DrawSync + VSync before PutDispEnv). A solid background plus a
 * marker sliding across the screen make any tearing or flicker obvious in
 * motion. No Quake engine code yet.
 */

#include <stdint.h>
#include <stdio.h>
#include <psxgpu.h>

#define SCREEN_W 320
#define SCREEN_H 240

#define OT_LEN       8      /* ordering-table buckets (nothing 3D yet) */
#define PACKET_BYTES 8192   /* per-frame GPU primitive scratch        */

typedef struct {
	DISPENV  disp;
	DRAWENV  draw;
	uint32_t ot[OT_LEN];
	uint8_t  packets[PACKET_BYTES];
} Framebuffer;

static Framebuffer fb[2];
static int         back;          /* index of the buffer being drawn */
static uint8_t    *packet_top;    /* bump pointer into fb[back].packets */

static void video_init(void)
{
	ResetGraph(0);

	/* fb 0 at the top of VRAM, fb 1 directly below it */
	SetDefDrawEnv(&fb[0].draw, 0, 0,        SCREEN_W, SCREEN_H);
	SetDefDispEnv(&fb[0].disp, 0, 0,        SCREEN_W, SCREEN_H);
	SetDefDrawEnv(&fb[1].draw, 0, SCREEN_H, SCREEN_W, SCREEN_H);
	SetDefDispEnv(&fb[1].disp, 0, SCREEN_H, SCREEN_W, SCREEN_H);

	for (int i = 0; i < 2; i++) {
		setRGB0(&fb[i].draw, 40, 20, 80); /* background clear colour */
		fb[i].draw.isbg = 1;             /* clear the buffer each frame */
	}

	back       = 0;
	packet_top = fb[0].packets;
	ClearOTagR(fb[0].ot, OT_LEN);

	FntLoad(960, 0);
	SetDispMask(1);
}

static void video_flip(void)
{
	DrawSync(0);
	VSync(0);

	Framebuffer *drawn = &fb[back];
	Framebuffer *shown = &fb[back ^ 1];

	PutDispEnv(&shown->disp);
	DrawOTagEnv(&drawn->ot[OT_LEN - 1], &drawn->draw);

	back       = back ^ 1;
	packet_top = fb[back].packets;
	ClearOTagR(fb[back].ot, OT_LEN);
}

static void *packet_alloc(int bucket, int size)
{
	uint8_t *p = packet_top;

	addPrim(&fb[back].ot[bucket], p);
	packet_top += size;
	return p;
}

int main(void)
{
	video_init();

	int      marker_x  = 0;
	int      marker_dx = 2;
	uint32_t frame      = 0;

	for (;;) {
		marker_x += marker_dx;
		if (marker_x <= 0 || marker_x >= SCREEN_W - 24)
			marker_dx = -marker_dx;

		TILE *marker = packet_alloc(1, sizeof(TILE));
		setTile(marker);
		setXY0(marker, marker_x, SCREEN_H / 2 - 12);
		setWH(marker, 24, 24);
		setRGB0(marker, 230, 230, 40);

		char line[32];
		snprintf(line, sizeof(line), "q1psx  frame %u", frame);
		packet_top = FntSort(&fb[back].ot[0], packet_top, 8, 8, line);

		video_flip();
		frame++;
	}

	return 0;
}
