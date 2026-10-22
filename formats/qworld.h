/*
 * q1psx — cooked world "QWVF" (architecture.md §6), one per map and geometry
 * level (formats/qlevels.h). Written by `qcook`, loaded by engine/world.c,
 * walked by the asm renderer (engine/r_*.s).
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
 *     The cuts and the UVs are those of the finest texture level, so one
 *     world file serves every texture level: the texture set (formats/
 *     qtex.h) brings the table each record names its texture in (qw_tex:
 *     the CLUT, tpage and texture-window words) and the map from these UVs
 *     to its own tiles (qtex_uvmap), which the loader applies once. Pieces
 *     are grouped into "face records" of <= 63 vertices.
 *   - The ultra level also cuts the faces on a grid of QL_LIGHT_GRID
 *     texels (their vertex light follows the lightmap more closely); the
 *     low level leaves the T-junctions unrepaired (fewer polygons, a crack
 *     may show where a face's edge meets another's vertex).
 *   - A face record is drawn when its face_id was stamped this frame by a
 *     visible leaf's marksurfaces (Quake's visframe scheme).
 *   - Brush models (Quake's submodels *1.., doors, plats, buttons, walls):
 *     their faces are records on their own BSP nodes, in the same node
 *     array and face stream, with face_id 0 (no visibility slot: the
 *     renderer walks a model's subtree from its root when the game draws
 *     it, with the model's own matrix, so the node sides sort its faces).
 *     qw_bmodel gives the root, bounds and clip hull roots; the entities
 *     (qw_ent, cooked from the entity lump) name their model by index.
 *   - A drawn brush model is cut by the world's BSP (engine/bmfrag.c); the
 *     cook cuts it the same way wherever its entity can stand (a mover at
 *     every 1/8 unit of its travel) and writes the most its pieces take in
 *     its qw_bmodel: their room in RAM, with no cut at the load
 *     (tools/qcook/bmf.c, docs/porting-notes.md §46).
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

#include "qtex.h"          /* qw_tex, qw_anim: the texture set's tables */

#define QWORLD_MAGIC "QWVF"   /* (QWVE until §59: an entity was a whole qw_ent;
                                 QWVD until §46: the clip roots were 32-bit) */

#define QW_VSCALE       4      /* vertex units per world unit            */
#define QW_CLIP_SUB     8      /* physics positions (the clip hulls', the
                                  entities' origins): 1/8 world unit     */
#define QW_MAX_VERTS   42      /* per face record (multiple of 3): the
                                  renderer's scratchpad holds SXY, SZ and
                                  light for 42 vertices                  */
#define QW_MAX_REC_PKT 2048     /* GPU packet bytes one face record may emit
                                  on the fast path: 52 per quad, 40 per tri,
                                  8 for a texture window. The renderer
                                  tests the arena once per record against
                                  an end this far short of the real one  */
#define QW_MAX_DEPTH  256      /* BSP depth the renderer's stack holds:
                                  63 words of scratchpad + 4 RAM blocks   */
#define QW_MAX_BM_DEPTH 60     /* a brush model's subtree depth: the
                                  renderer's own stack for it (64 words) */
#define QW_NONE        0xffffffffu

/* face record flags */
#define QWF_SKY     (1 << 0)
#define QWF_LIQUID  (1 << 1)

typedef struct {
	int32_t pos[3];        /* world units                    */
	int16_t yaw, pitch;    /* whole degrees                  */
} qw_view;

