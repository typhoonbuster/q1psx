/*
 * q1psx — Quake for the PlayStation 1
 *
 * Boot so far (phases 2-4): bring up video/pad/CD, the Hunk/Zone allocator,
 * run the self-tests, then stream pak0.pak + progs.dat + a cooked texture pack
 * off the disc behind a progress bar. The loop is still a validation harness
 * (debug overlay, a steerable marker, one drawn texture) — no game yet.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psxgpu.h>
#include <psxpad.h>

#include "vid.h"
#include "loading.h"
#include "pad.h"
#include "cd.h"
#include "pak.h"
#include "progs.h"
#include "tex.h"
#include "sys.h"
#include "zone.h"
#include "qcommon.h"
#include "selftest.h"

/*
 * Main Quake memory block. Retail PS1 has ~1.94 MB of APP_RAM; PSn00bSDK's
 * startup hands almost all of it to malloc. We take one contiguous slice and
 * let Hunk/Zone manage it, leaving the rest for the stack and psxcd. Tunable —
 * revisit in phase 11 with real level data.
 */
#define HUNK_SIZE  (1536 * 1024)

/* --- boot-time state kept for the demo overlay --------------------------- */
static int        g_selftest_fail;
static int        g_hello_size;
static uint32_t   g_hello_sum;
static char       g_hello_line[40];
static vram_tex_t g_demo_tex;
static int        g_demo_tex_ok;

static uint32_t cd_scratch[512];   /* one sector, word-aligned */

static void first_line(char *dst, int cap, const char *src, int n)
{
	int k = 0;
	for (; k < n && k < cap - 1 && src[k] != '\n' && src[k] != '\r'; k++)
		dst[k] = (src[k] >= 32 && src[k] < 127) ? src[k] : '.';
	dst[k] = 0;
}

/* ------------------------------------------------------------- boot steps -- */

static void boot_selftests(void)
{
	g_selftest_fail = run_selftests();
	Mem_Report();
}

static void boot_read_hello(void)
{
	g_hello_size = cd_load("\\HELLO.TXT;1", cd_scratch, sizeof cd_scratch);
	g_hello_sum  = 0;
	if (g_hello_size > 0) {
		const uint8_t *b = (const uint8_t *) cd_scratch;
		for (int i = 0; i < g_hello_size; i++)
			g_hello_sum += b[i];
		first_line(g_hello_line, sizeof g_hello_line, (const char *) cd_scratch, g_hello_size);
	} else {
		snprintf(g_hello_line, sizeof g_hello_line, "<read failed>");
	}
}

static pak_t g_pak0;
static int   g_pak0_ok;

static void boot_open_pak(void)
{
	g_pak0_ok = pak_open("\\PAK0.PAK;1", &g_pak0);
}

static void boot_load_progs(void)
{
	if (g_pak0_ok)
		PR_LoadProgs(&g_pak0);
}

static void boot_load_textures(void)
{
	cd_file_t qf;
	if (!cd_find("\\START.QTX;1", &qf)) {
		printf("q1psx: START.QTX not on disc (run scripts/cook.sh)\n");
		return;
	}
	int   padded = (qf.size + 2047) & ~2047;
	void *blob   = Hunk_AllocName(padded, "qtx");
	if (cd_read_at(&qf, 0, blob, qf.size) < 0 || qtx_open(blob, qf.size) <= 0)
		return;

	const char *want[] = { "tech08_1", "wbrick1_5", "city4_6" };
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++)
		if ((g_demo_tex_ok = qtx_upload(want[i], &g_demo_tex)))
			break;
}

/* ------------------------------------------------------------- demo loop -- */

static void describe_buttons(char *buf, int cap, uint16_t held)
{
	static const struct { uint16_t mask; const char *name; } BTN[] = {
		{ PAD_UP, "UP" }, { PAD_DOWN, "DOWN" }, { PAD_LEFT, "LEFT" }, { PAD_RIGHT, "RIGHT" },
		{ PAD_TRIANGLE, "TRI" }, { PAD_CIRCLE, "CIR" }, { PAD_CROSS, "CRO" }, { PAD_SQUARE, "SQR" },
		{ PAD_L1, "L1" }, { PAD_R1, "R1" }, { PAD_L2, "L2" }, { PAD_R2, "R2" },
		{ PAD_SELECT, "SEL" }, { PAD_START, "STA" },
	};
	buf[0] = 0;
	for (unsigned i = 0; i < sizeof(BTN) / sizeof(BTN[0]); i++)
		if (held & BTN[i].mask) {
			int l = (int) strlen(buf);
			snprintf(buf + l, cap - l, " %s", BTN[i].name);
		}
	if (!buf[0])
		snprintf(buf, cap, " (none)");
}

static void demo_loop(void)
{
	vid_set_bg(40, 20, 80);

	int      mx = VID_W / 2 - 12, mdx = 2;
	uint32_t frame = 0;
	PadState pad;
	char     held[96], overlay[320];

	for (;;) {
		pad_read(&pad);

		if      (pad.buttons & PAD_LEFT)  mx -= 3;
		else if (pad.buttons & PAD_RIGHT) mx += 3;
		else                              mx += mdx;
		if      (mx <= 0)            { mx = 0;            mdx = +2; }
		else if (mx >= VID_W - 24)   { mx = VID_W - 24;   mdx = -2; }

		TILE *marker = vid_alloc(1, sizeof(TILE));
		setTile(marker);
		setXY0(marker, mx, VID_H / 2 - 12);
		setWH(marker, 24, 24);
		setRGB0(marker, 230, 230, 40);

		if (g_demo_tex_ok) {
			POLY_FT4 *q = vid_alloc(2, sizeof(POLY_FT4));
			setPolyFT4(q);
			setRGB0(q, 128, 128, 128);
			setXYWH(q, VID_W - g_demo_tex.w - 8, VID_H - g_demo_tex.h - 8,
			        g_demo_tex.w, g_demo_tex.h);
			setUVWH(q, 0, 0, g_demo_tex.w - 1, g_demo_tex.h - 1);
			q->tpage = g_demo_tex.tpage;
			q->clut  = g_demo_tex.clut;
		}

		describe_buttons(held, sizeof held, pad.buttons);
		snprintf(overlay, sizeof overlay,
			"q1psx  frame %u\n\n"
			"mem: %d KiB hunk, selftests %s\n"
			"CD HELLO.TXT: %d B  sum 0x%X\n  \"%s\"\n\n"
			"pad1: %s  type 0x%X  btn 0x%04X\n"
			"held:%s",
			frame, Hunk_TotalSize() / 1024, g_selftest_fail ? "FAIL" : "OK",
			g_hello_size, g_hello_sum, g_hello_line,
			pad.connected ? "connected" : "no device", pad.type, pad.buttons, held);
		vid_text(0, 8, 8, overlay);

		vid_flip();
		frame++;
	}
}

/* ------------------------------------------------------------------ main -- */

int main(void)
{
	vid_init();
	pad_init();
	cd_init();

	void *hunk = malloc(HUNK_SIZE);
	if (!hunk)
		Sys_Error("no RAM for a %d-byte hunk", HUNK_SIZE);
	Memory_Init(hunk, HUNK_SIZE);

	load_begin(5);
	load_step("running self-tests");   boot_selftests();
	load_step("reading disc");         boot_read_hello();
	load_step("opening pak0.pak");     boot_open_pak();
	load_step("loading progs.dat");    boot_load_progs();
	load_step("loading textures");     boot_load_textures();
	load_done();

	demo_loop();
	return 0;
}
