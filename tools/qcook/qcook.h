/*
 * q1psx — asset cooker (host tool), shared declarations.
 *
 *   qcook.c   command line, pak access, BSP lump parsing, the levels
 *   tex.c     palette -> CLUT, texture tiles, VRAM page packing, QTX6 output
 *   world.c   face splitting, face records, nodes/leaves/PVS, QWVF output
 *   game.c    models, sprites, HUD pictures -> QGM6 (formats/qgame.h)
 *   items.c   the map's own models -> QMM1 (formats/qgame.h)
 *   decim.c   an alias model's triangles reduced (the model levels)
 *   ents.c    the entity lump: parsed for the world's entity table, the
 *             sound bank and the player start
 *   sound.c   the map's sound bank -> QSND (formats/qsound.h)
 *   bmf.c     the brush models' room for their cut pieces (engine/bmfrag.c)
 *   zones.c   a world split into its skeleton and zone chunks (QWZF, QZN1)
 *
 * See docs/architecture.md §6-§7, formats/qworld.h / qtex.h / qlevels.h.
 */

#ifndef QCOOK_H
#define QCOOK_H

#include <stddef.h>
#include <stdint.h>

#include "../../formats/qlevels.h"
#include "../../formats/qworld.h"    /* qw_ent (ents_pack) */

/* ------------------------------------------------------------------ util -- */

void  die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);

/* a growing byte buffer (the cooked files are assembled in one) */
typedef struct { uint8_t *p; size_t len, cap; } buf_t;
void buf_put(buf_t *b, const void *data, size_t n);
void buf_align4(buf_t *b);

/* ------------------------------------------------------------------- pak -- */

uint8_t       *read_file(const char *path, long *len);
const uint8_t *pak_find(const uint8_t *pak, long pak_len, const char *name, uint32_t *len);

/* ----------------------------------------------------------------- game -- */

/* `qcook game`: models, sprites and HUD pictures -> one QGM6 file per model
   level (game.c) */
int cmd_game(int argc, char **argv);

/* --------------------------------------------------------------- levels -- */

/* formats/qlevels.h: the names, and an axis's level by name (dies if the
   axis has no such level) */
extern const char *const level_name[QL_LEVELS];
int level_find(const char *name, int mask, const char *axis);

/* A texture level: the world's textures at native >> tex_shift, no side of
   one repetition over tex_max texels. The finest (QL_ULTRA) is the layout
   the world and the map's models are cooked for (formats/qtex.h
   qtex_uvmap). */
typedef struct {
	int tex_shift;
	int tex_max;
} tex_level_t;

extern const tex_level_t tex_levels[QL_LEVELS];

/* ------------------------------------------------------------------ bsp -- */

typedef struct { float n[3]; float dist; int type; } bsp_plane;
typedef struct { int planenum; int child[2]; int16_t mins[3], maxs[3];
                 int firstface, numfaces; } bsp_node;
typedef struct { int contents, visofs; int16_t mins[3], maxs[3];
                 int firstmark, nummarks;
                 uint8_t ambient[4]; } bsp_leaf;   /* water, sky, slime, lava levels */
typedef struct { float vecs[2][4]; int miptex, flags; } bsp_texinfo;
typedef struct { int planenum, side, firstedge, numedges, texinfo;
                 uint8_t styles[4]; int lightofs; } bsp_face;   /* lightofs -1: none */
typedef struct { int planenum; int child[2]; } bsp_clipnode;
typedef struct { float mins[3], maxs[3]; int headnode[4], visleafs, firstface, numfaces; } bsp_model;

typedef struct {
	char     name[16];
	int      w, h;            /* 0 if the miptex slot is missing          */
	const uint8_t *mip[4];    /* mip k: (w>>k)*(h>>k) palette indices    */
} bsp_miptex;

