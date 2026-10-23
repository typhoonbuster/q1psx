#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxspu.h>
#include <hwregs_c.h>

#include "cd.h"
#include "dlight.h"
#include "game.h"
#include "mem.h"
#include "music.h"
#include "q_fmt.h"
#include "qsound.h"
#include "render.h"
#include "snd.h"
#include "stream.h"
#include "world.h"

int snd_log;

/* ---------------------------------------------------------------- voices -- */

#define V_AMBIENT   0            /* water, wind                              */
#define V_DYNAMIC   2            /* MAX_DYNAMIC_CHANNELS                     */
#define NUM_DYNAMIC 8
#define V_STATIC    (V_DYNAMIC + NUM_DYNAMIC)
#define NUM_STATIC  (24 - V_STATIC - MUSIC_SPU_VOICES)   /* (the SPU music's two last) */
#define SND_VOICES  ((1u << (V_STATIC + NUM_STATIC)) - 1)   /* the sounds' voices */

#define CD_VOL      0x2ccc       /* bgmvolume 1: the CD as loud as it was under
                                    the old 0.7 master (the mix unchanged)  */
static int sfx_tenths = 7;       /* Quake's volume cvar, 0.7: the voices'    */
#define NOMINAL_CLIP_DIST 1000   /* snd_dma.c sound_nominal_clip_dist        */
#define CLIP_UNITS  (NOMINAL_CLIP_DIST * 64 * R_POS_SUB)   /* x dist_mult: the silence line, 1/R_POS_SUB units */
#define DIST_M(dist_mult) ((dist_mult) * 65536 / (CLIP_UNITS / 256))   /* dist x256 = len x DIST_M >> 16 */
#define AMBIENT_LEVEL 77         /* ambient_level 0.3, x256                  */
#define AMBIENT_FADE  427        /* ambient_fade 100 per second: per tick, x256 */
#define NEVER 0x7fffffff

typedef struct {
	int      sfx;                /* a sound (snd_find's), -1 free            */
	int32_t  origin[3];          /* 1/R_POS_SUB units                        */
	int      entnum, entchannel;
	int      master_vol;         /* 0..255                                   */
	int      dist_mult;          /* atten x64 (x 1 / 64000 per world unit)   */
	uint32_t reach;              /* the Manhattan distance from which it is silent */
	int      dist_m;             /* DIST_M(dist_mult)                        */
	int      leftvol, rightvol;  /* 0..255                                   */
	int      end;                /* tick it ends (NEVER: a loop)             */
	int      start_tick;         /* (the same-sound-this-frame offset)       */
} channel_t;

static channel_t dyn[NUM_DYNAMIC];

/* a static source as the per-frame loop takes it (snd_map_start makes
   them from the bank's qs_static entries, in place) */
typedef struct {
	int16_t  origin[3];          /* 1/R_POS_SUB units                        */
	uint8_t  voice;              /* V_STATIC-relative                        */
	uint8_t  volume;             /* 0..255                                   */
	uint16_t reach;              /* the Manhattan distance (1/R_POS_SUB units)
	                                from which it is silent; 0xffff: never   */
	uint16_t dist_m;             /* DIST_M(atten)                            */
} static_t;                      /* 12 bytes                                 */

/* the bank's directory, in the game arena (fixed slots) */
static qs_sound  *bank;
static int        bank_n;
static qs_static *statics;       /* the bank's, then static_t's (snd_map_start) */
static int        statics_n;     /* static_t's with a voice                   */
static int        cd_track;
static int         named[S_COUNT];          /* S_* -> bank index or -1     */

/* the static voices: one per distinct static sound, keyed on while its
   samples are in (a streamed sound's come and go) */
static int static_sfx[NUM_STATIC], static_n;
static int static_lvol[NUM_STATIC], static_rvol[NUM_STATIC];
static uint8_t static_on[NUM_STATIC];

/* the ambients */
static int amb_vol[2];                        /* master_vol x256, fading   */
static int amb_sfx[2];

static int32_t listener[3];
static int32_t listener_right[3];              /* x4096                     */
static int     snd_tick;                       /* the sound clock, ticks    */

