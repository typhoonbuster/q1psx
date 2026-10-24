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
# \p\()_root: the walk down from the root before any split (f1 = 0, f2 =
# F_ONE: t1 = ds, t2 = ds + dd), v0 the node. It finds the guard's node
# (PS_GNODE, the residual-solid backing out's contents walk): the first
# node whose plane the sweep does not clear by GUARD_M on its side, else
# the first split. Every plane above it leaves every point within a
# sub-unit of the sweep (an endpos: set_endpos rounds) on the sweep's
# side, so the walk from there meets the root's (clip.c walks from the
# root; the bench compares them).
\p\()_root:
	LEAF    \h0, 1f
	b       \p\()_leaf
	nop
1:	move    $v0, $t9
	NODE_DSDD \h0
	addu    $v1, $t5, $t6                # t2 = ds + dd
	or      $t7, $t5, $v1
	bltz    $t7, 3f                      # not both in front
	addiu   $t7, $t5, -GUARD_M           # (delay)
	addiu   $t8, $v1, -GUARD_M
	or      $t7, $t7, $t8
	bgez    $t7, \p\()_root             # in front, clear: child 0
	move    $t9, $a2
	b       5f                           # in front but close: the guard's node
	nop
3:	and     $t7, $t5, $v1
	bgez    $t7, 6f                      # across: the first split
	addiu   $t7, $t5, GUARD_M            # (delay)
	addiu   $t8, $v1, GUARD_M
	and     $t7, $t7, $t8
	bltz    $t7, \p\()_root             # behind, clear: child 1
	move    $t9, $a3
5:	b       \p\()_next                  # (on with the child, the usual loop)
	sw      $v0, PS_GNODE($s7)
6:	sw      $v0, PS_GNODE($s7)
	b       \p\()_split
	move    $v0, $t5                     # t1 = ds

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
\p\()_split:
	srl     $t8, $v0, 31                 # side = t1 < 0
	li      $a0, DIST_EPS
	beqz    $t8, 3f
	move    $a1, $t6                     # den = dd (non-zero here)
	li      $a0, -DIST_EPS
3:	subu    $a0, $a0, $t5                # num = +-DIST_EPS - ds
	bgez    $a1, 4f
	move    $t7, $zero                   # (fm = 0, for 6f)
	subu    $a0, $zero, $a0
	subu    $a1, $zero, $a1
4:	blez    $a0, 6f                      # fm <= 0
	slt     $t4, $a0, $a1                # (6f sets t4 again)
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
	beqz    $t4, 81f
	addiu   $sp, $sp, -16                # (the push's first, below)
	b       81f
	move    $t7, $s5
7:	move    $t7, $s4
	addiu   $sp, $sp, -16                # push the split (t9 already holds
81:	or      $t4, $t0, $t8                # a child: push the node pointer)
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
\p\()_leaf:

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
	lw      $t4, 0($sp)                  # (the split's, below; ct_done leaves t4)

	# the near side of the innermost split was open: its far side
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
	lw      $t0, PS_SIGHT($s7)
	nop
	bnez    $t0, 3f                      # a line of sight: the impact as found
	nop
\p\()_guard:
	lw      $t9, PS_GNODE($s7)
	bal     \p\()_pcontents              # contents(root, endpos), from the
	addiu   $a0, $s0, TR_ENDPOS          # guard's node
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
	lui     $s7, 0x1F80
	sll     $t6, $a0, 3                  # the mover: its hull's box (hull x 24)
	sll     $t7, $a0, 4
	addu    $t6, $t6, $t7
	lui     $t7, %hi(ct_hull_box)
	addiu   $t7, $t7, %lo(ct_hull_box)
	addu    $t6, $t6, $t7
	sw      $t6, PS_MBOX($s7)
	sw      $t6, PS_MBOX2($s7)
	sw      $zero, PS_ROOT0($s7)
	sw      $zero, PS_SIGHT($s7)
	lui     $t8, %hi(clip_bmodels)       # entry 0 passed: the player's box is
	addiu   $t8, $t8, %lo(clip_bmodels)  # its own
	sw      $t8, PS_PASS($s7)
	b       ct_body
	sw      $zero, PS_NOMON($s7)

