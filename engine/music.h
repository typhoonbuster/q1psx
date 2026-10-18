/*
 * q1psx — music: Quake's CD audio (cd_*.c) on the disc's CD-DA tracks.
 *
 * The map's track (worldspawn "sounds", the bank's cd_track) plays in a
 * loop; it stops for the CD reads of a map load (one drive) and starts
 * again after. A disc without audio tracks (the bench's) plays nothing.
 */

#ifndef Q1PSX_MUSIC_H
#define Q1PSX_MUSIC_H

void music_init(void);
void music_play(int track);      /* the CD track number (2 = the first audio track) */
void music_stop(void);
void music_tick(void);           /* once per frame: a track that ended starts over */
int  music_track(void);          /* the one playing, 0 = none */

/* The drive is wanted for a data read while the game runs (the streamed
   assets, engine/stream.c): the music fades out over a few frames, its
   place on the disc is kept and the drive paused. music_suspend() returns
   1 once the drive is free (at once when nothing plays), 0 while it fades:
   call it again the next frame. music_resume() plays on from the kept
   place, fading in. */
int  music_suspend(void);
void music_resume(void);
int  music_suspended(void);

#endif
