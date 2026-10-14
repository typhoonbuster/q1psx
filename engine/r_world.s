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
#   s4 framecount   s5 visframe   s6 packet ptr   s7 packet end
#   fp OT world-bucket link (24 bits)   t8 scratchpad base (0x1F800000)

	.set noreorder
	.set noat

	.include "gtereg.inc"
	.include "inline_s.inc"
	.include "r_defs.inc"

# SIZEOK r, fail: r = SXY(corner) - SXY(corner 0); branch to `fail` unless
# |dy| <= 250 and |dx| <= 500 (packed-word difference: dy may be off by one,
# fine for a margin). Uses a1.
.macro SIZEOK r, fail
	sra     $a1, \r, 16
	addiu   $a1, $a1, 250
	sltiu   $a1, $a1, 501
	beqz    $a1, \fail
	sll     $a1, \r, 16
	sra     $a1, $a1, 16
	addiu   $a1, $a1, 500
	sltiu   $a1, $a1, 1001
	beqz    $a1, \fail
	nop
.endm

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
	li      $t0, CODE_FT4
	li      $t1, CODE_FT3
	sw      $t0, SP_CODE4($t8)
	sw      $t1, SP_CODE3($t8)
	sw      $zero, SP_NODES($t8)
	sw      $zero, SP_RECORDS($t8)
	sw      $zero, SP_VERTS($t8)
	sw      $zero, SP_POLYS($t8)
	sw      $zero, SP_SKIPPED($t8)
	sw      $zero, SP_TBLOCK($t8)
	sw      $zero, SP_FULL($t8)
	sw      $zero, SP_SLOWTRI($t8)
	sw      $zero, SP_TSLOW($t8)
	lw      $s4, RS_FRAMECOUNT($s0)
	lw      $s6, RS_PKT($s0)
	lw      $s7, RS_PKT_END($s0)
	lw      $t0, RS_OT_WORLD($s0)
	nop
	lw      $fp, 0($t0)

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

walk:
	andi    $t0, $s1, 1
	bnez    $t0, walk_leaf
	nop
	lhu     $t0, N_VISFRAME($s1)
	nop
	bne     $t0, $s5, pop                # not in the PVS
	nop
	lw      $t0, SP_NODES($t8)
	nop
	addiu   $t0, $t0, 1
	beqz    $s2, wn_side                 # fully inside the frustum already
	sw      $t0, SP_NODES($t8)
	jal     frustum
	move    $a0, $s1
	bltz    $v0, pop                     # culled
	nop
	move    $s2, $v0
wn_side:
	jal     plane_dist
	move    $a0, $s1
	slt     $t0, $v0, $zero              # side: 1 if the camera is behind
	sra     $t1, $v0, 31
	xor     $t2, $v0, $t1
	subu    $t2, $t2, $t1                # |d|
	slti    $t2, $t2, BACKFACE_EPS + 1
	xori    $t2, $t2, 1                  # draw faces only if |d| > eps
	sll     $t3, $s1, 8
	srl     $t3, $t3, 8                  # node pointer, low 24 bits
	sll     $t4, $s2, 24
	or      $t3, $t3, $t4
	sll     $t4, $t0, 28
	or      $t3, $t3, $t4
	sll     $t4, $t2, 29
	or      $t3, $t3, $t4
	sw      $t3, SP_STACK($s3)           # push (node, flags, side, draw)
	addiu   $s3, $s3, 4
	sll     $t0, $t0, 2
	addu    $t0, $t0, $s1
	lw      $s1, N_CHILD($t0)            # near child first
	b       walk
	nop

walk_leaf:
	addiu   $a0, $s1, -1
	lhu     $t0, L_VISFRAME($a0)
	nop
	bne     $t0, $s5, pop
	nop
	beqz    $s2, wl_marks
	nop
	jal     frustum
	nop
	bltz    $v0, pop
	addiu   $a0, $s1, -1
wl_marks:
	lhu     $t1, L_NMARKS($a0)
	lw      $t2, L_MARKS($a0)
	lw      $t3, SP_FVBASE($t8)
	beqz    $t1, pop
	nop
wl_mark:
	lhu     $t0, 0($t2)
	addiu   $t2, $t2, 2
	sll     $t0, $t0, 1
	addu    $t0, $t0, $t3
	addiu   $t1, $t1, -1
	bgtz    $t1, wl_mark
	sh      $s4, 0($t0)                  # face_visframe[mark] = framecount

