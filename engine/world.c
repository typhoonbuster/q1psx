#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "cd.h"
#include "mem.h"
#include "q_fmt.h"
#include "qtex.h"
#include "gamma.h"
#include "world.h"
#include "zone.h"

world_t world;
static uint8_t *faces_end;           /* the face stream's end, its records lit (world_relight) */

/* Every Quake map, in play order; the disc carries some of them
   (scripts/cook.sh: episode 1 by default, all with COOK_MAPS=all). */
static const char *const quake_maps[] = {
	"START", "E1M1", "E1M2", "E1M3", "E1M4", "E1M5", "E1M6", "E1M7", "E1M8",
	"E2M1", "E2M2", "E2M3", "E2M4", "E2M5", "E2M6", "E2M7",
	"E3M1", "E3M2", "E3M3", "E3M4", "E3M5", "E3M6", "E3M7",
	"E4M1", "E4M2", "E4M3", "E4M4", "E4M5", "E4M6", "E4M7", "E4M8", "END",
};
#define NUM_QUAKE_MAPS (int) (sizeof quake_maps / sizeof quake_maps[0])

static const char *disc_maps[NUM_QUAKE_MAPS];
static int         num_disc_maps;

static uint8_t map_on_disc[NUM_QUAKE_MAPS];

/* cd_list_dirs's call on \LVL: a directory named as a map holds that map
   (its section: docs/porting-notes.md §84) */
static void note_map_dir(const char *name)
{
	for (int i = 0; i < NUM_QUAKE_MAPS; i++)
		if (!strcmp(name, quake_maps[i]))
			map_on_disc[i] = 1;
}

Q_COLD int world_scan_maps(void)
{
	num_disc_maps = 0;
	memset(map_on_disc, 0, sizeof map_on_disc);
	cd_list_dirs("\\LVL", note_map_dir);          /* (one directory read) */
	for (int i = 0; i < NUM_QUAKE_MAPS; i++)
		if (map_on_disc[i])
			disc_maps[num_disc_maps++] = quake_maps[i];
	printf("world: %d maps on the disc\n", num_disc_maps);
	return num_disc_maps;
}

int world_num_maps(void)
{
	return num_disc_maps;
}

const char *world_map_name(int i)
{
	return i >= 0 && i < num_disc_maps ? disc_maps[i] : "?";
}

int world_find_map(const char *name)
{
	for (int i = 0; i < num_disc_maps; i++) {
		const char *a = disc_maps[i], *b = name;
		while (*a && *b && (*a | 0x20) == (*b | 0x20))
			a++, b++;
		if (!*a && !*b)
			return i;
	}
	return -1;
}

int world_registered(void)
{
	return world_find_map("E2M1") >= 0;
}

/* load progress: bytes of the files before the current one, and in all */
static world_progress_fn load_progress;
static uint32_t          load_base, load_total;

static void load_poll(uint32_t bytes)
{
	uint32_t done = load_base + bytes;
	load_progress(done < load_total ? done : load_total, load_total);
}

static void load_step(uint32_t base)
{
	load_base = base;
	if (load_progress)
		load_poll(0);
}

/* LoadImage sends the rectangle's w x h halfwords by DMA in units of 16
   words, rounding a shorter last unit up: up to 60 bytes past the data go
   to the GPU too, as commands. A stray word made DuckStation's GPU (and
   would make the machine's) lose the command stream: the load hung on the
   next DrawSync; PCSX-Redux ignores them. A rectangle whose size is not a
   multiple of 64 bytes (a 4-bit CLUT row of an odd count of CLUTs: 32
   bytes each) goes up from a zero-padded copy (GP0 NOPs after the data). */
