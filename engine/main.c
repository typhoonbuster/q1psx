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
#include "fidelity.h"
#include "loading.h"
#include "render.h"
#include "geo.h"
#include "surf.h"
#include "bsp.h"
#include "pmove.h"
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

static void *load_disc_file_to(const char *iso_name, const char *tag,
                               int *out_size, int high)
{
	cd_file_t f;
	if (!cd_find(iso_name, &f)) {
		printf("q1psx: %s not on disc (run scripts/cook.sh)\n", iso_name);
		return NULL;
	}
	int   padded = (f.size + 2047) & ~2047;
	void *blob   = high ? Hunk_HighAllocName(padded, tag)
	                    : Hunk_AllocName(padded, tag);
	if (cd_read_at(&f, 0, blob, f.size) < 0)
		return NULL;
	*out_size = f.size;
	return blob;
}

static void *load_disc_file(const char *iso_name, const char *tag, int *out_size)
{
	return load_disc_file_to(iso_name, tag, out_size, 0);
}

/*
 * Fidelity: the disc carries all four levels as START<N>.QTX / START<N>.QSF
 * (N = fidelity_t order). "high" is the boot default; the demo loop's L2 button
 * cycles it and reloads the scene. Textures + the surface set both change; the
 * BSP tree (START.QBS) is fidelity-independent — a dropped face is still an
 * empty QSF slot, so its marksurf index stays valid.
 */
static fidelity_t g_fidelity = FIDELITY_HIGH;
static int        g_qsf_mark;   /* high-hunk mark — the QSF blob sits on top */

static void fidelity_load_textures(fidelity_t fid)
{
	/* The QTX blob is only needed until qtx_open() has DMA'd every texture to
	   VRAM — load it into the high hunk and hand that space straight back. */
	char name[24];
	snprintf(name, sizeof name, "\\START%d.QTX;1", (int) fid);
	int   mark = Hunk_HighMark();
	int   size;
	void *blob = load_disc_file_to(name, "qtx", &size, 1);
	g_qtx_ok = blob && qtx_open(blob, size) > 0;
	Hunk_FreeToHighMark(mark);
}

static void fidelity_load_surfaces(fidelity_t fid)
{
	/* Kept resident, but as the top-most high-hunk block: a fidelity switch
	   frees back to g_qsf_mark and loads the next set in the same place. */
	char name[24];
	snprintf(name, sizeof name, "\\START%d.QSF;1", (int) fid);
	g_qsf_mark = Hunk_HighMark();
	int   size;
	void *blob = load_disc_file_to(name, "surf", &size, 1);
	g_surf_ok = blob && surf_load(blob, size, &g_surf);
}

/* Swap the scene to another fidelity level at runtime. Blocks on the CD reads
   (~1 s freeze) — acceptable for a debug control. */
static void reload_fidelity(fidelity_t fid)
{
	if (g_surf_ok) {
		Hunk_FreeToHighMark(g_qsf_mark);
		g_surf_ok = 0;
	}
	g_fidelity = fid;
	fidelity_load_textures(fid);
	fidelity_load_surfaces(fid);
}

static void boot_load_textures(void) { fidelity_load_textures(g_fidelity); }
static void boot_load_surfaces(void) { fidelity_load_surfaces(g_fidelity); }

static geo_t g_geo;
static int   g_geo_ok;

static void boot_load_geometry(void)
{
	int   size;
	void *blob = load_disc_file("\\START.QGE;1", "geo", &size);
	g_geo_ok = blob && geo_load(blob, size, &g_geo);
}

static void boot_load_bsp(void)
{
	int   size;
	void *blob = load_disc_file("\\START.QBS;1", "bsp", &size);
	g_bsp_ok = blob && bsp_load(blob, size, &g_bsp);
}

/* ------------------------------------------------------------- demo loop -- */

/* ---------------------------------------------------------- player input -- */
/*
 * Debug player camera (Phase 6). Pad map:
 *   d-pad L/R       turn (yaw)
 *   d-pad U/D       move forward / back
 *   Square/Circle   strafe left / right
 *   L1/R1           look up / down (pitch, clamped +-89 deg)
 *   Cross           jump          (noclip: move down)
 *   Triangle        walk (hold)   (noclip: move up)
 *   L2 + R2 (hold)  snap back to spawn
 *   START           toggle noclip (free flight, no collision)
 * (SELECT cycles the view — handled in demo_loop.)
 */
