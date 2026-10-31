/*
 * q1psx — `qcook zones`: a cooked world (QWVI) split into its skeleton
 * (QWZJ) and its zone chunks (QZN2), from the level-chunks analysis'
 * tables (formats/qworld.h "zones", docs/porting-notes.md §56, §57).
 *
 *   qcook zones --world <map>.wld --tables <dir>/<map>.zones
 *               --skeleton <out.skl> --chunks <out.zon>
 *
 * The tables (one text file a map: analysis/export.py's docstring gives
 * the format) are made outside the repo from Quake's maps; the cut in use
 * sits in the repo's zones/auto_1536_s3, the one scripts/cook.sh takes
 * unless Q1PSX_ZONE_TABLES names another (docs/porting-notes.md §118:
 * numbers only). They list each zone's
 * records (the world records' visibility ids) and, for each leaf, its zone
 * and the three nearest other zones by walking distance. The engine keeps
 * ZONE_SLOTS slots, each the size of the biggest chunk: a camera leaf's
 * zone and its two nearest.
 *
 * The map stays whole (nothing written, an older skeleton removed) when
 * its skeleton and slots would take as much RAM as its whole world,
 * when it has one zone, or when the tables do not match the world (another
 * cook's).
 *
 * `--fmap <tables' cook's> <this world's>` (scripts/cook.sh with
 * QCOOK_MERGE: the coplanar faces merged renumber the records): two
 * `qcook map --fmap` files, each world record's Quake faces; a zone then
 * takes every record of this world that holds a face of one of its
 * records in the tables' (docs/porting-notes.md §113).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qworld.h"

#define SECTOR    2048
#define ZONE_SLOTS 3u      /* the chunks resident at once (engine/zone.c MAX_RES): a camera
                              leaf's zone and its two nearest (docs/porting-notes.md §93;
                              4 and 5 until §119) */

/* ---------------------------------------------------------- the world -- */

static const qw_header *H;
static const qw_node   *NODES;      /* [num_nodes + 1] */
static const uint8_t   *FACES;
static const qw_vert   *VERTS;

/* the leaf holding a point (world units): the engine's descent (engine/
   zone.c leaf_at) on the file's nodes and the clip section's planes */
static uint32_t leaf_of_point(const uint8_t *f, const int32_t pos[3])
{
	const qw_plane *PL = (const qw_plane *) (f + H->ofs_clip + sizeof(qw_clip_header));
	int32_t c[3];
	for (int k = 0; k < 3; k++)
		c[k] = pos[k] * QW_VSCALE;
	uint32_t at = H->root;
	for (;;) {
		const qw_node  *n  = &NODES[at];
		const qw_plane *pl = (const qw_plane *) ((const int32_t *) PL + n->plane3);
		int32_t dist = pl->dist >> 1;
		int32_t d = pl->type < 3 ? c[pl->type] * 4096 - dist
		          : pl->n[0] * c[0] + pl->n[1] * c[1] + pl->n[2] * c[2] - dist;
		uint32_t ch = n->child[d <= 0];
		if (ch & 0x80000000u)
			return ch & 0x7fffffffu;
		at = ch;
	}
}

/* a node's block on `side`: [*at, *end) in the face stream */
static void block_of(uint32_t n, int side, uint32_t *at, uint32_t *end)
{
	*at  = NODES[n].faces[side];
	*end = side ? NODES[n + 1].faces[0] : NODES[n].faces[1];
}

/* ---------------------------------------------------------- the tables -- */

typedef struct {
	uint32_t  num_leaves, num_faces, num_zones;
	uint32_t  reach;                 /* D (+ the margin of the "pvs" tables) */
	uint8_t **member;                /* [zone][face]: the zone's chunk holds it */
	int16_t  *leaf_zone;             /* [leaf] */
	int16_t (*leaf_near)[3];         /* [leaf][3]: -1 none */
} tables_t;

