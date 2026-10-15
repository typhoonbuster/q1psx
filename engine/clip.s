# q1psx — hull trace, hand-written MIPS assembly (R4.3).
#
# Port of engine/clip.c, which is the specification: same algorithm, same
# integer operations, bit-identical results (the bench build links both and
# compares them call by call). In short:
#
#   clip_trace(a0 = hull, a1 = start[3], a2 = end[3], a3 = trace_t *) -> v0
#     Quake's SV_RecursiveHullCheck, iterative: a sweep that crosses a plane
#     is split DIST_EPS short of it, the near side is walked first and the
#     split is pushed on the stack; when a sub-sweep ends in an open leaf the
#     innermost split resumes with its far side. Plane distances are never
#     taken at materialised points but interpolated along the whole sweep:
#     t(f) = ds + (dd * f >> 16), ds = n.start - dist * 8, dd = n.delta.
#   clip_contents(a0 = hull, a1 = p[3]) -> v0 = Quake CONTENTS_*
#
# Plane dot products: axial planes (78 % of start.bsp's clipnodes) on the
# CPU (n = 4096 on one axis: start and delta x 4096 are kept in the
# scratchpad, indexed by the axis); general planes on the GTE, whose
# rotation matrix holds start, delta and a probe point as its three rows:
# one MVMVA of the normal (V0) returns n.start, n.delta and n.point in
# MAC1-3. That needs positions in int16 (+-4095 world units, Quake's limit).
#
# The renderer reloads the GTE matrices at the start of every r_render, so
# borrowing the rotation matrix here is free. Scratchpad: see p_defs.inc.

	.set noreorder
	.set noat

	.include "gtereg.inc"
	.include "inline_s.inc"
	.include "p_defs.inc"

# -----------------------------------------------------------------------------
# NODE_DSDD: t9 = clipnode number (>= 0), s1 = clipnodes, s2 = planes,
# s7 = scratchpad (PS_S4K / PS_D4K = start / delta x 4096), GTE rows 1-2 =
# start / delta. Out: t0 = clipnode pointer, a2 / a3 = children, t5 = ds,
# t6 = dd. Clobbers t1-t4, v1. pre=0: t0 = t9 << 2 is already done (the
# walk does it in a delay slot). Clipnodes are 6 bytes.
.macro NODE_DSDD pre=1
	.if \pre
	sll     $t0, $t9, 2
	.endif
	sll     $t2, $t9, 1
	addu    $t0, $t0, $t2                # num * 6
	addu    $t0, $s1, $t0                # clipnode
	lhu     $t1, CN_PLANE3($t0)
	lh      $a2, CN_CHILD+0($t0)
	sll     $t1, $t1, 2
	addu    $t1, $s2, $t1                # plane
	lbu     $t2, PL_TYPE($t1)
	lw      $t3, PL_DIST($t1)
	lh      $a3, CN_CHILD+2($t0)
	sltiu   $t4, $t2, 3
	beqz    $t4, .Lgen\@
	sll     $t2, $t2, 2
	addu    $t2, $s7, $t2                # axial: n = 4096 on axis `type`
	lw      $t5, PS_S4K($t2)
	lw      $t6, PS_D4K($t2)             # dd
	b       .Ldone\@
	subu    $t5, $t5, $t3                # ds
.Lgen\@:
	lh      $v1, PL_N+4($t1)             # n[2] into VZ0: not lwc2, the word's
	lwc2    C2_VXY0, PL_N+0($t1)         # upper half is the plane type, which
	mtc2    $v1, C2_VZ0                  # PCSX-Redux does not drop (see the
	nop                                  # renderer's R_SC)
	nop
	MVMVA   0, 0, 0, 3, 0                # MAC1 = n.start, MAC2 = n.delta
	mfc2    $t5, C2_MAC1
	mfc2    $t6, C2_MAC2
	subu    $t5, $t5, $t3                # ds
.Ldone\@:
.endm

# NODE_DP: like NODE_DSDD for a point: a0 = point (int32[3]), GTE row 3 =
# the same point. Out: a2 / a3 = children, t5 = n.p - dist.
# Clobbers t0-t4, v1. t0 = t9 << 2 is done by the caller (a delay slot).
.macro NODE_DP
	sll     $t2, $t9, 1
	addu    $t0, $t0, $t2                # num * 6
	addu    $t0, $s1, $t0
	lhu     $t1, CN_PLANE3($t0)
	lh      $a2, CN_CHILD+0($t0)
	sll     $t1, $t1, 2
	addu    $t1, $s2, $t1
	lbu     $t2, PL_TYPE($t1)
	lw      $t3, PL_DIST($t1)
	lh      $a3, CN_CHILD+2($t0)
	sltiu   $t4, $t2, 3
	beqz    $t4, .Lgen\@
	sll     $t2, $t2, 2
	addu    $t2, $a0, $t2
	lw      $t5, 0($t2)
	b       .Ldone\@
	sll     $t5, $t5, 12
