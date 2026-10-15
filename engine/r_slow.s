# q1psx — renderer slow path, hand-written MIPS assembly (architecture.md §8).
#
# Port of engine/r_slow.c, which stays as the reference implementation
# (R_SLOW_C = 1 in r_defs.inc calls it instead). Same algorithm:
#   1. view-transform the polygon (GTE MVMVA) and compute clip codes against
#      the near plane + the 4 frustum planes; all outside one plane -> done
#   2. Sutherland-Hodgman against each plane some vertex is outside of
#   3. project with the GTE: rotation = 2 x identity, translation = 0, so
#      RTPS takes view-space points directly (exact down to 6.7 world units)
#   4. per-edge subdivision of each fan triangle (an edge splits when it is
#      longer than SLOW_SPAN px or its depth ratio exceeds 4/3; the decision
#      depends on the edge alone, so shared edges never crack), emitting
#      POLY_FT3 packets prepended to the world bucket
#   5. reload the view rotation / translation into the GTE
#
#   slow_poly(a0 = face record, a1 = quad/tri, a2 = 4 or 3)
#
# Called from draw_records (r_world.s), which saves at, a0-a3, v0, v1, ra
# around the call. Preserves s0-s7, fp, gp, sp, t8 (scratchpad base);
# advances s6 (packet pointer) and fp (world-bucket link) in place.
#
# Vertex pool entry (32 B): +0 x, +4 y, +8 z (view space, world x 6),
# +12 u, +16 v (texels x 256), +20 SXY. Indices are 8-bit; triangles on the
# scratchpad stack are one word: i0 | i1 << 8 | i2 << 16 | depth << 24.

	.set noreorder
	.set noat

	.include "gtereg.inc"
	.include "inline_s.inc"
	.include "r_defs.inc"

.set PV_X,     0
.set PV_Y,     4
.set PV_Z,     8
.set PV_U,    12
.set PV_V,    16
.set PV_SXY,  20
.set PV_C,    24       # light, 0..255 (interpolated like u / v)

	.section .bss.r_slow_pool, "aw", @nobits
	.align  4
r_slow_pool:
	.space  POOL_MAX * 32

	.section .text.slow_poly, "ax", @progbits
	.global slow_poly
	.type   slow_poly, @function

# -----------------------------------------------------------------------------
slow_poly:
	sw      $ra, SP_SPRA($t8)            # (SP_CLUT16 / SP_TPAGE16: set by
	lhu     $t0, R_POLYS($a0)            # draw_records for the record)
	lbu     $t1, R_NVERTS($a0)
	lui     $v1, %hi(r_slow_pool)
	addiu   $t1, $t1, 3                  # the record's light bytes:
	srl     $t1, $t1, 2                  # polys - ((nv + 3) & ~3), kept in
	sll     $t1, $t1, 2                  # SP_P0 during the transform
	addu    $t0, $t0, $a0
	subu    $t0, $t0, $t1
	sw      $t0, SP_P0($t8)
	addiu   $v1, $v1, %lo(r_slow_pool)   # v1 = pool, for the whole routine

	# ---- 1. transform + clip codes ------------------------------------------
	move    $a3, $zero                   # k
	li      $at, 0x1f                    # codes AND
	move    $v0, $zero                   # codes OR
sp_xf:
	srl     $t0, $a3, 1
	slti    $t1, $a2, 4
	beqz    $t1, 1f                      # quad: ring order k ^ (k >> 1) = 0,1,3,2
	xor     $t0, $t0, $a3
	move    $t0, $a3                     # triangle: k
