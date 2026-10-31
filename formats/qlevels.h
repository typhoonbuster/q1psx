/*
 * q1psx — the quality levels: three separate axes, each from low to ultra
 * (docs/porting-notes.md §39). The cook writes one file per level of the
 * axes that change the cooked data. Since §52 a disc carries one level of
 * each: the textures the build's (medium or high, Q1PSX_TEXTURES), the
 * geometry and the models low (the effects, an axis of the engine's own
 * until §119, make the port's high counts of particles):
 *
 *   textures   the world's and the map's models' texture resolution
 *              <MAP>_T<n>.TEX / .TX4 (formats/qtex.h)
 *   geometry   the map's polygons: low leaves the T-junctions unrepaired,
 *              ultra cuts the faces on a finer grid for the vertex light
 *              <MAP>_G<n>.WLD (formats/qworld.h)
 *   models     the alias models' triangles: the projectiles (GAME_<n>.DAT)
 *              and the map's own models, the items' (<MAP>_M<n>.MDL;
 *              formats/qgame.h); the view models stay Quake's
 *
 * A level's number is the same on every axis (2 is high on each); an axis
 * has only the levels listed in its mask.
 */

#ifndef Q1PSX_QLEVELS_H
#define Q1PSX_QLEVELS_H

#define QL_LOW     0
#define QL_MEDIUM  1
#define QL_HIGH    2
#define QL_ULTRA   3
#define QL_LEVELS  4

enum { QL_TEXTURES, QL_GEOMETRY, QL_MODELS, QL_AXES };

/* the levels each axis has (bit n: level n) */
#define QL_TEXTURE_MASK  0x06   /* medium, high             */
#define QL_GEOMETRY_MASK 0x01   /* low                      */
#define QL_MODEL_MASK    0x01   /* low                      */

/* the ultra geometry's light grid: the faces are cut every this many
   texels as well, so their vertex light follows the lightmap (Quake's
   luxels are 16 texels) */
#define QL_LIGHT_GRID   128
/* the brush models' faces at every geometry level: a door's or a bridge's
   face is one big polygon whose corner may sit in a shadow of Quake's
   lightmap (E1M1's bridge: a 64-texel shadow at one corner), which four
   corner lights would spread over half of it; a 128-texel cell keeps it
   to a quarter (64: to the cell, at twice the records). Only the faces
   whose corner lights miss their lightmap (QL_BM_LIGHT_ERR) are cut
   (docs/porting-notes.md §96: 1-3 KB a map of records and arena) */
#define QL_BM_LIGHT_GRID 128
#define QL_BM_LIGHT_ERR  56   /* the face is cut when its corner lights' Gouraud misses a
                                 luxel of its lightmap by more than this (0..255, after the gain) */

#endif