# clip_trace_ent(hull, start, end, tr): the same for clip_mover (clip.h):
# its own box, every entry from the player's (0) but its pass one, the
# boxes left out with nomonsters, MOVE_MISSILE's box for the bounds and
# the monsters' boxes with missile.
	.global clip_trace_ent
	.type   clip_trace_ent, @function
clip_trace_ent:
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
	lui     $s7, 0x1F80
	lui     $t6, %hi(clip_mover)
	addiu   $t6, $t6, %lo(clip_mover)
	lw      $t7, MV_PASS($t6)
	lw      $t8, MV_NOMON($t6)
	lw      $t9, MV_MISSILE($t6)
	lw      $t0, MV_ROOT0($t6)           # (one sweep's)
	lw      $t1, MV_SIGHT($t6)           # (one sweep's)
	sw      $zero, MV_ROOT0($t6)
	sw      $zero, MV_SIGHT($t6)
	sw      $t0, PS_ROOT0($s7)
	sw      $t1, PS_SIGHT($s7)
	sw      $t6, PS_MBOX($s7)
	sw      $t8, PS_NOMON($s7)
	beqz    $t9, 1f
	sw      $t6, PS_MBOX2($s7)
	lui     $t9, %hi(ct_missile_box)     # MOVE_MISSILE
	addiu   $t9, $t9, %lo(ct_missile_box)
	sw      $t9, PS_MBOX2($s7)
1:	beqz    $t7, ct_body                 # the pass entry (index + 1) as its
	sw      $zero, PS_PASS($s7)          # address (none: 0)
	sll     $t0, $t7, 5                  # x CB_SIZE: 48 = 32 + 16
	sll     $t1, $t7, 4
	addu    $t0, $t0, $t1
	lui     $t1, %hi(clip_bmodels - CB_SIZE)
	addiu   $t1, $t1, %lo(clip_bmodels - CB_SIZE)
	addu    $t0, $t0, $t1
	sw      $t0, PS_PASS($s7)

ct_body:
	move    $s0, $a3
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
2:	lw      $t0, PS_ROOT0($s7)           # (a node given: from there)
	srl     $s3, $s3, 8
	beqz    $t0, 3f
	nop
	move    $s3, $t0
3:	bal     ct_core                      # the world
	nop

	# ---- the brush models: SV_ClipToLinks, over the area nodes ----------------
	lui     $t5, %hi(clip_num_bmodels)
	lw      $t5, %lo(clip_num_bmodels)($t5)
	lw      $a1, PS_WSTART($s7)
	beqz    $t5, ct_ret
	lw      $a2, PS_WEND($s7)
	addiu   $t0, $s7, PS_ASTK + 4        # the nodes to walk: the root
	sw      $zero, PS_ASTK($s7)
	sw      $t0, PS_ASP($s7)
	sh      $zero, PS_ZERO($s7)          # an empty list to start from: on to
	addiu   $t4, $s7, PS_ZERO - CB_NEXT  # the root's
	sw      $t4, PS_BMI($s7)
	# the swept box: min / max of start and end (the world's sweep's, still
	# in the scratchpad: start, delta), the mover's box around it
	# (MOVE_MISSILE's), a unit wider (PS_MBOX2: mins[3], maxs[3])
	lw      $t6, PS_MBOX2($s7)
	.irp    k, 0, 4, 8
	lw      $t2, PS_START+\k($s7)       # start[k]
	lw      $t3, PS_DELTA+\k($s7)       # delta[k]
	lw      $t0, \k($t6)                # the hull's mins[k]
	lw      $t1, 12+\k($t6)             # maxs[k]
	bgez    $t3, 1f
	addu    $t3, $t2, $t3                # (delay) end[k]: lo = start, hi = end
	move    $v1, $t3                     # lo = end, hi = start
	b       2f
	move    $t3, $t2
1:	move    $v1, $t2
2:	addu    $v1, $v1, $t0
	addu    $t3, $t3, $t1
	addiu   $v1, $v1, -CLIP_SUB
	addiu   $t3, $t3, CLIP_SUB
	sw      $v1, PS_BMIN+\k($s7)
	sw      $t3, PS_BMAX+\k($s7)
	sw      $v1, PS_FBMIN+\k($s7)
	sw      $t3, PS_FBMAX+\k($s7)
	.endr
