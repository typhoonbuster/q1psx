/*
 * q1psx — cooked texture set "QTX3" (architecture.md §7), one per map and
 * fidelity level. Written by `qcook`, read by engine/world.c.
 *
 * The cook packs every texture tile of the map into whole 8bpp texture pages
 * and ships each page as one VRAM rectangle, so the engine just DMAs the
 * rectangles and frees the file. Polygons in the matching world file carry
 * pre-packed tpage/CLUT/UV words pointing into these pages.
 *
 * VRAM layout (halfword coordinates):
 *   framebuffers   (0,0)-(319,479)
 *   CLUT           (0,480), 256 entries (Quake palette)
 *   texture pages  x = 320, 448, 576, 704, 832 (128 halfwords = 256 8bpp
 *                  texels each), y = 0 and 256 -> 10 pages: 0..5 for the
 *                  map, 6..9 for the game data (models' skins, HUD;
 *                  formats/qgame.h, its CLUT at (0,504))
 *   debug font     x >= 960
 *
 * Little-endian; host and R3000 agree, no swapping.
 *
 * Layout:
 *   qtex_header
 *   qtex_rect[num_rects]
 *   pixel data (each rect: w*h halfwords at data_ofs, 32-byte multiple)
 */

#ifndef Q1PSX_QTEX_H
#define Q1PSX_QTEX_H

#include <stdint.h>

#define QTEX_MAGIC "QTX3"

#define QTEX_CLUT_X     0
#define QTEX_CLUT_Y   480
#define QTEX_PAGE_X0  320    /* first page column, halfwords        */
#define QTEX_PAGE_COLS  5    /* page columns at x = 320 + 128 * i   */
#define QTEX_PAGE_ROWS  2    /* page rows at y = 0, 256             */
#define QTEX_PAGES     (QTEX_PAGE_COLS * QTEX_PAGE_ROWS)
#define QTEX_MAP_PAGES  6    /* pages 0..5 hold a map's textures (start
                                at ultra needs all six); 6..9 hold the
                                game data (formats/qgame.h)           */

typedef struct {
	char     magic[4];       /* "QTX3"                                  */
	uint32_t num_rects;
	uint16_t clut[256];      /* PS1 BGR555; pure black is 0x8000 (0x0000
	                            would be transparent)                    */
} qtex_header;

typedef struct {
	uint16_t x, y, w, h;     /* VRAM rectangle, halfword units           */
	uint32_t data_ofs;       /* from file start; w*h*2 bytes, 64-aligned
	                            (the GPU DMA's 16-word unit; the cook
	                            zero-fills up to the next rectangle, the
	                            loader pads a copy of older files) */
} qtex_rect;

/* "QTX4": the same texture set in 4bpp (the menu's 4-bit textures), one per
   map and fidelity level next to its QTX3 (<MAP>_<N>.TX4 on the disc).
   Every tile sits at the same texel position in the same page as in the
   QTX3, so the world file's UVs and texture windows stay valid; each
   texture is quantized to its own 16 colours.
     pages  x = 320 + 64 * p, y = 0 (64 halfwords = 256 4bpp texels)
     CLUTs  16 entries per texture (texture index i) at x = 16 * (i % 20),
            y = 481 + i / 20: under the framebuffers (x < 320), where no
            texture page reaches (the second row's pages, from x 320, go
            down to row 511), rows 481 .. 502 at most
   The CLUT area, x 0 .. 319 of rows 480 .. 511: the Quake palette (QTX3)
   row 480, the 4-bit CLUTs rows 481 .. 502, the game data's palette row
   504, its aux pictures' CLUTs rows 505 .. (formats/qgame.h).
   Layout:
     qtex4_header
     qtex_rect[num_rects]     the CLUT block first, then the pages
     qtex4_tex[num_tex]       at ofs_tex: each texture's CLUT and tpage
                              words, which replace the world's qw_tex ones
     pixel data               as in QTX3 */
#define QTEX4_MAGIC          "QTX5"
#define QTEX4_PAGE_X0        320
#define QTEX4_CLUT_Y         481
#define QTEX4_CLUTS_PER_ROW  20
#define QTEX4_CLUT_ROWS      22

typedef struct {
	char     magic[4];       /* "QTX4"                                   */
	uint32_t num_rects;
	uint32_t num_tex;        /* = the world's num_tex (miptex count)     */
	uint32_t ofs_tex;        /* qtex4_tex[num_tex], from file start      */
} qtex4_header;

typedef struct {
	uint16_t clut, tpage;    /* 4bpp tpage; 0, 0 for an unused miptex    */
} qtex4_tex;

#endif
