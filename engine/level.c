/*
 * q1psx — client.qc's level end: trigger_changelevel, execute_changelevel,
 * FindIntermission, IntermissionThink, ExitIntermission, GotoNextMap.
 * See engine/level.h.
 */

#include <stdio.h>
#include <string.h>

#include "ents.h"
#include "game.h"
#include "hud.h"
#include "level.h"
#include "mem.h"
#include "music.h"
#include "q_fmt.h"
#include "render.h"
#include "snd.h"
#include "world.h"

int         level_intermission;
int32_t     level_completed_ticks;
const char *level_finale_text;
int32_t     level_finale_tick;
int32_t     level_start_tick;
int32_t     level_camera[3];
int         level_camera_angles[3];
int         game_serverflags;
int         game_skill = 1;

static int32_t intermission_exittime;
static char    nextmap[16];
static int     change_wanted;

#define NO_INTERMISSION 1
#define DEG(d) ((d) * ANG_TURN / 360)

int level_found_secrets(void)   { return ent_found_secrets; }
int level_total_secrets(void)   { return ent_total_secrets; }
int level_killed_monsters(void) { return ent_killed_monsters; }
int level_total_monsters(void)  { return ent_total_monsters; }

static char style0[32] = "m";

void level_lightstyle0(const char *s)
{
	q_snprintf(style0, sizeof style0, "%s", s);
}

int level_style0_dim(void)
{
	/* (the letter's light, ('x' - 'a') x 22 / 256, as the nearest power of
	   a half: a black, b 1/16 .. i 1/2, j and up as they are) */
	static const uint8_t dim[13] = { 5, 4, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0 };
	size_t n = strlen(style0);
	if (!n)
		return 0;
	int c = style0[(size_t) (game_tick_count / 6) % n] - 'a';   /* (cl.time x 10) */
	return c >= 0 && c < 13 ? dim[c] : 0;
}

void level_finale_begin(const edict_t *pos)
{
	level_intermission    = 2;
	intermission_exittime = 0x3fffffff;          /* (time + 10000000) */
	level_finale_text     = "";
	level_finale_tick     = game_tick_count;
	memcpy(level_camera, pos->origin, sizeof level_camera);
	for (int k = 0; k < 3; k++)
		level_camera_angles[k] = DEG(pos->mangle[k]);
	hud_centerprint_clear();
}

void level_finale_say(const char *text)
{
	level_finale_text = text ? text : "";
	level_finale_tick = game_tick_count;
}

void level_begin(void)
{
	level_intermission = 0;
	level_finale_text  = 0;
	style0[0] = 'm';
	style0[1] = 0;
	level_start_tick   = game_tick_count;
	change_wanted      = 0;
}

int level_change_wanted(char name[16])
{
	if (!change_wanted)
		return 0;
	change_wanted = 0;
	memcpy(name, nextmap, 16);
	return 1;
}

/* GotoNextMap: changelevel (nextmap), between frames */
static void goto_next_map(void)
{
	change_wanted = 1;
}

/* FindIntermission: a random info_intermission (of the first four), else
   the player start */
static edict_t *find_intermission(void)
{
	edict_t *spot = ent_find_class(0, QE_INFO_INTERMISSION);
	if (spot) {
		int cyc = (int) (q_rand() % 4000);           /* random () * 4, x1000 */
		while (cyc > 1000) {
			spot = ent_find_class(spot, QE_INFO_INTERMISSION);
			if (!spot)
				spot = ent_find_class(0, QE_INFO_INTERMISSION);
			cyc -= 1000;
		}
		return spot;
	}
	return ent_find_class(0, QE_INFO_PLAYER_START);
}

/* the stats and the camera: SVC_INTERMISSION */
Q_COLD void execute_changelevel(edict_t *self)
{
	(void) self;
	level_intermission    = 1;
	intermission_exittime = ent_time + 2 * GAME_HZ;
	level_completed_ticks = game_tick_count - level_start_tick;
	music_play(3);                                   /* SVC_CDTRACK 3 */
	edict_t *pos = find_intermission();
	if (pos) {
		memcpy(level_camera, pos->origin, sizeof level_camera);
		for (int k = 0; k < 3; k++)
			level_camera_angles[k] = DEG(pos->mangle[k]);
		if (pos->class == QE_INFO_PLAYER_START) {    /* (its view: the angle) */
			level_camera_angles[0] = level_camera_angles[2] = 0;
			level_camera_angles[1] = DEG(pos->angle);
		}
	} else {
		memcpy(level_camera, pl.pm.pos, sizeof level_camera);
		level_camera_angles[0] = level_camera_angles[2] = 0;
		level_camera_angles[1] = pl.pm.yaw;
	}
	hud_centerprint_clear();
	printf("level: intermission, %d secrets of %d, %d s\n", ent_found_secrets, ent_total_secrets,
	       level_completed_ticks / GAME_HZ);
}

void changelevel_touch(edict_t *self)
{
	if (ent_other != ent_player)
		return;
	q_snprintf(nextmap, sizeof nextmap, "%s", world_string(self->map));
	sub_use_targets(self);
	if (self->spawnflags & NO_INTERMISSION) {
		goto_next_map();
		return;
	}
	self->touch     = EF_sub_null;
	self->think     = EF_execute_changelevel;
	self->nextthink = ent_time + TICKS(10);
	ent_wake(self);
}

/* ExitIntermission: the stats, then an episode's end text (its own CD
   track), then the next map */
/* ExitIntermission: the end of an episode shows its text (SVC_FINALE, the
   CD's track 2); one more press, the registered game's four runes text
   when they are all there (the shareware game's sell screen is not
   reproduced: the next map at once); then GotoNextMap */
static void exit_intermission(void)
{
	intermission_exittime = ent_time + GAME_HZ;
	level_intermission++;
	if (level_intermission == 2) {
		const char *t = game_finale_text(world.map_name);
		if (t) {
			music_play(2);                           /* SVC_CDTRACK 2 */
			level_finale_text = t;
			level_finale_tick = game_tick_count;
			return;
		}
		goto_next_map();
		return;
	}
	if (level_intermission == 3 && world_registered() && (game_serverflags & 15) == 15) {
		const char *t = game_finale_text(0);
		if (t) {
			level_finale_text = t;
			level_finale_tick = game_tick_count;
			return;
		}
	}
	goto_next_map();
}

void level_think(const game_cmd_t *cmd)
{
	if (ent_time < intermission_exittime)
		return;
	if (!cmd->fire && !cmd->jump)
		return;
	if (change_wanted)
		return;
	exit_intermission();
}

Q_COLD void level_spawn(edict_t *e, const qw_ent *q)
{
	(void) q;
	switch (e->class) {
	case QE_TRIGGER_CHANGELEVEL:                     /* trigger_changelevel */
		if (!e->map) {
			printf("level: changelevel trigger without a map\n");
			ent_remove(e);
			return;
		}
		init_trigger(e);
		e->touch  = EF_changelevel_touch;
		e->flags |= FL_TOUCH_PLAYER;
		break;
	default:                                         /* info_intermission: a spot */
		e->solid    = SOLID_NOT;
		e->movetype = MOVETYPE_NONE;
		break;
	}
}
