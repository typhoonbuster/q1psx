# q1psx — the world's zones, hand-written MIPS assembly (engine/zone.h,
# formats/qworld.h "zones", docs/porting-notes.md §57).
#
#   zone_mark    the renderer's camera-leaf call while a zoned map is loaded
#                (r_world.s mark_call, patched by render.c mark_patch):
#                a0 = the camera leaf (KUSEG), s0 = r_state, t8 = the
#                scratchpad, ra = mark_done. The leaf's zone set is looked
#                up (its index: its offset over 28, by a multiply); a set
#                other than the last call's goes to zone_enter (zone.c, C),
#                which may make another chunk current, so the vertex pool
#                the walk keeps in the scratchpad is taken again from
#                r_state. Then on to the marking render.c chose (zone_rt
#                mark: mark_pvs or mark_pvs_far), which returns to
#                mark_done. Clobbers what mark_pvs_far may (t0-t7, t9,
#                a1-a3, v0, v1, at, hi / lo); keeps a0, ra, the s
#                registers, fp, t8, gp. The same set: 17 instructions and
#                the multiply's wait.
#   zone_patch   C ABI: a chunk's patch words into the world nodes' face
#                blocks (formats/qworld.h QZP_*): a0 = the first node, a1 =
#                the words, a2 = their count, a3 = the chunk's records
#   zone_move    C ABI: a0 = dst, a1 = src, a2 = bytes (a multiple of 16,
#                not 0), copied forwards: dst below src or apart
#
# The C engine (CMake Q1PSX_ENGINE_C: ZONE_C = 1) takes zone_patch and
# zone_move in C (engine/zone.c); zone_mark is the renderer's (r_world.s
# has no C).

	.set noreorder
	.set noat

	.include "r_defs.inc"

.ifndef ZONE_C
.set ZONE_C, 0
.endif

.set ZR_LEAVES,  0       # zone.h zone_rt_t
.set ZR_LEAFSET, 4
.set ZR_SET,     8
.set ZR_MARK,    12

	.section .text.zone_mark, "ax", @progbits
	.global zone_mark
	.type   zone_mark, @function
zone_mark:                               # (zone_rt is 16-byte aligned: one %hi for its words)
	lui     $t0, %hi(zone_rt)
	lw      $t1, %lo(zone_rt + ZR_LEAVES)($t0)
	lui     $t3, 0x2492
	ori     $t3, $t3, 0x4925             # 2^32 / 7, rounded up
	subu    $t1, $a0, $t1                # 28 x the leaf's index
	srl     $t1, $t1, 2                  # 7 x
	multu   $t1, $t3
	lw      $t2, %lo(zone_rt + ZR_LEAFSET)($t0)
	lw      $t4, %lo(zone_rt + ZR_SET)($t0)
	mfhi    $t1                          # the index (exact while 3 x it < 2^32;
	addu    $t2, $t2, $t1                #  waits for the product)
	lbu     $t2, 0($t2)                  # the leaf's set
	lw      $t9, %lo(zone_rt + ZR_MARK)($t0)
	bne     $t2, $t4, zm_enter
	nop
	jr      $t9                          # the same set: on to the marking
	nop
zm_enter:
	addiu   $sp, $sp, -32                # (o32 argument slots, then the kept words)
	sw      $ra, 28($sp)
	sw      $a0, 24($sp)
	sw      $gp, 20($sp)                 # (the walk's own use of gp)
	lui     $gp, %hi(_gp)
	addiu   $gp, $gp, %lo(_gp)
	jal     zone_enter
	move    $a0, $t2
	lw      $ra, 28($sp)
	lw      $a0, 24($sp)
	lw      $gp, 20($sp)
	lui     $t0, %hi(zone_rt)
	lw      $t9, %lo(zone_rt + ZR_MARK)($t0)
	lw      $t1, RS_VERTPOOL($s0)        # (another chunk current: its pool)
	lui     $t8, 0x1f80                  # (the scratchpad's base, the walk's t8)
	addiu   $sp, $sp, 32
	jr      $t9
	sw      $t1, SP_VERTPOOL($t8)
	.size   zone_mark, . - zone_mark

.if ZONE_C == 0
# Per word: the empty nodes before its node at 12 cycles each (two stores,
# the count, the step), then its node's two pointers and the word's
# unpacking, 32 cycles.
	.section .text.zone_patch, "ax", @progbits
	.global zone_patch
	.type   zone_patch, @function
zone_patch:
	lw      $t0, 0($a1)
zp_word:
	addiu   $a1, $a1, 4
	srl     $t1, $t0, 24                 # the nodes before it without records:
	beqz    $t1, zp_node                 # their blocks empty at the pointer
	srl     $t2, $t0, 10
zp_skip:
	sw      $a3, N_FACES($a0)
	addiu   $t1, $t1, -1
	addiu   $a0, $a0, N_SIZE
	bnez    $t1, zp_skip
	sw      $a3, N_FACES+4-N_SIZE($a0)
zp_node:
	andi    $t2, $t2, 0x3ffc             # its front block's bytes
	sll     $t3, $t0, 2
	andi    $t3, $t3, 0x3ffc             # its back block's
	sw      $a3, N_FACES($a0)
	addu    $a3, $a3, $t2
	sw      $a3, N_FACES+4($a0)
	addiu   $a2, $a2, -1
	lw      $t0, 0($a1)                  # (the next word; past the last: the
	addu    $a3, $a3, $t3                #  records, never used)
	bnez    $a2, zp_word
	addiu   $a0, $a0, N_SIZE
	jr      $ra
	nop
	.size   zone_patch, . - zone_patch

	.section .text.zone_move, "ax", @progbits
	.global zone_move
	.type   zone_move, @function
zone_move:
	addu    $a2, $a1, $a2                # the source's end
zv_copy:
	lw      $t0, 0($a1)                  # four words read, then written: an
	lw      $t1, 4($a1)                  # overlap below the source is safe
	lw      $t2, 8($a1)
	lw      $t3, 12($a1)
	addiu   $a1, $a1, 16
	sw      $t0, 0($a0)
	sw      $t1, 4($a0)
	sw      $t2, 8($a0)
	sw      $t3, 12($a0)
	bne     $a1, $a2, zv_copy
	addiu   $a0, $a0, 16
	jr      $ra
	nop
	.size   zone_move, . - zone_move
.endif
