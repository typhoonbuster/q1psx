# q1psx — physics asm definitions (engine/clip.s, engine/pmove.s). Mirrors
# engine/clip.h (trace_t), engine/world.h (world_t), formats/qworld.h
# (qw_plane / qw_clipnode); the headers static-assert the C side.

# ---- trace_t (44 B) -----------------------------------------------------------
.set TR_STARTSOLID,  0
.set TR_ALLSOLID,    4
.set TR_INOPEN,      8
.set TR_INWATER,    12
.set TR_FRACTION,   16
.set TR_ENDPOS,     20     # int32[3]
.set TR_PLANE_N,    32     # int16[3] + pad
.set TR_PLANE_D,    40
.set TR_SIZE,       44

# ---- world_t: the clip-hull fields --------------------------------------------
.set W_ROOT,         8     # the render BSP's root: hull 0, the point hull
.set W_PLANES,      76
.set W_CLIPNODES,   80
.set W_HULL1,       84
.set W_HULL2,       88

# ---- qw_node / qw_leaf (hull 0 walks the render BSP; engine/r_defs.inc) ------
.set N_PLANE3,      14     # plane index x 3 = word offset in the table
.set N_CHILD,       20     # child[2]: pointers (KUSEG), bit 0 = a leaf
.set L_CONTENTS,    26     # int8 Quake CONTENTS_*

# ---- qw_plane (12 B) / qw_clipnode (8 B) --------------------------------------
.set PL_N,           0     # int16 n[3] x4096: lwc2 VXY0 at +0, VZ0 at +4
.set PL_TYPE,        6     # 0..2 axial (n = 4096 on that axis), 3 general
.set PL_DIST,        8     # x4096, 1/8 units (pre-scaled by the cook)
.set CN_PLANE3,      0     # plane index x 3 = word offset
.set CN_CHILD,       2     # int16[2]: >= 0 clipnode, < 0 CONTENTS_*

.set QW_CONTENTS_EMPTY, -1
.set QW_CONTENTS_SOLID, -2
.set QW_CONTENTS_WATER, -3
.set QW_CONTENTS_SLIME, -4
.set QW_CONTENTS_LAVA,  -5

# ---- constants (engine/clip.c) ------------------------------------------------
.set CLIP_SUB,       8
.set DIST_EPS,    4096
.set F_ONE,    0x10000     # sweep fractions are 16-bit
.set Q_FIX_ONE,   4096

# ---- scratchpad, physics (base 0x1F800000) ------------------------------------
# The physics runs before the renderer in the frame, so it borrows the
# renderer's per-frame areas (walk stack / vertex caches, 0x000-0x2FF).
.set PS_START,   0x000     # trace start[3], sub-units
.set PS_DELTA,   0x00C     # end - start   (start + 12: indexable by axis)
.set PS_S4K,     0x018     # start x 4096  (axial planes: n.start)
.set PS_D4K,     0x024     # delta x 4096  (PS_S4K + 12)
.set PS_SNODE,   0x030     # the split being resolved: clipnode pointer,
.set PS_SSIDE,   0x034     #   near side,
.set PS_SF1,     0x038     #   its sub-sweep f1,
.set PS_SFM,     0x03C     #   split fraction,
.set PS_SF2,     0x040     #   sub-sweep f2

# ---- pmove_t (80 B) / pmove_cmd_t (24 B) --------------------------------------
.set PMT_SIZE,      80
.set CMD_FWD,        0
.set CMD_SIDE,       4
.set CMD_UP,         8
.set CMD_JUMP,      12     # held
.set CMD_WALK,      16
.set CMD_WATERJUMP, 20     # this tick: the game's CheckWaterJump found the ledge

# ---- scratchpad, pmove.s ------------------------------------------------------
# pmove_step copies the pmove_t here (same layout) and back at the end.
.set PM_POS,      0x050     # int32[3], 1/8 units
.set PM_VEL,      0x05C     # int32[3], units/s
.set PM_YAW,      0x068
.set PM_PITCH,    0x06C
.set PM_ONGROUND, 0x070
.set PM_NOCLIP,   0x074
.set PM_POSACC,   0x078     # int32[3]
.set PM_GRAVACC,  0x084
.set PM_WATERLEVEL, 0x088   # the water (pmove.h): 0..3
.set PM_WATERTYPE, 0x08C    # QW_CONTENTS_* at the feet
.set PM_WATERJUMP, 0x090    # ticks of FL_WATERJUMP left
.set PM_JUMPREL,  0x094     # FL_JUMPRELEASED
.set PM_MOVEDIR,  0x098     # int32[2]: the water jump's push
.set PS_TRB,      0x0A0     # trace_t for every pmove trace (44 B)
.set PS_SY,       0x0CC     # sin / cos of the yaw
.set PS_CY,       0x0D0
.set PS_WISH,     0x0D4     # wishdir[3]
.set PS_MOVE,     0x0E0     # this tick's displacement[3]
.set PS_FLATPOS,  0x0EC     # position / velocity before the flat slide
.set PS_FLATVEL,  0x0F8
.set PS_AFTER,    0x104     # position / velocity after it
.set PS_FOUTVEL,  0x110
.set PS_TO,       0x11C     # end point of a vertical trace[3]
.set PS_SVEL,     0x128     # slide_move: vel[3]
.set PS_PRIMAL,   0x134     #   primal velocity[3]
.set PS_CUR,      0x140     #   current position[3]
.set PS_REM,      0x14C     #   full-tick displacement[3]
.set PS_DISP,     0x158     #   this bump's displacement[3]
.set PS_END,      0x164     #   its end point[3]
.set PS_NEWVEL,   0x170     #   clipped velocity candidate[3]
.set PS_DIR,      0x17C     #   crease direction[3]
.set PS_PLANES,   0x188     #   planes hit: 5 x 8 B (int16 n[3] + pad)
.set PS_FWD3,     0x1B0     # water_move: the view's forward with its pitch[3]
.set PS_WV,       0x1BC     #   wishvel[3], x64
.set PS_PT,       0x1C8     # check_water's point[3]
