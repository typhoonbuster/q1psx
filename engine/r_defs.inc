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
.set RS_ITEMS,        184
.set RS_LISTS,        188
.set RS_RGB,          192
.set RS_VIEWMODEL,    196
.set RS_OFY,          200
.set RS_VH2,          204
.set RS_NLIGHTS,      208   # dynamic lights this frame, and their table
.set RS_LIGHTS,       212   #   (engine/dlight.c rlight_t, 32 bytes each)
.set RS_MARK_R,       216   # mark_pvs_far's reach, vertex units (render.c)
.set RS_FRAGPOOL,     220   # the brush models' pieces' vertex pool (bmfrag.c)
.set DL_O,            0     # int32 origin[3], vertex units
.set DL_RADIUS,       12    # vertex units
.set DL_MINLIGHT,     16
.set DL_RMM,          20    # radius - minlight
.set DL_SIZE,         32
.set DLB_FIRST,       16    # dl_blk: word 0 the block's node, then an entry per light (dlight.c dlblk_t)
.set DLB_IX,          0     # the light's impact point on the block's plane, vertex units
.set DLB_IY,          4
.set DLB_IZ,          8
.set DLB_CUTOFF,      12    # rad - minlight (< 0: the light does not reach the plane)
.set DLB_MIN,         16
.set DLB_VALID,       36    # computed for this block
.set DLB_SIZE,        48
.set DL_SAVE_ENTRY,   52    # dlight.c: a saved record (the record, its vertex count, 44 bytes)
.set DL_SAVE_MAX,     470

# ---- the render list (engine/rlist.h): r_item_t (16 B), r_ent_t (80 B) -----
.set RI_NEXT,     0     # next item of the list + 1, 0 ends
.set RI_TYPE,     2     # 0 particle, 1 model, 0x80 a brush model's pieces (lb: < 0)
.set RI_COLOUR,   3
.set RI_X,        4     # particle: x | y << 16 (lwc2 VXY0), z at +8 (lh)
.set RI_Z,        8
.set RI_LEN,     10     # pieces: their records' bytes
.set RI_ENT,     12     # model: the r_ent_t; pieces: their first record
.set RE_ROT,      0     # GTE rotation, 5 words
.set RE_TR,      20     # 3 words
.set RE_POOL,    32
.set RE_RECS,    36
.set RE_RECS_END, 40
.set RE_TEX,     44
.set RE_DOTS,    48     # light table (162 bytes) or 0: flat
.set RE_LIGHT,   52
.set RE_FLAGS,   56     # 1 (RE_VIEW): no warp bound, the near clip at 5 units;
                        # 2 (RE_BMODEL): a brush model, its subtree walked (draw_bmodel)
.set RE_CAM,     60     # RE_BMODEL: the camera in the model's space, vertex units

# ---- qw_node (36 B) ----------------------------------------------------------
.set N_C,         0     # int16 c[3]: VXY0 at +0, VZ0 with lh at +4
.set N_VISFRAME,  6
.set N_H,         8     # int16 h[3]
.set N_PLANE3,   14     # plane index x 3: word offset in the plane table
.set N_DLSTAMP,  18     # the frame count when a dynamic light reaches the box
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
.set PL_N,        0     # qw_plane: int16 n[3] x4096
.set PL_TYPE,     6     # 0..2 axial (n = 4096 on that axis), 3 general
.set PL_DIST,     8     # x4096, 1/8 units
.set R_FACE_ID,   0
.set R_NVERTS,    2
.set R_NQUADS,    3
.set R_NTRIS,     4
.set R_FLAGS,     5     # QWF_SKY 1, QWF_LIQUID 2
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
.set SP_DMRA,     0x39c   # draw_model: its return address
.set SP_NEXTITEM, 0x3a0   # draw_ents: the next item of the list + 1
.set SP_STAMP,    0x3a4   # the frame count as a halfword: a model's records'
                          # face stamp (their face_id is 0: SP_FVBASE points here)
.set SP_OFY,      0x3a8   # the viewport's vertical centre (sp_proj)
.set SP_VH2,      0x3ac   # its height
.set SP_SZCLIP,   0x3b0   # SZ below which a polygon's corner sends it to the
                          # slow path's clip (96, r_slow.c SZ_CLIP; the view
                          # model: 124, its rear is right behind the eye)
.set SP_ZNEAR,    0x3b4   # the slow path's near plane, SZ units (48 = 8 world
                          # units; the view model: 120, Quake's 5 for it, its
                          # matrix's rows all x4: rlist.c rl_view_gun)
.set SP_HSHIFT,   0x3b8   # the GTE's H as a shift of 5x: 5 (H = 160; until
                          # §53 the view model's 3, H = 40)