typedef struct {
	char     magic[4];             /* "QWVB"                                  */
	uint32_t num_nodes;            /* + 1 sentinel node in the file           */
	uint32_t num_leaves;           /* leaf 0 (solid), the world's 1..num_visleafs,
	                                  then one per contents for every brush
	                                  model's leaves (nothing reads them but
	                                  their contents: shared, §58)            */
	uint32_t num_marks;
	uint32_t num_faces;            /* the world's records (visframe ids)      */
	uint32_t num_records;          /* face records in the stream, the brush
	                                  models' included                        */
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
	uint32_t num_tex;              /* the texture slots record.tex indexes: the
	                                  texture set's table (formats/qtex.h)    */
	uint32_t ofs_verts, num_verts; /* qw_vert[num_verts]: the map's vertex pool,
	                                  shared by every record (16-bit indices) */
	uint32_t ofs_ambient;          /* uint8_t[num_leaves][2]: the leaf's water and
	                                  sky ambient sound levels (Quake's
	                                  ambient_level[AMBIENT_WATER / SKY])      */
	uint32_t ofs_bmodels, num_bmodels;   /* qw_bmodel[num_bmodels]: Quake's models,
	                                  0 = the world (its entry is unused)     */
	uint32_t ofs_ents, num_ents;   /* the entity lump cooked, num_ents entities in
	                                  the compact table (qw_ent_decode); the
	                                  section runs to ofs_strings             */
	uint32_t ofs_strings, len_strings;   /* the entities' strings, NUL-ended;
	                                  offset 0 is "" (no string)              */
	uint32_t worldtype;            /* worldspawn's: 0 medieval, 1 metal, 2 base */
	uint32_t title;                /* string: worldspawn's message, the map's name */
	uint32_t max_bm_depth;         /* deepest brush model subtree (<= QW_MAX_BM_DEPTH) */
} qw_header;

/* (The map's own models, the items', are in their own file, one per model
   level: formats/qgame.h qmm_header. The animated textures' tiles are the
   texture set's: formats/qtex.h qw_anim.) */

/* A brush model: Quake's submodel. Bounds in the physics' units, spread by
   a unit like Mod_LoadSubmodels; the root is its hull-0 BSP (its nodes in
   the node array; disk: index, bit31 = leaf; RAM: pointer, bit0 = leaf);
   the clip hull roots index the clipnode table like the world's (BSP29's
   clipnode children are 16-bit: any index fits). The room its pieces take
   when the world's BSP cuts it (engine/bmfrag.c), the most of every place
   its entity can stand at (tools/qcook/bmf.c): its records' bytes, their
   vertices in the pieces' pool, their groups; 0 for a model no drawn
   entity uses. */
typedef struct {
	int32_t  mins[3];      /* +0  1/QW_CLIP_SUB world units                    */
	int32_t  maxs[3];      /* +12                                              */
	uint32_t root;         /* +24 hull 0: node index (disk) / pointer (RAM)    */
	int16_t  hull1;        /* +28 clipnode roots: player-sized, large          */
	int16_t  hull2;        /* +30                                              */
	uint16_t room_recs;    /* +32 the pieces' records, bytes / 4               */
	uint16_t room_verts;   /* +34 their vertices                               */
	uint16_t room_groups;  /* +36 their groups (a leaf's records on a node side) */
	uint16_t depth;        /* +38 its subtree's depth                          */
} qw_bmodel;               /* 40 bytes */

/* The entity classes the cook knows (tools/qcook/ents.c names them); an
   unknown classname cooks as QE_UNKNOWN with its name kept. Lights without
   a targetname, info_null, the ambient_* sounds (the sound bank has them),
   worldspawn and what single player never spawns (the coop / deathmatch
   starts, the entities on no skill) are left out. */
