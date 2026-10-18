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
# Hulls 1 and 2 (the player's sizes) are clipnodes: 6 bytes, int16 children
# (>= 0 a clipnode index, < 0 contents). Hull 0, the point hull (projectiles,
# shots), is the world's own BSP: 36-byte nodes with pointer children (bit 0
# set: a leaf, whose contents byte is read). The loop exists once per kind
# (the CT_LOOP macro), the planes come from the one table either way.
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
# NODE_DSDD h0: the node t9 (h0 = 0: a clipnode number, with t0 = t9 << 2
# done by the caller; h0 = 1: the node's address), s2 = planes, s7 =
# scratchpad (PS_S4K / PS_D4K = start / delta x 4096), GTE rows 1-2 = start /
# delta. Out: t0 = node pointer, a2 / a3 = children, t5 = ds, t6 = dd.
# Clobbers t1-t4, v1.
.macro NODE_DSDD h0
	.if \h0
	lhu     $t1, N_PLANE3($t9)
	lw      $a2, N_CHILD+0($t9)
	move    $t0, $t9
	.else
	sll     $t2, $t9, 1
	addu    $t0, $t0, $t2                # num * 6
	addu    $t0, $s1, $t0                # clipnode
	lhu     $t1, CN_PLANE3($t0)
	lh      $a2, CN_CHILD+0($t0)
	.endif
	sll     $t1, $t1, 2
	addu    $t1, $s2, $t1                # plane
	lbu     $t2, PL_TYPE($t1)
	lw      $t3, PL_DIST($t1)
	.if \h0
	lw      $a3, N_CHILD+4($t9)
	.else
	lh      $a3, CN_CHILD+2($t0)
	.endif
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

# NODE_DP h0: like NODE_DSDD for a point: a0 = point (int32[3]), GTE row 3 =
# the same point. Out: a2 / a3 = children, t5 = n.p - dist.
# Clobbers t0-t4, v1. h0 = 0: t0 = t9 << 2 is done by the caller.
.macro NODE_DP h0
	.if \h0
	lhu     $t1, N_PLANE3($t9)
	lw      $a2, N_CHILD+0($t9)
	.else
	sll     $t2, $t9, 1
	addu    $t0, $t0, $t2                # num * 6
	addu    $t0, $s1, $t0
	lhu     $t1, CN_PLANE3($t0)
	lh      $a2, CN_CHILD+0($t0)
	.endif
	sll     $t1, $t1, 2
	addu    $t1, $s2, $t1
	lbu     $t2, PL_TYPE($t1)
	lw      $t3, PL_DIST($t1)
	.if \h0
	lw      $a3, N_CHILD+4($t9)
	.else
	lh      $a3, CN_CHILD+2($t0)
	.endif
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

# LEAF h0, node: branch to `node` if t9 is a node; else t9 becomes its
# contents (hull 0: the leaf's byte). The delay slot does NODE_*'s first
# step (hull 0: nothing to do; it holds a nop).
.macro LEAF h0, node
	.if \h0
	andi    $t0, $t9, 1
	beqz    $t0, \node
	nop
	lb      $t9, L_CONTENTS-1($t9)       # (the pointer carries bit 0)
	.else
	bgez    $t9, \node
	sll     $t0, $t9, 2
	.endif
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

# -----------------------------------------------------------------------------
# CT_LOOP h0, p: the trace loop for one kind of hull, labels prefixed by p.
# Registers: s0 trace_t, s1 clipnodes, s2 planes, s3 root, s4 f1, s5 f2,
# fp F_ONE, s6 pending splits (16 B each on the stack: node pointer | side,
# f1, fm, f2), s7 scratchpad, t9 current node / contents.
.macro CT_LOOP h0, p
\p\()_node:
	NODE_DSDD \h0
	beqz    $s4, 1f
	move    $v0, $t5                     # t1 = ds when f1 = 0
	FRAC_MUL $t7, $t6, $s4
	addu    $v0, $t5, $t7                # t1 = t(f1)
1:	beq     $s5, $fp, 2f
	addu    $v1, $t5, $t6                # t2 = ds + dd when f2 = F_ONE
	FRAC_MUL $t7, $t6, $s5
	addu    $v1, $t5, $t7                # t2 = t(f2)
