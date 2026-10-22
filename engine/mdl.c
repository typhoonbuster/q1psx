/*
 * q1psx — the streamed models' expanded frames: a ring of qw_vert pools in
 * the level arena, by (asset, frame). See engine/mdl.h.
 */

#include <stdint.h>
#include <string.h>

#include "mdl.h"

#define ENTRIES 32

typedef struct {
	int16_t  asset;          /* -1: free                                      */
	uint16_t frame;          /* the frame stored (qst_frame_index)            */
	uint32_t ofs, len;       /* in the ring                                   */
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
	for (int i = 0; i < ENTRIES; i++)
		entries[i].asset = -1;
}

void mdl_forget(int a)
{
	for (int i = 0; i < ENTRIES; i++)
		if (entries[i].asset == a)
			entries[i].asset = -1;
}

void mdl_new_frame(void)
{
	stamp++;
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
	/* an entry: a free one, else the one drawn longest ago (not this frame) */
	int slot = -1;
	for (int i = 0; i < ENTRIES && slot < 0; i++)
		if (entries[i].asset < 0)
			slot = i;
	if (slot < 0) {
		slot = 0;
		for (int i = 1; i < ENTRIES; i++)
			if (entries[i].stamp < entries[slot].stamp)
				slot = i;
	}
	uint32_t need = (uint32_t) m->num_verts * sizeof(qw_vert);
	if (!ring || need > ring_size || (entries[slot].asset >= 0 && entries[slot].stamp == stamp)) {
		mdl_stat_refused++;
		return 0;
	}
	if (head + need > ring_size)
		head = 0;
	/* the ring's next bytes: what they held goes, unless this frame draws it */
	for (int i = 0; i < ENTRIES; i++) {
		entry_t *e = &entries[i];
		if (e->asset >= 0 && e->ofs < head + need && head < e->ofs + e->len && e->stamp == stamp) {
			mdl_stat_refused++;
			return 0;
		}
	}
	for (int i = 0; i < ENTRIES; i++) {
		entry_t *e = &entries[i];
		if (e->asset >= 0 && e->ofs < head + need && head < e->ofs + e->len)
			e->asset = -1;
	}
	entry_t *e = &entries[slot];
	e->asset = (int16_t) a;
	e->frame = (uint16_t) f;
	e->ofs   = head;
	e->len   = need;
	e->stamp = stamp;
	head += need;
	qw_vert *dst = (qw_vert *) (ring + e->ofs);
	const uint32_t *src = (const uint32_t *) ((const uint8_t *) m + m->ofs_frames) + f * m->num_verts;
	if (m->shift)
		mdl_expand_shift(dst, src, m->num_verts, m->shift);
	else
		mdl_expand(dst, src, m->num_verts);
	mdl_stat_expanded++;
	return dst;
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
