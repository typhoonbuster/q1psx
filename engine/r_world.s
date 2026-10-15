# q1psx — world renderer, hand-written MIPS assembly (architecture.md §8).
#
#   r_render(r_state_t *s)
#     1. load the view into the GTE (rotation, translation; |rotation| as the
#        light matrix for bbox radii)
#     2. find the camera leaf; when it changed, decode its PVS row and stamp
#        the visible leaves + their ancestors (Quake R_MarkLeaves)
#     3. walk the BSP front to back on an explicit scratchpad stack, culling
#        nodes/leaves by visframe and view frustum (clip flags inherited);
#        visible leaves stamp their marksurfaces with the frame count
#     4. after a node's near child, draw the face block on the camera's side
#        of its plane: transform the record's vertices 3 at a time (RTPT) into
#        the scratchpad, then write POLY_FT4 / POLY_FT3 packets, prepending
#        each to the world OT bucket (front-to-back walk + prepend = painter's
#        order)
#
# Hazards are handled by hand (.set noreorder): one slot after every load /
# mfc2 / cfc2, branch delay slots, two instructions between a GTE register
# load and the command reading it, two instructions between mflo and mult.
#
# Register roles during the walk (callee-saved, preserved by r_render):
#   s0 r_state   s1 current node / leaf|1   s2 clip flags   s3 stack ptr
#   s4 framecount   s5 visframe   s6 packet ptr   s7 packet end (minus
#   QW_MAX_REC_PKT + 8)   fp OT world-bucket link (24 bits)   t8 scratchpad base
#   (0x1F800000)   v1 tpage << 16 of the last drawn texture (draw_records'
#   texture cache: the walk must leave v1 alone)

	.set noreorder
	.set noat

	.include "gtereg.inc"
	.include "inline_s.inc"
	.include "r_defs.inc"

# SIZEOK r, fail: r = SXY(corner) - SXY(corner 0); branch to `fail` unless
# |dy| <= 250 and |dx| <= 500 (packed-word difference: dy may be off by one,
# fine for a margin). Uses t0 (free once the differences are taken).
.macro SIZEOK r, fail
	sra     $t0, \r, 16
	addiu   $t0, $t0, 250
	sltiu   $t0, $t0, 501
	beqz    $t0, \fail
	sll     $t0, \r, 16
	sra     $t0, $t0, 16
	addiu   $t0, $t0, 500
	sltiu   $t0, $t0, 1001
	beqz    $t0, \fail
	nop
.endm

# FRUSTUM cull: test the box whose centre / half extents are in V0 / V1
# (vertex units; loaded by the caller at least two instructions back) against
# the frustum planes still set in s2. A plane's bit is cleared once the box is
# fully inside it; branches to `cull` if the box is outside one. View space:
# x right, y down, z forward, H = 160 on a 320x240 screen -> right z-x >= 0,
# left z+x >= 0, bottom 3z-4y >= 0, top 3z+4y >= 0. The box goes to view space
# as centre (rotation + translation) and radius (|rotation| x half extents).
# Clobbers t0-t7, t9, at, v0, a1.
.macro FRUSTUM cull
	MVMVA   1, 0, 0, 0, 0                # IR = rot * centre + TR
	andi    $at, $s2, 1                  # (right plane still set?)
	mfc2    $t0, C2_IR1                  # x
	mfc2    $t1, C2_IR2                  # y
	mfc2    $t2, C2_IR3                  # z
	sll     $v0, $t1, 2                  # 4y
	MVMVA   1, 1, 1, 3, 0                # IR = |rot| * half extents
	sll     $a1, $t2, 1
	addu    $a1, $a1, $t2                # 3z
	mfc2    $t3, C2_IR1                  # rx
	mfc2    $t4, C2_IR2                  # ry
	mfc2    $t5, C2_IR3                  # rz
	sll     $t7, $t4, 2                  # 4 ry
	addu    $t6, $t5, $t3                # rz + rx: radius for left/right
	sll     $t9, $t5, 1
	addu    $t9, $t9, $t5
	addu    $t7, $t7, $t9                # 3 rz + 4 ry: radius for top/bottom
	beqz    $at, 1f
	andi    $at, $s2, 2
	subu    $t3, $t2, $t0                # right: z - x
	addu    $t4, $t3, $t6
	bltz    $t4, \cull
	subu    $t4, $t3, $t6
	bltz    $t4, 1f
	nop
	xori    $s2, $s2, 1
1:	beqz    $at, 2f
	andi    $at, $s2, 4
	addu    $t3, $t2, $t0                # left: z + x
	addu    $t4, $t3, $t6
	bltz    $t4, \cull
	subu    $t4, $t3, $t6
	bltz    $t4, 2f
	nop
	xori    $s2, $s2, 2
2:	beqz    $at, 3f
	andi    $at, $s2, 8
	subu    $t3, $a1, $v0                # bottom: 3z - 4y
	addu    $t4, $t3, $t7
	bltz    $t4, \cull
	subu    $t4, $t3, $t7
	bltz    $t4, 3f
	nop
	xori    $s2, $s2, 4
3:	beqz    $at, 4f
	addu    $t3, $a1, $v0                # top: 3z + 4y
	addu    $t4, $t3, $t7
	bltz    $t4, \cull
	subu    $t4, $t3, $t7
	bltz    $t4, 4f
	nop
	xori    $s2, $s2, 8
4:
.endm

	.section .bss.r_walk_spill, "aw", @nobits
	.align  4
r_walk_spill:                            # walk stack overflow (see walk_spill)
	.space  WALK_RAM

	.section .text.r_render, "ax", @progbits
	.global r_render
	.type   r_render, @function
