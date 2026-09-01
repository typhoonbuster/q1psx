/*
 * q1psx — controller input (Phase 2.2)
 *
 * Thin wrapper over the BIOS pad driver: it polls port 1 once per frame (the
 * BIOS refreshes the buffers on every vblank) and hands back a decoded state.
 * Port 2 / multitap / analog sticks come later if a phase needs them.
 */

#pragma once

#include <stdint.h>

typedef struct {
	int      connected;   /* 1 if a device is responding on port 1 */
	uint8_t  type;        /* PadTypeID (0x4 = digital, 0x7 = DualShock, ...) */
	uint16_t buttons;     /* PAD_* mask, active-HIGH: bit set == pressed */
	uint16_t pressed;     /* buttons that went down since the last pad_read() */
} PadState;

void pad_init(void);

/* Refresh *out from the BIOS buffer. Call once per frame. */
void pad_read(PadState *out);
