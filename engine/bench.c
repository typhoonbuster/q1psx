#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"
#include "prof.h"
#include "render.h"
#include "vid.h"

#if Q1PSX_BENCH_SLOWSTAT
extern volatile uint32_t r_slow_stat[16];     /* (engine/r_slow.s) */
static const char *const slowstat_name[9] = {
	"polys", "clipped", "popped", "onscreen", "leaves", "split", "refined", "mids", "forced",
};
#endif

void bench_reset(bench_acc_t *b, const char *name)
{
	memset(b, 0, sizeof *b);
	b->name = name;
#if Q1PSX_BENCH_SLOWSTAT
	for (int i = 0; i < 16; i++)
		r_slow_stat[i] = 0;
#endif
}

void bench_add(bench_acc_t *b, uint32_t vbl)
{
	b->frames++;
	if (vid_stats.pkt_bytes > b->pkt_max)
		b->pkt_max = vid_stats.pkt_bytes;
	b->full_frames += r_arena_full != 0;
	b->vbl_sum += vbl;
	b->gpu_sum += vid_stats.gpu_cycles;
	if (vid_stats.gpu_cycles > b->gpu_max)
		b->gpu_max = vid_stats.gpu_cycles;
	for (int i = 0; i < PROF_STAGES; i++) {
		b->stage_sum[i] += prof_stage[i];
		if (prof_stage[i] > b->stage_max[i])
			b->stage_max[i] = prof_stage[i];
	}
	for (int i = 0; i < PROF_COUNTERS; i++)
		b->count_sum[i] += prof_count[i];
#if Q1PSX_BENCH_IRQTIME
	extern volatile uint32_t q_irq_ticks, q_irq_count;   /* (engine/irq.s) */
	static uint32_t last_ticks, last_count;
	uint32_t t = q_irq_ticks, c = q_irq_count;
	if (b->frames > 1) {                   /* (since the last frame's) */
		b->irq_sum   += (t - last_ticks) * PROF_CYCLES_PER_TICK;
		b->irq_count += c - last_count;
	}
	last_ticks = t;
	last_count = c;
#endif
}

void bench_print(const bench_acc_t *b)
{
	uint32_t n = b->frames ? b->frames : 1;

	printf("bench %s n=%u vbl=%u", b->name, b->frames, b->vbl_sum * 100 / n);
	for (int i = 0; i < PROF_STAGES; i++)
		if (b->stage_max[i])
			printf(" %s=%u/%u", prof_stage_name[i],
			       b->stage_sum[i] / n, b->stage_max[i]);
	for (int i = 0; i < PROF_COUNTERS; i++)
		if (b->count_sum[i])
			printf(" %s=%u", prof_count_name[i], b->count_sum[i] / n);
	if (b->gpu_max)
		printf(" gpu=%u/%u", b->gpu_sum / n, b->gpu_max);
	if (b->irq_count && n > 1)             /* (IRQTIME: the handler's cycles a frame, interrupts x 100) */
		printf(" irq=%u/%u", b->irq_sum / (n - 1), b->irq_count * 100 / (n - 1));
	printf(" pkt=%u%s\n", b->pkt_max, b->full_frames ? " FULL" : "");
#if Q1PSX_BENCH_SLOWSTAT
	printf("slowstat %s", b->name);         /* (a frame's, on average) */
	for (int i = 0; i < 9; i++)
		printf(" %s=%u", slowstat_name[i], r_slow_stat[i] / n);
	printf("\n");
#endif
}
