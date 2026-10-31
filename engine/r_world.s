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
#   s0 the plane table (r_state before and after, and in SP_RSTATE)
#   s1 current node / leaf|1 (KUSEG pointers)   s2 clip flags   s3 stack ptr
#   s4 framecount   s5 visframe   s6 packet ptr   s7 packet end (minus
#   QW_MAX_REC_PKT + 48)   fp OT world-bucket link (24 bits)   t8 scratchpad base
#   (0x1F800000)   v1 tpage << 16 of the last drawn texture (draw_records'
#   texture cache: the walk must leave v1 alone)

	.set noreorder
	.set noat

	.include "gtereg.inc"
	.include "inline_s.inc"
	.include "r_defs.inc"

# The far polygons' screen bounds (CAREFUL's other exits, below): each test's
# "lui v0, 240" and "lui v0, 320" as a pair in this table, which render.c
# r_set_res patches to a smaller target's rows and width (the 3D view's
# internal resolution, docs/porting-notes.md §78). FARSITE in each. A pair
# is one word (SITES, the link moving the renderer's chains apart, §81.5):
# the first's address, the second's distance from it in bits 21-30 (the
# addresses' bits 21-30 are 0: 2 MB of RAM in KSEG0); the two lie in one
# chain.
	.pushsection .rodata.r_far_sites, "a", @progbits
	.align  2
	.global r_far_sites
r_far_sites:
	.popsection

.macro FARSITE rows, cols
	.pushsection .rodata.r_far_sites, "a", @progbits
	.word   \rows + ((\cols - \rows) << 21)
	.popsection
.endm

# The frustum's vertical planes (FRUSTUM's boxes, the records' and the
# sorted models' spheres): 3z -+ 4y, the spheres 8 r from them. A target
# whose pixels are not square (256 x 224) takes the view's y row x b
# (render.c r_set_view, b up to 7/6): r_set_res then makes them 7z -+ 8y,
# the spheres 24 r from them (rho x 16: the sphere an ellipsoid, its reach
# sqrt(64 b^2 + 49) x 1.5 r = 17.5 r at most). YSITE2 puts two
# instructions in this table as FARSITE does, each one's kind in its low
# bits (the first's in bits 0-1, the second's in bits 21-22): 0 sll 2 (4y) or 3 (8y),
# 1 sll 1 or 3 (the 3z's or 7z's first), 2 addu or subu (its second), 3
# sll t5, t3, 3 (8 r) or sll t5, t7, 4 (24 r: t7 = rho there).
	.pushsection .rodata.r_ysites, "a", @progbits
	.align  2
	.global r_ysites
r_ysites:
	.popsection

.macro YSITE2 a, ka, b, kb              # (none in a R_YSLOPE 5 build: render.c
.if R_YSLOPE == 4                        #  then has no 256 x 224)
	.pushsection .rodata.r_ysites, "a", @progbits
	.word   \a + \ka + ((\b - \a + \kb) << 21)
	.popsection
.endif
.endm

# FRUSTUM cull: test the box whose centre / half extents are in V0 / V1
# (vertex units; loaded by the caller at least two instructions back) against
# the frustum planes still set in s2. A plane's bit is cleared once the box is
# fully inside it; branches to `cull` if the box is outside one. View space:
# x right, y down, z forward, H = 160 on a 320x240 screen -> right z-x >= 0,
# left z+x >= 0, bottom 3z-4y >= 0, top 3z+4y >= 0. The box goes to view space
# as centre (rotation + translation) and radius (|rotation| x half extents).
# Clobbers t0-t7, t9, at, v0, a1. s1a..s1c / s2a, s2b: the caller's
# instructions for the two MVMVAs' latency (8 cycles each), if it has any.
.macro FRUSTUM cull, s1a=, s1b=, s1c=, s2a=, s2b=
	MVMVA   1, 0, 0, 0, 0                # IR = rot * centre + TR
	andi    $at, $s2, 1                  # (right plane still set?)
	\s1a
	\s1b
	\s1c
	mfc2    $t0, C2_IR1                  # x
	mfc2    $t1, C2_IR2                  # y
	mfc2    $t2, C2_IR3                  # z
.Lfy\@:
	sll     $v0, $t1, 2                  # 4y
	MVMVA   1, 1, 1, 3, 0                # IR = |rot| * half extents
.Lfz\@:
	sll     $a1, $t2, 1
.if R_YSLOPE == 5
	addu    $v0, $v0, $t1                # 5y (a 192-row viewport: R_YSLOPE)
.endif
.Lfz2\@:
	addu    $a1, $a1, $t2                # 3z
	\s2a
	\s2b
	mfc2    $t3, C2_IR1                  # rx
	mfc2    $t4, C2_IR2                  # ry
	mfc2    $t5, C2_IR3                  # rz
.Lfry\@:
	sll     $t7, $t4, 2                  # 4 ry
	addu    $t6, $t5, $t3                # rz + rx: radius for left/right
.Lfrz\@:
	sll     $t9, $t5, 1
.if R_YSLOPE == 5
	addu    $t7, $t7, $t4                # 5 ry
.endif
.Lfrz2\@:
	addu    $t9, $t9, $t5
	addu    $t7, $t7, $t9                # 3 rz + 4 ry: radius for top/bottom
	YSITE2  .Lfy\@, 0, .Lfz\@, 1
	YSITE2  .Lfz2\@, 2, .Lfry\@, 0
	YSITE2  .Lfrz\@, 1, .Lfrz2\@, 2
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

# XFORM sz, loop, one, done [, light, fog]: a record's vertices into the
# scratchpad cache (t3 = its indices, t9 = the vertex pool, at = its light
# bytes, t4 = the cache cursor, t5 = vertex count - 2): RTPT while three are
# left, then one RTPS each, SXY / light (grey 0x00cccccc) per vertex, SZ too
# if `sz`. On to `done` (from the RTPT loop, when no vertex is left) or past
# the end. light = 0 (the flat shading's): SXY only, no light. fog = 1 (the
# draw distance's): each light times its vertex's factor in r_fogtab (t2 =
# the table, by SZ / 64, 128 = as it is), t7 clobbered.
.macro XFORM sz, loop, one, done, light=1, fog=0
\loop:
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
.if \light == 2
	sw      $at, SP_COL+0($t4)           # the record's one colour
	sw      $at, SP_COL+4($t4)
	sw      $at, SP_COL+8($t4)
.endif
.if \light == 1 || \light == 3
.if \light == 1
	lbu     $t0, 0($at)                  # while the GTE works: the three
	lbu     $t1, 1($at)                  # lights, grey c -> 0x00cccccc
	lbu     $t6, 2($at)                  # (x = c | c << 8; x | x << 8)
	addiu   $at, $at, 3
.else
	lbu     $t0, 7($t0)                  # light 3: each vertex's own, byte 7 of
	lbu     $t1, 7($t1)                  # its pool's entry (light_pool's; the
	lbu     $t6, 7($t6)                  # addresses the loads above used)
.endif
.if \fog
	mfc2    $a3, C2_SZ1                  # (waits for the RTPT) each light x
	mfc2    $t7, C2_SZ2                  # its depth's factor / 128
	srl     $a3, $a3, 6
	addu    $a3, $a3, $t2
	lbu     $a3, 0($a3)
	srl     $t7, $t7, 6
	addu    $t7, $t7, $t2
	mult    $t0, $a3
	lbu     $t7, 0($t7)
	mfc2    $a3, C2_SZ3
	mflo    $t0                          # (two instructions at least between
	srl     $a3, $a3, 6                  #  an mflo and the next mult)
	addu    $a3, $a3, $t2
	lbu     $a3, 0($a3)
	srl     $t0, $t0, 7
	mult    $t1, $t7
	sll     $t7, $t0, 8
	or      $t0, $t0, $t7
	sll     $t7, $t0, 8
	or      $t0, $t0, $t7
	mflo    $t1
	sw      $t0, SP_COL+0($t4)
	srl     $t1, $t1, 7
	mult    $t6, $a3
	sll     $t7, $t1, 8
	or      $t1, $t1, $t7
	sll     $t7, $t1, 8
	or      $t1, $t1, $t7
	mflo    $t6
	sw      $t1, SP_COL+4($t4)
	srl     $t6, $t6, 7
	sll     $t7, $t6, 8
	or      $t6, $t6, $t7
	sll     $t7, $t6, 8
	or      $t6, $t6, $t7
	sw      $t6, SP_COL+8($t4)
.else
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
.endif
.endif
	swc2    C2_SXY0, SP_SXY+0($t4)
	swc2    C2_SXY1, SP_SXY+4($t4)
	swc2    C2_SXY2, SP_SXY+8($t4)
.if \sz
	swc2    C2_SZ1,  SP_SZ+0($t4)
	swc2    C2_SZ2,  SP_SZ+4($t4)
	swc2    C2_SZ3,  SP_SZ+8($t4)
.endif
	bgtz    $t5, \loop
	addiu   $t4, $t4, 12
	addiu   $t5, $t5, 2                  # 0, 1 or 2 left: one RTPS each
	blez    $t5, \done                   # (rather than a padded RTPT)
	lhu     $t0, 0($t3)
\one:
	addiu   $t3, $t3, 2
	sll     $t0, $t0, 3
	addu    $t0, $t0, $t9
	lwc2    C2_VXY0, 0($t0)
	lwc2    C2_VZ0,  4($t0)
.if \light == 1 || \light == 3
.if \light == 1
	lbu     $t1, 0($at)
	addiu   $at, $at, 1
.else
	lbu     $t1, 7($t0)                  # (light 3: the vertex's own)
	nop                                  # (two after the lwc2: the GTE's wait)
.endif
	RTPS
.if \fog
	addiu   $t5, $t5, -1
	lhu     $t0, 0($t3)                  # (the next index; the padding at the end)
	mfc2    $a3, C2_SZ3                  # (waits for the RTPS)
	swc2    C2_SXY2, SP_SXY($t4)
	srl     $a3, $a3, 6
	addu    $a3, $a3, $t2
	lbu     $a3, 0($a3)
.if \sz
	swc2    C2_SZ3,  SP_SZ($t4)
.else
	nop
.endif
	mult    $t1, $a3
	mflo    $t1
	srl     $t1, $t1, 7
	sll     $a3, $t1, 8
	or      $t1, $t1, $a3
	sll     $a3, $t1, 8
	or      $t1, $t1, $a3
	sw      $t1, SP_COL($t4)
.else
	sll     $a3, $t1, 8
	or      $t1, $t1, $a3
	sll     $a3, $t1, 8
	or      $t1, $t1, $a3
	sw      $t1, SP_COL($t4)
	addiu   $t5, $t5, -1
	lhu     $t0, 0($t3)                  # (the next index; the padding at the end)
	swc2    C2_SXY2, SP_SXY($t4)
.if \sz
	swc2    C2_SZ3,  SP_SZ($t4)
.endif
.endif
.else
	addiu   $t5, $t5, -1                 # (two before the GTE command)
	lhu     $t0, 0($t3)                  # (the next index; the padding at the end)
	RTPS
.if \light == 2
	sw      $at, SP_COL($t4)
.endif
	swc2    C2_SXY2, SP_SXY($t4)
.if \sz
	swc2    C2_SZ3,  SP_SZ($t4)
.endif
.endif
	bgtz    $t5, \one
	addiu   $t4, $t4, 4
.endm

# MINMAX4 a, b, c, d, t [, tail]: a = the least, b = the greatest of the
# four (signed; all four clobbered, t a temporary). A swap goes through t,
# loaded in its branch's delay slot. `tail` fills the last branch's delay
# slot (default nop): an instruction of the caller's that reads neither b
# nor d and writes none of a, b, d. (Not at: the polygon loops count in it.)
.macro MINMAX4 a, b, c, d, t, tail=nop
	slt     \t, \b, \a
	beqz    \t, 1f
	move    \t, \a
	move    \a, \b
	move    \b, \t
1:	slt     \t, \d, \c
	beqz    \t, 2f
	move    \t, \c
	move    \c, \d
	move    \d, \t
2:	slt     \t, \c, \a
	beqz    \t, 3f
	slt     \t, \b, \d
	move    \a, \c
3:	beqz    \t, 4f
	\tail
	move    \b, \d
4:
.endm

# MINMAX4D: MINMAX4 with a second temporary u, which takes the second
# pair's compare into the first branch's delay slot.
.macro MINMAX4D a, b, c, d, t, u, tail=nop
	slt     \t, \b, \a
	beqz    \t, 1f
	slt     \u, \d, \c
	xor     \a, \a, \b
	xor     \b, \a, \b
	xor     \a, \a, \b
1:	beqz    \u, 2f
	move    \u, \c
	move    \c, \d
	move    \d, \u
2:	slt     \t, \c, \a
	beqz    \t, 3f
	slt     \t, \b, \d
	move    \a, \c
3:	beqz    \t, 4f
	\tail
	move    \b, \d
4:
.endm

# MINMAX3 a, b, c, t [, tail]: likewise for three; `tail` must not write c.
.macro MINMAX3 a, b, c, t, tail=nop
	slt     \t, \b, \a
	beqz    \t, 1f
	move    \t, \a
	move    \a, \b
	move    \b, \t
1:	slt     \t, \c, \a
	beqz    \t, 2f
	slt     \t, \b, \c
	move    \a, \c
2:	beqz    \t, 3f
	\tail
	move    \b, \c
3:
.endm

# CAREFUL quad, skip, var: a conservative test, per polygon of a record the
# sphere flags, before the subdivision's exact one (engine/r_slow.c): every
# corner at SZ >= 96 (var = 1: SP_SZCLIP, a model's own bound, one more
# load), W + H <= 500 px for the bbox and (W + H) x (zmax - zmin) <= 2 T2
# zmin. Then no edge splits (|dx| + |dy| <= W + H, |dz| <= the range,
# za + zb >= 2 zmin) and the polygon goes out as is; otherwise to `skip`.
# (A corner the GTE saturated, |x| or |y| >= 1023, needs no test of its
# own: W + H <= 500 puts the whole polygon off screen then.) Expects t3..t5
# (t6: quad) = the corners' cache addresses; clobbers t0-t2, t7, t9, v0, v1
# (reloaded with clut / tpage), and t6 for a triangle (its y0).
.macro CAREFUL quad, far, var, near
	lw      $t0, SP_SZ($t3)
	lw      $t1, SP_SZ($t4)
	lw      $t2, SP_SZ($t5)
.if \var
	lw      $v1, SP_SZCLIP($t8)
.else
	lw      $v1, SP_T4($t8)
.endif
.if \quad
	lw      $t9, SP_SZ($t6)
.if \var
	MINMAX4D $t0, $t1, $t2, $t9, $t7, $v0, "slt $t7, $t0, $v1"   # zmin, zmax
.else
	MINMAX4D $t0, $t1, $t2, $t9, $t7, $v0, "sltiu $t7, $t0, 96"
.endif
.else
.if \var
	MINMAX3 $t0, $t1, $t2, $t7, "slt $t7, $t0, $v1"
.else
	MINMAX3 $t0, $t1, $t2, $t7, "sltiu $t7, $t0, 96"
.endif
.endif
.if \var
	bnez    $t7, \near                   # a corner nearer than the bound
	lw      $v1, SP_T4($t8)              # (\near: t1 = zmax)
	subu    $t7, $t1, $t0                # depth range
.else
	bnez    $t7, \near
	subu    $t7, $t1, $t0                # depth range
.endif
	mult    $v1, $t0                     # 2 T2 zmin (LO holds it through the
	lh      $t0, SP_SXY($t3)             # x          ranges: v1 takes W)
	lh      $t1, SP_SXY($t4)
	lh      $t2, SP_SXY($t5)
.if \quad
	lh      $t9, SP_SXY($t6)
	MINMAX4D $t0, $t1, $t2, $t9, $v0, $v1, "lh $t2, SP_SXY+2($t5)"
.else
	MINMAX3 $t0, $t1, $t2, $v0, "lh $t6, SP_SXY+2($t3)"   # (y0: t6, free in a
.endif                                   #  triangle, in the last delay slot)
	subu    $v1, $t1, $t0                # W
.if \quad
	lh      $t0, SP_SXY+2($t3)           # y
	lh      $t1, SP_SXY+2($t4)
	lh      $t9, SP_SXY+2($t6)
	MINMAX4 $t0, $t1, $t2, $t9, $v0, "mflo $v0"   # (2 T2 zmin, in the last
	subu    $t0, $t1, $t0                # H            delay slot)
.else
	lh      $t0, SP_SXY+2($t4)           # y1, y2 (the compares read y0, y1
	lh      $t1, SP_SXY+2($t5)           #  first)
	MINMAX3 $t6, $t0, $t1, $v0, "mflo $v0"   # (2 T2 zmin, in the last delay slot)
	subu    $t0, $t0, $t6                # H
.endif
	addu    $t9, $v1, $t0                # W + H
	sltiu   $t0, $t9, 501                # (r_slow.c SPAN)
	mult    $t9, $t7                     # (W + H) x depth range
	beqz    $t0, \far
	lw      $v1, SP_TPAGE16($t8)         # (clut / tpage back; the far and
	mflo    $t9                          #  slow paths reload them)
	sltu    $t9, $v0, $t9
	bnez    $t9, \far
	lw      $v0, SP_CLUT16($t8)
.endm

# POLYS q, var, flat, rec1: a record's polygons from its vertex cache (the
# record loop's second half, labels prefixed by q): each quad, then each
# triangle, emitted at once or through CAREFUL (a2 != 0, var as the loop's)
# to the slow path; back to rec1 for the next record. flat = 1: POLY_FT4 /
# POLY_FT3 in the record's one colour (SP_FT4COL / SP_FT3COL: the graphics
# options' flat shading), the slow path's triangles from the vertices'
# colour words, the same colour (XFORM light 2).
.macro POLYS q, var, flat, rec1
\q\()_polys:
	lhu     $t0, R_POLYS($a0)
\q\()_polys_1:
	lbu     $at, R_NQUADS($a0)
.if R_PROFILE
	lw      $t1, SP_POLYS($t8)
	addu    $a3, $a0, $t0                # first polygon
	lbu     $t0, R_NTRIS($a0)
	addu    $t1, $t1, $at
	addu    $t1, $t1, $t0
	beqz    $at, \q\()_tris
	sw      $t1, SP_POLYS($t8)
.else
	addu    $a3, $a0, $t0                # first polygon
	beqz    $at, \q\()_tris
.endif
\q\()_quad:                                 # (release: in the delay slot above)
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
	beqz    $a2, \q\()_quad_emit
	addu    $t6, $t6, $t8
	CAREFUL 1, \q\()_quad_far, \var, \q\()_quad_near
.if \flat
\q\()_quad_emit:                            # POLY_FT4: xy, uv per corner, the
	lw      $t0, SP_FT4COL($t8)          #  record's colour
	lw      $t1, 4($a3)                  # uv0 | uv1 << 16
	lw      $t3, SP_SXY($t3)             # xy0
	lw      $t4, SP_SXY($t4)             # xy1
	sw      $t0, 4($s6)
	sw      $t3, 8($s6)
	andi    $t0, $t1, 0xffff
	or      $t0, $t0, $v0                # uv0 | clut
	sw      $t0, 12($s6)
	sw      $t4, 16($s6)
	srl     $t1, $t1, 16
	or      $t1, $t1, $v1                # uv1 | tpage
	sw      $t1, 20($s6)
	lw      $t5, SP_SXY($t5)             # xy2
	lw      $t2, 8($a3)                  # uv2 | uv3 << 16
	lw      $t6, SP_SXY($t6)             # xy3
	sw      $t5, 24($s6)
	andi    $t0, $t2, 0xffff
	sw      $t0, 28($s6)
	srl     $t2, $t2, 16
	sw      $t6, 32($s6)
	sw      $t2, 36($s6)
	lui     $t9, 0x0900
	or      $t9, $t9, $fp                # tag: 9 words, link to previous head
	sw      $t9, 0($s6)
	move    $fp, $s6                     # new bucket head
	addiu   $s6, $s6, 40
.else
\q\()_quad_emit:                            # POLY_GT4: colour, xy, uv per corner
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
.endif
\q\()_quad_next:
	bgtz    $at, \q\()_quad
	addiu   $a3, $a3, POLY_SIZE

\q\()_tris:
	lbu     $at, R_NTRIS($a0)
	lw      $t0, 0($a3)                  # (none: a harmless read)
	beqz    $at, \q\()_next
\q\()_tri:                                  # t0 = the triangle's 3 vertex offsets
	addiu   $at, $at, -1                 # (count down; the delay slot above)
	andi    $t3, $t0, 0xff
	srl     $t4, $t0, 8
	andi    $t4, $t4, 0xff
	srl     $t5, $t0, 16
	andi    $t5, $t5, 0xff
	addu    $t3, $t3, $t8
	addu    $t4, $t4, $t8
	beqz    $a2, \q\()_tri_emit
	addu    $t5, $t5, $t8
	CAREFUL 0, \q\()_tri_far, \var, \q\()_tri_near
.if \var
# A model's triangle turned away from the eye (or edge-on) is left out, as
# Quake's alias drawer does (d_polyse.c D_DrawNonSubdiv): the GTE's NCLIP on
# its corners, the emit's loads and sums in its eight cycles; the cook
# winds every model's triangles as the world's faces (tools/qcook/game.c:
# front, MAC0 < 0; a brush model's faces, walked here, are front ones).
# Until 2026-10-21 a model drew its back too, under or over its front by
# the records' order alone (the super nailgun's turning barrels).
# The corners' xy go to the GTE by lwc2 and from it to the packet by swc2
# (§66: no copy in the CPU's registers). The flat branch, which no
# instance assembles (a model's loop has no flat shading), keeps the copy.
.if \flat
\q\()_tri_emit:                             # POLY_FT3
	lw      $t6, SP_SXY($t3)             # xy0
	lw      $t7, SP_SXY($t4)             # xy1
	lw      $t9, SP_SXY($t5)             # xy2
	mtc2    $t6, C2_SXY0
	mtc2    $t7, C2_SXY1
	mtc2    $t9, C2_SXY2
	lw      $t0, SP_FT3COL($t8)          # (two instructions before the NCLIP)
	lw      $t1, 4($a3)                  # uv0 | uv1 << 16
	NCLIP
	lw      $t2, 8($a3)                  # uv2 | pad (0, see qw_tri)
	andi    $t3, $t1, 0xffff
	or      $t3, $t3, $v0                # uv0 | clut
	srl     $t1, $t1, 16
	or      $t1, $t1, $v1                # uv1 | tpage
	lui     $t5, 0x0700
	or      $t5, $t5, $fp                # tag: 7 words
	mfc2    $t4, C2_MAC0
	nop
	bgez    $t4, \q\()_tri_next          # turned away: left out
	sw      $t0, 4($s6)                  # (a packet not linked: harmless)
	sw      $t6, 8($s6)
	sw      $t3, 12($s6)
	sw      $t7, 16($s6)
	sw      $t1, 20($s6)
	sw      $t9, 24($s6)
	sw      $t2, 28($s6)
	sw      $t5, 0($s6)
	move    $fp, $s6
	addiu   $s6, $s6, 32
