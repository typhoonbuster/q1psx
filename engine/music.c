/*
 * q1psx — the music: Quake's CD tracks (cd_*.c), streamed from the disc
 * (docs/porting-notes.md §46). See engine/music.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxcd.h>
#include <psxgpu.h>

#include "cd.h"
#include "mem.h"
#include "music.h"
#include "qxa.h"
#include "snd.h"
#include "stream.h"

#define MAX_TRACK 11              /* Quake's: 2 .. 11 */

/* the fade around a read that leaves the stream (music_suspend), x256 of
   the setting's volume */
#define FADE_STEP 32             /* 8 frames from full to silent        */
static int         fade = 256;
static int         fading_out;
static int         suspended;
static int         playing;      /* the track, 0 none                   */

static void set_fade(int f)
{
	if (f != fade) {
		fade = f;
		snd_cd_fade(fade);
	}
}

#if Q1PSX_MUSIC_XA

/* ------------------------------------------------------------ XA streams -- */

static uint32_t track_lba[MAX_TRACK + 1];   /* XA_<nn>.STR's first sector, 0 none */
static qxa_header *head;          /* the playing stream's header (the game arena) */
#define HEAD_SECTORS 1            /* (the cook keeps it to one) */
static int32_t  stream_lba;       /* the stream's first sector (after the header) */
static uint32_t audio_end;        /* its music's end, from stream_lba             */
static volatile uint32_t last_i;  /* the last sector the drive gave, from stream_lba */
static volatile uint8_t  at_end;  /* the music is over: back to the start         */
static volatile uint8_t  failed;  /* the drive reported an error                 */
static uint32_t resume_i;         /* music_suspend's place                       */

static void list_track(const char *name, uint32_t lba, uint32_t size)
{
	(void) size;
	if (name[0] != 'X' || name[1] != 'A' || name[2] != '_' || name[3] < '0' || name[3] > '9' ||
	    name[4] < '0' || name[4] > '9')
		return;
	int t = (name[3] - '0') * 10 + (name[4] - '0');
	if (t <= MAX_TRACK)
		track_lba[t] = lba;
}

static int bcd(uint8_t b)
{
	return (b >> 4) * 10 + (b & 15);
}

/* The CD interrupt, once a data sector is in the drive's buffer (the XA
   sectors go to the SPU on their own): its header (CdlModeSize: the
   sector's address, then its subheader), then stream.c takes its data
   when it is an asset's piece it waits for. */
static void xa_ready(CdlIntrResult irq, uint8_t *result)
{
	(void) result;
	if (irq != CdlDataReady) {
		failed = 1;
		return;
	}
	uint32_t h[3];
	CdGetSector(h, 3);
	const uint8_t *b = (const uint8_t *) h;
	int32_t i = bcd(b[0]) * 4500 + bcd(b[1]) * 75 + bcd(b[2]) - 150 - stream_lba;
	if (i < 0)
		return;
	if ((uint32_t) i >= audio_end) {          /* (past it: another stream's sectors) */
		at_end = 1;
		return;
	}
	last_i = (uint32_t) i;
	if ((uint32_t) i % QXA_INTERLEAVE)
		stream_xa_sector((uint32_t) i);
}

/* the drive on the stream from its sector i: single speed, the music's
   sectors played (CdlModeRT) when they are the track's (CdlModeSF: the
   file number), the data with its header (CdlModeSize) */
static void xa_start(uint32_t i)
{
	uint8_t mode = CdlModeRT | CdlModeSF | CdlModeSize;
	CdControl(CdlSetmode, &mode, 0);
	CdlFILTER f = { (uint8_t) playing, 0, 0 };
	CdControl(CdlSetfilter, &f, 0);
	at_end = failed = 0;
	last_i = i;
	CdReadyCallback(xa_ready);
	CdlLOC loc;
	CdIntToPos(stream_lba + (int32_t) i, &loc);
	CdControl(CdlReadS, &loc, 0);
}