ct_bm_sight:                             # a line of sight: its entries' box
	lw      $t0, PS_SIGHT($s7)           # (ct_bm_box)
	nop
	beqz    $t0, ct_bm_reload
	nop
	bal     ct_bm_box
	nop

ct_bm_reload:                            # the swept box in registers (ct_core
	lw      $s4, PS_BMIN+0($s7)          # takes them), the loop's state too:
	lw      $s5, PS_BMIN+4($s7)          # t4 the entry last tried, t5 the
	lw      $s6, PS_BMIN+8($s7)          # table less an entry (the links'
	lw      $fp, PS_BMAX+0($s7)          # base), t7 the pass entry, t8
	lw      $v1, PS_BMAX+4($s7)          # MOVE_NOMONSTERS
	lw      $a3, PS_BMAX+8($s7)
	lw      $t4, PS_BMI($s7)
	lui     $t5, %hi(clip_bmodels - CB_SIZE)
	addiu   $t5, $t5, %lo(clip_bmodels - CB_SIZE)
	lw      $t7, PS_PASS($s7)
	lw      $t8, PS_NOMON($s7)
ct_bm_next:                              # the area's next entry
	lhu     $t0, CB_NEXT($t4)
	nop
	beqz    $t0, ct_bm_area              # the list's end: the next area node
	addu    $t4, $t5, $t0                # (delay) the entry
	lw      $t0, CB_ABSMAX+0($t4)        # the boxes apart on an axis: next (the
	lw      $t1, CB_ABSMIN+0($t4)        # cheap test first, all in registers)
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
	lw      $t2, CB_BM($t4)              # (delay: the entry's model)
	beq     $t7, $t4, ct_bm_next         # the mover's own entry: next
	sltiu   $t0, $t2, 2                  # a box (bm 0, or 1: a monster's) ...
	and     $t0, $t0, $t8                # ... in a MOVE_NOMONSTERS trace: next
	bnez    $t0, ct_bm_next
	lbu     $t6, CB_SELF($t4)            # (delay) a candidate: where the loop
	sw      $t4, PS_BMI($s7)             # goes on, its index + 1 (what is hit)
	sw      $t6, PS_BMIDX($s7)
	b       ct_bm_take
	move    $t6, $t2                     # the qw_bmodel

# ct_bm_area: the next area node to walk (Quake's walk is depth first: a
# node's list, then its children[0]'s subtree, then children[1]'s: a stack
# of the nodes reached, children[0] pushed last), the children the swept
# box reaches pushed, its list's head as t4's next
ct_bm_area:
	lw      $t3, PS_ASP($s7)
	addiu   $t0, $s7, PS_ASTK
	beq     $t3, $t0, ct_ret             # none left
	lw      $t2, -4($t3)                 # (delay) pop: the node
	addiu   $t3, $t3, -4
	sll     $t1, $t2, 3                  # x CA_SIZE (8)
	lui     $t0, %hi(clip_areas)
	addiu   $t0, $t0, %lo(clip_areas)
	addu    $t1, $t1, $t0
	lw      $t0, 0($t1)                  # dist | flags << 16 | children[1] << 24
	nop
	sll     $a0, $t0, 8
	sra     $a0, $a0, 24                 # flags
	bltz    $a0, 3f                      # a leaf: no children
	sll     $a1, $t0, 16                 # (delay)
	sra     $a1, $a1, 16                 # dist
	srl     $a2, $t0, 24                 # children[1]
	andi    $t6, $a0, 1                  # the whole swept box against the
	sll     $t6, $t6, 2                  # split, on its axis
	addu    $t6, $s7, $t6
	lw      $t0, PS_FBMIN($t6)
	lw      $t6, PS_FBMAX($t6)
	addiu   $t2, $t2, 1                  # children[0], the next node
	slt     $t0, $t0, $a1                #   bmin < dist: children[1]
	slt     $t6, $a1, $t6                #   bmax > dist: children[0]
	beqz    $t0, 5f                      # pushed, when reached and holding
	andi    $t9, $a0, 4                  # entries: children[1], then
	beqz    $t9, 5f                      # children[0] (walked first)
	nop
	sw      $a2, 0($t3)
	addiu   $t3, $t3, 4
