#include <stdint.h>
#include <psxapi.h>
#include <psxpad.h>

#include "pad.h"

static uint8_t pad_buf[2][34];

pad_t pad;

void pad_init(void)
{
	InitPAD(pad_buf[0], 34, pad_buf[1], 34);
	pad_buf[0][0] = pad_buf[1][0] = 0xff;   /* "no data" until the first poll */
	StartPAD();
	ChangeClearPAD(0);   /* let psxgpu's handler see the vblank IRQ too */
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
}