void music_init(void)
{
	memset(track_lba, 0, sizeof track_lba);
	head = mem_game_alloc(HEAD_SECTORS * 2048);
	int n = 0;
	cd_list_dir("\\MUSIC", list_track);
	for (int t = 0; t <= MAX_TRACK; t++)
		n += track_lba[t] != 0;
	printf("music: %d XA streams on the disc\n", n);
}

void music_play(int track)
{
	music_stop();
	set_fade(256);
	if (track < 0 || track > MAX_TRACK || !track_lba[track] || !head)
		return;
	/* the header (the drive's, before the stream: a map's load, a track
	   change) */
	CdlLOC loc;
	CdIntToPos((int) track_lba[track], &loc);
	CdControl(CdlSetloc, &loc, 0);
	if (!CdReadRetry(1, (uint32_t *) head, CdlModeSpeed, 3) || CdReadSync(0, 0) < 0 ||
	    memcmp(head->magic, QXA_MAGIC, 4) || head->track != track || head->head_sectors < 1 ||
	    head->head_sectors > HEAD_SECTORS) {
		printf("music: XA_%02d.STR unreadable or of another version\n", track);
		return;
	}
	if (head->head_sectors > 1) {
		CdControl(CdlSetloc, &loc, 0);
		if (!CdReadRetry(head->head_sectors, (uint32_t *) head, CdlModeSpeed, 3) || CdReadSync(0, 0) < 0)
			return;
	}
	if (head->ofs_lut + head->period > (uint32_t) head->head_sectors * 2048 ||
	    head->ofs_entries + head->num_assets * sizeof(qxa_entry) > head->ofs_lut) {
		printf("music: XA_%02d.STR's carousel out of bounds\n", track);
		return;
	}
	stream_lba = (int32_t) track_lba[track] + head->head_sectors;
	audio_end  = head->audio_end;
	playing    = track;
	stream_carousel(head);                       /* (its assets taken as they pass) */
	xa_start(0);
	printf("music: track %d, %u sectors, a carousel of %u assets in %u sectors\n", track,
	       (unsigned) head->stream_sectors, (unsigned) head->num_assets, (unsigned) head->period);
}

void music_stop(void)
{
	stream_carousel(0);                          /* (no capture from here) */
	suspended = fading_out = 0;
	if (!playing)
		return;
	CdReadyCallback(0);
	CdControlB(CdlPause, 0, 0);                  /* (a load or a track change) */
	playing = 0;
}

void music_tick(void)
{
	if (!playing || suspended)
		return;
	if (!fading_out && fade < 256)               /* fading back in after a read */
		set_fade(fade + FADE_STEP > 256 ? 256 : fade + FADE_STEP);
	if (at_end || failed)                        /* the track over: from its start
	                                                (CDAudio_Play(track, looping));
	                                                an error: on from where it was */
		xa_start(at_end ? 0 : last_i + 1);
}

int music_suspend(void)
{
	if (suspended || !playing)
		return 1;
	fading_out = 1;
	if (fade > 0) {
		set_fade(fade - FADE_STEP < 0 ? 0 : fade - FADE_STEP);
		if (fade)
			return 0;
	}
	CdReadyCallback(0);
	resume_i = last_i + 1;
	CdControlB(CdlPause, 0, 0);
	suspended  = 1;
	fading_out = 0;
	return 1;
}

void music_resume(void)
{
	if (!suspended)
		return;
	suspended = 0;
	xa_start(resume_i);
}

#else

/* ------------------------------------------------------------ no music -- */

void music_init(void) { }
void music_play(int track) { (void) track; }
void music_stop(void) { }
void music_tick(void) { }
int  music_suspend(void) { return 1; }
void music_resume(void) { }

#endif

int music_track(void)
{
	return playing;
}

int music_suspended(void)
{
	return suspended;
}
