#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxetc.h>
#include <hwregs_c.h>

#include "prof.h"

/* Root counter 2 mode bits (psx-spx "Timers"):
     5   IRQ when the counter reaches 0xFFFF
     6   IRQ repeatedly (not one-shot)
     8-9 clock source: 2 = sysclock/8                                      */
#define RC2_MODE ((1 << 5) | (1 << 6) | (2 << 8))

uint32_t prof_stage[PROF_STAGES];
uint32_t prof_count[PROF_COUNTERS];

const char *const prof_stage_name[PROF_STAGES] = {
	"frame", "sim", "r_setup", "r_walk", "r_block", "r_slow", "overlay", "ents", "dlight", "snd", "hud",
};
const char *const prof_count_name[PROF_COUNTERS] = {
	"nodes", "blocks", "verts", "polys", "slow", "slowtri", "dlnodes", "dlrecs", "dlverts",
};

static volatile uint32_t prof_hi;

static void rc2_wrap(void)
{
	prof_hi += 0x10000;
}

void prof_init(void)
{
	prof_hi = 0;
	InterruptCallback(IRQ_TIMER2, rc2_wrap);
	TIMER_RELOAD(2) = 0;
	TIMER_CTRL(2)   = RC2_MODE;   /* writing the mode also zeroes the counter */
	prof_frame_reset();
}

uint32_t prof_now(void)
{
	static uint32_t last;
	uint32_t hi, lo;
	do {
		hi = prof_hi;
		lo = TIMER_VALUE(2) & 0xffff;
	} while (hi != prof_hi);   /* a wrap IRQ landed between the two reads */
	/* The counter can also have wrapped with its IRQ still pending: the
	   reading then goes back by 0x10000 (it gave bogus 524 k-cycle maxima).
	   Time does not go back, and calls are far less than a wrap apart. */
	uint32_t now = hi | lo;
	if (now < last)
		now += 0x10000;
	last = now;
	return now;
}

#if defined(Q1PSX_BENCH) && Q1PSX_BENCH_SAMPLE
#define SAMPLE_BLOCKS 8192                 /* 256 KB of code, 32 bytes a block */
extern char __text_start[];
static uint16_t          sample_hist[SAMPLE_BLOCKS];
static volatile uint32_t sample_total, sample_out;

static void sample_isr(void)
{
	uint32_t epc;
	__asm__ volatile ("mfc0 %0, $14" : "=r" (epc));
	uint32_t i = (epc - (uint32_t) __text_start) >> 5;
	if (i < SAMPLE_BLOCKS) {
		if (sample_hist[i] < 0xffff)
			sample_hist[i]++;
	} else {
		sample_out++;                      /* the BIOS's, the kernel's */
	}
	sample_total++;
}

void prof_sample_start(void)
{
	memset(sample_hist, 0, sizeof sample_hist);
	sample_total = sample_out = 0;
	InterruptCallback(IRQ_TIMER1, sample_isr);
	TIMER_RELOAD(1) = 16934;               /* 33.87 MHz / 2000 */
	TIMER_CTRL(1)   = (1 << 3) | (1 << 4) | (1 << 6);   /* reset and IRQ at the target, repeatedly; sysclock */
}

void prof_sample_stop(void)
{
	TIMER_CTRL(1) = 0;
	InterruptCallback(IRQ_TIMER1, 0);
}

void prof_sample_print(const char *name)
{
	printf("sample %s total %u outside %u\n", name, (unsigned) sample_total, (unsigned) sample_out);
	int n = 0;                                 /* every block hit: "block:count", hex block */
	for (int i = 0; i < SAMPLE_BLOCKS; i++) {
		if (!sample_hist[i])
			continue;
		if (!n)
			printf("samples %s %08x", name, (unsigned) (uint32_t) __text_start);
		printf(" %x:%u", i, sample_hist[i]);
		if (++n == 24) {
			printf("\n");
			n = 0;
		}
	}
	if (n)
		printf("\n");
}
#else
void prof_sample_start(void) {}
void prof_sample_stop(void) {}
void prof_sample_print(const char *name) { (void) name; }
#endif

void prof_frame_reset(void)
{
	memset(prof_stage, 0, sizeof prof_stage);
	memset(prof_count, 0, sizeof prof_count);
}
