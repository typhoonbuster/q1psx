/*
 * q1psx — the Options menu's Brightness: Quake's gamma (menu.c
 * M_AdjustSliders: v_gamma from 1 down to 0.5, a twentieth a step;
 * view.c BuildGammaTable on the palette), on the CLUTs (engine/gamma.c).
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

#endif
