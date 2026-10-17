/*
 * q1psx — `qcook map --sounds`: the map's sound bank (formats/qsound.h).
 *
 * The sound set: Quake's always-precached list (world.qc worldspawn,
 * weapons.qc W_Precache, the menu's) with the groups the map cannot use left
 * out, the ambient sounds its entities spawn with (misc.qc ambientsound
 * calls, triggers.qc's teleporter hum) and the area ambients the BSP's leaf
 * levels can raise (snd_dma.c ambient_sfx: water1, wind2). Each WAV is read
 * as Quake's snd_mem.c GetWavinfo does (8 or 16 bits, mono; the cue point
 * and the LIST "mark" length give the loop) and encoded to SPU ADPCM.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qsound.h"

/* ----------------------------------------------------------------- sets -- */

enum { SG_CORE, SG_WATER, SG_LAVA, SG_SLIME, SG_SKY, SG_TELE, SG_ITEMS, SG_BOSS, SG_COUNT };
static const char *const group_name[SG_COUNT] = { "core", "water", "lava", "slime", "sky", "teleporters", "items", "boss" };

typedef struct { const char *name; int group; } sound_spec;

/* world.qc worldspawn + weapons.qc W_Precache + the menu (menu.c) */
static const sound_spec ALWAYS[] = {
	{ "demon/dland2.wav",   SG_CORE },    /* landing thud                        */
	{ "misc/h2ohit1.wav",   SG_WATER },   /* landing splash                      */
	{ "items/itembk2.wav",  SG_ITEMS },   /* item respawn                        */
	{ "player/plyrjmp8.wav", SG_CORE },
	{ "player/land.wav",    SG_CORE },
	{ "player/land2.wav",   SG_CORE },
	{ "player/drown1.wav",  SG_WATER },
	{ "player/drown2.wav",  SG_WATER },
	{ "player/gasp1.wav",   SG_WATER },
	{ "player/gasp2.wav",   SG_WATER },
	{ "player/h2odeath.wav", SG_WATER },
	{ "misc/talk.wav",      SG_CORE },
	{ "player/teledth1.wav", SG_TELE },
	{ "misc/r_tele1.wav",   SG_TELE },
	{ "misc/r_tele2.wav",   SG_TELE },
	{ "misc/r_tele3.wav",   SG_TELE },
	{ "misc/r_tele4.wav",   SG_TELE },
	{ "misc/r_tele5.wav",   SG_TELE },
	{ "weapons/lock4.wav",  SG_ITEMS },   /* ammo pick up                        */
	{ "weapons/pkup.wav",   SG_ITEMS },   /* weapon up                           */
	{ "items/armor1.wav",   SG_ITEMS },
	{ "weapons/lhit.wav",   SG_CORE },
	{ "weapons/lstart.wav", SG_CORE },
	{ "items/damage3.wav",  SG_ITEMS },
	{ "misc/power.wav",     SG_BOSS },
	{ "player/gib.wav",     SG_CORE },
	{ "player/udeath.wav",  SG_CORE },
	{ "player/tornoff2.wav", SG_CORE },
	{ "player/pain1.wav",   SG_CORE },
	{ "player/pain2.wav",   SG_CORE },
	{ "player/pain3.wav",   SG_CORE },
	{ "player/pain4.wav",   SG_CORE },
	{ "player/pain5.wav",   SG_CORE },
	{ "player/pain6.wav",   SG_CORE },
	{ "player/death1.wav",  SG_CORE },
	{ "player/death2.wav",  SG_CORE },
	{ "player/death3.wav",  SG_CORE },
	{ "player/death4.wav",  SG_CORE },
	{ "player/death5.wav",  SG_CORE },
	{ "weapons/ax1.wav",    SG_CORE },
	{ "player/axhit1.wav",  SG_CORE },
	{ "player/axhit2.wav",  SG_CORE },
	{ "player/h2ojump.wav", SG_WATER },
	{ "player/slimbrn2.wav", SG_SLIME },
	{ "player/inh2o.wav",   SG_WATER },
	{ "player/inlava.wav",  SG_LAVA },
	{ "misc/outwater.wav",  SG_WATER },
	{ "player/lburn1.wav",  SG_LAVA },    /* (slime pain too: lava or slime)     */
	{ "player/lburn2.wav",  SG_LAVA },
	{ "misc/water1.wav",    SG_WATER },   /* swimming                            */
	{ "misc/water2.wav",    SG_WATER },
	{ "weapons/r_exp3.wav", SG_CORE },
	{ "weapons/rocket1i.wav", SG_CORE },
	{ "weapons/sgun1.wav",  SG_CORE },
	{ "weapons/guncock.wav", SG_CORE },
	{ "weapons/ric1.wav",   SG_CORE },
	{ "weapons/ric2.wav",   SG_CORE },
	{ "weapons/ric3.wav",   SG_CORE },
	{ "weapons/spike2.wav", SG_CORE },
	{ "weapons/tink1.wav",  SG_CORE },
	{ "weapons/grenade.wav", SG_CORE },
	{ "weapons/bounce.wav", SG_CORE },
	{ "weapons/shotgn2.wav", SG_CORE },
	{ "misc/menu1.wav",     SG_CORE },
	{ "misc/menu2.wav",     SG_CORE },
	{ "misc/menu3.wav",     SG_CORE },
	{ "ambience/water1.wav", SG_WATER },  /* the area ambients (snd_dma.c)       */
	{ "ambience/wind2.wav", SG_SKY },
};

