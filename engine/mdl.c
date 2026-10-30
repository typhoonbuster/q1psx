/*
 * q1psx — the streamed models' expanded frames: a ring of qw_vert pools in
 * the level arena, by (asset, frame). See engine/mdl.h.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mdl.h"

#define ENTRIES 32
#define FREE    0xffffffffu      /* a free entry's key: no (asset, frame)    */
#define NOWHERE 0xffffffffu      /* a free entry's ofs: past any bytes asked */

/* the entries by field, the lookup's keys together (engine/mdl_frame.s's
   MC_* offsets) */
typedef struct {
	uint32_t key[ENTRIES + 4];   /* asset << 16 | the frame stored
	                                (qst_frame_index); FREE. [ENTRIES]: the
	                                lookup's sentinel, the 3 after it read
	                                past it                                 */
	uint32_t ofs[ENTRIES];       /* its bytes in the ring: ofs..end (ofs     */
	uint32_t end[ENTRIES];       /* NOWHERE: free)                           */
	uint32_t stamp[ENTRIES];     /* the rendered frame that last drew it     */
	uint8_t *ring;
	uint32_t now;                /* this rendered frame's stamp              */
} mdl_cache_t;

_Static_assert(offsetof(mdl_cache_t, ofs) == 144 && offsetof(mdl_cache_t, stamp) == 400 &&
               offsetof(mdl_cache_t, ring) == 528 && offsetof(mdl_cache_t, now) == 532,
               "engine/mdl_frame.s MC_*");

mdl_cache_t mdl_cache = { .now = 1 };
static uint32_t ring_size, head;

uint32_t mdl_stat_expanded, mdl_stat_refused, mdl_stat_lerped;

void mdl_reset(void *buf, uint32_t bytes)
{
	mdl_cache.ring = buf;
	ring_size = buf ? bytes & ~7u : 0;
	head = 0;
	for (int i = 0; i < ENTRIES; i++) {
		mdl_cache.key[i] = FREE;
		mdl_cache.ofs[i] = NOWHERE;
	}
}

void mdl_forget(int a)
{
	for (int i = 0; i < ENTRIES; i++)
		if (mdl_cache.key[i] >> 16 == (uint32_t) a) {
			mdl_cache.key[i] = FREE;
			mdl_cache.ofs[i] = NOWHERE;
		}
}

void mdl_new_frame(void)
{
	mdl_cache.now++;
}

const qw_vert *mdl_expand_new(int a, const qst_model *m, uint32_t f, uint32_t bkey)
{
	mdl_cache_t *c = &mdl_cache;
	uint32_t need = (uint32_t) m->num_verts * sizeof(qw_vert);
	if (!c->ring || need > ring_size) {
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
			if (c->ofs[i] >= lim || c->end[i] <= at)
				continue;
			if (c->stamp[i] != c->now)
				gone |= 1u << i;
			else if (c->end[i] > past)
				past = c->end[i];
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
				c->key[i] = FREE;
				c->ofs[i] = NOWHERE;
				slot = i;
			}
	} else {
		for (int i = 0; i < ENTRIES; i++) {
			if (c->key[i] == FREE) {
				slot = i;
				break;
			}
			if (c->stamp[i] < c->stamp[slot])
				slot = i;
		}
		if (c->key[slot] != FREE && c->stamp[slot] == c->now) {
			mdl_stat_refused++;               /* every entry drawn this frame */
			return 0;
		}
	}
	c->key[slot]   = (uint32_t) a << 16 | bkey | f;
	c->ofs[slot]   = at;
	c->end[slot]   = at + need;
	c->stamp[slot] = c->now;
	head = at + need;
	qw_vert *dst = (qw_vert *) (c->ring + at);
	const uint32_t *frames = (const uint32_t *) ((const uint8_t *) m + m->ofs_frames);
	const uint32_t *src = frames + f * m->num_verts;
	/* the level of detail's band (QSTM_LOD): its vertices expanded, the
	   others copied from their parents (the first n of a frame are the
	   band's: formats/qstream.h) */
	int n = m->num_verts;
	const qst_lod *l = 0;
	if (bkey) {
		l = qst_lod_of(m);
		n = l->num_verts[bkey >> 11];
	}
	if (f & QST_LERP) {                       /* a frame its neighbours make (§97): their
		                                         average's words in the pool's upper half,
		                                         expanded in place (each read before its
		                                         place is written) */
		const uint16_t *map = (const uint16_t *) (m + 1);
		uint32_t q = f & (QST_LERP - 1), *words = (uint32_t *) dst + n;
		mdl_frame_avg(words, frames + map[q - 1] * m->num_verts, frames + map[q + 1] * m->num_verts, n);
		src = words;
		mdl_stat_lerped++;
	}
	if (m->shift)
		mdl_expand_shift(dst, src, n, m->shift);
	else
		mdl_expand(dst, src, n);
	if (n < m->num_verts)
		mdl_lod_copy(dst + n, qst_lod_parents(l) + (n - l->num_min), m->num_verts - n, dst);
	mdl_stat_expanded++;
	return dst;
}

#if Q1PSX_ENGINE_C
/* engine/mdl_frame.s in C (the C engine, CMake Q1PSX_ENGINE_C): the key
   looked for from the first entry, the sentinel past the last ending the
   scan */
const qw_vert *mdl_frame(int a, const qst_model *m, int frame, uint32_t bkey)
{
	uint32_t f = qst_frame_index(m, frame), key = (uint32_t) a << 16 | bkey | f;
	mdl_cache.key[ENTRIES] = key;
	int i = 0;
	while (mdl_cache.key[i] != key)
		i++;
	if (i == ENTRIES)
		return mdl_expand_new(a, m, f, bkey);
	mdl_cache.stamp[i] = mdl_cache.now;
	return (const qw_vert *) (mdl_cache.ring + mdl_cache.ofs[i]);
}

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

/* engine/mdl_expand.s's mdl_frame_avg in C (the C engine): each vertex
   word's bytes floor-averaged (the SWAR way), a's normal */
void mdl_frame_avg(uint32_t *dst, const uint32_t *a, const uint32_t *b, int n)
{
	for (int i = 0; i < n; i++) {
		uint32_t x = a[i], y = b[i];
		uint32_t v = (x & y) + (((x ^ y) & 0xfefefefeu) >> 1);
		dst[i] = (v & 0x00ffffffu) | (x & 0xff000000u);
	}
}

/* engine/mdl_expand.s's mdl_lod_copy in C (the C engine): in order, each
   collapsed vertex its parent's qw_vert */
void mdl_lod_copy(qw_vert *dst, const uint16_t *par, int n, const qw_vert *pool)
{
	for (int i = 0; i < n; i++)
		dst[i] = pool[par[i]];
}
#endif
