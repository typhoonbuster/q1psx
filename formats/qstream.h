/*
 * q1psx — the streamed assets "QST1": every resource a map loads only when
 * the player comes near what uses it (docs/porting-notes.md §40). Written
 * by `qcook stream`, STREAM.DAT on the disc for every map (the models'
 * triangles reduced: tools/qcook/game.c stream_reduce, §43); engine/
 * stream.c reads its directory at boot, then each asset on demand,
 * straight from the disc into its RAM pool, while the game runs.
 *
 * An asset is one Quake resource, named by the qs_hash() of its pak name
 * ("progs/soldier.mdl", "soldier/sight1.wav"), so the engine finds what an
 * entity needs from the names the QC uses. Each starts on a sector of its
 * own (QST_SECTOR bytes), so a read starts and ends on whole sectors; the
 * cook puts the assets that are wanted together next to each other (a
 * monster's model, its head, its sounds), so that the engine can take
 * several in one read.
 *
 * A model asset (QSTK_MODEL) is read into the RAM pool whole: qst_model,
 * its frames, its records, its skin. The skin goes to the VRAM pool when
 * the asset arrives (the records' UVs are patched with its place there)
 * and its pixels are dropped from RAM.
 *
 * A sound asset (QSTK_SOUND: a sound with a place in the map, a monster's,
 * a door's, an item's, an ambient entity's) goes into the SPU pool (SPU RAM
 * past the map's bank, formats/qsound.h), at its full rate, through a small
 * staging buffer a few sectors at a time: its 64-byte header keeps every
 * piece's samples a multiple of 64 bytes (the SPU's DMA unit).
 *
 * Models: the compact format. A frame is Quake's own: one word per vertex,
 * x in bits 0-7, z in 8-15, y in 16-23 and the light normal's index in
 * 24-31 (trivertx_t, reordered), so x | y << 16 and z | normal << 16 are
 * the word and its right shift by 8, both masked with 0x00ff00ff: the
 * engine expands the frame it draws into the renderer's 8-byte qw_vert
 * pool with two ANDs and a shift a vertex (engine/mdl_expand.s). The
 * model's scale and origin go into its GTE matrix instead of its vertices
 * (Quake's R_AliasSetUpTransform does the same). Records index the frame's
 * vertices directly (no duplicated slots); their UVs are relative to the
 * skin image's corner, its border included; their bounding sphere's centre
 * is in the expanded units, its radius in vertex units (world x 4).
 *
 * A monster's body keeps only the frames its QC plays, each one once
 * (QSTM_FMAP, docs/porting-notes.md §67): a frame map after the header,
 * uint16_t[num_frames] (Quake's frame numbers, the state tables'), each
 * the frame's place among the frames stored (QST_NOFRAME: one no state
 * plays), padded to 4 bytes; the frames stored follow it (ofs_frames),
 * Quake's order, a frame equal to an earlier one (every vertex's word)
 * not stored again. A frame its neighbours make well enough (§97: the
 * animations thinned, QST_LERP) is not stored: its entry QST_LERP | its
 * own number, the frame cache's key as a stored one's place is.
 *
 * A body's level of detail by its vertices' order (QSTM_LOD, docs/
 * porting-notes.md §88): the cook numbers the vertices by what the eye
 * would lose without them, the most needed first, collapsing them one by
 * one onto a neighbour (the parent, always of a lower index); a frame's
 * first vertices are a band's, the others take their parent's place when
 * the frame is expanded (engine/mdl.c). After the frame map, a qst_lod:
 * for each band of QST_LOD_BAND world units of the model's depth the
 * vertices to expand, then the parents of the vertices that collapse
 * within the draw distance, uint16_t[num_verts - num_min], padded to 4;
 * the frames follow (ofs_frames). Each record's slots are ordered by the
 * band their last triangle vanishes at and its triangles by theirs, the
 * longest-lasting first, and the record ends with its own table
 * (QST_LOD_REC_BYTES: per band its slots and its triangles still drawn,
 * uint8_t each): the renderer transforms and draws those alone
 * (engine/r_world.s ds_records, light_recs_lod). Every sorted body has
 * the block and the tables; QCOOK_NOLOD=1 cooks today's order with
 * nothing cut at any band, QCOOK_LOD_PX the tolerance (scripts/cook.sh).
 *
 * Little-endian; offsets from the asset's start (models) or the file's
 * start (the directory).
 */

#ifndef Q1PSX_QSTREAM_H
#define Q1PSX_QSTREAM_H

#include <stdint.h>

#define QSTREAM_MAGIC "QST1"
#define QST_SECTOR    2048

/* asset kinds */
enum { QSTK_NONE, QSTK_MODEL, QSTK_SOUND };

