#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "prof.h"
#include "gdata.h"
#include "ui.h"
#include "vid.h"

typedef struct {
	DISPENV  disp;
	DRAWENV  draw;
	uint32_t ot[VID_OT_LEN];
	uint8_t  arena[VID_ARENA_BYTES] __attribute__((aligned(4)));
} vid_buffer_t;

static vid_buffer_t buf[2];
static int          back;          /* buffer the CPU is building into */
static uint32_t     last_vbl;
static uint8_t      cap_vbl[3] = { 1, 1, 1 };   /* vblanks a frame, in turn */
static int          cap_len = 1, cap_pos;
static int          viewport_rows = VID_H;
static uint32_t    *ovl_head, *ovl_tail;   /* this frame's overlay chain */

vid_frame_t vid_frame;
vid_stats_t vid_stats;

static void begin_buffer(int i)
{
	ClearOTagR(buf[i].ot, VID_OT_LEN);
	vid_frame.ot      = buf[i].ot;
	vid_frame.pkt     = buf[i].arena;
	vid_frame.end     = buf[i].arena + VID_ARENA_BYTES - VID_HUD_BYTES;
	vid_frame.dropped = 0;
	ovl_head = ovl_tail = NULL;
	if (viewport_rows < VID_H) {         /* the overlay draws over the whole screen */
		DR_AREA *a = vid_ovl_alloc(sizeof *a);
		if (a) {
			RECT r = { 0, (int16_t) (i * VID_H), VID_W, VID_H };
			setDrawArea(a, &r);
		}
	}
}

void vid_set_viewport(int rows)
{
	viewport_rows = rows;
	for (int i = 0; i < 2; i++)
		buf[i].draw.clip.h = (int16_t) rows;
}

int vid_viewport_rows(void)
{
	return viewport_rows;
}

void *vid_ovl_alloc_run(int bytes, int n)
{
	uint32_t *p = vid_alloc(bytes * n);
	if (!p)
		return NULL;
	uint32_t len = (uint32_t) (bytes - 4) / 4 << 24, *q = p;
	for (int i = 1; i < n; i++, q += bytes / 4)
		q[0] = len | (((uint32_t) q + (uint32_t) bytes) & 0xffffffu);
	q[0] = len | 0xffffffu;
	if (ovl_tail)
		*ovl_tail = (*ovl_tail & 0xff000000u) | ((uint32_t) p & 0xffffffu);
	else
		ovl_head = p;
	ovl_tail = q;
	return p;
}

void *vid_ovl_alloc(int bytes)
{
	uint32_t *p = vid_alloc(bytes);
	if (!p)
		return NULL;
	p[0] = (uint32_t) (bytes - 4) / 4 << 24 | 0xffffffu;   /* setlen, termPrim */
	if (ovl_tail)                                           /* setaddr, as words */
		*ovl_tail = (*ovl_tail & 0xff000000u) | ((uint32_t) p & 0xffffffu);
	else
		ovl_head = p;
	ovl_tail = p;
	return p;
}

/* a semi-transparent flat over the viewport, B/2 + F/2 (the GPU's blend
   mode 0, set by a DR_TPAGE for it): the water's, slime's and lava's tints */
void vid_tint(int r, int g, int b)
{
	DR_TPAGE *tp = vid_ovl_alloc(sizeof *tp);
	if (!tp)
		return;
	setDrawTPage(tp, 0, 0, getTPage(0, 0, 0, 0));
	TILE *t = vid_ovl_alloc(sizeof *t);
	if (!t)
		return;
	setTile(t);
	setSemiTrans(t, 1);
	setXY0(t, 0, 0);
	setWH(t, VID_W, viewport_rows);
	setRGB0(t, r, g, b);
}

static void blend_tile(int mode, int r, int g, int b)
{
	DR_TPAGE *tp = vid_ovl_alloc(sizeof *tp);
	if (!tp)
		return;
	setDrawTPage(tp, 0, 0, getTPage(0, mode, 0, 0));
	TILE *t = vid_ovl_alloc(sizeof *t);
	if (!t)
		return;
	setTile(t);
	setSemiTrans(t, 1);
	setXY0(t, 0, 0);
	setWH(t, VID_W, viewport_rows);
	setRGB0(t, r, g, b);
}

/* Quake lerps each palette colour toward a shift's colour (r += pct *
   (dest - r) >> 8); the GPU blends a flat only as B + F, B - F or
   (B + F) / 2. The lerps are taken exact on the mid grey: kept contrast
   from 3/4 up, its offset added and subtracted (B + F, B - F); below, the
   half blend whose flat lands the grey on its colour ((128 + F) / 2 = v) */
void vid_shift(const int v[3], int contrast)
{
	if (contrast >= 192) {
		int add[3], sub[3], any_add = 0, any_sub = 0;
		for (int k = 0; k < 3; k++) {
			int d = v[k] - 128;
			add[k] = d > 0 ? (d > 255 ? 255 : d) : 0;
			sub[k] = d < 0 ? (-d > 255 ? 255 : -d) : 0;
			any_add |= add[k];
			any_sub |= sub[k];
		}
		if (any_add)
			blend_tile(1, add[0], add[1], add[2]);
		if (any_sub)
			blend_tile(2, sub[0], sub[1], sub[2]);
		return;
	}
	int f[3];
	for (int k = 0; k < 3; k++) {
		f[k] = 2 * v[k] - 128;
		f[k] = f[k] < 0 ? 0 : f[k] > 255 ? 255 : f[k];
	}
	blend_tile(0, f[0], f[1], f[2]);
}

void vid_ovl_append(void *head, void *tail)
{
	if (ovl_tail)
		setaddr(ovl_tail, head);
	else
		ovl_head = head;
	ovl_tail = tail;
}

