/*
 * q1psx — the music's XA streams (the default music, Q1PSX_MUSIC=xa): one
 * file per CD track, \MUSIC\XA_<nn>.STR, written by `qcook carousel`
 * (tools/qcook/carousel.c), played by engine/music.c, its data taken as it
 * passes by engine/stream.c (docs/porting-notes.md §46).
 *
 * A stream is read at single speed, 75 sectors a second, and never left
 * while the game runs: one sector in QXA_INTERLEAVE is the track's music,
 * XA ADPCM at 37.8 kHz, stereo, 4-bit (18.75 sectors a second), which the
 * drive decodes itself into the SPU's CD input; the other three are data
 * sectors (Mode 2 Form 1, 2048 bytes) carrying the streamed assets that
 * the track's maps use (the union of their sets), one after the other,
 * over and over for the whole track: a carousel (56.25 sectors, 112.5 KB a
 * second; every asset passes once a turn). The engine marks the assets it
 * wants and copies their sectors as they pass under the head: no seek, so
 * no cut in the music. A model passes as its shared asset (the three model
 * levels in one, formats/qstream.h qst_shared), a sound as in STREAM.DAT.
 * A track no map plays (2: the menu's and the episodes' ends', 3: the
 * intermission's) has no carousel: zeros in its data sectors.
 *
 * Single speed because of the disc's room: a stream lasts as long as its
 * music, 59 minutes for the ten tracks; at double speed that would be
 * 530 000 sectors, the disc has 333 000. At single speed it takes what the
 * CD-DA tracks took.
 *
 * The file: the header (head_sectors Form 1 sectors, one: qxa_header,
 * then qxa_entry[num_assets], then uint8_t lut[period]), then the stream. Its
 * sector i (from the stream's first) is the music's when
 * i % QXA_INTERLEAVE == 0, else the data sector d = i - i / 4 - 1, the
 * carousel's sector c = d % period: lut[c] is its entry, c minus the
 * entry's first its piece (the asset's sector). After audio_end the stream
 * holds no more music (the engine goes back to the start: the track loops,
 * as Quake's CD track).
 *
 * Every sector's subheader: file = the CD track, channel 0; the music's
 * submode 0x64 (real time, form 2, audio) and coding 0x01 (stereo, 37.8
 * kHz, 4-bit); the data's submode 0x08 (data). The cooked file holds
 * QXA_SECTOR bytes a sector (the subheader twice, then the data),
 * mkpsxiso's "mixed" files: it writes the sync, the header, the EDC/ECC.
 */

#ifndef Q1PSX_QXA_H
#define Q1PSX_QXA_H

#include <stdint.h>

#define QXA_MAGIC      "QXA1"
#define QXA_INTERLEAVE 4
#define QXA_SECTOR     2336       /* a cooked sector: subheader x2 + 2328 bytes */
#define QXA_RATE       37800      /* the music's samples a second              */
#define QXA_SAMPLES    2016       /* a music sector's, a channel's             */
#define QXA_MAX_ASSETS 255        /* (lut entries are bytes)                   */

/* the subheader's submode bits */
#define QXA_SM_EOR   0x01
#define QXA_SM_AUDIO 0x04
#define QXA_SM_DATA  0x08
#define QXA_SM_FORM2 0x20
#define QXA_SM_RT    0x40
#define QXA_SM_EOF   0x80

typedef struct {
	char     magic[4];            /* "QXA1"                                     */
	uint16_t track;               /* the CD track: the sectors' file number     */
	uint16_t head_sectors;        /* this header's: the stream follows         */
	uint32_t stream_sectors;      /* the stream's                               */
	uint32_t audio_end;           /* its last music sector + 1                  */
	uint32_t period;              /* data sectors in a turn, 0 = no carousel    */
	uint16_t num_assets;          /* in a turn                                  */
	uint16_t pad;
	uint32_t ofs_entries;         /* qxa_entry[num_assets], in the turn's order  */
	uint32_t ofs_lut;             /* uint8_t[period]: each turn sector's entry   */
} qxa_header;                     /* 32 bytes */

typedef struct {
	uint16_t asset;               /* its index in STREAM_<n>.DAT's directory     */
	uint16_t sectors;             /* its sectors here (a model: the shared asset) */
	uint32_t first;               /* its first sector in the turn                */
} qxa_entry;                      /* 8 bytes */

_Static_assert(sizeof(qxa_header) == 32 && sizeof(qxa_entry) == 8, "qxa.h: sizes");

#endif