2:	or      $t7, $v0, $v1
	bgez    $t7, \p\()_next              # both in front: child 0
	move    $t9, $a2
	and     $t7, $v0, $v1
	bltz    $t7, \p\()_next              # both behind: child 1
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
	beqz    $t8, \p\()_next
	move    $t9, $a2
	move    $t9, $a3
\p\()_next:
	LEAF    \h0, \p\()_node

	# ---- leaf -----------------------------------------------------------
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
	subu    $t4, $t4, $t8                # the node
	sw      $t4, PS_SNODE($s7)
	sw      $t8, PS_SSIDE($s7)
	sw      $t5, PS_SF1($s7)
	sw      $t7, PS_SFM($s7)
	sw      $t6, PS_SF2($s7)
	.if \h0
	sll     $t0, $t8, 2
	subu    $t0, $t4, $t0
	lw      $t9, N_CHILD+4($t0)          # far = child[side ^ 1]
	.else
	sll     $t0, $t8, 1
	subu    $t0, $t4, $t0
	lh      $t9, CN_CHILD+2($t0)         # far = child[side ^ 1]
	.endif
	bal     \p\()_fcontents              # (t9, t7) -> v0
	nop
	li      $t0, QW_CONTENTS_SOLID
	beq     $v0, $t0, 9f
	lw      $t4, PS_SNODE($s7)
	lw      $t8, PS_SSIDE($s7)
	lw      $s4, PS_SFM($s7)             # far side: [fm, f2]
	lw      $s5, PS_SF2($s7)
	.if \h0
	sll     $t0, $t8, 2
	subu    $t0, $t4, $t0
	lw      $t9, N_CHILD+4($t0)
	.else
	sll     $t0, $t8, 1
	subu    $t0, $t4, $t0
	lh      $t9, CN_CHILD+2($t0)
	.endif
	b       \p\()_next
	nop
9:	lw      $t0, TR_ALLSOLID($s0)
	lw      $t8, PS_SSIDE($s7)
	bnez    $t0, ct_done                 # never got out of solid
	sll     $t0, $s6, 4

	# ---- impact: the far side is solid --------------------------------------
	addu    $sp, $sp, $t0                # drop the pending splits
	.if \h0
	lhu     $t1, N_PLANE3($t4)
	.else
	lhu     $t1, CN_PLANE3($t4)
	.endif
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
\p\()_guard:
	move    $t9, $s3
	bal     \p\()_pcontents              # contents(root, endpos)
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
	bnez    $s4, \p\()_guard
	nop
3:	sra     $t0, $s4, 4                  # 16-bit -> Q_FIX fraction
	b       ct_core_ret
	sw      $t0, TR_FRACTION($s0)

# \p\()_fcontents: v0 = contents of node t9 at sweep fraction t7.
# Clobbers t0-t6, t9, a2, a3, v1, at.
\p\()_fcontents:
	LEAF    \h0, 1f
	b       9f
	nop
1:	NODE_DSDD \h0
	FRAC_MUL $t0, $t6, $t7
	addu    $t5, $t5, $t0                # t(f)
	bgez    $t5, 3f                      # front
	move    $t9, $a2
	move    $t9, $a3                     # back
3:	LEAF    \h0, 1b
9:	jr      $ra
	move    $v0, $t9

# \p\()_pcontents: v0 = contents of node t9 at the point a0 (GTE row 3
# must hold the same point). Clobbers t0-t5, t9, a2, a3, v1.
\p\()_pcontents:
	LEAF    \h0, 1f
	b       9f
	nop
1:	NODE_DP \h0
	bgez    $t5, 3f                      # front
	move    $t9, $a2
	move    $t9, $a3                     # back
3:	LEAF    \h0, 1b
9:	jr      $ra
	move    $v0, $t9
.endm

	.section .text.clip_trace, "ax", @progbits
	.global clip_trace
	.type   clip_trace, @function

