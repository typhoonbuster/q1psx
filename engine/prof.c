#include <stdint.h>
#include <psxetc.h>
#include <hwregs_c.h>

#include "prof.h"

/* Root counter 2 mode bits (psx-spx "Timers"):
     5   IRQ when the counter reaches 0xFFFF
     6   IRQ repeatedly (not one-shot)
     8-9 clock source: 2 = sysclock/8                                      */
#define RC2_MODE ((1 << 5) | (1 << 6) | (2 << 8))

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
}

uint32_t prof_now(void)
{
	uint32_t hi, lo;
	do {
		hi = prof_hi;
		lo = TIMER_VALUE(2) & 0xffff;
	} while (hi != prof_hi);   /* a wrap IRQ landed between the two reads */
	return hi | lo;
}