static const char *const names[S_COUNT] = {
	"weapons/ax1.wav", "player/axhit2.wav", "weapons/guncock.wav",
	"weapons/shotgn2.wav", "weapons/rocket1i.wav", "weapons/spike2.wav", "weapons/grenade.wav",
	"weapons/bounce.wav", "weapons/sgun1.wav", "weapons/r_exp3.wav", "weapons/lstart.wav",
	"weapons/lhit.wav", "weapons/tink1.wav", "weapons/ric1.wav", "weapons/ric2.wav", "weapons/ric3.wav",
	"player/plyrjmp8.wav", "player/land.wav", "player/land2.wav", "player/h2ojump.wav",
	"player/pain1.wav", "player/pain2.wav", "player/pain3.wav", "player/pain4.wav", "player/pain5.wav",
	"player/pain6.wav",
	"player/death1.wav", "player/death2.wav", "player/death3.wav", "player/death4.wav",
	"player/death5.wav", "player/h2odeath.wav",
	"player/drown1.wav", "player/drown2.wav", "player/lburn1.wav", "player/lburn2.wav",
	"player/gasp1.wav", "player/gasp2.wav",
	"player/inh2o.wav", "player/inlava.wav", "player/slimbrn2.wav", "misc/outwater.wav",
	"misc/water1.wav", "misc/water2.wav", "misc/h2ohit1.wav",
	"player/gib.wav", "player/udeath.wav",
	"misc/r_tele1.wav", "misc/r_tele2.wav", "misc/r_tele3.wav", "misc/r_tele4.wav", "misc/r_tele5.wav",
	"misc/menu1.wav", "misc/menu2.wav", "misc/menu3.wav", "misc/talk.wav",
	"weapons/lock4.wav", "weapons/pkup.wav", "items/armor1.wav",
	"items/damage3.wav", "misc/power.wav", "demon/dland2.wav",
	"ambience/water1.wav", "ambience/wind2.wav",
	"misc/secret.wav", "misc/trigger1.wav", "ambience/windfly.wav", "ambience/hum1.wav",
};

/* ------------------------------------------------------------------- spu -- */

/* what each voice's volume registers were last set to, before the scale
   (-1: unknown): an unchanged voice is not written again (the SPU keeps
   them; the volumes are set for every voice every frame) */
static int16_t voice_in[24][2];

static void voice_set(int v, int left, int right)
{
	if (voice_in[v][0] == left && voice_in[v][1] == right)
		return;
	voice_in[v][0] = (int16_t) left;
	voice_in[v][1] = (int16_t) right;
	SPU_CH_VOL_L(v) = (uint16_t) ((left << 6) * sfx_tenths / 10);   /* 255 -> 0x3fc0 x volume */
	SPU_CH_VOL_R(v) = (uint16_t) ((right << 6) * sfx_tenths / 10);
}

void snd_set_volume(int tenths)
{
	sfx_tenths = tenths < 0 ? 0 : tenths > 10 ? 10 : tenths;
	memset(voice_in, 0xff, sizeof voice_in);         /* every voice again, scaled anew */
}

static int cd_tenths = 10;

static void cd_volume_set(void)
{
	int v = CD_VOL * cd_tenths / 10;
	SpuSetCommonCDVolume((int16_t) v, (int16_t) v);
}

void snd_set_cd_volume(int tenths)
{
	cd_tenths = tenths < 0 ? 0 : tenths > 10 ? 10 : tenths;
	cd_volume_set();
}

/* sound s's samples (a bank sound, or a streamed one: SND_STREAM + its
   asset): their SPU address and header, 0 when they are not in */
typedef struct { uint32_t addr, len, ticks; uint16_t pitch, flags; } sfx_t;

static int sfx_get(int s, sfx_t *o)
{
	if (s >= SND_STREAM) {
		const stream_sound_t *ss = stream_sound(s - SND_STREAM);
		if (!ss)
			return 0;
		o->addr = ss->spu;
		o->len = ss->len;
		o->ticks = ss->ticks;
		o->pitch = ss->pitch;
		o->flags = ss->flags;
		return 1;
	}
	if (s < 0 || s >= bank_n)
		return 0;
	const qs_sound *q = &bank[s];
	o->addr = QS_SPU_BASE + q->ofs;
	o->len = q->len;
	o->ticks = q->ticks;
	o->pitch = q->pitch;
	o->flags = q->flags;
	return 1;
}

