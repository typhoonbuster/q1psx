# q1psx — the brush models cut by the world's BSP, hand-written MIPS
# assembly. engine/bmfrag.c's cut_model_c is the specification: the same
# pieces, records, groups and vertices, byte for byte (BMF_C=2
# scripts/bench.sh cuts with both and compares every cut; BMF_C=1 builds
# the C alone). C ABI:
#
#   int bmf_cut(bmf_cache *c, int model, const int32_t org[3], uint32_t top);
#     -> 1 cut, 0 given up (no room, or too cut up: bmf_cut_overflow 1)
#
# The model's nodes in build_nodes' order; per node side, each record:
#   1. its sphere down the world's BSP from `top`: a node it lies wholly on
#      one side of sends every polygon there (each corner's distance has
#      the centre's sign); a record wholly in a solid leaf makes no piece;
#   2. each polygon unpacked into the next piece's slot (its corners in the
#      outline's order, where the model stands), with its box; down the BSP
#      from where the sphere stopped: an axial plane against the box (the
#      least and greatest coordinate on its axis), a general one on the GTE
#      (the corners as the rotation matrix's rows, a quad's fourth as the
#      light matrix's first row: one or two MVMVAs of the normal); in a
#      leaf it is a piece as it is (in a solid one, none);
#   3. a polygon on both sides of a plane takes the C's way from there:
#      its corners' distances, the split (a corner on the plane with its
#      neighbours' sides, a crossing from its edge's lower end), the back
#      piece pushed, the front one on, cut pieces taken corner by corner;
#   4. the pieces put per leaf into records (put_pieces, put_record) when
#      half the room is taken and at the record's end; a lone piece as it
#      was (most records of a big mover) the short way: its sizes known,
#      its box its own, its group put at once.
# Positions are within int16 (the map's, vertex units), so the sums need
# no truncation to 16 bits.
#
# Registers through a cut: s0 the cache, s1 the plane table, s2 the vertex
# pool, fp the scratchpad. cut_record: s3 the record, s4 its pieces so far,
# s5 0x7fffffff (a tagged leaf + s5 = the leaf as the C keeps it, a KSEG0
# pointer), s6 the polygon, s7 the next piece's slot. The scratchpad's
# 0x000-0x2ff is the renderer's per-frame part: the cut runs before
# r_render (or at a level's load), as the physics does. The renderer
# reloads the GTE's matrices in every r_render: borrowing the rotation and
# light matrices here is free.

	.set noreorder
	.set noat

	.include "gtereg.inc"
	.include "inline_s.inc"
	.include "r_defs.inc"

# ---- engine/bmfrag.c (static-asserted there) ----------------------------------
.set CA_NODES,      20     # bmf_cache
.set CA_RECS,       24
.set CA_RECS_LEN,   28
.set CA_RECS_CAP,   32
.set CA_GROUPS,     36
.set CA_NUM_GROUPS, 40
.set CA_GROUPS_CAP, 44
.set CA_POOL_FIRST, 48
.set CA_POOL_CAP,   52
.set CA_POOL_LEN,   56
.set BN_FIRST,       6     # bmf_node: uint16 first[2], count[2]
.set BN_COUNT,      10
.set BN_SIZE,       14
.set BG_LEAF,        0     # bmf_group
.set BG_OFS,         4
.set BG_LEN,         8
.set BG_SIZE,       12
.set PC_LEAF,        0     # bmf_piece
.set PC_N,           4     # a word: n | cut << 8 | quad << 16 | done << 24
.set PC_DONE,        7     #   (done: put_pieces' mark, 0 when the piece is made)
.set PC_V,           8     # bmf_v v[12]: x | y << 16, z | u << 16 | v << 24, light
.set PC_BOX,        56     # an uncut piece's box, in v[4]: int16 lo[3], hi[3]
.set PC_SIZE,      152
.set PD_AT,          0     # bmf_pend: the node, the corners
.set PD_N,           4
.set PD_V,           8
.set V_SIZE,        12     # bmf_v
.set BMF_CORNERS,   12
.set BMF_PIECES,    64
.set BMF_STACK,     24
.set QW_MAX_VERTS,  42
.set W_PLANES,      76     # world_t
.set W_VERTS,      100
.set W_BMODELS,    120
.set QB_ROOT,       24     # qw_bmodel (40 B)
.set LF_CONTENTS,   26     # qw_leaf: int8 CONTENTS_*
.set CONTENTS_SOLID, -2
.set R_UV,           4     # qw_quad / qw_tri: uint16 uv[] after the index bytes

# ---- the frame: the model's nodes still to cut (cut_model's stack) -----------
.set FR_RA,          0
.set FR_S0,          4
.set FR_STACK,      40
.set FR_SIZE,       FR_STACK + 4 * (QW_MAX_BM_DEPTH + 2)

# ---- scratchpad (0x1F800000) ----------------------------------------------------
.set BS_T,       0x000     # a cut piece's corners (12 x 12 B)
.set BS_U,       0x090     # the other one (a split writes its front into it)
.set BS_D,       0x120     # the corners' distances (12 words)
.set BS_SC4K,    0x150     # the record's sphere centre x 4096 (3 words)
.set BS_OFF,     0x15c     # where the model stands, vertex units (3 words)
.set BS_LIGHT,   0x168     # the record's light bytes
.set BS_QUADS,   0x16c     # its quads
.set BS_TRIS,    0x170     # its triangles
.set BS_NQ,      0x174     # its quads' count
.set BS_NPOLY,   0x178     # its polygons' count
.set BS_SIDEF,   0x17c     # the node side's first group (put_group merges after it)
.set BS_TOP,     0x180     # the model's top node
.set BS_END,     0x184     # the face block's end
.set BS_P,       0x188     # bmf_cut's state across cut_record: the record,
.set BS_DFSP,    0x18c     #   its stack pointer,
.set BS_ND,      0x190     #   the node,
.set BS_BN,      0x194     #   its bmf_node,
.set BS_SIDE,    0x198     #   the side
.set BS_RA1,     0x19c     # cut_record's return
.set BS_RA2,     0x1a0     # put_pieces'
.set BS_SPEND,   0x1a4     # the back pieces pending
.set BS_GTEP,    0x1a8     # the polygon's corners are in the GTE's matrices
.set BS_TOPR,    0x1ac     # where the record's polygons start
.set BS_PENT,    0x1b0     # the split's pending entry
.set BS_A0,      0x1b4     # put_record keeps put_pieces' a0, a1 here
.set BS_A1,      0x1b8
.set PR_NT,      0x1bc     # put_record: its triangles,
.set PR_NV,      0x1c0     #   corners,
.set PR_NQ,      0x1c4     #   quads,
.set PR_NIDX,    0x1c8     #   indices,
.set PR_IBYTES,  0x1cc     #   their bytes,
.set PR_CBYTES,  0x1d0     #   the light bytes,
.set PR_POLYS,   0x1d4     #   the first polygon's offset,
.set PR_SIZE,    0x1d8     #   the record's bytes,
.set PR_REC,     0x1dc     #   the record,
.set PR_BASE,    0x1e0     #   its first vertex's pool index,
.set PR_LSUM,    0x1e4     #   the light's sum,
.set PR_LO,      0x1e8     #   its sphere's centre (3 words)
.set BS_BATCH,   0x200     # put_pieces' batch: the pieces of one record (14 at most)

# -----------------------------------------------------------------------------
# PAIR lo, hi, a, b: lo = min(a, b), hi = max(a, b). MINW lo, x: lo =
# min(lo, x). MAXW hi, x: hi = max(hi, x). Int16 values: the differences do
# not overflow. Clobber v0, at.
.macro PAIR lo, hi, a, b
	subu    $v0, \b, \a
	sra     $at, $v0, 31
	and     $v0, $v0, $at                # b - a when b < a, else 0
	addu    \lo, \a, $v0
	subu    \hi, \b, $v0
.endm
.macro MINW lo, x
	subu    $v0, \x, \lo
	sra     $at, $v0, 31
	and     $v0, $v0, $at
	addu    \lo, \lo, $v0
.endm
.macro MAXW hi, x
	subu    $v0, \x, \hi
	sra     $at, $v0, 31
	and     $v0, $v0, $at
	subu    \hi, \x, $v0
.endm

# SQD r, p, x, y, z: r = the squared distance of corner p (3 halfwords)
# from (x, y, z), unsigned as the C's sum: the GTE's SQR of the differences
# (each within int16: a polygon's extent). Clobbers t6, t8.
.macro SQD r, p, x, y, z
	lh      \r, 0(\p)
	lh      $t6, 2(\p)
	lh      $t8, 4(\p)
	subu    \r, \r, \x
	subu    $t6, $t6, \y
	subu    $t8, $t8, \z
	mtc2    \r, C2_IR1
	mtc2    $t6, C2_IR2
	mtc2    $t8, C2_IR3
	nop
	nop
	SQR     0                            # MAC1-3 = their squares
	mfc2    \r, C2_MAC1
	mfc2    $t6, C2_MAC2
	mfc2    $t8, C2_MAC3
	addu    \r, \r, $t6
	addu    \r, \r, $t8
.endm

# MAXU m, r: m = max(m, r), unsigned. Clobbers v0, at.
.macro MAXU m, r
	sltu    $at, \m, \r
	subu    $v0, \r, \m
	subu    $at, $zero, $at
	and     $v0, $v0, $at
	addu    \m, \m, $v0
.endm

