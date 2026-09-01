/*
 * q1psx — Quake for the PlayStation 1
 *
 * Phase 2.1: minimal boot with double buffering.
 * Phase 2.2: read controller port 1 and show its state on screen.
 *
 * Two framebuffers stacked in VRAM, swapped during vblank. A marker slides
 * across a solid background (steerable with the d-pad once a pad is plugged
 * in), and a debug overlay prints the raw button word plus the pressed keys.
 * No Quake engine code yet.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>
#include <psxpad.h>

#include "pad.h"

#define SCREEN_W 320
#define SCREEN_H 240

#define OT_LEN       8
#define PACKET_BYTES 8192

typedef struct {
	DISPENV  disp;
	DRAWENV  draw;
	uint32_t ot[OT_LEN];
	uint8_t  packets[PACKET_BYTES];
} Framebuffer;

static Framebuffer fb[2];
static int         back;
static uint8_t    *packet_top;

static void video_init(void)
{
	ResetGraph(0);

	SetDefDrawEnv(&fb[0].draw, 0, 0,        SCREEN_W, SCREEN_H);
	SetDefDispEnv(&fb[0].disp, 0, 0,        SCREEN_W, SCREEN_H);
	SetDefDrawEnv(&fb[1].draw, 0, SCREEN_H, SCREEN_W, SCREEN_H);
	SetDefDispEnv(&fb[1].disp, 0, SCREEN_H, SCREEN_W, SCREEN_H);

	for (int i = 0; i < 2; i++) {
		setRGB0(&fb[i].draw, 40, 20, 80);
		fb[i].draw.isbg = 1;
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

/* Append " NAME" to buf for each held button, for the debug overlay. */
static void describe_buttons(char *buf, int cap, uint16_t held)
{
	static const struct { uint16_t mask; const char *name; } BTN[] = {
		{ PAD_UP, "UP" },       { PAD_DOWN, "DOWN" },
		{ PAD_LEFT, "LEFT" },   { PAD_RIGHT, "RIGHT" },
		{ PAD_TRIANGLE, "TRI" },{ PAD_CIRCLE, "CIR" },
		{ PAD_CROSS, "CRO" },   { PAD_SQUARE, "SQR" },
		{ PAD_L1, "L1" },       { PAD_R1, "R1" },
		{ PAD_L2, "L2" },       { PAD_R2, "R2" },
		{ PAD_SELECT, "SEL" },  { PAD_START, "STA" },
	};

	buf[0] = 0;
	for (unsigned i = 0; i < sizeof(BTN) / sizeof(BTN[0]); i++) {
		if (held & BTN[i].mask) {
			int len = (int) strlen(buf);
			snprintf(buf + len, cap - len, " %s", BTN[i].name);
		}
	}
	if (!buf[0])
		snprintf(buf, cap, " (none)");
}

int main(void)
{
	video_init();
	pad_init();

	int      marker_x  = SCREEN_W / 2 - 12;
	int      marker_dx = 2;
	uint32_t frame     = 0;

	PadState pad;
	char     held_str[96];
	char     overlay[256];

	for (;;) {
		pad_read(&pad);

		/* d-pad steers the marker; otherwise it bounces on its own */
		if (pad.buttons & PAD_LEFT)
			marker_x -= 3;
		else if (pad.buttons & PAD_RIGHT)
			marker_x += 3;
		else
			marker_x += marker_dx;

		if (marker_x <= 0) {
			marker_x = 0;
			marker_dx = +2;
		} else if (marker_x >= SCREEN_W - 24) {
			marker_x = SCREEN_W - 24;
			marker_dx = -2;
		}

		TILE *marker = packet_alloc(1, sizeof(TILE));
		setTile(marker);
		setXY0(marker, marker_x, SCREEN_H / 2 - 12);
		setWH(marker, 24, 24);
		setRGB0(marker, 230, 230, 40);

		describe_buttons(held_str, sizeof(held_str), pad.buttons);
		snprintf(overlay, sizeof(overlay),
			"q1psx  frame %u\n"
			"\n"
			"pad1: %s   type=0x%X\n"
			"btn word: 0x%04X\n"
			"held:%s",
			frame,
			pad.connected ? "connected" : "no device",
			pad.type,
			pad.buttons,
			held_str);

		packet_top = FntSort(&fb[back].ot[0], packet_top, 8, 8, overlay);

		video_flip();
		frame++;
	}

	return 0;
}
