/*
 * q1psx — cooked texture pack ("QTX2"), the output of `qcook textures`.
 *
 * One shared 256-entry CLUT (Quake's global palette in PS1 BGR555) + the
 * textures used by one map, 8bpp indexed, each with a VRAM slot assigned at
 * cook time (a simple grid atlas in the free region right of the framebuffers).
 * Entry i == miptex i for that map (gaps get a 2x2 magenta placeholder) so BSP
 * faces can index straight in.
 *
 * Little-endian; host x86 and the PS1 R3000 are both LE, no swapping.
 *
 * Layout:
 *   qtx_header
 *   qtx_entry  [num_tex]
 *   <8bpp pixel data, each entry at data_ofs, length w*h, 4-byte aligned>
 */

#ifndef Q1PSX_QTX_H
#define Q1PSX_QTX_H

#include <stdint.h>

#define QTX_MAGIC   "QTX2"
#define QTX_NAMELEN 16

/* VRAM atlas: 64x64 texel cells to the right of the two 320x240 framebuffers,
 * left of the debug font at x=960. Word (16-bit) coordinates; an 8bpp cell is
 * 32 words wide, 64 lines tall. 20 x 7 = 140 cells. The shared CLUT goes at
 * (QTX_ATLAS_X, 480), below the grid. */
#define QTX_ATLAS_X      320
#define QTX_ATLAS_Y        0
#define QTX_ATLAS_CELL_W  32   /* words  (64 8bpp texels) */
#define QTX_ATLAS_CELL_H  64
#define QTX_ATLAS_COLS    20
#define QTX_ATLAS_ROWS     7
#define QTX_CLUT_X       320
#define QTX_CLUT_Y       480

typedef struct {
	char     magic[4];       /* "QTX2" */
	uint16_t clut[256];      /* PS1 BGR555, bit15 = semi-transparent */
	uint32_t num_tex;
	uint32_t reserved;
} qtx_header;

typedef struct {
	char     name[QTX_NAMELEN];
	uint16_t width, height;         /* cooked size (<= 64 for world tiles)   */
	uint16_t src_width, src_height; /* original size                         */
	uint16_t vram_x, vram_y;        /* LoadImage() destination, word coords  */
	uint8_t  flags;                 /* QTX_F_*                               */
	uint8_t  pad[3];
	uint32_t data_ofs;              /* byte offset from file start           */
	uint32_t data_len;              /* == width * height                     */
} qtx_entry;

#define QTX_F_SKY         (1 << 0)  /* name starts "sky" */
#define QTX_F_LIQUID      (1 << 1)  /* name starts "*"   */
#define QTX_F_MASKED      (1 << 2)  /* name starts "{" — palette 255 is clear */
#define QTX_F_PLACEHOLDER (1 << 3)  /* missing miptex, filled with a marker   */

#endif