.Lgen\@:
	lh      $v1, PL_N+4($t1)
	lwc2    C2_VXY0, PL_N+0($t1)
	mtc2    $v1, C2_VZ0
	nop
	nop
	MVMVA   0, 0, 0, 3, 0                # MAC3 = n.point
	mfc2    $t5, C2_MAC3
	nop
.Ldone\@:
	subu    $t5, $t5, $t3
.endm

# FRAC_MUL rd, a, f: rd = (a * f) >> 16 (64-bit product, floor). Clobbers $at.
.macro FRAC_MUL rd, a, f
	mult    \a, \f
	mflo    \rd
	mfhi    $at
	srl     \rd, \rd, 16
	sll     $at, $at, 16
	or      \rd, \rd, $at
.endm

	.section .text.clip_trace, "ax", @progbits
	.global clip_trace
	.type   clip_trace, @function

# -----------------------------------------------------------------------------
# Registers: s0 trace_t, s1 clipnodes, s2 planes, s3 root, s4 f1, s5 f2,
# fp F_ONE,
# s6 pending splits (16 B each on the stack: clipnode pointer | side, f1,
# fm, f2),
# s7 scratchpad, t9 current clipnode / contents.
clip_trace:
	addiu   $sp, $sp, -40
	sw      $fp, 36($sp)
	sw      $ra, 0($sp)
	sw      $s0, 4($sp)
	sw      $s1, 8($sp)
	sw      $s2, 12($sp)
	sw      $s3, 16($sp)
	sw      $s4, 20($sp)
	sw      $s5, 24($sp)
	sw      $s6, 28($sp)
	sw      $s7, 32($sp)

	move    $s0, $a3
	lw      $t0, 0($a2)                  # end
	lw      $t1, 4($a2)
	lw      $t2, 8($a2)
	li      $t3, 1
	li      $t4, Q_FIX_ONE
	sw      $zero, TR_STARTSOLID($s0)
	sw      $t3, TR_ALLSOLID($s0)
	sw      $zero, TR_INOPEN($s0)
	sw      $zero, TR_INWATER($s0)
	sw      $t4, TR_FRACTION($s0)
	sw      $t0, TR_ENDPOS+0($s0)
	sw      $t1, TR_ENDPOS+4($s0)
	sw      $t2, TR_ENDPOS+8($s0)
	sw      $zero, TR_PLANE_N+0($s0)
	sw      $zero, TR_PLANE_N+4($s0)
	sw      $zero, TR_PLANE_D($s0)

	lui     $t5, %hi(world)
	addiu   $t5, $t5, %lo(world)
	lw      $s1, W_CLIPNODES($t5)
	lw      $s2, W_PLANES($t5)
	bnez    $s1, 1f
	li      $t6, 2
	sw      $zero, TR_ALLSOLID($s0)      # no clip hulls: a free move
	b       ct_ret
	sw      $t3, TR_INOPEN($s0)
1:	bne     $a0, $t6, 2f
	lw      $s3, W_HULL1($t5)
	lw      $s3, W_HULL2($t5)
2:
	# start / delta to the scratchpad and into GTE rows 1-2
	lui     $s7, 0x1F80
	lw      $t3, 0($a1)                  # start
	lw      $t4, 4($a1)
	lw      $t5, 8($a1)
	subu    $t0, $t0, $t3                # delta = end - start
	subu    $t1, $t1, $t4
	subu    $t2, $t2, $t5
	sw      $t3, PS_START+0($s7)
	sw      $t4, PS_START+4($s7)
	sw      $t5, PS_START+8($s7)
	sw      $t0, PS_DELTA+0($s7)
	sw      $t1, PS_DELTA+4($s7)
	sw      $t2, PS_DELTA+8($s7)
	sll     $t6, $t3, 12                 # x4096: n.p for axial planes
	sll     $t7, $t4, 12
	sll     $t8, $t5, 12
	sw      $t6, PS_S4K+0($s7)
	sw      $t7, PS_S4K+4($s7)
	sw      $t8, PS_S4K+8($s7)
	sll     $t6, $t0, 12
	sll     $t7, $t1, 12
	sll     $t8, $t2, 12
	sw      $t6, PS_D4K+0($s7)
	sw      $t7, PS_D4K+4($s7)
	sw      $t8, PS_D4K+8($s7)
	andi    $t3, $t3, 0xffff             # R11R12 = sx | sy << 16
	sll     $t4, $t4, 16
	or      $t3, $t3, $t4
	ctc2    $t3, C2_R11R12
	andi    $t5, $t5, 0xffff             # R13R21 = sz | dx << 16
	sll     $t0, $t0, 16
	or      $t5, $t5, $t0
	ctc2    $t5, C2_R13R21
	andi    $t1, $t1, 0xffff             # R22R23 = dy | dz << 16
	sll     $t2, $t2, 16
	or      $t1, $t1, $t2
	ctc2    $t1, C2_R22R23

	move    $s4, $zero                   # f1
	lui     $fp, 1                       # F_ONE
	lui     $s5, 1                       # f2 = F_ONE
	move    $s6, $zero
	b       ct_next
	move    $t9, $s3

