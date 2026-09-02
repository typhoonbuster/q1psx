/*
 * q1psx — cooked map surfaces (QSF1) in RAM. See formats/qsrf.h.
 */

#pragma once

#include <stdint.h>
#include <psxgte.h>

#include "qsrf.h"   /* formats/qsrf.h */

typedef struct {
	int                  num_verts;
	int                  num_faces;
	int                  num_fv;
	const SVECTOR       *verts;
	const qsrf_face     *faces;
	const qsrf_facevert *fv;
	int16_t              mins[3], maxs[3];
} surf_t;

int surf_load(const void *blob, int len, surf_t *out);
