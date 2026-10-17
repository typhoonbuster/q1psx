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

# (SZ_CLIP, 96 in r_slow.c: SP_SZCLIP here; the view model lowers it)
# (ZNEAR, the near clip plane: SP_ZNEAR, 48 = 8 world units for the world)
.set SPAN,    500      # split edges with |dx| + |dy| above this (r_slow.c)
.set KOFS,    8192     # midpoint rounding offset (r_slow.c rdiv)
.set POOL_N,  512      # pool entries (9-bit indices)
.set HASH_N,  512      # midpoint table slots
.set STACK_N, 64       # triangle stack words (depth first: 3 x MAXD + 2 + fan)

	.section .bss.r_slow_pool, "aw", @nobits
	.align  4
r_slow_pool:
	.space  POOL_N * 32
r_slow_view:                             # clip path: view x, y, z of pool entries
	.space  6 * 32                       # 0..5, at VIEW_OFS from each (32 B apart)
r_slow_hash:                             # (HASH_OFS from the pool: v1-relative)
	.space  HASH_N * 2

	.section .data.r_slow_stack, "aw", @progbits
	.align  2
r_slow_stack:                            # word 0: the end marker under every
	.word   -1                           # polygon's triangles (never written)
	.space  (STACK_N - 1) * 4

.set HASH_OFS, r_slow_hash - r_slow_pool
.set VIEW_OFS, r_slow_view - r_slow_pool

	.section .rodata.r_slow, "a", @progbits
	.align  2
sp_ctab:                                 # children by split mask (1..7)
	.word   0, sp_c1, sp_c2, sp_c3, sp_c4, sp_c5, sp_c6, sp_c7

# NOSPLIT out, xa, ya, za, zta, xb, yb, zb, ztb [, first]: out = 1 when edge
# (a, b) does not split: |dx| + |dy| <= SPAN and (|dx| + |dy|) |dz| <= zta +
# ztb (z * T2 each). first = 0: the caller did `subu out, xa, xb` already (a
# delay slot); next: an instruction for the multiply's shadow (the next
# test's first). Clobbers v0, t9.
.macro NOSPLIT out, xa, ya, za, zta, xb, yb, zb, ztb, first=1, next=
.if \first
	subu    \out, \xa, \xb
.endif
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
	\next                                # (the next test's first, if given)
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

# CORNER k: pool entry k from the record's vertex cache, for corner k of the
# polygon at a1 (t7 = T2, t9 = 2045, s2 = SP_SZCLIP): SXY, z, z T2, u and v
# (x 256), the packet uv word, the grey colour word; bit k of s1 set when the
# GTE could not project it (SZ < SP_SZCLIP, or x / y saturated: outside
# -1023..1022).
# Its key stays that of a corner: entries 0..2 are never midpoints (their
# key word is never written), the caller clears entry 3's. Clobbers t0-t5.
.macro CORNER k
	lbu     $t1, \k($a1)                 # slot x 4
	lhu     $t3, (4 + 2 * \k)($a1)       # uv
	addu    $t1, $t1, $t8
	lw      $t5, SP_SZ($t1)
	lw      $t4, SP_SXY($t1)
	lw      $t1, SP_COL($t1)             # grey colour, light in its low byte
	mult    $t5, $t7                     # z T2
	sw      $t5, (32 * \k + PV_Z)($v1)
	sw      $t4, (32 * \k + PV_SXY)($v1)
	sw      $t1, (32 * \k + PV_GC)($v1)
	andi    $t0, $t3, 0xff
	sll     $t0, $t0, 8                  # u x 256
	andi    $t2, $t3, 0xff00             # v x 256
	sw      $t0, (32 * \k + PV_U)($v1)
	sw      $t2, (32 * \k + PV_V)($v1)
	sw      $t3, (32 * \k + PV_UVW)($v1) # (u x 256 + 128) >> 8 = the byte
	sll     $t0, $t4, 16
	sra     $t0, $t0, 16                 # x
	addiu   $t0, $t0, 1023
	sltu    $t0, $t9, $t0                # 1: x off range (unsigned x + 1023 > 2045)
	sra     $t1, $t4, 16                 # y
	addiu   $t1, $t1, 1023
	sltu    $t1, $t9, $t1                # 1: y off range
	or      $t0, $t0, $t1
	slt     $t2, $t5, $s2                # 1: too near (SP_SZCLIP)
	or      $t2, $t2, $t0
