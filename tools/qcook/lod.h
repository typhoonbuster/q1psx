/*
 * q1psx — a model's level of detail by its vertices' order (formats/
 * qstream.h QSTM_LOD, docs/porting-notes.md §88): the vertices collapsed
 * one by one onto a neighbour, the least visible first, each with the
 * error its loss costs in world units; from it the numbering (the most
 * needed vertex first), each vertex's parent, the band of distance each
 * triangle vanishes at.
 */

#ifndef QCOOK_LOD_H
#define QCOOK_LOD_H

#include <stdint.h>

#include "../../formats/qstream.h"

/* a model as the ordering sees it */
typedef struct {
	int           nverts, ntris;
	int           nframes;      /* the frames played (the others do not count) */
	const double *pos;          /* [nframes][nverts][3], world units            */
	const int   (*tris)[4];     /* facesfront, v0, v1, v2                       */
	const int   (*st)[3];       /* onseam, s, t                                 */
	int           skinw;
} lod_mesh;

typedef struct {
	int       nverts, ntris, ncollapse;
	int      *perm;             /* [nverts]: the new index's old vertex           */
	int      *rank;             /* [nverts]: an old vertex's new index            */
	uint16_t  nv[QST_LOD_BANDS];/* each band's vertices kept: the new numbering's
	                               first nv (non-increasing; nv[0] = nverts)      */
	uint8_t  *group;            /* [ntris]: the band the triangle vanishes at (1 ..
	                               QST_LOD_BANDS - 1); QST_LOD_BANDS: never within
	                               the draw distance                              */
	uint16_t *parent;           /* [nverts], the new numbering: a vertex's parent
	                               (a lower index); QST_LOD_NONE: never collapses  */
	double   *err;              /* [ncollapse]: the collapses' errors in their
	                               order, made monotonic, world units             */
	int       nerr_dist;        /* collapses within the draw distance             */
} lod_plan;

/* the plan at a tolerance in pixels (the PS1 view 320 wide, 90 degrees: the
   GTE's H is 160) up to the draw distance in world units */
void lod_plan_make(const lod_mesh *mesh, double tol_px, int draw_dist, lod_plan *out);
void lod_plan_free(lod_plan *p);

#endif