.set SP_VERTPOOL, 0x3c0   # r_state.vertpool (the map's vertex pool)
.set SP_SPILLS,   0x3c4   # walk: 64-word stack blocks spilled to r_walk_spill
.set SP_TEXBASE,  0x3c8   # qw_tex table
.set SP_TWIN,     0x3cc   # draw_records: the current texture's window command
                          # (emitted when its run of records ends)
.set SP_SLOWB,    0x3d0   # packet bytes the slow path wrote this frame
.set SP_CLUT16,   0x3d4   # current texture's clut << 16 (draw_records' v0;
.set SP_TPAGE16,  0x3d8   # tpage << 16, its v1): the slow path reads them
.set SP_SPRA,     0x3dc   # slow path: its own return address
.set SP_NLIGHTS,  0x3e0   # dynamic lights this frame (r_state.nlights): the
                          # face blocks go through dr_lights first
.set SP_T2,       0x3e4   # r_state.warp_t2: 2 x the affine-error threshold
.set SP_T4,       0x3e8   # 2 x warp_t2: CAREFUL's 2 T2 zmin as one product
.set SP_K16,      0x3ec   # r_state.warp_k16, the record warp bound
.set SP_BMWALK,   0x3f0   # draw_bmodel: where dm_exit goes on after a block
                          # of the model's walk, 0 outside one
.set SP_FT4COL,   0x3f4   # the flat shading: a flat record's POLY_FT4 code |
.set SP_FT3COL,   0x3f8   # colour, its POLY_FT3's (RECLOOP opt bit 1, POLYS)
.set SP_TSLOW,    0x3fc   # slow path: timer-2 ticks this frame (last word)

.ifndef R_SLOW_C          # 1: the C version (engine/r_slow.c; CMake
.set R_SLOW_C,    0       # Q1PSX_ENGINE_C passes --defsym R_SLOW_C=1)
.endif
.set R_MODEL_ROOM, 24576  # draw_model: an alias model (a monster, an item, a
                          # flame, a gib) is left out when less packet room
                          # than this is left: the walk's farther records keep
                          # it (START's crucified zombies, 12.5 KB each, filled
                          # the arena and the far rooms went undrawn)
.ifndef R_YSLOPE          # the frustum's vertical slope x4: 4 (240 rows: y <= 3z/4)
.set R_YSLOPE,    4       # or 5 (the 192-row viewport over the status bar: y <= 3z/5)
.endif
.ifndef R_DLIGHT_C        # the dynamic lights' record hook: 0 the asm, 1 the C
.set R_DLIGHT_C,  0       # reference (engine/dlight.c dl_light_record), 2 the asm
.endif                    # checked record by record by the C (CMake Q1PSX_DLIGHT_C)
.ifndef R_PROFILE           # renderer counters + stage timers (CMake option
.set R_PROFILE,   1         # Q1PSX_PROFILE passes --defsym R_PROFILE=0|1)
.endif
.set WALK_SHIFT,  8       # walk stack block = 1 << 8 bytes: the scratchpad
                          # part (63 entries from +4: an empty pop reads the
                          # word at +0), and the unit spilled to RAM
.set WALK_RAM,    1024    # RAM behind it: depth <= 63 x 5 = 315 >= QW_MAX_DEPTH
.set QW_MAX_DEPTH, 256    # formats/qworld.h: the deepest node chain
.set QW_MAX_BM_DEPTH, 60  # formats/qworld.h: a brush model's subtree depth
.set BM_S3,       0       # bm_area (draw_bmodel): the world walk's stack pointer,
.set BM_CAM,      4       # its camera (3 words),
.set BM_STACK,    16      # the model walk's stack: the empty word, then
                          # QW_MAX_BM_DEPTH entries
.set BM_SIZE,     BM_STACK + 4 + 4 * QW_MAX_BM_DEPTH
.set DL_PRUNE,    64      # dynamic lights: a plane farther than radius + this
                          # (vertex units) has none of the light's vertices on
                          # its far side (dl_sqrt's and the roundings' error
                          # stay under it): that side is not descended
.set DSN_STACK,   4 * QW_MAX_DEPTH + 16   # r_render's dynamic-light descent:
                          # its stack of pending front children, on sp

.set TIMER2,      0x1120  # root counter 2 value, offset from 0x1F800000's
                          # upper half (0x1F801120)

# ---- GPU packets --------------------------------------------------------------
.set CODE_GT4,    0x3c000000   # Gouraud textured quad, opaque, texture x colour
.set CODE_GT3,    0x34000000   # Gouraud textured triangle, opaque
.set CODE_FT4,    0x2c000000   # flat textured quad, opaque, texture x colour
.set CODE_FT3,    0x24000000   # flat textured triangle, opaque
.set BACKFACE_EPS, 164         # Quake's 0.01 unit, x4096 x4
