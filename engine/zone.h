/*
 * q1psx — the world's zones (formats/qworld.h "zones", docs/porting-notes.md
 * §57): a zoned map keeps its skeleton in the level arena and streams its
 * zone chunks from <MAP>_Z<t>.ZON into three slots while the player moves.
 *
 *   - the camera leaf's zone is the one drawn: when the renderer finds the
 *     camera in a leaf of another set (engine/zone.s zone_mark, on the
 *     walk's camera-leaf call), zone_enter makes that set's zone current if
 *     its chunk is in: its patch words set the world nodes' face blocks
 *     (zone.s zone_patch), r_state.vertpool its pool;
 *   - a chunk not in yet ("late"): the old zone is drawn meanwhile, the
 *     late frames counted, the camera's zone read first;
 *   - between frames (zone_frame, before the stream's turn): a read's
 *     progress, the late switch, the next read: the camera's zone, then its
 *     nearest by walking distance (a teleporter one step), into a free
 *     slot, else the slot of a chunk no set wants now, else the least
 *     wanted's below it (never the drawn one). The drive is shared with the
 *     streamed assets (engine/stream.c, which waits for a chunk's read) and
 *     the SPU music (engine/music.c: music_read_ok), engine/cd.h's rules.
 *
 * Nothing here runs inside the renderer but zone_enter, before the walk.
 */

#ifndef Q1PSX_ZONE_H
#define Q1PSX_ZONE_H

#include <stdint.h>

#include "qworld.h"

/* engine/zone.s's (ZR_* there) */
typedef struct {
	uint32_t       leaves;     /* +0  world.leaves, KUSEG (the walk's leaf pointers)    */
	const uint8_t *leaf_set;   /* +4  [num_leaves]: a leaf's zone set                    */
	uint32_t       set;        /* +8  the camera leaf's set at the last call (0x100: none) */
	uint32_t       mark;       /* +12 the marking zone_mark goes on to (render.c)        */
} zone_rt_t;

extern zone_rt_t zone_rt;

typedef struct {
	uint32_t loads, load_bytes, load_frames;   /* chunks read in play, their bytes, the
	                                              frames their reads took             */
	uint32_t late_events, late_frames;         /* the camera in a zone not in; the frames
	                                              drawn with another zone             */
	uint32_t switches, switch_cycles, switch_max;   /* zones made current, the patch's cost */
	uint32_t evictions, sync_loads, read_errors;
	uint32_t waits;                            /* frames a read waited for the drive  */
} zone_stats_t;

extern zone_stats_t zone_stats;

/* world_load, a QWZF skeleton just read and linked: its zone tables, the
   slots allocated, the .ZON file found (texture level `tex`). 0 on
   failure (the map then fails to load). */
int  zone_map_start(const uint8_t *skeleton, const char *map, int tex);

/* the map being left or none: a read in flight stopped (the level arena is
   about to be reset), the zones off */
void zone_abort(void);

/* the map is zoned */
int  zone_on(void);

/* the map's start (the loading screen): the zone of the leaf holding `eye`
   (1/8 units) and its nearest read at once, that zone made current */
void zone_preload(const int32_t eye[3]);

/* once a frame between frames, before stream_frame: see above */
void zone_frame(void);

/* engine/zone.s, inside r_render before the walk: the camera leaf's set
   changed (QZ_NONE: the camera in solid, its zone kept) */
void zone_enter(uint32_t set);

/* the drive's: a chunk read in flight, or one placed and waiting for the
   drive (the stream starts none meanwhile) */
int  zone_reading(void);
int  zone_drive_wanted(void);

/* a chunk's patch words into the world nodes' face blocks (zone.s; the C
   engine's in zone.c): `nodes` the first patched node, `recs` the chunk's
   records in RAM */
void zone_patch(qw_node *nodes, const uint32_t *words, uint32_t n, uint8_t *recs);

#ifdef Q1PSX_BENCH
/* the bench: a camera leaf's zone not in is read at once, before its frame
   (0: as in play, late) */
extern int zone_sync;
/* before a frame (run_frame), the camera at `pos` (1/8 units): its leaf
   found as the renderer will (from r_state's camera arithmetic), its zone
   read at once if it is not in and made current (zone_sync) */
void zone_bench_frame(const int32_t pos[3]);
/* the leaf the renderer would find for `pos` (1/8 units) */
uint32_t zone_bench_leaf(const int32_t pos[3]);
/* the zone a leaf's set draws, QZ_NONE none */
int  zone_of_leaf(uint32_t leaf);
int  zone_current(void);
int  zone_count(void);
uint32_t zone_arena_bytes(void);       /* the slots' bytes in all */
uint32_t zone_skeleton_bytes(void);
/* the patch of the current chunk checked against the C one: the world
   nodes' face pointers (0 mismatches, or the first ones printed) */
int  zone_check_patch(void);
#endif

#endif
