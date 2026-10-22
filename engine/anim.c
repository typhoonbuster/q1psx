#include <stdint.h>
#include <psxgpu.h>

#include <stdio.h>

#include "anim.h"
#include "ents.h"
#include "qtex.h"
#include "vid.h"
#include "world.h"

#define ANIM_MAX 32
static int16_t shown[ANIM_MAX];          /* alt << 8 | frame in the display tile, -1 unknown */

void anim_reset(void)
{
	for (int i = 0; i < ANIM_MAX; i++)
		shown[i] = 0;                     /* (the cook cut the display from frame 0) */
}

void anim_invalidate(void)
{
	for (int i = 0; i < ANIM_MAX; i++)
		shown[i] = -1;
}

/* a tile's VRAM rectangle in the 4-bit set (four texels a halfword), its
   whole halfwords (the cook keeps an animated texture's tiles on them) */
static void tile_rect(const qw_anim_tile *t, RECT *r)
{
	r->x = (int16_t) (QTEX4_PAGE_X0 + 64 * t->page + t->u0 / 4);
	r->y = (int16_t) t->v0;
	r->w = (int16_t) ((t->w + 3) / 4);
	r->h = (int16_t) t->h;
}

static void move(int sx, int sy, int dx, int dy, int w, int h)
{
	DR_MOVE *m = vid_alloc(sizeof *m);
	if (!m)
		return;
	setlen(m, 4);
	m->code = 0x80;                      /* GP0(80h): VRAM to VRAM */
	m->p0 = m->p1 = m->p2 = 0;
	m->x0 = (uint16_t) sx; m->y0 = (uint16_t) sy;
	m->x1 = (uint16_t) dx; m->y1 = (uint16_t) dy;
	m->w  = (uint16_t) w;  m->h  = (uint16_t) h;
	vid_link(OT_SKY, m);                 /* the bucket drawn first */
}

void anim_frame(int tick)
{
	int n = (int) world.num_anims < ANIM_MAX ? (int) world.num_anims : ANIM_MAX, moved = 0;
	for (int c = 0; c < n; c++) {
		const qw_anim *a = &world.anims[c];
		int alt = !a->num_frames;        /* (a chain of alternates only: always) */
		for (int k = 0; k < a->num_models && !alt && a->num_alt; k++)
			alt = ents_model_frame(a->models[k]) != 0;
		int count = alt ? a->num_alt : a->num_frames;
		if (!count)
			continue;
		int f = (tick / 12) % count;      /* (int) (cl.time * 10) % anim_total / ANIM_CYCLE */
		int key = alt << 8 | f;
		if (shown[c] == key)
			continue;
		if (!moved) {                     /* GP0(01h): the texture cache cleared after the moves */
			uint32_t *cc = vid_alloc(8);
			if (!cc)
				return;
			setlen(cc, 1);
			cc[1] = 0x01000000u;
			vid_link(OT_SKY, cc);         /* (linked first: drawn after them) */
		}
		const qw_anim_tile *src = alt ? &a->alt[f] : &a->frames[f];
		RECT s, d;
		tile_rect(src, &s);
		tile_rect(&a->display, &d);
		move(s.x, s.y, d.x, d.y, s.w, s.h);
		move(16 * (src->slot % QTEX4_CLUTS_PER_ROW), QTEX4_CLUT_Y + src->slot / QTEX4_CLUTS_PER_ROW,
		     16 * (a->display.slot % QTEX4_CLUTS_PER_ROW),   /* its colours: one 16-entry */
		     QTEX4_CLUT_Y + a->display.slot / QTEX4_CLUTS_PER_ROW, 16, 1);   /* CLUT a texture */
		shown[c] = (int16_t) key;
		moved = 1;
#ifdef Q1PSX_BENCH
		if (ents_log)
			printf("anim: texture %d -> %s frame %d\n", c, alt ? "alternate" : "normal", f);
#endif
	}
}
