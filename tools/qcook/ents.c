/*
 * q1psx — the entity lump: parsed once (ents_parse) for the world's entity
 * table (ents_cook: formats/qworld.h qw_ent), the sound bank's entity-driven
 * sounds (sound.c) and the player start. Keys and values stay strings; the
 * accessors convert like Quake's ED_ParseEdict / ED_NewString ("\n" in a
 * value is a newline). The entities single player never spawns are left
 * out of the list (ent_sp_never): the port is single player only, so
 * nothing is cooked for them.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qworld.h"

/* ---------------------------------------------------------------- parse -- */

static void list_add(ent_list *l, int ent, const char *key, const char *val)
{
	if (l->num_kv == l->cap_kv) {
		l->cap_kv = l->cap_kv ? 2 * l->cap_kv : 1024;
		l->kv = realloc(l->kv, (size_t) l->cap_kv * sizeof *l->kv);
		if (!l->kv) die("out of memory (entities)");
	}
	ent_kv *kv = &l->kv[l->num_kv++];
	kv->key = strdup(key);
	kv->val = strdup(val);
	if (!kv->key || !kv->val) die("out of memory (entities)");
	l->ents[ent].num_kv++;
}

/* An entity single player never spawns, coop and deathmatch's only:
   their players' starts (client.qc SelectSpawnPoint looks for them in
   coop or deathmatch only), and what is on no skill (all three skill
   flags, not easy 256, not medium 512, not hard 1024: ED_LoadFromFile
   inhibits it at every skill; deathmatch alone ignores them). */
static int ent_sp_never(const ent_list *l, const ent_rec *e)
{
	if (!strcmp(e->classname, "info_player_coop") || !strcmp(e->classname, "info_player_deathmatch"))
		return 1;
	long flags = lround(ent_float(l, e, "spawnflags", 0));
	return (flags & (256 | 512 | 1024)) == (256 | 512 | 1024);
}

void ents_parse(const bsp_t *b, ent_list *l)
{
	memset(l, 0, sizeof *l);
	const char *p = b->entities, *end = p + b->ent_len;
	while (p < end) {
		while (p < end && *p != '{') p++;
		if (p >= end) break;
		p++;
		if (l->num == l->cap) {
			l->cap = l->cap ? 2 * l->cap : 256;
			l->ents = realloc(l->ents, (size_t) l->cap * sizeof *l->ents);
			if (!l->ents) die("out of memory (entities)");
		}
		ent_rec *e = &l->ents[l->num++];
		e->first_kv  = l->num_kv;
		e->num_kv    = 0;
		e->classname = "";
		while (p < end && *p != '}') {
			char key[64], val[1024];
			int i;
			while (p < end && *p != '"' && *p != '}') p++;
			if (p >= end || *p == '}') break;
			for (i = 0, p++; p < end && *p != '"'; p++)
				if (i < 63) key[i++] = *p;
			key[i] = 0;
			if (p < end) p++;
			while (p < end && *p != '"' && *p != '}') p++;
			if (p >= end || *p == '}') break;
			for (i = 0, p++; p < end && *p != '"'; p++)
				if (i < 1023) val[i++] = *p;
			val[i] = 0;
			if (p < end) p++;
			list_add(l, l->num - 1, key, val);
		}
		if (p < end) p++;
		for (int k = 0; k < e->num_kv; k++)
			if (!strcmp(l->kv[e->first_kv + k].key, "classname"))
				e->classname = l->kv[e->first_kv + k].val;
		if (ent_sp_never(l, e)) {                 /* (its keys and values dropped too) */
			for (int k = e->first_kv; k < l->num_kv; k++) {
				free(l->kv[k].key);
				free(l->kv[k].val);
			}
			l->num_kv = e->first_kv;
			l->num--;
			l->sp_never++;
		}
	}
}