void vram_upload(const uint8_t *data, const qtex_rect *r)
{
	if (gamma_level() && r->y >= 480 && r->y + r->h <= 505 && r->x + r->w <= 320) {
		gamma_upload(data, r->x, r->y, r->w, r->h);  /* (a CLUT row: the Options' Brightness) */
		return;
	}
	RECT rc = { (int16_t) r->x, (int16_t) r->y, (int16_t) r->w, (int16_t) r->h };
	uint32_t len = (uint32_t) r->w * r->h * 2;
	if (len & 63) {
		uint32_t mark = mem_level_mark();
		uint8_t *tmp = mem_level_alloc((len + 63) & ~63u);
		if (tmp) {
			memcpy(tmp, data, len);
			memset(tmp + len, 0, 64 - (len & 63));
			LoadImage(&rc, (const uint32_t *) tmp);
			DrawSync(0);                     /* (the DMA done before the copy goes) */
			mem_level_release(mark);
			return;
		}
	}
	LoadImage(&rc, (const uint32_t *) data);
}

static void upload_rects(const uint8_t *f, const qtex_rect *r, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		vram_upload(f + r[i].data_ofs, &r[i]);
}

/* what the map's texture rectangles take of its 4-bit pages (world_t
   page4_rows: the streamed skins get the rest, engine/stream.c); a CLUT
   row (y >= 480) takes none */
static void note_rects(const qtex_rect *r, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (r[i].y >= 480 || r[i].x < QTEX_PAGE_X0)
			continue;
		int p = (r[i].x - QTEX_PAGE_X0) / 64, rows = r[i].y + r[i].h;
		if (p < QTEX_MAP_PAGES && rows > world.page4_rows[p])
			world.page4_rows[p] = (uint16_t) rows;
	}
}

/* The map's texture set into VRAM: QTX8 (4bpp, 16 colours per texture).
   The file is transient but for its tables (the texture words the
   renderer reads, the animated textures), moved to its start: the arena's
   bottom (the arena was reset). */
static int load_textures(const char *name)
{
	gamma_keep_map(NULL, NULL, 0);
	uint32_t mark = mem_level_mark(), size;
	uint8_t *f = cd_load(name, &size, load_progress ? load_poll : NULL);
	uint32_t num_slots = 0, ofs_slots = 0, ofs_anims = 0, nanims = 0;
	if (f && size >= sizeof(qtex4_header) && !memcmp(f, QTEX4_MAGIC, 4)) {
		const qtex4_header *h = (const qtex4_header *) f;
		upload_rects(f, (const qtex_rect *) (f + sizeof *h), h->num_rects);
		note_rects((const qtex_rect *) (f + sizeof *h), h->num_rects);
		num_slots = h->num_slots; ofs_slots = h->ofs_slots;
		nanims    = h->num_anims; ofs_anims = h->ofs_anims;
		printf("world: %s — 4bpp, %u rects -> VRAM\n", name, h->num_rects);
	} else {
		printf("world: bad texture file %s\n", name);
		mem_level_release(mark);
		return 0;
	}
	DrawSync(0);                     /* pixels live in VRAM now */
	uint32_t tables = num_slots * (uint32_t) sizeof(qw_tex);
	uint32_t len    = ofs_anims + nanims * (uint32_t) sizeof(qw_anim) - ofs_slots;
	if (ofs_anims != ofs_slots + tables || ofs_slots + len > size) {
		printf("world: bad texture tables in %s\n", name);
		mem_level_release(mark);
		return 0;
	}
	/* the CLUT rows as cooked (VRAM rows 480-504), kept after the tables for
	   the Options' Brightness (engine/gamma.c): their rectangles (data_ofs
	   from the copy), then their halfwords; above the file until the tables
	   have moved */
	const qtex_rect *rr = (const qtex_rect *) (f + sizeof(qtex4_header));
	uint32_t nr = ((const qtex4_header *) f)->num_rects, ncl = 0, clen = 0;
	for (uint32_t i = 0; i < nr; i++)
		if (rr[i].y >= 480 && rr[i].y + rr[i].h <= 505) {
			ncl++;
			clen += (uint32_t) rr[i].w * rr[i].h * 2;
		}
	uint32_t klen = ncl * (uint32_t) sizeof(qtex_rect) + clen;
	uint8_t *keep = klen ? mem_level_alloc(klen) : NULL;
	if (keep) {
		qtex_rect *kr = (qtex_rect *) keep;
		uint32_t at = ncl * (uint32_t) sizeof(qtex_rect), k = 0;
		for (uint32_t i = 0; i < nr; i++)
			if (rr[i].y >= 480 && rr[i].y + rr[i].h <= 505) {
				uint32_t n = (uint32_t) rr[i].w * rr[i].h * 2;
				kr[k] = rr[i];
				kr[k].data_ofs = at;
				memcpy(keep + at, f + rr[i].data_ofs, n);
				at += n;
				k++;
			}
	}
	memmove(f, f + ofs_slots, len);
	world.textures  = (const qw_tex *) f;
	world.num_tex   = num_slots;
	world.anims     = (const qw_anim *) (f + tables);
	world.num_anims = nanims;
	uint32_t keep_at = (len + 15) & ~15u;            /* (the arena's 16-byte steps) */
	if (keep) {
		memmove(f + keep_at, keep, klen);
		gamma_keep_map((const qtex_rect *) (f + keep_at), f + keep_at, ncl);
	}
	mem_level_release(mark + keep_at + (keep ? ((klen + 15) & ~15u) : 0));
	return 1;
}

