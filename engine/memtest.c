/*
 * q1psx — boot-time self-test for the Hunk/Zone allocator (phase 2.4).
 *
 * Runs a battery of allocations against a small private block and prints
 * PASS/FAIL lines to serial. Not linked into a release build later; for now it
 * is the concrete validation that Memory_Init / Hunk_* / Z_* behave.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zone.h"
#include "memtest.h"

static int failures;

#define CHECK(cond, msg) do {                                   \
		if (cond) {                                             \
			printf("  PASS  %s\n", msg);                        \
		} else {                                                \
			printf("  FAIL  %s\n", msg);                        \
			failures++;                                         \
		}                                                       \
	} while (0)

static int is_zero(const void *p, int n)
{
	const uint8_t *b = p;
	for (int i = 0; i < n; i++)
		if (b[i])
			return 0;
	return 1;
}

int mem_selftest(void *block, int block_size)
{
	failures = 0;
	printf("q1psx mem selftest: block=%p size=%d\n", block, block_size);

	Memory_Init(block, block_size);

	/* --- low hunk --- */
	int mark0 = Hunk_LowMark();

	void *a = Hunk_AllocName(100, "a");
	void *b = Hunk_AllocName(200, "b");
	void *c = Hunk_AllocName(1, "c");

	CHECK(a && b && c, "hunk allocs returned non-null");
	CHECK(((uintptr_t) a & 15) == 0 && ((uintptr_t) b & 15) == 0 &&
	      ((uintptr_t) c & 15) == 0, "hunk allocs 16-byte aligned");
	CHECK((char *) b >= (char *) a + 100, "b does not overlap a");
	CHECK((char *) c >= (char *) b + 200, "c does not overlap b");
	CHECK(is_zero(b, 200), "hunk memory is zero-filled");

	memset(a, 0xAA, 100);
	memset(b, 0xBB, 200);

	int used_before = Hunk_LowUsed();
	Hunk_FreeToLowMark(mark0);
	CHECK(Hunk_LowUsed() == mark0, "FreeToLowMark reset the low pointer");
	CHECK(used_before > mark0, "low hunk had actually grown");

	void *a2 = Hunk_AllocName(100, "a2");
	CHECK(a2 == a, "reallocating after free reuses the same address");
	CHECK(is_zero(a2, 100), "reused block was re-zeroed");

	/* --- high hunk + temp --- */
	int hmark = Hunk_HighMark();
	void *h1 = Hunk_HighAllocName(500, "h1");
	void *h2 = Hunk_HighAllocName(500, "h2");
	CHECK(h1 && h2, "high hunk allocs returned non-null");
	CHECK((char *) h2 + 500 <= (char *) h1, "high allocs grow downward, no overlap");
	Hunk_FreeToHighMark(hmark);
	CHECK(Hunk_HighUsed() == hmark, "FreeToHighMark reset the high pointer");

	void *t1 = Hunk_TempAlloc(4096);
	void *t2 = Hunk_TempAlloc(4096);   /* releases t1 automatically */
	CHECK(t1 && t2, "temp allocs returned non-null");
	CHECK(t2 == t1, "second TempAlloc reuses the first's space");
	(void) Hunk_HighMark();            /* clears the pending temp */

	/* --- zone --- */
	int zfree0 = Z_FreeMemory();
	void *z1 = Z_Malloc(64);
	void *z2 = Z_Malloc(128);
	void *z3 = Z_Malloc(64);
	CHECK(z1 && z2 && z3, "zone allocs returned non-null");
	CHECK(z1 != z2 && z2 != z3, "zone allocs are distinct");
	CHECK(is_zero(z2, 128), "Z_Malloc returns zeroed memory");
	CHECK(Z_FreeMemory() < zfree0, "zone free space dropped after allocs");

	memset(z1, 0x11, 64);
	memset(z2, 0x22, 128);
	memset(z3, 0x33, 64);

	Z_Free(z2);          /* free the middle block */
	Z_CheckHeap();       /* Sys_Errors if the free list is corrupt */
	CHECK(*(volatile uint8_t *) z1 == 0x11 &&
	      *(volatile uint8_t *) z3 == 0x33, "neighbours intact after middle free");

	Z_Free(z1);
	Z_Free(z3);
	Z_CheckHeap();
	CHECK(Z_FreeMemory() >= zfree0 - 64,
	      "freeing every block coalesces back to ~full zone");

	Hunk_Check();
	Hunk_FreeToLowMark(mark0);

	printf("q1psx mem selftest: %s (%d failure%s)\n",
	       failures ? "FAIL" : "OK", failures, failures == 1 ? "" : "s");
	return failures;
}