static void read_player_cmd(pmove_t *pm, pmove_cmd_t *cmd, const PadState *pad)
{
	if ((pad->pressed & PAD_START) && !(pad->buttons & PAD_SELECT))
		pm->noclip = !pm->noclip;   /* SELECT+START is the FOV cycle */

	if ((pad->buttons & PAD_L2) && (pad->buttons & PAD_R2)) {
		for (int k = 0; k < 3; k++) {
			pm->pos[k]     = pm->home[k];
			pm->vel[k]     = 0;
			pm->pos_acc[k] = 0;
		}
		pm->yaw   = pm->home_yaw;
		pm->pitch = 0;
	}

	if (pad->buttons & PAD_LEFT)  pm->yaw   += Q_FIX(4);
	if (pad->buttons & PAD_RIGHT) pm->yaw   -= Q_FIX(4);
	if (pad->buttons & PAD_L1)    pm->pitch += Q_FIX(3);
	if (pad->buttons & PAD_R1)    pm->pitch -= Q_FIX(3);
	if (pm->pitch >  Q_FIX(89)) pm->pitch =  Q_FIX(89);
	if (pm->pitch < -Q_FIX(89)) pm->pitch = -Q_FIX(89);

	cmd->fwd  = (pad->buttons & PAD_UP)     ? 1 : (pad->buttons & PAD_DOWN)   ? -1 : 0;
	cmd->side = (pad->buttons & PAD_CIRCLE) ? 1 : (pad->buttons & PAD_SQUARE) ? -1 : 0;
	cmd->up   = (pad->buttons & PAD_TRIANGLE) ? 1 : (pad->buttons & PAD_CROSS) ? -1 : 0;
	cmd->jump = (pad->pressed & PAD_CROSS) ? 1 : 0;
	cmd->walk = (pad->buttons & PAD_TRIANGLE) ? 1 : 0;
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
	vid_set_bg(22, 26, 38);   /* flat "sky" seen through skybox faces */
	r_init();

	pmove_t player;
	memset(&player, 0, sizeof player);
	player.pos[0] = 64;
	if (g_bsp_ok && g_bsp.has_spawn) {
		player.pos[0] = g_bsp.spawn[0];
		player.pos[1] = g_bsp.spawn[1];
		player.pos[2] = g_bsp.spawn[2] + 1;   /* Quake drops the player to the floor */
		player.yaw    = Q_FIX(g_bsp.spawn_yaw);
	} else if (g_geo_ok) {
		player.pos[0] = (g_geo.mins[0] + g_geo.maxs[0]) / 2;
		player.pos[1] = (g_geo.mins[1] + g_geo.maxs[1]) / 2;
		player.pos[2] = (g_geo.mins[2] + g_geo.maxs[2]) / 2;
	}
	for (int k = 0; k < 3; k++)
		player.home[k] = player.pos[k];
	player.home_yaw = player.yaw;

	const bsp_t *clip_bsp = g_bsp_ok ? &g_bsp : NULL;

	/* view: 0 = world (PVS + frustum), 1 = all surfaces, 2 = wireframe.
	   SELECT cycles; unavailable modes are skipped. */
	int have_world = g_surf_ok && g_qtx_ok && g_bsp_ok;
	int have_surf  = g_surf_ok && g_qtx_ok;
	int view = have_world ? 0 : have_surf ? 1 : 2;

	int      drawn   = 0;
	int      vbl_avg = 1 << 4;   /* 4.4 fixed, smoothed vblanks/frame */
	int      dbg     = 1;        /* overlay: 0 off, 1 compact, 2 full */
	int      both_l1r1_prev = 0;
	int      fov_i = 1;         /* index into HVAL, 1 == 160 (~90 deg) */
	PadState pad;
	char     overlay[384];

	for (;;) {
		pad_read(&pad);
		if (pad.pressed & PAD_SELECT) {
			do
				view = (view + 1) % 3;
			while ((view == 0 && !have_world) || (view == 1 && !have_surf));
		}
		/* L1+R1 together cycles the debug overlay (pitch nets to zero) */
		int both_l1r1 = (pad.buttons & PAD_L1) && (pad.buttons & PAD_R1);
		if (both_l1r1 && !both_l1r1_prev)
			dbg = (dbg + 1) % 3;
		both_l1r1_prev = both_l1r1;

		/* L2 cycles the cooked fidelity level (low/medium/high/ultra) and
		   reloads the scene's textures + surfaces. SELECT+START cycles FOV.
		   (L2+R2 together is "snap home", handled in read_player_cmd.) */
		if (!((pad.buttons & PAD_L2) && (pad.buttons & PAD_R2)) &&
		    (pad.pressed & PAD_L2)) {
			fidelity_t nf = (fidelity_t) ((g_fidelity + 1) % FIDELITY_COUNT);
			snprintf(overlay, sizeof overlay,
			         "loading fidelity: %s ...", FIDELITY[nf].name);
			vid_text(0, 8, 8, overlay);
			vid_flip();
			reload_fidelity(nf);
			have_world = g_surf_ok && g_qtx_ok && g_bsp_ok;
			have_surf  = g_surf_ok && g_qtx_ok;
			if ((view == 0 && !have_world) || (view == 1 && !have_surf))
				view = 2;
			continue;
		}
		if ((pad.pressed & PAD_START) && (pad.buttons & PAD_SELECT)) {
			static const int HVAL[4] = { 120, 160, 200, 256 };
			fov_i = (fov_i + 1) & 3;
			r_set_screen_h(HVAL[fov_i]);
		}

		uint32_t tp0 = vid_vbl_now();
		pmove_cmd_t cmd;
		read_player_cmd(&player, &cmd, &pad);
		pmove_step(&player, &cmd, clip_bsp);
		uint32_t tp1 = vid_vbl_now();

		int32_t eye[3] = { player.pos[0], player.pos[1], player.pos[2] + 22 };
		vec3_t  ang    = { player.pitch, player.yaw, 0 };
		r_begin_view(eye, ang);

		const char *mode;
		if (view == 0) {
			drawn = r_world(&g_surf, &g_bsp, eye, ang);
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

		uint32_t tr1 = vid_vbl_now();

		int vbl = vid_frame_vbl();
		vbl_avg = (vbl_avg * 7 + (vbl << 4)) >> 3;   /* 4.4 fixed, ~1s smoothing */

		if (dbg == 1) {
			snprintf(overlay, sizeof overlay,
				"~%d fps (%d.%d vbl)  %s\n"
				"walk %d us   draw %d us\n"
				"faces %d drawn %d  fidelity %s",
				vbl ? 60 / vbl : 60,
				vbl_avg >> 4, ((vbl_avg & 15) * 10) >> 4,
				vid_gpu_busy() ? "GPU-bound" : "cpu-bound",
				r_dbg_us_walk, r_dbg_us_draw,
				r_dbg_faces, drawn,
				FIDELITY[g_fidelity].name);
			vid_text(0, 8, 8, overlay);
		} else if (dbg == 2) {
			snprintf(overlay, sizeof overlay,
				"q1psx  %s\n"
				"  %d.%d vbl/frame  ~%d fps  %s   %d prim B%s\n\n"
				"  nodes %d  leaves %d  faces %d  drawn %d\n"
				"  drop near %d back %d sky %d tiny %d\n"
				"  us: walk %d  draw %d   vbl pm %u rn %u\n"
				"  fidelity %s  subdiv %d  fov-h %d\n"
				"  pos %d %d %d  yaw %d pit %d  %s %s\n\n"
				"  L2 fidelity  SEL view  SEL+START fov\n"
				"  L1+R1 overlay  START noclip  L2+R2 home\n"
				"mem %d KiB  sel %s",
				mode,
				vbl_avg >> 4, ((vbl_avg & 15) * 10) >> 4,
				vbl ? 60 / vbl : 60,
				vid_gpu_busy() ? "GPU-bound" : "cpu/ok",
				vid_prim_used(), vid_prim_overflowed() ? " OVF" : "",
				r_dbg_nodes, r_dbg_leaves, r_dbg_faces, drawn,
				r_dbg_drop_near, r_dbg_drop_back, r_dbg_drop_sky, r_dbg_drop_tiny,
				r_dbg_us_walk, r_dbg_us_draw,
				tp1 - tp0, tr1 - tp1,
				FIDELITY[g_fidelity].name, r_subdiv, r_screen_h,
				player.pos[0], player.pos[1], player.pos[2],
				player.yaw >> Q_FIX_SHIFT, player.pitch >> Q_FIX_SHIFT,
				player.onground ? "ground" : "air",
				player.noclip ? "noclip" : "clip",
				Hunk_TotalSize() / 1024, g_selftest_fail ? "FAIL" : "OK");
			vid_text(0, 8, 8, overlay);
		}

		vid_flip();
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