/* the entities that call ambientsound() when they spawn: misc.qc, and
   triggers.qc's trigger_teleport (unless SILENT) */
typedef struct { const char *classname; const char *sound; double vol, atten; } ambient_spec;
static const ambient_spec AMBIENTS[] = {
	{ "ambient_suck_wind",           "ambience/suck1.wav",    1,   3 },
	{ "ambient_drone",               "ambience/drone6.wav",   0.5, 3 },
	{ "ambient_flouro_buzz",         "ambience/buzz1.wav",    1,   3 },
	{ "ambient_drip",                "ambience/drip1.wav",    0.5, 3 },
	{ "ambient_comp_hum",            "ambience/comp1.wav",    1,   3 },
	{ "ambient_thunder",             "ambience/thunder1.wav", 0.5, 3 },
	{ "ambient_light_buzz",          "ambience/fl_hum1.wav",  0.5, 3 },
	{ "ambient_swamp1",              "ambience/swamp1.wav",   0.5, 3 },
	{ "ambient_swamp2",              "ambience/swamp2.wav",   0.5, 3 },
	{ "light_fluoro",                "ambience/fl_hum1.wav",  0.5, 3 },
	{ "light_fluorospark",           "ambience/buzz1.wav",    0.5, 3 },
	{ "light_globe",                 "ambience/fire1.wav",    0.5, 3 },
	{ "light_torch_small_walltorch", "ambience/fire1.wav",    0.5, 3 },
	{ "light_flame_large_yellow",    "ambience/fire1.wav",    0.5, 3 },
	{ "light_flame_small_yellow",    "ambience/fire1.wav",    0.5, 3 },
	{ "light_flame_small_white",     "ambience/fire1.wav",    0.5, 3 },
	{ "trigger_teleport",            "ambience/hum1.wav",     0.5, 3 },
};

/* ------------------------------------------------------------- entities -- */

typedef struct {
	char   classname[64];
	double origin[3];
	int    have_origin;
	int    spawnflags, sounds;
} ent_t;

/* the next entity of the lump at *pp; 0 at the end */
static int ent_next(const char **pp, const char *end, ent_t *e)
{
	const char *p = *pp;
	while (p < end && *p != '{') p++;
	if (p >= end) return 0;
	p++;
	memset(e, 0, sizeof *e);
	while (p < end && *p != '}') {
		char key[64], val[256];
		int i;
		while (p < end && *p != '"' && *p != '}') p++;
		if (p >= end || *p == '}') break;
		for (i = 0, p++; p < end && *p != '"' && i < 63; i++) key[i] = *p++;
		key[i] = 0;
		if (p < end) p++;
		while (p < end && *p != '"' && *p != '}') p++;
		if (p >= end || *p == '}') break;
		for (i = 0, p++; p < end && *p != '"' && i < 255; i++) val[i] = *p++;
		val[i] = 0;
		if (p < end) p++;
		if (!strcmp(key, "classname")) snprintf(e->classname, sizeof e->classname, "%s", val);
		else if (!strcmp(key, "origin"))
			e->have_origin = sscanf(val, "%lf %lf %lf", &e->origin[0], &e->origin[1], &e->origin[2]) == 3;
		else if (!strcmp(key, "spawnflags")) e->spawnflags = atoi(val);
		else if (!strcmp(key, "sounds")) e->sounds = atoi(val);
	}
	if (p < end) p++;
	*pp = p;
	return 1;
}

