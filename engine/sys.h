/*
 * q1psx — platform / fatal-error hooks
 *
 * Quake calls Sys_Error() from deep inside the engine when an invariant breaks.
 * On the PS1 there is nowhere to return to, so it logs to the serial port and
 * parks the CPU. Keep the signature identical to the original engine.
 */

#pragma once

void Sys_Error(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