/* key voice v on with sound q, `skip` ADPCM blocks in */
static void voice_start(int v, const sfx_t *q, int left, int right, int skip)
{
	uint32_t addr = q->addr + (uint32_t) skip * 16;
	SpuSetKey(0, 1u << v);
	voice_set(v, left, right);
	SPU_CH_FREQ(v)  = q->pitch;
	SPU_CH_ADDR(v)  = (uint16_t) (addr >> 3);
	SPU_CH_ADSR1(v) = 0x000f;                        /* at once to full */
	SPU_CH_ADSR2(v) = 0x0000;                        /* off at once on key-off / end */
	SpuSetKey(1, 1u << v);
}

Q_COLD void snd_init(void)
{
	SpuInit();
	memset(voice_in, 0xff, sizeof voice_in);
	SPU_CTRL = 0xc001;                               /* on, unmuted, CD audio in */
	SpuSetCommonMasterVolume(0x3fff, 0x3fff);
	SpuSetCommonCDVolume(CD_VOL, CD_VOL);
	SpuSetKey(0, 0xffffff);
	bank    = mem_game_alloc(QS_MAX_SOUNDS * sizeof *bank);
	statics = mem_game_alloc(QS_MAX_STATICS * sizeof *statics);
	for (int i = 0; i < S_COUNT; i++)
		named[i] = -1;
	for (int i = 0; i < NUM_DYNAMIC; i++)
		dyn[i].sfx = -1;
}

Q_COLD int snd_load_bank(const char *map)
{
	char name[24];
	q_snprintf(name, sizeof name, "\\MAPS\\%s.SND;1", map);
	bank_n = statics_n = cd_track = 0;
	for (int i = 0; i < S_COUNT; i++)
		named[i] = -1;
	mem_level_reset();                               /* the bank passes through the arena */
	uint32_t size;
	const uint8_t *f = cd_load(name, &size, NULL);
	if (!f || size < sizeof(qs_header) || memcmp(f, QSOUND_MAGIC, 4)) {
		printf("snd: no bank %s\n", name);
		return 0;
	}
	const qs_header *h = (const qs_header *) f;
	if (h->num_sounds > QS_MAX_SOUNDS || h->num_statics > QS_MAX_STATICS || h->len_data > QS_SPU_BYTES) {
		printf("snd: bank %s out of bounds\n", name);
		return 0;
	}
	bank_n = (int) h->num_sounds;
	memcpy(bank, f + h->ofs_sounds, bank_n * sizeof *bank);
	statics_n = (int) h->num_statics;
	memcpy(statics, f + h->ofs_statics, statics_n * sizeof *statics);
	cd_track = (int) h->cd_track;
	for (int i = 0; i < S_COUNT; i++) {
		int s = snd_find(names[i]);
		named[i] = s >= 0 ? s : -1;
	}
	SpuSetKey(0, SND_VOICES);
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(QS_SPU_BASE);
	SpuWrite((const uint32_t *) (f + h->ofs_data), (h->len_data + 63) & ~63u);
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
	stream_spu_pool(QS_SPU_BASE + ((h->len_data + 63) & ~63u),   /* the rest: the streamed sounds' */
	                512 * 1024 - MUSIC_SPU_BYTES);                /* (but the SPU music's ring) */
	printf("snd: %s — %d sounds, %u KB of SPU RAM, %d static sounds, CD track %d\n",
	       name, bank_n, (unsigned) h->len_data / 1024, statics_n, cd_track);
	mem_level_reset();
	return 1;
}

int snd_cd_track(void)
{
	return cd_track;
}

/* the sound of that name's hash: the bank's, else a streamed one's */
static int sound_of_hash(uint32_t h)
{
	for (int s = 0; s < bank_n; s++)
		if (bank[s].hash == h)
			return s;
	int a = stream_find_hash(h);
	if (a >= 0 && stream_is_sound(a))
		return SND_STREAM + a;
	return SND_NONE;
}

