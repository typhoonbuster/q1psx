#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxspu.h>
#include <hwregs_c.h>

#include "cd.h"
#include "game.h"
#include "mem.h"
#include "qsound.h"
#include "render.h"
#include "snd.h"
#include "world.h"

int snd_log;

/* ---------------------------------------------------------------- voices -- */

#define V_AMBIENT   0            /* water, wind                              */
#define V_DYNAMIC   2            /* MAX_DYNAMIC_CHANNELS                     */
#define NUM_DYNAMIC 8
#define V_STATIC    (V_DYNAMIC + NUM_DYNAMIC)
#define NUM_STATIC  (24 - V_STATIC)

#define MASTER_VOL  0x2ccc       /* Quake's volume cvar, 0.7                 */
#define NOMINAL_CLIP_DIST 1000   /* snd_dma.c sound_nominal_clip_dist        */
#define AMBIENT_LEVEL 77         /* ambient_level 0.3, x256                  */
#define AMBIENT_FADE  427        /* ambient_fade 100 per second: per tick, x256 */
#define NEVER 0x7fffffff

typedef struct {
	int      sfx;                /* bank index, -1 free                      */
	int32_t  origin[3];          /* 1/R_POS_SUB units                        */
	int      entnum, entchannel;
	int      master_vol;         /* 0..255                                   */
	int      dist_mult;          /* atten x64 (x 1 / 64000 per world unit)   */
	int      leftvol, rightvol;  /* 0..255                                   */
	int      end;                /* tick it ends (NEVER: a loop)             */
	int      start_tick;         /* (the same-sound-this-frame offset)       */
} channel_t;

static channel_t dyn[NUM_DYNAMIC];

/* the bank's directory, in the game arena (fixed slots) */
static qs_sound  *bank;
static int        bank_n;
static qs_static *statics;
static int        statics_n;
static int        cd_track;
static int         named[S_COUNT];          /* S_* -> bank index or -1     */

/* the static voices: one per distinct static sound */
static int static_sfx[NUM_STATIC], static_n;
static int static_lvol[NUM_STATIC], static_rvol[NUM_STATIC];

/* the ambients */
static int amb_vol[2];                        /* master_vol x256, fading   */
static int amb_sfx[2];

static int32_t listener[3];
static int32_t listener_right[3];              /* x4096                     */
static int     snd_tick;                       /* the sound clock, ticks    */

static const char *const names[S_COUNT] = {
	"weapons/ax1.wav", "player/axhit1.wav", "player/axhit2.wav", "weapons/guncock.wav",
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
	"player/gib.wav", "player/udeath.wav", "player/tornoff2.wav", "player/teledth1.wav",
	"misc/r_tele1.wav", "misc/r_tele2.wav", "misc/r_tele3.wav", "misc/r_tele4.wav", "misc/r_tele5.wav",
	"misc/menu1.wav", "misc/menu2.wav", "misc/menu3.wav", "misc/talk.wav",
	"items/itembk2.wav", "weapons/lock4.wav", "weapons/pkup.wav", "items/armor1.wav",
	"items/damage3.wav", "misc/power.wav", "demon/dland2.wav",
	"ambience/water1.wav", "ambience/wind2.wav",
};

/* ------------------------------------------------------------------- spu -- */

static void voice_set(int v, int left, int right)
{
	SPU_CH_VOL_L(v) = (uint16_t) (left << 6);        /* 255 -> 0x3fc0 */
	SPU_CH_VOL_R(v) = (uint16_t) (right << 6);
}

/* key voice v on with the bank's sound s, `skip` ADPCM blocks in */
static void voice_start(int v, int s, int left, int right, int skip)
{
	const qs_sound *q = &bank[s];
	uint32_t addr = QS_SPU_BASE + q->ofs + (uint32_t) skip * 16;
	SpuSetKey(0, 1u << v);
	voice_set(v, left, right);
	SPU_CH_FREQ(v)  = q->pitch;
	SPU_CH_ADDR(v)  = (uint16_t) (addr >> 3);
	SPU_CH_ADSR1(v) = 0x000f;                        /* at once to full */
	SPU_CH_ADSR2(v) = 0x0000;                        /* off at once on key-off / end */
	SpuSetKey(1, 1u << v);
}

