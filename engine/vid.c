#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxapi.h>
#include <psxgpu.h>

#include "mem.h"
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
#ifdef Q1PSX_BENCH
uint32_t    vid_shown_for[4];
#endif

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

/* The internal resolution's packets (docs/porting-notes.md §78). The target
   R is the viewport's bottom-right w x h corner, stretched over it by
   raw textured quads whose texture is the framebuffer (15-bit pages): the
   pixel (x, y) reads R's texel (x w / 320, y h / rows), rounded to the
   nearest (the GPU's, §71: w / 320 above 1/2 keeps it inside R; cut by an
   emulator that does not round, inside too). That source is never left
   of the pixel nor above it, and the GPU draws each triangle top-down,
   each row left to right: the stretch reads every pixel of R before it
   writes over it, in place. R starts left of x 128, so its first columns
   are page 64's, up to a column whose source is whole, the rest page
   128's (u up to 192 at x 320); its rows lie in one page (h <= rows - 16:
   render.c). A texel 0 is transparent: the mask bit set while R draws
   (GP0(E6h)) makes every pixel of R a texel the stretch draws. GP0(E6h)
   acts on VRAM transfers too: OT_SKY's are before it, and it is reset
   before the overlay (the next frame's). The texture cache is cleared
   before each quad (R is new; the second quad's page aliases the first's
   lines). The stretch covers the viewport: the frame's draw environment
   clears nothing then (isbg, set again once the list is kicked). */
__attribute__((noinline)) Q_COLD static uint32_t gp_area(int x0, int y0, int x1, int y1, uint32_t *w)
{
	w[0] = 0xe3000000u | (uint32_t) (y0 & 0x3ff) << 10 | (uint32_t) (x0 & 0x3ff);
	w[1] = 0xe4000000u | (uint32_t) (y1 & 0x3ff) << 10 | (uint32_t) (x1 & 0x3ff);
	return 0xe5000000u | (uint32_t) (y0 & 0x7ff) << 11 | (uint32_t) (x0 & 0x7ff);   /* (its offset) */
}

Q_COLD void vid_res_frame(int w, int h, int xs)
{
	int rows = viewport_rows, by = back * VID_H;  /* (this frame's buffer) */
	int rx = VID_W - w, ry = by + rows - h;       /* R's top left */
	uint32_t *t = vid_alloc((8 + 5 + 11 + 11) * 4), *u = t + 8;
	if (!t)
		return;
	DRAWENV *d = &buf[back].draw;
	d->isbg = 0;
	t[0] = 7u << 24 | 0xffffffu;
	t[3] = gp_area(rx, ry, VID_W - 1, by + rows - 1, &t[1]);
	t[4] = 0xe6000001u;                           /* the mask bit set while drawing */
	t[5] = 0x60000000u | (uint32_t) d->b0 << 16 | (uint32_t) d->g0 << 8 | d->r0;   /* R cleared */
	t[6] = 0;
	t[7] = (uint32_t) h << 16 | (uint32_t) w;
	addPrim(&vid_frame.ot[OT_TARGET], t);

	int page_y = ry >= 256 ? 256 : 0;
	uint32_t v0 = (uint32_t) (ry - page_y), v1 = v0 + (uint32_t) h, y1 = (uint32_t) rows << 16;
	uint32_t us = (uint32_t) (rx - 64 + xs * w / VID_W);
	u[0] = 4u << 24 | ((uint32_t) &u[5] & 0xffffffu);
	u[1] = 0xe6000000u;                           /* (before the overlay's transfers) */
	u[4] = gp_area(0, by, VID_W - 1, by + rows - 1, &u[2]);
	for (int i = 0; i < 2; i++) {
		uint32_t *q = &u[5 + 11 * i];
		uint32_t x0 = i ? (uint32_t) xs : 0, x1 = i ? VID_W : (uint32_t) xs;
		uint32_t u0 = i ? us - 64 : (uint32_t) rx - 64, u1 = i ? 192 : us;
		q[0] = 10u << 24 | ((uint32_t) &u[16] & 0xffffffu);   /* (the second's: addPrims) */
		q[1] = 0x01000000u;                       /* the texture cache cleared */
		q[2] = 0x2d808080u;                       /* textured quad, raw */
		q[3] = x0;
		q[4] = v0 << 8 | u0;
		q[5] = x1;
		q[6] = (uint32_t) getTPage(2, 0, i ? 128 : 64, page_y) << 16 | v0 << 8 | u1;
		q[7] = y1 | x0;
		q[8] = v1 << 8 | u0;
		q[9] = y1 | x1;
		q[10] = v1 << 8 | u1;
	}
	addPrims(&vid_frame.ot[OT_STRETCH], u, &u[16]);
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

void vid_dim(int n)
{
	if (n >= 5) {                                /* black: an opaque tile */
		TILE *t = vid_ovl_alloc(sizeof *t);
		if (!t)
			return;
		setTile(t);
		setXY0(t, 0, 0);
		setWH(t, VID_W, viewport_rows);
		setRGB0(t, 0, 0, 0);
		return;
	}
	while (n-- > 0)
		blend_tile(0, 0, 0, 0);                  /* (B + F) / 2, F black: B / 2 */
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

Q_COLD void vid_init(void)
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

#if !Q1PSX_RELEASE
	FntLoad(960, 0);                     /* (vid_text before the game data) */
#endif

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
#if !Q1PSX_RELEASE                       /* (the release loads the game data first:
                                            no SDK font, docs/porting-notes.md §60) */
	/* FntSort emits one SPRT per glyph (~4 words) + a DR_TPAGE */
	int need = 20 * ((int) strlen(s) + 2);
	if (vid_frame.pkt + need > vid_frame.end) {
		vid_frame.dropped++;
		return;
	}
	vid_frame.pkt = FntSort(&vid_frame.ot[OT_OVERLAY], vid_frame.pkt, x, y, s);
#endif
}