void snd_stop_all(void)
{
	SpuSetKey(0, SND_VOICES);
	for (int i = 0; i < NUM_DYNAMIC; i++)
		dyn[i].sfx = -1;
	for (int v = 0; v < 24; v++)
		voice_set(v, 0, 0);
	memset(static_on, 0, sizeof static_on);
	amb_vol[0] = amb_vol[1] = 0;
}

void snd_forget(int s)
{
	for (int i = 0; i < NUM_DYNAMIC; i++)
		if (dyn[i].sfx == s) {
			dyn[i].sfx = -1;
			SpuSetKey(0, 1u << (V_DYNAMIC + i));
		}
	for (int v = 0; v < static_n; v++)
		if (static_sfx[v] == s && static_on[v]) {
			static_on[v] = 0;
			SpuSetKey(0, 1u << (V_STATIC + v));
		}
}

/* the static sounds: a looping voice per distinct sound, silent until the
   listener comes near; the ambients' voices too (S_StopAllSounds, then
   S_StaticSound for each) */
Q_COLD void snd_map_start(void)
{
	snd_stop_all();
	static_n = 0;
	static_t *rt = (static_t *) statics;     /* (12 bytes over 16: in place, forwards) */
	int n = 0;
	for (int i = 0; i < statics_n; i++) {
		qs_static st = statics[i];
		int s = sound_of_hash(st.hash), v;
		if (s < 0)
			continue;
		for (v = 0; v < static_n && static_sfx[v] != s; v++)
			;
		if (v == static_n) {
			if (static_n >= NUM_STATIC)
				continue;                        /* (no voice left: never heard) */
			static_sfx[static_n++] = s;
			static_on[v] = 0;                    /* (keyed once its samples are in) */
		}
		static_t *r = &rt[n++];
		for (int k = 0; k < 3; k++) {
			int32_t o = st.origin[k] * R_POS_SUB;
			r->origin[k] = (int16_t) (o > 32767 ? 32767 : o < -32768 ? -32768 : o);
		}
		r->voice  = (uint8_t) v;
		r->volume = st.volume;
		r->reach  = st.atten ? (uint16_t) (CLIP_UNITS / st.atten) : 0xffff;
		r->dist_m = (uint16_t) DIST_M(st.atten);
	}
	statics_n = n;
	amb_sfx[0] = named[S_AMB_WATER1];
	amb_sfx[1] = named[S_AMB_WIND2];
	for (int a = 0; a < 2; a++) {
		sfx_t q;
		if (amb_sfx[a] >= 0 && sfx_get(amb_sfx[a], &q))
			voice_start(V_AMBIENT + a, &q, 0, 0, 0);
	}
}

/* ----------------------------------------------------------- spatialize -- */

/* sqrt(x) within +-1 for x < 2^31, from dl_sqrt (sqrt of i << 11, i <
   1024): x shifted by an even count into [2^19, 2^21), its table entry
   within 1/512, one Newton step, the count shifted back */
static uint32_t isqrt_fast(uint32_t x)
{
	int k = 0;
	if (x < 1u << 19) {
		if (x < 2)
			return x;
		while (x < 1u << 19) { x <<= 2; k--; }
	} else {
		if (x >= 1u << 27) { x >>= 8; k = 4; }
		if (x >= 1u << 23) { x >>= 4; k += 2; }
		if (x >= 1u << 21) { x >>= 2; k += 1; }
	}
	uint32_t r = dl_sqrt[x >> 11];
	r = (r + x / r) >> 1;
	return k >= 0 ? r << k : r >> -k;
}

/* SND_Spatialize for a source d (1/R_POS_SUB units) off the listener,
   whose Manhattan distance is under its reach: the distance attenuation
   dist = len x dist_mult / 1000 units (x256 as len x dist_m >> 16), the
   stereo split from the listener's right vector; volumes 0..255 */
