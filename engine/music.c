/*
 * q1psx — the music: Quake's CD tracks (cd_*.c), streamed from the disc by
 * the SPU (docs/porting-notes.md §48, §52). See engine/music.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxcd.h>
#include <psxgpu.h>

#include <psxspu.h>
#include <hwregs_c.h>

#include "cd.h"
#include "mem.h"
#include "music.h"
#include "prof.h"
#include "q_fmt.h"
#include "qspu.h"
#include "settings.h"
#include "snd.h"
#include "stream.h"

#define MAX_TRACK 11              /* Quake's: 2 .. 11 */

static int playing;               /* the track, 0 none                   */

/* ---------------------------------------------------- the SPU's ring -- */

/* The track's chunks (formats/qspu.h) go round a ring of RING_CHUNKS a
   channel at the top of SPU RAM, which two voices play at the track's rate
   (22.05 kHz, the menu's theme 32 kHz): the
   ring's last block loops to its first (the flags set at the upload), so
   the voices never stop; the drive refills the chunks they are through,
   READ_CHUNKS at a time, between the streamed assets' reads (stream.c
   asks music_read_ok before each). Where the voices are comes from root
   counter 2, sysclock / 8, which the SPU's clock shares (44.1 kHz is
   sysclock / 768): a sample at the voices' pitch p (the track's rate x
   4096 / 44100) is 393216 / p of its ticks (192 at 22.05 kHz, the
   menu's 32 kHz 132.3: a chunk's ticks rounded, a tick in 0.2 s). */
#define RING_CHUNKS    MUSIC_RING_CHUNKS   /* (engine/music.h) */
#define RING_L         (512 * 1024 - MUSIC_SPU_BYTES)
#define RING_R         (RING_L + RING_CHUNKS * QSPU_CHUNK)
#define V_LEFT         22
#define V_RIGHT        23
#define READ_CHUNKS    MUSIC_REFILL_CHUNKS   /* a refill (engine/music.h): its chunks fill
                                                the staging buffer (stream.c STAGE_SECTORS) */
#define TICKS_SAMPLE_P 393216u   /* a sample's ticks x the pitch       */
#define SEEK_MS        100       /* a seek's guess: a hop within a level's
                                    section (the music files lie next to the
                                    streamed assets: short ones; §84) */
#define MUSIC_VOL      0x2ccc    /* as the CD's (engine/snd.c CD_VOL)        */

static uint32_t track_lba[MAX_TRACK + 1];   /* \MUSIC\SPU_<nn>.DAT's first sector, 0 none */
static char     level_dir[12];     /* the map loading or loaded: its section holds its
                                      track's copy (music_level), "" none            */
static uint32_t file_lba, chunks;  /* the playing file's first chunk, its chunks */
static uint32_t pitch, rate;       /* its voices' pitch, its samples a second   */
static uint32_t ticks_chunk;       /* root counter 2's ticks a chunk lasts      */
static uint32_t filled;            /* chunks put in the ring since the start    */
static uint32_t base, t_base;      /* chunks the voices were through at t_base  */
static int      reading_n;         /* a refill in flight: its chunks             */
static uint8_t *buf;               /* the refills' (the stream's staging buffer,
                                      or the level arena's before any map)      */
static int      volume = -1;
static uint32_t underruns;

static void list_track(const char *name, uint32_t lba, uint32_t size)
{
	(void) size;
	if (name[0] != 'S' || name[1] != 'P' || name[2] != 'U' || name[3] != '_' || name[4] < '0' ||
	    name[4] > '9' || name[5] < '0' || name[5] > '9')
		return;
	int t = (name[4] - '0') * 10 + (name[5] - '0');
	if (t <= MAX_TRACK)
		track_lba[t] = lba;
}

Q_COLD void music_init(void)
{
	memset(track_lba, 0, sizeof track_lba);
	cd_list_dir("\\MUSIC", list_track);
	int n = 0;
	for (int t = 0; t <= MAX_TRACK; t++)
		n += track_lba[t] != 0;
	printf("music: %d SPU streams on the disc\n", n);
}