r_render:
	addiu   $sp, $sp, -48
	sw      $ra, 44($sp)
	sw      $fp, 40($sp)
	sw      $s7, 36($sp)
	sw      $s6, 32($sp)
	sw      $s5, 28($sp)
	sw      $s4, 24($sp)
	sw      $s3, 20($sp)
	sw      $s2, 16($sp)
	sw      $s1, 12($sp)
	sw      $s0,  8($sp)
	move    $s0, $a0

	# ---- GTE: rotation + translation, light matrix = |rotation| ---------
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
	lw      $t3, RS_ABSROT+0($s0)
	lw      $t4, RS_ABSROT+4($s0)
	ctc2    $t0, C2_TRX
	ctc2    $t1, C2_TRY
	ctc2    $t2, C2_TRZ
	ctc2    $t3, C2_L11L12
	ctc2    $t4, C2_L13L21
	lw      $t0, RS_ABSROT+8($s0)
	lw      $t1, RS_ABSROT+12($s0)
	lh      $t2, RS_ABSROT+16($s0)
	ctc2    $t0, C2_L22L23
	ctc2    $t1, C2_L31L32
	ctc2    $t2, C2_L33

	# ---- scratchpad globals ---------------------------------------------
	lui     $t8, 0x1f80
	lw      $t0, RS_CAM+0($s0)
	lw      $t1, RS_CAM+4($s0)
	lw      $t2, RS_CAM+8($s0)
	lw      $t3, RS_FVBASE($s0)
	lw      $t4, RS_SZ_FAST($s0)
	sw      $t0, SP_CAM+0($t8)
	sw      $t1, SP_CAM+4($t8)
	sw      $t2, SP_CAM+8($t8)
	sw      $t3, SP_FVBASE($t8)
	sw      $t4, SP_SZFAST($t8)
	sw      $zero, SP_FULL($t8)
.if R_PROFILE
	sw      $zero, SP_NODES($t8)
	sw      $zero, SP_RECORDS($t8)
	sw      $zero, SP_VERTS($t8)
	sw      $zero, SP_POLYS($t8)
	sw      $zero, SP_SKIPPED($t8)
	sw      $zero, SP_TBLOCK($t8)
	sw      $zero, SP_SLOWTRI($t8)
	sw      $zero, SP_TSLOW($t8)
.endif
	li      $t0, -1
	sw      $t0, SP_LASTTEX($t8)         # texture cache: nothing yet
	lw      $s4, RS_FRAMECOUNT($s0)
	lw      $s6, RS_PKT($s0)
	lw      $s7, RS_PKT_END($s0)
	lw      $t0, RS_OT_WORLD($s0)
	sll     $s6, $s6, 3                  # packets through KUSEG (0x00xxxxxx):
	srl     $s6, $s6, 3                  # a packet's address is then its own
	lw      $fp, 0($t0)                  # OT link, no masking per packet
	sll     $s7, $s7, 3
	srl     $s7, $s7, 3
	lw      $t0, RS_TEXTURES($s0)
	lw      $t2, RS_PLANES($s0)
	lw      $t3, RS_VERTPOOL($s0)
	lui     $t1, 0x0100                  # tag: 1 word
	sw      $t0, SP_TEXBASE($t8)
	sw      $t2, SP_PLANES($t8)
	sw      $t3, SP_VERTPOOL($t8)
	or      $t1, $t1, $fp
	lui     $t2, 0xE200
	sw      $t1, 0($s6)                  # texture window off, first in the
	sw      $t2, 4($s6)                  # bucket = drawn after the world (the
	move    $fp, $s6                     # overlay and the next frame start
	addiu   $s6, $s6, 8                  # unwrapped)

	# ---- camera leaf ----------------------------------------------------
	lw      $s1, RS_ROOT($s0)
	nop
fl_loop:
	andi    $t0, $s1, 1
	bnez    $t0, fl_done
	nop
	jal     plane_dist
	move    $a0, $s1
	slti    $t0, $v0, 1                  # d <= 0 -> back child (Quake tie rule)
	sll     $t0, $t0, 2
	addu    $t0, $t0, $s1
	lw      $s1, N_CHILD($t0)
	b       fl_loop
	nop
fl_done:
	addiu   $s1, $s1, -1                 # leaf pointer

	lw      $t0, RS_VIEW_LEAF($s0)
	lw      $t1, RS_FORCE_MARK($s0)
	bne     $t0, $s1, do_mark
	nop
	beqz    $t1, mark_done
	nop
do_mark:
	jal     mark_pvs
	move    $a0, $s1
mark_done:

	# ---- BSP walk -------------------------------------------------------
	lw      $s5, RS_VISFRAME($s0)
	lw      $s1, RS_ROOT($s0)
	li      $s2, 0xf                     # all four frustum planes active
	move    $s3, $t8                     # walk stack = scratchpad + 0
	sw      $zero, SP_SPILLS($t8)

walk:
	andi    $t0, $s1, 1
walk_t0:                                 # (t0 = s1 & 1: a leaf?)
	bnez    $t0, walk_leaf
	addiu   $a0, $s1, -1                 # (the leaf, for walk_leaf)
	lhu     $t0, N_VISFRAME($s1)
	lh      $t1, N_C+4($s1)              # the frustum test's box, loaded ahead
	bne     $t0, $s5, pop                # (harmless when unused). VZ with lh:
	lwc2    C2_VXY0, N_C+0($s1)          # the halfwords after c[2] and h[2]
	lh      $t2, N_H+4($s1)              # are visframe / plane3, not part of
	lwc2    C2_VXY1, N_H+0($s1)          # the vectors
.if R_PROFILE
	lw      $t3, SP_NODES($t8)
.endif
	mtc2    $t1, C2_VZ0
	mtc2    $t2, C2_VZ1
.if R_PROFILE
	addiu   $t3, $t3, 1
	sw      $t3, SP_NODES($t8)
.endif
	beqz    $s2, wn_side_1               # fully inside the frustum already
	lhu     $t0, N_PLANE3($s1)           # (wn_side's first)
	FRUSTUM pop

	# the node's plane: d = n . cam - dist (x4096, vertex units); axial
	# planes skip the products
wn_side:
	lhu     $t0, N_PLANE3($s1)           # plane index x 3: word offset in the
