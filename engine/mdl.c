/*
 * q1psx — the streamed models' expanded frames: a ring of qw_vert pools in
 * the level arena, by (asset, frame). See engine/mdl.h.
 */

#include <stdint.h>
#include <string.h>

#include "mdl.h"

#define ENTRIES 32
#define NOWHERE 0xffffffffu      /* a free entry's ofs: past any bytes asked */

typedef struct {
	int16_t  asset;          /* -1: free                                      */
	uint16_t frame;          /* the frame stored (qst_frame_index)            */
	uint32_t ofs, end;       /* its bytes in the ring (ofs NOWHERE: free)     */
	uint32_t stamp;          /* the rendered frame that last drew it          */
} entry_t;

static entry_t  entries[ENTRIES];
static uint8_t *ring;
static uint32_t ring_size, head;
static uint32_t stamp = 1;

uint32_t mdl_stat_expanded, mdl_stat_refused;

void mdl_reset(void *buf, uint32_t bytes)
{
	ring      = buf;
	ring_size = buf ? bytes & ~7u : 0;
	head      = 0;
	for (int i = 0; i < ENTRIES; i++) {
		entries[i].asset = -1;
		entries[i].ofs   = NOWHERE;
	}
}

void mdl_forget(int a)
{
	for (int i = 0; i < ENTRIES; i++)
		if (entries[i].asset == a) {
			entries[i].asset = -1;
			entries[i].ofs   = NOWHERE;
		}
}

void mdl_new_frame(void)
{
	stamp++;
}

/* frame f (stored) of model a, not in the cache: its bytes, its entry, its
   expansion (apart from mdl_frame, whose hits then save no register) */
static __attribute__((noinline)) const qw_vert *expand_new(int a, const qst_model *m, uint32_t f)
{
	uint32_t need = (uint32_t) m->num_verts * sizeof(qw_vert);
	if (!ring || need > ring_size) {
		mdl_stat_refused++;
		return 0;
	}
	/* its bytes, in one piece: at the head, else just past the expansions
	   this frame draws in the way (they never move), else from the ring's
	   start; the entries of the others there go (`gone`) */
	uint32_t at = head, gone;
	int wrapped = 0;
	for (;;) {
		if (at + need > ring_size) {
			if (wrapped) {                    /* no room but this frame's */
				mdl_stat_refused++;
				return 0;
			}
			at = 0;
			wrapped = 1;
		}
		uint32_t lim = at + need, past = at;
		gone = 0;
		for (int i = 0; i < ENTRIES; i++) {
			const entry_t *e = &entries[i];
			if (e->ofs >= lim || e->end <= at)
				continue;
			if (e->stamp != stamp)
				gone |= 1u << i;
			else if (e->end > past)
				past = e->end;
		}
		if (past == at)
			break;
		at = past;
	}
	/* its entry: one of those, else a free one, else the one drawn longest
	   ago (not this frame) */
	int slot = 0;
	if (gone) {
		for (int i = 0; gone; i++, gone >>= 1)
			if (gone & 1) {
				entries[i].asset = -1;
				entries[i].ofs   = NOWHERE;
				slot = i;
			}
	} else {
		for (int i = 0; i < ENTRIES; i++) {
			if (entries[i].asset < 0) {
				slot = i;
				break;
			}
			if (entries[i].stamp < entries[slot].stamp)
				slot = i;
		}
		if (entries[slot].asset >= 0 && entries[slot].stamp == stamp) {
			mdl_stat_refused++;               /* every entry drawn this frame */
			return 0;
		}
	}
	entry_t *e = &entries[slot];
	e->asset = (int16_t) a;
	e->frame = (uint16_t) f;
	e->ofs   = at;
	e->end   = at + need;
	e->stamp = stamp;
	head = at + need;
	qw_vert *dst = (qw_vert *) (ring + at);
	const uint32_t *src = (const uint32_t *) ((const uint8_t *) m + m->ofs_frames) + f * m->num_verts;
	if (m->shift)
		mdl_expand_shift(dst, src, m->num_verts, m->shift);
	else
		mdl_expand(dst, src, m->num_verts);
	mdl_stat_expanded++;
	return dst;
}

const qw_vert *mdl_frame(int a, const qst_model *m, int frame)
{
	uint32_t f = qst_frame_index(m, frame);
	for (int i = 0; i < ENTRIES; i++) {
		entry_t *e = &entries[i];
		if (e->asset == a && e->frame == f) {
			e->stamp = stamp;
			return (const qw_vert *) (ring + e->ofs);
		}
	}
	return expand_new(a, m, f);
}

#if Q1PSX_ENGINE_C
/* engine/mdl_expand.s in C (the C engine, CMake Q1PSX_ENGINE_C), the asm's
   way: word w gives x | y << 16 = w & 0x00ff00ff and z | normal << 16 =
   (w >> 8) & 0x00ff00ff, two stores a vertex */
void mdl_expand(qw_vert *dst, const uint32_t *src, int n)
{
	uint32_t *d = (uint32_t *) dst;
	for (int i = 0; i < n; i++) {
		uint32_t w = src[i];
		d[2 * i]     = w & 0x00ff00ff;
		d[2 * i + 1] = w >> 8 & 0x00ff00ff;
	}
}

/* x, y and z shifted left (each in its half), the normal kept */
void mdl_expand_shift(qw_vert *dst, const uint32_t *src, int n, int shift)
{
	uint32_t *d = (uint32_t *) dst;
	for (int i = 0; i < n; i++) {
		uint32_t w = src[i];
		d[2 * i]     = (w & 0x00ff00ff) << shift;
		d[2 * i + 1] = (w >> 8 & 0xff) << shift | (w >> 8 & 0x00ff0000);
	}
}
#endif