.else
\q\()_tri_emit:                             # POLY_GT3
	lwc2    C2_SXY0, SP_SXY($t3)         # the corners' xy: into the GTE for the
	lwc2    C2_SXY1, SP_SXY($t4)         # NCLIP, from there to the packet (no
	lwc2    C2_SXY2, SP_SXY($t5)         # copy in the CPU's registers: §66)
	lw      $t0, SP_COL($t3)             # c0 (two instructions before the NCLIP)
	lw      $t1, 4($a3)                  # uv0 | uv1 << 16
	NCLIP
	lw      $t2, SP_COL($t4)             # c1
	lw      $t5, SP_COL($t5)             # c2
	lui     $t3, CODE_GT3 >> 16
	or      $t0, $t0, $t3
	andi    $t3, $t1, 0xffff
	or      $t3, $t3, $v0                # uv0 | clut
	srl     $t1, $t1, 16
	mfc2    $t4, C2_MAC0
	or      $t1, $t1, $v1                # uv1 | tpage
	bgez    $t4, \q\()_tri_next          # turned away: left out
	sw      $t0, 4($s6)                  # (a packet not linked: harmless)
	lw      $t4, 8($a3)                  # uv2 | pad (0, see qw_tri)
	swc2    C2_SXY0, 8($s6)              # xy0 (the NCLIP done: no GTE command
	sw      $t3, 12($s6)                 #  runs while they are read)
	sw      $t2, 16($s6)
	swc2    C2_SXY1, 20($s6)             # xy1
	sw      $t1, 24($s6)
	sw      $t5, 28($s6)
	swc2    C2_SXY2, 32($s6)             # xy2
	sw      $t4, 36($s6)
	lui     $t9, 0x0900
	or      $t9, $t9, $fp                # tag: 9 words
	sw      $t9, 0($s6)
	move    $fp, $s6
	addiu   $s6, $s6, 40
.endif
.else
.if \flat
\q\()_tri_emit:                             # POLY_FT3
	lw      $t0, SP_FT3COL($t8)
	lw      $t1, 4($a3)                  # uv0 | uv1 << 16
	lw      $t3, SP_SXY($t3)             # xy0
	lw      $t4, SP_SXY($t4)             # xy1
	sw      $t0, 4($s6)
	sw      $t3, 8($s6)
	andi    $t0, $t1, 0xffff
	or      $t0, $t0, $v0                # uv0 | clut
	sw      $t0, 12($s6)
	sw      $t4, 16($s6)
	srl     $t1, $t1, 16
	or      $t1, $t1, $v1                # uv1 | tpage
	sw      $t1, 20($s6)
	lw      $t1, SP_SXY($t5)             # xy2
	lw      $t2, 8($a3)                  # uv2 | pad (0, see qw_tri)
	sw      $t1, 24($s6)
	sw      $t2, 28($s6)
	lui     $t9, 0x0700
	or      $t9, $t9, $fp                # tag: 7 words
	sw      $t9, 0($s6)
	move    $fp, $s6
	addiu   $s6, $s6, 32
.else
\q\()_tri_emit:                             # POLY_GT3
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
.endif
.endif
\q\()_tri_next:
	lw      $t0, POLY_SIZE($a3)          # the next one's (after the last: harmless)
\q\()_tri_next_1:
	bgtz    $at, \q\()_tri
	addiu   $a3, $a3, POLY_SIZE
\q\()_next:
	lw      $a0, SP_NEXTREC($t8)
	b       \rec1
	lw      $t0, SP_FVBASE($t8)          # (\q\()_rec's)

# A corner nearer than CAREFUL's bound (t1 = the farthest corner's SZ):
# every corner behind the near plane (SZ < SP_ZNEAR, the GTE's SZ being
# 0 behind the eye) leaves the slow path nothing to draw (its clip finds
# them all behind): on to the next polygon. Else the slow path.
	ICSEC   \q\()_quad_near
\q\()_quad_near:
	lw      $t0, SP_ZNEAR($t8)
	lw      $v0, SP_CLUT16($t8)          # (CAREFUL used v0, v1)
	slt     $t0, $t1, $t0
	beqz    $t0, \q\()_quad_skip
	lw      $v1, SP_TPAGE16($t8)
	b       \q\()_quad_next
	nop
	ICSEC   \q\()_tri_near
\q\()_tri_near:
	lw      $t0, SP_ZNEAR($t8)
	lw      $v0, SP_CLUT16($t8)
	slt     $t0, $t1, $t0
	beqz    $t0, \q\()_tri_skip
	lw      $v1, SP_TPAGE16($t8)
	b       \q\()_tri_next
	nop

# CAREFUL's other exits (too big, or the affine bound): no corner near, so
# every corner's SXY is its projection (saturated toward the screen at
# worst). A polygon wholly off one side of the screen by the slow path's
# own test (all x < 0, all y < 0, all x >= 320 or all y >= 240: its
# triangles would all be culled, and the clip path's exact projections lie
# on the same side) is left out; the others go to the slow path. The sign
# bits of the SXY words carry y < 0 (31) and x < 0 (15); w - (240 << 16)
# has the sign of y - 240, (w << 16) - (320 << 16) that of x - 320.
	ICSEC   \q\()_quad_far
\q\()_quad_far:
	lw      $t0, SP_SXY($t3)
	lw      $t1, SP_SXY($t4)
	lw      $t2, SP_SXY($t5)
	lw      $t7, SP_SXY($t6)
	and     $t9, $t0, $t1
	and     $t9, $t9, $t2
	and     $t9, $t9, $t7
	bltz    $t9, \q\()_quad_off         # all y < 0
	andi    $t9, $t9, 0x8000
	bnez    $t9, \q\()_quad_off         # all x < 0
\q\()_fq_r:
	lui     $v0, 240
	subu    $t9, $t0, $v0
	subu    $v1, $t1, $v0
	or      $t9, $t9, $v1
	subu    $v1, $t2, $v0
	or      $t9, $t9, $v1
	subu    $v1, $t7, $v0
	or      $t9, $t9, $v1
	bgez    $t9, \q\()_quad_off         # all y >= 240
\q\()_fq_c:
	lui     $v0, 320
	FARSITE \q\()_fq_r, \q\()_fq_c
	sll     $t0, $t0, 16
	sll     $t1, $t1, 16
	sll     $t2, $t2, 16
	sll     $t7, $t7, 16
	subu    $t0, $t0, $v0
	subu    $t1, $t1, $v0
	or      $t0, $t0, $t1
	subu    $t2, $t2, $v0
	or      $t0, $t0, $t2
	subu    $t7, $t7, $v0
	or      $t0, $t0, $t7
	bltz    $t0, \q\()_quad_skip        # not all x >= 320: the slow path
	nop
\q\()_quad_off:
	lw      $v0, SP_CLUT16($t8)
	b       \q\()_quad_next
	lw      $v1, SP_TPAGE16($t8)
	ICSEC   \q\()_tri_far
\q\()_tri_far:
.if \var
	lwc2    C2_SXY0, SP_SXY($t3)         # a model's triangle turned away is left
	lwc2    C2_SXY1, SP_SXY($t4)         # out here too: the slow path draws a
	lwc2    C2_SXY2, SP_SXY($t5)         # back face, and a box's (no cooked order)
	lw      $t0, SP_SXY($t3)             # came out over its front ones; the NCLIP
	lw      $t1, SP_SXY($t4)             # as the emit's, the bounds' tests in its
	NCLIP                                # cycles (the near exit keeps them: its
	lw      $t2, SP_SXY($t5)             # corners' SXY are not their projection)
.else
	lw      $t0, SP_SXY($t3)
	lw      $t1, SP_SXY($t4)
	lw      $t2, SP_SXY($t5)
.endif
\q\()_ft_r:
	lui     $v0, 240
	and     $t9, $t0, $t1
	and     $t9, $t9, $t2
	bltz    $t9, \q\()_tri_off          # all y < 0
	andi    $t9, $t9, 0x8000
	bnez    $t9, \q\()_tri_off          # all x < 0
	subu    $t9, $t0, $v0
	subu    $v1, $t1, $v0
	or      $t9, $t9, $v1
	subu    $v1, $t2, $v0
	or      $t9, $t9, $v1
	bgez    $t9, \q\()_tri_off          # all y >= 240
\q\()_ft_c:
	lui     $v0, 320
	FARSITE \q\()_ft_r, \q\()_ft_c
.if \var
	mfc2    $t9, C2_MAC0
.endif
	sll     $t0, $t0, 16
	sll     $t1, $t1, 16
	sll     $t2, $t2, 16
	subu    $t0, $t0, $v0
	subu    $t1, $t1, $v0
	or      $t0, $t0, $t1
	subu    $t2, $t2, $v0
.if \var
	bgez    $t9, \q\()_tri_off          # turned away (NCLIP >= 0): left out
.endif
	or      $t0, $t0, $t2
	bltz    $t0, \q\()_tri_skip         # not all x >= 320: the slow path
	nop
\q\()_tri_off:
	lw      $v0, SP_CLUT16($t8)
	b       \q\()_tri_next
	lw      $v1, SP_TPAGE16($t8)

# Near polygon: hand it to the slow path (engine/r_slow.c) — near clip,
# subdivision, exact projection. t0 = 4 (quad) or 3 (triangle).
	ICSEC   \q\()_quad_skip
\q\()_quad_skip:
	b       \q\()_slow_call
	li      $t0, 4
	ICSEC   \q\()_tri_skip
\q\()_tri_skip:
	li      $t0, 3
\q\()_slow_call:
	sw      $at, SP_SAVE+0($t8)
	sw      $a0, SP_SAVE+4($t8)
	sw      $a1, SP_SAVE+8($t8)
	sw      $a2, SP_SAVE+12($t8)
	sw      $a3, SP_SAVE+16($t8)
	sw      $t0, SP_SAVE+32($t8)
	sw      $s6, SP_SAVE+20($t8)         # (the slow path's bytes, below)
.if R_PROFILE
	lw      $t1, SP_SKIPPED($t8)         # polygons sent to the slow path
	lhu     $t2, TIMER2($t8)
	addiu   $t1, $t1, 1
	sw      $t1, SP_SKIPPED($t8)
	sw      $t2, SP_SAVE+36($t8)         # slow-path start time
.endif
.if R_SLOW_C
	lw      $t9, SP_RSTATE($t8)          # (s0 holds the plane table)
	move    $a1, $a0                     # record
	sw      $s6, RS_PKT($t9)             # the C side emits through r_state
	sw      $fp, RS_OT_HEAD($t9)
	move    $a2, $a3                     # polygon
	move    $a3, $t0                     # 4 or 3
	move    $a0, $t9                     # r_state
	addiu   $sp, $sp, -32                # o32 argument area, gp (the walk's
	sw      $gp, 16($sp)                 # block light table) and the real one
	lui     $gp, %hi(r_gp_saved)
	jal     r_slow
	lw      $gp, %lo(r_gp_saved)($gp)
	lw      $gp, 16($sp)
	addiu   $sp, $sp, 32
	lui     $t8, 0x1f80
	lw      $t9, SP_RSTATE($t8)
	nop
	lw      $s6, RS_PKT($t9)
	lw      $fp, RS_OT_HEAD($t9)
.else
	move    $a1, $a3                     # polygon
	.globl  \q\()_ts
\q\()_ts:                                 # (render.c r_tess_select: tess_poly or slow_poly)
	jal     slow_poly                    # a0 = record, a1 = polygon, a2 = 4 / 3: the slow path,
	move    $a2, $t0                     #  or the regular subdivision (engine/r_tess.s) first
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
	lw      $t1, SP_SAVE+20($t8)         # bytes it wrote, for the warp
	lw      $t2, SP_SLOWB($t8)           # control's triangle count
	lw      $at, SP_SAVE+0($t8)
	subu    $t1, $s6, $t1
	addu    $t2, $t2, $t1
	sw      $t2, SP_SLOWB($t8)
	lw      $a0, SP_SAVE+4($t8)
	lw      $a1, SP_SAVE+8($t8)
	lw      $a2, SP_SAVE+12($t8)
	lw      $a3, SP_SAVE+16($t8)
	lw      $t0, SP_SAVE+32($t8)
	lw      $v0, SP_CLUT16($t8)          # (the careful test used them)
	li      $t1, 4
	beq     $t0, $t1, \q\()_quad_next
	lw      $v1, SP_TPAGE16($t8)
	b       \q\()_tri_next_1
	lw      $t0, POLY_SIZE($a3)          # (\q\()_tri_next's first)

.endm

# RECLOOP p, exit, var [, opt]: the record loop, labels prefixed by p,
# leaving for `exit` when the block is done or the arena is full.
# Instantiated for the world's face blocks (dr, back to the walk; var = 0:
# the careful test's near bound is 96) and for a model's records (de,
# draw_model; var = 1: the bound is SP_SZCLIP, the view model's 124). The
# comment above describes it. opt: the graphics options' copies, entered
# instead while an option is on (render.c r_set_options patches the first
# word of dr_records / de_records into a jump to dro<opt>_records /
# deo1_records). opt bit 0, the draw distance: a record wholly beyond it
# is culled, one reaching into the fog goes through the fogged transform
# (XFORM fog). opt bit 1, the world's flat shading: a record wholly beyond
# its start (or any), its block without a dynamic light, is drawn in one
# colour, its average light (times the fog's factor at its centre): its
# vertices without their lights (their colour words that colour, for the
# slow path), its polygons POLY_FT4 / POLY_FT3 (POLYS flat). The
# thresholds are slti immediates (\p\()_o_dcull, \p\()_o_fogz,
# \p\()_o_flatz), patched (the flat shading everywhere: li 0).
.macro RECLOOP p, exit, var, opt=0, wx=0
.if \opt
	ICSEC   \p\()_records             # (dr, de: the walk, draw_model fall in)
.endif
	.globl  \p\()_records
\p\()_records:
.if R_PROFILE
	sw      $t9, SP_T0($t8)
	lw      $v0, SP_CLUT16($t8)
.endif
.if \wx
	.globl  \p\()_xa                   # (the world's loops: their three ways out to
\p\()_xa:                                 #  the walk, pointed at the cooked walk's
.endif                                   #  seq_next while one runs: render.c r_seq_set)
	bnez    $t0, \exit                    # packet arena full: nothing more
	move    $t2, $zero
\p\()_cull:
	addu    $a0, $a0, $t2                # culled record (t2 = its size): next
\p\()_rec:
	lw      $t0, SP_FVBASE($t8)          # (kept through the unstamped run)
\p\()_rec_1:
.if R_PROFILE
	beq     $a0, $a1, \p\()_done
.else
.if \wx
	.globl  \p\()_xb
\p\()_xb:
.endif
	beq     $a0, $a1, \exit
.endif
	lhu     $t1, R_FACE_ID($a0)          # (at the block end: a harmless read)
	lhu     $t2, R_SIZE($a0)
	addu    $t1, $t1, $t0                # (face_id: a byte offset, world.c)
	lhu     $t1, 0($t1)
	addu    $a0, $a0, $t2                # a0 = next record
	bne     $t1, $s4, \p\()_rec_1           # not stamped this frame: next
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
	lhu     $t3, R_SR($a0)
	mtc2    $t9, C2_VZ0
	lhu     $a3, R_TEX($a0)
	lw      $t9, SP_K16($t8)
	MVMVA   1, 0, 0, 0, 0                # IR = rot * centre + TR
	srl     $t7, $t3, 1                  # (while the GTE works; t2 keeps the
	addu    $t7, $t7, $t3                #  record's size for \p\()_cull) rho = 1.5 r
	mult    $t9, $t7                     # k16 rho, for a2 bit 2 below
	lw      $at, SP_LASTTEX($t8)
.if !\var
	mfc2    $t4, C2_IR3                  # z
	beqz    $s2, \p\()_inside           # the node's box inside every plane: its
	mfc2    $t0, C2_IR1                  # faces too, no plane can cull (s2: the
	mfc2    $t1, C2_IR2                  # world block's node's clip flags)
.else
	mfc2    $t0, C2_IR1                  # x
	mfc2    $t1, C2_IR2                  # y
	mfc2    $t4, C2_IR3                  # z
.endif
	sll     $t5, $t3, 1
	srl     $t6, $t3, 3
	addu    $t5, $t5, $t6                # 2.125 r
	subu    $t6, $t4, $t0                # right: z - x
	addu    $t6, $t6, $t5
	bltz    $t6, \p\()_cull
	addu    $t6, $t4, $t0                # left: z + x
	addu    $t6, $t6, $t5
	bltz    $t6, \p\()_cull
\p\()_ys_r:
	sll     $t5, $t3, 3                  # 8 r
\p\()_ys_z:
	sll     $t6, $t4, 1
\p\()_ys_z2:
	addu    $t6, $t6, $t4                # 3z
\p\()_ys_y:
	sll     $t0, $t1, 2                  # 4y
.if R_YSLOPE == 5
	addu    $t5, $t5, $t3                # 9 r: |n| = sqrt(34) x 1.5 = 8.75
	addu    $t0, $t0, $t1                # 5y (a 192-row viewport: R_YSLOPE)
.endif
	YSITE2  \p\()_ys_r, 3, \p\()_ys_z, 1
	YSITE2  \p\()_ys_z2, 2, \p\()_ys_y, 0
	subu    $t9, $t6, $t0                # bottom: 3z - 4y
	addu    $t9, $t9, $t5
	bltz    $t9, \p\()_cull
	addu    $t9, $t6, $t0                # top: 3z + 4y
	addu    $t9, $t9, $t5
	bltz    $t9, \p\()_cull
	sll     $t5, $t3, 1                  # delay: 2r
\p\()_join:
	subu    $t6, $t4, $t7                # nearest depth z - rho
.if \opt & 1
	addu    $t3, $t4, $t7                # farthest depth z + rho (the fog's
	.globl  \p\()_o_dcull               #  test, below; t4 = z kept there)
\p\()_o_dcull:
	slti    $t0, $t6, 0                  # nearer than the draw distance?
	beqz    $t0, \p\()_cull              # (wholly beyond it: black, culled)
.endif
.if \opt & 2
	move    $t1, $t6                     # (the flat shading's test, below)
.endif
	lw      $t7, SP_SZFAST($t8)
	subu    $t5, $t4, $t5                # nearest possible SZ (>= 1.5 r away)
	slt     $a2, $t5, $t7                # a2 bit 0: may come too near
	sltu    $t0, $s7, $s6                # room for the whole record?
	bnez    $t0, \p\()_full
	sll     $t6, $t6, 4
	# a2 bit 2: an affine error above the threshold is possible: (z - rho)
	# x 16 < k16 x rho (render.c, warp_k16). It also covers an edge over
	# the subdivision's 500 px span: k16 >= 67 (T <= 32) puts the bound at
	# z < 7.8 r, beyond the 6 r where a record's edges can reach 500 px.
	mflo    $t9
	slt     $t9, $t6, $t9
	sll     $t9, $t9, 2
	or      $a2, $a2, $t9
.if !\var
	bnez    $gp, dr_light_rec            # dynamic lights reach the block: the
	.globl  \p\()_lit_back
\p\()_lit_back:                           # record, in view, is lit first
.endif
	lbu     $t2, R_NVERTS($a0)
	beq     $at, $a3, \p\()_tex_done        # same texture as the last record
	lhu     $t0, R_POLYS($a0)            # (the light bytes' offset, below)
	bltz    $at, \p\()_tex_load             # first of the frame: no run to close
	lw      $t5, SP_TEXBASE($t8)
	lw      $t6, SP_TWIN($t8)            # close the last texture's run: its
	lui     $t9, 0x0100                  # window command, prepended after its
	or      $t9, $t9, $fp                # polygons, comes first in the bucket
	sw      $t9, 0($s6)                  # (one per run, not per record: the
	sw      $t6, 4($s6)                  # run's polygons all draw after it)
	move    $fp, $s6
	addiu   $s6, $s6, 8
\p\()_tex_load:
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
\p\()_tex_done:
.if R_PROFILE
.if \opt & 2
	lw      $t9, SP_RECORDS($t8)         # (t1 kept)
	nop
	addiu   $t9, $t9, 1
	sw      $t9, SP_RECORDS($t8)
	lw      $t9, SP_VERTS($t8)
	nop
	addu    $t9, $t9, $t2
	sw      $t9, SP_VERTS($t8)
.else
	lw      $t1, SP_RECORDS($t8)
	lw      $t9, SP_VERTS($t8)
	addiu   $t1, $t1, 1
	addu    $t9, $t9, $t2
	sw      $t1, SP_RECORDS($t8)
	sw      $t9, SP_VERTS($t8)
.endif
.endif
.if \opt & 2
	.globl  \p\()_o_flatz
\p\()_o_flatz:
	slti    $t5, $t1, 0                  # nearest depth (the join's) beyond the
	beqz    $t5, \p\()_o_flat            #  flat shading's start: flat (a dynamic
\p\()_o_gouraud:                          #  light on the block: back here)
	lw      $t9, SP_VERTPOOL($t8)        # (padded to threes: no stray reads)
.endif
	addiu   $t1, $t2, 3                  # light bytes: polys - ((nv + 3) & ~3)
	andi    $t1, $t1, 0xfc
	addu    $t0, $t0, $a0
	subu    $at, $t0, $t1
.if \opt & 1
	.globl  \p\()_o_fogz
\p\()_o_fogz:
	slti    $t0, $t3, 0                  # farthest depth (the join's) before the fog?
.endif
	addiu   $t3, $a0, R_VERTS            # the record's indices into the pool
.if !(\opt & 2)
	lw      $t9, SP_VERTPOOL($t8)        # (padded to threes: no stray reads)
.endif
.if \opt & 1
	beqz    $t0, \p\()_o_fog             # reaching into the fog: the fogged
	addiu   $t5, $t2, -2                 #  transforms
	beqz    $a2, \p\()_xfn
	move    $t4, $t8                     # cache cursor
.else
	move    $t4, $t8                     # cache cursor
	beqz    $a2, \p\()_xfn                  # nothing near or warping: no SZ (only
	addiu   $t5, $t2, -2                 # CAREFUL and \p\()_nz read it). Vertices
.endif
	XFORM   1, \p\()_xf, \p\()_one, \p\()_xf_done #  left - 2: RTPT while 3+
\p\()_xf_done:                              # (a2 != 0 here)
	move    $t4, $t8
	andi    $t0, $a2, 4                  # big or warping: careful regardless
	bnez    $t0, \p\()_polys
	move    $t5, $t2
	move    $a2, $zero                   # exact check: any real vertex near?
