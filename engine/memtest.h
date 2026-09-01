#pragma once

/* Run the Hunk/Zone self-test against `block` (block_size bytes).
   Prints PASS/FAIL lines to serial; returns the failure count (0 = OK). */
int mem_selftest(void *block, int block_size);