static inline void spatialize_d(int32_t dx, int32_t dy, int32_t dz, int master_vol, int dist_m,
                                int *leftvol, int *rightvol)
{
	int32_t len = (int32_t) isqrt_fast((uint32_t) (dx * dx + dy * dy + dz * dz));
	int scale = 256 - ((len * dist_m) >> 16);        /* (1 - dist) x256 */
	int dot = len ? (listener_right[0] * dx + listener_right[1] * dy +
	                 listener_right[2] * dz) / len : 0;   /* x4096 (the sum < 2^31) */
	int vs = master_vol * scale;
	int r = vs * (4096 + dot) >> 20;
	int l = vs * (4096 - dot) >> 20;
	*rightvol = r < 0 ? 0 : r > 255 ? 255 : r;
	*leftvol  = l < 0 ? 0 : l > 255 ? 255 : l;
}

/* a channel's: the view entity is always full volume; a source past its
   reach is silent (the Manhattan distance bounds the real one, so this is
   exact); the rest within 2048 units of the listener */
static void spatialize(int entnum, const int32_t origin[3], int master_vol, uint32_t reach, int dist_m,
                       int *leftvol, int *rightvol)
{
	if (entnum == ENT_PLAYER) {
		*leftvol = *rightvol = master_vol;
		return;
	}
	int32_t d[3];
	uint32_t manhattan = 0;
	for (int k = 0; k < 3; k++) {
		d[k] = origin[k] - listener[k];
		if (d[k] > 16383) d[k] = 16383;              /* 2048 units: beyond hearing */
		if (d[k] < -16383) d[k] = -16383;
		manhattan += (uint32_t) (d[k] < 0 ? -d[k] : d[k]);
	}
	if (manhattan >= reach) {
		*leftvol = *rightvol = 0;
		return;
	}
	spatialize_d(d[0], d[1], d[2], master_vol, dist_m, leftvol, rightvol);
}

/* ----------------------------------------------------------------- start -- */

/* SND_PickChannel */
static int pick_channel(int entnum, int entchannel)
{
	int first_to_die = -1, life_left = NEVER;
	for (int i = 0; i < NUM_DYNAMIC; i++) {
		channel_t *ch = &dyn[i];
		if (entchannel != 0 && ch->entnum == entnum &&
		    (ch->entchannel == entchannel || entchannel == -1)) {
			first_to_die = i;                        /* always override the same entity's */
			break;
		}
		if (ch->entnum == ENT_PLAYER && entnum != ENT_PLAYER && ch->sfx >= 0)
			continue;                                /* monsters never override the player */
		int life = ch->sfx < 0 ? -NEVER : ch->end - snd_tick;
		if (life < life_left) {
			life_left = life;
			first_to_die = i;
		}
	}
	return first_to_die;
}

int snd_find(const char *name)
{
	if (!strcmp(name, "misc/null.wav"))
		return SND_NULL;
	return sound_of_hash(qs_hash(name));
}

void snd_add_static(const int32_t origin[3], int s, int vol, int atten)
{
	if (s < 0 || statics_n >= QS_MAX_STATICS)
		return;
	qs_static *st = &statics[statics_n++];
	memset(st, 0, sizeof *st);
	for (int k = 0; k < 3; k++)
		st->origin[k] = origin[k] / R_POS_SUB;
	st->hash   = s >= SND_STREAM ? stream_hash(s - SND_STREAM) : bank[s].hash;
	st->volume = (uint8_t) vol;
	st->atten  = (uint8_t) atten;
}

void snd_start(int entnum, int entchannel, int sfx, const int32_t origin[3], int vol, int atten)
{
	if (sfx < 0 || sfx >= S_COUNT || named[sfx] < 0)
		return;
	snd_start_idx(entnum, entchannel, named[sfx], origin, vol, atten);
}

