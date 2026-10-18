#include <stdint.h>
#include <string.h>

#include "gdata.h"
#include "mdl.h"
#include "render.h"
#include "rlist.h"
#include "stream.h"
#include "world.h"

r_item_t r_items[RL_MAX_ITEMS];
uint16_t r_lists[256];
static r_ent_t r_ents[RL_MAX_ENTS];
static int     n_items, n_ents, n_lists;
static qw_leaf *touched[RL_MAX_LISTS];

void rl_begin(void)
{
	n_items = n_ents = n_lists = 0;
	mdl_new_frame();
}

int rl_num_items(void)
{
	return n_items;
}

static int lists_used;      /* (the last frame's, for the bench) */

int rl_num_lists(void)
{
	return lists_used;
}

/* Quake's AngleVectors: forward, right, up of (pitch, yaw, roll), x4096 */
static void angle_vectors(int pitch, int yaw, int roll, int32_t f[3], int32_t r[3], int32_t u[3])
{
	int32_t sy = r_sin(yaw), cy = r_cos(yaw), sp = r_sin(pitch), cp = r_cos(pitch);
	int32_t sr = r_sin(roll), cr = r_cos(roll);
	f[0] = (cp * cy) >> 12;
	f[1] = (cp * sy) >> 12;
	f[2] = -sp;
	r[0] = ((-sr * sp >> 12) * cy >> 12) + (cr * sy >> 12);
	r[1] = ((-sr * sp >> 12) * sy >> 12) - (cr * cy >> 12);
	r[2] = -sr * cp >> 12;
	u[0] = ((cr * sp >> 12) * cy >> 12) + (sr * sy >> 12);
	u[1] = ((cr * sp >> 12) * sy >> 12) - (sr * cy >> 12);
	u[2] = cr * cp >> 12;
}

/* the entity's GTE rotation: view rotation x model-to-world, and its
   translation, by the GTE (engine/ent_mat.s). Quake's model axes are
   forward, -right, up (r_alias.c R_AliasSetUpTransform, with the pitch
   negated: entity pitches are backward): the model-to-world matrix's
   columns */
void rl_ent_matrix(r_ent_t *e, const int16_t cols[3][4], const int16_t o[4]);

static void ent_rotation(r_ent_t *e, int yaw, int pitch, int roll, const int16_t o[4])
{
	int32_t f[3], r[3], u[3];
	angle_vectors(-pitch, yaw, roll, f, r, u);
	int16_t cols[3][4];
	for (int i = 0; i < 3; i++) {
		cols[0][i] = (int16_t) f[i];
		cols[1][i] = (int16_t) -r[i];
		cols[2][i] = (int16_t) u[i];
	}
	cols[0][3] = cols[1][3] = cols[2][3] = 0;
	rl_ent_matrix(e, cols, o);
}

static void ent_model(r_ent_t *e, const qg_model *m, int frame, int light)
{
	if (frame < 0 || frame >= m->num_frames)
		frame = 0;
	const uint8_t *recs;
	if (m->flags & QGF_MAP) {               /* a map's own model: its block, its textures */
		e->pool = (uint32_t) ((const qw_vert *) (world.mgeo + m->ofs_frames) + (uint32_t) frame * m->num_verts);
		recs    = world.mgeo + m->ofs_records;
	} else {
		e->pool = (uint32_t) gd_frame(m, frame);
		recs    = gd_records(m);
	}
	if (m->flags & QGF_SPRITE) {            /* record k = frame k */
		const uint8_t *r = recs;
		for (int k = 0; k < frame; k++)
			r += ((const qw_record *) r)->size;
		e->recs     = (uint32_t) r;
		e->recs_end = (uint32_t) (r + ((const qw_record *) r)->size);
	} else {
		e->recs     = (uint32_t) recs;
		e->recs_end = (uint32_t) (recs + m->len_records);
	}
	e->tex = (m->flags & QGF_MAP) ? (uint32_t) world.model_tex[m - world.models] : (uint32_t) &m->tex;
	if (light > 170) light = 170;
	if (light < 0) light = 0;
	e->light = (uint32_t) light;
	e->dots  = 0;
	e->flags = 0;
}

