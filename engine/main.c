/*
 * q1psx — Quake for the PlayStation 1
 *
 * Boot so far (phases 2-5): bring up video/pad/CD, the Hunk/Zone allocator,
 * run the self-tests, then stream pak0.pak + progs.dat + the cooked texture
 * atlas + geometry + surfaces off the disc behind a progress bar. The loop is
 * still a validation harness: a debug free-fly camera over the textured
 * `start.bsp` with an overlay — no game logic yet.
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
#include "surf.h"
#include "bsp.h"
#include "tex.h"
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
static int      g_qtx_ok;
static surf_t   g_surf;
static int      g_surf_ok;
static bsp_t    g_bsp;
static int      g_bsp_ok;

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
	g_qtx_ok = blob && qtx_open(blob, size) > 0;
}

static geo_t g_geo;
static int   g_geo_ok;

static void boot_load_geometry(void)
{
	int   size;
	void *blob = load_disc_file("\\START.QGE;1", "geo", &size);
	g_geo_ok = blob && geo_load(blob, size, &g_geo);
}

static void boot_load_surfaces(void)
{
	int   size;
	void *blob = load_disc_file("\\START.QSF;1", "surf", &size);
	g_surf_ok = blob && surf_load(blob, size, &g_surf);
}

static void boot_load_bsp(void)
{
	int   size;
	void *blob = load_disc_file("\\START.QBS;1", "bsp", &size);
	g_bsp_ok = blob && bsp_load(blob, size, &g_bsp);
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

/* ------------------------------------------------------------ fly camera -- */
/*
 * Debug fly camera. Pad map:
 *   d-pad L/R       yaw
 *   d-pad U/D       move forward / back
 *   Square/Circle   strafe left / right
 *   Triangle/Cross  move up / down (world Z, stays intuitive when pitched)
 *   L1/R1           pitch up / down (clamped +-89 deg)
 *   R2 / L2 (hold)  move fast / slow
 *   L2 + R2 (hold)  snap back to the spawn pose
 *   START           toggle collision (Phase 6.1: sweep the player box through
 *                   the BSP clip hull; off = noclip flight)
 * (SELECT cycles the view — handled in demo_loop.)
 */
typedef struct {
	int32_t pos[3];      /* world units                       */
	int32_t yaw, pitch;  /* q1psx degrees, 20.12              */
	int32_t home[3];     /* L2+R2 restores this               */
	int     collide;     /* 1 = trace moves against the world */
} fly_cam_t;

