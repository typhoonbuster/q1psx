/*
 * q1psx — what the release shows before any map is loaded: the console's
 * background "QCB2" (CONBACK.DAT on the disc), and what the boot's
 * loading screen draws on it before the game data is in (engine/loading.c).
 * Written by `qcook menu` (tools/qcook/menu.c), read by engine/ui.c.
 *
 * With no world and no demo, Quake's console is forced down
 * (SCR_SetUpToDrawConsole: con_forcedup) and its menu draws the console's
 * background in place of the faded game (M_Draw: Draw_ConsoleBackground,
 * gfx/conback.lmp stretched over the screen's height). The engine uploads
 * the picture once, where the map's texture pages go (no map is loaded
 * yet: a map's load writes its own textures over it), its palette to the
 * map palette's CLUT row (formats/qtex.h). The picture carries the version
 * number in its corner, written in at the cook as Draw_ConsoleBackground
 * writes it in (Draw_CharToConback: the font's texels + 0x60).
 *
 * The boot's pictures (the game data's aux pictures are not in VRAM yet):
 * the loading plaque (gfx/loading.lmp, 144 x 24, through the background's
 * CLUT: Draw_Pic is opaque) and six of the console font's glyphs for the
 * loading bar (conchars 128, 129, 130: the slider's track; 29, 30, 31: the
 * console's separator line), 8 x 8 each from texel 144 of the strip, through
 * the glyph CLUT (the palette with index 0 transparent: Draw_Character).
 * The strip goes under the background, the glyph CLUT to the map's first
 * texture CLUT row (no map is loaded at boot).
 *
 * Layout: qcb_header, the CLUT (256 halfwords, PS1 BGR555, every entry
 * opaque: Draw_ConsoleBackground copies the bytes, index 255 included),
 * the glyph CLUT (256 halfwords), then w x h palette indices, then the
 * boot strip's QCB_BOOT_W x QCB_BOOT_H (each section 64-byte aligned, a
 * whole number of 64-byte units: LoadImage's DMA). Little-endian.
 */

#ifndef Q1PSX_QMENU_H
#define Q1PSX_QMENU_H

#include <stdint.h>

#define QCB_MAGIC "QCB2"     /* (QCB1 until §120: no version, no boot strip) */

#define QCB_VRAM_X  320      /* the map's first texture page (formats/qtex.h)  */
#define QCB_VRAM_Y  0
#define QCB_CLUT_X  0        /* the map palette's row                          */
#define QCB_CLUT_Y  480
#define QCB_BOOT_X  320      /* the boot strip: under the background, in the   */
#define QCB_BOOT_Y  200      /* same texture page (halfwords)                  */
#define QCB_BOOT_W  192      /* texels (8 bits): the plaque, then the glyphs   */
#define QCB_BOOT_H  24
#define QCB_GLYPHS_U 144     /* the glyphs' first texel in the strip           */
#define QCB_GLYPH_CLUT_X 0   /* the map's first texture CLUT row (formats/qtex.h) */
#define QCB_GLYPH_CLUT_Y 481
#define QCB_VERSION "(PlayStation) 1.09"   /* (Draw_ConsoleBackground's "(WinQuake) 1.09") */

typedef struct {
	char     magic[4];       /* "QCB2"                                        */
	uint16_t w, h;           /* texels: 320 x 200                             */
	uint32_t ofs_clut;       /* uint16_t[256]                                 */
	uint32_t ofs_pixels;     /* w x h bytes                                   */
	uint32_t ofs_glyph_clut; /* uint16_t[256]                                 */
	uint32_t ofs_boot;       /* QCB_BOOT_W x QCB_BOOT_H bytes                 */
} qcb_header;

#endif
