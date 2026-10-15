# q1psx — renderer subdivision paths, hand-written MIPS assembly
# (architecture.md §8, docs/porting-notes.md §19).
#
# Port of engine/r_slow.c, the specification (R_SLOW_C = 1 in r_defs.inc calls
# the C instead): the same decisions in the same order, the same packets.
#
#   slow_poly(a0 = face record, a1 = quad / tri, a2 = 4 or 3)
#
# draw_records sends here the polygons its per-polygon test (CAREFUL) cannot
# clear: a vertex may be too near, an edge too long, the affine error too
# high.
#   1. corners from the record's vertex cache (the fast path's RTPT SXY / SZ
#      and grey colour); a corner the GTE could not project (SZ < SZ_CLIP,
#      SXY at its limits) sends the polygon to the clip path
#   2. no clip: subdivide the triangle, or a quad's two GPU halves
#   3. clip path: view space (MVMVA, the view matrix still loaded), clip
#      against the near plane only, crossings interpolated from a fixed end,
#      project what the cache does not hold by division; fan, subdivide
#   4. subdivision, depth first: an edge splits when |dx| + |dy| > SPAN or
#      (|dx| + |dy|) |dz| > T2 (za + zb); its midpoint is the projected 3D
#      midpoint s = (sa za + sb zb) / (za + zb), shared by the triangles of
#      the polygon that meet on that edge (a table on the end pair);
#      POLY_GT3 leaves prepended to the world bucket
#
# Called from draw_records (r_world.s), which saves at, a0-a3, v0, v1 around
# the call. Preserves s0-s7, fp, gp, sp, t8 (scratchpad base); advances s6
# (packet pointer) and fp (world-bucket link) in place.
#
# Registers for the whole call: v1 = the pool, s4 = vertices in it, a3 = the
# triangle stack's top. The node loop also takes s0 / s5 / ra (saved on
# entry) for its three z * T2.
#
# Pool entry (32 B), index i at r_slow_pool + i * 32: +0 SXY (x | y << 16),
# +4 z, +8 z * T2, +12 u, +16 v (texels x 256), +20 key (a midpoint: its
# edge, lower index << 9 | higher; else -1), +24 grey colour word (light in
# its low byte), +28 uv word (packet-ready). Corners take entries 0..n-1,
# then clip crossings, then midpoints. A triangle on the stack is one word:
# i0 | i1 << 9 | i2 << 18 | depth << 27; a negative word under the first
# ends the polygon.
#
# Midpoint table: HASH_N halfwords, slot = f(key) -> a pool index. A slot
# is believed only if that index is below s4 and the entry there carries the
# key: no clearing between polygons. A collision costs a second copy of the
# same midpoint, never a different one.

	.set noreorder
	.set noat

	.include "gtereg.inc"
	.include "inline_s.inc"
	.include "r_defs.inc"

.set PV_SXY,   0
.set PV_Z,     4
.set PV_ZT,    8
.set PV_U,    12
.set PV_V,    16
.set PV_KEY,  20
.set PV_GC,   24
.set PV_UVW,  28

.set SZ_CLIP, 96       # SZ below this: the GTE's projection is off (r_slow.c)
.set ZNEAR,   48       # near clip plane, 8 world units
.set SPAN,    500      # split edges with |dx| + |dy| above this (r_slow.c)
.set KOFS,    8192     # midpoint rounding offset (r_slow.c rdiv)
.set POOL_N,  512      # pool entries (9-bit indices)
.set HASH_N,  512      # midpoint table slots
.set STACK_N, 64       # triangle stack words (depth first: 3 x MAXD + 2 + fan)

	.section .bss.r_slow_pool, "aw", @nobits
	.align  4
r_slow_pool:
	.space  POOL_N * 32
r_slow_stack:
	.space  STACK_N * 4
r_slow_view:                             # clip path: view x, y, z per pool entry 0..5
	.space  6 * 16
r_slow_hash:
	.space  HASH_N * 2

	.section .rodata.r_slow, "a", @progbits
	.align  2
sp_ctab:                                 # children by split mask (1..7)
	.word   0, sp_c1, sp_c2, sp_c3, sp_c4, sp_c5, sp_c6, sp_c7

# NOSPLIT out, xa, ya, za, zta, xb, yb, zb, ztb: out = 1 when edge (a, b)
# does not split: |dx| + |dy| <= SPAN and (|dx| + |dy|) |dz| <= zta + ztb
# (z * T2 each). Clobbers v0, t9.
.macro NOSPLIT out, xa, ya, za, zta, xb, yb, zb, ztb
	subu    \out, \xa, \xb
	bgez    \out, 1f
	subu    $v0, \ya, \yb
	subu    \out, $zero, \out