void ents_free(ent_list *l)
{
	for (int i = 0; i < l->num_kv; i++) {
		free(l->kv[i].key);
		free(l->kv[i].val);
	}
	free(l->kv);
	free(l->ents);
	memset(l, 0, sizeof *l);
}

const char *ent_value(const ent_list *l, const ent_rec *e, const char *key)
{
	for (int k = 0; k < e->num_kv; k++)
		if (!strcmp(l->kv[e->first_kv + k].key, key))
			return l->kv[e->first_kv + k].val;
	return NULL;
}

double ent_float(const ent_list *l, const ent_rec *e, const char *key, double def)
{
	const char *v = ent_value(l, e, key);
	return v ? atof(v) : def;
}

int ent_vector(const ent_list *l, const ent_rec *e, const char *key, double v[3])
{
	const char *s = ent_value(l, e, key);
	return s && sscanf(s, "%lf %lf %lf", &v[0], &v[1], &v[2]) == 3;
}

/* ----------------------------------------------------------------- cook -- */

static const struct { const char *name; int cls; } CLASSES[] = {
	{ "info_player_start", QE_INFO_PLAYER_START },
	{ "info_player_start2", QE_INFO_PLAYER_START2 },
	{ "info_teleport_destination", QE_INFO_TELEPORT_DESTINATION },
	{ "info_intermission", QE_INFO_INTERMISSION },
	{ "info_notnull", QE_INFO_NOTNULL },
	{ "func_door", QE_FUNC_DOOR },
	{ "func_door_secret", QE_FUNC_DOOR_SECRET },
	{ "func_button", QE_FUNC_BUTTON },
	{ "func_plat", QE_FUNC_PLAT },
	{ "func_train", QE_FUNC_TRAIN },
	{ "func_wall", QE_FUNC_WALL },
	{ "func_illusionary", QE_FUNC_ILLUSIONARY },
	{ "func_episodegate", QE_FUNC_EPISODEGATE },
	{ "func_bossgate", QE_FUNC_BOSSGATE },
	{ "trigger_once", QE_TRIGGER_ONCE },
	{ "trigger_multiple", QE_TRIGGER_MULTIPLE },
	{ "trigger_secret", QE_TRIGGER_SECRET },
	{ "trigger_relay", QE_TRIGGER_RELAY },
	{ "trigger_counter", QE_TRIGGER_COUNTER },
	{ "trigger_teleport", QE_TRIGGER_TELEPORT },
	{ "trigger_changelevel", QE_TRIGGER_CHANGELEVEL },
	{ "trigger_setskill", QE_TRIGGER_SETSKILL },
	{ "trigger_onlyregistered", QE_TRIGGER_ONLYREGISTERED },
	{ "trigger_push", QE_TRIGGER_PUSH },
	{ "trigger_hurt", QE_TRIGGER_HURT },
	{ "trigger_monsterjump", QE_TRIGGER_MONSTERJUMP },
	{ "path_corner", QE_PATH_CORNER },
	{ "misc_fireball", QE_MISC_FIREBALL },
	{ "misc_explobox", QE_MISC_EXPLOBOX },
	{ "misc_explobox2", QE_MISC_EXPLOBOX2 },
	{ "misc_teleporttrain", QE_MISC_TELEPORTTRAIN },
	{ "trap_spikeshooter", QE_TRAP_SPIKESHOOTER },
	{ "trap_shooter", QE_TRAP_SHOOTER },
	{ "air_bubbles", QE_AIR_BUBBLES },
	{ "event_lightning", QE_EVENT_LIGHTNING },
	{ "light", QE_LIGHT },
	{ "light_fluoro", QE_LIGHT_FLUORO },
	{ "light_fluorospark", QE_LIGHT_FLUOROSPARK },
	{ "light_globe", QE_LIGHT_GLOBE },
	{ "light_torch_small_walltorch", QE_LIGHT_TORCH_SMALL_WALLTORCH },
	{ "light_flame_large_yellow", QE_LIGHT_FLAME_LARGE_YELLOW },
	{ "light_flame_small_yellow", QE_LIGHT_FLAME_SMALL_YELLOW },
	{ "light_flame_small_white", QE_LIGHT_FLAME_SMALL_WHITE },
	{ "item_health", QE_ITEM_HEALTH },
	{ "item_armor1", QE_ITEM_ARMOR1 },
	{ "item_armor2", QE_ITEM_ARMOR2 },
	{ "item_armorInv", QE_ITEM_ARMORINV },
	{ "item_shells", QE_ITEM_SHELLS },
	{ "item_spikes", QE_ITEM_SPIKES },
	{ "item_rockets", QE_ITEM_ROCKETS },
	{ "item_cells", QE_ITEM_CELLS },
	{ "item_key1", QE_ITEM_KEY1 },
	{ "item_key2", QE_ITEM_KEY2 },
	{ "item_sigil", QE_ITEM_SIGIL },
	{ "item_artifact_invulnerability", QE_ITEM_ARTIFACT_INVULNERABILITY },
	{ "item_artifact_envirosuit", QE_ITEM_ARTIFACT_ENVIROSUIT },
	{ "item_artifact_invisibility", QE_ITEM_ARTIFACT_INVISIBILITY },
	{ "item_artifact_super_damage", QE_ITEM_ARTIFACT_SUPER_DAMAGE },
	{ "item_weapon", QE_ITEM_WEAPON },
	{ "weapon_supershotgun", QE_WEAPON_SUPERSHOTGUN },
	{ "weapon_nailgun", QE_WEAPON_NAILGUN },
	{ "weapon_supernailgun", QE_WEAPON_SUPERNAILGUN },
	{ "weapon_grenadelauncher", QE_WEAPON_GRENADELAUNCHER },
	{ "weapon_rocketlauncher", QE_WEAPON_ROCKETLAUNCHER },
	{ "weapon_lightning", QE_WEAPON_LIGHTNING },
	{ "monster_army", QE_MONSTER_ARMY },
	{ "monster_dog", QE_MONSTER_DOG },
	{ "monster_ogre", QE_MONSTER_OGRE },
	{ "monster_ogre_marksman", QE_MONSTER_OGRE_MARKSMAN },
	{ "monster_knight", QE_MONSTER_KNIGHT },
	{ "monster_zombie", QE_MONSTER_ZOMBIE },
	{ "monster_wizard", QE_MONSTER_WIZARD },
	{ "monster_demon1", QE_MONSTER_DEMON1 },
	{ "monster_shambler", QE_MONSTER_SHAMBLER },
	{ "monster_boss", QE_MONSTER_BOSS },
	{ "monster_enforcer", QE_MONSTER_ENFORCER },
	{ "monster_hell_knight", QE_MONSTER_HELL_KNIGHT },
	{ "monster_shalrath", QE_MONSTER_SHALRATH },
	{ "monster_tarbaby", QE_MONSTER_TARBABY },
	{ "monster_fish", QE_MONSTER_FISH },
	{ "monster_oldone", QE_MONSTER_OLDONE },
};