/* The map's own models (formats/qgame.h qmm_header): their tables, a
   texture table each from the set's (an alias model's skin does not wrap). */
static int load_models(const char *name)
{
	uint32_t size;
	uint8_t *f = cd_load(name, &size, load_progress ? load_poll : NULL);
	if (!f || size < sizeof(qmm_header) || memcmp(f, QMAPMDL_MAGIC, 4)) {
		printf("world: bad models file %s\n", name);
		return 0;
	}
	const qmm_header *h = (const qmm_header *) f;
	world.num_models = h->num_models;
	world.mgeo       = f + h->ofs_geo;
	if (!h->num_models)
		return 1;
	const qw_model *qm = (const qw_model *) (f + h->ofs_models);
	world.models    = mem_level_alloc(h->num_models * sizeof *world.models);
	world.model_tex = mem_level_alloc(h->num_models * sizeof *world.model_tex);
	if (!world.models || !world.model_tex)
		return 0;
	for (uint32_t i = 0; i < h->num_models; i++) {
		world.models[i] = qm[i].m;
		qw_tex *t = mem_level_alloc(QW_MODEL_TEX * sizeof *t);
		if (!t)
			return 0;
		for (int k = 0; k < qm[i].num_tex && k < QW_MODEL_TEX; k++)
			t[k] = qm[i].tex[k] < world.num_tex ? world.textures[qm[i].tex[k]] : world.textures[0];
		if (!(world.models[i].flags & QGF_FLAT))           /* (an alias model: no wrap) */
			t[0].window = 0xE2000000u;
		world.models[i].tex = t[0];
		world.model_tex[i]  = t;
		if (qm[i].qim < QIM_COUNT)
			world.model_of[qm[i].qim] = (int16_t) i;
	}
	return 1;
}

#define REL(base, ofs) ((uint32_t) (base) + (ofs))