1:	bgez    $v0, 2f
	subu    $t9, \za, \zb
	subu    $v0, $zero, $v0
2:	bgez    $t9, 3f
	addu    \out, \out, $v0              # |dx| + |dy|
	subu    $t9, $zero, $t9
3:	mult    \out, $t9                    # (|dx| + |dy|) |dz|
	sltiu   $v0, \out, SPAN + 1          # 1: short enough
	addu    $t9, \zta, \ztb              # T2 (za + zb)
	mflo    \out
	sltu    \out, $t9, \out              # 1: affine error over the threshold
	sltu    \out, \out, $v0              # 1: short and under: no split
.endm

# PUSHW off, ra, rb, rc: triangle (pool indices) at depth t9 (<< 27) into the
# stack word off(a3); the caller moves a3. Clobbers v0, at.
.macro PUSHW off, ra, rb, rc
	sll     $v0, \rb, 9
	sll     $at, \rc, 18
	or      $v0, $v0, \ra
	or      $v0, $v0, $at
	or      $v0, $v0, $t9
	sw      $v0, \off($a3)
.endm

	.section .text.slow_poly, "ax", @progbits
	.global slow_poly
	.type   slow_poly, @function

# -----------------------------------------------------------------------------
slow_poly:
	sw      $ra, SP_SPRA($t8)            # (SP_CLUT16 / SP_TPAGE16: set by
	sw      $s0, SP_SV+0($t8)            # draw_records for the record)
	sw      $s1, SP_SV+4($t8)
	sw      $s2, SP_SV+8($t8)
	sw      $s3, SP_SV+12($t8)
	sw      $s4, SP_SV+16($t8)
	sw      $s5, SP_SV+20($t8)
	lw      $t7, SP_T2($t8)              # T2
	lui     $v1, %hi(r_slow_pool)
	addiu   $v1, $v1, %lo(r_slow_pool)   # v1 = pool, for the whole call

	# ---- 1. corners (PS1 order) from the cache ---------------------------
	move    $s1, $zero                   # clip mask: bit k = corner k off range
	move    $t9, $zero                   # k
	move    $t6, $v1                     # pool[k]
sp_corner:
	addu    $t0, $a1, $t9
	lbu     $t1, 0($t0)                  # slot x 4
	sll     $t2, $t9, 1
	addu    $t2, $t2, $a1
	lhu     $t3, 4($t2)                  # uv
	addu    $t1, $t1, $t8
	lw      $t5, SP_SZ($t1)
	lw      $t4, SP_SXY($t1)
	mult    $t5, $t7                     # z T2
	lw      $t1, SP_COL($t1)             # grey colour, light in its low byte
	sw      $t5, PV_Z($t6)
	sw      $t4, PV_SXY($t6)
	sw      $t1, PV_GC($t6)
	andi    $t0, $t3, 0xff
	sll     $t0, $t0, 8                  # u x 256
	andi    $t2, $t3, 0xff00             # v x 256
	sw      $t0, PV_U($t6)
	sw      $t2, PV_V($t6)
	sw      $t3, PV_UVW($t6)             # (u x 256 + 128) >> 8 = the byte
	li      $t0, -1
	sw      $t0, PV_KEY($t6)             # not a midpoint
	sltiu   $t2, $t5, SZ_CLIP            # off range: SZ < SZ_CLIP, or x / y
	sll     $t0, $t4, 16                 # outside -1023..1022
	sra     $t0, $t0, 16                 # x
	addiu   $t0, $t0, 1023
	sltiu   $t0, $t0, 2046               # 1: x in range
	sra     $t1, $t4, 16                 # y
	addiu   $t1, $t1, 1023
	sltiu   $t1, $t1, 2046               # 1: y in range
	and     $t0, $t0, $t1
	xori    $t0, $t0, 1
	or      $t2, $t2, $t0
	sllv    $t2, $t2, $t9
	or      $s1, $s1, $t2
	mflo    $t0
	sw      $t0, PV_ZT($t6)
	addiu   $t9, $t9, 1
	bne     $t9, $a2, sp_corner
	addiu   $t6, $t6, 32

	move    $s4, $a2                     # pool: the corners
	lui     $a3, %hi(r_slow_stack)
	addiu   $a3, $a3, %lo(r_slow_stack)  # a3 = triangle stack
	li      $t0, -1
	sw      $t0, 0($a3)                  # the end marker
	bnez    $s1, sp_clip
	addiu   $a3, $a3, 4

	# ---- 2. straight from the cache: (0, 1, 2), then a quad's (1, 3, 2) ----
	li      $t0, 2 << 18 | 1 << 9 | 0    # (0, 1, 2), depth 0
	li      $t1, 4
	bne     $a2, $t1, 1f
	nop
	li      $t1, 2 << 18 | 3 << 9 | 1    # (1, 3, 2)
	sw      $t1, 0($a3)
	addiu   $a3, $a3, 4
