# q1psx — the near polygons' regular subdivision, hand-written MIPS assembly
# (docs/porting-notes.md §92). Where the record loop sends a polygon to the
# slow path (engine/r_world.s POLYS: CAREFUL's exits), it comes here first
# on the maps that take it (render.c r_tess_select patches the call):
#
#   tess_poly(a0 = face record, a1 = quad / tri, a2 = 4 or 3)
#
# the same contract as slow_poly's (engine/r_slow.s): advances s6 and fp,
# keeps s0-s7, gp, sp, t8; at, v0, v1, a0-a3, t0-t9 are the caller's to
# save.
#
# The slow path subdivides a polygon triangle by triangle, each edge tested
# and split until the affine error is under the threshold: about 950
# cycles a triangle emitted (§79, §81). Most of its polygons need no near
# clip: they are big on the screen, or deep. For those, the level a
# uniform subdivision needs is known from the polygon's corners alone,
# the slow path's own edge test: an edge of screen length |ds| (the
# octagon's max + 3/8 min of |dx|, |dy|) and depth range |dz| splits while
# |ds| x |dz| > T2 (za + zb); halving it m times halves both m times, and
# the half at the nearer end, zm deep, then has za + zb = 2 zm + |dz| /
# 2^m (the others more), so the level an edge needs is the first m with
# |ds| x |dz| <= 4^m x 2 T2 zm + 2^m x T2 |dz| (and |dx| + |dy| <= 500 x
# 2^m, the GPU's span). The polygon is cut along its rows at the level its
# top and bottom edges need (nu = 2^ku columns) and down its columns at
# the level its left and right edges need (nv = 2^kv rows): a wall seen
# along its length is cut along it alone; the diagonal the GPU draws
# (corners 1-2) raises the larger of the two levels to its own. The grid's
# points are interpolated in the world (midpoints along the edges, then
# along each row, each the mean of two points, the same two from either
# side: a shared edge gets the same points from both polygons at one
# level), projected by the GTE three at a time (RTPT, as the record's
# vertices), and every cell emitted as a POLY_GT4 (the last row of a
# triangle, cut as a quad whose fourth corner is its third, as POLY_GT3s),
# off-screen cells left out, none once the arena is full (a cell goes out
# only while it ends at most 40 bytes past the bound, the slow path's own
# overshoot). The work follows the screen as the slow path's does (it
# drops a triangle off the screen before splitting it): the two columns
# are made and projected first, then strip by strip, a strip whose four
# column points are off the screen on one side is skipped whole, the
# others get their rows' inner points made and projected as they come
# (a row's right column point's pad word marks it done). No test per
# cell, no division.
#
# A polygon whose five edges pass at level 0 is what the slow path emits
# unsplit, as its two triangles: it goes out as one packet straight from
# the cache, the same pixels (the GPU draws a quad as those two
# triangles), without the slow path's stack and tests.
#
# To slow_poly, as before: a corner nearer than SP_SZCLIP or projected
# off the GTE's range (the near clip), a level past KMAX (a polygon right
# under the eye: its grid would be too fine for the packet arena; KMAX 0
# sends it every split polygon), and levels summing past CAP (more than
# 2^CAP cells: the slow path's splits, which follow the near corner alone,
# are then the fewer; measured, §92).
#
# The grid lives in r_slow.s's pool from its entry 256 (the slow path's
# midpoint indices stay far below, so its hash never believes a slot
# pointing there), a point 32 bytes:
#   +0 x, +2 y, +4 z (halfwords, vertex units), +6 0, +8 u, +10 v (texels
#   x 256), +12 light, +16 the grey colour word, +20 the packet's uv word,
#   +24 SXY, +28 SZ (the GTE's)
# row j, column i at base + j x RS + i x 32, RS = 32 x (nu + 1).
#
# The code is kept small for the instruction cache (§81.5): the edge test,
# a corner's load, a line's midpoints and a line's projection are
# subroutines (tess_lvl, tess_corner, tess_line, tess_proj: a0-a3, v0,
# v1, t0-t7, t9, at between them as each says).

	.set noreorder
	.set noat

	.include "gtereg.inc"
	.include "inline_s.inc"
	.include "r_defs.inc"

.ifndef TESS_KMAX                        # the finest level, 2^KMAX cells along an axis
.set TESS_KMAX, 3                        #  (CMake Q1PSX_TESS_KMAX: --defsym; 0: the grid
.endif                                   #  never, the unsplit polygons alone)
.set KMAX,     TESS_KMAX
.ifndef TESS_CAP                         # the cap: a polygon whose levels sum past it (more
.set TESS_CAP, 4                         #  than 2^CAP cells) goes to the slow path, whose
.endif                                   #  splits follow the near corner alone (CMake
                                         #  Q1PSX_TESS_CAP)