enum {
	QE_UNKNOWN,
	QE_INFO_PLAYER_START, QE_INFO_PLAYER_START2, QE_INFO_TELEPORT_DESTINATION,
	QE_INFO_INTERMISSION, QE_INFO_NOTNULL,
	QE_FUNC_DOOR, QE_FUNC_DOOR_SECRET, QE_FUNC_BUTTON, QE_FUNC_PLAT, QE_FUNC_TRAIN,
	QE_FUNC_WALL, QE_FUNC_ILLUSIONARY, QE_FUNC_EPISODEGATE, QE_FUNC_BOSSGATE,
	QE_TRIGGER_ONCE, QE_TRIGGER_MULTIPLE, QE_TRIGGER_SECRET, QE_TRIGGER_RELAY,
	QE_TRIGGER_COUNTER, QE_TRIGGER_TELEPORT, QE_TRIGGER_CHANGELEVEL, QE_TRIGGER_SETSKILL,
	QE_TRIGGER_ONLYREGISTERED, QE_TRIGGER_PUSH, QE_TRIGGER_HURT, QE_TRIGGER_MONSTERJUMP,
	QE_PATH_CORNER, QE_MISC_FIREBALL, QE_MISC_EXPLOBOX, QE_MISC_EXPLOBOX2,
	QE_MISC_TELEPORTTRAIN, QE_TRAP_SPIKESHOOTER, QE_TRAP_SHOOTER, QE_AIR_BUBBLES,
	QE_EVENT_LIGHTNING,
	QE_LIGHT, QE_LIGHT_FLUORO, QE_LIGHT_FLUOROSPARK, QE_LIGHT_GLOBE,
	QE_LIGHT_TORCH_SMALL_WALLTORCH, QE_LIGHT_FLAME_LARGE_YELLOW,
	QE_LIGHT_FLAME_SMALL_YELLOW, QE_LIGHT_FLAME_SMALL_WHITE,
	QE_ITEM_HEALTH, QE_ITEM_ARMOR1, QE_ITEM_ARMOR2, QE_ITEM_ARMORINV,
	QE_ITEM_SHELLS, QE_ITEM_SPIKES, QE_ITEM_ROCKETS, QE_ITEM_CELLS,
	QE_ITEM_KEY1, QE_ITEM_KEY2, QE_ITEM_SIGIL,
	QE_ITEM_ARTIFACT_INVULNERABILITY, QE_ITEM_ARTIFACT_ENVIROSUIT,
	QE_ITEM_ARTIFACT_INVISIBILITY, QE_ITEM_ARTIFACT_SUPER_DAMAGE, QE_ITEM_WEAPON,
	QE_WEAPON_SUPERSHOTGUN, QE_WEAPON_NAILGUN, QE_WEAPON_SUPERNAILGUN,
	QE_WEAPON_GRENADELAUNCHER, QE_WEAPON_ROCKETLAUNCHER, QE_WEAPON_LIGHTNING,
	QE_MONSTER_ARMY, QE_MONSTER_DOG, QE_MONSTER_OGRE, QE_MONSTER_OGRE_MARKSMAN,
	QE_MONSTER_KNIGHT, QE_MONSTER_ZOMBIE, QE_MONSTER_WIZARD, QE_MONSTER_DEMON1,
	QE_MONSTER_SHAMBLER, QE_MONSTER_BOSS, QE_MONSTER_ENFORCER, QE_MONSTER_HELL_KNIGHT,
	QE_MONSTER_SHALRATH, QE_MONSTER_TARBABY, QE_MONSTER_FISH, QE_MONSTER_OLDONE,
	QE_COUNT
};

/* An entity, its keys as the id1 maps use them (the rest of the QC's fields
   are set by the spawn functions). Strings are offsets into the string
   section, 0 = not given. Angles are whole degrees as the map gives them
   (`angle`: -1 up, -2 down for SetMovedir), times are hundredths of a
   second, the origin is in the physics' units. The file holds them in the
   compact table below; qw_ent_decode gives this struct back. */
typedef struct {
	uint8_t  class;        /* +0  QE_*                                         */
	uint8_t  sounds;       /* +1                                               */
	uint16_t spawnflags;   /* +2                                               */
	int16_t  model;        /* +4  brush model index ("*N"), 0 = none           */
	int16_t  angle;        /* +6  degrees; -1 up, -2 down; 0 when not given    */
	int32_t  origin[3];    /* +8  1/QW_CLIP_SUB world units                    */
	int16_t  mangle[3];    /* +20 degrees: pitch, yaw, roll                    */
	int16_t  speed;        /* +26 world units per second                       */
	int16_t  wait;         /* +28 x100 (-100 = -1: never)                      */
	int16_t  delay;        /* +30 x100                                         */
	int16_t  lip;          /* +32                                              */
	int16_t  height;       /* +34                                              */
	int16_t  dmg;          /* +36                                              */
	int16_t  health;       /* +38                                              */
	int16_t  count;        /* +40                                              */
	int16_t  style;        /* +42                                              */
	uint16_t target;       /* +44 strings                                      */
	uint16_t targetname;   /* +46                                              */
	uint16_t killtarget;   /* +48                                              */
	uint16_t message;      /* +50                                              */
	uint16_t map;          /* +52                                              */
	uint16_t classname;    /* +54 (every entity keeps its name)                */
} qw_ent;                  /* 56 bytes */

_Static_assert(sizeof(qw_bmodel) == 40 && sizeof(qw_ent) == 56, "qworld.h: the cooked tables' sizes");
_Static_assert(__builtin_offsetof(qw_bmodel, root) == 24 && __builtin_offsetof(qw_bmodel, hull1) == 28 &&
               __builtin_offsetof(qw_bmodel, hull2) == 30, "engine/p_defs.inc BM_*");