1:	sw      $t0, 0($a3)
	b       sp_sub
	addiu   $a3, $a3, 4

	# ---- 3. clip path ---------------------------------------------------------
	# Corners to view space in ring order (a quad: 0, 1, 3, 2); a corner the
	# cache holds keeps its SZ as z. Then the near plane.
sp_clip:
	lw      $t9, SP_VERTPOOL($t8)
	lui     $t7, %hi(r_slow_view)
	addiu   $t7, $t7, %lo(r_slow_view)
	move    $t6, $zero                   # j: corner (PS1 order)
	move    $s3, $zero                   # behind count
1:	addu    $t0, $a1, $t6
	lbu     $t0, 0($t0)                  # slot x 4
	nop
	srl     $t0, $t0, 1                  # slot x 2: its pool-index offset
	addu    $t0, $t0, $a0
	lhu     $t0, R_VERTS($t0)            # pool index
	nop
	sll     $t0, $t0, 3
	addu    $t0, $t0, $t9
	lwc2    C2_VXY0, 0($t0)
	lwc2    C2_VZ0,  4($t0)
	sll     $t1, $t6, 4
	addu    $t1, $t1, $t7                # view[j]
	MVMVA   1, 0, 0, 0, 0                # MAC = rot v + TR (>> 12)
	sll     $t2, $t6, 5
	addu    $t2, $t2, $v1                # pool[j]
	mfc2    $t3, C2_MAC1
	mfc2    $t4, C2_MAC2
	mfc2    $t5, C2_MAC3
	srlv    $t0, $s1, $t6
	andi    $t0, $t0, 1                  # 1: not from the cache
	bnez    $t0, 2f
	sw      $t3, 0($t1)                  # view x
	lw      $t5, PV_Z($t2)               # cached: its SZ
2:	sw      $t4, 4($t1)                  # view y
	sw      $t5, 8($t1)                  # view z
	slti    $t0, $t5, ZNEAR
	addu    $s3, $s3, $t0
	addiu   $t6, $t6, 1
	bne     $t6, $a2, 1b
	nop
	beq     $s3, $a2, sp_done            # all behind the near plane
	nop

	# the ring (pool indices) in SP_PLA, n in s2
	li      $t0, 0x0100                  # 0, 1
	li      $t1, 4
	beq     $a2, $t1, 3f
	lui     $t1, 0x0203                  # quad ring: 0, 1, 3, 2
	lui     $t1, 0x0002                  # triangle ring: 0, 1, 2
3:	or      $t0, $t0, $t1
	sw      $t0, SP_PLA($t8)
	move    $s2, $a2
	addiu   $a0, $t8, SP_PLA             # a0 = the list to fan
	beqz    $s3, sp_project
	nop

	# Sutherland-Hodgman against z >= ZNEAR: SP_PLA -> SP_PLB, crossings
	# appended to the pool (from the end before() picks: the same point for
	# both polygons sharing the edge)
	addiu   $a1, $t8, SP_PLB             # output cursor
	move    $t6, $zero                   # i
sc_edge:
	addu    $t0, $a0, $t6
	lbu     $t2, 0($t0)                  # a
	addiu   $t1, $t6, 1
	bne     $t1, $s2, 4f
	nop
	move    $t1, $zero
4:	addu    $t1, $a0, $t1
	lbu     $t3, 0($t1)                  # b
	sll     $t4, $t2, 4
	addu    $t4, $t4, $t7                # view[a]
	sll     $t5, $t3, 4
	addu    $t5, $t5, $t7                # view[b]
	lw      $t0, 8($t4)                  # za
	lw      $t1, 8($t5)                  # zb
	addiu   $t0, $t0, -ZNEAR             # da
	addiu   $t1, $t1, -ZNEAR             # db
	bltz    $t0, 5f
	nop
	sb      $t2, 0($a1)                  # a kept
	addiu   $a1, $a1, 1
