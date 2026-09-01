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
#include "q_fixed.h"
#include "mathlib.h"
#include "cvar.h"
#include "cmd.h"
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

	/* --- Q_atoi / fixed-point string conversion --- */
	CHECK(Q_atoi("42") == 42 && Q_atoi("-7") == -7 && Q_atoi("0x1F") == 31,
	      "Q_atoi handles decimal, sign and hex");
	CHECK(Q_atofix("800") == Q_FIX(800) && Q_atofix("-1.5") == -(Q_FIX_ONE + Q_FIX_HALF) &&
	      Q_atofix("0.25") == Q_FIX_ONE / 4, "Q_atofix parses int/frac/sign");
	{
		char buf[32];
		Q_fixtoa(Q_FIX(800), buf, sizeof buf);
		int a = !strcmp(buf, "800");
		Q_fixtoa(Q_FIX_ONE + Q_FIX_HALF, buf, sizeof buf);
		int b = !strcmp(buf, "1.5");
		Q_fixtoa(-(Q_FIX_ONE / 4), buf, sizeof buf);
		int c = !strcmp(buf, "-0.25");
		CHECK(a && b && c, "Q_fixtoa formats and trims");
	}

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

/* --------------------------------------------------- fixed-point math (3.2) */
/* Device build has no float reference (see tools/mathtest for that); check a
 * handful of exact / known-answer cases to confirm the port runs on MIPS. */

static int q_close(q_fixed got, q_fixed want, q_fixed tol)
{
	return q_abs(got - want) <= tol;
}

static void test_mathlib(void)
{
	printf("[selftest] mathlib (fixed 20.12)\n");

	CHECK(q_mul(Q_FIX(3), Q_FIX(4)) == Q_FIX(12), "q_mul integer case exact");
	CHECK(q_mul(Q_FIX_HALF, Q_FIX_HALF) == (Q_FIX_ONE >> 2), "q_mul 0.5*0.5 = 0.25");
	CHECK(q_div(Q_FIX(12), Q_FIX(4)) == Q_FIX(3), "q_div integer case exact");
	CHECK(q_sqrt(Q_FIX(4)) == Q_FIX(2) && q_sqrt(Q_FIX(9)) == Q_FIX(3),
	      "q_sqrt of perfect squares");
	CHECK(q_close(q_sqrt(Q_FIX(2)), 5793 /* ~1.41421*4096 */, 2), "q_sqrt(2) ~ 1.4142");

	vec3_t a = { Q_FIX(1), Q_FIX(2), Q_FIX(3) };
	vec3_t b = { Q_FIX(4), Q_FIX(-5), Q_FIX(6) };
	CHECK(DotProduct(a, b) == Q_FIX(1 * 4 + 2 * -5 + 3 * 6), "DotProduct known vectors");

	vec3_t x = { Q_FIX(1), 0, 0 }, y = { 0, Q_FIX(1), 0 }, cr;
	CrossProduct(x, y, cr);
	CHECK(cr[0] == 0 && cr[1] == 0 && cr[2] == Q_FIX(1), "CrossProduct X x Y = Z");

	vec3_t v = { Q_FIX(3), Q_FIX(4), 0 };
	vec_t  len = VectorNormalize(v);
	CHECK(q_close(len, Q_FIX(5), 2), "VectorNormalize returns length 5 for (3,4,0)");
	CHECK(q_close(v[0], (3 * Q_FIX_ONE) / 5, 3) && q_close(v[1], (4 * Q_FIX_ONE) / 5, 3),
	      "normalized (3,4,0) ~ (0.6, 0.8, 0)");
	CHECK(q_close(Length(v), Q_FIX_ONE, 4), "normalized vector has unit length");

	vec3_t s, m;
	VectorScale(a, Q_FIX(2), s);
	CHECK(s[0] == Q_FIX(2) && s[2] == Q_FIX(6), "VectorScale");
	VectorMA(a, Q_FIX(3), b, m);
	CHECK(m[1] == Q_FIX(2) + Q_FIX(3 * -5), "VectorMA");

	/* --- trig / angles (3.2b) --- */
	CHECK(q_close(q_sin(0), 0, 2) && q_close(q_sin(Q_FIX(90)), Q_FIX_ONE, 4),
	      "q_sin 0 and 90 deg");
	CHECK(q_close(q_cos(0), Q_FIX_ONE, 4) && q_close(q_cos(Q_FIX(180)), -Q_FIX_ONE, 4),
	      "q_cos 0 and 180 deg");
	CHECK(q_close(q_sin(Q_FIX(30)), Q_FIX_ONE / 2, 8), "q_sin 30 deg ~ 0.5");
	CHECK(anglemod(Q_FIX(370)) == Q_FIX(10) && anglemod(Q_FIX(-90)) == Q_FIX(270),
	      "anglemod wraps");

	vec3_t ang = { 0, Q_FIX(90), 0 };   /* look along +Y */
	vec3_t fwd, rgt, up;
	AngleVectors(ang, fwd, rgt, up);
	CHECK(q_close(fwd[0], 0, 4) && q_close(fwd[1], Q_FIX_ONE, 4) && q_close(fwd[2], 0, 4),
	      "AngleVectors yaw 90 -> forward +Y");
	CHECK(q_abs(DotProduct(fwd, rgt)) < 8 && q_abs(DotProduct(fwd, up)) < 8,
	      "AngleVectors basis is orthogonal");
}

