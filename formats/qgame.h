/*
 * q1psx — cooked game data "QGM8": the models, sprites and HUD pictures the
 * engine needs on every map. Written by `qcook game`, one file per model
 * level (GAME_<n>.DAT on the disc, formats/qlevels.h: the models'
 * triangles differ, the pictures and skins do not), loaded at boot by
 * engine/gdata.c, its models again when the level changes.
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
 * The 2D weapons (Graphics: "Draw weapon: 2D")
 *   Every view model frame as a picture, rendered by the cook as the eye
 *   sees it (the status bar's viewport, the gun at rest), at half the
 *   screen's resolution (as the view models' skins), its colour scaled for
 *   the light by the engine: one 255-colour CLUT per weapon (index 0
 *   transparent). The engine copies the frame on screen into one slot of
 *   the game's pages (qg_header wslot) and the weapon's CLUT to
 *   (QG_WCLUT_X, QG_WCLUT_Y) through the display list, and draws one quad
 *   twice the size. Both are stored as ready display-list packets (a tag
 *   word, GP0(A0h) to the slot, the texels; a frame's chain ends with
 *   GP0(01h), the texture cache cleared): the engine makes their links
 *   absolute at the load and hangs a chain in the ordering table as it is.
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

#define QGAME_MAGIC "QGM8"   /* (QGM7 until §47: a frame's whole texels, a chain each) */

#define QG_CLUT_X   0
#define QG_CLUT_Y   504          /* below the 4bpp texture CLUT rows (480..488) */

#define QG_WCLUT_X  0
#define QG_WCLUT_Y  507          /* the 2D weapon's CLUT: two free rows under the aux
                                    CLUTs, 507 and 508, in turn at each weapon (the
                                    GPU's CLUT cache never keeps the last one's) */

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
#define QGF_MAP      4           /* a map's model (formats/qworld.h's models
                                    section): its geometry in the map's block,
                                    its textures among the map's               */
#define QGF_ROTATE   8           /* EF_ROTATE: the pickups turn, 100 deg / s   */