# ---- interior node ------------------------------------------------------------
ct_node:                                 # t0 = t9 << 3 (ct_next's delay slot)
	NODE_DSDD 0
	beqz    $s4, 1f
	move    $v0, $t5                     # t1 = ds when f1 = 0
	FRAC_MUL $t7, $t6, $s4
	addu    $v0, $t5, $t7                # t1 = t(f1)
1:	beq     $s5, $fp, 2f
	addu    $v1, $t5, $t6                # t2 = ds + dd when f2 = F_ONE
	FRAC_MUL $t7, $t6, $s5
	addu    $v1, $t5, $t7                # t2 = t(f2)
2:	or      $t7, $v0, $v1
	bgez    $t7, ct_next                 # both in front: child 0
	move    $t9, $a2
	and     $t7, $v0, $v1
	bltz    $t7, ct_next                 # both behind: child 1
	move    $t9, $a3

	# the sweep crosses the plane: split DIST_EPS short of it, near side
	srl     $t8, $v0, 31                 # side = t1 < 0
	li      $a0, DIST_EPS
	beqz    $t8, 3f
	move    $a1, $t6                     # den = dd (non-zero here)
	li      $a0, -DIST_EPS
3:	subu    $a0, $a0, $t5                # num = +-DIST_EPS - ds
	bgez    $a1, 4f
	nop
	subu    $a0, $zero, $a0
	subu    $a1, $zero, $a1
4:	blez    $a0, 6f                      # fm <= 0
	move    $t7, $zero
	slt     $t4, $a0, $a1
	beqz    $t4, 6f                      # num >= den: fm = F_ONE
	lui     $t7, 1
	mtc2    $a1, C2_LZCS                 # shift so den fits 15 bits
	nop
	nop
	mfc2    $t4, C2_LZCR                 # leading zeros of den
	li      $t3, 17
	subu    $t3, $t3, $t4                # s = 17 - lzc
	bgez    $t3, 5f
	nop
	move    $t3, $zero
5:	srlv    $a0, $a0, $t3
	srlv    $a1, $a1, $t3
	sll     $a0, $a0, 16
	divu    $zero, $a0, $a1             # bare divu (2-operand form is a GAS macro)
	mflo    $t7                          # fm (stalls for the divide)
6:	slt     $t4, $t7, $s4                # clamp fm to [f1, f2]
	bnez    $t4, 7f
	slt     $t4, $s5, $t7
	beqz    $t4, 8f
	nop
	b       8f
	move    $t7, $s5
7:	move    $t7, $s4
8:	addiu   $sp, $sp, -16                # push the split (t9 already holds
	or      $t4, $t0, $t8                # a child: push the node pointer)
	sw      $t4, 0($sp)
	sw      $s4, 4($sp)
	sw      $t7, 8($sp)
	sw      $s5, 12($sp)
	addiu   $s6, $s6, 1
	move    $s5, $t7                     # near side: [f1, fm]
	beqz    $t8, ct_next
	move    $t9, $a2
	move    $t9, $a3
ct_next:
	bgez    $t9, ct_node
	sll     $t0, $t9, 2                  # ct_node's first step

# ---- leaf ---------------------------------------------------------------------
	li      $t0, QW_CONTENTS_SOLID
	beq     $t9, $t0, 2f
	li      $t1, 1
	li      $t0, QW_CONTENTS_EMPTY
	bne     $t9, $t0, 1f
	sw      $zero, TR_ALLSOLID($s0)
	b       3f
	sw      $t1, TR_INOPEN($s0)
1:	b       3f
	sw      $t1, TR_INWATER($s0)
