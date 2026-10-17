/*
 * q1psx — cooked sound bank "QSB1": the sounds a map can play, as SPU ADPCM,
 * with the map's static (ambient entity) sounds and its CD track. One file
 * per map (MAPS/<MAP>.SND), written by `qcook map --sounds`, loaded whole into
 * SPU RAM by engine/snd.c when the map starts.
 *
 * What goes in (the cook decides from the BSP): the sounds Quake precaches
 * on every map (world.qc worldspawn, weapons.qc W_Precache), less the ones
 * the map cannot trigger (no water: no drowning; no lava, slime, sky,
 * teleporter, item, boss), plus the ambient sounds of its entities (misc.qc
 * ambient_*, the torches and flames, a teleporter's hum) and the two area
 * ambients (water, wind) when the map has leaves with those levels. The
 * SPU holds 512 KB less its first 4 KB; when a map's set would not fit, the
 * cook halves the sample rate of its longest looping ambients, largest
 * first, and says so.
 *
 * ADPCM: 16-byte blocks of 28 samples (nocash: SPU ADPCM Samples). A looping
 * sound (Quake: the WAV's cue point) has its loop start on a block boundary
 * (silence prepended) and its last block flagged end + repeat; a one-shot's
 * last block is flagged end, which keys the voice off.
 *
 * Little-endian; every section 4-byte aligned, the ADPCM data 16-aligned;
 * offsets from the file start.
 */

#ifndef Q1PSX_QSOUND_H
#define Q1PSX_QSOUND_H

#include <stdint.h>

#define QSOUND_MAGIC "QSB1"

#define QS_SPU_BASE   0x1010               /* SPU RAM below: capture buffers      */
#define QS_SPU_BYTES  (512 * 1024 - QS_SPU_BASE)
#define QS_MAX_SOUNDS  160
#define QS_MAX_STATICS 256

#define QSF_LOOP 1

typedef struct {
	uint32_t hash;         /* qs_hash() of the Quake name, "weapons/guncock.wav" */
	uint32_t ofs;          /* bytes from the data start = from QS_SPU_BASE      */
	uint32_t len;          /* bytes, a multiple of 16                           */
	uint16_t pitch;        /* SPU pitch: rate x 4096 / 44100 (11025 Hz: 0x400)  */
	uint16_t flags;        /* QSF_*                                             */
	uint32_t ticks;        /* length in 60 Hz ticks, rounded up (a one-shot
	                          ends then; Quake: channel end = start + length)   */
} qs_sound;                /* 20 bytes */

typedef struct {
	int32_t  origin[3];    /* world units                                       */
	uint16_t sound;        /* index into the sounds                             */
	uint8_t  volume;       /* x255 (ambientsound's 0.5 -> 127)                  */
	uint8_t  atten;        /* x64 (ATTN_STATIC 3 -> 192)                        */
} qs_static;               /* 16 bytes */

typedef struct {
	char     magic[4];     /* "QSB1"                                            */
	uint32_t num_sounds, ofs_sounds;     /* qs_sound[]                          */
	uint32_t num_statics, ofs_statics;   /* qs_static[]                         */
	uint32_t ofs_data, len_data;         /* the ADPCM block, 16-aligned         */
	uint32_t cd_track;     /* worldspawn "sounds": the CD-DA track; 0 = none    */
	uint32_t ofs_names, len_names;       /* the names in sound order, NUL
	                                        separated (listing, debugging)      */
} qs_header;

/* FNV-1a over the name's bytes, lower-cased */
static inline uint32_t qs_hash(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s; s++) {
		uint32_t c = (uint8_t) *s;
		if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
		h = (h ^ c) * 16777619u;
	}
	return h;
}

#endif