wn_side_1:
	lw      $t1, SP_PLANES($t8)          # shared table (dist in 1/8 units)
	sll     $t0, $t0, 2
	addu    $t5, $t0, $t1
	lw      $t1, PN_DIST($t5)
	lbu     $t0, PN_TYPE($t5)
	sra     $t1, $t1, 1                  # dist -> vertex units, x4096
	sltiu   $t2, $t0, 3
	beqz    $t2, wn_general
	sll     $t0, $t0, 2
	addu    $t0, $t0, $t8
	lw      $v0, SP_CAM($t0)             # axial: n . cam = cam[type] x4096
	sll     $t7, $s1, 8                  # (the push's node pointer, low 24 bits)
	sll     $v0, $v0, 12
	subu    $v0, $v0, $t1
	# Stack word: node address, low 24 bits (a KUSEG pointer: no mask on
	# pop) | side x 4 << 24 | no-draw << 27 | clip flags << 28.
wn_push:
	slt     $t0, $v0, $zero              # side: 1 if the camera is behind
	addiu   $t2, $v0, BACKFACE_EPS       # no faces if |d| <= eps:
	sltiu   $t2, $t2, 2 * BACKFACE_EPS + 1   # (unsigned) d + eps <= 2 eps
	sll     $t2, $t2, 1
	or      $t2, $t2, $t0
	sll     $t2, $t2, 26
	srl     $t3, $t7, 8
	or      $t3, $t3, $t2
	sll     $t4, $s2, 28
	or      $t3, $t3, $t4
	sll     $t0, $t0, 2
	addu    $t0, $t0, $s1
	lw      $s1, N_CHILD($t0)            # near child first
	sw      $t3, SP_STACK($s3)           # push
	addiu   $s3, $s3, 4
	andi    $at, $s3, (1 << WALK_SHIFT) - 1
	bnez    $at, walk_t0                 # room left in the scratchpad block
	andi    $t0, $s1, 1

	# Deep BSPs (Quake maps reach 110 levels): the full scratchpad stack is
	# moved to the next RAM block and starts over; `pop` brings it back when
	# it runs empty. Only pushes pay a test.
walk_spill:
	lw      $t1, SP_SPILLS($t8)
	lui     $t2, %hi(r_walk_spill)
	addiu   $t2, $t2, %lo(r_walk_spill)
	sll     $t4, $t1, WALK_SHIFT         # block offset
	addu    $t2, $t2, $t4
	addiu   $t1, $t1, 1
	sw      $t1, SP_SPILLS($t8)
	move    $t4, $t8
1:	lw      $t5, 0($t4)
	addiu   $t4, $t4, 4
	sw      $t5, 0($t2)
	bne     $t4, $s3, 1b
	addiu   $t2, $t2, 4
	b       walk
	move    $s3, $t8

	# general plane: three products (two instructions between an mflo and
	# the next mult)
wn_general:
	lh      $t2, PN_N+0($t5)
	lw      $t3, SP_CAM+0($t8)
	lh      $t4, PN_N+2($t5)
	mult    $t2, $t3
	lw      $t6, SP_CAM+4($t8)
	mflo    $v0
	lh      $t2, PN_N+4($t5)
	subu    $v0, $v0, $t1
	mult    $t4, $t6
	lw      $t3, SP_CAM+8($t8)
	mflo    $t4
	addu    $v0, $v0, $t4
	sll     $t7, $s1, 8                  # (the push's node pointer)
	mult    $t2, $t3
	mflo    $t4
	b       wn_push
	addu    $v0, $v0, $t4

walk_refill:                             # scratchpad stack empty: next RAM block
	lw      $t1, SP_SPILLS($t8)
	nop
	beqz    $t1, walk_done               # nothing spilled: the walk is over
	addiu   $t1, $t1, -1
	sw      $t1, SP_SPILLS($t8)
	lui     $t2, %hi(r_walk_spill)
	addiu   $t2, $t2, %lo(r_walk_spill)
	sll     $t4, $t1, WALK_SHIFT
	addu    $t2, $t2, $t4
	move    $t4, $t8
	addiu   $s3, $t8, 1 << WALK_SHIFT
2:	lw      $t5, 0($t2)
	addiu   $t2, $t2, 4
	sw      $t5, 0($t4)
	addiu   $t4, $t4, 4
	bne     $t4, $s3, 2b
	nop
	b       pop
	nop

walk_leaf:                               # a0 = the leaf (s1 - 1)
	lhu     $t0, L_VISFRAME($a0)
	lh      $t1, L_C+4($a0)
	bne     $t0, $s5, pop
	lwc2    C2_VXY0, L_C+0($a0)
	lh      $t2, L_H+4($a0)
	lwc2    C2_VXY1, L_H+0($a0)
	mtc2    $t1, C2_VZ0
	lhu     $a3, L_NMARKS($a0)
	mtc2    $t2, C2_VZ1
	lw      $a2, L_MARKS($a0)
	beqz    $s2, wl_marks
	sll     $a3, $a3, 1                  # marks x 2 bytes
	FRUSTUM pop
wl_marks:
	lw      $t3, SP_FVBASE($t8)
	beqz    $a3, pop
	addu    $a3, $a3, $a2                # end of the marks
	lhu     $t0, 0($a2)
wl_mark:
	addiu   $a2, $a2, 2
	sll     $t0, $t0, 1
	addu    $t0, $t0, $t3
	sh      $s4, 0($t0)                  # face_visframe[mark] = framecount
	bne     $a2, $a3, wl_mark
	lhu     $t0, 0($a2)                  # (next; one past the last: harmless)

pop:
	beq     $s3, $t8, walk_refill
	addiu   $s3, $s3, -4                 # (the refill resets s3)
	lw      $t0, SP_STACK($s3)           # (see wn_push)
	nop
	srl     $s2, $t0, 28                 # clip flags
	srl     $t3, $t0, 24
	andi    $t4, $t3, 8                  # no faces
	andi    $t3, $t3, 4                  # side * 4
	sll     $t2, $t0, 8
	srl     $t2, $t2, 8                  # node (KUSEG)
	subu    $t5, $t2, $t3
	lw      $s1, N_CHILD+4($t5)          # far child child[1 - side]: walked
	bnez    $t4, walk                    # after the faces
	addu    $t5, $t2, $t3
	lw      $a0, N_FACES($t5)            # block on the camera's side
	sll     $t6, $t3, 3                  # side * 32 (= N_SIZE - 4): the block
	addu    $t6, $t6, $t2                # ends at faces[1] (front) or at the
	lw      $a1, N_FACES+4($t6)          # next node's faces[0] (back)