.set TP_SIZE,  32
.set TP_U,     8
.set TP_V,     10
.set TP_C,     12
.set TP_GC,    16
.set TP_UVW,   20
.set TP_SXY,   24
.set TP_SZ,    28
.set TG_OFS,   8192                      # the grid's place in the pool: its entries from
                                         #  256 (slow_poly's midpoints never index so far)
.set SPAN,     500

	.section .bss.r_tess_stat, "aw", @nobits
	.align  2
	.global r_tess_stat
r_tess_stat:                             # the frame's (R_PROFILE): polygons cut, their cells,
	.space  24                           # to the slow path near, to it too fine, unsplit (one
                                         # packet), to the slow path over the cap

# TSTAT k: counter k plus one (R_PROFILE builds); clobbers at, t9
.macro TSTAT k
.if R_PROFILE
	lui     $t9, %hi(r_tess_stat + 4 * \k)
	lw      $at, %lo(r_tess_stat + 4 * \k)($t9)
	nop
	addiu   $at, $at, 1
	sw      $at, %lo(r_tess_stat + 4 * \k)($t9)
.endif
.endm

# TSTATADD k, r: counter k plus register \r (not at, t9); clobbers at, t9
.macro TSTATADD k, r
.if R_PROFILE
	lui     $t9, %hi(r_tess_stat + 4 * \k)
	lw      $at, %lo(r_tess_stat + 4 * \k)($t9)
	nop
	addu    $at, $at, \r
	sw      $at, %lo(r_tess_stat + 4 * \k)($t9)
.endif
.endm

# MID a, b, m: the point between a and b into m (the mean of each field,
# rounded down: the same from either side). Clobbers t0, t1, a1, at.
.macro MID a, b, m
	lh      $t0, 0(\a)                   # x
	lh      $t1, 0(\b)
	lh      $a1, 2(\a)                   # y
	lh      $at, 2(\b)
	addu    $t0, $t0, $t1
	sra     $t0, $t0, 1
	addu    $a1, $a1, $at
	sh      $t0, 0(\m)
	sra     $a1, $a1, 1
	lh      $t0, 4(\a)                   # z
	lh      $t1, 4(\b)
	sh      $a1, 2(\m)
	addu    $t0, $t0, $t1
	sra     $t0, $t0, 1
	andi    $t0, $t0, 0xffff
	sw      $t0, 4(\m)                   # (its pad 0: one store)
	lhu     $t0, TP_U(\a)                # u, v (x 256)
	lhu     $t1, TP_U(\b)
	lhu     $a1, TP_V(\a)
	lhu     $at, TP_V(\b)
	addu    $t0, $t0, $t1
	srl     $t0, $t0, 1
	addu    $a1, $a1, $at
	srl     $a1, $a1, 1
	sh      $t0, TP_U(\m)
	sh      $a1, TP_V(\m)
	addiu   $t0, $t0, 128                # the packet's uv word: u + 128 >> 8,
	srl     $t0, $t0, 8                  #  v + 128 & 0xff00
	addiu   $a1, $a1, 128
	andi    $a1, $a1, 0xff00
	or      $t0, $t0, $a1
	lhu     $at, TP_C(\a)                # the light
	lhu     $t1, TP_C(\b)
	sw      $t0, TP_UVW(\m)
	addu    $t0, $at, $t1
	srl     $t0, $t0, 1
	sh      $t0, TP_C(\m)
	sll     $t1, $t0, 8                  # grey: c | c << 8 | c << 16
	or      $t0, $t0, $t1
	sll     $t1, $t0, 8
	or      $t0, $t0, $t1
	sw      $t0, TP_GC(\m)
.endm

# TRANGE xy, y [, x]: a corner's packed xy as x (into xy itself, or x) and
# y, each within the GTE's range, else the slow path's clip (the last
# branch's delay slot is the caller's). Clobbers at, v0.
.macro TRANGE xy, y, x
	sra     \y, \xy, 16                  # y
.ifb \x
	sll     \xy, \xy, 16
	sra     \xy, \xy, 16                 # x
	addiu   $at, \xy, 1023               # within -1023 .. 1022 (r_slow.c): past
.else
	sll     \x, \xy, 16                  #  them the GTE saturated it
	sra     \x, \x, 16
	addiu   $at, \x, 1023
.endif
	sltiu   $at, $at, 2046
	beqz    $at, tess_near
	addiu   $v0, \y, 1023
	sltiu   $v0, $v0, 2046
	beqz    $v0, tess_near
.endm

