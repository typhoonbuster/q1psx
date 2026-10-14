#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"
#include "prof.h"

void bench_reset(bench_acc_t *b, const char *name)
{
	memset(b, 0, sizeof *b);
	b->name = name;
}

void bench_add(bench_acc_t *b, uint32_t vbl)
{
	b->frames++;
	b->vbl_sum += vbl;
	for (int i = 0; i < PROF_STAGES; i++) {
		b->stage_sum[i] += prof_stage[i];
		if (prof_stage[i] > b->stage_max[i])
			b->stage_max[i] = prof_stage[i];
	}
	for (int i = 0; i < PROF_COUNTERS; i++)
		b->count_sum[i] += prof_count[i];
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
	printf("\n");
}
