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

	/* stat 0 = data valid; types 4 (digital), 5/7 (analog) carry buttons */
	pad.connected = p->stat == 0;
	if (pad.connected && (p->type == 4 || p->type == 5 || p->type == 7))
		now = (uint16_t) ~p->btn;

	pad.pressed = now & (uint16_t) ~pad.held;
	pad.held    = now;
}
