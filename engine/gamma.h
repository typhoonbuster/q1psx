/*
 * q1psx — the Options menu's Brightness: Quake's gamma (menu.c
 * M_AdjustSliders: v_gamma from 1 down to 0.5, a twentieth a step;
 * view.c BuildGammaTable on the palette), on the CLUTs and on the light
 * (engine/gamma.c).
 */

#ifndef Q1PSX_GAMMA_H
#define Q1PSX_GAMMA_H

#include <stdint.h>

#include "qtex.h"

#define GAMMA_LEVELS 11          /* the slider's steps: v_gamma 1, 0.95, .. 0.5 */

/* the level the CLUTs are at now (0: Quake's 1, the colours as cooked) */
int  gamma_level(void);

/* to a level: the map's and the game's CLUTs sent again through its table */
void gamma_set(int level);

/* a CLUT's halfwords to VRAM (x, y, w, h: halfwords) through the current
   level's table, a row at a time (the data itself left as it is) */
void gamma_upload(const uint8_t *data, int x, int y, int w, int h);

/* the CLUTs as cooked, kept to be sent again: the game's palette (256
   halfwords, copied), the map's CLUT rectangles (theirs, kept by the
   caller for the map: data_ofs from base) */
void gamma_keep_game(const uint8_t *pal);
void gamma_keep_map(const qtex_rect *rects, const uint8_t *base, uint32_t n);
#ifdef Q1PSX_BENCH
uint32_t gamma_kept_hash(int *rects);
#endif

/* The light's share (engine/gamma.c): the light bytes in RAM are at
   gamma_light_level(); what the disc gives goes through gamma_light (the
   curve at that level, the identity at 0), and so does a model's light. */
extern uint8_t gamma_light[256];
int  gamma_light_level(void);

/* the slider's level for the light (gamma_set's) differs from the bytes'?
   t[256] then takes each byte from the old level to the new one, which
   becomes gamma_light_level(); the caller puts every resident byte through
   t (between frames: engine/main.c light_commit) */
int  gamma_light_change(uint8_t t[256]);

/* the whole records in [p, end), n at most, until one of size 0: their
   light bytes through t, their average light in their flags again (the
   flat shading's, engine/world.c); where it stopped (the first record
   left). engine/zone.s (the C engine: engine/gamma.c). */
uint8_t *gamma_light_records(uint8_t *p, const uint8_t *end, uint32_t n, const uint8_t *t);

#endif
