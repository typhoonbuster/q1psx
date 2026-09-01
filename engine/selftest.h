#pragma once

/*
 * Boot-time engine self-tests. Each phase that ports a foundational module adds
 * its checks here; they print PASS/FAIL lines to serial and the total failure
 * count is surfaced in the on-screen overlay. Not linked in a release build.
 *
 * Call after Memory_Init() — some tests use the hunk. They leave the hunk at
 * the low mark they found it at.
 */

int run_selftests(void);