/* The compact entity table at ofs_ents (docs/porting-notes.md §59): most
   of a qw_ent's fields are zero, so the file keeps the ones that are not.
     uint16_t names[QE_COUNT]   each class's classname (a string offset; 0
                                for a class the map has none of)
     then num_ents entities in the table's order, each:
       uint32_t head            bits 0-6 the class, bit 7 + QWE_* each field
                                present, bit 31 zero
       the fields present       in the order of their bits, little-endian,
                                unaligned (QWE_FIELD_AT: their bytes)
       uint16_t classname       for QE_UNKNOWN only (a known class's is
                                names[class])
     then zero bytes to the strings' 4-byte alignment.
   The origin's fields are whole world units in 2 bytes (x QW_CLIP_SUB
   decoded), or with QWE_FINE 1/QW_CLIP_SUB units in 4 (every id1 origin
   is whole). A map has at most 256 brush models (BSP29), so the model
   takes a byte; the spawnflags' two bytes are a field each (the skill
   flags are the high byte). The fields go by how often the id1 maps give
   them, so the decoder's loop ends early (7.6 bits an entity, not 14).
   The cook decodes every entity it writes and checks it against its
   qw_ent, byte for byte (tools/qcook/ents.c). */
enum {
	QWE_ORIGIN_X, QWE_ORIGIN_Y, QWE_ORIGIN_Z, QWE_TARGETNAME, QWE_ANGLE,
	QWE_TARGET, QWE_FLAGS_HI, QWE_MODEL, QWE_FLAGS_LO, QWE_WAIT, QWE_SOUNDS,
	QWE_SPEED, QWE_STYLE, QWE_LIP, QWE_MESSAGE, QWE_MANGLE, QWE_DELAY,
	QWE_HEALTH, QWE_HEIGHT, QWE_DMG, QWE_MAP, QWE_COUNT, QWE_KILLTARGET,
	QWE_FINE,
	QWE_FIELDS
};
#define QWE_CLASS   0x7fu                       /* the head's class bits           */
#define QWE_BIT(f)  (1u << (7 + (f)))           /* the head's bit of field f       */

/* each field's offset in the qw_ent and its bytes in the table (the
   origin's: 2, or 4 with QWE_FINE; QWE_FINE itself: none) */
static const uint8_t QWE_FIELD_AT[QWE_FIELDS][2] = {
	{  8, 2 }, { 12, 2 }, { 16, 2 }, { 46, 2 }, {  6, 2 },   /* origin, targetname, angle   */
	{ 44, 2 }, {  3, 1 }, {  4, 1 }, {  2, 1 }, { 28, 2 },   /* target, spawnflags' high byte,
	                                                            model, low byte, wait        */
	{  1, 1 }, { 26, 2 }, { 42, 2 }, { 32, 2 }, { 50, 2 },   /* sounds, speed, style, lip,
	                                                            message                      */
	{ 20, 6 }, { 30, 2 }, { 38, 2 }, { 34, 2 }, { 36, 2 },   /* mangle, delay, health, height,
	                                                            dmg                          */
	{ 52, 2 }, { 40, 2 }, { 48, 2 }, {  0, 0 },              /* map, count, killtarget, FINE */
};

_Static_assert(QE_COUNT <= 128 && QWE_FIELDS <= 24, "qworld.h: the compact entity head's bits");
_Static_assert(__builtin_offsetof(qw_ent, origin) == 8 && __builtin_offsetof(qw_ent, mangle) == 20 &&
               __builtin_offsetof(qw_ent, map) == 52 && __builtin_offsetof(qw_ent, classname) == 54,
               "qworld.h: QWE_FIELD_AT");

/* The entity at p into q (every field: the absent ones zero); where the
   next one starts. names: the table's class names (ofs_ents). */