.if \k
	sll     $t2, $t2, \k
.endif
	or      $s1, $s1, $t2
	mflo    $t0
	sw      $t0, (32 * \k + PV_ZT)($v1)
.endm

# MIDLOOK dst: dst = the pool index of the midpoint of entries t0, t1
# (indices), found through the table or made by sm_new (the table slot is
# written in the call's delay slot; sm_new's first instruction is done
# here, in the table load's slot). Clobbers at, v0, t0-t7, t9, a0-a2.
.macro MIDLOOK dst
	sltu    $at, $t0, $t1                # key: lower << 9 | higher
	bnez    $at, 91f
	sll     $t2, $t0, 9
	xor     $t0, $t0, $t1
	xor     $t1, $t0, $t1
	xor     $t0, $t0, $t1
	sll     $t2, $t0, 9
91:	or      $t2, $t2, $t1
	srl     $t3, $t2, 4
	xor     $t3, $t3, $t2
	andi    $t3, $t3, HASH_N - 1         # slot
	sll     $t3, $t3, 1
	addu    $t6, $t3, $v1                # (pool-relative)
	lhu     $v0, HASH_OFS($t6)           # the index it holds
	sll     $t0, $t0, 5                  # (sm_new's first: p's offset)
	sltu    $at, $v0, $s4                # one of this polygon's?
	beqz    $at, 92f
	sll     $t4, $v0, 5
	addu    $t4, $t4, $v1
	lw      $t4, PV_KEY($t4)
	move    \dst, $v0                    # (if it is the one)
	beq     $t4, $t2, 93f                # found
	nop
92:	jal     sm_new_1                     # else made: v0 = its index
	sh      $s4, HASH_OFS($t6)           # (the table: this edge -> n)
	move    \dst, $v0
93:
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
	lw      $s2, SP_SZCLIP($t8)          # (CORNER's near bound)
	lui     $v1, %hi(r_slow_pool)
	addiu   $v1, $v1, %lo(r_slow_pool)   # v1 = pool, for the whole call

	# ---- 1. corners (PS1 order) from the cache ---------------------------
	move    $s1, $zero                   # clip mask: bit k = corner k off range
	li      $t9, 2045                    # (CORNER's range bound)
	CORNER  0
	CORNER  1
	CORNER  2
	li      $t0, 3
	beq     $a2, $t0, sp_tri0
	lui     $a3, %hi(r_slow_stack + 4)
	CORNER  3
	sw      $zero, (3 * 32 + PV_KEY)($v1)    # a corner, not a stale midpoint
	addiu   $a3, $a3, %lo(r_slow_stack + 4)  # a3 = the first free stack word
	bnez    $s1, sp_clip
	li      $s4, 4                       # (pool: the corners)

	# ---- 2. straight from the cache: (1, 3, 2) under (0, 1, 2), depth 0 ---
	li      $t0, 2 << 18 | 3 << 9 | 1
	li      $t1, 2 << 18 | 1 << 9 | 0
	sw      $t0, 0($a3)
	sw      $t1, 4($a3)
	b       sp_sub
	addiu   $a3, $a3, 8
sp_tri0:                                 # a triangle: (0, 1, 2)
	addiu   $a3, $a3, %lo(r_slow_stack + 4)
	bnez    $s1, sp_clip
	li      $s4, 3
	li      $t1, 2 << 18 | 1 << 9 | 0
	sw      $t1, 0($a3)
	b       sp_sub
	addiu   $a3, $a3, 4

	# ---- 3. clip path ---------------------------------------------------------
	# Corners to view space in ring order (a quad: 0, 1, 3, 2); a corner the
	# cache holds keeps its SZ as z. Then the near plane. A pool entry's view
	# x, y, z (entries 0..5) sit at VIEW_OFS from it.
