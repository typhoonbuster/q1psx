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
#include "surf.h"
#include "bsp.h"

void r_init(void);

/* Set up the view for this frame. `pos` is the camera position in integer world
   units; `ang` is PITCH/YAW/ROLL in q1psx degrees (20.12). Call once per frame
   before any r_wireframe call. */
void r_begin_view(const int32_t pos[3], const vec3_t ang);

/* Draw an edge list as flat-colour lines. `verts` are world-space SVECTORs,
   `edges` is `2*nedges` vertex indices. Lines with an endpoint behind the near
   plane are skipped. Returns the number of lines actually emitted (capped —
   no culling yet, phase 5.4). */
int r_wireframe(const SVECTOR *verts, int nverts,
                const uint16_t *edges, int nedges,
                int r, int g, int b);

/* Draw every cooked textured surface from the current view — no visibility
   culling, face-budgeted. The SELECT fallback view. Needs qtx_open() first.
   Returns faces drawn. */
int r_surfaces(const surf_t *s);

/* Draw the world model culled to the camera's PVS + view frustum: locate the
   camera leaf, decompress its PVS, walk the BSP front-to-back rejecting whole
   subtrees against the frustum, draw each visible face once. `pos` is the
   camera in integer world units, `ang` PITCH/YAW/ROLL in q1psx degrees.
   Needs qtx_open() + bsp_load(). Returns faces drawn. */
int r_world(const surf_t *s, const bsp_t *b,
            const int32_t pos[3], const vec3_t ang);

/* Per-frame r_world diagnostics (valid after the call). */
extern int r_dbg_nodes;       /* BSP nodes visited                     */
extern int r_dbg_leaves;      /* leaves that passed PVS + frustum      */
extern int r_dbg_faces;       /* unique faces handed to draw_face      */
extern int r_dbg_drop_near;   /* faces dropped: all verts behind near  */
extern int r_dbg_drop_back;   /* faces dropped: back-face              */
extern int r_dbg_drop_sky;    /* faces dropped: sky                    */
extern int r_dbg_drop_slot, r_dbg_drop_tiny;
extern int r_dbg_vz_lo, r_dbg_vz_hi;   /* view_z range over all face verts */
extern int r_dbg_vm[4];                /* stashed view fwd row + t         */
extern int r_dbg_us_walk, r_dbg_us_draw;   /* ~microseconds: BSP walk, draw_face */

/* quality knobs (set from main.c, for A/B testing the perf trade-offs) */
extern int r_subdiv;    /* 1 = adaptive triangle subdivision on (anti-warp)  */
extern int r_farclip;   /* 0 = off, else skip leaves whose box is > N units  */
extern int r_minface;   /* 0 = off, else skip faces smaller than N px        */
extern int r_screen_h;  /* GTE projection distance (FOV): 160 ~= 90 deg      */
void r_set_screen_h(int h);
