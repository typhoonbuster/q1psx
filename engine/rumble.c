/*
 * q1psx — the DualShock's vibration. See engine/rumble.h.
 */

#include <stdint.h>

#include "clip.h"
#include "game.h"
#include "pad.h"
#include "rumble.h"
#include "settings.h"

static int large_speed, large_left, small_left;   /* the pulses' speed and ticks left */

void rumble_pulse(int speed, int ticks)
{
	if (!settings.vibration)
		return;
	if (speed > 255)
		speed = 255;
	if (large_left <= 0 || speed >= large_speed) {
		large_speed = speed;
		if (ticks > large_left)
			large_left = ticks;
	}
}

void rumble_buzz(int ticks)
{
	if (settings.vibration && ticks > small_left)
		small_left = ticks;
}

void rumble_hit(int take)
{
	if (take > 0)
		rumble_pulse(0x60 + take * 4, 6 + take / 4);
}

void rumble_blast(const int32_t org[3])
{
	uint32_t d2 = 0;
	for (int k = 0; k < 3; k++) {
		int32_t d = (org[k] - pl.pm.pos[k]) / CLIP_SUB;
		if (d <= -256 || d >= 256)
			return;
		d2 += (uint32_t) (d * d);
	}
	if (d2 < 256 * 256)                              /* 0xff at the player, 0x80 at 256 units */
		rumble_pulse(0xff - (int) (d2 * 0x7f / (256 * 256)), 12);
}

void rumble_tick(void)
{
	uint8_t big = 0, small = 0;
	if (large_left > 0) {
		large_left--;
		big = (uint8_t) large_speed;
	}
	if (small_left > 0) {
		small_left--;
		small = 1;
	}
	pad_motor_large = big;
	pad_motor_small = small;
}

void rumble_stop(void)
{
	large_left = small_left = 0;
	pad_motor_large = pad_motor_small = 0;
}
