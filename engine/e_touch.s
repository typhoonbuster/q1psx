# q1psx — SV_TouchLinks' walk, hand-written MIPS assembly
# (docs/porting-notes.md §72). C ABI:
#
#   int touch_gather(const edict_t *e, uint16_t out[64], const touch_walk_t *w);
#
# The trigger slots of the list w (engine/ents.c touch_walk_t: every slot,
# the player's, or the monsters' list) whose box the box of e reaches,
# their edicts written to out (64 at most), their count returned. The
# boxes are in whole units, a unit wider (the floor of the mins, the
# ceiling of the maxs, int16: box16's); e's world box is its origin plus
# its local box, a unit wider, an item's 15 more across (ents.h
# ent_absbox: the edict keeps no absmin / absmax since §90.5), kept on
# the frame for the nodes' split tests. The area nodes are walked as
# Quake's SV_TouchLinks does (depth first, children[0] first: the
# children pushed lower then upper half), a node whose subtree holds none
# of the list's slots left out; each node's slots in their link order.
# Two of a slot's six tests, its x span against e's, come first: most
# slots fail there.
#
# engine/ents.c touch_gather_c is its C (the C engine's; the bench
# compares the two: the touchdiff line).

	.set noreorder
	.set noat

	.include "p_defs.inc"

	.section .text.touch_gather, "ax", @progbits
	.global touch_gather
	.type   touch_gather, @function
touch_gather:                            # a0 = e, a1 = out, a2 = w
	addiu   $sp, $sp, -64                # s0-s3, the nodes' stack (bytes, 16..31),
	sw      $s0, 0($sp)                  #  e's world box (32..55)
	sw      $s1, 4($sp)
	sw      $s2, 8($sp)
	sw      $s3, 12($sp)
	lw      $t0, E_ORIGIN+0($a0)         # the world box: origin + mins - 1, + maxs + 1
	lw      $t1, E_ORIGIN+4($a0)
	lw      $t2, E_ORIGIN+8($a0)
	lw      $s0, E_MINS+0($a0)
	lw      $s1, E_MINS+4($a0)
	lw      $s2, E_MINS+8($a0)
	lw      $s3, E_MAXS+0($a0)
	lw      $t8, E_MAXS+4($a0)
	lw      $t9, E_MAXS+8($a0)
	lw      $v0, E_FLAGS($a0)
	addu    $s0, $s0, $t0
	addu    $s1, $s1, $t1
	addu    $s2, $s2, $t2
	addu    $s3, $s3, $t0
	addu    $t8, $t8, $t1
	addu    $t9, $t9, $t2
	andi    $v0, $v0, FL_ITEM
	addiu   $s0, $s0, -CLIP_SUB
	addiu   $s1, $s1, -CLIP_SUB
	addiu   $s2, $s2, -CLIP_SUB
	addiu   $s3, $s3, CLIP_SUB
	addiu   $t8, $t8, CLIP_SUB
	beqz    $v0, 0f
	addiu   $t9, $t9, CLIP_SUB           # (delay)
	addiu   $s0, $s0, -15 * CLIP_SUB     # an item: 15 more across
	addiu   $s1, $s1, -15 * CLIP_SUB
	addiu   $s3, $s3, 15 * CLIP_SUB
	addiu   $t8, $t8, 15 * CLIP_SUB
0:	sw      $s0, 32($sp)                 # absmin, absmax by axis (the split tests)
	sw      $s1, 36($sp)
	sw      $s2, 40($sp)
	sw      $s3, 44($sp)
	sw      $t8, 48($sp)
	sw      $t9, 52($sp)
	sra     $s0, $s0, 3                  # the mins' floor (1/8 units)
	sra     $s1, $s1, 3
	sra     $s2, $s2, 3
	addiu   $s3, $s3, 7
	addiu   $t8, $t8, 7
	addiu   $t9, $t9, 7
	sra     $s3, $s3, 3                  # the maxs' ceiling
	sra     $t8, $t8, 3
	sra     $t9, $t9, 3
	sll     $s0, $s0, 16                 # int16, as box16's
	sll     $s1, $s1, 16
	sll     $s2, $s2, 16
	sll     $s3, $s3, 16
	sll     $t8, $t8, 16
	sll     $t9, $t9, 16
	sra     $s0, $s0, 16
	sra     $s1, $s1, 16
	sra     $s2, $s2, 16
	sra     $s3, $s3, 16
	sra     $t8, $t8, 16
	sra     $t9, $t9, 16
	lw      $t4, TW_COUNT($a2)
	lw      $t3, TW_HEAD($a2)
	lw      $t0, TW_NEXT($a2)
	lw      $t1, TW_SLOTS($a2)
	lw      $t2, TW_LIST($a2)
	lhu     $v0, 0($t4)                  # the root's subtree's count
	move    $a2, $zero                   # (the bench: the slots tested)
	addiu   $a3, $a1, 128                # out's end
	beqz    $v0, 8f                      # none in the list
	move    $t6, $zero                   # (delay) the stack: empty
	sb      $zero, 16($sp)               # the root
	li      $t6, 1

