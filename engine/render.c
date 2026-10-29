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
_Static_assert(offsetof(r_state_t, mark_r)       == 216, "r_state layout");
_Static_assert(offsetof(r_state_t, fragpool)     == 220, "r_state layout");
_Static_assert(offsetof(r_state_t, yrow)         == 224, "r_state layout");
_Static_assert(offsetof(r_state_t, seq)          == 236, "r_state layout");
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
static void mark_patch(void);

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
	95, 92, 89, 87, 85, 83, 81, 79, 77, 76, 74, 73, 72, 70, 69, 68, 67,
};

static int32_t yb_num = 1, yb_den = 1;   /* the view's y row x num / den: the rows'
                                            H over the columns' (256 x 224's; §78) */
static int32_t yb12 = 4096;              /* the same, x 4096 rounded */

/* T is in the target's pixels: below the native 320 the bound follows H's
   share, k16 = sqrt(560 x (2H / 320) / T) x 16 = sqrt(448 x 2H / T), H the
   larger of the columns' and the rows' (256 x 224: the rows' 149.3),
   rounded as the table is (it is that at H 160) */
Q_COLD static void warp_apply(int t)
{
	if (t < 1) t = 1;
	if (t > R_WARP_T_MAX) t = R_WARP_T_MAX;
	r_warp.t_eff     = t;
	r_state.warp_t2  = 2u * (uint32_t) t;
	uint32_t w2 = (uint32_t) ((r_res_w * yb_num + yb_den - 1) / yb_den);   /* 2H, up */
	if (w2 < (uint32_t) r_res_w)
		w2 = (uint32_t) r_res_w;
	r_state.warp_k16 = r_res ? (q_isqrt32(448u * 4u * w2 / (uint32_t) t) + 1) / 2 : warp_k16[t];
}

