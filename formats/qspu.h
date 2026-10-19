/*
 * q1psx — the music streamed by the SPU (the XA streams' alternative,
 * Q1PSX_MUSIC_MODE=spu): one file per CD track, \MUSIC\SPU_<nn>.DAT,
 * written by `qcook spumusic` (tools/qcook/carousel.c), played by
 * engine/music.c (docs/porting-notes.md §46).
 *
 * The track as SPU ADPCM at 22.05 kHz, stereo, in chunks of QSPU_CHUNK
 * bytes a channel (256 blocks, 7168 samples, 0.325 s): the left's, then
 * the right's, four sectors a chunk. The engine reads them at double speed
 * into a ring of six chunks a channel at the top of SPU RAM (2 s, 48 KB),
 * which two voices play round and round; the drive refills the ring
 * between the streamed assets' reads, wherever those are on the disc.
 * The blocks carry no flags: the engine sets the ring's loop start on its
 * first block and its loop end on its last as it uploads them. After the
 * last chunk the first again: the track loops, seamless.
 *
 * Layout: the header's sector (qspu_header), then chunk k at sector
 * 1 + 4 k. Little-endian.
 */

#ifndef Q1PSX_QSPU_H
#define Q1PSX_QSPU_H

#include <stdint.h>

#define QSPU_MAGIC   "QSP1"
#define QSPU_RATE    22050
#define QSPU_PITCH   2048                       /* the SPU's: rate x 4096 / 44100 */
#define QSPU_CHUNK   4096                       /* a channel's ADPCM bytes a chunk */
#define QSPU_SAMPLES (QSPU_CHUNK / 16 * 28)     /* 7168                           */
#define QSPU_CHUNK_SECTORS (2 * QSPU_CHUNK / 2048)

typedef struct {
	char     magic[4];       /* "QSP1"                                     */
	uint16_t track;          /* the CD track                               */
	uint16_t rate;           /* QSPU_RATE                                  */
	uint32_t chunks;         /* the track's                                */
	uint32_t pad;
} qspu_header;

#endif
