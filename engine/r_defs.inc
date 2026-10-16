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
.set RS_VIS_LASTMASK, 140
.set RS_OT_HEAD,      144
.set RS_STAT_SLOW,    148
.set RS_TICKS_SLOW,   152
.set RS_TEXTURES,     156
.set RS_VERTPOOL,     160
.set RS_NODES,        164
.set RS_PLANES,       168
.set RS_WARP_T2,      172
.set RS_WARP_K16,     176
.set RS_SLOW_BYTES,   180

# ---- qw_node (36 B) ----------------------------------------------------------
.set N_C,         0     # int16 c[3]: VXY0 at +0, VZ0 with lh at +4
.set N_VISFRAME,  6
.set N_H,         8     # int16 h[3]
.set N_PLANE3,   14     # plane index x 3: word offset in the plane table
.set N_PARENT,   16     # uint16 word offset into the node array, 0xffff: none
.set N_CHILD,    20     # child[2] pointers, bit 0 = leaf
.set N_FACES,    28     # faces[2]: front start, back start (back end = the
                        # next node's front start; a sentinel ends the array)
.set N_SIZE,     36

# ---- qw_plane (12 B, the clip section's table; dist x4096 in 1/8 units) ------
.set PN_N,        0
.set PN_TYPE,     6
.set PN_DIST,     8

# ---- qw_leaf (28 B) ----------------------------------------------------------
.set L_C,         0
.set L_VISFRAME,  6
.set L_H,         8
.set L_NMARKS,   14
.set L_MARKS,    16
.set L_VIS,      20
.set L_PARENT,   24     # uint16 word offset, like N_PARENT
.set L_SIZE,     28

# ---- qw_record (20 B) + polygons (12 B) -------------------------------------
.set R_FACE_ID,   0
.set R_NVERTS,    2
.set R_NQUADS,    3
.set R_NTRIS,     4
.set R_SIZE,      6
.set R_TEX,       8     # index into the qw_tex table (8 B: clut, tpage, window)
.set R_SR,       10     # bounding sphere radius
.set R_SC,       12     # bounding sphere centre (lwc2-ready)
.set R_POLYS,    18     # byte offset of the first quad / tri
.set R_VERTS,    20
.set POLY_SIZE,  12

# ---- scratchpad map (base 0x1F800000) ---------------------------------------
.set SP_STACK,    0x000   # walk stack: +4..+0xff, 63 words (spills to RAM beyond)
.set SP_SXY,      0x100   # projected XY per record vertex (index x4), 42 words
.set SP_SZ,       0x1a8   # SZ per record vertex, 42 words
.set SP_COL,      0x250   # light per record vertex as 0x00cccccc, 42 words
.set SP_CAM,      0x300   # cam[3], vertex units
.set SP_FVBASE,   0x30c
.set SP_SZFAST,   0x310
.set SP_LASTTEX,  0x314   # draw_records: the last record's texture index
.set SP_PLANES,   0x318   # r_state.planes (the walk's plane table)
.set SP_NEXTREC,  0x31c
.set SP_NODES,    0x320   # counters
.set SP_RECORDS,  0x324
.set SP_VERTS,    0x328
.set SP_POLYS,    0x32c
.set SP_SKIPPED,  0x330
.set SP_TBLOCK,   0x334
.set SP_T0,       0x338
.set SP_FULL,     0x33c   # non-zero once the packet arena is full
.set SP_SAVE,     0x340   # 10 words: registers kept across the slow path + its start time
.set SP_PLA,      0x368   # slow path: polygon index list A (12 bytes)
.set SP_PLB,      0x374   # slow path: polygon index list B (12 bytes)
.set SP_SV,       0x380   # slow path: s0-s5 kept across it (6 words)
.set SP_RSTATE,   0x398   # r_render's r_state (s0 holds the plane table during the walk)
.set SP_VERTPOOL, 0x3c0   # r_state.vertpool (the map's vertex pool)
.set SP_SPILLS,   0x3c4   # walk: 64-word stack blocks spilled to r_walk_spill
.set SP_TEXBASE,  0x3c8   # qw_tex table
.set SP_TWIN,     0x3cc   # draw_records: the current texture's window command
                          # (emitted when its run of records ends)
.set SP_SLOWB,    0x3d0   # packet bytes the slow path wrote this frame
.set SP_CLUT16,   0x3d4   # current texture's clut << 16 (draw_records' v0;
.set SP_TPAGE16,  0x3d8   # tpage << 16, its v1): the slow path reads them
.set SP_SPRA,     0x3dc   # slow path: its own return address
.set SP_FREE3E0,  0x3e0   # (free)
.set SP_T2,       0x3e4   # r_state.warp_t2: 2 x the affine-error threshold
.set SP_FREE3E8,  0x3e8   # (free)
.set SP_K16,      0x3ec   # r_state.warp_k16, the record warp bound (free: 0x3f0 - 0x3fb)
.set SP_TSLOW,    0x3fc   # slow path: timer-2 ticks this frame (last word)

.set R_SLOW_C,    0       # 1: use the C reference (engine/r_slow.c)
.ifndef R_PROFILE           # renderer counters + stage timers (CMake option
.set R_PROFILE,   1         # Q1PSX_PROFILE passes --defsym R_PROFILE=0|1)
.endif
.set WALK_SHIFT,  8       # walk stack block = 1 << 8 bytes: the scratchpad
                          # part (63 entries from +4: an empty pop reads the
                          # word at +0), and the unit spilled to RAM
.set WALK_RAM,    1024    # RAM behind it: depth <= 63 x 5 = 315 >= QW_MAX_DEPTH

.set TIMER2,      0x1120  # root counter 2 value, offset from 0x1F800000's
                          # upper half (0x1F801120)

# ---- GPU packets --------------------------------------------------------------
.set CODE_GT4,    0x3c000000   # Gouraud textured quad, opaque, texture x colour
.set CODE_GT3,    0x34000000   # Gouraud textured triangle, opaque
.set BACKFACE_EPS, 164         # Quake's 0.01 unit, x4096 x4