# ISQRT: t3 = floor(sqrt(t2)) bit by bit (t2 consumed), from the largest
# power of 4 not above it (the GTE's leading zeros). Clobbers t4-t6.
.macro ISQRT
	move    $t3, $zero
	beqz    $t2, 3f                      # (0: 0)
	lui     $t4, 0x4000                  # (delay) 1 << 30 for t2 >= 2^30
	srl     $t5, $t2, 30
	bnez    $t5, 1f
	nop
	mtc2    $t2, C2_LZCS
	nop
	nop
	mfc2    $t5, C2_LZCR                 # leading zeros
	li      $t6, 31
	subu    $t5, $t6, $t5                # the top bit,
	srl     $t5, $t5, 1
	sll     $t5, $t5, 1                  # made even
	li      $t4, 1
	sllv    $t4, $t4, $t5
1:	addu    $t5, $t3, $t4
	srl     $t3, $t3, 1
	sltu    $t6, $t2, $t5
	bnez    $t6, 2f                      # below res + bit: res >> 1
	nop
	subu    $t2, $t2, $t5                # else take it: res >> 1 + bit
	addu    $t3, $t3, $t4
2:	srl     $t4, $t4, 2
	bnez    $t4, 1b
	nop
3:
.endm

# SCAX r, k: r = sc on axis k from the piece t9's box: (lo + hi) / 2,
# truncated toward zero as C divides. Clobbers v0.
.macro SCAX r, k
	lh      \r, PC_BOX + 2 * \k($t9)
	lh      $v0, PC_BOX + 6 + 2 * \k($t9)
	nop
	addu    \r, \r, $v0
	srl     $v0, \r, 31
	addu    \r, \r, $v0
	sra     \r, \r, 1
.endm

# COPYV d, s: a corner (3 words). Clobbers v0.
.macro COPYV d, s
	lw      $v0, 0(\s)
	nop
	sw      $v0, 0(\d)
	lw      $v0, 4(\s)
	nop
	sw      $v0, 4(\d)
	lw      $v0, 8(\s)
	nop
	sw      $v0, 8(\d)
.endm

# -----------------------------------------------------------------------------
	.section .text.bmf_cut, "ax", @progbits
	.global bmf_cut
	.type   bmf_cut, @function
bmf_cut:
	addiu   $sp, $sp, -FR_SIZE
	sw      $ra, FR_RA($sp)
	sw      $s0, FR_S0+0($sp)
	sw      $s1, FR_S0+4($sp)
	sw      $s2, FR_S0+8($sp)
	sw      $s3, FR_S0+12($sp)
	sw      $s4, FR_S0+16($sp)
	sw      $s5, FR_S0+20($sp)
	sw      $s6, FR_S0+24($sp)
	sw      $s7, FR_S0+28($sp)
	sw      $fp, FR_S0+32($sp)
	lui     $t0, %hi(world)
	addiu   $t0, $t0, %lo(world)
	move    $s0, $a0
	lw      $s1, W_PLANES($t0)
	lw      $s2, W_VERTS($t0)
	lw      $t1, W_BMODELS($t0)
	lui     $fp, 0x1f80
	lw      $t2, 0($a2)                  # off[k] = org[k] x 4 / 8: 1/8 units to
	lw      $t3, 4($a2)                  # vertex units, truncated toward zero
	lw      $t4, 8($a2)                  # as C divides
	sll     $t2, $t2, 2
	sll     $t3, $t3, 2
	sll     $t4, $t4, 2
	sra     $t5, $t2, 31
	sra     $t6, $t3, 31
	sra     $t7, $t4, 31
	andi    $t5, $t5, 7
	andi    $t6, $t6, 7
	andi    $t7, $t7, 7
	addu    $t2, $t2, $t5
	addu    $t3, $t3, $t6
	addu    $t4, $t4, $t7
	sra     $t2, $t2, 3
	sra     $t3, $t3, 3
	sra     $t4, $t4, 3
	sw      $t2, BS_OFF+0($fp)
	sw      $t3, BS_OFF+4($fp)
	sw      $t4, BS_OFF+8($fp)
	sw      $zero, CA_RECS_LEN($s0)
	sw      $zero, CA_NUM_GROUPS($s0)
	sw      $zero, CA_POOL_LEN($s0)
	lui     $t6, %hi(bmf_cut_pieces)
	sw      $zero, %lo(bmf_cut_pieces)($t6)
	sw      $a3, BS_TOP($fp)
	sll     $t5, $a1, 3                  # world.bmodels[model] (40 B)
	sll     $t6, $a1, 5
	addu    $t5, $t5, $t6
	addu    $t5, $t1, $t5
	lw      $t7, QB_ROOT($t5)
	addiu   $s3, $sp, FR_STACK           # the nodes still to cut
	andi    $t0, $t7, 1
	bnez    $t0, bc_ok                   # a leaf: nothing to cut
	lw      $s5, CA_NODES($s0)           # (delay) its bmf_node: the first
	sw      $t7, 0($s3)
	addiu   $s3, $s3, 4
bc_node:
	addiu   $s3, $s3, -4
	lw      $s4, 0($s3)                  # the next node (preorder, as build_nodes)
	move    $s6, $zero                   # side 0, then 1
bc_side:
	lw      $t0, CA_NUM_GROUPS($s0)
	sll     $t1, $s6, 1
	addu    $t1, $s5, $t1
	sh      $t0, BN_FIRST($t1)           # the side's first group
	sw      $t0, BS_SIDEF($fp)
	sll     $t2, $s6, 2
	addu    $t2, $s4, $t2
	lw      $s7, N_FACES($t2)            # its face block
	lw      $t3, N_FACES+4($s4)          # its end: the back block's start,
	beqz    $s6, 1f
	sw      $s3, BS_DFSP($fp)            # (delay) the walk's state (cut_record
	lw      $t3, N_SIZE+N_FACES($s4)     # or the next node's front start
	nop
1:	sw      $t3, BS_END($fp)
	sw      $s4, BS_ND($fp)              #  keeps s0-s2, fp)
	sw      $s5, BS_BN($fp)
	sw      $s6, BS_SIDE($fp)
bc_rec:
	lw      $t3, BS_END($fp)
	nop
	sltu    $t4, $s7, $t3
	beqz    $t4, bc_side_end
	nop
	lhu     $t5, R_SIZE($s7)
	nop
	beqz    $t5, bc_side_end             # (a zero size ends the block)
	sw      $s7, BS_P($fp)               # (delay)
	jal     cut_record
	move    $a0, $s7                     # (delay) the record
	lw      $s7, BS_P($fp)
	beqz    $v0, bc_fail
	nop
	lhu     $t5, R_SIZE($s7)
	b       bc_rec
	addu    $s7, $s7, $t5                # (delay) the next record
bc_side_end:
	lw      $s3, BS_DFSP($fp)
	lw      $s4, BS_ND($fp)
	lw      $s5, BS_BN($fp)
	lw      $s6, BS_SIDE($fp)
	lw      $t0, CA_NUM_GROUPS($s0)
	sll     $t1, $s6, 1
	addu    $t1, $s5, $t1
	lhu     $t2, BN_FIRST($t1)
	nop
	subu    $t0, $t0, $t2
	sh      $t0, BN_COUNT($t1)           # the side's groups
	beqz    $s6, bc_side
	li      $s6, 1                       # (delay)
	lw      $t0, N_CHILD+4($s4)          # the children, the back one pushed first
	lw      $t1, N_CHILD+0($s4)          # (the front subtree comes first)
	addiu   $s5, $s5, BN_SIZE            # the next bmf_node
	andi    $t2, $t0, 1
	bnez    $t2, 2f
	addiu   $t3, $sp, FR_STACK + 4 * (QW_MAX_BM_DEPTH + 2)   # (delay) the stack's end
	beq     $s3, $t3, bc_fail            # (full: given up, as the C)
	nop
	sw      $t0, 0($s3)
	addiu   $s3, $s3, 4
2:	andi    $t2, $t1, 1
	bnez    $t2, 3f
	addiu   $t3, $sp, FR_STACK + 4 * (QW_MAX_BM_DEPTH + 2)
	beq     $s3, $t3, bc_fail
	nop
	sw      $t1, 0($s3)
	addiu   $s3, $s3, 4
3:	addiu   $t3, $sp, FR_STACK
	bne     $s3, $t3, bc_node
	nop
bc_ok:
	b       bc_ret
	li      $v0, 1
bc_fail:
	move    $v0, $zero
bc_ret:
	lw      $ra, FR_RA($sp)
	lw      $s0, FR_S0+0($sp)
	lw      $s1, FR_S0+4($sp)
	lw      $s2, FR_S0+8($sp)
	lw      $s3, FR_S0+12($sp)
	lw      $s4, FR_S0+16($sp)
	lw      $s5, FR_S0+20($sp)
	lw      $s6, FR_S0+24($sp)
	lw      $s7, FR_S0+28($sp)
	lw      $fp, FR_S0+32($sp)
	jr      $ra
	addiu   $sp, $sp, FR_SIZE

