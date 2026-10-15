/*
 * q1psx — cooked world "QWV3" (architecture.md §6), one per map and fidelity
 * level. Written by `qcook`, loaded by engine/world.c, walked by the asm
 * renderer (engine/r_*.s).
 *
 * Units
 *   "vertex units" = Quake world units x 4 (QW_VSCALE), stored as int16.
 *   Plane normals are x4096; plane distances are x4096 in vertex units, so
 *   n . p(vertex units) - dist is the plane distance x4096 x4.
 *
 * Model
 *   - Faces belong to the BSP node whose plane they lie on (Quake's model).
 *     Each node has two face blocks: faces facing its front side, then faces
 *     facing its back side. The side the camera is on picks the block — no
 *     per-face backface test.
 *   - Every Quake face is cut so each piece's UVs fit a byte. A texture axis
 *     whose size is a power of two repeats in hardware: the GPU texture
 *     window (GP0 E2h) wraps it, so that axis is only cut every 256 texels.
 *     Other axes are cut at every repetition (UVs inside the VRAM tile).
 *     Each record names its texture in the per-map table (qw_tex), which
 *     holds the CLUT, tpage and texture-window words. Pieces are grouped
 *     into "face records" of <= 63 vertices.
 *   - A face record is drawn when its face_id was stamped this frame by a
 *     visible leaf's marksurfaces (Quake's visframe scheme).
 *
 * Pointers
 *   On disk, node/leaf link fields hold indices / byte offsets; the loader
 *   (engine/world.c) rewrites them in place into absolute pointers. The asm
 *   renderer only ever sees pointers. Leaf pointers are tagged with bit 0.
 *
 * Every section is 4-byte aligned; offsets are from the file start.
 */

#ifndef Q1PSX_QWORLD_H
#define Q1PSX_QWORLD_H

#include <stdint.h>

#define QWORLD_MAGIC "QWV3"

#define QW_VSCALE       4      /* vertex units per world unit            */
#define QW_MAX_VERTS   63      /* per face record (multiple of 3)        */
#define QW_MAX_DEPTH  256      /* BSP depth the renderer's stack holds:
                                  64 words of scratchpad + 3 RAM blocks   */
#define QW_NONE        0xffffffffu

/* face record flags */
#define QWF_SKY     (1 << 0)
#define QWF_LIQUID  (1 << 1)

typedef struct {
	int32_t pos[3];        /* world units                    */
	int16_t yaw, pitch;    /* whole degrees                  */
} qw_view;

typedef struct {
	char     magic[4];             /* "QWV3"                                  */
	uint32_t num_nodes;
	uint32_t num_leaves;           /* including leaf 0 (solid)                */
	uint32_t num_marks;
	uint32_t num_faces;            /* Quake world-model faces (visframe ids)  */
	uint32_t num_records;          /* face records in the stream              */
	uint32_t num_pieces;           /* polygons (quads + tris), statistics     */
	uint32_t num_views;            /* bench viewpoints                        */
	uint32_t ofs_nodes, ofs_leaves, ofs_marks, ofs_vis, ofs_faces, ofs_views;
	uint32_t len_vis, len_faces;
	uint32_t ofs_clip, len_clip;   /* clip hulls (R4), 0 if absent            */
	int32_t  spawn[3];             /* info_player_start, world units          */
	int32_t  spawn_yaw;            /* whole degrees                           */
	uint32_t has_spawn;
	uint32_t root;                 /* node index of the world model root      */
	uint32_t max_depth;            /* deepest node chain (<= QW_MAX_DEPTH)    */
	uint32_t num_visleafs;          /* PVS row = (num_visleafs + 7) / 8 bytes */
	uint32_t ofs_tex, num_tex;     /* qw_tex[num_tex], indexed by record.tex  */
	uint32_t reserved[1];
} qw_header;

/* One per Quake miptex (unused ones are zero). */
typedef struct {
	uint16_t clut;         /* GPU CLUT word                                   */
	uint16_t tpage;        /* GPU tpage word (8bpp)                           */
	uint32_t window;       /* GP0 E2h texture-window command; 0xE2000000 = no
	                          wrap (the axes that are not powers of two)      */
} qw_tex;                  /* 8 bytes */

typedef struct {
	int16_t  c[3], pad0;   /* +0  bbox centre, vertex units (lwc2-ready)       */
	int16_t  h[3], pad1;   /* +8  bbox half extents, vertex units              */
	int16_t  n[3];         /* +16 plane normal x4096                           */
	uint8_t  type;         /* +22 0/1/2 = axial X/Y/Z, 3 = general             */
	uint8_t  pad2;         /* +23                                              */
	int32_t  dist;         /* +24 plane distance x4096, vertex units           */
	uint32_t child[2];     /* +28 disk: index, bit31 = leaf; RAM: ptr, bit0 = leaf */
	uint16_t visframe;     /* +36 runtime                                      */
	uint16_t pad3;         /* +38                                              */
	uint32_t parent;       /* +40 disk: node index or QW_NONE; RAM: ptr / 0    */
	uint32_t faces[3];     /* +44 front start, back start, back end:
	                              disk: offsets in the face stream; RAM: ptrs   */
} qw_node;                 /* 56 bytes */