void snd_start_idx(int entnum, int entchannel, int s, const int32_t origin[3], int vol, int atten)
{
	if (s == SND_NULL) {                         /* a silent sound replaces the channel's */
		if (entchannel == 0)
			return;
		for (int i = 0; i < NUM_DYNAMIC; i++)
			if (dyn[i].entnum == entnum && dyn[i].entchannel == entchannel && dyn[i].sfx >= 0) {
				dyn[i].sfx = -1;
				SpuSetKey(0, 1u << (V_DYNAMIC + i));
			}
		return;
	}
	sfx_t q;
	if (!sfx_get(s, &q))
		return;                                  /* (a streamed sound not in: unheard) */
	int i = pick_channel(entnum, entchannel);
	if (i < 0)
		return;
	channel_t *ch = &dyn[i];
	memset(ch, 0, sizeof *ch);
	for (int k = 0; k < 3; k++)
		ch->origin[k] = origin[k];
	ch->dist_mult  = atten;
	ch->reach      = atten ? (uint32_t) (CLIP_UNITS / atten) : 0xffffffffu;
	ch->dist_m     = DIST_M(atten);
	ch->master_vol = vol;
	ch->entnum     = entnum;
	ch->entchannel = entchannel;
	spatialize(entnum, origin, vol, ch->reach, ch->dist_m, &ch->leftvol, &ch->rightvol);
	if (!ch->leftvol && !ch->rightvol) {
		ch->sfx = -1;
		return;                                      /* not audible at all */
	}
	ch->sfx        = s;
	ch->start_tick = snd_tick;
	ch->end        = q.flags & QSF_LOOP ? NEVER : snd_tick + (int) q.ticks;
	/* an identical sound started this frame: offset this one a little so
	   it does not just make the first one louder */
	int skip = 0;
	for (int j = 0; j < NUM_DYNAMIC; j++) {
		channel_t *c = &dyn[j];
		if (c == ch || c->sfx != s || c->start_tick != snd_tick)
			continue;
		int rate = q.pitch * 44100 / 4096;
		skip = (int) (q_rand() % (uint32_t) (rate / 10)) / 28;
		if ((uint32_t) skip * 16 >= q.len)
			skip = (int) (q.len / 16) - 1;
		break;
	}
	voice_start(V_DYNAMIC + i, &q, ch->leftvol, ch->rightvol, skip);
	if (snd_log)
		printf("snd e%d c%d %08x v%d a%d L%d R%d ch%d k%d\n", entnum, entchannel,
		       (unsigned) (s >= SND_STREAM ? stream_hash(s - SND_STREAM) : bank[s].hash), vol, atten,
		       ch->leftvol, ch->rightvol, i, skip);
}

/* ---------------------------------------------------------------- update -- */

/* S_UpdateAmbientSounds: the listener's leaf levels, faded. The leaf is
   the renderer's (r_frame found the camera's this frame, at the quarter
   unit; the listener is the camera): a KUSEG pointer, as the walk keeps
   them (render.c r_bind_world), brought back to KSEG0 before the index
   against world.leaves. (world_point_leaf's leaf was KUSEG too, and the
   index garbage: the ambients read a wild address, 0 on PCSX-Redux, an
   invalid read on DuckStation, a bus error on the machine.) */
static void update_ambients(int ticks)
{
	const qw_leaf *l = world.ambient && r_state.view_leaf
	                 ? (const qw_leaf *) (r_state.view_leaf | 0x80000000u) : 0;
	for (int a = 0; a < 2; a++) {
		if (amb_sfx[a] < 0)
			continue;
		int vol = 0;
		if (l) {
			int idx = (int) (l - world.leaves);
			vol = AMBIENT_LEVEL * world.ambient[idx * 2 + a];   /* x256 */
			if (vol < 8 * 256) vol = 0;
		}
		if (amb_vol[a] < vol) {
			amb_vol[a] += AMBIENT_FADE * ticks;
			if (amb_vol[a] > vol) amb_vol[a] = vol;
		} else if (amb_vol[a] > vol) {
			amb_vol[a] -= AMBIENT_FADE * ticks;
			if (amb_vol[a] < vol) amb_vol[a] = vol;
		}
		int v = amb_vol[a] >> 8;
		voice_set(V_AMBIENT + a, v, v);
	}
}