# -----------------------------------------------------------------------------
# cut_record(a0 = the record): v0 = 1, 0 given up. Keeps s0-s2, fp.
cut_record:
	sw      $ra, BS_RA1($fp)
	move    $s3, $a0
	lbu     $t0, R_NVERTS($s3)
	lbu     $t1, R_NQUADS($s3)
	lbu     $t2, R_NTRIS($s3)
	lhu     $t3, R_POLYS($s3)
	addiu   $t5, $t0, 3
	srl     $t5, $t5, 2
	sll     $t5, $t5, 2                  # (nv + 3) & ~3: the light bytes
	addu    $t6, $s3, $t3                # the quads
	subu    $t7, $t6, $t5                # the light bytes, before them
	sll     $t8, $t1, 1
	addu    $t8, $t8, $t1
	sll     $t8, $t8, 2
	addu    $t8, $t6, $t8                # the triangles
	addu    $t9, $t1, $t2
	sw      $t7, BS_LIGHT($fp)
	sw      $t6, BS_QUADS($fp)
	sw      $t8, BS_TRIS($fp)
	sw      $t1, BS_NQ($fp)
	sw      $t9, BS_NPOLY($fp)
	sw      $zero, BS_SPEND($fp)
	lui     $t0, %hi(bmf_cut_overflow)
	sw      $zero, %lo(bmf_cut_overflow)($t0)
	move    $s4, $zero                   # no piece yet
	lui     $s5, 0x7fff
	ori     $s5, $s5, 0xffff
	# the sphere (sc + off, its radius x 4097 the bound) from the top node
	lh      $t0, R_SC+0($s3)
	lh      $t1, R_SC+2($s3)
	lh      $t2, R_SC+4($s3)
	lw      $t3, BS_OFF+0($fp)
	lw      $t4, BS_OFF+4($fp)
	lw      $t5, BS_OFF+8($fp)
	lhu     $t6, R_SR($s3)
	addu    $t0, $t0, $t3
	addu    $t1, $t1, $t4
	addu    $t2, $t2, $t5
	andi    $t7, $t0, 0xffff             # the GTE's first row = sc: a general
	sll     $t8, $t1, 16                 # plane's n . sc in MAC1 (R21, the
	or      $t7, $t7, $t8                # upper half, not used)
	ctc2    $t7, C2_R11R12
	ctc2    $t2, C2_R13R21
	sll     $t0, $t0, 12                 # an axial plane's: sc x 4096 by the axis
	sll     $t1, $t1, 12
	sll     $t2, $t2, 12
	sw      $t0, BS_SC4K+0($fp)
	sw      $t1, BS_SC4K+4($fp)
	sw      $t2, BS_SC4K+8($fp)
	lw      $a0, BS_TOP($fp)
	sll     $a1, $t6, 12
	addu    $a1, $a1, $t6                # the bound
	subu    $a2, $zero, $a1
cr_sph:
	andi    $t0, $a0, 1
	bnez    $t0, cr_sph_leaf
	move    $t9, $a0                     # (delay) the node
	lhu     $t1, N_PLANE3($t9)
	lw      $a0, N_CHILD+0($t9)          # (the front child, by default)
	lw      $t8, N_CHILD+4($t9)
	sll     $t1, $t1, 2
	addu    $t1, $s1, $t1                # the plane
	lbu     $t2, PL_TYPE($t1)
	lw      $t3, PL_DIST($t1)
	sltiu   $t4, $t2, 3
	beqz    $t4, cr_sph_gen
	sll     $t2, $t2, 2                  # (delay)
	addu    $t2, $fp, $t2
	lw      $t5, BS_SC4K($t2)
	sra     $t3, $t3, 1                  # dist, vertex units x 4096
	subu    $t5, $t5, $t3                # d = sc . n - dist
cr_sph_d:
	slt     $t6, $a1, $t5                # d > bound: every corner in front
	bnez    $t6, cr_sph
	slt     $t7, $t5, $a2                # (delay) d < -bound: every one behind
	beqz    $t7, cr_polys                # neither: the polygons start here
	move    $a0, $t9                     # (delay)
	b       cr_sph
	move    $a0, $t8                     # (delay) the back child
cr_sph_gen:
	lwc2    C2_VXY0, PL_N+0($t1)         # V0 = n (VZ0 from a sign-extended
	lh      $t4, PL_N+4($t1)             # halfword: PCSX-Redux keeps the upper
	sra     $t3, $t3, 1                  # half of a word there)
	mtc2    $t4, C2_VZ0
	nop
	nop
	MVMVA   0, 0, 0, 3, 0                # MAC1 = sc . n
	mfc2    $t5, C2_MAC1
	nop
	b       cr_sph_d
	subu    $t5, $t5, $t3                # (delay)
cr_sph_leaf:
	lb      $t0, LF_CONTENTS - 1($a0)    # (the tag bit)
	li      $t1, CONTENTS_SOLID
	beq     $t0, $t1, cr_ret1            # all of it in a solid leaf: no piece
	nop
cr_polys:
	sw      $a0, BS_TOPR($fp)            # where the polygons start
	move    $s6, $zero                   # the first polygon
	lui     $s7, %hi(bmf_pieces)
	addiu   $s7, $s7, %lo(bmf_pieces)    # its slot
cr_poly:
	lw      $t0, BS_NPOLY($fp)
	slti    $t1, $s4, BMF_PIECES / 2 + 1
	slt     $t2, $s6, $t0
	beqz    $t2, cr_end                  # every polygon cut
	nop
	bnez    $t1, cr_unpack
	lw      $t0, BS_NQ($fp)              # (delay)
	jal     put_pieces                   # half the room taken: the pieces put
	nop                                  # (s4 0, s7 the first slot after)
	beqz    $v0, cr_fail
	lw      $t0, BS_NQ($fp)              # (delay)
cr_unpack:
	lw      $at, BS_LIGHT($fp)
	lw      $a1, BS_OFF+0($fp)
	lw      $a2, BS_OFF+4($fp)
	lw      $a3, BS_OFF+8($fp)
	slt     $t8, $s6, $t0                # a quad?
	bnez    $t8, 1f
	move    $t2, $s6                     # (delay) its index among the quads,
	lw      $t9, BS_TRIS($fp)            # or among the triangles
	b       2f
	subu    $t2, $s6, $t0                # (delay)
1:	lw      $t9, BS_QUADS($fp)
2:	sll     $t3, $t2, 1
	addu    $t3, $t3, $t2
	sll     $t3, $t3, 2
	addu    $t9, $t9, $t3                # the polygon
	addiu   $v1, $s3, R_VERTS            # the record's vertex indices
	lui     $v0, 0x0002                  # the PS1 corners in the outline's order,
	beqz    $t8, 5f                      # a byte each, the first lowest: a
	ori     $v0, $v0, 0x0100             # (delay) triangle's 0 1 2, a quad's
	lui     $v0, 0x0203                  # 0 1 3 2 (none left: the end; the loops
	                                     # here unrolled were 3 % faster and 530
	                                     # bytes more than the stack's room allows)
	ori     $v0, $v0, 0x0100
5:	addiu   $a0, $s7, PC_V               # the corner's slot
6:	andi    $t0, $v0, 0xff               # a corner where the model stands:
	srl     $v0, $v0, 8
	addu    $t1, $t9, $t0
	lbu     $t2, 0($t1)                  # its vertex (x 4),
	sll     $t0, $t0, 1
	addu    $t1, $t9, $t0
	lhu     $t1, R_UV($t1)               # its texels,
	srl     $t3, $t2, 1
	addu    $t3, $v1, $t3
	lhu     $t3, 0($t3)                  # the vertex's pool index,
	srl     $t2, $t2, 2
	addu    $t2, $at, $t2
	lbu     $t7, 0($t2)                  # its light
	sll     $t3, $t3, 3
	addu    $t3, $s2, $t3
	lh      $t4, 0($t3)
	lh      $t5, 2($t3)
	lh      $t6, 4($t3)
	sll     $t1, $t1, 16
	addu    $t4, $t4, $a1
	addu    $t5, $t5, $a2
	addu    $t6, $t6, $a3
	andi    $t4, $t4, 0xffff
	sll     $t5, $t5, 16
	or      $t4, $t4, $t5                # x | y << 16
	andi    $t6, $t6, 0xffff
	or      $t6, $t6, $t1                # z | u << 16 | v << 24
	sw      $t4, 0($a0)
	sw      $t6, 4($a0)
	sw      $t7, 8($a0)
	bnez    $v0, 6b
	addiu   $a0, $a0, V_SIZE             # (delay)
	bnez    $t8, 7f                      # a triangle's box as a quad's: corner 2
	nop                                  # again as the fourth, never drawn
	lw      $t0, PC_V + 24($s7)
	lw      $t1, PC_V + 28($s7)
	nop
	sw      $t0, PC_V + 36($s7)
	sw      $t1, PC_V + 40($s7)
7:	move    $t6, $s7                     # the box, axis by axis
	li      $t7, 3
8:	lh      $t0, PC_V + 0($t6)
	lh      $t1, PC_V + 12($t6)
	lh      $t2, PC_V + 24($t6)
	lh      $t3, PC_V + 36($t6)
	PAIR    $t4, $t5, $t0, $t1
	PAIR    $t0, $t1, $t2, $t3
	MINW    $t4, $t0
	MAXW    $t5, $t1
	sh      $t4, PC_BOX($t6)
	addiu   $t7, $t7, -1
	sh      $t5, PC_BOX + 6($t6)
	bnez    $t7, 8b
	addiu   $t6, $t6, 2                  # (delay)
	sll     $a3, $t8, 16                 # quad << 16 | n (cut 0)
	addiu   $a3, $a3, 3
	addu    $a3, $a3, $t8
cr_desc:
	lw      $a0, BS_TOPR($fp)
	andi    $a2, $a3, 0xff               # n
	sw      $zero, BS_GTEP($fp)          # its corners not in the GTE yet

	# ---- the polygon as it is: its box, or its corners on the GTE ----------