1:	addu    $t1, $a1, $t0
	lbu     $t2, 0($t1)                  # vertex slot (x 4)
	sll     $t3, $t0, 1
	addu    $t3, $t3, $a1
	lhu     $t3, 4($t3)                  # uv
	srl     $t9, $t2, 2                  # slot
	lw      $t1, SP_P0($t8)              # the record's light bytes
	srl     $t2, $t2, 1                  # slot x 2: its pool index's offset
	addu    $t1, $t1, $t9
	lbu     $t1, 0($t1)                  # light
	addu    $t2, $t2, $a0
	lhu     $t2, R_VERTS($t2)            # pool index
	lw      $t9, RS_VERTPOOL($s0)
	sll     $t2, $t2, 3
	addu    $t2, $t2, $t9
	lwc2    C2_VXY0, 0($t2)
	lwc2    C2_VZ0,  4($t2)
	sll     $t4, $a3, 5
	addu    $t4, $t4, $v1                # pool[k]
	MVMVA   1, 0, 0, 0, 0                # MAC = rot * v + TR (view space)
	mfc2    $t5, C2_MAC1                 # x
	mfc2    $t6, C2_MAC2                 # y
	mfc2    $t7, C2_MAC3                 # z
	andi    $t0, $t3, 0xff
	sll     $t0, $t0, 8                  # u x 256
	srl     $t3, $t3, 8
	sll     $t3, $t3, 8                  # v x 256
	sw      $t5, PV_X($t4)
	sw      $t6, PV_Y($t4)
	sw      $t7, PV_Z($t4)
	sw      $t0, PV_U($t4)
	sw      $t3, PV_V($t4)
	sw      $t1, PV_C($t4)

	addiu   $t0, $t7, -SLOW_ZNEAR        # bit 0: near
	slt     $t9, $t0, $zero
	addu    $t0, $t7, $t5                # bit 1: left   z + x + 12
	addiu   $t0, $t0, 12
	slt     $t0, $t0, $zero
	sll     $t0, $t0, 1
	or      $t9, $t9, $t0
	subu    $t0, $t7, $t5                # bit 2: right  z - x + 12
	addiu   $t0, $t0, 12
	slt     $t0, $t0, $zero
	sll     $t0, $t0, 2
	or      $t9, $t9, $t0
	sll     $t1, $t7, 1
	addu    $t1, $t1, $t7                # 3z
	sll     $t2, $t6, 2                  # 4y
	addu    $t0, $t1, $t2                # bit 3: top    3z + 4y + 48
	addiu   $t0, $t0, 48
	slt     $t0, $t0, $zero
	sll     $t0, $t0, 3
	or      $t9, $t9, $t0
	subu    $t0, $t1, $t2                # bit 4: bottom 3z - 4y + 48
	addiu   $t0, $t0, 48
	slt     $t0, $t0, $zero
	sll     $t0, $t0, 4
	or      $t9, $t9, $t0
	and     $at, $at, $t9
	or      $v0, $v0, $t9

	addu    $t0, $t8, $a3
	sb      $a3, SP_PLA($t0)             # polygon list A[k] = k
	addiu   $a3, $a3, 1
	bne     $a3, $a2, sp_xf
	nop

	bnez    $at, sp_exit                 # entirely outside one plane
	sw      $v0, SP_CODES($t8)
	sw      $a2, SP_POOLN($t8)

	# ---- 2. clip against the planes some vertex is outside of ---------------
	addiu   $a0, $t8, SP_PLA             # src list
	addiu   $a1, $t8, SP_PLB             # dst list
	move    $a3, $zero                   # plane
sp_plane:
	lw      $t0, SP_CODES($t8)
	nop
	srlv    $t0, $t0, $a3
	andi    $t0, $t0, 1
	beqz    $t0, sp_plane_next
	nop
	jal     sp_clip                      # (a0 src, a1 dst, a2 n, a3 plane) -> a2
	nop
	move    $t0, $a0                     # the output becomes the input
	move    $a0, $a1
	move    $a1, $t0
	slti    $t0, $a2, 3
	bnez    $t0, sp_exit                 # clipped away
	nop
sp_plane_next:
	addiu   $a3, $a3, 1
	slti    $t0, $a3, 5
	bnez    $t0, sp_plane
	nop

	# ---- 3. projection mode: rotation = 2 I, translation = 0 ----------------
	li      $t0, 0x2000                  # 2.0 in 1.3.12
	ctc2    $t0, C2_R11R12
	ctc2    $zero, C2_R13R21
	ctc2    $t0, C2_R22R23
	ctc2    $zero, C2_R31R32
	ctc2    $t0, C2_R33
	ctc2    $zero, C2_TRX
	ctc2    $zero, C2_TRY
	ctc2    $zero, C2_TRZ

	move    $at, $zero
