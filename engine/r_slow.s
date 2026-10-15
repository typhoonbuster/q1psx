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
#      POLY_GT3 packets prepended to the world bucket
#   5. reload the view rotation / translation into the GTE
#
#   slow_poly(a0 = face record, a1 = quad/tri, a2 = 4 or 3)
#
# Called from draw_records (r_world.s), which saves at, a0-a3, v0, v1, ra
# around the call. Preserves s0-s7, fp, gp, sp, t8 (scratchpad base);
# advances s6 (packet pointer) and fp (world-bucket link) in place.
#
# Vertex pool entry (32 B): +0 x, +4 y, +8 z (view space, world x 6),
# +12 u, +16 v (texels x 256), +20 SXY, +24 light then grey colour word,
# +28 uv word (the last three set by the projection, once per vertex). Indices are 8-bit; triangles on the
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
.set PV_GC,   24       # light 0..255 (interpolated like u / v); once
                        # projected, the grey colour word 0x00cccccc
.set PV_UVW,  28       # once projected: the packet's uv word (rounded texels)

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
sp_xf_1:
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
	lw      $t9, SP_VERTPOOL($t8)
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
	andi    $t3, $t3, 0xff00             # v x 256
	sw      $t5, PV_X($t4)
	sw      $t6, PV_Y($t4)
	sw      $t7, PV_Z($t4)
	sw      $t0, PV_U($t4)
	sw      $t3, PV_V($t4)
	sw      $t1, PV_GC($t4)

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
	bne     $a3, $a2, sp_xf_1
	srl     $t0, $a3, 1                  # (the next k's first step)

	bnez    $at, sp_exit                 # entirely outside one plane
	sw      $v0, SP_CODES($t8)
	sw      $a2, SP_POOLN($t8)

	# ---- 2. clip against the planes some vertex is outside of ---------------
	# Sutherland-Hodgman, one plane at a time: index list a0 (a2 entries)
	# -> a1. First the plane's signed distance of every input vertex
	# (inside >= 0), kept in its SXY slot (free until the projection); then
	# the edges: a kept vertex, then the crossing if the edge crosses
	# (t = (da << 14) / (da - db), one division per crossing).
	addiu   $a0, $t8, SP_PLA             # src list
	addiu   $a1, $t8, SP_PLB             # dst list
	move    $a3, $zero                   # plane
sp_plane:
	lw      $t0, SP_CODES($t8)
	nop
	srlv    $t0, $t0, $a3
	andi    $t0, $t0, 1
	beqz    $t0, sp_plane_next
	addu    $t3, $a0, $a2                # end of the input list
	lbu     $t0, 0($a0)
	move    $at, $a0
	sb      $t0, 0($t3)                  # l[n] = l[0]: the closing edge
	beqz    $a3, sd_near
	li      $t2, 2
	slt     $t1, $a3, $t2
	bnez    $t1, sd_left                 # 1
	li      $t1, 3
	beq     $a3, $t2, sd_right
	nop
	beq     $a3, $t1, sd_top
	nop
sd_bottom:                               # bottom: 3z - 4y + 48
	lbu     $t0, 0($at)
	addiu   $at, $at, 1
	sll     $t0, $t0, 5
	addu    $t0, $t0, $v1
	lw      $t1, PV_Z($t0)
	lw      $t2, PV_Y($t0)
	sll     $t9, $t1, 1
	addu    $t1, $t1, $t9
	sll     $t2, $t2, 2
	subu    $t1, $t1, $t2
	addiu   $t1, $t1, 48
	bne     $at, $t3, sd_bottom
	sw      $t1, PV_SXY($t0)
	b       sc_edges_1
	lbu     $t0, 0($a0)
sd_top:                                  # top: 3z + 4y + 48
	lbu     $t0, 0($at)
	addiu   $at, $at, 1
	sll     $t0, $t0, 5
	addu    $t0, $t0, $v1
	lw      $t1, PV_Z($t0)
	lw      $t2, PV_Y($t0)
	sll     $t9, $t1, 1
	addu    $t1, $t1, $t9
	sll     $t2, $t2, 2
	addu    $t1, $t1, $t2
	addiu   $t1, $t1, 48
	bne     $at, $t3, sd_top
	sw      $t1, PV_SXY($t0)
	b       sc_edges_1
	lbu     $t0, 0($a0)
