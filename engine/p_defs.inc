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
.set TR_PLANE_N,    32     # int16[3]
.set TR_ENT,        38     # int16: the brush model hit (clip_bmodels index + 1), 0 the world
.set TR_PLANE_D,    40
.set TR_SIZE,       44

# ---- clip_bmodel_t (16 B, engine/clip.h) / qw_bmodel (40 B, formats/qworld.h)
.set CB_BM,          0     # the qw_bmodel
.set CB_ORG,         4     # int32 origin[3], 1/8 units
.set CB_ABSMIN,     16     # int32[3]: origin + the model's bounds
.set CB_ABSMAX,     28
.set CB_SIZE,       40
.set BM_MINS,        0     # int32[3], 1/8 units, a unit spread
.set BM_MAXS,       12
.set BM_ROOT,       24     # hull 0: its render nodes' root (KUSEG pointer)
.set BM_HULL1,      28     # clipnode roots
.set BM_HULL2,      32

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

# ---- pmove_t (84 B) / pmove_cmd_t (24 B) --------------------------------------
.set PMT_SIZE,      84
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
.set PM_TOUCHED,  0x0A0     # the last brush model a slide hit: its trace ent (SV_Impact)
.set PS_TRB,      0x0A4     # trace_t for every pmove trace (44 B)
.set PS_SY,       0x0D0     # sin / cos of the yaw
.set PS_CY,       0x0D4
.set PS_WISH,     0x0D8     # wishdir[3]
.set PS_MOVE,     0x0E4     # this tick's displacement[3]
.set PS_FLATPOS,  0x0F0     # position / velocity before the flat slide
.set PS_FLATVEL,  0x0FC
.set PS_AFTER,    0x108     # position / velocity after it
.set PS_FOUTVEL,  0x114
.set PS_TO,       0x120     # end point of a vertical trace[3]
.set PS_SVEL,     0x12C     # slide_move: vel[3]
.set PS_PRIMAL,   0x138     #   primal velocity[3]
.set PS_CUR,      0x144     #   current position[3]
.set PS_REM,      0x150     #   full-tick displacement[3]
.set PS_DISP,     0x15C     #   this bump's displacement[3]
.set PS_END,      0x168     #   its end point[3]
.set PS_NEWVEL,   0x174     #   clipped velocity candidate[3]
.set PS_DIR,      0x180     #   crease direction[3]
.set PS_PLANES,   0x18C     #   planes hit: 5 x 8 B (int16 n[3] + pad)
.set PS_FWD3,     0x1B4     # water_move: the view's forward with its pitch[3]
.set PS_WV,       0x1C0     #   wishvel[3], x64
.set PS_PT,       0x1CC     # check_water's point[3]

# ---- scratchpad, clip_trace's brush models (after pmove's words) --------------
.set PS_HULL,     0x1D8     # the call's hull, start, end and trace_t
.set PS_WSTART,   0x1DC
.set PS_WEND,     0x1E0
.set PS_WTR,      0x1E4
.set PS_CORERA,   0x1E8     # ct_core's return address
.set PS_BMI,      0x1EC     # the clip_bmodels entry being tried, and the end
.set PS_BMEND,    0x1F0
.set PS_BMIDX,    0x250     # its index + 1 (the trace's ent)
.set PS_BMIN,     0x1F4     # the swept box[3], a unit wider (SV_MoveBounds)
.set PS_BMAX,     0x200
.set PS_BSTART,   0x20C     # the sweep in the model's space[3]
.set PS_BEND,     0x218
.set PS_BTRACE,   0x224     # its trace_t (44 B, to 0x250)
