/*
 * qcook — the music's XA streams (formats/qxa.h): `qcook carousel --pak
 * <pak0.pak> [--pak1 <pak1.pak>] --pool <pool.dat> --music <dir> --track
 * <n> --out <xa_nn.str> [--maps "start e1m1 ..."]`, one CD track a run
 * (cook.sh runs them side by side), the maps the disc carries (default:
 * every one in the paks).
 *
 * The music: <dir>/trackNN.wav, 37.8 kHz 16-bit stereo (cook.sh has ffmpeg
 * resample the rerelease's tracks), encoded to XA ADPCM, 4-bit: per 28
 * samples of a channel (a sound unit) the filter (4, the CD-ROM's: 0, 60,
 * 115 -52, 98 -55, over 64) and the shift (0..12) with the least squared
 * error, the residual rounded and encoded against the decoder's own
 * history (psx-spx's decoder: s = (t << 12) >> shift, plus (old f0 + older
 * f1 + 32) >> 6, clamped), so that the error never adds up; the shift
 * tried around the one the unit's residual needs.
 *
 * The carousel: the streamed assets of every map whose worldspawn plays
 * the track ("sounds"): its monsters' kinds (stream.c), its items' models
 * (items.c), its entities' and ambients' sounds (sound.c), as the engine
 * asks for them (engine/monsters.c, items.c, ents.c, snd.c), from the pool
 * of shared assets (`qcook stream --pool`), in the pool's order (a
 * monster's assets side by side).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qsound.h"
#include "../../formats/qstream.h"
#include "../../formats/qspu.h"
#include "../../formats/qxa.h"

/* ------------------------------------------------------------------ XA -- */

static const int XA_K0[5] = { 0, 60, 115, 98, 122 };    /* (the 5th: the SPU's only) */
static const int XA_K1[5] = { 0, 0, -52, -55, -60 };

typedef struct { int h1, h2; } xa_chan;

