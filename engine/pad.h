/*
 * q1psx — controller input (port 1, BIOS pad driver).
 *
 * pad_read() once per frame; `held` = buttons down now, `pressed` = buttons
 * that went down since the previous read. Bits are psxpad.h's PAD_* values,
 * active high (the BIOS buffer is active low; we flip it).
 */

#ifndef Q1PSX_PAD_H
#define Q1PSX_PAD_H

#include <stdint.h>
#include <psxpad.h>

typedef struct {
	uint16_t held;
	uint16_t pressed;
	int      connected;
	int      type;        /* PADTYPE type nibble (4 digital, 7 DualShock...) */
} pad_t;

extern pad_t pad;

void pad_init(void);
void pad_read(void);

#endif
