#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qgeo.h"   /* formats/qgeo.h */
#include "geo.h"

int geo_load(const void *blob, int len, geo_t *out)
{
	if (len < (int) sizeof(qgeo_header) || memcmp(blob, QGEO_MAGIC, 4) != 0)
		return 0;

	const qgeo_header *h = (const qgeo_header *) blob;
	const uint8_t     *p = (const uint8_t *) blob + sizeof(qgeo_header);

	long need = (long) sizeof(qgeo_header)
	          + (long) h->num_verts * 8
	          + (long) h->num_edges * 4;
	if (need > len)
		return 0;

	out->num_verts = (int) h->num_verts;
	out->num_edges = (int) h->num_edges;
	out->verts     = (const SVECTOR *) p;
	out->edges     = (const uint16_t *) (p + (long) h->num_verts * 8);
	memcpy(out->mins, h->mins, sizeof out->mins);
	memcpy(out->maxs, h->maxs, sizeof out->maxs);

	printf("geo_load: %d verts, %d edges, bounds [%d %d %d]..[%d %d %d]\n",
	       out->num_verts, out->num_edges,
	       out->mins[0], out->mins[1], out->mins[2],
	       out->maxs[0], out->maxs[1], out->maxs[2]);
	return 1;
}
