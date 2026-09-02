#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qbsp.h"
#include "bsp.h"

/* ---- point / plane ----------------------------------------------------- */

/* 0 = point is on the plane's front (positive) side, 1 = back. Axial planes
   are exact; non-axial carry the ~1/4096 rounding of the cooked normal. */
int bsp_plane_side(const qbsp_plane *pl, const int32_t p[3])
{
	int64_t d;
	switch (pl->type) {
	case 0:  d = (int64_t) pl->n[0] * p[0]; break;
	case 1:  d = (int64_t) pl->n[1] * p[1]; break;
	case 2:  d = (int64_t) pl->n[2] * p[2]; break;
	default: d = (int64_t) pl->n[0] * p[0]
	           + (int64_t) pl->n[1] * p[1]
	           + (int64_t) pl->n[2] * p[2];
	}
	return (d - pl->dist) >= 0 ? 0 : 1;
}

int bsp_point_leaf(const bsp_t *b, const int32_t p[3])
{
	int idx = b->root;
	while (idx >= 0) {
		const qbsp_node *nd = &b->nodes[idx];
		idx = nd->child[bsp_plane_side(&b->planes[nd->plane], p)];
	}
	return -idx - 1;
}

/* ---- PVS ------------------------------------------------------------------ */

static uint8_t      pvs_buf[1024];   /* MAX_MAP_LEAFS / 8 */
static int          pvs_leaf = -1;
static const bsp_t *pvs_bsp;

int bsp_pvs_row(const bsp_t *b)
{
	int row = (b->num_visleaves + 7) >> 3;
	return row > (int) sizeof pvs_buf ? (int) sizeof pvs_buf : row;
}

const uint8_t *bsp_pvs(const bsp_t *b, int leaf)
{
	int row = bsp_pvs_row(b);

	if (leaf <= 0) {
		if (pvs_leaf > 0 && pvs_bsp == b)
			return pvs_buf;                 /* keep the last good set */
		memset(pvs_buf, 0xff, sizeof pvs_buf);
		return pvs_buf;
	}
	if (leaf == pvs_leaf && pvs_bsp == b)
		return pvs_buf;

	pvs_leaf = leaf;
	pvs_bsp  = b;

	int32_t visofs = b->leaves[leaf].visofs;
	if (visofs < 0 || (uint32_t) visofs >= b->vis_len) {
		memset(pvs_buf, 0xff, sizeof pvs_buf);
		return pvs_buf;
	}

	/* Quake RLE: a zero byte means "in[1] zero bytes follow". */
	const uint8_t *in   = b->vis + visofs;
	const uint8_t *vend  = b->vis + b->vis_len;
	uint8_t       *out   = pvs_buf;
	uint8_t       *oend  = pvs_buf + row;
	while (out < oend && in < vend) {
		if (*in) {
			*out++ = *in++;
			continue;
		}
		if (in + 1 >= vend)
			break;
		int c = in[1];
		in += 2;
		while (c-- > 0 && out < oend)
			*out++ = 0;
	}
	while (out < oend)
		*out++ = 0;
	return pvs_buf;
}

/* ---- load --------------------------------------------------------------- */

int bsp_load(const void *blob, int len, bsp_t *out)
{
	if (len < (int) sizeof(qbsp_header) || memcmp(blob, QBSP_MAGIC, 4) != 0)
		return 0;

	const qbsp_header *h = (const qbsp_header *) blob;
	const uint8_t     *p = (const uint8_t *) blob + sizeof *h;

	long pb = (long) h->num_planes    * (long) sizeof(qbsp_plane);
	long nb = (long) h->num_nodes     * (long) sizeof(qbsp_node);
	long lb = (long) h->num_leaves    * (long) sizeof(qbsp_leaf);
	long mb = (long) h->num_marksurf  * 2;
	if ((long) sizeof *h + pb + nb + lb + mb + (long) h->vis_len > len)
		return 0;
	if (h->root < 0 || (uint32_t) h->root >= h->num_nodes)
		return 0;

	out->num_planes    = (int) h->num_planes;
	out->num_nodes     = (int) h->num_nodes;
	out->num_leaves    = (int) h->num_leaves;
	out->num_visleaves = (int) h->num_visleaves;
	out->num_marksurf  = (int) h->num_marksurf;
	out->root          = (int) h->root;
	out->vis_len       = h->vis_len;

	out->planes   = (const qbsp_plane *) p;  p += pb;
	out->nodes    = (const qbsp_node *)  p;  p += nb;
	out->leaves   = (const qbsp_leaf *)  p;  p += lb;
	out->marksurf = (const uint16_t *)   p;  p += mb;
	out->vis      = p;

	memcpy(out->mins, h->mins, sizeof out->mins);
	memcpy(out->maxs, h->maxs, sizeof out->maxs);

	pvs_leaf = -1;
	pvs_bsp  = NULL;

	printf("bsp_load: %d nodes, %d leaves (%d vis), %d marksurf, %u B vis\n",
	       out->num_nodes, out->num_leaves, out->num_visleaves,
	       out->num_marksurf, out->vis_len);
	return 1;
}
