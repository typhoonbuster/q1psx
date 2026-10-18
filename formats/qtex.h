/*
 * q1psx — cooked texture set "QTX6" (architecture.md §7), one per map and
 * texture level (formats/qlevels.h). Written by `qcook`, read by
 * engine/world.c.
 *
 * The cook packs every texture tile of the map into whole 8bpp texture pages
 * and ships each page as one VRAM rectangle, so the engine just DMAs the
 * rectangles and frees the file. The set also carries the map's texture
 * table: each slot's CLUT, tpage and texture-window words (qw_tex, which the
 * renderer reads), how the world's texture coordinates land on its tile
 * (qtex_uvmap), and the animated textures' tiles (qw_anim). The world file
 * is the same for every texture level (formats/qworld.h): its polygons'
 * UVs are cooked for the finest level, and the loader maps them onto this
 * set's tiles once, at load.
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
 *   qw_tex[num_slots], qtex_uvmap[num_slots]     at ofs_slots
 *   qw_anim[num_anims]                           at ofs_anims
 *   pixel data (each rect: w*h halfwords at data_ofs, 64-byte aligned)
 */

#ifndef Q1PSX_QTEX_H
#define Q1PSX_QTEX_H

#include <stdint.h>

#define QTEX_MAGIC "QTX6"

#define QTEX_CLUT_X     0
#define QTEX_CLUT_Y   480
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

/* Where the world's texture coordinates land in this set. A world file's
   UVs are in texels of the finest texture level: on an axis that wraps (a
   power of two: the texture window folds it) a virtual 0..255 coordinate,
   otherwise from the tile's corner. At load each one becomes
     u' = u0 + ((u * su + rnd) >> 15)        (and v' alike)
   su = 32768 at the finest level; a level at half the resolution has
   16384. u0, v0: the tile's corner in its page on an axis that does not
   wrap, 0 on one that does (a model's skin: its own corner, inside its
   border). rnd: 16384 rounds (a miptex: the cook rounds a world texel),
   0 truncates (a skin: the cook divides a skin texel). */
typedef struct {
	uint8_t  u0, v0;
	uint16_t su, sv;
	uint16_t rnd;
} qtex_uvmap;              /* 8 bytes */

/* An animated texture (Quake's "+0name" .. "+9name" and the alternates
   "+aname" .. "+jname", R_TextureAnimation): every frame is a tile of the
   map's texture pages, and the faces of the chain draw with a display tile
   of their own; the engine copies the frame of the moment into it (a
   frame every 0.2 s; the alternates while a brush model listed has its
   frame set: a pressed button, a toggled wall). Tiles in texels of the
   8-bit pages (page p at x 320 + 128 (p % 5), y 256 (p / 5); in the 4-bit
   set, x 320 + 64 p, y 0, the same texels), with their texture-table index
   (the 4-bit set's CLUT for it, 16 a row from (0, 480)). */
#define QW_ANIM_FRAMES 10
#define QW_ANIM_MODELS 8
typedef struct {
	uint8_t  page, pad;
	uint16_t slot;             /* its qw_tex index                        */
	uint16_t u0, v0, w, h;     /* texels in its page                      */
} qw_anim_tile;                /* 12 bytes */

typedef struct {
	qw_anim_tile display;
	uint8_t      num_frames, num_alt, num_models, pad;
	qw_anim_tile frames[QW_ANIM_FRAMES];
	qw_anim_tile alt[QW_ANIM_FRAMES];
	uint16_t     models[QW_ANIM_MODELS];   /* brush models drawing it (their frame) */
} qw_anim;                     /* 272 bytes */

typedef struct {
	char     magic[4];       /* "QTX6"                                  */
	uint32_t num_rects;
	uint16_t clut[256];      /* PS1 BGR555; pure black is 0x8000 (0x0000
	                            would be transparent)                    */
	uint32_t num_slots;      /* the texture table (= the world's num_tex) */
	uint32_t ofs_slots;      /* qw_tex[num_slots] then qtex_uvmap[num_slots] */
	uint32_t num_anims;      /* qw_anim[num_anims] (0 when the level's pages
	                            had no room for the frames: the faces show
	                            their first)                             */
	uint32_t ofs_anims;
} qtex_header;

typedef struct {
	uint16_t x, y, w, h;     /* VRAM rectangle, halfword units           */
	uint32_t data_ofs;       /* from file start; w*h*2 bytes, 64-aligned
	                            (the GPU DMA's 16-word unit; the cook
	                            zero-fills up to the next rectangle, the
	                            loader pads a copy of older files) */
} qtex_rect;

/* "QTX7": the same texture set in 4bpp (the menu's 4-bit textures), one per
   map and texture level next to its QTX6 (<MAP>_T<N>.TX4 on the disc).
   Every tile sits at the same texel position in the same page as in the
   QTX6, so the UV maps and texture windows are the same; each texture is
   quantized to its own 16 colours, so its table's CLUT and tpage words are
   the 4-bit ones.
     pages  x = 320 + 64 * p, y = 0 (64 halfwords = 256 4bpp texels)
     CLUTs  16 entries per texture (texture index i) at x = 16 * (i % 20),
            y = 481 + i / 20: under the framebuffers (x < 320), where no
            texture page reaches (the second row's pages, from x 320, go
            down to row 511), rows 481 .. 502 at most
   The CLUT area, x 0 .. 319 of rows 480 .. 511: the Quake palette (QTX6)
   row 480, the 4-bit CLUTs rows 481 .. 502, the game data's palette row
   504, its aux pictures' CLUTs rows 505 .. (formats/qgame.h).
   Layout:
     qtex4_header
     qtex_rect[num_rects]     the CLUT block first, then the pages
     qw_tex[num_slots], qtex_uvmap[num_slots], qw_anim[num_anims]   as QTX6
     pixel data               as in QTX6 */
#define QTEX4_MAGIC          "QTX7"
#define QTEX4_PAGE_X0        320
#define QTEX4_CLUT_Y         481
#define QTEX4_CLUTS_PER_ROW  20
#define QTEX4_CLUT_ROWS      22

typedef struct {
	char     magic[4];       /* "QTX7"                                   */
	uint32_t num_rects;
	uint32_t num_slots, ofs_slots;   /* as QTX6's                        */
	uint32_t num_anims, ofs_anims;
} qtex4_header;

#endif
