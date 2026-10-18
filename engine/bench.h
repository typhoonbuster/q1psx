/*
 * q1psx — benchmark accumulator (architecture.md §9).
 *
 * A scenario renders a few warm-up frames, then N measured frames; after each
 * measured frame bench_add() folds in prof_stage[] / prof_count[] and the
 * frame's vblank count. bench_print() emits one parseable line:
 *
 *   bench <name> n=<frames> vbl=<avg x100> <stage>=<avg>/<max> ... <count>=<avg>
 *
 * Only stages / counters that were non-zero are printed. scripts/bench.sh runs
 * the bench build headless and collects these lines.
 */

#ifndef Q1PSX_BENCH_H
#define Q1PSX_BENCH_H

#include <stdint.h>

#include "prof.h"

typedef struct {
	const char *name;
	uint32_t    frames;
	uint32_t    vbl_sum;
	uint32_t    stage_sum[PROF_STAGES];
	uint32_t    stage_max[PROF_STAGES];
	uint32_t    count_sum[PROF_COUNTERS];
	uint32_t    pkt_max;        /* the most arena bytes a frame used          */
	uint32_t    full_frames;    /* frames whose walk ran out of arena         */
} bench_acc_t;

void bench_reset(bench_acc_t *b, const char *name);
void bench_add(bench_acc_t *b, uint32_t vbl);
void bench_print(const bench_acc_t *b);

#endif