/* The stack reached its guard (mem.h MEM_STACK_GUARD): below it lies the
   level arena, which a deeper stack would overwrite unseen. The game stops
   here on a red screen rather than run on (docs/porting-notes.md §81.7). */
Q_COLD void vid_fatal(void)
{
	printf("vid: the stack reached its guard (%u bytes never reached): stopped\n",
	       (unsigned) mem_stack_untouched());
	DrawSync(0);
	FastEnterCriticalSection();
	volatile uint32_t *gp0 = (volatile uint32_t *) 0x1f801810;
	gp0[0] = 0x020000c0;                 /* GP0(02h) fill, red: both framebuffers */
	gp0[0] = 0;
	gp0[0] = (uint32_t) (2 * VID_H) << 16 | VID_W;
	for (;;)
		;
}

void vid_flip(void)
{
	vid_buffer_t *drawn = &buf[back];

	if (!mem_stack_ok())
		vid_fatal();

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
#ifdef Q1PSX_BENCH
	vid_shown_for[vid_stats.vbl < 4 ? vid_stats.vbl - 1 : 3]++;   /* (the buffer shown below's predecessor) */
#endif

	/* show what the GPU just finished, start drawing what the CPU built
	   (its clear: none under the internal resolution's stretch, for this
	   frame only) */
	PutDispEnv(&buf[back ^ 1].disp);
	DrawOTagEnv(&drawn->ot[VID_OT_LEN - 1], &drawn->draw);
	drawn->draw.isbg = 1;
#if defined(Q1PSX_BENCH) && Q1PSX_BENCH_GPUTIME
	uint32_t g0 = prof_now();              /* (the GPU's time: on an emulator that
	                                          counts it, DuckStation) */
	DrawSync(0);
	vid_stats.gpu_cycles = prof_cycles(g0, prof_now());
#endif

	back ^= 1;
	begin_buffer(back);
}

/* Print this frame's display list (draw order) to stdout for tools/pktview:
   one "P <hex words...>" line per packet, then "E". Bench builds only. The
   "H" line: the payloads' hash, in order; the "Q" line: the packets as a
   set (each one's hash summed), the texture-window commands left out, and
   their count: a change of order alone keeps it (docs/porting-notes.md
   §63). */
uint32_t vid_dump_hash;

void vid_dump(int hash_only)
{
	const uint32_t *ot = vid_frame.ot;
	if (ovl_head) {                      /* (as vid_flip will) */
		addPrims(&vid_frame.ot[OT_OVERLAY], ovl_head, ovl_tail);
		ovl_head = ovl_tail = NULL;
	}
	if (!hash_only)
		printf("V %d\n", viewport_rows);
	if (!hash_only && !buf[back].draw.isbg)  /* (the target's packets tell its size) */
		printf("R\n");
	uint32_t at = (uint32_t) &ot[VID_OT_LEN - 1];
	uint32_t hash = 2166136261u, words = 0;   /* FNV-1a over the payloads */
	uint32_t set = 0, windows = 0;

	while ((at & 0xffffff) != 0xffffff) {
		const uint32_t *p = (const uint32_t *) (0x80000000u | (at & 0xffffff));
		uint32_t n = p[0] >> 24;
		if (n) {
			if (!hash_only)
				printf("P");
			uint32_t one = 2166136261u;
			for (uint32_t i = 1; i <= n; i++) {
				if (!hash_only)
					printf(" %08x", (unsigned) p[i]);
				hash = (hash ^ p[i]) * 16777619u;
				one = (one ^ p[i]) * 16777619u;
			}
			if (!hash_only)
				printf("\n");
			words += n;
			if (n == 1 && p[1] >> 24 == 0xe2)
				windows++;
			else
				set += one;
		}
		at = p[0];
	}
	/* the hash survives a console that drops or doubles characters (the
	   emulator's interpreter does on long dumps) */
	printf("E\nH %08x %u\nQ %08x %u\n", (unsigned) hash, (unsigned) words, (unsigned) set, (unsigned) windows);
	vid_dump_hash = hash;
}
