#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <psxgte.h>
#include <inline_c.h>

#include "gdata.h"
#include "prof.h"
#include "render.h"
#include "rlist.h"
#include "vid.h"
#include "world.h"

r_state_t r_state;

/* the asm reads these offsets — see engine/r_defs.inc */
_Static_assert(offsetof(r_state_t, cam)          == 16,  "r_state layout");
_Static_assert(offsetof(r_state_t, framecount)   == 28,  "r_state layout");
_Static_assert(offsetof(r_state_t, ot_world)     == 40,  "r_state layout");
_Static_assert(offsetof(r_state_t, sz_fast)      == 52,  "r_state layout");
_Static_assert(offsetof(r_state_t, rot)          == 56,  "r_state layout");
_Static_assert(offsetof(r_state_t, tr)           == 76,  "r_state layout");
_Static_assert(offsetof(r_state_t, absrot)       == 88,  "r_state layout");
_Static_assert(offsetof(r_state_t, stat_nodes)   == 108, "r_state layout");
_Static_assert(offsetof(r_state_t, ticks_block)  == 128, "r_state layout");
_Static_assert(offsetof(r_state_t, force_mark)   == 136, "r_state layout");
_Static_assert(offsetof(r_state_t, vis_lastmask) == 140, "r_state layout");
_Static_assert(offsetof(r_state_t, ot_head)      == 144, "r_state layout");
_Static_assert(offsetof(r_state_t, textures)     == 156, "r_state layout");
_Static_assert(offsetof(r_state_t, vertpool)     == 160, "r_state layout");
_Static_assert(offsetof(r_state_t, stat_slow)    == 148, "r_state layout");
_Static_assert(offsetof(r_state_t, ticks_slow)   == 152, "r_state layout");
_Static_assert(offsetof(qw_node, visframe)       == 6,   "qw_node layout");
_Static_assert(offsetof(qw_node, plane3)         == 14,  "qw_node layout");
_Static_assert(offsetof(qw_node, parent)         == 16,  "qw_node layout");
_Static_assert(offsetof(qw_node, child)          == 20,  "qw_node layout");
_Static_assert(offsetof(qw_node, faces)          == 28,  "qw_node layout");
_Static_assert(offsetof(r_state_t, planes)       == 168, "r_state layout");
_Static_assert(offsetof(r_state_t, warp_t2)      == 172, "r_state layout");
_Static_assert(offsetof(r_state_t, warp_k16)     == 176, "r_state layout");
_Static_assert(offsetof(r_state_t, slow_bytes)   == 180, "r_state layout");
_Static_assert(offsetof(r_state_t, items)        == 184, "r_state layout");
_Static_assert(offsetof(r_state_t, viewmodel)    == 196, "r_state layout");
_Static_assert(offsetof(r_state_t, vh2)          == 204, "r_state layout");
_Static_assert(offsetof(r_state_t, nlights)      == 208, "r_state layout");
_Static_assert(offsetof(r_state_t, lights)       == 212, "r_state layout");
_Static_assert(offsetof(qw_leaf, light)          == 27,  "qw_leaf layout");
_Static_assert(offsetof(qw_leaf, visframe)       == 6,   "qw_leaf layout");
_Static_assert(offsetof(qw_leaf, num_marks)      == 14,  "qw_leaf layout");
_Static_assert(offsetof(qw_leaf, marks)          == 16,  "qw_leaf layout");
_Static_assert(offsetof(qw_leaf, vis)            == 20,  "qw_leaf layout");
_Static_assert(offsetof(qw_leaf, parent)         == 24,  "qw_leaf layout");
_Static_assert(sizeof(qw_node)                   == 36,  "qw_node layout");
_Static_assert(sizeof(qw_leaf)                   == 28,  "qw_leaf layout");
_Static_assert(offsetof(r_state_t, nodes)        == 164, "r_state layout");
_Static_assert(sizeof(qw_record)                 == 20,  "qw_record layout");
_Static_assert(offsetof(qw_record, sc)           == 12,  "qw_record layout");

static int bound;

/* ------------------------------------------------------------------ trig -- */

static int16_t sintab[1025];   /* quarter wave, sin(a * 90deg / 1024) x 4096 */