# -----------------------------------------------------------------------------
# clip_trace(hull, start, end, tr): SV_Move. The world's hull first
# (ct_core), then (SV_ClipToLinks) every brush model of clip_bmodels whose
# box the swept box reaches (SV_MoveBounds: the hull's box around the
# sweep, a unit wider): the sweep in the model's space through ct_core into
# a scratch trace, its end point put back in the world, and taken when it
# is hit first or the sweep starts inside it. Mirrors engine/clip.c.
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
	lui     $s7, 0x1F80
	lui     $t5, %hi(world)
	addiu   $t5, $t5, %lo(world)
	lw      $s1, W_CLIPNODES($t5)
	lw      $s2, W_PLANES($t5)
	lw      $s3, W_ROOT($t5)
	sw      $a0, PS_HULL($s7)
	sw      $a1, PS_WSTART($s7)
	sw      $a2, PS_WEND($s7)
	sw      $a3, PS_WTR($s7)
	bnez    $s1, 1f
	li      $t6, 2
	lw      $t0, 0($a2)                  # no clip hulls: a free move
	lw      $t1, 4($a2)
	lw      $t2, 8($a2)
	li      $t3, 1
	li      $t4, Q_FIX_ONE
	sw      $zero, TR_STARTSOLID($s0)
	sw      $zero, TR_ALLSOLID($s0)
	sw      $t3, TR_INOPEN($s0)
	sw      $zero, TR_INWATER($s0)
	sw      $t4, TR_FRACTION($s0)
	sw      $t0, TR_ENDPOS+0($s0)
	sw      $t1, TR_ENDPOS+4($s0)
	sw      $t2, TR_ENDPOS+8($s0)
	sw      $zero, TR_PLANE_N+0($s0)
	sw      $zero, TR_PLANE_N+4($s0)
	b       ct_ret
	sw      $zero, TR_PLANE_D($s0)
1:	beqz    $a0, 2f                      # hull 0: the world's BSP, from its
	sll     $s3, $s3, 8                  # root as a KUSEG pointer (like
	bne     $a0, $t6, 3f                 # its children)
	lw      $s3, W_HULL1($t5)
	lw      $s3, W_HULL2($t5)
	b       3f
	nop
2:	srl     $s3, $s3, 8
3:	bal     ct_core                      # the world
	nop

	# ---- the brush models ---------------------------------------------------
	lui     $t5, %hi(clip_num_bmodels)
	lw      $t5, %lo(clip_num_bmodels)($t5)
	lw      $a1, PS_WSTART($s7)
	beqz    $t5, ct_ret
	lw      $a2, PS_WEND($s7)
	lw      $a0, PS_HULL($s7)
	lui     $t4, %hi(clip_bmodels)
	addiu   $t4, $t4, %lo(clip_bmodels)
	sll     $t6, $t5, 3                  # n * 40 = n * 8 + n * 32
	sll     $t5, $t5, 5
	addu    $t5, $t5, $t6
	addu    $t5, $t5, $t4
	sw      $t4, PS_BMI($s7)
	sw      $t5, PS_BMEND($s7)
	sw      $zero, PS_BMIDX($s7)
	# the swept box: min / max of start and end, the hull's box around
	# it, a unit wider (ct_hull_box: 6 words per hull)
	sll     $t6, $a0, 3
	sll     $t7, $a0, 4
	addu    $t6, $t6, $t7                # hull * 24
	lui     $t7, %hi(ct_hull_box)
	addiu   $t7, $t7, %lo(ct_hull_box)
	addu    $t6, $t6, $t7
	move    $t8, $zero                   # axis * 4
4:	addu    $t0, $a1, $t8
	addu    $t1, $a2, $t8
	lw      $t2, 0($t0)                  # start[k]
	lw      $t3, 0($t1)                  # end[k]
	addu    $t7, $t6, $t8
	lw      $t0, 0($t7)                  # the hull's mins[k]
	lw      $t1, 12($t7)                 # maxs[k]
	slt     $v0, $t3, $t2                # end < start?
	beqz    $v0, 5f
	move    $v1, $t2                     # lo = start, hi = end
	move    $v1, $t3                     # lo = end, hi = start
	move    $t3, $t2
5:	addu    $v1, $v1, $t0
	addu    $t3, $t3, $t1
	addiu   $v1, $v1, -CLIP_SUB
	addiu   $t3, $t3, CLIP_SUB
	addu    $t9, $s7, $t8
	sw      $v1, PS_BMIN($t9)
	sw      $t3, PS_BMAX($t9)
	addiu   $t8, $t8, 4
	li      $t0, 12
	bne     $t8, $t0, 4b
	nop