static int read_tables(const char *path, tables_t *t)
{
	FILE *fp = fopen(path, "r");
	if (!fp)
		return 0;
	char line[1024], name[32], variant[16];
	int zone = -1, have_map = 0;
	unsigned D = 0;
	memset(t, 0, sizeof *t);
	while (fgets(line, sizeof line, fp)) {
		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (!strncmp(line, "map ", 4)) {
			if (sscanf(line, "map %31s %u %15s %u %u %u", name, &D, variant, &t->num_leaves, &t->num_faces,
			           &t->num_zones) != 6 || !t->num_zones || t->num_zones >= QZ_NONE)
				die("%s: bad map line", path);
			t->reach     = D + (strcmp(variant, "pvs") ? 0 : 32);   /* (export.py's MARGIN) */
			t->member    = xcalloc(t->num_zones, sizeof *t->member);
			for (uint32_t z = 0; z < t->num_zones; z++)
				t->member[z] = xcalloc(t->num_faces, 1);
			t->leaf_zone = xmalloc(t->num_leaves * sizeof *t->leaf_zone);
			t->leaf_near = xmalloc(t->num_leaves * sizeof *t->leaf_near);
			for (uint32_t l = 0; l < t->num_leaves; l++)
				t->leaf_zone[l] = -1;
			have_map = 1;
		} else if (!have_map) {
			die("%s: no map line first", path);
		} else if (!strncmp(line, "zone ", 5)) {
			unsigned bytes, n;
			if (sscanf(line, "zone %d %u %u", &zone, &bytes, &n) != 3 || zone < 0 ||
			    (uint32_t) zone >= t->num_zones)
				die("%s: bad zone line", path);
		} else if (!strncmp(line, "r ", 2)) {
			if (zone < 0)
				die("%s: records before a zone", path);
			char *s = line + 2, *e;
			for (long id; (id = strtol(s, &e, 10)), e != s; s = e) {
				if (id < 0 || (uint32_t) id >= t->num_faces)
					die("%s: record %ld past the %u faces", path, id, t->num_faces);
				t->member[zone][id] = 1;
			}
		} else if (!strncmp(line, "leaf ", 5)) {
			int l, z, p[3], d[3];
			if (sscanf(line, "leaf %d %d %d %d %d %d %d %d", &l, &z, &p[0], &p[1], &p[2], &d[0], &d[1], &d[2]) != 8 ||
			    l < 0 || (uint32_t) l >= t->num_leaves || z < 0 || (uint32_t) z >= t->num_zones)
				die("%s: bad leaf line", path);
			t->leaf_zone[l] = (int16_t) z;
			for (int k = 0; k < 3; k++) {
				if (p[k] >= (int) t->num_zones)
					die("%s: leaf %d: zone %d past the %u zones", path, l, p[k], t->num_zones);
				t->leaf_near[l][k] = (int16_t) (p[k] < 0 ? -1 : p[k]);
			}
		}
	}
	fclose(fp);
	if (!have_map)
		die("%s: no map line", path);
	for (uint32_t l = 0; l < t->num_leaves; l++)
		if (t->leaf_zone[l] < 0)
			die("%s: leaf %u has no line", path, l);
	return 1;
}

/* a `qcook map --fmap` file: its records' count, its (record, face) pairs */
static uint32_t read_fmap(const char *path, int (**pairs)[2], int *npairs)
{
	FILE *fp = fopen(path, "r");
	unsigned nrec;
	if (!fp || fscanf(fp, "fmap %u %d", &nrec, npairs) != 2 || *npairs < 0)
		die("%s: not a face map", path);
	*pairs = xmalloc((size_t) (*npairs + 1) * sizeof **pairs);
	for (int i = 0; i < *npairs; i++)
		if (fscanf(fp, "%d %d", &(*pairs)[i][0], &(*pairs)[i][1]) != 2 || (*pairs)[i][0] < 0 ||
		    (uint32_t) (*pairs)[i][0] >= nrec || (*pairs)[i][1] < 0)
			die("%s: a bad pair", path);
	fclose(fp);
	return nrec;
}

/* the tables' records (another cook's: from) translated to this world's
   (to) through the Quake faces each holds: a zone takes every record that
   holds a face of one of its records */
