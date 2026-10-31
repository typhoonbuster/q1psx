#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxapi.h>
#include <psxgte.h>
#include <inline_c.h>

#include "bmfrag.h"
#include "gdata.h"
#include "mem.h"
#include "prof.h"
#include "q_fixed.h"
#include "render.h"
#include "rlist.h"
#include "vid.h"
#include "world.h"
#include "zone.h"

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
_Static_assert(offsetof(r_state_t, fragpool)     == 216, "r_state layout");
_Static_assert(offsetof(r_state_t, seq)          == 220, "r_state layout");
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
#define WARP_PROBE  128      /* frames at the floor with a vblank more before the
                                cap's share is tried again (r_warp_feedback); it
                                doubles at each try that fails, up to x 8 */

/* The record bound's k = sqrt(560 / T) (x16), T = 1 .. R_WARP_T_MAX: with
   the record's nearest depth z_m = z - rho and radius rho, any edge inside
   it has (|dx| + |dy|) |dz| <= 12.6 H rho^2 / z_m (H = 160, |x|/z and |y|/z
   bounded by the frustum test), below 2T x 2 z_m once z_m >= k rho, with
   ~10 % margin for the integer screen coordinates. */
static const uint16_t warp_k16[R_WARP_T_MAX + 1] = {
	0, 379, 268, 219, 189, 169, 155, 143, 134, 126, 120, 114, 109, 105, 101, 98,
	95, 92, 89, 87, 85, 83, 81, 79, 77, 76, 74, 73, 72, 70, 69, 68,
	67, 66, 65, 64, 63, 62, 61, 61, 60, 59, 58, 58, 57, 56, 56, 55,
	55,
};

Q_COLD static void warp_apply(int t)
{
	if (t < 1) t = 1;
	if (t > R_WARP_T_MAX) t = R_WARP_T_MAX;
	r_warp.t_eff     = t;
	r_state.warp_t2  = 2u * (uint32_t) t;
	r_state.warp_k16 = warp_k16[t];
}

void r_set_warp(int mode, int t_px, int t_max)
{
	r_warp.mode  = mode;
	r_warp.t     = t_px;
	r_warp.t_max = t_max < t_px ? t_px : t_max > R_WARP_T_MAX ? R_WARP_T_MAX : t_max;
	r_warp.calm  = 0;
	r_warp.more  = 0;
	r_warp.ref   = 0;
	r_warp.stuck = 0;
	r_warp.wait  = WARP_PROBE;
	warp_apply(t_px);
}

/* An NTSC frame of CPU time, and the marks around it: above WARP_HIGH (or
   past a vblank) T rises by a quarter at once; it comes down one pixel at a
   time after WARP_CALM frames below WARP_LOW, so it does not oscillate.
   The marks are for the vblanks aimed at: the cap's share, and `more` when
   a frame at the warp's cap (r_warp.t_max) missed them all the same (the
   view then drawn at the floor with a vblank more, as it would take that
   vblank anyway); back to the share once a frame at the floor is lighter
   than the first one there by what the cap lacked. */
#define WARP_FRAME  (33868800 / 60)
#define WARP_HIGH   (WARP_FRAME * 7 / 8)
#define WARP_LOW    (WARP_FRAME * 3 / 4)
#define WARP_CALM   4
#define WARP_STUCK  30       /* frames in a row at the cap missing the vblanks
                                aimed at before a vblank more: a view that cannot
                                make them (a load's or a view's first frames miss
                                them for a while) */

/* Under the default's floor (ultra's 1 px) the near polygons' packets grow
   as T shrinks (a polygon's pieces as 1 / T): at 1 px the heaviest of the
   128 views filled the arena or left far models out (docs/porting-notes.md
   §117). T then goes no lower than what the last frame's packets, every
   one scaled by T / T' (a bound: the fast path's do not grow), say would
   still leave a far model its room (r_defs.inc R_MODEL_ROOM; an arena
   filled says nothing of what was left unwritten: WARP_GUARD at once), and
   never above WARP_GUARD for it: the levels whose floor is there or higher
   run the control as it was. */
#define WARP_GUARD  4
#define WARP_ROOM   24576

static int warp_room_floor(int t)
{
	if (r_warp.t >= WARP_GUARD)
		return r_warp.t;
	if (r_arena_full)
		return WARP_GUARD;
	uint32_t avail = r_warp.avail > WARP_ROOM ? r_warp.avail - WARP_ROOM : 1;
	uint32_t need  = (r_warp.used * (uint32_t) t + avail - 1) / avail;
	if (need > WARP_GUARD)
		need = WARP_GUARD;
	return need > (uint32_t) r_warp.t ? (int) need : r_warp.t;
}