# EDGE xa, ya, za, xb, yb, zb, k [, max]: the level edge (a, b) needs (the
# header's rule) into k, which the caller cleared (max = 1: k raised to it,
# a level of an edge before); the test at level 0 here, the slow path's own
# (|ds| |dz| <= T2 (za + zb), |dx| + |dy| <= SPAN): most edges pass it,
# k left alone; the levels past it in tess_lvl (KMAX + 1: finer than
# KMAX). t6 = T2. Clobbers at, v0, v1, a3, t7, t9, ra.
.macro EDGE xa, ya, za, xb, yb, zb, k, max=0
	subu    $at, \xa, \xb                # dx
	subu    $v0, \ya, \yb                # dy
	bgez    $at, 1f
	subu    $v1, \za, \zb                # dz
	subu    $at, $zero, $at              # |dx|
1:	bgez    $v0, 2f
	addu    $t9, \za, \zb                # za + zb
	subu    $v0, $zero, $v0              # |dy|
2:	mult    $t6, $t9                     # T2 (za + zb): the bound at level 0
	bgez    $v1, 3f
	addu    $a3, $at, $v0                # |dx| + |dy|: the span
	subu    $v1, $zero, $v1              # |dz|
3:	sltu    $t7, $at, $v0                # the octagon: max + 3/8 min of |dx|, |dy|
	beqz    $t7, 4f
	move    $t7, $v0                     # (min |dy|, |dx| the max)
	move    $t7, $at                     # min |dx|,
	move    $at, $v0                     #  max |dy|
4:	sll     $v0, $t7, 1
	addu    $v0, $v0, $t7
	mflo    $t9                          # T2 (za + zb)
	srl     $v0, $v0, 3
	addu    $at, $at, $v0                # |ds|
	multu   $at, $v1                     # |ds| |dz|
	sltiu   $v0, $a3, SPAN + 1           # 1: within the span
	move    $v1, \za                     # (tess_lvl's)
	mflo    $t7                          # |ds| |dz|
	sltu    $at, $t9, $t7                # 1: the warp over the bound
	sltu    $at, $at, $v0                # 1: neither: level 0
	bnez    $at, 5f
	move    $v0, \zb                     # (tess_lvl's)
	jal     tess_lvl_1                   # past level 0: v0 = the level
	slt     $at, $v0, $v1                # (tess_lvl's first)
.if \max
	slt     $at, \k, $v0
	beqz    $at, 5f
	nop
.endif
	move    \k, $v0
5:
.endm

# TBOUNDS: the cells' registers: v0 = clut << 16, v1 = tpage << 16, a1 =
# the width << 16, a0 = the rows << 16 (the off-screen test's bounds).
# Clobbers t9.
.macro TBOUNDS
	lw      $v0, SP_CLUT16($t8)
	lw      $v1, SP_TPAGE16($t8)
	lui     $t9, %hi(r_res_w)
	lw      $a1, %lo(r_res_w)($t9)
	lui     $t9, %hi(r_res_h)
	lw      $a0, %lo(r_res_h)($t9)
	nop
	sll     $a1, $a1, 16
	sll     $a0, $a0, 16
.endm

# OFF4 lbl: to \lbl when the four screen positions t0, t1, t2, t7 are all
# off the screen on one side (the last branch's delay slot is the
# caller's). Clobbers t9, ra.
.macro OFF4 lbl
	and     $t9, $t0, $t1
	and     $t9, $t9, $t2
	and     $t9, $t9, $t7
	bltz    $t9, \lbl                    # all y < 0
	andi    $t9, $t9, 0x8000
	bnez    $t9, \lbl                    # all x < 0
	subu    $t9, $t0, $a0
	subu    $ra, $t1, $a0
	or      $t9, $t9, $ra
	subu    $ra, $t2, $a0
	or      $t9, $t9, $ra
	subu    $ra, $t7, $a0
	or      $t9, $t9, $ra
	bgez    $t9, \lbl                    # all y >= the rows
	sll     $t9, $t0, 16
	subu    $t9, $t9, $a1
	sll     $ra, $t1, 16
	subu    $ra, $ra, $a1
	or      $t9, $t9, $ra
	sll     $ra, $t2, 16
	subu    $ra, $ra, $a1
	or      $t9, $t9, $ra
	sll     $ra, $t7, 16
	subu    $ra, $ra, $a1
	or      $t9, $t9, $ra
	bgez    $t9, \lbl                    # all x >= the width
.endm

	ICSEC   tess_poly                    # (the I-cache's chains: r_defs.inc ICSEC)
	.global tess_poly
	.type   tess_poly, @function
