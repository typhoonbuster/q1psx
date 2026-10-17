#include <stdint.h>
#include <stdio.h>
#include <psxcd.h>

#include "music.h"

static CdlLOC      toc[100];
static int         ntracks;
static int         playing;
static volatile int ended;

static void on_end(CdlIntrResult status, uint8_t *result)
{
	(void) status;
	(void) result;
	ended = 1;                       /* CdlModeAP: the drive paused at the track's end */
}

void music_init(void)
{
	ntracks = CdGetToc(toc);
	if (ntracks < 0)
		ntracks = 0;
	printf("music: %d tracks on the disc\n", ntracks);
	CdAutoPauseCallback(on_end);
}

void music_play(int track)
{
	playing = 0;
	if (track < 2 || track > ntracks)
		return;                      /* no such audio track */
	uint8_t mode = CdlModeDA | CdlModeAP;
	CdControl(CdlSetmode, &mode, 0);
	CdControl(CdlSetloc, &toc[track], 0);
	CdControl(CdlPlay, 0, 0);
	playing = track;
	ended   = 0;
}

void music_stop(void)
{
	if (!playing)
		return;
	CdControlB(CdlPause, 0, 0);
	playing = 0;
}

void music_tick(void)
{
	if (playing && ended) {
		int t = playing;
		music_play(t);               /* CDAudio_Play(track, looping = true) */
	}
}

int music_track(void)
{
	return playing;
}