void r_warp_feedback(uint32_t frame_cycles, uint32_t vbl, uint32_t share)
{
	if (r_warp.mode != R_WARP_DYNAMIC)
		return;
	int t  = r_warp.t_eff;
	int lo = warp_room_floor(t);                  /* (the floor, ultra's arena allowing) */
	if (r_warp.more && t <= lo) {                 /* (at the floor: like for like) */
		if (!r_warp.ref) {
			r_warp.ref   = frame_cycles;
			r_warp.probe = 0;
		} else if (frame_cycles + r_warp.shortfall < r_warp.ref) {
			r_warp.more--;                        /* lighter: the vblank less again */
			r_warp.ref  = 0;
			r_warp.wait = WARP_PROBE;
		} else if (++r_warp.probe >= r_warp.wait) {
			r_warp.more--;                        /* a while: tried again, and if it */
			r_warp.ref  = 0;                      /*  fails, twice as long next time */
			if (r_warp.wait < WARP_PROBE * 8)
				r_warp.wait *= 2;
		}
	}
	uint32_t aim = share + (uint32_t) r_warp.more;
	int over = vbl > aim || frame_cycles > WARP_HIGH * aim;   /* (against the vblanks */
	int room = vbl <= aim && frame_cycles < WARP_LOW * aim;   /*  aimed at) */
	if (vbl > aim && t >= r_warp.t_max) {         /* the largest T missed them too */
		uint32_t s = frame_cycles > WARP_FRAME * aim ? frame_cycles - WARP_FRAME * aim : WARP_FRAME / 16;
		if (!r_warp.stuck || s < r_warp.shortfall)
			r_warp.shortfall = s;                 /* (the least, over the frames) */
		if (++r_warp.stuck >= WARP_STUCK) {
			r_warp.more++;
			r_warp.ref   = 0;
			r_warp.calm  = 0;
			r_warp.stuck = 0;
			warp_apply(lo);
			return;
		}
	} else {
		r_warp.stuck = 0;
	}
	if (over) {
		t += t >= 4 ? t / 4 : 1;
		r_warp.calm = 0;
	} else if (room && t > lo) {
		if (++r_warp.calm >= WARP_CALM) {
			t--;
			r_warp.calm = 0;
		}
	} else {
		r_warp.calm = 0;
	}
	warp_apply(t < lo ? lo : t > r_warp.t_max ? r_warp.t_max : t);
}

/* ------------------------------------------------------- the cooked walk -- */

/* the world's record loops' three ways out to the tree walk (r_world.s
   RECLOOP wx: dr and the bench's copy drn) and draw_ents' way back, PC-
   relative branches, pointed at seq_next while a cooked walk runs */
extern uint32_t dr_xa[], dr_xb[], dr_xc[], drn_xa[], drn_xb[], drn_xc[], ei_x[], seq_next[], seq_ents_back[];
static uint32_t *const seq_site[] = { dr_xa, dr_xb, dr_xc, drn_xa, drn_xb, drn_xc, ei_x };
#define SEQ_SITES (int) (sizeof seq_site / sizeof seq_site[0])
static uint32_t seq_orig[SEQ_SITES];
#define SEQ_IMM16(w, v)  (((w) & 0xffff0000u) | ((uint32_t) (v) & 0xffffu))
static int      seq_mode = -1;           /* -1 as built (the tree's), 0 the tree, 1 the cooked walk */

/* the sites as the mode wants them (no cache flush here). A site holding
   a `j` is the flat shading's (r_set_flat puts one in dr's first word,
   which is dr_xa's in a build without the profile): left as it is, the
   copy it jumps to has its own sites. draw_ents' way back goes to
   seq_ents_back (the registers a leaf's list clobbers taken back). */
static void seq_apply(void)
{
	if (seq_mode < 0)
		return;
	for (int i = 0; i < SEQ_SITES; i++) {
		uint32_t w = seq_orig[i];
		if (*seq_site[i] >> 26 == 2)
			continue;
		if (seq_mode) {
			uint32_t *to = seq_site[i] == ei_x ? seq_ents_back : seq_next;
			int32_t off = ((int32_t) (uintptr_t) to - ((int32_t) (uintptr_t) seq_site[i] + 4)) >> 2;
			if (off != (int16_t) off) {      /* (the link put them too far apart: the tree walk then) */
				printf("render: the cooked walk's loop is out of a branch's reach\n");
				seq_mode = 0;
				r_state.seq = 0;
				seq_apply();
				return;
			}
			w = SEQ_IMM16(w, off);
		}
		*seq_site[i] = w;
	}
}

