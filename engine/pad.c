#include <stdint.h>
#include <stdio.h>
#include <psxapi.h>
#include <psxgpu.h>
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

/* ----------------------------------------------------------- the poll -- */

/* The BIOS driver (StartPAD) polls both ports in the vblank handler, a byte
   at a time, waiting for each: 13 k cycles a frame on PCSX-Redux (its serial
   port takes ~535 cycles a byte), landing anywhere in the renderer. This
   driver polls port 1 only, the same way, in the vblank callback: half the
   waiting. (An interrupt-driven poll, a byte per /ACK interrupt, was tried
   first: the kernel's interrupt dispatch costs ~2.7 k cycles a byte on
   PCSX-Redux, 22 k a frame; on the real machine, whose serial byte takes
   ~40 us, it would be the better driver.) The pad's id byte says how long
   the poll is (0x41: a digital pad, 5 bytes; 0x73: analog, 9; 0xf3: config
   mode, 9); a pad that stops answering ends the poll by a short timeout. */

static const uint8_t poll_tx[9] = { 0x01, PAD_CMD_READ, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static volatile int   driver_on;

static void poll_sync(void)
{
	uint8_t rx[9];
	int n = 9, got = 0;
	SIO_CTRL(0) = 0x1003;                  /* TX on, port 1 selected, /ACK sets the IRQ flag */
	for (volatile int i = 0; i < 40; i++)
		;                                  /* (the select settles) */
	for (int i = 0; i < n; i++) {
		SIO_DATA(0) = poll_tx[i];
		if (!sio_wait(0x0002, 0x800))      /* the answer byte */
			break;
		rx[got++] = SIO_DATA(0);
		if (i == 1) {                      /* the id byte: the poll's length */
			int len = rx[1] & 0xf;
			n = rx[1] == 0xff || len == 0xf ? 2 : 3 + 2 * len;
			if (n > 9)
				n = 9;
		}
		if (i == n - 1)
			break;
		if (!sio_wait(0x0200, 0x400))      /* /ACK: the pad takes another byte */
			break;
		SIO_CTRL(0) = 0x1013;              /* the flag cleared, the pad still selected */
	}
	SIO_CTRL(0) = 0x0010;                  /* deselected, any flag cleared */
	uint8_t *b = pad_buf[0];
	if (got >= 5 && got >= n && rx[1] != 0xff) {
		b[0] = 0;                          /* PADTYPE: stat, id, buttons, sticks */
		b[1] = rx[1];
		b[2] = rx[3];
		b[3] = rx[4];
		for (int i = 5; i < 9; i++)
			b[i - 1] = got > i ? rx[i] : 0x80;
	} else {
		b[0] = 0xff;
	}
}

/* the polls, as the game takes them: written in the vblank, read by the
   frame (a single writer and a single reader; the reader stays 2 slots
   behind the writer so a slot is never read while it is written) */
#define PAD_RING 8
typedef struct {
	uint16_t held;
	int8_t   lx, ly, rx, ry;
	uint8_t  type, connected, analog, pad;
} pad_snap;
static volatile pad_snap ring[PAD_RING];
static volatile uint32_t ring_head;      /* polls written */
static uint32_t          ring_tail;      /* polls taken   */
static uint16_t          last_held;      /* the last taken poll's buttons */

/* the poll's answer (pad_buf) as a snapshot. stat 0 = data valid. Every
   pad type carries the button word at the same place (digital, analog,
   DualShock in either mode), so the type is not filtered: an earlier
   filter (4/5/7 only) silently dropped all input from PCSX-Redux's pad. */
static void snap_poll(volatile pad_snap *s)
{
	const PADTYPE *p = (const PADTYPE *) pad_buf[0];
	s->connected = p->stat == 0;
	s->type      = p->type;
	s->held      = s->connected ? (uint16_t) ~p->btn : 0;
	s->analog    = s->connected && (p->type == PAD_ID_ANALOG || p->type == PAD_ID_ANALOG_STICK);
	if (s->analog) {
		s->lx = (int8_t) ((int) p->ls_x - 128);
		s->ly = (int8_t) ((int) p->ls_y - 128);
		s->rx = (int8_t) ((int) p->rs_x - 128);
		s->ry = (int8_t) ((int) p->rs_y - 128);
	} else {
		s->lx = s->ly = s->rx = s->ry = 0;
	}
}

#if Q1PSX_PADSCRIPT
/* the release build's scripted check (CMake Q1PSX_PADSCRIPT): the pad's
   polls replaced by presses at given vblanks */
static const struct { uint16_t from, to, buttons; } pad_script[] = {
	/* (the boot's loads take their vblanks: the presses come late, and a
	   press during a load is lost, as a player's would be) */
	{ 1500, 1504, PAD_CROSS },     /* Single Player */
	{ 1560, 1564, PAD_CROSS },     /* New Game (START reloaded) */
	{ 2100, 2160, PAD_UP },        /* a second's walk */
	{ 2220, 2224, PAD_START },     /* the menu */
	{ 2280, 2284, PAD_DOWN },      /* (Multiplayer greyed: Options) */
	{ 2340, 2344, PAD_CROSS },     /* Options */
	{ 2400, 2404, PAD_UP },        /* (to Graphics) */
	{ 2460, 2464, PAD_CROSS },     /* Graphics: the textures */
	{ 2520, 2524, PAD_RIGHT },     /* textures: low (the release disc's after high) */
	{ 2580, 2584, PAD_DOWN },
	{ 2640, 2644, PAD_LEFT },      /* map geometry: low */
	{ 2700, 2704, PAD_DOWN },
	{ 2760, 2764, PAD_LEFT },      /* models: medium */
	{ 2820, 2824, PAD_DOWN },
	{ 2880, 2884, PAD_RIGHT },     /* effects: ultra (at once) */
	{ 2940, 2944, PAD_DOWN }, { 3000, 3004, PAD_DOWN }, { 3060, 3064, PAD_DOWN },
	{ 3120, 3124, PAD_RIGHT },     /* warp: dynamic */
	{ 3180, 3184, PAD_DOWN }, { 3240, 3244, PAD_DOWN },
	{ 3300, 3304, PAD_RIGHT },     /* HUD: q2 psx (the budget skipped: greyed) */
	{ 3360, 3364, PAD_DOWN },
	{ 3420, 3424, PAD_CROSS },     /* dynamic lights: off */
	{ 3480, 3484, PAD_DOWN },
	{ 3540, 3544, PAD_RIGHT },     /* draw weapon: 2D */
	{ 3600, 3604, PAD_DOWN },
	{ 3660, 3664, PAD_RIGHT },     /* draw distance: 2048 */
	{ 3720, 3724, PAD_DOWN },
	{ 3780, 3784, PAD_RIGHT },     /* flat shading: far */
	{ 3840, 3844, PAD_START }, { 3900, 3904, PAD_START }, { 3960, 3964, PAD_START },   /* out: no load */
	{ 4020, 4140, PAD_R2 },        /* two seconds of fire */
	{ 4200, 4204, PAD_START },     /* the menu again (its cursor on Options) */
	{ 4230, 4234, PAD_UP },
	{ 4260, 4264, PAD_CROSS },     /* Single Player */
	{ 4320, 4324, PAD_CROSS },     /* New Game: "Are you sure" */
	{ 4380, 4384, PAD_CROSS },     /* yes: START at the levels chosen */
};
static uint32_t script_vbl;

static void script_poll(void)
{
	uint16_t b = 0;
	for (unsigned i = 0; i < sizeof pad_script / sizeof pad_script[0]; i++)
		if (script_vbl >= pad_script[i].from && script_vbl < pad_script[i].to)
			b |= pad_script[i].buttons;
	script_vbl++;
	uint8_t *p = pad_buf[0];
	p[0] = 0;                              /* data valid, a DualShock (analog), active low */
	p[1] = 0x73;
	p[2] = (uint8_t) ~b;
	p[3] = (uint8_t) (~b >> 8);
	p[4] = p[5] = p[6] = p[7] = 0x80;      /* the sticks centred */
}
#endif

static void pad_vblank(void)
{
	if (!driver_on)
		return;
#if Q1PSX_PADSCRIPT
	script_poll();
#else
	poll_sync();
#endif
	uint32_t h = ring_head;
	snap_poll(&ring[h & (PAD_RING - 1)]);
	ring_head = h + 1;
}

static void driver_start(void)
{
	SIO_CTRL(0) = 0x0040;                  /* the port reset */
	SIO_MODE(0) = 0x000d;                  /* 8 bits, no parity, x1 */
	SIO_BAUD(0) = 0x0088;                  /* 250 kbps */
	SIO_CTRL(0) = 0x0010;
	driver_on = 1;
}

/* the driver off and idle, for a synchronous exchange (pad_force_analog) */
static void driver_stop(void)
{
	driver_on = 0;
	VSync(0);                              /* a poll in flight ends with the vblank */
}

void pad_init(void)
{
	pad_buf[0][0] = pad_buf[1][0] = 0xff;   /* "no data" until the first poll */
	attempts = 1;
	int analog = pad_force_analog();         /* (not in the printf: the release has none) */
	printf("pad: analog mode %s\n", analog ? "on" : "not set (digital pad, or no answer)");
	VSyncCallback(pad_vblank);
	driver_start();
}

static void snap_to_pad(const volatile pad_snap *s, pad_t *p)
{
	p->held      = s->held;
	p->pressed   = s->held & (uint16_t) ~last_held;
	p->connected = s->connected;
	p->type      = s->type;
	p->analog    = s->analog;
	p->lx = s->lx; p->ly = s->ly; p->rx = s->rx; p->ry = s->ry;
	last_held    = s->held;
}

int pad_frame(pad_t *ticks, int max)
{
	uint32_t head = ring_head;
	if (head - ring_tail > PAD_RING - 2)        /* a long frame (a load): the oldest gone */
		ring_tail = head - (PAD_RING - 2);
	uint16_t pressed = 0, carried = 0;
	int n = 0;
	pad_t p = pad;
	while (ring_tail != head) {
		snap_to_pad(&ring[ring_tail & (PAD_RING - 1)], &p);
		ring_tail++;
		pressed |= p.pressed;
		if (head - ring_tail >= (uint32_t) max) {   /* beyond the frame's ticks: its presses carried */
			carried |= p.pressed;
			continue;
		}
		p.pressed |= carried;
		carried = 0;
		ticks[n++] = p;
	}
	pad = p;
	pad.pressed = pressed;

	/* a pad plugged in later, or one that missed the commands at boot: two
	   more tries, two seconds apart, while it reports digital */
	if (pad.connected && pad.type == PAD_ID_DIGITAL) {
		if (++digital_frames >= 120 && attempts < 3) {
			digital_frames = 0;
			attempts++;
			driver_stop();
			int ok = pad_force_analog();
			driver_start();
			printf("pad: analog mode %s (try %d)\n", ok ? "on" : "not set", attempts);
		}
	} else {
		digital_frames = 0;
		if (pad.connected && pad.type != PAD_ID_CONFIG_MODE)
			attempts = 0;
	}
	return n;
}

void pad_read(void)
{
	pad_frame(0, 0);
}
