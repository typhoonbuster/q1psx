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
 *                  texels each), y = 0 and 256 -> 10 pages
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

typedef struct {
	char     magic[4];       /* "QTX3"                                  */
	uint32_t num_rects;
	uint16_t clut[256];      /* PS1 BGR555; pure black is 0x8000 (0x0000
	                            would be transparent)                    */
} qtex_header;

typedef struct {
	uint16_t x, y, w, h;     /* VRAM rectangle, halfword units           */
	uint32_t data_ofs;       /* from file start; w*h*2 bytes, 32-aligned */
} qtex_rect;

#endif
