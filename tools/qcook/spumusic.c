/*
 * qcook — the music the SPU streams (formats/qspu.h): `qcook spumusic`, one
 * CD track a run (cook.sh runs them side by side), from <dir>/trackNN.wav
 * (cook.sh has ffmpeg resample the rerelease's tracks: 22.05 kHz, the
 * menu's theme 32 kHz; docs/porting-notes.md §48, §52), encoded to SPU
 * ADPCM, 4-bit: per 28 samples of a channel (a sound unit) the filter (the
 * SPU's five: 0, 60, 115 -52, 98 -55, 122 -60, over 64) and the shift
 * (0..12) with the least squared error, the residual rounded and encoded
 * against the decoder's own history (psx-spx's decoder: s = (t << 12) >>
 * shift, plus (old f0 + older f1 + 32) >> 6, clamped), so that the error
 * never adds up; the shift tried around the one the unit's residual needs.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qspu.h"

/* ---------------------------------------------------------------- ADPCM -- */

static const int ADPCM_K0[5] = { 0, 60, 115, 98, 122 };    /* (the CD-ROM's four, the SPU's fifth) */
static const int ADPCM_K1[5] = { 0, 0, -52, -55, -60 };

typedef struct { int h1, h2; } adpcm_chan;

static int clamp16(int v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

/* one sound unit of 28 samples (x, stride apart): its parameter byte and
   nibbles, the best of `filters` filters (5: the SPU's; psx-spx's
   decoder); the channel's history moved on as the decoder will; returns
   the squared error */
static double adpcm_unit(const int16_t *x, int stride, adpcm_chan *c, uint8_t *param, uint8_t nib[28], int filters)
{
	double best = 1e300;
	int bf = 0, bs = 0;
	uint8_t bn[28] = { 0 };
	for (int f = 0; f < filters; f++) {
		/* the shift the residual needs, predicted from the signal itself */
		int p1 = c->h1, p2 = c->h2, maxd = 0;
		for (int i = 0; i < 28; i++) {
			int pred = (p1 * ADPCM_K0[f] + p2 * ADPCM_K1[f] + 32) >> 6;
			int d = x[i * stride] - pred;
			if (d < 0) d = -d;
			if (d > maxd) maxd = d;
			p2 = p1;
			p1 = x[i * stride];
		}
		int sh = 12;
		while (sh > 0 && (int64_t) maxd * (1 << sh) > 7 * 4096 + 2047)
			sh--;
		for (int s = sh - 1; s <= sh + 1; s++) {
			if (s < 0 || s > 12)
				continue;
			int h1 = c->h1, h2 = c->h2;
			double err = 0;
			uint8_t q4[28];
			for (int i = 0; i < 28 && err < best; i++) {
				int pred = (h1 * ADPCM_K0[f] + h2 * ADPCM_K1[f] + 32) >> 6;
				int d = x[i * stride] - pred;
				int q = (int) (((int64_t) d * (1 << s) + 2048) >> 12);
				if (q > 7) q = 7;
				if (q < -8) q = -8;
				q4[i] = (uint8_t) (q & 15);
				int r = clamp16(pred + ((q * 4096) >> s));
				double e = r - x[i * stride];
				err += e * e;
				h2 = h1;
				h1 = r;
			}
			if (err < best) {
				best = err;
				bf = f;
				bs = s;
				memcpy(bn, q4, sizeof bn);
			}
		}
	}
	for (int i = 0; i < 28; i++) {               /* the decoder's history after it */
		int q = (int8_t) (bn[i] << 4) >> 4;
		int pred = (c->h1 * ADPCM_K0[bf] + c->h2 * ADPCM_K1[bf] + 32) >> 6;
		int r = clamp16(pred + ((q * 4096) >> bs));
		c->h2 = c->h1;
		c->h1 = r;
	}
	*param = (uint8_t) (bs | bf << 4);
	memcpy(nib, bn, 28);
	return best;
}

/* ----------------------------------------------------------------- WAV -- */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }

/* a 16-bit stereo PCM WAV at `rate`: its frames */
static const int16_t *wav_stereo(const uint8_t *d, long len, const char *path, uint32_t rate, uint32_t *frames)
{
	if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4))
		die("%s: not a WAV", path);
	int ok = 0;
	for (long p = 12; p + 8 <= len; ) {
		uint32_t n = rd32(d + p + 4);
		if (!memcmp(d + p, "fmt ", 4)) {
			if (rd16(d + p + 8) != 1 || rd16(d + p + 10) != 2 || rd32(d + p + 12) != rate ||
			    rd16(d + p + 22) != 16)
				die("%s: not 16-bit stereo PCM at %u Hz", path, (unsigned) rate);
			ok = 1;
		} else if (!memcmp(d + p, "data", 4)) {
			if (!ok)
				die("%s: data before its format", path);
			if ((uint64_t) p + 8 + n > (uint64_t) len)
				n = (uint32_t) (len - p - 8);
			*frames = n / 4;
			return (const int16_t *) (d + p + 8);
		}
		p += 8 + n + (n & 1);
	}
	die("%s: no data", path);
	return NULL;
}

/* ------------------------------------------------------------ SPU music -- */

/* `qcook spumusic --music <dir> --track <n> [--rate <hz>] --out
   <spu_nn.dat>`: the track (<dir>/trackNN.wav, 16-bit stereo at the rate:
   22050 by default, 32000 the menu's theme) as SPU ADPCM, chunk by chunk,
   each channel encoded against its own decoder's history through the
   whole track (formats/qspu.h) */
int cmd_spumusic(int argc, char **argv)
{
	const char *music = NULL, *out_path = NULL;
	int track = 0, rate = QSPU_RATE;
	for (int i = 2; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--music")) music    = argv[i + 1];
		if (!strcmp(argv[i], "--track")) track    = atoi(argv[i + 1]);
		if (!strcmp(argv[i], "--rate"))  rate     = atoi(argv[i + 1]);
		if (!strcmp(argv[i], "--out"))   out_path = argv[i + 1];
	}
	if (!music || !out_path || track < 2 || track > 99 || rate < 8000 || rate > 44100)
		die("usage: qcook spumusic --music <dir> --track <n> [--rate <hz>] --out <spu_nn.dat>");
	char wpath[512];
	snprintf(wpath, sizeof wpath, "%s/track%02d.wav", music, track);
	long wlen;
	uint8_t *wav = read_file(wpath, &wlen);
	uint32_t frames;
	const int16_t *pcm = wav_stereo(wav, wlen, wpath, (uint32_t) rate, &frames);
	uint32_t chunks = (frames + QSPU_SAMPLES - 1) / QSPU_SAMPLES;

	FILE *f = fopen(out_path, "wb");
	if (!f)
		die("cannot write %s", out_path);
	uint8_t sec[2048] = { 0 };
	qspu_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QSPU_MAGIC, 4);
	h.track  = (uint16_t) track;
	h.rate   = (uint16_t) rate;
	h.chunks = chunks;
	memcpy(sec, &h, sizeof h);
	fwrite(sec, 1, sizeof sec, f);
	adpcm_chan ch[2] = { { 0, 0 }, { 0, 0 } };
	double err = 0, sig = 0;
	uint8_t chunk[2 * QSPU_CHUNK];
	for (uint32_t k = 0; k < chunks; k++) {
		for (int c = 0; c < 2; c++)
			for (int b = 0; b < QSPU_CHUNK / 16; b++) {
				int16_t x[28];
				for (int i = 0; i < 28; i++) {
					uint32_t fr = k * QSPU_SAMPLES + (uint32_t) (b * 28 + i);
					x[i] = fr < frames ? pcm[fr * 2 + c] : 0;
					sig += (double) x[i] * x[i];
				}
				uint8_t *blk = chunk + c * QSPU_CHUNK + b * 16, nib[28];
				err += adpcm_unit(x, 1, &ch[c], &blk[0], nib, 5);
				blk[1] = 0;                           /* (no flags: the engine's) */
				for (int i = 0; i < 14; i++)
					blk[2 + i] = (uint8_t) (nib[2 * i] | nib[2 * i + 1] << 4);
			}
		fwrite(chunk, 1, sizeof chunk, f);
	}
	fclose(f);
	printf("qcook: %s — track %d, %.1f s at %d Hz, %.1f dB, %u chunks, %.1f MB\n", out_path, track,
	       frames / (double) rate, rate, err > 0 ? 10 * log10(sig / err) : 99.0, chunks,
	       (1 + chunks * QSPU_CHUNK_SECTORS) * 2048.0 / 1048576);
	free(wav);
	return 0;
}