/* ------------------------------------------------------------------ wav -- */

typedef struct {
	int16_t *s;          /* mono samples                                   */
	int      n;          /* samples (the loop's end when it has one)       */
	int      rate;
	int      loopstart;  /* -1: none                                       */
} wav_t;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }

static const uint8_t *find_chunk(const uint8_t *p, const uint8_t *end, const char *id, uint32_t *len)
{
	while (end - p >= 8) {
		uint32_t n = rd32(p + 4);
		if (!memcmp(p, id, 4)) {
			if (n > (uint32_t) (end - p - 8)) n = (uint32_t) (end - p - 8);
			*len = n;
			return p + 8;
		}
		p += 8 + ((n + 1) & ~1u);
	}
	return NULL;
}

/* snd_mem.c GetWavinfo: the cue point is the loop start; a LIST chunk right
   after it with a "mark" gives the loop's length, else the loop runs to the
   end of the data */
static void wav_load(const uint8_t *d, uint32_t len, const char *name, wav_t *w)
{
	const uint8_t *end = d + len;
	if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4))
		die("%s: not a WAV", name);
	uint32_t n;
	const uint8_t *fmt = find_chunk(d + 12, end, "fmt ", &n);
	if (!fmt || n < 16) die("%s: no fmt chunk", name);
	int format = rd16(fmt), channels = rd16(fmt + 2), bits = rd16(fmt + 14);
	w->rate = (int) rd32(fmt + 4);
	if (format != 1 || channels != 1 || (bits != 8 && bits != 16))
		die("%s: not 8/16-bit mono PCM", name);
	const uint8_t *data = find_chunk(d + 12, end, "data", &n);
	if (!data) die("%s: no data chunk", name);
	int samples = (int) (n / (bits / 8));
	w->loopstart = -1;
	uint32_t cn;
	const uint8_t *cue = find_chunk(d + 12, end, "cue ", &cn);
	if (cue && cn >= 28) {
		/* the first cue point's sample offset (GetWavinfo: data_p += 32 from
		   the chunk id); then a LIST chunk after it whose "mark" purpose id
		   is at +28 from its id carries the loop's length at +24 */
		w->loopstart = (int) rd32(cue + 24);
		uint32_t ln;
		const uint8_t *list = find_chunk(cue + ((cn + 1) & ~1u), end, "LIST", &ln);
		if (list && ln >= 24 && !memcmp(list + 20, "mark", 4))
			samples = w->loopstart + (int) rd32(list + 16);
	}
	if (w->loopstart >= samples) w->loopstart = -1;
	if (samples > (int) (n / (bits / 8))) samples = (int) (n / (bits / 8));
	w->n = samples;
	w->s = xmalloc((size_t) samples * sizeof *w->s);
	for (int i = 0; i < samples; i++)
		w->s[i] = bits == 8 ? (int16_t) ((data[i] - 128) << 8) : (int16_t) rd16(data + 2 * i);
}

/* halve the sample rate: a 9-tap half-band filter, then every other sample */
static void wav_halve(wav_t *w)
{
	static const double k[9] = { -0.0208, 0, 0.1178, 0.25, 0.3061, 0.25, 0.1178, 0, -0.0208 };
	int n2 = (w->n + 1) / 2;
	int16_t *o = xmalloc((size_t) n2 * sizeof *o);
	for (int i = 0; i < n2; i++) {
		double a = 0;
		for (int t = 0; t < 9; t++) {
			int j = 2 * i + t - 4;
			if (j < 0) j = 0;
			if (j >= w->n) j = w->n - 1;
			a += k[t] * w->s[j];
		}
		if (a > 32767) a = 32767;
		if (a < -32768) a = -32768;
		o[i] = (int16_t) lround(a);
	}
	free(w->s);
	w->s = o;
	w->n = n2;
	w->rate /= 2;
	if (w->loopstart >= 0) w->loopstart /= 2;
}

/* ---------------------------------------------------------------- adpcm -- */

static const int K0[5] = { 0, 60, 115, 98, 122 };
static const int K1[5] = { 0, 0, -52, -55, -60 };

typedef struct { uint8_t *p; size_t len, cap; } bytes_t;