/* --------------------------------------------------- cvar + cmd (3.1b/3.1c) */

static int cmd_hit;
static void cmd_hit_fn(void) { cmd_hit = Cmd_Argc(); }

static void test_cvar_cmd(void)
{
	printf("[selftest] cvar + cmd\n");

	cvar_t *saved_vars = cvar_vars;
	cvar_vars = NULL;
	Cbuf_Init();

	static cvar_t sv_test = { "sv_test", "320" };
	Cvar_RegisterVariable(&sv_test);
	CHECK(sv_test.value == Q_FIX(320), "cvar registered: value parsed from string");
	CHECK(Cvar_VariableValue("sv_test") == Q_FIX(320), "Cvar_VariableValue by name");
	CHECK(Cvar_FindVar("nope") == NULL, "Cvar_FindVar misses unknown");

	Cvar_Set("sv_test", "1.5");
	CHECK(sv_test.value == Q_FIX_ONE + Q_FIX_HALF && !strcmp(sv_test.string, "1.5"),
	      "Cvar_Set updates string and value");
	Cvar_SetValue("sv_test", Q_FIX(2));
	CHECK(sv_test.value == Q_FIX(2) && !strcmp(sv_test.string, "2"), "Cvar_SetValue");

	Cmd_AddCommand("test_hit", cmd_hit_fn);
	CHECK(Cmd_Exists("test_hit") && !Cmd_Exists("test_miss"), "Cmd_Exists");

	Cmd_TokenizeString("test_hit alpha \"two words\"");
	CHECK(Cmd_Argc() == 3 && !strcmp(Cmd_Argv(1), "alpha") &&
	      !strcmp(Cmd_Argv(2), "two words"), "Cmd_TokenizeString splits args + quotes");
	CHECK(Cmd_Argv(9)[0] == 0, "Cmd_Argv out of range is empty string");

	cmd_hit = -1;
	Cbuf_AddText("test_hit one two\n");
	Cbuf_Execute();
	CHECK(cmd_hit == 3, "Cbuf_Execute dispatched the command with its args");

	Cbuf_AddText("sv_test 5\n");   /* cvar set via the command path */
	Cbuf_Execute();
	CHECK(sv_test.value == Q_FIX(5), "Cvar_Command sets a cvar from the buffer");

	Cbuf_AddText("a ; b\n");       /* ';' separates commands */
	Cbuf_Execute();
	CHECK(Cbuf_Pending() == 0, "Cbuf drains ';'-separated lines");

	/* tear down so run_selftests can roll the hunk back */
	Z_Free(sv_test.string);
	sv_test.string = NULL;
	cvar_vars = saved_vars;
	Cmd_Shutdown();
}

/* ----------------------------------------------------------------- driver */

int run_selftests(void)
{
	int start_mark = Hunk_LowMark();

	failures = 0;
	printf("=== q1psx self-tests ===\n");

	test_allocator();
	test_qcommon();
	test_mathlib();
	test_cvar_cmd();

	Hunk_FreeToLowMark(start_mark);

	printf("=== self-tests: %s (%d failure%s) ===\n",
	       failures ? "FAIL" : "OK", failures, failures == 1 ? "" : "s");
	return failures;
}