sp_projl:
	addu    $t0, $a0, $at
	lbu     $t0, 0($t0)
	nop
	sll     $t0, $t0, 5
	jal     sp_proj
	addu    $t0, $t0, $v1
	addiu   $at, $at, 1
	bne     $at, $a2, sp_projl
	nop

	# ---- 4. fan triangles, each subdivided depth-first ----------------------
	li      $at, 1                       # fan index
	addiu   $a3, $t8, SP_TRI             # stack pointer
sp_fan:
	lbu     $t4, 0($a0)                  # l[0]
	addu    $t0, $a0, $at
	lbu     $t1, 0($t0)                  # l[i]
	lbu     $t2, 1($t0)                  # l[i + 1]
	sll     $t1, $t1, 8
	sll     $t2, $t2, 16
	or      $t1, $t1, $t4
	or      $t1, $t1, $t2
	sw      $t1, 0($a3)                  # depth 0
	addiu   $a3, $a3, 4

sp_sub:
	addiu   $t0, $t8, SP_TRI
	beq     $a3, $t0, sp_fan_next        # stack empty
	nop
	addiu   $a3, $a3, -4
	lw      $t6, 0($a3)                  # triangle word
	nop
	andi    $t0, $t6, 0xff
	srl     $t1, $t6, 8
	andi    $t1, $t1, 0xff
	srl     $t2, $t6, 16
	andi    $t2, $t2, 0xff
	sll     $t0, $t0, 5
	sll     $t1, $t1, 5
	sll     $t2, $t2, 5
	addu    $t0, $t0, $v1
	addu    $t1, $t1, $v1
	addu    $t2, $t2, $v1
	sw      $t0, SP_P0($t8)         # p0 / p1 / p2 pointers
	sw      $t1, SP_P1($t8)
	sw      $t2, SP_P2($t8)

	# off screen? all x < 0, all x >= 320, all y < 0, all y >= 240
	lw      $t3, PV_SXY($t0)
	lw      $t4, PV_SXY($t1)
	lw      $t5, PV_SXY($t2)
	sll     $t0, $t3, 16
	sll     $t1, $t4, 16
	sll     $t2, $t5, 16
	and     $t9, $t0, $t1
	and     $t9, $t9, $t2
	bltz    $t9, sp_sub                  # all x < 0 (sign of x << 16)
	sra     $t0, $t0, 16
	sra     $t1, $t1, 16
	sra     $t2, $t2, 16
	addiu   $t0, $t0, -320
	addiu   $t1, $t1, -320
	addiu   $t2, $t2, -320
	or      $t9, $t0, $t1
	or      $t9, $t9, $t2
	bgez    $t9, sp_sub                  # all x >= 320
	and     $t9, $t3, $t4
	and     $t9, $t9, $t5
	bltz    $t9, sp_sub                  # all y < 0
	sra     $t3, $t3, 16
	sra     $t4, $t4, 16
	sra     $t5, $t5, 16
	addiu   $t3, $t3, -240
	addiu   $t4, $t4, -240
	addiu   $t5, $t5, -240
	or      $t9, $t3, $t4
	or      $t9, $t9, $t5
	bgez    $t9, sp_sub                  # all y >= 240
	nop

	# split decisions (none at max depth or with the pool nearly full)
	move    $t7, $zero
	srl     $t0, $t6, 24
	sltiu   $t0, $t0, SLOW_MAXD
	beqz    $t0, sp_decided
	lw      $t0, SP_POOLN($t8)
	nop
	sltiu   $t0, $t0, POOL_MAX - 3
	beqz    $t0, sp_decided
	nop
	lw      $t4, SP_P0($t8)
	lw      $t5, SP_P1($t8)
	jal     sp_split
	nop
	or      $t7, $t7, $v0                # bit 0: p0-p1
	lw      $t4, SP_P1($t8)
	lw      $t5, SP_P2($t8)
	jal     sp_split
	nop
	sll     $v0, $v0, 1
	or      $t7, $t7, $v0                # bit 1: p1-p2
	lw      $t4, SP_P2($t8)
	lw      $t5, SP_P0($t8)
	jal     sp_split
	nop
	sll     $v0, $v0, 2
	or      $t7, $t7, $v0                # bit 2: p2-p0