5:	xor     $at, $t0, $t1
	bgez    $at, sc_next                 # no crossing
	nop
	# before(a, b): z, then x, then y
	lw      $at, 8($t4)
	lw      $v0, 8($t5)
	nop
	bne     $at, $v0, 6f
	slt     $at, $at, $v0                # (delay: za < zb)
	lw      $at, 0($t4)
	lw      $v0, 0($t5)
	nop
	bne     $at, $v0, 6f
	slt     $at, $at, $v0
	lw      $at, 4($t4)
	lw      $v0, 4($t5)
	nop
	slt     $at, $at, $v0
6:	bnez    $at, 7f                      # a first: lerp(a, b, da, da - db)
	nop
	move    $v0, $t2                     # else lerp(b, a, db, db - da)
	move    $t2, $t3
	move    $t3, $v0
	move    $v0, $t4
	move    $t4, $t5
	move    $t5, $v0
	move    $v0, $t0
	move    $t0, $t1
	move    $t1, $v0
7:	jal     sp_lerp                      # (t2 / t4 from, t3 / t5 to, t0 num,
	subu    $t1, $t0, $t1                #  t1 den) -> new pool entry
	sb      $v0, 0($a1)
	addiu   $a1, $a1, 1
sc_next:
	addiu   $t6, $t6, 1
	bne     $t6, $s2, sc_edge
	nop
	addiu   $a0, $t8, SP_PLB
	subu    $s2, $a1, $a0                # n

	# project what the cache does not hold: clamp z, divide; fill the entry
sp_project:
	move    $t6, $zero
8:	addu    $t0, $a0, $t6
	lbu     $t0, 0($t0)                  # pool index
	nop
	sltu    $t1, $t0, $a2
	beqz    $t1, 9f                      # a crossing (index >= nv): project
	srlv    $t1, $s1, $t0
	andi    $t1, $t1, 1
	beqz    $t1, sp_proj_next            # a cached corner: as it is
	nop
9:	jal     sp_proj
	nop
sp_proj_next:
	addiu   $t6, $t6, 1
	bne     $t6, $s2, 8b
	nop

	# fan from the first vertex: push the last triangle first
	move    $t9, $zero                   # depth 0
	addiu   $t6, $s2, -2                 # i = n - 2 .. 1
10:	addu    $t3, $a0, $t6
	lbu     $t0, 0($a0)                  # l[0]
	lbu     $t1, 0($t3)                  # l[i]
	lbu     $t2, 1($t3)                  # l[i + 1]
	PUSHW   0, $t0, $t1, $t2
	addiu   $t6, $t6, -1
	bgtz    $t6, 10b
	addiu   $a3, $a3, 4

	# ---- 4. subdivision -----------------------------------------------------------
sp_sub:
	lw      $s2, -4($a3)                 # the triangle on top
	addiu   $a3, $a3, -4
	bltz    $s2, sp_done                 # the end marker: done
	sll     $a0, $s2, 5
	andi    $a0, $a0, 0x3fe0
	srl     $a1, $s2, 4
	andi    $a1, $a1, 0x3fe0
	srl     $a2, $s2, 13
	andi    $a2, $a2, 0x3fe0
	addu    $a0, $a0, $v1                # p0 / p1 / p2
	addu    $a1, $a1, $v1
	addu    $a2, $a2, $v1
	lh      $t0, PV_SXY($a0)             # x0 y0
	lh      $t1, PV_SXY+2($a0)
	lh      $t3, PV_SXY($a1)             # x1 y1
	lh      $t4, PV_SXY+2($a1)
	lh      $t6, PV_SXY($a2)             # x2 y2
	lh      $t7, PV_SXY+2($a2)

	# off screen? all x < 0 or all y < 0; all x >= 320 or all y >= 240
	and     $at, $t0, $t3
	and     $at, $at, $t6
	and     $v0, $t1, $t4
	and     $v0, $v0, $t7
	or      $at, $at, $v0                # < 0: all x < 0 or all y < 0
	bltz    $at, sp_sub
	addiu   $at, $t0, -320
	addiu   $v0, $t3, -320
	or      $at, $at, $v0
	addiu   $v0, $t6, -320
	or      $at, $at, $v0                # >= 0: all x >= 320
	addiu   $v0, $t1, -240
	addiu   $t9, $t4, -240
	or      $v0, $v0, $t9
	addiu   $t9, $t7, -240
	or      $v0, $v0, $t9                # >= 0: all y >= 240
	and     $at, $at, $v0
	bgez    $at, sp_sub
	lw      $t2, PV_Z($a0)               # z0 z1 z2, z0 T2 .. z2 T2
	lw      $t5, PV_Z($a1)
	lw      $s1, PV_Z($a2)
	lw      $s0, PV_ZT($a0)
	lw      $s5, PV_ZT($a1)
	lw      $ra, PV_ZT($a2)

	# split decisions (none at max depth, or with the pool nearly full)
	sll     $at, $s2, 1                  # depth 8 (MAXD): bit 30
	bltz    $at, sp_leaf_max
	sltiu   $at, $s4, POOL_N - 2
	beqz    $at, sp_leaf_max
	nop
	NOSPLIT $s3, $t0, $t1, $t2, $s0, $t3, $t4, $t5, $s5      # p0-p1
	NOSPLIT $at, $t3, $t4, $t5, $s5, $t6, $t7, $s1, $ra      # p1-p2
	sll     $at, $at, 1
	or      $s3, $s3, $at
	NOSPLIT $at, $t6, $t7, $s1, $ra, $t0, $t1, $t2, $s0      # p2-p0
	sll     $at, $at, 2
	or      $s3, $s3, $at
	xori    $s3, $s3, 7                  # split: bit 0 p0-p1, 1 p1-p2, 2 p2-p0
	bnez    $s3, sp_split
	nop

	# ---- a leaf: one POLY_GT3 (a0 / a1 / a2 = its pool entries) ------------
