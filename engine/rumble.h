/*
 * q1psx — the DualShock's vibration (docs/porting-notes.md §120): the
 * game's shots, the player's wounds and death, the explosions near him,
 * as pulses of the pad's two motors (engine/pad.h pad_motor_*), run at
 * the player's ticks. The Options' Vibration turns it off; the menu and
 * the loads stop it.
 */

#ifndef Q1PSX_RUMBLE_H
#define Q1PSX_RUMBLE_H

#include <stdint.h>

/* the large motor at `speed` (0x60 .. 0xff: under ~0x50 it does not
   start) for `ticks` player ticks: the stronger of the running pulse and
   this one */
void rumble_pulse(int speed, int ticks);
/* the small motor's buzz for `ticks` */
void rumble_buzz(int ticks);
/* a wound of `take` points (T_Damage's take) */
void rumble_hit(int take);
/* an explosion at org (1/8 units): felt within 256 units of the player */
void rumble_blast(const int32_t org[3]);
/* once a player tick: the motors' bytes from the pulses left */
void rumble_tick(void);
/* the motors off now, the pulses dropped */
void rumble_stop(void);

#endif