Q_COLD int world_load(int map, world_progress_fn progress)
{
	zone_abort();                        /* (a chunk read stopped before the arena goes) */
	mem_level_reset();
	memset(&world, 0, sizeof world);
	faces_end = 0;
	for (int i = 0; i < QIM_COUNT; i++)
		world.model_of[i] = -1;
	if (map < 0 || map >= num_disc_maps)
		return 0;

	char name[32], tex_name[32], mdl_name[32];
	uint32_t tex_size = 0, wld_size = 0, mdl_size = 0;
	q_snprintf(tex_name, sizeof tex_name, "\\LVL\\%s\\%s.TX4;1", disc_maps[map], disc_maps[map]);
	q_snprintf(name, sizeof name, "\\LVL\\%s\\%s.WLD;1", disc_maps[map], disc_maps[map]);
	q_snprintf(mdl_name, sizeof mdl_name, "\\LVL\\%s\\%s.MDL;1", disc_maps[map], disc_maps[map]);
	load_progress = progress;
	if (progress) {
		cd_exists(tex_name, &tex_size);
		cd_exists(name, &wld_size);
		cd_exists(mdl_name, &mdl_size);
		load_total = tex_size + wld_size + mdl_size;
		load_step(0);
	}

	if (!load_textures(tex_name))
		return 0;

	load_step(tex_size);                 /* the pages are in VRAM */
	uint32_t size;
	uint8_t *f = cd_load(name, &size, progress ? load_poll : NULL);
	int zoned = f && size >= sizeof(qw_header) + sizeof(qw_zone_header) && !memcmp(f, QWORLD_ZONED_MAGIC, 4);
	if (!f || size < sizeof(qw_header) || (memcmp(f, QWORLD_MAGIC, 4) && !zoned)) {
		printf("world: bad world file %s\n", name);
		return 0;
	}
	const qw_header *h = (const qw_header *) f;
	if (h->num_tex != world.num_tex) {
		printf("world: %s names %u textures, its set %u\n", name, h->num_tex, world.num_tex);
		return 0;
	}

	world.nodes       = (qw_node *) (f + h->ofs_nodes);
	world.leaves      = (qw_leaf *) (f + h->ofs_leaves);
	world.marks       = (uint16_t *) (f + h->ofs_marks);
	for (uint32_t i = 0; i < h->num_marks; i++)
		world.marks[i] <<= 1;            /* face index -> byte offset in
		                                    face_visframe (faces < 32768) */
	world.faces       = f + h->ofs_faces;
	world.views       = (const qw_view *) (f + h->ofs_views);
	world.verts       = (const qw_vert *) (f + h->ofs_verts);
	world.num_verts   = h->num_verts;
	world.ambient     = f + h->ofs_ambient;
	world.num_nodes   = h->num_nodes;
	world.num_leaves  = h->num_leaves;
	world.num_faces   = h->num_faces;
	world.num_views   = h->num_views;
	world.num_records = h->num_records;
	world.num_pieces  = h->num_pieces;
	/* PVS rows: leaves 1..num_visleafs, which a malformed map could put
	   past the leaf array (mark_pvs stamps every leaf a row names) */
	world.vis_leaves  = h->num_visleafs < h->num_leaves ? h->num_visleafs
	                  : h->num_leaves ? h->num_leaves - 1 : 0;
	world.vis_rowbytes = (world.vis_leaves + 7) >> 3;
	world.root        = &world.nodes[h->root];
	world.has_spawn   = (int) h->has_spawn;
	world.map_name    = disc_maps[map];
	world.spawn_yaw   = h->spawn_yaw;
	memcpy(world.spawn, h->spawn, sizeof world.spawn);

	/* links: indices / offsets -> pointers (leaf pointers tagged with bit 0).
	   Child pointers go through KUSEG (0x00xxxxxx, the same RAM): the asm
	   walk ORs them into its stack words as they are, flags in the top byte. */
	const uint8_t *vis = f + h->ofs_vis;
	for (uint32_t i = 0; i < h->num_nodes; i++) {
		qw_node *n = &world.nodes[i];
		for (int c = 0; c < 2; c++) {
			uint32_t ch = n->child[c];
			n->child[c] = ((ch & 0x80000000u)
			            ? (uint32_t) &world.leaves[ch & 0x7fffffffu] | 1u
			            : (uint32_t) &world.nodes[ch]) & 0x00ffffffu;
		}
		n->visframe = 0;                 /* parents stay word offsets */
		for (int k = 0; k < 2; k++)
			n->faces[k] = REL(world.faces, n->faces[k]);
	}
	world.nodes[h->num_nodes].faces[0] = REL(world.faces, world.nodes[h->num_nodes].faces[0]);
	for (uint32_t i = 0; i < h->num_leaves; i++) {
		qw_leaf *l = &world.leaves[i];
		l->marks    = (uint32_t) &world.marks[l->marks];
		l->vis      = l->vis == QW_NONE ? 0 : REL(vis, l->vis) & 0x00ffffffu;   /* KUSEG: the top
		                                                                        byte holds the leaf's
		                                                                        render list (rlist.h) */
		l->visframe = 0;
	}

	if (h->ofs_clip && h->len_clip >= sizeof(qw_clip_header)) {
		const qw_clip_header *ch = (const qw_clip_header *) (f + h->ofs_clip);
		world.planes     = (const qw_plane *) (ch + 1);
		world.clipnodes  = (const qw_clipnode *) (world.planes + ch->num_planes);
		world.hull1_root = ch->hull1_root;
		world.hull2_root = ch->hull2_root;
	}

	/* the brush models (their roots like the children: KUSEG, bit 0 for a
	   leaf), the entities and their strings */
	world.bmodels      = (qw_bmodel *) (f + h->ofs_bmodels);
	world.num_bmodels  = h->num_bmodels;
	for (uint32_t i = 1; i < h->num_bmodels; i++) {
		uint32_t r = world.bmodels[i].root;
		world.bmodels[i].root = ((r & 0x80000000u)
		                      ? (uint32_t) &world.leaves[r & 0x7fffffffu] | 1u
		                      : (uint32_t) &world.nodes[r]) & 0x00ffffffu;
	}
	world.ent_names    = (const uint16_t *) (f + h->ofs_ents);
	world.ents         = (const uint8_t *) (world.ent_names + QE_COUNT);
	world.num_ents     = h->num_ents;
	world.strings      = (const char *) (f + h->ofs_strings);
	world.worldtype    = h->worldtype;
	world.title        = world.strings + h->title;
	world.max_bm_depth = h->max_bm_depth;
	world.bmf_arena_recs  = h->bmf_arena_recs;
	world.bmf_arena_verts = h->bmf_arena_verts;

	world.face_visframe = mem_level_alloc(h->num_faces * 2);
	if (!world.face_visframe)
		return 0;
	memset(world.face_visframe, 0, h->num_faces * 2);

	/* each record's face index -> its byte offset in face_visframe (the
	   asm adds it to the array as it is; faces < 32768); its light bytes at
	   the Brightness's level (engine/gamma.c), their average / 4 into its
	   flags' bits 2 - 7 (the flat shading's colour: r_world.s RECLOOP opt) */
	{
		uint8_t *r = (uint8_t *) world.faces;
		for (uint32_t i = 0; i < h->num_records; i++) {
			qw_record *rec = (qw_record *) r;
			if (!rec->size)
				break;
			rec->face_id = (uint16_t) (rec->face_id << 1);
			r += rec->size;
		}
		faces_end = f + h->ofs_faces + h->len_faces;
		gamma_light_records((uint8_t *) world.faces, faces_end, h->num_records, gamma_light);
	}

	load_step(tex_size + wld_size);
	if (!load_models(mdl_name))
		return 0;
	/* a zoned world (formats/qworld.h "zones"): the skeleton read, its
	   world blocks empty until zone_preload makes a chunk current; the
	   slots last, under the world's cap */
	if (zoned && !zone_map_start(f, disc_maps[map]))
		return 0;

	printf("world: %s — %u nodes, %u leaves, %u records, %u polys, %u brush models, "
	       "%u entities, %u models, arena %u KB\n",
	       name, h->num_nodes, h->num_leaves,
	       zoned ? ((const qw_zone_header *) (f + sizeof *h))->whole_records : h->num_records, h->num_pieces,
	       h->num_bmodels ? h->num_bmodels - 1 : 0, h->num_ents, world.num_models, mem_level_used() / 1024);
	load_step(load_total);
	return 1;
}

