# q1psx — rl_order: an alias model's or a particle's place among its leaf's
# brush-model pieces (docs/porting-notes.md §50.1, §51). C ABI:
#
#   void rl_order(int id, int n, const int32_t org[3], const int32_t *box);
#
# id: the leaf's list, which held brush models' pieces once they were all in
# (rl_list_pieces[id], r_lists' upper half: not 0); n: the item, filled (a
# model, its depth in x, or a particle); its point: org (1/8 units), or with
# box (an edict's mins then maxs) org plus the box's centre, (mins + maxs) / 2
# rounded toward 0 as the C's.
#
# Item n goes into the list after the last pieces item drawn over the point
# (none: at the list's head), then, a model, after the models nearer than it
# that follow. The list holds each brush model's items together, from its
# walk's front. An item is over the point when, at the deepest node of the
# point's way down its model's BSP whose subtree holds the item's node, the
# point is beyond the plane from the eye; the first one that is not ends its
# model's asks. A model whose box the segment from the eye to the point
# misses is not asked.
#
# The C specification is engine/rlist.c rl_order_c with engine/bmfrag.c's
# bmf_may_hide, bmf_pieces_node and bmf_node_over (ORDER_C=1 builds it
# alone, ORDER_C=2 checks this by it, ask by ask). As the C:
# - the first ask of a frame at a list chains its pieces items (z: the next
#   one + 1, from the first one; x: -1, their node not looked for) and marks
#   the pieces' head (| 0x8000);
# - an item's node is looked for once a frame and kept in its x: the last
#   node whose groups' records start at or before its own (the C finds the
#   group, then its node; the groups' offsets rise, so one binary search
#   over the nodes gives the same);
# - a plane's side is d <= 0 behind: d = v[type] x 4096 - dist / 2 on an
#   axial plane (one compare), n . v - dist / 2 on another: the GTE's
#   (MVMVA, the point and the eye as the rotation's rows 1 and 2: both
#   products at once), the CPU's when one of their coordinates is past 16
#   bits (int32 products, wrapping as the C's).
# Unlike the C, the eye's side is taken with the point's at every node of
# the way (the C takes it at the nodes it decides at), and the way is not
# kept from one call to the next.
#
# The cook bounds a model's depth to QW_MAX_BM_DEPTH nodes (tools/qcook/
# world.c depth_of; the renderer's model walk counts on it too): the way
# holds them all, and the C's bound on it never acts.
#
# The scratchpad (free in the ents stage: r_render sets its own again) holds
# the way, the point, the segment's box; the GTE's rotation is left changed
# (rl_ent_matrix, r_render load theirs). Registers through the asks: v1 the
# item (its address), t8 its model, t9 r_items - 16 (item i, index + 1, at
# t9 + 16 i), v0 world.planes, a2 the scratchpad, t7 the model's bmf_cache,
# t6 its nodes, t5 the way's end, t3 the item's node, t0 the step checked.

	.set noreorder
	.set noat

	.include "gtereg.inc"
	.include "inline_s.inc"
	.include "r_defs.inc"

.set CA_ORG,         0     # bmf_cache (engine/bmfrag.c, its _Static_asserts)
.set CA_NUM_NODES,  14
.set CA_NODES,      20
.set CA_RECS,       24
.set CA_GROUPS,     36
.set CA_NUM_GROUPS, 40
.set CA_CAM,        60     # the eye in its space, vertex units (bmf_draw's)
.set BN_PLANE3,      0     # bmf_node, 16 bytes
.set BN_CHILD,       2     # int16 child[2], -1 a leaf
.set BN_FIRST,       6     # uint16 first[2]: its groups
.set BN_END,        14     # its subtree: [the node, end)
.set BG_OFS,         4     # bmf_group, 12 bytes
.set QB_MINS,        0     # qw_bmodel, 40 bytes
.set QB_MAXS,       12
.set PL_TYPE,        6     # qw_plane: int16 n[3], type, dist
.set PL_DIST,        8
.set W_PLANES,      76     # world_t
.set W_BMODELS,    116
.set RI_PIECES,   0x80     # r_item_t.type: a brush model's pieces (rlist.h)
.set LP_PIECES,    256     # r_lists[128 + id]: the list's pieces' head (rlist.c)
.set WAY_STEPS,    QW_MAX_BM_DEPTH + 1   # the C's way (bmfrag.c)