.if R_PROFILE
	lhu     $t9, TIMER2($t8)
	beq     $a0, $a1, walk               # empty block
	lw      $t0, SP_FULL($t8)
.else
	lw      $t0, SP_FULL($t8)
	beq     $a0, $a1, walk               # empty block
	lw      $v0, SP_CLUT16($t8)
.endif

# -----------------------------------------------------------------------------
# draw_records, inline in the walk: a0 = first face record, a1 = block end
# (kept throughout). For each record whose face was stamped this frame:
# transform its vertices into the scratchpad (SXY / SZ / COL, 4 bytes each per
# vertex), then emit its quads and triangles. A record with a vertex nearer
# than sz_fast takes the careful loop, which hands the polygons touching such
# a vertex to the slow path (R3.3).
# Texture cache: v0 / v1 = clut << 16 / tpage << 16 of SP_LASTTEX (3 records
# in 4 reuse the previous record's texture). v1 lives across face blocks (the
# walk never touches it); v0 is reloaded on entry.
# Texture window (GP0 E2h: power-of-two textures wrap in hardware, the cook
# only cuts faces every 256 texels): one command per run of records sharing
# a texture, prepended when the run ends (a texture change, or walk_done) so
# it comes first in the bucket, before all of the run's polygons.
# Packet space: s7 stops QW_MAX_REC_PKT + 8 short of the arena end (r_frame),
# so one test per record covers all of its packets (the window command of the
# run it closes included) and walk_done's; the slow path keeps s6 <= s7.
# Clobbers at, v0, a0-a3, t0-t7, t9; back to `walk` when done.
draw_records:
.if R_PROFILE
	sw      $t9, SP_T0($t8)
	lw      $v0, SP_CLUT16($t8)
.endif
	bnez    $t0, walk                    # packet arena full: nothing more
	move    $t2, $zero
dr_cull:
	addu    $a0, $a0, $t2                # culled record (t2 = its size): next
dr_rec:
.if R_PROFILE
	beq     $a0, $a1, dr_done
.else
	beq     $a0, $a1, walk
.endif
	lhu     $t1, R_FACE_ID($a0)          # (at the block end: a harmless read)
	lw      $t0, SP_FVBASE($t8)
	sll     $t1, $t1, 1
	addu    $t1, $t1, $t0
	lhu     $t1, 0($t1)
	lhu     $t2, R_SIZE($a0)
	bne     $t1, $s4, dr_rec             # not stamped this frame: next
	addu    $a0, $a0, $t2                # (a0 = next record)
	sw      $a0, SP_NEXTREC($t8)
	subu    $a0, $a0, $t2                # stamped: back to it

	# bounding-sphere frustum test (view space x6; r in vertex units, view
	# radius 1.5 r): left/right planes have |n| = sqrt2 -> 2.125 r, top/bottom
	# |n| = 5 -> 8 r. All conservative.
	# VZ0 is loaded from a sign-extended halfword, not with lwc2: the upper
	# half of the word at +16 is not part of it, and PCSX-Redux does not
	# drop those bits the way the 16-bit hardware register does.
	lh      $t9, R_SC+4($a0)
	lwc2    C2_VXY0, R_SC+0($a0)
	mtc2    $t9, C2_VZ0
	lhu     $t3, R_SR($a0)
	lhu     $a3, R_TEX($a0)
	MVMVA   1, 0, 0, 0, 0                # IR = rot * centre + TR
	lw      $at, SP_LASTTEX($t8)         # (while the GTE works)
	mfc2    $t0, C2_IR1                  # x
	mfc2    $t1, C2_IR2                  # y
	mfc2    $t4, C2_IR3                  # z
	sll     $t5, $t3, 1
	srl     $t6, $t3, 3
	addu    $t5, $t5, $t6                # 2.125 r
	subu    $t6, $t4, $t0                # right: z - x
	addu    $t6, $t6, $t5
	bltz    $t6, dr_cull
	addu    $t6, $t4, $t0                # left: z + x
	addu    $t6, $t6, $t5
	bltz    $t6, dr_cull
	sll     $t5, $t3, 3                  # 8 r
	sll     $t6, $t4, 1
	addu    $t6, $t6, $t4                # 3z
	sll     $t7, $t1, 2                  # 4y
	subu    $t9, $t6, $t7                # bottom: 3z - 4y
	addu    $t9, $t9, $t5
	bltz    $t9, dr_cull
	addu    $t9, $t6, $t7                # top: 3z + 4y
	addu    $t9, $t9, $t5
	bltz    $t9, dr_cull
	sll     $t5, $t3, 1                  # delay: 2r
	lw      $t7, SP_SZFAST($t8)
	subu    $t5, $t4, $t5                # nearest possible SZ (>= 1.5 r away)
	slt     $a2, $t5, $t7                # a2 bit 0: may come too near
	sll     $t5, $t3, 1
	addu    $t5, $t5, $t3                # 3r
	slt     $t5, $t4, $t5                # z < 3r: a polygon may exceed the
	sll     $t5, $t5, 1                  # GPU's 1023 x 511 px limit
	sltu    $t0, $s7, $s6                # room for the whole record?
	bnez    $t0, dr_full
	or      $a2, $a2, $t5                # a2 bit 1
.if R_SLOW_C
	# a2 bit 2: an affine error above the threshold is possible:
	# (z - rho) x 16 < k16 x rho, rho = 1.5 r (r_set_fidelity)
	srl     $t5, $t3, 1
	addu    $t5, $t5, $t3                # rho
	lw      $t9, RS_WARP_K16($s0)
	subu    $t6, $t4, $t5                # nearest depth
	mult    $t9, $t5
	sll     $t6, $t6, 4
	mflo    $t9
	slt     $t9, $t6, $t9
	sll     $t9, $t9, 2
	or      $a2, $a2, $t9
