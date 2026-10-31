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

#define PAD_MOUSE_LEFT  1
#define PAD_MOUSE_RIGHT 2

typedef struct {
	uint16_t held;
	uint16_t pressed;
	int      connected;
	int      type;        /* PADTYPE type nibble (4 digital, 7 DualShock...) */
	int      analog;      /* the sticks below are live                       */
	int      lx, ly;      /* left stick, -128 .. 127 (up is negative)        */
	int      rx, ry;      /* right stick                                     */
	int      mouse;       /* a PlayStation Mouse answered (port 2 polled: pad_port2) */
	int      mdx, mdy;    /* its motion since the poll before (right, down: positive) */
	uint8_t  mheld, mpressed;   /* its buttons, PAD_MOUSE_*                */
} pad_t;

extern pad_t pad;           /* the last frame's: held now, pressed since the read before */

/* port 2 polled too, after port 1 (the Options' mouse): the pad is the
   first port's pad, the mouse the first port's mouse */
extern int pad_port2;

/* the DualShock's motors (engine/rumble.c): the small one on (1) or off,
   the large one's speed 0-255; sent with each poll of the pad's port */
extern uint8_t pad_motor_small, pad_motor_large;

void pad_init(void);
void pad_vblank(void);      /* the vblank's poll begun (engine/vid.c's interrupt calls it) */

/* The poll stopped (and its port idle) for a synchronous exchange on SIO0
   (the memory card, engine/card.c), and started again */
void pad_suspend(void);
void pad_resume(void);

/* an exchange of n bytes with the device on `port` (0, 1; the poll
   suspended): tx out, the answers in rx; how many came back (a device
   not there or done answering: fewer) */
int  pad_sio_exchange(int port, const uint8_t *tx, int n, uint8_t *rx);

/* the polls since the last call, oldest first, one per vblank: at most
   `max` (older ones dropped, their presses kept in the first); `pad` set
   to the frame's merged state. Returns how many (0: no vblank since). */
#define PAD_MAX_TICKS 4
int  pad_frame(pad_t *ticks, int max);
void pad_read(void);        /* pad_frame without the per-poll states (menus, loads) */

#endif