/* the records' light bytes (the world's, or a zoned map's brush models')
   through t: the Brightness's light at another level (engine/main.c
   light_commit) */
Q_COLD void world_relight(const uint8_t *t)
{
	if (faces_end)
		gamma_light_records((uint8_t *) world.faces, faces_end, world.num_records, t);
}

/* the box (vertex units: centre c, half extents h) down the tree from a
   KUSEG node or leaf pointer */
static int box_leaves(uint32_t at, const int32_t c[3], const int32_t h[3], uint16_t *out,
                      int n, int max)
{
	for (;;) {
		if (at & 1) {
			const qw_leaf *l = (const qw_leaf *) ((at & ~1u) | 0x80000000u);
			if (l->contents != QW_CONTENTS_SOLID && n < max)
				out[n++] = (uint16_t) (l - world.leaves);
			return n;
		}
		const qw_node  *nd = (const qw_node *) (at | 0x80000000u);
		const qw_plane *pl = (const qw_plane *) ((const int32_t *) world.planes + nd->plane3);
		int32_t dist = pl->dist >> 1;    /* 1/8 units -> vertex units, x4096 */
		int32_t d, ext;
		if (pl->type < 3) {
			d   = c[pl->type] * 4096 - dist;
			ext = h[pl->type] * 4096;
		} else {
			d   = pl->n[0] * c[0] + pl->n[1] * c[1] + pl->n[2] * c[2] - dist;
			ext = (pl->n[0] < 0 ? -pl->n[0] : pl->n[0]) * h[0] +
			      (pl->n[1] < 0 ? -pl->n[1] : pl->n[1]) * h[1] +
			      (pl->n[2] < 0 ? -pl->n[2] : pl->n[2]) * h[2];
		}
		if (d - ext > 0) {
			at = nd->child[0];
		} else if (d + ext < 0) {
			at = nd->child[1];
		} else {
			n  = box_leaves(nd->child[0], c, h, out, n, max);
			at = nd->child[1];
		}
	}
}