void r_seq_set(uint32_t seq)
{
	r_state.seq = seq;
	int on = seq != 0;
	if (on == seq_mode)
		return;
	if (seq_mode < 0)
		for (int i = 0; i < SEQ_SITES; i++)
			seq_orig[i] = *seq_site[i];
	seq_mode = on;
	seq_apply();
	FlushCache();
}

/* ------------------------------------------------- the near polygons' grid -- */

/* the maps that take the regular subdivision (engine/r_tess.s, §92): a
   build setting, CMake Q1PSX_TESS_MAPS: names separated by commas, "all",
   or nothing. The world record loops' calls to the slow path (r_world.s
   RECLOOP: dr's and drn's, `jal`) go to tess_poly on those, to slow_poly
   on the others, whose frames stay the tree walk's. */
#ifndef Q1PSX_TESS_MAPS
#define Q1PSX_TESS_MAPS ""
#endif
int r_tess_on;
#if !Q1PSX_ENGINE_C
extern uint32_t dr_ts[], drn_ts[];
extern void tess_poly(void), slow_poly(void);
#endif

static int tess_wanted(const char *map)
{
	const char *l = Q1PSX_TESS_MAPS;
	if (!map || !*map)
		return 0;
	while (*l) {
		const char *e = l;
		while (*e && *e != ',')
			e++;
		size_t n = (size_t) (e - l);
		if ((n == 3 && !strncmp(l, "all", 3)) || (n == strlen(map) && !strncmp(l, map, n)))
			return 1;
		l = *e ? e + 1 : e;
	}
	return 0;
}

void r_tess_select(const char *map)
{
	r_tess_on = tess_wanted(map);
#if !Q1PSX_ENGINE_C
	uint32_t *const site[] = { dr_ts, drn_ts };
	uint32_t to = (uint32_t) (uintptr_t) (r_tess_on ? tess_poly : slow_poly);
	for (int i = 0; i < 2; i++)
		*site[i] = (3u << 26) | ((to >> 2) & 0x03ffffffu);   /* jal to */
	FlushCache();
#endif
}

/* --------------------------------------------------------------- options -- */

/* the fogged transform's table (r_world.s XFORM fog): by SZ / 64, a
   vertex's light factor x 128, from 128 at the fog's start (R_FOG_Z) down
   to 0 at the draw distance (r_init) */
uint8_t r_fogtab[1024];
uint8_t r_vis_row[256];     /* the camera leaf's PVS row expanded (r_world.s vis_expand: the
                               marking reads it byte by byte; formats/qworld.h qw_leaf vis,
                               docs/porting-notes.md §90.9); the cook keeps rows under 256 */

static int opt_flat = R_FLAT_FAR;            /* the record loop's (r_set_flat) */
static int32_t mark_cam[3];                  /* the camera at the last marking */
static uint32_t mark_visframe;
#define R_MARK_MOVE 64                       /* world units the camera moves before
                                                mark_pvs's marks are redone (its reach's
                                                margin: r_defs.inc R_MARK_R) */

/* the patched words (r_world.s): the world record loop's entry (the
   bench's copy without the flat shading: drn), the dynamic lights' three
   ways back into it, its flat shading's threshold, the camera-leaf call */
extern uint32_t dr_records[], drn_records[], dlr_ret0[], dlr_ret1[], dlr_ret2[], mark_call[];
extern uint32_t drn_lit_ret[], dr_o_flatz[];

enum { S_DR, S_RET0, S_RET1, S_RET2, S_FLATZ, S_MARK, S_ENTRIES };
static uint32_t *const entry[S_ENTRIES] = { dr_records, dlr_ret0, dlr_ret1, dlr_ret2, dr_o_flatz, mark_call };
static uint32_t orig[S_ENTRIES];             /* their words as built */

#define J_TO(op, a)   ((op) | (((uint32_t) (a) >> 2) & 0x03ffffffu))
#define LI(w, v)      (0x24000000u | ((w) & 0x001f0000u) | (uint32_t) (v))   /* addiu rt, $zero, v */

/* the patched words as built, once */
static void keep_orig(void)
{
	if (orig[S_DR])
		return;
	for (int i = 0; i < S_ENTRIES; i++)
		orig[i] = *entry[i];
}