static void bput(bytes_t *b, const void *d, size_t n)
{
	if (b->len + n > b->cap) {
		b->cap = (b->len + n) * 2 + 1024;
		b->p = realloc(b->p, b->cap);
		if (!b->p) die("out of memory");
	}
	memcpy(b->p + b->len, d, n);
	b->len += n;
}

/* one block: the filter and shift with the least squared error, encoding
   with the decoder's own history (s1, s2) so the error never accumulates */
static void adpcm_block(const int16_t *s, int n, int *s1, int *s2, uint8_t flags, uint8_t out[16])
{
	double best = 1e30;
	int bf = 0, bs = 0;
	uint8_t bn[28];
	for (int f = 0; f < 5; f++)
		for (int sh = 0; sh < 13; sh++) {
			int h1 = *s1, h2 = *s2;
			double err = 0;
			uint8_t nib[28];
			for (int i = 0; i < 28; i++) {
				int x = i < n ? s[i] : 0;
				int pred = (h1 * K0[f] + h2 * K1[f]) >> 6;
				int d = x - pred;
				int q = (d << sh) >> 12;                 /* the residual in 4 bits */
				if (q > 7) q = 7;
				if (q < -8) q = -8;
				nib[i] = (uint8_t) (q & 15);
				int r = pred + ((q << 12) >> sh);        /* what the SPU will play */
				if (r > 32767) r = 32767;
				if (r < -32768) r = -32768;
				double e = r - x;
				err += e * e;
				h2 = h1;
				h1 = r;
			}
			if (err < best) {
				best = err;
				bf = f;
				bs = sh;
				memcpy(bn, nib, 28);
			}
		}
	out[0] = (uint8_t) (bs | bf << 4);
	out[1] = flags;
	for (int i = 0; i < 14; i++)
		out[2 + i] = (uint8_t) (bn[2 * i] | bn[2 * i + 1] << 4);
	/* the decoder's history after the block */
	for (int i = 0; i < 28; i++) {
		int q = (int8_t) (bn[i] << 4) >> 4;
		int pred = (*s1 * K0[bf] + *s2 * K1[bf]) >> 6;
		int r = pred + ((q << 12) >> bs);
		if (r > 32767) r = 32767;
		if (r < -32768) r = -32768;
		*s2 = *s1;
		*s1 = r;
	}
}

/* the whole sound; a looping one has its loop start moved to a block
   boundary (silence before it) and the loop's last block filled with the
   loop's first samples (the loop then runs at most 27 samples long) */
static void adpcm_encode(const wav_t *w, bytes_t *out, double *snr)
{
	int pre = 0;
	if (w->loopstart >= 0)
		pre = (28 - w->loopstart % 28) % 28;
	int total = pre + w->n, nblocks = (total + 27) / 28;
	int16_t *s = xcalloc((size_t) nblocks * 28, sizeof *s);
	memcpy(s + pre, w->s, (size_t) w->n * sizeof *s);
	if (w->loopstart >= 0)                            /* the tail: the loop again */
		for (int i = total; i < nblocks * 28; i++)
			s[i] = s[pre + w->loopstart + (i - total) % (w->n - w->loopstart)];
	int loop_block = w->loopstart >= 0 ? (pre + w->loopstart) / 28 : -1;
	int s1 = 0, s2 = 0;
	double err = 0, sig = 0;
	for (int b = 0; b < nblocks; b++) {
		uint8_t flags = 0, blk[16];
		if (b == loop_block) flags |= 4;                 /* loop start */
		if (b == nblocks - 1) flags |= loop_block >= 0 ? 3 : 1;   /* end (+ repeat) */
		int h1 = s1, h2 = s2;
		adpcm_block(s + b * 28, 28, &s1, &s2, flags, blk);
		bput(out, blk, 16);
		/* the error of this block, from its decoded samples */
		for (int i = 0; i < 28; i++) {
			int q = (int8_t) (blk[2 + i / 2] << (i & 1 ? 0 : 4)) >> 4;
			int f = blk[0] >> 4, sh = blk[0] & 15;
			int pred = (h1 * K0[f] + h2 * K1[f]) >> 6;
			int r = pred + ((q << 12) >> sh);
			if (r > 32767) r = 32767;
			if (r < -32768) r = -32768;
			double e = r - s[b * 28 + i];
			err += e * e;
			sig += (double) s[b * 28 + i] * s[b * 28 + i];
			h2 = h1;
			h1 = r;
		}
	}
	*snr = err > 0 ? 10 * log10(sig / err) : 99;
	free(s);
}

