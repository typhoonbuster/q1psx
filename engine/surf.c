#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qsrf.h"
#include "surf.h"

int surf_load(const void *blob, int len, surf_t *out)
{
	if (len < (int) sizeof(qsrf_header) || memcmp(blob, QSRF_MAGIC, 4) != 0)
		return 0;

	const qsrf_header *h = (const qsrf_header *) blob;
	const uint8_t     *p = (const uint8_t *) blob + sizeof(qsrf_header);

	long vb = (long) h->num_verts * 8;
	long fb = (long) h->num_faces * (long) sizeof(qsrf_face);
	long xb = (long) h->num_faceverts * (long) sizeof(qsrf_facevert);
	if ((long) sizeof(qsrf_header) + vb + fb + xb > len)
		return 0;

	out->num_verts = (int) h->num_verts;
	out->num_faces = (int) h->num_faces;
	out->num_fv    = (int) h->num_faceverts;
	out->verts     = (const SVECTOR *) p;
	out->faces     = (const qsrf_face *) (p + vb);
	out->fv        = (const qsrf_facevert *) (p + vb + fb);
	memcpy(out->mins, h->mins, sizeof out->mins);
	memcpy(out->maxs, h->maxs, sizeof out->maxs);

	printf("surf_load: %d verts, %d faces, %d faceverts\n",
	       out->num_verts, out->num_faces, out->num_fv);
	return 1;
}