static int clamp16(int v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

/* one sound unit of 28 samples (x, stride apart): its parameter byte and
   nibbles, with `filters` of them (4: the CD-ROM's; 5: the SPU's, whose
   decoder is the same, psx-spx); the channel's history moved on as the
   decoder will; returns the squared error */
static double xa_unit(const int16_t *x, int stride, xa_chan *c, uint8_t *param, uint8_t nib[28], int filters)
{
	double best = 1e300;
	int bf = 0, bs = 0;
	uint8_t bn[28] = { 0 };
	for (int f = 0; f < filters; f++) {
		/* the shift the residual needs, predicted from the signal itself */
		int p1 = c->h1, p2 = c->h2, maxd = 0;
		for (int i = 0; i < 28; i++) {
			int pred = (p1 * XA_K0[f] + p2 * XA_K1[f] + 32) >> 6;
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
				int pred = (h1 * XA_K0[f] + h2 * XA_K1[f] + 32) >> 6;
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
		int pred = (c->h1 * XA_K0[bf] + c->h2 * XA_K1[bf] + 32) >> 6;
		int r = clamp16(pred + ((q * 4096) >> bs));
		c->h2 = c->h1;
		c->h1 = r;
	}
	*param = (uint8_t) (bs | bf << 4);
	memcpy(nib, bn, 28);
	return best;
}

/* one music sector's data (2324 bytes: 18 sound groups, then zeros)
   from its 2016 stereo frames (pcm: interleaved, frames of them left;
   past them silence) */
static void xa_sector(const int16_t *pcm, uint32_t left, xa_chan ch[2], uint8_t out[2324], double *err, double *sig)
{
	memset(out, 0, 2324);
	for (int g = 0; g < 18; g++) {
		uint8_t *grp = out + g * 128, param[8];
		for (int b = 0; b < 8; b++) {             /* blocks: left, right, left, ... */
			int c = b & 1, u = b >> 1;
			int16_t x[28];
			for (int i = 0; i < 28; i++) {
				uint32_t fr = (uint32_t) (g * 112 + u * 28 + i);
				x[i] = fr < left ? pcm[fr * 2 + c] : 0;
				*sig += (double) x[i] * x[i];
			}
			uint8_t nib[28];
			*err += xa_unit(x, 1, &ch[c], &param[b], nib, 4);
			for (int i = 0; i < 28; i++)
				grp[16 + i * 4 + (b >> 1)] |= (uint8_t) (nib[i] << ((b & 1) * 4));
		}
		memcpy(grp + 4, param, 4);                /* blocks 0-3's, copied before */
		memcpy(grp + 0, param, 4);
		memcpy(grp + 8, param + 4, 4);            /* blocks 4-7's, copied after  */
		memcpy(grp + 12, param + 4, 4);
	}
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

/* -------------------------------------------------------------- the maps -- */

static const char *const MAPS[] = {
	"start", "e1m1", "e1m2", "e1m3", "e1m4", "e1m5", "e1m6", "e1m7", "e1m8",
	"e2m1", "e2m2", "e2m3", "e2m4", "e2m5", "e2m6", "e2m7",
	"e3m1", "e3m2", "e3m3", "e3m4", "e3m5", "e3m6", "e3m7",
	"e4m1", "e4m2", "e4m3", "e4m4", "e4m5", "e4m6", "e4m7", "e4m8", "end",
};

typedef struct {
	const uint8_t *data;          /* the pool file                         */
	const qst_asset *dir;
	int num;
} pool_t;

static int pool_find(const pool_t *p, uint32_t hash)
{
	int lo = 0, hi = p->num - 1;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		if (p->dir[mid].hash == hash)
			return mid;
		if (p->dir[mid].hash < hash) lo = mid + 1;
		else                        hi = mid - 1;
	}
	return -1;
}

/* the pool's assets the map uses (want[i] = 1); its CD track */
static int map_needs(const uint8_t *raw, uint32_t len, const pool_t *pool, uint8_t *want, FILE *log,
                     const char *name)
{
	bsp_t b;
	bsp_parse(&b, raw, len);
	ent_list l;
	ents_parse(&b, &l);
	int track = 0, worldtype = 0;
	for (int i = 0; i < l.num; i++)
		if (!strcmp(l.ents[i].classname, "worldspawn")) {
			track     = (int) ent_float(&l, &l.ents[i], "sounds", 0);
			worldtype = (int) ent_float(&l, &l.ents[i], "worldtype", 0);
		}
	/* the entities single player spawns at some skill (ED_LoadFromFile: not
	   one flagged not easy, not medium and not hard: a deathmatch item) */
	ent_list sp = l;
	sp.ents = xmalloc((size_t) (l.num ? l.num : 1) * sizeof *sp.ents);
	sp.num = 0;
	for (int i = 0; i < l.num; i++)
		if (((int) ent_float(&l, &l.ents[i], "spawnflags", 0) & 0x700) != 0x700)
			sp.ents[sp.num++] = l.ents[i];
	const char *names[512];
	int n = stream_monster_needs(&sp, names, 512);
	n += items_streamed(&sp, worldtype, names + n, 512 - n);
	n += sound_map_streamed(&sp, worldtype, names + n, 512 - n);
	free(sp.ents);
	if (log)
		fprintf(log, "map %s track %d", name, track);
	for (int i = 0; i < n; i++) {
		int a = pool_find(pool, qs_hash(names[i]));
		if (a < 0)
			continue;                             /* (not a streamed asset: a bank's) */
		want[a] = 1;
		if (log)
			fprintf(log, " %08x", pool->dir[a].hash);
	}
	if (log)
		fprintf(log, "\n");
	ents_free(&l);
	return track;
}

int cmd_carousel(int argc, char **argv)
{
	const char *pak_path = NULL, *pak1_path = NULL, *pool_path = NULL, *music = NULL, *out_path = NULL;
	const char *needs_path = NULL, *maps_list = NULL;
	int track = 0;
	for (int i = 2; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--pak"))   pak_path   = argv[i + 1];
		if (!strcmp(argv[i], "--pak1"))  pak1_path  = argv[i + 1];
		if (!strcmp(argv[i], "--pool"))  pool_path  = argv[i + 1];
		if (!strcmp(argv[i], "--music")) music      = argv[i + 1];
		if (!strcmp(argv[i], "--track")) track      = atoi(argv[i + 1]);
		if (!strcmp(argv[i], "--out"))   out_path   = argv[i + 1];
		if (!strcmp(argv[i], "--needs")) needs_path = argv[i + 1];
		if (!strcmp(argv[i], "--maps"))  maps_list  = argv[i + 1];
	}
	if (!pak_path || !pool_path || !music || !out_path || track < 2 || track > 99)
		die("usage: qcook carousel --pak <pak0.pak> [--pak1 <pak1.pak>] --pool <pool.dat> --music <dir> "
		    "--track <n> --out <xa_nn.str> [--maps \"start e1m1 ...\"] [--needs <list.txt>]");

	long pak_len, pak1_len = 0, pool_len;
	uint8_t *pak = read_file(pak_path, &pak_len), *pak1 = pak1_path ? read_file(pak1_path, &pak1_len) : NULL;
	uint8_t *pf = read_file(pool_path, &pool_len);
	const qst_header *ph = (const qst_header *) pf;
	if (pool_len < (long) sizeof *ph || memcmp(ph->magic, QSX_MAGIC, 4) ||
	    ph->ofs_assets + ph->num_assets * sizeof(qst_asset) > (uint64_t) pool_len)
		die("%s: not a pool of shared assets", pool_path);
	pool_t pool = { pf, (const qst_asset *) (pf + ph->ofs_assets), (int) ph->num_assets };

	/* the maps playing the track: their assets */
	uint8_t *want = xcalloc((size_t) pool.num, 1);
	FILE *log = needs_path ? fopen(needs_path, "w") : NULL;
	char maps[256] = "";
	for (size_t m = 0; m < sizeof MAPS / sizeof MAPS[0]; m++) {
		if (maps_list) {                          /* the disc's maps only */
			const char *at = maps_list;
			size_t len = strlen(MAPS[m]);
			int found = 0;
			while ((at = strstr(at, MAPS[m])) && !found) {
				found = (at == maps_list || at[-1] == ' ') && (at[len] == 0 || at[len] == ' ');
				at += len;
			}
			if (!found)
				continue;
		}
		char path[64];
		snprintf(path, sizeof path, "maps/%s.bsp", MAPS[m]);
		uint32_t len;
		const uint8_t *raw = pak_find(pak, pak_len, path, &len);
		if (!raw && pak1)
			raw = pak_find(pak1, pak1_len, path, &len);
		if (!raw)
			continue;
		uint8_t *mine = xcalloc((size_t) pool.num, 1);
		if (map_needs(raw, len, &pool, mine, log, MAPS[m]) == track) {
			for (int a = 0; a < pool.num; a++)
				want[a] |= mine[a];
			snprintf(maps + strlen(maps), sizeof maps - strlen(maps), " %s", MAPS[m]);
		}
		free(mine);
	}
	if (log)
		fclose(log);

	/* the turn: the assets in the pool's order (a monster's together) */
	int order[QXA_MAX_ASSETS], na = 0;
	for (int a = 0; a < pool.num; a++) {
		if (!want[a])
			continue;
		if (na == QXA_MAX_ASSETS)
			die("track %d: more than %d streamed assets", track, QXA_MAX_ASSETS);
		order[na++] = a;
	}
	for (int i = 1; i < na; i++)
		for (int j = i; j > 0 && pool.dir[order[j - 1]].sector > pool.dir[order[j]].sector; j--) {
			int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
		}
	qxa_entry *ent = xcalloc((size_t) (na ? na : 1), sizeof *ent);
	uint32_t period = 0;
	for (int i = 0; i < na; i++) {
		const qst_asset *d = &pool.dir[order[i]];
		ent[i].asset   = (uint16_t) order[i];      /* (the pool's order is every STREAM_<n>.DAT's) */
		ent[i].sectors = (uint16_t) ((d->size + QST_SECTOR - 1) / QST_SECTOR);
		ent[i].first   = period;
		period += ent[i].sectors;
	}
	uint8_t *lut = xcalloc(period ? period : 1, 1);
	uint8_t *turn = xcalloc((size_t) (period ? period : 1) * QST_SECTOR, 1);
	for (int i = 0; i < na; i++) {
		const qst_asset *d = &pool.dir[order[i]];
		memset(lut + ent[i].first, i, ent[i].sectors);
		memcpy(turn + (size_t) ent[i].first * QST_SECTOR, pf + (size_t) d->sector * QST_SECTOR, d->size);
	}

	/* the music */
	char wpath[512];
	snprintf(wpath, sizeof wpath, "%s/track%02d.wav", music, track);
	long wlen;
	uint8_t *wav = read_file(wpath, &wlen);
	uint32_t frames;
	const int16_t *pcm = wav_stereo(wav, wlen, wpath, QXA_RATE, &frames);
	uint32_t na_sec = (frames + QXA_SAMPLES - 1) / QXA_SAMPLES;

	/* the header */
	buf_t head = { 0 };
	qxa_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QXA_MAGIC, 4);
	h.track          = (uint16_t) track;
	h.stream_sectors = na_sec * QXA_INTERLEAVE;
	h.audio_end      = (na_sec - 1) * QXA_INTERLEAVE + 1;
	h.period         = period;
	h.num_assets     = (uint16_t) na;
	h.ofs_entries    = sizeof h;
	h.ofs_lut        = h.ofs_entries + (uint32_t) na * sizeof *ent;
	buf_put(&head, &h, sizeof h);
	buf_put(&head, ent, (size_t) na * sizeof *ent);
	buf_put(&head, lut, period);
	h.head_sectors = (uint16_t) ((head.len + QST_SECTOR - 1) / QST_SECTOR);
	if (h.head_sectors != 1)                     /* (engine/music.c reads one) */
		die("track %d: a carousel header of %u bytes, over a sector", track, (unsigned) head.len);
	memcpy(head.p, &h, sizeof h);

	/* the file: the header's sectors, then the stream */
	FILE *f = fopen(out_path, "wb");
	if (!f)
		die("cannot write %s", out_path);
	uint8_t sec[QXA_SECTOR];
	for (uint32_t s = 0; s < h.head_sectors; s++) {
		memset(sec, 0, sizeof sec);
		sec[0] = sec[4] = (uint8_t) track;
		sec[2] = sec[6] = QXA_SM_DATA;
		size_t at = (size_t) s * QST_SECTOR, n = head.len - at < QST_SECTOR ? head.len - at : QST_SECTOR;
		memcpy(sec + 8, head.p + at, n);
		fwrite(sec, 1, sizeof sec, f);
	}
	xa_chan ch[2] = { { 0, 0 }, { 0, 0 } };
	double err = 0, sig = 0;
	uint32_t audio = 0;
	for (uint32_t i = 0; i < h.stream_sectors; i++) {
		memset(sec, 0, sizeof sec);
		sec[0] = sec[4] = (uint8_t) track;
		if (i % QXA_INTERLEAVE == 0) {
			sec[2] = sec[6] = QXA_SM_RT | QXA_SM_FORM2 | QXA_SM_AUDIO;
			sec[3] = sec[7] = 0x01;                /* stereo, 37.8 kHz, 4-bit */
			uint32_t at = audio * QXA_SAMPLES;
			xa_sector(pcm + (size_t) at * 2, frames - at, ch, sec + 8, &err, &sig);
			audio++;
		} else {
			sec[2] = sec[6] = QXA_SM_DATA;
			uint32_t d = i - i / QXA_INTERLEAVE - 1;
			if (period)
				memcpy(sec + 8, turn + (size_t) (d % period) * QST_SECTOR, QST_SECTOR);
		}
		fwrite(sec, 1, sizeof sec, f);
	}
	fclose(f);
	uint32_t total = h.head_sectors + h.stream_sectors;
	printf("qcook: %s — track %d (%s), %.1f s of music at %.1f dB, %u sectors (%.1f MB); carousel: %d assets, "
	       "%u sectors (%u KB), a turn %.1f s\n", out_path, track, maps[0] ? maps + 1 : "no map",
	       frames / (double) QXA_RATE, err > 0 ? 10 * log10(sig / err) : 99.0, total,
	       total * 2352.0 / 1048576, na, period, period * 2, period / (75.0 * 3 / QXA_INTERLEAVE));
	free(head.p);
	free(wav);
	free(turn);
	free(lut);
	free(ent);
	free(want);
	free(pf);
	free(pak);
	free(pak1);
	return 0;
}

/* ------------------------------------------------------------ SPU music -- */

/* `qcook spumusic --music <dir> --track <n> --out <spu_nn.dat>`: the track
   (<dir>/trackNN.wav, 22.05 kHz 16-bit stereo) as SPU ADPCM, chunk by
   chunk, each channel encoded against its own decoder's history through
   the whole track (formats/qspu.h), as the XA is (xa_unit: the SPU's five
   filters, the residual rounded) */
int cmd_spumusic(int argc, char **argv)
{
	const char *music = NULL, *out_path = NULL;
	int track = 0;
	for (int i = 2; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--music")) music    = argv[i + 1];
		if (!strcmp(argv[i], "--track")) track    = atoi(argv[i + 1]);
		if (!strcmp(argv[i], "--out"))   out_path = argv[i + 1];
	}
	if (!music || !out_path || track < 2 || track > 99)
		die("usage: qcook spumusic --music <dir> --track <n> --out <spu_nn.dat>");
	char wpath[512];
	snprintf(wpath, sizeof wpath, "%s/track%02d.wav", music, track);
	long wlen;
	uint8_t *wav = read_file(wpath, &wlen);
	uint32_t frames;
	const int16_t *pcm = wav_stereo(wav, wlen, wpath, QSPU_RATE, &frames);
	uint32_t chunks = (frames + QSPU_SAMPLES - 1) / QSPU_SAMPLES;

	FILE *f = fopen(out_path, "wb");
	if (!f)
		die("cannot write %s", out_path);
	uint8_t sec[2048] = { 0 };
	qspu_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QSPU_MAGIC, 4);
	h.track  = (uint16_t) track;
	h.rate   = QSPU_RATE;
	h.chunks = chunks;
	memcpy(sec, &h, sizeof h);
	fwrite(sec, 1, sizeof sec, f);
	xa_chan ch[2] = { { 0, 0 }, { 0, 0 } };
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
				err += xa_unit(x, 1, &ch[c], &blk[0], nib, 5);
				blk[1] = 0;                           /* (no flags: the engine's) */
				for (int i = 0; i < 14; i++)
					blk[2 + i] = (uint8_t) (nib[2 * i] | nib[2 * i + 1] << 4);
			}
		fwrite(chunk, 1, sizeof chunk, f);
	}
	fclose(f);
	printf("qcook: %s — track %d, %.1f s at %d Hz, %.1f dB, %u chunks, %.1f MB\n", out_path, track,
	       frames / (double) QSPU_RATE, QSPU_RATE, err > 0 ? 10 * log10(sig / err) : 99.0, chunks,
	       (1 + chunks * QSPU_CHUNK_SECTORS) * 2048.0 / 1048576);
	free(wav);
	return 0;
}
