/*
 * qcook — the streamed assets: `qcook stream --pak <pak0.pak> [--pak1
 * <pak1.pak>] --out <stream.dat>` (formats/qstream.h).
 *
 * The models the maps' monsters bring, each with what its QC precaches
 * beside it (its head, its projectiles), the gibs every monster throws and
 * the backpack a soldier drops. A monster's assets form a group, laid out
 * next to each other so that the engine reads them in one go. Skins at
 * half their resolution (the view models' and the monsters': the screen
 * never shows them larger), a quarter for what is small (the gibs, the
 * projectiles, the backpack). The registered game's monsters come from
 * pak1 when it is given.
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
} stream_model;

enum {
	G_GIBS = 1, G_BACKPACK, G_ARMY, G_DOG, G_OGRE, G_KNIGHT, G_ZOMBIE, G_WIZARD, G_DEMON,
	G_SHAMBLER, G_BOSS
};

static const stream_model MODELS[] = {
	{ "progs/gib1.mdl",     G_GIBS,     4, 0, 0 },     /* player.qc ThrowGib: every monster */
	{ "progs/gib2.mdl",     G_GIBS,     4, 0, 0 },
	{ "progs/gib3.mdl",     G_GIBS,     4, 0, 0 },
	{ "progs/backpack.mdl", G_BACKPACK, 4, 0, 0 },     /* items.qc DropBackpack */
	{ "progs/soldier.mdl",  G_ARMY,     2, 0, 0 },     /* monster_army */
	{ "progs/h_guard.mdl",  G_ARMY,     4, 0, 0 },
	{ "progs/dog.mdl",      G_DOG,      2, 0, 0 },     /* monster_dog */
	{ "progs/h_dog.mdl",    G_DOG,      4, 0, 0 },
	{ "progs/ogre.mdl",     G_OGRE,     2, 0, 0 },     /* monster_ogre (its grenade: GAME.DAT) */
	{ "progs/h_ogre.mdl",   G_OGRE,     4, 0, 0 },
	{ "progs/knight.mdl",   G_KNIGHT,   2, 0, 0 },     /* monster_knight */
	{ "progs/h_knight.mdl", G_KNIGHT,   4, 0, 0 },
	{ "progs/zombie.mdl",   G_ZOMBIE,   2, 0, 0 },     /* monster_zombie */
	{ "progs/h_zombie.mdl", G_ZOMBIE,   4, 0, 0 },
	{ "progs/zom_gib.mdl",  G_ZOMBIE,   4, 0, 0 },
	{ "progs/wizard.mdl",   G_WIZARD,   2, 0, 0 },     /* monster_wizard */
	{ "progs/h_wizard.mdl", G_WIZARD,   4, 0, 0 },
	{ "progs/w_spike.mdl",  G_WIZARD,   4, 1, 0 },
	{ "progs/demon.mdl",    G_DEMON,    2, 0, 0 },     /* monster_demon1 */
	{ "progs/h_demon.mdl",  G_DEMON,    4, 0, 0 },
	{ "progs/shambler.mdl", G_SHAMBLER, 2, 0, 0 },     /* monster_shambler */
	{ "progs/h_shams.mdl",  G_SHAMBLER, 4, 0, 0 },
	{ "progs/s_light.mdl",  G_SHAMBLER, 4, 1, 0 },
	{ "progs/bolt.mdl",     G_SHAMBLER, 4, 1, 0 },
	{ "progs/boss.mdl",     G_BOSS,     2, 0, 0 },     /* monster_boss */
	{ "progs/lavaball.mdl", G_BOSS,     4, 1, 0 },
};
#define NUM_MODELS (int) (sizeof MODELS / sizeof MODELS[0])

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

	entry  *ents = xcalloc(NUM_MODELS, sizeof *ents);
	int     n = 0;
	buf_t   data = { 0 };
	size_t  total = 0;
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
		if (!d)
			die("%s missing", sm->file);
		qc_mdl *q = qc_mdl_load(sm->file, d, len, 0, sm->div, sm->flat);
		entry *e = &ents[n++];
		/* each asset on a sector of its own */
		static const uint8_t zero[QST_SECTOR];
		buf_put(&data, zero, (QST_SECTOR - data.len % QST_SECTOR) % QST_SECTOR);
		e->ofs = data.len;
		qc_mdl_cook_compact(q, &data);
		e->len = data.len - e->ofs;
		e->a.hash  = qs_hash(sm->file);
		e->a.kind  = QSTK_MODEL;
		e->a.group = (uint16_t) sm->group;
		e->a.size  = (uint32_t) e->len;
		total += e->len;
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
	static const uint8_t zero[QST_SECTOR];
	fwrite(zero, 1, (size_t) dir_sectors * QST_SECTOR - dir_bytes, f);
	fwrite(data.p, 1, data.len, f);
	fwrite(zero, 1, (QST_SECTOR - data.len % QST_SECTOR) % QST_SECTOR, f);
	fclose(f);
	printf("qcook: %s — %d assets, %zu KB (+ %u directory sectors)\n", out_path, n, total / 1024, dir_sectors);
	free(data.p);
	free(ents);
	return 0;
}