\p\()_nz:
	lw      $t0, SP_SZ($t4)
	addiu   $t5, $t5, -1
	sltu    $t0, $t0, $t7
	or      $a2, $a2, $t0
	bgtz    $t5, \p\()_nz
	addiu   $t4, $t4, 4

	POLYS   \p, \var, 0, \p\()_rec_1

	# the transform of a record CAREFUL will not see: no SZ
	ICSEC   \p\()_xfn
	XFORM   0, \p\()_xfn, \p\()_onen, \p\()_polys
	b       \p\()_polys_1
	lhu     $t0, R_POLYS($a0)            # (\p\()_polys's first)

.if \opt & 1
.if !\var
	# the dynamic lights' way back here (render.c points dr_light_rec's
	# returns at it): t3, the far depth, is lost: the fog's test passed
	ICSEC   \p\()_lit_ret
	.globl  \p\()_lit_ret
\p\()_lit_ret:
	b       \p\()_lit_back
	li      $t3, 0x7fff
.endif
	# a record reaching into the fog (a2: its near and warp bits; t4 =
	# z, its centre's depth): the world's with its centre in the fog drawn
	# flat, darkened as a whole (\p\()_o_fflat: cheaper than Gouraud);
	# the others (a big one reaching from before it, a model) each light
	# times its vertex's factor (XFORM fog, t2 = r_fogtab)
	ICSEC   \p\()_o_fog
\p\()_o_fog:
.if !\var
	.globl  \p\()_o_fogc
\p\()_o_fogc:
	slti    $t0, $t4, 0                  # the centre before the fog?
	beqz    $t0, \p\()_o_fflat
	nop
\p\()_o_fogv:
.endif
	move    $t4, $t8                     # cache cursor
	lui     $t2, %hi(r_fogtab)
	bnez    $a2, \p\()_o_fogsz
	addiu   $t2, $t2, %lo(r_fogtab)
	XFORM   0, \p\()_xfgn, \p\()_onegn, \p\()_polys, 1, 1
	b       \p\()_polys_1
	lhu     $t0, R_POLYS($a0)            # (\p\()_polys's first)
	ICSEC   \p\()_o_fogsz
\p\()_o_fogsz:
	XFORM   1, \p\()_xfg, \p\()_oneg, \p\()_xfg_done, 1, 1
\p\()_xfg_done:
	lbu     $t2, R_NVERTS($a0)           # (the vertex count and SZ_FAST back:
	b       \p\()_xf_done                #  the table and the fog's temporary
	lw      $t7, SP_SZFAST($t8)          #  had their registers)
.endif
.if \opt
.if !\var
	# flat: unless a dynamic light reaches the block (its records lit
	# vertex by vertex: back to the Gouraud or the fog's way), one colour,
	# the record's average light (its flags' bits 2 - 7, x 4: world.c)
	# times the fog's factor at its centre (t4, the join's z): in
	# SP_FT4COL / SP_FT3COL with the codes for POLYS, in every vertex's
	# colour word for the slow path (XFORM light 2, at = the colour)
.if \opt & 2
	ICSEC   \p\()_o_flat
\p\()_o_flat:                              # (the flat shading's)
	bnez    $gp, \p\()_o_gouraud
	lbu     $t1, R_FLAGS($a0)            # (t0, the polygons' offset, kept for there)
.if \opt & 1
	b       \p\()_o_flat_2
	sra     $t3, $t4, 31
.endif
.endif
.if \opt & 1
	ICSEC   \p\()_o_fflat
\p\()_o_fflat:                             # (the fog's: the centre in it)
	bnez    $gp, \p\()_o_fogv
	lbu     $t1, R_FLAGS($a0)
	sra     $t3, $t4, 31                 # the centre's depth (the join's t4;
\p\()_o_flat_2:                            #  behind the eye: 0)
	nor     $t3, $t3, $zero
	and     $t3, $t3, $t4
	srl     $t3, $t3, 6
	lui     $t9, %hi(r_fogtab)
	addu    $t3, $t3, $t9
	lbu     $t3, %lo(r_fogtab)($t3)
	andi    $t1, $t1, 0xfc               # the average light
	mult    $t1, $t3
	mflo    $t1
	srl     $t1, $t1, 7
.else
	addiu   $t3, $a0, R_VERTS            # the record's indices into the pool
	andi    $t1, $t1, 0xfc               # the average light
.endif
	sll     $at, $t1, 8                  # grey c -> 0x00cccccc
	or      $at, $at, $t1
	sll     $t1, $at, 8
	or      $at, $at, $t1
	lui     $t1, CODE_FT4 >> 16
	or      $t1, $t1, $at
	lui     $t9, CODE_FT3 >> 16
	or      $t9, $t9, $at
	sw      $t1, SP_FT4COL($t8)
	sw      $t9, SP_FT3COL($t8)
.if \opt & 1
	addiu   $t3, $a0, R_VERTS            # the record's indices into the pool
.endif
	lw      $t9, SP_VERTPOOL($t8)
	move    $t4, $t8                     # cache cursor
	bnez    $a2, \p\()_fsz
	addiu   $t5, $t2, -2
	XFORM   0, \p\()_fxfn, \p\()_fonen, \p\()f_polys, 0      # SXY only
	b       \p\()f_polys_1
	lhu     $t0, R_POLYS($a0)            # (\p\()f_polys's first)
	ICSEC   \p\()_fsz
\p\()_fsz:
	XFORM   1, \p\()_fxf, \p\()_fone, \p\()_fxf_done, 2      # SXY, SZ, the colour
\p\()_fxf_done:
	move    $t4, $t8
	andi    $t0, $a2, 4                  # big or warping: careful regardless
	bnez    $t0, \p\()f_polys
	move    $t5, $t2
	move    $a2, $zero                   # exact check: any real vertex near?
\p\()_fnz:
	lw      $t0, SP_SZ($t4)
	addiu   $t5, $t5, -1
	sltu    $t0, $t0, $t7
	or      $a2, $a2, $t0
	bgtz    $t5, \p\()_fnz
	addiu   $t4, $t4, 4
	POLYS   \p\()f, \var, 1, \p\()_rec_1
.endif
.endif

.if !\var
	ICSEC   \p\()_inside
\p\()_inside:                              # (no plane test: on with 2r)
	b       \p\()_join
	sll     $t5, $t3, 1
.endif

	ICSEC   \p\()_full
\p\()_full:
	li      $t0, 1
.if R_PROFILE
	sw      $t0, SP_FULL($t8)
	lw      $t0, SP_SKIPPED($t8)
	nop
	addiu   $t0, $t0, 1
	sw      $t0, SP_SKIPPED($t8)
\p\()_done:
	lhu     $t0, TIMER2($t8)
	lw      $t9, SP_T0($t8)
	lw      $t1, SP_TBLOCK($t8)
	subu    $t0, $t0, $t9
	andi    $t0, $t0, 0xffff
	addu    $t1, $t1, $t0
.if \wx
	.globl  \p\()_xb, \p\()_xc        # (one way out serves both)
\p\()_xb:
\p\()_xc:
.endif
	b       \exit
	sw      $t1, SP_TBLOCK($t8)
.else
.if \wx
	.globl  \p\()_xc
\p\()_xc:
.endif
	b       \exit
	sw      $t0, SP_FULL($t8)
.endif

.endm

	.section .bss.r_gp_saved, "aw", @nobits
	.align 2
r_gp_saved:
	.space 4
	.section .bss.bm_area, "aw", @nobits
	.align  2
bm_area:                                 # draw_bmodel's stack and kept words
	.space  BM_SIZE

	.section .bss.r_walk_spill, "aw", @nobits
	.align  4
r_walk_spill:                            # walk stack overflow (see walk_spill)
	.space  WALK_RAM

	ICSEC   r_render                     # (the I-cache's chains: r_defs.inc ICSEC)
	.global r_render
	.type   r_render, @function
r_render:
	addiu   $sp, $sp, -48
	sw      $ra, 44($sp)
	lui     $t0, %hi(r_gp_saved)
	sw      $gp, %lo(r_gp_saved)($t0)    # (the walk keeps a block's light table in gp)
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
	# The colour matrix's first row is the camera (vertex units, within
	# int16: the cooked map is), so a general plane's n . cam is one MVMVA
	# (plane_dist, wn_general).
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
	sw      $t4, SP_SZCLIP($t8)          # (= 96 = R_SZ_FAST: r_slow.c SZ_CLIP)
	li      $t4, 48
	sw      $t4, SP_ZNEAR($t8)           # (8 world units: r_slow.c ZNEAR)
	.global r_o_hshift
r_o_hshift:
	li      $t4, 5
	sw      $t4, SP_HSHIFT($t8)          # (the GTE's H, 160 = 5 << 5: render.c R_SCREEN_H;
	                                     #  r_set_res patches the shift, the mul in sp_proj)
	andi    $t0, $t0, 0xffff
	sll     $t1, $t1, 16
	or      $t0, $t0, $t1
	ctc2    $t0, C2_LR1LR2
	ctc2    $t2, C2_LR3LG1               # (LG1: row 2, unused)
	sw      $zero, SP_FULL($t8)
	sw      $zero, SP_SLOWB($t8)
.if R_PROFILE
	sw      $zero, SP_NODES($t8)
	sw      $zero, SP_RECORDS($t8)
	sw      $zero, SP_VERTS($t8)
	sw      $zero, SP_POLYS($t8)
	sw      $zero, SP_SKIPPED($t8)
	sw      $zero, SP_TBLOCK($t8)
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
	lw      $t2, RS_WARP_T2($s0)
	lw      $t4, RS_WARP_K16($s0)
	sw      $t3, SP_VERTPOOL($t8)
	sw      $t2, SP_T2($t8)
	sll     $t2, $t2, 1
	sw      $t2, SP_T4($t8)
	sw      $t4, SP_K16($t8)
	or      $t1, $t1, $fp
	lui     $t2, 0xE200
	sw      $t1, 0($s6)                  # texture window off, first in the
	sw      $t2, 4($s6)                  # bucket = drawn after the world (the
	move    $fp, $s6                     # overlay and the next frame start
	addiu   $s6, $s6, 8                  #  unwrapped)

	# ---- the view model: prepended first, so drawn last, on top -----------
	lw      $a0, RS_VIEWMODEL($s0)
	lw      $t0, RS_OFY($s0)
	lw      $t1, RS_VH2($s0)
	lw      $t2, RS_NLIGHTS($s0)
	sw      $s0, SP_RSTATE($t8)          # (draw_model, walk_done take it back)
	sh      $s4, SP_STAMP($t8)           # (a model's records' face stamp)
	sw      $t0, SP_OFY($t8)
	sw      $t2, SP_NLIGHTS($t8)
	beqz    $a0, 1f
	sw      $t1, SP_VH2($t8)
	jal     draw_model
	nop
1:
	# ---- camera leaf ----------------------------------------------------
	lw      $s1, RS_ROOT($s0)
	nop
	andi    $t0, $s1, 1
fl_loop:                                 # (t0 = s1 & 1: a leaf?)
	bnez    $t0, fl_done
	move    $a0, $s1
	jal     plane_dist_1
	lhu     $t0, N_PLANE3($a0)           # (plane_dist's first)
	slti    $t0, $v0, 1                  # d <= 0 -> back child (Quake tie rule)
	sll     $t0, $t0, 2
	addu    $t0, $t0, $s1
	lw      $s1, N_CHILD($t0)
	b       fl_loop
	andi    $t0, $s1, 1
	.size r_render, . - r_render
	ICSEC   fl_done
fl_done:
	addiu   $s1, $s1, -1                 # leaf pointer

	lw      $t0, RS_VIEW_LEAF($s0)
	lw      $t1, RS_FORCE_MARK($s0)
	bne     $t0, $s1, do_mark
	move    $a0, $s1
	beqz    $t1, mark_done
	lw      $s5, RS_VISFRAME($s0)        # (the walk's; mark_pvs sets it too)
do_mark:
	.globl  mark_call                    # (render.c: mark_pvs_far while the
mark_call:                               #  draw distance is on)
	jal     mark_pvs
	nop
mark_done:
	# the dynamic lights: the nodes their spheres reach (and whose plane
	# they straddle), stamped with the frame count for the walk's block
	# gate (dl_stamp_nodes_asm; R_DLIGHT_C = 1: the C reference through a
	# stub)
	lw      $t0, RS_NLIGHTS($s0)
	nop
	beqz    $t0, 1f
.if R_DLIGHT_C == 1
	addiu   $sp, $sp, -32                # (o32 argument slots, ra, fp)
	sw      $ra, 28($sp)
	sw      $fp, 24($sp)
	lui     $gp, %hi(r_gp_saved)
	jal     dl_stamp_nodes
	lw      $gp, %lo(r_gp_saved)($gp)
	lw      $ra, 28($sp)
	lw      $fp, 24($sp)
	lui     $t8, 0x1f80
1:	addiu   $sp, $sp, 32
.else
	# the asm (the C reference above documents it): for each live light,
	# from the root, a node out of the PVS or whose box is out of the
	# sphere's reach ends its subtree; one whose plane the sphere
	# straddles (|d| <= radius: its faces lie on that plane) takes the
	# frame count. A plane farther than radius + DL_PRUNE leaves only the
	# light's side to descend (R_MarkLights). Depth first, the back child
	# first and the front child pushed: a stack on sp of at most the
	# tree's depth; s2 = radius + DL_PRUNE (the walk sets s1 - s3 after).
	# The counters (dlnodes) are the C reference's only.
	addiu   $sp, $sp, -DSN_STACK
	lw      $a1, RS_LIGHTS($s0)
	lw      $v1, SP_PLANES($t8)          # the plane table
	sll     $a2, $t0, 5
	addu    $a2, $a2, $a1                # the lights' end
dsn_light:
	lw      $t0, DL_O+0($a1)             # o
dsn_light_b:
	lw      $t1, DL_O+4($a1)
	lw      $t2, DL_O+8($a1)
	lw      $t6, DL_RADIUS($a1)
	lw      $a0, RS_ROOT($s0)            # the root
	move    $a3, $sp                     # the stack, empty
	addiu   $s2, $t6, DL_PRUNE           # the side-only bound
dsn_node:
	lhu     $t3, N_VISFRAME($a0)
dsn_node_vis:
	lh      $t4, N_C+0($a0)
	bne     $t3, $s5, dsn_pop            # out of the PVS: its subtree too
	lh      $t5, N_C+2($a0)
	lh      $t3, N_C+4($a0)
	subu    $t4, $t0, $t4
	subu    $t5, $t1, $t5
	subu    $t3, $t2, $t3
	sra     $at, $t4, 31
	xor     $t4, $t4, $at
	subu    $t4, $t4, $at                # |dx|
	sra     $at, $t5, 31
	xor     $t5, $t5, $at
	subu    $t5, $t5, $at                # |dy|
	sra     $at, $t3, 31
	xor     $t3, $t3, $at
	subu    $t3, $t3, $at                # |dz|
	lh      $t7, N_H+0($a0)
	lh      $t9, N_H+2($a0)
	addu    $t7, $t7, $t6                # h + radius
	slt     $at, $t7, $t4                # < |dx|: the box out of reach
	bnez    $at, dsn_pop
	addu    $t9, $t9, $t6
	lh      $t7, N_H+4($a0)
	slt     $at, $t9, $t5
	bnez    $at, dsn_pop
	addu    $t7, $t7, $t6
	slt     $at, $t7, $t3
	bnez    $at, dsn_pop
	lhu     $t7, N_PLANE3($a0)
	lw      $v0, N_CHILD+0($a0)          # the front child, for later
	sll     $t7, $t7, 2
	addu    $t7, $t7, $v1                # the plane
	lbu     $t9, PL_TYPE($t7)
	lw      $t3, PL_DIST($t7)
	sltiu   $at, $t9, 3
	beqz    $at, 2f
	sra     $t3, $t3, 1                  # dist: 1/8 units -> vertex units, x4096
	sll     $t9, $t9, 2                  # axial: d = o[type] x 4096 - dist
	addu    $t9, $t9, $a1
	lw      $t9, DL_O($t9)
	lw      $t5, N_CHILD+4($a0)          # the back child, for later
	sll     $t9, $t9, 12
	b       4f
	subu    $t9, $t9, $t3
2:	lh      $t4, PL_N+0($t7)             # d = n . o - dist
	lh      $t5, PL_N+2($t7)
	mult    $t4, $t0
	lh      $t9, PL_N+4($t7)
	nop
	mflo    $t4
	subu    $t4, $t4, $t3
	nop                                  # (two apart from the next mult: HI/LO)
	mult    $t5, $t1
	nop
	nop
	mflo    $t5
	addu    $t4, $t4, $t5
	nop
	mult    $t9, $t2
	lw      $t5, N_CHILD+4($a0)          # the back child, for later
	nop
	mflo    $t9
	addu    $t9, $t9, $t4
4:	sra     $t9, $t9, 12                 # d, vertex units
	slt     $at, $s2, $t9
	bnez    $at, dsn_front               # d > radius + DL_PRUNE: the front only
	addu    $at, $t9, $s2
	bltz    $at, dsn_back                # d < -(radius + DL_PRUNE): the back only
	sra     $at, $t9, 31
	xor     $t9, $t9, $at
	subu    $t9, $t9, $at                # |d|
	slt     $at, $t6, $t9                # radius < |d|: no face of the plane lit
	bnez    $at, 3f
	andi    $t3, $t5, 1                  # (the back child a leaf?)
	sh      $s4, N_DLSTAMP($a0)          # stamped
3:	andi    $at, $v0, 1
	bnez    $at, 5f                      # the front child a leaf: nothing to push
	sw      $v0, 0($a3)                  # (written but not kept then)
	addiu   $a3, $a3, 4                  # pushed
5:	beqz    $t3, dsn_node                # the back child a node: on to it
	move    $a0, $t5
dsn_pop:
	beq     $a3, $sp, dsn_next           # the stack empty: this light is done
	addiu   $a3, $a3, -4
	lw      $a0, 0($a3)                  # popped
	b       dsn_node_vis
	lhu     $t3, N_VISFRAME($a0)
	ICSEC   dsn_front
dsn_front:                               # one side of the plane only: its
	move    $t5, $v0                     # child (v0 front, t5 back), unless
dsn_back:                                # a leaf
	andi    $at, $t5, 1
	bnez    $at, dsn_pop
	move    $a0, $t5                     # (the pop reloads a0)
	b       dsn_node_vis
	lhu     $t3, N_VISFRAME($a0)
	ICSEC   dsn_next
dsn_next:
	addiu   $a1, $a1, DL_SIZE
	bne     $a1, $a2, dsn_light_b
	lw      $t0, DL_O+0($a1)
1:	addiu   $sp, $sp, DSN_STACK
.endif

	# ---- BSP walk -------------------------------------------------------
	lw      $t0, RS_SEQ($s0)             # the zone's cooked walk, while the camera is
	lw      $s1, RS_ROOT($s0)            # in one of its leaves (zone.c): seq_start below
	lw      $s0, SP_PLANES($t8)          # s0 = the plane table for the walk
	li      $s2, 0xf                     # all four frustum planes active
	sw      $zero, SP_SPILLS($t8)
	bnez    $t0, seq_start
	addiu   $s3, $t8, 4                  # walk stack = scratchpad + 4

walk:
	andi    $t0, $s1, 1
walk_t0:                                 # (t0 = s1 & 1: a leaf?)
	bnez    $t0, walk_leaf
	addiu   $a0, $s1, -1                 # (the leaf, for walk_leaf)
	beqz    $s2, wn_inside               # fully inside the frustum already:
	lhu     $t0, N_VISFRAME($s1)         # no box to load
	lh      $t1, N_C+4($s1)              # the frustum test's box. VZ with lh:
	bne     $t0, $s5, pop                # the halfwords after c[2] and h[2]
	lwc2    C2_VXY0, N_C+0($s1)          # are visframe / plane3, not part of
	lh      $t2, N_H+4($s1)              # the vectors
	lwc2    C2_VXY1, N_H+0($s1)
.if R_PROFILE
	lw      $t3, SP_NODES($t8)
.endif
	mtc2    $t1, C2_VZ0
	mtc2    $t2, C2_VZ1
.if R_PROFILE
	addiu   $t3, $t3, 1
	sw      $t3, SP_NODES($t8)
.endif
	nop                                  # (two before the GTE reads them)
	lhu     $a2, N_PLANE3($s1)           # (wn_side's first; FRUSTUM keeps a0,
	FRUSTUM pop, "sll $a2, $a2, 2", "addu $a2, $a2, $s0", "lw $a3, PN_DIST($a2)", "lbu $a0, PN_TYPE($a2)", "sra $a3, $a3, 1"
	b       wn_side_2                    #  a2, a3: the plane's setup is in its
	addiu   $s3, $s3, 4                  #  MVMVAs' shadows)

	# the node's plane: d = n . cam - dist (x4096, vertex units); axial
	# planes skip the products
	ICSEC   wn_inside
wn_inside:                               # t0 = its visframe (the delay slot's)
	lhu     $a2, N_PLANE3($s1)           # a2: plane index x 3, a word offset in
	bne     $t0, $s5, pop                # the shared table s0 (dist in 1/8
	sll     $a2, $a2, 2                  # units; harmless when popping)
.if R_PROFILE
	lw      $t3, SP_NODES($t8)
	nop
	addiu   $t3, $t3, 1
	sw      $t3, SP_NODES($t8)
.endif
	addu    $a2, $a2, $s0
	lw      $a3, PN_DIST($a2)
	lbu     $a0, PN_TYPE($a2)
	addiu   $s3, $s3, 4                  # (s3: the push's, below)
	sra     $a3, $a3, 1                  # dist -> vertex units, x4096
wn_side_2:                               # a2 = the plane, a3 = dist, a0 = type
	sltiu   $t2, $a0, 3
	beqz    $t2, wn_general
	sll     $t0, $a0, 2
	addu    $t0, $t0, $t8
	lw      $v0, SP_CAM($t0)             # axial: n . cam = cam[type] x4096
	sll     $t4, $s2, 28                 # (the push word's clip flags,
	sll     $v0, $v0, 12
	or      $t4, $t4, $s1                #  node
	andi    $at, $s3, (1 << WALK_SHIFT) - 1   # and room test)
	subu    $v0, $v0, $a3
	# Stack word: node address (a KUSEG pointer, < 0x00200000: no mask on
	# pop or push) | side x 4 << 24 | no-draw << 27 | clip flags << 28.
wn_push:                                 # (t4 = node | clip flags << 28, at =
	slt     $t0, $v0, $zero              # room in the block, s3 moved)
	addiu   $t2, $v0, BACKFACE_EPS       # side: 1 if the camera is behind;
	sltiu   $t2, $t2, 2 * BACKFACE_EPS + 1   # no faces if |d| <= eps:
	sll     $t2, $t2, 1                  # (unsigned) d + eps <= 2 eps
	or      $t2, $t2, $t0
	sll     $t2, $t2, 26
	or      $t3, $t2, $t4
	sll     $t0, $t0, 2
	addu    $t0, $t0, $s1
	lw      $s1, N_CHILD($t0)            # near child first
	sw      $t3, SP_STACK - 4($s3)       # push
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
	addiu   $t4, $t8, 4