ct_bm_reload:                            # the swept box in registers (ct_core
	lw      $s4, PS_BMIN+0($s7)          # takes them)
	lw      $s5, PS_BMIN+4($s7)
	lw      $s6, PS_BMIN+8($s7)
	lw      $fp, PS_BMAX+0($s7)
	lw      $v1, PS_BMAX+4($s7)
	lw      $a3, PS_BMAX+8($s7)
ct_bm_next:
	lw      $t4, PS_BMI($s7)
	lw      $t5, PS_BMEND($s7)
	lw      $t6, PS_BMIDX($s7)
	beq     $t4, $t5, ct_ret
	addiu   $t5, $t4, CB_SIZE
	sw      $t5, PS_BMI($s7)
	addiu   $t6, $t6, 1
	sw      $t6, PS_BMIDX($s7)
	lw      $t0, CB_ABSMAX+0($t4)        # the boxes apart on an axis: next
	lw      $t1, CB_ABSMIN+0($t4)
	slt     $t0, $t0, $s4                # absmax.x < bmin.x
	bnez    $t0, ct_bm_next
	slt     $t1, $fp, $t1                # bmax.x < absmin.x
	bnez    $t1, ct_bm_next
	lw      $t0, CB_ABSMAX+4($t4)
	lw      $t1, CB_ABSMIN+4($t4)
	slt     $t0, $t0, $s5
	bnez    $t0, ct_bm_next
	slt     $t1, $v1, $t1
	bnez    $t1, ct_bm_next
	lw      $t0, CB_ABSMAX+8($t4)
	lw      $t1, CB_ABSMIN+8($t4)
	slt     $t0, $t0, $s6
	bnez    $t0, ct_bm_next
	slt     $t1, $a3, $t1
	bnez    $t1, ct_bm_next
	lw      $t6, CB_BM($t4)              # the qw_bmodel
	lw      $a1, PS_WSTART($s7)          # the sweep in the model's space
	lw      $a2, PS_WEND($s7)
	move    $t8, $zero
7:	addu    $t0, $t4, $t8
	lw      $t1, CB_ORG($t0)
	addu    $t2, $a1, $t8
	lw      $t3, 0($t2)
	addu    $t2, $a2, $t8
	lw      $t7, 0($t2)
	addu    $t9, $s7, $t8
	subu    $t3, $t3, $t1
	sw      $t3, PS_BSTART($t9)
	subu    $t7, $t7, $t1
	sw      $t7, PS_BEND($t9)
	addiu   $t8, $t8, 4
	li      $t0, 12
	bne     $t8, $t0, 7b
	nop
	lw      $a0, PS_HULL($s7)            # the model's hull of that size
	beqz    $t6, ct_box                  # none: a SOLID_BBOX entity's box
	li      $t1, 2
	lw      $s3, BM_HULL1($t6)
	beqz    $a0, 8f
	lw      $t0, BM_HULL2($t6)
	beq     $a0, $t1, 9f
	nop
	b       ct_bm_trace
	nop
8:	lw      $s3, BM_ROOT($t6)            # hull 0: its render nodes (KUSEG)
	b       ct_bm_trace
	nop
9:	move    $s3, $t0
ct_bm_trace:
	addiu   $s0, $s7, PS_BTRACE
	addiu   $a1, $s7, PS_BSTART
	bal     ct_core
	addiu   $a2, $s7, PS_BEND
ct_bm_done:
	lw      $t4, PS_BMI($s7)
	lw      $s0, PS_WTR($s7)             # the call's trace again
	addiu   $t4, $t4, -CB_SIZE           # the model's entry
	lw      $t0, CB_ORG+0($t4)           # its end point back in the world
	lw      $t1, CB_ORG+4($t4)
	lw      $t2, CB_ORG+8($t4)
	lw      $t5, PS_BTRACE+TR_ENDPOS+0($s7)
	lw      $t6, PS_BTRACE+TR_ENDPOS+4($s7)
	lw      $t7, PS_BTRACE+TR_ENDPOS+8($s7)
	addu    $t5, $t5, $t0
	addu    $t6, $t6, $t1
	addu    $t7, $t7, $t2
	sw      $t5, PS_BTRACE+TR_ENDPOS+0($s7)
	sw      $t6, PS_BTRACE+TR_ENDPOS+4($s7)
	sw      $t7, PS_BTRACE+TR_ENDPOS+8($s7)
	lw      $t0, PS_BTRACE+TR_ALLSOLID($s7)
	lw      $t1, PS_BTRACE+TR_STARTSOLID($s7)
	lw      $t2, PS_BTRACE+TR_FRACTION($s7)
	lw      $t3, TR_FRACTION($s0)
	or      $t0, $t0, $t1
	bnez    $t0, 1f                      # all solid or starting inside: taken
	slt     $t2, $t2, $t3
	beqz    $t2, 2f                      # not hit first
	nop