/* ----------------------------------------------------------------- bank -- */

typedef struct {
	char     name[64];
	int      group;      /* SG_*, or -1 for an entity's ambient */
	wav_t    wav;
	int      halved;
	bytes_t  adpcm;
	double   snr;
} bank_sound;

static bank_sound sounds[QS_MAX_SOUNDS];
static int        nsounds;

static int sound_find(const char *name)
{
	for (int i = 0; i < nsounds; i++)
		if (!strcmp(sounds[i].name, name))
			return i;
	return -1;
}

static int sound_add(const char *name, int group, const uint8_t *pak, long pak_len,
                     const uint8_t *pak0, long pak0_len)
{
	int i = sound_find(name);
	if (i >= 0) return i;
	if (nsounds >= QS_MAX_SOUNDS) die("%s: too many sounds", name);
	char path[80];
	snprintf(path, sizeof path, "sound/%s", name);
	uint32_t len;
	const uint8_t *d = pak_find(pak, pak_len, path, &len);
	if (!d && pak0) d = pak_find(pak0, pak0_len, path, &len);
	if (!d) die("%s: not in the paks", path);
	bank_sound *s = &sounds[nsounds];
	memset(s, 0, sizeof *s);
	snprintf(s->name, sizeof s->name, "%s", name);
	s->group = group;
	wav_load(d, len, path, &s->wav);
	return nsounds++;
}

static size_t sound_bytes(const bank_sound *s)
{
	int pre = s->wav.loopstart >= 0 ? (28 - s->wav.loopstart % 28) % 28 : 0;
	return (size_t) ((pre + s->wav.n + 27) / 28) * 16;
}

