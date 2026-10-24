# q1psx — the leaf of a point in the world's BSP, hand-written MIPS
# assembly (docs/porting-notes.md §72). C ABI:
#
#   const qw_leaf *world_point_leaf_from(uint32_t at, const int32_t pos[3]);
#
# at: a node (a KUSEG pointer, world_box_topnode's or the root), or a leaf
# (bit 0 set: given back, the bit cleared); pos: whole world units. The
# walk takes, at each node, its plane's side of the point: d = n . p -
# (dist >> 1) (p = pos x 4, vertex units; dist the cook's x 4096 in 1/8
# units, halved to vertex units x 4096), d > 0 the front child
# (children[0]), else the back; int32 and wrapping, as the C's. On an
# axial plane (n = 4096 on the axis `type`) n . p is pos[type] << 14: the
# three shifted coordinates are kept on the stack and the type indexes
# them, no multiply; on another, the CPU's three products (pos x n, x 4
# after their sum: the same int32 as the C's four-times-bigger terms),
# never the GTE (its callers may hold its matrices: the render lists'
# leaves).
#
# Both children are read with the plane, the side picks one, and the
# picked one's leaf bit is tested in the branches' delay slots: no load
# waits at the walk's end.
#
# engine/world.c world_point_leaf_from_c is its C (the C engine's; the
# bench compares the two: main.c bench_wleaf_diff).

	.set noreorder
	.set noat

	.include "p_defs.inc"

	.section .text.world_point_leaf_from, "ax", @progbits
	.global world_point_leaf_from
	.type   world_point_leaf_from, @function
world_point_leaf_from:                   # a0 = at, a1 = pos
	lw      $t0, 0($a1)
	lw      $t1, 4($a1)
	lw      $t2, 8($a1)
	addiu   $sp, $sp, -16
	sll     $t3, $t0, 14                 # pos x 4 x 4096: an axial plane's n . p
	sll     $t4, $t1, 14
	sll     $t5, $t2, 14
	sw      $t3, 0($sp)
	sw      $t4, 4($sp)
	sw      $t5, 8($sp)
	andi    $at, $a0, 1
	bnez    $at, 9f                      # a leaf already
	lui     $t9, %hi(world+W_PLANES)
	lw      $t9, %lo(world+W_PLANES)($t9)   # the planes (int32 words)

1:	lhu     $t6, N_PLANE3($a0)           # a0: the node
	lw      $a2, N_CHILD+0($a0)          # the front child
	sll     $t6, $t6, 2
	addu    $t6, $t9, $t6                # its plane
	lbu     $t7, PL_TYPE($t6)
	lw      $t8, PL_DIST($t6)
	lw      $a3, N_CHILD+4($a0)          # the back child
	sltiu   $at, $t7, 3
	beqz    $at, 5f                      # not axial
	sll     $t7, $t7, 2                  # (delay)
	addu    $t7, $sp, $t7
	lw      $t7, 0($t7)                  # pos[type] << 14
	sra     $t8, $t8, 1                  # dist >> 1
	subu    $t7, $t7, $t8                # d (wrapping as the C's)
3:	blez    $t7, 4f                      # d <= 0: the back
	andi    $v1, $a3, 1                  # (delay) the back: a leaf?
	move    $a3, $a2                     # the front
	andi    $v1, $a2, 1
4:	beqz    $v1, 1b                      # a node: on
	move    $a0, $a3                     # (delay) the child

9:	li      $v0, -2
	and     $v0, $a0, $v0                # the leaf (bit 0 cleared)
	jr      $ra
	addiu   $sp, $sp, 16

5:	lh      $t3, PL_N+0($t6)             # a general plane: n . pos x 4
	lh      $t4, PL_N+2($t6)
	mult    $t0, $t3
	lh      $t5, PL_N+4($t6)
	mflo    $t3
	sra     $t8, $t8, 1                  # dist >> 1 (no mult within two of
	nop                                  #  an mflo)
	mult    $t1, $t4
	mflo    $t4
	addu    $t3, $t3, $t4
	nop
	mult    $t2, $t5
	mflo    $t5
	addu    $t3, $t3, $t5
	sll     $t3, $t3, 2                  # x 4: n . p
	b       3b
	subu    $t7, $t3, $t8                # (delay) d
	.size   world_point_leaf_from, . - world_point_leaf_from