.endif

	lbu     $t2, R_NVERTS($a0)
	beq     $at, $a3, dr_tex_done        # same texture as the last record
	lhu     $t0, R_POLYS($a0)            # (the light bytes' offset, below)
	bltz    $at, dr_tex_load             # first of the frame: no run to close
	lw      $t5, SP_TEXBASE($t8)
	lw      $t6, SP_TWIN($t8)            # close the last texture's run: its
	lui     $t9, 0x0100                  # window command, prepended after its
	or      $t9, $t9, $fp                # polygons, comes first in the bucket
	sw      $t9, 0($s6)                  # (one per run, not per record: the
	sw      $t6, 4($s6)                  # run's polygons all draw after it)
	move    $fp, $s6
	addiu   $s6, $s6, 8
dr_tex_load:
	sll     $t6, $a3, 3                  # fetch its words (8 B entry)
	sw      $a3, SP_LASTTEX($t8)
	addu    $t6, $t6, $t5
	lw      $t9, 4($t6)                  # its texture-window command
	lw      $v0, 0($t6)                  # clut | tpage << 16
	sw      $t9, SP_TWIN($t8)
	lui     $t5, 0xffff
	and     $v1, $v0, $t5                # tpage << 16, ORed into uv1
	sll     $v0, $v0, 16                 # clut  << 16, ORed into uv0
	sw      $v0, SP_CLUT16($t8)          # (the slow path's copies)
	sw      $v1, SP_TPAGE16($t8)
dr_tex_done:
.if R_PROFILE
	lw      $t1, SP_RECORDS($t8)
	lw      $t9, SP_VERTS($t8)
	addiu   $t1, $t1, 1
	addu    $t9, $t9, $t2
	sw      $t1, SP_RECORDS($t8)
	sw      $t9, SP_VERTS($t8)
.endif
	addiu   $t1, $t2, 3                  # light bytes: polys - ((nv + 3) & ~3)
	andi    $t1, $t1, 0xfc
	addu    $t0, $t0, $a0
	subu    $at, $t0, $t1
	addiu   $t3, $a0, R_VERTS            # the record's indices into the pool
	lw      $t9, SP_VERTPOOL($t8)        # (padded to threes: no stray reads)
	move    $t4, $t8                     # cache cursor
	addiu   $t5, $t2, -2                 # vertices left - 2: RTPT while 3+
dr_xf:
	lhu     $t0, 0($t3)
	lhu     $t1, 2($t3)
	lhu     $t6, 4($t3)
	sll     $t0, $t0, 3
	addu    $t0, $t0, $t9
	sll     $t1, $t1, 3
	lwc2    C2_VXY0, 0($t0)
	lwc2    C2_VZ0,  4($t0)
	addu    $t1, $t1, $t9
	sll     $t6, $t6, 3
	lwc2    C2_VXY1, 0($t1)
	lwc2    C2_VZ1,  4($t1)
	addu    $t6, $t6, $t9
	lwc2    C2_VXY2, 0($t6)
	lwc2    C2_VZ2,  4($t6)
	addiu   $t3, $t3, 6
	addiu   $t5, $t5, -3
	RTPT
	lbu     $t0, 0($at)                  # while the GTE works: the three
	lbu     $t1, 1($at)                  # lights, grey c -> 0x00cccccc
	lbu     $t6, 2($at)                  # (x = c | c << 8; x | x << 8)
	addiu   $at, $at, 3
	sll     $a3, $t0, 8
	or      $t0, $t0, $a3
	sll     $a3, $t0, 8
	or      $t0, $t0, $a3
	sll     $a3, $t1, 8
	or      $t1, $t1, $a3
	sll     $a3, $t1, 8
	or      $t1, $t1, $a3
	sll     $a3, $t6, 8
	or      $t6, $t6, $a3
	sll     $a3, $t6, 8
	or      $t6, $t6, $a3
	sw      $t0, SP_COL+0($t4)
	sw      $t1, SP_COL+4($t4)
	sw      $t6, SP_COL+8($t4)
	swc2    C2_SXY0, SP_SXY+0($t4)
	swc2    C2_SXY1, SP_SXY+4($t4)
	swc2    C2_SXY2, SP_SXY+8($t4)
	swc2    C2_SZ1,  SP_SZ+0($t4)
	swc2    C2_SZ2,  SP_SZ+4($t4)
	swc2    C2_SZ3,  SP_SZ+8($t4)
	bgtz    $t5, dr_xf
	addiu   $t4, $t4, 12
	addiu   $t5, $t5, 2                  # 0, 1 or 2 left: one RTPS each
	blez    $t5, dr_xf_done              # (rather than a padded RTPT)
	lhu     $t0, 0($t3)
dr_one:
	addiu   $t3, $t3, 2
	sll     $t0, $t0, 3
	addu    $t0, $t0, $t9
	lwc2    C2_VXY0, 0($t0)
	lwc2    C2_VZ0,  4($t0)
	lbu     $t1, 0($at)
	addiu   $at, $at, 1
	RTPS
	sll     $a3, $t1, 8
	or      $t1, $t1, $a3
	sll     $a3, $t1, 8
	or      $t1, $t1, $a3
	sw      $t1, SP_COL($t4)
	addiu   $t5, $t5, -1
	swc2    C2_SXY2, SP_SXY($t4)
	swc2    C2_SZ3,  SP_SZ($t4)
	lhu     $t0, 0($t3)                  # (the next index; the padding at the end)
	bgtz    $t5, dr_one
	addiu   $t4, $t4, 4
dr_xf_done:

	beqz    $a2, dr_polys                # sphere says: nothing near or big
	move    $t4, $t8
	andi    $t0, $a2, 6                  # big or warping: careful regardless
	bnez    $t0, dr_polys                # may be big: careful loop regardless
	move    $t5, $t2
	move    $a2, $zero                   # exact check: any real vertex near?