static void translate_tables(tables_t *t, const char *from, const char *to)
{
	int (*pf)[2], (*pt)[2], nf, nt;
	uint32_t rf = read_fmap(from, &pf, &nf), rt = read_fmap(to, &pt, &nt);
	if (rf != t->num_faces)
		die("%s: %u records, the tables %u", from, rf, t->num_faces);
	int maxf = 0;
	for (int i = 0; i < nf; i++) if (pf[i][1] > maxf) maxf = pf[i][1];
	for (int i = 0; i < nt; i++) if (pt[i][1] > maxf) maxf = pt[i][1];
	int *first = xmalloc((size_t) (maxf + 2) * sizeof *first), *next = xmalloc((size_t) (nt + 1) * sizeof *next);
	for (int f = 0; f <= maxf + 1; f++)
		first[f] = -1;
	for (int i = nt - 1; i >= 0; i--) {        /* a face -> this world's records */
		next[i] = first[pt[i][1]];
		first[pt[i][1]] = i;
	}
	for (uint32_t z = 0; z < t->num_zones; z++) {
		uint8_t *m = xcalloc(rt + 1, 1);
		for (int i = 0; i < nf; i++)
			if (t->member[z][pf[i][0]])
				for (int j = first[pf[i][1]]; j >= 0; j = next[j])
					m[pt[j][0]] = 1;
		free(t->member[z]);
		t->member[z] = m;
	}
	printf("qcook: zones: the tables' %u records translated to %u by their faces\n", t->num_faces, rt);
	t->num_faces = rt;
	free(first); free(next); free(pf); free(pt);
}

/* ------------------------------------------------------------ helpers -- */

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
	const uint8_t *b = p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

static void buf_zero(buf_t *b, size_t n)
{
	static const uint8_t z[64];
	while (n) {
		size_t k = n < sizeof z ? n : sizeof z;
		buf_put(b, z, k);
		n -= k;
	}
}

static void buf_align(buf_t *b, size_t a)
{
	buf_zero(b, (a - b->len % a) % a);
}

static void write_file(const char *path, const buf_t *b)
{
	FILE *fp = fopen(path, "wb");
	if (!fp || fwrite(b->p, 1, b->len, fp) != b->len)
		die("cannot write %s", path);
	fclose(fp);
}

static uint32_t sectors_of(size_t bytes)
{
	return (uint32_t) ((bytes + SECTOR - 1) / SECTOR);
}

/* a pool being built: the world pool's vertices a set of records uses, in
   their first use's order */
typedef struct {
	int32_t  *map;             /* [world vertices]: own index + 1, 0 = not yet */
	uint32_t *src;             /* own index -> world index */
	uint32_t  n;
} pool_t;

static void pool_init(pool_t *p)
{
	p->map = xcalloc(H->num_verts + 1, sizeof *p->map);
	p->src = xmalloc((H->num_verts + 1) * sizeof *p->src);
	p->n   = 0;
}

static void pool_free(pool_t *p)
{
	free(p->map);
	free(p->src);
}

static uint16_t pool_index(pool_t *p, uint16_t v)
{
	if (v >= H->num_verts)
		die("a record names vertex %u of %u", v, H->num_verts);
	if (!p->map[v]) {
		if (p->n >= 65535)
			die("a pool past 65535 vertices");
		p->src[p->n] = v;
		p->map[v] = (int32_t) ++p->n;
	}
	return (uint16_t) (p->map[v] - 1);
}

static void put_pool(buf_t *out, const pool_t *p)
{
	for (uint32_t i = 0; i < p->n; i++)
		buf_put(out, &VERTS[p->src[i]], sizeof(qw_vert));
}

/* a record copied out, its vertex indices into `pool`; a world record
   (`loaded`) as the loader leaves one (engine/world.c world_load): its face
   id doubled, its average light in the flags' bits 2-7 (its UVs on the
   set's tiles already: the cook's world, tex_map_uvs) */
static void put_record(buf_t *out, const qw_record *src, pool_t *pool, int loaded)
{
	size_t at = out->len;
	buf_put(out, src, src->size);
	uint8_t   *r   = out->p + at;
	qw_record *rec = (qw_record *) r;
	uint16_t  *idx = (uint16_t *) (r + sizeof *rec);
	for (int i = 0, nidx = (rec->num_verts + 2) / 3 * 3; i < nidx; i++)
		idx[i] = pool_index(pool, idx[i]);
	if (!loaded)
		return;
	rec->face_id = (uint16_t) (rec->face_id << 1);
	const uint8_t *light = r + rec->polys - ((rec->num_verts + 3) & ~3);
	uint32_t sum = 0, n = rec->num_verts;
	for (uint32_t k = 0; k < n; k++)
		sum += light[k];
	uint32_t avg = n ? (sum + n * 2) / (n * 4) : 0;
	rec->flags = (uint8_t) ((rec->flags & (QWF_SKY | QWF_LIQUID)) | (avg > 63 ? 63 : avg) << 2);
}

