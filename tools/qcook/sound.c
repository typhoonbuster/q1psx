/*
 * q1psx — `qcook map --sounds`: the map's sound bank (formats/qsound.h);
 * the streamed sounds' encoding (`qcook stream`, formats/qstream.h).
 *
 * The bank: Quake's always-precached list (world.qc worldspawn, weapons.qc
 * W_Precache, the menu's: the player's, the weapons') with the groups the
 * map cannot use and the sounds only multiplayer plays left out (ALWAYS),
 * the area ambients the BSP's leaf levels can
 * raise (snd_dma.c ambient_sfx: water1, wind2), and the map's static
 * sources (misc.qc ambientsound calls) by their sound's hash. The sounds
 * with a place in the map are streamed instead: the ambient entities'
 * (misc.qc, triggers.qc's teleporter hum), the ones the entities precache
 * when they spawn (doors.qc, plats.qc, buttons.qc, triggers.qc, items.qc,
 * misc.qc: by class, `sounds` and worldtype; sound_streamed lists them),
 * the monsters' (tools/qcook/stream.c). Each WAV is read as Quake's
 * snd_mem.c GetWavinfo does (8 or 16 bits, mono; the cue point and the
 * LIST "mark" length give the loop) and encoded to SPU ADPCM.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "qcook.h"
#include "../../formats/qsound.h"
#include "../../formats/qstream.h"

/* ----------------------------------------------------------------- sets -- */

enum { SG_CORE, SG_WATER, SG_LAVA, SG_SLIME, SG_SKY, SG_TELE, SG_ITEMS, SG_BOSS, SG_COUNT };
static const char *const group_name[SG_COUNT] = { "core", "water", "lava", "slime", "sky", "teleporters", "items", "boss" };

typedef struct { const char *name; int group; } sound_spec;

/* world.qc worldspawn + weapons.qc W_Precache + the menu (menu.c), but
   the four sounds only multiplayer plays (the port is single player
   only):
   - items/itembk2.wav, an item's respawn (items.qc SUB_regen: deathmatch
     alone sets the think time);
   - player/teledth1.wav, a player telefragged (player.qc PainSound,
     GibPlayer): a teleporter's death hurts a player only when another
     player arrives on him (triggers.qc tdeath_touch);
   - player/axhit1.wav, a player's pain from an axe (PainSound's axhitme):
     only a player swings one (weapons.qc W_FireAxe);
   - player/tornoff2.wav, a player leaving the game (client.qc
     ClientDisconnect). */