typedef struct {
	const uint8_t *raw;
	uint32_t       len;

	bsp_plane    *planes;    int num_planes;
	bsp_node     *nodes;     int num_nodes;
	bsp_leaf     *leaves;    int num_leaves;
	bsp_texinfo  *texinfo;   int num_texinfo;
	bsp_face     *faces;     int num_faces;
	bsp_clipnode *clipnodes; int num_clipnodes;
	bsp_model    *models;    int num_models;
	float        *verts;     int num_verts;      /* xyz triplets */
	int          *surfedges; int num_surfedges;
	uint16_t     *edges;     int num_edges;      /* v0,v1 pairs  */
	uint16_t     *marks;     int num_marks;
	bsp_miptex   *miptex;    int num_miptex;
	const uint8_t *vis;      uint32_t vis_len;
	const uint8_t *lighting; uint32_t light_len;   /* lightmaps, 1 byte per luxel */
	const char   *entities;  uint32_t ent_len;
} bsp_t;

void bsp_parse(bsp_t *b, const uint8_t *raw, uint32_t len);

/* ------------------------------------------------------------- entities -- */

typedef struct { char *key, *val; } ent_kv;
typedef struct { int first_kv, num_kv; const char *classname; } ent_rec;
typedef struct {
	ent_rec *ents; int num, cap;
	ent_kv  *kv;   int num_kv, cap_kv;
	int      sp_never;        /* the entities left out: single player never
	                             spawns them (coop and deathmatch's only) */
} ent_list;

/* the map's entities, but those single player never spawns: the coop and
   deathmatch starts, and what is on no skill (all three skill flags) */
void        ents_parse(const bsp_t *b, ent_list *l);
void        ents_free(ent_list *l);
const char *ent_value(const ent_list *l, const ent_rec *e, const char *key);   /* NULL if absent */
double      ent_float(const ent_list *l, const ent_rec *e, const char *key, double def);
int         ent_vector(const ent_list *l, const ent_rec *e, const char *key, double v[3]);
int         ent_class(const char *classname);                             /* QE_* */

/* the world's entity table (qw_ent[]) and string pool from the parsed lump;
   worldspawn's worldtype and message (title); how many classnames the cook
   does not know */
void ents_cook(const ent_list *l, int num_models, buf_t *table, buf_t *strings,
               uint32_t *worldtype, uint32_t *title, int *unknown);

/* that table compact, as the world file holds it (formats/qworld.h
   qw_ent_decode), appended to out */
void ents_pack(const qw_ent *ents, uint32_t n, buf_t *out);

/* drawn[m] = 1 for the world and every brush model some entity other
   than a trigger uses (a trigger's brushes are never drawn: no faces
   cooked, no textures packed for them) */
void ents_drawn_models(const bsp_t *b, uint8_t *drawn);

/* used[m] = 1 for the world and every brush model an entity of the list
   uses (the triggers' too); in the id1 maps, a model no entity uses
   belongs to an entity single player never spawns */
void ents_used_models(const ent_list *l, int num_models, uint8_t *used);

/* ------------------------------------------------------- a map's models -- */

/* game.c: an alias model cooked for a map: its skin reduced (1 / skin_div)
   with a border of `qc_mdl_border` texels, then its records, their UVs
   from the skin's own corner inside the border (the world file's kind:
   formats/qtex.h qtex_uvmap), its triangles reduced at a model level
   below high (decim.c) */
typedef struct qc_mdl qc_mdl;
extern const int qc_mdl_border;
void    qc_set_palette(const uint8_t *pal);
qc_mdl *qc_mdl_load(const char *file, const uint8_t *data, uint32_t len, int skin, int skin_div, int flat);
void    qc_mdl_skin(const qc_mdl *q, int *w, int *h, const uint8_t **px);
int     qc_mdl_flags(const qc_mdl *q);
void    qc_mdl_cook(qc_mdl *q, int model_level, buf_t *geo, void *qg_model_out);
/* the compact format of the streamed models (formats/qstream.h): each
   model level's whole asset (qst_model, frames, records, skin: out[level],
   STREAM_<level>.DAT's) and their shared asset (qst_shared) */
void    qc_mdl_cook_levels(qc_mdl *q, int body, buf_t out[3], buf_t *shared);

/* ---------------------------------------------------------------- stream -- */

/* `qcook stream`: the streamed assets -> one QST1 file (stream.c) */
int cmd_stream(int argc, char **argv);

/* `qcook menu`: the release menu's bank and the console's background
   (menu.c, formats/qmenu.h) */
int cmd_menu(int argc, char **argv);

