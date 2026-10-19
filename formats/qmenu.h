/*
 * q1psx — what the release's menu shows before any map is loaded: the
 * console's background "QCB1" (CONBACK.DAT on the disc). Written by `qcook
 * menu` (tools/qcook/menu.c), read by engine/ui.c.
 *
 * With no world and no demo, Quake's console is forced down
 * (SCR_SetUpToDrawConsole: con_forcedup) and its menu draws the console's
 * background in place of the faded game (M_Draw: Draw_ConsoleBackground,
 * gfx/conback.lmp stretched over the screen's height). The engine uploads
 * the picture once, where the map's texture pages go (no map is loaded
 * yet: a map's load writes its own textures over it), its palette to the
 * map palette's CLUT row (formats/qtex.h).
 *
 * Layout: qcb_header, the CLUT (256 halfwords, PS1 BGR555, every entry
 * opaque: Draw_ConsoleBackground copies the bytes, index 255 included),
 * then w x h palette indices (64-byte aligned, a whole number of 64-byte
 * units: LoadImage's DMA). Little-endian.
 */

#ifndef Q1PSX_QMENU_H
#define Q1PSX_QMENU_H

#include <stdint.h>

#define QCB_MAGIC "QCB1"

#define QCB_VRAM_X  320      /* the map's first texture page (formats/qtex.h)  */
#define QCB_VRAM_Y  0
#define QCB_CLUT_X  0        /* the map palette's row                          */
#define QCB_CLUT_Y  480

typedef struct {
	char     magic[4];       /* "QCB1"                                        */
	uint16_t w, h;           /* texels: 320 x 200                             */
	uint32_t ofs_clut;       /* uint16_t[256]                                 */
	uint32_t ofs_pixels;     /* w x h bytes                                   */
} qcb_header;

#endif