sd_left:                                 # left: z + x + 12
	lbu     $t0, 0($at)
	addiu   $at, $at, 1
	sll     $t0, $t0, 5
	addu    $t0, $t0, $v1
	lw      $t1, PV_Z($t0)
	lw      $t2, PV_X($t0)
	nop
	addu    $t1, $t1, $t2
	addiu   $t1, $t1, 12
	bne     $at, $t3, sd_left
	sw      $t1, PV_SXY($t0)
	b       sc_edges_1
	lbu     $t0, 0($a0)
sd_right:                                # right: z - x + 12
	lbu     $t0, 0($at)
	addiu   $at, $at, 1
	sll     $t0, $t0, 5
	addu    $t0, $t0, $v1
	lw      $t1, PV_Z($t0)
	lw      $t2, PV_X($t0)
	nop
	subu    $t1, $t1, $t2
	addiu   $t1, $t1, 12
	bne     $at, $t3, sd_right
	sw      $t1, PV_SXY($t0)
	b       sc_edges_1
	lbu     $t0, 0($a0)
sd_near:                                 # near: z - SLOW_ZNEAR
	lbu     $t0, 0($at)
	addiu   $at, $at, 1
	sll     $t0, $t0, 5
	addu    $t0, $t0, $v1
	lw      $t1, PV_Z($t0)
	nop
	addiu   $t1, $t1, -SLOW_ZNEAR
	bne     $at, $t3, sd_near
	sw      $t1, PV_SXY($t0)

sc_edges:
	lbu     $t0, 0($a0)                  # a = l[0]
sc_edges_1:
	sw      $a1, SP_CLIPM($t8)           # (the output list's start)
	sll     $t0, $t0, 5
	addu    $t4, $t0, $v1
	lw      $t6, PV_SXY($t4)             # da
	move    $at, $a0
sc_loop:                                 # a2 edges left; a1 = output cursor
	lbu     $t0, 1($at)                  # b = l[i + 1] (l[n] = l[0])
	bltz    $t6, 2f                      # a outside: dropped
	subu    $t1, $t4, $v1
	srl     $t1, $t1, 5                  # a's index
	sb      $t1, 0($a1)
	addiu   $a1, $a1, 1
2:	sll     $t0, $t0, 5
	addu    $t5, $t0, $v1                # b
	lw      $t7, PV_SXY($t5)             # db
	addiu   $at, $at, 1
	xor     $t0, $t6, $t7
	bgez    $t0, 3f                      # no crossing
	lw      $v0, SP_POOLN($t8)
	subu    $t9, $t6, $t7                # den = da - db (never 0: signs differ)
	sltiu   $t0, $v0, POOL_MAX
	beqz    $t0, 3f                      # pool full: drop the crossing
	sll     $t1, $t6, 14
	div     $zero, $t1, $t9
	sll     $t0, $v0, 5
	addu    $t0, $t0, $v1                # the new vertex
	addiu   $t2, $v0, 1
	sw      $t2, SP_POOLN($t8)
	sb      $v0, 0($a1)
	addiu   $a1, $a1, 1
	lw      $t1, PV_X($t4)               # a + ((b - a) t >> 14) per field,
	lw      $t2, PV_X($t5)               # two fields in flight
	lw      $t3, PV_Y($t4)
	lw      $v0, PV_Y($t5)
	subu    $t2, $t2, $t1
	mflo    $t9                          # t, 14-bit (waits for the division)
	subu    $v0, $v0, $t3
	nop
	mult    $t2, $t9
	mflo    $t2
	sra     $t2, $t2, 14
	addu    $t2, $t2, $t1
	mult    $v0, $t9
	sw      $t2, PV_X($t0)
	lw      $t1, PV_Z($t4)
	lw      $t2, PV_Z($t5)
	mflo    $v0
	sra     $v0, $v0, 14
	addu    $v0, $v0, $t3
	subu    $t2, $t2, $t1
	mult    $t2, $t9
	sw      $v0, PV_Y($t0)
	lw      $t3, PV_U($t4)
	lw      $v0, PV_U($t5)
	mflo    $t2
	sra     $t2, $t2, 14
	addu    $t2, $t2, $t1
	subu    $v0, $v0, $t3
	mult    $v0, $t9
	sw      $t2, PV_Z($t0)
	lw      $t1, PV_V($t4)
	lw      $t2, PV_V($t5)
	mflo    $v0
	sra     $v0, $v0, 14
	addu    $v0, $v0, $t3
	subu    $t2, $t2, $t1
	mult    $t2, $t9
	sw      $v0, PV_U($t0)
	lw      $t3, PV_GC($t4)
	lw      $v0, PV_GC($t5)
	mflo    $t2
	sra     $t2, $t2, 14
	addu    $t2, $t2, $t1
	subu    $v0, $v0, $t3
	mult    $v0, $t9
	sw      $t2, PV_V($t0)
	mflo    $v0
	sra     $v0, $v0, 14
	addu    $v0, $v0, $t3
	sw      $v0, PV_GC($t0)
