/*
 * q1psx — sound: Quake's snd_dma.c on the SPU.
 *
 * The map's sound bank (formats/qsound.h) sits in SPU RAM; a Quake channel
 * is an SPU voice: 2 for the area ambients (water, wind), 8 dynamic ones
 * (S_StartSound: picked and spatialized as snd_dma.c does), and one per
 * distinct static sound (the map's ambient entities: Quake sums the volumes
 * of every static source of one sound into a single channel, "so we don't
 * mix five torches every frame"; here that channel is a looping voice).
 *
 * Positions are the game's (1/R_POS_SUB world units); volumes 0..255
 * (Quake's fvol x 255); attenuations x64 (ATTN_NORM 1 -> 64); entity
 * numbers: the player is ENT_PLAYER (never spatialized, cl.viewentity),
 * temp entities ENT_TEMP, projectiles ENT_PROJ + their index.
 */

#ifndef Q1PSX_SND_H
#define Q1PSX_SND_H

#include <stdint.h>

/* the named sounds the game code plays (resolved in the bank by name) */
enum {
	S_AX1, S_AXHIT1, S_AXHIT2, S_GUNCOCK, S_SHOTGN2, S_ROCKET1I, S_SPIKE2, S_GRENADE,
	S_BOUNCE, S_SGUN1, S_R_EXP3, S_LSTART, S_LHIT, S_TINK1, S_RIC1, S_RIC2, S_RIC3,
	S_PLYRJMP8, S_LAND, S_LAND2, S_H2OJUMP,
	S_PAIN1, S_PAIN2, S_PAIN3, S_PAIN4, S_PAIN5, S_PAIN6,
	S_DEATH1, S_DEATH2, S_DEATH3, S_DEATH4, S_DEATH5, S_H2ODEATH,
	S_DROWN1, S_DROWN2, S_LBURN1, S_LBURN2, S_GASP1, S_GASP2,
	S_INH2O, S_INLAVA, S_SLIMBRN2, S_OUTWATER, S_WATER1, S_WATER2, S_H2OHIT1,
	S_GIB, S_UDEATH, S_TORNOFF2, S_TELEDTH1,
	S_R_TELE1, S_R_TELE2, S_R_TELE3, S_R_TELE4, S_R_TELE5,
	S_MENU1, S_MENU2, S_MENU3, S_TALK,
	S_ITEMBK2, S_LOCK4, S_PKUP, S_ARMOR1, S_DAMAGE3, S_POWER, S_DLAND2,
	S_AMB_WATER1, S_AMB_WIND2,
	S_COUNT
};

#define CHAN_AUTO   0
#define CHAN_WEAPON 1
#define CHAN_VOICE  2
#define CHAN_ITEM   3
#define CHAN_BODY   4

#define ATTN_NONE   0
#define ATTN_NORM   64
#define ATTN_IDLE   128
#define ATTN_STATIC 192

#define ENT_TEMP   -1
#define ENT_WORLD   0
#define ENT_PLAYER  1
#define ENT_PROJ    64

void snd_init(void);

/* the map's bank: MAPS/<MAP>.SND into SPU RAM (through the level arena,
   which it resets: call it before the world loads). 0 if there is none. */
int  snd_load_bank(const char *map);

/* the map starts: channels cleared, the static sounds keyed on */
void snd_map_start(void);

/* S_StartSound; `sfx` an S_* id (missing from this map's bank: nothing) */
void snd_start(int entnum, int entchannel, int sfx, const int32_t origin[3], int vol, int atten);
void snd_stop_all(void);

/* once per frame, after the game ticks: the listener (the camera) and
   `ticks` elapsed since the last update; respatializes every channel */
void snd_update(const int32_t origin[3], int yaw, int pitch, int roll, int ticks);

/* the bank's CD track (worldspawn "sounds"), 0 if none */
int  snd_cd_track(void);

/* the bench: print every start ("snd ..." lines) */
extern int snd_log;

#endif
