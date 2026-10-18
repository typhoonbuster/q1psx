/*
 * qcook — the streamed assets: `qcook stream --pak <pak0.pak> [--pak1
 * <pak1.pak>] --out <stream.dat>` (formats/qstream.h).
 *
 * The models the maps' monsters bring, each with what its QC precaches
 * beside it (its head, its projectiles, its sounds), the gibs every monster
 * throws and the backpack a soldier drops; the sounds with a place in the
 * maps (the doors', plats', buttons', items', traps', the ambient
 * entities': tools/qcook/sound.c sound_streamed). A monster's assets form a
 * group, laid out next to each other so that the engine reads them in one
 * go. Skins at half their resolution (the view models' and the monsters':
 * the screen never shows them larger), a quarter for what is small (the
 * gibs, the projectiles, the backpack); sounds at their full rate. The
 * registered game's monsters come from pak1 when it is given.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qsound.h"
#include "../../formats/qstream.h"

typedef struct {
	const char *file;
	int         group;      /* the engine reads a group's wanted assets together */
	int         div;        /* the skin's resolution: 1 / div             */
	int         flat;       /* no directional light (QGF_FLAT)            */
	int         pak1;       /* the registered game's                      */
	int         skin;       /* its skin (the asset's name: "<file>#<skin>" past 0) */
	int         item;       /* an item's: skipped when the paks lack it (the
	                           registered game's keys and runes)            */
} stream_model;

enum {
	G_GIBS = 1, G_BACKPACK, G_ARMY, G_DOG, G_OGRE, G_KNIGHT, G_ZOMBIE, G_WIZARD, G_DEMON,
	G_SHAMBLER, G_BOSS, G_ITEMS, G_WORLD
};