/* The deepest node (or the leaf) whose region holds the whole box, world
   units, inclusive: each node above it has every point of the box on one
   side of its plane, by world_point_leaf's own integer test (d linear in
   p: its extremes over the box are at corners), so world_point_leaf_from
   it agrees with the descent from the root for every point of the box. */
uint32_t world_box_topnode(const int32_t mins[3], const int32_t maxs[3])
{
	int32_t lo[3], hi[3];
	for (int k = 0; k < 3; k++) {
		lo[k] = mins[k] * QW_VSCALE;
		hi[k] = maxs[k] * QW_VSCALE;
	}
	uint32_t at = (uint32_t) world.root;
	while (!(at & 1)) {
		const qw_node *n = (const qw_node *) at;
		const qw_plane *pl = (const qw_plane *) ((const int32_t *) world.planes + n->plane3);
		int32_t dist = pl->dist >> 1;
		int32_t dmin, dmax;
		if (pl->type < 3) {
			dmin = lo[pl->type] * 4096 - dist;
			dmax = hi[pl->type] * 4096 - dist;
		} else {
			dmin = dmax = -dist;
			for (int k = 0; k < 3; k++) {
				int32_t a = pl->n[k] * lo[k], b = pl->n[k] * hi[k];
				dmin += a < b ? a : b;
				dmax += a < b ? b : a;
			}
		}
		if (dmin > 0)
			at = n->child[0];             /* the whole box in front */
		else if (dmax <= 0)
			at = n->child[1];             /* the whole box behind (d <= 0) */
		else
			break;
	}
	return at;
}

