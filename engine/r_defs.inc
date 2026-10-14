# q1psx — renderer asm definitions. Mirrors engine/render.h (r_state_t),
# formats/qworld.h (qw_node / qw_leaf / qw_record) and engine/prof.h.
# render.c static-asserts the C side of every offset used here.

# ---- r_state_t ---------------------------------------------------------------
.set RS_ROOT,        0
.set RS_FVBASE,      4
.set RS_LEAVES,      8
.set RS_ROWBYTES,   12
.set RS_CAM,        16
.set RS_FRAMECOUNT, 28
.set RS_VISFRAME,   32
.set RS_VIEW_LEAF,  36
.set RS_OT_WORLD,   40
.set RS_PKT,        44
.set RS_PKT_END,    48
.set RS_SZ_FAST,    52
.set RS_ROT,        56
.set RS_TR,         76
.set RS_ABSROT,     88
.set RS_STAT_NODES,   108
.set RS_STAT_RECORDS, 112
.set RS_STAT_VERTS,   116
.set RS_STAT_POLYS,   120
.set RS_STAT_SKIPPED, 124
.set RS_TICKS_BLOCK,  128
.set RS_TICKS_MARK,   132
.set RS_FORCE_MARK,   136
.set RS_LEAVES_END,   140

# ---- qw_node (56 B) ----------------------------------------------------------
.set N_C,         0     # int16 c[3] (+ pad): lwc2-ready
.set N_H,         8
.set N_N,        16     # int16 n[3]
.set N_TYPE,     22
.set N_DIST,     24
.set N_CHILD,    28     # child[2] pointers, bit 0 = leaf
.set N_VISFRAME, 36
.set N_PARENT,   40
.set N_FACES,    44     # faces[3]: front start, back start, back end

# ---- qw_leaf (36 B) ----------------------------------------------------------
.set L_C,         0
.set L_H,         8
.set L_VISFRAME, 16
.set L_NMARKS,   18
.set L_MARKS,    20
.set L_PARENT,   24
.set L_VIS,      28
.set L_SIZE,     36

# ---- qw_record (12 B) + polygons (12 B) -------------------------------------
.set R_FACE_ID,   0
.set R_NVERTS,    2
.set R_NQUADS,    3
.set R_NTRIS,     4
.set R_SIZE,      6
.set R_CLUT,      8
.set R_TPAGE,    10
.set R_VERTS,    12
.set POLY_SIZE,  12

# ---- scratchpad map (base 0x1F800000) ---------------------------------------
.set SP_STACK,    0x000   # walk stack, 64 words
.set SP_SXY,      0x100   # projected XY per record vertex (index x4)
.set SP_SZ,       0x200   # SZ per record vertex
.set SP_CAM,      0x300   # cam[3], vertex units
.set SP_FVBASE,   0x30c
.set SP_SZFAST,   0x310
.set SP_CODE4,    0x314   # POLY_FT4 code word
.set SP_CODE3,    0x318   # POLY_FT3 code word
.set SP_NEXTREC,  0x31c
.set SP_NODES,    0x320   # counters
.set SP_RECORDS,  0x324
.set SP_VERTS,    0x328
.set SP_POLYS,    0x32c
.set SP_SKIPPED,  0x330
.set SP_TBLOCK,   0x334
.set SP_T0,       0x338
.set SP_FULL,     0x33c   # non-zero once the packet arena is full

.set TIMER2,      0x1120  # root counter 2 value, offset from 0x1F800000's
                          # upper half (0x1F801120)

# ---- GPU packets --------------------------------------------------------------
.set CODE_FT4,    0x2d808080   # textured quad, opaque, raw texture
.set CODE_FT3,    0x25808080   # textured triangle, opaque, raw texture
.set BACKFACE_EPS, 164         # Quake's 0.01 unit, x4096 x4