tess_poly:
	sw      $ra, SP_SPRA($t8)            # (slow_poly's own slots: it is not in a call)
	sw      $s0, SP_SV+0($t8)
	sw      $s1, SP_SV+4($t8)
	sw      $s2, SP_SV+8($t8)
	sw      $s3, SP_SV+12($t8)
	sw      $s4, SP_SV+16($t8)
	sw      $s5, SP_SV+20($t8)
	lw      $t0, 0($a1)                  # the corners' cache offsets (a byte each)
	lw      $v1, SP_SZCLIP($t8)          # (the near bound)
	andi    $t3, $t0, 0xff
	srl     $t4, $t0, 8
	andi    $t4, $t4, 0xff
	srl     $t5, $t0, 16
	andi    $t5, $t5, 0xff
	srl     $t6, $t0, 24
	addu    $t3, $t3, $t8
	addu    $t4, $t4, $t8
	addu    $t5, $t5, $t8
	li      $t9, 3
	bne     $a2, $t9, 1f
	addu    $t6, $t6, $t8
	move    $t6, $t5                     # a triangle: its third corner is the grid's fourth
1:
	# the corners' depths and screen positions: one too near or off the
	# GTE's range goes to the slow path (its near clip). The edges below
	# take every register: the corners' cache addresses wait in slow_poly's
	# lists (SP_PLA, SP_PLB: free until it runs), the record, the polygon
	# and its corner count in draw_records' saves (r_world.s RECLOOP); the
	# corners as x, y, z: 0 in s0-s2, 1 in s3-s5, 2 in t0-t2, 3 in t3-t5
	lw      $s2, SP_SZ($t3)              # z0
	lw      $s5, SP_SZ($t4)              # z1
	lw      $t2, SP_SZ($t5)              # z2
	lw      $t9, SP_SZ($t6)              # z3
	slt     $at, $s2, $v1
	bnez    $at, tess_near
	slt     $v0, $s5, $v1
	bnez    $v0, tess_near
	slt     $at, $t2, $v1
	bnez    $at, tess_near
	slt     $v0, $t9, $v1
	bnez    $v0, tess_near
	lw      $s0, SP_SXY($t3)             # xy0
	lw      $s3, SP_SXY($t4)             # xy1
	lw      $t0, SP_SXY($t5)             # xy2
	lw      $t7, SP_SXY($t6)             # xy3
	sw      $t3, SP_PLA+0($t8)
	sw      $t4, SP_PLA+4($t8)
	sw      $t5, SP_PLA+8($t8)
	sw      $t6, SP_PLB+0($t8)
	TRANGE  $s0, $s1
	TRANGE  $s3, $s4
	TRANGE  $t0, $t1
	TRANGE  $t7, $t4, $t3
	move    $t5, $t9                     # z3
	lw      $t6, SP_T2($t8)              # T2 (EDGE's)

	# the levels (EDGE): along the rows the top and bottom edges' greatest
	# (ku, a0), down the columns the left and right ones' (kv, a1); the
	# diagonal the GPU draws, corners 1-2 (kd, a2), raises the larger of
	# the two to its own. A triangle (its corner 3 is corner 2): its bottom
	# edge is a point, its right edge the diagonal
	move    $a0, $zero
	move    $a1, $zero
	move    $a2, $zero
	EDGE    $s0, $s1, $s2, $s3, $s4, $s5, $a0      # top: 0-1
	EDGE    $s0, $s1, $s2, $t0, $t1, $t2, $a1      # left: 0-2
	EDGE    $s3, $s4, $s5, $t0, $t1, $t2, $a2      # the diagonal: 1-2
	lw      $v0, SP_SAVE+32($t8)         # (draw_records' t0: 4 or 3)
	li      $at, 3
	bne     $v0, $at, .Ltp_quad
	slt     $at, $a1, $a2
	beqz    $at, .Ltp_levels             # a triangle: kv the left edge's or the
	nop                                  #  diagonal's, ku the top's
	b       .Ltp_levels
	move    $a1, $a2
.Ltp_quad:
	EDGE    $t0, $t1, $t2, $t3, $t4, $t5, $a0, 1   # bottom: 2-3
	EDGE    $s3, $s4, $s5, $t3, $t4, $t5, $a1, 1   # right: 1-3
	slt     $at, $a0, $a1                # ku < kv: the columns' axis is the larger
	bnez    $at, .Ltp_kv
	slt     $v0, $a1, $a2                # (kv < kd, for .Ltp_kv)
	slt     $v0, $a0, $a2                # ku < kd
	beqz    $v0, .Ltp_levels
	nop
	b       .Ltp_levels
	move    $a0, $a2
.Ltp_kv:
	beqz    $v0, .Ltp_levels
	nop
	move    $a1, $a2
