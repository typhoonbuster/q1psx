#include <stdint.h>
#include <string.h>

#include "gdata.h"
#include "render.h"
#include "rlist.h"
#include "world.h"

r_item_t r_items[RL_MAX_ITEMS];
uint16_t r_lists[256];
static r_ent_t r_ents[RL_MAX_ENTS];
static int     n_items, n_ents, n_lists;
static qw_leaf *touched[RL_MAX_LISTS];

void rl_begin(void)
{
	n_items = n_ents = n_lists = 0;
}

int rl_num_items(void)
{
	return n_items;
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

/* the entity's GTE rotation: view rotation x model-to-world; Quake's model
   axes are forward, -right, up (r_alias.c R_AliasSetUpTransform, with the
   pitch negated: entity pitches are backward) */
static void ent_rotation(r_ent_t *e, int yaw, int pitch, int roll)
{
	int32_t f[3], r[3], u[3];
	angle_vectors(-pitch, yaw, roll, f, r, u);
	int32_t mw[3][3];                       /* model -> world, x4096 */
	for (int i = 0; i < 3; i++) {
		mw[i][0] = f[i];
		mw[i][1] = -r[i];
		mw[i][2] = u[i];
	}
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			int32_t s = 0;
			for (int k = 0; k < 3; k++)
				s += r_state.rot[i * 3 + k] * mw[k][j];
			e->rot[i * 3 + j] = (int16_t) (s >> 12);
		}
	e->rot[9] = 0;
}

static void ent_model(r_ent_t *e, const qg_model *m, int frame, int light)
{
	if (frame < 0 || frame >= m->num_frames)
		frame = 0;
	e->pool = (uint32_t) gd_frame(m, frame);
	const uint8_t *recs = gd_records(m);
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
	e->tex = (uint32_t) &m->tex;
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
	if (m->flags & QGF_SPRITE) {            /* a billboard: its quad is in the view plane */
		memset(e->rot, 0, sizeof e->rot);
		e->rot[0] = e->rot[4] = e->rot[8] = R_MATRIX_ONE;
	} else {
		ent_rotation(e, yaw, pitch, roll);
	}
	if (!(m->flags & QGF_FLAT))
		e->dots = (uint32_t) (gdata.dots + ((yaw * QG_LIGHT_YAWS / ANG_TURN) & (QG_LIGHT_YAWS - 1)) * QG_NORMALS);
	/* translation: the view's, plus the origin through the view rotation */
	int32_t o[3];
	for (int k = 0; k < 3; k++)
		o[k] = org[k] * QW_VSCALE / R_POS_SUB;
	for (int i = 0; i < 3; i++)
		e->tr[i] = r_state.tr[i] + ((r_state.rot[i * 3] * o[0] + r_state.rot[i * 3 + 1] * o[1] +
		                             r_state.rot[i * 3 + 2] * o[2]) >> 12);
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

static int leaf_list(qw_leaf *leaf)
{
	int id = (int) (leaf->vis >> 24);
	if (!id) {
		if (n_lists >= RL_MAX_LISTS)
			return 0;
		touched[n_lists++] = leaf;
		id = n_lists;
		leaf->vis |= (uint32_t) id << 24;
		r_lists[id] = 0;
	}
	return id;
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
	n_lists = 0;
}