sp_clip:
	lw      $t9, SP_VERTPOOL($t8)
	lw      $s0, SP_ZNEAR($t8)           # (s0 = the near plane through the clip)
	move    $t2, $v1                     # pool[j]
	move    $t6, $zero                   # j: corner (PS1 order)
	move    $s3, $zero                   # behind count
	lbu     $t0, 0($a1)                  # corner 0's slot x 4
1:	srlv    $t1, $s1, $t6
	srl     $t0, $t0, 1                  # slot x 2: its pool-index offset
	addu    $t0, $t0, $a0
	lhu     $t0, R_VERTS($t0)            # pool index
	andi    $t1, $t1, 1                  # 1: not from the cache
	sll     $t0, $t0, 3
	addu    $t0, $t0, $t9
	lwc2    C2_VXY0, 0($t0)
	lwc2    C2_VZ0,  4($t0)
	addiu   $t6, $t6, 1
	lw      $t5, PV_Z($t2)               # (a cached corner's z: its SZ)
	MVMVA   1, 0, 0, 0, 0                # MAC = rot v + TR (>> 12)
	addu    $t0, $a1, $t6
	lbu     $t0, 0($t0)                  # (the next corner's slot; after the
	addiu   $t2, $t2, 32                 #  last: harmless)
	mfc2    $t3, C2_MAC1
	mfc2    $t4, C2_MAC2
	beqz    $t1, 2f                      # from the cache: z = its SZ
	sw      $t3, VIEW_OFS-32+0($t2)      # view x
	mfc2    $t5, C2_MAC3                 # else the view z
2:	sw      $t4, VIEW_OFS-32+4($t2)      # view y
	sw      $t5, VIEW_OFS-32+8($t2)      # view z
	slt     $t3, $t5, $s0
	bne     $t6, $a2, 1b
	addu    $s3, $s3, $t3
	beq     $s3, $a2, sp_done            # all behind the near plane
	li      $t0, 0x0100                  # (ring: 0, 1)

	# the ring (pool indices) in SP_PLA, n in s2; ring[n] = ring[0] = 0
	li      $t1, 4
	beq     $a2, $t1, 3f
	lui     $t1, 0x0203                  # quad ring: 0, 1, 3, 2
	lui     $t1, 0x0002                  # triangle ring: 0, 1, 2 (then 0)
3:	or      $t0, $t0, $t1
	sw      $t0, SP_PLA($t8)
	sw      $zero, SP_PLA+4($t8)         # (the quad's ring[4])
	move    $s2, $a2
	beqz    $s3, sp_project
	addiu   $a0, $t8, SP_PLA             # a0 = the list to fan

	# Sutherland-Hodgman against z >= SP_ZNEAR (s0): SP_PLA -> SP_PLB, crossings
	# appended to the pool (sc_cross)
	addiu   $a1, $t8, SP_PLB             # output cursor
	lbu     $t2, 0($a0)                  # a = ring[0]
	addu    $t6, $a0, $s2                # the ring's end
	sll     $t4, $t2, 5
	addu    $t4, $t4, $v1                # pool[a]
	lw      $t0, VIEW_OFS+8($t4)         # za
	move    $s5, $a0                     # cursor
	subu    $t0, $t0, $s0                # da
sc_edge:                                 # a: t2 index, t4 entry, t0 da
	lbu     $t3, 1($s5)                  # b
	addiu   $s5, $s5, 1
	sll     $t5, $t3, 5
	addu    $t5, $t5, $v1                # pool[b]
	lw      $t1, VIEW_OFS+8($t5)         # zb
	bltz    $t0, 5f                      # a behind the plane: dropped
	sb      $t2, 0($a1)                  # (else kept)
	addiu   $a1, $a1, 1