/* `qcook spumusic`: a CD track as the SPU music (spumusic.c,
   formats/qspu.h) */
int cmd_spumusic(int argc, char **argv);
/* what a map's entities take from the stream, by name (its monsters',
   stream.c; its items' models, items.c; its entities' and ambients'
   sounds, sound.c): the XA carousels' lists until docs/porting-notes.md
   §52, kept for the streaming's redesign */
int stream_monster_needs(const ent_list *l, const char **list, int max);
int items_streamed(const ent_list *l, int worldtype, const char **list, int max);
int sound_map_streamed(const ent_list *ents, int worldtype, const char **list, int max);

/* ---------------------------------------------------------------- sound -- */

/* `qcook map --sounds`: the map's sound bank (formats/qsound.h, sound.c).
   Sounds come from the map's pak, then from pak0. */
void sound_build(const bsp_t *b, const uint8_t *pak, long pak_len, const uint8_t *pak0,
                 long pak0_len, const char *out_path);
/* the streamed sounds (the ones with a place in a map: its entities' and
   ambient entities'; the monsters' are stream.c's) into list, their count;
   a bank sound (always in a map's bank); one streamed sound's asset
   (qst_sound, then its ADPCM) */
int  sound_streamed(const char **list, int max);
/* the release menu's bank: its clicks, CD track 2 (`qcook menu`, menu.c) */
void sound_build_menu(const uint8_t *pak0, long pak0_len, const char *out_path);
int  sound_is_banked(const char *name);
int  sound_in_paks(const char *name, const uint8_t *pak, long pak_len, const uint8_t *pak0, long pak0_len);
void sound_encode_stream(const char *name, const uint8_t *pak, long pak_len, const uint8_t *pak0, long pak0_len,
                         buf_t *out, uint32_t *ticks);


/* ------------------------------------------------------------- textures -- */

/* A texture slot: its size at the set's level and, once packed, where it
   lives in VRAM plus the words a polygon needs. */
typedef struct {
	int      present;        /* drawn (has pixels and is not sky); packed in
	                            a set that was placed                     */
	int      index;          /* its slot = qw_tex table index             */
	int      rep_w, rep_h;   /* texels of one texture repetition, scaled  */
	int      win_x, win_y;   /* axis wraps in hardware (power of two)     */
	double   res_u, res_v;   /* the set's texels per source texel (a
	                            miptex's per Quake texel, a skin's per skin
	                            texel): the UVs' scale                    */
	int      inner;          /* a skin's border: its texels start this far
	                            inside the tile                           */
	int      u0, v0;         /* top-left texel inside its texture page    */
	int      page;           /* texture page index (0 .. QTEX_PAGES - 1)  */
	int      tile_w, tile_h; /* texels of the tile, its gutter not counted */
	int      gut_x, gut_y;   /* 1 on an axis that does not wrap: the texel
	                            column / row past the tile's end repeats its
	                            first (the gutter, docs/porting-notes.md §69) */
	uint16_t tpage;          /* GPU tpage word (8bpp)                     */
	uint16_t clut;           /* GPU CLUT word                             */
	uint32_t window;         /* GP0 E2h texture-window command            */
	uint8_t  flags;          /* QWF_* derived from the name               */
} tex_slot;

/* An animated texture's chain (Mod_LoadTextures: "+0name" .. "+9name",
   the alternates "+aname" .. "+jname"): its frames' slots, and the display
   slot every face of the chain uses (the engine copies the frame of the
   moment into it) */
#define TEX_ANIM_FRAMES 10
typedef struct {
	int display;
	int frames[TEX_ANIM_FRAMES], nframes;
	int alt[TEX_ANIM_FRAMES], nalt;
	int src;                 /* the miptex the display slot is cut from    */
} tex_anim;

typedef struct {
	tex_slot *slots;         /* [num]: the miptex, the extras, then the
	                            animations' display slots                 */
	int       num;           /* num_miptex + the extras + the animations  */
	int       pages_used;
	uint32_t  bytes;         /* texture pixels uploaded                   */
	int      *remap;         /* [num_miptex]: the slot a face of the miptex
	                            draws with (an animated one's display)    */
	tex_anim *anims;
	int       num_anims;
} tex_set;