2:	sw      $t1, TR_STARTSOLID($s0)
3:	beqz    $s6, ct_done                 # the whole sweep is open
	nop

	# the near side of the innermost split was open: its far side
	lw      $t4, 0($sp)
	lw      $t5, 4($sp)                  # f1
	lw      $t7, 8($sp)                  # fm
	lw      $t6, 12($sp)                 # f2
	addiu   $sp, $sp, 16
	addiu   $s6, $s6, -1
	andi    $t8, $t4, 1                  # side
	subu    $t4, $t4, $t8                # clipnode
	sw      $t4, PS_SNODE($s7)
	sw      $t8, PS_SSIDE($s7)
	sw      $t5, PS_SF1($s7)
	sw      $t7, PS_SFM($s7)
	sw      $t6, PS_SF2($s7)
	sll     $t0, $t8, 1
	subu    $t0, $t4, $t0
	lh      $t9, CN_CHILD+2($t0)         # far = child[side ^ 1]
	bal     fcontents                    # (t9, t7) -> v0
	nop
	li      $t0, QW_CONTENTS_SOLID
	beq     $v0, $t0, 9f
	lw      $t4, PS_SNODE($s7)
	lw      $t8, PS_SSIDE($s7)
	lw      $s4, PS_SFM($s7)             # far side: [fm, f2]
	lw      $s5, PS_SF2($s7)
	sll     $t0, $t8, 1
	subu    $t0, $t4, $t0
	lh      $t9, CN_CHILD+2($t0)
	b       ct_next
	nop
9:	lw      $t0, TR_ALLSOLID($s0)
	lw      $t8, PS_SSIDE($s7)
	bnez    $t0, ct_done                 # never got out of solid
	sll     $t0, $s6, 4

	# ---- impact: the far side is solid --------------------------------------
	addu    $sp, $sp, $t0                # drop the pending splits
	lhu     $t1, CN_PLANE3($t4)
	nop
	sll     $t1, $t1, 2
	addu    $t1, $s2, $t1
	lh      $t2, PL_N+0($t1)
	lh      $t3, PL_N+2($t1)
	lh      $t4, PL_N+4($t1)
	lw      $t5, PL_DIST($t1)
	beqz    $t8, 1f
	nop
	subu    $t2, $zero, $t2              # near side behind: flip the plane
	subu    $t3, $zero, $t3
	subu    $t4, $zero, $t4
	subu    $t5, $zero, $t5
1:	sh      $t2, TR_PLANE_N+0($s0)
	sh      $t3, TR_PLANE_N+2($s0)
	sh      $t4, TR_PLANE_N+4($s0)
	sw      $t5, TR_PLANE_D($s0)

	# back out of residual solid: step = (f2 - f1) / 10 + 1
	lw      $t0, PS_SF1($s7)
	lw      $t1, PS_SF2($s7)
	li      $t2, 10
	subu    $t1, $t1, $t0
	divu    $zero, $t1, $t2
	lw      $s4, PS_SFM($s7)             # s4 = fm
	move    $s6, $zero                   # s6 = guard
	mflo    $s5
	bal     set_endpos
	addiu   $s5, $s5, 1                  # s5 = step
ct_guard:
	move    $t9, $s3
	bal     pcontents                    # contents(root, endpos)
	addiu   $a0, $s0, TR_ENDPOS
	li      $t0, QW_CONTENTS_SOLID
	bne     $v0, $t0, 3f
	slti    $t1, $s6, 16
	beqz    $t1, 1f                      # fm = guard < 16 && fm > step
	slt     $t1, $s5, $s4                #      ? fm - step : 0
	beqz    $t1, 1f
	nop
	b       2f
	subu    $s4, $s4, $s5
1:	move    $s4, $zero
2:	bal     set_endpos
	addiu   $s6, $s6, 1
	bnez    $s4, ct_guard
	nop
3:	sra     $t0, $s4, 4                  # 16-bit -> Q_FIX fraction
	b       ct_ret
	sw      $t0, TR_FRACTION($s0)

ct_done:
	sll     $t0, $s6, 4
	addu    $sp, $sp, $t0                # drop any pending splits
ct_ret:
	lw      $v0, TR_FRACTION($s0)
	lw      $ra, 0($sp)
	lw      $s0, 4($sp)
	lw      $s1, 8($sp)
	lw      $s2, 12($sp)
	lw      $s3, 16($sp)
	lw      $s4, 20($sp)
	lw      $s5, 24($sp)
	lw      $s6, 28($sp)
	lw      $s7, 32($sp)
	lw      $fp, 36($sp)
	jr      $ra
	addiu   $sp, $sp, 40

