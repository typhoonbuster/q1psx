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

#endif