1:	lw      $t5, TR_STARTSOLID($s0)      # (kept set)
	lw      $t0, PS_BTRACE+0($s7)
	lw      $t1, PS_BTRACE+4($s7)
	lw      $t2, PS_BTRACE+8($s7)
	lw      $t3, PS_BTRACE+12($s7)
	or      $t0, $t0, $t5
	sw      $t0, 0($s0)
	sw      $t1, 4($s0)
	sw      $t2, 8($s0)
	sw      $t3, 12($s0)
	lw      $t0, PS_BTRACE+16($s7)
	lw      $t1, PS_BTRACE+20($s7)
	lw      $t2, PS_BTRACE+24($s7)
	lw      $t3, PS_BTRACE+28($s7)
	sw      $t0, 16($s0)
	sw      $t1, 20($s0)
	sw      $t2, 24($s0)
	sw      $t3, 28($s0)
	lw      $t0, PS_BTRACE+32($s7)
	lw      $t1, PS_BTRACE+36($s7)
	lw      $t2, PS_BTRACE+40($s7)
	lw      $t3, PS_BMIDX($s7)           # the model's index + 1: what was hit
	sw      $t0, 32($s0)
	sw      $t1, 36($s0)
	sw      $t2, 40($s0)
	b       ct_bm_reload
	sh      $t3, TR_ENT($s0)
2:	beqz    $t1, ct_bm_reload            # not even starting inside it
	li      $t0, 1
	b       ct_bm_reload
	sw      $t0, TR_STARTSOLID($s0)

# ct_box: SV_HullForBox. The entry's box (absmin / absmax less its origin)
# grown by the mover's (hullmins = mins - the hull's maxs, hullmaxs = maxs
# - the hull's mins) as the 6 axial planes' distances of the box hull
# (ct_box_nodes), traced as a clip hull from its first node, the world's
# tables back after. t4 = the entry, a0 = the call's hull.
ct_box:
	sll     $t6, $a0, 3
	sll     $t7, $a0, 4
	addu    $t6, $t6, $t7                # hull * 24
	lui     $t7, %hi(ct_hull_box)
	addiu   $t7, $t7, %lo(ct_hull_box)
	addu    $t6, $t6, $t7                # the hull's box
	lui     $t9, %hi(ct_box_planes)
	addiu   $t9, $t9, %lo(ct_box_planes)
	move    $t8, $zero                   # axis * 4
1:	addu    $t0, $t4, $t8
	lw      $t1, CB_ORG($t0)
	lw      $t2, CB_ABSMAX($t0)
	lw      $t3, CB_ABSMIN($t0)
	addu    $t0, $t6, $t8
	lw      $t5, 0($t0)                  # the hull's mins[k]
	lw      $t7, 12($t0)                 # maxs[k]
	subu    $t2, $t2, $t1
	subu    $t2, $t2, $t5                # maxs - the hull's mins
	subu    $t3, $t3, $t1
	subu    $t3, $t3, $t7                # mins - the hull's maxs
	sll     $t2, $t2, 12
	sll     $t3, $t3, 12
	sw      $t2, PL_DIST($t9)            # plane 2k: the max side
	sw      $t3, 12+PL_DIST($t9)         # plane 2k + 1: the min side
	addiu   $t8, $t8, 4
	li      $t0, 12
	bne     $t8, $t0, 1b
	addiu   $t9, $t9, 24
	lui     $s1, %hi(ct_box_nodes)
	addiu   $s1, $s1, %lo(ct_box_nodes)
	lui     $s2, %hi(ct_box_planes)
	addiu   $s2, $s2, %lo(ct_box_planes)
	move    $s3, $zero                   # its first node
	li      $a0, 1                       # (a clip hull)
	addiu   $s0, $s7, PS_BTRACE
	addiu   $a1, $s7, PS_BSTART
	bal     ct_core
	addiu   $a2, $s7, PS_BEND
	lui     $t5, %hi(world)              # the world's tables back
	addiu   $t5, $t5, %lo(world)
	lw      $s1, W_CLIPNODES($t5)
	b       ct_bm_done
	lw      $s2, W_PLANES($t5)