dr_nz:
	lw      $t0, SP_SZ($t4)
	addiu   $t5, $t5, -1
	sltu    $t0, $t0, $t7
	or      $a2, $a2, $t0
	bgtz    $t5, dr_nz
	addiu   $t4, $t4, 4

dr_polys:
	lhu     $t0, R_POLYS($a0)
	lbu     $at, R_NQUADS($a0)
.if R_PROFILE
	lw      $t1, SP_POLYS($t8)
	addu    $a3, $a0, $t0                # first polygon
	lbu     $t0, R_NTRIS($a0)
	addu    $t1, $t1, $at
	addu    $t1, $t1, $t0
	beqz    $at, dr_tris
	sw      $t1, SP_POLYS($t8)
.else
	addu    $a3, $a0, $t0                # first polygon
	beqz    $at, dr_tris
	nop
.endif

dr_quad:
	lw      $t0, 0($a3)                  # 4 vertex offsets
	addiu   $at, $at, -1                 # (count down)
	andi    $t3, $t0, 0xff
	srl     $t4, $t0, 8
	andi    $t4, $t4, 0xff
	srl     $t5, $t0, 16
	andi    $t5, $t5, 0xff
	srl     $t6, $t0, 24
	addu    $t3, $t3, $t8
	addu    $t4, $t4, $t8
	addu    $t5, $t5, $t8
	beqz    $a2, dr_quad_emit
	addu    $t6, $t6, $t8
.if R_SLOW_C
	b       dr_quad_skip                 # the C reference decides per polygon
	nop
.endif
	lw      $t0, SP_SZ($t3)              # careful: any corner too near?
	lw      $t1, SP_SZ($t4)
	lw      $t2, SP_SZ($t5)
	lw      $t9, SP_SZ($t6)
	sltu    $t0, $t0, $t7
	sltu    $t1, $t1, $t7
	sltu    $t2, $t2, $t7
	sltu    $t9, $t9, $t7
	or      $t0, $t0, $t1
	or      $t0, $t0, $t2
	or      $t0, $t0, $t9
	bnez    $t0, dr_quad_skip
	lw      $t0, SP_SXY($t3)             # size: every corner within 500 x 250 px
	lw      $t1, SP_SXY($t4)             # of corner 0 -> bbox fits the GPU
	lw      $t2, SP_SXY($t5)
	lw      $t9, SP_SXY($t6)
	subu    $t1, $t1, $t0
	subu    $t2, $t2, $t0
	subu    $t9, $t9, $t0
	SIZEOK  $t1, dr_quad_skip
	SIZEOK  $t2, dr_quad_skip
	SIZEOK  $t9, dr_quad_skip
dr_quad_emit:                            # POLY_GT4: colour, xy, uv per corner
	lw      $t0, SP_COL($t3)             # c0
	lw      $t1, 4($a3)                  # uv0 | uv1 << 16
	lw      $t3, SP_SXY($t3)             # xy0
	lui     $t9, CODE_GT4 >> 16
	lw      $t2, SP_COL($t4)             # c1
	lw      $t4, SP_SXY($t4)             # xy1
	or      $t0, $t0, $t9                # code | c0
	sw      $t0, 4($s6)
	sw      $t3, 8($s6)
	andi    $t0, $t1, 0xffff
	or      $t0, $t0, $v0                # uv0 | clut
	sw      $t0, 12($s6)
	sw      $t2, 16($s6)
	sw      $t4, 20($s6)
	srl     $t1, $t1, 16
	or      $t1, $t1, $v1                # uv1 | tpage
	sw      $t1, 24($s6)
	lw      $t0, SP_COL($t5)             # c2
	lw      $t1, SP_SXY($t5)             # xy2
	lw      $t2, 8($a3)                  # uv2 | uv3 << 16
	sw      $t0, 28($s6)
	sw      $t1, 32($s6)
	andi    $t0, $t2, 0xffff
	sw      $t0, 36($s6)
	lw      $t0, SP_COL($t6)             # c3
	lw      $t1, SP_SXY($t6)             # xy3
	srl     $t2, $t2, 16
	sw      $t0, 40($s6)
	sw      $t1, 44($s6)
	sw      $t2, 48($s6)
	lui     $t9, 0x0c00
	or      $t9, $t9, $fp                # tag: 12 words, link to previous head
	sw      $t9, 0($s6)
	move    $fp, $s6                     # new bucket head
	addiu   $s6, $s6, 52
dr_quad_next:
	bgtz    $at, dr_quad
	addiu   $a3, $a3, POLY_SIZE

dr_tris:
	lbu     $at, R_NTRIS($a0)
	nop
	beqz    $at, dr_next
	nop
dr_tri:
	lw      $t0, 0($a3)
	addiu   $at, $at, -1                 # (count down)
	andi    $t3, $t0, 0xff
	srl     $t4, $t0, 8
	andi    $t4, $t4, 0xff
	srl     $t5, $t0, 16
	andi    $t5, $t5, 0xff
	addu    $t3, $t3, $t8
	addu    $t4, $t4, $t8
	beqz    $a2, dr_tri_emit
	addu    $t5, $t5, $t8
.if R_SLOW_C
	b       dr_tri_skip
	nop
.endif
	lw      $t0, SP_SZ($t3)
	lw      $t1, SP_SZ($t4)
	lw      $t2, SP_SZ($t5)
	sltu    $t0, $t0, $t7
	sltu    $t1, $t1, $t7
	sltu    $t2, $t2, $t7
	or      $t0, $t0, $t1
	or      $t0, $t0, $t2
	bnez    $t0, dr_tri_skip
	lw      $t0, SP_SXY($t3)             # size check, as for quads
	lw      $t1, SP_SXY($t4)
	lw      $t2, SP_SXY($t5)
	subu    $t1, $t1, $t0
	subu    $t2, $t2, $t0
	SIZEOK  $t1, dr_tri_skip
	SIZEOK  $t2, dr_tri_skip
