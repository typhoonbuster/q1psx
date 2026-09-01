/*
 * q1psx — cooked texture pack ("QTX1"), the output of `qcook textures`.
 *
 * Little-endian (host x86 and the PS1 R3000 are both LE, so no swapping on
 * either side). One shared 256-entry CLUT (Quake's global palette in PS1
 * BGR555) followed by 8bpp indexed textures. This is a phase-4.1 container for
 * getting pixels on screen; the BSP geometry format comes with the renderer.
 *
 * Layout:
 *   qtx_header
 *   qtx_entry  [num_tex]
 *   <8bpp pixel data, each entry's block at data_ofs, length w*h, 4-byte aligned>
 */

#ifndef Q1PSX_QTX_H
#define Q1PSX_QTX_H

#include <stdint.h>

#define QTX_MAGIC   "QTX1"
#define QTX_NAMELEN 16

typedef struct {
	char     magic[4];       /* "QTX1"                                   */
	uint16_t clut[256];      /* PS1 BGR555, bit15 = semi-transparent     */
	uint32_t num_tex;
	uint32_t reserved;
} qtx_header;

typedef struct {
	char     name[QTX_NAMELEN]; /* Quake texture name, NUL-padded        */
	uint16_t width, height;     /* after fidelity downscale (power of 2) */
	uint16_t src_width, src_height; /* original size, for UV scaling     */
	uint8_t  flags;             /* QTX_F_*                               */
	uint8_t  pad[3];
	uint32_t data_ofs;          /* byte offset from file start           */
	uint32_t data_len;          /* == width * height                     */
} qtx_entry;

#define QTX_F_SKY    (1 << 0)   /* name starts "sky"  */
#define QTX_F_LIQUID (1 << 1)   /* name starts "*"    */
#define QTX_F_MASKED (1 << 2)   /* name starts "{" — palette index 255 is transparent */

#endif
