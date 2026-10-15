#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <psxgte.h>
#include <inline_c.h>

#include "prof.h"
#include "render.h"
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
_Static_assert(offsetof(r_state_t, leaves_end)   == 140, "r_state layout");
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

/* ------------------------------------------------------------------ view -- */

void r_init(void)
{
	build_sintab();
	InitGeom();
	gte_SetGeomOffset(VID_W / 2, VID_H / 2);
	gte_SetGeomScreen(R_SCREEN_H);
	memset(&r_state, 0, sizeof r_state);
	r_state.sz_fast = R_SZ_FAST;
}

void r_bind_world(void)
{
	r_state.root          = (uint32_t) world.root;
	r_state.face_visframe = (uint32_t) world.face_visframe;
	r_state.leaves        = (uint32_t) world.leaves;
	r_state.leaves_end    = (uint32_t) (world.leaves + world.num_leaves);
	r_state.rowbytes      = world.vis_rowbytes;
	r_state.textures      = (uint32_t) world.textures;
	r_state.vertpool      = (uint32_t) world.verts;
	r_state.nodes         = (uint32_t) world.nodes;
	r_state.planes        = (uint32_t) world.planes;
	r_state.framecount    = 1;
	r_state.visframe      = 1;
	r_state.view_leaf     = 0;
	r_state.force_mark    = 1;
	bound = 1;
}

void r_set_view(const int32_t pos[3], int yaw, int pitch)
{
	int32_t sy = r_sin(yaw), cy = r_cos(yaw), sp = r_sin(pitch), cp = r_cos(pitch);

	/* rows: right (sy,-cy,0), down = -up (-sp*cy,-sp*sy,-cp), forward
	   (cp*cy,cp*sy,-sp); x4096 * 1.5 */
	int32_t m[3][3] = {
		{  sy,             -cy,             0   },
		{ -(sp * cy) >> 12, -(sp * sy) >> 12, -cp },
		{  (cp * cy) >> 12,  (cp * sy) >> 12, -sp },
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
	r_state.pkt_end      = (uint32_t) (vid_frame.end - QW_MAX_REC_PKT - 8);   /* see r_world.s */
#if Q1PSX_PROFILE
	r_state.stat_nodes   = r_state.stat_records = r_state.stat_verts = 0;
	r_state.stat_polys   = r_state.stat_skipped = 0;
	r_state.ticks_block  = r_state.ticks_mark = 0;
	r_state.stat_slow    = 0;

	uint32_t t0 = prof_now();
	r_render(&r_state);
	uint32_t t1 = prof_now();
#else
	r_render(&r_state);   /* release: no counters, no stage timers */
#endif

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
	prof_count[PROF_N_SLOWTRI] += r_state.stat_slow;
#endif
}