sp_emit:
	addiu   $t0, $s6, 40
	sltu    $t0, $s7, $t0
	bnez    $t0, sp_full
	lui     $t9, CODE_GT3 >> 16
	lw      $t0, PV_GC($a0)
	lw      $t1, PV_SXY($a0)
	lw      $t2, PV_UVW($a0)
	lw      $t3, SP_CLUT16($t8)
	or      $t0, $t0, $t9                # code | c0
	sw      $t0, 4($s6)
	sw      $t1, 8($s6)
	or      $t2, $t2, $t3                # uv0 | clut
	sw      $t2, 12($s6)
	lw      $t0, PV_GC($a1)
	lw      $t1, PV_SXY($a1)
	lw      $t2, PV_UVW($a1)
	lw      $t3, SP_TPAGE16($t8)
	sw      $t0, 16($s6)
	sw      $t1, 20($s6)
	or      $t2, $t2, $t3                # uv1 | tpage
	sw      $t2, 24($s6)
	lw      $t0, PV_GC($a2)
	lw      $t1, PV_SXY($a2)
	lw      $t2, PV_UVW($a2)
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

	# ---- split: the midpoints, then the children ---------------------------
sp_split:                                # s3 = mask, s2 = the triangle
	andi    $t0, $s3, 1
	beqz    $t0, 1f
	andi    $t0, $s2, 0x1ff              # (i0)
	srl     $t1, $s2, 9
	jal     sp_mid
	andi    $t1, $t1, 0x1ff              # i1
	move    $s0, $v0                     # s0 = m01
1:	andi    $t0, $s3, 2
	beqz    $t0, 2f
	srl     $t0, $s2, 9
	andi    $t0, $t0, 0x1ff              # i1
	srl     $t1, $s2, 18
	jal     sp_mid
	andi    $t1, $t1, 0x1ff              # i2
	move    $s5, $v0                     # s5 = m12
2:	andi    $t0, $s3, 4
	beqz    $t0, 3f
	srl     $t0, $s2, 18
	andi    $t0, $t0, 0x1ff              # i2
	jal     sp_mid
	andi    $t1, $s2, 0x1ff              # i0
	move    $s1, $v0                     # s1 = m20
3:	# children in r_slow.c's order (the last pushed comes out first):
	# t0..t2 = i0..i2, t9 = (depth + 1) << 27
	srl     $at, $s2, 27
	addiu   $at, $at, 1
	sll     $t9, $at, 27
	sll     $at, $s3, 2
	lui     $v0, %hi(sp_ctab)
	addu    $v0, $v0, $at
	lw      $v0, %lo(sp_ctab)($v0)
	andi    $t0, $s2, 0x1ff
	srl     $t1, $s2, 9
	andi    $t1, $t1, 0x1ff
	srl     $t2, $s2, 18
	jr      $v0
	andi    $t2, $t2, 0x1ff
sp_c7:                                   # all three: 4 triangles
	PUSHW   0,  $t0, $s0, $s1            # (p0, m01, m20)
	PUSHW   4,  $s0, $t1, $s5            # (m01, p1, m12)
	PUSHW   8,  $s1, $s5, $t2            # (m20, m12, p2)
	PUSHW   12, $s0, $s5, $s1            # (m01, m12, m20)
	b       sp_sub
	addiu   $a3, $a3, 16