dr_tri_emit:                             # POLY_GT3
	lw      $t0, SP_COL($t3)             # c0
	lw      $t1, 4($a3)                  # uv0 | uv1 << 16
	lw      $t3, SP_SXY($t3)             # xy0
	lui     $t9, CODE_GT3 >> 16
	lw      $t2, SP_COL($t4)             # c1
	lw      $t4, SP_SXY($t4)             # xy1
	or      $t0, $t0, $t9
	sw      $t0, 4($s6)
	sw      $t3, 8($s6)
	andi    $t0, $t1, 0xffff
	or      $t0, $t0, $v0                # uv0 | clut
	sw      $t0, 12($s6)
	sw      $t2, 16($s6)
	sw      $t4, 20($s6)
	srl     $t1, $t1, 16
	or      $t1, $t1, $v1                # uv1 | tpage
	sw      $t1, 24($s6)
	lw      $t0, SP_COL($t5)             # c2
	lw      $t1, SP_SXY($t5)             # xy2
	lw      $t2, 8($a3)                  # uv2 | pad (0, see qw_tri)
	sw      $t0, 28($s6)
	sw      $t1, 32($s6)
	sw      $t2, 36($s6)
	lui     $t9, 0x0900
	or      $t9, $t9, $fp                # tag: 9 words
	sw      $t9, 0($s6)
	move    $fp, $s6
	addiu   $s6, $s6, 40
dr_tri_next:
	bgtz    $at, dr_tri
	addiu   $a3, $a3, POLY_SIZE
dr_next:
	lw      $a0, SP_NEXTREC($t8)
	b       dr_rec
	nop

# Near polygon: hand it to the slow path (engine/r_slow.c) — near clip,
# subdivision, exact projection. t0 = 4 (quad) or 3 (triangle).
dr_quad_skip:
	b       dr_slow_call
	li      $t0, 4
dr_tri_skip:
	li      $t0, 3
dr_slow_call:
	sw      $at, SP_SAVE+0($t8)
	sw      $a0, SP_SAVE+4($t8)
	sw      $a1, SP_SAVE+8($t8)
	sw      $a2, SP_SAVE+12($t8)
	sw      $a3, SP_SAVE+16($t8)
	sw      $v0, SP_SAVE+20($t8)
	sw      $v1, SP_SAVE+24($t8)
	sw      $t0, SP_SAVE+32($t8)
.if R_PROFILE
	lw      $t1, SP_SKIPPED($t8)         # polygons sent to the slow path
	lhu     $t2, TIMER2($t8)
	addiu   $t1, $t1, 1
	sw      $t1, SP_SKIPPED($t8)
	sw      $t2, SP_SAVE+36($t8)         # slow-path start time
.endif
.if R_SLOW_C
	sw      $s6, RS_PKT($s0)             # the C side emits through r_state
	sw      $fp, RS_OT_HEAD($s0)
	move    $a1, $a0                     # record
	move    $a2, $a3                     # polygon
	move    $a3, $t0                     # 4 or 3
	move    $a0, $s0                     # r_state
	jal     r_slow
	addiu   $sp, $sp, -16                # o32 argument area
	addiu   $sp, $sp, 16
	lui     $t8, 0x1f80
	lw      $s6, RS_PKT($s0)
	lw      $fp, RS_OT_HEAD($s0)
.else
	move    $a1, $a3                     # polygon
	jal     slow_poly                    # a0 = record, a1 = polygon, a2 = 4 / 3
	move    $a2, $t0
.endif
.if R_PROFILE
	lhu     $t0, TIMER2($t8)
	lw      $t1, SP_SAVE+36($t8)
	lw      $t2, SP_TSLOW($t8)
	subu    $t0, $t0, $t1
	andi    $t0, $t0, 0xffff
	addu    $t2, $t2, $t0
	sw      $t2, SP_TSLOW($t8)
.endif
	lw      $at, SP_SAVE+0($t8)
	lw      $a0, SP_SAVE+4($t8)
	lw      $a1, SP_SAVE+8($t8)
	lw      $a2, SP_SAVE+12($t8)
	lw      $a3, SP_SAVE+16($t8)
	lw      $v0, SP_SAVE+20($t8)
	lw      $v1, SP_SAVE+24($t8)
	lw      $t0, SP_SAVE+32($t8)
	lw      $t7, SP_SZFAST($t8)
	li      $t1, 4
	beq     $t0, $t1, dr_quad_next
	nop
	b       dr_tri_next
	nop

dr_full:
	li      $t0, 1
.if R_PROFILE
	sw      $t0, SP_FULL($t8)
	lw      $t0, SP_SKIPPED($t8)
	nop
	addiu   $t0, $t0, 1
	sw      $t0, SP_SKIPPED($t8)
dr_done:
	lhu     $t0, TIMER2($t8)
	lw      $t9, SP_T0($t8)
	lw      $t1, SP_TBLOCK($t8)
	subu    $t0, $t0, $t9
	andi    $t0, $t0, 0xffff
	addu    $t1, $t1, $t0
	b       walk
	sw      $t1, SP_TBLOCK($t8)
.else
	b       walk
	sw      $t0, SP_FULL($t8)
.endif

walk_done:
	lw      $t0, SP_LASTTEX($t8)
	lw      $t1, SP_TWIN($t8)
	bltz    $t0, 1f                      # nothing drawn
	lui     $t9, 0x0100
	or      $t9, $t9, $fp                # the last run's texture window
	sw      $t9, 0($s6)                  # (room: s7 is QW_MAX_REC_PKT + 8
	sw      $t1, 4($s6)                  # short of the arena end)
	move    $fp, $s6
	addiu   $s6, $s6, 8
1:
	lui     $t0, 0x8000                  # back to a KSEG0 pointer for C
	or      $s6, $s6, $t0
	sw      $s6, RS_PKT($s0)
	lw      $t0, RS_OT_WORLD($s0)
	nop
	sw      $fp, 0($t0)