5:	beqz    $t6, 3f
	andi    $t9, $a0, 2
	beqz    $t9, 3f
	nop
	sw      $t2, 0($t3)
	addiu   $t3, $t3, 4
3:	sw      $t3, PS_ASP($s7)
	b       ct_bm_next
	addiu   $t4, $t1, CA_HEAD - CB_NEXT  # (its head read as an entry's next)
ct_bm_take:
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
	sltiu   $t0, $t6, 2                  # none: a SOLID_BBOX entity's box
	bnez    $t0, ct_box
	li      $t1, 2
	lh      $s3, BM_HULL1($t6)
	beqz    $a0, 8f
	lh      $t0, BM_HULL2($t6)
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
	lw      $t4, PS_BMI($s7)             # the model's entry
	lw      $s0, PS_WTR($s7)             # the call's trace again
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
	b       ct_bm_sight                  # (its fraction changed)
	sh      $t3, TR_ENT($s0)
2:	beqz    $t1, ct_bm_reload            # not even starting inside it
	li      $t0, 1
	b       ct_bm_reload
	sw      $t0, TR_STARTSOLID($s0)

# ct_box: SV_HullForBox. The entry's box (absmin / absmax less its origin)
# grown by the mover's (hullmins = mins - the hull's maxs, hullmaxs = maxs
# - the hull's mins; a monster's by PS_MBOX2) as the 6 axial planes'
# distances of the box hull (ct_box_nodes), traced as a clip hull from its
# first node, the world's tables back after. t4 = the entry, t6 its bm (0,
# or 1 a monster's), a0 = the call's hull.
.if PS_MBOX2 - PS_MBOX - 16
	.error "ct_box takes PS_MBOX2 at PS_MBOX + 16"
.endif
ct_box:
	sll     $t6, $t6, 4                  # the mover's box (a monster's: PS_MBOX2,
	addu    $t6, $t6, $s7                # PS_MBOX + 16)
	lw      $t6, PS_MBOX($t6)
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

# ct_bm_box: a line of sight's entries' box (PS_SIGHT): blocked (the
# trace's fraction under 1), its start point (only an entry holding it can
# clear the line again, starting it in solid), else the whole swept box.
# s0 = the call's trace. Clobbers t0-t3.
ct_bm_box:
	lw      $t0, TR_FRACTION($s0)
	lw      $t1, PS_WSTART($s7)
	slti    $t0, $t0, Q_FIX_ONE
	beqz    $t0, 1f
	nop
	lw      $t2, 0($t1)                  # blocked: the start
	lw      $t3, 4($t1)
	lw      $t1, 8($t1)
	sw      $t2, PS_BMIN+0($s7)
	sw      $t2, PS_BMAX+0($s7)
	sw      $t3, PS_BMIN+4($s7)
	sw      $t3, PS_BMAX+4($s7)
	sw      $t1, PS_BMIN+8($s7)
	jr      $ra
	sw      $t1, PS_BMAX+8($s7)
1:	lw      $t0, PS_FBMIN+0($s7)         # clear: the whole box
	lw      $t1, PS_FBMIN+4($s7)
	lw      $t2, PS_FBMIN+8($s7)
	sw      $t0, PS_BMIN+0($s7)
	sw      $t1, PS_BMIN+4($s7)
	sw      $t2, PS_BMIN+8($s7)
	lw      $t0, PS_FBMAX+0($s7)
	lw      $t1, PS_FBMAX+4($s7)
	lw      $t2, PS_FBMAX+8($s7)
	sw      $t0, PS_BMAX+0($s7)
	sw      $t1, PS_BMAX+4($s7)
	jr      $ra
	sw      $t2, PS_BMAX+8($s7)

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
	beqz    $a0, c0_root
	move    $t9, $s3
	b       ct_root
	nop

	CT_LOOP 0, ct
	CT_LOOP 1, c0

ct_done:
	lw      $ra, PS_CORERA($s7)
	sll     $t0, $s6, 4
	jr      $ra
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

	.section .bss.clip_mover, "aw", @nobits
	.align  2
	.global clip_mover
	.type   clip_mover, @object