sp_c3:                                   # p0-p1 and p1-p2 (p2-p0 whole)
	PUSHW   0,  $t0, $s0, $t2            # (p0, m01, p2)
	PUSHW   4,  $s0, $t1, $s5            # (m01, p1, m12)
	PUSHW   8,  $s0, $s5, $t2            # (m01, m12, p2)
	b       sp_sub
	addiu   $a3, $a3, 12
sp_c6:                                   # p1-p2 and p2-p0 (p0-p1 whole)
	PUSHW   0,  $t1, $s5, $t0            # (p1, m12, p0)
	PUSHW   4,  $s5, $t2, $s1            # (m12, p2, m20)
	PUSHW   8,  $s5, $s1, $t0            # (m12, m20, p0)
	b       sp_sub
	addiu   $a3, $a3, 12
sp_c5:                                   # p2-p0 and p0-p1 (p1-p2 whole)
	PUSHW   0,  $t2, $s1, $t1            # (p2, m20, p1)
	PUSHW   4,  $s1, $t0, $s0            # (m20, p0, m01)
	PUSHW   8,  $s1, $s0, $t1            # (m20, m01, p1)
	b       sp_sub
	addiu   $a3, $a3, 12
sp_c1:                                   # p0-p1
	PUSHW   0,  $t0, $s0, $t2            # (p0, m01, p2)
	PUSHW   4,  $s0, $t1, $t2            # (m01, p1, p2)
	b       sp_sub
	addiu   $a3, $a3, 8
sp_c2:                                   # p1-p2
	PUSHW   0,  $t1, $s5, $t0            # (p1, m12, p0)
	PUSHW   4,  $s5, $t2, $t0            # (m12, p2, p0)
	b       sp_sub
	addiu   $a3, $a3, 8
sp_c4:                                   # p2-p0
	PUSHW   0,  $t2, $s1, $t1            # (p2, m20, p1)
	PUSHW   4,  $s1, $t0, $t1            # (m20, p0, p1)
	b       sp_sub
	addiu   $a3, $a3, 8

	# a forced leaf (max depth, pool full): only if the GPU takes it
sp_leaf_max:
	move    $at, $t0                     # max x - min x <= 1023?
	slt     $v0, $t3, $at
	beqz    $v0, 1f
	nop
	move    $at, $t3
1:	slt     $v0, $t6, $at
	beqz    $v0, 2f
	nop
	move    $at, $t6
2:	move    $t9, $t0                     # (at = min x, t9 = max x)
	slt     $v0, $t9, $t3
	beqz    $v0, 3f
	nop
	move    $t9, $t3
3:	slt     $v0, $t9, $t6
	beqz    $v0, 4f
	nop
	move    $t9, $t6
4:	subu    $t9, $t9, $at
	sltiu   $t9, $t9, 1024
	beqz    $t9, sp_sub
	move    $at, $t1                     # max y - min y <= 511?
	slt     $v0, $t4, $at
	beqz    $v0, 1f
	nop
	move    $at, $t4
1:	slt     $v0, $t7, $at
	beqz    $v0, 2f
	nop
	move    $at, $t7
2:	move    $t9, $t1
	slt     $v0, $t9, $t4
	beqz    $v0, 3f
	nop
	move    $t9, $t4
3:	slt     $v0, $t9, $t7
	beqz    $v0, 4f
	nop
	move    $t9, $t7
4:	subu    $t9, $t9, $at
	sltiu   $t9, $t9, 512
	bnez    $t9, sp_emit
	nop
	b       sp_sub
	nop

sp_full:
	li      $t0, 1
	sw      $t0, SP_FULL($t8)
sp_done:
	lw      $ra, SP_SPRA($t8)
	lw      $s0, SP_SV+0($t8)
	lw      $s1, SP_SV+4($t8)
	lw      $s2, SP_SV+8($t8)
	lw      $s3, SP_SV+12($t8)
	lw      $s4, SP_SV+16($t8)
	jr      $ra
	lw      $s5, SP_SV+20($t8)

# -----------------------------------------------------------------------------
# sp_mid: the midpoint of pool entries t0, t1 (indices): found through the
# table, or made. v0 = its index. Clobbers at, t0-t7, t9, a0-a2 (the
# s-registers, v1 and a3 kept; s4 grows by a new one).
sp_mid:
	sltu    $at, $t0, $t1                # key: lower << 9 | higher
	bnez    $at, 1f
	nop
	xor     $t0, $t0, $t1
	xor     $t1, $t0, $t1
	xor     $t0, $t0, $t1
