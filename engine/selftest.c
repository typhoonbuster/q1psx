/*
 * q1psx — boot-time engine self-tests.
 *
 * Runs against the *live* allocator (call after Memory_Init). Every test takes
 * a Hunk_LowMark on entry and frees back to it, and frees anything it puts in
 * the zone, so the heap is left exactly as it was found.
 *
 * Phase 2.4: Hunk/Zone allocator.
 * Phase 3.1: qcommon (links, sizebuf, byte order, COM_Parse, filename utils).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zone.h"
#include "qcommon.h"
#include "selftest.h"

static int failures;

#define CHECK(cond, msg) do {                          \
		if (cond) {                                    \
			printf("  PASS  %s\n", (msg));             \
		} else {                                       \
			printf("  FAIL  %s\n", (msg));             \
			failures++;                                \
		}                                              \
	} while (0)

static int is_zero(const void *p, int n)
{
	const uint8_t *b = p;
	for (int i = 0; i < n; i++)
		if (b[i])
			return 0;
	return 1;
}

/* ---------------------------------------------------- allocator (phase 2.4) */

static void test_allocator(void)
{
	printf("[selftest] allocator\n");

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

	int used_before = Hunk_LowUsed();
	Hunk_FreeToLowMark(mark0);
	CHECK(Hunk_LowUsed() == mark0, "FreeToLowMark reset the low pointer");
	CHECK(used_before > mark0, "low hunk had actually grown");

	void *a2 = Hunk_AllocName(100, "a2");
	CHECK(a2 == a, "reallocating after free reuses the same address");
	CHECK(is_zero(a2, 100), "reused block was re-zeroed");
	Hunk_FreeToLowMark(mark0);

	int hmark = Hunk_HighMark();
	void *h1 = Hunk_HighAllocName(500, "h1");
	void *h2 = Hunk_HighAllocName(500, "h2");
	CHECK(h1 && h2, "high hunk allocs returned non-null");
	CHECK((char *) h2 + 500 <= (char *) h1, "high allocs grow downward, no overlap");
	Hunk_FreeToHighMark(hmark);
	CHECK(Hunk_HighUsed() == hmark, "FreeToHighMark reset the high pointer");

	void *t1 = Hunk_TempAlloc(4096);
	void *t2 = Hunk_TempAlloc(4096);
	CHECK(t1 && t2, "temp allocs returned non-null");
	CHECK(t2 == t1, "second TempAlloc reuses the first's space");
	(void) Hunk_HighMark();   /* clears the pending temp */

	int zfree0 = Z_FreeMemory();
	void *z1 = Z_Malloc(64);
	void *z2 = Z_Malloc(128);
	void *z3 = Z_Malloc(64);
	CHECK(z1 && z2 && z3, "zone allocs returned non-null");
	CHECK(z1 != z2 && z2 != z3, "zone allocs are distinct");
	CHECK(is_zero(z2, 128), "Z_Malloc returns zeroed memory");
	CHECK(Z_FreeMemory() < zfree0, "zone free space dropped after allocs");

	memset(z1, 0x11, 64);
	memset(z3, 0x33, 64);
	Z_Free(z2);
	Z_CheckHeap();
	CHECK(*(volatile uint8_t *) z1 == 0x11 && *(volatile uint8_t *) z3 == 0x33,
	      "neighbours intact after middle free");
	Z_Free(z1);
	Z_Free(z3);
	Z_CheckHeap();
	CHECK(Z_FreeMemory() >= zfree0 - 64, "freeing every block coalesces back");

	Hunk_Check();
	Hunk_FreeToLowMark(mark0);
}

/* ------------------------------------------------------ qcommon (phase 3.1) */

typedef struct { link_t link; int id; } listnode_t;

