/*
 * q1psx — the music: Quake's CD audio (cd_*.c), Quake's CD tracks streamed
 * from the disc (docs/porting-notes.md §48, §52): each track ADPCM streamed
 * into a ring in SPU RAM and played by two voices (formats/qspu.h,
 * \MUSIC\SPU_<nn>.DAT: 22.05 kHz, the menu's theme 32 kHz); the drive
 * refills the ring between the streamed assets' reads (stream.c's turn).
 *
 * The map's track (worldspawn "sounds", the bank's cd_track) plays in a
 * loop; a map's load stops it (the drive is the loader's) and plays the
 * next. A disc without the music (the bench's) plays nothing.
 */

#ifndef Q1PSX_MUSIC_H
#define Q1PSX_MUSIC_H

/* the SPU music's: its voices (the last two: left, right) and its ring at
   the top of SPU RAM, which the sounds leave it (engine/snd.c) */
#define MUSIC_SPU_VOICES 2
#define MUSIC_SPU_BYTES  (2 * 6 * 4096)  /* 6 chunks a channel: 2 s at 22 kHz */

void music_init(void);
void music_play(int track);      /* the CD track number (2: the theme, ...) */
void music_stop(void);
void music_tick(void);           /* once per frame: the stream kept going  */
int  music_track(void);          /* the one playing, 0 = none */

/* The music shares the drive with the streamed assets' reads
   (engine/stream.c): a read of `sectors` may start when the ring holds
   the music for it (and the refill after it) and the music is not
   reading; the music refills when the stream is not (stream_reading). */
int  music_read_ok(int sectors);
int  music_reading(void);

#endif