static void fly_cam_update(fly_cam_t *c, const PadState *pad, const bsp_t *b)
{
	if (pad->pressed & PAD_START)
		c->collide = !c->collide;

	if ((pad->buttons & PAD_L2) && (pad->buttons & PAD_R2)) {
		for (int k = 0; k < 3; k++)
			c->pos[k] = c->home[k];
		c->yaw = c->pitch = 0;
		return;
	}

	if (pad->buttons & PAD_LEFT)  c->yaw   += Q_FIX(4);
	if (pad->buttons & PAD_RIGHT) c->yaw   -= Q_FIX(4);
	if (pad->buttons & PAD_L1)    c->pitch += Q_FIX(3);
	if (pad->buttons & PAD_R1)    c->pitch -= Q_FIX(3);
	if (c->pitch >  Q_FIX(89)) c->pitch =  Q_FIX(89);
	if (c->pitch < -Q_FIX(89)) c->pitch = -Q_FIX(89);

	int spd = 32;
	if (pad->buttons & PAD_R2) spd = 160;   /* fast */
	if (pad->buttons & PAD_L2) spd = 8;     /* slow */

	int32_t fb = 0, lr = 0, ud = 0;
	if (pad->buttons & PAD_UP)       fb += spd;
	if (pad->buttons & PAD_DOWN)     fb -= spd;
	if (pad->buttons & PAD_CIRCLE)   lr += spd;
	if (pad->buttons & PAD_SQUARE)   lr -= spd;
	if (pad->buttons & PAD_TRIANGLE) ud += spd;
	if (pad->buttons & PAD_CROSS)    ud -= spd;

	vec3_t ang = { c->pitch, c->yaw, 0 };
	vec3_t fwd, right, up;
	AngleVectors(ang, fwd, right, up);

	int32_t want[3];
	for (int k = 0; k < 3; k++)
		want[k] = c->pos[k] + ((fwd[k] * fb + right[k] * lr) >> Q_FIX_SHIFT);
	want[2] += ud;

	if (c->collide && b) {
		trace_t tr;
		bsp_hull_trace(b, 1, c->pos, want, &tr);
		for (int k = 0; k < 3; k++)
			c->pos[k] = tr.endpos[k];
	} else {
		for (int k = 0; k < 3; k++)
			c->pos[k] = want[k];
	}
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

	fly_cam_t cam = { .pos = { 64, 0, 0 }, .collide = 1 };
	if (g_geo_ok) {
		cam.pos[0] = (g_geo.mins[0] + g_geo.maxs[0]) / 2;
		cam.pos[1] = (g_geo.mins[1] + g_geo.maxs[1]) / 2;
		cam.pos[2] = (g_geo.mins[2] + g_geo.maxs[2]) / 2;
	}
	for (int k = 0; k < 3; k++)
		cam.home[k] = cam.pos[k];

	const bsp_t *clip_bsp = g_bsp_ok ? &g_bsp : NULL;

	/* view: 0 = world (PVS + frustum), 1 = all surfaces, 2 = wireframe.
	   SELECT cycles; unavailable modes are skipped. */
	int have_world = g_surf_ok && g_qtx_ok && g_bsp_ok;
	int have_surf  = g_surf_ok && g_qtx_ok;
	int view = have_world ? 0 : have_surf ? 1 : 2;

	uint32_t frame = 0;
	int      drawn = 0;
	PadState pad;
	char     held[96], overlay[320];

	for (;;) {
		pad_read(&pad);
		if (pad.pressed & PAD_SELECT) {
			do
				view = (view + 1) % 3;
			while ((view == 0 && !have_world) || (view == 1 && !have_surf));
		}
		fly_cam_update(&cam, &pad, clip_bsp);

		vec3_t ang = { cam.pitch, cam.yaw, 0 };
		r_begin_view(cam.pos, ang);

		const char *mode;
		if (view == 0) {
			drawn = r_world(&g_surf, &g_bsp, cam.pos, ang);
			mode  = "world PVS+frustum";
		} else if (view == 1) {
			drawn = r_surfaces(&g_surf);
			mode  = "all surfaces";
		} else if (g_geo_ok) {
			drawn = r_wireframe(g_geo.verts, g_geo.num_verts,
			                    g_geo.edges, g_geo.num_edges, 90, 200, 120);
			mode  = "wireframe";
		} else {
			drawn = r_wireframe(ROOM_VERTS, 8, ROOM_EDGES, 12, 90, 220, 120);
			mode  = "test room";
		}

		describe_buttons(held, sizeof held, pad.buttons);
		snprintf(overlay, sizeof overlay,
			"q1psx  frame %u   %s   %d prims%s\n\n"
			"  dpad LR yaw / UD move   SQ CI strafe\n"
			"  TR CR up-down   L1 R1 pitch   R2 L2 spd\n"
			"  START clip:%s   L2+R2 home   SEL view\n"
			"  pos %d %d %d   yaw %d  pitch %d\n\n"
			"mem %d KiB   selftests %s\n"
			"held:%s",
			frame, mode, drawn,
			vid_prim_overflowed() ? " OVERFLOW" : "",
			cam.collide ? "ON " : "OFF",
			cam.pos[0], cam.pos[1], cam.pos[2],
			cam.yaw >> Q_FIX_SHIFT, cam.pitch >> Q_FIX_SHIFT,
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

	load_begin(8);
	load_step("running self-tests");   boot_selftests();
	load_step("reading disc");         boot_read_hello();
	load_step("opening pak0.pak");     boot_open_pak();
	load_step("loading progs.dat");    boot_load_progs();
	load_step("loading textures");     boot_load_textures();
	load_step("loading geometry");     boot_load_geometry();
	load_step("loading surfaces");     boot_load_surfaces();
	load_step("loading BSP tree");     boot_load_bsp();
	load_done();

	demo_loop();
	return 0;
}