# ---- scratchpad (0x1F800000) --------------------------------------------------
.set OR_WAY,     0x000     # the point's way down the model's BSP, 3 words a step:
                           # the child on the point's side, its subtree's size (0:
                           # a leaf), the point beyond the plane from the eye (1)
.set OR_P,       0x2dc     # the point, 1/8 units (3 words; OR_WAY + 12 WAY_STEPS)
.set OR_LO,      0x2e8     # the segment eye - point's box (3 words),
.set OR_HI,      0x2f4     #   its other corner
.set OR_N,       0x300     # the item
.set OR_LIST,    0x304     # &r_lists[id]
.set OR_OVER,    0x308     # the last pieces item over the point (its address; 0: none)
.set OR_GTE,     0x30c     # this model's general planes: 0 not set up, 1 the
                           # GTE's rows, 2 the CPU's (a coordinate past 16 bits)
.set OR_V,       0x310     # the point in the model's space, vertex units (3 words)
.set OR_ROWS,    0x31c     # the GTE's rows 1, 2 as halfwords: v, the eye (3 words)
.set OR_DOT,     0x328     # the CPU's n . v, kept while it takes the eye's

# SEG e, p, lo, hi: the segment's box on an axis, stored (e < p ? e : p, and
# the other, the C's); clobbers t7, t8, t9
.macro SEG e, p, lo, hi
	slt     $t7, \e, \p
	subu    $t7, $zero, $t7              # -1: e < p
	xor     $t8, \e, \p
	and     $t8, $t8, $t7
	xor     $t9, \p, $t8                 # the lower
	xor     $t8, \e, $t8                 # the higher
	sw      $t9, \lo($a2)
	sw      $t8, \hi($a2)
.endm

# FIT x: t1 |= not 0 when x is past 16 bits; clobbers at
.macro FIT x
	sra     $at, \x, 15
	addiu   $at, $at, 1
	srl     $at, $at, 1                  # 0: x within 16 bits (x >> 15 is 0 or -1)
	or      $t1, $t1, $at
.endm

	.section .text.rl_order, "ax", @progbits
	.global rl_order
	.type rl_order, @function
rl_order:
	lw      $t0, 0($a2)                  # the point: org,
	lw      $t1, 4($a2)
	beqz    $a3, 1f
	lw      $t2, 8($a2)
	lw      $t3, 0($a3)                  # plus the box's centre: (mins + maxs) / 2,
	lw      $t4, 12($a3)                 # toward 0
	lw      $t5, 4($a3)
	lw      $t6, 16($a3)
	addu    $t3, $t3, $t4
	srl     $t4, $t3, 31
	addu    $t3, $t3, $t4
	sra     $t3, $t3, 1
	addu    $t0, $t0, $t3
	addu    $t5, $t5, $t6
	lw      $t3, 8($a3)
	lw      $t4, 20($a3)
	srl     $t6, $t5, 31
	addu    $t5, $t5, $t6
	sra     $t5, $t5, 1
	addu    $t1, $t1, $t5
	addu    $t3, $t3, $t4
	srl     $t4, $t3, 31
	addu    $t3, $t3, $t4
	sra     $t3, $t3, 1
	addu    $t2, $t2, $t3
