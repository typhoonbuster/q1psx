/*
 * q1psx — `qcook section`: a map's own file of the streamed assets
 * (docs/porting-notes.md §84). The pool's directory (STREAM_<n>.DAT's,
 * formats/qstream.h) with, for each asset the map's entities bring (its
 * monsters' kinds, its items, its entities' sounds: the lists the XA
 * carousels used, §48), its sector in this file, and QST_ABSENT for the
 * others; the assets' bytes copied from the pool, the nearest to the
 * player's start first (a monster's group at the distance of its nearest
 * monster, the gibs and the backpack at the nearest monster's; the items
 * and the world's sounds after, in the pool's order), so that the engine's
 * preload, which reads by distance, sweeps the file forward. The file goes
 * in the level's section on the disc, right before its music.
 *
 * With the level's sound set (`--sndset <m>.sndset`, the bank's cook,
 * §86): the sounds the bank holds are QST_BANKED (not in the file); the
 * others are flagged QSTF_RAM and lie first, together, for the engine to
 * read them in one go at the map's start and play them from main RAM.
 *
 *   qcook section --pak <pak0.pak> [--pak1 <pak1.pak>] --stream <stream.dat>
 *                 --map maps/<m>.bsp --out <m>_s.dat [--sndset <m>.sndset]
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qsound.h"
#include "../../formats/qstream.h"

typedef struct {
	int    asset;              /* the pool's index                         */
	double dist;               /* from the player's start (1e9: unknown)   */
} pick;

static int by_dist(const void *x, const void *y)
{
	const pick *a = x, *b = y;
	if (a->dist != b->dist)
		return a->dist < b->dist ? -1 : 1;
	return a->asset < b->asset ? -1 : a->asset > b->asset;   /* (the pool's order: a monster's together) */
}

static int pool_find(const qst_asset *dir, int n, uint32_t hash)
{
	int lo = 0, hi = n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		if (dir[mid].hash == hash)
			return mid;
		if (dir[mid].hash < hash) lo = mid + 1;
		else                      hi = mid - 1;
	}
	return -1;
}