5:	subu    $t1, $t1, $s0                # db
	xor     $at, $t0, $t1
	bltz    $at, sc_cross                # the ends on both sides: a crossing
	move    $t7, $t4                     # (from a, if a is behind)
sc_next:
	move    $t2, $t3                     # b: the next edge's a
	move    $t4, $t5
	bne     $s5, $t6, sc_edge
	move    $t0, $t1
	addiu   $a0, $t8, SP_PLB
	subu    $s2, $a1, $a0                # n

	# project what the cache does not hold: clamp z, divide; fill the entry.
	# t7: bit i set for pool index i to project (the corners not from the
	# cache, every crossing)
sp_project:
	li      $t7, -1
	sllv    $t7, $t7, $a2
	or      $t7, $t7, $s1
	addu    $t6, $a0, $s2                # the list's end
	move    $s5, $a0
	lbu     $t0, 0($s5)                  # pool index
8:	addiu   $s5, $s5, 1
	srlv    $t1, $t7, $t0
	andi    $t1, $t1, 1
	beqz    $t1, 9f                      # from the cache: as it is
	sll     $t0, $t0, 5                  # (sp_proj's first two)
	jal     sp_proj_2
	addu    $t0, $t0, $v1
9:	bne     $s5, $t6, 8b
	lbu     $t0, 0($s5)                  # (the next; after the last: harmless)

	# fan from the first vertex, depth 0: push the last triangle first
	lbu     $t0, 0($a0)                  # l[0]
	addiu   $t6, $s2, -2                 # i = n - 2 .. 1
	addu    $t3, $a0, $t6
10:	lbu     $t1, 0($t3)                  # l[i]
	lbu     $t2, 1($t3)                  # l[i + 1]
	addiu   $t3, $t3, -1
	sll     $v0, $t1, 9
	sll     $at, $t2, 18
	or      $v0, $v0, $t0
	or      $v0, $v0, $at
	sw      $v0, 0($a3)
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

	# split decisions (none at max depth)
	sll     $at, $s2, 1                  # depth 8 (MAXD): bit 30
	bltz    $at, sp_leaf_max
	subu    $s3, $t0, $t3                # (the first test's x0 - x1)
	NOSPLIT $s3, $t0, $t1, $t2, $s0, $t3, $t4, $t5, $s5, 0, "subu $at, $t3, $t6"   # p0-p1
	NOSPLIT $at, $t3, $t4, $t5, $s5, $t6, $t7, $s1, $ra, 0   # p1-p2
	sll     $at, $at, 1
	or      $s3, $s3, $at
	NOSPLIT $at, $t6, $t7, $s1, $ra, $t0, $t1, $t2, $s0, 1, "sltu $t2, $s7, $s6"   # p2-p0
	sll     $at, $at, 2                  # (t2: sp_emit's arena test, r_frame
	or      $s3, $s3, $at                #  leaves 40 bytes over s7 for it)
	xori    $s3, $s3, 7                  # split: bit 0 p0-p1, 1 p1-p2, 2 p2-p0
	bnez    $s3, sp_split
	lui     $t9, CODE_GT3 >> 16          # (sp_emit's; sp_split's t9 is its own)

	# ---- a leaf: one POLY_GT3 (a0 / a1 / a2 = its pool entries; t2 = 1 if
	# the arena is full, t9 = the command) ---------------------------------------
sp_emit:
	bnez    $t2, sp_full
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
	b       sp_sub
	addiu   $s6, $s6, 40

	# ---- split: the midpoints, then the children ---------------------------
sp_split:                                # s3 = mask, s2 = the triangle
	sltiu   $at, $s4, POOL_N - 2         # the pool nearly full: a forced leaf
	beqz    $at, sp_leaf_max
	andi    $v0, $s3, 1                  # (harmless there)
	beqz    $v0, 1f
	andi    $t0, $s2, 0x1ff              # (i0)
	srl     $t1, $s2, 9
	andi    $t1, $t1, 0x1ff              # i1
	MIDLOOK $s0                          # s0 = m01