pop:
	beq     $s3, $t8, walk_done
	nop
	addiu   $s3, $s3, -4
	lw      $t0, SP_STACK($s3)
	lui     $t1, 0x8000
	sll     $t2, $t0, 8
	srl     $t2, $t2, 8
	or      $t2, $t2, $t1                # node pointer
	srl     $s2, $t0, 24
	andi    $s2, $s2, 0xf                # clip flags
	srl     $t3, $t0, 28
	andi    $t4, $t3, 2                  # draw flag
	andi    $t3, $t3, 1                  # side
	xori    $t5, $t3, 1
	sll     $t5, $t5, 2
	addu    $t5, $t5, $t2
	lw      $s1, N_CHILD($t5)            # far child: walked after the faces
	beqz    $t4, walk
	sll     $t3, $t3, 2
	addu    $t3, $t3, $t2
	lw      $a0, N_FACES($t3)            # block on the camera's side
	lw      $a1, N_FACES+4($t3)
	nop
	beq     $a0, $a1, walk
	nop
	jal     draw_records
	nop
	b       walk
	nop

walk_done:
	sw      $s6, RS_PKT($s0)
	lw      $t0, RS_OT_WORLD($s0)
	nop
	sw      $fp, 0($t0)
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
	lbu     $t0, N_TYPE($a0)
	lw      $t1, N_DIST($a0)
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
	lh      $t2, N_N+0($a0)
	lw      $t3, SP_CAM+0($t8)
	nop
	mult    $t2, $t3
	lh      $t2, N_N+2($a0)
	lw      $t3, SP_CAM+4($t8)
	mflo    $v0
	nop
	nop
	mult    $t2, $t3
	lh      $t2, N_N+4($a0)
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
# frustum: cull the bbox of node/leaf a0 (centre + half extents at +0/+8,
# vertex units) against the planes still set in s2. Returns v0 = remaining
# clip flags (a plane is dropped once the box is fully inside it), or -1 if
# the box is outside. View space: x right, y down, z forward, H = 160 on a
# 320x240 screen -> right z-x>=0, left z+x>=0, bottom 3z-4y>=0, top 3z+4y>=0.
# The box is taken to view space as centre (rotation + translation) and
# radius (|rotation| x half extents). Clobbers t0-t7, t9, v0.
frustum:
	lwc2    C2_VXY0, N_C+0($a0)
	lwc2    C2_VZ0,  N_C+4($a0)
	lwc2    C2_VXY1, N_H+0($a0)
	lwc2    C2_VZ1,  N_H+4($a0)
	move    $v0, $s2
	nop
	MVMVA   1, 0, 0, 0, 0                # IR = rot * centre + TR
	mfc2    $t0, C2_IR1                  # x
	mfc2    $t1, C2_IR2                  # y
	mfc2    $t2, C2_IR3                  # z
	nop
	MVMVA   1, 1, 1, 3, 0                # IR = |rot| * half extents
	mfc2    $t3, C2_IR1                  # rx
	mfc2    $t4, C2_IR2                  # ry
	mfc2    $t5, C2_IR3                  # rz
	sll     $t7, $t4, 2                  # 4 ry
	addu    $t6, $t5, $t3                # rz + rx: radius for left/right
	sll     $t9, $t5, 1
	addu    $t9, $t9, $t5
	addu    $t7, $t7, $t9                # 3 rz + 4 ry: radius for top/bottom

	andi    $t3, $v0, 1                  # right: z - x
	beqz    $t3, f_left
	subu    $t3, $t2, $t0
	addu    $t4, $t3, $t6
	bltz    $t4, f_cull
	subu    $t4, $t3, $t6
	bltz    $t4, f_left
	nop
	xori    $v0, $v0, 1
f_left:
	andi    $t3, $v0, 2                  # left: z + x
	beqz    $t3, f_bottom
	addu    $t3, $t2, $t0
	addu    $t4, $t3, $t6
	bltz    $t4, f_cull
	subu    $t4, $t3, $t6
	bltz    $t4, f_bottom
	nop
	xori    $v0, $v0, 2
f_bottom:
	sll     $t9, $t2, 1
	addu    $t9, $t9, $t2                # 3z
	sll     $t5, $t1, 2                  # 4y
	andi    $t3, $v0, 4                  # bottom: 3z - 4y
	beqz    $t3, f_top
	subu    $t3, $t9, $t5
	addu    $t4, $t3, $t7
	bltz    $t4, f_cull
	subu    $t4, $t3, $t7
	bltz    $t4, f_top
	nop
	xori    $v0, $v0, 4