3:	move    $t4, $t5
	addiu   $a2, $a2, -1
	bnez    $a2, sc_loop
	move    $t6, $t7
	lw      $t0, SP_CLIPM($t8)           # the output list
	move    $t1, $a0
	subu    $a2, $a1, $t0                # its count
	move    $a0, $t0                     # the output becomes the input
	move    $a1, $t1
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

	# project every vertex of the clipped polygon (once each) and ready its
	# packet words: its light becomes the grey colour word in place, u / v
	# the rounded uv word
	move    $at, $a0
	addu    $t9, $a0, $a2
sp_projl:
	lbu     $t0, 0($at)
	addiu   $at, $at, 1
	sll     $t0, $t0, 5
	addu    $t0, $t0, $v1
	lw      $t1, PV_X($t0)
	lw      $t2, PV_Y($t0)
	lw      $t3, PV_Z($t0)
	andi    $t1, $t1, 0xffff
	sll     $t2, $t2, 16
	or      $t1, $t1, $t2
	mtc2    $t1, C2_VXY0
	mtc2    $t3, C2_VZ0
	lw      $t1, PV_U($t0)
	lw      $t2, PV_V($t0)
	RTPS
	addiu   $t1, $t1, 128
	addiu   $t2, $t2, 128
	sra     $t1, $t1, 8
	sra     $t2, $t2, 8
	sll     $t2, $t2, 8
	or      $t1, $t1, $t2
	lw      $t2, PV_GC($t0)
	sw      $t1, PV_UVW($t0)
	sll     $t3, $t2, 8
	or      $t2, $t2, $t3
	sll     $t3, $t2, 8
	or      $t2, $t2, $t3
	sw      $t2, PV_GC($t0)
	bne     $at, $t9, sp_projl
	swc2    C2_SXY2, PV_SXY($t0)

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
	or      $t6, $t1, $t4
	b       sp_tri                       # (the stack is empty: no push / pop)
	or      $t6, $t6, $t2                # depth 0

sp_sub:
	addiu   $t0, $t8, SP_TRI
	beq     $a3, $t0, sp_fan_next        # stack empty
	lw      $t6, -4($a3)                 # (below the stack when empty: harmless)
	addiu   $a3, $a3, -4
sp_tri:                                  # t6 = i0 | i1 << 8 | i2 << 16 | depth << 24
	sll     $t0, $t6, 5                  # index x 32 = pool offset
	andi    $t0, $t0, 0x1fe0
	srl     $t1, $t6, 3
	andi    $t1, $t1, 0x1fe0
	srl     $t2, $t6, 11
	andi    $t2, $t2, 0x1fe0
	addu    $t0, $t0, $v1
	addu    $t1, $t1, $v1
	addu    $t2, $t2, $v1
	lw      $t3, PV_SXY($t0)
	lw      $t4, PV_SXY($t1)
	lw      $t5, PV_SXY($t2)
	sw      $t0, SP_P0($t8)              # p0 / p1 / p2 pointers
	sw      $t1, SP_P1($t8)
	sw      $t2, SP_P2($t8)

	# off screen? all x < 0, all x >= 320, all y < 0, all y >= 240
	sll     $t7, $t3, 16
	sll     $t9, $t4, 16
	sll     $v0, $t5, 16
	and     $a1, $t7, $t9
	and     $a1, $a1, $v0
	bltz    $a1, sp_sub                  # all x < 0 (sign of x << 16)
	sra     $t7, $t7, 16                 # x0 (x1 / x2: t9 / v0)
	sra     $t9, $t9, 16
	sra     $v0, $v0, 16
	addiu   $a1, $t7, -320
	addiu   $t0, $t9, -320
	or      $a1, $a1, $t0
	addiu   $t0, $v0, -320
	or      $a1, $a1, $t0
	bgez    $a1, sp_sub                  # all x >= 320
	and     $a1, $t3, $t4
	and     $a1, $a1, $t5
	bltz    $a1, sp_sub                  # all y < 0
	sra     $t3, $t3, 16                 # y0 (y1 / y2: t4 / t5)
	sra     $t4, $t4, 16
	sra     $t5, $t5, 16
	addiu   $a1, $t3, -240
	addiu   $t0, $t4, -240
	or      $a1, $a1, $t0
	addiu   $t0, $t5, -240
	or      $a1, $a1, $t0
	bgez    $a1, sp_sub                  # all y >= 240
	srl     $t0, $t6, 24

	# split decisions (none at max depth or with the pool nearly full). An
	# edge splits when |dx| + |dy| > SLOW_SPAN px, i.e. |dp| or |dq| > SPAN
	# with p = x + y, q = x - y; or when its depth ratio exceeds 4/3 (z > 0
	# past the near clip: 4 z_lo < 3 z_hi <=> 4 zb < 3 za or 4 za < 3 zb).
	# Symmetric in the edge's ends: shared edges never crack.
	sltiu   $t0, $t0, SLOW_MAXD
	beqz    $t0, sp_emit
	lw      $t1, SP_POOLN($t8)
	addu    $t0, $t7, $t3                # p0
	subu    $t7, $t7, $t3                # q0
	sltiu   $t1, $t1, POOL_MAX - 3
	beqz    $t1, sp_emit
	addu    $t1, $t9, $t4                # p1
	subu    $t9, $t9, $t4                # q1
	addu    $t2, $v0, $t5                # p2
	subu    $v0, $v0, $t5                # q2