.Ltp_levels:
	move    $s3, $a0                     # ku
	move    $s4, $a1                     # kv
	lw      $a0, SP_SAVE+4($t8)          # the record, the polygon, its corner count
	lw      $a1, SP_SAVE+16($t8)
	lw      $a2, SP_SAVE+32($t8)
	sltiu   $at, $s3, KMAX + 1           # a level past KMAX: the slow path's own
	beqz    $at, tess_fine               #  subdivision
	sltiu   $at, $s4, KMAX + 1
	beqz    $at, tess_fine
	lw      $t3, SP_PLA+0($t8)           # the corners' cache addresses back
	lw      $t4, SP_PLA+4($t8)
	lw      $t5, SP_PLA+8($t8)
	lw      $t6, SP_PLB+0($t8)
	addu    $t0, $s3, $s4
	sltiu   $at, $t0, TESS_CAP + 1
	beqz    $at, tess_capped             # past the cap: the slow path
	addiu   $at, $s6, 12                 # (the arena's bound, as a cell's)
	bnez    $t0, 6f                      # a level somewhere: the grid
	nop
	# the five edges pass: the polygon unsplit, one packet from the cache
	# (the slow path's two triangles)
	sltu    $at, $s7, $at
	bnez    $at, tess_full
	nop
	TSTAT   4
	lhu     $s0, 4($a1)                  # the uv words (TBOUNDS takes a1)
	lhu     $s1, 6($a1)
	lhu     $s2, 8($a1)
	lhu     $s5, 10($a1)
	TBOUNDS
	lw      $t0, SP_SXY($t3)
	lw      $t1, SP_SXY($t4)
	lw      $t2, SP_SXY($t5)
	lw      $t7, SP_SXY($t6)
	OFF4    tess_done
	lw      $t9, SP_COL($t3)             # c0
	li      $ra, 3
	lui     $at, CODE_GT4 >> 16
	bne     $a2, $ra, 11f
	or      $s0, $s0, $v0                # uv0 | clut
	lui     $at, CODE_GT3 >> 16          # a triangle
11:	or      $t9, $t9, $at
	sw      $t9, 4($s6)                  # code | c0
	sw      $t0, 8($s6)                  # xy0
	lw      $ra, SP_COL($t4)             # c1
	sw      $s0, 12($s6)
	or      $s1, $s1, $v1                # uv1 | tpage
	sw      $ra, 16($s6)
	sw      $t1, 20($s6)                 # xy1
	lw      $ra, SP_COL($t5)             # c2
	sw      $s1, 24($s6)
	sw      $t2, 32($s6)                 # xy2
	sw      $ra, 28($s6)
	li      $ra, 3
	bne     $a2, $ra, 12f
	sw      $s2, 36($s6)                 # uv2
	lui     $ra, 0x0900                  # a triangle's tag: 9 words, linked to the
	or      $ra, $ra, $fp                #  bucket's head
	sw      $ra, 0($s6)
	move    $fp, $s6
	b       tess_done
	addiu   $s6, $s6, 40
12:	lw      $ra, SP_COL($t6)             # c3
	sw      $t7, 44($s6)                 # xy3
	sw      $s5, 48($s6)                 # uv3
	sw      $ra, 40($s6)
	lui     $ra, 0x0c00                  # a quad's tag: 12 words
	or      $ra, $ra, $fp
	sw      $ra, 0($s6)
	move    $fp, $s6
	b       tess_done
	addiu   $s6, $s6, 52
6:
	# the grid: nu x nv cells, (nu + 1) x (nv + 1) points
	li      $t0, 1
	sllv    $s1, $t0, $s3                # nu: cells along a row
	sllv    $s5, $t0, $s4                # nv: rows
	sll     $s2, $s1, 5
	addiu   $s2, $s2, TP_SIZE            # RS: a row's bytes (nu + 1 points)
	lui     $s0, %hi(r_slow_pool + TG_OFS)
	addiu   $s0, $s0, %lo(r_slow_pool + TG_OFS)   # the grid
.if R_PROFILE
	mult    $s1, $s5                     # (the cells)
	TSTAT   0
	mflo    $t0
	TSTATADD 1, $t0
.endif

	# the corners into the grid (tess_corner): 0 top left, 1 top right,
	# 2 bottom left, 3 bottom right (a triangle's: 2 again)
	lw      $t7, SP_VERTPOOL($t8)        # (tess_corner's)
	mult    $s5, $s2                     # nv x RS
	addiu   $s3, $a2, -3                 # 0: a triangle (its last row's cells are triangles)
	sll     $t9, $s1, 5                  # nu x 32
	mflo    $a3
	addu    $a3, $s0, $a3                # g[nv][0]
	move    $v0, $t3                     # corner 0 into g[0][0]
	lhu     $v1, 4($a1)
	jal     tess_corner
	move    $a2, $s0
	addu    $a2, $s0, $t9                # corner 1 into g[0][nu]
	move    $v0, $t4
	jal     tess_corner
	lhu     $v1, 6($a1)
	move    $v0, $t5                     # corner 2 into g[nv][0]
	lhu     $v1, 8($a1)
	jal     tess_corner
	move    $a2, $a3
	addu    $a2, $a3, $t9                # corner 3 into g[nv][nu]
	bnez    $s3, 7f
	lhu     $v1, 10($a1)                 # a quad's fourth uv word
	lhu     $v1, 8($a1)                  # a triangle's: its third again
