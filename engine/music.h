/*
 * q1psx — the music: Quake's CD audio (cd_*.c), Quake's CD tracks streamed
 * from the disc (docs/porting-notes.md §46). The build picks how
 * (Q1PSX_MUSIC_MODE):
 *
 * - xa, the default: each track an XA stream (formats/qxa.h,
 *   \MUSIC\XA_<nn>.STR) the drive plays itself at single speed; its other
 *   sectors carry the carousel of the streamed assets its maps use, which
 *   engine/stream.c takes as they pass (stream_carousel,
 *   stream_xa_sector). The drive never leaves the stream while the game
 *   runs: no seek, no cut. At the music's end it goes back to its start
 *   (the track loops, as CDAudio_Play(track, true)).
 * - spu: each track ADPCM streamed into a ring in SPU RAM and played by
 *   two voices (formats/qspu.h, \MUSIC\SPU_<nn>.DAT); the drive refills
 *   the ring between the streamed assets' reads (stream.c's turn).
 *
 * The map's track (worldspawn "sounds", the bank's cd_track) plays in a
 * loop; a map's load stops it (the drive is the loader's) and plays the
 * next. A disc without the music (the bench's) plays nothing.
 */

#ifndef Q1PSX_MUSIC_H
#define Q1PSX_MUSIC_H

void music_init(void);
void music_play(int track);      /* the CD track number (2: the theme, ...) */
void music_stop(void);
void music_tick(void);           /* once per frame: the stream kept going  */
int  music_track(void);          /* the one playing, 0 = none */

/* A data read must leave the stream while the game runs (an asset no
   carousel carries; the drive's own reads, stream.c): the music fades out
   over a few frames, its place is kept and the drive paused.
   music_suspend() returns 1 once the drive is free (at once when nothing
   plays), 0 while it fades: call it again the next frame. music_resume()
   plays on from the kept place, fading in. */
int  music_suspend(void);
void music_resume(void);
int  music_suspended(void);

#endif
