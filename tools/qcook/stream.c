/*
 * qcook — the streamed assets: `qcook stream --pak <pak0.pak> [--pak1
 * <pak1.pak>] --out <prefix>` (formats/qstream.h: <prefix>_0.dat,
 * STREAM_0.DAT on the disc): the models' triangles reduced
 * (tools/qcook/game.c stream_reduce; the monsters' bodies by hand).
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
	int         frame1;     /* a frame + 1: that frame alone (the finale's
	                           player), 0: every frame                      */
} stream_model;

/* (the groups: qcook.h G_*) */

static const stream_model MODELS[] = {
	{ "progs/gib1.mdl",     G_GIBS,     4, 0, 0, 0, 0, 0 },     /* player.qc ThrowGib: every monster */
	{ "progs/gib2.mdl",     G_GIBS,     4, 0, 0, 0, 0, 0 },
	{ "progs/gib3.mdl",     G_GIBS,     4, 0, 0, 0, 0, 0 },
	{ "progs/backpack.mdl", G_BACKPACK, 4, 0, 0, 0, 0, 0 },     /* items.qc DropBackpack */
	{ "progs/soldier.mdl",  G_ARMY,     2, 0, 0, 0, 0, 0 },     /* monster_army */
	{ "progs/h_guard.mdl",  G_ARMY,     4, 0, 0, 0, 0, 0 },
	{ "progs/dog.mdl",      G_DOG,      2, 0, 0, 0, 0, 0 },     /* monster_dog */
	{ "progs/h_dog.mdl",    G_DOG,      4, 0, 0, 0, 0, 0 },
	{ "progs/ogre.mdl",     G_OGRE,     2, 0, 0, 0, 0, 0 },     /* monster_ogre (its grenade: GAME_<n>.DAT) */
	{ "progs/h_ogre.mdl",   G_OGRE,     4, 0, 0, 0, 0, 0 },
	{ "progs/knight.mdl",   G_KNIGHT,   2, 0, 0, 0, 0, 0 },     /* monster_knight */
	{ "progs/h_knight.mdl", G_KNIGHT,   4, 0, 0, 0, 0, 0 },
	{ "progs/zombie.mdl",   G_ZOMBIE,   2, 0, 0, 0, 0, 0 },     /* monster_zombie */
	{ "progs/h_zombie.mdl", G_ZOMBIE,   4, 0, 0, 0, 0, 0 },
	{ "progs/zom_gib.mdl",  G_ZOMBIE,   4, 0, 0, 0, 0, 0 },
	{ "progs/wizard.mdl",   G_WIZARD,   2, 0, 0, 0, 0, 0 },     /* monster_wizard */
	{ "progs/h_wizard.mdl", G_WIZARD,   4, 0, 0, 0, 0, 0 },
	{ "progs/w_spike.mdl",  G_WIZARD,   4, 1, 0, 0, 0, 0 },
	{ "progs/demon.mdl",    G_DEMON,    2, 0, 0, 0, 0, 0 },     /* monster_demon1 */
	{ "progs/h_demon.mdl",  G_DEMON,    4, 0, 0, 0, 0, 0 },
	{ "progs/shambler.mdl", G_SHAMBLER, 2, 0, 0, 0, 0, 0 },     /* monster_shambler */
	{ "progs/h_shams.mdl",  G_SHAMBLER, 4, 0, 0, 0, 0, 0 },
	{ "progs/s_light.mdl",  G_SHAMBLER, 4, 1, 0, 0, 0, 0 },
	{ "progs/bolt.mdl",     G_SHAMBLER, 4, 1, 0, 0, 0, 0 },
	{ "progs/boss.mdl",     G_BOSS,     2, 0, 0, 0, 0, 0 },     /* monster_boss */
	{ "progs/lavaball.mdl", G_BOSS,     4, 1, 0, 0, 0, 0 },
	{ "progs/bolt3.mdl",    G_BOSS,     4, 1, 0, 0, 0, 0 },     /* event_lightning's beam */
	/* the registered game's (pak1) */
	{ "progs/enforcer.mdl", G_ENFORCER, 2, 0, 1, 0, 0, 0 },     /* monster_enforcer */
	{ "progs/h_mega.mdl",   G_ENFORCER, 4, 0, 1, 0, 0, 0 },
	{ "progs/laser.mdl",    G_ENFORCER, 4, 1, 1, 0, 0, 0 },
	{ "progs/hknight.mdl",  G_HKNIGHT,  2, 0, 1, 0, 0, 0 },     /* monster_hell_knight */
	{ "progs/h_hellkn.mdl", G_HKNIGHT,  4, 0, 1, 0, 0, 0 },
	{ "progs/k_spike.mdl",  G_HKNIGHT,  4, 1, 1, 0, 0, 0 },
	{ "progs/tarbaby.mdl",  G_TARBABY,  2, 0, 1, 0, 0, 0 },     /* monster_tarbaby */
	{ "progs/shalrath.mdl", G_SHALRATH, 2, 0, 1, 0, 0, 0 },     /* monster_shalrath */
	{ "progs/h_shal.mdl",   G_SHALRATH, 4, 0, 1, 0, 0, 0 },
	{ "progs/v_spike.mdl",  G_SHALRATH, 4, 1, 1, 0, 0, 0 },
	{ "progs/fish.mdl",     G_FISH,     2, 0, 1, 0, 0, 0 },     /* monster_fish */
	{ "progs/oldone.mdl",   G_OLDONE,   2, 0, 1, 0, 0, 0 },     /* monster_oldone */
	{ "progs/player.mdl",   G_OLDONE,   4, 0, 0, 0, 0, 2 },     /* its finale's player (frame 1) */
	/* the items' models (items.qc, misc.qc: the map keeps the boxes, BSP
	   models over its own textures): a quarter of their skins' resolution,
	   the flames a half (fullbright: flat) */
	{ "progs/g_shot.mdl",   G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/g_nail.mdl",   G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/g_nail2.mdl",  G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/g_rock.mdl",   G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/g_rock2.mdl",  G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/g_light.mdl",  G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/armor.mdl",    G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/armor.mdl",    G_ITEMS,    4, 0, 0, 1, 1, 0 },
	{ "progs/armor.mdl",    G_ITEMS,    4, 0, 0, 2, 1, 0 },
	{ "progs/w_s_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/w_g_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/m_s_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/m_g_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/b_s_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/b_g_key.mdl",  G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/end1.mdl",     G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/end2.mdl",     G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/end3.mdl",     G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/end4.mdl",     G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/invulner.mdl", G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/suit.mdl",     G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/invisibl.mdl", G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/quaddama.mdl", G_ITEMS,    4, 0, 0, 0, 1, 0 },
	{ "progs/flame.mdl",    G_ITEMS,    2, 1, 0, 0, 1, 0 },
	{ "progs/flame2.mdl",   G_ITEMS,    2, 1, 0, 0, 1, 0 },
	{ "progs/teleport.mdl", G_ITEMS,    4, 1, 0, 0, 1, 0 },
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
	{ "enforcer/death1.wav", G_ENFORCER }, { "enforcer/enfire.wav", G_ENFORCER },
	{ "enforcer/enfstop.wav", G_ENFORCER }, { "enforcer/idle1.wav", G_ENFORCER },
	{ "enforcer/pain1.wav",  G_ENFORCER }, { "enforcer/pain2.wav",  G_ENFORCER },
	{ "enforcer/sight1.wav", G_ENFORCER }, { "enforcer/sight2.wav", G_ENFORCER },
	{ "enforcer/sight3.wav", G_ENFORCER }, { "enforcer/sight4.wav", G_ENFORCER },
	{ "hknight/attack1.wav", G_HKNIGHT },  { "hknight/death1.wav",  G_HKNIGHT },
	{ "hknight/pain1.wav",   G_HKNIGHT },  { "hknight/sight1.wav",  G_HKNIGHT },
	{ "hknight/hit.wav",     G_HKNIGHT },  { "hknight/slash1.wav",  G_HKNIGHT },
	{ "hknight/idle.wav",    G_HKNIGHT },
	{ "blob/death1.wav",     G_TARBABY },  { "blob/hit1.wav",       G_TARBABY },
	{ "blob/land1.wav",      G_TARBABY },  { "blob/sight1.wav",     G_TARBABY },
	{ "shalrath/attack.wav", G_SHALRATH }, { "shalrath/attack2.wav", G_SHALRATH },
	{ "shalrath/death.wav",  G_SHALRATH }, { "shalrath/idle.wav",   G_SHALRATH },
	{ "shalrath/pain.wav",   G_SHALRATH }, { "shalrath/sight.wav",  G_SHALRATH },
	{ "fish/death.wav",      G_FISH },     { "fish/bite.wav",       G_FISH },
	{ "fish/idle.wav",       G_FISH },
	{ "boss2/death.wav",     G_OLDONE },   { "boss2/pop2.wav",      G_OLDONE },
};
#define NUM_MONSTER_SOUNDS (int) (sizeof MONSTER_SOUNDS / sizeof MONSTER_SOUNDS[0])

/* a kind's sounds another kind's group streams (the hell knight's swords:
   the knight's), wanted with that kind too */
static const struct { const char *file; int group; } SHARED_SOUNDS[] = {
	{ "knight/sword1.wav", G_HKNIGHT }, { "knight/sword2.wav", G_HKNIGHT },
};
#define MAX_WORLD_SOUNDS 96

typedef struct {
	qst_asset a;
	size_t    ofs, len;        /* in the data buffer                        */
} entry;

static int by_hash(const void *x, const void *y)
{
	const entry *a = x, *b = y;
	return a->a.hash < b->a.hash ? -1 : a->a.hash > b->a.hash;
}

static void sector_pad(buf_t *b)
{
	static const uint8_t zero[QST_SECTOR];
	buf_put(b, zero, (QST_SECTOR - b->len % QST_SECTOR) % QST_SECTOR);
}

/* the file: the directory (the assets' sizes and first sectors in it),
   sorted by hash, then the data a sector apart */
static void write_out(const char *path, entry *ents, int n, const buf_t *data)
{
	static const uint8_t zero[QST_SECTOR];
	size_t dir_bytes = sizeof(qst_header) + (size_t) n * sizeof(qst_asset);
	uint32_t dir_sectors = (uint32_t) ((dir_bytes + QST_SECTOR - 1) / QST_SECTOR);
	qst_header h;
	memcpy(h.magic, QSTREAM_MAGIC, 4);
	h.num_assets  = (uint32_t) n;
	h.ofs_assets  = sizeof h;
	h.dir_sectors = dir_sectors;
	FILE *f = fopen(path, "wb");
	if (!f)
		die("cannot write %s", path);
	fwrite(&h, sizeof h, 1, f);
	for (int i = 0; i < n; i++) {
		qst_asset a = ents[i].a;
		a.sector = dir_sectors + (uint32_t) (ents[i].ofs / QST_SECTOR);
		a.size   = (uint32_t) ents[i].len;
		fwrite(&a, sizeof a, 1, f);
	}
	fwrite(zero, 1, (size_t) dir_sectors * QST_SECTOR - dir_bytes, f);
	fwrite(data->p, 1, data->len, f);
	fwrite(zero, 1, (QST_SECTOR - data->len % QST_SECTOR) % QST_SECTOR, f);
	fclose(f);
	printf("qcook: %s — %d assets, %zu KB (+ %u directory sectors)\n", path, n, data->len / 1024, dir_sectors);
}

int cmd_stream(int argc, char **argv)
{
	const char *pak_path = NULL, *pak1_path = NULL, *out_prefix = NULL;
	for (int i = 2; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--pak"))   pak_path   = argv[i + 1];
		if (!strcmp(argv[i], "--pak1"))  pak1_path  = argv[i + 1];
		if (!strcmp(argv[i], "--out"))   out_prefix = argv[i + 1];
	}
	if (!pak_path || !out_prefix)
		die("usage: qcook stream --pak <pak0.pak> [--pak1 <pak1.pak>] --out <prefix>   (<prefix>_0.dat)");

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
	size_t  sound_bytes = 0;
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
		if (sm->frame1)
			qc_mdl_one_frame(q, sm->frame1 - 1);
		entry *e = &ents[n++];
		int body = sm->group >= G_ARMY && sm->group <= G_LAST_MONSTER && sm->div == 2;   /* a monster's body */
		buf_t b;
		qc_mdl_cook_stream(q, body, &b);
		sector_pad(&data);                    /* each asset on a sector of its own */
		e->ofs = data.len;
		buf_put(&data, b.p, b.len);
		e->len = b.len;
		free(b.p);
		e->a.hash  = qs_hash(name);
		e->a.kind  = QSTK_MODEL;
		e->a.group = (uint16_t) sm->group;
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
			buf_t sb = { 0 };
			sound_encode_stream(name, pak, pak_len, pak1, pak1_len, &sb, NULL);
			sector_pad(&data);
			e->ofs = data.len;
			buf_put(&data, sb.p, sb.len);
			e->len = sb.len;
			free(sb.p);
			e->a.hash  = qs_hash(name);
			e->a.kind  = QSTK_SOUND;
			e->a.group = (uint16_t) (world ? G_WORLD : sm->group);
			sound_bytes += sb.len;
			nsnd++;
		}
	}

	for (int i = 0; i < n; i++)
		for (int j = 0; j < i; j++)
			if (ents[i].a.hash == ents[j].a.hash)
				die("two assets with one hash (%08x)", ents[i].a.hash);
	qsort(ents, (size_t) n, sizeof *ents, by_hash);
	char path[512];
	snprintf(path, sizeof path, "%s_0.dat", out_prefix);
	write_out(path, ents, n, &data);
	free(data.p);
	int cls_n;
	size_t cls_saved;
	sound_class_halved(&cls_n, &cls_saved);
	printf("qcook: streamed — %d assets, %d sounds (%zu KB; %d halved by their class, %zu KB saved)\n", n, nsnd,
	       sound_bytes / 1024, cls_n, cls_saved / 1024);
	free(ents);
	return 0;
}