.macro SHORT pa, pb, qa, qb, out         # out = 1: |dp| <= SPAN and |dq| <= SPAN
	subu    \out, \pa, \pb
	addiu   \out, \out, SLOW_SPAN
	sltiu   \out, \out, 2 * SLOW_SPAN + 1
	subu    $a1, \qa, \qb
	addiu   $a1, $a1, SLOW_SPAN
	sltiu   $a1, $a1, 2 * SLOW_SPAN + 1
	and     \out, \out, $a1
.endm
	SHORT   $t0, $t1, $t7, $t9, $t3      # p0-p1
	SHORT   $t1, $t2, $t9, $v0, $t4      # p1-p2
	SHORT   $t2, $t0, $v0, $t7, $t5      # p2-p0

	lw      $t0, SP_P0($t8)
	lw      $t1, SP_P1($t8)
	lw      $t2, SP_P2($t8)
	lw      $t7, PV_Z($t0)
	lw      $t9, PV_Z($t1)
	lw      $v0, PV_Z($t2)
	sll     $t0, $t7, 2                  # 4 z0
	sll     $t1, $t9, 2                  # 4 z1
	sll     $t2, $v0, 2                  # 4 z2
	sll     $a1, $t7, 1
	addu    $t7, $t7, $a1                # 3 z0
	sll     $a1, $t9, 1
	addu    $t9, $t9, $a1                # 3 z1
	sll     $a1, $v0, 1
	addu    $v0, $v0, $a1                # 3 z2

.macro SPLIT r, z4a, z3a, z4b, z3b       # r: short -> split (long or deep)
	xori    \r, \r, 1
	slt     $a1, \z4b, \z3a
	or      \r, \r, $a1
	slt     $a1, \z4a, \z3b
	or      \r, \r, $a1
.endm
	SPLIT   $t3, $t0, $t7, $t1, $t9      # p0-p1
	SPLIT   $t4, $t1, $t9, $t2, $v0      # p1-p2
	SPLIT   $t5, $t2, $v0, $t0, $t7      # p2-p0
	sll     $t4, $t4, 1
	sll     $t5, $t5, 2
	or      $t7, $t3, $t4                # t7 bit 0: p0-p1, 1: p1-p2, 2: p2-p0
	or      $t7, $t7, $t5
	beqz    $t7, sp_emit
	move    $a1, $zero

	# midpoints of the split edges -> a1 = m01 | m12 << 8 | m20 << 16
	andi    $t0, $t7, 1
	beqz    $t0, 2f
	lw      $t4, SP_P0($t8)
	jal     sp_mid
	lw      $t5, SP_P1($t8)
	or      $a1, $a1, $v0
2:	andi    $t0, $t7, 2
	beqz    $t0, 3f
	lw      $t4, SP_P1($t8)
	jal     sp_mid
	lw      $t5, SP_P2($t8)
	sll     $v0, $v0, 8
	or      $a1, $a1, $v0
3:	andi    $t0, $t7, 4
	beqz    $t0, 4f
	lw      $t4, SP_P2($t8)
	jal     sp_mid
	lw      $t5, SP_P0($t8)
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
	# colour and uv words are ready in the pool (projection, sp_mid)