typedef struct {
	char     magic[4];       /* "QST1"                                      */
	uint32_t num_assets;
	uint32_t ofs_assets;     /* qst_asset[num_assets], sorted by hash       */
	uint32_t dir_sectors;    /* the header and the directory, whole sectors:
	                            what the engine reads at boot               */
} qst_header;

typedef struct {
	uint32_t hash;           /* qs_hash() of the pak name                    */
	uint8_t  kind;           /* QSTK_*                                       */
	uint8_t  flags;          /* QSTF_*                                       */
	uint16_t group;          /* the cook's group: the assets of one monster
	                            share it (the engine reads a group's wanted
	                            assets in one go)                            */
	uint32_t sector;         /* its first sector, from the file's start      */
	uint32_t size;           /* bytes                                        */
} qst_asset;                 /* 16 bytes */

#define QSTF_RAM       2     /* a level's file: this sound is read into main RAM at the
                                map's load with the others so flagged (they lie first in
                                the file, one read) and played from there, its samples
                                put in the SPU's swap room when wanted (docs/porting-notes.md
                                §86) */
#define QST_ABSENT 0xffffffffu   /* a level's file (<MAP>_S.DAT, its section: the pool's
                                    directory with the sectors of the assets the level
                                    uses, docs/porting-notes.md §84): this asset is not
                                    in it */
#define QST_BANKED 0xfffffffeu   /* a level's file: this sound is in the level's bank
                                    (<MAP>.SND, resident in SPU RAM for the whole map):
                                    not in the file, never wanted of the stream (§86) */

/* a model asset's header */
#define QSTM_FLAT 1          /* no directional light (QGF_FLAT)              */
#define QSTM_SORT 2          /* a monster's body: its triangles drawn sorted
                                by depth (engine/r_world.s draw_model, the
                                per-model ordering table; triangles only),
                                its radius the table's range              */
#define QSTM_FMAP 4          /* a frame map after the header (the frames
                                stored: qst_frame_index)                  */
#define QSTM_LOD  8          /* the vertices in relevance order, a qst_lod
                                after the frame map (above, §88)          */
#define QST_NOFRAME 0xffffu  /* the map's entry for a frame not stored    */
#define QST_LERP    1024u    /* the map's entry QST_LERP | f: frame f not
                                stored, made from its neighbours' (f - 1
                                and f + 1, both stored): each byte of their
                                vertex words floor-averaged, f - 1's normal
                                (§97: the animations thinned)             */
#define QSTM_EF(flags) ((flags) >> 8)   /* the MDL's own flags (its EF_*: the client's
                                           rotation, trails and light) */
#define QSTM_EF_ROCKET   1   /* the fire trail and a light (lavaball.mdl)         */
#define QSTM_EF_GIB      4   /* a blood trail (gibs, heads)                       */
#define QSTM_EF_ROTATE   8   /* turns, 100 degrees a second (backpack.mdl)        */
#define QSTM_EF_TRACER  16   /* the wizard's green tracer (w_spike.mdl)           */
#define QSTM_EF_ZOMGIB  32   /* a slight blood trail (zom_gib.mdl)                */

/* A skin image: Quake puts a model's front on the left half of its skin
   and its back on the right half, and a triangle takes its texels from
   one half; the cook cuts the reduced skin there (when every triangle
   keeps to one), each half with its own border, so that the halves go to
   VRAM apart, where narrower room is free (the engine may also lay one
   transposed). A record draws with one image (its tex: 0 the front, 1 the
   back). Each image comes twice: at the skin's resolution, then at half
   of it, which the engine takes when VRAM has no room for the first
   (the records' UVs are the first's: the engine halves them, from x0). */
#define QST_BORDER 2         /* an image's border of its edge texels         */

typedef struct {
	uint16_t w, h;           /* texels, the border included (w even: a VRAM
	                            halfword holds two)                         */
	uint32_t ofs;            /* its pixels, from the asset's start: w x h
	                            palette indices, then zeros to a multiple of
	                            64 bytes (LoadImage's DMA unit)             */
	uint16_t x0;             /* its first column of the reduced skin         */
	uint16_t pad;
} qst_image;                 /* 12 bytes */

