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
   between the expansions this frame draws, or every entry drawn) */
const qw_vert *mdl_frame(int a, const qst_model *m, int frame);

/* engine/mdl_expand.s: n vertex words into qw_vert's: x | y << 16, then
   z | normal << 16 (x, y, z shifted left by `shift` in the second) */
void mdl_expand(qw_vert *dst, const uint32_t *src, int n);
void mdl_expand_shift(qw_vert *dst, const uint32_t *src, int n, int shift);

/* the bench: expansions done, frames the cache could not take */
extern uint32_t mdl_stat_expanded, mdl_stat_refused;

#endif
