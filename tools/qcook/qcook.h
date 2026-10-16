/*
 * q1psx — asset cooker (host tool), shared declarations.
 *
 *   qcook.c   command line, pak access, BSP lump parsing
 *   tex.c     palette -> CLUT, texture tiles, VRAM page packing, QTX3 output
 *   world.c   face splitting, face records, nodes/leaves/PVS, QWV2 output
 *
 * See docs/architecture.md §6-§7 and formats/qworld.h / formats/qtex.h.
 */

#ifndef QCOOK_H
#define QCOOK_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ util -- */

void  die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);

/* ------------------------------------------------------------- fidelity -- */

typedef struct {
	const char *name;
	int  tex_shift;     /* texture resolution = native >> tex_shift          */
	int  tex_max;       /* max texels per side of one repetition, scaled     */
	int  tile_min;      /* axes that are not powers of two (no hardware
	                       wrap): repeat the texture in its VRAM tile until
	                       the tile is >= this many texels, so one polygon
	                       covers several repetitions. 0 = never.            */
} fidelity_t;

const fidelity_t *fidelity_find(const char *name);

/* ------------------------------------------------------------------ bsp -- */

typedef struct { float n[3]; float dist; int type; } bsp_plane;
typedef struct { int planenum; int child[2]; int16_t mins[3], maxs[3];
                 int firstface, numfaces; } bsp_node;
typedef struct { int contents, visofs; int16_t mins[3], maxs[3];
                 int firstmark, nummarks; } bsp_leaf;
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

/* ------------------------------------------------------------- textures -- */

/* Where a miptex lives in VRAM once packed, plus the words a polygon needs. */
typedef struct {
	int      present;        /* packed (has pixels and is not sky)        */
	int      index;          /* its miptex index = qw_tex table index     */
	int      rep_w, rep_h;   /* texels of one texture repetition, scaled  */
	int      win_x, win_y;   /* axis wraps in hardware (power of two)     */
	int      reps_x, reps_y; /* repetitions laid side by side in the tile
	                            (1 on a wrapped axis)                     */
	int      u0, v0;         /* top-left texel inside its texture page    */
	int      page;           /* texture page index (0 .. QTEX_PAGES - 1)  */
	int      tile_w, tile_h; /* texels of the whole tile                  */
	uint16_t tpage;          /* GPU tpage word (8bpp)                     */
	uint16_t clut;           /* GPU CLUT word                             */
	uint32_t window;         /* GP0 E2h texture-window command            */
	uint8_t  flags;          /* QWF_* derived from the name               */
} tex_slot;

typedef struct {
	tex_slot *slots;         /* [num_miptex]                              */
	int       pages_used;
	uint32_t  bytes;         /* texture pixels uploaded                   */
} tex_set;

/* Writes the QTX3 set to out_path and, when out4_path is not NULL, the
   same tiles in 4bpp (QTX4, 16 colours per texture) to out4_path. */
void tex_build(tex_set *ts, const bsp_t *b, const uint8_t palette[768],
               const fidelity_t *fid, const char *out_path, const char *out4_path);

/* ---------------------------------------------------------------- world -- */

extern double light_gain;   /* vertex light scale (qcook --light-gain), 1 = Quake */
void world_build(const bsp_t *b, const tex_set *ts, const fidelity_t *fid,
                 const char *out_path);

/* `qcook check`: reload a QWV2 file and verify every invariant. */
int  world_check(const char *path);

#endif
