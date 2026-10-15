/*
 * q1psx — controller input (port 1, BIOS pad driver).
 *
 * pad_read() once per frame; `held` = buttons down now, `pressed` = buttons
 * that went down since the previous read. Bits are psxpad.h's PAD_* values,
 * active high (the BIOS buffer is active low; we flip it).
 *
 * Sticks: only in analog mode (DualShock / Dual Analog with the ANALOG
 * button's LED on: type 7, or 5 for the green-LED mode); centred, -128
 * (left / up) .. 127 (right / down), 0 otherwise. The BIOS driver cannot
 * switch a pad to analog mode itself.
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
	int      analog;      /* the sticks below are live                       */
	int      lx, ly;      /* left stick, -128 .. 127 (up is negative)        */
	int      rx, ry;      /* right stick                                     */
} pad_t;

extern pad_t pad;

void pad_init(void);
void pad_read(void);

#endif