void snd_update(const int32_t origin[3], int yaw, int pitch, int roll, int ticks)
{
	snd_tick += ticks;
	for (int k = 0; k < 3; k++)
		listener[k] = origin[k];
	/* AngleVectors' right vector */
	int32_t sy = r_sin(yaw), cy = r_cos(yaw), sp = r_sin(pitch), cp = r_cos(pitch);
	int32_t sr = r_sin(roll), cr = r_cos(roll);
	listener_right[0] = ((-sr * sp >> 12) * cy >> 12) + (cr * sy >> 12);
	listener_right[1] = ((-sr * sp >> 12) * sy >> 12) - (cr * cy >> 12);
	listener_right[2] = -sr * cp >> 12;

	update_ambients(ticks);

	/* the dynamic channels: ended ones freed, the others respatialized */
	for (int i = 0; i < NUM_DYNAMIC; i++) {
		channel_t *ch = &dyn[i];
		if (ch->sfx < 0)
			continue;
		if (snd_tick >= ch->end) {
			ch->sfx = -1;
			continue;
		}
		spatialize(ch->entnum, ch->origin, ch->master_vol, ch->reach, ch->dist_m, &ch->leftvol, &ch->rightvol);
		voice_set(V_DYNAMIC + i, ch->leftvol, ch->rightvol);
	}

	/* the static sounds: each source spatialized, the sources of one sound
	   summed into its voice (S_Update's combine); most are past their
	   reach, a compare each. (An asm version of this loop was tried and
	   came out 85 cycles slower than the compiler's: not kept.) */
	for (int v = 0; v < static_n; v++)
		static_lvol[v] = static_rvol[v] = 0;
	const static_t *st = (const static_t *) statics;
	for (int i = 0; i < statics_n; i++, st++) {
		int32_t dx = st->origin[0] - listener[0], dy = st->origin[1] - listener[1],
		        dz = st->origin[2] - listener[2];
		uint32_t m = (uint32_t) (dx < 0 ? -dx : dx) + (uint32_t) (dy < 0 ? -dy : dy) +
		             (uint32_t) (dz < 0 ? -dz : dz);
		if (m >= st->reach)
			continue;
		if (st->reach == 0xffff) {               /* (no attenuation: within 2048 units) */
			if (dx > 16383) dx = 16383; else if (dx < -16383) dx = -16383;
			if (dy > 16383) dy = 16383; else if (dy < -16383) dy = -16383;
			if (dz > 16383) dz = 16383; else if (dz < -16383) dz = -16383;
		}
		int l, r;
		spatialize_d(dx, dy, dz, st->volume, st->dist_m, &l, &r);
		static_lvol[st->voice] += l;
		static_rvol[st->voice] += r;
	}
	for (int v = 0; v < static_n; v++) {
		int l = static_lvol[v] > 255 ? 255 : static_lvol[v];
		int r = static_rvol[v] > 255 ? 255 : static_rvol[v];
		if (!static_on[v]) {                     /* its samples in now: keyed on */
			sfx_t q;
			if ((!l && !r) || !sfx_get(static_sfx[v], &q))
				continue;
			voice_start(V_STATIC + v, &q, l, r, 0);
			static_on[v] = 1;
			continue;
		}
		voice_set(V_STATIC + v, l, r);
	}
}

/* the residency pass's (engine/stream.c): the static sources' streamed
   sounds, by their distance from the listener (Chebyshev, world units)
   over their reach as a full one's (ATTN_STATIC's 333 units weigh as
   ATTN_NORM's 1000) */
void snd_stream_wants(void)
{
	const static_t *st = (const static_t *) statics;
	for (int i = 0; i < statics_n; i++, st++) {
		int s = static_sfx[st->voice];
		if (s < SND_STREAM)
			continue;
		uint32_t d = 0;
		for (int k = 0; k < 3; k++) {
			int32_t v = st->origin[k] - stream_origin[k];
			uint32_t x = (uint32_t) (v < 0 ? -v : v);
			if (x > d) d = x;
		}
		if (st->reach != 0xffff)
			d = d * NOMINAL_CLIP_DIST / st->reach;   /* (both 1/R_POS_SUB units) */
		else
			d /= R_POS_SUB;
		stream_want(s - SND_STREAM, d);
	}
}