static inline const uint8_t *qw_ent_decode(const uint8_t *p, const uint16_t *names, qw_ent *q)
{
	uint32_t head = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
	uint32_t cls = head & QWE_CLASS, f = 0;
	p += 4;
	*q = (qw_ent) { .class = (uint8_t) cls };   /* (word stores: a memset would be
	                                                 trimmed to bytes 1-55, a call) */
	for (uint32_t bits = head << 1 >> 8; bits; bits >>= 1, f++) {   /* (bits 7-30) */
		if (!(bits & 1))
			continue;
		uint8_t *d = (uint8_t *) q + QWE_FIELD_AT[f][0];
		uint32_t n = QWE_FIELD_AT[f][1];
		if (f <= QWE_ORIGIN_Z) {
			int32_t v;
			if (head & QWE_BIT(QWE_FINE)) {
				v = (int32_t) (p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24);
				p += 4;
			} else {
				v = (int16_t) (p[0] | p[1] << 8) * QW_CLIP_SUB;
				p += 2;
			}
			__builtin_memcpy(d, &v, sizeof v);
			continue;
		}
		while (n--)
			*d++ = *p++;
	}
	if (cls) {
		q->classname = names[cls];
	} else {
		q->classname = (uint16_t) (p[0] | p[1] << 8);
		p += 2;
	}
	return p;
}

/* Nodes and leaves share their bbox layout (the renderer's frustum test
   takes either): c at +0, h at +8, each followed by a 16-bit field, so VZ is
   loaded with lh + mtc2, never lwc2. Parents are word offsets into the node
   array (node index x 9), QW_NOPARENT for the root. A node's plane is the
   BSP plane in the clip section's table (shared with the clip hulls): the
   renderer works in vertex units (world x 4), the table in 1/8 units, so
   the renderer halves the distance. */
#define QW_NOPARENT 0xffffu
#define QW_NODE_WORDS 9        /* sizeof(qw_node) / 4                      */

typedef struct {
	int16_t  c[3];         /* +0  bbox centre, vertex units                    */
	uint16_t visframe;     /* +6  runtime                                      */
	int16_t  h[3];         /* +8  bbox half extents, vertex units              */
	uint16_t plane3;       /* +14 plane index x 3 = word offset in the table   */
	uint16_t parent;       /* +16 word offset of the parent, or QW_NOPARENT    */
	uint16_t dlstamp;      /* +18 runtime: the frame count when a dynamic light
	                              reaches the node's box (engine/dlight.c)    */
	uint32_t child[2];     /* +20 disk: index, bit31 = leaf; RAM: ptr, bit0 = leaf */
	uint32_t faces[2];     /* +28 front start, back start: disk offsets in the
	                              face stream, RAM pointers. The back block
	                              ends where the next node's front block
	                              starts: nodes[num_nodes] is a sentinel whose
	                              faces[0] is the end of the stream.           */
} qw_node;                 /* 36 bytes */

typedef struct {
	int16_t  c[3];         /* +0  bbox centre, vertex units                    */
	uint16_t visframe;     /* +6  runtime                                      */
	int16_t  h[3];         /* +8  bbox half extents, vertex units              */
	uint16_t num_marks;    /* +14                                              */
	uint32_t marks;        /* +16 disk: index into marks[]; RAM: ptr           */
	uint32_t vis;          /* +20 disk: offset into vis or QW_NONE; RAM: ptr/0 */
	uint16_t parent;       /* +24 word offset of the parent node               */
	int8_t   contents;     /* +26 Quake CONTENTS_*                             */
	uint8_t  light;        /* +27 the light of the leaf's faces, averaged
	                              (empty leaves: their surroundings'); models
	                              in the leaf are lit with it (128 = as is)   */
} qw_leaf;                 /* 28 bytes */

/* where a node's face block on `side` ends (nodes is an array with its
   sentinel) */
static inline uint32_t qw_face_end(const qw_node *n, int side)
{
	return side ? n[1].faces[0] : n->faces[1];
}

/* Face record: header, then its vertex indices into the map's pool (uint16,
   padded with copies of the first to a multiple of 3 entries, then to 4
   bytes: the renderer transforms three at a time), then one light byte per
   vertex (Quake's lightmap at the vertex, 128 = texture as is), padded to
   4 bytes, so they start at polys - ((num_verts + 3) & ~3); then quads,
   then tris. The pieces of a record
   share one texture (one or several adjacent Quake faces). Polygons are
   drawn Gouraud-shaded, texture x light, dithered. */