f_top:
	andi    $t3, $v0, 8                  # top: 3z + 4y
	beqz    $t3, f_ret
	addu    $t3, $t9, $t5
	addu    $t4, $t3, $t7
	bltz    $t4, f_cull
	subu    $t4, $t3, $t7
	bltz    $t4, f_ret
	nop
	xori    $v0, $v0, 8
f_ret:
	jr      $ra
	nop
f_cull:
	jr      $ra
	li      $v0, -1

# -----------------------------------------------------------------------------
# mark_pvs: a0 = new camera leaf. visframe++, then walk the leaf's
# RLE-compressed PVS row (a zero byte is followed by a count of zero bytes;
# no row = everything visible) and stamp each visible leaf and its
# not-yet-stamped ancestors. Clobbers t0-t3, t9, a1-a3, v1, s5.
mark_pvs:
	lhu     $t9, TIMER2($t8)
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
	addu    $t1, $t1, $t2                # bytes * 8 leaves * 36 B
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
	lw      $t2, L_PARENT($a3)
	nop
mp_up:
	beqz    $t2, mp_next
	nop
	lhu     $t1, N_VISFRAME($t2)
	nop
	beq     $t1, $s5, mp_next            # ancestors already stamped
	nop
	sh      $s5, N_VISFRAME($t2)
	lw      $t2, N_PARENT($t2)
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

# -----------------------------------------------------------------------------
# draw_records: a0 = first face record, a1 = block end. For each record whose
# face was stamped this frame: transform its vertices into the scratchpad
# (SXY / SZ, 4 bytes per vertex), then emit its quads and triangles.
# A record with a vertex nearer than sz_fast takes the careful loop, which
# skips the polygons touching such a vertex (slow path: R3.3).
# Clobbers at, v0, v1, a0-a3, t0-t7, t9.
draw_records:
	lhu     $t9, TIMER2($t8)
	sw      $a1, SP_BLKEND($t8)          # a1 is a scratch register below
	sw      $t9, SP_T0($t8)
dr_rec:
	lw      $a1, SP_BLKEND($t8)
	nop
	beq     $a0, $a1, dr_done
	nop
	lw      $t0, SP_FVBASE($t8)
	lhu     $t1, R_FACE_ID($a0)
	lhu     $t2, R_SIZE($a0)
	sll     $t1, $t1, 1
	addu    $t1, $t1, $t0
	lhu     $t1, 0($t1)
	addu    $t2, $t2, $a0                # next record
	beq     $t1, $s4, dr_draw            # stamped this frame?
	sw      $t2, SP_NEXTREC($t8)
	b       dr_rec
	move    $a0, $t2

dr_draw:
	lw      $t0, SP_FULL($t8)
	lbu     $t2, R_NVERTS($a0)
	bnez    $t0, dr_done                 # packet arena full
	lhu     $v0, R_CLUT($a0)

	# bounding-sphere frustum test (view space x6; r in vertex units, view
	# radius 1.5 r): left/right planes have |n| = sqrt2 -> 2.125 r, top/bottom
	# |n| = 5 -> 8 r. All conservative.
	# VZ0 is loaded from a sign-extended halfword, not with lwc2: the word
	# at +16 carries the radius in its upper half, and PCSX-Redux does not
	# drop those bits the way the 16-bit hardware register does.
	lwc2    C2_VXY0, R_SC+0($a0)
	lh      $t9, R_SC+4($a0)
	lhu     $t3, R_SR($a0)
	mtc2    $t9, C2_VZ0
	lhu     $v1, R_TPAGE($a0)
	nop
	MVMVA   1, 0, 0, 0, 0                # IR = rot * centre + TR
	mfc2    $t0, C2_IR1                  # x
	mfc2    $t1, C2_IR2                  # y
	mfc2    $t4, C2_IR3                  # z
	sll     $t5, $t3, 1
	srl     $t6, $t3, 3
	addu    $t5, $t5, $t6                # 2.125 r
	subu    $t6, $t4, $t0                # right: z - x
	addu    $t6, $t6, $t5
	bltz    $t6, dr_rec_next
	addu    $t6, $t4, $t0                # left: z + x
	addu    $t6, $t6, $t5
	bltz    $t6, dr_rec_next
	sll     $t5, $t3, 3                  # 8 r
	sll     $t6, $t4, 1
	addu    $t6, $t6, $t4                # 3z
	sll     $t7, $t1, 2                  # 4y
	subu    $t9, $t6, $t7                # bottom: 3z - 4y
	addu    $t9, $t9, $t5
	bltz    $t9, dr_rec_next
	addu    $t9, $t6, $t7                # top: 3z + 4y
	addu    $t9, $t9, $t5
	bltz    $t9, dr_rec_next
	sll     $t5, $t3, 1                  # delay: 2r
	lw      $t7, SP_SZFAST($t8)
	subu    $t5, $t4, $t5                # nearest possible SZ (>= 1.5 r away)
	slt     $a2, $t5, $t7                # a2 bit 0: may come too near
	sll     $t5, $t3, 1
	addu    $t5, $t5, $t3                # 3r
	slt     $t5, $t4, $t5                # z < 3r: a polygon may exceed the
	sll     $t5, $t5, 1                  # GPU's 1023 x 511 px limit
	or      $a2, $a2, $t5                # a2 bit 1

	sll     $v0, $v0, 16                 # clut  << 16, ORed into uv0
	sll     $v1, $v1, 16                 # tpage << 16, ORed into uv1
	lw      $t0, SP_RECORDS($t8)
	lw      $t1, SP_VERTS($t8)
	addiu   $t0, $t0, 1
	addu    $t1, $t1, $t2
	sw      $t0, SP_RECORDS($t8)
	sw      $t1, SP_VERTS($t8)

	addiu   $t3, $a0, R_VERTS            # vertex source
	move    $t4, $t8                     # cache cursor
	move    $t5, $t2                     # vertices left
