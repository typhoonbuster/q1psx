#include <stdint.h>
#include <stdio.h>
#include <psxcd.h>
#include <psxgpu.h>

#include "music.h"
#include "snd.h"

static CdlLOC      toc[100];
static int         ntracks;
static int         playing;
static volatile int ended;

/* the streamed reads' pause (music_suspend): the fade, x256 of the
   setting's volume, and the place to play on from */
#define FADE_STEP 32             /* 8 frames from full to silent        */
static int         fade = 256;   /* the CD input's level now             */
static int         fading_out;   /* music_suspend began the fade         */
static int         suspended;    /* the drive paused for a read          */
static int         suspended_track;
static CdlLOC      resume_at;

static void on_end(CdlIntrResult status, uint8_t *result)
{
	(void) status;
	(void) result;
	ended = 1;                       /* CdlModeAP: the drive paused at the track's end */
}

/* The drive is not ready for the TOC right after CdInit on an accurate
   emulator or the machine (DuckStation answered 0 tracks; PCSX-Redux is
   instant): a few tries, a quarter second apart. */
void music_init(void)
{
	ntracks = 0;
	for (int try = 0; try < 12 && ntracks <= 0; try++) {
		if (try)
			for (int i = 0; i < 15; i++)
				VSync(0);
		ntracks = CdGetToc(toc);
	}
	if (ntracks < 0)
		ntracks = 0;
	printf("music: %d tracks on the disc\n", ntracks);
	CdAutoPauseCallback(on_end);
}

/* track = the disc's track number (Quake's worldspawn "sounds": 2..11);
   CdGetToc fills toc[0] with track 1 */
void music_play(int track)
{
	playing = 0;
	suspended = fading_out = 0;
	if (fade != 256) {
		fade = 256;
		snd_cd_fade(fade);
	}
	if (track < 2 || track > ntracks)
		return;                      /* no such audio track */
	uint8_t mode = CdlModeDA | CdlModeAP;
	CdControl(CdlSetmode, &mode, 0);
	CdControl(CdlSetloc, &toc[track - 1], 0);
	CdControl(CdlPlay, 0, 0);
	playing = track;
	ended   = 0;
}

void music_stop(void)
{
	suspended = fading_out = 0;
	if (!playing)
		return;
	CdControlB(CdlPause, 0, 0);
	playing = 0;
}

void music_tick(void)
{
	if (suspended)
		return;
	if (!fading_out && fade < 256) {             /* fading back in after a read */
		fade += FADE_STEP;
		if (fade > 256) fade = 256;
		snd_cd_fade(fade);
	}
	if (playing && ended) {
		int t = playing;
		music_play(t);               /* CDAudio_Play(track, looping = true) */
	}
}

int music_track(void)
{
	return playing;
}

int music_suspended(void)
{
	return suspended;
}

int music_suspend(void)
{
	if (suspended || !playing)
		return 1;
	fading_out = 1;
	if (fade > 0) {
		fade -= FADE_STEP;
		if (fade < 0) fade = 0;
		snd_cd_fade(fade);
		if (fade)
			return 0;
	}
	/* silent: where the head is (subchannel Q), then the drive paused */
	CdlLOCINFOP at;
	if (CdControlB(CdlGetlocP, 0, (uint8_t *) &at)) {
		resume_at.minute = at.minute;
		resume_at.second = at.second;
		resume_at.sector = at.sector;
		resume_at.track  = at.track;
	} else {
		resume_at = toc[playing - 1];            /* (no answer: from the track's start) */
	}
	CdControlB(CdlPause, 0, 0);
	suspended_track = playing;
	suspended  = 1;
	fading_out = 0;
	return 1;
}

void music_resume(void)
{
	if (!suspended)
		return;
	suspended = 0;
	playing   = suspended_track;
	ended     = 0;
	uint8_t mode = CdlModeDA | CdlModeAP;
	CdControl(CdlSetmode, &mode, 0);
	CdControl(CdlSetloc, &resume_at, 0);
	CdControl(CdlPlay, 0, 0);                    /* from the Setloc place */
}