typedef struct {
	uint16_t id;                 /* QGM_* (a map's model: QIM_*)                 */
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

/* A map's own models (the items', the flames', the explosive boxes'): its
   models file (qmm_header below), each the cooked model (frames and
   records as GAME.DAT's, offsets in the file's geometry block) and the
   entries of the map's texture table its records' tex 0 .. num_tex - 1
   stand for (a skin among the map's textures, or a BSP item's few). Only
   the models the map's entities use are cooked. Their UVs are the world
   file's kind (formats/qtex.h qtex_uvmap: the finest texture level's,
   tile-relative), mapped at load like the world's. */
enum {
	QIM_G_SHOT, QIM_G_NAIL, QIM_G_NAIL2, QIM_G_ROCK, QIM_G_ROCK2, QIM_G_LIGHT,
	QIM_ARMOR1, QIM_ARMOR2, QIM_ARMOR3,             /* armor.mdl's three skins */
	QIM_W_S_KEY, QIM_W_G_KEY, QIM_M_S_KEY, QIM_M_G_KEY, QIM_B_S_KEY, QIM_B_G_KEY,
	QIM_INVULNER, QIM_SUIT, QIM_INVISIBL, QIM_QUADDAMA,
	QIM_END1, QIM_END2, QIM_END3, QIM_END4,
	QIM_FLAME, QIM_FLAME2, QIM_LAVABALL, QIM_TELEPORT,
	QIM_B_BH10, QIM_B_BH25, QIM_B_BH100, QIM_B_SHELL0, QIM_B_SHELL1, QIM_B_NAIL0, QIM_B_NAIL1,
	QIM_B_ROCK0, QIM_B_ROCK1, QIM_B_BATT0, QIM_B_BATT1, QIM_B_EXPLOB, QIM_B_EXBOX2,
	QIM_COUNT
};
#define QW_MODEL_TEX 6

typedef struct {
	uint16_t qim;                /* QIM_*                                        */
	uint16_t num_tex;
	uint16_t tex[QW_MODEL_TEX];  /* the map texture table's entries              */
	qg_model m;
} qw_model;                      /* 48 bytes */

/* "QMM1": a map's own models at one model level (<MAP>_M<n>.MDL) */
#define QMAPMDL_MAGIC "QMM1"
typedef struct {
	char     magic[4];
	uint32_t num_models, ofs_models;   /* qw_model[num_models]                  */
	uint32_t ofs_geo, len_geo;         /* their geometry block                  */
} qmm_header;

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
	QGP_FACE1, QGP_FACE2, QGP_FACE3, QGP_FACE4, QGP_FACE5,   /* healthy .. near death */
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

/* The 2D weapons: per weapon (QGM_V_AXE .. QGM_V_LIGHT), its frames (the
   model's, in its order) and its CLUT's packet; per frame, where it goes
   on the screen at the gun's rest pose and its texels' chain (half
   resolution, a transparent margin, rows of w bytes, w even): GP0(02h)
   filling its rows of the slot with 0 (the transparent index; the slot
   starts on 32 texels, the GPU's fill unit) then GP0(A0h) copying its
   opaque spans, or GP0(A0h) copying the whole frame, whichever is
   smaller; GP0(01h) last. Frames of the same texels share a chain (the
   same ofs). A packet's tag links to the next one's offset in the pixel
   block, the chain's last to 0xffffff. */
#define QG_WEAPONS     8
#define QG_WS_L0       86        /* the light the frames are shaded at: the
                                    engine's colour is light x 128 / QG_WS_L0 */
typedef struct {
	uint16_t first, count;       /* its frames in the frame table               */
	uint16_t anchor;             /* the gun's depth at rest (frame 0's vertices'
	                                mean), 1/16 units: the bob's scale           */
	uint16_t pad;
	uint32_t ofs_clut;           /* its CLUT's packet in the pixel block         */
} qg_wsprite;                    /* 12 bytes */

typedef struct {
	int16_t  x, y;               /* its top-left, screen pixels from the
	                                projection's centre (160, 96: the status
	                                bar's viewport, the gun 2 units up)         */
	uint8_t  w, h;               /* texels, the last column and row transparent
	                                (drawn without them, twice the size: the
	                                GPU never reads past the frame's texels);
	                                0: the frame is drawn 3D (larger than 80)   */
	uint16_t pad;
	uint32_t ofs;                /* its texels' first packet in the pixel block */
	uint32_t ofs_last;           /* and its last (GP0(01h))                     */
} qg_wframe;                     /* 16 bytes */

/* The aux pictures: the intermission's and the menu's (gfx/ lumps) and the
   console font (gfx.wad conchars), 4bpp with 16 colours each, in the two
   4-bit pages of the last VRAM column (x 960: QG_AUX_X, y 0 and 256); their
   CLUTs in the CLUT area under the framebuffers (formats/qtex.h), 20 a row
   from (QG_AUX_CLUT_X, QG_AUX_CLUT_Y): picture i at x 16 (i % 20), y
   QG_AUX_CLUT_Y + i / 20. (Row 504 from x 256 overlapped the map's sixth
   page and the game's pages, x 320 .. 959: a map loaded over them.) Index 0
   of a picture's CLUT is its transparent colour when it has one. */
#define QG_AUX_X             960
#define QG_AUX_CLUT_X        0
#define QG_AUX_CLUT_Y        505
#define QG_AUX_CLUTS_PER_ROW 20
enum {
	QGA_COMPLETE, QGA_INTER, QGA_FINALE_L, QGA_FINALE_R,   /* the intermission  */
	QGA_CONCHARS, QGA_CONCHARS2, /* 128 x 64 each: 16 x 8 glyphs of 8 x 8, chars
	                                0..127 then 128..255 (the bronze ones)       */
	QGA_QPLAQUE, QGA_TTL_MAIN, QGA_MAINMENU,
	QGA_MENUDOT1,                /* 6 frames of the spinning cursor              */
	QGA_TTL_SGL = QGA_MENUDOT1 + 6, QGA_SP_MENU, QGA_P_OPTION,
	QGA_BOX_TL, QGA_BOX_ML, QGA_BOX_BL, QGA_BOX_TM, QGA_BOX_MM, QGA_BOX_MM2, QGA_BOX_BM,
	QGA_BOX_TR, QGA_BOX_MR, QGA_BOX_BR,
	QGA_DISC,                    /* the armour slot under invulnerability (Sbar)   */
	QGA_COUNT                    /* (vidmodes, loading and pause: no room left) */
};

typedef struct {
	uint16_t tpage;              /* GPU tpage word, 4bpp                         */
	uint16_t clut;               /* its CLUT                                     */
	uint8_t  u, v;
	uint16_t w, h;
	uint16_t pad;
} qg_aux;                        /* 12 bytes */

typedef struct {
	char     magic[4];           /* "QGM8"                                       */
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
	uint32_t num_aux, ofs_aux;   /* qg_aux[QGA_COUNT] (their pixels and CLUTs are
	                                among the rects)                             */
	uint32_t num_finales, ofs_finales, len_finales;   /* qg_finale[num_finales]
	                                then their texts (the block's len bytes)     */
	uint32_t ofs_wsprites;       /* qg_wsprite[QG_WEAPONS]                       */
	uint32_t num_wframes, ofs_wframes;   /* qg_wframe[num_wframes]               */
	uint32_t ofs_wpix, len_wpix; /* the 2D weapons' packets: texels and CLUTs   */
	qg_pic   wslot;              /* the VRAM slot a frame is copied to (u even) */
	uint32_t level;              /* the model level (formats/qlevels.h)          */
	uint32_t len_geo_max;        /* the largest geometry block of all the levels'
	                                files: the engine's room for any of them     */
	uint32_t num_free, ofs_free; /* qg_free[num_free]: what the images leave of
	                                the game's pages (the streamed skins' room,
	                                engine/stream.c)                           */
} qg_header;

/* A free rectangle of the game's pages, as the packer left them (maximal
   rectangles: they overlap one another; a skin placed in one must not
   overlap another skin placed in an overlapping one). */
typedef struct {
	uint8_t  page;               /* the texture page (formats/qtex.h: 6 .. 9)    */
	uint8_t  pad;
	uint8_t  u, v;               /* its corner, page-relative texels             */
	uint16_t w, h;               /* texels                                       */
} qg_free;                       /* 8 bytes */

/* The episodes' end texts (client.qc ExitIntermission's SVC_FINALE strings,
   read from the pak's progs.dat): the map whose intermission shows it, which
   edition (e1m7 has one for the shareware, one for the registered game), or
   the four runes' text after the end of an episode. */
#define QGFIN_SHAREWARE  1
#define QGFIN_REGISTERED 2
#define QGFIN_RUNES      4
typedef struct {
	char     map[12];            /* "E1M7" ("" for the runes' text)            */
	uint16_t flags;              /* QGFIN_*, 0: either edition                 */
	uint16_t len;
	uint32_t ofs;                /* from the block's start, NUL-ended          */
} qg_finale;                     /* 20 bytes */

#endif