1:	lw      $t5, 0($t4)
	addiu   $t4, $t4, 4
	sw      $t5, 0($t2)
	bne     $t4, $s3, 1b
	addiu   $t2, $t2, 4
	b       walk
	addiu   $s3, $t8, 4

	# general plane: n . cam by the GTE (the colour matrix's first row is
	# the camera: r_render), sf 0, exact (|n . cam| < 2^28)
	ICSEC   wn_general
wn_general:
	lwc2    C2_VXY0, PN_N+0($a2)         # nx | ny << 16
	lh      $t2, PN_N+4($a2)             # nz (VZ0 from a halfword: see the
	sll     $t4, $s2, 28                 # record test). (the push word's clip
	mtc2    $t2, C2_VZ0                  # flags, node and room test, as on
	or      $t4, $t4, $s1                # the axial path)
	andi    $at, $s3, (1 << WALK_SHIFT) - 1
	MVMVA   0, 2, 0, 3, 0                # MAC1 = cam . n
	mfc2    $v0, C2_MAC1                 # (waits for the GTE)
	b       wn_push
	subu    $v0, $v0, $a3

	ICSEC   walk_refill
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
	addiu   $t4, $t8, 4
	addiu   $s3, $t8, 1 << WALK_SHIFT
2:	lw      $t5, 0($t2)
	addiu   $t2, $t2, 4
	sw      $t5, 0($t4)
	addiu   $t4, $t4, 4
	bne     $t4, $s3, 2b
	nop
	b       pop
	nop

	ICSEC   walk_leaf
walk_leaf:                               # a0 = the leaf (s1 - 1)
	lhu     $t0, L_VISFRAME($a0)
	lhu     $a3, L_NMARKS($a0)
	bne     $t0, $s5, pop
	lw      $a2, L_MARKS($a0)
	beqz    $s2, wl_marks                # fully inside the frustum: no box
	sll     $a3, $a3, 1                  # marks x 2 bytes
	lh      $t1, L_C+4($a0)
	lwc2    C2_VXY0, L_C+0($a0)
	lh      $t2, L_H+4($a0)
	lwc2    C2_VXY1, L_H+0($a0)
	mtc2    $t1, C2_VZ0
	mtc2    $t2, C2_VZ1
	nop                                  # (two before the GTE reads them)
	nop
	FRUSTUM pop
wl_marks:
	lw      $t3, SP_FVBASE($t8)
	lw      $t9, L_VIS($a0)              # (the leaf's render list: its top byte)
	beqz    $a3, wl_ents
	addu    $a3, $a3, $a2                # end of the marks
	lhu     $t0, 0($a2)
wl_mark:                                 # (marks are byte offsets: world.c)
	addiu   $a2, $a2, 2
	addu    $t0, $t0, $t3
	sh      $s4, 0($t0)                  # face_visframe[mark] = framecount
	bne     $a2, $a3, wl_mark
	lhu     $t0, 0($a2)                  # (next; one past the last: harmless)
wl_ents:
	bltz    $t9, draw_ents               # models / particles in this leaf (the
	addiu   $s3, $s3, -4                 # list id's bit 7 is the word's sign):
	                                     # drawn here, in the painter's order
	b       pop_1
	lw      $t0, SP_STACK($s3)           # (pop_1's first)

	ICSEC   pop
pop:
	addiu   $s3, $s3, -4
	lw      $t0, SP_STACK($s3)           # (see wn_push; empty: the word at +0)
pop_1:                                   # (t0 loaded; draw_ents, wl_ents come here)
	beq     $s3, $t8, walk_refill        # empty (the refill resets s3)
	srl     $s2, $t0, 28                 # clip flags
	srl     $t3, $t0, 24
	andi    $t4, $t3, 8                  # no faces
	andi    $t3, $t3, 4                  # side * 4
	sll     $t2, $t0, 8
	srl     $t2, $t2, 8                  # node (KUSEG)
	subu    $t5, $t2, $t3
	lhu     $t7, N_DLSTAMP($t2)          # (a dynamic light reaches the node? below)
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
# Packet space: s7 stops QW_MAX_REC_PKT + 48 short of the arena end
# (r_frame), so one test per record covers all of its packets (the window
# command of the run it closes included) and walk_done's; the slow path
# emits a triangle only while s6 <= s7, so it ends at most 40 bytes over.
# Clobbers at, v0, a0-a3, t0-t7, t9; back to `walk` when done.
# Dynamic lights: a node whose box a live light reaches carries this
# frame's count (dl_push stamps them from the root, box by box); gp holds
# the node then, 0 otherwise, and the record loop tests each stamped
# record of such a block against the lights and lights it in place before
# drawing it (dr_light_rec), the walk being the one place that holds the
# drawn records and their plane together.
	bne     $t7, $s4, dr_lit
	move    $gp, $zero
	move    $gp, $t2
dr_lit:
	RECLOOP dr, walk, 0, 0, 1

# dr_light_rec: the stamped record a0 of a block a dynamic light reaches
# (gp = the node), in view (past the frustum test, so no light is spent on
# a culled record), about to be drawn. Sky and liquid records are unlit in
# Quake (no lightmap); the others' sphere against each live light in the
# loop's own spare registers (nothing saved: a2, the record's near and
# affine bits, is kept clear of; a3 and at are reloaded). The first light
# that reaches it sends the record to dr_light_full, which keeps what the
# record loop takes (a1, a2, v0, v1, fp, gp, t8; t2 is the gate's slot,
# run again on the way back) and lights it: the asm body right there
# (dl_light_record_asm, its light loop going on from that light, whose
# test is not run again) or, R_DLIGHT_C = 1, the C reference
# dl_light_record through a call (the C callee may write the frame's
# first 16 bytes, the o32 argument slots, and gets the real gp back from
# r_gp_saved). Then back to the loop.
	ICSEC   dr_light_rec
dr_light_rec:
	lbu     $t0, R_FLAGS($a0)
	lw      $t1, SP_RSTATE($t8)
	andi    $t0, $t0, 3                  # sky | liquid
	.globl  dlr_ret0                     # (the three ways back: render.c
dlr_ret0:                                #  points them at dro_lit_back when
	bnez    $t0, dr_lit_back             #  the options' loop runs)
	lh      $t3, R_SC+0($a0)             # the record's sphere
	lw      $a3, RS_LIGHTS($t1)
	lw      $t2, RS_NLIGHTS($t1)
	lh      $t4, R_SC+2($a0)
	lh      $t5, R_SC+4($a0)
	sll     $t2, $t2, 5
	lhu     $t6, R_SR($a0)
	addu    $t2, $t2, $a3                # the lights' end
	lw      $t0, DL_O+0($a3)             # against the first light
dlr_test:
	lw      $t1, DL_O+4($a3)
	lw      $t7, DL_O+8($a3)
	subu    $t0, $t3, $t0
	subu    $t1, $t4, $t1
	subu    $t7, $t5, $t7
	sra     $at, $t0, 31
	xor     $t0, $t0, $at
	subu    $t0, $t0, $at                # |dx|
	sra     $at, $t1, 31
	xor     $t1, $t1, $at
	subu    $t1, $t1, $at                # |dy|
	slt     $at, $t0, $t1
	beqz    $at, 1f
	sra     $at, $t7, 31                 # (|dz| begins in the slot)
	move    $t0, $t1
1:	xor     $t7, $t7, $at
	subu    $t7, $t7, $at                # |dz|
	slt     $at, $t0, $t7
	beqz    $at, 2f
	lw      $t1, DL_RMM($a3)             # radius - minlight (used two below)
	move    $t0, $t7
2:	subu    $t0, $t0, $t6                # far - sr
	slt     $at, $t0, $t1
	bnez    $at, dr_light_full           # under radius - minlight: lit
	addiu   $a3, $a3, DL_SIZE
	bne     $a3, $t2, dlr_test
	lw      $t0, DL_O+0($a3)             # (one past the last: harmless)
	lhu     $a3, R_TEX($a0)              # no light reaches it: the loop's a3, at
	.globl  dlr_ret1
dlr_ret1:
	j       dr_lit_back
	lw      $at, SP_LASTTEX($t8)

	.set DLR_A0,    16     # dr_light_full's keeps of the record loop (0..15: the
	.set DLR_FP,    20     # o32 argument slots of the C calls; a0, fp, gp: the
	.set DLR_GP,    24     # C variants only)
	.set DLR_RA,    28
	.set DLR_A1,    32
	.set DLR_FLAGS, 36     # a2: the record's near and affine bits
	.set DLR_V0,    40
	.set DLR_V1,    44
	.set DLR_ID,    48     # dl_light_record_asm's own
	.set DLR_NV,    52
	.set DLR_LIGHT, 56
	.set DLR_SQRT,  60
	.set DLR_END,   64
	.set DLR_PLANE, 68
	.set DLR_A2,    72     # the light loop's cursor and end, across the vertex loop
	.set DLR_A3,    76
	.set DLR_SIZE,  80
	ICSEC   dr_light_full
dr_light_full:
	addiu   $sp, $sp, -DLR_SIZE
	sw      $ra, DLR_RA($sp)
	sw      $a1, DLR_A1($sp)
	sw      $a2, DLR_FLAGS($sp)
	sw      $v0, DLR_V0($sp)
	sw      $v1, DLR_V1($sp)
	addiu   $a2, $a3, -DL_SIZE           # the light that reached the record
	move    $a3, $t2                     # (the test's slot went past it), the end
.if R_DLIGHT_C != 0                      # (the asm keeps a0, fp and gp itself)
	sw      $a0, DLR_A0($sp)
	sw      $fp, DLR_FP($sp)
	sw      $gp, DLR_GP($sp)
.endif
.if R_DLIGHT_C == 1
	move    $a1, $gp
	lui     $gp, %hi(r_gp_saved)
	lw      $gp, %lo(r_gp_saved)($gp)
	jal     dl_light_record
	nop
	b       dlr_back
	nop
.else
# dl_light_record_asm: a0 = the record, gp = its node, a2 = the first light
# that reaches the record, a3 = the lights' end. The C reference
# (engine/dlight.c dl_light_record) documents it: for each live light
# whose sphere reaches the record's, the light's terms on the node's plane
# (d, rad = radius - |d|, the impact point; cached in dl_blk while the
# block lasts, DLB_*), nothing under minlight; the light bytes saved once
# a frame (dl_touched, a dl_save_buf entry); then each vertex within
# cutoff of the impact point gets (rad - dist) x 53/128, saturating, dist
# from dl_sqrt on the squared distance >> 11. Clobbers at, v0, v1, a1-a3,
# t0-t7, t9, ra.
dl_light_record_asm:
	lhu     $t4, R_FACE_ID($a0)
	lw      $t0, SP_RSTATE($t8)
	srl     $t4, $t4, 1                  # the id
	sltiu   $at, $t4, 8192
	beqz    $at, dlr_back                # (past the bitmap: unlit)
	sw      $t4, DLR_ID($sp)
	lhu     $t1, N_PLANE3($gp)
	lw      $t2, SP_PLANES($t8)
	la      $v1, dl_blk
	sll     $t1, $t1, 2
	addu    $t2, $t2, $t1                # the node's plane
	sw      $t2, DLR_PLANE($sp)
	lw      $t3, 0($v1)                  # the cache's block
	lw      $t7, RS_LIGHTS($t0)          # the lights
	beq     $t3, $gp, 1f                 # this block's terms are in
	subu    $t5, $a2, $t7                # (the light's offset in the table)
	sw      $gp, 0($v1)                  # another block: its terms stale
	addiu   $t4, $v1, DLB_FIRST
2:	sw      $zero, DLB_VALID($t4)
	addiu   $t7, $t7, DL_SIZE
	bne     $t7, $a3, 2b
	addiu   $t4, $t4, DLB_SIZE
1:	lbu     $t6, R_NVERTS($a0)
	lhu     $t9, R_POLYS($a0)
	la      $t3, dl_sqrt
	sw      $t6, DLR_NV($sp)
	addiu   $t7, $t6, 3
	andi    $t7, $t7, 0xfc
	addu    $t9, $t9, $a0
	subu    $v0, $t9, $t7                # the light bytes
	sw      $t3, DLR_SQRT($sp)
	sw      $v0, DLR_LIGHT($sp)
	sll     $t6, $t6, 1
	addiu   $t6, $t6, R_VERTS
	addu    $t6, $t6, $a0
	sw      $t6, DLR_END($sp)            # the indices' end
	srl     $t4, $t5, 1                  # the light's cache entry: DLB_FIRST +
	addu    $t5, $t5, $t4                # its offset x 48 / 32
	addu    $v1, $v1, $t5
	lw      $t0, DL_O+0($a2)             # its o (dlr_light loads the next ones')
	lw      $t1, DL_O+4($a2)
	lw      $t2, DL_O+8($a2)
	addiu   $v1, $v1, DLB_FIRST
	b       dlr_pass                     # its sphere test is done
	lw      $t4, DLB_VALID($v1)
	ICSEC   dlr_light
dlr_light:
	beq     $a2, $a3, dlr_done
	lh      $t3, R_SC+0($a0)             # the record's sphere against the light
	lw      $t0, DL_O+0($a2)
	lh      $t4, R_SC+2($a0)
	lw      $t1, DL_O+4($a2)
	lh      $t5, R_SC+4($a0)
	lw      $t2, DL_O+8($a2)
	subu    $t3, $t3, $t0
	subu    $t4, $t4, $t1
	subu    $t5, $t5, $t2
	sra     $at, $t3, 31
	xor     $t3, $t3, $at
	subu    $t3, $t3, $at
	sra     $at, $t4, 31
	xor     $t4, $t4, $at
	subu    $t4, $t4, $at
	sra     $at, $t5, 31
	xor     $t5, $t5, $at
	subu    $t5, $t5, $at
	slt     $at, $t3, $t4
	beqz    $at, 1f
	lhu     $t6, R_SR($a0)
	move    $t3, $t4
1:	slt     $at, $t3, $t5
	beqz    $at, 2f
	lw      $t7, DL_RMM($a2)
	move    $t3, $t5
2:	subu    $t3, $t3, $t6                # far - sr
	slt     $at, $t3, $t7
	beqz    $at, dlr_next                # >= radius - minlight: out of reach
	lw      $t4, DLB_VALID($v1)
dlr_pass:                                # (the first light: its test done by the gate)
	lw      $t3, DLB_CUTOFF($v1)         # (the cached cutoff, for dlr_terms_in)
	bnez    $t4, dlr_terms_in
	lw      $t7, DLR_PLANE($sp)          # (harmless when cached: t7 is written before read)
	# the light's terms on the plane: d = n . o - dist (an axial plane:
	# o[type] x 4096), rad = radius - |d|, cutoff = rad - minlight (< 0:
	# under minlight), the impact point o - n d >> 12 (an axial plane's n
	# is 4096 on its axis)
	lw      $t6, DL_RADIUS($a2)
	lbu     $t9, PL_TYPE($t7)
	lw      $t3, PL_DIST($t7)
	sltiu   $at, $t9, 3
	beqz    $at, 3f
	sra     $t3, $t3, 1                  # 1/8 units -> vertex units, x4096
	sll     $t9, $t9, 2
	addu    $t9, $t9, $a2
	lw      $t9, DL_O($t9)               # o[type]
	b       4f
	sll     $t9, $t9, 12
3:	lh      $t4, PL_N+0($t7)
	lh      $t5, PL_N+2($t7)
	lh      $t9, PL_N+4($t7)
	mult    $t4, $t0
	mflo    $t4
	nop                                  # (two apart from the next mult: HI/LO)
	nop
	mult    $t5, $t1
	mflo    $t5
	nop
	nop
	mult    $t9, $t2
	addu    $t4, $t4, $t5
	mflo    $t9
	addu    $t9, $t9, $t4
4:	subu    $t9, $t9, $t3
	sra     $t9, $t9, 12                 # d
	sra     $at, $t9, 31
	xor     $t3, $t9, $at
	subu    $t3, $t3, $at                # |d|
	lw      $t4, DL_MINLIGHT($a2)
	subu    $t3, $t6, $t3                # rad = radius - |d|
	subu    $t3, $t3, $t4                # cutoff = rad - minlight
	sw      $t3, DLB_CUTOFF($v1)
	sw      $t4, DLB_MIN($v1)
	lh      $t4, PL_N+0($t7)
	lh      $t5, PL_N+2($t7)
	lh      $t7, PL_N+4($t7)
	li      $t6, 1
	sw      $t6, DLB_VALID($v1)
	mult    $t4, $t9
	mflo    $t4
	sra     $t4, $t4, 12
	subu    $t0, $t0, $t4
	mult    $t5, $t9
	mflo    $t5
	sra     $t5, $t5, 12
	subu    $t1, $t1, $t5
	mult    $t7, $t9
	mflo    $t7
	sw      $t0, DLB_IX($v1)
	sra     $t7, $t7, 12
	subu    $t2, $t2, $t7
	sw      $t1, DLB_IY($v1)
	sw      $t2, DLB_IZ($v1)
dlr_terms_in:                            # t3 = cutoff
	bltz    $t3, dlr_next                # under minlight: nothing
	lw      $t4, DLR_ID($sp)
	# saved once a frame: the dl_touched bit
	la      $t6, dl_touched
	lw      $t6, 0($t6)
	srl     $t5, $t4, 5
	sll     $t5, $t5, 2
	addu    $t6, $t6, $t5                # its word
	andi    $t7, $t4, 31
	li      $t9, 1
	sllv    $t7, $t9, $t7                # its bit
	lw      $t9, 0($t6)
	lw      $t0, DLR_NV($sp)
	and     $at, $t9, $t7
	bnez    $at, dlr_lit                 # saved already
	lw      $v0, DLR_LIGHT($sp)
	la      $t4, dl_save_n
	lw      $t5, 0($t4)
	or      $t9, $t9, $t7
	sltiu   $at, $t5, DL_SAVE_MAX
	beqz    $at, dlr_done                # the frame's budget: the rest stays unlit
	sw      $t9, 0($t6)
	addiu   $t6, $t5, 1
	sw      $t6, 0($t4)                  # dl_save_n++
	la      $t4, dl_save_buf
	lw      $t4, 0($t4)
	sll     $t6, $t5, 6                  # entry = n x 52 = (n << 6) - (n << 3) - (n << 2)
	sll     $t7, $t5, 3
	subu    $t6, $t6, $t7
	sll     $t7, $t5, 2
	subu    $t6, $t6, $t7
	addu    $t4, $t4, $t6                # the entry
	sw      $a0, 0($t4)
	sw      $t0, 4($t4)
	addiu   $t5, $t0, 3
	andi    $t5, $t5, 0xfc               # the light bytes, padded to 4
	srl     $t5, $t5, 2                  # as words
	move    $t7, $v0
	addiu   $t4, $t4, 8
5:	lw      $t9, 0($t7)
	addiu   $t5, $t5, -1
	addiu   $t7, $t7, 4
	sw      $t9, 0($t4)
	bnez    $t5, 5b
	addiu   $t4, $t4, 4
dlr_lit:
	# the vertices: ra = their indices, t6 = how many are left, v0 = the
	# light bytes; a3 = the next vertex's address is fetched an iteration
	# ahead, in the GTE's shadow (SQR: the squared offset's three terms);
	# the light loop's a2, a3 are kept on the stack meanwhile
	sw      $a2, DLR_A2($sp)
	sw      $a3, DLR_A3($sp)
	lw      $t0, DLB_IX($v1)
	lw      $t1, DLB_IY($v1)
	lw      $t2, DLB_IZ($v1)
	lw      $t4, DLB_MIN($v1)
	lw      $t5, DLR_SQRT($sp)
	lw      $t6, DLR_NV($sp)
	lw      $t9, SP_VERTPOOL($t8)
	lhu     $a3, R_VERTS($a0)            # the first vertex
	addiu   $ra, $a0, R_VERTS + 2
	sll     $a3, $a3, 3
	addu    $a3, $a3, $t9
dlr_v:
	lh      $at, 0($a3)                  # x
	lh      $a1, 2($a3)                  # y  (a1, the node: dlr_done reloads nothing from it)
	lh      $t7, 4($a3)                  # z
	subu    $at, $at, $t0
	subu    $a1, $a1, $t1
	mtc2    $at, C2_IR1                  # the offset to the GTE: SQR squares
	subu    $t7, $t7, $t2                # its three terms at once (sf 0:
	mtc2    $a1, C2_IR2                  # exact, |offset| < 2^15)
	mtc2    $t7, C2_IR3
	lbu     $a2, 0($v0)                  # its light byte
	lhu     $a3, 0($ra)                  # the next vertex (one past the last: harmless)
	SQR     0
	addiu   $ra, $ra, 2
	sll     $a3, $a3, 3
	addu    $a3, $a3, $t9
	addiu   $t6, $t6, -1                 # one fewer left
	mfc2    $at, C2_MAC1
	mfc2    $a1, C2_MAC2
	mfc2    $t7, C2_MAC3
	addu    $at, $at, $a1
	addu    $t7, $t7, $at                # the squared distance
	srl     $t7, $t7, 11
	sltiu   $at, $t7, 1024
	beqz    $at, dlr_v_next              # past the table: far
	sll     $t7, $t7, 1
	addu    $t7, $t7, $t5
	lhu     $t7, 0($t7)                  # dist
	nop
	subu    $t7, $t3, $t7                # cutoff - dist
	blez    $t7, dlr_v_next
	addu    $t7, $t7, $t4                # rad - dist
	sll     $at, $t7, 6                  # x 53 = 64 - 1 - 2 - 8 (a quarter
	subu    $at, $at, $t7                #  world unit per vertex unit x the
	sll     $t7, $t7, 1                  #  cook's light gain, 1.656: dlight.h
	subu    $at, $at, $t7                #  DL_GAIN)
	sll     $t7, $t7, 2
	subu    $at, $at, $t7
	sra     $t7, $at, 7                  # / 128
	addu    $a2, $a2, $t7
	sltiu   $at, $a2, 256
	bnez    $at, dlr_v_next
	sb      $a2, 0($v0)                  # (overwritten below when it overflowed)
	li      $a2, 255
	sb      $a2, 0($v0)
dlr_v_next:
	bnez    $t6, dlr_v
	addiu   $v0, $v0, 1
	lw      $v0, DLR_LIGHT($sp)          # (the next light's pass)
	lw      $a2, DLR_A2($sp)
	lw      $a3, DLR_A3($sp)
dlr_next:
	addiu   $v1, $v1, DLB_SIZE
	b       dlr_light
	addiu   $a2, $a2, DL_SIZE
	ICSEC   dlr_done