sp_decided:
	beqz    $t7, sp_emit
	nop

	# midpoints of the split edges -> a1 = m01 | m12 << 8 | m20 << 16
	move    $a1, $zero
	andi    $t0, $t7, 1
	beqz    $t0, 2f
	lw      $t4, SP_P0($t8)
	lw      $t5, SP_P1($t8)
	jal     sp_mid
	nop
	or      $a1, $a1, $v0
2:	andi    $t0, $t7, 2
	beqz    $t0, 3f
	lw      $t4, SP_P1($t8)
	lw      $t5, SP_P2($t8)
	jal     sp_mid
	nop
	sll     $v0, $v0, 8
	or      $a1, $a1, $v0
3:	andi    $t0, $t7, 4
	beqz    $t0, 4f
	lw      $t4, SP_P2($t8)
	lw      $t5, SP_P0($t8)
	jal     sp_mid
	nop
	sll     $v0, $v0, 16
	or      $a1, $a1, $v0
4:
	# children: t0..t2 = i0..i2, t3..t5 = m01, m12, m20, t9 = (depth + 1) << 24
	andi    $t0, $t6, 0xff
	srl     $t1, $t6, 8
	andi    $t1, $t1, 0xff
	srl     $t2, $t6, 16
	andi    $t2, $t2, 0xff
	andi    $t3, $a1, 0xff
	srl     $t4, $a1, 8
	andi    $t4, $t4, 0xff
	srl     $t5, $a1, 16
	andi    $t5, $t5, 0xff
	srl     $t9, $t6, 24
	addiu   $t9, $t9, 1
	sll     $t9, $t9, 24

.macro PUSH3 ra, rb, rc
	sll     $v0, \rb, 8
	or      $v0, $v0, \ra
	sll     $a1, \rc, 16
	or      $v0, $v0, $a1
	or      $v0, $v0, $t9
	sw      $v0, 0($a3)
	addiu   $a3, $a3, 4
.endm

	li      $v0, 7
	beq     $t7, $v0, sp_c7
	li      $v0, 3
	beq     $t7, $v0, sp_c3
	li      $v0, 6
	beq     $t7, $v0, sp_c6
	li      $v0, 5
	beq     $t7, $v0, sp_c5
	li      $v0, 1
	beq     $t7, $v0, sp_c1
	li      $v0, 2
	beq     $t7, $v0, sp_c2
	nop
	# 4: p2-p0 only
	PUSH3   $t2, $t5, $t1
	PUSH3   $t5, $t0, $t1
	b       sp_sub
	nop
sp_c1:                                   # p0-p1
	PUSH3   $t0, $t3, $t2
	PUSH3   $t3, $t1, $t2
	b       sp_sub
	nop
sp_c2:                                   # p1-p2
	PUSH3   $t1, $t4, $t0
	PUSH3   $t4, $t2, $t0
	b       sp_sub
	nop
sp_c3:                                   # p0-p1 and p1-p2 (p2-p0 whole)
	PUSH3   $t0, $t3, $t2
	PUSH3   $t3, $t1, $t4
	PUSH3   $t3, $t4, $t2
	b       sp_sub
	nop
sp_c6:                                   # p1-p2 and p2-p0 (p0-p1 whole)
	PUSH3   $t1, $t4, $t0
	PUSH3   $t4, $t2, $t5
	PUSH3   $t4, $t5, $t0
	b       sp_sub
	nop
sp_c5:                                   # p2-p0 and p0-p1 (p1-p2 whole)
	PUSH3   $t2, $t5, $t1
	PUSH3   $t5, $t0, $t3
	PUSH3   $t5, $t3, $t1
	b       sp_sub
	nop
sp_c7:                                   # all three: 4 triangles
	PUSH3   $t0, $t3, $t5
	PUSH3   $t3, $t1, $t4
	PUSH3   $t5, $t4, $t2
	PUSH3   $t3, $t4, $t5
	b       sp_sub
	nop

	# ---- emit one POLY_GT3 ----------------------------------------------------