void sound_build(const bsp_t *b, const uint8_t *pak, long pak_len, const uint8_t *pak0,
                 long pak0_len, const char *out_path)
{
	nsounds = 0;

	/* what the map has */
	int have[SG_COUNT] = { 0 };
	have[SG_CORE] = 1;
	for (int i = 0; i < b->num_leaves; i++) {
		int c = b->leaves[i].contents;
		if (c == -3) have[SG_WATER] = 1;
		if (c == -4) have[SG_SLIME] = 1;
		if (c == -5) have[SG_LAVA] = 1;
		if (b->leaves[i].ambient[1]) have[SG_SKY] = 1;
		if (b->leaves[i].ambient[0]) have[SG_WATER] = 1;
	}
	int cd_track = 0;
	qs_static statics[QS_MAX_STATICS];
	int nstatics = 0;
	ent_t e;
	const char *p = b->entities, *end = p + b->ent_len;
	while (ent_next(&p, end, &e)) {
		if (!strcmp(e.classname, "worldspawn")) cd_track = e.sounds;
		if (!strncmp(e.classname, "item_", 5) || !strncmp(e.classname, "weapon_", 7)) have[SG_ITEMS] = 1;
		if (!strcmp(e.classname, "trigger_teleport") || !strcmp(e.classname, "info_teleport_destination"))
			have[SG_TELE] = 1;
		if (!strcmp(e.classname, "monster_boss")) have[SG_BOSS] = 1;
		for (size_t k = 0; k < sizeof AMBIENTS / sizeof AMBIENTS[0]; k++) {
			const ambient_spec *a = &AMBIENTS[k];
			if (strcmp(e.classname, a->classname)) continue;
			if (!strcmp(a->classname, "trigger_teleport") && (e.spawnflags & 1)) continue;   /* SILENT */
			if (!e.have_origin) continue;          /* (a brush entity: no origin; its hum
			                                           needs the brush centre: Phase 7) */
			if (nstatics >= QS_MAX_STATICS) die("too many static sounds");
			qs_static *st = &statics[nstatics++];
			for (int i = 0; i < 3; i++) st->origin[i] = (int32_t) lround(e.origin[i]);
			st->sound  = (uint16_t) sound_add(a->sound, -1, pak, pak_len, pak0, pak0_len);
			st->volume = (uint8_t) lround(a->vol * 255);
			st->atten  = (uint8_t) lround(a->atten * 64);
		}
	}
	for (size_t k = 0; k < sizeof ALWAYS / sizeof ALWAYS[0]; k++)
		if (have[ALWAYS[k].group])
			sound_add(ALWAYS[k].name, ALWAYS[k].group, pak, pak_len, pak0, pak0_len);

	/* the budget: halve the longest looping ambients until it fits */
	size_t total = 0;
	for (int i = 0; i < nsounds; i++) total += sound_bytes(&sounds[i]);
	int halved = 0;
	while (total > QS_SPU_BYTES) {
		int best = -1;
		for (int i = 0; i < nsounds; i++)
			if (sounds[i].wav.loopstart >= 0 && !sounds[i].halved &&
			    (best < 0 || sounds[i].wav.n > sounds[best].wav.n))
				best = i;
		if (best < 0)
			die("the sounds do not fit the SPU (%zu KB of %u KB)", total / 1024, QS_SPU_BYTES / 1024);
		total -= sound_bytes(&sounds[best]);
		wav_halve(&sounds[best].wav);
		sounds[best].halved = 1;
		total += sound_bytes(&sounds[best]);
		halved++;
	}

	/* encode */
	double worst_snr = 99;
	const char *worst = "";
	for (int i = 0; i < nsounds; i++) {
		adpcm_encode(&sounds[i].wav, &sounds[i].adpcm, &sounds[i].snr);
		if (sounds[i].snr < worst_snr) { worst_snr = sounds[i].snr; worst = sounds[i].name; }
	}

	/* the file */
	bytes_t f = { 0 };
	qs_header h;
	memset(&h, 0, sizeof h);
	bput(&f, &h, sizeof h);
	qs_sound *dir = xcalloc((size_t) nsounds, sizeof *dir);
	size_t data = 0;
	for (int i = 0; i < nsounds; i++) {
		bank_sound *s = &sounds[i];
		dir[i].hash  = qs_hash(s->name);
		dir[i].ofs   = (uint32_t) data;
		dir[i].len   = (uint32_t) s->adpcm.len;
		dir[i].pitch = (uint16_t) ((s->wav.rate * 4096 + 22050) / 44100);
		dir[i].flags = s->wav.loopstart >= 0 ? QSF_LOOP : 0;
		dir[i].ticks = (uint32_t) ((s->wav.n * 60 + s->wav.rate - 1) / s->wav.rate) + 1;
		data += s->adpcm.len;
	}
	h.num_sounds = (uint32_t) nsounds; h.ofs_sounds = (uint32_t) f.len;
	bput(&f, dir, (size_t) nsounds * sizeof *dir);
	h.num_statics = (uint32_t) nstatics; h.ofs_statics = (uint32_t) f.len;
	bput(&f, statics, (size_t) nstatics * sizeof *statics);
	h.ofs_names = (uint32_t) f.len;
	for (int i = 0; i < nsounds; i++) bput(&f, sounds[i].name, strlen(sounds[i].name) + 1);
	h.len_names = (uint32_t) f.len - h.ofs_names;
	while (f.len & 15) bput(&f, "", 1);
	h.ofs_data = (uint32_t) f.len;
	for (int i = 0; i < nsounds; i++) bput(&f, sounds[i].adpcm.p, sounds[i].adpcm.len);
	h.len_data = (uint32_t) data;
	h.cd_track = (uint32_t) cd_track;
	memcpy(h.magic, QSOUND_MAGIC, 4);
	memcpy(f.p, &h, sizeof h);
	FILE *fp = fopen(out_path, "wb");
	if (!fp || fwrite(f.p, 1, f.len, fp) != f.len) die("cannot write %s", out_path);
	fclose(fp);

	int per_group[SG_COUNT] = { 0 }, nambient = 0;
	for (int i = 0; i < nsounds; i++) {
		if (sounds[i].group >= 0) per_group[sounds[i].group]++;
		else nambient++;
	}
	printf("qcook: sounds — %d sounds, %zu KB of SPU RAM (%d halved), %d static sounds, CD track %d, worst SNR %.1f dB (%s)\n",
	       nsounds, data / 1024, halved, nstatics, cd_track, worst_snr, worst);
	printf("qcook: sounds — core %d", per_group[SG_CORE]);
	for (int g = 1; g < SG_COUNT; g++)
		if (per_group[g]) printf(", %s %d", group_name[g], per_group[g]);
	printf(", ambients %d\n", nambient);

	for (int i = 0; i < nsounds; i++) {
		free(sounds[i].wav.s);
		free(sounds[i].adpcm.p);
	}
	free(dir);
	free(f.p);
}