1:	sll     $t2, $t0, 9
	or      $t2, $t2, $t1                # key
	srl     $t3, $t2, 4
	xor     $t3, $t3, $t2
	andi    $t3, $t3, HASH_N - 1         # slot
	sll     $t3, $t3, 1
	lui     $t6, %hi(r_slow_hash)
	addu    $t6, $t6, $t3
	lhu     $v0, %lo(r_slow_hash)($t6)   # the index it holds
	addiu   $t6, $t6, %lo(r_slow_hash)   # (the slot, for an insert)
	sltu    $at, $v0, $s4                # one of this polygon's?
	beqz    $at, sm_new
	sll     $t4, $v0, 5
	addu    $t4, $t4, $v1
	lw      $t4, PV_KEY($t4)
	nop
	bne     $t4, $t2, sm_new             # another edge's
	nop
	jr      $ra
	nop

	# a new vertex at index n = s4: the projected 3D midpoint
sm_new:
	sh      $s4, 0($t6)                  # table: this edge -> n
	sll     $t0, $t0, 5
	addu    $a0, $t0, $v1                # p
	sll     $t1, $t1, 5
	addu    $a1, $t1, $v1                # q
	sll     $a2, $s4, 5
	addu    $a2, $a2, $v1                # m
	addiu   $s4, $s4, 1
	lw      $t3, PV_Z($a0)               # pz
	lw      $t5, PV_Z($a1)               # qz
	lh      $t0, PV_SXY($a0)             # px
	lh      $t1, PV_SXY($a1)             # qx
	sw      $t2, PV_KEY($a2)
	mult    $t0, $t3                     # px pz
	addu    $t6, $t3, $t5                # zs
	sll     $t9, $t6, 13                 # zs KOFS
	srl     $t7, $t6, 1
	addu    $t9, $t9, $t7                # + zs / 2: the rounding offset
	mflo    $t0
	lh      $t2, PV_SXY+2($a0)           # py
	lh      $t4, PV_SXY+2($a1)           # qy
	mult    $t1, $t5                     # qx qz
	addu    $t0, $t0, $t9
	lw      $t7, PV_U($a0)
	mflo    $t1
	lw      $at, PV_U($a1)
	addu    $t0, $t0, $t1                # px pz + qx qz + offset
	divu    $zero, $t0, $t6              # x + KOFS (while it divides: u, v)
	lw      $t0, PV_V($a0)
	lw      $t1, PV_V($a1)
	addu    $t7, $t7, $at
	sra     $t7, $t7, 1                  # u
	addu    $t0, $t0, $t1
	sra     $t0, $t0, 1                  # v
	sw      $t7, PV_U($a2)
	sw      $t0, PV_V($a2)
	addiu   $t7, $t7, 128                # uv word: rounded texels
	srl     $t7, $t7, 8
	addiu   $t0, $t0, 128
	andi    $t0, $t0, 0xff00
	or      $t7, $t7, $t0
	sw      $t7, PV_UVW($a2)
	mflo    $t0                          # x + KOFS
	addiu   $t0, $t0, -KOFS
	andi    $t0, $t0, 0xffff
	mult    $t2, $t3                     # py pz
	lw      $t1, PV_GC($a0)
	lw      $t7, PV_GC($a1)
	mflo    $t2
	andi    $t1, $t1, 0xff
	andi    $t7, $t7, 0xff
	mult    $t4, $t5                     # qy qz
	addu    $t2, $t2, $t9
	addu    $t1, $t1, $t7
	mflo    $t4
	srl     $t1, $t1, 1                  # light
	addu    $t2, $t2, $t4                # py pz + qy qz + offset
	divu    $zero, $t2, $t6              # y + KOFS (while it divides: light, z)
	sll     $t7, $t1, 8                  # grey c -> 0x00cccccc
	or      $t1, $t1, $t7
	sll     $t7, $t1, 8
	or      $t1, $t1, $t7
	sw      $t1, PV_GC($a2)
	srl     $t6, $t6, 1                  # z = zs >> 1
	sw      $t6, PV_Z($a2)
	lw      $t1, SP_T2($t8)
	mflo    $t2                          # y + KOFS
	addiu   $t2, $t2, -KOFS
	sll     $t2, $t2, 16
	mult    $t6, $t1                     # z T2
	or      $t0, $t0, $t2
	sw      $t0, PV_SXY($a2)
	mflo    $t0
	sw      $t0, PV_ZT($a2)
	jr      $ra
	addiu   $v0, $s4, -1                 # its index