void r_set_warp(int mode, int t_px)
{
	r_warp.mode = mode;
	r_warp.t    = t_px;
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
   a frame at R_WARP_T_MAX missed them all the same (the view then drawn at
   the floor with a vblank more, as it would take that vblank anyway); back
   to the share once a frame at the floor is lighter than the first one
   there by what R_WARP_T_MAX lacked. */
#define WARP_FRAME  (33868800 / 60)
#define WARP_HIGH   (WARP_FRAME * 7 / 8)
#define WARP_LOW    (WARP_FRAME * 3 / 4)
#define WARP_CALM   4
#define WARP_STUCK  30       /* frames in a row at R_WARP_T_MAX missing the vblanks
                                aimed at before a vblank more: a view that cannot
                                make them (a load's or a view's first frames miss
                                them for a while) */

void r_warp_feedback(uint32_t frame_cycles, uint32_t vbl, uint32_t share)
{
	if (r_warp.mode != R_WARP_DYNAMIC)
		return;
	int t = r_warp.t_eff;
	if (r_warp.more && t <= r_warp.t) {           /* (at the floor: like for like) */
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
	if (vbl > aim && t >= R_WARP_T_MAX) {         /* the largest T missed them too */
		uint32_t s = frame_cycles > WARP_FRAME * aim ? frame_cycles - WARP_FRAME * aim : WARP_FRAME / 16;
		if (!r_warp.stuck || s < r_warp.shortfall)
			r_warp.shortfall = s;                 /* (the least, over the frames) */
		if (++r_warp.stuck >= WARP_STUCK) {
			r_warp.more++;
			r_warp.ref   = 0;
			r_warp.calm  = 0;
			r_warp.stuck = 0;
			warp_apply(r_warp.t);
			return;
		}
	} else {
		r_warp.stuck = 0;
	}
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

/* ------------------------------------------------------- the cooked walk -- */

/* the world's record loops' three ways out to the tree walk (r_world.s
   RECLOOP wx: dr and its options' copies) and draw_ents' way back, PC-
   relative branches, pointed at seq_next while a cooked walk runs */
extern uint32_t dr_xa[], dr_xb[], dr_xc[], dro1_xa[], dro1_xb[], dro1_xc[], dro2_xa[], dro2_xb[], dro2_xc[];
extern uint32_t dro3_xa[], dro3_xb[], dro3_xc[], ei_x[], seq_next[], seq_ents_back[];
static uint32_t *const seq_site[] = {
	dr_xa, dr_xb, dr_xc, dro1_xa, dro1_xb, dro1_xc, dro2_xa, dro2_xb, dro2_xc, dro3_xa, dro3_xb, dro3_xc, ei_x,
};
#define SEQ_SITES (int) (sizeof seq_site / sizeof seq_site[0])
static uint32_t seq_orig[SEQ_SITES];
#define SEQ_IMM16(w, v)  (((w) & 0xffff0000u) | ((uint32_t) (v) & 0xffffu))
static int      seq_mode = -1;           /* -1 as built (the tree's), 0 the tree, 1 the cooked walk */

/* the sites as the mode wants them (no cache flush here). A site holding
   a `j` is the options' (r_set_options puts one in a loop's first word,
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
   or nothing. The record loops' calls to the slow path (r_world.s
   RECLOOP: dr's and its options' copies', `jal`) go to tess_poly on
   those, to slow_poly on the others, whose frames stay the tree walk's. */
#ifndef Q1PSX_TESS_MAPS
#define Q1PSX_TESS_MAPS ""
#endif
int r_tess_on;
#if !Q1PSX_ENGINE_C
extern uint32_t dr_ts[], dro1_ts[], dro2_ts[], dro3_ts[];
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
	uint32_t *const site[] = { dr_ts, dro1_ts, dro2_ts, dro3_ts };
	uint32_t to = (uint32_t) (uintptr_t) (r_tess_on ? tess_poly : slow_poly);
	for (int i = 0; i < 4; i++)
		*site[i] = (3u << 26) | ((to >> 2) & 0x03ffffffu);   /* jal to */
	FlushCache();
#endif
}

/* --------------------------------------------------------------- options -- */

/* the fogged transform's table (r_world.s XFORM fog): by SZ / 64, a
   vertex's light factor x 128, from 128 at half the draw distance down to
   0 at it */
uint8_t r_fogtab[1024];
uint8_t r_vis_row[256];     /* the camera leaf's PVS row expanded (r_world.s vis_expand: the
                               marking reads it byte by byte; formats/qworld.h qw_leaf vis,
                               docs/porting-notes.md §90.9); the cook keeps rows under 256 */
int32_t r_part_z;

static int opt_dist, opt_flat;
static int32_t mark_cam[3];                  /* the camera at the last marking */
static uint32_t mark_visframe;
#define R_MARK_MOVE 64                       /* world units the camera moves before
                                                mark_pvs_far's marks are redone */

/* the record loops' entries, their options' copies (dro1: the draw
   distance, dro2: the flat shading, dro3: both; deo1: the models' draw
   distance), the patched words (r_world.s) */
extern uint32_t dr_records[], de_records[], dlr_ret0[], dlr_ret1[], dlr_ret2[], mark_call[];
extern uint32_t dro1_records[], dro2_records[], dro3_records[], deo1_records[];
extern uint32_t dro1_lit_ret[], dro2_lit_back[], dro3_lit_ret[];   /* (the draw distance's
                    copies: a stub first, their far depth lost to the light) */
extern uint32_t dro1_o_dcull[], dro1_o_fogz[], dro1_o_fogc[], dro2_o_flatz[];
extern uint32_t dro3_o_dcull[], dro3_o_fogz[], dro3_o_fogc[], dro3_o_flatz[], deo1_o_dcull[], deo1_o_fogz[];
extern uint32_t ds_o_dcull[], ds_o_fogz[];   /* the monsters' sorted loop: the draw distance's
                                                tests always (r_world.s dm_sort) */
extern uint32_t mark_pvs_far[];

enum { S_DR, S_DE, S_RET0, S_RET1, S_RET2, S_MARK, S_ENTRIES };
static uint32_t *const entry[S_ENTRIES] = { dr_records, de_records, dlr_ret0, dlr_ret1, dlr_ret2, mark_call };
static uint32_t orig[S_ENTRIES];             /* their words as built */
static uint32_t *const dcull[] = { dro1_o_dcull, dro3_o_dcull, deo1_o_dcull };
static uint32_t *const fogz[]  = { dro1_o_fogz, dro3_o_fogz, deo1_o_fogz };
static uint32_t *const fogc[]  = { dro1_o_fogc, dro3_o_fogc };
static uint32_t *const flatz[] = { dro2_o_flatz, dro3_o_flatz };
static uint32_t orig_flatz[2];

#define J_TO(op, a)   ((op) | (((uint32_t) (a) >> 2) & 0x03ffffffu))
#define SLTI(w, imm)  (((w) & 0xffff0000u) | ((uint32_t) (imm) & 0xffffu))
#define LI(w, v)      (0x24000000u | ((w) & 0x001f0000u) | (uint32_t) (v))   /* addiu rt, $zero, v */

/* the patched words as built, once */
static void keep_orig(void)
{
	if (orig[S_DR])
		return;
	for (int i = 0; i < S_ENTRIES; i++)
		orig[i] = *entry[i];
	for (int i = 0; i < 2; i++)
		orig_flatz[i] = *flatz[i];
}

/* The camera-leaf call (r_world.s mark_call): the marking the draw
   distance takes (mark_pvs, mark_pvs_far), through the zones' hook while a
   zoned map is loaded (engine/zone.s zone_mark: the leaf's zone made
   current before the walk, then on to the marking, zone_rt.mark). */
extern uint32_t zone_mark[];

static void mark_patch(void)
{
	keep_orig();
	uint32_t mark = opt_dist ? (uint32_t) mark_pvs_far
	              : ((uint32_t) mark_call & 0xf0000000u) | (orig[S_MARK] & 0x03ffffffu) << 2;   /* (jal mark_pvs) */
	zone_rt.mark = mark;
	*entry[S_MARK] = zone_on() ? J_TO(0x0c000000u, zone_mark) : J_TO(0x0c000000u, mark);
	FlushCache();
}

void r_set_options(int dist, int flat)
{
	keep_orig();
	opt_dist = dist;
	opt_flat = flat;
	/* view units (world x 6): the draw distance, the fog's start (its last
	   quarter: the fogged transform for as few records as can be) */
	int32_t dz = dist * 6, fz = dist * 9 / 2;
	for (int i = 0; i < 1024; i++) {
		int32_t z = i * 64 + 32;
		r_fogtab[i] = (uint8_t) (!dist || z <= fz ? 128 : z >= dz ? 0 : 128 * (dz - z) / (dz - fz));
	}
	r_part_z = dist ? (fz + dz) / 2 : 0;
	r_state.mark_r = (dist * 161 / 100 + R_MARK_MOVE) * QW_VSCALE;   /* (1.61: a frustum
	                    corner's distance over its depth, 1 / 0.6247 for the 240 rows) */
	for (unsigned i = 0; i < sizeof dcull / sizeof dcull[0]; i++) {
		*dcull[i] = SLTI(*dcull[i], dz);
		*fogz[i]  = SLTI(*fogz[i], fz);      /* the farthest depth: into the fog */
	}
	for (unsigned i = 0; i < sizeof fogc / sizeof fogc[0]; i++)
		*fogc[i] = SLTI(*fogc[i], fz);       /* the centre: in it (drawn flat) */
	*ds_o_dcull = SLTI(*ds_o_dcull, dist ? dz : 0x7fff);   /* none: at their most (a record */
	*ds_o_fogz  = SLTI(*ds_o_fogz, dist ? fz : 0x7fff);    /* nearer than 5461 units drawn, unfogged) */
	for (int i = 0; i < 2; i++)                  /* everywhere: li 0 (never nearer) */
		*flatz[i] = flat == R_FLAT_ALL ? LI(orig_flatz[i], 0) : SLTI(orig_flatz[i], R_FLAT_FAR_UNITS * 6);
	int v = (dist ? 1 : 0) | (flat ? 2 : 0);
	static uint32_t *const recs[4] = { 0, dro1_records, dro2_records, dro3_records };
	static uint32_t *const back[4] = { 0, dro1_lit_ret, dro2_lit_back, dro3_lit_ret };
	*entry[S_DR]   = v    ? J_TO(0x08000000u, recs[v]) : orig[S_DR];
	*entry[S_DE]   = dist ? J_TO(0x08000000u, deo1_records) : orig[S_DE];
	*entry[S_RET0] = v    ? (orig[S_RET0] & 0xffff0000u) |
	                        ((((uint32_t) back[v] - ((uint32_t) dlr_ret0 + 4)) >> 2) & 0xffffu) : orig[S_RET0];
	*entry[S_RET1] = v    ? J_TO(0x08000000u, back[v]) : orig[S_RET1];
	*entry[S_RET2] = v    ? J_TO(0x08000000u, back[v]) : orig[S_RET2];
	seq_apply();                                 /* (dr's first word back from the options: the
	                                                cooked walk's way out again if one runs) */
	FlushCache();
	mark_patch();
	r_state.force_mark = 1;
	mark_visframe = 0;
	if (dist)
		vid_set_bg(0, 0, 0);                 /* (the fog's colour: what is culled shows it) */
	else
		vid_set_bg(R_SKY_RGB);
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

/* The internal resolutions (docs/porting-notes.md §78): the width, the rows
   over the whole screen's 240, and the slow path's H = mul << shift, the
   GTE's half the width (the field of view kept): sp_proj's x 5 as built,
   x 15 and x 1 patched in its two words, x 25 its second entry. The
   stretch (vid.c) needs each a share of the screen above 1/2. */
const r_res_t r_res_table[R_RES_COUNT] = {
	{ 320, 240, 5, 5, 0, 0 }, { 256, 224, 1, 7, 80, 0 }, { 240, 180, 15, 3, 64, 0 }, { 200, 150, 25, 2, 16, 0 },
};
int r_res;
int r_res_w = VID_W, r_res_h = VID_H;
int r_proj_h = R_SCREEN_H;

/* The patched words (docs/porting-notes.md §78): the walk's SP_HSHIFT
   (r_world.s), the off-screen tests' bounds (every POLYS and the sorted
   models' lui v0, 240 / 320, as pairs; the slow path's), the frustum's
   vertical planes (r_ysites: 4y / 3z or 8y / 7z), the slow path's x H, y H,
   centre and entry (r_slow.s sp_proj; the C engine's slow path reads
   r_proj_h and r_res_w). */
extern uint32_t r_o_hshift[];
extern const uint32_t r_far_sites[], r_far_sites_end[], r_ysites[], r_ysites_end[];   /* (pairs: SITE_A, SITE_B) */
#if !Q1PSX_ENGINE_C
extern uint32_t sp_o_entry[], sp_o_xmul[], sp_o_ymul[], sp_o_ofx[], sp_o_bounds[], sp_proj_25[];
static uint32_t sp_entry_word;             /* sp_o_entry's own (its z's load) */
#endif

#define MIPS_SLL(rd, rt, sa)  (((uint32_t) (rt) << 16) | ((uint32_t) (rd) << 11) | ((uint32_t) (sa) << 6))
#define MIPS_ADDU(rd, rs, rt) (((uint32_t) (rs) << 21) | ((uint32_t) (rt) << 16) | ((uint32_t) (rd) << 11) | 0x21)
#define MIPS_SUBU(rd, rs, rt) (((uint32_t) (rs) << 21) | ((uint32_t) (rt) << 16) | ((uint32_t) (rd) << 11) | 0x23)
#define MIPS_J(to)            (0x08000000u | ((uint32_t) (to) >> 2 & 0x3ffffffu))
#define IMM16(w, v)           (((w) & 0xffff0000u) | ((uint32_t) (v) & 0xffffu))
/* a pair of sites in one word (r_world.s FARSITE, YSITE2): the first's
   address and kind (bits 0-1), the second's distance from it and kind in
   bits 21-30 (RAM addresses have none there): SITE_NEXT makes the second
   the first */
#define SITE_AT(w)            ((uint32_t *) (uintptr_t) ((w) & 0x801ffffcu))
#define SITE_NEXT(w)          (((w) & 0x801ffffcu) + ((w) >> 21 & 0x3ffu))

/* r x mul (5, 15 or 1) into r, at the scratch ($at, 1): two words, as
   sp_proj has them for 5 (sll at, r, 2; addu r, r, at) */
Q_COLD static void mul_words(uint32_t *w, int mul, int r)
{
	if (mul == 15) {
		w[0] = MIPS_SLL(1, r, 4);
		w[1] = MIPS_SUBU(r, 1, r);
	} else if (mul == 1) {
		w[0] = w[1] = 0;                       /* nop, nop */
	} else {
		w[0] = MIPS_SLL(1, r, 2);
		w[1] = MIPS_ADDU(r, r, 1);
	}
}

/* the off-screen tests' bounds, the far polygons' and the slow path's
   triangles': the target's (native: as built, 240 and 320, the whole
   screen's); the frustum's vertical planes: 3z -+ 4y and 8 r, or 7z -+ 8y
   and 24 r when the view's y row is scaled (r_world.s YSITE) */
Q_COLD static void res_patch_bounds(void)
{
	int rows = r_res ? r_res_h : VID_H, cols = r_res ? r_res_w : VID_W, an = yb_num != yb_den;
	for (const uint32_t *p = r_far_sites; p < r_far_sites_end; p++) {
		uint32_t *lr = SITE_AT(*p), *lc = SITE_AT(SITE_NEXT(*p));
		*lr = IMM16(*lr, rows);
		*lc = IMM16(*lc, cols);
	}
	static const uint32_t ys_mask[4] = { 31u << 6, 31u << 6, 63u, 31u << 6 | 31u << 16 };
	static const uint32_t ys_set[2][4] = {
		{ 2u << 6, 1u << 6, 0x21, 3u << 6 | 11u << 16 },   /* 4y, 3z = z << 1 + z, 8 r = r (t3) << 3 */
		{ 3u << 6, 3u << 6, 0x23, 4u << 6 | 15u << 16 },   /* 8y, 7z = z << 3 - z, 24 r = rho (t7) << 4 */
	};
	for (const uint32_t *p = r_ysites; p < r_ysites_end; p++)
		for (uint32_t s = *p, i = 0; i < 2; i++, s = SITE_NEXT(s)) {
			uint32_t *w = SITE_AT(s), k = s & 3u;
			*w = (*w & ~ys_mask[k]) | ys_set[an][k];
		}
#if !Q1PSX_ENGINE_C
	static const uint8_t col_w[3] = { 0, 1, 3 }, row_w[3] = { 5, 6, 8 };
	for (int i = 0; i < 3; i++) {
		sp_o_bounds[col_w[i]] = IMM16(sp_o_bounds[col_w[i]], -cols);
		sp_o_bounds[row_w[i]] = IMM16(sp_o_bounds[row_w[i]], -rows);
	}
#endif
	FlushCache();
}

/* The target: the viewport's rows in proportion, at most rows - 16 (its
   rows within one texture page: vid.c); its pixels' height over their
   width, the view's y row's scale (1 but for 256 x 224: 7 / 6). */
Q_COLD void r_set_viewport(int rows)
{
	const r_res_t *rs = &r_res_table[r_res];
	viewport_rows = rows;
	r_res_w = rs->w;
	r_res_h = rows;
	yb_num = yb_den = 1;
	if (r_res) {
		r_res_h = (rows * rs->h + VID_H / 2) / VID_H;
		if (r_res_h > rows - 16)
			r_res_h = rows - 16;
		yb_num = VID_W * r_res_h;              /* (r_res_h 160 / rows) / (w / 2) */
		yb_den = rows * r_res_w;
		if (yb_num == yb_den || r_ysites == r_ysites_end)
			yb_num = yb_den = 1;               /* (square, or a build without the sites) */
	}
	yb12 = (yb_num * 4096 + yb_den / 2) / yb_den;
	gte_SetGeomOffset(r_res_w / 2, r_res_h / 2);
	r_state.ofy = r_res_h / 2;
	r_state.vh2 = rows;
	res_patch_bounds();
}

Q_COLD void r_set_res(int i)
{
	if (i < 0 || i >= R_RES_COUNT)
		i = 0;
	r_res = i;
	const r_res_t *rs = &r_res_table[i];
	r_proj_h = rs->w / 2;
	gte_SetGeomScreen(r_proj_h);
	*r_o_hshift = IMM16(*r_o_hshift, rs->shift);
#if !Q1PSX_ENGINE_C
	if (!sp_entry_word)
		sp_entry_word = *sp_o_entry;
	*sp_o_entry = rs->mul == 25 ? MIPS_J(sp_proj_25) : sp_entry_word;
	mul_words(sp_o_xmul, rs->mul, 10);         /* x in t2 */
	mul_words(sp_o_ymul, rs->mul, 11);         /* y in t3 */
	*sp_o_ofx = IMM16(*sp_o_ofx, rs->w / 2);
#endif
	r_set_viewport(viewport_rows);             /* (the target, the centre, the bounds; FlushCache) */
	warp_apply(r_warp.t_eff);                  /* (the bound's share of H) */
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

/* 256 x 224: the view's y row kept as it is for the C's frustum tests
   (render.h yrow), then scaled to the rows' H (r_state.cam already set) */
__attribute__((noinline)) Q_COLD static void y_scale(int16_t r[9])
{
	r_state.yrow[0] = r[3];
	r_state.yrow[1] = r[4];
	r_state.yrow[2] = r[5];
	r_state.ytr = -((r[3] * r_state.cam[0] + r[4] * r_state.cam[1] + r[5] * r_state.cam[2]) >> 12);
	for (int i = 3; i < 6; i++)
		r[i] = (int16_t) ((r[i] * yb12 + 2048) >> 12);
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
	if (yb_num != yb_den)                          /* (pixels not square: the y row to */
		y_scale(r);                                /*  the rows' H, r_set_viewport) */
	for (int i = 0; i < 9; i++) {
		r_state.rot[i]    = r[i];
		r_state.absrot[i] = (int16_t) (r[i] < 0 ? -r[i] : r[i]);
	}
	r_state.rot[9] = r_state.absrot[9] = 0;
	for (int i = 0; i < 3; i++)
		r_state.tr[i] = -((r[i * 3] * r_state.cam[0] + r[i * 3 + 1] * r_state.cam[1] +
		                   r[i * 3 + 2] * r_state.cam[2]) >> 12);
	if (yb_num == yb_den) {                        /* (the C's frustum tests' y row) */
		r_state.yrow[0] = r[3];
		r_state.yrow[1] = r[4];
		r_state.yrow[2] = r[5];
		r_state.ytr     = r_state.tr[1];
	}
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

	if (r_res)                                     /* (the target's packets: before the view's) */
		vid_res_frame(r_res_w, r_res_h, r_res_table[r_res].seam);
	r_state.ot_world     = (uint32_t) &vid_frame.ot[OT_WORLD];
	r_state.pkt          = (uint32_t) vid_frame.pkt;
	r_state.pkt_end      = (uint32_t) (vid_frame.end - QW_MAX_REC_PKT - 48 - r_ui_room);  /* see r_world.s */
	if (opt_dist) {                                /* mark_pvs_far's marks: again once the
	                                                  camera has moved R_MARK_MOVE */
		for (int k = 0; k < 3; k++) {
			int32_t d = r_state.cam[k] - mark_cam[k];
			if (d > R_MARK_MOVE * QW_VSCALE || d < -R_MARK_MOVE * QW_VSCALE)
				r_state.force_mark = 1;
		}
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
	if (opt_dist && r_state.visframe != mark_visframe) {   /* marked this frame */
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
