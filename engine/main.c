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
#include "render.h"
#include "geo.h"
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

static void *load_disc_file(const char *iso_name, const char *tag, int *out_size)
{
	cd_file_t f;
	if (!cd_find(iso_name, &f)) {
		printf("q1psx: %s not on disc (run scripts/cook.sh)\n", iso_name);
		return NULL;
	}
	int   padded = (f.size + 2047) & ~2047;
	void *blob   = Hunk_AllocName(padded, tag);
	if (cd_read_at(&f, 0, blob, f.size) < 0)
		return NULL;
	*out_size = f.size;
	return blob;
}

static void boot_load_textures(void)
{
	int   size;
	void *blob = load_disc_file("\\START.QTX;1", "qtx", &size);
	if (!blob || qtx_open(blob, size) <= 0)
		return;

	const char *want[] = { "tech08_1", "wbrick1_5", "city4_6" };
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++)
		if ((g_demo_tex_ok = qtx_upload(want[i], &g_demo_tex)))
			break;
}

static geo_t g_geo;
static int   g_geo_ok;

static void boot_load_geometry(void)
{
	int   size;
	void *blob = load_disc_file("\\START.QGE;1", "geo", &size);
	g_geo_ok = blob && geo_load(blob, size, &g_geo);
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

/* an inside-out box in Quake coords (X fwd, Y left, Z up), world units */
static const SVECTOR ROOM_VERTS[8] = {
	{   0, -256, -128 }, {   0,  256, -128 }, {   0,  256,  128 }, {   0, -256,  128 },
	{ 768, -256, -128 }, { 768,  256, -128 }, { 768,  256,  128 }, { 768, -256,  128 },
};
static const uint16_t ROOM_EDGES[24] = {
	0,1, 1,2, 2,3, 3,0,          /* near wall  */
	4,5, 5,6, 6,7, 7,4,          /* far wall   */
	0,4, 1,5, 2,6, 3,7,          /* stringers  */
};

static void demo_loop(void)
{
	vid_set_bg(8, 8, 16);
	r_init();

	int32_t cam[3] = { 64, 0, 0 };
	if (g_geo_ok) {
		cam[0] = (g_geo.mins[0] + g_geo.maxs[0]) / 2;
		cam[1] = (g_geo.mins[1] + g_geo.maxs[1]) / 2;
		cam[2] = (g_geo.mins[2] + g_geo.maxs[2]) / 2;
	}

	int32_t  yaw   = 0;              /* q1psx degrees, 20.12 */
	int32_t  pitch = 0;
	uint32_t frame = 0;
	int      lines = 0;
	PadState pad;
	char     held[96], overlay[320];

	for (;;) {
		pad_read(&pad);

		if (pad.buttons & PAD_LEFT)  yaw   += Q_FIX(3);
		if (pad.buttons & PAD_RIGHT) yaw   -= Q_FIX(3);
		if (pad.buttons & PAD_L1)    pitch += Q_FIX(2);
		if (pad.buttons & PAD_R1)    pitch -= Q_FIX(2);
		if (pitch >  Q_FIX(89)) pitch =  Q_FIX(89);
		if (pitch < -Q_FIX(89)) pitch = -Q_FIX(89);
		vec3_t ang = { pitch, yaw, 0 };

		vec3_t f, r, u;
		AngleVectors(ang, f, r, u);
		int step = (pad.buttons & PAD_UP) ? 16 : (pad.buttons & PAD_DOWN) ? -16 : 0;
		for (int k = 0; k < 3; k++)
			cam[k] += (f[k] * step) >> Q_FIX_SHIFT;

		r_begin_view(cam, ang);
		if (g_geo_ok)
			lines = r_wireframe(g_geo.verts, g_geo.num_verts,
			                    g_geo.edges, g_geo.num_edges, 90, 200, 120);
		else
			lines = r_wireframe(ROOM_VERTS, 8, ROOM_EDGES, 12, 90, 220, 120);

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
			"%s wireframe (GTE)\n"
			"  dpad LR turn / UD move, L1R1 pitch\n"
			"  cam %d %d %d  yaw %d pitch %d\n"
			"  %d lines\n\n"
			"mem %d KiB  selftests %s\n"
			"held:%s",
			frame,
			g_geo_ok ? "start.bsp" : "test room",
			cam[0], cam[1], cam[2], yaw >> Q_FIX_SHIFT, pitch >> Q_FIX_SHIFT,
			lines,
			Hunk_TotalSize() / 1024, g_selftest_fail ? "FAIL" : "OK",
			held);
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

	load_begin(6);
	load_step("running self-tests");   boot_selftests();
	load_step("reading disc");         boot_read_hello();
	load_step("opening pak0.pak");     boot_open_pak();
	load_step("loading progs.dat");    boot_load_progs();
	load_step("loading textures");     boot_load_textures();
	load_step("loading geometry");     boot_load_geometry();
	load_done();

	demo_loop();
	return 0;
}