7:	jal     tess_corner
	move    $v0, $t6

	# the columns (tess_line, tess_proj): their points, projected
	li      $t0, 1
	beq     $s5, $t0, 8f                 # nv = 1: the columns are the corners
	move    $s4, $s2                     # the stride down a column
	move    $a0, $s0                     # the left column
	jal     tess_line
	move    $a1, $s5
	sll     $a0, $s1, 5
	addu    $a0, $a0, $s0                # the right column
	jal     tess_line
	move    $a1, $s5
8:	move    $a0, $s0
	jal     tess_proj
	addiu   $a1, $s5, 1
	sll     $a0, $s1, 5
	addu    $a0, $a0, $s0
	jal     tess_proj
	addiu   $a1, $s5, 1

	# the strips, row j to row j + 1: skipped whole when its four column
	# points are off the screen on one side; else its rows' inner points
	# made and projected (row j's unless the strip above did them: its
	# right column point's pad says), then its cells: a POLY_GT4
	# prepended to the world bucket (the triangles' row's POLY_GT3s),
	# unless off the screen; the arena's bound as the slow path's
	# (nothing more once s6 passes s7)
	TBOUNDS
	addiu   $t0, $s5, -1                 # the last row
	bnez    $s3, 40f
	li      $s3, -1                      # the row drawn as triangles: a triangle's
	move    $s3, $t0                     #  last, a quad's none
40:	move    $a2, $zero                   # j
	move    $a3, $s0                     # the row's first point
41:	sll     $t9, $s1, 5
	addu    $t4, $a3, $t9                # the strip's column points: right, row j
	addu    $t5, $a3, $s2                # left, row j + 1
	addu    $t6, $t5, $t9                # right, row j + 1
	lw      $t0, TP_SXY($a3)
	lw      $t1, TP_SXY($t4)
	lw      $t2, TP_SXY($t5)
	lw      $t7, TP_SXY($t6)
	OFF4    48f
	lh      $t1, 6($t4)                  # row j's inner points, unless done
	li      $t0, 1
	beq     $s1, $t0, 45f                # nu = 1: the rows are the columns' points
	li      $s4, TP_SIZE                 # (the stride along a row)
	bnez    $t1, 44f
	move    $a0, $a3
	jal     tess_line
	move    $a1, $s1
	addiu   $a0, $a3, TP_SIZE
	jal     tess_proj
	addiu   $a1, $s1, -1
44:	addu    $a0, $a3, $s2                # row j + 1's
	jal     tess_line
	move    $a1, $s1
	addu    $a0, $a3, $s2
	addiu   $a0, $a0, TP_SIZE
	jal     tess_proj
	addiu   $a1, $s1, -1
	sll     $t9, $s1, 5
	addu    $t6, $a3, $s2
	addu    $t6, $t6, $t9
	li      $t0, 1
	sh      $t0, 6($t6)                  # row j + 1 done: the strip below's top
	TBOUNDS                              # (the subroutines clobbered them)
45:	move    $s4, $zero                   # i
	move    $t3, $a3                     # the cell's top left
42:	addiu   $at, $s6, 12                 # the arena's bound: a cell goes out only while it
	sltu    $at, $s7, $at                #  ends at most 40 bytes past s7, the slow path's
	bnez    $at, tess_full               #  own overshoot (r_frame's margin counts on it)
	addiu   $t4, $t3, TP_SIZE            # top right
	addu    $t5, $t3, $s2                # bottom left
	addiu   $t6, $t5, TP_SIZE            # bottom right
	lw      $t0, TP_SXY($t3)
	lw      $t1, TP_SXY($t4)
	lw      $t2, TP_SXY($t5)
	lw      $t7, TP_SXY($t6)
	OFF4    49f
	lui     $ra, CODE_GT4 >> 16          # POLY_GT4, or the triangles' row's POLY_GT3:
	bne     $a2, $s3, 43f                #  the cell's top left, top right, bottom left
	lw      $t9, TP_GC($t3)              #  (its bottom right is the third corner again)
	lui     $ra, CODE_GT3 >> 16