/* The streamed assets a map's monsters bring (the XA carousels' until
   docs/porting-notes.md §52): each kind's group (its models, head,
   projectiles, sounds), the gibs and the backpack with any monster; their
   names */
static const struct { const char *classname; int group; } KINDS[] = {
	{ "monster_army", G_ARMY }, { "monster_dog", G_DOG }, { "monster_ogre", G_OGRE },
	{ "monster_ogre_marksman", G_OGRE }, { "monster_knight", G_KNIGHT }, { "monster_zombie", G_ZOMBIE },
	{ "monster_wizard", G_WIZARD }, { "monster_demon1", G_DEMON }, { "monster_shambler", G_SHAMBLER },
	{ "monster_boss", G_BOSS }, { "monster_enforcer", G_ENFORCER }, { "monster_hell_knight", G_HKNIGHT },
	{ "monster_tarbaby", G_TARBABY }, { "monster_shalrath", G_SHALRATH }, { "monster_fish", G_FISH },
	{ "monster_oldone", G_OLDONE },
};

/* a streamed sound's group (a monster's, or G_WORLD), 0 for a name that is none */
int stream_sound_group(const char *name)
{
	for (int k = 0; k < NUM_MONSTER_SOUNDS; k++)
		if (!strcmp(MONSTER_SOUNDS[k].file, name))
			return MONSTER_SOUNDS[k].group;
	return 0;
}