/* The camera-leaf call (r_world.s mark_call): mark_pvs, through the zones'
   hook while a zoned map is loaded (engine/zone.s zone_mark: the leaf's
   zone made current before the walk, then on to mark_pvs). */
extern uint32_t zone_mark[];

static void mark_patch(void)
{
	keep_orig();
	*entry[S_MARK] = zone_on() ? J_TO(0x0c000000u, zone_mark) : orig[S_MARK];
	FlushCache();
}

void r_set_flat(int flat)
{
	keep_orig();
	opt_flat = flat;
	int off = flat == R_FLAT_OFF;                /* drn, its lights' way back its own */
	uint32_t ret0 = (orig[S_RET0] & 0xffff0000u) |
	                ((((uint32_t) drn_lit_ret - ((uint32_t) dlr_ret0 + 4)) >> 2) & 0xffffu);
	*entry[S_DR]    = off ? J_TO(0x08000000u, drn_records) : orig[S_DR];
	*entry[S_RET0]  = off ? ret0 : orig[S_RET0];
	*entry[S_RET1]  = off ? J_TO(0x08000000u, drn_lit_ret) : orig[S_RET1];
	*entry[S_RET2]  = off ? J_TO(0x08000000u, drn_lit_ret) : orig[S_RET2];
	*entry[S_FLATZ] = flat == R_FLAT_ALL ? LI(orig[S_FLATZ], 0) : orig[S_FLATZ];   /* everywhere: li 0
	                                                                                  (never nearer) */
	seq_apply();                                 /* (dr's first word back from FLAT=0: the
	                                                cooked walk's way out again if one runs) */
	FlushCache();
}

int32_t r_view_depth(const int32_t pos[3])
{
	int32_t x = pos[0] * QW_VSCALE / R_POS_SUB - r_state.cam[0];
	int32_t y = pos[1] * QW_VSCALE / R_POS_SUB - r_state.cam[1];
	int32_t z = pos[2] * QW_VSCALE / R_POS_SUB - r_state.cam[2];
	return (r_state.rot[6] * x + r_state.rot[7] * y + r_state.rot[8] * z) >> 12;
}

/* ------------------------------------------------------------------ view -- */

static int viewport_rows = VID_H;

Q_COLD void r_set_viewport(int rows)
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

Q_COLD void r_init(void)
{
	build_sintab();
	InitGeom();
	gte_SetGeomScreen(R_SCREEN_H);
	memset(&r_state, 0, sizeof r_state);
	r_state.sz_fast = R_SZ_FAST;
	r_state.items   = (uint32_t) r_items;
	r_state.lists   = (uint32_t) r_lists;
	r_set_viewport(VID_H);
	for (int i = 0; i < 1024; i++) {           /* the fog's factors */
		int32_t z = i * 64 + 32;
		r_fogtab[i] = (uint8_t) (z <= R_FOG_Z ? 128 : z >= R_DRAW_Z ? 0 : 128 * (R_DRAW_Z - z) / (R_DRAW_Z - R_FOG_Z));
	}
}

void r_bind_world(void)
{
	r_warp.more  = 0;                         /* (a new map: the cap's share again) */
	r_warp.ref   = 0;
	r_warp.stuck = 0;
	r_warp.wait  = WARP_PROBE;
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
	r_seq_set(0);                                /* (the tree walk until a zone's chunk says otherwise) */
	r_tess_select(world.map_name);               /* (the near polygons' grid on the maps that take it) */
	mark_patch();                                /* (through the zones' hook or not) */
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

	for (int k = 0; k < 3; k++)                    /* 1/8 units -> vertex units (x4) */
		r_state.cam[k] = pos[k] * QW_VSCALE / R_POS_SUB;
	for (int i = 0; i < 9; i++) {
		r_state.rot[i]    = r[i];
		r_state.absrot[i] = (int16_t) (r[i] < 0 ? -r[i] : r[i]);
	}
	r_state.rot[9] = r_state.absrot[9] = 0;
	for (int i = 0; i < 3; i++)
		r_state.tr[i] = -((r[i * 3] * r_state.cam[0] + r[i * 3 + 1] * r_state.cam[1] +
		                   r[i * 3 + 2] * r_state.cam[2]) >> 12);
}

/* ----------------------------------------------------------------- frame -- */

int r_arena_full;
#ifdef Q1PSX_BENCH
int32_t r_model_room_min;   /* the least packet room an alias model found this frame (r_world.s
                               draw_model's test against R_MODEL_ROOM): the arenas' sizing */