r_ent_t *rl_model(const qg_model *m, int frame, const int32_t org[3], int yaw, int pitch,
                  int roll, int light)
{
	if (n_ents >= RL_MAX_ENTS)
		return 0;
	r_ent_t *e = &r_ents[n_ents++];
	ent_model(e, m, frame, light);
	/* translation: the view's, plus the origin through the view rotation */
	int16_t o[4];
	for (int k = 0; k < 3; k++)
		o[k] = (int16_t) (org[k] * QW_VSCALE / R_POS_SUB);
	o[3] = 0;
	if (m->flags & QGF_SPRITE) {            /* a billboard: its quad is in the view plane */
		memset(e->rot, 0, sizeof e->rot);
		e->rot[0] = e->rot[4] = e->rot[8] = R_MATRIX_ONE;
		rl_ent_matrix(e, 0, o);
	} else {
		ent_rotation(e, yaw, pitch, roll, o);
	}
	if (!(m->flags & QGF_FLAT))
		e->dots = (uint32_t) (gdata.dots + ((yaw * QG_LIGHT_YAWS / ANG_TURN) & (QG_LIGHT_YAWS - 1)) * QG_NORMALS);
	return e;
}

/* A streamed model (formats/qstream.h): its frame expanded (engine/mdl.c),
   its matrix the model-to-world columns scaled by its steps (Quake's
   R_AliasSetUpTransform folds the scale in the same way), the origin moved
   by its own origin turned with it. */
r_ent_t *rl_model_stream(int a, int frame, const int32_t org[3], int yaw, int pitch, int roll, int light)
{
	const qst_model *m = stream_model(a);
	if (!m || n_ents >= RL_MAX_ENTS)
		return 0;
	if (frame < 0 || frame >= m->num_frames)
		frame = 0;
	const qw_vert *pool = mdl_frame(a, m, frame);
	if (!pool)
		return 0;
	r_ent_t *e = &r_ents[n_ents++];
	e->pool     = (uint32_t) pool;
	e->recs     = (uint32_t) ((const uint8_t *) m + m->ofs_records);
	e->recs_end = e->recs + m->len_records;
	e->tex      = (uint32_t) stream_model_tex(a);
	if (light > 170) light = 170;
	if (light < 0) light = 0;
	e->light = (uint32_t) light;
	e->dots  = 0;
	e->flags = 0;
	int32_t f[3], r[3], u[3];
	angle_vectors(-pitch, yaw, roll, f, r, u);
	int16_t cols[3][4], o[4];
	for (int i = 0; i < 3; i++) {
		cols[0][i] = (int16_t) ((f[i] * m->scale[0] + 32768) >> 16);
		cols[1][i] = (int16_t) ((-r[i] * m->scale[1] + 32768) >> 16);
		cols[2][i] = (int16_t) ((u[i] * m->scale[2] + 32768) >> 16);
		int32_t mo = f[i] * m->origin[0] - r[i] * m->origin[1] + u[i] * m->origin[2];   /* x 4096 x 16 */
		o[i] = (int16_t) (org[i] * QW_VSCALE / R_POS_SUB + ((mo + 32768) >> 16));
	}
	cols[0][3] = cols[1][3] = cols[2][3] = 0;
	o[3] = 0;
	rl_ent_matrix(e, (const int16_t (*)[4]) cols, o);
	if (!(m->flags & QSTM_FLAT))
		e->dots = (uint32_t) (gdata.dots + ((yaw * QG_LIGHT_YAWS / ANG_TURN) & (QG_LIGHT_YAWS - 1)) * QG_NORMALS);
	return e;
}