int world_box_leaves(const int32_t mins[3], const int32_t maxs[3], uint16_t *out, int max)
{
	int32_t c[3], h[3];
	for (int k = 0; k < 3; k++) {
		/* 1/8 units -> vertex units (x4): the box rounded outwards */
		int32_t lo = mins[k] >> 1, hi = (maxs[k] + 1) >> 1;
		c[k] = (lo + hi) >> 1;
		h[k] = ((hi - lo) >> 1) + 1;
	}
	return box_leaves((uint32_t) world.root & 0x00ffffffu, c, h, out, 0, max);
}

/* Quake's RLE of a row (a zero byte, then the count of zero bytes) expanded
   into row[rowbytes], or XORed into it (a cooked delta row: formats/qworld.h
   qw_leaf vis, docs/porting-notes.md §90.9); the rows are the cook's, whole */
static void rle_expand(const uint8_t *in, uint8_t *row, uint32_t rowbytes)
{
	uint32_t o = 0;
	while (o < rowbytes) {
		if (*in) {
			row[o++] = *in++;
			continue;
		}
		uint32_t c = in[1];
		in += 2;
		while (c-- > 0 && o < rowbytes)
			row[o++] = 0;
	}
}

static void rle_xor(const uint8_t *in, uint8_t *row, uint32_t rowbytes)
{
	uint32_t o = 0;
	while (o < rowbytes) {
		if (*in) {
			row[o++] ^= *in++;
			continue;
		}
		o += in[1];
		in += 2;
	}
}

Q_COLD void world_vis_row(const qw_leaf *leaf, uint8_t *row, uint32_t rowbytes)
{
	const uint8_t *in = 0;
	if (leaf) {
		leaf = (const qw_leaf *) ((uintptr_t) leaf | 0x80000000u);
		if (leaf->vis)
			in = (const uint8_t *) ((leaf->vis & 0x00ffffffu) | 0x80000000u);
	}
	if (!in) {
		memset(row, 0xff, rowbytes);
		return;
	}
	uint32_t back = *in++;                        /* 0: the row itself; else the XOR against the
	                                                 plain row of the leaf `back` before */
	if (back) {
		const qw_leaf *base = leaf - back;
		rle_expand((const uint8_t *) ((base->vis & 0x00ffffffu) | 0x80000000u) + 1, row, rowbytes);
		rle_xor(in, row, rowbytes);
	} else {
		rle_expand(in, row, rowbytes);
	}
}

const qw_leaf *world_point_leaf(const int32_t pos[3])
{
	return world_point_leaf_from((uint32_t) world.root, pos);
}

/* The descent of world_point_leaf from `at` (a node or a tagged leaf): the
   leaf it gives the root's, for a point inside the box world_box_topnode
   took `at` from. engine/w_leaf.s is the base; this C follows it: the C
   engine's, and the bench's check (main.c bench_wleaf_diff). */
#if Q1PSX_ENGINE_C
#define world_point_leaf_from_c world_point_leaf_from
#endif
#if Q1PSX_ENGINE_C || defined(Q1PSX_BENCH)
const qw_leaf *world_point_leaf_from_c(uint32_t at, const int32_t pos[3])
{
	int32_t p[3] = { pos[0] * QW_VSCALE, pos[1] * QW_VSCALE, pos[2] * QW_VSCALE };
	while (!(at & 1)) {
		const qw_node *n = (const qw_node *) at;
		const qw_plane *pl = (const qw_plane *) ((const int32_t *) world.planes + n->plane3);
		int32_t dist = pl->dist >> 1;    /* 1/8 units -> vertex units, x4096 */
		int32_t d = pl->type < 3
		          ? p[pl->type] * 4096 - dist
		          : pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - dist;
		at = n->child[d <= 0];   /* Quake: d > 0 is the front child */
	}
	return (const qw_leaf *) (at & ~1u);
}
#endif