clip_mover:                              # clip_mover_t (clip.h): clip_trace_ent's mover
	.space  44

	.section .rodata.ct_hull_box, "a", @progbits
	.align  2
ct_hull_box:                             # the hulls' mins[3], maxs[3], 1/8 units (clip.c hull_box)
	.word   0, 0, 0, 0, 0, 0
	.word   -16 * CLIP_SUB, -16 * CLIP_SUB, -24 * CLIP_SUB, 16 * CLIP_SUB, 16 * CLIP_SUB, 32 * CLIP_SUB
	.word   -32 * CLIP_SUB, -32 * CLIP_SUB, -24 * CLIP_SUB, 32 * CLIP_SUB, 32 * CLIP_SUB, 64 * CLIP_SUB
ct_missile_box:                          # MOVE_MISSILE's mins2 / maxs2 (clip.c missile_box)
	.word   -15 * CLIP_SUB, -15 * CLIP_SUB, -15 * CLIP_SUB, 15 * CLIP_SUB, 15 * CLIP_SUB, 15 * CLIP_SUB
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

# -----------------------------------------------------------------------------
# clip_box_node0(a0 = mins[3], a1 = maxs[3]) -> v0: the hull-0 node every
# point and sweep inside the box walks through (clip.h): from the root,
# down while the box is on one side of the node's plane, by the walks' own
# test (n.p - dist >= 0 the front) at the box's extreme corners. Axial
# planes on the CPU; general ones on the GTE, whose rotation matrix holds
# mins, maxs and maxs - mins as its rows: n.mins + n.maxs - 2 dist -+
# |n|.(maxs - mins) is twice the least / greatest distance.
	.global clip_box_node0
	.type   clip_box_node0, @function
clip_box_node0:
	lui     $t5, %hi(world)
	addiu   $t5, $t5, %lo(world)
	lw      $t6, W_CLIPNODES($t5)
	lw      $t9, W_ROOT($t5)
	lw      $t8, W_PLANES($t5)
	beqz    $t6, 9f                      # no clip hulls: no node
	move    $v0, $zero
	beqz    $t9, 9f
	lui     $t7, 0x1F80                  # (delay) the scratchpad
	sll     $t9, $t9, 8                  # the root as a KUSEG pointer
	srl     $t9, $t9, 8
	lw      $t0, 0($a0)                  # the box: to the scratchpad (indexed
	lw      $t1, 4($a0)                  # by an axial plane's axis) and the GTE
	lw      $t2, 8($a0)
	lw      $t3, 0($a1)
	lw      $t4, 4($a1)
	lw      $t5, 8($a1)
	sw      $t0, PS_BOXLO+0($t7)
	sw      $t1, PS_BOXLO+4($t7)
	sw      $t2, PS_BOXLO+8($t7)
	sw      $t3, PS_BOXHI+0($t7)
	sw      $t4, PS_BOXHI+4($t7)
	sw      $t5, PS_BOXHI+8($t7)
	andi    $t6, $t0, 0xffff             # R11R12 = mins.x | mins.y << 16
	sll     $a2, $t1, 16
	or      $t6, $t6, $a2
	ctc2    $t6, C2_R11R12
	andi    $t6, $t2, 0xffff             # R13R21 = mins.z | maxs.x << 16
	sll     $a2, $t3, 16
	or      $t6, $t6, $a2
	ctc2    $t6, C2_R13R21
	andi    $t6, $t4, 0xffff             # R22R23 = maxs.y | maxs.z << 16
	sll     $a2, $t5, 16
	or      $t6, $t6, $a2
	ctc2    $t6, C2_R22R23
	subu    $t3, $t3, $t0                # the extents
	subu    $t4, $t4, $t1
	subu    $t5, $t5, $t2
	andi    $t3, $t3, 0xffff             # R31R32 = ex | ey << 16
	sll     $t4, $t4, 16
	or      $t3, $t3, $t4
	ctc2    $t3, C2_R31R32
	ctc2    $t5, C2_R33                  # R33 = ez