dr_xf:
	lwc2    C2_VXY0, 0($t3)
	lwc2    C2_VZ0,  4($t3)
	lwc2    C2_VXY1, 8($t3)
	lwc2    C2_VZ1, 12($t3)
	lwc2    C2_VXY2,16($t3)
	lwc2    C2_VZ2, 20($t3)
	addiu   $t3, $t3, 24
	addiu   $t5, $t5, -3
	RTPT
	swc2    C2_SXY0, SP_SXY+0($t4)
	swc2    C2_SXY1, SP_SXY+4($t4)
	swc2    C2_SXY2, SP_SXY+8($t4)
	swc2    C2_SZ1,  SP_SZ+0($t4)
	swc2    C2_SZ2,  SP_SZ+4($t4)
	swc2    C2_SZ3,  SP_SZ+8($t4)
	bgtz    $t5, dr_xf
	addiu   $t4, $t4, 12

	beqz    $a2, dr_polys                # sphere says: nothing near or big
	move    $t4, $t8
	andi    $t0, $a2, 2
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

	sll     $t0, $t2, 3
	addu    $a3, $a0, $t0
	addiu   $a3, $a3, R_VERTS            # first polygon
	lbu     $at, R_NQUADS($a0)
	lw      $t1, SP_POLYS($t8)
	lbu     $t0, R_NTRIS($a0)
	addu    $t1, $t1, $at
	addu    $t1, $t1, $t0
	beqz    $at, dr_tris
	sw      $t1, SP_POLYS($t8)

dr_quad:
	addiu   $t0, $s6, 40
	sltu    $t0, $s7, $t0
	bnez    $t0, dr_full
	lw      $t0, 0($a3)                  # 4 vertex offsets
	nop
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
	nop
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
dr_quad_emit:
	lw      $t1, 4($a3)                  # uv0 | uv1 << 16
	lw      $t2, 8($a3)                  # uv2 | uv3 << 16
	lw      $t3, SP_SXY($t3)
	lw      $t4, SP_SXY($t4)
	lw      $t5, SP_SXY($t5)
	lw      $t6, SP_SXY($t6)
	andi    $t0, $t1, 0xffff
	or      $t0, $t0, $v0                # uv0 | clut
	srl     $t1, $t1, 16
	or      $t1, $t1, $v1                # uv1 | tpage
	lui     $t9, 0x0900
	or      $t9, $t9, $fp                # tag: 9 words, link to previous head
	sw      $t9, 0($s6)
	lw      $t9, SP_CODE4($t8)
	sw      $t3, 8($s6)
	sw      $t9, 4($s6)
	sw      $t0, 12($s6)
	sw      $t4, 16($s6)
	sw      $t1, 20($s6)
	sw      $t5, 24($s6)
	andi    $t0, $t2, 0xffff
	sw      $t0, 28($s6)
	srl     $t2, $t2, 16
	sw      $t6, 32($s6)
	sw      $t2, 36($s6)
	sll     $fp, $s6, 8
	srl     $fp, $fp, 8                  # new bucket head
	addiu   $s6, $s6, 40
dr_quad_next:
	addiu   $at, $at, -1
	bgtz    $at, dr_quad
	addiu   $a3, $a3, POLY_SIZE

dr_tris:
	lbu     $at, R_NTRIS($a0)
	nop
	beqz    $at, dr_rec_next
	nop