static void build_sintab(void)
{
	/* Taylor series in Q30 fixed point, exact to the last bit of x4096.
	   Only unsigned 64-bit products and 32-bit divisions. The first version
	   (signed int64 terms + __divdi3 per term) came out as sin(x) = x under
	   PCSX-Redux's dynarec — correct under its interpreter and on the host,
	   GCC's output checked — which skewed every view matrix. See
	   docs/porting-notes.md §12. */
	for (int a = 0; a <= 1024; a++) {
		uint32_t x    = (uint32_t) (((uint64_t) a * 1686629713u) >> 10);  /* a * (pi/2) / 1024, Q30 */
		uint32_t x2   = (uint32_t) (((uint64_t) x * x) >> 30);
		uint32_t term = x;
		int32_t  sum  = (int32_t) x;
		for (uint32_t k = 1; k <= 6; k++) {
			term = (uint32_t) (((uint64_t) term * x2) >> 30) / ((2 * k) * (2 * k + 1));
			sum += (k & 1) ? -(int32_t) term : (int32_t) term;
		}
		sintab[a] = (int16_t) ((sum + (1 << 17)) >> 18);      /* Q30 -> x4096 */
	}
}

int16_t r_sin(int a)
{
	a &= ANG_TURN - 1;
	if (a < 1024) return sintab[a];
	if (a < 2048) return sintab[2048 - a];
	if (a < 3072) return (int16_t) -sintab[a - 2048];
	return (int16_t) -sintab[4096 - a];
}

int16_t r_cos(int a)
{
	return r_sin(a + 1024);
}

/* atan2 in binary degrees: an octant reduction, then a 65-entry table of
   atan(t) for t = 0..1 with linear interpolation (worst error ~1 bdeg) */
int r_atan2(int32_t y, int32_t x)
{
	static const uint16_t atab[65] = {   /* atan(i / 64) in binary degrees */
		  0,  10,  20,  31,  41,  51,  61,  71,  81,  91, 101, 111, 121, 131, 140, 150,
		160, 169, 179, 188, 197, 207, 216, 225, 234, 243, 252, 260, 269, 277, 286, 294,
		302, 310, 318, 326, 334, 342, 349, 357, 364, 371, 379, 386, 393, 399, 406, 413,
		419, 426, 432, 439, 445, 451, 457, 463, 469, 474, 480, 486, 491, 496, 502, 507,
		512,
	};
	if (!x && !y)
		return 0;
	int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
	int32_t num = ay < ax ? ay : ax, den = ay < ax ? ax : ay;
	while (den > 0x7fff) { num >>= 1; den >>= 1; }
	int32_t t = den ? (num << 12) / den : 0;           /* 0..4096 */
	int32_t i = t >> 6, f = t & 63;
	int32_t a = atab[i] + (((int32_t) atab[i + 1 < 65 ? i + 1 : 64] - atab[i]) * f >> 6);
	if (ay >= ax) a = 1024 - a;                        /* octant */
	if (x < 0) a = 2048 - a;
	if (y < 0) a = -a;
	return a & 4095;
}

/* ------------------------------------------------------------------ warp -- */

r_warp_t r_warp;

/* The record bound's k = sqrt(560 / T) (x16), T = 1 .. R_WARP_T_MAX: with
   the record's nearest depth z_m = z - rho and radius rho, any edge inside
   it has (|dx| + |dy|) |dz| <= 12.6 H rho^2 / z_m (H = 160, |x|/z and |y|/z
   bounded by the frustum test), below 2T x 2 z_m once z_m >= k rho, with
   ~10 % margin for the integer screen coordinates. */
static const uint16_t warp_k16[R_WARP_T_MAX + 1] = {
	0, 379, 268, 219, 189, 169, 155, 143, 134, 126, 120, 114, 109, 105, 101, 98,
	95, 92, 89, 87, 85, 83, 81, 79, 77, 76, 74, 73, 72, 70, 69, 68, 67,
};

static void warp_apply(int t)
{
	if (t < 1) t = 1;
	if (t > R_WARP_T_MAX) t = R_WARP_T_MAX;
	r_warp.t_eff     = t;
	r_state.warp_t2  = 2u * (uint32_t) t;
	r_state.warp_k16 = warp_k16[t];
}

void r_set_warp(int mode, int t_px, int budget)
{
	r_warp.mode   = mode;
	r_warp.t      = t_px;
	r_warp.budget = budget;
	r_warp.calm   = 0;
	warp_apply(t_px);
}