.macro UVWORD rp, dst
	lw      \dst, PV_U(\rp)
	lw      $v0, PV_V(\rp)
	addiu   \dst, \dst, 128
	addiu   $v0, $v0, 128
	sra     \dst, \dst, 8
	sra     $v0, $v0, 8
	sll     $v0, $v0, 8
	or      \dst, \dst, $v0
.endm

# GREY r: light 0..255 -> GPU colour 0x00cccccc. Clobbers v0.
.macro GREY r
	sll     $v0, \r, 8
	or      \r, \r, $v0
	sll     $v0, \r, 8
	or      \r, \r, $v0
.endm

sp_emit:
	addiu   $t0, $s6, 40
	sltu    $t0, $s7, $t0
	bnez    $t0, sp_full
	lw      $t3, SP_P0($t8)
	lw      $t4, SP_P1($t8)
	lw      $t5, SP_P2($t8)
	lui     $t0, 0x0900
	or      $t0, $t0, $fp                # tag: 9 words, link to the old head
	sw      $t0, 0($s6)
	lw      $t1, PV_C($t3)
	lw      $t0, SP_CODE3($t8)
	GREY    $t1
	or      $t1, $t1, $t0                # code | c0
	sw      $t1, 4($s6)
	lw      $t1, PV_SXY($t3)
	nop
	sw      $t1, 8($s6)
	UVWORD  $t3, $t1
	lw      $t0, SP_CLUT16($t8)
	nop
	or      $t1, $t1, $t0
	sw      $t1, 12($s6)
	lw      $t1, PV_C($t4)
	nop
	GREY    $t1
	sw      $t1, 16($s6)
	lw      $t1, PV_SXY($t4)
	nop
	sw      $t1, 20($s6)
	UVWORD  $t4, $t1
	lw      $t0, SP_TPAGE16($t8)
	nop
	or      $t1, $t1, $t0
	sw      $t1, 24($s6)
	lw      $t1, PV_C($t5)
	nop
	GREY    $t1
	sw      $t1, 28($s6)
	lw      $t1, PV_SXY($t5)
	nop
	sw      $t1, 32($s6)
	UVWORD  $t5, $t1
	sw      $t1, 36($s6)
	sll     $fp, $s6, 8
	srl     $fp, $fp, 8                  # new bucket head
	addiu   $s6, $s6, 40
	lw      $t0, SP_SLOWTRI($t8)
	nop
	addiu   $t0, $t0, 1
	b       sp_sub
	sw      $t0, SP_SLOWTRI($t8)

sp_full:
	li      $t0, 1
	b       sp_done
	sw      $t0, SP_FULL($t8)

sp_fan_next:
	addiu   $at, $at, 1
	addiu   $t0, $a2, -1
	bne     $at, $t0, sp_fan
	nop

	# ---- 5. back to view mode ---------------------------------------------------
sp_done:
	lw      $t0, RS_ROT+0($s0)
	lw      $t1, RS_ROT+4($s0)
	lw      $t2, RS_ROT+8($s0)
	lw      $t3, RS_ROT+12($s0)
	lh      $t4, RS_ROT+16($s0)
	ctc2    $t0, C2_R11R12
	ctc2    $t1, C2_R13R21
	ctc2    $t2, C2_R22R23
	ctc2    $t3, C2_R31R32
	ctc2    $t4, C2_R33
	lw      $t0, RS_TR+0($s0)
	lw      $t1, RS_TR+4($s0)
	lw      $t2, RS_TR+8($s0)
	ctc2    $t0, C2_TRX
	ctc2    $t1, C2_TRY
	ctc2    $t2, C2_TRZ
sp_exit:
	lw      $ra, SP_SPRA($t8)
	nop
	jr      $ra
	nop