static const stream_model MODELS[] = {
	{ "progs/gib1.mdl",     G_GIBS,     4, 0, 0, 0, 0 },     /* player.qc ThrowGib: every monster */
	{ "progs/gib2.mdl",     G_GIBS,     4, 0, 0, 0, 0 },
	{ "progs/gib3.mdl",     G_GIBS,     4, 0, 0, 0, 0 },
	{ "progs/backpack.mdl", G_BACKPACK, 4, 0, 0, 0, 0 },     /* items.qc DropBackpack */
	{ "progs/soldier.mdl",  G_ARMY,     2, 0, 0, 0, 0 },     /* monster_army */
	{ "progs/h_guard.mdl",  G_ARMY,     4, 0, 0, 0, 0 },
	{ "progs/dog.mdl",      G_DOG,      2, 0, 0, 0, 0 },     /* monster_dog */
	{ "progs/h_dog.mdl",    G_DOG,      4, 0, 0, 0, 0 },
	{ "progs/ogre.mdl",     G_OGRE,     2, 0, 0, 0, 0 },     /* monster_ogre (its grenade: GAME.DAT) */
	{ "progs/h_ogre.mdl",   G_OGRE,     4, 0, 0, 0, 0 },
	{ "progs/knight.mdl",   G_KNIGHT,   2, 0, 0, 0, 0 },     /* monster_knight */
	{ "progs/h_knight.mdl", G_KNIGHT,   4, 0, 0, 0, 0 },
	{ "progs/zombie.mdl",   G_ZOMBIE,   2, 0, 0, 0, 0 },     /* monster_zombie */
	{ "progs/h_zombie.mdl", G_ZOMBIE,   4, 0, 0, 0, 0 },
	{ "progs/zom_gib.mdl",  G_ZOMBIE,   4, 0, 0, 0, 0 },
	{ "progs/wizard.mdl",   G_WIZARD,   2, 0, 0, 0, 0 },     /* monster_wizard */
	{ "progs/h_wizard.mdl", G_WIZARD,   4, 0, 0, 0, 0 },
	{ "progs/w_spike.mdl",  G_WIZARD,   4, 1, 0, 0, 0 },
	{ "progs/demon.mdl",    G_DEMON,    2, 0, 0, 0, 0 },     /* monster_demon1 */
	{ "progs/h_demon.mdl",  G_DEMON,    4, 0, 0, 0, 0 },
	{ "progs/shambler.mdl", G_SHAMBLER, 2, 0, 0, 0, 0 },     /* monster_shambler */
	{ "progs/h_shams.mdl",  G_SHAMBLER, 4, 0, 0, 0, 0 },
	{ "progs/s_light.mdl",  G_SHAMBLER, 4, 1, 0, 0, 0 },
	{ "progs/bolt.mdl",     G_SHAMBLER, 4, 1, 0, 0, 0 },
	{ "progs/boss.mdl",     G_BOSS,     2, 0, 0, 0, 0 },     /* monster_boss */
	{ "progs/lavaball.mdl", G_BOSS,     4, 1, 0, 0, 0 },
	{ "progs/bolt3.mdl",    G_BOSS,     4, 1, 0, 0, 0 },     /* event_lightning's beam */
	/* the items' models (items.qc, misc.qc: the map keeps the boxes, BSP
	   models over its own textures): a quarter of their skins' resolution,
	   the flames a half (fullbright: flat) */
	{ "progs/g_shot.mdl",   G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/g_nail.mdl",   G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/g_nail2.mdl",  G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/g_rock.mdl",   G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/g_rock2.mdl",  G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/g_light.mdl",  G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/armor.mdl",    G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/armor.mdl",    G_ITEMS,    4, 0, 0, 1, 1 },
	{ "progs/armor.mdl",    G_ITEMS,    4, 0, 0, 2, 1 },
	{ "progs/w_s_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/w_g_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/m_s_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/m_g_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/b_s_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/b_g_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/end1.mdl",     G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/end2.mdl",     G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/end3.mdl",     G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/end4.mdl",     G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/invulner.mdl", G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/suit.mdl",     G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/invisibl.mdl", G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/quaddama.mdl", G_ITEMS,    4, 0, 0, 0, 1 },
	{ "progs/flame.mdl",    G_ITEMS,    2, 1, 0, 0, 1 },
	{ "progs/flame2.mdl",   G_ITEMS,    2, 1, 0, 0, 1 },
	{ "progs/teleport.mdl", G_ITEMS,    4, 1, 0, 0, 1 },
};
#define NUM_MODELS (int) (sizeof MODELS / sizeof MODELS[0])

/* the monsters' sounds: their spawn functions' precaches (1.06), less the
   ones every map's bank has */
static const struct { const char *file; int group; } MONSTER_SOUNDS[] = {
	{ "soldier/death1.wav",  G_ARMY },     { "soldier/idle.wav",    G_ARMY },
	{ "soldier/pain1.wav",   G_ARMY },     { "soldier/pain2.wav",   G_ARMY },
	{ "soldier/sattck1.wav", G_ARMY },     { "soldier/sight1.wav",  G_ARMY },
	{ "dog/dattack1.wav",    G_DOG },      { "dog/ddeath.wav",      G_DOG },
	{ "dog/dpain1.wav",      G_DOG },      { "dog/dsight.wav",      G_DOG },
	{ "dog/idle.wav",        G_DOG },
	{ "ogre/ogdrag.wav",     G_OGRE },     { "ogre/ogdth.wav",      G_OGRE },
	{ "ogre/ogidle.wav",     G_OGRE },     { "ogre/ogidle2.wav",    G_OGRE },
	{ "ogre/ogpain1.wav",    G_OGRE },     { "ogre/ogsawatk.wav",   G_OGRE },
	{ "ogre/ogwake.wav",     G_OGRE },
	{ "knight/kdeath.wav",   G_KNIGHT },   { "knight/khurt.wav",    G_KNIGHT },
	{ "knight/ksight.wav",   G_KNIGHT },   { "knight/sword1.wav",   G_KNIGHT },
	{ "knight/sword2.wav",   G_KNIGHT },   { "knight/idle.wav",     G_KNIGHT },
	{ "zombie/z_idle.wav",   G_ZOMBIE },   { "zombie/z_idle1.wav",  G_ZOMBIE },
	{ "zombie/z_shot1.wav",  G_ZOMBIE },   { "zombie/z_gib.wav",    G_ZOMBIE },
	{ "zombie/z_pain.wav",   G_ZOMBIE },   { "zombie/z_pain1.wav",  G_ZOMBIE },
	{ "zombie/z_fall.wav",   G_ZOMBIE },   { "zombie/z_miss.wav",   G_ZOMBIE },
	{ "zombie/z_hit.wav",    G_ZOMBIE },   { "zombie/idle_w2.wav",  G_ZOMBIE },
	{ "wizard/hit.wav",      G_WIZARD },   { "wizard/wattack.wav",  G_WIZARD },
	{ "wizard/wdeath.wav",   G_WIZARD },   { "wizard/widle1.wav",   G_WIZARD },
	{ "wizard/widle2.wav",   G_WIZARD },   { "wizard/wpain.wav",    G_WIZARD },
	{ "wizard/wsight.wav",   G_WIZARD },
	{ "demon/ddeath.wav",    G_DEMON },    { "demon/dhit2.wav",     G_DEMON },
	{ "demon/djump.wav",     G_DEMON },    { "demon/dpain1.wav",    G_DEMON },
	{ "demon/idle1.wav",     G_DEMON },    { "demon/sight2.wav",    G_DEMON },
	{ "shambler/sattck1.wav", G_SHAMBLER }, { "shambler/sboom.wav",  G_SHAMBLER },
	{ "shambler/sdeath.wav", G_SHAMBLER }, { "shambler/shurt2.wav", G_SHAMBLER },
	{ "shambler/sidle.wav",  G_SHAMBLER }, { "shambler/ssight.wav", G_SHAMBLER },
	{ "shambler/melee1.wav", G_SHAMBLER }, { "shambler/melee2.wav", G_SHAMBLER },
	{ "shambler/smack.wav",  G_SHAMBLER },
	{ "boss1/out1.wav",      G_BOSS },     { "boss1/sight1.wav",    G_BOSS },
	{ "boss1/throw.wav",     G_BOSS },     { "boss1/pain.wav",      G_BOSS },
	{ "boss1/death.wav",     G_BOSS },
};
#define NUM_MONSTER_SOUNDS (int) (sizeof MONSTER_SOUNDS / sizeof MONSTER_SOUNDS[0])
#define MAX_WORLD_SOUNDS 96

typedef struct {
	qst_asset a;
	size_t    ofs, len;     /* in the data buffer */
} entry;

static int by_hash(const void *x, const void *y)
{
	const entry *a = x, *b = y;
	return a->a.hash < b->a.hash ? -1 : a->a.hash > b->a.hash;
}

int cmd_stream(int argc, char **argv)
{
	const char *pak_path = NULL, *pak1_path = NULL, *out_path = NULL;
	for (int i = 2; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--pak"))  pak_path  = argv[i + 1];
		if (!strcmp(argv[i], "--pak1")) pak1_path = argv[i + 1];
		if (!strcmp(argv[i], "--out"))  out_path  = argv[i + 1];
	}
	if (!pak_path || !out_path)
		die("usage: qcook stream --pak <pak0.pak> [--pak1 <pak1.pak>] --out <stream.dat>");

	long pak_len, pak1_len = 0;
	uint8_t *pak = read_file(pak_path, &pak_len), *pak1 = pak1_path ? read_file(pak1_path, &pak1_len) : NULL;
	uint32_t len;
	const uint8_t *pal = pak_find(pak, pak_len, "gfx/palette.lmp", &len);
	if (!pal || len < 768)
		die("gfx/palette.lmp missing");
	qc_set_palette(pal);

	const char *world_sounds[MAX_WORLD_SOUNDS];
	int nworld = sound_streamed(world_sounds, MAX_WORLD_SOUNDS);
	entry  *ents = xcalloc((size_t) (NUM_MODELS + NUM_MONSTER_SOUNDS + nworld), sizeof *ents);
	int     n = 0, nsnd = 0;
	buf_t   data = { 0 };
	size_t  total = 0, sound_bytes = 0;
	static const uint8_t zero[QST_SECTOR];
	for (int i = 0; i < NUM_MODELS; i++) {
		const stream_model *sm = &MODELS[i];
		const uint8_t *d = NULL;
		if (sm->pak1) {
			if (!pak1)
				continue;
			d = pak_find(pak1, pak1_len, sm->file, &len);
		} else {
			d = pak_find(pak, pak_len, sm->file, &len);
		}
		if (!d && sm->item && !(pak1 && (d = pak_find(pak1, pak1_len, sm->file, &len)))) {
			printf("qcook: %s not in the paks: not streamed\n", sm->file);
			continue;
		}
		if (!d)
			die("%s missing", sm->file);
		char name[64];
		if (sm->skin)
			snprintf(name, sizeof name, "%s#%d", sm->file, sm->skin);
		else
			snprintf(name, sizeof name, "%s", sm->file);
		qc_mdl *q = qc_mdl_load(name, d, len, sm->skin, sm->div, sm->flat);
		entry *e = &ents[n++];
		/* each asset on a sector of its own */
		buf_put(&data, zero, (QST_SECTOR - data.len % QST_SECTOR) % QST_SECTOR);
		e->ofs = data.len;
		qc_mdl_cook_compact(q, &data);
		e->len = data.len - e->ofs;
		e->a.hash  = qs_hash(name);
		e->a.kind  = QSTK_MODEL;
		e->a.group = (uint16_t) sm->group;
		e->a.size  = (uint32_t) e->len;
		total += e->len;
		/* the monster's sounds after its last model */
		if (i + 1 < NUM_MODELS && MODELS[i + 1].group == sm->group)
			continue;
		for (int k = 0; k < NUM_MONSTER_SOUNDS + (sm->group == G_BOSS ? nworld : 0); k++) {
			int world = k >= NUM_MONSTER_SOUNDS;
			const char *name = world ? world_sounds[k - NUM_MONSTER_SOUNDS] : MONSTER_SOUNDS[k].file;
			if (!world && MONSTER_SOUNDS[k].group != sm->group)
				continue;
			if (sound_is_banked(name))
				die("%s: a bank sound, not streamed", name);
			if (!sound_in_paks(name, pak, pak_len, pak1, pak1_len)) {   /* (the registered game's) */
				printf("qcook: %s not in the paks: not streamed\n", name);
				continue;
			}
			e = &ents[n++];
			buf_put(&data, zero, (QST_SECTOR - data.len % QST_SECTOR) % QST_SECTOR);
			e->ofs = data.len;
			sound_encode_stream(name, pak, pak_len, pak1, pak1_len, &data, NULL);
			e->len = data.len - e->ofs;
			e->a.hash  = qs_hash(name);
			e->a.kind  = QSTK_SOUND;
			e->a.group = (uint16_t) (world ? G_WORLD : sm->group);
			e->a.size  = (uint32_t) e->len;
			total += e->len;
			sound_bytes += e->len;
			nsnd++;
		}
	}

	/* the directory, sorted by hash; the data after it, a sector apart */
	for (int i = 0; i < n; i++)
		for (int j = 0; j < i; j++)
			if (ents[i].a.hash == ents[j].a.hash)
				die("two assets with one hash (%08x)", ents[i].a.hash);
	qsort(ents, (size_t) n, sizeof *ents, by_hash);
	size_t dir_bytes = sizeof(qst_header) + (size_t) n * sizeof(qst_asset);
	uint32_t dir_sectors = (uint32_t) ((dir_bytes + QST_SECTOR - 1) / QST_SECTOR);
	qst_header h;
	memcpy(h.magic, QSTREAM_MAGIC, 4);
	h.num_assets  = (uint32_t) n;
	h.ofs_assets  = sizeof h;
	h.dir_sectors = dir_sectors;
	for (int i = 0; i < n; i++)
		ents[i].a.sector = dir_sectors + (uint32_t) (ents[i].ofs / QST_SECTOR);

	FILE *f = fopen(out_path, "wb");
	if (!f)
		die("cannot write %s", out_path);
	fwrite(&h, sizeof h, 1, f);
	for (int i = 0; i < n; i++)
		fwrite(&ents[i].a, sizeof ents[i].a, 1, f);
	fwrite(zero, 1, (size_t) dir_sectors * QST_SECTOR - dir_bytes, f);
	fwrite(data.p, 1, data.len, f);
	fwrite(zero, 1, (QST_SECTOR - data.len % QST_SECTOR) % QST_SECTOR, f);
	fclose(f);
	printf("qcook: %s — %d assets (%d sounds, %zu KB), %zu KB (+ %u directory sectors)\n", out_path, n, nsnd,
	       sound_bytes / 1024, total / 1024, dir_sectors);
	free(data.p);
	free(ents);
	return 0;
}