/* An NTSC frame of CPU time, and the marks around it: above WARP_HIGH (or
   past a vblank) T rises by a quarter at once; it comes down one pixel at a
   time after WARP_CALM frames below WARP_LOW, so it does not oscillate. */
#define WARP_FRAME  (33868800 / 60)
#define WARP_HIGH   (WARP_FRAME * 7 / 8)
#define WARP_LOW    (WARP_FRAME * 3 / 4)
#define WARP_CALM   4

void r_warp_feedback(uint32_t frame_cycles, uint32_t vbl)
{
	int over, room;
	switch (r_warp.mode) {
	case R_WARP_DYNAMIC:
		over = vbl > 1 || frame_cycles > WARP_HIGH;
		room = vbl <= 1 && frame_cycles < WARP_LOW;
		break;
	case R_WARP_BUDGET:
		over = r_warp.tris > (uint32_t) r_warp.budget;
		room = r_warp.tris < (uint32_t) r_warp.budget * 7 / 8;
		break;
	default:
		return;
	}
	int t = r_warp.t_eff;
	if (over) {
		t += t >= 4 ? t / 4 : 1;
		r_warp.calm = 0;
	} else if (room && t > r_warp.t) {
		if (++r_warp.calm >= WARP_CALM) {
			t--;
			r_warp.calm = 0;
		}
	} else {
		r_warp.calm = 0;
	}
	warp_apply(t < r_warp.t ? r_warp.t : t);
}

/* ------------------------------------------------------------------ view -- */

static int viewport_rows = VID_H;

void r_set_viewport(int rows)
{
	viewport_rows = rows;
	gte_SetGeomOffset(VID_W / 2, rows / 2);
	r_state.ofy = rows / 2;
	r_state.vh2 = rows;
}

int r_viewport_rows(void)
{
	return viewport_rows;
}

void r_init(void)
{
	build_sintab();
	InitGeom();
	gte_SetGeomScreen(R_SCREEN_H);
	memset(&r_state, 0, sizeof r_state);
	r_state.sz_fast = R_SZ_FAST;
	r_state.items   = (uint32_t) r_items;
	r_state.lists   = (uint32_t) r_lists;
	r_set_viewport(VID_H);
}

void r_bind_world(void)
{
	r_state.root          = (uint32_t) world.root & 0x00ffffffu;   /* KUSEG, as the children */
	r_state.face_visframe = (uint32_t) world.face_visframe;
	r_state.leaves        = (uint32_t) world.leaves;
	r_state.rowbytes      = world.vis_rowbytes;
	r_state.vis_lastmask  = world.vis_rowbytes   /* (the bits of leaves 1..vis_leaves) */
	                      ? (1u << (world.vis_leaves - 8 * (world.vis_rowbytes - 1))) - 1 : 0;
	r_state.textures      = (uint32_t) world.textures;
	r_state.vertpool      = (uint32_t) world.verts;
	r_state.nodes         = (uint32_t) world.nodes;
	r_state.planes        = (uint32_t) world.planes;
	r_state.rgb           = (uint32_t) gdata.rgb;
	r_state.framecount    = 1;
	r_state.visframe      = 1;
	r_state.view_leaf     = 0;
	r_state.force_mark    = 1;
	bound = 1;
}

void r_set_view(const int32_t pos[3], int yaw, int pitch, int roll)
{
	int32_t sy = r_sin(yaw), cy = r_cos(yaw), sp = r_sin(pitch), cp = r_cos(pitch);
	int32_t sr = r_sin(roll), cr = r_cos(roll);

	/* rows: right, down = -up, forward of Quake's AngleVectors (pitch, yaw,
	   roll); x4096 * 1.5. Without roll: right (sy,-cy,0), down
	   (-sp*cy,-sp*sy,-cp), forward (cp*cy,cp*sy,-sp). */
	int32_t sp_cy = (sp * cy) >> 12, sp_sy = (sp * sy) >> 12;
	int32_t m[3][3] = {
		{ (-sr * sp_cy >> 12) + (cr * sy >> 12), (-sr * sp_sy >> 12) - (cr * cy >> 12), -sr * cp >> 12 },
		{ -((cr * sp_cy >> 12) + (sr * sy >> 12)), -((cr * sp_sy >> 12) - (sr * cy >> 12)), -(cr * cp >> 12) },
		{ (cp * cy) >> 12,  (cp * sy) >> 12, -sp },
	};
	int16_t r[9];
	for (int i = 0; i < 9; i++)
		r[i] = (int16_t) (m[i / 3][i % 3] * R_MATRIX_ONE / 4096);

	for (int i = 0; i < 9; i++) {
		r_state.rot[i]    = r[i];
		r_state.absrot[i] = (int16_t) (r[i] < 0 ? -r[i] : r[i]);
	}
	r_state.rot[9] = r_state.absrot[9] = 0;

	for (int k = 0; k < 3; k++)                    /* 1/8 units -> vertex units (x4) */
		r_state.cam[k] = pos[k] * QW_VSCALE / R_POS_SUB;
	for (int i = 0; i < 3; i++)
		r_state.tr[i] = -((r[i * 3] * r_state.cam[0] + r[i * 3 + 1] * r_state.cam[1] +
		                   r[i * 3 + 2] * r_state.cam[2]) >> 12);
}