typedef struct {
	uint16_t num_frames;
	uint16_t num_verts;      /* per frame                                    */
	uint16_t num_records;
	uint16_t flags;          /* QSTM_*                                       */
	uint32_t ofs_frames;     /* uint32_t[num_frames][num_verts]              */
	uint32_t ofs_records;    /* the record stream (formats/qworld.h); the
	                            images' pixels follow it: the asset's RAM
	                            block ends with the records once they are in
	                            VRAM                                         */
	uint32_t len_records;
	uint8_t  shift;          /* the expansion's left shift of x, y and z (a
	                            big model: its matrix within the GTE's 4.12) */
	uint8_t  num_images;     /* 1, or 2: the skin's front and back halves    */
	uint16_t radius;         /* vertex units, about the entity's origin, over
	                            every frame                                  */
	int32_t  scale[3];       /* vertex units per expanded step, x 65536      */
	int32_t  origin[3];      /* the expanded 0's place, vertex units x 16    */
	qst_image images[2];     /* at the skin's resolution                     */
	qst_image small[2];      /* at half of it: VRAM's fallback               */
} qst_model;                 /* 96 bytes */

_Static_assert(sizeof(qst_asset) == 16 && sizeof(qst_model) == 96, "qstream.h: sizes");

/* a frame map's bytes (QSTM_FMAP: num_frames entries, to 4 bytes; 0 none) */
static inline uint32_t qst_map_bytes(uint32_t flags, uint32_t num_frames)
{
	return flags & QSTM_FMAP ? (num_frames * 2 + 3) & ~3u : 0;
}

/* Quake's frame `frame` (< num_frames): its place among the frames stored
   (a frame no state plays: the first's) */
static inline uint32_t qst_frame_index(const qst_model *m, int frame)
{
	if (!(m->flags & QSTM_FMAP))
		return (uint32_t) frame;
	uint32_t s = ((const uint16_t *) (m + 1))[frame];
	return s == QST_NOFRAME ? 0 : s;
}

/* The level of detail (QSTM_LOD): band b is a model whose origin's depth
   is within [b, b + 1) x QST_LOD_BAND world units (the last band: from its
   start, the draw distance). Band 0 is the whole model (nothing collapses
   at an error of 0). */
#define QST_LOD_BANDS 24
#define QST_LOD_BAND  64         /* world units */
#define QST_LOD_NONE  0xffffu    /* (a parent no vertex has) */

typedef struct {
	uint16_t num_min;        /* num_verts[QST_LOD_BANDS - 1]: the first vertex
	                            with a parent                               */
	uint16_t pad;
	uint16_t num_verts[QST_LOD_BANDS];   /* each band's vertices to expand: a
	                            frame's first; the others copy their parent's
	                            qw_vert (non-increasing; [0] = the model's) */
	/* uint16_t parent[num_verts - num_min]: vertex num_min + i's parent (a
	   lower index), padded to 4 bytes */
} qst_lod;                   /* 52 bytes */

_Static_assert(sizeof(qst_lod) == 4 + 2 * QST_LOD_BANDS, "qstream.h: qst_lod");

/* a record's table (QSTM_LOD): its last QST_LOD_REC_BYTES bytes, per band
   its slots to transform (a prefix of its index list) and its triangles
   to draw (a prefix of its polygons), uint8_t each */
#define QST_LOD_REC_BYTES (2 * QST_LOD_BANDS)

/* the model's qst_lod (QSTM_LOD): after its frame map */
static inline const qst_lod *qst_lod_of(const qst_model *m)
{
	return (const qst_lod *) ((const uint8_t *) (m + 1) + qst_map_bytes(m->flags, m->num_frames));
}

/* its parents */
static inline const uint16_t *qst_lod_parents(const qst_lod *l)
{
	return (const uint16_t *) (l + 1);
}

/* a qst_lod's bytes, the parents and their padding included */
static inline uint32_t qst_lod_bytes(uint32_t num_verts, uint32_t num_min)
{
	return (uint32_t) sizeof(qst_lod) + (((num_verts - num_min) * 2 + 3) & ~3u);
}

/* the frames stored (the frame map's: up to the records) */
static inline uint32_t qst_stored_frames(const qst_model *m)
{
	if (!(m->flags & QSTM_FMAP))
		return m->num_frames;
	return m->num_verts && m->ofs_records > m->ofs_frames ?
	       (m->ofs_records - m->ofs_frames) / ((uint32_t) m->num_verts * 4) : 0;
}

/* a sound asset: qst_sound, then its ADPCM data (len bytes, 16-aligned) */
typedef struct {
	uint16_t pitch;          /* SPU pitch: rate x 4096 / 44100               */
	uint16_t flags;          /* QSF_* (formats/qsound.h)                     */
	uint32_t ticks;          /* length in 60 Hz ticks, rounded up            */
	uint32_t len;            /* ADPCM bytes                                  */
	uint32_t pad[13];        /* (to 64 bytes: the samples DMA-aligned)       */
} qst_sound;                 /* 64 bytes */
_Static_assert(sizeof(qst_sound) == 64, "qstream.h: qst_sound");

#endif