void music_level(const char *map)
{
	level_dir[0] = 0;
	if (map && strlen(map) < sizeof level_dir)
		strcpy(level_dir, map);
}

/* the track's file: the level's own copy (its section, found in the
   level's directory kept by cd_cache_dir: no read), else \MUSIC's */
static uint32_t track_file(int track)
{
	char name[32];
	uint32_t lba;
	if (level_dir[0]) {
		q_snprintf(name, sizeof name, "\\LVL\\%s\\SPU_%02d.DAT;1", level_dir, track);
		if (cd_find(name, &lba, NULL))
			return lba;
	}
	return track_lba[track];
}

/* the chunks the voices are through, exact to the sample */
static uint32_t played(uint32_t *samples)
{
	uint32_t d = prof_now() - t_base;
	if (d >= ticks_chunk) {                      /* (folded often: no wrap) */
		base   += d / ticks_chunk;
		t_base += d / ticks_chunk * ticks_chunk;
		d %= ticks_chunk;
	}
	if (samples)
		*samples = base * QSPU_SAMPLES + d * pitch / TICKS_SAMPLE_P;
	return base;
}

/* the read chunk at src into its ring slot, both channels; the ring's
   first block marked its loop's start, its last its end */
static void upload(uint8_t *src, uint32_t slot)
{
	for (int c = 0; c < 2; c++) {
		uint8_t *d = src + c * QSPU_CHUNK;
		d[1] = slot == 0 ? 0x04 : 0;
		d[QSPU_CHUNK - 16 + 1] = slot == RING_CHUNKS - 1 ? 0x03 : 0;
		snd_dma_wait();                          /* (the other channel's, a RAM sound's swap:
		                                            engine/stream.c) */
		SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
		SpuSetTransferStartAddr((c ? RING_R : RING_L) + slot * QSPU_CHUNK);
		SpuWrite((const uint32_t *) d, QSPU_CHUNK);
	}
	snd_dma_wait();                              /* (the buffer read into again) */
}

/* a refill: the next chunks of the file into the buffer (to its end at most) */
static int refill_start(uint32_t n)
{
	uint32_t k = filled % chunks;
	if (n > chunks - k)
		n = chunks - k;
	if (!cd_read_start((int32_t) (file_lba + k * QSPU_CHUNK_SECTORS), n * QSPU_CHUNK_SECTORS, buf, CDW_MUSIC,
	                   (int) k))
		return 0;
	reading_n = (int) n;
	return 1;
}

static void refill_done(void)
{
	for (int j = 0; j < reading_n; j++)
		upload(buf + (uint32_t) j * 2 * QSPU_CHUNK, (filled + (uint32_t) j) % RING_CHUNKS);
	filled += (uint32_t) reading_n;
	reading_n = 0;
}

