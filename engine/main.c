/*
 * q1psx — Quake for the PlayStation 1
 *
 * Phase 2.1: minimal boot with double buffering.
 * Phase 2.2: read controller port 1 and show its state on screen.
 * Phase 2.3: load a test file off the CD and show it worked.
 *
 * Two framebuffers stacked in VRAM, swapped during vblank. A marker slides
 * across a solid background (steerable with the d-pad), and a debug overlay
 * prints the pad state plus the result of reading HELLO.TXT from the disc.
 * No Quake engine code yet.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psxgpu.h>
#include <psxpad.h>

#include "pad.h"
#include "cd.h"
#include "pak.h"
#include "progs.h"
#include "sys.h"
#include "zone.h"
#include "qcommon.h"
#include "selftest.h"

/*
 * Main Quake memory block. Retail PS1 has ~1.94 MB of APP_RAM; PSn00bSDK's
 * startup hands almost all of it to malloc. We take one contiguous slice here
 * and let Hunk/Zone manage it, leaving the rest for the stack and psxcd's
 * internal allocations. Tunable — revisit in phase 11 with real level data.
 */
#define HUNK_SIZE  (1536 * 1024)

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

/* one CD sector, word-aligned for CdRead() */
static uint32_t cd_buf[512];

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

/* Copy the first line of a text blob into dst, replacing non-printables. */
static void first_line(char *dst, int cap, const char *src, int src_len)
{
	int n = 0;
	for (; n < src_len && n < cap - 1 && src[n] != '\n' && src[n] != '\r'; n++)
		dst[n] = (src[n] >= 32 && src[n] < 127) ? src[n] : '.';
	dst[n] = 0;
}

int main(void)
{
	video_init();
	pad_init();
	cd_init();

	/* --- phase 2.4: bring up the Hunk/Zone allocator --- */
	void *hunk = malloc(HUNK_SIZE);
	if (!hunk)
		Sys_Error("no RAM for a %d-byte hunk", HUNK_SIZE);
	Memory_Init(hunk, HUNK_SIZE);

	/* --- phases 2.4 + 3.1: exercise the ported foundations --- */
	int st_fail = run_selftests();
	Mem_Report();

	/* --- phase 2.3: read a file off the disc, once, at boot --- */
	int      cd_size = cd_load("\\HELLO.TXT;1", cd_buf, sizeof(cd_buf));
	uint32_t cd_sum  = 0;
	char     cd_line[40];
	if (cd_size > 0) {
		const uint8_t *bytes = (const uint8_t *) cd_buf;
		for (int i = 0; i < cd_size; i++)
			cd_sum += bytes[i];
		first_line(cd_line, sizeof(cd_line), (const char *) cd_buf, cd_size);
	} else {
		snprintf(cd_line, sizeof(cd_line), "<read failed>");
	}

	/* also log to serial so headless boots can be checked without a screen */
	printf("q1psx: cd_load HELLO.TXT -> size=%d sum=0x%X line=\"%s\"\n",
	       cd_size, cd_sum, cd_line);

	/* --- phase 3.3: open pak0.pak and list its directory --- */
	static pak_t pak0;
	int pak_ok = pak_open("\\PAK0.PAK;1", &pak0);
	if (pak_ok) {
		pak_list(&pak0);

		/* --- phase 3.4: load progs.dat off the pak (no execution) --- */
		PR_LoadProgs(&pak0);
		const char *probe[] = { "main", "worldspawn", "SUB_Null", "PlayerPreThink" };
		for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
			int fi = PR_FindFunction(probe[i]);
			printf("  fn %-16s -> %s (first_statement %d)\n", probe[i],
			       fi >= 0 ? "found" : "MISSING",
			       fi >= 0 ? pr_functions[fi].first_statement : 0);
		}
	}

	int      marker_x  = SCREEN_W / 2 - 12;
	int      marker_dx = 2;
	uint32_t frame     = 0;

	PadState pad;
	char     held_str[96];
	char     overlay[320];

	for (;;) {
		pad_read(&pad);

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
			"mem: %d KiB hunk, selftests %s\n"
			"CD HELLO.TXT: size=%d sum=0x%X\n"
			"  \"%s\"\n"
			"\n"
			"pad1: %s   type=0x%X\n"
			"btn word: 0x%04X\n"
			"held:%s",
			frame,
			Hunk_TotalSize() / 1024,
			st_fail == 0 ? "OK" : "FAIL",
			cd_size,
			cd_sum,
			cd_line,
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