fd_node:
	andi    $t0, $a0, 1
	bnez    $t0, fd_leaf
	move    $t9, $a0                     # (delay) the node
	lhu     $t1, N_PLANE3($t9)
	lw      $a0, N_CHILD+0($t9)          # (the front child, by default)
	lw      $a1, N_CHILD+4($t9)          # (the back one)
	sll     $t1, $t1, 2
	addu    $t1, $s1, $t1                # the plane
	lbu     $t2, PL_TYPE($t1)
	lw      $t3, PL_DIST($t1)
	sltiu   $t4, $t2, 3
	beqz    $t4, fd_gen
	sll     $t2, $t2, 1                  # (delay) the axis x 2
	addu    $t2, $s7, $t2
	lh      $t5, PC_BOX+0($t2)           # the box's least coordinate on the axis
	lh      $t6, PC_BOX+6($t2)           # and its greatest
	sra     $t3, $t3, 1                  # dist, vertex units x 4096
	sll     $t5, $t5, 12
	sll     $t6, $t6, 12
	slt     $t7, $t5, $t3                # a corner behind (d < 0)
	slt     $t8, $t3, $t6                # a corner in front (d > 0)
fd_side:
	bnez    $t7, 1f
	nop
	bnez    $t8, fd_node                 # all >= 0, one > 0: the front
	nop
	b       fd_node                      # all on the plane: the back (Quake's
	move    $a0, $a1                     # d <= 0)
1:	beqz    $t8, fd_node                 # all <= 0, one < 0: the back
	move    $a0, $a1                     # (delay)
	addiu   $a1, $s7, PC_V               # both sides: cut, the C's way (the
	b       gs_node                      # corners' distances again)
	move    $a0, $t9                     # (delay)
fd_gen:                                   # V0 = n, the corners in the matrices
	lw      $t4, BS_GTEP($fp)
	lh      $t5, PL_N+4($t1)             # (VZ0 sign-extended)
	lwc2    C2_VXY0, PL_N+0($t1)
	bnez    $t4, 2f
	sra     $t3, $t3, 1                  # (delay) dist, vertex units x 4096
	lw      $t4, PC_V + 0($s7)           # R1 = corner 0, R2 = corner 1,
	lh      $t6, PC_V + 4($s7)           # R3 = corner 2 (R13R21 = z0 | x1 << 16,
	lw      $t7, PC_V + 12($s7)          # R22R23 = y1 | z1 << 16; R33 alone,
	lh      $t8, PC_V + 16($s7)          # sign-extended)
	ctc2    $t4, C2_R11R12
	andi    $t6, $t6, 0xffff
	sll     $v0, $t7, 16
	or      $t6, $t6, $v0
	ctc2    $t6, C2_R13R21
	srl     $t7, $t7, 16
	sll     $t8, $t8, 16
	or      $t7, $t7, $t8
	ctc2    $t7, C2_R22R23
	lw      $t4, PC_V + 24($s7)
	lh      $t6, PC_V + 28($s7)
	ctc2    $t4, C2_R31R32
	ctc2    $t6, C2_R33
	lw      $t4, PC_V + 36($s7)          # a quad's fourth: the light matrix's
	lh      $t6, PC_V + 40($s7)          # first row (L13 sign-extended: L21,
	ctc2    $t4, C2_L11L12               # its upper half, not used)
	ctc2    $t6, C2_L13L21
	li      $t4, 1
	sw      $t4, BS_GTEP($fp)
2:	mtc2    $t5, C2_VZ0
	srl     $t4, $a3, 16                 # (a quad?)
	nop
	MVMVA   0, 0, 0, 3, 0                # MAC1-3 = n . corners 0-2
	mfc2    $t5, C2_MAC1
	mfc2    $t6, C2_MAC2
	mfc2    $t7, C2_MAC3
	subu    $t5, $t5, $t3                # their d
	subu    $t6, $t6, $t3
	subu    $t7, $t7, $t3
	or      $t8, $t5, $t6                # d's or'ed: the sign, a corner behind
	or      $t8, $t8, $t7
	subu    $v0, $zero, $t5              # -d's or'ed: the sign, one in front
	subu    $at, $zero, $t6
	or      $v0, $v0, $at
	subu    $at, $zero, $t7
	beqz    $t4, 3f
	or      $v0, $v0, $at                # (delay)
	MVMVA   0, 1, 0, 3, 0                # a quad: MAC1 = n . corner 3
	mfc2    $t5, C2_MAC1
	nop
	subu    $t5, $t5, $t3
	or      $t8, $t8, $t5
	subu    $t5, $zero, $t5
	or      $v0, $v0, $t5
3:	srl     $t7, $t8, 31                 # one behind
	b       fd_side
	srl     $t8, $v0, 31                 # (delay) one in front
fd_leaf:
	lb      $t0, LF_CONTENTS - 1($a0)
	li      $t1, CONTENTS_SOLID
	beq     $t0, $t1, cr_next            # in a solid leaf: no piece (the slot
	addu    $t2, $a0, $s5                # (delay) the leaf     taken again)
	sw      $t2, PC_LEAF($s7)            # a piece of it, as it was
	sw      $a3, PC_N($s7)
	addiu   $s4, $s4, 1
	addiu   $s7, $s7, PC_SIZE
cr_next:
	b       cr_poly
	addiu   $s6, $s6, 1
cr_end:
	jal     put_pieces                   # the record's last pieces
	nop
	lw      $ra, BS_RA1($fp)
	nop
	jr      $ra
	nop
cr_overflow:
	li      $t0, 1                       # too cut up (bmf_cut_overflow): given up
	lui     $t1, %hi(bmf_cut_overflow)
	sw      $t0, %lo(bmf_cut_overflow)($t1)
cr_fail:
	lw      $ra, BS_RA1($fp)
	move    $v0, $zero
	jr      $ra
	nop
cr_ret1:
	lw      $ra, BS_RA1($fp)
	li      $v0, 1
	jr      $ra
	nop

	# ---- a cut piece: a1 its corners (a2 of them), a0 the node, corner by
	# corner (the C's cut_poly) -----------------------------------------------
gs_node:
	andi    $t0, $a0, 1
	bnez    $t0, gs_leaf
	move    $t9, $a0                     # (delay) the node
	lhu     $t1, N_PLANE3($t9)
	lw      $a0, N_CHILD+0($t9)          # (the front child)
	lw      $v1, N_CHILD+4($t9)          # (the back one)
	addiu   $t6, $fp, BS_D               # the distances
	sll     $t1, $t1, 2
	addu    $t1, $s1, $t1
	lbu     $t2, PL_TYPE($t1)
	lw      $t3, PL_DIST($t1)
	move    $t7, $zero                   # d's or'ed (the sign: one behind)
	sltiu   $t4, $t2, 3
	move    $t8, $zero                   # -d's or'ed (the sign: one in front)
	beqz    $t4, gs_gen
	sra     $t3, $t3, 1                  # (delay) dist, vertex units x 4096
	sll     $t2, $t2, 1
	addu    $t4, $a1, $t2                # the corners' coordinate on the axis
	move    $t5, $a2
1:	lh      $v0, 0($t4)
	addiu   $t4, $t4, V_SIZE
	sll     $v0, $v0, 12
	subu    $v0, $v0, $t3                # d
	sw      $v0, 0($t6)
	or      $t7, $t7, $v0
	subu    $v0, $zero, $v0
	or      $t8, $t8, $v0
	addiu   $t5, $t5, -1
	bnez    $t5, 1b
	addiu   $t6, $t6, 4
	b       gs_side
	nop
gs_gen:                                   # the normal as R1 (the plane's words:
	lw      $t4, PL_N+0($t1)             # R13 = n2, R21 its type, not used)
	lw      $t5, PL_N+4($t1)
	ctc2    $t4, C2_R11R12
	ctc2    $t5, C2_R13R21
	sw      $zero, BS_GTEP($fp)          # (the polygon's corners gone)
	move    $t4, $a1
	move    $t5, $a2
2:	lwc2    C2_VXY0, 0($t4)              # V0 = the corner (VZ0 sign-extended)
	lh      $v0, 4($t4)
	addiu   $t4, $t4, V_SIZE
	mtc2    $v0, C2_VZ0
	addiu   $t5, $t5, -1
	nop
	MVMVA   0, 0, 0, 3, 0                # MAC1 = n . corner
	mfc2    $v0, C2_MAC1
	nop
	subu    $v0, $v0, $t3
	sw      $v0, 0($t6)
	or      $t7, $t7, $v0
	subu    $v0, $zero, $v0
	or      $t8, $t8, $v0
	bnez    $t5, 2b
	addiu   $t6, $t6, 4
gs_side:
	srl     $t7, $t7, 31                 # one behind
	bnez    $t7, 3f
	srl     $t8, $t8, 31                 # (delay) one in front
	bnez    $t8, gs_node                 # all >= 0, one > 0: the front
	nop
	b       gs_node                      # all on the plane: the back
	move    $a0, $v1
3:	beqz    $t8, gs_node                 # all <= 0: the back
	move    $a0, $v1                     # (delay)

	# ---- both sides: the front piece into the other buffer, the back one
	# pushed (a corner on the plane with its neighbours' sides) -------------
	lw      $v1, BS_SPEND($fp)
	addiu   $t1, $fp, BS_T               # the front piece: the buffer a1 is not
	bne     $a1, $t1, 4f
	slti    $t0, $v1, BMF_STACK          # (delay) room for the back one?
	addiu   $t1, $fp, BS_U
4:	beqz    $t0, cr_overflow
	sll     $t2, $v1, 3                  # (delay) its entry: v1 x 152
	sll     $t3, $v1, 4
	sll     $t4, $v1, 7
	addu    $t2, $t2, $t3
	addu    $t2, $t2, $t4
	lui     $t3, %hi(bmf_pending)
	addiu   $t3, $t3, %lo(bmf_pending)
	addu    $t2, $t3, $t2
	sw      $t2, BS_PENT($fp)
	addiu   $t2, $t2, PD_V               # the back piece's corners
	move    $t0, $zero                   # corner i
	move    $t3, $zero                   # the front's
	move    $t4, $zero                   # the back's
