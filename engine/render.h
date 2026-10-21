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
#define R_SZ_FAST      96     /* records with a vertex nearer (z < 16 u) need the
                                 clip path (r_slow.c SZ_CLIP)               */

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
	uint32_t vis_lastmask;   /* +140 a PVS row's last byte: its leaves' bits */
	uint32_t ot_head;        /* +144 world-bucket link while asm calls C     */
	uint32_t stat_slow;      /* +148 triangles emitted by the slow path: its
	                                 POLY_GT3s, slow_bytes / 40 (render.c)   */
	uint32_t ticks_slow;     /* +152 timer-2 ticks spent in the slow path    */
	uint32_t textures;       /* +156 qw_tex * (CLUT, tpage, texture window)  */
	uint32_t vertpool;       /* +160 qw_vert *: the map's vertex pool        */
	uint32_t nodes;          /* +164 qw_node * (parents are word offsets)    */
	uint32_t planes;         /* +168 qw_plane *: the nodes' planes (1/8 u)   */
	uint32_t warp_t2;        /* +172 2 x the affine-error threshold (px)     */
	uint32_t warp_k16;       /* +176 record bound: no edge of a record whose
	                                 nearest depth z - 1.5r >= k16 x 1.5r / 16
	                                 can exceed it (porting-notes §19)       */
	uint32_t slow_bytes;     /* +180 packet bytes the slow path wrote       */
	uint32_t items;          /* +184 r_item_t *: the render list (rlist.h)   */
	uint32_t lists;          /* +188 uint16_t[256]: a leaf's list -> item + 1 */
	uint32_t rgb;            /* +192 uint32_t[256]: palette colour words     */
	uint32_t viewmodel;      /* +196 r_ent_t * drawn first (on top), or 0    */
	int32_t  ofy;            /* +200 the viewport's vertical centre (120, 96) */
	int32_t  vh2;            /* +204 the viewport's height (240, 192)         */
	uint32_t nlights;        /* +208 dynamic lights this frame (dlight.c)      */
	uint32_t lights;         /* +212 their table, 32 bytes each (DL_*)        */
	int32_t  mark_r;         /* +216 mark_pvs_far's reach, vertex units: a leaf
	                                 farther on an axis is not marked          */
	uint32_t fragpool;       /* +220 qw_vert *: the brush models' pieces'
	                                 vertices (bmfrag.c)                       */
} r_state_t;

extern r_state_t r_state;

void r_init(void);

/* The 3D viewport: the whole screen (240 rows) or the rows above the
   original status bar (192). Sets the GTE's projection centre. */
void r_set_viewport(int rows);
int  r_viewport_rows(void);

/* Camera for the next r_frame(): position in 1/R_POS_SUB world units (the
   player physics' own resolution), angles in binary degrees. */
#define R_POS_SUB 8
void r_set_view(const int32_t pos[3], int yaw, int pitch, int roll);

/* Render the world into the current frame's packet arena (asm). */
void r_frame(void);

/* Must be called after world_load(): binds the renderer to the new world. */
void r_bind_world(void);

/* Affine-warp control (docs/porting-notes.md §19.4). The subdivision's
   threshold T (px on screen) is one value per frame, so every edge's split
   decision stays the same on both sides whatever T a frame gets: no crack.
     R_WARP_FIXED    T as chosen (the bench's);
     R_WARP_DYNAMIC  T rises for the next frames when a frame overruns its
                     NTSC frame (CPU time or a missed vblank), and falls back
                     to the chosen T, its floor, once frames have room again
                     (the game's, from 4 px: engine/settings.h). */
enum { R_WARP_FIXED, R_WARP_DYNAMIC, R_WARP_MODES };
#define R_WARP_T_MAX 32       /* px: the dynamic mode stops raising T there */

typedef struct {
	int      mode;           /* R_WARP_*                                    */
	int      t;              /* the chosen threshold, px                    */
	int      t_eff;          /* the threshold the next frame renders with   */
	uint32_t tris;           /* the last frame's world triangles (estimate:
	                            the debug overlay's)                        */
	int      calm;           /* frames in a row with room to spare          */
} r_warp_t;

extern r_warp_t r_warp;

void r_set_warp(int mode, int t_px);

/* the last frame's walk ran out of packet arena (its farthest records,
   emitted last by the front-to-back walk, were not drawn) */
extern int r_arena_full;

/* packet bytes the walk leaves at the arena's end for what is drawn after
   the world (Quake's menu over the paused game: R_MENU_ROOM while it is
   up), set before r_frame */
#define R_MENU_ROOM (12 * 1024)
extern int r_ui_room;

/* After each frame: its CPU cycles (input to the end of the overlay) and
   the vblanks it took. Adjusts T for the next frame in the dynamic modes. */
void r_warp_feedback(uint32_t frame_cycles, uint32_t vbl, uint32_t share);   /* share: the
                                              vblanks the frame rate cap gives this frame */

/* The graphics options (the settings' Draw distance and Flat shading).
   dist: the draw distance, world units (0: off): the depth where the fog
   is black (from half of it), a record wholly beyond culled, the PVS's
   leaves beyond reach not marked, the background black; flat: R_FLAT_*,
   a far record (or any) drawn in one colour, its average light. Both off,
   the record loops run as they are: on, the entries into them are patched
   into their options' copies (r_world.s RECLOOP opt = 1, mark_pvs_far),
   so an option costs nothing while it is off. */
enum { R_FLAT_OFF, R_FLAT_FAR, R_FLAT_ALL, R_FLAT_MODES };
#define R_FLAT_FAR_UNITS 512       /* R_FLAT_FAR: records wholly beyond this depth */
#define R_SKY_RGB 40, 44, 60       /* the background: a flat "sky" until R3.4 */
void r_set_options(int dist, int flat);

/* particles deeper than this (view units, world x 6) are not drawn: where
   the fog passes half (0: all drawn); and a point's depth (1/R_POS_SUB
   world units) */
extern int32_t r_part_z;
int32_t r_view_depth(const int32_t pos[3]);

/* asm entry point (engine/r_world.s) */
void r_render(r_state_t *s);

/* slow path (engine/r_slow.c), called from the asm */
void r_slow(r_state_t *s, const uint8_t *rec, const uint8_t *poly, int nv);

int16_t r_sin(int a);   /* binary degrees -> x4096 */
int16_t r_cos(int a);
int     r_atan2(int32_t y, int32_t x);   /* -> binary degrees, 0..4095 */

#endif
