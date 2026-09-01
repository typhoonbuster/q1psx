#include <stdint.h>
#include <psxpad.h>
#include <psxapi.h>

#include "pad.h"

/* BIOS pad driver buffers, refreshed automatically on every vblank. */
static uint8_t pad_buff[2][34];

static uint16_t prev_buttons;

void pad_init(void)
{
	InitPAD(pad_buff[0], 34, pad_buff[1], 34);
	StartPAD();
	ChangeClearPAD(0); /* leave the vblank IRQ ack to the main loop */
	prev_buttons = 0;
}

void pad_read(PadState *out)
{
	const PADTYPE *pad = (const PADTYPE *) pad_buff[0];

	out->connected = (pad->stat == 0);
	out->type      = pad->type;

	/* BIOS reports buttons active-LOW; invert so a set bit means "pressed". */
	uint16_t held = out->connected ? (uint16_t) ~pad->btn : 0;

	out->buttons = held;
	out->pressed = held & ~prev_buttons;
	prev_buttons = held;
}