1:	addiu   $t6, $t6, -1                 # a node off the stack
11:	addu    $v0, $sp, $t6
	lbu     $t5, 16($v0)                 # a
	lui     $at, %hi(clip_areas)
	sll     $v0, $t5, 3
	addu    $at, $at, $v0
	lw      $t7, %lo(clip_areas)($at)    # dist | flags << 16 | child1 << 24
	nop
	srl     $at, $t7, 16
	andi    $v1, $at, 0x80
	bnez    $v1, 3f                      # a leaf: no children
	andi    $at, $at, 1                  # (delay) its axis: y?
	sll     $at, $at, 2
	addu    $at, $sp, $at                # the frame's box + axis x 4
	lw      $v0, 32($at)                 # absmin[axis]
	sll     $v1, $t7, 16
	sra     $v1, $v1, 16                 # dist (int16)
	srl     $t7, $t7, 24                 # child1
	slt     $v0, $v0, $v1                # absmin[axis] < dist
	beqz    $v0, 2f
	sll     $v0, $t7, 1                  # (delay)
	addu    $v0, $t4, $v0
	lhu     $v0, 0($v0)                  # count[child1]
	nop
	beqz    $v0, 2f
	addu    $v0, $sp, $t6                # (delay)
	sb      $t7, 16($v0)                 # pushed: children[1]
	addiu   $t6, $t6, 1
2:	lw      $v0, 44($at)                 # absmax[axis]
	nop
	slt     $v0, $v1, $v0                # absmax[axis] > dist
	beqz    $v0, 3f
	addiu   $t7, $t5, 1                  # (delay) children[0]: the next node
	sll     $v0, $t7, 1
	addu    $v0, $t4, $v0
	lhu     $v0, 0($v0)                  # count[a + 1]
	nop
	beqz    $v0, 3f
	addu    $v0, $sp, $t6                # (delay)
	sb      $t7, 16($v0)                 # pushed: on top
	addiu   $t6, $t6, 1

3:	sll     $v0, $t5, 1                  # the node's slots
	addu    $v0, $t3, $v0
	lhu     $v1, 0($v0)                  # l = head[a] (slot + 1)
	nop
	beqz    $v1, 7f
	sll     $v0, $v1, 4                  # (delay: the loop's first)

4:	addu    $v0, $t1, $v0                # the slot, at -16 (v0 = l x 16)
	.if ASM_BENCH
	addiu   $a2, $a2, 1
	.endif
	lh      $t7, -16+6($v0)              # its maxs x
	lh      $at, -16+0($v0)              # its mins x
	slt     $t7, $t7, $s0                # e past it
	bnez    $t7, 6f
	slt     $at, $s3, $at                # (delay) e short of it
	bnez    $at, 6f
	lh      $t7, -16+8($v0)              # (delay) y
	lh      $at, -16+2($v0)
	slt     $t7, $t7, $s1
	bnez    $t7, 6f
	slt     $at, $t8, $at                # (delay)
	bnez    $at, 6f
	lh      $t7, -16+10($v0)             # (delay) z
	lh      $at, -16+4($v0)
	slt     $t7, $t7, $s2
	bnez    $t7, 6f
	slt     $at, $t9, $at                # (delay)
	bnez    $at, 6f
	sltu    $at, $a1, $a3                # (delay) room in out?
	beqz    $at, 6f
	sll     $v0, $v1, 1                  # (delay)
	addu    $v0, $t2, $v0
	lhu     $v0, -2($v0)                 # its edict
	addiu   $a1, $a1, 2
	sh      $v0, -2($a1)
6:	sll     $v0, $v1, 1
	addu    $v0, $t0, $v0
	lhu     $v1, -2($v0)                 # l = next[l - 1]
	nop
	bnez    $v1, 4b
	sll     $v0, $v1, 4                  # (delay: the loop's first)

7:	bnez    $t6, 11b                     # nodes left
	addiu   $t6, $t6, -1                 # (delay: 1's first; done: unused)

8:
	.if ASM_BENCH
	lui     $at, %hi(ai_tr_n)            # ai_tr_n[AI_TR_OTHER]: the slots tested
	lw      $v0, %lo(ai_tr_n)($at)
	nop
	addu    $v0, $v0, $a2
	sw      $v0, %lo(ai_tr_n)($at)
	.endif
	addiu   $v0, $a3, -128               # out
	subu    $v0, $a1, $v0
	sra     $v0, $v0, 1                  # the edicts written
	lw      $s0, 0($sp)
	lw      $s1, 4($sp)
	lw      $s2, 8($sp)
	lw      $s3, 12($sp)
	jr      $ra
	addiu   $sp, $sp, 64
	.size   touch_gather, . - touch_gather