dlr_done:
.if R_DLIGHT_C == 2                      # then the C checks what the asm wrote
	lw      $a0, DLR_A0($sp)
	lw      $a1, DLR_GP($sp)
	lui     $gp, %hi(r_gp_saved)
	lw      $gp, %lo(r_gp_saved)($gp)
	jal     dl_check_record
	lui     $t8, 0x1f80
.endif
.endif
dlr_back:
.if R_DLIGHT_C != 0
	lw      $a0, DLR_A0($sp)
	lw      $fp, DLR_FP($sp)
	lw      $gp, DLR_GP($sp)
	lui     $t8, 0x1f80
.endif
	lw      $ra, DLR_RA($sp)
	lw      $a1, DLR_A1($sp)
	lw      $a2, DLR_FLAGS($sp)
	lw      $v0, DLR_V0($sp)
	lw      $v1, DLR_V1($sp)
	lhu     $a3, R_TEX($a0)              # the loop's a3, at again
	lw      $at, SP_LASTTEX($t8)
	.globl  dlr_ret2
dlr_ret2:
	j       dr_lit_back
	addiu   $sp, $sp, DLR_SIZE

# -----------------------------------------------------------------------------
# dl_restore_asm: the records' light bytes back (C ABI, from
# game_render_done): dl_save_n entries of DL_SAVE_ENTRY bytes in
# dl_save_buf (the record, its vertex count, the bytes as words).
# Clobbers at, v0, v1, a0-a3, t0-t9.
	ICSEC   dl_restore_asm
	.global dl_restore_asm
dl_restore_asm:
	lui     $a0, %hi(dl_save_n)
	lw      $a1, %lo(dl_save_n)($a0)
	lui     $t0, %hi(dl_save_buf)
	lw      $t0, %lo(dl_save_buf)($t0)
	sw      $zero, %lo(dl_save_n)($a0)   # none left after this
	beqz    $a1, 2f
	nop
1:	lw      $t1, 0($t0)                  # the record
	lw      $t2, 4($t0)                  # its vertex count
	addiu   $a1, $a1, -1
	lhu     $t3, R_POLYS($t1)
	addiu   $t2, $t2, 3
	andi    $t2, $t2, 0xfc
	addu    $t3, $t3, $t1
	subu    $t3, $t3, $t2                # the light bytes
	srl     $t2, $t2, 2                  # as words
	addiu   $t4, $t0, 8
3:	lw      $t5, 0($t4)
	addiu   $t2, $t2, -1
	addiu   $t4, $t4, 4
	sw      $t5, 0($t3)
	bnez    $t2, 3b
	addiu   $t3, $t3, 4
	bnez    $a1, 1b
	addiu   $t0, $t0, DL_SAVE_ENTRY
2:	jr      $ra
	nop


	ICSEC   walk_done
walk_done:
	lw      $t0, SP_LASTTEX($t8)
	lw      $t1, SP_TWIN($t8)
	lw      $s0, SP_RSTATE($t8)          # r_state again
	bltz    $t0, 1f                      # nothing drawn
	lui     $t9, 0x0100
	or      $t9, $t9, $fp                # the last run's texture window
	sw      $t9, 0($s6)                  # (room: s7 is QW_MAX_REC_PKT + 48
	sw      $t1, 4($s6)                  # short of the arena end)
	move    $fp, $s6
	addiu   $s6, $s6, 8
1:
	lui     $t0, 0x8000                  # back to a KSEG0 pointer for C
	or      $s6, $s6, $t0
	sw      $s6, RS_PKT($s0)
	lw      $t0, RS_OT_WORLD($s0)
	lw      $t1, SP_SLOWB($t8)
	sw      $fp, 0($t0)
	sw      $t1, RS_SLOW_BYTES($s0)
.if R_PROFILE
	lw      $t0, SP_NODES($t8)
	lw      $t1, SP_RECORDS($t8)
	lw      $t2, SP_VERTS($t8)
	lw      $t3, SP_POLYS($t8)
	lw      $t4, SP_SKIPPED($t8)
	lw      $t5, SP_TBLOCK($t8)
	lw      $t6, SP_TSLOW($t8)
	sw      $t0, RS_STAT_NODES($s0)
	sw      $t1, RS_STAT_RECORDS($s0)
	sw      $t2, RS_STAT_VERTS($s0)
	sw      $t3, RS_STAT_POLYS($s0)
	sw      $t4, RS_STAT_SKIPPED($s0)
	sw      $t5, RS_TICKS_BLOCK($s0)
	sw      $t6, RS_TICKS_SLOW($s0)
.endif

	lw      $ra, 44($sp)                 # first: jr must not sit in its delay slot
	lui     $gp, %hi(r_gp_saved)
	lw      $gp, %lo(r_gp_saved)($gp)
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
# Axial planes skip the products; a general one is an MVMVA, as in the walk.
# Clobbers t0-t2, t5, v0.
	ICSEC   plane_dist
plane_dist:
	lhu     $t0, N_PLANE3($a0)           # the node's plane, in the shared
plane_dist_1:                            # table (dist in 1/8 units; fl_loop
	lw      $t1, SP_PLANES($t8)          # does the load in its call's slot)
	sll     $t0, $t0, 2
	addu    $t5, $t0, $t1
	lbu     $t0, PN_TYPE($t5)
	lw      $t1, PN_DIST($t5)
	lwc2    C2_VXY0, PN_N+0($t5)         # (a general plane's normal, ahead)
	sltiu   $t2, $t0, 3
	beqz    $t2, pd_general
	sll     $t0, $t0, 2
	addu    $t0, $t0, $t8
	lw      $v0, SP_CAM($t0)
	sra     $t1, $t1, 1                  # -> vertex units, x4096
	sll     $v0, $v0, 12
	jr      $ra
	subu    $v0, $v0, $t1
	ICSEC   pd_general
pd_general:
	lh      $t2, PN_N+4($t5)
	sra     $t1, $t1, 1
	mtc2    $t2, C2_VZ0
	nop
	nop
	MVMVA   0, 2, 0, 3, 0                # MAC1 = cam . n
	mfc2    $v0, C2_MAC1                 # (waits for the GTE)
	jr      $ra
	subu    $v0, $v0, $t1

# -----------------------------------------------------------------------------
# close_run: end the texture run in progress, if any: its window command
# goes out after its polygons (so before them in the bucket) and
# SP_LASTTEX becomes -1, so the next record reloads its texture.
# Clobbers t6, t9.
	ICSEC   close_run
close_run:
	lw      $t9, SP_LASTTEX($t8)
	lw      $t6, SP_TWIN($t8)
	bltz    $t9, 1f
	li      $t9, -1
	lui     $t9, 0x0100
	or      $t9, $t9, $fp
	sw      $t9, 0($s6)
	sw      $t6, 4($s6)
	move    $fp, $s6
	addiu   $s6, $s6, 8
	li      $t9, -1
1:	jr      $ra
	sw      $t9, SP_LASTTEX($t8)

# -----------------------------------------------------------------------------
# light_recs: the light bytes of the records a1 .. a2 (a model's, pool t4)
# for this draw: each vertex's normal through the table a3 (its factor x128)
# times the light t1 (<= 170, so the byte never overflows); a3 = 0: the
# light itself, flat. Keeps a0, a2, a3, t1, t4, t5; clobbers a1, t0, t6,
# t7, v0, v1.
	ICSEC   light_recs
light_recs:
lr_rec:
	beq     $a1, $a2, lr_done
	lbu     $t6, R_NVERTS($a1)           # (at the end: a harmless read)
	lhu     $t7, R_POLYS($a1)
	lhu     $t0, R_SIZE($a1)
	addiu   $v0, $t6, 3
	andi    $v0, $v0, 0xfc
	addu    $t7, $t7, $a1
	addiu   $v1, $a1, R_VERTS            # the indices
	subu    $t7, $t7, $v0                # the light bytes
	beqz    $a3, lr_flat
	addu    $a1, $a1, $t0                # (the next record)
lr_v:
	lhu     $t0, 0($v1)
	addiu   $v1, $v1, 2
	sll     $t0, $t0, 3
	addu    $t0, $t0, $t4
	lbu     $t0, 6($t0)                  # the normal index
	addiu   $t6, $t6, -1
	addu    $t0, $t0, $a3
	lbu     $t0, 0($t0)                  # its factor
	addiu   $t7, $t7, 1
	mult    $t0, $t1
	mflo    $t0
	srl     $t0, $t0, 7
	bnez    $t6, lr_v
	sb      $t0, -1($t7)
	b       lr_rec
	nop
	ICSEC   lr_flat
lr_flat:
	sb      $t1, 0($t7)
	addiu   $t6, $t6, -1
	bnez    $t6, lr_flat
	addiu   $t7, $t7, 1
	b       lr_rec
	nop
	ICSEC   lr_done
lr_done:
	jr      $ra
	nop

# light_pool: a sorted body's lights (QSTM_SORT): byte 7 of each of the t5
# entries of its frame's pool t4 (engine/mdl.c: a level of detail's band
# expanded, the others copies of their parents, so every slot finds its
# own), its normal's factor in the table a3 (x128) times the light t1, or
# the light itself (a3 = 0: flat); XFORM's light 3 reads it where a byte a
# slot of the records was written (2.3 slots a vertex of the band, 1.4 an
# entry of the pool in E2M7's fight: docs/porting-notes.md §107). Two
# entries in flight: the next one's factor read while the multiply runs
# (its read past the last entry harmless: the frame cache's ring, a light
# table's neighbour). Keeps a0, a2, a3, t1, t2, t4; clobbers t0, t5-t7.
	ICSEC   light_pool
light_pool:
	beqz    $a3, lp_flat
	addiu   $t6, $t4, 6                  # (the entry's normal)
	lbu     $t7, 0($t6)                  # the first normal
	nop
	addu    $t7, $t7, $a3
	lbu     $t0, 0($t7)                  # its factor
lp_v:
	lbu     $t7, 8($t6)                  # the next entry's normal
	mult    $t0, $t1                     # this one's light
	addiu   $t5, $t5, -1
	addu    $t7, $t7, $a3
	lbu     $t7, 0($t7)                  # the next one's factor
	addiu   $t6, $t6, 8
	mflo    $t0
	srl     $t0, $t0, 7
	sb      $t0, -7($t6)                 # (byte 7 of the entry just passed)
	bnez    $t5, lp_v
	move    $t0, $t7
	jr      $ra
	nop
lp_flat:
	sb      $t1, 1($t6)                  # byte 7: the light itself
	addiu   $t5, $t5, -1
	bnez    $t5, lp_flat
	addiu   $t6, $t6, 8
	jr      $ra
	nop

# -----------------------------------------------------------------------------
# draw_model: a0 = an r_ent_t (rlist.h). Its records go through the record
# loop (RECLOOP de; a monster's, RE_SORT, through ds_records: dm_sort)
# with its own GTE matrix, vertex pool, skin and light; the texture run in
# progress is closed before, the model's own after, and the view matrix
# and the world's pointers are back on return. Needs the walk's s4, s6 /
# s7 / fp, t8 and SP_RSTATE; clobbers at, v0, v1, a0-a3, t0-t7, t9 (a
# sorted model also s1, s2 and gp: the walk's dead ones at a leaf).
	ICSEC   draw_model
draw_model:
	sw      $ra, SP_DMRA($t8)
dm_body:                                 # (ei_model: its ra and SP_DMRA set)
	lw      $t2, RE_FLAGS($a0)
	subu    $t3, $s7, $s6                # the packet room left (bytes; < 0 past)
	andi    $t2, $t2, 3                  # a brush model, the view model: drawn
.if ASM_BENCH
	bnez    $t2, dm_draw                 # whatever the room (the world's look)
	lui     $t5, %hi(r_model_room_min)   # (delay; the bench: the least room an
	lw      $t4, %lo(r_model_room_min)($t5)   # alias model found this frame,
	nop                                  # render.c, the arenas' sizing)
	slt     $t4, $t3, $t4
	beqz    $t4, 1f
	nop
	sw      $t3, %lo(r_model_room_min)($t5)
1:	slti    $t3, $t3, R_MODEL_ROOM
.else
	bnez    $t2, dm_draw                 # whatever the room (the world's look)
	slti    $t3, $t3, R_MODEL_ROOM
.endif
	bnez    $t3, dm_skip                 # an alias model with less room left
	nop                                  # than R_MODEL_ROOM: left out, the room
dm_draw:                                 # kept for the farther world's records
	jal     close_run
	nop
	lw      $t0, RE_ROT+0($a0)
	lw      $t1, RE_ROT+4($a0)
	lw      $t2, RE_ROT+8($a0)
	lw      $t3, RE_ROT+12($a0)
	lh      $t4, RE_ROT+16($a0)
	ctc2    $t0, C2_R11R12
	ctc2    $t1, C2_R13R21
	ctc2    $t2, C2_R22R23
	ctc2    $t3, C2_R31R32
	ctc2    $t4, C2_R33
	lw      $t0, RE_TR+0($a0)
	lw      $t1, RE_TR+4($a0)
	lw      $t2, RE_TR+8($a0)
	lw      $t5, RE_POOL($a0)
	lw      $t6, RE_TEX($a0)
	ctc2    $t0, C2_TRX
	ctc2    $t1, C2_TRY
	ctc2    $t2, C2_TRZ
	addiu   $t7, $t8, SP_STAMP
	sw      $t5, SP_VERTPOOL($t8)
	sw      $t6, SP_TEXBASE($t8)
	sw      $t7, SP_FVBASE($t8)          # (records' face_id 0 -> the stamp)
	lw      $t2, RE_FLAGS($a0)
	lw      $a1, RE_RECS($a0)
	andi    $t3, $t2, 2                  # RE_BMODEL: a brush model: its
	bnez    $t3, draw_bmodel             # subtree walked (below)
	lw      $a2, RE_RECS_END($a0)
	lw      $a3, RE_DOTS($a0)
	lw      $t1, RE_LIGHT($a0)
	andi    $t3, $t2, RE_SORT            # a monster: its records' level-of-detail
	lw      $t0, RE_LODBAND($a0)         # tables, read at its band (SP_LODBAND:
	beqz    $t3, dm_lit                  # ds_records), its pool's entries lit
	move    $t4, $t5                     # (light_pool: their count the word's
	srl     $t5, $t0, 16                 #  upper half, rlist.c)
	andi    $t0, $t0, 0xffff
	jal     light_pool
	sw      $t0, SP_LODBAND($t8)
	b       dm_lit_done
	li      $t7, -32768
dm_lit:
	jal     light_recs
	nop
	li      $t7, -32768
dm_lit_done:
	andi    $t3, $t2, RE_VIEW | RE_SORT
	beqz    $t3, 1f                      # an alias model: on
	andi    $t2, $t2, RE_SORT | RE_WATER
	bnez    $t2, dm_sort                 # a monster: its triangles sorted (below)
	li      $t3, 124                     # RE_VIEW: the view model. Nothing
	                                     # warps (k16 rho = -32768 rho is under
	                                     # any z - rho; T2 huge: no edge splits:
	li      $t6, 0x7fff                  # the cook cut it, tools/qcook/game.c
	sw      $t3, SP_SZFAST($t8)          # tess_build); its rear is behind the
	sw      $t3, SP_SZCLIP($t8)          # eye: the slow path clips at 5 world
	li      $t3, 120                     # units (SZ 120, Quake's
	sw      $t6, SP_T2($t8)              # ALIAS_Z_CLIP_PLANE: every row of its
	sll     $t6, $t6, 1                  # matrix is x4, rlist.c rl_view_gun, so
	sw      $t3, SP_ZNEAR($t8)           # the GTE's divide, which holds down
	sw      $t7, SP_K16($t8)             # to SZ = H / 2 = 80, gives the screen
	sw      $t6, SP_T4($t8)              # down to 3.3 units)
1:
.if R_PROFILE
	lhu     $t9, TIMER2($t8)             # (the record loop's start time)
.endif
	lw      $t0, SP_FULL($t8)
	lw      $a1, RE_RECS_END($a0)
	lw      $a0, RE_RECS($a0)
	RECLOOP de, dm_exit, 1
	ICSEC   dm_exit
dm_exit:
	lw      $t0, SP_BMWALK($t8)          # a brush model's block (bm_block: on
	nop                                  # with its walk) or a model's pieces
	beqz    $t0, dm_exit_1               # (ep_back)
	nop
	jr      $t0
	nop
	ICSEC   dm_exit_1
dm_exit_1:
	jal     close_run
	sw      $zero, SP_WATER($t8)         # (no liquid's surface for what follows)
	lw      $t0, SP_RSTATE($t8)          # the view's GTE matrix again, and
	li      $t7, 96                      # the world's pointers and thresholds
	lw      $t3, RS_SZ_FAST($t0)
	lw      $t4, RS_WARP_K16($t0)
	lw      $t6, RS_WARP_T2($t0)
	sw      $t7, SP_SZCLIP($t8)
	li      $t7, 48
	sw      $t7, SP_ZNEAR($t8)
	sw      $t3, SP_SZFAST($t8)
	sw      $t4, SP_K16($t8)
	sw      $t6, SP_T2($t8)
	sll     $t6, $t6, 1
	sw      $t6, SP_T4($t8)
	lw      $t1, RS_ROT+0($t0)
	lw      $t2, RS_ROT+4($t0)
	lw      $t3, RS_ROT+8($t0)
	lw      $t4, RS_ROT+12($t0)
	lh      $t5, RS_ROT+16($t0)
	ctc2    $t1, C2_R11R12
	ctc2    $t2, C2_R13R21
	ctc2    $t3, C2_R22R23
	ctc2    $t4, C2_R31R32
	ctc2    $t5, C2_R33
	lw      $t1, RS_TR+0($t0)
	lw      $t2, RS_TR+4($t0)
	lw      $t3, RS_TR+8($t0)
	lw      $t4, RS_VERTPOOL($t0)
	lw      $t5, RS_TEXTURES($t0)
	lw      $t6, RS_FVBASE($t0)
	ctc2    $t1, C2_TRX
	ctc2    $t2, C2_TRY
	ctc2    $t3, C2_TRZ
	sw      $t4, SP_VERTPOOL($t8)
	sw      $t5, SP_TEXBASE($t8)
	sw      $t6, SP_FVBASE($t8)
	lw      $ra, SP_DMRA($t8)
	nop
	jr      $ra
	nop
	ICSEC   dm_skip
dm_skip:                                 # (nothing of the view's state touched)
.if ASM_BENCH
	lui     $t5, %hi(r_model_skipped)    # (the bench: the alias models left out)
	lw      $t4, %lo(r_model_skipped)($t5)
	lw      $ra, SP_DMRA($t8)
	addiu   $t4, $t4, 1
	jr      $ra
	sw      $t4, %lo(r_model_skipped)($t5)
.else
	lw      $ra, SP_DMRA($t8)
	nop
	jr      $ra
	nop
.endif

# -----------------------------------------------------------------------------
# dm_sort: a monster (RE_SORT, formats/qstream.h QSTM_SORT), its matrix,
# pool, skin and light set: its triangles drawn far to near, as Quake's
# z-buffer shows them, through an ordering table of its own: R_SORT_N
# nodes (0-word tags) taken at the packet pointer, a ring, node j linked
# to node j - 1. A triangle goes at the head of its bucket's chain, the
# bucket being its corners' SZ summed times ZSF3 >> 14 (AVSZ3's OTZ / 4,
# mod R_SORT_N); rlist.c ent_sort scales the depths the model can take
# onto R_SORT_N - 2 buckets, so that its range, starting anywhere on the
# ring (RE_ZNEAR, its nearest bucket), never meets itself. That bucket's
# node takes the world's chain as it stands (fp); the model's chain starts
# at its predecessor, the farthest bucket (SP_SORTHEAD: fp at ds_exit),
# and the table is drawn whole there, among the world's faces and the
# leaf's items where the model always was. gp holds the table; s1, s2, ra
# are the sorted loop's temporaries (the walk's dead ones at a leaf;
# SP_DMRA keeps ra). Then dm_exit_1, as for any model.
	ICSEC   dm_sort
dm_sort:
	andi    $t2, $t2, RE_WATER           # a monster through a liquid's surface
	bnez    $t2, dm_wsort                # (rlist.c rl_water): its word in
	lw      $t0, RE_ZSF($a0)             # SP_WATER (dm_wsort; 0 else: dm_exit_1)
dm_sort_1:
	lw      $t1, RE_ZNEAR($a0)
	move    $gp, $s6                     # the table
	ctc2    $t0, C2_ZSF3
	addiu   $t3, $s6, -4                 # (node 0's: set again below)
	move    $t4, $s6
	addiu   $t5, $s6, 4 * R_SORT_N
1:	sw      $t3, 0($t4)                  # node j -> node j - 1, four at a time
	sw      $t4, 4($t4)
	addiu   $t3, $t4, 4
	sw      $t3, 8($t4)
	addiu   $t3, $t4, 8
	sw      $t3, 12($t4)
	addiu   $t4, $t4, 16
	bne     $t4, $t5, 1b
	addiu   $t3, $t4, -4
	addiu   $t3, $t5, -4
	sw      $t3, 0($gp)                  # node 0 -> node N - 1: the ring
	addiu   $t3, $t1, -4                 # the farthest bucket: the nearest's
	andi    $t3, $t3, 4 * R_SORT_N - 1   # predecessor
	addu    $t3, $t3, $gp
	sw      $t3, SP_SORTHEAD($t8)
	addu    $t1, $t1, $gp
	sw      $fp, 0($t1)                  # the nearest's node -> the world's chain
	move    $s6, $t5
.if R_PROFILE
	lhu     $t9, TIMER2($t8)             # (the record loop's start time)
.endif
	lw      $t0, SP_FULL($t8)
	lw      $a1, RE_RECS_END($a0)
	b       ds_records
	lw      $a0, RE_RECS($a0)

# The sorted model's record loop: RECLOOP de's with the draw distance
# (deo1's cull and fog, their thresholds patched the same way: ds_o_dcull,
# ds_o_fogz; render.c sets both to their most when the distance is off),
# for a model of triangles only (the cook's, QSTM_SORT), each emitted into
# its bucket (the GTE's AVSZ3 of its corners' SZ: every vertex's SZ goes
# to the cache), the slow path's pieces into the bucket of the triangle
# they cut. Its records' face ids are not looked at (a model's are 0, the
# stamp); a texture change closes no run (a streamed model's two images
# share their window, none: stream.c), dm_exit_1 giving the table its one
# window command, drawn before it. The level of detail (formats/qstream.h
# QSTM_LOD, §88): each record ends with a table, two bytes a band, the
# slots and the triangles still drawn at that band (their prefixes: the
# cook orders them by the band they last serve); SP_LODBAND holds the
# model's entry, SP_LODCNT the record's two counts: the slots transformed
# (its light bytes stay the record's own) and the triangles emitted; a
# record with no triangle at the band is passed over before its sphere.
	ICSEC   ds_records
	.globl  ds_records