typedef struct {
	int16_t  c[3], pad0;   /* +0  bbox centre, vertex units                    */
	int16_t  h[3], pad1;   /* +8  bbox half extents, vertex units              */
	uint16_t visframe;     /* +16 runtime                                      */
	uint16_t num_marks;    /* +18                                              */
	uint32_t marks;        /* +20 disk: index into marks[]; RAM: ptr           */
	uint32_t parent;       /* +24 disk: node index; RAM: ptr                   */
	uint32_t vis;          /* +28 disk: offset into vis or QW_NONE; RAM: ptr/0 */
	int32_t  contents;     /* +32 Quake CONTENTS_*                             */
} qw_leaf;                 /* 36 bytes */

/* Face record: header, then verts, then quads, then tris. All the pieces of
   a record come from one Quake face, hence one texture: the GPU tpage and
   CLUT words are stored once here. */
typedef struct {
	uint16_t face_id;      /* +0  Quake face index (visframe slot)            */
	uint8_t  num_verts;    /* +2  <= QW_MAX_VERTS (the renderer transforms in
	                                threes and may read up to 2 verts past the
	                                list: harmless, never referenced)          */
	uint8_t  num_quads;    /* +3                                              */
	uint8_t  num_tris;     /* +4                                              */
	uint8_t  flags;        /* +5  QWF_*                                       */
	uint16_t size;         /* +6  whole record, bytes                         */
	uint16_t tex;          /* +8  index into the qw_tex table                 */
	uint16_t sr;           /* +10 bounding sphere radius, vertex units         */
	int16_t  sc[3];        /* +12 bounding sphere centre, vertex units:
	                              +12 / +16 load straight into VXY0 / VZ0       */
	uint16_t pad;          /* +18                                             */
} qw_record;               /* 20 bytes */

typedef struct {
	int16_t x, y, z, pad;  /* vertex units                                   */
} qw_vert;

/* Vertex indices are pre-multiplied by 4 (byte offsets into the renderer's
   per-record vertex cache). Quad corners are in PS1 order: the GPU draws
   triangles (0,1,2) and (1,2,3). uv[k] = u | v << 8 in texels: on a wrapped
   axis a virtual 0..255 coordinate the texture window folds onto the
   texture, otherwise page-relative. The renderer ORs the CLUT into uv[0]
   and the tpage into uv[1]. */
typedef struct {
	uint8_t  idx[4];
	uint16_t uv[4];
} qw_quad;                 /* 12 bytes */

typedef struct {
	uint8_t  idx[3], pad;
	uint16_t uv[3], pad2;
} qw_tri;                  /* 12 bytes */

/* Clip hulls (Quake hulls 1 and 2, used by the player / monster traces):
   qw_clip_header at ofs_clip, then the planes, then the clipnodes. Laid out
   for the asm trace (engine/clip.s): distances are pre-scaled to the
   physics' position unit (1/QW_CLIP_SUB world unit) and clipnodes hold their
   plane's offset in words, so no per-node multiply is left. */
#define QW_CLIP_SUB 8              /* physics positions: 1/8 world unit  */
#define QW_MAX_SPLIT 128           /* clip-hull depth the trace handles
                                      (Quake maps reach 65)              */
typedef struct {
	uint32_t num_planes;
	uint32_t num_clipnodes;
	int32_t  hull1_root;       /* player-sized hull (32x32x56)        */
	int32_t  hull2_root;       /* large hull (64x64x88)               */
} qw_clip_header;

typedef struct {
	int16_t  n[3];             /* normal x4096                        */
	uint8_t  type;             /* 0/1/2 axial, 3 general              */
	uint8_t  pad;
	int32_t  dist;             /* x4096, in 1/QW_CLIP_SUB units       */
} qw_plane;                    /* 12 bytes */

typedef struct {
	uint16_t plane3;           /* plane index x 3 = its word offset   */
	int16_t  child[2];         /* >= 0 clipnode, < 0 Quake CONTENTS_* */
	uint16_t pad;
} qw_clipnode;                 /* 8 bytes */

_Static_assert(sizeof(qw_plane) == 12 && __builtin_offsetof(qw_plane, type) == 6 &&
               __builtin_offsetof(qw_plane, dist) == 8, "engine/p_defs.inc PL_*");
_Static_assert(sizeof(qw_clipnode) == 8 && __builtin_offsetof(qw_clipnode, child) == 2,
               "engine/p_defs.inc CN_*");

#define QW_CONTENTS_EMPTY  (-1)
#define QW_CONTENTS_SOLID  (-2)

/* The face stream is followed by at least this many bytes of padding so the
   renderer's 3-at-a-time vertex loads never run off the end. */
#define QW_STREAM_TAIL 16

#endif