gs_corner:
	slti    $v0, $t3, BMF_CORNERS - 1    # 11 corners on a side: too cut up
	beqz    $v0, cr_overflow
	slti    $v0, $t4, BMF_CORNERS - 1    # (delay)
	beqz    $v0, cr_overflow
	addiu   $t5, $t0, -1                 # (delay) h = i - 1, or the last
	bgez    $t5, 5f
	addiu   $t6, $t0, 1                  # (delay) j = i + 1, or the first
	addiu   $t5, $a2, -1
5:	bne     $t6, $a2, 6f
	sll     $t5, $t5, 2                  # (delay)
	move    $t6, $zero
6:	sll     $t6, $t6, 2
	sll     $t7, $t0, 2
	addu    $t5, $fp, $t5
	addu    $t6, $fp, $t6
	addu    $t7, $fp, $t7
	lw      $t5, BS_D($t5)               # d[h]
	lw      $t6, BS_D($t6)               # d[j]
	lw      $t7, BS_D($t7)               # d[i]
	sll     $t8, $t0, 1
	addu    $t8, $t8, $t0
	sll     $t8, $t8, 2
	bgtz    $t7, 7f                      # the front: d[i] > 0, or 0 with a
	addu    $t8, $a1, $t8                # (delay) corner i        neighbour > 0
	bnez    $t7, 8f
	nop
	bgtz    $t5, 7f
	nop
	blez    $t6, 8f
	nop
7:	sll     $a0, $t3, 1
	addu    $a0, $a0, $t3
	sll     $a0, $a0, 2
	addu    $a0, $t1, $a0
	COPYV   $a0, $t8
	addiu   $t3, $t3, 1
8:	bltz    $t7, 9f                      # the back: d[i] < 0, or 0 with a
	nop                                  # neighbour < 0
	bnez    $t7, 10f
	nop
	bltz    $t5, 9f
	nop
	bgez    $t6, 10f
	nop
9:	sll     $a0, $t4, 1
	addu    $a0, $a0, $t4
	sll     $a0, $a0, 2
	addu    $a0, $t2, $a0
	COPYV   $a0, $t8
	addiu   $t4, $t4, 1
10:	beqz    $t7, 11f                     # an edge crossing: d[i] and d[j] of
	xor     $v0, $t7, $t6                # (delay)        opposite signs
	beqz    $t6, 11f
	nop
	bgez    $v0, 11f
	nop
	sll     $a0, $t3, 1                  # into the front's next corner
	addu    $a0, $a0, $t3
	sll     $a0, $a0, 2
	addu    $a0, $t1, $a0
	move    $t5, $t8                     # a = corner i (da = d[i] in t7)
	move    $t8, $t6                     # db = d[j]
	addiu   $v0, $t0, 1                  # corner j
	bne     $v0, $a2, 12f
	nop
	move    $v0, $zero
12:	sll     $at, $v0, 1
	addu    $at, $at, $v0
	sll     $at, $at, 2
	jal     cross
	addu    $t6, $a1, $at                # (delay) b = corner j
	sll     $t5, $t4, 1                  # the crossing to the back too
	addu    $t5, $t5, $t4
	sll     $t5, $t5, 2
	addu    $t5, $t2, $t5
	COPYV   $t5, $a0
	addiu   $t3, $t3, 1
	addiu   $t4, $t4, 1
11:	addiu   $t0, $t0, 1
	bne     $t0, $a2, gs_corner
	nop
	lw      $t5, BS_PENT($fp)            # the back piece: the far child, later
	lw      $t6, N_CHILD+4($t9)
	sw      $t4, PD_N($t5)
	sw      $t6, PD_AT($t5)
	addiu   $v1, $v1, 1
	sw      $v1, BS_SPEND($fp)
	lw      $a0, N_CHILD+0($t9)          # the front one goes on
	move    $a1, $t1
	b       gs_node
	move    $a2, $t3                     # (delay)

gs_leaf:
	lb      $t0, LF_CONTENTS - 1($a0)
	li      $t1, CONTENTS_SOLID
	beq     $t0, $t1, gs_pop             # in a solid leaf: nothing
	slti    $t2, $s4, BMF_PIECES         # (delay) room for the piece?
	beqz    $t2, cr_overflow
	addu    $t3, $a0, $s5                # (delay) the leaf
	sw      $t3, PC_LEAF($s7)
	srl     $t4, $a3, 16                 # its polygon's quad flag,
	sll     $t4, $t4, 16
	ori     $t4, $t4, 0x100              # cut
	or      $t4, $t4, $a2                # n
	sw      $t4, PC_N($s7)
	addiu   $t5, $s7, PC_V
	move    $t6, $a1
	move    $t7, $a2
14:	COPYV   $t5, $t6
	addiu   $t7, $t7, -1
	addiu   $t5, $t5, V_SIZE
	bnez    $t7, 14b
	addiu   $t6, $t6, V_SIZE
	addiu   $s4, $s4, 1
	addiu   $s7, $s7, PC_SIZE
gs_pop:
	lw      $v1, BS_SPEND($fp)
	nop
	beqz    $v1, cr_next                 # no back piece left: the polygon is cut
	addiu   $v1, $v1, -1                 # (delay)
	sw      $v1, BS_SPEND($fp)
	sll     $t2, $v1, 3                  # the entry
	sll     $t3, $v1, 4
	sll     $t4, $v1, 7
	addu    $t2, $t2, $t3
	addu    $t2, $t2, $t4
	lui     $t3, %hi(bmf_pending)
	addiu   $t3, $t3, %lo(bmf_pending)
	addu    $t2, $t3, $t2
	lw      $a0, PD_AT($t2)
	lw      $a2, PD_N($t2)
	addiu   $a1, $fp, BS_T               # its corners into T (a split of it
	addiu   $t5, $fp, BS_T               # writes the front into U)
	addiu   $t6, $t2, PD_V
	move    $t7, $a2
15:	COPYV   $t5, $t6
	addiu   $t7, $t7, -1
	addiu   $t5, $t5, V_SIZE
	bnez    $t7, 15b
	addiu   $t6, $t6, V_SIZE
	b       gs_node
	nop

# -----------------------------------------------------------------------------
# cross: where the edge a-b (t5, t6: corners; t7, t8 their distances, of
# opposite signs) crosses the plane, into a0: from the edge's lower end
# (x, then y, then z), whichever way it runs (two faces sharing it get the
# same point); t = da / (da - db) x 4096, both shifted down together until
# (da << 12) fits, the division exact to the last bits; each coordinate,
# the texels and the light a + ((b - a) t + 2048 >> 12). Clobbers t5-t8,
# v0, at, HI / LO.
cross:
	lh      $v0, 0($t5)
	lh      $at, 0($t6)
	nop
	bne     $v0, $at, 1f
	slt     $v0, $at, $v0                # (delay) b < a: from b
	lh      $v0, 2($t5)
	lh      $at, 2($t6)
	nop
	bne     $v0, $at, 1f
	slt     $v0, $at, $v0
	lh      $v0, 4($t5)
	lh      $at, 4($t6)
	nop
	slt     $v0, $at, $v0
1:	beqz    $v0, 2f
	move    $v0, $t5                     # (delay)
	move    $t5, $t6                     # a, b swapped, and their d's
	move    $t6, $v0
	move    $v0, $t7
	move    $t7, $t8
	move    $t8, $v0
2:	subu    $t8, $t7, $t8                # den = da - db (num = da)
	bgez    $t8, 3f
	lui     $at, 8                       # (delay) 1 << 19
	subu    $t7, $zero, $t7
	subu    $t8, $zero, $t8
3:	slt     $v0, $t8, $at
	bnez    $v0, 4f                      # den < 1 << 19: (num << 12) fits
	nop
	sra     $t7, $t7, 1
	b       3b
	sra     $t8, $t8, 1
4:	sll     $t7, $t7, 12
	div     $zero, $t7, $t8
	li      $v0, 3                       # (while it divides: x, y, z)
	nop
	mflo    $at                          # t
5:	lh      $t7, 0($t5)                  # a coordinate: a + ((b - a) t + 2048 >> 12)
	lh      $t8, 0($t6)
	addiu   $v0, $v0, -1
	subu    $t8, $t8, $t7
	mult    $t8, $at
	addiu   $t5, $t5, 2
	addiu   $t6, $t6, 2
	mflo    $t8
	addiu   $a0, $a0, 2
	nop
	addiu   $t8, $t8, 2048
	sra     $t8, $t8, 12
	addu    $t7, $t7, $t8
	bnez    $v0, 5b
	sh      $t7, -2($a0)                 # (delay)
	li      $v0, 3                       # u, v, the light (bytes)
6:	lbu     $t7, 0($t5)
	lbu     $t8, 0($t6)
	addiu   $v0, $v0, -1
	subu    $t8, $t8, $t7
	mult    $t8, $at
	addiu   $t5, $t5, 1
	addiu   $t6, $t6, 1
	mflo    $t8
	addiu   $a0, $a0, 1
	nop
	addiu   $t8, $t8, 2048
	sra     $t8, $t8, 12
	addu    $t7, $t7, $t8
	bnez    $v0, 6b
	sb      $t7, -1($a0)                 # (delay)
	sb      $zero, 0($a0)                # the pads (a0: the corner + 9)
	sh      $zero, 1($a0)
	jr      $ra
	addiu   $a0, $a0, -9                 # (delay) a0 as it came