ds_records:
.if R_PROFILE
	sw      $t9, SP_T0($t8)
.endif
	bnez    $t0, ds_exit                 # packet arena full: nothing more
	move    $t2, $zero
ds_cull:
	addu    $a0, $a0, $t2                # a culled record (t2 = its size): the next
	lhu     $t2, R_SIZE($a0)             # (at the end: a harmless read)
ds_rec:
.if R_PROFILE
	beq     $a0, $a1, ds_done
.else
	beq     $a0, $a1, ds_exit
.endif
	lw      $t0, SP_LODBAND($t8)         # the band's entry of its table (at the
	addu    $t1, $a0, $t2                # end: a harmless read): the record's end,
	addu    $t1, $t1, $t0                # the entry
	lbu     $t0, -QST_LOD_REC_BYTES+1($t1)   # its triangles at this band
	lbu     $t1, -QST_LOD_REC_BYTES+0($t1)   # its slots
	beqz    $t0, ds_cull                 # none: the next record
	sll     $t0, $t0, 8
	or      $t0, $t0, $t1
	sw      $t0, SP_LODCNT($t8)
	lh      $t9, R_SC+4($a0)             # its bounding sphere (as RECLOOP's)
	lwc2    C2_VXY0, R_SC+0($a0)
	lhu     $t3, R_SR($a0)
	mtc2    $t9, C2_VZ0
	addu    $t0, $a0, $t2
	lw      $t9, SP_K16($t8)
	sw      $t0, SP_NEXTREC($t8)         # the next record
	MVMVA   1, 0, 0, 0, 0                # IR = rot * centre + TR
	srl     $t7, $t3, 1                  # (while the GTE works; t2 keeps the
	addu    $t7, $t7, $t3                #  record's size for ds_cull) rho = 1.5 r
	mult    $t9, $t7                     # k16 rho, for a2 bit 2 below
	lhu     $a3, R_TEX($a0)
	lw      $at, SP_LASTTEX($t8)
	mfc2    $t0, C2_IR1                  # x
	mfc2    $t1, C2_IR2                  # y
	mfc2    $t4, C2_IR3                  # z
	sll     $t5, $t3, 1
	srl     $t6, $t3, 3
	addu    $t5, $t5, $t6                # 2.125 r
	subu    $t6, $t4, $t0                # right: z - x
	addu    $t6, $t6, $t5
	bltz    $t6, ds_cull
	addu    $t6, $t4, $t0                # left: z + x
	addu    $t6, $t6, $t5
	bltz    $t6, ds_cull
ds_ys_r:
	sll     $t5, $t3, 3                  # 8 r
ds_ys_z:
	sll     $t6, $t4, 1
ds_ys_z2:
	addu    $t6, $t6, $t4                # 3z
ds_ys_y:
	sll     $t0, $t1, 2                  # 4y
.if R_YSLOPE == 5
	addu    $t5, $t5, $t3                # 9 r
	addu    $t0, $t0, $t1                # 5y
.endif
	YSITE2  ds_ys_r, 3, ds_ys_z, 1
	YSITE2  ds_ys_z2, 2, ds_ys_y, 0
	subu    $t9, $t6, $t0                # bottom: 3z - 4y
	addu    $t9, $t9, $t5
	bltz    $t9, ds_cull
	addu    $t9, $t6, $t0                # top: 3z + 4y
	addu    $t9, $t9, $t5
	bltz    $t9, ds_cull
	sll     $t5, $t3, 1                  # delay: 2r
	subu    $t6, $t4, $t7                # nearest depth z - rho
	addu    $t3, $t4, $t7                # farthest depth z + rho
	.globl  ds_o_dcull
ds_o_dcull:
	slti    $t0, $t6, 0                  # nearer than the draw distance?
	beqz    $t0, ds_cull                 # (wholly beyond it: culled)
	lw      $t7, SP_SZFAST($t8)
	subu    $t5, $t4, $t5                # nearest possible SZ (>= 1.5 r away)
	slt     $a2, $t5, $t7                # a2 bit 0: may come too near
	sltu    $t0, $s7, $s6                # room for the whole record?
	bnez    $t0, ds_full
	sll     $t6, $t6, 4
	mflo    $t9                          # a2 bit 2: (z - rho) x 16 < k16 x rho,
	slt     $t9, $t6, $t9                # an affine error over the threshold
	sll     $t9, $t9, 2                  # possible (RECLOOP's)
	or      $a2, $a2, $t9
	beq     $at, $a3, ds_tex_done        # the last record's texture
	lw      $t4, SP_LODCNT($t8)          # (the band's slots of it: the transform's)
	lw      $t5, SP_TEXBASE($t8)         # its words (8 B entry)
	sll     $t6, $a3, 3
	sw      $a3, SP_LASTTEX($t8)
	addu    $t6, $t6, $t5
	lw      $t9, 4($t6)                  # its texture-window command
	lw      $v0, 0($t6)                  # clut | tpage << 16
	sw      $t9, SP_TWIN($t8)            # (dm_exit_1's window command)
	lui     $t5, 0xffff
	and     $v1, $v0, $t5                # tpage << 16, ORed into uv1
	sll     $v0, $v0, 16                 # clut  << 16, ORed into uv0
	sw      $v0, SP_CLUT16($t8)          # (the slow path's copies)
	sw      $v1, SP_TPAGE16($t8)
ds_tex_done:
	nop                                  # (the branch's slot loaded the count;
	andi    $t4, $t4, 0xff               #  the lights are the pool's: XFORM light 3)
.if R_PROFILE
	lw      $t9, SP_RECORDS($t8)
	lw      $t2, SP_VERTS($t8)
	addiu   $t9, $t9, 1
	addu    $t2, $t2, $t4
	sw      $t9, SP_RECORDS($t8)
	sw      $t2, SP_VERTS($t8)
.endif
	.globl  ds_o_fogz
ds_o_fogz:
	slti    $t0, $t3, 0                  # farthest depth before the fog?
	addiu   $t3, $a0, R_VERTS            # the record's indices into the pool
	lw      $t9, SP_VERTPOOL($t8)        # (padded to threes: no stray reads)
	move    $t2, $t4
	addiu   $t5, $t2, -2                 # (vertices left - 2: RTPT while 3+)
	beqz    $t0, ds_fog                  # reaching into the fog: the fogged
	move    $t4, $t8                     #  transform (cache cursor)
	XFORM   1, ds_xf, ds_one, ds_xf_done, 3
ds_xf_done:
	beqz    $a2, ds_polys                # nothing near nor warping: no careful test
	andi    $t0, $a2, 4
	bnez    $t0, ds_polys                # big or warping: careful regardless
	move    $t4, $t8
	move    $t5, $t2
	move    $a2, $zero                   # exact check: any real vertex near?
ds_nz:
	lw      $t0, SP_SZ($t4)
	addiu   $t5, $t5, -1
	sltu    $t0, $t0, $t7
	or      $a2, $a2, $t0
	bgtz    $t5, ds_nz
	addiu   $t4, $t4, 4

	# its triangles (a sorted model has no quads), each, turned toward the
	# eye (NCLIP, as POLYS), at the head of its bucket's chain
ds_polys:
	lw      $t9, SP_WATER($t8)           # a monster through a liquid's surface:
	lhu     $t0, R_POLYS($a0)            # its corners past it marked (ds_water)
	bnez    $t9, ds_water
	lw      $at, SP_LODCNT($t8)          # the band's triangles of it
ds_polys_1:
.if R_PROFILE
	lw      $t1, SP_POLYS($t8)
	addu    $a3, $a0, $t0                # the first triangle
	srl     $at, $at, 8
	addu    $t1, $t1, $at
	sw      $t1, SP_POLYS($t8)
.else
	addu    $a3, $a0, $t0                # the first triangle
	srl     $at, $at, 8
.endif
	lw      $t0, 0($a3)                  # (none: a harmless read)
	beqz    $at, ds_next
ds_tri:                                  # t0 = the triangle's 3 vertex offsets
	addiu   $at, $at, -1                 # (count down; the delay slot above)
	andi    $t3, $t0, 0xff
	srl     $t4, $t0, 8
	andi    $t4, $t4, 0xff
	srl     $t5, $t0, 16
	andi    $t5, $t5, 0xff
	addu    $t3, $t3, $t8
	addu    $t4, $t4, $t8
	beqz    $a2, ds_tri_emit
	addu    $t5, $t5, $t8
	CAREFUL 0, ds_tri_far, 1, ds_tri_near
ds_tri_emit:                             # POLY_GT3 (t3..t5 kept for its bucket)
	lwc2    C2_SXY0, SP_SXY($t3)         # the corners' xy: into the GTE for the
	lwc2    C2_SXY1, SP_SXY($t4)         # NCLIP, from there to the packet (no
	lwc2    C2_SXY2, SP_SXY($t5)         # copy in the CPU's registers)
	lw      $t0, SP_COL($t3)             # c0 (two instructions before the NCLIP)
	lw      $t1, 4($a3)                  # uv0 | uv1 << 16
	NCLIP
	lw      $t2, SP_COL($t4)             # c1
	lw      $s1, SP_COL($t5)             # c2
	lui     $s2, CODE_GT3 >> 16
	or      $t0, $t0, $s2
	andi    $s2, $t1, 0xffff
	or      $s2, $s2, $v0                # uv0 | clut
	srl     $t1, $t1, 16
	mfc2    $ra, C2_MAC0
	or      $t1, $t1, $v1                # uv1 | tpage
	bgez    $ra, ds_tri_next             # turned away: left out
	sw      $t0, 4($s6)                  # (a packet not linked: harmless)
	lwc2    C2_SZ1, SP_SZ($t3)           # its bucket: AVSZ3, ZSF3 x the
	lwc2    C2_SZ2, SP_SZ($t4)           # corners' SZ summed >> 12, / 4
	lwc2    C2_SZ3, SP_SZ($t5)
	swc2    C2_SXY0, 8($s6)              # xy0 (before the AVSZ3: no GTE read
	swc2    C2_SXY1, 20($s6)             # xy1  while it runs)
	swc2    C2_SXY2, 32($s6)             # xy2
	AVSZ3
	lw      $t4, 8($a3)                  # uv2 | pad (0, see qw_tri)
	sw      $s2, 12($s6)
	sw      $t2, 16($s6)
	sw      $t1, 24($s6)
	mfc2    $t3, C2_OTZ
	sw      $s1, 28($s6)
	andi    $t3, $t3, (R_SORT_N - 1) << 2
	addu    $t3, $t3, $gp                # the bucket's node
	lw      $t0, 0($t3)                  # its chain's head
	sw      $t4, 36($s6)
	lui     $t9, 0x0900
	or      $t9, $t9, $t0                # tag: 9 words, linked to that head
	sw      $t9, 0($s6)
	sw      $s6, 0($t3)                  # the chain's new head
	addiu   $s6, $s6, 40
ds_tri_next:
	lw      $t0, POLY_SIZE($a3)          # the next one's (after the last: harmless)
ds_tri_next_1:
	bgtz    $at, ds_tri
	addiu   $a3, $a3, POLY_SIZE
ds_next:
	lw      $a0, SP_NEXTREC($t8)
	b       ds_rec
	lhu     $t2, R_SIZE($a0)             # (ds_cull's second)

	# dm_sort's for a monster through a liquid's surface (RE_WATER): its word
	# in SP_WATER, for ds_records (ds_water) and the slow path (slow_wpoly)
	ICSEC   dm_wsort
dm_wsort:
	lw      $t9, RE_WATERZ($a0)
	nop
	b       dm_sort_1
	sw      $t9, SP_WATER($t8)

	# A monster through a liquid's surface (SP_WATER, t9: the surface in
	# the model's z, the halfword above 1 when the part above it shows, -1
	# when the part below does): each slot of the record on the other side
	# marked, its light word's top bit (the fast path's packets never see
	# it: a triangle with one goes to ds_tri_water); t0, at as ds_polys
	# loaded them
	ICSEC   ds_water
ds_water:
	lw      $t1, SP_LODCNT($t8)
	lw      $t3, SP_VERTPOOL($t8)
	sll     $t4, $t9, 16
	sra     $t4, $t4, 16                 # the surface
	sra     $t5, $t9, 31                 # 0: the part above shows, -1: below
	andi    $t1, $t1, 0xff               # the slots transformed
	addiu   $t6, $a0, R_VERTS            # their pool indices
	addiu   $t2, $t8, SP_COL             # their light words
	lui     $ra, 0x8000                  # the mark
	move    $s1, $zero                   # the slots marked
ds_w_slot:
	lhu     $t9, 0($t6)
	addiu   $t6, $t6, 2
	sll     $t9, $t9, 3
	addu    $t9, $t9, $t3
	lh      $t9, 4($t9)                  # its z
	addiu   $t1, $t1, -1
	subu    $t9, $t9, $t4                # over the surface
	xor     $t9, $t9, $t5
	subu    $t9, $t9, $t5                # on the side shown: >= 0
	bgez    $t9, 1f
	lw      $t9, 0($t2)
	addiu   $s1, $s1, 1
	or      $t9, $t9, $ra
	sw      $t9, 0($t2)
1:	bgtz    $t1, ds_w_slot
	addiu   $t2, $t2, 4
	beqz    $s1, ds_polys_1              # none marked: the loop as it was
	nop
	b       dw_polys_1                   # else the water loop
	nop

	# The water loop: a monster's record with corners past a liquid's
	# surface (ds_water marked them), ds_polys_1's copy with a front
	# triangle's marks tested (ds_tri_water) and no CAREFUL (its exits are
	# the other loop's): a front triangle of a record that may come near
	# or warp goes to the slow path (dw_tri_skip). The loop as it was pays
	# nothing for it
	ICSEC   dw_polys
dw_polys_1:
.if R_PROFILE
	lw      $t1, SP_POLYS($t8)
	addu    $a3, $a0, $t0                # the first triangle
	srl     $at, $at, 8
	addu    $t1, $t1, $at
	sw      $t1, SP_POLYS($t8)
.else
	addu    $a3, $a0, $t0                # the first triangle
	srl     $at, $at, 8
.endif
	lw      $t0, 0($a3)                  # (none: a harmless read)
	beqz    $at, dw_done
dw_tri:                                  # t0 = the triangle's 3 vertex offsets
	addiu   $at, $at, -1                 # (count down; the delay slot above)
	andi    $t3, $t0, 0xff
	srl     $t4, $t0, 8
	andi    $t4, $t4, 0xff
	srl     $t5, $t0, 16
	andi    $t5, $t5, 0xff
	addu    $t3, $t3, $t8
	addu    $t4, $t4, $t8
	addu    $t5, $t5, $t8
dw_tri_emit:                             # POLY_GT3 (t3..t5 kept for its bucket)
	lwc2    C2_SXY0, SP_SXY($t3)         # the corners' xy: into the GTE for the
	lwc2    C2_SXY1, SP_SXY($t4)         # NCLIP, from there to the packet (no
	lwc2    C2_SXY2, SP_SXY($t5)         # copy in the CPU's registers)
	lw      $t0, SP_COL($t3)             # c0 (two instructions before the NCLIP)
	lw      $t1, 4($a3)                  # uv0 | uv1 << 16
	NCLIP
	lw      $t2, SP_COL($t4)             # c1
	lw      $s1, SP_COL($t5)             # c2
	lui     $s2, CODE_GT3 >> 16
	or      $t9, $t0, $t2                # (a corner past the surface: its
	or      $t0, $t0, $s2                #  light word's top bit, ds_water)
	andi    $s2, $t1, 0xffff
	or      $t9, $t9, $s1
	or      $s2, $s2, $v0                # uv0 | clut
	srl     $t1, $t1, 16
	mfc2    $ra, C2_MAC0
	or      $t1, $t1, $v1                # uv1 | tpage
	bgez    $ra, dw_tri_next             # turned away: left out
	sw      $t0, 4($s6)                  # (a packet not linked: harmless)
	bltz    $t9, ds_tri_water            # a corner past the surface
	nop
	bnez    $a2, dw_tri_skip             # may come near or warp: the slow path
	lwc2    C2_SZ1, SP_SZ($t3)           # its bucket: AVSZ3, ZSF3 x the
	lwc2    C2_SZ2, SP_SZ($t4)           # corners' SZ summed >> 12, / 4
	lwc2    C2_SZ3, SP_SZ($t5)
	swc2    C2_SXY0, 8($s6)              # xy0 (before the AVSZ3: no GTE read
	swc2    C2_SXY1, 20($s6)             # xy1  while it runs)
	swc2    C2_SXY2, 32($s6)             # xy2
	AVSZ3
	lw      $t4, 8($a3)                  # uv2 | pad (0, see qw_tri)
	sw      $s2, 12($s6)
	sw      $t2, 16($s6)
	sw      $t1, 24($s6)
	mfc2    $t3, C2_OTZ
	sw      $s1, 28($s6)
	andi    $t3, $t3, (R_SORT_N - 1) << 2
	addu    $t3, $t3, $gp                # the bucket's node
	lw      $t0, 0($t3)                  # its chain's head
	sw      $t4, 36($s6)
	lui     $t9, 0x0900
	or      $t9, $t9, $t0                # tag: 9 words, linked to that head
	sw      $t9, 0($s6)
	sw      $s6, 0($t3)                  # the chain's new head
	addiu   $s6, $s6, 40
dw_tri_next:
	lw      $t0, POLY_SIZE($a3)          # the next one's (after the last: harmless)
dw_tri_next_1:
	bgtz    $at, dw_tri
	addiu   $a3, $a3, POLY_SIZE
dw_done:
	b       ds_next
	nop

	# a front triangle with a corner past the surface (t0: c0 with the
	# packet's code, t2 c1, s1 c2): all three: left out (none of it is
	# seen); else cut by the slow path (slow_poly's water clip)
	ICSEC   ds_tri_water
ds_tri_water:
	lw      $t0, SP_COL($t3)             # (c0 as cached)
	and     $t9, $t2, $s1
	and     $t9, $t9, $t0
	bltz    $t9, dw_tri_next
	nop
	b       dw_tri_skip
	nop

	# CAREFUL's exits, as POLYS's: a corner nearer than its bound (t1 =
	# the farthest corner's SZ): all behind the near plane, nothing to
	# draw; else the slow path
	ICSEC   ds_tri_near
ds_tri_near:
	lw      $t0, SP_ZNEAR($t8)
	lw      $v0, SP_CLUT16($t8)          # (CAREFUL used v0, v1)
	slt     $t0, $t1, $t0
	beqz    $t0, ds_tri_skip
	lw      $v1, SP_TPAGE16($t8)
	b       ds_tri_next
	nop
	# too big or warping: wholly off one side of the screen, left out;
	# else the slow path
	ICSEC   ds_tri_far
ds_tri_far:
	lw      $t0, SP_SXY($t3)
	lw      $t1, SP_SXY($t4)
	lw      $t2, SP_SXY($t5)
ds_ft_r:
	lui     $v0, 240
	and     $t9, $t0, $t1
	and     $t9, $t9, $t2
	bltz    $t9, ds_tri_off              # all y < 0
	andi    $t9, $t9, 0x8000
	bnez    $t9, ds_tri_off              # all x < 0
	subu    $t9, $t0, $v0
	subu    $v1, $t1, $v0
	or      $t9, $t9, $v1
	subu    $v1, $t2, $v0
	or      $t9, $t9, $v1
	bgez    $t9, ds_tri_off              # all y >= 240
ds_ft_c:
	lui     $v0, 320
	FARSITE ds_ft_r, ds_ft_c
	sll     $t0, $t0, 16
	sll     $t1, $t1, 16
	sll     $t2, $t2, 16
	subu    $t0, $t0, $v0
	subu    $t1, $t1, $v0
	or      $t0, $t0, $t1
	subu    $t2, $t2, $v0
	or      $t0, $t0, $t2
	bltz    $t0, ds_tri_skip             # not all x >= 320: the slow path
	nop
ds_tri_off:
	lw      $v0, SP_CLUT16($t8)
	b       ds_tri_next
	lw      $v1, SP_TPAGE16($t8)

	# the slow path (engine/r_slow.c: the near clip, the subdivision), its
	# pieces into the triangle's bucket: fp, the link it prepends to, is
	# that chain's head before and after (s1 keeps the node: the slow path
	# keeps s0-s7)
	ICSEC   ds_tri_skip
ds_tri_skip:
	lwc2    C2_SZ1, SP_SZ($t3)           # (t3..t5: the corners, CAREFUL's)
	lwc2    C2_SZ2, SP_SZ($t4)
	lwc2    C2_SZ3, SP_SZ($t5)
	sw      $at, SP_SAVE+0($t8)
	sw      $a0, SP_SAVE+4($t8)
	AVSZ3
	sw      $a1, SP_SAVE+8($t8)
	sw      $a2, SP_SAVE+12($t8)
	sw      $a3, SP_SAVE+16($t8)
	mfc2    $s1, C2_OTZ
	sw      $s6, SP_SAVE+20($t8)         # (the slow path's bytes, below)
.if R_PROFILE
	lw      $t1, SP_SKIPPED($t8)         # polygons sent to the slow path
	lhu     $t2, TIMER2($t8)
	addiu   $t1, $t1, 1
	sw      $t1, SP_SKIPPED($t8)
	sw      $t2, SP_SAVE+36($t8)         # slow-path start time
.endif
	andi    $s1, $s1, (R_SORT_N - 1) << 2
	addu    $s1, $s1, $gp                # its bucket's node
	lw      $fp, 0($s1)
.if R_SLOW_C
	lw      $t9, SP_RSTATE($t8)          # (s0 holds the plane table)
	move    $a1, $a0                     # record
	sw      $s6, RS_PKT($t9)             # the C side emits through r_state
	sw      $fp, RS_OT_HEAD($t9)
	move    $a2, $a3                     # polygon
	li      $a3, 3
	move    $a0, $t9                     # r_state
	addiu   $sp, $sp, -32                # o32 argument area, gp (the table)
	sw      $gp, 16($sp)                 # and the real one
	lui     $gp, %hi(r_gp_saved)
	jal     r_slow
	lw      $gp, %lo(r_gp_saved)($gp)
	lw      $gp, 16($sp)
	addiu   $sp, $sp, 32
	lui     $t8, 0x1f80
	lw      $t9, SP_RSTATE($t8)
	nop
	lw      $fp, RS_OT_HEAD($t9)
	lw      $s6, RS_PKT($t9)
.else
	move    $a1, $a3                     # polygon
	jal     slow_poly                    # a0 = record, a1 = polygon, a2 = 3
	li      $a2, 3
.endif
	sw      $fp, 0($s1)                  # the chain's head after its pieces