void ents_drawn_models(const bsp_t *b, uint8_t *drawn)
{
	ent_list l;
	ents_parse(b, &l);
	memset(drawn, 0, (size_t) b->num_models);
	drawn[0] = 1;
	for (int i = 0; i < l.num; i++) {
		const ent_rec *e = &l.ents[i];
		const char *m = ent_value(&l, e, "model");
		if (!m || m[0] != '*' || !strncmp(e->classname, "trigger_", 8))
			continue;
		int idx = atoi(m + 1);
		if (idx > 0 && idx < b->num_models)
			drawn[idx] = 1;
	}
	ents_free(&l);
}

void ents_used_models(const ent_list *l, int num_models, uint8_t *used)
{
	memset(used, 0, (size_t) num_models);
	used[0] = 1;
	for (int i = 0; i < l->num; i++) {
		const char *m = ent_value(l, &l->ents[i], "model");
		if (m && m[0] == '*' && atoi(m + 1) > 0 && atoi(m + 1) < num_models)
			used[atoi(m + 1)] = 1;
	}
}

int ent_class(const char *classname)
{
	for (size_t i = 0; i < sizeof CLASSES / sizeof CLASSES[0]; i++)
		if (!strcmp(CLASSES[i].name, classname))
			return CLASSES[i].cls;
	return QE_UNKNOWN;
}