dr_tri:
	addiu   $t0, $s6, 32
	sltu    $t0, $s7, $t0
	bnez    $t0, dr_full
	lw      $t0, 0($a3)
	nop
	andi    $t3, $t0, 0xff
	srl     $t4, $t0, 8
	andi    $t4, $t4, 0xff
	srl     $t5, $t0, 16
	andi    $t5, $t5, 0xff
	addu    $t3, $t3, $t8
	addu    $t4, $t4, $t8
	beqz    $a2, dr_tri_emit
	addu    $t5, $t5, $t8
	lw      $t0, SP_SZ($t3)
	lw      $t1, SP_SZ($t4)
	lw      $t2, SP_SZ($t5)
	sltu    $t0, $t0, $t7
	sltu    $t1, $t1, $t7
	sltu    $t2, $t2, $t7
	or      $t0, $t0, $t1
	or      $t0, $t0, $t2
	bnez    $t0, dr_tri_skip
	nop
	lw      $t0, SP_SXY($t3)             # size check, as for quads
	lw      $t1, SP_SXY($t4)
	lw      $t2, SP_SXY($t5)
	subu    $t1, $t1, $t0
	subu    $t2, $t2, $t0
	SIZEOK  $t1, dr_tri_skip
	SIZEOK  $t2, dr_tri_skip
dr_tri_emit:
	lw      $t1, 4($a3)                  # uv0 | uv1 << 16
	lw      $t2, 8($a3)                  # uv2 | pad
	lw      $t3, SP_SXY($t3)
	lw      $t4, SP_SXY($t4)
	lw      $t5, SP_SXY($t5)
	andi    $t0, $t1, 0xffff
	or      $t0, $t0, $v0
	srl     $t1, $t1, 16
	or      $t1, $t1, $v1
	lui     $t9, 0x0700
	or      $t9, $t9, $fp
	sw      $t9, 0($s6)
	lw      $t9, SP_CODE3($t8)
	sw      $t3, 8($s6)
	sw      $t9, 4($s6)
	sw      $t0, 12($s6)
	sw      $t4, 16($s6)
	sw      $t1, 20($s6)
	andi    $t2, $t2, 0xffff
	sw      $t5, 24($s6)
	sw      $t2, 28($s6)
	sll     $fp, $s6, 8
	srl     $fp, $fp, 8
	addiu   $s6, $s6, 32
dr_tri_next:
	addiu   $at, $at, -1
	bgtz    $at, dr_tri
	addiu   $a3, $a3, POLY_SIZE

dr_rec_next:
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
	lw      $t1, SP_SKIPPED($t8)         # counts polygons sent to the slow path
	sw      $at, SP_SAVE+0($t8)
	sw      $a0, SP_SAVE+4($t8)
	sw      $a1, SP_SAVE+8($t8)
	sw      $a2, SP_SAVE+12($t8)
	sw      $a3, SP_SAVE+16($t8)
	sw      $v0, SP_SAVE+20($t8)
	sw      $v1, SP_SAVE+24($t8)
	sw      $ra, SP_SAVE+28($t8)
	sw      $t0, SP_SAVE+32($t8)
	addiu   $t1, $t1, 1
	sw      $t1, SP_SKIPPED($t8)
	lhu     $t1, TIMER2($t8)
	nop
	sw      $t1, SP_SAVE+36($t8)         # slow-path start time
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
	lhu     $t0, TIMER2($t8)
	lw      $t1, SP_SAVE+36($t8)
	lw      $t2, SP_TSLOW($t8)
	subu    $t0, $t0, $t1
	andi    $t0, $t0, 0xffff
	addu    $t2, $t2, $t0
	sw      $t2, SP_TSLOW($t8)
	lw      $at, SP_SAVE+0($t8)
	lw      $a0, SP_SAVE+4($t8)
	lw      $a1, SP_SAVE+8($t8)
	lw      $a2, SP_SAVE+12($t8)
	lw      $a3, SP_SAVE+16($t8)
	lw      $v0, SP_SAVE+20($t8)
	lw      $v1, SP_SAVE+24($t8)
	lw      $ra, SP_SAVE+28($t8)
	lw      $t0, SP_SAVE+32($t8)
	lw      $t7, SP_SZFAST($t8)
	li      $t1, 4
	beq     $t0, $t1, dr_quad_next
	nop
	b       dr_tri_next
	nop

dr_full:
	li      $t0, 1
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
	jr      $ra
	sw      $t1, SP_TBLOCK($t8)

	.size r_render, . - r_render