1:	lui     $a2, 0x1f80                  # the scratchpad
	lui     $t3, %hi(r_state)
	addiu   $t3, $t3, %lo(r_state)
	lw      $t4, RS_CAM+0($t3)           # the eye, vertex units: x 2, 1/8 units
	lw      $t5, RS_CAM+4($t3)
	lw      $t6, RS_CAM+8($t3)
	sll     $t4, $t4, 1
	sw      $t0, OR_P+0($a2)
	SEG     $t4, $t0, OR_LO+0, OR_HI+0
	sll     $t5, $t5, 1
	sw      $t1, OR_P+4($a2)
	SEG     $t5, $t1, OR_LO+4, OR_HI+4
	sll     $t6, $t6, 1
	sw      $t2, OR_P+8($a2)
	SEG     $t6, $t2, OR_LO+8, OR_HI+8
	lui     $v0, %hi(world + W_PLANES)
	lw      $v0, %lo(world + W_PLANES)($v0)
	lui     $t9, %hi(r_lists)
	addiu   $t9, $t9, %lo(r_lists)
	sll     $a0, $a0, 1
	addu    $a0, $t9, $a0                # &r_lists[id]
	lhu     $v1, LP_PIECES($a0)          # its pieces' head
	sw      $zero, OR_OVER($a2)          # over the point: none so far
	sw      $a1, OR_N($a2)
	sw      $a0, OR_LIST($a2)
	lui     $t9, %hi(r_items - 16)
	addiu   $t9, $t9, %lo(r_items - 16)
	andi    $t0, $v1, 0x8000
	bnez    $t0, walk                    # chained this frame
	andi    $v1, $v1, 0x7fff

	# The first ask this frame: the list's pieces items chained (z), from the
	# first of them, their nodes not looked for (x -1). (The head is not 0.)
	move    $t1, $zero                   # the first pieces item
	move    $t2, $zero                   # the last one (its address; 0: none yet)
	li      $t3, -1
	li      $t4, RI_PIECES
2:	sll     $t0, $v1, 4
	addu    $t0, $t9, $t0                # item v1
	lbu     $t5, RI_TYPE($t0)
	lhu     $t6, RI_NEXT($t0)
	bne     $t5, $t4, 5f                 # not pieces: on
	nop
	beqz    $t2, 3f
	sh      $t3, RI_X($t0)
	b       4f
	sh      $v1, RI_Z($t2)               # (the last one's next: it)
3:	move    $t1, $v1                     # (the first)
4:	move    $t2, $t0
5:	bnez    $t6, 2b
	move    $v1, $t6
	beqz    $t2, 6f
	ori     $v1, $t1, 0x8000             # the pieces' head: the first, chained
	sh      $zero, RI_Z($t2)             # (the last one's next: none)
6:	sh      $v1, LP_PIECES($a0)
	move    $v1, $t1

walk:	# v1: the first pieces item, index + 1 (0: none)
	beqz    $v1, done
	sll     $v1, $v1, 4
	addu    $v1, $t9, $v1
	lbu     $t2, RI_COLOUR($v1)
	b       run1
	move    $t8, $t2

skipv:	lhu     $t1, RI_Z($v1)               # past item v1, of the model not asked
	nop
	sll     $t1, $t1, 4
skip:	beqz    $t1, done                    # t1: the next pieces item x 16 (0: none)
	addu    $v1, $t9, $t1
1:	lbu     $t2, RI_COLOUR($v1)
	lhu     $t1, RI_Z($v1)
	bne     $t2, $t8, run1               # another model's
	move    $t8, $t2
	sll     $t1, $t1, 4
	bnez    $t1, 1b
	addu    $v1, $t9, $t1
	b       done_1
	lw      $v0, OR_OVER($a2)            # (delay: done's first)