r_ent_t *rl_bmodel(const qw_bmodel *bm, const int32_t org[3])
{
	if (n_ents >= RL_MAX_ENTS)
		return 0;
	r_ent_t *e = &r_ents[n_ents++];
	const uint32_t *vr = (const uint32_t *) r_state.rot;   /* unrotated: the view's rotation */
	uint32_t       *er = (uint32_t *) e->rot;
	er[0] = vr[0]; er[1] = vr[1]; er[2] = vr[2]; er[3] = vr[3]; er[4] = vr[4];
	int16_t o[4];
	for (int k = 0; k < 3; k++)
		o[k] = (int16_t) (org[k] * QW_VSCALE / R_POS_SUB);
	o[3] = 0;
	if (!(org[0] | org[1] | org[2])) {            /* in place: the view's translation */
		e->tr[0] = r_state.tr[0];
		e->tr[1] = r_state.tr[1];
		e->tr[2] = r_state.tr[2];
	} else {
		rl_ent_matrix(e, 0, o);                   /* (the translation only) */
	}
	e->pool     = (uint32_t) world.verts;
	e->recs     = bm->root;
	e->recs_end = 0;
	e->tex      = (uint32_t) world.textures;
	e->dots     = 0;
	e->light    = 0;
	e->flags    = RE_BMODEL;
	for (int k = 0; k < 3; k++)
		e->cam[k] = r_state.cam[k] - o[k];
	return e;
}

void rl_view_model(r_ent_t *e)
{
	if (!e)
		return;
	for (int i = 0; i < 6; i++)
		e->rot[i] = (int16_t) (e->rot[i] * 4);
	e->tr[0] *= 4;
	e->tr[1] *= 4;
	e->flags = RE_VIEW;
}

/* the leaf's list id in its vis word's top byte, with bit 31 set: the walk
   tests the word's sign for "anything to draw here" (r_world.s wl_ents) */
static int leaf_list(qw_leaf *leaf)
{
	int id = (int) (leaf->vis >> 24) & 0x7f;
	if (!id) {
		if (n_lists >= RL_MAX_LISTS)
			return 0;
		touched[n_lists++] = leaf;
		id = n_lists;
		leaf->vis |= (uint32_t) (id | 0x80) << 24;
		r_lists[id] = 0;
	}
	return id;
}

void rl_add_pieces(const qw_leaf *leaf, const uint8_t *recs, uint32_t len)
{
	if (!leaf || !len || n_items >= RL_MAX_ITEMS)
		return;
	int id = leaf_list((qw_leaf *) leaf);
	if (!id)
		return;
	r_item_t *it = &r_items[n_items];
	it->next   = r_lists[id];
	it->type   = RI_PIECES;
	it->colour = 0;
	it->pad    = (uint16_t) len;
	it->ent    = (uint32_t) recs;
	r_lists[id] = (uint16_t) (n_items + 1);
	n_items++;
}

void rl_add_model(const qw_leaf *leaf, r_ent_t *e)
{
	if (!leaf || !e || n_items >= RL_MAX_ITEMS)
		return;
	int id = leaf_list((qw_leaf *) leaf);
	if (!id)
		return;
	r_item_t *it = &r_items[n_items];
	it->next   = r_lists[id];
	it->type   = 1;
	it->colour = 0;
	it->ent    = (uint32_t) e;
	r_lists[id] = (uint16_t) (n_items + 1);
	n_items++;
}

void rl_add_particle(const qw_leaf *leaf, const int32_t pos[3], int colour)
{
	if (!leaf || n_items >= RL_MAX_ITEMS)
		return;
	int id = leaf_list((qw_leaf *) leaf);
	if (!id)
		return;
	r_item_t *it = &r_items[n_items];
	it->next   = r_lists[id];
	it->type   = 0;
	it->colour = (uint8_t) colour;
	it->x = (int16_t) (pos[0] * QW_VSCALE / R_POS_SUB);
	it->y = (int16_t) (pos[1] * QW_VSCALE / R_POS_SUB);
	it->z = (int16_t) (pos[2] * QW_VSCALE / R_POS_SUB);
	it->ent = 0;
	r_lists[id] = (uint16_t) (n_items + 1);
	n_items++;
}

void rl_end(void)
{
	for (int i = 0; i < n_lists; i++)
		touched[i]->vis &= 0x00ffffffu;
#ifdef Q1PSX_BENCH
	lists_used = n_lists;
#endif
	n_lists = 0;
}