static void test_qcommon(void)
{
	printf("[selftest] qcommon\n");

	/* --- intrusive linked list --- */
	link_t head;
	listnode_t n[3] = { { .id = 0 }, { .id = 1 }, { .id = 2 } };
	ClearLink(&head);
	InsertLinkBefore(&n[0].link, &head);
	InsertLinkBefore(&n[1].link, &head);
	InsertLinkBefore(&n[2].link, &head);   /* order should be 0,1,2 */

	int seq[3], k = 0;
	for (link_t *l = head.next; l != &head && k < 3; l = l->next)
		seq[k++] = STRUCT_FROM_LINK(l, listnode_t, link)->id;
	CHECK(k == 3 && seq[0] == 0 && seq[1] == 1 && seq[2] == 2,
	      "list iterates in insertion order via STRUCT_FROM_LINK");

	RemoveLink(&n[1].link);
	k = 0;
	for (link_t *l = head.next; l != &head && k < 3; l = l->next)
		seq[k++] = STRUCT_FROM_LINK(l, listnode_t, link)->id;
	CHECK(k == 2 && seq[0] == 0 && seq[1] == 2, "RemoveLink unlinks the middle node");

	/* --- byte order (PS1 = little-endian) --- */
	CHECK(LittleLong(0x11223344) == 0x11223344, "LittleLong is identity");
	CHECK(BigLong(0x11223344) == 0x44332211, "BigLong swaps");
	CHECK(BigShort((short) 0x1234) == (short) 0x3412, "BigShort swaps");

	/* --- Q_atoi --- */
	CHECK(Q_atoi("42") == 42 && Q_atoi("-7") == -7 && Q_atoi("0x1F") == 31,
	      "Q_atoi handles decimal, sign and hex");

	/* --- sizebuf (hunk-backed; SZ_Alloc clamps startsize up to 256) --- */
	int mark = Hunk_LowMark();
	sizebuf_t sb;
	memset(&sb, 0, sizeof sb);       /* SZ_Alloc leaves the flags to the caller */
	SZ_Alloc(&sb, 8);
	CHECK(sb.maxsize == 256 && sb.cursize == 0, "SZ_Alloc clamps startsize to 256");
	sb.allowoverflow = true;
	uint8_t chunk[100];
	memset(chunk, 0xAB, sizeof chunk);
	SZ_Write(&sb, chunk, 100);
	SZ_Write(&sb, chunk, 100);
	CHECK(sb.cursize == 200 && !sb.overflowed && sb.data[100] == 0xAB,
	      "SZ_Write appends into the buffer");
	SZ_Write(&sb, chunk, 100);       /* 300 > 256 -> overflow, clears */
	CHECK(sb.overflowed && sb.cursize == 100,
	      "SZ_Write past maxsize overflows and restarts");
	Hunk_FreeToLowMark(mark);

	/* --- COM_Parse --- */
	char script[] = "  // comment\n  set  \"a b\"  { 42 }";
	char *p = script;
	const char *want[] = { "set", "a b", "{", "42", "}" };
	int ok = 1;
	for (int i = 0; i < 5; i++) {
		p = COM_Parse(p);
		if (!p || strcmp(com_token, want[i]) != 0) { ok = 0; break; }
	}
	CHECK(ok && COM_Parse(p) == NULL, "COM_Parse tokenizes comments/quotes/braces");

	/* --- filename utils --- */
	char base[64];
	COM_FileBase("maps/e1m1.bsp", base);
	CHECK(strcmp(base, "e1m1") == 0, "COM_FileBase strips dir and extension");
	char stripped[64];
	COM_StripExtension("progs/player.mdl", stripped);
	CHECK(strcmp(stripped, "progs/player") == 0, "COM_StripExtension");
	char path[64] = "config";
	COM_DefaultExtension(path, ".cfg");
	CHECK(strcmp(path, "config.cfg") == 0, "COM_DefaultExtension appends when missing");
	COM_DefaultExtension(path, ".xxx");
	CHECK(strcmp(path, "config.cfg") == 0, "COM_DefaultExtension no-ops when present");
}

/* ----------------------------------------------------------------- driver */

int run_selftests(void)
{
	int start_mark = Hunk_LowMark();

	failures = 0;
	printf("=== q1psx self-tests ===\n");

	test_allocator();
	test_qcommon();

	Hunk_FreeToLowMark(start_mark);

	printf("=== self-tests: %s (%d failure%s) ===\n",
	       failures ? "FAIL" : "OK", failures, failures == 1 ? "" : "s");
	return failures;
}