/* ---------------------------------------------------------------- run -- */

int cmd_zones(int argc, char **argv)
{
	const char *wld = 0, *tab = 0, *skl = 0, *zon = 0, *fm_from = 0, *fm_to = 0;
	for (int i = 2; i < argc; i++) {
		if (i + 1 >= argc)
			die("zones: %s needs a value", argv[i]);
		if (!strcmp(argv[i], "--world"))           wld = argv[++i];
		else if (!strcmp(argv[i], "--fmap") && i + 2 < argc) {
			fm_from = argv[++i];
			fm_to   = argv[++i];
		}
		else if (!strcmp(argv[i], "--tables"))     tab = argv[++i];
		else if (!strcmp(argv[i], "--skeleton"))   skl = argv[++i];
		else if (!strcmp(argv[i], "--chunks"))     zon = argv[++i];
		else die("zones: unknown option %s", argv[i]);
	}
	if (!wld || !tab || !skl || !zon)
		die("usage: qcook zones --world <wld> --tables <zones> --skeleton <skl> --chunks <zon>");
	remove(skl);                              /* (a map kept whole leaves none) */
	remove(zon);

	long flen;
	uint8_t *f = read_file(wld, &flen);
	H = (const qw_header *) f;
	if ((size_t) flen < sizeof *H || memcmp(H->magic, QWORLD_MAGIC, 4))
		die("%s: not a %s world", wld, QWORLD_MAGIC);
	NODES = (const qw_node *) (f + H->ofs_nodes);
	FACES = f + H->ofs_faces;
	VERTS = (const qw_vert *) (f + H->ofs_verts);
	const uint32_t nn = H->num_nodes, nl = H->num_leaves;

	tables_t T;
	if (!read_tables(tab, &T)) {
		printf("qcook: zones: no table %s: the map stays whole\n", tab);
		free(f);
		return 0;
	}
	if (fm_from)
		translate_tables(&T, fm_from, fm_to);
	/* (a table may count the brush models' leaves one by one, as the cook
	   wrote them before they were shared, docs/porting-notes.md §58: the
	   world's leaves are the same, the camera's never past them) */
	if ((T.num_leaves != nl && (T.num_leaves < nl || nl <= H->num_visleafs)) || T.num_faces != H->num_faces) {
		printf("qcook: zones: %s is for %u leaves, %u faces; the world has %u, %u: the map stays whole\n",
		       tab, T.num_leaves, T.num_faces, nl, H->num_faces);
		free(f);
		return 0;
	}
	const uint32_t nz = T.num_zones;

	/* the world's nodes (its root's tree); the others are the brush models' */
	uint8_t *world = xcalloc(nn, 1);
	{
		uint32_t *stack = xmalloc((nn + 1) * sizeof *stack), sp = 0;
		if (!(H->root & 0x80000000u))
			stack[sp++] = H->root;
		while (sp) {
			uint32_t n = stack[--sp];
			if (n >= nn || world[n])
				die("%s: a bad world tree (node %u)", wld, n);
			world[n] = 1;
			for (int c = 0; c < 2; c++)
				if (!(NODES[n].child[c] & 0x80000000u))
					stack[sp++] = NODES[n].child[c];
		}
		free(stack);
	}

	/* the world's records, each once (its id its visibility slot); the
	   world nodes holding a record some chunk takes: the patched ones */
	uint8_t *seen = xcalloc(H->num_faces, 1), *patched = xcalloc(nn, 1);
	uint32_t world_records = 0;
	for (uint32_t n = 0; n < nn; n++) {
		if (!world[n])
			continue;
		for (int sd = 0; sd < 2; sd++) {
			uint32_t at, end;
			block_of(n, sd, &at, &end);
			while (at < end) {
				const qw_record *r = (const qw_record *) (FACES + at);
				if (!r->size || at + r->size > end || r->face_id >= H->num_faces || seen[r->face_id])
					die("%s: node %u: a bad record at %u", wld, n, at);
				seen[r->face_id] = 1;
				world_records++;
				for (uint32_t z = 0; z < nz && !patched[n]; z++)
					patched[n] = T.member[z][r->face_id];
				at += r->size;
			}
		}
	}
	if (world_records != H->num_faces)
		die("%s: %u world records, %u faces", wld, world_records, H->num_faces);

	/* the skeleton's node order: the patched nodes, the end node, the
	   world's others, the brush models', then the sentinel */
	uint32_t *new_of = xmalloc(nn * sizeof *new_of), *old_at = xmalloc((nn + 1) * sizeof *old_at);
	uint32_t k = 0, E = 0;
	for (uint32_t n = 0; n < nn; n++)
		if (world[n] && patched[n])
			old_at[k] = n, new_of[n] = k++;
	const uint32_t W = k;
	old_at[k++] = QW_NONE;                    /* the end node */
	for (uint32_t n = 0; n < nn; n++)
		if (world[n] && !patched[n])
			old_at[k] = n, new_of[n] = k++, E++;
	for (uint32_t n = 0; n < nn; n++)
		if (!world[n])
			old_at[k] = n, new_of[n] = k++;
	const uint32_t nn2 = k;                   /* nn + 1 */
	if ((long) nn2 * QW_NODE_WORDS >= QW_NOPARENT)
		die("%u nodes: parent offsets do not fit 16 bits", nn2);

	/* the nodes; the skeleton's face stream: the brush models' blocks in
	   their nodes' order, their vertices into its own pool. The world's
	   blocks are all empty at the stream's start until a chunk is patched in
	   (engine/zone.c), so are the never patched ones' for good */
	buf_t bstream;
	memset(&bstream, 0, sizeof bstream);
	pool_t bpool;
	pool_init(&bpool);
	qw_node *nodes2 = xcalloc(nn2 + 1, sizeof *nodes2);
	uint32_t brecords = 0;
	for (uint32_t i = 0; i < nn2; i++) {
		uint32_t o = old_at[i];
		qw_node *d = &nodes2[i];
		if (o == QW_NONE) {                   /* the end node: no walk reaches it */
			d->child[0] = d->child[1] = 0x80000000u;   /* (leaf 0) */
			d->parent = QW_NOPARENT;
			continue;
		}
		*d = NODES[o];
		d->visframe = 0;
		d->dlstamp  = 0;
		for (int c = 0; c < 2; c++)
			if (!(d->child[c] & 0x80000000u))
				d->child[c] = new_of[d->child[c]];
		if (d->parent != QW_NOPARENT)
			d->parent = (uint16_t) (new_of[d->parent / QW_NODE_WORDS] * QW_NODE_WORDS);
		if (world[o]) {
			d->faces[0] = d->faces[1] = 0;
			continue;
		}
		for (int sd = 0; sd < 2; sd++) {
			d->faces[sd] = (uint32_t) bstream.len;
			uint32_t at, end;
			block_of(o, sd, &at, &end);
			while (at < end) {
				const qw_record *r = (const qw_record *) (FACES + at);
				if (!r->size || at + r->size > end || r->face_id != 0)
					die("%s: brush model node %u: a bad record at %u", wld, o, at);
				put_record(&bstream, r, &bpool, 0);
				brecords++;
				at += r->size;
			}
		}
	}
	nodes2[nn2].faces[0] = (uint32_t) bstream.len;   /* the sentinel: the stream's end */

	/* the chunks (their UVs as in the world: mapped per texture set below):
	   per zone, the patched nodes' records it takes, block by block, the
	   patch words, its own pool */
	buf_t *chunk = xcalloc(nz, sizeof *chunk);
	uint32_t max_bytes = 0, min_bytes = ~0u, max_patch = 0, total = 0, chunk_records = 0;
	for (uint32_t z = 0; z < nz; z++) {
		pool_t pool;
		pool_init(&pool);
		buf_t recs, patch;
		memset(&recs, 0, sizeof recs);
		memset(&patch, 0, sizeof patch);
		uint32_t skip = 0, nrec = 0, npatch = 0;
		for (uint32_t i = 0; i <= W; i++) {
			uint32_t size[2] = { 0, 0 };
			if (i < W)
				for (int sd = 0; sd < 2; sd++) {
					uint32_t at, end;
					block_of(old_at[i], sd, &at, &end);
					size_t before = recs.len;
					for (; at < end; at += ((const qw_record *) (FACES + at))->size) {
						const qw_record *r = (const qw_record *) (FACES + at);
						if (T.member[z][r->face_id]) {
							put_record(&recs, r, &pool, 1);
							nrec++;
						}
					}
					size[sd] = (uint32_t) (recs.len - before);
				}
			if (i < W && !size[0] && !size[1]) {
				skip++;
				continue;
			}
			for (; skip > QZP_MAX_SKIP; skip -= QZP_MAX_SKIP + 1) {   /* (256 empty nodes a word) */
				uint32_t w = QZP_MAX_SKIP << 24;
				buf_put(&patch, &w, sizeof w);
				npatch++;
			}
			if (size[0] / 4 > QZP_MAX_WORDS || size[1] / 4 > QZP_MAX_WORDS)
				die("zone %u: node %u: blocks of %u / %u B, past a patch word's", z, i, size[0], size[1]);
			uint32_t w = skip << 24 | size[0] / 4 << 12 | size[1] / 4;   /* (i == W: the end node's) */
			buf_put(&patch, &w, sizeof w);
			npatch++;
			skip = 0;
		}
		if (npatch > 0xffff)
			die("zone %u: %u patch words", z, npatch);
		qw_chunk hd;
		memset(&hd, 0, sizeof hd);
		hd.zone        = (uint16_t) z;
		hd.num_patch   = (uint16_t) npatch;
		hd.ofs_records = (uint32_t) (sizeof hd + patch.len);
		hd.len_records = (uint32_t) recs.len;
		hd.ofs_verts   = (uint32_t) ((hd.ofs_records + recs.len + QW_STREAM_TAIL + 7) & ~7u);
		hd.num_verts   = pool.n;
		hd.num_records = nrec;
		buf_t *b = &chunk[z];
		buf_put(b, &hd, sizeof hd);
		buf_put(b, patch.p, patch.len);
		buf_put(b, recs.p, recs.len);
		buf_zero(b, QW_STREAM_TAIL);
		buf_align(b, 8);
		put_pool(b, &pool);
		if (b->len > max_bytes) max_bytes = (uint32_t) b->len;
		if (b->len < min_bytes) min_bytes = (uint32_t) b->len;
		if (npatch > max_patch) max_patch = npatch;
		total += (uint32_t) b->len;
		chunk_records += nrec;
		free(recs.p);
		free(patch.p);
		pool_free(&pool);
	}

	/* the camera leaves' sets: (zone, its three nearest), deduplicated (a
	   halfword a leaf: the finer cuts make more than 255) */
	qw_zone_set *sets = xmalloc(nl * sizeof *sets);
	uint32_t nsets = 0;
	uint16_t *leaf_set = xmalloc(nl * sizeof *leaf_set);
	for (uint32_t l = 0; l < nl; l++) {
		if (!l || l > H->num_visleafs) {
			leaf_set[l] = QZ_LEAF_NONE;       /* the camera in solid: its zone kept (the
			                                     brush models' leaves: never the camera's) */
			continue;
		}
		qw_zone_set st = { (uint8_t) T.leaf_zone[l], { QZ_NONE, QZ_NONE, QZ_NONE } };
		for (int j = 0; j < 3; j++)
			if (T.leaf_near[l][j] >= 0)
				st.near[j] = (uint8_t) T.leaf_near[l][j];
		uint32_t i = 0;
		while (i < nsets && memcmp(&sets[i], &st, sizeof st))
			i++;
		if (i == nsets) {
			if (nsets >= QZ_LEAF_NONE)
				die("%s: more than %u zone sets", tab, QZ_LEAF_NONE);
			sets[nsets++] = st;
		}
		leaf_set[l] = (uint16_t) i;
	}

	/* the slots: ZONE_SLOTS of them (but no more than the zones), each the
	   biggest chunk in whole sectors (a chunk is read whole) */
	const uint32_t slot_bytes = sectors_of(max_bytes) * SECTOR;
	const uint32_t nslots = ZONE_SLOTS < nz ? ZONE_SLOTS : nz;

	/* the skeleton: the header (the zones' after it), the sections */
	uint32_t stamp = fnv(2166136261u, &flen, sizeof flen);
	stamp = fnv(stamp, &nz, sizeof nz);
	stamp = fnv(stamp, &W, sizeof W);
	for (uint32_t z = 0; z < nz; z++)
		stamp = fnv(stamp, &chunk[z].len, sizeof chunk[z].len);
	for (uint32_t z = 0; z < nz; z++)
		((qw_chunk *) chunk[z].p)->stamp = stamp;

	qw_header h = *H;
	memcpy(h.magic, QWORLD_ZONED_MAGIC, 4);
	h.num_nodes   = nn2;
	h.num_records = brecords;                 /* (the loader's loop: the brush models') */
	h.root        = new_of[H->root];
	qw_zone_header zh;
	memset(&zh, 0, sizeof zh);
	zh.num_zones     = nz;
	zh.num_slots     = nslots;
	zh.slot_bytes    = slot_bytes;
	zh.world_nodes   = W;
	zh.num_sets      = nsets;
	zh.reach         = T.reach;
	zh.stamp         = stamp;
	zh.whole_records = H->num_records;
	zh.whole_bytes   = (uint32_t) flen;

	qw_leaf *leaves2 = xmalloc(nl * sizeof *leaves2);
	memcpy(leaves2, f + H->ofs_leaves, nl * sizeof *leaves2);
	for (uint32_t l = 0; l < nl; l++) {
		leaves2[l].visframe = 0;
		if (leaves2[l].parent != QW_NOPARENT)
			leaves2[l].parent = (uint16_t) (new_of[leaves2[l].parent / QW_NODE_WORDS] * QW_NODE_WORDS);
	}
	qw_bmodel *bm2 = xmalloc((H->num_bmodels + 1) * sizeof *bm2);
	memcpy(bm2, f + H->ofs_bmodels, H->num_bmodels * sizeof *bm2);
	for (uint32_t m = 1; m < H->num_bmodels; m++)
		if (!(bm2[m].root & 0x80000000u))
			bm2[m].root = new_of[bm2[m].root];

	buf_t s;
	memset(&s, 0, sizeof s);
	buf_put(&s, &h, sizeof h);
	buf_put(&s, &zh, sizeof zh);
	h.ofs_nodes   = (uint32_t) s.len; buf_put(&s, nodes2, (nn2 + 1) * sizeof *nodes2);
	h.ofs_leaves  = (uint32_t) s.len; buf_put(&s, leaves2, nl * sizeof *leaves2);
	h.ofs_marks   = (uint32_t) s.len; buf_put(&s, f + H->ofs_marks, H->num_marks * 2); buf_align4(&s);
	h.ofs_vis     = (uint32_t) s.len; buf_put(&s, f + H->ofs_vis, H->len_vis); buf_align4(&s);
	h.ofs_faces   = (uint32_t) s.len; buf_put(&s, bstream.p, bstream.len);
	h.len_faces   = (uint32_t) bstream.len;
	buf_zero(&s, QW_STREAM_TAIL);
	h.ofs_views   = (uint32_t) s.len; buf_put(&s, f + H->ofs_views, H->num_views * sizeof(qw_view));
	buf_align4(&s);
	h.ofs_verts   = (uint32_t) s.len; h.num_verts = bpool.n; put_pool(&s, &bpool);
	h.ofs_ambient = (uint32_t) s.len; buf_put(&s, f + H->ofs_ambient, nl * 2); buf_align4(&s);
	h.ofs_clip    = (uint32_t) s.len; buf_put(&s, f + H->ofs_clip, H->len_clip); buf_align4(&s);
	h.ofs_bmodels = (uint32_t) s.len; buf_put(&s, bm2, H->num_bmodels * sizeof *bm2);
	h.ofs_ents    = (uint32_t) s.len; buf_put(&s, f + H->ofs_ents, H->ofs_strings - H->ofs_ents);
	h.ofs_strings = (uint32_t) s.len; buf_put(&s, f + H->ofs_strings, H->len_strings); buf_align4(&s);
	/* the chunks' order in the file: the player's start's zone and its
	   nearest first (the load's first reads, one sweep), then outward
	   through the sets' neighbourhoods (neighbouring zones side by side on
	   the disc), any zone no set reaches last (docs/porting-notes.md §84) */
	uint32_t *ord = xmalloc(nz * sizeof *ord), *rank = xmalloc(nz * sizeof *rank), no = 0;
	for (uint32_t z = 0; z < nz; z++)
		rank[z] = ~0u;
#define PUSH(z) do { if ((uint32_t) (z) < nz && rank[z] == ~0u) { rank[z] = no; ord[no++] = (uint32_t) (z); } } while (0)
	if (H->has_spawn) {
		uint32_t l0 = leaf_of_point(f, H->spawn);
		if (l0 < nl && T.leaf_zone[l0] >= 0) {
			PUSH(T.leaf_zone[l0]);
			for (int j = 0; j < 3; j++)
				if (T.leaf_near[l0][j] >= 0)
					PUSH(T.leaf_near[l0][j]);
		}
	}
	for (uint32_t i = 0; i < no; i++)
		for (uint32_t l = 0; l < nl; l++)
			if (T.leaf_zone[l] == (int16_t) ord[i])
				for (int j = 0; j < 3; j++)
					if (T.leaf_near[l][j] >= 0)
						PUSH(T.leaf_near[l][j]);
	for (uint32_t z = 0; z < nz; z++)
		PUSH(z);
#undef PUSH
	qw_chunk_dir *dir = xcalloc(nz, sizeof *dir);
	for (uint32_t i = 0, sector = 1; i < nz; i++) {   /* (sector 0: the file's header) */
		uint32_t z = ord[i];
		dir[z].sector = sector;
		dir[z].bytes  = (uint32_t) chunk[z].len;
		sector += sectors_of(dir[z].bytes);
	}
	zh.ofs_chunks   = (uint32_t) s.len; buf_put(&s, dir, nz * sizeof *dir);
	zh.ofs_leaf_set = (uint32_t) s.len; buf_put(&s, leaf_set, nl * sizeof *leaf_set); buf_align4(&s);
	zh.ofs_sets     = (uint32_t) s.len; buf_put(&s, sets, nsets * sizeof *sets);
	memcpy(s.p, &h, sizeof h);
	memcpy(s.p + sizeof h, &zh, sizeof zh);

	/* zoned only when the skeleton and its slots take less than the whole
	   world */
	const uint32_t resident = (uint32_t) s.len + nslots * sectors_of(max_bytes) * SECTOR;
	printf("qcook: zones %s: %u zones, %u records in them (the world's %u), chunks %u..%u B, %u KB on the "
	       "disc; %u nodes patched (%u never, %u the brush models'), %u patch words at most; %u sets; the "
	       "skeleton %u B + %u slots of %u B = %u KB, the whole %u KB", wld, nz, chunk_records, H->num_faces,
	       min_bytes, max_bytes, total / 1024, W, E, nn - W - E, max_patch, nsets, (uint32_t) s.len, nslots,
	       slot_bytes, resident / 1024, (uint32_t) (flen / 1024));
	if (nz < 2 || resident >= (uint32_t) flen) {
		printf(": the map stays whole\n");
	} else {
		printf(": zoned, %u KB freed\n", (uint32_t) (flen - resident) / 1024);
		write_file(skl, &s);
		/* the chunks (their UVs on the texture set's tiles: the world's) */
		buf_t zf;
		memset(&zf, 0, sizeof zf);
		qz_file fh;
		memcpy(fh.magic, QZONES_MAGIC, 4);
		fh.stamp     = stamp;
		fh.num_zones = nz;
		buf_put(&zf, &fh, sizeof fh);
		buf_align(&zf, SECTOR);
		for (uint32_t i = 0; i < nz; i++) {
			uint32_t z = ord[i];
			if (zf.len != (size_t) dir[z].sector * SECTOR)
				die("zone %u: at byte %zu, its directory says sector %u", z, zf.len, dir[z].sector);
			buf_put(&zf, chunk[z].p, chunk[z].len);
			buf_align(&zf, SECTOR);
		}
		write_file(zon, &zf);
		printf("qcook:   %s: %zu KB\n", zon, zf.len / 1024);
		free(zf.p);
	}

	for (uint32_t z = 0; z < nz; z++) {
		free(chunk[z].p);
		free(T.member[z]);
	}
	free(chunk); free(dir); free(ord); free(rank); free(T.member); free(T.leaf_zone); free(T.leaf_near);
	free(sets); free(leaf_set); free(leaves2); free(bm2); free(s.p); free(bstream.p); pool_free(&bpool);
	free(nodes2); free(new_of); free(old_at); free(world); free(seen); free(patched); free(f);
	return 0;
}