# -----------------------------------------------------------------------------
# sp_lerp: a near-plane crossing, from end t2 (pool index) / t4 (view entry)
# toward t3 / t5: t = (num << 14) / den (t0 num, t1 den), each of view x,
# y, z and u, v, light moved by (b - a) t >> 14. Appended to the pool (its
# view entry too); v0 = its index. Clobbers at, t0-t5, t9. Keeps t6, t7, a*.
.macro LERPF off, ba, bb, bd
	lw      $at, \off(\ba)
	lw      $v0, \off(\bb)
	nop
	subu    $v0, $v0, $at
	mult    $v0, $t9
	mflo    $v0
	sra     $v0, $v0, 14
	addu    $v0, $v0, $at
	sw      $v0, \off(\bd)
.endm

sp_lerp:
	sll     $t0, $t0, 14
	div     $zero, $t0, $t1
	sll     $t2, $t2, 5
	addu    $t2, $t2, $v1                # pool[a]
	sll     $t3, $t3, 5
	addu    $t3, $t3, $v1                # pool[b]
	sll     $t0, $s4, 4
	addu    $t0, $t0, $t7                # view[n]
	sll     $t1, $s4, 5
	addu    $t1, $t1, $v1                # pool[n]
	li      $at, -1
	sw      $at, PV_KEY($t1)             # not a midpoint
	mflo    $t9                          # t
	LERPF   0, $t4, $t5, $t0             # view x
	LERPF   4, $t4, $t5, $t0             #      y
	LERPF   8, $t4, $t5, $t0             #      z
	LERPF   PV_U, $t2, $t3, $t1
	LERPF   PV_V, $t2, $t3, $t1
	lw      $at, PV_GC($t2)              # light (the grey's low byte)
	lw      $v0, PV_GC($t3)
	andi    $at, $at, 0xff
	andi    $v0, $v0, 0xff
	subu    $v0, $v0, $at
	mult    $v0, $t9
	mflo    $v0
	sra     $v0, $v0, 14
	addu    $v0, $v0, $at
	sll     $at, $v0, 8                  # grey c -> 0x00cccccc
	or      $v0, $v0, $at
	sll     $at, $v0, 8
	or      $v0, $v0, $at
	sw      $v0, PV_GC($t1)
	move    $v0, $s4
	jr      $ra
	addiu   $s4, $s4, 1

# -----------------------------------------------------------------------------
# sp_proj: fill pool entry t0 (not from the cache) from its view entry: z
# clamped to the near plane, sx = OFX + x H / z, sy = OFY + y H / z, z T2,
# uv word. Clobbers at, t0-t5. Keeps t6, t7, a*.
sp_proj:
	lui     $t1, %hi(r_slow_view)
	addiu   $t1, $t1, %lo(r_slow_view)
	sll     $t2, $t0, 4
	addu    $t1, $t1, $t2                # view[i]
	sll     $t0, $t0, 5
	addu    $t0, $t0, $v1                # pool[i]
	lw      $t4, 8($t1)                  # z
	lw      $t2, 0($t1)                  # x
	lw      $t3, 4($t1)                  # y
	slti    $t5, $t4, ZNEAR
	beqz    $t5, 1f
	li      $at, 160
	li      $t4, ZNEAR                   # (the lerp's rounding)
1:	mult    $t2, $at
	mflo    $t2
	nop
	nop
	div     $zero, $t2, $t4
	mflo    $t2
	nop
	nop
	mult    $t3, $at
	mflo    $t3
	nop
	nop
	div     $zero, $t3, $t4
	addiu   $t2, $t2, 160                # sx
	andi    $t2, $t2, 0xffff
	mflo    $t3
	addiu   $t3, $t3, 120                # sy
	sll     $t3, $t3, 16
	or      $t2, $t2, $t3
	sw      $t2, PV_SXY($t0)
	sw      $t4, PV_Z($t0)
	lw      $t5, SP_T2($t8)
	nop
	mult    $t4, $t5                     # z T2
	lw      $t2, PV_U($t0)
	lw      $t3, PV_V($t0)
	nop
	addiu   $t2, $t2, 128
	srl     $t2, $t2, 8
	addiu   $t3, $t3, 128
	andi    $t3, $t3, 0xff00
	or      $t2, $t2, $t3
	sw      $t2, PV_UVW($t0)
	mflo    $t2
	jr      $ra
	sw      $t2, PV_ZT($t0)

	.size slow_poly, . - slow_poly