void music_play(int track)
{
	music_stop();
	if (track < 0 || track > MAX_TRACK)
		return;
	uint32_t file = track_file(track);
	if (!file)
		return;
	buf = stream_stage();
	if (!buf)
		buf = mem_level_alloc(READ_CHUNKS * 2 * QSPU_CHUNK);   /* (no map: the menu's) */
	if (!buf)
		return;
	const qspu_header *h = (const qspu_header *) buf;
	if (!cd_read_now((int32_t) file, 1, buf, CDW_DIR, track) ||
	    memcmp(h->magic, QSPU_MAGIC, 4) || h->track != track || h->rate < 8000 || h->rate > 44100 ||
	    !h->chunks) {
		printf("music: SPU_%02d.DAT unreadable or of another version\n", track);
		return;
	}
	chunks   = h->chunks;
	rate     = h->rate;
	pitch    = (rate * 4096u + 22050u) / 44100u;
	ticks_chunk = QSPU_SAMPLES * TICKS_SAMPLE_P / pitch;
	file_lba = file + 1;
	filled   = 0;
	/* the ring full first (the drive's: a map's load, a track change) */
	while (filled < RING_CHUNKS) {
		uint32_t n = RING_CHUNKS - filled < READ_CHUNKS ? RING_CHUNKS - filled : READ_CHUNKS;
		if (!refill_start(n) || cd_read_sync() < 0) {
			reading_n = 0;
			printf("music: SPU_%02d.DAT unreadable\n", track);
			return;
		}
		refill_done();
	}
	for (int c = 0; c < 2; c++) {
		int v = c ? V_RIGHT : V_LEFT;
		SpuSetKey(0, 1u << v);
		SPU_CH_FREQ(v)  = (uint16_t) pitch;
		SPU_CH_ADDR(v)  = (uint16_t) ((c ? RING_R : RING_L) >> 3);
		SPU_CH_ADSR1(v) = 0x000f;                /* full at once, held */
		SPU_CH_ADSR2(v) = 0x0000;
	}
	volume = -1;
	music_tick();                                /* (the volume) */
	SpuSetKey(1, 1u << V_LEFT | 1u << V_RIGHT);  /* together: the channels in step */
	base = 0;
	t_base = prof_now();
	playing = track;
	printf("music: track %d at %u Hz, %u chunks, the ring at SPU %05x, %s\n", track, (unsigned) rate,
	       (unsigned) chunks, (unsigned) RING_L, file == track_lba[track] ? "from \\MUSIC" : "the level's copy");
}

void music_stop(void)
{
	if (reading_n) {
		/* a refill cut short: the drive paused, and that waited for, before
		   the next command (without the wait, a map's first read after the
		   menu's music took one of the refill's sectors: START.SND "not
		   found", the drive's error 20h; docs/porting-notes.md §53) */
		cd_read_cut();
		cd_read_sync();
		CdControlB(CdlPause, 0, 0);
		cd_busy();
		reading_n = 0;
	}
	if (!playing)
		return;
	SpuSetKey(0, 1u << V_LEFT | 1u << V_RIGHT);
	playing = 0;
}

void music_tick(void)
{
	int v = MUSIC_VOL * settings.music / 10;
	if (v != volume) {
		volume = v;
		SPU_CH_VOL_L(V_LEFT)  = (uint16_t) v;
		SPU_CH_VOL_R(V_LEFT)  = 0;
		SPU_CH_VOL_L(V_RIGHT) = 0;
		SPU_CH_VOL_R(V_RIGHT) = (uint16_t) v;
	}
	if (!playing)
		return;
	if (reading_n) {
		int left = cd_poll();                    /* (also runs CdReadRetry's retries) */
		if (left > 0)
			return;
		if (left < 0)
			reading_n = 0;                       /* (an error: read again) */
		else
			refill_done();
	}
	uint32_t p = played(0);
	if (p >= filled) {                           /* the voices past the music in: the ring
		                                            played again; on from where they are */
		underruns++;
		printf("music: the ring ran dry (%u)\n", (unsigned) underruns);
		filled = p + 1;
	}
	uint32_t room = p + RING_CHUNKS - filled;    /* the slots the voices are through */
	if (room && (room >= READ_CHUNKS || filled - p < 3) && !stream_reading() && cd_ready())
		refill_start(room < READ_CHUNKS ? room : READ_CHUNKS);   /* (cd_ready: the last read's
		                                                    Pause done, engine/cd.h) */
}

/* a read of `sectors` may start: the music not reading, and what the ring
   holds lasting through it, its seek and the refill after */
int music_read_ok(int sectors)
{
	if (!playing)
		return 1;
	if (reading_n)
		return 0;
	uint32_t s;
	played(&s);
	uint32_t held_ms = (filled * QSPU_SAMPLES - s) * 1000u / rate;
	uint32_t need_ms = (uint32_t) (sectors + READ_CHUNKS * QSPU_CHUNK_SECTORS) * 1000u / 150u + 3 * SEEK_MS;
	return held_ms > need_ms;
}


