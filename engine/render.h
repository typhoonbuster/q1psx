/*
 * q1psx — world renderer (architecture.md §8).
 *
 * The per-frame work is entirely in MIPS assembly (engine/r_world.s). This C
 * side only prepares what asm needs once per frame (view matrices from the
 * camera angles, packet pointers, frame counters) and folds the asm's
 * statistics into the profiler.
 *
 * View conventions: Quake world coordinates (X forward at yaw 0, Y left, Z up),
 * angles in "binary degrees" (4096 = one turn), positive pitch looks down.
 * View space: x right, y down, z forward, scaled by R_VIEW_SCALE (world
 * vertices are x4, the matrix x1.5): this keeps the GTE's perspective divide
 * accurate down to ~13 units from the eye (it saturates at SZ <= H/2).
 */

#ifndef Q1PSX_RENDER_H
#define Q1PSX_RENDER_H

#include <stdint.h>

#define R_SCREEN_H     160    /* GTE projection distance: 90 deg horizontal FOV */
#define R_MATRIX_ONE  6144    /* 1.5 in GTE 1.3.12: view = world x 4 x 1.5 = x6 */
#define R_SZ_FAST      256    /* polys with a vertex nearer (z < ~43 u) are slow */

#define ANG_TURN 4096         /* binary-degree units per full turn */

/* Shared with asm: keep in sync with engine/r_defs.inc (checked at compile
   time in render.c). */
typedef struct {
	uint32_t root;           /* +0   qw_node *                              */
	uint32_t face_visframe;  /* +4   uint16_t * [num_faces]                 */
	uint32_t leaves;         /* +8   qw_leaf *  (leaf 0)                    */
	uint32_t rowbytes;       /* +12  PVS row bytes                          */
	int32_t  cam[3];         /* +16  camera, vertex units (world x 4)       */
	uint32_t framecount;     /* +28  stamps faces this frame (1..65535)     */
	uint32_t visframe;       /* +32  stamps PVS marks (1..65535)            */
	uint32_t view_leaf;      /* +36  qw_leaf * of the last PVS marking      */
	uint32_t ot_world;       /* +40  uint32_t * ordering-table bucket       */
	uint32_t pkt;            /* +44  packet write pointer (in/out)          */
	uint32_t pkt_end;        /* +48                                         */
	uint32_t sz_fast;        /* +52                                         */
	int16_t  rot[10];        /* +56  GTE rotation, 5 words (r11r12...r33)   */
	int32_t  tr[3];          /* +76  GTE translation                        */
	int16_t  absrot[10];     /* +88  |rot|, loaded as the GTE light matrix  */
	uint32_t stat_nodes;     /* +108 nodes visited                          */
	uint32_t stat_records;   /* +112 face records drawn                     */
	uint32_t stat_verts;     /* +116 vertices transformed                   */
	uint32_t stat_polys;     /* +120 polygons emitted                       */
	uint32_t stat_skipped;   /* +124 polygons skipped (near / overflow)     */
	uint32_t ticks_block;    /* +128 timer-2 ticks spent in face records    */
	uint32_t ticks_mark;     /* +132 timer-2 ticks spent in PVS marking     */
	uint32_t force_mark;     /* +136 non-zero: re-mark even if leaf same    */
	uint32_t leaves_end;     /* +140 qw_leaf * one past the last leaf      */
	uint32_t ot_head;        /* +144 world-bucket link while asm calls C     */
	uint32_t stat_slow;      /* +148 triangles emitted by the slow path      */
	uint32_t ticks_slow;     /* +152 timer-2 ticks spent in the slow path    */
	uint32_t textures;       /* +156 qw_tex * (CLUT, tpage, texture window)  */
	uint32_t vertpool;       /* +160 qw_vert *: the map's vertex pool        */
	uint32_t nodes;          /* +164 qw_node * (parents are word offsets)    */
	uint32_t planes;         /* +168 qw_plane *: the nodes' planes (1/8 u)   */
} r_state_t;

extern r_state_t r_state;

void r_init(void);

/* Camera for the next r_frame(): position in 1/R_POS_SUB world units (the
   player physics' own resolution), angles in binary degrees. */
#define R_POS_SUB 8
void r_set_view(const int32_t pos[3], int yaw, int pitch);

/* Render the world into the current frame's packet arena (asm). */
void r_frame(void);

/* Must be called after world_load(): binds the renderer to the new world. */
void r_bind_world(void);

/* asm entry point (engine/r_world.s) */
void r_render(r_state_t *s);

/* slow path (engine/r_slow.c), called from the asm */
void r_slow(r_state_t *s, const uint8_t *rec, const uint8_t *poly, int nv);

int16_t r_sin(int a);   /* binary degrees -> x4096 */
int16_t r_cos(int a);

#endif