void snd_init(void)
{
	SpuInit();
	SPU_CTRL = 0xc001;                               /* on, unmuted, CD audio in */
	SpuSetCommonMasterVolume(MASTER_VOL, MASTER_VOL);
	SpuSetCommonCDVolume(0x3fff, 0x3fff);
	SpuSetKey(0, 0xffffff);
	bank    = mem_game_alloc(QS_MAX_SOUNDS * sizeof *bank);
	statics = mem_game_alloc(QS_MAX_STATICS * sizeof *statics);
	for (int i = 0; i < S_COUNT; i++)
		named[i] = -1;
	for (int i = 0; i < NUM_DYNAMIC; i++)
		dyn[i].sfx = -1;
}

int snd_load_bank(const char *map)
{
	char name[24];
	snprintf(name, sizeof name, "\\MAPS\\%s.SND;1", map);
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
	for (int i = 0; i < statics_n; i++)
		for (int k = 0; k < 3; k++)
			statics[i].origin[k] *= R_POS_SUB;
	cd_track = (int) h->cd_track;
	for (int i = 0; i < S_COUNT; i++) {
		uint32_t hh = qs_hash(names[i]);
		for (int s = 0; s < bank_n; s++)
			if (bank[s].hash == hh) {
				named[i] = s;
				break;
			}
	}
	SpuSetKey(0, 0xffffff);
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(QS_SPU_BASE);
	SpuWrite((const uint32_t *) (f + h->ofs_data), (h->len_data + 63) & ~63u);
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
	printf("snd: %s — %d sounds, %u KB of SPU RAM, %d static sounds, CD track %d\n",
	       name, bank_n, (unsigned) h->len_data / 1024, statics_n, cd_track);
	mem_level_reset();
	return 1;
}

int snd_cd_track(void)
{
	return cd_track;
}

void snd_stop_all(void)
{
	SpuSetKey(0, 0xffffff);
	for (int i = 0; i < NUM_DYNAMIC; i++)
		dyn[i].sfx = -1;
	for (int v = 0; v < 24; v++)
		voice_set(v, 0, 0);
	amb_vol[0] = amb_vol[1] = 0;
}

/* the static sounds: a looping voice per distinct sound, silent until the
   listener comes near; the ambients' voices too (S_StopAllSounds, then
   S_StaticSound for each) */
void snd_map_start(void)
{
	snd_stop_all();
	static_n = 0;
	for (int i = 0; i < statics_n; i++) {
		int s = statics[i].sound, v;
		for (v = 0; v < static_n && static_sfx[v] != s; v++)
			;
		if (v == static_n) {
			if (static_n >= NUM_STATIC)
				continue;
			static_sfx[static_n++] = s;
			voice_start(V_STATIC + v, s, 0, 0, 0);
		}
	}
	amb_sfx[0] = named[S_AMB_WATER1];
	amb_sfx[1] = named[S_AMB_WIND2];
	for (int a = 0; a < 2; a++)
		if (amb_sfx[a] >= 0)
			voice_start(V_AMBIENT + a, amb_sfx[a], 0, 0, 0);
}

/* ----------------------------------------------------------- spatialize -- */

static uint32_t isqrt32(uint32_t x)
{
	uint32_t r = 0, bit = 1u << 30;
	while (bit > x) bit >>= 2;
	while (bit) {
		if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
		else r >>= 1;
		bit >>= 2;
	}
	return r;
}

/* SND_Spatialize: distance attenuation and the stereo split from the
   listener's right vector; the view entity is always full volume */