# -----------------------------------------------------------------------------
# put_pieces: the record's pieces so far (s4, from bmf_pieces) per leaf in
# the order they came: records of up to QW_MAX_VERTS corners (put_record),
# then their group (the side's last one grows when it is the same leaf's
# and the records follow). v0 = 1, 0 no room. After: s4 0, s7 the first
# slot. Keeps s0-s3, s5, s6, fp.
put_pieces:
	sw      $ra, BS_RA2($fp)
	lui     $t0, %hi(bmf_cut_pieces)
	lw      $t1, %lo(bmf_cut_pieces)($t0)
	lui     $a0, %hi(bmf_pieces)
	addiu   $a0, $a0, %lo(bmf_pieces)    # piece i
	addu    $t1, $t1, $s4
	sw      $t1, %lo(bmf_cut_pieces)($t0)
	li      $t3, 1
	bne     $s4, $t3, pp_many
	addiu   $a2, $a0, PC_SIZE            # (delay) one piece: its record, its
	sw      $a0, BS_BATCH($fp)           # group (a0 the piece, a2 past it)
	lw      $a3, PC_LEAF($a0)
	jal     put_record
	lw      $v1, CA_RECS_LEN($s0)        # (delay) the group's start
	b       pp_group
	nop
pp_many:
	sll     $t2, $s4, 3                  # s4 x 152
	sll     $t3, $s4, 4
	sll     $t4, $s4, 7
	addu    $t2, $t2, $t3
	addu    $t2, $t2, $t4
	addu    $a2, $a0, $t2                # the pieces' end
pp_i:
	beq     $a0, $a2, pp_done
	nop
	lbu     $t0, PC_DONE($a0)
	lw      $a3, PC_LEAF($a0)            # its leaf
	bnez    $t0, pp_next_i               # put already
	lw      $v1, CA_RECS_LEN($s0)        # (delay) the group's start
	move    $a1, $a0                     # piece j
	move    $s4, $zero                   # the batch's pieces
	move    $s7, $zero                   # and corners
pp_j:
	beq     $a1, $a2, pp_last
	nop
	lbu     $t0, PC_DONE($a1)
	lw      $t1, PC_LEAF($a1)
	bnez    $t0, pp_next_j
	lbu     $t2, PC_N($a1)               # (delay)
	bne     $t1, $a3, pp_next_j          # another leaf's
	addu    $t3, $s7, $t2                # (delay)
	slti    $t3, $t3, QW_MAX_VERTS + 1
	bnez    $t3, 1f
	nop
	jal     put_record                   # no room for it in the batch: a
	nop                                  # record of the batch first
	beqz    $v0, pp_fail
	move    $s4, $zero                   # (delay)
	lbu     $t2, PC_N($a1)
	move    $s7, $zero
1:	sll     $t5, $s4, 2
	addu    $t5, $fp, $t5
	sw      $a1, BS_BATCH($t5)
	addiu   $s4, $s4, 1
	addu    $s7, $s7, $t2
	li      $t6, 1
	sb      $t6, PC_DONE($a1)
pp_next_j:
	b       pp_j
	addiu   $a1, $a1, PC_SIZE
pp_last:
	jal     put_record
	nop
pp_group:
	beqz    $v0, pp_fail
	lw      $t0, CA_RECS_LEN($s0)        # (delay)
	lw      $t1, CA_NUM_GROUPS($s0)
	lw      $t2, BS_SIDEF($fp)
	lw      $t3, CA_GROUPS($s0)
	subu    $t0, $t0, $v1                # the group's bytes
	sll     $t5, $t1, 3
	sll     $t6, $t1, 2
	addu    $t5, $t5, $t6
	addu    $t5, $t3, $t5                # groups + num_groups
	sltu    $t4, $t2, $t1                # a group of this side before
	beqz    $t4, 2f
	nop
	lw      $t6, BG_LEAF - BG_SIZE($t5)  # the side's last group
	lw      $t7, BG_OFS - BG_SIZE($t5)
	lw      $t8, BG_LEN - BG_SIZE($t5)
	bne     $t6, $a3, 2f                 # another leaf's
	addu    $t7, $t7, $t8                # (delay) its end
	bne     $t7, $v1, 2f                 # not followed by these
	addu    $t8, $t8, $t0                # (delay)
	b       pp_next_i                    # it grows
	sw      $t8, BG_LEN - BG_SIZE($t5)
2:	lw      $t6, CA_GROUPS_CAP($s0)
	nop
	sltu    $t7, $t1, $t6
	beqz    $t7, pp_fail                 # no room for a group
	addiu   $t1, $t1, 1                  # (delay)
	sw      $a3, BG_LEAF($t5)
	sw      $v1, BG_OFS($t5)
	sw      $t0, BG_LEN($t5)
	sw      $t1, CA_NUM_GROUPS($s0)
pp_next_i:
	b       pp_i
	addiu   $a0, $a0, PC_SIZE
pp_done:
	li      $v0, 1
pp_ret:
	lw      $ra, BS_RA2($fp)
	move    $s4, $zero
	lui     $s7, %hi(bmf_pieces)
	jr      $ra
	addiu   $s7, $s7, %lo(bmf_pieces)
pp_fail:
	b       pp_ret
	move    $v0, $zero

# -----------------------------------------------------------------------------
# put_record: the batch (BS_BATCH: s4 pieces of one leaf) as one record,
# appended to c->recs, its vertices to bmf_pool: the corners in order, a
# piece as it was its own polygon (a quad back in the PS1's corner order),
# a cut one fanned from its first corner into quads (m, 0, m+1, m+2) and a
# last triangle (0, m, m+1); the indices padded with the first's to a
# multiple of 3, then to 4 bytes, the light bytes with zeros; its sphere
# about its box's centre (truncated) to its farthest corner, + 1; its
# average light (the flat shading's) in its flags. v0 = 1, 0 no room.
# Keeps s0-s3, s5, s6, fp, a0-a3, v1 (s4, s7: put_pieces sets them again).
put_record:
	sw      $a0, BS_A0($fp)
	sw      $a1, BS_A1($fp)
	lw      $t9, BS_BATCH($fp)           # the first piece
	li      $t0, 1
	lw      $t6, PC_N($t9)
	bne     $s4, $t0, pr_many
	andi    $t1, $t6, 0x100              # (delay) cut?
	beqz    $t1, pr_one                  # one piece as it was: the short way
	nop
pr_many:
	addiu   $t0, $fp, BS_BATCH           # the counts
	sll     $t1, $s4, 2
	addu    $t1, $t0, $t1                # the batch's end
	move    $t2, $zero                   # corners
	move    $t3, $zero                   # quads
	move    $t4, $zero                   # triangles
pr_count:
	lw      $t5, 0($t0)
	addiu   $t0, $t0, 4
	lw      $t6, PC_N($t5)               # n | cut << 8 | quad << 16
	nop
	andi    $t7, $t6, 0xff
	addu    $t2, $t2, $t7
	andi    $t8, $t6, 0x100
	bnez    $t8, 1f                      # a cut piece: its fan
	addiu   $t7, $t7, -2                 # (delay) n - 2
	srl     $t8, $t6, 16
	andi    $t8, $t8, 1                  # as it was: a quad or a triangle
	addu    $t3, $t3, $t8
	xori    $t8, $t8, 1
	b       2f
	addu    $t4, $t4, $t8
1:	srl     $t8, $t7, 1
	addu    $t3, $t3, $t8
	andi    $t8, $t7, 1
	addu    $t4, $t4, $t8
2:	bne     $t0, $t1, pr_count
	nop
	addiu   $t5, $t2, 2                  # the sizes: nidx = (nv + 2) / 3 x 3
	li      $t6, 171                     # (x 171 >> 9: / 3 for x < 128)
	multu   $t5, $t6
	addiu   $t7, $t2, 3
	srl     $t7, $t7, 2
	sll     $t7, $t7, 2                  # the light bytes: (nv + 3) & ~3
	addu    $t8, $t3, $t4                # the polygons
	mflo    $t5
	srl     $t5, $t5, 9
	sll     $t6, $t5, 1
	addu    $t5, $t5, $t6                # nidx
	sll     $t6, $t5, 1
	addiu   $t6, $t6, 3
	srl     $t6, $t6, 2
	sll     $t6, $t6, 2                  # their bytes: (nidx x 2 + 3) & ~3
	addiu   $t9, $t6, R_VERTS
	addu    $t9, $t9, $t7                # polys = 20 + ibytes + cbytes
	sll     $v0, $t8, 1
	addu    $v0, $v0, $t8
	sll     $v0, $v0, 2
	addu    $v0, $t9, $v0                # the record's bytes
	lw      $at, CA_RECS_LEN($s0)        # the room
	lw      $t8, CA_RECS_CAP($s0)
	addu    $at, $at, $v0
	sltu    $t8, $t8, $at
	bnez    $t8, pr_full
	lw      $t8, CA_POOL_LEN($s0)        # (delay)
	lw      $at, CA_POOL_CAP($s0)
	addu    $t8, $t8, $t2
	sltu    $at, $at, $t8
	bnez    $at, pr_full
	sw      $t2, PR_NV($fp)              # (delay)
	sw      $t3, PR_NQ($fp)
	sw      $t4, PR_NT($fp)
	sw      $t5, PR_NIDX($fp)
	sw      $t6, PR_IBYTES($fp)
	sw      $t7, PR_CBYTES($fp)
	sw      $t9, PR_POLYS($fp)
	sw      $v0, PR_SIZE($fp)
	lw      $t0, CA_RECS($s0)
	lw      $t1, CA_RECS_LEN($s0)
	lw      $t2, CA_POOL_FIRST($s0)
	lw      $t3, CA_POOL_LEN($s0)
	addu    $t0, $t0, $t1                # the record
	addu    $t2, $t2, $t3                # its first vertex's pool index
	sw      $t0, PR_REC($fp)
	sw      $t2, PR_BASE($fp)

	# the vertices: the pool, the indices, the light bytes
	lui     $t1, %hi(bmf_pool)
	lw      $t1, %lo(bmf_pool)($t1)
	sll     $t3, $t2, 3
	addu    $t1, $t1, $t3                # the pool's next vertex
	addiu   $t3, $t0, R_VERTS            # the next index
	addu    $t4, $t0, $t9
	subu    $t4, $t4, $t7                # the next light byte
	move    $t5, $t2                     # the next pool index
	move    $t6, $zero                   # the light's sum
	addiu   $t7, $fp, BS_BATCH
	sll     $t8, $s4, 2
	addu    $t8, $t7, $t8