43:	sw      $t0, 8($s6)                  # xy0
	or      $t9, $t9, $ra
	sw      $t9, 4($s6)                  # code | c0
	lw      $t9, TP_UVW($t3)
	lw      $ra, TP_GC($t4)
	or      $t9, $t9, $v0
	sw      $t9, 12($s6)                 # uv0 | clut
	sw      $ra, 16($s6)                 # c1
	sw      $t1, 20($s6)                 # xy1
	lw      $t9, TP_UVW($t4)
	lw      $ra, TP_GC($t5)
	or      $t9, $t9, $v1
	sw      $t9, 24($s6)                 # uv1 | tpage
	sw      $ra, 28($s6)                 # c2
	sw      $t2, 32($s6)                 # xy2
	lw      $t9, TP_UVW($t5)
	lw      $ra, TP_GC($t6)
	bne     $a2, $s3, 46f
	sw      $t9, 36($s6)                 # uv2
	lui     $ra, 0x0900                  # a triangle's tag: 9 words, linked to the
	or      $ra, $ra, $fp                #  bucket's head
	sw      $ra, 0($s6)
	move    $fp, $s6
	b       49f
	addiu   $s6, $s6, 40
46:	lw      $t9, TP_UVW($t6)
	sw      $ra, 40($s6)                 # c3
	sw      $t7, 44($s6)                 # xy3
	lui     $ra, 0x0c00                  # a quad's tag: 12 words
	or      $ra, $ra, $fp
	sw      $t9, 48($s6)                 # uv3
	sw      $ra, 0($s6)
	move    $fp, $s6
	addiu   $s6, $s6, 52
49:	addiu   $s4, $s4, 1                  # the next cell of the row
	bne     $s4, $s1, 42b
	addiu   $t3, $t3, TP_SIZE
48:	addiu   $a2, $a2, 1                  # the next strip
	bne     $a2, $s5, 41b
	addu    $a3, $a3, $s2
tess_done:
	lw      $ra, SP_SPRA($t8)
	lw      $s0, SP_SV+0($t8)
	lw      $s1, SP_SV+4($t8)
	lw      $s2, SP_SV+8($t8)
	lw      $s3, SP_SV+12($t8)
	lw      $s4, SP_SV+16($t8)
	lw      $s5, SP_SV+20($t8)
	jr      $ra
	nop

# tess_lvl: the level an edge that fails level 0 needs (EDGE's: t7 = |ds|
# |dz|, t9 = T2 (za + zb), a3 = |dx| + |dy|, v1 = za, v0 = zb; t6 = T2):
# the first m with |ds| |dz| <= 4^m 2 T2 zm + 2^m T2 |dz| (zm the nearer
# end's depth: 2 T2 zm + T2 |dz| is T2 (za + zb)) and |dx| + |dy| <= 2^m
# SPAN (the span halved instead, rounded up), into v0; KMAX + 1 past KMAX.
# EDGE enters at tess_lvl_1, its first instruction done. Clobbers at, v1,
# a3, t9.
	ICSEC   tess_lvl
tess_lvl:
	slt     $at, $v0, $v1                # zb < za
tess_lvl_1:
	beqz    $at, 1f
	sll     $v1, $v1, 1                  # 2 za
	sll     $v1, $v0, 1                  # 2 zb: 2 zm
1:	mult    $t6, $v1                     # 2 T2 zm (T2 as rs: the short multiply)
	li      $v0, 1                       # m
	addiu   $a3, $a3, 1
	srl     $a3, $a3, 1                  # the span at level 1 (halved, rounded up)
	mflo    $v1                          # 2 T2 zm
	subu    $t9, $t9, $v1                # T2 |dz|
	sll     $v1, $v1, 2                  # the bounds at level 1
	sll     $t9, $t9, 1
2:	addu    $at, $v1, $t9                # the bound at level m
	sltu    $at, $at, $t7                # 1: the warp over it
	bnez    $at, 3f
	sltiu   $at, $a3, SPAN + 1           # 1: within the span at level m
	bnez    $at, 4f                      # both: level m
	nop
3:	addiu   $v0, $v0, 1                  # the next level
	sll     $v1, $v1, 2
	sll     $t9, $t9, 1
	addiu   $a3, $a3, 1
	sltiu   $at, $v0, KMAX + 1
	bnez    $at, 2b
	srl     $a3, $a3, 1
4:	jr      $ra
	nop

# tess_corner: the polygon's corner at cache address v0 into grid point
# a2: its vertex from the pool (a0 = the record, its index list; t7 = the
# pool), its uv word v1, its light and colour from the cache. Clobbers
# t0-t2.
	ICSEC   tess_corner
tess_corner:
	subu    $t0, $v0, $t8                # the cache offset: the vertex x 4
	srl     $t0, $t0, 1
	addu    $t0, $t0, $a0
	lhu     $t0, R_VERTS($t0)            # its index in the pool
	lw      $t2, SP_COL($v0)             # its colour word
	sll     $t0, $t0, 3
	addu    $t0, $t0, $t7
	lw      $t1, 0($t0)                  # x | y << 16
	lh      $t0, 4($t0)                  # z
	sw      $t2, TP_GC($a2)
	sw      $t1, 0($a2)
	andi    $t2, $t2, 0xff               # its light
	sh      $t0, 4($a2)
	sh      $zero, 6($a2)
	sh      $t2, TP_C($a2)
	sw      $v1, TP_UVW($a2)
	andi    $t0, $v1, 0xff
	sll     $t0, $t0, 8                  # u x 256
	andi    $t1, $v1, 0xff00             # v x 256
	sh      $t0, TP_U($a2)
	jr      $ra
	sh      $t1, TP_V($a2)