run1:	# v1: the first item of model t8 in a run: can the model hide the point,
	# the segment from the eye to it crossing its box?
	lui     $t4, %hi(bmf_models)
	lw      $t4, %lo(bmf_models)($t4)
	sll     $t2, $t8, 2
	addu    $t4, $t4, $t2
	lw      $t7, 0($t4)                  # its bmf_cache
	lui     $t4, %hi(world + W_BMODELS)
	lw      $t4, %lo(world + W_BMODELS)($t4)
	addu    $t2, $t2, $t8
	sll     $t2, $t2, 3                  # model x 40
	addu    $t4, $t4, $t2                # its qw_bmodel
	lw      $t2, CA_ORG+0($t7)           # where it stands
	lw      $t1, QB_MINS+0($t4)
	lw      $a0, OR_HI+0($a2)
	addu    $t1, $t1, $t2
	slt     $t1, $a0, $t1                # the segment short of its box,
	bnez    $t1, skipv
	lw      $a0, QB_MAXS+0($t4)
	lw      $t1, OR_LO+0($a2)
	addu    $a0, $a0, $t2
	slt     $a0, $a0, $t1                # or past it: not asked
	bnez    $a0, skipv
	lw      $t2, CA_ORG+4($t7)
	lw      $t1, QB_MINS+4($t4)
	lw      $a0, OR_HI+4($a2)
	addu    $t1, $t1, $t2
	slt     $t1, $a0, $t1
	bnez    $t1, skipv
	lw      $a0, QB_MAXS+4($t4)
	lw      $t1, OR_LO+4($a2)
	addu    $a0, $a0, $t2
	slt     $a0, $a0, $t1
	bnez    $a0, skipv
	lw      $t2, CA_ORG+8($t7)
	lw      $t1, QB_MINS+8($t4)
	lw      $a0, OR_HI+8($a2)
	addu    $t1, $t1, $t2
	slt     $t1, $a0, $t1
	bnez    $t1, skipv
	lw      $a0, QB_MAXS+8($t4)
	lw      $t1, OR_LO+8($a2)
	addu    $a0, $a0, $t2
	slt     $a0, $a0, $t1
	bnez    $a0, skipv
	lw      $t6, CA_NODES($t7)           # its nodes
	sw      $zero, OR_GTE($a2)           # (its general planes' rows: not set up)
	addiu   $t5, $a2, OR_WAY             # its way: not taken yet

	# v1: an item of model t8, asked: its node, then the way's steps that hold
	# it, from the root, the way taken deeper as it needs
	lh      $t3, RI_X($v1)
	addiu   $t0, $a2, OR_WAY
ask1:	bltz    $t3, find                    # not looked for yet this frame
	move    $t1, $zero                   # (the root, the way's first node)
chk:	beq     $t0, $t5, ext                # past the way's end: its step at node t1
	lw      $a1, 0($t0)                  # the step's child on the point's side,
	lw      $at, 4($t0)                  # its subtree's size
	subu    $t2, $t3, $a1
	sltu    $t2, $t2, $at
chk4:	addiu   $t0, $t0, 12
	bnez    $t2, chk                     # the item's node under the child: deeper
	move    $t1, $a1
dec:	lw      $a3, -4($t0)                 # decided: the point beyond the plane?
	lhu     $t1, RI_Z($v1)               # (the next pieces item)
	beqz    $a3, skip                    # under it: the model's next ones too
	sll     $t1, $t1, 4
	beqz    $t1, done
	sw      $v1, OR_OVER($a2)            # over it: the last one so far
	addu    $v1, $t9, $t1
	lbu     $t2, RI_COLOUR($v1)
	lh      $t3, RI_X($v1)
	beq     $t2, $t8, ask1               # the model's next item (its way taken)
	addiu   $t0, $a2, OR_WAY
	b       run1
	move    $t8, $t2

ext:	# the way's step at node t1, at t5 (t0 there too)
	sll     $t2, $t1, 4
	addu    $t2, $t6, $t2                # the node
	lhu     $a3, BN_PLANE3($t2)
	addiu   $t5, $t5, 12                 # (the step: at t5 - 12)
	sll     $a3, $a3, 2
	addu    $a3, $v0, $a3                # its plane
	lbu     $t4, PL_TYPE($a3)
	lw      $a0, PL_DIST($a3)
	sltiu   $at, $t4, 3
	beqz    $at, obl                     # a general plane
	sll     $t4, $t4, 2
	addu    $at, $a2, $t4
	lw      $a1, OR_P($at)               # the point on the plane's axis,
	addu    $t4, $t7, $t4
	lw      $at, CA_ORG($t4)             # where the model stands on it,
	lw      $t4, CA_CAM($t4)             # the eye in its space
	subu    $a1, $a1, $at
	sra     $a1, $a1, 1                  # the point in its space, vertex units
	sll     $a1, $a1, 12                 # (x 4096, wrapping as the C's)
	sll     $t4, $t4, 12