# -----------------------------------------------------------------------------
# fcontents: v0 = contents of clipnode t9 at sweep fraction t7.
# Clobbers t0-t6, t9, a2, a3, v1, at.
fcontents:
	bltz    $t9, 9f
	sll     $t0, $t9, 2                  # (NODE_DSDD's first step)
1:	NODE_DSDD 0
	FRAC_MUL $t0, $t6, $t7
	addu    $t5, $t5, $t0                # t(f)
	bgez    $t5, 3f                      # front
	move    $t9, $a2
	move    $t9, $a3                     # back
3:	bgez    $t9, 1b
	sll     $t0, $t9, 2
9:	jr      $ra
	move    $v0, $t9

# pcontents: v0 = contents of clipnode t9 at the point a0 (GTE row 3 must
# hold the same point). Clobbers t0-t5, t9, a2, a3, v1.
pcontents:
	bltz    $t9, 9f
	sll     $t0, $t9, 2                  # (NODE_DP's first step)
1:	NODE_DP
	bgez    $t5, 3f                      # front
	move    $t9, $a2
	move    $t9, $a3                     # back
3:	bgez    $t9, 1b
	sll     $t0, $t9, 2
9:	jr      $ra
	move    $v0, $t9

# set_endpos: endpos = start + delta * s4 >> 16 per axis, rounded toward the
# open side of the plane just stored in the trace (toward the start where
# its normal is 0); GTE row 3 = endpos for pcontents.
# Clobbers t0-t7, v1, at.
set_endpos:
	move    $v1, $zero                   # axis * 4
1:	addu    $t0, $s7, $v1
	lw      $t1, PS_DELTA($t0)
	lw      $t2, PS_START($t0)
	srl     $t3, $v1, 1
	mult    $t1, $s4
	addu    $t3, $s0, $t3
	lh      $t3, TR_PLANE_N($t3)         # n[axis]
	mflo    $t4
	mfhi    $t5
	srl     $t6, $t4, 16
	sll     $t5, $t5, 16
	or      $t6, $t6, $t5                # floor(delta * f / 65536)
	andi    $t4, $t4, 0xffff
	beqz    $t4, 3f                      # exact
	nop
	bgtz    $t3, 2f                      # n > 0: round up
	nop
	bnez    $t3, 3f                      # n < 0: round down
	nop
	bgez    $t1, 3f                      # n = 0: toward the start
	nop
2:	addiu   $t6, $t6, 1
3:	addu    $t6, $t2, $t6
	addu    $t0, $s0, $v1
	sw      $t6, TR_ENDPOS($t0)
	addiu   $v1, $v1, 4
	li      $t7, 12
	bne     $v1, $t7, 1b
	nop
	lw      $t0, TR_ENDPOS+0($s0)        # GTE row 3 = endpos
	lw      $t1, TR_ENDPOS+4($s0)
	lw      $t2, TR_ENDPOS+8($s0)
	andi    $t0, $t0, 0xffff
	sll     $t1, $t1, 16
	or      $t0, $t0, $t1
	ctc2    $t0, C2_R31R32
	jr      $ra
	ctc2    $t2, C2_R33

	.size   clip_trace, . - clip_trace

# -----------------------------------------------------------------------------
	.section .text.clip_contents, "ax", @progbits
	.global clip_contents
	.type   clip_contents, @function

clip_contents:                           # a0 = hull, a1 = p
	addiu   $sp, $sp, -16
	sw      $ra, 0($sp)
	sw      $s1, 4($sp)
	sw      $s2, 8($sp)
	lui     $t5, %hi(world)
	addiu   $t5, $t5, %lo(world)
	lw      $s1, W_CLIPNODES($t5)
	lw      $s2, W_PLANES($t5)
	li      $v0, QW_CONTENTS_EMPTY
	beqz    $s1, 3f                      # no clip hulls
	li      $t6, 2
	bne     $a0, $t6, 1f
	lw      $t9, W_HULL1($t5)
	lw      $t9, W_HULL2($t5)
1:	lw      $t0, 0($a1)                  # GTE row 3 = p
	lw      $t1, 4($a1)
	lw      $t2, 8($a1)
	andi    $t0, $t0, 0xffff
	sll     $t1, $t1, 16
	or      $t0, $t0, $t1
	ctc2    $t0, C2_R31R32
	ctc2    $t2, C2_R33
	bal     pcontents
	move    $a0, $a1
3:	lw      $ra, 0($sp)
	lw      $s1, 4($sp)
	lw      $s2, 8($sp)
	jr      $ra
	addiu   $sp, $sp, 16

	.size   clip_contents, . - clip_contents