typedef struct {
	uint16_t face_id;      /* +0  Quake face index (visframe slot)            */
	uint8_t  num_verts;    /* +2  <= QW_MAX_VERTS (the renderer transforms in
	                                threes and may read up to 2 verts past the
	                                list: harmless, never referenced)          */
	uint8_t  num_quads;    /* +3                                              */
	uint8_t  num_tris;     /* +4                                              */
	uint8_t  flags;        /* +5  QWF_* (bits 2 - 7: the loader's, the
	                              record's average light / 4)                 */
	uint16_t size;         /* +6  whole record, bytes                         */
	uint16_t tex;          /* +8  index into the qw_tex table                 */
	uint16_t sr;           /* +10 bounding sphere radius, vertex units         */
	int16_t  sc[3];        /* +12 bounding sphere centre, vertex units:
	                              +12 / +16 load straight into VXY0 / VZ0       */
	uint16_t polys;        /* +18 byte offset of the first quad / tri         */
} qw_record;               /* 20 bytes */

typedef struct {
	int16_t x, y, z, pad;  /* vertex units                                   */
} qw_vert;

/* Vertex indices are pre-multiplied by 4 (byte offsets into the renderer's
   per-record vertex cache). Quad corners are in PS1 order: the GPU draws
   triangles (0,1,2) and (1,2,3). uv[k] = u | v << 8 in texels: on a wrapped
   axis a virtual 0..255 coordinate the texture window folds onto the
   texture, otherwise page-relative. On the disc they are the finest
   texture level's, tile-relative (formats/qtex.h qtex_uvmap); the loader
   makes them the loaded set's, page-relative. The renderer ORs the CLUT
   into uv[0] and the tpage into uv[1]. */
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
   plane's offset in words, so no per-node multiply is left. Identical
   subtrees (the same plane, the same children) are one clipnode: the hulls
   are a DAG, numbered depth first from the roots (the world's hull 1, the
   brush models', then the hulls 2; docs/porting-notes.md §58). */
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
} qw_clipnode;                 /* 6 bytes */

_Static_assert(sizeof(qw_plane) == 12 && __builtin_offsetof(qw_plane, type) == 6 &&
               __builtin_offsetof(qw_plane, dist) == 8, "engine/p_defs.inc PL_*");
_Static_assert(sizeof(qw_clipnode) == 6 && __builtin_offsetof(qw_clipnode, child) == 2,
               "engine/p_defs.inc CN_*");

#define QW_CONTENTS_EMPTY  (-1)
#define QW_CONTENTS_SOLID  (-2)
#define QW_CONTENTS_WATER  (-3)
#define QW_CONTENTS_SLIME  (-4)
#define QW_CONTENTS_LAVA   (-5)
#define QW_CONTENTS_SKY    (-6)

/* The face stream is followed by at least this many bytes of padding so the
   renderer's 3-at-a-time vertex loads never run off the end. */
#define QW_STREAM_TAIL 16

/* ------------------------------------------------------------ zones --------
 * A zoned world (docs/porting-notes.md §57): the cook (`qcook zones`,
 * tools/qcook/zones.c) splits the world's face records into zone chunks
 * from the tables of the level-chunks analysis. A zone is a set of camera
 * leaves; its chunk holds every world record the PSX may draw from any of
 * them (records shared between zones are in each one's chunk). The level
 * arena keeps the skeleton and a few chunk slots: the camera leaf's zone,
 * the one drawn, and the zones nearest to it by walking distance,
 * prefetched (engine/zone.c).
 *
 *   <MAP>_G<n>.WLD  the skeleton, "QWZF": a QWVF file's header and
 *                   sections, a qw_zone_header right after the header, the
 *                   face stream and the vertex pool holding the brush
 *                   models' records only (their UVs mapped by the loader,
 *                   as a QWVF's);
 *   <MAP>_Z<t>.ZON  the chunks at texture level t (formats/qlevels.h): a
 *                   qz_file header in sector 0, then each zone's qw_chunk
 *                   from a sector of its own (qw_chunk_dir). A chunk's
 *                   records come as the loader would leave them: face ids
 *                   doubled, the average light in the flags, the UVs on
 *                   the texture set's tiles, the vertex indices into the
 *                   chunk's own pool.
 *
 * The skeleton's node array: the world's nodes holding records in some
 * chunk first (world_nodes of them), then a node no walk reaches whose
 * faces[0] ends the current chunk's last block, then the world's nodes no
 * chunk gives a record (their blocks empty for good, never patched), then
 * the brush models' nodes, then the sentinel. The walk follows children
 * pointers, so the order changes nothing it draws. When a chunk becomes
 * current, its patch list sets faces[] of the first world_nodes + 1 nodes
 * (engine/zone.s zone_patch); before any, every world block is empty.
 *
 * The chunks live in num_slots slots of slot_bytes each in the level arena
 * (engine/zone.c): a camera leaf's zone and its nearest, any of them in
 * any slot. (An arena holding just the biggest such set, compacted when it
 * is in pieces, would take 10-80 KB less a map, but moving the drawn
 * chunk costs a frame: docs/porting-notes.md §57.)
 */