pr_vpiece:
	lw      $t9, 0($t7)
	addiu   $t7, $t7, 4
	lbu     $v0, PC_N($t9)
	addiu   $t9, $t9, PC_V
pr_vert:
	lw      $at, 4($t9)                  # z | texels << 16
	lw      $t0, 0($t9)                  # x | y << 16
	lw      $t2, 8($t9)                  # the light
	andi    $at, $at, 0xffff             # z, pad 0
	sw      $t0, 0($t1)
	sw      $at, 4($t1)
	sh      $t5, 0($t3)
	sb      $t2, 0($t4)
	addu    $t6, $t6, $t2
	addiu   $t1, $t1, 8
	addiu   $t3, $t3, 2
	addiu   $t4, $t4, 1
	addiu   $t5, $t5, 1
	addiu   $v0, $v0, -1
	bnez    $v0, pr_vert
	addiu   $t9, $t9, V_SIZE
	bne     $t7, $t8, pr_vpiece
	nop
	sw      $t6, PR_LSUM($fp)
	lw      $t0, PR_BASE($fp)            # the padding: the indices with the
	lw      $t2, PR_NV($fp)              # first's to nidx, then a 0 to 4 bytes;
	lw      $t5, PR_NIDX($fp)            # the light bytes with zeros
	lw      $t6, PR_IBYTES($fp)
	subu    $t7, $t5, $t2
	beqz    $t7, 4f
	nop
3:	sh      $t0, 0($t3)
	addiu   $t7, $t7, -1
	bnez    $t7, 3b
	addiu   $t3, $t3, 2
4:	sll     $t7, $t5, 1
	sltu    $t7, $t7, $t6
	lw      $t6, PR_CBYTES($fp)
	beqz    $t7, 5f
	nop
	sh      $zero, 0($t3)
5:	subu    $t7, $t6, $t2
	beqz    $t7, 7f
	nop
6:	sb      $zero, 0($t4)
	addiu   $t7, $t7, -1
	bnez    $t7, 6b
	addiu   $t4, $t4, 1
7:
	# the box: a piece as it was by its box, a cut one by its corners
	li      $t0, 32767
	li      $t1, 32767
	li      $t2, 32767
	li      $t3, -32768
	li      $t4, -32768
	li      $t5, -32768
	addiu   $t7, $fp, BS_BATCH
	sll     $t8, $s4, 2
	addu    $t8, $t7, $t8
pr_bpiece:
	lw      $t9, 0($t7)
	addiu   $t7, $t7, 4
	lw      $t6, PC_N($t9)
	nop
	andi    $a0, $t6, 0x100
	bnez    $a0, pr_bcut
	andi    $a1, $t6, 0xff               # (delay) its corners
	lh      $t6, PC_BOX+0($t9)
	nop
	MINW    $t0, $t6
	lh      $t6, PC_BOX+2($t9)
	nop
	MINW    $t1, $t6
	lh      $t6, PC_BOX+4($t9)
	nop
	MINW    $t2, $t6
	lh      $t6, PC_BOX+6($t9)
	nop
	MAXW    $t3, $t6
	lh      $t6, PC_BOX+8($t9)
	nop
	MAXW    $t4, $t6
	lh      $t6, PC_BOX+10($t9)
	nop
	MAXW    $t5, $t6
	b       pr_bnext
	nop
pr_bcut:
	addiu   $t9, $t9, PC_V
8:	lh      $t6, 0($t9)
	nop
	MINW    $t0, $t6
	MAXW    $t3, $t6
	lh      $t6, 2($t9)
	nop
	MINW    $t1, $t6
	MAXW    $t4, $t6
	lh      $t6, 4($t9)
	addiu   $a1, $a1, -1
	MINW    $t2, $t6
	MAXW    $t5, $t6
	bnez    $a1, 8b
	addiu   $t9, $t9, V_SIZE
pr_bnext:
	bne     $t7, $t8, pr_bpiece
	nop
	addu    $t0, $t0, $t3                # sc = (lo + hi) / 2, truncated
	addu    $t1, $t1, $t4                # toward zero as C divides
	addu    $t2, $t2, $t5
	srl     $t3, $t0, 31
	srl     $t4, $t1, 31
	srl     $t5, $t2, 31
	addu    $t0, $t0, $t3
	addu    $t1, $t1, $t4
	addu    $t2, $t2, $t5
	sra     $t0, $t0, 1
	sra     $t1, $t1, 1
	sra     $t2, $t2, 1
	sw      $t0, PR_LO+0($fp)            # (kept for the header)
	sw      $t1, PR_LO+4($fp)
	sw      $t2, PR_LO+8($fp)

	# the radius: the farthest corner's squared distance from sc
pr_radius:                                # (sc: t0, t1, t2)
	move    $t7, $t2                     # (sc: t0, t1, t7)
	lw      $t3, PR_BASE($fp)
	lw      $t4, PR_NV($fp)
	lui     $t9, %hi(bmf_pool)
	lw      $t9, %lo(bmf_pool)($t9)
	sll     $t3, $t3, 3
	addu    $t9, $t9, $t3                # the record's first vertex
	move    $t2, $zero                   # r2
9:	SQD     $t5, $t9, $t0, $t1, $t7
	addiu   $t4, $t4, -1
	MAXU    $t2, $t5
	bnez    $t4, 9b
	addiu   $t9, $t9, 8
	ISQRT
	addiu   $t3, $t3, 1                  # sr = isqrt + 1

	# the header and the polygons
	lw      $t4, PR_REC($fp)
	lw      $t5, PR_NV($fp)
	lw      $t6, PR_NQ($fp)
	lw      $t7, PR_LSUM($fp)
	sll     $t8, $t5, 16
	sll     $v0, $t6, 24
	or      $t8, $t8, $v0                # face_id 0 | nv << 16 | nq << 24
	sw      $t8, 0($t4)
	sll     $t8, $t5, 1
	addu    $t7, $t7, $t8                # (lsum + nv x 2) / (nv x 4): the
	sll     $t8, $t5, 2                  # average light (world.c's)
	divu    $zero, $t7, $t8
	lhu     $t8, R_TEX($s3)
	sll     $t3, $t3, 16
	or      $t8, $t8, $t3                # tex | sr << 16
	sw      $t8, 8($t4)
	lw      $t0, PR_LO+0($fp)
	lw      $t1, PR_LO+4($fp)
	lw      $t2, PR_LO+8($fp)
	lw      $t3, PR_POLYS($fp)
	andi    $t0, $t0, 0xffff
	sll     $t1, $t1, 16
	or      $t0, $t0, $t1
	sw      $t0, 12($t4)                 # sc0 | sc1 << 16
	andi    $t2, $t2, 0xffff
	sll     $t1, $t3, 16
	or      $t2, $t2, $t1
	sw      $t2, 16($t4)                 # sc2 | polys << 16
	lbu     $t1, R_FLAGS($s3)
	lw      $t2, PR_SIZE($fp)
	lw      $t0, PR_NT($fp)
	mflo    $t7                          # the average light
	andi    $t1, $t1, 3                  # QWF_SKY | QWF_LIQUID
	sltiu   $t3, $t7, 64
	bnez    $t3, 14f
	nop
	li      $t7, 63
14:	sll     $t7, $t7, 2
	or      $t1, $t1, $t7                # the flags
	sll     $t1, $t1, 8
	or      $t0, $t0, $t1
	sll     $t3, $t2, 16
	or      $t0, $t0, $t3                # nt | flags << 8 | size << 16
	sw      $t0, 4($t4)

	# the polygons, piece by piece
	lw      $t1, PR_POLYS($fp)
	lw      $t3, PR_NQ($fp)
	addu    $t0, $t4, $t1                # the next quad
	sll     $t1, $t3, 1
	addu    $t1, $t1, $t3
	sll     $t1, $t1, 2
	addu    $t1, $t0, $t1                # the next triangle
	move    $t2, $zero                   # the piece's first corner x 4 (v4)
	addiu   $t7, $fp, BS_BATCH
	sll     $t8, $s4, 2
	addu    $t8, $t7, $t8
pr_ppiece:
	lw      $t9, 0($t7)
	addiu   $t7, $t7, 4
	lw      $t6, PC_N($t9)
	sll     $t3, $t2, 8                  # v4 in every byte (v4 < 168)
	or      $t3, $t3, $t2
	sll     $t4, $t3, 16
	or      $t3, $t3, $t4
	andi    $t4, $t6, 0x100
	bnez    $t4, pr_pcut
	andi    $a1, $t6, 0xff               # (delay) n
	srl     $t4, $t6, 16
	andi    $t4, $t4, 1
	beqz    $t4, pr_ptri
	lhu     $t5, PC_V + 6($t9)           # (delay) corner 0's texels
	lhu     $t6, PC_V + 18($t9)          # a quad as it was, in the PS1's
	lhu     $v0, PC_V + 42($t9)          # order: the outline's 0 1 3 2
	lhu     $at, PC_V + 30($t9)
	lui     $t4, 0x080c                  # its indices v4 + 0, 4, 12, 8
	ori     $t4, $t4, 0x0400
	addu    $t4, $t4, $t3
	sw      $t4, 0($t0)
	sll     $t6, $t6, 16
	or      $t5, $t5, $t6
	sw      $t5, 4($t0)                  # the texels of corners 0, 1
	sll     $at, $at, 16
	or      $v0, $v0, $at
	sw      $v0, 8($t0)                  # 3, 2
	b       pr_pnext
	addiu   $t0, $t0, 12                 # (delay)