# -----------------------------------------------------------------------------
# sp_pdist: v0 = signed distance of pool vertex t0 to plane a3 (inside >= 0).
# Clobbers t2, t3, v0.
sp_pdist:
	lw      $t2, PV_Z($t0)
	li      $v0, 1
	beq     $a3, $zero, pd_near
	lw      $t3, PV_X($t0)
	beq     $a3, $v0, pd_left
	li      $v0, 2
	beq     $a3, $v0, pd_right
	li      $v0, 3
	lw      $t3, PV_Y($t0)
	beq     $a3, $v0, pd_top
	sll     $v0, $t2, 1
	addu    $v0, $v0, $t2                # bottom: 3z - 4y + 48
	sll     $t3, $t3, 2
	subu    $v0, $v0, $t3
	jr      $ra
	addiu   $v0, $v0, 48
pd_top:                                  # top: 3z + 4y + 48
	addu    $v0, $v0, $t2
	sll     $t3, $t3, 2
	addu    $v0, $v0, $t3
	jr      $ra
	addiu   $v0, $v0, 48
pd_near:
	jr      $ra
	addiu   $v0, $t2, -SLOW_ZNEAR
pd_left:                                 # left: z + x + 12
	addu    $v0, $t2, $t3
	jr      $ra
	addiu   $v0, $v0, 12
pd_right:                                # right: z - x + 12
	subu    $v0, $t2, $t3
	jr      $ra
	addiu   $v0, $v0, 12

# -----------------------------------------------------------------------------
# sp_clip: Sutherland-Hodgman of index list a0 (a2 entries) against plane
# a3 into list a1; returns the new count in a2. Keeps a0, a1, a3, v1.
# Clobbers at, t0-t7, t9, v0.
sp_clip:
	sw      $ra, SP_CLIPRA($t8)
	sw      $zero, SP_CLIPM($t8)       # m
	lbu     $t0, 0($a0)
	move    $at, $zero                   # i
	sll     $t0, $t0, 5
	addu    $t4, $t0, $v1                # a = pool[l[0]]
	jal     sp_pdist
	move    $t0, $t4
	move    $t6, $v0                     # da
sc_loop:
	addiu   $t0, $at, 1
	bne     $t0, $a2, 1f
	nop
	move    $t0, $zero                   # wrap to l[0]
1:	addu    $t0, $t0, $a0
	lbu     $t0, 0($t0)
	nop
	sll     $t0, $t0, 5
	addu    $t5, $t0, $v1                # b
	jal     sp_pdist
	move    $t0, $t5
	move    $t7, $v0                     # db
	bltz    $t6, 2f                      # a outside: dropped
	subu    $t0, $t4, $v1
	srl     $t0, $t0, 5                  # a's index
	lw      $t1, SP_CLIPM($t8)
	nop
	addu    $t2, $a1, $t1
	sb      $t0, 0($t2)
	addiu   $t1, $t1, 1
	sw      $t1, SP_CLIPM($t8)
2:	xor     $t0, $t6, $t7
	bgez    $t0, 3f                      # no crossing
	nop
	jal     sp_lerp                      # keeps t4-t7
	nop
	bltz    $v0, 3f                      # pool full: drop the crossing
	nop
	lw      $t1, SP_CLIPM($t8)
	nop
	addu    $t2, $a1, $t1
	sb      $v0, 0($t2)
	addiu   $t1, $t1, 1
	sw      $t1, SP_CLIPM($t8)
3:	move    $t4, $t5
	move    $t6, $t7
	addiu   $at, $at, 1
	bne     $at, $a2, sc_loop
	nop
	lw      $a2, SP_CLIPM($t8)
	lw      $ra, SP_CLIPRA($t8)
	nop
	jr      $ra
	nop

# -----------------------------------------------------------------------------
# sp_lerp: append the point where edge t4 -> t5 crosses the plane (signed
# distances t6, t7) to the pool; v0 = its index, or -1 when the pool is
# full. One division: t = (da << 14) / (da - db). Keeps t4-t7.
# Clobbers t0-t3, t9, v0, HI/LO.
.macro LERPC off
	lw      $t1, \off($t4)
	lw      $t2, \off($t5)
	nop
	subu    $t2, $t2, $t1
	mult    $t2, $t9
	mflo    $t3
	sra     $t3, $t3, 14
	addu    $t3, $t3, $t1
	sw      $t3, \off($t0)
.endm