1:	andi    $v0, $s3, 2
	beqz    $v0, 2f
	srl     $t0, $s2, 9
	andi    $t0, $t0, 0x1ff              # i1
	srl     $t1, $s2, 18
	andi    $t1, $t1, 0x1ff              # i2
	MIDLOOK $s5                          # s5 = m12
2:	andi    $v0, $s3, 4
	beqz    $v0, 3f
	srl     $t0, $s2, 18
	andi    $t0, $t0, 0x1ff              # i2
	andi    $t1, $s2, 0x1ff              # i0
	MIDLOOK $s1                          # s1 = m20
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
	beqz    $t9, sp_sub
	sltu    $t2, $s7, $s6                # (sp_emit's arena test)
	b       sp_emit
	lui     $t9, CODE_GT3 >> 16

	# a near-plane crossing (sc_edge), interpolated from its end behind the
	# plane: r_slow.c before() compares z first, and a crossing's two ends
	# differ in z, so both polygons sharing the edge make the same point
sc_cross:
	move    $t9, $t5                     # to b
	move    $at, $t0                     # num: da
	bltz    $t0, 7f                      # a behind: t = da / (da - db)
	subu    $v0, $t0, $t1
	move    $t7, $t5                     # else from b: t = db / (db - da)
	move    $t9, $t4
	move    $at, $t1
	subu    $v0, $zero, $v0
7:	jal     sp_lerp_1
	sll     $at, $at, 14                 # (sp_lerp's first)
	sb      $v0, 0($a1)
	b       sc_next
	addiu   $a1, $a1, 1

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
# sm_new: a new vertex at index n = s4, the projected 3D midpoint of pool
# entries t0, t1 (indices; t2 = their key). v0 = n. Clobbers at, t0-t7, t9,
# a0-a2 (s-registers, v1, a3 kept; s4 grows by one). MIDLOOK enters at
# sm_new_1, its first instruction done.
# Scheduled around HI / LO: the four products, then the two divisions with
# the u, v and light work in their shadows (36 cycles each). z T2 needs no
# multiply: the pool holds pz T2 and qz T2, T2 is even, so
# T2 (zs >> 1) = (pz T2 + qz T2) / 2 - (zs & 1) T, exactly.
sm_new:
	sll     $t0, $t0, 5