pr_ptri:
	lhu     $t6, PC_V + 18($t9)          # a triangle as it was
	lhu     $v0, PC_V + 30($t9)
	lui     $at, 0x0008                  # its indices v4 + 0, 4, 8, a 0 pad
	ori     $at, $at, 0x0400
	sll     $t4, $t3, 8
	srl     $t4, $t4, 8
	addu    $t4, $t4, $at
	sw      $t4, 0($t1)
	sll     $t6, $t6, 16
	or      $t5, $t5, $t6
	sw      $t5, 4($t1)
	sw      $v0, 8($t1)                  # (its pad2 0)
	b       pr_pnext
	addiu   $t1, $t1, 12                 # (delay)
pr_pcut:                                  # a cut one: quads (m, 0, m+1, m+2),
	li      $t4, 1                       # m = 1, 3...; a triangle (0, m, m+1)
	lhu     $t5, PC_V + 6($t9)           # for an odd n
16:	addiu   $t6, $t4, 2
	slt     $t6, $t6, $a1
	beqz    $t6, 17f                     # m + 2 < n: a quad
	sll     $t6, $t4, 1                  # (delay)
	addu    $t6, $t6, $t4
	sll     $t6, $t6, 2
	addu    $t6, $t9, $t6                # corner m, less PC_V
	lhu     $v0, PC_V + 6($t6)
	lhu     $at, PC_V + 18($t6)
	lhu     $t6, PC_V + 30($t6)
	sll     $a0, $t5, 16
	or      $v0, $v0, $a0                # the texels of m, 0
	sw      $v0, 4($t0)
	sll     $t6, $t6, 16
	or      $at, $at, $t6                # m + 1, m + 2
	sw      $at, 8($t0)
	sll     $v0, $t4, 2                  # 4m in bytes 0, 2, 3; then + 0, 0, 4, 8
	sll     $at, $v0, 16
	or      $at, $at, $v0
	sll     $a0, $v0, 24
	or      $at, $at, $a0
	lui     $a0, 0x0804
	addu    $at, $at, $a0
	addu    $at, $at, $t3                # v4 + 4m, v4, v4 + 4m + 4, v4 + 4m + 8
	sw      $at, 0($t0)
	addiu   $t0, $t0, 12
	b       16b
	addiu   $t4, $t4, 2                  # (delay)
17:	addiu   $t6, $t4, 1
	slt     $t6, $t6, $a1
	beqz    $t6, pr_pnext                # m + 1 < n: the triangle
	sll     $t6, $t4, 1                  # (delay)
	addu    $t6, $t6, $t4
	sll     $t6, $t6, 2
	addu    $t6, $t9, $t6
	lhu     $v0, PC_V + 6($t6)
	lhu     $at, PC_V + 18($t6)
	sll     $v0, $v0, 16
	or      $v0, $v0, $t5                # the texels of 0, m
	sw      $v0, 4($t1)
	sw      $at, 8($t1)                  # m + 1 (pad2 0)
	sll     $v0, $t4, 10                 # 4m in bytes 1, 2; then + 0, 0, 4
	sll     $at, $t4, 18
	or      $v0, $v0, $at
	lui     $at, 0x0004
	addu    $v0, $v0, $at
	sll     $at, $t3, 8
	srl     $at, $at, 8                  # v4 in bytes 0-2
	addu    $v0, $v0, $at                # v4, v4 + 4m, v4 + 4m + 4, 0
	sw      $v0, 0($t1)
	addiu   $t1, $t1, 12
pr_pnext:
	sll     $a1, $a1, 2
	bne     $t7, $t8, pr_ppiece
	addu    $t2, $t2, $a1                # (delay) the next piece's first corner
	lw      $t0, CA_RECS_LEN($s0)        # appended
	lw      $t1, PR_SIZE($fp)
	lw      $t2, CA_POOL_LEN($s0)
	lw      $t3, PR_NV($fp)
	addu    $t0, $t0, $t1
	addu    $t2, $t2, $t3
	sw      $t0, CA_RECS_LEN($s0)
	sw      $t2, CA_POOL_LEN($s0)
	lw      $a0, BS_A0($fp)
	lw      $a1, BS_A1($fp)
	jr      $ra
	li      $v0, 1

	# ---- one piece as it was: a quad (4 corners, 48 bytes: 6 indices, 4
	# light bytes, 1 quad) or a triangle (3 corners, 44 bytes: 3 indices
	# and a 0, 3 light bytes and a 0, 1 triangle): no counting, its box
	# its own; then the general way's radius, header and polygon (their
	# own copy here was 4.5 % faster and 400 bytes more than the stack's
	# room allows) -------------------------------------------------------------
pr_one:                                   # t9 the piece, t6 its PC_N word
	srl     $a0, $t6, 16
	andi    $a0, $a0, 1                  # 1 a quad, 0 a triangle
	lw      $t0, CA_RECS_LEN($s0)
	lw      $t1, CA_RECS_CAP($s0)
	sll     $t4, $a0, 2
	addiu   $t4, $t4, 44                 # its bytes
	addu    $t2, $t0, $t4
	sltu    $t2, $t1, $t2
	bnez    $t2, pr_full                 # no room
	lw      $t5, CA_POOL_LEN($s0)        # (delay)
	lw      $t1, CA_POOL_CAP($s0)
	addiu   $t3, $a0, 3                  # its corners
	addu    $t2, $t5, $t3
	sltu    $t2, $t1, $t2
	bnez    $t2, pr_full
	lw      $t1, CA_POOL_FIRST($s0)      # (delay)
	lw      $t2, CA_RECS($s0)
	sw      $t3, PR_NV($fp)              # the general way's values: its radius,
	sw      $t4, PR_SIZE($fp)            # header and polygon, the appending
	sw      $a0, PR_NQ($fp)
	xori    $t6, $a0, 1
	sw      $t6, PR_NT($fp)
	addiu   $t6, $t4, -12
	sw      $t6, PR_POLYS($fp)           # (its bytes less its polygon)
	addu    $t1, $t1, $t5                # its first vertex's pool index
	addu    $t0, $t2, $t0                # the record
	sw      $t1, PR_BASE($fp)
	sw      $t0, PR_REC($fp)
	lui     $t2, %hi(bmf_pool)
	lw      $t2, %lo(bmf_pool)($t2)
	sll     $t5, $t1, 3
	addu    $t2, $t2, $t5                # its vertices
	lw      $t5, PC_V + 0($t9)           # corners 0-2: the pool, the light bytes
	lhu     $t6, PC_V + 4($t9)
	lbu     $t7, PC_V + 8($t9)
	lw      $t8, PC_V + 12($t9)
	sw      $t5, 0($t2)
	sw      $t6, 4($t2)
	lhu     $t5, PC_V + 16($t9)
	lbu     $t6, PC_V + 20($t9)
	sw      $t8, 8($t2)
	sw      $t5, 12($t2)
	lw      $t5, PC_V + 24($t9)
	lhu     $t8, PC_V + 28($t9)
	lbu     $v0, PC_V + 32($t9)
	sw      $t5, 16($t2)
	sw      $t8, 20($t2)
	sll     $t8, $t6, 8
	or      $t8, $t8, $t7                # the light bytes
	addu    $t7, $t7, $t6                # their sum
	sll     $t6, $v0, 16
	or      $t8, $t8, $t6
	addu    $t7, $t7, $v0
	addiu   $t5, $t1, 1
	sll     $t5, $t5, 16
	or      $t5, $t5, $t1
	sw      $t5, R_VERTS($t0)            # indices 0, 1
	beqz    $a0, 1f                      # a triangle: index 2 and a 0
	addiu   $t5, $t1, 2                  # (delay)
	lw      $t6, PC_V + 36($t9)          # a quad: corner 3
	lhu     $v0, PC_V + 40($t9)
	lbu     $at, PC_V + 44($t9)
	sw      $t6, 24($t2)
	sw      $v0, 28($t2)
	sll     $t6, $at, 24
	or      $t8, $t8, $t6
	addu    $t7, $t7, $at
	addiu   $t6, $t1, 3
	sll     $t6, $t6, 16
	or      $t5, $t5, $t6                # indices 2, 3
	sll     $t6, $t1, 16
	or      $t6, $t6, $t1
	sw      $t6, R_VERTS + 8($t0)        # 4, 5: the first's
1:	sw      $t5, R_VERTS + 4($t0)
	sll     $t6, $a0, 2
	addu    $t6, $t0, $t6
	sw      $t8, R_VERTS + 8($t6)        # the light bytes (a quad's after 12
	sw      $t7, PR_LSUM($fp)            #  index bytes, a triangle's after 8)
	SCAX    $t0, 0                       # sc: its box's centre
	SCAX    $t1, 1
	SCAX    $t2, 2
	sw      $t0, PR_LO+0($fp)
	sw      $t1, PR_LO+4($fp)
	b       pr_radius                    # the rest the general way's
	sw      $t2, PR_LO+8($fp)            # (delay)

pr_full:
	lw      $a0, BS_A0($fp)
	lw      $a1, BS_A1($fp)
	jr      $ra
	move    $v0, $zero

	.size   bmf_cut, . - bmf_cut