static const sound_spec ALWAYS[] = {
	{ "demon/dland2.wav",   SG_CORE },    /* landing thud                        */
	{ "misc/h2ohit1.wav",   SG_WATER },   /* landing splash                      */
	{ "player/plyrjmp8.wav", SG_CORE },
	{ "player/land.wav",    SG_CORE },
	{ "player/land2.wav",   SG_CORE },
	{ "player/drown1.wav",  SG_WATER },
	{ "player/drown2.wav",  SG_WATER },
	{ "player/gasp1.wav",   SG_WATER },
	{ "player/gasp2.wav",   SG_WATER },
	{ "player/h2odeath.wav", SG_WATER },
	{ "misc/talk.wav",      SG_CORE },
	{ "weapons/lock4.wav",  SG_ITEMS },   /* ammo pick up                        */
	{ "weapons/pkup.wav",   SG_ITEMS },   /* weapon up                           */
	{ "items/armor1.wav",   SG_ITEMS },
	{ "weapons/lhit.wav",   SG_CORE },
	{ "weapons/lstart.wav", SG_CORE },
	{ "items/damage3.wav",  SG_ITEMS },
	{ "misc/power.wav",     SG_BOSS },
	{ "player/gib.wav",     SG_CORE },
	{ "player/udeath.wav",  SG_CORE },
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

/* The sounds the entities precache when they spawn (their spawn functions
   in doors.qc, plats.qc, buttons.qc, triggers.qc, items.qc, misc.qc): by
   class, the `sounds` key (-1: any) and worldtype (-1: any). A door's
   key sounds (noise3 / noise4) only when it has a key flag: the QC
   precaches them for every door, the bank keeps its SPU RAM. misc/null.wav
   is not banked: an absent sound plays nothing. */
#define ANY (-1)
typedef struct { const char *classname; int sounds, worldtype; const char *sound; } ent_sound_spec;
static const ent_sound_spec ENT_SOUNDS[] = {
	{ "func_door",        1, ANY, "doors/drclos4.wav" },
	{ "func_door",        1, ANY, "doors/doormv1.wav" },
	{ "func_door",        2, ANY, "doors/hydro1.wav" },
	{ "func_door",        2, ANY, "doors/hydro2.wav" },
	{ "func_door",        3, ANY, "doors/stndr1.wav" },
	{ "func_door",        3, ANY, "doors/stndr2.wav" },
	{ "func_door",        4, ANY, "doors/ddoor1.wav" },
	{ "func_door",        4, ANY, "doors/ddoor2.wav" },
	{ "func_door_secret", 1, ANY, "doors/latch2.wav" },
	{ "func_door_secret", 1, ANY, "doors/winch2.wav" },
	{ "func_door_secret", 1, ANY, "doors/drclos4.wav" },
	{ "func_door_secret", 2, ANY, "doors/airdoor1.wav" },
	{ "func_door_secret", 2, ANY, "doors/airdoor2.wav" },
	{ "func_door_secret", 3, ANY, "doors/basesec1.wav" },   /* (sounds 0 -> 3) */
	{ "func_door_secret", 3, ANY, "doors/basesec2.wav" },
	{ "func_plat",        1, ANY, "plats/plat1.wav" },
	{ "func_plat",        1, ANY, "plats/plat2.wav" },
	{ "func_plat",        2, ANY, "plats/medplat1.wav" },   /* (sounds 0 -> 2) */
	{ "func_plat",        2, ANY, "plats/medplat2.wav" },
	{ "func_button",      0, ANY, "buttons/airbut1.wav" },
	{ "func_button",      1, ANY, "buttons/switch21.wav" },
	{ "func_button",      2, ANY, "buttons/switch02.wav" },
	{ "func_button",      3, ANY, "buttons/switch04.wav" },
	{ "func_train",       1, ANY, "plats/train2.wav" },
	{ "func_train",       1, ANY, "plats/train1.wav" },
	{ "trigger_multiple", 1, ANY, "misc/secret.wav" },
	{ "trigger_multiple", 2, ANY, "misc/talk.wav" },
	{ "trigger_multiple", 3, ANY, "misc/trigger1.wav" },
	{ "trigger_once",     1, ANY, "misc/secret.wav" },
	{ "trigger_once",     2, ANY, "misc/talk.wav" },
	{ "trigger_once",     3, ANY, "misc/trigger1.wav" },
	{ "trigger_secret",   1, ANY, "misc/secret.wav" },      /* (sounds 0 -> 1) */
	{ "trigger_secret",   2, ANY, "misc/talk.wav" },
	{ "trigger_push",     ANY, ANY, "ambience/windfly.wav" },
	{ "item_health",      ANY, ANY, "items/health1.wav" },  /* (by spawnflags below) */
	{ "item_key1",        ANY, 0, "misc/medkey.wav" },
	{ "item_key1",        ANY, 1, "misc/runekey.wav" },
	{ "item_key1",        ANY, 2, "misc/basekey.wav" },
	{ "item_key2",        ANY, 0, "misc/medkey.wav" },
	{ "item_key2",        ANY, 1, "misc/runekey.wav" },
	{ "item_key2",        ANY, 2, "misc/basekey.wav" },
	{ "item_sigil",       ANY, ANY, "misc/runekey.wav" },
	{ "item_artifact_invulnerability", ANY, ANY, "items/protect.wav" },
	{ "item_artifact_invulnerability", ANY, ANY, "items/protect2.wav" },
	{ "item_artifact_invulnerability", ANY, ANY, "items/protect3.wav" },
	{ "item_artifact_envirosuit", ANY, ANY, "items/suit.wav" },
	{ "item_artifact_envirosuit", ANY, ANY, "items/suit2.wav" },
	{ "item_artifact_invisibility", ANY, ANY, "items/inv1.wav" },
	{ "item_artifact_invisibility", ANY, ANY, "items/inv2.wav" },
	{ "item_artifact_invisibility", ANY, ANY, "items/inv3.wav" },
	{ "item_artifact_super_damage", ANY, ANY, "items/damage.wav" },
	{ "item_artifact_super_damage", ANY, ANY, "items/damage2.wav" },
	{ "item_artifact_super_damage", ANY, ANY, "items/damage3.wav" },
	{ "trap_spikeshooter", ANY, ANY, "weapons/spike2.wav" },
	{ "misc_explobox",    ANY, ANY, "weapons/r_exp3.wav" },
	{ "misc_explobox2",   ANY, ANY, "weapons/r_exp3.wav" },
};
/* the teleporters' fog, one of five at random (misc.qc spawn_tfog): the
   rarest sounds a map plays, the first to leave SPU RAM for main RAM in the
   split (docs/porting-notes.md §87.5), where the engine keeps two of them
   in the swap room and rotates after each teleport */
static const char *const TELE_SOUNDS[5] = {
	"misc/r_tele1.wav", "misc/r_tele2.wav", "misc/r_tele3.wav", "misc/r_tele4.wav", "misc/r_tele5.wav",
};

static const char *const KEY_DOOR_SOUNDS[3][2] = {
	{ "doors/medtry.wav", "doors/meduse.wav" },
	{ "doors/runetry.wav", "doors/runeuse.wav" },
	{ "doors/basetry.wav", "doors/baseuse.wav" },
};
#define DOOR_GOLD_KEY   8
#define DOOR_SILVER_KEY 16
#define H_ROTTEN 1
#define H_MEGA   2

/* The sounds of the weapons (weapons.qc) and what else plays them: a map
   keeps one only when the player can hold the weapon on entering it or an
   entity of the map plays it (ogre.qc's grenades, shambler.qc's lightning,
   misc.qc's spike shooters and exploding boxes). The rest of ALWAYS stays
   by its group. */
enum { W_SSG = 1, W_NG = 2, W_SNG = 4, W_GL = 8, W_RL = 16, W_LG = 32 };
typedef struct { const char *name; unsigned weapons; const char *classes[5]; } weapon_sound;
static const weapon_sound WEAPON_SOUNDS[] = {
	{ "weapons/shotgn2.wav",  W_SSG,        { 0 } },
	{ "weapons/rocket1i.wav", W_NG,         { 0 } },
	{ "weapons/spike2.wav",   W_SNG,        { "trap_spikeshooter", 0 } },
	{ "weapons/ric1.wav",     W_NG | W_SNG, { "trap_spikeshooter", 0 } },
	{ "weapons/ric2.wav",     W_NG | W_SNG, { "trap_spikeshooter", 0 } },
	{ "weapons/ric3.wav",     W_NG | W_SNG, { "trap_spikeshooter", 0 } },
	{ "weapons/tink1.wav",    W_NG | W_SNG, { "trap_spikeshooter", 0 } },
	{ "weapons/grenade.wav",  W_GL,         { "monster_ogre", "monster_ogre_marksman", 0 } },
	{ "weapons/bounce.wav",   W_GL,         { "monster_ogre", "monster_ogre_marksman", 0 } },
	{ "weapons/r_exp3.wav",   W_GL | W_RL,  { "monster_ogre", "monster_ogre_marksman", "misc_explobox", "misc_explobox2", 0 } },
	{ "weapons/sgun1.wav",    W_RL,         { 0 } },
	{ "weapons/lstart.wav",   W_LG,         { 0 } },
	{ "weapons/lhit.wav",     W_LG,         { "monster_shambler", 0 } },
};
static const struct { const char *classname; unsigned bit; } WEAPON_ENTS[] = {
	{ "weapon_supershotgun", W_SSG }, { "weapon_nailgun", W_NG }, { "weapon_supernailgun", W_SNG },
	{ "weapon_grenadelauncher", W_GL }, { "weapon_rocketlauncher", W_RL }, { "weapon_lightning", W_LG },
};
static const char *const WEAPON_NAMES[6] = { "super shotgun", "nailgun", "super nailgun", "grenade launcher",
                                             "rocket launcher", "lightning gun" };

/* Every map, and the weapons a player can hold on entering each: the
   shotgun and the axe always (Quake takes the rest away when a map leads
   to START: client.qc SetNewParms), and every weapon_* single player
   spawns on any path from START to the map, through the maps'
   trigger_changelevel targets (the secret levels: E1M4 -> E1M8 -> E1M5
   and the like; END is reached from START). Read from the paks' maps. */
static const char *const EPISODE_MAPS[] = {
	"start", "e1m1", "e1m2", "e1m3", "e1m4", "e1m5", "e1m6", "e1m7", "e1m8",
	"e2m1", "e2m2", "e2m3", "e2m4", "e2m5", "e2m6", "e2m7",
	"e3m1", "e3m2", "e3m3", "e3m4", "e3m5", "e3m6", "e3m7",
	"e4m1", "e4m2", "e4m3", "e4m4", "e4m5", "e4m6", "e4m7", "e4m8", "end",
};
#define NUM_EPISODE_MAPS (int) (sizeof EPISODE_MAPS / sizeof EPISODE_MAPS[0])

static int episode_index(const char *name)
{
	for (int m = 0; m < NUM_EPISODE_MAPS; m++)
		if (!strcasecmp(name, EPISODE_MAPS[m]))
			return m;
	return -1;
}

static unsigned episode_weapons(const char *map, const uint8_t *pak, long pak_len, const uint8_t *pak0, long pak0_len)
{
	unsigned has[NUM_EPISODE_MAPS] = { 0 }, obt[NUM_EPISODE_MAPS] = { 0 };
	int      next[NUM_EPISODE_MAPS][6], nnext[NUM_EPISODE_MAPS] = { 0 }, present[NUM_EPISODE_MAPS] = { 0 };
	for (int m = 0; m < NUM_EPISODE_MAPS; m++) {
		char path[32];
		snprintf(path, sizeof path, "maps/%s.bsp", EPISODE_MAPS[m]);
		uint32_t len;
		const uint8_t *raw = pak_find(pak, pak_len, path, &len);
		if (!raw && pak0)
			raw = pak_find(pak0, pak0_len, path, &len);
		if (!raw)
			continue;                             /* (a pak the cook has not: its maps are not on the disc) */
		present[m] = 1;
		bsp_t b;
		bsp_parse(&b, raw, len);
		ent_list l;
		ents_parse(&b, &l);
		for (int i = 0; i < l.num; i++) {
			const ent_rec *e = &l.ents[i];
			for (size_t k = 0; k < sizeof WEAPON_ENTS / sizeof WEAPON_ENTS[0]; k++)
				if (!strcmp(e->classname, WEAPON_ENTS[k].classname))
					has[m] |= WEAPON_ENTS[k].bit;
			if (!strcmp(e->classname, "trigger_changelevel")) {
				const char *to = ent_value(&l, e, "map");
				int t = to ? episode_index(to) : -1;
				if (t > 0 && nnext[m] < 6) {      /* (0 is START: the inventory is taken away) */
					int dup = 0;
					for (int j = 0; j < nnext[m]; j++)
						dup |= next[m][j] == t;
					if (!dup)
						next[m][nnext[m]++] = t;
				}
			}
		}
		ents_free(&l);
	}
	/* from START, every path: a map's weapons add to what the paths before
	   it gave, until nothing new reaches any map */
	int changed = 1;
	for (int pass = 0; changed && pass < 64; pass++) {
		changed = 0;
		for (int j = 0; j < nnext[0]; j++) {
			int t = next[0][j];
			unsigned v = has[t];
			if ((obt[t] | v) != obt[t]) {
				obt[t] |= v;
				changed = 1;
			}
		}
		for (int m = 1; m < NUM_EPISODE_MAPS; m++) {
			if (!present[m] || (!obt[m] && !has[m]))
				continue;
			unsigned v = obt[m] | has[m];
			for (int j = 0; j < nnext[m]; j++) {
				int t = next[m][j];
				if ((obt[t] | v | has[t]) != obt[t]) {
					obt[t] |= v | has[t];
					changed = 1;
				}
			}
		}
	}
	int m = episode_index(map);
	return m >= 0 ? obt[m] : 0;
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

/* The low-pass that halves a rate: a windowed sinc (Blackman, HALVE_TAPS
   taps) cut at a quarter of the rate; flat to a fifth of it, 60 dB down
   past three tenths. (The 9-tap half-band it replaces was 0.82 at a
   tenth of the rate and 0.56 at 0.15: it dulled what it kept.) */
#define HALVE_TAPS 95
static const double *halve_kernel(void)
{
	static double k[HALVE_TAPS];
	static int made;
	if (!made) {
		double sum = 0;
		for (int t = 0; t < HALVE_TAPS; t++) {
			double m = t - (HALVE_TAPS - 1) / 2.0, x = M_PI * m / 2;   /* sinc at fc = 1/4 */
			double s = m == 0 ? 0.5 : sin(x) / (M_PI * m);
			double wn = 0.42 - 0.5 * cos(2 * M_PI * t / (HALVE_TAPS - 1)) + 0.08 * cos(4 * M_PI * t / (HALVE_TAPS - 1));
			k[t] = s * wn;
			sum += k[t];
		}
		for (int t = 0; t < HALVE_TAPS; t++)
			k[t] /= sum;
		made = 1;
	}
	return k;
}

/* halve the sample rate: the low-pass above, then every other sample */
static void wav_halve(wav_t *w)
{
	const double *k = halve_kernel();
	int n2 = (w->n + 1) / 2;
	int16_t *o = xmalloc((size_t) n2 * sizeof *o);
	for (int i = 0; i < n2; i++) {
		double a = 0;
		for (int t = 0; t < HALVE_TAPS; t++) {
			int j = 2 * i + t - (HALVE_TAPS - 1) / 2;
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
	int      group;      /* SG_*                                */
	wav_t    wav;
	int      halved;
	bytes_t  adpcm;
	double   snr;
	int      cls;        /* the split's order: 0 the always set, 1 the ambients, 2 the
	                        monsters', 3 the entities' (docs/porting-notes.md §86) */
	int      weight;     /* within a class: the entities bringing it (more first) */
	int      ram;        /* put in main RAM, not the bank                        */
	int      disc;       /* put out of the SPU, read from the level's file when
	                        wanted, not kept in RAM (a fog, a powerup's sound:
	                        docs/porting-notes.md §98)                          */
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

static void halve_by_class(bank_sound *s);

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
	halve_by_class(s);
	return nsounds++;
}

static size_t sound_bytes(const bank_sound *s)
{
	int pre = s->wav.loopstart >= 0 ? (28 - s->wav.loopstart % 28) % 28 : 0;
	return (size_t) ((pre + s->wav.n + 27) / 28) * 16;
}

/* ------------------------------------------------------ the rate by class -- */

/* The SPU plays its own ADPCM alone, 4 bits a sample: a sound's bytes in
   SPU RAM follow its rate, and the rate is the one lever on them. Most of
   Quake's sounds are 11 025 Hz 8-bit; many hold next to nothing above
   2.76 kHz (rumbles, hums, the player's cries), some are all highs (the
   ricochets, the metal tinks, the pickups). Every sound, the bank's and
   the streamed ones alike, is cooked at its own rate: the lowest that
   keeps QCOOK_SOUND_KEEP percent of its energy (default 98; 100 or 0:
   its own rate), read off its spectrum (an FFT), with 15 % of room above
   that frequency and 2756 Hz at the least; the SPU plays it at the pitch
   of that rate (docs/porting-notes.md §85, §87: the halvings of §85-86
   gave an uneven fidelity, a sound losing 5 % of its energy beside one
   losing nothing). The budget below still halves sounds for room. */
/* an in-place radix-2 FFT (re, im interleaved), n a power of two */
static void fft(double *z, int n)
{
	for (int i = 1, j = 0; i < n; i++) {           /* bit reversal */
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			double tr = z[2 * i], ti = z[2 * i + 1];
			z[2 * i] = z[2 * j]; z[2 * i + 1] = z[2 * j + 1];
			z[2 * j] = tr;       z[2 * j + 1] = ti;
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		double ang = -2 * M_PI / len, wr = cos(ang), wi = sin(ang);
		for (int i = 0; i < n; i += len) {
			double cr = 1, ci = 0;
			for (int j = 0; j < len / 2; j++) {
				int a = 2 * (i + j), b = 2 * (i + j + len / 2);
				double ur = z[a], ui = z[a + 1];
				double vr = z[b] * cr - z[b + 1] * ci, vi = z[b] * ci + z[b + 1] * cr;
				z[a] = ur + vr;     z[a + 1] = ui + vi;
				z[b] = ur - vr;     z[b + 1] = ui - vi;
				double ncr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = ncr;
			}
		}
	}
}

/* the frequency below which `keep` of a sound's energy lies (its
   spectrum, by an FFT over the sound zero-padded), in Hz */
static double keep_freq(const wav_t *w, double keep)
{
	int n = 1;
	while (n < w->n)
		n <<= 1;
	double *z = xcalloc((size_t) n * 2, sizeof *z), e_all = 0, acc = 0;
	for (int i = 0; i < w->n; i++)
		z[2 * i] = w->s[i];
	fft(z, n);
	for (int k = 0; k <= n / 2; k++)                /* (the real signal's half) */
		e_all += z[2 * k] * z[2 * k] + z[2 * k + 1] * z[2 * k + 1];
	int k = 0;
	for (; k < n / 2 && e_all > 0; k++) {
		acc += z[2 * k] * z[2 * k] + z[2 * k + 1] * z[2 * k + 1];
		if (acc >= keep * e_all)
			break;
	}
	free(z);
	return (double) k * w->rate / n;
}

/* the sound at `rate` (below its own): the halvings first (wav_halve's
   clean low-pass while the rate allows), then the last step by a
   windowed-sinc resampler at a ratio in (1/2, 1], its low-pass at 0.45 of
   the new rate (Blackman, HALVE_TAPS taps); the loop's start follows */
static void wav_resample(wav_t *w, int rate)
{
	while (w->rate / 2 >= rate && w->rate > 2756)
		wav_halve(w);
	if (rate >= w->rate || w->n < 2)
		return;
	double ratio = (double) rate / w->rate, fc = 0.45 * ratio;   /* (cycles a sample of the old rate) */
	double half = (HALVE_TAPS + 1) / 2.0;
	int n2 = (int) lround(w->n * ratio);
	if (n2 < 1)
		n2 = 1;
	int16_t *o = xmalloc((size_t) n2 * sizeof *o);
	for (int j = 0; j < n2; j++) {
		double t = j / ratio, a = 0, wsum = 0;    /* (the output sample's time, in old samples) */
		int c = (int) floor(t);
		for (int k = c - (HALVE_TAPS - 1) / 2; k <= c + (HALVE_TAPS + 1) / 2; k++) {
			double d = k - t;
			if (fabs(d) >= half)
				continue;
			double x = M_PI * (d / half + 1);        /* the Blackman window over (-half, half) */
			double win = 0.42 - 0.5 * cos(x) + 0.08 * cos(2 * x);
			double h = (d == 0 ? 2 * fc : sin(2 * M_PI * fc * d) / (M_PI * d)) * win;
			int kk = k < 0 ? 0 : k >= w->n ? w->n - 1 : k;
			a    += h * w->s[kk];
			wsum += h;
		}
		if (wsum > 0)
			a /= wsum;                            /* (unity gain) */
		if (a > 32767) a = 32767;
		if (a < -32768) a = -32768;
		o[j] = (int16_t) lround(a);
	}
	free(w->s);
	w->s = o;
	w->n = n2;
	if (w->loopstart >= 0)
		w->loopstart = (int) lround(w->loopstart * ratio);
	w->rate = rate;
}

static double keep_share(void)
{
	static double t = -1;
	if (t < 0) {
		const char *e = getenv("QCOOK_SOUND_KEEP");
		t = e ? atof(e) : 98.0;
		if (t <= 0 || t >= 100)
			t = 0;                                /* (none: the sound's own rate) */
	}
	return t / 100;
}

static int    class_halved_n;                     /* the sounds at a rate under their own, the bytes saved */
static size_t class_halved_saved;

static void halve_by_class(bank_sound *s)
{
	double keep = keep_share();
	if (keep <= 0 || s->wav.n < 64 || s->wav.rate <= 2756)
		return;
	double fc = keep_freq(&s->wav, keep);
	int rate = (int) lround(2 * fc * 1.15);        /* (15 % above the Nyquist the energy asks for) */
	if (rate < 2756)
		rate = 2756;
	if (rate >= s->wav.rate)
		return;
	rate = (int) lround(lround(rate * 4096.0 / 44100) * 44100.0 / 4096);   /* (a pitch the SPU has) */
	size_t before = sound_bytes(s);
	wav_resample(&s->wav, rate);
	s->halved = 1;
	class_halved_n++;
	class_halved_saved += before - sound_bytes(s);
}

void sound_class_halved(int *n, size_t *saved)
{
	*n     = class_halved_n;
	*saved = class_halved_saved;
}

/* the bank's sounds (sounds[0 .. nsounds)) encoded and written, with its
   static sources and its CD track */
static void bank_write(const qs_static *statics, int nstatics, int cd_track, int halved, const char *out_path)
{
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
	if (nstatics)
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

	int per_group[SG_COUNT] = { 0 };
	for (int i = 0; i < nsounds; i++)
		per_group[sounds[i].group]++;
	int cls_n;
	size_t cls_saved;
	sound_class_halved(&cls_n, &cls_saved);
	printf("qcook: sounds — %d sounds, %zu KB of SPU RAM (%d halved by their class, %zu KB saved; %d for room), %d static "
	       "sounds, CD track %d, worst SNR %.1f dB (%s)\n",
	       nsounds, data / 1024, cls_n, cls_saved / 1024, halved, nstatics, cd_track, worst_snr, worst);
	printf("qcook: sounds — core %d", per_group[SG_CORE]);
	for (int g = 1; g < SG_COUNT; g++)
		if (per_group[g]) printf(", %s %d", group_name[g], per_group[g]);
	printf("\n");

	for (int i = 0; i < nsounds; i++) {
		free(sounds[i].wav.s);
		free(sounds[i].adpcm.p);
	}
	free(dir);
	free(f.p);
}

/* the powerups' sounds while they run: the player's, anywhere (engine/
   game.c game_stream_wants pins them from the pickup; engine/items.c has
   the artifacts want them by their distance before) */
static const char *const powerup_sounds[] = { "items/inv2.wav", "items/inv3.wav", "items/protect2.wav",
                                              "items/protect3.wav", "items/damage2.wav", "items/suit2.wav" };

static int powerup_sound(const char *name)
{
	for (size_t k = 0; k < sizeof powerup_sounds / sizeof powerup_sounds[0]; k++)
		if (!strcmp(name, powerup_sounds[k]))
			return 1;
	return 0;
}

void sound_build(const bsp_t *b, const uint8_t *pak, long pak_len, const uint8_t *pak0,
                 long pak0_len, const char *out_path, const char *map, const char *sndset_path)
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
	ent_list ents;
	ents_parse(b, &ents);
	for (int i = 0; i < ents.num; i++) {
		const ent_rec *e = &ents.ents[i];
		const char *cls = e->classname;
		if (!strcmp(cls, "worldspawn"))
			cd_track = (int) ent_float(&ents, e, "sounds", 0);
	}
	for (int i = 0; i < ents.num; i++) {
		const ent_rec *e = &ents.ents[i];
		const char *cls = e->classname;
		int spawnflags = (int) ent_float(&ents, e, "spawnflags", 0);
		double origin[3];
		int have_origin = ent_vector(&ents, e, "origin", origin);
		if (!strncmp(cls, "item_", 5) || !strncmp(cls, "weapon_", 7)) have[SG_ITEMS] = 1;
		if (!strcmp(cls, "trigger_teleport") || !strcmp(cls, "info_teleport_destination"))
			have[SG_TELE] = 1;
		if (!strcmp(cls, "monster_boss")) have[SG_BOSS] = 1;
		for (size_t k = 0; k < sizeof AMBIENTS / sizeof AMBIENTS[0]; k++) {
			const ambient_spec *a = &AMBIENTS[k];
			if (strcmp(cls, a->classname)) continue;
			if (!strcmp(a->classname, "trigger_teleport") && (spawnflags & 1)) continue;   /* SILENT */
			if (!have_origin)                      /* a brush entity: no origin; the engine
			                                           places its hum at the brush centre */
				continue;
			if (nstatics >= QS_MAX_STATICS) die("too many static sounds");
			qs_static *st = &statics[nstatics++];
			memset(st, 0, sizeof *st);
			for (int c = 0; c < 3; c++) st->origin[c] = (int32_t) lround(origin[c]);
			st->hash   = qs_hash(a->sound);    /* (streamed) */
			st->volume = (uint8_t) lround(a->vol * 255);
			st->atten  = (uint8_t) lround(a->atten * 64);
		}
	}
	/* The level's whole set of sounds (docs/porting-notes.md §86): the
	   always set, less the weapons' sounds the player cannot need here
	   (the weapons they can hold on entering, the entities that play them);
	   the ambients' (the static sources'); the monsters' kinds'; the
	   entities' (doors, plats, buttons, items...). Every one single player
	   can meet, none it cannot. */
	unsigned weapons = map ? episode_weapons(map, pak, pak_len, pak0, pak0_len) : ~0u;
	for (size_t k = 0; k < sizeof ALWAYS / sizeof ALWAYS[0]; k++) {
		if (!have[ALWAYS[k].group])
			continue;
		int keep = 1;
		for (size_t w = 0; w < sizeof WEAPON_SOUNDS / sizeof WEAPON_SOUNDS[0]; w++) {
			const weapon_sound *ws = &WEAPON_SOUNDS[w];
			if (strcmp(ws->name, ALWAYS[k].name))
				continue;
			keep = (ws->weapons & weapons) != 0;
			for (int c = 0; !keep && ws->classes[c]; c++)
				for (int i = 0; i < ents.num && !keep; i++)
					keep = !strcmp(ents.ents[i].classname, ws->classes[c]);
		}
		if (keep) {
			int i = sound_add(ALWAYS[k].name, ALWAYS[k].group, pak, pak_len, pak0, pak0_len);
			sounds[i].cls = 0;
			sounds[i].weight = 1000;
		}
	}
	for (int st = 0; st < nstatics; st++)       /* the ambients: by their sources */
		for (size_t k = 0; k < sizeof AMBIENTS / sizeof AMBIENTS[0]; k++)
			if (qs_hash(AMBIENTS[k].sound) == statics[st].hash) {
				int i = sound_add(AMBIENTS[k].sound, SG_CORE, pak, pak_len, pak0, pak0_len);
				if (sounds[i].cls < 1)
					sounds[i].cls = 1;
				sounds[i].weight++;
				break;
			}
	{                                            /* the monsters' kinds' sounds: by the kind's count */
		int kind_count[G_WORLD + 1] = { 0 };
		for (int i = 0; i < ents.num; i++) {
			int g = stream_kind_group(ents.ents[i].classname);
			if (g)
				kind_count[g]++;
		}
		const char *names[512];
		int n = stream_monster_needs(&ents, names, 512);
		for (int k = 0; k < n; k++) {
			size_t ln = strlen(names[k]);
			if (ln < 4 || strcmp(names[k] + ln - 4, ".wav"))
				continue;                         /* (a model) */
			int g = stream_sound_group(names[k]);
			int i = sound_add(names[k], SG_CORE, pak, pak_len, pak0, pak0_len);
			if (!sounds[i].weight) {
				sounds[i].cls = 2;
				sounds[i].weight = g >= 0 && g <= G_WORLD ? kind_count[g] : 1;
			}
		}
	}
	{                                            /* the entities' sounds: by the entities bringing them */
		const char *names[256];
		int counts[256];
		int worldtype = 0;
		for (int i = 0; i < ents.num; i++)
			if (!strcmp(ents.ents[i].classname, "worldspawn"))
				worldtype = (int) ent_float(&ents, &ents.ents[i], "worldtype", 0);
		int n = sound_map_streamed_n(&ents, worldtype, names, counts, 256);
		for (int k = 0; k < n; k++) {
			int i = sound_add(names[k], SG_CORE, pak, pak_len, pak0, pak0_len);
			if (!sounds[i].weight) {
				sounds[i].cls = 3;
				sounds[i].weight = counts[k];
			}
		}
	}
	if (have[SG_TELE])                           /* the teleporters' fog: the last class */
		for (int k = 0; k < 5; k++) {
			int i = sound_add(TELE_SOUNDS[k], SG_TELE, pak, pak_len, pak0, pak0_len);
			sounds[i].cls = 4;
			sounds[i].weight = 1;
		}
	ents_free(&ents);
	/* a variant among several (a name ending in a digit, siblings in the
	   set: the deaths, the ricochets, a monster's idles...) is played a
	   fraction of the times its event is: its weight shrinks by the count,
	   so it leaves SPU RAM before the sound every event plays (§87.5); the
	   always set keeps its place */
	for (int i = 0; i < nsounds; i++) {
		size_t ln = strlen(sounds[i].name);
		if (sounds[i].cls == 0 || ln < 6 || sounds[i].name[ln - 5] < '0' || sounds[i].name[ln - 5] > '9')
			continue;
		int siblings = 0;
		for (int j = 0; j < nsounds; j++)
			siblings += strlen(sounds[j].name) == ln && !strncmp(sounds[j].name, sounds[i].name, ln - 5);
		if (siblings > 1)
			sounds[i].weight = sounds[i].weight / siblings > 0 ? sounds[i].weight / siblings : 1;
	}

	/* The split (§86): SPU RAM holds, beside the music's ring and a swap
	   room, as many as fit, the always set first, then the ambients, the
	   monsters' (the most numerous kind first), the entities' (the most
	   entities first); the rest go to main RAM, read with the level and
	   put in the swap room when wanted (engine/stream.c). The swap room
	   holds the biggest of them and 16 KB more; the split is made again
	   when it grows. */
	size_t ring = 48 * 1024;
	if (getenv("QCOOK_MUSIC_RING_KB"))
		ring = (size_t) atoi(getenv("QCOOK_MUSIC_RING_KB")) * 1024;
	int *order = xmalloc((size_t) (nsounds ? nsounds : 1) * sizeof *order);
	for (int i = 0; i < nsounds; i++)
		order[i] = i;
	for (int i = 1; i < nsounds; i++)            /* (insertion: by class, then weight down, then size up) */
		for (int j = i; j > 0; j--) {
			const bank_sound *x = &sounds[order[j - 1]], *y = &sounds[order[j]];
			int before = x->cls < y->cls || (x->cls == y->cls && (x->weight > y->weight ||
			             (x->weight == y->weight && sound_bytes(x) <= sound_bytes(y))));
			if (before)
				break;
			int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
		}
	/* The fogs and the powerups' sounds put out of the SPU are read from
	   the level's file when wanted, not kept in RAM (§98): a fog is played
	   among the ones in (engine/snd.c snd_tele_fog), a powerup's sounds are
	   wanted from the artifact's approach on (engine/items.c);
	   QCOOK_NO_DISC_SOUNDS=1: in RAM as before. */
	int disc_ok = !getenv("QCOOK_NO_DISC_SOUNDS");
	size_t swap = 32 * 1024, spu_budget = 0, in_spu = 0, in_ram = 0, in_disc = 0;
	int nram = 0, ndisc = 0;
	for (int pass = 0; pass < 6; pass++) {
		spu_budget = QS_SPU_BYTES - ring - swap;
		in_spu = in_ram = in_disc = 0;
		nram = ndisc = 0;
		size_t biggest_ram = 0, tele1 = 0, tele2 = 0;   /* (the two biggest teleporter fogs out: pinned) */
		for (int k = 0; k < nsounds; k++) {
			bank_sound *sn = &sounds[order[k]];
			size_t bytes = sound_bytes(sn);
			int out = sn->cls > 0 && in_spu + bytes > spu_budget;   /* (the always set never leaves the SPU) */
			sn->disc = out && disc_ok && (sn->cls == 4 || powerup_sound(sn->name));
			sn->ram = out && !sn->disc;
			if (!out) {
				in_spu += bytes;
				continue;
			}
			if (sn->disc) {
				in_disc += bytes;
				ndisc++;
			} else {
				in_ram += bytes;
				nram++;
			}
			if (sn->cls == 4) {
				if (bytes > tele1) { tele2 = tele1; tele1 = bytes; }
				else if (bytes > tele2) tele2 = bytes;
			} else if (bytes > biggest_ram) {
				biggest_ram = bytes;
			}
		}
		/* the swap room: the two pinned fogs, the biggest other sound out
		   (RAM's or the disc's), 16 KB of play */
		size_t need = tele1 + tele2 + biggest_ram + 16 * 1024;
		if ((!nram && !ndisc) || need <= swap)
			break;
		swap = need;
	}
	if (ndisc)
		printf("qcook: sounds — %d put out of the SPU read from the level's file when wanted, not kept in RAM "
		       "(%zu KB: the fogs, the powerups')\n", ndisc, in_disc / 1024);
	if (sndset_path) {
		FILE *sf = fopen(sndset_path, "w");
		if (!sf)
			die("cannot write %s", sndset_path);
		fprintf(sf, "swap %zu\n", nram || ndisc ? swap : 0);
		for (int k = 0; k < nsounds; k++)
			fprintf(sf, "%s %s\n", sounds[order[k]].ram ? "ram" : sounds[order[k]].disc ? "disc" : "spu",
			        sounds[order[k]].name);
		fclose(sf);
	}
	printf("qcook: sounds — the level's %d sounds, %zu KB: %d in the bank (%zu KB), %d in RAM (%zu KB), the swap "
	       "room %zu KB; the weapons held:", nsounds, (in_spu + in_ram + in_disc) / 1024, nsounds - nram - ndisc,
	       in_spu / 1024, nram, in_ram / 1024, nram || ndisc ? swap / 1024 : 0);
	for (int w = 0; w < 6; w++)
		if (weapons & (1u << w))
			printf(" %s,", WEAPON_NAMES[w]);
	printf(" shotgun, axe\n");
	int kept = 0;
	for (int i = 0; i < nsounds; i++) {          /* the bank: the SPU's sounds alone */
		if (sounds[i].ram || sounds[i].disc) {
			free(sounds[i].wav.s);
			continue;
		}
		if (kept != i)
			sounds[kept] = sounds[i];
		kept++;
	}
	nsounds = kept;
	free(order);

	/* the budget: halve the longest looping ambients until it fits; then
	   the sounds above 11 kHz (the rest's rate); then, biggest first, the
	   sounds outside the core set (items, doors, triggers: the rarer ones)
	   (a safety: the split above fits the budget) */
	size_t total = 0;
	for (int i = 0; i < nsounds; i++) total += sound_bytes(&sounds[i]);
	int halved = 0;
	for (int pass = 0; total > spu_budget; ) {
		int best = -1;
		for (int i = 0; i < nsounds; i++) {
			const bank_sound *sn = &sounds[i];
			if (sn->halved) continue;
			int ok = pass == 0 ? sn->wav.loopstart >= 0
			       : pass == 1 ? sn->wav.loopstart < 0 && sn->wav.rate > 11025
			       : sn->wav.loopstart < 0 && sn->group != SG_CORE;
			if (ok && (best < 0 || sn->wav.n > sounds[best].wav.n))
				best = i;
		}
		if (best < 0 && pass < 2) {
			pass++;
			continue;
		}
		if (best < 0) {
			for (int i = 0; i < nsounds; i++)
				fprintf(stderr, "  %6zu B %5d Hz %s%s\n", sound_bytes(&sounds[i]), sounds[i].wav.rate,
				        sounds[i].name, sounds[i].wav.loopstart >= 0 ? " (loop)" : "");
			die("the sounds do not fit the SPU (%zu KB of %zu KB)", total / 1024, spu_budget / 1024);
		}
		total -= sound_bytes(&sounds[best]);
		wav_halve(&sounds[best].wav);
		sounds[best].halved = 1;
		total += sound_bytes(&sounds[best]);
		halved++;
	}

	bank_write(statics, nstatics, cd_track, halved, out_path);
}

/* The release's menu bank (`qcook menu`): what the menu plays before any
   map is loaded, the clicks (M_Main_Key and the others' S_LocalSound), with
   CD track 2. Quake starts its demo loop under the menu (quake.rc:
   startdemos demo1 demo2 demo3); demo1.dem was recorded with a forced
   track, 2, on its first line (CL_PlayDemo_f's cls.forcetrack, which
   svc_cdtrack plays in place of E1M3's 9), so the theme plays under the
   menu until demo2 (E1M4: track 5). The port has no demos: the theme
   loops. */
void sound_build_menu(const uint8_t *pak0, long pak0_len, const char *out_path)
{
	static const char *const clicks[] = { "misc/menu1.wav", "misc/menu2.wav", "misc/menu3.wav" };
	nsounds = 0;
	for (size_t k = 0; k < sizeof clicks / sizeof clicks[0]; k++)
		sound_add(clicks[k], SG_CORE, pak0, pak0_len, NULL, 0);
	bank_write(NULL, 0, 2, 0, out_path);
}

/* ------------------------------------------------------------- streamed -- */

static int in_bank_set(const char *name)
{
	for (size_t k = 0; k < sizeof ALWAYS / sizeof ALWAYS[0]; k++)
		if (!strcmp(ALWAYS[k].name, name))
			return 1;
	return 0;
}

static void list_add(const char **list, int *n, int max, const char *name)
{
	if (in_bank_set(name))                       /* a map's bank has it */
		return;
	for (int i = 0; i < *n; i++)
		if (!strcmp(list[i], name))
			return;
	if (*n >= max)
		die("too many streamed sounds");
	list[(*n)++] = name;
}

int sound_streamed(const char **list, int max)
{
	int n = 0;
	for (size_t k = 0; k < sizeof AMBIENTS / sizeof AMBIENTS[0]; k++)
		list_add(list, &n, max, AMBIENTS[k].sound);
	for (size_t k = 0; k < sizeof ENT_SOUNDS / sizeof ENT_SOUNDS[0]; k++)
		list_add(list, &n, max, ENT_SOUNDS[k].sound);
	list_add(list, &n, max, "items/r_item1.wav");    /* item_health's rotten and mega */
	list_add(list, &n, max, "items/r_item2.wav");
	for (int w = 0; w < 3; w++)
		for (int k = 0; k < 2; k++)
			list_add(list, &n, max, KEY_DOOR_SOUNDS[w][k]);
	for (int k = 0; k < 5; k++)
		list_add(list, &n, max, TELE_SOUNDS[k]);
	return n;
}

/* The streamed sounds a map's entities use (the XA carousels' until
   docs/porting-notes.md §52): their spawn functions' precaches as the map's
   bank took them before the stream (the QC's defaults for `sounds`,
   item_health's by its flags, a key door's), the ambient sources'; what
   every bank has left out */
int sound_map_streamed(const ent_list *ents, int worldtype, const char **list, int max)
{
	return sound_map_streamed_n(ents, worldtype, list, NULL, max);
}

/* the same, with how many entities bring each name (counts[i] for list[i]) */
int sound_map_streamed_n(const ent_list *ents, int worldtype, const char **list, int *counts, int max)
{
	int n = 0;
#define ADD(name) do { int before = n; list_add(list, &n, max, name); \
	if (counts) { int at = -1; for (int q = 0; q < n && at < 0; q++) if (!strcmp(list[q], name)) at = q; \
		if (at >= 0) counts[at] = (n > before ? 0 : counts[at]) + 1; } } while (0)
	for (int i = 0; i < ents->num; i++) {
		const ent_rec *e = &ents->ents[i];
		const char *cls = e->classname;
		int spawnflags = (int) ent_float(ents, e, "spawnflags", 0);
		int sn = (int) ent_float(ents, e, "sounds", 0);
		for (size_t k = 0; k < sizeof AMBIENTS / sizeof AMBIENTS[0]; k++) {
			if (strcmp(cls, AMBIENTS[k].classname))
				continue;
			if (!strcmp(cls, "trigger_teleport") && (spawnflags & 1))   /* SILENT */
				continue;
			ADD(AMBIENTS[k].sound);
		}
		if (!strcmp(cls, "func_door_secret") && !sn) sn = 3;
		if (!strcmp(cls, "func_plat") && !sn) sn = 2;
		if (!strcmp(cls, "trigger_secret") && !sn) sn = 1;
		for (size_t k = 0; k < sizeof ENT_SOUNDS / sizeof ENT_SOUNDS[0]; k++) {
			const ent_sound_spec *sp = &ENT_SOUNDS[k];
			if (strcmp(cls, sp->classname))
				continue;
			if (sp->sounds != ANY && sp->sounds != sn)
				continue;
			if (sp->worldtype != ANY && sp->worldtype != worldtype)
				continue;
			const char *name = sp->sound;
			if (!strcmp(cls, "item_health"))          /* items.qc: rotten, mega, or plain */
				name = (spawnflags & H_ROTTEN) ? "items/r_item1.wav"
				     : (spawnflags & H_MEGA) ? "items/r_item2.wav" : "items/health1.wav";
			ADD(name);
		}
		if (!strcmp(cls, "func_door") && worldtype >= 0 && worldtype <= 2)   /* (doors.qc gives */
			for (int k = 0; k < 2; k++)                                     /* every door them) */
				ADD(KEY_DOOR_SOUNDS[worldtype][k]);
	}
#undef ADD
	return n;
}

int sound_is_banked(const char *name)
{
	return in_bank_set(name);
}

int sound_in_paks(const char *name, const uint8_t *pak, long pak_len, const uint8_t *pak0, long pak0_len)
{
	char path[80];
	snprintf(path, sizeof path, "sound/%s", name);
	uint32_t len;
	return pak_find(pak, pak_len, path, &len) || (pak0 && pak_find(pak0, pak0_len, path, &len));
}

void sound_encode_stream(const char *name, const uint8_t *pak, long pak_len, const uint8_t *pak0, long pak0_len,
                         buf_t *out, uint32_t *ticks_out)
{
	nsounds = 0;
	int i = sound_add(name, SG_CORE, pak, pak_len, pak0, pak0_len);
	bank_sound *sn = &sounds[i];
	adpcm_encode(&sn->wav, &sn->adpcm, &sn->snr);
	qst_sound h;
	memset(&h, 0, sizeof h);
	h.pitch = (uint16_t) ((sn->wav.rate * 4096 + 22050) / 44100);
	h.flags = sn->wav.loopstart >= 0 ? QSF_LOOP : 0;
	h.ticks = (uint32_t) ((sn->wav.n * 60 + sn->wav.rate - 1) / sn->wav.rate) + 1;
	h.len   = (uint32_t) sn->adpcm.len;
	buf_put(out, &h, sizeof h);
	buf_put(out, sn->adpcm.p, sn->adpcm.len);
	if (ticks_out)
		*ticks_out = h.ticks;
	free(sn->wav.s);
	free(sn->adpcm.p);
	nsounds = 0;
}