/* the string pool: deduplicated, "" at 0, Quake's "\n" made a newline */
static uint16_t str_add(buf_t *pool, const char *s)
{
	if (!s || !*s)
		return 0;
	char tmp[1024];
	size_t n = 0;
	for (size_t i = 0; s[i] && n < sizeof tmp - 1; i++) {
		if (s[i] == '\\' && s[i + 1] == 'n') {
			tmp[n++] = '\n';
			i++;
		} else {
			tmp[n++] = s[i];
		}
	}
	tmp[n] = 0;
	if (!pool->len)
		buf_put(pool, "", 1);
	for (size_t at = 1; at < pool->len; ) {
		size_t l = strlen((const char *) pool->p + at);
		if (!strcmp((const char *) pool->p + at, tmp))
			return (uint16_t) at;
		at += l + 1;
	}
	if (pool->len + n + 1 > 65535)
		die("more than 64 KB of entity strings");
	size_t at = pool->len;
	buf_put(pool, tmp, n + 1);
	return (uint16_t) at;
}

static int16_t i16(double v, const char *what)
{
	long r = lround(v);
	if (r < -32768 || r > 32767)
		die("entity %s %ld does not fit 16 bits", what, r);
	return (int16_t) r;
}

/* Which entities the table keeps: not worldspawn (its fields go to the
   header), not info_null and lights without a targetname (Quake removes
   them at spawn), not the ambient_* sounds (the sound bank places them).
   (Those single player never spawns are not in the list: ents_parse.) */
static int ent_kept(const ent_list *l, const ent_rec *e)
{
	const char *c = e->classname;
	if (!strcmp(c, "worldspawn") || !strcmp(c, "info_null") || !strncmp(c, "ambient_", 8))
		return 0;
	if (!strcmp(c, "light") && !ent_value(l, e, "targetname"))
		return 0;
	return 1;
}

void ents_cook(const ent_list *l, int num_models, buf_t *table, buf_t *strings,
               uint32_t *worldtype, uint32_t *title, int *unknown)
{
	*worldtype = 0;
	*title     = 0;
	*unknown   = 0;
	buf_put(strings, "", 1);
	for (int i = 0; i < l->num; i++) {
		const ent_rec *e = &l->ents[i];
		if (!strcmp(e->classname, "worldspawn")) {
			*worldtype = (uint32_t) lround(ent_float(l, e, "worldtype", 0));
			*title     = str_add(strings, ent_value(l, e, "message"));
			continue;
		}
		if (!ent_kept(l, e))
			continue;
		qw_ent q;
		memset(&q, 0, sizeof q);
		q.class = (uint8_t) ent_class(e->classname);
		if (q.class == QE_UNKNOWN)
			(*unknown)++;
		q.classname  = str_add(strings, e->classname);
		q.sounds     = (uint8_t) lround(ent_float(l, e, "sounds", 0));
		q.spawnflags = (uint16_t) lround(ent_float(l, e, "spawnflags", 0));
		const char *m = ent_value(l, e, "model");
		if (m) {
			if (m[0] != '*' || atoi(m + 1) <= 0 || atoi(m + 1) >= num_models)
				die("entity %d (%s): model \"%s\" is not a brush model", i, e->classname, m);
			q.model = (int16_t) atoi(m + 1);
		}
		q.angle = i16(ent_float(l, e, "angle", 0), "angle");
		double v[3];
		if (ent_vector(l, e, "origin", v))
			for (int k = 0; k < 3; k++)
				q.origin[k] = (int32_t) lround(v[k] * QW_CLIP_SUB);
		if (ent_vector(l, e, "mangle", v))
			for (int k = 0; k < 3; k++)
				q.mangle[k] = i16(v[k], "mangle");
		q.speed  = i16(ent_float(l, e, "speed", 0), "speed");
		q.wait   = i16(ent_float(l, e, "wait", 0) * 100, "wait");
		q.delay  = i16(ent_float(l, e, "delay", 0) * 100, "delay");
		q.lip    = i16(ent_float(l, e, "lip", 0), "lip");
		q.height = i16(ent_float(l, e, "height", 0), "height");
		q.dmg    = i16(ent_float(l, e, "dmg", 0), "dmg");
		q.health = i16(ent_float(l, e, "health", 0), "health");
		q.count  = i16(ent_float(l, e, "count", 0), "count");
		q.style  = i16(ent_float(l, e, "style", 0), "style");
		q.target     = str_add(strings, ent_value(l, e, "target"));
		q.targetname = str_add(strings, ent_value(l, e, "targetname"));
		q.killtarget = str_add(strings, ent_value(l, e, "killtarget"));
		q.message    = str_add(strings, ent_value(l, e, "message"));
		q.map        = str_add(strings, ent_value(l, e, "map"));
		buf_put(table, &q, sizeof q);
	}
}