static void spatialize(int entnum, const int32_t origin[3], int master_vol, int dist_mult,
                       int *leftvol, int *rightvol)
{
	if (entnum == ENT_PLAYER) {
		*leftvol = *rightvol = master_vol;
		return;
	}
	int32_t d[3], manhattan = 0;
	for (int k = 0; k < 3; k++) {
		d[k] = origin[k] - listener[k];
		if (d[k] > 16383) d[k] = 16383;              /* 2048 units: beyond hearing */
		if (d[k] < -16383) d[k] = -16383;
		manhattan += d[k] < 0 ? -d[k] : d[k];
	}
	/* silent past 1000 / atten units (the Manhattan distance bounds the
	   real one, so this is exact): most static sources, every frame */
	if (dist_mult && manhattan * dist_mult >= NOMINAL_CLIP_DIST * 64 * R_POS_SUB) {
		*leftvol = *rightvol = 0;
		return;
	}
	int32_t len = (int32_t) isqrt32((uint32_t) (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
	/* dist x dist_mult, x256: len in 1/8 units (< 28 k), dist_mult in 1/64
	   per unit: their product fits 32 bits */
	int dist = len * dist_mult / (NOMINAL_CLIP_DIST * 64 * R_POS_SUB / 256);
	int dot = len ? (listener_right[0] * d[0] + listener_right[1] * d[1] +
	                 listener_right[2] * d[2]) / len : 0;   /* x4096 (the sum < 2^31) */
	int scale = 256 - dist;                          /* (1 - dist) x256 */
	int r = master_vol * scale * (4096 + dot) / (256 * 4096);
	int l = master_vol * scale * (4096 - dot) / (256 * 4096);
	*rightvol = r < 0 ? 0 : r > 255 ? 255 : r;
	*leftvol  = l < 0 ? 0 : l > 255 ? 255 : l;
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

void snd_start(int entnum, int entchannel, int sfx, const int32_t origin[3], int vol, int atten)
{
	if (sfx < 0 || sfx >= S_COUNT || named[sfx] < 0)
		return;
	int s = named[sfx];
	int i = pick_channel(entnum, entchannel);
	if (i < 0)
		return;
	channel_t *ch = &dyn[i];
	memset(ch, 0, sizeof *ch);
	for (int k = 0; k < 3; k++)
		ch->origin[k] = origin[k];
	ch->dist_mult  = atten;
	ch->master_vol = vol;
	ch->entnum     = entnum;
	ch->entchannel = entchannel;
	spatialize(entnum, origin, vol, atten, &ch->leftvol, &ch->rightvol);
	if (!ch->leftvol && !ch->rightvol) {
		ch->sfx = -1;
		return;                                      /* not audible at all */
	}
	ch->sfx        = s;
	ch->start_tick = snd_tick;
	ch->end        = bank[s].flags & QSF_LOOP ? NEVER : snd_tick + (int) bank[s].ticks;
	/* an identical sound started this frame: offset this one a little so
	   it does not just make the first one louder */
	int skip = 0;
	for (int j = 0; j < NUM_DYNAMIC; j++) {
		channel_t *c = &dyn[j];
		if (c == ch || c->sfx != s || c->start_tick != snd_tick)
			continue;
		int rate = bank[s].pitch * 44100 / 4096;
		skip = (int) (q_rand() % (uint32_t) (rate / 10)) / 28;
		if ((uint32_t) skip * 16 >= bank[s].len)
			skip = (int) (bank[s].len / 16) - 1;
		break;
	}
	voice_start(V_DYNAMIC + i, s, ch->leftvol, ch->rightvol, skip);
	if (snd_log)
		printf("snd e%d c%d %08x v%d a%d L%d R%d ch%d k%d\n", entnum, entchannel,
		       (unsigned) bank[s].hash, vol, atten, ch->leftvol, ch->rightvol, i, skip);
}

/* ---------------------------------------------------------------- update -- */

/* S_UpdateAmbientSounds: the listener's leaf levels, faded */
static void update_ambients(int ticks)
{
	int32_t p[3] = { listener[0] / R_POS_SUB, listener[1] / R_POS_SUB, listener[2] / R_POS_SUB };
	const qw_leaf *l = world.ambient ? world_point_leaf(p) : 0;
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
		spatialize(ch->entnum, ch->origin, ch->master_vol, ch->dist_mult, &ch->leftvol, &ch->rightvol);
		voice_set(V_DYNAMIC + i, ch->leftvol, ch->rightvol);
	}

	/* the static sounds: each source spatialized, the sources of one sound
	   summed into its voice (S_Update's combine) */
	for (int v = 0; v < static_n; v++)
		static_lvol[v] = static_rvol[v] = 0;
	for (int i = 0; i < statics_n; i++) {
		const qs_static *st = &statics[i];
		int v;
		for (v = 0; v < static_n && static_sfx[v] != st->sound; v++)
			;
		if (v == static_n)
			continue;
		int l, r;
		spatialize(ENT_WORLD, st->origin, st->volume, st->atten, &l, &r);
		static_lvol[v] += l;
		static_rvol[v] += r;
	}
	for (int v = 0; v < static_n; v++) {
		int l = static_lvol[v] > 255 ? 255 : static_lvol[v];
		int r = static_rvol[v] > 255 ? 255 : static_rvol[v];
		voice_set(V_STATIC + v, l, r);
	}
}