sm_new_1:
	addu    $a0, $t0, $v1                # p
	lh      $t0, PV_SXY($a0)             # px
	lw      $t3, PV_Z($a0)               # pz
	sll     $t1, $t1, 5
	addu    $a1, $t1, $v1                # q
	mult    $t0, $t3                     # px pz
	lh      $t1, PV_SXY($a1)             # qx
	lw      $t5, PV_Z($a1)               # qz
	sll     $a2, $s4, 5
	addu    $a2, $a2, $v1                # m
	mflo    $t0
	addu    $t6, $t3, $t5                # zs
	sw      $t2, PV_KEY($a2)
	mult    $t1, $t5                     # qx qz
	srl     $t7, $t6, 1                  # z = zs >> 1
	sll     $t9, $t6, 13
	addu    $t9, $t9, $t7                # zs KOFS + zs / 2: the rounding offset
	lh      $at, PV_SXY+2($a0)           # py
	mflo    $t1
	addu    $t0, $t0, $t9
	sw      $t7, PV_Z($a2)
	mult    $at, $t3                     # py pz
	addu    $t0, $t0, $t1                # px pz + qx qz + offset
	lh      $t1, PV_SXY+2($a1)           # qy
	lw      $t3, PV_ZT($a0)              # pz T2
	mflo    $at
	lw      $t7, PV_ZT($a1)              # qz T2
	addu    $at, $at, $t9
	mult    $t1, $t5                     # qy qz
	lw      $t9, SP_T2($t8)
	addu    $t3, $t3, $t7                # T2 zs
	srl     $t3, $t3, 1                  # T zs
	mflo    $t1
	sll     $t5, $t6, 31
	sra     $t5, $t5, 31                 # -1 if zs is odd
	divu    $zero, $t0, $t6              # x + KOFS (while it divides: z T2, u, v)
	addu    $at, $at, $t1                # py pz + qy qz + offset
	srl     $t9, $t9, 1                  # T
	and     $t5, $t5, $t9
	subu    $t3, $t3, $t5                # z T2
	sw      $t3, PV_ZT($a2)
	lw      $t1, PV_U($a0)
	lw      $t3, PV_U($a1)
	lw      $t5, PV_V($a0)
	lw      $t7, PV_V($a1)
	addu    $t1, $t1, $t3
	sra     $t1, $t1, 1                  # u
	addu    $t5, $t5, $t7
	sra     $t5, $t5, 1                  # v
	sw      $t1, PV_U($a2)
	sw      $t5, PV_V($a2)
	addiu   $t1, $t1, 128                # uv word: rounded texels
	srl     $t1, $t1, 8
	addiu   $t5, $t5, 128
	mflo    $t0                          # x + KOFS
	andi    $t5, $t5, 0xff00
	or      $t1, $t1, $t5
	divu    $zero, $at, $t6              # y + KOFS (while it divides: x, light)
	sw      $t1, PV_UVW($a2)
	addiu   $t0, $t0, -KOFS
	sh      $t0, PV_SXY($a2)             # x
	lw      $t1, PV_GC($a0)
	lw      $t3, PV_GC($a1)
	addiu   $s4, $s4, 1
	andi    $t1, $t1, 0xff
	andi    $t3, $t3, 0xff
	addu    $t1, $t1, $t3
	srl     $t1, $t1, 1                  # light
	sll     $t3, $t1, 8                  # grey c -> 0x00cccccc
	or      $t1, $t1, $t3
	sll     $t3, $t1, 8
	or      $t1, $t1, $t3
	sw      $t1, PV_GC($a2)
	addiu   $v0, $s4, -1                 # its index
	mflo    $t1                          # y + KOFS
	addiu   $t1, $t1, -KOFS
	jr      $ra
	sh      $t1, PV_SXY+2($a2)           # y

# -----------------------------------------------------------------------------
# sp_lerp: a near-plane crossing from pool entry t7 toward t9 (their view
# entries at VIEW_OFS): t = (at << 14) / v0, each of view x, y, z and u, v,
# light moved by (to - from) t >> 14. Appended to the pool (its view entry
# too); v0 = its index. The six products go one after the other, the next
# field's loads and the last one's store in each one's shadow (four
# registers in turn: at / s0 the from value, s3 the difference, v0 the
# result). Clobbers at, t0, t2, t4, t7, t9, s0, s3 (sc_edge's a: dead by
# then). Keeps t1, t3, t5, t6, a*. sc_cross enters at sp_lerp_1.
sp_lerp:
	sll     $at, $at, 14
