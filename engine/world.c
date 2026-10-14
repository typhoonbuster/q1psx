#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "cd.h"
#include "mem.h"
#include "qtex.h"
#include "world.h"

world_t world;

const char *const fidelity_name[FIDELITY_LEVELS] = { "low", "medium", "high", "ultra" };

static int load_textures(int fid)
{
	char name[24];
	snprintf(name, sizeof name, "\\START%d.TEX;1", fid);

	uint32_t mark = mem_level_mark(), size;
	const uint8_t *f = cd_load(name, &size);
	if (!f || size < sizeof(qtex_header) || memcmp(f, QTEX_MAGIC, 4)) {
		printf("world: bad texture file %s\n", name);
		mem_level_release(mark);
		return 0;
	}
	const qtex_header *h = (const qtex_header *) f;
	const qtex_rect   *r = (const qtex_rect *) (f + sizeof *h);

	RECT clut = { QTEX_CLUT_X, QTEX_CLUT_Y, 256, 1 };
	LoadImage(&clut, (const uint32_t *) h->clut);
	for (uint32_t i = 0; i < h->num_rects; i++) {
		RECT rc = { (int16_t) r[i].x, (int16_t) r[i].y, (int16_t) r[i].w, (int16_t) r[i].h };
		LoadImage(&rc, (const uint32_t *) (f + r[i].data_ofs));
	}
	DrawSync(0);
	printf("world: %s — %u texture pages -> VRAM\n", name, h->num_rects);

	mem_level_release(mark);   /* pixels live in VRAM now */
	return 1;
}

#define REL(base, ofs) ((uint32_t) (base) + (ofs))

int world_load(int fid)
{
	mem_level_reset();
	memset(&world, 0, sizeof world);

	if (!load_textures(fid))
		return 0;

	char name[24];
	snprintf(name, sizeof name, "\\START%d.WLD;1", fid);
	uint32_t size;
	uint8_t *f = cd_load(name, &size);
	if (!f || size < sizeof(qw_header) || memcmp(f, QWORLD_MAGIC, 4)) {
		printf("world: bad world file %s\n", name);
		return 0;
	}
	const qw_header *h = (const qw_header *) f;

	world.nodes       = (qw_node *) (f + h->ofs_nodes);
	world.leaves      = (qw_leaf *) (f + h->ofs_leaves);
	world.marks       = (uint16_t *) (f + h->ofs_marks);
	world.faces       = f + h->ofs_faces;
	world.views       = (const qw_view *) (f + h->ofs_views);
	world.num_nodes   = h->num_nodes;
	world.num_leaves  = h->num_leaves;
	world.num_faces   = h->num_faces;
	world.num_views   = h->num_views;
	world.num_records = h->num_records;
	world.num_pieces  = h->num_pieces;
	world.vis_rowbytes = (h->num_visleafs + 7) >> 3;
	world.root        = &world.nodes[h->root];
	world.has_spawn   = (int) h->has_spawn;
	world.spawn_yaw   = h->spawn_yaw;
	memcpy(world.spawn, h->spawn, sizeof world.spawn);

	/* links: indices / offsets -> pointers (leaf pointers tagged with bit 0) */
	const uint8_t *vis = f + h->ofs_vis;
	for (uint32_t i = 0; i < h->num_nodes; i++) {
		qw_node *n = &world.nodes[i];
		for (int c = 0; c < 2; c++) {
			uint32_t ch = n->child[c];
			n->child[c] = (ch & 0x80000000u)
			            ? (uint32_t) &world.leaves[ch & 0x7fffffffu] | 1u
			            : (uint32_t) &world.nodes[ch];
		}
		n->parent   = n->parent == QW_NONE ? 0 : (uint32_t) &world.nodes[n->parent];
		n->visframe = 0;
		for (int k = 0; k < 3; k++)
			n->faces[k] = REL(world.faces, n->faces[k]);
	}
	for (uint32_t i = 0; i < h->num_leaves; i++) {
		qw_leaf *l = &world.leaves[i];
		l->marks    = (uint32_t) &world.marks[l->marks];
		l->parent   = l->parent == QW_NONE ? 0 : (uint32_t) &world.nodes[l->parent];
		l->vis      = l->vis == QW_NONE ? 0 : REL(vis, l->vis);
		l->visframe = 0;
	}

	world.face_visframe = mem_level_alloc(h->num_faces * 2);
	if (!world.face_visframe)
		return 0;
	memset(world.face_visframe, 0, h->num_faces * 2);

	printf("world: %s — %u nodes, %u leaves, %u records, %u polys, arena %u KB\n",
	       name, h->num_nodes, h->num_leaves, h->num_records, h->num_pieces,
	       mem_level_used() / 1024);
	return 1;
}

const qw_leaf *world_point_leaf(const int32_t pos[3])
{
	int32_t p[3] = { pos[0] * QW_VSCALE, pos[1] * QW_VSCALE, pos[2] * QW_VSCALE };
	uint32_t at = (uint32_t) world.root;
	while (!(at & 1)) {
		const qw_node *n = (const qw_node *) at;
		int32_t d = n->type < 3
		          ? p[n->type] * 4096 - n->dist
		          : n->n[0] * p[0] + n->n[1] * p[1] + n->n[2] * p[2] - n->dist;
		at = n->child[d <= 0];   /* Quake: d > 0 is the front child */
	}
	return (const qw_leaf *) (at & ~1u);
}
