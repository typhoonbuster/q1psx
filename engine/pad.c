#include <stdint.h>
#include <stdio.h>
#include <psxapi.h>
#include <psxetc.h>
#include <psxgpu.h>
#include <psxpad.h>
#include <hwregs_c.h>

#include "mem.h"
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

/* The BIOS driver (StartPAD) polls both ports in the vblank handler, a byte
   at a time, waiting for each: 13 k cycles a frame on PCSX-Redux (its serial
   port takes ~535 cycles a byte), landing anywhere in the renderer. The
   game's first driver polled port 1 only the same way: 10 k. This one is
   interrupt-driven (docs/porting-notes.md §81.2): the vblank sends the
   first byte, then each byte's /ACK interrupt (SIO0, IRQ 7) reads its
   answer and sends the next one; the last answer, which no /ACK follows,
   comes by the receive interrupt. The CPU never waits for the port: the
   kernel's dispatch, ~2.7 k cycles an interrupt, made this the dearer
   driver until engine/irq.s took it out. The pad's id byte says how long
   the poll is (0x41: a digital pad, 5 bytes; 0x73: analog, 9; 0xf3: config
   mode, 9). A poll still going at the next vblank (no pad, or one that
   stopped answering) ends there with what came. */

static volatile int   driver_on;

/* The poll's state. engine/irq.s reads and writes it too: its short way
   for SIO0 sends the middle bytes (§81.4), at the PP_* offsets there. */
typedef struct {
	volatile uint8_t  busy;          /* +0  a poll in flight              */
	volatile uint8_t  i, n;          /* +1, +2  the answers in, its length */
	uint8_t           pad0;
	volatile uint16_t t;             /* +4  timer 2 at its last byte      */
	uint8_t           pad1[2];
	uint8_t           rx[9];         /* +8  the answers                   */
	uint8_t           tx[9];         /* +17 the bytes sent                */
} pad_poll_t;
pad_poll_t pad_poll = {
	.tx = { 0x01, PAD_CMD_READ, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};
_Static_assert(__builtin_offsetof(pad_poll_t, t) == 4 && __builtin_offsetof(pad_poll_t, rx) == 8 &&
               __builtin_offsetof(pad_poll_t, tx) == 17, "irq.s's PP_* offsets");

static void snap_poll(volatile pad_snap *s);

/* (interrupts off) the poll ended: its answer into pad_buf, then the ring */
static void poll_end(void)
{
	int got = pad_poll.i, n = pad_poll.n;
	if (got == n - 1 && (SIO_STAT(0) & 0x0002))
		pad_poll.rx[got++] = SIO_DATA(0);  /* (the last answer, its interrupt not taken yet) */
	SIO_CTRL(0) = 0x0010;                  /* deselected, any flag cleared */
	pad_poll.busy = 0;
	uint8_t *b = pad_buf[0];
	if (got >= 5 && got >= n && pad_poll.rx[1] != 0xff) {
		b[0] = 0;                          /* PADTYPE: stat, id, buttons, sticks */
		b[1] = pad_poll.rx[1];
		b[2] = pad_poll.rx[3];
		b[3] = pad_poll.rx[4];
		for (int i = 5; i < 9; i++)
			b[i - 1] = got > i ? pad_poll.rx[i] : 0x80;
	} else {
		b[0] = 0xff;
	}
	uint32_t h = ring_head;
	snap_poll(&ring[h & (PAD_RING - 1)]);
	ring_head = h + 1;
}

/* SIO0's interrupt: the answer to byte i came (the middle bytes take
   engine/irq.s's short way, the same steps; the id byte and the last one
   come here) */
static void poll_isr(void)
{
	if (!pad_poll.busy) {
		SIO_CTRL(0) = 0x0010;              /* (one after the poll ended) */
		return;
	}
	int i = pad_poll.i;
	pad_poll.rx[i++] = SIO_DATA(0);
	pad_poll.i = i;
	if (i == 2) {                          /* the id byte: the poll's length */
		int id = pad_poll.rx[1], len = id & 0xf, n = id == 0xff || len == 0xf ? 2 : 3 + 2 * len;
		pad_poll.n = n > 9 ? 9 : n;
	}
	if (i >= pad_poll.n) {
		poll_end();
		return;
	}
	/* the flag cleared, the pad still selected; the last answer by the
	   receive interrupt (no /ACK follows it), the others by /ACK */
	SIO_CTRL(0) = i == pad_poll.n - 1 ? 0x0813 : 0x1013;
	SIO_DATA(0) = pad_poll.tx[i];
	pad_poll.t = (uint16_t) TIMER_VALUE(2);
}

static void poll_start(void)
{
	SIO_CTRL(0) = 0x1003;                  /* TX on, port 1 selected, /ACK raises the IRQ */
	for (volatile int i = 0; i < 40; i++)
		;                                  /* (the select settles) */
	pad_poll.i    = 0;
	pad_poll.n    = 9;
	pad_poll.busy = 1;
	SIO_DATA(0) = pad_poll.tx[0];
	pad_poll.t = (uint16_t) TIMER_VALUE(2);
}

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

/* (the vblank's interrupt: engine/vid.c vid_vblank calls it first) */
void pad_vblank(void)
{
	if (!driver_on)
		return;
	if (pad_poll.busy)
		poll_end();                        /* the last one, as far as it went */
	poll_start();
}

static void driver_start(void)
{
	SIO_CTRL(0) = 0x0040;                  /* the port reset */
	SIO_MODE(0) = 0x000d;                  /* 8 bits, no parity, x1 */
	SIO_BAUD(0) = 0x0088;                  /* 250 kbps */
	SIO_CTRL(0) = 0x0010;
	pad_poll.busy = 0;
	InterruptCallback(IRQ_SIO0, poll_isr);
	driver_on = 1;
}

/* the driver off and idle, for a synchronous exchange (pad_force_analog):
   a poll in flight dropped, the port's interrupt off */
static void driver_stop(void)
{
	driver_on = 0;
	FastEnterCriticalSection();
	InterruptCallback(IRQ_SIO0, 0);
	pad_poll.busy = 0;
	SIO_CTRL(0) = 0x0010;
	FastExitCriticalSection();
}

Q_COLD void pad_init(void)
{
	pad_buf[0][0] = pad_buf[1][0] = 0xff;   /* "no data" until the first poll */
	attempts = 1;
	int analog = pad_force_analog();         /* (not in the printf: the release has none) */
	printf("pad: analog mode %s\n", analog ? "on" : "not set (digital pad, or no answer)");
	driver_start();                          /* (polled from the next vblank on) */
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

/* A poll still going: wait while its bytes keep coming (the game reads the
   pad soon after the vblank; a poll takes 9 bytes of ~40 us on the
   machine), then end it as it is (no pad: no byte comes). */
#define POLL_WAIT 512                    /* timer 2 ticks since its last byte: ~120 us */

static void poll_wait(void)
{
	for (;;) {
		if (!pad_poll.busy)
			return;
		/* the last byte's time read before the timer: an answer taken
		   between the two reads makes this wait a little longer. Read
		   after the timer, a fresher time made the difference wrap to
		   ~65535 and the poll was cut there, half of them on DuckStation
		   (docs/porting-notes.md §101) */
		uint16_t t = pad_poll.t;
		if ((uint16_t) ((uint16_t) TIMER_VALUE(2) - t) >= POLL_WAIT)
			break;
	}
	FastEnterCriticalSection();
	if (pad_poll.busy)
		poll_end();
	FastExitCriticalSection();
}

int pad_frame(pad_t *ticks, int max)
{
	if (driver_on)
		poll_wait();
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
