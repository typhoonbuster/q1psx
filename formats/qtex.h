/*
 * q1psx — cooked texture set "QTX8" (architecture.md §7), one per map
 * (<MAP>.TX4 on the disc). Written by `qcook`, read by engine/world.c.
 *
 * The cook packs every texture tile of the map into whole 4bpp texture
 * pages and ships each page as one VRAM rectangle, so the engine just DMAs
 * the rectangles and frees the file. Each texture is quantized to its own
 * 16 colours. A tile's axis that does not wrap (its texture's size there
 * is not a power of two) has a gutter: the texel column / row past the
 * tile's end repeats its first, for the GPU reads it along a polygon's far
 * edge (its texture coordinates are rounded to the nearest texel:
 * docs/porting-notes.md §71). The set also carries the map's texture
 * table: each slot's CLUT, tpage and texture-window words (qw_tex, which
 * the renderer reads), and the animated textures' tiles (qw_anim). The
 * world's and the map's models' UVs are on this set's tiles (the cook maps
 * them from the finest layout it cuts the faces for: tools/qcook
 * tex_uvmaps).
 *
 * VRAM layout (halfword coordinates):
 *   framebuffers   (0,0)-(319,479)
 *   CLUTs          16 entries per texture (texture index i) at
 *                  x = 16 * (i % 20), y = 481 + i / 20: under the
 *                  framebuffers (x < 320), where no texture page reaches
 *                  (the second row's pages, from x 320, go down to row
 *                  511), rows 481 .. 502 at most; the game data's palette
 *                  row 504, its aux pictures' CLUTs rows 505 ..
 *                  (formats/qgame.h)
 *   texture pages  the map's at x = 320 + 64 * p, y = 0 (64 halfwords =
 *                  256 4bpp texels); the page grid (QTEX_PAGE_*): x = 320,
 *                  448, 576, 704, 832, y = 0 and 256, 10 pages: 0..5 the
 *                  map's room, 6..9 the game data's (formats/qgame.h)
 *   debug font     x >= 960
 *
 * Little-endian; host and R3000 agree, no swapping.
 *
 * Layout:
 *   qtex4_header
 *   qtex_rect[num_rects]     the CLUT block first, then the pages
 *   qw_tex[num_slots]                            at ofs_slots
 *   qw_anim[num_anims]                           at ofs_anims
 *   pixel data (each rect: w*h halfwords at data_ofs, 64-byte aligned)
 */

#ifndef Q1PSX_QTEX_H
#define Q1PSX_QTEX_H

#include <stdint.h>

#define QTEX_PAGE_X0  320    /* first page column, halfwords        */
#define QTEX_PAGE_COLS  5    /* page columns at x = 320 + 128 * i   */
#define QTEX_PAGE_ROWS  2    /* page rows at y = 0, 256             */
#define QTEX_PAGES     (QTEX_PAGE_COLS * QTEX_PAGE_ROWS)
#define QTEX_MAP_PAGES  6    /* pages 0..5 hold a map's textures (start
                                at ultra needs all six); 6..9 hold the
                                game data (formats/qgame.h)           */

/* One per texture slot (the map's miptex, its own models' textures, the
   animated textures' display tiles; unused ones are zero). */
typedef struct {
	uint16_t clut;         /* GPU CLUT word                                   */
	uint16_t tpage;        /* GPU tpage word                                  */
	uint32_t window;       /* GP0 E2h texture-window command; 0xE2000000 = no
	                          wrap (the axes that are not powers of two)      */
} qw_tex;                  /* 8 bytes */

/* An animated texture (Quake's "+0name" .. "+9name" and the alternates
   "+aname" .. "+jname", R_TextureAnimation): every frame is a tile of the
   map's texture pages, and the faces of the chain draw with a display tile
   of their own; the engine copies the frame of the moment into it (a
   frame every 0.2 s; the alternates while a brush model listed has its
   frame set: a pressed button, a toggled wall). Tiles in texels of the
   map's 4-bit pages (page p at x 320 + 64 p, y 0), with their
   texture-table index (its qw_tex: the CLUT). */
#define QW_ANIM_FRAMES 10
#define QW_ANIM_MODELS 8
typedef struct {
	uint8_t  page, pad;
	uint16_t slot;             /* its qw_tex index                        */
	uint16_t u0, v0, w, h;     /* texels in its page, the gutter included */
} qw_anim_tile;                /* 12 bytes */

typedef struct {
	qw_anim_tile display;
	uint8_t      num_frames, num_alt, num_models, pad;
	qw_anim_tile frames[QW_ANIM_FRAMES];
	qw_anim_tile alt[QW_ANIM_FRAMES];
	uint16_t     models[QW_ANIM_MODELS];   /* brush models drawing it (their frame) */
} qw_anim;                     /* 272 bytes */

typedef struct {
	uint16_t x, y, w, h;     /* VRAM rectangle, halfword units           */
	uint32_t data_ofs;       /* from file start; w*h*2 bytes, 64-aligned
	                            (the GPU DMA's 16-word unit; the cook
	                            zero-fills up to the next rectangle, the
	                            loader pads a copy of older files) */
} qtex_rect;

#define QTEX4_MAGIC          "QTX8"   /* (QTX7 until §119: the UV maps the loader
                                         applied, after the slots' words) */
#define QTEX4_PAGE_X0        320
#define QTEX4_CLUT_Y         481
#define QTEX4_CLUTS_PER_ROW  20
#define QTEX4_CLUT_ROWS      22

typedef struct {
	char     magic[4];       /* "QTX8"                                   */
	uint32_t num_rects;
	uint32_t num_slots, ofs_slots;   /* qw_tex                           */
	uint32_t num_anims, ofs_anims;
} qtex4_header;

#endif