1:	andi    $t0, $t9, 1
	bnez    $t0, 8f                      # a leaf: the box is in it
	nop
	lhu     $t0, N_PLANE3($t9)
	nop
	sll     $t0, $t0, 2
	addu    $t0, $t8, $t0                # the plane
	lbu     $t1, PL_TYPE($t0)
	lw      $t2, PL_DIST($t0)
	sltiu   $t3, $t1, 3
	beqz    $t3, 3f
	sll     $t1, $t1, 2                  # (delay)
	addu    $t1, $t7, $t1                # axial: n = 4096 on its axis
	lw      $a2, PS_BOXLO($t1)
	lw      $a3, PS_BOXHI($t1)
	sll     $a2, $a2, 12
	sll     $a3, $a3, 12
	subu    $a2, $a2, $t2                # the least distance
	b       4f
	subu    $a3, $a3, $t2                # the greatest
3:	lwc2    C2_VXY0, PL_N+0($t0)         # general: V0 = n, V1 = |n|
	lh      $t3, PL_N+4($t0)
	lh      $t4, PL_N+0($t0)
	lh      $t5, PL_N+2($t0)
	mtc2    $t3, C2_VZ0
	sra     $t6, $t4, 31
	xor     $t4, $t4, $t6
	subu    $t4, $t4, $t6
	MVMVA   0, 0, 0, 3, 0                # MAC1 = n.mins, MAC2 = n.maxs
	sra     $t6, $t5, 31
	xor     $t5, $t5, $t6
	subu    $t5, $t5, $t6
	sra     $t6, $t3, 31
	xor     $t3, $t3, $t6
	subu    $t3, $t3, $t6
	sll     $t5, $t5, 16
	or      $t4, $t4, $t5
	mfc2    $a2, C2_MAC1
	mfc2    $a3, C2_MAC2
	mtc2    $t4, C2_VXY1
	mtc2    $t3, C2_VZ1
	addu    $a2, $a2, $a3                # n.mins + n.maxs - 2 dist
	sll     $t2, $t2, 1
	subu    $a2, $a2, $t2
	MVMVA   0, 0, 1, 3, 0                # MAC3 = |n|.(maxs - mins)
	mfc2    $t3, C2_MAC3
	nop
	addu    $a3, $a2, $t3                # twice the greatest
	subu    $a2, $a2, $t3                # twice the least
4:	bgez    $a2, 5f                      # all in front
	nop
	bltz    $a3, 6f                      # all behind
	nop
8:	move    $v0, $t9                     # across the plane (or a leaf): here
9:	jr      $ra
	nop
5:	lw      $t9, N_CHILD+0($t9)
	b       1b
	nop
6:	lw      $t9, N_CHILD+4($t9)
	b       1b
	nop
	.size   clip_box_node0, . - clip_box_node0

# clip_contents_node0(a0 = node, a1 = p): clip_contents(0, p) walked from
# clip_box_node0's node (p in its box)
	.global clip_contents_node0
	.type   clip_contents_node0, @function
clip_contents_node0:
	addiu   $sp, $sp, -16
	sw      $ra, 0($sp)
	sw      $s1, 4($sp)
	sw      $s2, 8($sp)
	lui     $t5, %hi(world)
	addiu   $t5, $t5, %lo(world)
	lw      $s1, W_CLIPNODES($t5)
	lw      $s2, W_PLANES($t5)
	move    $t9, $a0
	beqz    $s1, 3f                      # no clip hulls
	li      $v0, QW_CONTENTS_EMPTY
	beqz    $t9, 3f                      # no node
	lw      $t0, 0($a1)                  # (delay) GTE row 3 = p
	lw      $t1, 4($a1)
	lw      $t2, 8($a1)
	andi    $t0, $t0, 0xffff
	sll     $t1, $t1, 16
	or      $t0, $t0, $t1
	ctc2    $t0, C2_R31R32
	ctc2    $t2, C2_R33
	bal     c0_pcontents
	move    $a0, $a1
3:	lw      $ra, 0($sp)
	lw      $s1, 4($sp)
	lw      $s2, 8($sp)
	jr      $ra
	addiu   $sp, $sp, 16
	.size   clip_contents_node0, . - clip_contents_node0
