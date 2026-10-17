#include <stdint.h>
#include <stdio.h>
#include <psxapi.h>
#include <psxpad.h>
#include <hwregs_c.h>

#include "pad.h"

static uint8_t pad_buf[2][34];

pad_t pad;

/* ---------------------------------------------------- analog mode, forced -- */

/* The BIOS driver only polls the pad. A DualShock powers up in digital mode
   (type 4: no sticks) until its ANALOG button is pressed, and most emulators
   start it that way too, so the game switches it itself with the pad's
   configuration commands (0x43 enter / leave config mode, 0x44 analog mode
   on, locked), as games like Ape Escape do. Sent with the BIOS driver
   stopped, one byte at a time on SIO0: a byte goes out, its answer comes
   back, then the pad pulses /ACK (the port's IRQ flag) if it has more to
   say. A pad that does not answer, or does not know the command (a digital
   pad; some emulated ones), ends the exchange by a timeout. */

static void sio_delay(int n)
{
	for (volatile int i = 0; i < n; i++)
		;
}

/* wait for STAT bit(s); 0 on timeout */
static int sio_wait(uint16_t mask, int tries)
{
	while (tries-- > 0)
		if (SIO_STAT(0) & mask)
			return 1;
	return 0;
}

/* exchange: tx[n] out, the answers in rx; returns how many came back */
static int sio_exchange(const uint8_t *tx, int n, uint8_t *rx)
{
	int got = 0;
	SIO_CTRL(0) = 0x0010;                  /* any old IRQ acknowledged, port idle */
	sio_delay(500);
	SIO_CTRL(0) = 0x1003;                  /* TX on, pad 1 selected, /ACK raises the IRQ */
	sio_delay(500);
	for (int i = 0; i < n; i++) {
		SIO_DATA(0) = tx[i];
		if (!sio_wait(0x0002, 0x2000))     /* the answer byte */
			break;
		rx[got++] = SIO_DATA(0);
		if (i == n - 1)
			break;
		if (!sio_wait(0x0200, 0x1000))     /* /ACK: the pad takes another byte */
			break;
		for (int k = 0; k < 0x1000 && (SIO_STAT(0) & 0x0080); k++)
			;                              /* /ACK released again */
		SIO_CTRL(0) = 0x1013;              /* the IRQ flag reset, pad still selected */
	}
	SIO_CTRL(0) = 0x0010;
	sio_delay(500);
	SIO_CTRL(0) = 0x0000;
	for (int k = 0; k < 16 && (SIO_STAT(0) & 0x0002); k++)
		(void) SIO_DATA(0);                /* leftovers out of the FIFO */
	return got;
}

/* 1 when the pad went into analog mode (or already was) */
static int pad_force_analog(void)
{
	static const uint8_t poll[5]   = { 0x01, PAD_CMD_READ, 0x00, 0x00, 0x00 };
	static const uint8_t enter[9]  = { 0x01, PAD_CMD_CONFIG_MODE, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t analog[9] = { 0x01, PAD_CMD_SET_ANALOG,  0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t leave[9]  = { 0x01, PAD_CMD_CONFIG_MODE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	uint8_t rx[9];

	SIO_CTRL(0) = 0x0040;                  /* the port reset */
	SIO_MODE(0) = 0x000d;                  /* 8 bits, no parity, x1 */
	SIO_BAUD(0) = 0x0088;                  /* 250 kbps */

	int n = sio_exchange(poll, 5, rx);     /* rx[1]: the pad's id byte */
	if (n < 2 || rx[1] == 0xff)
		return 0;                          /* nothing there */
	if ((rx[1] >> 4) == PAD_ID_ANALOG)
		return 1;
	if ((rx[1] >> 4) != PAD_ID_DIGITAL)
		return 0;                          /* a mouse, a neGcon...: leave it */
	sio_exchange(enter, 9, rx);
	n = sio_exchange(analog, 9, rx);       /* answered in config mode (id 0xf3) */
	int ok = n >= 2 && (rx[1] >> 4) == PAD_ID_CONFIG_MODE;
	sio_exchange(leave, 9, rx);
	return ok;
}

static int digital_frames, attempts;

static void bios_start(void)
{
	StartPAD();
	ChangeClearPAD(0);   /* let psxgpu's handler see the vblank IRQ too */
}

void pad_init(void)
{
	InitPAD(pad_buf[0], 34, pad_buf[1], 34);
	pad_buf[0][0] = pad_buf[1][0] = 0xff;   /* "no data" until the first poll */
	attempts = 1;
	printf("pad: analog mode %s\n", pad_force_analog() ? "on" : "not set (digital pad, or no answer)");
	bios_start();
}

void pad_read(void)
{
	const PADTYPE *p = (const PADTYPE *) pad_buf[0];
	uint16_t now = 0;

	/* stat 0 = data valid. Every pad type carries the button word at the same
	   place (digital, analog, DualShock in either mode), so do not filter on
	   the type: an earlier filter (4/5/7 only) silently dropped all input
	   from PCSX-Redux's pad. */
	pad.connected = p->stat == 0;
	pad.type      = p->type;
	if (pad.connected)
		now = (uint16_t) ~p->btn;

	pad.analog = pad.connected && (p->type == PAD_ID_ANALOG || p->type == PAD_ID_ANALOG_STICK);
	if (pad.analog) {
		pad.lx = (int) p->ls_x - 128;
		pad.ly = (int) p->ls_y - 128;
		pad.rx = (int) p->rs_x - 128;
		pad.ry = (int) p->rs_y - 128;
	} else {
		pad.lx = pad.ly = pad.rx = pad.ry = 0;
	}

	pad.pressed = now & (uint16_t) ~pad.held;
	pad.held    = now;

	/* a pad plugged in later, or one that missed the commands at boot: two
	   more tries, two seconds apart, while it reports digital */
	if (pad.connected && p->type == PAD_ID_DIGITAL) {
		if (++digital_frames >= 120 && attempts < 3) {
			digital_frames = 0;
			attempts++;
			StopPAD();
			int ok = pad_force_analog();
			bios_start();
			printf("pad: analog mode %s (try %d)\n", ok ? "on" : "not set", attempts);
		}
	} else {
		digital_frames = 0;
		if (pad.connected && p->type != PAD_ID_CONFIG_MODE)
			attempts = 0;
	}
}
