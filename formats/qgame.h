/*
 * q1psx — cooked game data "QGM1": the models, sprites and HUD pictures the
 * engine needs on every map. Written by `qcook game`, loaded once at boot by
 * engine/gdata.c (GAME.DAT on the disc).
 *
 * Models
 *   A Quake MDL becomes a set of face records in the world's format
 *   (formats/qworld.h: qw_record + vertex indices + light bytes + quads /
 *   tris), so the renderer draws it with the world's own record loop, and
 *   one vertex pool per frame (qw_vert[num_verts], vertex units = world x 4,
 *   the vertex's light-normal index in the first spare byte). A record's
 *   indices point into whichever frame's pool the engine selects. The light
 *   bytes in the stream are placeholders the engine fills per draw (the
 *   stream must live in RAM). Record face ids are 0: the engine makes them
 *   pass its visibility stamp. Skins are in the palette, at half resolution
 *   (view models) or a quarter (projectiles).
 *   Sprites are models too: one record (a quad in the view plane, x right,
 *   y down) per frame, drawn alone (QGF_SPRITE).
 *
 * VRAM
 *   The texture pages the maps leave free (qtex.h QTEX_MAP_PAGES and up) and
 *   one 256-colour CLUT: the Quake palette with index 255 transparent (the
 *   pictures' transparent colour).
 *
 * Little-endian; every section 4-byte aligned, offsets from the file start.
 */

#ifndef Q1PSX_QGAME_H
#define Q1PSX_QGAME_H

#include <stdint.h>

#include "qworld.h"

#define QGAME_MAGIC "QGM1"

#define QG_CLUT_X   0
#define QG_CLUT_Y   504          /* below the 4bpp texture CLUT rows (480..488) */

#define QG_LIGHT_YAWS 16         /* light tables: one per 22.5 degrees of yaw */
#define QG_NORMALS   162         /* Quake's vertex normals (anorms.h)         */

/* models, in this order in the file */
enum {
	QGM_V_AXE, QGM_V_SHOT, QGM_V_SHOT2, QGM_V_NAIL, QGM_V_NAIL2, QGM_V_ROCK, QGM_V_ROCK2,
	QGM_V_LIGHT,                 /* the 8 view models: weapon 0..7               */
	QGM_SPIKE, QGM_S_SPIKE, QGM_GRENADE, QGM_MISSILE, QGM_BOLT,
	QGM_S_EXPLOD,                /* the explosion sprite, 6 frames               */
	QGM_S_BUBBLE,                /* the air bubble sprite, 2 frames              */
	QGM_COUNT
};

#define QGF_SPRITE   1           /* record k = frame k's quad: draw that one only */
#define QGF_FLAT     2           /* no directional light (a small model)          */

typedef struct {
	uint16_t id;                 /* QGM_*                                        */
	uint16_t flags;              /* QGF_*                                        */
	uint16_t num_frames;
	uint16_t num_verts;          /* per frame: the pool entries                  */
	uint16_t num_records;
	uint16_t radius;             /* vertex units, about the origin, every frame  */
	uint32_t ofs_frames;         /* in the geometry block: qw_vert[num_frames][num_verts] */
	uint32_t ofs_records;        /* in the geometry block: the record stream     */
	uint32_t len_records;
	qw_tex   tex;                /* the skin: CLUT, tpage, window (none)         */
} qg_model;                      /* 32 bytes */

/* pictures, in this order in the file (gfx.wad lumps) */
enum {
	QGP_NUM_0,                   /* 0-9 then the minus sign; big digits, 24 x 24  */
	QGP_NUM_MINUS = QGP_NUM_0 + 10,
	QGP_ANUM_0,                  /* the same in red (low health / ammo)          */
	QGP_ANUM_MINUS = QGP_ANUM_0 + 10,
	QGP_NUM_COLON, QGP_NUM_SLASH,
	QGP_SBAR_L, QGP_SBAR_R,      /* the status bar's two halves, 160 x 24        */
	QGP_IBAR_L, QGP_IBAR_R,      /* the inventory bar                            */
	QGP_SCOREBAR_L, QGP_SCOREBAR_R,
	QGP_ARMOR1, QGP_ARMOR2, QGP_ARMOR3,
	QGP_AMMO_SHELLS, QGP_AMMO_NAILS, QGP_AMMO_ROCKET, QGP_AMMO_CELLS,
	QGP_INV_SHOTGUN,             /* 7 weapon icons: 24 x 16, the lightning 48 x 16 */
	QGP_INV2_SHOTGUN = QGP_INV_SHOTGUN + 7,      /* selected                     */
	QGP_INVA_SHOTGUN = QGP_INV2_SHOTGUN + 7,     /* 5 flash frames x 7 weapons   */
	QGP_KEY1 = QGP_INVA_SHOTGUN + 35,            /* sb_items: keys, powerups     */
	QGP_KEY2, QGP_INVIS, QGP_INVULN, QGP_SUIT, QGP_QUAD,
	QGP_SIGIL1, QGP_SIGIL2, QGP_SIGIL3, QGP_SIGIL4,
	QGP_FACE1, QGP_FACE2, QGP_FACE3, QGP_FACE4, QGP_FACE5,   /* dying .. healthy */
	QGP_FACE_P1, QGP_FACE_P2, QGP_FACE_P3, QGP_FACE_P4, QGP_FACE_P5,   /* in pain */
	QGP_FACE_INVIS, QGP_FACE_INVUL1, QGP_FACE_INVUL2, QGP_FACE_INV2, QGP_FACE_QUAD,
	QGP_SMALLNUM,                /* the inventory bar's yellow digits 0-9, 80 x 8 */
	QGP_COUNT
};

typedef struct {
	uint16_t tpage;              /* GPU tpage word, 8bpp                         */
	uint8_t  u, v;               /* page-relative texel origin                   */
	uint16_t w, h;               /* texels                                       */
} qg_pic;                        /* 8 bytes */

typedef struct {
	char     magic[4];           /* "QGM1"                                       */
	uint32_t num_rects, ofs_rects;   /* qtex_rect[]: the CLUT row, then the pages' used rows */
	uint32_t num_models, ofs_models; /* qg_model[QGM_COUNT]                      */
	uint32_t num_pics, ofs_pics;     /* qg_pic[QGP_COUNT]                        */
	uint32_t ofs_dots;           /* uint8_t[QG_LIGHT_YAWS][QG_NORMALS]: a vertex's
	                                light factor (x128) by its normal, for a model
	                                yawed by k x 22.5 degrees                    */
	uint32_t ofs_rgb;            /* uint32_t[256]: the palette as GPU colour
	                                words (0x00BBGGRR), for particles            */
	uint32_t ofs_geo, len_geo;   /* the models' geometry block (frames, records) */
	uint16_t clut;               /* GPU CLUT word of the game palette            */
	uint16_t pad;
} qg_header;

#endif