sp_emit:
	addiu   $t0, $s6, 40
	sltu    $t0, $s7, $t0
	bnez    $t0, sp_full
	lw      $t3, SP_P0($t8)
	lw      $t4, SP_P1($t8)
	lw      $t5, SP_P2($t8)
	lw      $t0, PV_GC($t3)
	lw      $t1, PV_SXY($t3)
	lw      $t2, PV_UVW($t3)
	lui     $t7, CODE_GT3 >> 16
	lw      $t9, SP_CLUT16($t8)
	or      $t0, $t0, $t7                # code | c0
	sw      $t0, 4($s6)
	sw      $t1, 8($s6)
	or      $t2, $t2, $t9                # uv0 | clut
	sw      $t2, 12($s6)
	lw      $t0, PV_GC($t4)
	lw      $t1, PV_SXY($t4)
	lw      $t2, PV_UVW($t4)
	lw      $t9, SP_TPAGE16($t8)
	sw      $t0, 16($s6)
	sw      $t1, 20($s6)
	or      $t2, $t2, $t9                # uv1 | tpage
	sw      $t2, 24($s6)
	lw      $t0, PV_GC($t5)
	lw      $t1, PV_SXY($t5)
	lw      $t2, PV_UVW($t5)
	lui     $t9, 0x0900
	sw      $t0, 28($s6)
	sw      $t1, 32($s6)
	sw      $t2, 36($s6)
	or      $t9, $t9, $fp                # tag: 9 words, link to the old head
	sw      $t9, 0($s6)
	move    $fp, $s6                     # new bucket head (KUSEG address)
.if R_PROFILE
	addiu   $s6, $s6, 40
	lw      $t0, SP_SLOWTRI($t8)
	nop
	addiu   $t0, $t0, 1
	b       sp_sub
	sw      $t0, SP_SLOWTRI($t8)
.else
	b       sp_sub
	addiu   $s6, $s6, 40
.endif

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
# sp_mid: append the midpoint of pool vertices t4, t5 (symmetric, so both
# triangles sharing the edge build the same point), project it and ready its
# packet words; v0 = index (callers have checked the pool has room).
# Clobbers t0-t5, t9, v0.
sp_mid:
	lw      $v0, SP_POOLN($t8)
	lw      $t1, PV_X($t4)
	lw      $t2, PV_X($t5)
	sll     $t0, $v0, 5
	addu    $t0, $t0, $v1                # the new vertex
	addu    $t1, $t1, $t2
	lw      $t2, PV_Y($t4)
	lw      $t3, PV_Y($t5)
	sra     $t1, $t1, 1                  # x
	sw      $t1, PV_X($t0)
	addu    $t2, $t2, $t3
	sra     $t2, $t2, 1                  # y
	sw      $t2, PV_Y($t0)
	andi    $t1, $t1, 0xffff
	sll     $t3, $t2, 16
	or      $t1, $t1, $t3
	mtc2    $t1, C2_VXY0
	lw      $t1, PV_Z($t4)
	lw      $t2, PV_Z($t5)
	addiu   $t3, $v0, 1
	addu    $t1, $t1, $t2
	sra     $t1, $t1, 1                  # z
	mtc2    $t1, C2_VZ0
	sw      $t1, PV_Z($t0)
	sw      $t3, SP_POOLN($t8)
	RTPS                                 # (rotation 2 I: view space in)
	lw      $t1, PV_U($t4)               # while the GTE works: u, v, light
	lw      $t2, PV_U($t5)
	lw      $t3, PV_V($t4)
	lw      $t9, PV_V($t5)
	addu    $t1, $t1, $t2
	sra     $t1, $t1, 1                  # u x 256
	addu    $t3, $t3, $t9
	sra     $t3, $t3, 1                  # v x 256
	sw      $t1, PV_U($t0)
	sw      $t3, PV_V($t0)
	lw      $t4, PV_GC($t4)              # (the ends are projected: their
	lw      $t5, PV_GC($t5)              # light is the low byte of GC)
	addiu   $t1, $t1, 128
	addiu   $t3, $t3, 128
	sra     $t1, $t1, 8
	sra     $t3, $t3, 8
	sll     $t3, $t3, 8
	or      $t1, $t1, $t3
	sw      $t1, PV_UVW($t0)             # uv word, rounded texels
	andi    $t4, $t4, 0xff
	andi    $t5, $t5, 0xff
	addu    $t4, $t4, $t5
	sra     $t4, $t4, 1                  # light
	sll     $t5, $t4, 8
	or      $t4, $t4, $t5
	sll     $t5, $t4, 8
	or      $t4, $t4, $t5
	sw      $t4, PV_GC($t0)              # grey 0x00cccccc
	swc2    C2_SXY2, PV_SXY($t0)
	jr      $ra
	nop

	.size slow_poly, . - slow_poly
