/*
 * q1psx — the quality levels: four separate axes, each from low to ultra
 * (docs/porting-notes.md §39). The cook writes one file per level of the
 * axes that change the cooked data, the player picks each in Graphics:
 *
 *   textures   the world's and the map's models' texture resolution
 *              <MAP>_T<n>.TEX / .TX4 (formats/qtex.h)
 *   geometry   the map's polygons: low leaves the T-junctions unrepaired,
 *              ultra cuts the faces on a finer grid for the vertex light
 *              <MAP>_G<n>.WLD (formats/qworld.h)
 *   models     the alias models' triangles: the projectiles (GAME_<n>.DAT)
 *              and the map's own models, the items' (<MAP>_M<n>.MDL;
 *              formats/qgame.h); the view models stay Quake's
 *   effects    the particles: their counts (the engine alone)
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

enum { QL_TEXTURES, QL_GEOMETRY, QL_MODELS, QL_EFFECTS, QL_AXES };

/* the levels each axis has (bit n: level n) */
#define QL_TEXTURE_MASK  0x0f   /* low, medium, high, ultra */
#define QL_GEOMETRY_MASK 0x0d   /* low, high, ultra         */
#define QL_MODEL_MASK    0x07   /* low, medium, high        */
#define QL_EFFECT_MASK   0x0f   /* low, medium, high, ultra */

/* the ultra geometry's light grid: the faces are cut every this many
   texels as well, so their vertex light follows the lightmap (Quake's
   luxels are 16 texels) */
#define QL_LIGHT_GRID   128

#endif