int vid_back(void)
{
	return back;
}

void vid_set_fps_cap(int fps)
{
	cap_pos = 0;
	if (fps == 45) {
		cap_vbl[0] = 1; cap_vbl[1] = 1; cap_vbl[2] = 2;   /* 3 frames in 4 vblanks */
		cap_len = 3;
		return;
	}
	cap_vbl[0] = (uint8_t) (fps == 30 ? 2 : fps == 15 ? 4 : 1);
	cap_len = 1;
}

int vid_frame_vbl(void)
{
	return cap_vbl[cap_pos];
}

void *vid_hud_block(int previous)
{
	return buf[back ^ previous].arena + VID_ARENA_BYTES - VID_HUD_BYTES;
}

void vid_init(void)
{
	ResetGraph(0);

	for (int i = 0; i < 2; i++) {
		SetDefDispEnv(&buf[i].disp, 0, i * VID_H, VID_W, VID_H);
		SetDefDrawEnv(&buf[i].draw, 0, i * VID_H, VID_W, VID_H);
		buf[i].draw.isbg = 1;
		buf[i].draw.dtd  = 1;   /* dither the shaded (lit) world: GP0(E1h)
		                           bit 9; polygons cannot set it themselves */
	}
	vid_set_bg(0, 0, 0);

	FntLoad(960, 0);

	back = 0;
	begin_buffer(back);
	last_vbl = (uint32_t) VSync(-1);
	memset(&vid_stats, 0, sizeof vid_stats);
	vid_stats.vbl = 1;

	SetDispMask(1);
}

void vid_set_bg(int r, int g, int b)
{
	for (int i = 0; i < 2; i++)
		setRGB0(&buf[i].draw, r, g, b);
}

void vid_set_dither(int on)
{
	for (int i = 0; i < 2; i++)
		buf[i].draw.dtd = on ? 1 : 0;
}

void *vid_alloc(int bytes)
{
	uint8_t *p = vid_frame.pkt;
	if (p + bytes > vid_frame.end) {
		vid_frame.dropped++;
		return NULL;
	}
	vid_frame.pkt = p + bytes;
	return p;
}

void vid_link(int bucket, void *packet)
{
	addPrim(&vid_frame.ot[bucket], packet);
}

void vid_text(int x, int y, const char *s)
{
	if (gdata.aux) {                     /* Quake's console font (ui.h) */
		ui_begin();
		ui_text(x, y, s);
		return;
	}
	/* FntSort emits one SPRT per glyph (~4 words) + a DR_TPAGE */
	int need = 20 * ((int) strlen(s) + 2);
	if (vid_frame.pkt + need > vid_frame.end) {
		vid_frame.dropped++;
		return;
	}
	vid_frame.pkt = FntSort(&vid_frame.ot[OT_OVERLAY], vid_frame.pkt, x, y, s);
}

void vid_flip(void)
{
	vid_buffer_t *drawn = &buf[back];

	if (ovl_head)                        /* the overlay chain, first in its bucket */
		addPrims(&vid_frame.ot[OT_OVERLAY], ovl_head, ovl_tail);
	vid_stats.pkt_bytes = (uint32_t) (vid_frame.pkt - drawn->arena);
	vid_stats.dropped   = vid_frame.dropped;

	uint32_t t0 = prof_now();
	vid_stats.gpu_busy = DrawSync(1) != 0;
	DrawSync(0);                     /* the previous frame is fully drawn */
	uint32_t t1 = prof_now();
	VSync(0);
	while ((uint32_t) VSync(-1) - last_vbl < cap_vbl[cap_pos])
		VSync(0);                    /* the cap: this frame's share of vblanks */
	cap_pos = cap_pos + 1 < cap_len ? cap_pos + 1 : 0;
	uint32_t t2 = prof_now();
	vid_stats.sync_cycles  = prof_cycles(t0, t1);
	vid_stats.vsync_cycles = prof_cycles(t1, t2);

	uint32_t now = (uint32_t) VSync(-1);
	vid_stats.vbl = now - last_vbl;
	last_vbl = now;

	/* show what the GPU just finished, start drawing what the CPU built */
	PutDispEnv(&buf[back ^ 1].disp);
	DrawOTagEnv(&drawn->ot[VID_OT_LEN - 1], &drawn->draw);

	back ^= 1;
	begin_buffer(back);
}

/* Print this frame's display list (draw order) to stdout for tools/pktview:
   one "P <hex words...>" line per packet, then "E". Bench builds only. */
void vid_dump(int hash_only)
{
	const uint32_t *ot = vid_frame.ot;
	if (ovl_head) {                      /* (as vid_flip will) */
		addPrims(&vid_frame.ot[OT_OVERLAY], ovl_head, ovl_tail);
		ovl_head = ovl_tail = NULL;
	}
	if (!hash_only)
		printf("V %d\n", viewport_rows);
	uint32_t at = (uint32_t) &ot[VID_OT_LEN - 1];
	uint32_t hash = 2166136261u, words = 0;   /* FNV-1a over the payloads */

	while ((at & 0xffffff) != 0xffffff) {
		const uint32_t *p = (const uint32_t *) (0x80000000u | (at & 0xffffff));
		uint32_t n = p[0] >> 24;
		if (n) {
			if (!hash_only)
				printf("P");
			for (uint32_t i = 1; i <= n; i++) {
				if (!hash_only)
					printf(" %08x", (unsigned) p[i]);
				hash = (hash ^ p[i]) * 16777619u;
			}
			if (!hash_only)
				printf("\n");
			words += n;
		}
		at = p[0];
	}
	/* the hash survives a console that drops or doubles characters (the
	   emulator's interpreter does on long dumps) */
	printf("E\nH %08x %u\n", (unsigned) hash, (unsigned) words);
}