sides:	# a1, t4: the point's and the eye's d + dist / 2, a0: the plane's dist,
	# t5 past the step
	sra     $a0, $a0, 1
	subu    $a1, $a1, $a0
	subu    $t4, $t4, $a0
	slti    $a1, $a1, 1                  # the point behind the plane (d <= 0): 1
	slti    $t4, $t4, 1                  # the eye
	xor     $t4, $t4, $a1                # the point beyond the plane from the eye
	sll     $a1, $a1, 1
	addu    $a1, $t2, $a1
	lh      $a1, BN_CHILD($a1)           # the child on the point's side
	sw      $t4, -4($t5)
	sw      $a1, -12($t5)
	bltz    $a1, 2f                      # a leaf: no node there, the step decides
	sll     $a0, $a1, 4
	addu    $a0, $t6, $a0
	lhu     $at, BN_END($a0)
	subu    $t2, $t3, $a1                # (the item's node against the child)
	subu    $at, $at, $a1                # its subtree's size
	sw      $at, -8($t5)
	b       chk4
	sltu    $t2, $t2, $at
2:	sw      $zero, -8($t5)
	b       dec
	move    $t0, $t5

obl:	# a general plane (a3; a0 its dist, t2 the node): this model's rows?
	lw      $at, OR_GTE($a2)
	nop
	beqz    $at, rows
	addiu   $at, $at, -1
	bnez    $at, cpu
	nop
	lwc2    C2_VXY0, 0($a3)              # the normal: n0 | n1 << 16, n2
	lh      $at, 4($a3)
	nop
	mtc2    $at, C2_VZ0
	nop
	nop
	MVMVA   0, 0, 0, 3, 0                # MAC1 = v . n, MAC2 = eye . n (rows 1, 2)
	mfc2    $a1, C2_MAC1
	mfc2    $t4, C2_MAC2
	b       sides
	nop

rows:	# the point (v: (p - org) / 2, the C's) and the eye in the model's
	# space, as the GTE's rows 1 and 2 when all six fit 16 bits
	lw      $t4, OR_P+0($a2)
	lw      $at, CA_ORG+0($t7)
	move    $t1, $zero
	subu    $t4, $t4, $at
	sra     $t4, $t4, 1
	lw      $a1, OR_P+4($a2)
	lw      $at, CA_ORG+4($t7)
	sw      $t4, OR_V+0($a2)
	sh      $t4, OR_ROWS+0($a2)
	subu    $a1, $a1, $at
	sra     $a1, $a1, 1
	sw      $a1, OR_V+4($a2)
	sh      $a1, OR_ROWS+2($a2)
	FIT     $t4
	FIT     $a1
	lw      $t4, OR_P+8($a2)
	lw      $at, CA_ORG+8($t7)
	lw      $a1, CA_CAM+0($t7)
	subu    $t4, $t4, $at
	sra     $t4, $t4, 1
	sw      $t4, OR_V+8($a2)
	sh      $t4, OR_ROWS+4($a2)
	sh      $a1, OR_ROWS+6($a2)
	FIT     $t4
	FIT     $a1
	lw      $t4, CA_CAM+4($t7)
	lw      $a1, CA_CAM+8($t7)
	nop
	sh      $t4, OR_ROWS+8($a2)
	sh      $a1, OR_ROWS+10($a2)
	FIT     $t4
	FIT     $a1
	bnez    $t1, 2f                      # past 16 bits: the CPU's
	li      $at, 2
	lw      $t4, OR_ROWS+0($a2)
	lw      $a1, OR_ROWS+4($a2)
	lw      $t1, OR_ROWS+8($a2)
	ctc2    $t4, C2_R11R12               # row 1: the point,
	ctc2    $a1, C2_R13R21
	ctc2    $t1, C2_R22R23               # row 2: the eye
	li      $at, 1
2:	b       obl
	sw      $at, OR_GTE($a2)

cpu:	# n . v, then n . eye: int32 products (t0 the step's again after)
	addiu   $t0, $a2, OR_V
	addiu   $t1, $a3, 6
	move    $a1, $zero