/* the compact table (formats/qworld.h qw_ent_decode): each class's name,
   then each entity's head and the fields that are not zero; every entity
   decoded again and checked against its qw_ent, byte for byte */
void ents_pack(const qw_ent *ents, uint32_t n, buf_t *out)
{
	uint16_t names[QE_COUNT];
	memset(names, 0, sizeof names);
	for (uint32_t i = 0; i < n; i++) {
		const qw_ent *q = &ents[i];
		if (q->class && names[q->class] && names[q->class] != q->classname)
			die("entity %u: class %u under two names", i, q->class);
		if (q->class)
			names[q->class] = q->classname;
	}
	buf_put(out, names, sizeof names);
	for (uint32_t i = 0; i < n; i++) {
		const qw_ent *q = &ents[i];
		const uint8_t *s = (const uint8_t *) q;
		uint8_t rec[4 + sizeof *q];
		uint32_t len = 4, head = q->class, fine = 0;
		for (int k = 0; k < 3; k++)
			fine |= q->origin[k] % QW_CLIP_SUB || q->origin[k] / QW_CLIP_SUB < -32768 ||
			        q->origin[k] / QW_CLIP_SUB > 32767;
		if (fine)
			head |= QWE_BIT(QWE_FINE);
		for (uint32_t f = 0; f < QWE_FIELDS; f++) {
			uint32_t at = QWE_FIELD_AT[f][0], nb = QWE_FIELD_AT[f][1], any = 0;
			if (f <= QWE_ORIGIN_Z) {                      /* (the origin: fields 0-2) */
				int32_t v = q->origin[f - QWE_ORIGIN_X];
				if (!v)
					continue;
				head |= QWE_BIT(f);
				if (!fine) {
					int16_t w = (int16_t) (v / QW_CLIP_SUB);
					memcpy(rec + len, &w, 2);
					len += 2;
				} else {
					memcpy(rec + len, &v, 4);
					len += 4;
				}
				continue;
			}
			for (uint32_t k = 0; k < nb; k++)
				any |= s[at + k];
			if (!any)
				continue;
			head |= QWE_BIT(f);
			memcpy(rec + len, s + at, nb);
			len += nb;
		}
		if (!q->class) {
			memcpy(rec + len, &q->classname, 2);
			len += 2;
		}
		memcpy(rec, &head, 4);
		qw_ent back;
		if (qw_ent_decode(rec, names, &back) != rec + len || memcmp(&back, q, sizeof back))
			die("entity %u (class %u): its compact form decodes otherwise", i, q->class);
		buf_put(out, rec, len);
	}
}