/* ----------------------------------------------------------------- frame -- */

void r_frame(void)
{
	if (!bound)
		return;

	if (++r_state.framecount >= 0xffff) {          /* u16 stamps wrap */
		memset(world.face_visframe, 0, world.num_faces * 2);
		r_state.framecount = 1;
	}
	if (r_state.visframe >= 0xfff0) {
		for (uint32_t i = 0; i < world.num_nodes; i++)  world.nodes[i].visframe = 0;
		for (uint32_t i = 0; i < world.num_leaves; i++) world.leaves[i].visframe = 0;
		r_state.visframe   = 1;
		r_state.force_mark = 1;
	}

	r_state.ot_world     = (uint32_t) &vid_frame.ot[OT_WORLD];
	r_state.pkt          = (uint32_t) vid_frame.pkt;
	r_state.pkt_end      = (uint32_t) (vid_frame.end - QW_MAX_REC_PKT - 48);  /* see r_world.s */
#if Q1PSX_PROFILE
	r_state.stat_nodes   = r_state.stat_records = r_state.stat_verts = 0;
	r_state.stat_polys   = r_state.stat_skipped = 0;
	r_state.ticks_block  = r_state.ticks_mark = 0;

	uint32_t t0 = prof_now();
	r_render(&r_state);
	uint32_t t1 = prof_now();
#else
	r_render(&r_state);   /* release: no counters, no stage timers */
#endif

	/* triangles for the warp budget: fast-path quads are 26 bytes per
	   triangle (POLY_GT4, 52 B), the slow path's are POLY_GT3 (40 B); the
	   few window commands go uncounted */
	uint32_t bytes = r_state.pkt - (uint32_t) vid_frame.pkt;
	uint32_t slowb = r_state.slow_bytes <= bytes ? r_state.slow_bytes : bytes;
	r_warp.tris = (bytes - slowb) / 26 + slowb / 40;
	vid_frame.pkt = (uint8_t *) r_state.pkt;

#if Q1PSX_PROFILE
	/* the slow path runs inside the face-record timing: take it out */
	uint32_t total = prof_cycles(t0, t1);
	uint32_t slow  = r_state.ticks_slow * PROF_CYCLES_PER_TICK;
	uint32_t block = r_state.ticks_block * PROF_CYCLES_PER_TICK;
	uint32_t mark  = r_state.ticks_mark * PROF_CYCLES_PER_TICK;
	block = block > slow ? block - slow : 0;
	prof_stage[PROF_R_SETUP] += mark;
	prof_stage[PROF_R_BLOCK] += block;
	prof_stage[PROF_R_SLOW]  += slow;
	prof_stage[PROF_R_WALK]  += total > block + mark + slow ? total - block - mark - slow : 0;
	prof_count[PROF_N_NODES]  += r_state.stat_nodes;
	prof_count[PROF_N_BLOCKS] += r_state.stat_records;
	prof_count[PROF_N_VERTS]  += r_state.stat_verts;
	prof_count[PROF_N_POLYS]  += r_state.stat_polys;
	prof_count[PROF_N_SLOW]   += r_state.stat_skipped;
	r_state.stat_slow = r_state.slow_bytes / 40;   /* (POLY_GT3s only) */
	prof_count[PROF_N_SLOWTRI] += r_state.stat_slow;
#endif
}