.if R_PROFILE
	lhu     $t0, TIMER2($t8)
	lw      $t1, SP_SAVE+36($t8)
	lw      $t2, SP_TSLOW($t8)
	subu    $t0, $t0, $t1
	andi    $t0, $t0, 0xffff
	addu    $t2, $t2, $t0
	sw      $t2, SP_TSLOW($t8)
.endif
	lw      $t1, SP_SAVE+20($t8)         # bytes it wrote, for the warp
	lw      $t2, SP_SLOWB($t8)           # control's triangle count
	lw      $at, SP_SAVE+0($t8)
	subu    $t1, $s6, $t1
	addu    $t2, $t2, $t1
	sw      $t2, SP_SLOWB($t8)
	lw      $a0, SP_SAVE+4($t8)
	lw      $a1, SP_SAVE+8($t8)
	lw      $a2, SP_SAVE+12($t8)
	lw      $a3, SP_SAVE+16($t8)
	lw      $v0, SP_CLUT16($t8)          # (the careful test used them)
	lw      $v1, SP_TPAGE16($t8)
	b       ds_tri_next_1
	lw      $t0, POLY_SIZE($a3)          # (ds_tri_next's first)

	# the water loop's slow path: ds_tri_skip's copy through slow_wpoly
	# (the corners' marks), back to the water loop
	ICSEC   dw_tri_skip
dw_tri_skip:
	lwc2    C2_SZ1, SP_SZ($t3)           # (t3..t5: the corners, CAREFUL's)
	lwc2    C2_SZ2, SP_SZ($t4)
	lwc2    C2_SZ3, SP_SZ($t5)
	sw      $at, SP_SAVE+0($t8)
	sw      $a0, SP_SAVE+4($t8)
	AVSZ3
	sw      $a1, SP_SAVE+8($t8)
	sw      $a2, SP_SAVE+12($t8)
	sw      $a3, SP_SAVE+16($t8)
	mfc2    $s1, C2_OTZ
	sw      $s6, SP_SAVE+20($t8)         # (the slow path's bytes, below)
.if R_PROFILE
	lw      $t1, SP_SKIPPED($t8)         # polygons sent to the slow path
	lhu     $t2, TIMER2($t8)
	addiu   $t1, $t1, 1
	sw      $t1, SP_SKIPPED($t8)
	sw      $t2, SP_SAVE+36($t8)         # slow-path start time
.endif
	andi    $s1, $s1, (R_SORT_N - 1) << 2
	addu    $s1, $s1, $gp                # its bucket's node
	lw      $fp, 0($s1)
.if R_SLOW_C
	lw      $t9, SP_RSTATE($t8)          # (s0 holds the plane table)
	move    $a1, $a0                     # record
	sw      $s6, RS_PKT($t9)             # the C side emits through r_state
	sw      $fp, RS_OT_HEAD($t9)
	move    $a2, $a3                     # polygon
	li      $a3, 3
	move    $a0, $t9                     # r_state
	addiu   $sp, $sp, -32                # o32 argument area, gp (the table)
	sw      $gp, 16($sp)                 # and the real one
	lui     $gp, %hi(r_gp_saved)
	jal     r_slow
	lw      $gp, %lo(r_gp_saved)($gp)
	lw      $gp, 16($sp)
	addiu   $sp, $sp, 32
	lui     $t8, 0x1f80
	lw      $t9, SP_RSTATE($t8)
	nop
	lw      $fp, RS_OT_HEAD($t9)
	lw      $s6, RS_PKT($t9)
.else
	move    $a1, $a3                     # polygon
	jal     slow_wpoly                   # a0 = record, a1 = polygon, a2 = 3
	li      $a2, 3
.endif
	sw      $fp, 0($s1)                  # the chain's head after its pieces
.if R_PROFILE
	lhu     $t0, TIMER2($t8)
	lw      $t1, SP_SAVE+36($t8)
	lw      $t2, SP_TSLOW($t8)
	subu    $t0, $t0, $t1
	andi    $t0, $t0, 0xffff
	addu    $t2, $t2, $t0
	sw      $t2, SP_TSLOW($t8)
.endif
	lw      $t1, SP_SAVE+20($t8)         # bytes it wrote, for the warp
	lw      $t2, SP_SLOWB($t8)           # control's triangle count
	lw      $at, SP_SAVE+0($t8)
	subu    $t1, $s6, $t1
	addu    $t2, $t2, $t1
	sw      $t2, SP_SLOWB($t8)
	lw      $a0, SP_SAVE+4($t8)
	lw      $a1, SP_SAVE+8($t8)
	lw      $a2, SP_SAVE+12($t8)
	lw      $a3, SP_SAVE+16($t8)
	lw      $v0, SP_CLUT16($t8)          # (the careful test used them)
	lw      $v1, SP_TPAGE16($t8)
	b       dw_tri_next_1
	lw      $t0, POLY_SIZE($a3)          # (dw_tri_next's first)

	# a record reaching into the fog: each light times its vertex's
	# factor (XFORM fog, t2 = r_fogtab), then on as the others
	ICSEC   ds_fog
ds_fog:
	lui     $t2, %hi(r_fogtab)
	addiu   $t2, $t2, %lo(r_fogtab)
	XFORM   1, ds_xfg, ds_oneg, ds_xfg_done, 3, 1
ds_xfg_done:
	lw      $t2, SP_LODCNT($t8)          # (the band's slot count and SZ_FAST back:
	lw      $t7, SP_SZFAST($t8)          #  the table and the fog's temporary
	b       ds_xf_done                   #  had their registers)
	andi    $t2, $t2, 0xff

	ICSEC   ds_full
ds_full:
	li      $t0, 1
.if R_PROFILE
	sw      $t0, SP_FULL($t8)
	lw      $t0, SP_SKIPPED($t8)
	nop
	addiu   $t0, $t0, 1
	sw      $t0, SP_SKIPPED($t8)
ds_done:
	lhu     $t0, TIMER2($t8)
	lw      $t9, SP_T0($t8)
	lw      $t1, SP_TBLOCK($t8)
	subu    $t0, $t0, $t9
	andi    $t0, $t0, 0xffff
	addu    $t1, $t1, $t0
	sw      $t1, SP_TBLOCK($t8)
.else
	sw      $t0, SP_FULL($t8)
.endif
ds_exit:
	b       dm_exit_1                    # (its window command, drawn first)
	lw      $fp, SP_SORTHEAD($t8)        # the model's chain: its farthest bucket on

# -----------------------------------------------------------------------------
# draw_bmodel: a0 = an r_ent_t with RE_BMODEL: a brush model, from
# draw_model once the model's GTE matrix, pool, texture table and face
# stamp are set and the texture run closed. Its hull-0 subtree (RE_RECS =
# the root) is walked like the world's BSP, front to back from the
# model-space camera (RE_CAM, put in SP_CAM and the colour matrix's first
# row for the plane tests; the world's kept in bm_area): each node's face
# block on the camera's side goes through the record loop (RECLOOP de,
# entered at de_records with the world walk's entry state; dm_exit comes
# back to bm_block), after the frustum test on the node's box (the model's
# matrix: the view's rotation, translated); no PVS (its leaves are not the
# world's: nothing to do at one). The faces keep their cooked light. Its
# own stack in RAM (bm_area, QW_MAX_BM_DEPTH entries: the cook bounds the
# depth), the world walk's s3 kept around it; s1, s2 are the walk's dead
# ones at a leaf, gp a block's (set again before each world block).
	ICSEC   draw_bmodel
draw_bmodel:
	lui     $gp, %hi(bm_area)
	addiu   $gp, $gp, %lo(bm_area)
	sw      $s3, BM_S3($gp)
	lw      $t0, SP_CAM+0($t8)           # the world's camera, kept
	lw      $t1, SP_CAM+4($t8)
	lw      $t2, SP_CAM+8($t8)
	sw      $t0, BM_CAM+0($gp)
	sw      $t1, BM_CAM+4($gp)
	sw      $t2, BM_CAM+8($gp)
	lw      $t0, RE_CAM+0($a0)           # the model-space camera: SP_CAM
	lw      $t1, RE_CAM+4($a0)           # (axial planes) and the colour
	lw      $t2, RE_CAM+8($a0)           # matrix's first row (general ones)
	sw      $t0, SP_CAM+0($t8)
	sw      $t1, SP_CAM+4($t8)
	sw      $t2, SP_CAM+8($t8)
	andi    $t0, $t0, 0xffff
	sll     $t1, $t1, 16
	or      $t0, $t0, $t1
	ctc2    $t0, C2_LR1LR2
	ctc2    $t2, C2_LR3LG1
	lw      $s1, RE_RECS($a0)            # the root (KUSEG; bit 0: a leaf)
	li      $s2, 0xf                     # all four frustum planes active
	addiu   $s3, $gp, BM_STACK + 4       # the model's stack
	la      $t0, bm_block
	sw      $t0, SP_BMWALK($t8)
bm_walk:
	andi    $t0, $s1, 1
bm_walk_t0:                              # (t0 = s1 & 1: a leaf?)
	bnez    $t0, bm_pop                  # a leaf: nothing to draw there
	nop
	lh      $t1, N_C+4($s1)              # the frustum test's box (as the
	lwc2    C2_VXY0, N_C+0($s1)          # world walk loads it)
	lh      $t2, N_H+4($s1)
	lwc2    C2_VXY1, N_H+0($s1)
	mtc2    $t1, C2_VZ0
	mtc2    $t2, C2_VZ1
	beqz    $s2, bm_side_1               # fully inside the frustum already
	lhu     $a2, N_PLANE3($s1)
	FRUSTUM bm_pop, "sll $a2, $a2, 2", "addu $a2, $a2, $s0", "lw $a3, PN_DIST($a2)", "lbu $a0, PN_TYPE($a2)", "sra $a3, $a3, 1"
	b       bm_side_2
	addiu   $s3, $s3, 4
	ICSEC   bm_side_1
bm_side_1:                               # the node's plane: d = n . cam - dist
	addiu   $s3, $s3, 4                  # (x4096, vertex units)
	sll     $a2, $a2, 2
	addu    $a2, $a2, $s0
	lw      $a3, PN_DIST($a2)
	lbu     $a0, PN_TYPE($a2)
	sra     $a3, $a3, 1
bm_side_2:
	sltiu   $t2, $a0, 3
	beqz    $t2, bm_general
	sll     $t0, $a0, 2
	addu    $t0, $t0, $t8
	lw      $v0, SP_CAM($t0)             # axial: cam[type] x4096
	sll     $t4, $s2, 28
	sll     $v0, $v0, 12
	or      $t4, $t4, $s1
	subu    $v0, $v0, $a3
bm_push:                                 # the stack word: as the world's
	slt     $t0, $v0, $zero              # (node | side x 4 << 24 | no faces
	addiu   $t2, $v0, BACKFACE_EPS       # << 27 | clip flags << 28)
	sltiu   $t2, $t2, 2 * BACKFACE_EPS + 1
	sll     $t2, $t2, 1
	or      $t2, $t2, $t0
	sll     $t2, $t2, 26
	or      $t3, $t2, $t4
	sll     $t0, $t0, 2
	addu    $t0, $t0, $s1
	lw      $s1, N_CHILD($t0)            # near child first
	b       bm_walk
	sw      $t3, -4($s3)                 # push
	ICSEC   bm_general
bm_general:
	lwc2    C2_VXY0, PN_N+0($a2)
	lh      $t2, PN_N+4($a2)
	sll     $t4, $s2, 28
	mtc2    $t2, C2_VZ0
	or      $t4, $t4, $s1
	nop                                  # (the GTE's two slots after mtc2)
	MVMVA   0, 2, 0, 3, 0                # MAC1 = cam . n
	mfc2    $v0, C2_MAC1                 # (waits for the GTE)
	b       bm_push
	subu    $v0, $v0, $a3
	ICSEC   bm_pop
bm_pop:
	addiu   $s3, $s3, -4
	addiu   $t1, $gp, BM_STACK
	lw      $t0, 0($s3)                  # (empty: the word at BM_STACK)
	beq     $s3, $t1, bm_done
	srl     $s2, $t0, 28                 # clip flags
	srl     $t3, $t0, 24
	andi    $t4, $t3, 8                  # no faces
	andi    $t3, $t3, 4                  # side * 4
	sll     $t2, $t0, 8
	srl     $t2, $t2, 8                  # node (KUSEG)
	subu    $t5, $t2, $t3
	lw      $s1, N_CHILD+4($t5)          # far child: walked after the faces
	bnez    $t4, bm_walk
	addu    $t5, $t2, $t3
	lw      $a0, N_FACES($t5)            # block on the camera's side
	sll     $t6, $t3, 3
	addu    $t6, $t6, $t2
	lw      $a1, N_FACES+4($t6)
.if R_PROFILE
	lhu     $t9, TIMER2($t8)
	beq     $a0, $a1, bm_walk            # empty block
	lw      $t0, SP_FULL($t8)
.else
	lw      $t0, SP_FULL($t8)
	beq     $a0, $a1, bm_walk
	lw      $v0, SP_CLUT16($t8)
.endif
	b       de_records                   # the record loop; dm_exit -> bm_block
	nop
	ICSEC   bm_block
bm_block:                                # a block drawn (or the arena full)
	lw      $t0, SP_FULL($t8)
	nop
	beqz    $t0, bm_walk
	nop
bm_done:
	sw      $zero, SP_BMWALK($t8)
	lw      $s3, BM_S3($gp)              # the world walk's stack pointer
	lw      $t0, BM_CAM+0($gp)           # and camera
	lw      $t1, BM_CAM+4($gp)
	lw      $t2, BM_CAM+8($gp)
	sw      $t0, SP_CAM+0($t8)
	sw      $t1, SP_CAM+4($t8)
	sw      $t2, SP_CAM+8($t8)
	andi    $t0, $t0, 0xffff
	sll     $t1, $t1, 16
	or      $t0, $t0, $t1
	ctc2    $t0, C2_LR1LR2
	b       dm_exit_1
	ctc2    $t2, C2_LR3LG1

# -----------------------------------------------------------------------------
# draw_ents: the render list of the leaf the walk just visited (t9 = its
# vis word: the list id | 0x80 in the top byte): particles as one TILE_1
# each, projected with the view matrix, models through draw_model.
# Prepended here, between the faces behind the leaf and those in front of
# it. Back to pop_1 (s3 already moved).
	ICSEC   draw_ents
draw_ents:
	lw      $t0, SP_RSTATE($t8)
	srl     $t9, $t9, 23                 # the id x 2 (bit 8: the flag, dropped)
	lw      $t1, RS_LISTS($t0)
	andi    $t9, $t9, 0xfe
	addu    $t1, $t1, $t9
	lhu     $t9, 0($t1)                  # the first item + 1
	nop
ei_item:
	.globl  ei_x                         # (the cooked walk's leaves come back to seq_next:
ei_x:                                    #  render.c r_seq_set points this branch there)
	beqz    $t9, pop_1
	lw      $t0, SP_STACK($s3)           # (pop_1's first)
	lw      $t0, SP_RSTATE($t8)
	addiu   $t9, $t9, -1
	sll     $t9, $t9, 4
	lw      $t1, RS_ITEMS($t0)
	lw      $t2, RS_RGB($t0)
	addu    $t9, $t9, $t1                # the item
	lb      $t3, RI_TYPE($t9)
	lhu     $t1, RI_NEXT($t9)
	lw      $a0, RI_ENT($t9)
	sw      $t1, SP_NEXTITEM($t8)
	bnez    $t3, ei_model
	lwc2    C2_VXY0, RI_X($t9)
	lh      $t3, RI_Z($t9)               # a particle
	lbu     $t4, RI_COLOUR($t9)
	mtc2    $t3, C2_VZ0
	sll     $t4, $t4, 2
	addu    $t4, $t4, $t2
	RTPS
	lw      $t4, 0($t4)                  # its colour word
	sltu    $t0, $s7, $s6
	bnez    $t0, ei_next                 # (the arena is full)
	lui     $t6, 0x6800                  # TILE_1
	mfc2    $t3, C2_SZ3
	or      $t4, $t4, $t6
	sltiu   $t3, $t3, 96                 # too near or behind: none
	bnez    $t3, ei_next
	mfc2    $t5, C2_SXY2
	lui     $t6, 0x0200                  # tag: 2 words
	or      $t6, $t6, $fp
	sw      $t6, 0($s6)
	sw      $t4, 4($s6)
	sw      $t5, 8($s6)
	move    $fp, $s6
	addiu   $s6, $s6, 12
ei_next:
	lhu     $t9, SP_NEXTITEM($t8)
	b       ei_item
	nop
	ICSEC   ei_model
ei_model:
	bltz    $t3, ei_pieces               # (RI_PIECES: a negative byte)
	lui     $ra, %hi(ei_next)            # a model: draw_model's return
	addiu   $ra, $ra, %lo(ei_next)       # straight to the next item
	j       dm_body
	sw      $ra, SP_DMRA($t8)            # (draw_model's first)

# A brush model's pieces in this leaf (bmfrag.c, rl_add_pieces): records in
# the world's space and its texture table, with their own vertex pool,
# through the models' record loop as one of a brush model's blocks is:
# no matrix to load or put back (the walk's is the world's), the texture
# run going on; the models' stamp for their face ids, the world's pool
# and stamp again after them (ep_back, from dm_exit).
	ICSEC   ei_pieces
ei_pieces:                               # a0 = the first record (RI_ENT)
	lw      $t0, SP_RSTATE($t8)
	lhu     $t1, RI_LEN($t9)
	lw      $t2, RS_FRAGPOOL($t0)
	addiu   $t3, $t8, SP_STAMP
	lw      $t0, SP_FULL($t8)
	addu    $a1, $a0, $t1                # the end
	sw      $t2, SP_VERTPOOL($t8)
	sw      $t3, SP_FVBASE($t8)
	la      $t3, ep_back
	lw      $v0, SP_CLUT16($t8)          # the run's CLUT back in v0 (the walk's
.if R_PROFILE                            #  plane tests took it: a first piece of the
	lhu     $t9, TIMER2($t8)             #  run's texture would carry the camera's
.endif                                   #  distance as its CLUT, see-through or noise)
	b       de_records
	sw      $t3, SP_BMWALK($t8)
	ICSEC   ep_back
ep_back:                                 # (ei_next's own work here)
	lw      $t0, SP_RSTATE($t8)
	sw      $zero, SP_BMWALK($t8)
	lw      $t1, RS_VERTPOOL($t0)
	lw      $t2, RS_FVBASE($t0)
	sw      $t1, SP_VERTPOOL($t8)
	lhu     $t9, SP_NEXTITEM($t8)
	b       ei_item
	sw      $t2, SP_FVBASE($t8)

# -----------------------------------------------------------------------------
# The cooked walk (formats/qworld.h "the cooked walk", docs/porting-notes.md
# §91). While the camera's leaf is one of the current zone's and its chunk
# carries one, r_state.seq points at the zone's sequence (zone.c), and
# this runs it in place of the tree walk above: the tree's steps for a
# camera in the zone, flat, the cook having taken the camera's side of
# every plane that leaves the zone's leaves on one side. Each word, by its
# type (bits 30-31; an offset is the node's or the leaf's from the leaves'
# array, 24 bits signed, in s0):
#   LEAF   the leaf: in the PVS (its stamp), its box against the frustum
#          when the cook asked (bit 29), its marks stamped, its render list
#          drawn (draw_ents, which comes back to seq_next: render.c points
#          ei_item's branch here, and the record loops' exits, r_seq_set)
#   BLOCK  the node's face block on the side of bit 29, through the record
#          loop as the tree's pop sends it (gp = the node when a dynamic
#          light reaches it)
#   NODE   a subtree's root, the next word its words: out of the PVS, the
#          subtree skipped; a tested one (bit 29) keeps a frame (the clip
#          flags before its box's test, back at its END), its subtree run
#          with the flags the test leaves, or skipped when the box is out
#   VAR    a node whose plane crosses the zone, the next word its two
#          subtrees' words (F's, then B's << 16): the camera's side as the
#          walk takes it (d < 0: behind; |d| <= eps: no faces), written
#          into the word's bits 26-27 for the subtrees' ENDs, which name
#          their VAR (their way back, in words): F's END draws the front
#          block when in front (B follows) or jumps past B when behind;
#          B's END draws the back block and jumps to F when behind
#   ENDN   a tested node's end (its flags back);  STOP  the walk's: walk_done
# A tested node's flags go on the walk's scratchpad stack (s3; the cook
# bounds their nesting to QW_SEQ_MAX_DEPTH). The walk's registers: s1 the sequence,
# s2 the clip flags, s4 / s5 the stamps, s6 / s7 / fp the packets, t8 the
# scratchpad, v1 the texture cache (never touched), gp a frame being made
# or a block's lit node.
	ICSEC   seq_start
seq_start:                               # t0 = the sequence (r_state.seq)
	lw      $t1, SP_RSTATE($t8)
	sll     $s1, $t0, 8
	srl     $s1, $s1, 8                  # KUSEG: a frame holds it in 24 bits
	lw      $s0, RS_LEAVES($t1)          # the offsets' base
	.globl  seq_next                     # (render.c: the record loops' and draw_ents' way here)
seq_next:
	lw      $t1, 0($s1)                  # the entry
.if R_PROFILE
	lui     $t4, %hi(r_seq_stat)
	lw      $t3, %lo(r_seq_stat + 12)($t4)   # (the entries dispatched)
	addiu   $s1, $s1, 4
	sll     $t2, $t1, 8
	addiu   $t3, $t3, 1
	sw      $t3, %lo(r_seq_stat + 12)($t4)
.else
	addiu   $s1, $s1, 4
	sll     $t2, $t1, 8
.endif
	bltz    $t1, seq_hi                  # NODE, VAR, END, STOP
	sra     $t2, $t2, 8                  # its offset, signed
	sll     $t0, $t1, 1
	bltz    $t0, seq_block               # BLOCK
	addu    $a0, $s0, $t2                # LEAF: a0 = the leaf
	lhu     $t0, L_VISFRAME($a0)
	lhu     $a3, L_NMARKS($a0)
	bne     $t0, $s5, seq_next           # not in the PVS: on
	lw      $a2, L_MARKS($a0)
	sll     $t3, $t1, 2                  # (its test bit to the sign)
	bgez    $t3, sl_marks                # no box test asked (the fast walk)
	sll     $a3, $a3, 1                  # marks x 2 bytes
	beqz    $s2, sl_marks                # fully inside the frustum: no box
	move    $gp, $s2                     # (the flags back after: a leaf's test is its own)
.if R_PROFILE
	lui     $t4, %hi(r_seq_stat)
	lw      $t3, %lo(r_seq_stat)($t4)
.endif
	lh      $t1, L_C+4($a0)              # the box, as walk_leaf loads it
	lwc2    C2_VXY0, L_C+0($a0)
	lh      $t2, L_H+4($a0)
	lwc2    C2_VXY1, L_H+0($a0)
	mtc2    $t1, C2_VZ0
	mtc2    $t2, C2_VZ1