/* A texture the map's own models bring (tools/qcook/items.c): a BSP item's
   miptex (it tiles: scaled to the level, its power-of-two axes wrap), or
   a model's skin, placed as it is (bordered: its texels `border` inside,
   at 1 / div of the model's skin). */
typedef struct {
	bsp_miptex mt;
	int        skin;
	int        div, border;   /* a skin's                                     */
	int        shift;   /* a miptex's: levels below the set's (the pages full) */
} tex_extra;

/* The map's texture set at a level: every slot sized (the miptex, the
   extras after them, the animations' display slots last). With out_path
   NULL that is all (the layout the world and the map's models are cut
   and mapped for: the finest level's, `canon` NULL). Otherwise the tiles
   are packed into the map's pages and written as a QTX6 set to out_path
   and, when out4_path is not NULL, the same tiles in 4bpp (QTX7, 16
   colours per texture) to out4_path; their tables map the layout `canon`
   onto this set (formats/qtex.h qtex_uvmap). 1 when done; 0 when the
   tiles overflow the map's pages and may_fail (nothing written), else it
   dies. */
int tex_build(tex_set *ts, const tex_set *canon, const bsp_t *b, const tex_extra *extra,
              int nextra, const uint8_t palette[768], int level, const char *out_path,
              const char *out4_path, int anims, int may_fail);

/* ------------------------------------------------------ the map's models -- */

/* items.c: the models the map's entities use (their extras for tex_build),
   cooked for the layout `canon` at a model level, written as the map's
   models file (formats/qgame.h qmm_header) */
/* squeeze 0 as they are; when the map's pages overflow: 1 the boxes'
   textures a level down, 2 the skins at half (the flames' kept), 3 the
   flames' too, 4 the boxes' a level further */
#define ITEMS_SQUEEZE_MAX 4
void items_prepare(const bsp_t *b, const uint8_t *pak, long pak_len, const uint8_t *pak0,
                   long pak0_len, const uint8_t *palette, int squeeze, tex_extra **extra, int *nextra);
void items_cook(const tex_set *canon, int first_extra, int model_level);
void items_write(const char *out_path);

/* ------------------------------------------------------------- decim.c -- */

/* An alias model's triangles reduced for a model level below high
   (decim.c): edges collapsed onto one of their ends (no vertex moves:
   every frame stays Quake's), cheapest first by how far the removed
   vertex's surface and texture move over all the frames, the seam and the
   open edges kept, until only `keep` of the triangles are left or the
   next would cost more than `max_err` (model units). `tri` holds n MDL
   triangles (facesfront, v0, v1, v2), rewritten in place; returns the
   triangles left (the first ones of `tri`). */
typedef struct {
	int            nverts, nframes;
	const double  *pos;        /* [nframes][nverts][3], model units          */
	const double  *uv;         /* [nverts][2], skin texels (the front face's) */
	double         uv_scale;   /* skin texels -> units in the error          */
	const uint8_t *seam;       /* [nverts]: MDL onseam (a back face adds half
	                              the skin's width)                         */
	double         skin_w;
} dmesh;
int decimate(const dmesh *m, int (*tri)[4], int n, double keep, double max_err);

/* ---------------------------------------------------------------- world -- */

extern double light_gain;   /* vertex light scale (qcook --light-gain), 1 = Quake */
void world_build(const bsp_t *b, const tex_set *canon, int geo_level, const char *out_path);

/* `qcook check`: reload a QWV2 file and verify every invariant. */
int  world_check(const char *path);

/* ------------------------------------------------------------------ bmf -- */

/* bmf.c: the room each drawn brush model's pieces take when the world's BSP
   cuts it (engine/bmfrag.c), wherever its entity can stand, written into
   the cooked world's qw_bmodel table (f: the whole file, len bytes) */
void bmf_rooms(uint8_t *f, size_t len);
/* `qcook bmf <file.wld> [-v]`: measured again, against the file's */
int  cmd_bmf(int argc, char **argv);

/* `qcook zones`: a cooked world split into its skeleton and its zone chunks
   from the level-chunks analysis' tables (zones.c, formats/qworld.h) */
int  cmd_zones(int argc, char **argv);

#endif
