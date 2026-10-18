/*
 * q1psx — the streamed assets (formats/qstream.h, docs/porting-notes.md
 * §40): what a map loads only when the player comes near what uses it.
 *
 * Four pools, each a cache of the one above it:
 *   the disc     STREAM.DAT, every asset a sector apart;
 *   RAM          what the level arena leaves once the map is in (bump
 *                allocation, compacted when a block no longer fits);
 *   VRAM         a model's skin images, in what the map's textures and the
 *                game's pictures leave of the texture pages;
 *   SPU RAM      a sound's samples, past the map's bank (engine/snd.c).
 * A model is resident (usable) once it is in RAM and its images are in
 * VRAM; it leaves both at once. A sound passes through RAM: resident once
 * its samples are in SPU RAM.
 *
 * Who needs what: every residency pass (a few frames apart) the game says
 * which assets its entities use and how much (stream_want: a score, the
 * smaller the more needed, a distance from the player in world units, 0
 * for the pinned ones that must always be there: the keys, the weapons).
 * The manager then loads, nearest first, what is wanted within its kind's
 * reach and not resident, one CD read at a time while the game runs,
 * making room by dropping what is wanted least (never a pinned asset, nor
 * one wanted more than the one it makes room for). A CD read during the
 * game pauses the CD-DA music (engine/music.c), which plays on from where
 * it was once the reads are done.
 *
 * Nothing here runs between rl_begin() and the end of r_frame(): the
 * render list holds pointers into the RAM pool, which compaction moves.
 */

#ifndef Q1PSX_STREAM_H
#define Q1PSX_STREAM_H

#include <stdint.h>

#include "qstream.h"
#include "qworld.h"

#define STREAM_PIN    0u            /* stream_want's score: always resident */
#define STREAM_REACH  2500u         /* world units: a model wanted from farther
                                       is not loaded (nor kept against a nearer
                                       one)                                   */
#define STREAM_SOUND_REACH 1500u    /* a sound's (a full sound is heard to 1000) */

/* boot, after cd_init and gdata_load: STREAM.DAT's directory */
void stream_init(void);

/* an asset's index from its pak name ("progs/soldier.mdl") or its qs_hash,
   -1 if the disc has none; an asset's hash */
int  stream_find(const char *name);
int  stream_find_hash(uint32_t hash);
uint32_t stream_hash(int a);

/* a map is about to load (the level arena reset, the drive taken): a read
   in flight stopped, every asset out */
void stream_abort(void);

/* the map is loaded, its edicts allocated: the RAM pool takes what the
   level arena has left (less `keep` bytes), the VRAM pool what the map's
   textures leave; every asset out */
void stream_map_start(uint32_t keep);

/* demand, within a pass (engine/ents.c ents_stream_wants): asset `a` is
   needed at `score` (STREAM_PIN, or a distance in world units) */
void stream_want(int a, uint32_t score);

/* where the passes measure from (1/CLIP_SUB units): the player, or the
   map's start while it loads */
extern int32_t stream_origin[3];

/* once a frame, before the game's ticks: the pass when it is due, the CD
   read's progress, an asset installed, the next read */
void stream_frame(void);

/* the map's start (the loading screen): a pass, then every wanted asset
   read at once, nearest first, as far as the pools hold them */
void stream_preload(void);

/* a resident model's header and its images' texture words ([num_images]),
   or NULL when it is not resident */
const qst_model *stream_model(int a);
const qw_tex    *stream_model_tex(int a);

/* a resident sound: its samples' place in SPU RAM and its header (the
   bank's qs_sound's fields), or NULL when it is not resident */
typedef struct {
	uint32_t spu;            /* SPU RAM address, bytes                       */
	uint32_t len;            /* ADPCM bytes                                  */
	uint16_t pitch, flags;   /* formats/qsound.h qs_sound's                  */
	uint32_t ticks;
} stream_sound_t;
const stream_sound_t *stream_sound(int a);
int  stream_is_sound(int a);

/* the SPU pool's bounds (bytes), engine/snd.c's once a map's bank is in */
void stream_spu_pool(uint32_t lo, uint32_t hi);

/* demand for every asset of a's group (a monster's model, head, sounds:
   the cook's groups) */
void stream_want_group(int a, uint32_t score);

/* the bench and the overlay: how full the pools are, what was read */
typedef struct {
	uint32_t ram_size, ram_used;     /* bytes                              */
	uint32_t vram_texels, vram_used; /* texels of the free regions, used   */
	uint32_t reads, read_bytes;      /* since the map's start              */
	uint32_t evictions, failures;    /* assets dropped; placed nowhere     */
	uint32_t halved;                 /* skins at their fallback (VRAM short) */
	uint16_t resident, wanted;       /* assets, the last pass              */
	uint16_t music_pauses;
} stream_stats_t;
extern stream_stats_t stream_stats;
extern int stream_log;               /* print the loads and evictions      */

#ifdef Q1PSX_BENCH
void stream_dump(void);              /* before a frame's dump: the images' places */
#endif

#endif