#define QWORLD_ZONED_MAGIC "QWZF"   /* (QWVZ until §59: the entities of QWVE) */
#define QZONES_MAGIC       "QZN1"
#define QZ_NONE            0xffu    /* a qw_zone_set's empty entry; a leaf in no set */

typedef struct {
	uint32_t num_zones;
	uint32_t num_slots;      /* the chunks resident at once                     */
	uint32_t slot_bytes;     /* a slot: the biggest chunk, whole sectors        */
	uint32_t world_nodes;    /* the nodes the patch lists set, the end node next */
	uint32_t num_sets;
	uint32_t ofs_chunks;     /* qw_chunk_dir[num_zones]                         */
	uint32_t ofs_leaf_set;   /* uint8_t[num_leaves]: the leaf's set, QZ_NONE for
	                            leaf 0 (the camera in solid keeps its zone)     */
	uint32_t ofs_sets;       /* qw_zone_set[num_sets]                            */
	uint32_t reach;          /* world units: the tables hold every record the
	                            PVS lets the PSX draw within this depth (the
	                            draw distance + a margin)                       */
	uint32_t stamp;          /* the chunks' file carries the same               */
	uint32_t whole_records;  /* statistics: the world's records, its .wld's    */
	uint32_t whole_bytes;    /*   bytes (what a whole load took)               */
} qw_zone_header;            /* 48 bytes, at sizeof(qw_header) in a QWZF file  */

typedef struct {
	uint32_t sector;         /* the chunk's first sector in the .ZON file       */
	uint32_t bytes;          /* its bytes (read in whole sectors)               */
} qw_chunk_dir;

/* the zones a camera leaf wants: its own (drawn), then the nearest others
   by walking distance (a teleporter one step), QZ_NONE past the last */
typedef struct {
	uint8_t zone;
	uint8_t near[3];
} qw_zone_set;

typedef struct {
	char     magic[4];       /* "QZN1"                                          */
	uint32_t stamp;          /* the skeleton's                                  */
	uint32_t num_zones;
	uint32_t tex_level;      /* the texture set its UVs are mapped onto         */
} qz_file;

typedef struct {
	uint16_t zone;
	uint16_t num_patch;      /* patch words, the last one the end node's        */
	uint32_t stamp;
	uint32_t ofs_records;    /* from the chunk's start: the blocks, the patched
	                            nodes' in their order (front, back), then
	                            QW_STREAM_TAIL bytes                            */
	uint32_t len_records;
	uint32_t ofs_verts;      /* qw_vert[num_verts]: the chunk's vertex pool      */
	uint32_t num_verts;
	uint32_t num_records;
	uint32_t pad;
} qw_chunk;                  /* 32 bytes, then the patch words */

/* A patch word: a patched node and the patched nodes before it that hold
   no record in the chunk (their blocks empty at the running pointer):
   skip << 24 | front block's words << 12 | back block's words. A longer run
   of empty nodes takes words of skip 255 and blocks of none (256 nodes
   each). */
#define QZP_SKIP(w)   ((w) >> 24)
#define QZP_FRONT(w)  (((w) >> 10) & 0x3ffcu)    /* bytes */
#define QZP_BACK(w)   (((w) << 2) & 0x3ffcu)     /* bytes */
#define QZP_MAX_SKIP  255u
#define QZP_MAX_WORDS 4095u

_Static_assert(sizeof(qw_zone_header) == 48 && sizeof(qw_chunk) == 32 && sizeof(qz_file) == 16,
               "qworld.h: the zones' tables");

#endif