int cmd_section(int argc, char **argv)
{
	const char *pak_path = NULL, *pak1_path = NULL, *stream_path = NULL, *map = NULL, *out_path = NULL;
	const char *sndset_path = NULL;
	for (int i = 2; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--pak"))    pak_path    = argv[i + 1];
		if (!strcmp(argv[i], "--pak1"))   pak1_path   = argv[i + 1];
		if (!strcmp(argv[i], "--stream")) stream_path = argv[i + 1];
		if (!strcmp(argv[i], "--map"))    map         = argv[i + 1];
		if (!strcmp(argv[i], "--out"))    out_path    = argv[i + 1];
		if (!strcmp(argv[i], "--sndset")) sndset_path = argv[i + 1];
	}
	if (!pak_path || !stream_path || !map || !out_path)
		die("usage: qcook section --pak <pak0.pak> [--pak1 <pak1.pak>] --stream <stream.dat> --map maps/<m>.bsp "
		    "--out <m>_s.dat");

	long pak_len, pak1_len = 0, pool_len;
	uint8_t *pak = read_file(pak_path, &pak_len), *pak1 = pak1_path ? read_file(pak1_path, &pak1_len) : NULL;
	uint32_t len;
	const uint8_t *raw = pak_find(pak, pak_len, map, &len);
	if (!raw && pak1)
		raw = pak_find(pak1, pak1_len, map, &len);
	if (!raw)
		die("%s: not in the paks", map);
	uint8_t *pf = read_file(stream_path, &pool_len);
	const qst_header *ph = (const qst_header *) pf;
	if (pool_len < (long) sizeof *ph || memcmp(ph->magic, QSTREAM_MAGIC, 4) ||
	    ph->ofs_assets + (uint64_t) ph->num_assets * sizeof(qst_asset) > (uint64_t) pool_len)
		die("%s: not a file of streamed assets", stream_path);
	const qst_asset *dir = (const qst_asset *) (pf + ph->ofs_assets);
	if (ph->num_assets > 4096)
		die("%s: %u assets", stream_path, (unsigned) ph->num_assets);
	int n = (int) ph->num_assets;

	/* the map's entities: those single player spawns at some skill
	   (ED_LoadFromFile: not one flagged not easy, not medium and not hard),
	   the player's start, the world's type */
	bsp_t b;
	bsp_parse(&b, raw, len);
	ent_list l;
	ents_parse(&b, &l);
	ent_list sp = l;
	sp.ents = xmalloc((size_t) (l.num ? l.num : 1) * sizeof *sp.ents);
	sp.num = 0;
	int worldtype = 0, has_spawn = 0;
	double spawn[3] = { 0, 0, 0 };
	for (int i = 0; i < l.num; i++) {
		if (!strcmp(l.ents[i].classname, "worldspawn"))
			worldtype = (int) ent_float(&l, &l.ents[i], "worldtype", 0);
		if (!has_spawn && !strcmp(l.ents[i].classname, "info_player_start"))
			has_spawn = ent_vector(&l, &l.ents[i], "origin", spawn);
		if (((int) ent_float(&l, &l.ents[i], "spawnflags", 0) & 0x700) != 0x700)
			sp.ents[sp.num++] = l.ents[i];
	}

	/* each monster group's nearest member to the start; the gibs' and the
	   backpack's: the nearest monster of any kind */
	double gdist[G_WORLD + 1];
	for (int g = 0; g <= G_WORLD; g++)
		gdist[g] = 1e9;
	for (int i = 0; i < sp.num; i++) {
		int g = stream_kind_group(sp.ents[i].classname);
		double o[3];
		if (!g || !ent_vector(&sp, &sp.ents[i], "origin", o))
			continue;
		double d = has_spawn ? sqrt((o[0] - spawn[0]) * (o[0] - spawn[0]) + (o[1] - spawn[1]) * (o[1] - spawn[1]) +
		                            (o[2] - spawn[2]) * (o[2] - spawn[2])) : 0;
		if (d < gdist[g])
			gdist[g] = d;
		if (d < gdist[G_GIBS])
			gdist[G_GIBS] = gdist[G_BACKPACK] = d;
	}

	/* the assets the map brings */
	const char *names[768];
	int nn = stream_monster_needs(&sp, names, 512);
	nn += items_streamed(&sp, worldtype, names + nn, 128);
	nn += sound_map_streamed(&sp, worldtype, names + nn, 128);
	uint8_t *want = xcalloc((size_t) (n ? n : 1), 1);
	int nin = 0, nbank = 0;
	for (int i = 0; i < nn; i++) {
		int a = pool_find(dir, n, qs_hash(names[i]));
		if (a < 0) {
			nbank++;                              /* (a bank's sound, not streamed) */
			continue;
		}
		if (!want[a])
			nin++;
		want[a] = 1;
	}
	/* the level's sound set: the bank's sounds leave the file, the others
	   are read into RAM at the map's start: they go first; a `disc` one
	   stays in the file, read when wanted (docs/porting-notes.md §98) */
	uint8_t *banked = xcalloc((size_t) (n ? n : 1), 1), *ram = xcalloc((size_t) (n ? n : 1), 1);
	uint8_t *disc = xcalloc((size_t) (n ? n : 1), 1);
	int nbanked = 0, nram = 0, nunlisted = 0, ndisc = 0;
	if (sndset_path) {
		FILE *sf = fopen(sndset_path, "r");
		if (!sf)
			die("cannot read %s", sndset_path);
		char line[256], word[8], name[200];
		while (fgets(line, sizeof line, sf)) {
			if (sscanf(line, "%7s %199s", word, name) == 2 && !strcmp(word, "disc")) {
				int a = pool_find(dir, n, qs_hash(name));   /* (read from the file when wanted, */
				if (a >= 0) {                               /*  never into RAM: the fogs, the */
					disc[a] = 1;                            /*  powerups' sounds) */
					if (!want[a])
						nin++;
					want[a] = 1;
				}
				continue;
			}
			if (sscanf(line, "%7s %199s", word, name) == 2 && (!strcmp(word, "spu") || !strcmp(word, "ram"))) {
				int a = pool_find(dir, n, qs_hash(name));
				if (a < 0)
					continue;                     /* (a bank sound of the always set: not in the pool) */
				if (!strcmp(word, "spu")) {
					banked[a] = 1;
					want[a] = 0;
				} else {
					ram[a] = 1;
					if (!want[a])
						nin++;
					want[a] = 1;
				}
			}
		}
		fclose(sf);
		for (int a = 0; a < n; a++) {
			if (want[a] && dir[a].kind == QSTK_SOUND && !ram[a] && !disc[a]) {
				ram[a] = 1;                       /* (a sound the set did not list: into RAM too) */
				nunlisted++;
			}
			nbanked += banked[a];
			nram    += ram[a];
			ndisc   += disc[a];
		}
	}
	pick *order = xcalloc((size_t) (nin ? nin : 1), sizeof *order);
	int no = 0, nnear = 0;
	for (int a = 0; a < n; a++) {
		if (!want[a])
			continue;
		int g = dir[a].group;
		order[no].asset = a;
		order[no].dist  = ram[a] ? -1 : g >= 0 && g <= G_WORLD ? gdist[g] : 1e9;   /* (the RAM sounds first) */
		nnear += order[no].dist >= 0 && order[no].dist < 1e9;
		no++;
	}
	qsort(order, (size_t) no, sizeof *order, by_dist);

	/* the file: the pool's directory, the sectors this file's; then the
	   assets, each from a sector of its own, in that order */
	size_t dir_bytes = sizeof(qst_header) + (size_t) n * sizeof(qst_asset);
	uint32_t dir_sectors = (uint32_t) ((dir_bytes + QST_SECTOR - 1) / QST_SECTOR);
	qst_asset *d2 = xmalloc((size_t) (n ? n : 1) * sizeof *d2);
	memcpy(d2, dir, (size_t) n * sizeof *d2);
	for (int a = 0; a < n; a++)
		d2[a].sector = banked[a] ? QST_BANKED : QST_ABSENT;
	uint32_t sector = dir_sectors;
	for (int i = 0; i < no; i++) {
		int a = order[i].asset;
		d2[a].sector = sector;
		if (ram[a])
			d2[a].flags |= QSTF_RAM;
		sector += (dir[a].size + QST_SECTOR - 1) / QST_SECTOR;
	}
	FILE *f = fopen(out_path, "wb");
	if (!f)
		die("cannot write %s", out_path);
	static const uint8_t zero[QST_SECTOR];
	qst_header h = *ph;
	h.num_assets  = (uint32_t) n;
	h.ofs_assets  = sizeof h;
	h.dir_sectors = dir_sectors;
	fwrite(&h, sizeof h, 1, f);
	fwrite(d2, sizeof *d2, (size_t) n, f);
	fwrite(zero, 1, (size_t) dir_sectors * QST_SECTOR - dir_bytes, f);
	for (int i = 0; i < no; i++) {
		const qst_asset *a = &dir[order[i].asset];
		if ((uint64_t) a->sector * QST_SECTOR + a->size > (uint64_t) pool_len)
			die("%s: asset %08x past the file's end", stream_path, a->hash);
		fwrite(pf + (size_t) a->sector * QST_SECTOR, 1, a->size, f);
		fwrite(zero, 1, (QST_SECTOR - a->size % QST_SECTOR) % QST_SECTOR, f);
	}
	fclose(f);
	printf("qcook: %s — %d of %d assets, %u KB (%d sounds into RAM%s, %d read from the disc, %d in the bank; the "
	       "models %d by their distance from the start, %d after; %d names the bank's)\n", out_path, nin, n,
	       (unsigned) ((sector - dir_sectors) * QST_SECTOR / 1024), nram,
	       nunlisted ? " (some the set did not list)" : "", ndisc, nbanked, nnear, no - nnear - nram - ndisc, nbank);

	free(order); free(want); free(banked); free(ram); free(disc); free(d2); free(sp.ents);
	ents_free(&l);
	free(pf); free(pak); free(pak1);
	return 0;
}