sp_lerp_1:
	div     $zero, $at, $v0              # t
	sll     $t0, $s4, 5
	addu    $t0, $t0, $v1                # pool[n]
	lw      $at, VIEW_OFS+0($t7)         # view x
	lw      $s3, VIEW_OFS+0($t9)
	sw      $zero, PV_KEY($t0)           # not a midpoint
	subu    $s3, $s3, $at
	mflo    $t2                          # t
	lw      $t4, VIEW_OFS+4($t7)         # view y
	addiu   $s4, $s4, 1
	mult    $s3, $t2                     # x
	lw      $s3, VIEW_OFS+4($t9)
	nop
	nop
	mflo    $v0
	subu    $s3, $s3, $t4
	sra     $v0, $v0, 14
	mult    $s3, $t2                     # y
	addu    $v0, $v0, $at
	sw      $v0, VIEW_OFS+0($t0)         # x
	lw      $at, VIEW_OFS+8($t7)         # view z
	lw      $s3, VIEW_OFS+8($t9)
	mflo    $v0
	subu    $s3, $s3, $at
	sra     $v0, $v0, 14
	mult    $s3, $t2                     # z
	addu    $v0, $v0, $t4
	sw      $v0, VIEW_OFS+4($t0)         # y
	lw      $t4, PV_U($t7)               # u
	lw      $s3, PV_U($t9)
	mflo    $v0
	subu    $s3, $s3, $t4
	sra     $v0, $v0, 14
	mult    $s3, $t2                     # u
	addu    $v0, $v0, $at
	sw      $v0, VIEW_OFS+8($t0)         # z
	lw      $at, PV_V($t7)               # v
	lw      $s3, PV_V($t9)
	mflo    $v0
	subu    $s3, $s3, $at
	sra     $v0, $v0, 14
	mult    $s3, $t2                     # v
	addu    $v0, $v0, $t4
	sw      $v0, PV_U($t0)               # u
	lw      $t4, PV_GC($t7)              # light: the grey's low byte
	lw      $s3, PV_GC($t9)
	mflo    $v0
	andi    $t4, $t4, 0xff
	andi    $s3, $s3, 0xff
	subu    $s3, $s3, $t4
	sra     $v0, $v0, 14
	mult    $s3, $t2                     # light
	addu    $v0, $v0, $at
	sw      $v0, PV_V($t0)               # v
	nop
	nop
	mflo    $v0
	sra     $v0, $v0, 14
	addu    $v0, $v0, $t4                # light
	sll     $at, $v0, 8                  # grey c -> 0x00cccccc
	or      $v0, $v0, $at
	sll     $at, $v0, 8
	or      $v0, $v0, $at
	sw      $v0, PV_GC($t0)
	jr      $ra
	addiu   $v0, $s4, -1                 # its index

# -----------------------------------------------------------------------------
# sp_proj: fill pool entry t0 (an index; at sp_proj_2, its address) from its
# view entry (VIEW_OFS): z clamped to the near plane, sx = OFX + x H / z,
# sy = OFY + y H / z (H = 5 << SP_HSHIFT; OFY: SP_OFY), z T2, uv word. Clobbers at,
# t0-t5. Keeps t6, t7, a*, s*.
sp_proj:
	sll     $t0, $t0, 5
	addu    $t0, $t0, $v1                # pool[i]
sp_proj_2:
	lw      $t4, VIEW_OFS+8($t0)         # z
	lw      $t5, SP_T2($t8)
	lw      $t2, VIEW_OFS+0($t0)         # x
	slt     $t1, $t4, $s0
	beqz    $t1, 1f
	lw      $t3, VIEW_OFS+4($t0)         # y
	move    $t4, $s0                     # (the lerp's rounding; s0 = SP_ZNEAR)
1:	mult    $t5, $t4                     # z T2 (T2 as rs: the short multiply)
	lw      $t1, SP_HSHIFT($t8)
	sll     $at, $t2, 2
	addu    $t2, $t2, $at                # x H = 5 x << shift
	mflo    $t5
	sllv    $t2, $t2, $t1
	sll     $at, $t3, 2
	addu    $t3, $t3, $at                # y H
	sllv    $t3, $t3, $t1
	sw      $t5, PV_ZT($t0)
	sw      $t4, PV_Z($t0)
	div     $zero, $t2, $t4              # x H / z (while it divides: the uv word)
	lw      $t1, PV_U($t0)
	lw      $t5, PV_V($t0)
	addiu   $t1, $t1, 128
	srl     $t1, $t1, 8
	addiu   $t5, $t5, 128
	andi    $t5, $t5, 0xff00
	or      $t1, $t1, $t5
	sw      $t1, PV_UVW($t0)
	lw      $t5, SP_OFY($t8)             # (the viewport's centre: sy below)
	mflo    $t2
	addiu   $t2, $t2, 160                # sx
	andi    $t2, $t2, 0xffff
	div     $zero, $t3, $t4              # y H / z
	mflo    $t3
	addu    $t3, $t3, $t5                # sy
	sll     $t3, $t3, 16
	or      $t2, $t2, $t3
	jr      $ra
	sw      $t2, PV_SXY($t0)

	.size slow_poly, . - slow_poly