# tess_line: the points between a0 and a0 + a1 x s4 (the span a1 a power of
# two, at least 2; the stride s4), level by level: the midpoint of each
# pair, then of each half. Clobbers t0-t6, t9, a1, at, v0.
	ICSEC   tess_line
tess_line:
	mult    $a1, $s4
	srl     $a1, $a1, 1                  # the half span, points
	nop
	mflo    $t4
	addu    $t4, $a0, $t4                # the line's end point
	nop
	mult    $a1, $s4
	nop
	nop
	mflo    $v0                          # the half span's bytes
51:	sll     $t5, $v0, 1
	subu    $t6, $t4, $t5                # the last pair's first point
	move    $t2, $a0
52:	addu    $t9, $t2, $v0                # the midpoint
	addu    $t3, $t9, $v0                # the pair's end
	MID     $t2, $t3, $t9
	sltu    $at, $t6, $t3                # past the last pair?
	beqz    $at, 52b
	move    $t2, $t3
	srl     $v0, $v0, 1                  # the next level: half the span
	sltu    $at, $v0, $s4
	beqz    $at, 51b
	nop
	jr      $ra
	nop

# tess_proj: the points from a0, a1 of them (at least 1), s4 apart, through
# the GTE three at a time (the last one or two with the last again: the
# same results). Clobbers t0-t2, t5, t6, a0, a1, at.
	ICSEC   tess_proj
tess_proj:
	sll     $t5, $s4, 1                  # two strides
	addu    $t6, $t5, $s4                # three
	addiu   $a1, $a1, -3
61:	bltz    $a1, 62f                     # fewer than three left
	addu    $t1, $a0, $s4
	lwc2    C2_VXY0, 0($a0)
	lwc2    C2_VZ0,  4($a0)
	lwc2    C2_VXY1, 0($t1)
	lwc2    C2_VZ1,  4($t1)
	addu    $t2, $a0, $t5
	lwc2    C2_VXY2, 0($t2)
	lwc2    C2_VZ2,  4($t2)
	addiu   $a1, $a1, -3
	nop                                  # (two instructions before the command)
	RTPT
	swc2    C2_SXY0, TP_SXY($a0)
	swc2    C2_SZ1,  TP_SZ($a0)
	swc2    C2_SXY1, TP_SXY($t1)
	swc2    C2_SZ2,  TP_SZ($t1)
	swc2    C2_SXY2, TP_SXY($t2)
	swc2    C2_SZ3,  TP_SZ($t2)
	b       61b
	addu    $a0, $a0, $t6
62:	addiu   $a1, $a1, 3                  # none, one or two left
	beqz    $a1, 63f
	sltiu   $at, $a1, 2                  # one: the second is the first again
	bnez    $at, 64f
	move    $t1, $a0
	addu    $t1, $a0, $s4                # two: the second
64:	lwc2    C2_VXY0, 0($a0)
	lwc2    C2_VZ0,  4($a0)
	lwc2    C2_VXY1, 0($t1)
	lwc2    C2_VZ1,  4($t1)
	lwc2    C2_VXY2, 0($t1)
	lwc2    C2_VZ2,  4($t1)
	nop
	nop
	RTPT
	swc2    C2_SXY0, TP_SXY($a0)
	swc2    C2_SZ1,  TP_SZ($a0)
	swc2    C2_SXY1, TP_SXY($t1)
	swc2    C2_SZ2,  TP_SZ($t1)
63:	jr      $ra
	nop

	ICSEC   tess_full
tess_full:                               # the arena full: this polygon's rest given up
	li      $t0, 1
	b       tess_done
	sw      $t0, SP_FULL($t8)

# To the slow path (a0, a1, a2 as received): into slow_poly past its saves,
# ra and s0-s5 in the same slots already (tess_poly's), its first load in
# the jump's slot
	ICSEC   tess_near
tess_near:                               # a corner too near or off range: the slow path's clip
	TSTAT   2
	j       slow_poly_1
	lw      $t7, SP_T2($t8)
	ICSEC   tess_fine
tess_fine:                               # finer than KMAX: the slow path's own subdivision
	TSTAT   3
	j       slow_poly_1
	lw      $t7, SP_T2($t8)
	ICSEC   tess_capped
tess_capped:                             # past the cap: the slow path's own subdivision
	TSTAT   5
	j       slow_poly_1
	lw      $t7, SP_T2($t8)