/* a monster entity's group (its kind's assets), 0 for any other classname */
int stream_kind_group(const char *classname)
{
	for (size_t k = 0; k < sizeof KINDS / sizeof KINDS[0]; k++)
		if (!strcmp(classname, KINDS[k].classname))
			return KINDS[k].group;
	return 0;
}

int stream_monster_needs(const ent_list *l, const char **list, int max)
{
	int have[G_WORLD + 1] = { 0 }, any = 0;
	for (int i = 0; i < l->num; i++) {
		int g = stream_kind_group(l->ents[i].classname);
		if (g)
			have[g] = any = 1;
	}
	have[G_GIBS] = have[G_BACKPACK] = any;
	static char names[NUM_MODELS][64];
	int n = 0;
	if (have[G_OGRE] || have[G_DEMON] || have[G_SHAMBLER])   /* their melee's meat (SpawnMeatSpray: */
		list[n++] = "progs/zom_gib.mdl";                      /* the zombie's flesh; engine/m_*.c) */
	for (int i = 0; i < NUM_MODELS; i++) {
		if (!have[MODELS[i].group] || (n && list[0] == MODELS[i].file && !strcmp(MODELS[i].file, "progs/zom_gib.mdl")))
			continue;
		if (n >= max)
			die("too many monster assets");
		if (MODELS[i].skin)
			snprintf(names[i], sizeof names[i], "%s#%d", MODELS[i].file, MODELS[i].skin);
		else
			snprintf(names[i], sizeof names[i], "%s", MODELS[i].file);
		list[n++] = names[i];
	}
	for (int k = 0; k < NUM_MONSTER_SOUNDS; k++) {
		if (!have[MONSTER_SOUNDS[k].group])
			continue;
		if (n >= max)
			die("too many monster assets");
		list[n++] = MONSTER_SOUNDS[k].file;
	}
	for (size_t k = 0; k < sizeof SHARED_SOUNDS / sizeof SHARED_SOUNDS[0]; k++) {
		if (!have[SHARED_SOUNDS[k].group] || have[stream_sound_group(SHARED_SOUNDS[k].file)])
			continue;
		if (n >= max)
			die("too many monster assets");
		list[n++] = SHARED_SOUNDS[k].file;
	}
	return n;
}
