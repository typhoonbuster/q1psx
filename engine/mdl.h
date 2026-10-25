/*
 * q1psx — the streamed models' frames for the renderer (formats/qstream.h):
 * a frame is kept compact (Quake's own vertex words) and expanded into the
 * renderer's 8-byte qw_vert pool when it is drawn, the expansions kept in a
 * small cache by (asset, frame stored): a monster holds a frame for a tenth
 * of a second (six frames at 60), and the same frame serves every monster
 * of the kind that shows it (and a body's frames equal to it: its frame
 * map's, formats/qstream.h QSTM_FMAP).
 */

#ifndef Q1PSX_MDL_H
#define Q1PSX_MDL_H

#include <stdint.h>

#include "qstream.h"
#include "qworld.h"

/* the map's start: the cache's memory (the level arena's), emptied */
void mdl_reset(void *buf, uint32_t bytes);

/* asset `a` left the pools: its expansions go */
void mdl_forget(int a);

/* each rendered frame, before its models: the expansions the last frame
   drew may be replaced again */
void mdl_new_frame(void);

/* the pool of frame `frame` of the resident model `a` (its header m),
   expanded: NULL when the cache cannot take it this frame (no gap for it
   between the expansions this frame draws, or every entry drawn). bkey:
   a model with a level of detail (QSTM_LOD): its band << 11, part of the
   key (the band's expansion: its vertices, the others their parents'); 0
   the whole model. engine/mdl_frame.s (the C engine: engine/mdl.c's) */
const qw_vert *mdl_frame(int a, const qst_model *m, int frame, uint32_t bkey);

/* mdl_frame's miss: frame f (stored) of model a, not in the cache: its
   bytes, its entry, its expansion */
const qw_vert *mdl_expand_new(int a, const qst_model *m, uint32_t f, uint32_t bkey);

/* engine/mdl_expand.s: n vertex words into qw_vert's: x | y << 16, then
   z | normal << 16 (x, y, z shifted left by `shift` in the second) */
void mdl_expand(qw_vert *dst, const uint32_t *src, int n);
void mdl_expand_shift(qw_vert *dst, const uint32_t *src, int n, int shift);

/* engine/mdl_expand.s: the level of detail's collapsed vertices: n qw_vert's
   from dst on, each the copy of its parent's (par[i], an index into the
   pool, lower than its own: in order, a parent's is final) */
void mdl_lod_copy(qw_vert *dst, const uint16_t *par, int n, const qw_vert *pool);

/* the bench: expansions done, frames the cache could not take */
extern uint32_t mdl_stat_expanded, mdl_stat_refused;

#endif