# ct_core: one hull from one root. s0 = the trace_t to fill, s3 = the root
# (hull 0: a node pointer), a0 = hull, a1 = start, a2 = end, s1 / s2 / s7
# set. Returns through PS_CORERA (the loop's bal's take ra). Clobbers
# everything but s0-s3, s7.
ct_core:
	sw      $ra, PS_CORERA($s7)
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
	sw      $zero, TR_PLANE_N+4($s0)     # (n[2], and ent: the world)
	sw      $zero, TR_PLANE_D($s0)

	# start / delta to the scratchpad and into GTE rows 1-2
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
	beqz    $a0, c0_next
	move    $t9, $s3
	b       ct_next
	nop

	CT_LOOP 0, ct
	CT_LOOP 1, c0

ct_done:
	sll     $t0, $s6, 4
	addu    $sp, $sp, $t0                # drop any pending splits
ct_core_ret:
	lw      $ra, PS_CORERA($s7)
	nop
	jr      $ra
	nop

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

	.section .rodata.ct_hull_box, "a", @progbits
	.align  2
ct_hull_box:                             # the hulls' mins[3], maxs[3], 1/8 units (clip.c hull_box)
	.word   0, 0, 0, 0, 0, 0
	.word   -16 * CLIP_SUB, -16 * CLIP_SUB, -24 * CLIP_SUB, 16 * CLIP_SUB, 16 * CLIP_SUB, 32 * CLIP_SUB
	.word   -32 * CLIP_SUB, -32 * CLIP_SUB, -24 * CLIP_SUB, 32 * CLIP_SUB, 32 * CLIP_SUB, 64 * CLIP_SUB
ct_box_nodes:                            # SV_InitBoxHull: node i on plane i (plane3 =
	.hword  0,  QW_CONTENTS_EMPTY, 1     # i x 3), its outer side empty, the
	.hword  3,  2, QW_CONTENTS_EMPTY     # inner one the next node (the last:
	.hword  6,  QW_CONTENTS_EMPTY, 3     # solid)
	.hword  9,  4, QW_CONTENTS_EMPTY
	.hword  12, QW_CONTENTS_EMPTY, 5
	.hword  15, QW_CONTENTS_SOLID, QW_CONTENTS_EMPTY

	.section .data.ct_box_planes, "aw", @progbits
	.align  2
ct_box_planes:                           # qw_plane[6]: the normal, the axis (type), the
	.hword  4096, 0, 0                   # distance (set per box, x4096)
	.byte   0, 0
	.word   0
	.hword  4096, 0, 0
	.byte   0, 0
	.word   0
	.hword  0, 4096, 0
	.byte   1, 0
	.word   0
	.hword  0, 4096, 0
	.byte   1, 0
	.word   0
	.hword  0, 0, 4096
	.byte   2, 0
	.word   0
	.hword  0, 0, 4096
	.byte   2, 0
	.word   0

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
	lw      $t9, W_ROOT($t5)
	li      $v0, QW_CONTENTS_EMPTY
	beqz    $s1, 3f                      # no clip hulls
	li      $t6, 2
	lw      $t0, 0($a1)                  # GTE row 3 = p
	lw      $t1, 4($a1)
	lw      $t2, 8($a1)
	andi    $t0, $t0, 0xffff
	sll     $t1, $t1, 16
	or      $t0, $t0, $t1
	ctc2    $t0, C2_R31R32
	ctc2    $t2, C2_R33
	beqz    $a0, 2f                      # hull 0: the world's BSP
	sll     $t9, $t9, 8                  # (its root as a KUSEG pointer)
	bne     $a0, $t6, 1f
	lw      $t9, W_HULL1($t5)
	lw      $t9, W_HULL2($t5)
1:	bal     ct_pcontents
	move    $a0, $a1
	b       3f
	nop
2:	srl     $t9, $t9, 8
	bal     c0_pcontents
	move    $a0, $a1
3:	lw      $ra, 0($sp)
	lw      $s1, 4($sp)
	lw      $s2, 8($sp)
	jr      $ra
	addiu   $sp, $sp, 16

	.size   clip_contents, . - clip_contents
