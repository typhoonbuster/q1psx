/*
 * q1psx — controller input (port 1, the game's own poll in the vblank).
 *
 * The pad is polled every vblank and each poll kept (a ring of 8): the game
 * takes them one per 60 Hz tick (pad_frame), so its input is sampled at
 * 60 Hz whatever the frame rate; the menus read the frame's
 * merged state (pad_read). `held` = buttons down, `pressed` = buttons that
 * went down since the poll (or the read) before. Bits are psxpad.h's PAD_*
 * values, active high (the pad answers active low; we flip it).
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

extern pad_t pad;           /* the last frame's: held now, pressed since the read before */

void pad_init(void);
void pad_vblank(void);      /* the vblank's poll begun (engine/vid.c's interrupt calls it) */

/* the polls since the last call, oldest first, one per vblank: at most
   `max` (older ones dropped, their presses kept in the first); `pad` set
   to the frame's merged state. Returns how many (0: no vblank since). */
#define PAD_MAX_TICKS 4
int  pad_frame(pad_t *ticks, int max);
void pad_read(void);        /* pad_frame without the per-poll states (menus, loads) */

#endif
