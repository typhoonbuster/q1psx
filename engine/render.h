/*
 * q1psx — 3D rendering via the GTE.
 *
 * Phase 5.1a: bring the GTE up and draw wireframe geometry from a first-person
 * camera. World coordinates are 16-bit integers (SVECTOR), per the fixed-point
 * decision (porting-notes §2.1). No BSP yet — that's 5.1b.
 */

#pragma once

#include <stdint.h>
#include <psxgte.h>

#include "mathlib.h"

void r_init(void);

/* Set up the view for this frame. `pos` is the camera position in integer world
   units; `ang` is PITCH/YAW/ROLL in q1psx degrees (20.12). Call once per frame
   before any r_wireframe call. */
void r_begin_view(const int32_t pos[3], const vec3_t ang);

/* Draw an edge list as flat-colour lines. `verts` are world-space SVECTORs,
   `edges` is `2*nedges` vertex indices. Lines with an endpoint behind the near
   plane are skipped. */
void r_wireframe(const SVECTOR *verts, int nverts,
                 const uint16_t *edges, int nedges,
                 int r, int g, int b);