.if R_PROFILE
	addiu   $t3, $t3, 1
	sw      $t3, %lo(r_seq_stat)($t4)
.else
	nop                                  # (two before the GTE reads them)
	nop
.endif
	FRUSTUM sl_cull
	move    $s2, $gp
sl_marks:
	lw      $t3, SP_FVBASE($t8)
	lw      $t9, L_VIS($a0)              # (the leaf's render list: its top byte)
	beqz    $a3, sl_ents
	addu    $a3, $a3, $a2                # the marks' end
	lhu     $t0, 0($a2)
sl_mark:
	addiu   $a2, $a2, 2
	addu    $t0, $t0, $t3
	sh      $s4, 0($t0)                  # face_visframe[mark] = framecount
	bne     $a2, $a3, sl_mark
	lhu     $t0, 0($a2)                  # (next; one past the last: harmless)
sl_ents:
	bgez    $t9, seq_next                # nothing in the leaf
	lui     $t0, %hi(seq_keep)
	sw      $s1, %lo(seq_keep)($t0)      # models / particles / pieces here: draw_ents (a
	sw      $s2, %lo(seq_keep + 4)($t0)  # sorted model takes s1, s2, a brush model the
	sw      $s0, %lo(seq_keep + 8)($t0)  # plane table in s0, the walk's dead ones at a
	j       draw_ents                    # leaf); back at seq_ents_back (render.c's patch)
	lw      $s0, SP_PLANES($t8)
seq_block:                               # t1 = the word (bit 29: the side), t2 = the node's offset
	addu    $t2, $s0, $t2                # the node
	srl     $t3, $t1, 27
	andi    $t3, $t3, 4                  # the side x 4
seq_block_n:                             # (t2 = the node, t3 = the side x 4: the VAR ends too)
	lhu     $t7, N_DLSTAMP($t2)          # (pop's block: a dynamic light's node in gp)
	addu    $t5, $t2, $t3
	lw      $a0, N_FACES($t5)            # the block on the camera's side
	sll     $t6, $t3, 3                  # side x 32: its end, faces[1] or the next node's faces[0]
	addu    $t6, $t6, $t2
	lw      $a1, N_FACES+4($t6)
.if R_PROFILE
	lhu     $t9, TIMER2($t8)
	lw      $t0, SP_FULL($t8)
.else
	lw      $t0, SP_FULL($t8)
	lw      $v0, SP_CLUT16($t8)
.endif
	bne     $t7, $s4, 1f
	move    $gp, $zero
	move    $gp, $t2
1:	j       dr_records                   # (its first word: the options' copy; out at seq_next)
	nop

	ICSEC   sl_cull
sl_cull:                                 # the leaf's box out of the frustum: the flags back
	b       seq_next
	move    $s2, $gp

	ICSEC   seq_ents_back
	.globl  seq_ents_back
seq_ents_back:                           # a leaf's render list drawn (ei_item's branch, patched)
	lui     $t0, %hi(seq_keep)
	lw      $s1, %lo(seq_keep)($t0)
	lw      $s2, %lo(seq_keep + 4)($t0)
	lw      $s0, %lo(seq_keep + 8)($t0)
	b       seq_next
	nop

	ICSEC   seq_hi
seq_hi:                                  # t1 = the word, t2 = its offset
	sll     $t0, $t1, 1
	bgez    $t0, seq_node                # NODE
	addu    $t2, $s0, $t2                # (the node)
	sll     $t0, $t1, 2                  # the others: bits 28-29
	bgez    $t0, 1f                      # bit 29 clear: VAR (00), ENDF (01)
	sll     $t3, $t1, 3                  # (bit 28 to the sign)
	bltz    $t3, seq_endn                # 11: a tested node's END, or STOP
	nop
	b       seq_endb                     # 10: B's END
	nop
1:	bltz    $t3, seq_endf                # 01: F's END
	lhu     $a2, N_PLANE3($t2)           # VAR: its plane (a harmless read for an END)
seq_var:                                 # t1 = the VAR word, t2 = the node; s1 -> the lengths' word
	lhu     $t0, N_VISFRAME($t2)         # out of the PVS: F and B skipped at once
	lw      $a3, 0($s1)                  # the lengths: F's words | B's << 16
	lw      $a1, SP_PLANES($t8)
	bne     $t0, $s5, sv_skip
	sll     $a2, $a2, 2
	addu    $a2, $a2, $a1                # the plane
	lw      $a1, PN_DIST($a2)
	lbu     $a0, PN_TYPE($a2)
	addiu   $t5, $s1, -4                 # the VAR word: its state bits (26: behind, 27:
	lui     $t6, 0xf3ff                  #  no faces) rewritten below, its ENDs read them
	ori     $t6, $t6, 0xffff
	sra     $a1, $a1, 1                  # dist -> vertex units, x4096
	sltiu   $t0, $a0, 3
	beqz    $t0, sv_general
	sll     $t0, $a0, 2
	addu    $t0, $t0, $t8
	lw      $v0, SP_CAM($t0)             # axial: n . cam = cam[type] x4096
	addiu   $s1, $s1, 4                  # F follows the lengths
.if R_PROFILE
	lui     $t4, %hi(r_seq_stat)
	lw      $t3, %lo(r_seq_stat + 8)($t4)
	sll     $v0, $v0, 12
	addiu   $t3, $t3, 1
	sw      $t3, %lo(r_seq_stat + 8)($t4)
.else
	sll     $v0, $v0, 12
.endif
sv_side:                                 # v0 = n . cam, a1 = dist, a3 = the lengths, s1 = F,
	subu    $v0, $v0, $a1                # d (x4096, vertex units)   t5 = the VAR word, t6 its mask
	addiu   $t2, $v0, BACKFACE_EPS       # no faces if |d| <= eps:
	sltiu   $t2, $t2, 2 * BACKFACE_EPS + 1   # (unsigned) d + eps <= 2 eps
	srl     $t0, $v0, 31                 # behind when d < 0
	sll     $t2, $t2, 27
	sll     $t0, $t0, 26
	or      $t2, $t2, $t0                # the state
	and     $t1, $t1, $t6
	or      $t1, $t1, $t2
	bltz    $v0, sv_back                 # behind the plane: B first
	sw      $t1, 0($t5)
	b       seq_next                     # in front: F follows
	nop
sv_back:
	andi    $a3, $a3, 0xffff             # F's words: B after them
	sll     $a3, $a3, 2
	b       seq_next
	addu    $s1, $s1, $a3

	ICSEC   sv_skip
sv_skip:                                 # the VAR node out of the PVS: past the lengths, F and B
	srl     $t0, $a3, 16
	andi    $a3, $a3, 0xffff
	addu    $t0, $t0, $a3
	sll     $t0, $t0, 2
	addiu   $s1, $s1, 4
	b       seq_next
	addu    $s1, $s1, $t0

	ICSEC   sv_general
sv_general:                              # a general plane: n . cam by the GTE (the colour
	lwc2    C2_VXY0, PN_N+0($a2)         # matrix's first row is the camera: r_render)
	lh      $t2, PN_N+4($a2)
	addiu   $s1, $s1, 4                  # F follows the lengths
	mtc2    $t2, C2_VZ0
.if R_PROFILE
	lui     $t4, %hi(r_seq_stat)
	lw      $t3, %lo(r_seq_stat + 8)($t4)
	MVMVA   0, 2, 0, 3, 0                # MAC1 = cam . n
	addiu   $t3, $t3, 1
	sw      $t3, %lo(r_seq_stat + 8)($t4)
.else
	nop
	nop
	MVMVA   0, 2, 0, 3, 0                # MAC1 = cam . n
.endif
	mfc2    $v0, C2_MAC1                 # (waits for the GTE)
	b       sv_side
	nop

	ICSEC   seq_node
seq_node:                                # t1 = the word (bit 29: tested), t2 = the node
	lw      $a2, 0($s1)                  # its subtree's words
	lhu     $t0, N_VISFRAME($t2)
	addiu   $s1, $s1, 4
	sll     $a2, $a2, 2
	bne     $t0, $s5, sn_skip            # out of the PVS: the subtree too
	sll     $t3, $t1, 2                  # (the test bit to the sign)
	bgez    $t3, seq_next                # a PVS gate alone: on
	sll     $gp, $s2, 28                 # (a tested node's frame: the flags, kind 0)
	beqz    $s2, sn_push                 # fully inside already: nothing to test
	lh      $t1, N_C+4($t2)              # the box (a harmless load when inside)
.if R_PROFILE
	lui     $t4, %hi(r_seq_stat)
	lw      $t3, %lo(r_seq_stat + 4)($t4)
.endif
	lwc2    C2_VXY0, N_C+0($t2)
	lh      $t5, N_H+4($t2)
	lwc2    C2_VXY1, N_H+0($t2)
	mtc2    $t1, C2_VZ0
	mtc2    $t5, C2_VZ1
.if R_PROFILE
	addiu   $t3, $t3, 1
	sw      $t3, %lo(r_seq_stat + 4)($t4)
.else
	nop                                  # (two before the GTE reads them)
	nop
.endif
	FRUSTUM sn_cull
sn_push:
	sw      $gp, SP_STACK($s3)
	b       seq_next
	addiu   $s3, $s3, 4

	ICSEC   sn_cull
sn_cull:                                 # out of the frustum: the flags back (the test cleared
	srl     $s2, $gp, 28                 #  some), the subtree skipped
sn_skip:
	b       seq_next
	addu    $s1, $s1, $a2

	ICSEC   seq_endf
seq_endf:                                # F's end: t1 = the word, its way back to the VAR word
	sll     $t4, $t1, 8
	srl     $t4, $t4, 6                  # the way back x 4
	subu    $t2, $s1, $t4
	addiu   $t2, $t2, -4                 # the VAR word (s1 is past this END)
	lw      $t3, 0($t2)                  # its state: behind (bit 26), no faces (27)
	lw      $a3, 4($t2)                  # the lengths
	sll     $t0, $t3, 5
	bltz    $t0, ef_back                 # behind: F was last, B done before it: past B
	sll     $t4, $t3, 4
	bltz    $t4, seq_next                # in front, no faces: on to B, which follows
	sll     $t3, $t3, 8
	sra     $t3, $t3, 8                  # the node's offset
	addu    $t2, $s0, $t3
	b       seq_block_n                  # the front block, then B
	move    $t3, $zero
ef_back:
	srl     $a3, $a3, 16                 # B's words
	sll     $a3, $a3, 2
	b       seq_next
	addu    $s1, $s1, $a3

	ICSEC   seq_endb
seq_endb:                                # B's end
	sll     $t4, $t1, 8
	srl     $t4, $t4, 6
	subu    $t2, $s1, $t4
	addiu   $t2, $t2, -4                 # the VAR word
	lw      $t3, 0($t2)
	nop
	sll     $t0, $t3, 5
	bgez    $t0, seq_next                # in front: B was last: on
	sll     $t4, $t3, 4
	addiu   $s1, $t2, 8                  # behind: F next (after the lengths)
	bltz    $t4, seq_next                # no faces
	sll     $t3, $t3, 8
	sra     $t3, $t3, 8
	addu    $t2, $s0, $t3
	b       seq_block_n                  # the back block, then F
	li      $t3, 4

	ICSEC   seq_endn
seq_endn:                                # a tested node's end: its flags back; or STOP (bit 0)
	andi    $t0, $t1, 1
	bnez    $t0, seq_stop
	lw      $gp, SP_STACK-4($s3)
	addiu   $s3, $s3, -4
	b       seq_next
	srl     $s2, $gp, 28

	ICSEC   seq_stop
seq_stop:
	j       walk_done
	nop

	.section .bss.r_seq_stat, "aw", @nobits
	.align  2
	.global r_seq_stat
r_seq_stat:                              # the frame's: leaf tests, node tests, side tests (R_PROFILE)
	.space  16
seq_keep:                                # s1, s2, s0 across a leaf's render list
	.space  16

# -----------------------------------------------------------------------------
# The graphics options' record loops (RECLOOP opt): entered from the
# first word of dr_records / de_records, patched into a jump here while an
# option is on (render.c r_set_options); they leave as those do.
	RECLOOP dro1, walk, 0, 1, 1          # the draw distance
	RECLOOP dro2, walk, 0, 2, 1          # the flat shading
	RECLOOP dro3, walk, 0, 3, 1          # both
	RECLOOP deo1, dm_exit, 1, 1          # the models': the draw distance

# -----------------------------------------------------------------------------
# mark_pvs: a0 = new camera leaf. visframe++, then the leaf's PVS row
# expanded into r_vis_row (vis_expand: a plain row, or the XOR against an
# earlier leaf's plain row, formats/qworld.h qw_leaf vis, docs/porting-notes.md
# §90.9; no row = everything visible), then each visible leaf and its
# not-yet-stamped ancestors stamped, a byte of the row at a time. A byte's
# leaves go highest first. Only the row's last byte can name a leaf past the
# array: it is masked (r_state vis_lastmask), no other byte is tested.
# Clobbers t0-t7, t9, a1-a3, s5 (ra kept in the scratchpad's walk stack:
# empty before the walk).
# mark_pvs_far (the draw distance's, called instead: render.c patches the
# call): a leaf whose box is farther from the camera than r_state.mark_r
# on an axis (vertex units: past the draw distance's depth anywhere in the
# frustum, and the camera's moves before the next marking) is not stamped.
# Clobbers v0, v1, at, a0 too.
.macro MARKPVS n, far
	ICSEC   \n
\n:
.if R_PROFILE
	lhu     $t9, TIMER2($t8)
.endif
	lw      $t7, RS_NODES($s0)           # parents are word offsets from here
	li      $t6, 0xffff                  # no parent
	sw      $a0, RS_VIEW_LEAF($s0)
	sw      $zero, RS_FORCE_MARK($s0)
.if R_PROFILE
	sw      $t9, SP_T0($t8)
.endif
	lw      $s5, RS_VISFRAME($s0)
	sw      $ra, SP_STACK + 4($t8)       # (the walk's stack: empty until the walk)
	addiu   $s5, $s5, 1
	jal     vis_expand                   # the row into r_vis_row
	sw      $s5, RS_VISFRAME($s0)
	lw      $ra, SP_STACK + 4($t8)
	lw      $t7, RS_NODES($s0)           # (vis_expand's clobbers back)
	li      $t6, 0xffff
	lw      $a2, RS_ROWBYTES($s0)
	lw      $a3, RS_LEAVES($s0)
	lw      $t5, RS_VIS_LASTMASK($s0)
	lui     $a1, %hi(r_vis_row)
	addiu   $a1, $a1, %lo(r_vis_row)
.if \far
	lw      $v0, SP_CAM+0($t8)           # the camera, and the reach
	lw      $v1, SP_CAM+4($t8)
	lw      $a0, SP_CAM+8($t8)
	lw      $at, RS_MARK_R($s0)
.endif
	addiu   $a2, $a2, -1                 # a2 = row bytes left - 1
	addiu   $a3, $a3, 8 * L_SIZE         # byte 0's highest leaf: leaf 8
\n\()_byte:                               # a1 = the byte, a3 = its leaf 8
	blez    $a2, \n\()_last
	lbu     $t0, 0($a1)                  # (none left: a harmless read)
	addiu   $a1, $a1, 1
	addiu   $a2, $a2, -1
	bnez    $t0, \n\()_bits
	move    $t9, $a3                     # (the bits' leaf cursor)
	b       \n\()_byte
	addiu   $a3, $a3, 8 * L_SIZE         # a zero byte: the next
\n\()_last:                               # the last byte (t0)
	bltz    $a2, \n\()_done                 # (none: done)
	and     $t0, $t0, $t5                # (its bits past the last leaf off)
	addiu   $a2, $a2, -1
	beqz    $t0, \n\()_done
	move    $t9, $a3
\n\()_bits:                               # t0: a byte's bits, t9: its leaf 8
	sll     $t0, $t0, 24                 # this leaf's bit at bit 31
\n\()_bit:
	bgez    $t0, \n\()_skip
	sll     $t0, $t0, 1
.if \far
	# |cam - c| > h + reach on an axis: unsigned (cam - c) + (h + reach)
	# > 2 (h + reach)
	lh      $t2, L_C+0($t9)
	lh      $t3, L_H+0($t9)
	subu    $t2, $v0, $t2
	addu    $t3, $t3, $at
	addu    $t2, $t2, $t3
	sll     $t3, $t3, 1
	sltu    $t2, $t3, $t2
	bnez    $t2, \n\()_skip
	lh      $t2, L_C+2($t9)
	lh      $t3, L_H+2($t9)
	subu    $t2, $v1, $t2
	addu    $t3, $t3, $at
	addu    $t2, $t2, $t3
	sll     $t3, $t3, 1
	sltu    $t2, $t3, $t2
	bnez    $t2, \n\()_skip
	lh      $t2, L_C+4($t9)
	lh      $t3, L_H+4($t9)
	subu    $t2, $a0, $t2
	addu    $t3, $t3, $at
	addu    $t2, $t2, $t3
	sll     $t3, $t3, 1
	sltu    $t2, $t3, $t2
	bnez    $t2, \n\()_skip
	nop
.endif
	lhu     $t2, L_PARENT($t9)
	sh      $s5, L_VISFRAME($t9)
\n\()_up:
	beq     $t2, $t6, \n\()_skip            # past the root
	sll     $t2, $t2, 2
	addu    $t2, $t2, $t7                # word offset -> node
	lhu     $t1, N_VISFRAME($t2)
	lhu     $t3, N_PARENT($t2)
	beq     $t1, $s5, \n\()_skip            # ancestors already stamped
	sh      $s5, N_VISFRAME($t2)         # (stamped again then: harmless)
	b       \n\()_up
	move    $t2, $t3
\n\()_skip:
	bnez    $t0, \n\()_bit                  # set bits left in this byte
	addiu   $t9, $t9, -L_SIZE
	b       \n\()_byte
	addiu   $a3, $a3, 8 * L_SIZE
\n\()_done:
.if R_PROFILE
	lhu     $t0, TIMER2($t8)
	lw      $t9, SP_T0($t8)
	lw      $t1, RS_TICKS_MARK($s0)
	subu    $t0, $t0, $t9
	andi    $t0, $t0, 0xffff
	addu    $t1, $t1, $t0
	jr      $ra
	sw      $t1, RS_TICKS_MARK($s0)
.else
	jr      $ra
	nop
.endif
.endm

	MARKPVS mark_pvs, 0
	.globl  mark_pvs_far
	MARKPVS mark_pvs_far, 1

# vis_expand: a0 = the camera leaf (KUSEG), s0 = r_state. The leaf's PVS row
# into r_vis_row, r_state.rowbytes bytes: no row, every bit; a plain row (its
# first byte 0) expanded from Quake's RLE (a zero byte, then the count of
# zero bytes); a delta row (its first byte `back`), the plain row of the leaf
# `back` before it expanded, then the delta's bytes XORed in, its runs
# skipped. The cook's rows are whole: no count is clamped. Clobbers t0-t7,
# t9, a1-a3; keeps a0.
	.section .text.vis_expand, "ax", @progbits
	.type   vis_expand, @function
vis_expand:
	lw      $a1, L_VIS($a0)              # the row (its top byte: the render list), or 0
	lw      $a2, RS_ROWBYTES($s0)
	lui     $t3, %hi(r_vis_row)
	addiu   $t3, $t3, %lo(r_vis_row)     # the out cursor
	sll     $a1, $a1, 8
	beqz    $a1, ve_all
	srl     $a1, $a1, 8
	lbu     $t0, 0($a1)                  # back
	addiu   $a3, $a1, 1                  # the payload
	beqz    $t0, ve_plain
	move    $t6, $zero                   # (delay) no delta after the plain row
	sll     $t1, $t0, 5                  # the base leaf: back x 28 before
	sll     $t2, $t0, 2
	subu    $t1, $t1, $t2
	subu    $t1, $a0, $t1
	lw      $t1, L_VIS($t1)              # its row (plain: the cook's)
	move    $t6, $a3                     # the delta, after the base
	sll     $t1, $t1, 8
	srl     $t1, $t1, 8
	b       ve_pl
	addiu   $a3, $t1, 1                  # (delay) past its back byte
ve_plain:
ve_pl:                                   # a3: the RLE in, t3: out, a2: bytes left
	lbu     $t4, 0($a3)
	addiu   $a3, $a3, 1
	beqz    $t4, ve_prun
	addiu   $a2, $a2, -1                 # (delay) one byte out
	sb      $t4, 0($t3)
	bgtz    $a2, ve_pl
	addiu   $t3, $t3, 1                  # (delay)
	b       ve_pdone
	nop
ve_prun:                                 # a run of t5 zero bytes
	lbu     $t5, 0($a3)
	addiu   $a3, $a3, 1
	addiu   $a2, $a2, 1                  # (the byte's count undone: the run's)
	subu    $a2, $a2, $t5
ve_pz:
	sb      $zero, 0($t3)
	addiu   $t5, $t5, -1
	bgtz    $t5, ve_pz
	addiu   $t3, $t3, 1                  # (delay)
	bgtz    $a2, ve_pl
	nop
ve_pdone:
	beqz    $t6, ve_ret                  # a plain row: done
	lw      $a2, RS_ROWBYTES($s0)        # (delay) the delta over the whole row
	lui     $t3, %hi(r_vis_row)
	addiu   $t3, $t3, %lo(r_vis_row)
ve_x:                                    # t6: the delta's RLE, t3: the row
	lbu     $t4, 0($t6)
	addiu   $t6, $t6, 1
	beqz    $t4, ve_xrun
	addiu   $a2, $a2, -1                 # (delay)
	lbu     $t5, 0($t3)
	addiu   $t3, $t3, 1
	xor     $t5, $t5, $t4
	bgtz    $a2, ve_x
	sb      $t5, -1($t3)                 # (delay)
	jr      $ra
	nop
ve_xrun:                                 # a run: nothing changes over it
	lbu     $t5, 0($t6)
	addiu   $t6, $t6, 1
	addiu   $a2, $a2, 1
	subu    $a2, $a2, $t5
	bgtz    $a2, ve_x
	addu    $t3, $t3, $t5                # (delay)
ve_ret:
	jr      $ra
	nop
ve_all:                                  # no row: every bit
	li      $t4, 0xff
ve_al:
	sb      $t4, 0($t3)
	addiu   $a2, $a2, -1
	bgtz    $a2, ve_al
	addiu   $t3, $t3, 1                  # (delay)
	jr      $ra
	nop
	.size   vis_expand, . - vis_expand

	.pushsection .rodata.r_far_sites, "a", @progbits
	.global r_far_sites_end
r_far_sites_end:
	.popsection
	.pushsection .rodata.r_ysites, "a", @progbits
	.global r_ysites_end
r_ysites_end:
	.popsection