uint32_t r_model_skipped;   /* the alias models left out for the room (never reset: a run's count) */
#endif
int r_ui_room;

void r_frame(void)
{
	if (!bound)
		return;

	if (++r_state.framecount >= 0xffff) {          /* u16 stamps wrap */
		memset(world.face_visframe, 0, world.num_faces * 2);
		for (uint32_t i = 0; i < world.num_nodes; i++)   /* (the lights' stamps too: a node
		                                                    a light reached 65534 frames ago
		                                                    passed the walk's gate again, its
		                                                    test ran on no light) */
			world.nodes[i].dlstamp = 0;
		bmf_wrap();                                /* (the brush models' rooms' frames) */
		r_state.framecount = 1;
	}
	if (r_state.visframe >= 0xfff0) {
		for (uint32_t i = 0; i < world.num_nodes; i++)  world.nodes[i].visframe = 0;
		for (uint32_t i = 0; i < world.num_leaves; i++) world.leaves[i].visframe = 0;
		r_state.visframe   = 1;
		r_state.force_mark = 1;
	}

	*(volatile uint32_t *) 0x1f80035cu = 0;        /* SP_WATER (r_defs.inc): no liquid's surface
	                                                  but a monster's, set and cleared by
	                                                  draw_model (r_world.s dm_wsort, dm_exit_1) */
	r_state.ot_world     = (uint32_t) &vid_frame.ot[OT_WORLD];
	r_state.pkt          = (uint32_t) vid_frame.pkt;
	r_state.pkt_end      = (uint32_t) (vid_frame.end - QW_MAX_REC_PKT - 48 - r_ui_room);  /* see r_world.s */
	for (int k = 0; k < 3; k++) {                  /* mark_pvs's marks: again once the
	                                                  camera has moved R_MARK_MOVE */
		int32_t d = r_state.cam[k] - mark_cam[k];
		if (d > R_MARK_MOVE * QW_VSCALE || d < -R_MARK_MOVE * QW_VSCALE)
			r_state.force_mark = 1;
	}
#if Q1PSX_PROFILE
	r_state.stat_nodes   = r_state.stat_records = r_state.stat_verts = 0;
	r_state.stat_polys   = r_state.stat_skipped = 0;
	r_state.ticks_block  = r_state.ticks_mark = 0;
	{
		extern volatile uint32_t r_seq_stat[4];  /* (the cooked walk's tests, r_world.s) */
		extern volatile uint32_t r_tess_stat[6]; /* (the regular subdivision's, r_tess.s) */
		r_seq_stat[0] = r_seq_stat[1] = r_seq_stat[2] = r_seq_stat[3] = 0;
		r_tess_stat[0] = r_tess_stat[1] = r_tess_stat[2] = r_tess_stat[3] = r_tess_stat[4] = r_tess_stat[5] = 0;
	}

	uint32_t t0 = prof_now();
#ifdef Q1PSX_BENCH
	r_model_room_min = 0x7fffffff;
#endif
	r_render(&r_state);
	uint32_t t1 = prof_now();
#else
#ifdef Q1PSX_BENCH
	r_model_room_min = 0x7fffffff;
#endif
	r_render(&r_state);   /* release: no counters, no stage timers */
#endif
	if (r_state.visframe != mark_visframe) {       /* marked this frame */
		mark_visframe = r_state.visframe;
		for (int k = 0; k < 3; k++)
			mark_cam[k] = r_state.cam[k];
	}

#if !Q1PSX_RELEASE
	/* the debug overlay's triangles: fast-path quads are 26 bytes per
	   triangle (POLY_GT4, 52 B; the flat shading's POLY_FT4, 40 B: 20),
	   the slow path's are POLY_GT3 (40 B); the few window commands go
	   uncounted */
	uint32_t bytes = r_state.pkt - (uint32_t) vid_frame.pkt;
	uint32_t slowb = r_state.slow_bytes <= bytes ? r_state.slow_bytes : bytes;
	r_warp.tris = (bytes - slowb) / (opt_flat ? 20 : 26) + slowb / 40;
#endif
	r_warp.used   = r_state.pkt - (uint32_t) vid_frame.pkt;      /* (warp_room_floor's) */
	r_warp.avail  = r_state.pkt_end - (uint32_t) vid_frame.pkt;
	vid_frame.pkt = (uint8_t *) r_state.pkt;
	r_arena_full  = r_state.pkt >= r_state.pkt_end;   /* the walk stopped short: the farthest records lost */

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