.if R_PROFILE
	lw      $t0, SP_NODES($t8)
	lw      $t1, SP_RECORDS($t8)
	lw      $t2, SP_VERTS($t8)
	lw      $t3, SP_POLYS($t8)
	lw      $t4, SP_SKIPPED($t8)
	lw      $t5, SP_TBLOCK($t8)
	sw      $t0, RS_STAT_NODES($s0)
	sw      $t1, RS_STAT_RECORDS($s0)
	sw      $t2, RS_STAT_VERTS($s0)
	sw      $t3, RS_STAT_POLYS($s0)
	sw      $t4, RS_STAT_SKIPPED($s0)
	sw      $t5, RS_TICKS_BLOCK($s0)
	lw      $t0, SP_SLOWTRI($t8)
	lw      $t1, RS_STAT_SLOW($s0)
	lw      $t2, SP_TSLOW($t8)
	addu    $t1, $t1, $t0                # the C reference adds its own count
	sw      $t1, RS_STAT_SLOW($s0)
	sw      $t2, RS_TICKS_SLOW($s0)
.endif

	lw      $ra, 44($sp)                 # first: jr must not sit in its delay slot
	lw      $s0,  8($sp)
	lw      $s1, 12($sp)
	lw      $s2, 16($sp)
	lw      $s3, 20($sp)
	lw      $s4, 24($sp)
	lw      $s5, 28($sp)
	lw      $s6, 32($sp)
	lw      $s7, 36($sp)
	lw      $fp, 40($sp)
	jr      $ra
	addiu   $sp, $sp, 48

# -----------------------------------------------------------------------------
# plane_dist: v0 = n . cam - dist for node a0 (x4096, vertex units).
# Axial planes skip the products. Clobbers t0-t4, v0.
plane_dist:
	lhu     $t0, N_PLANE3($a0)           # the node's plane, in the shared
	lw      $t1, SP_PLANES($t8)          # table (dist in 1/8 units)
	sll     $t0, $t0, 2
	addu    $t5, $t0, $t1
	lbu     $t0, PN_TYPE($t5)
	lw      $t1, PN_DIST($t5)
	nop
	sra     $t1, $t1, 1                  # -> vertex units, x4096
	sltiu   $t2, $t0, 3
	beqz    $t2, pd_general
	sll     $t0, $t0, 2
	addu    $t0, $t0, $t8
	lw      $v0, SP_CAM($t0)
	nop
	sll     $v0, $v0, 12
	jr      $ra
	subu    $v0, $v0, $t1
pd_general:
	lh      $t2, PN_N+0($t5)
	lw      $t3, SP_CAM+0($t8)
	nop
	mult    $t2, $t3
	lh      $t2, PN_N+2($t5)
	lw      $t3, SP_CAM+4($t8)
	mflo    $v0
	nop
	nop
	mult    $t2, $t3
	lh      $t2, PN_N+4($t5)
	lw      $t3, SP_CAM+8($t8)
	mflo    $t4
	addu    $v0, $v0, $t4
	nop
	mult    $t2, $t3
	nop
	mflo    $t4
	addu    $v0, $v0, $t4
	jr      $ra
	subu    $v0, $v0, $t1

# -----------------------------------------------------------------------------
# mark_pvs: a0 = new camera leaf. visframe++, then walk the leaf's
# RLE-compressed PVS row (a zero byte is followed by a count of zero bytes;
# no row = everything visible) and stamp each visible leaf and its
# not-yet-stamped ancestors. Clobbers t0-t3, t9, a1-a3, v1, s5.
mark_pvs:
	lhu     $t9, TIMER2($t8)
	lw      $t7, RS_NODES($s0)           # parents are word offsets from here
	li      $t6, 0xffff                  # no parent
	sw      $a0, RS_VIEW_LEAF($s0)
	sw      $zero, RS_FORCE_MARK($s0)
	sw      $t9, SP_T0($t8)
	lw      $s5, RS_VISFRAME($s0)
	lw      $a1, L_VIS($a0)              # compressed row, or 0
	addiu   $s5, $s5, 1
	sw      $s5, RS_VISFRAME($s0)
	lw      $a2, RS_ROWBYTES($s0)
	lw      $a3, RS_LEAVES($s0)
	lw      $v1, RS_LEAVES_END($s0)
	addiu   $a3, $a3, L_SIZE             # bit 0 is leaf 1
mp_byte:
	blez    $a2, mp_done
	nop
	beqz    $a1, mp_bits
	li      $t0, 0xff                    # no row: all visible
	lbu     $t0, 0($a1)
	addiu   $a1, $a1, 1
	bnez    $t0, mp_bits
	nop
	lbu     $t1, 0($a1)                  # run of zero bytes
	addiu   $a1, $a1, 1
	subu    $a2, $a2, $t1
	sll     $t2, $t1, 8
	sll     $t1, $t1, 5
	subu    $t1, $t2, $t1                # bytes * 8 leaves * 28 B
	b       mp_byte
	addu    $a3, $a3, $t1
mp_bits:
	addiu   $a2, $a2, -1
	li      $t3, 8
mp_bit:
	andi    $t1, $t0, 1
	beqz    $t1, mp_next
	srl     $t0, $t0, 1
	sltu    $t1, $a3, $v1
	beqz    $t1, mp_next
	nop
	sh      $s5, L_VISFRAME($a3)
	lhu     $t2, L_PARENT($a3)
	nop
mp_up:
	beq     $t2, $t6, mp_next            # past the root
	sll     $t2, $t2, 2
	addu    $t2, $t2, $t7                # word offset -> node
	lhu     $t1, N_VISFRAME($t2)
	nop
	beq     $t1, $s5, mp_next            # ancestors already stamped
	nop
	sh      $s5, N_VISFRAME($t2)
	lhu     $t2, N_PARENT($t2)
	b       mp_up
	nop
mp_next:
	addiu   $t3, $t3, -1
	bgtz    $t3, mp_bit
	addiu   $a3, $a3, L_SIZE
	b       mp_byte
	nop
mp_done:
	lhu     $t0, TIMER2($t8)
	lw      $t9, SP_T0($t8)
	lw      $t1, RS_TICKS_MARK($s0)
	subu    $t0, $t0, $t9
	andi    $t0, $t0, 0xffff
	addu    $t1, $t1, $t0
	jr      $ra
	sw      $t1, RS_TICKS_MARK($s0)

	.size r_render, . - r_render
