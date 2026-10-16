#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "prof.h"
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

vid_frame_t vid_frame;
vid_stats_t vid_stats;

static void begin_buffer(int i)
{
	ClearOTagR(buf[i].ot, VID_OT_LEN);
	vid_frame.ot      = buf[i].ot;
	vid_frame.pkt     = buf[i].arena;
	vid_frame.end     = buf[i].arena + VID_ARENA_BYTES;
	vid_frame.dropped = 0;
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

	vid_stats.pkt_bytes = (uint32_t) (vid_frame.pkt - drawn->arena);
	vid_stats.dropped   = vid_frame.dropped;

	uint32_t t0 = prof_now();
	vid_stats.gpu_busy = DrawSync(1) != 0;
	DrawSync(0);                     /* the previous frame is fully drawn */
	uint32_t t1 = prof_now();
	VSync(0);
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
void vid_dump(void)
{
	const uint32_t *ot = vid_frame.ot;
	uint32_t at = (uint32_t) &ot[VID_OT_LEN - 1];
	uint32_t hash = 2166136261u, words = 0;   /* FNV-1a over the payloads */

	while ((at & 0xffffff) != 0xffffff) {
		const uint32_t *p = (const uint32_t *) (0x80000000u | (at & 0xffffff));
		uint32_t n = p[0] >> 24;
		if (n) {
			printf("P");
			for (uint32_t i = 1; i <= n; i++) {
				printf(" %08x", (unsigned) p[i]);
				hash = (hash ^ p[i]) * 16777619u;
			}
			printf("\n");
			words += n;
		}
		at = p[0];
	}
	/* the hash survives a console that drops or doubles characters (the
	   emulator's interpreter does on long dumps) */
	printf("E\nH %08x %u\n", (unsigned) hash, (unsigned) words);
}