sp_lerp:
	lw      $v0, SP_POOLN($t8)
	subu    $t9, $t6, $t7                # den = da - db (never 0: signs differ)
	sltiu   $t0, $v0, POOL_MAX
	beqz    $t0, sl_full
	sll     $t1, $t6, 14
	div     $zero, $t1, $t9
	sll     $t0, $v0, 5
	addu    $t0, $t0, $v1                # new vertex
	addiu   $t2, $v0, 1
	sw      $t2, SP_POOLN($t8)
	mflo    $t9                          # t, 14-bit
	nop
	nop
	LERPC   PV_X
	LERPC   PV_Y
	LERPC   PV_Z
	LERPC   PV_U
	LERPC   PV_V
	LERPC   PV_C
	jr      $ra
	nop
sl_full:
	jr      $ra
	li      $v0, -1

# -----------------------------------------------------------------------------
# sp_mid: append the midpoint of pool vertices t4, t5 (symmetric, so both
# triangles sharing the edge build the same point) and project it;
# v0 = index (callers have checked the pool has room). Clobbers t0-t3, v0.
.macro MIDC off
	lw      $t1, \off($t4)
	lw      $t2, \off($t5)
	nop
	addu    $t1, $t1, $t2
	sra     $t1, $t1, 1
	sw      $t1, \off($t0)
.endm

sp_mid:
	lw      $v0, SP_POOLN($t8)
	nop
	sll     $t0, $v0, 5
	addu    $t0, $t0, $v1
	addiu   $t1, $v0, 1
	sw      $t1, SP_POOLN($t8)
	MIDC    PV_X
	MIDC    PV_Y
	MIDC    PV_Z
	MIDC    PV_U
	MIDC    PV_V
	MIDC    PV_C
	# fall through: project pool vertex t0

# sp_proj: project pool vertex t0 (GTE in projection mode). Clobbers t1-t3.
sp_proj:
	lw      $t1, PV_X($t0)
	lw      $t2, PV_Y($t0)
	lw      $t3, PV_Z($t0)
	andi    $t1, $t1, 0xffff
	sll     $t2, $t2, 16
	or      $t1, $t1, $t2
	mtc2    $t1, C2_VXY0
	mtc2    $t3, C2_VZ0
	nop
	nop
	RTPS
	swc2    C2_SXY2, PV_SXY($t0)
	jr      $ra
	nop

# -----------------------------------------------------------------------------
# sp_split: v0 = 1 if edge t4-t5 must be split: longer than SLOW_SPAN px on
# screen (|dx| + |dy|), or depth ratio > 4/3. Symmetric in t4, t5.
# Clobbers t0-t3, v0.
sp_split:
	lw      $t0, PV_SXY($t4)
	lw      $t1, PV_SXY($t5)
	nop
	sll     $t2, $t0, 16
	sra     $t2, $t2, 16
	sll     $t3, $t1, 16
	sra     $t3, $t3, 16
	subu    $t2, $t2, $t3                # dx
	sra     $t3, $t2, 31
	xor     $t2, $t2, $t3
	subu    $t2, $t2, $t3                # |dx|
	sra     $t0, $t0, 16
	sra     $t1, $t1, 16
	subu    $t0, $t0, $t1                # dy
	sra     $t1, $t0, 31
	xor     $t0, $t0, $t1
	subu    $t0, $t0, $t1                # |dy|
	addu    $t2, $t2, $t0
	slti    $t2, $t2, SLOW_SPAN + 1
	beqz    $t2, ss_yes
	nop
	lw      $t0, PV_Z($t4)
	lw      $t1, PV_Z($t5)
	nop
	slt     $t2, $t0, $t1
	bnez    $t2, 1f                      # t0 = lower, t1 = higher
	nop
	move    $t2, $t0
	move    $t0, $t1
	move    $t1, $t2
1:	sll     $t2, $t1, 1
	addu    $t2, $t2, $t1                # 3 zhi
	sll     $t3, $t0, 2                  # 4 zlo
	jr      $ra
	slt     $v0, $t3, $t2
ss_yes:
	jr      $ra
	li      $v0, 1

	.size slow_poly, . - slow_poly