3:	lh      $at, 0($a3)
	lw      $t4, 0($t0)
	addiu   $a3, $a3, 2
	mult    $at, $t4
	addiu   $t0, $t0, 4
	mflo    $at
	bne     $a3, $t1, 3b
	addu    $a1, $a1, $at
	sw      $a1, OR_DOT($a2)
	addiu   $a3, $a3, -6
	addiu   $t0, $t7, CA_CAM
	move    $a1, $zero
4:	lh      $at, 0($a3)
	lw      $t4, 0($t0)
	addiu   $a3, $a3, 2
	mult    $at, $t4
	addiu   $t0, $t0, 4
	mflo    $at
	bne     $a3, $t1, 4b
	addu    $a1, $a1, $at
	move    $t4, $a1
	lw      $a1, OR_DOT($a2)
	b       sides
	addiu   $t0, $t5, -12

find:	# item v1's node: the last whose groups' records start at or before its
	# own (the C's group then node searches in one: the groups' offsets rise;
	# a node whose groups start past the last has none)
	lw      $t1, RI_ENT($v1)
	lw      $t2, CA_RECS($t7)
	lhu     $at, CA_NUM_NODES($t7)
	lw      $a3, CA_GROUPS($t7)
	lw      $t0, CA_NUM_GROUPS($t7)
	subu    $t1, $t1, $t2                # its records' offset
	move    $a0, $zero                   # lo
	addiu   $at, $at, -1                 # hi
	move    $t3, $zero                   # the node
1:	slt     $t2, $at, $a0
	bnez    $t2, 3f
	addu    $a1, $a0, $at
	sra     $a1, $a1, 1                  # mid
	sll     $t2, $a1, 4
	addu    $t2, $t6, $t2
	lhu     $t2, BN_FIRST($t2)           # its first group
	nop
	sltu    $t4, $t2, $t0
	beqz    $t4, 2f                      # none: lower
	sll     $t4, $t2, 1
	addu    $t4, $t4, $t2
	sll     $t4, $t4, 2
	addu    $t4, $a3, $t4
	lw      $t4, BG_OFS($t4)
	nop
	sltu    $t4, $t1, $t4                # its records past the item's: lower
	bnez    $t4, 2f
	nop
	move    $t3, $a1
	b       1b
	addiu   $a0, $a1, 1
2:	b       1b
	addiu   $at, $a1, -1
3:	sh      $t3, RI_X($v1)
	addiu   $t0, $a2, OR_WAY
	b       chk
	move    $t1, $zero

done:	# the last pieces item over the point (its address, 0: none) is the
	# item's place; a model's, after the models nearer than it that follow
	lw      $v0, OR_OVER($a2)
done_1:
	lw      $a1, OR_N($a2)
	lw      $a0, OR_LIST($a2)
	addiu   $a3, $a1, 1                  # n + 1
	sll     $t1, $a3, 4
	addu    $t1, $t9, $t1                # item n
	lbu     $t2, RI_TYPE($t1)
	li      $at, 1
	bne     $t2, $at, ins                # a particle
	lh      $t3, RI_X($t1)               # (a model: its depth)
	lhu     $t4, 0($a0)                  # (none over it: from the list's head)
	beqz    $v0, 7f
	nop
	lhu     $t4, RI_NEXT($v0)
	nop
7:	beqz    $t4, ins
	sll     $t5, $t4, 4
	addu    $t5, $t9, $t5
	lbu     $t6, RI_TYPE($t5)
	lh      $t7, RI_X($t5)
	bne     $t6, $at, ins                # not a model
	slt     $t7, $t7, $t3
	beqz    $t7, ins                     # not nearer
	nop
	move    $v0, $t5
	lhu     $t4, RI_NEXT($t5)
	b       7b
	nop
ins:	beqz    $v0, 8f
	nop
	lhu     $t5, RI_NEXT($v0)            # after item v0
	sh      $a3, RI_NEXT($v0)
	jr      $ra
	sh      $t5, RI_NEXT($t1)
8:	lhu     $t5, 0($a0)                  # at the list's head
	sh      $a3, 0($a0)
	jr      $ra
	sh      $t5, RI_NEXT($t1)
	.size rl_order, . - rl_order
