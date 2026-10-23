# q1psx — the render lists' cull: the next alias-model edict whose leaf the
# PVS sees this frame, hand-written MIPS assembly (docs/porting-notes.md
# §72). C ABI:
#
#   int ents_seen_streamed(const uint16_t *list, int n, int from, uint32_t vis);
#   int ents_seen_items(const uint16_t *list, int n, int from, uint32_t vis);
#
# list: edict indices (ents_streamed_list / ents_model_list), n of them;
# the first position from `from` on whose edict is in use, has its model
# (masset >= 0, or item_model >= 0) and whose centre's leaf (leaves[0],
# found by ent_model_leaf when not yet) is marked with `vis` (the frame's
# PVS stamp, r_state.visframe's low half); n if none. monsters_render and
# items_render walk their list with it: most of a map's monsters and
# items are out of the PVS, and each cost the C loop a call and its
# tests. The leaf is looked for exactly when the C loop would (in use,
# with a model), so the edicts keep the same leaves.
#
# engine/ents.c ents_seen_c is its C (the C engine's; the bench compares
# the two each frame of its fight: the seendiff line).

	.set noreorder
	.set noat

	.include "p_defs.inc"
	.include "r_defs.inc"                # (L_VISFRAME)

	.section .text.ents_seen, "ax", @progbits
	.global ents_seen_streamed
	.type   ents_seen_streamed, @function
	.global ents_seen_items
	.type   ents_seen_items, @function
ents_seen_items:
	b       0f
	li      $t9, -1                      # (delay) the items' model: item_model
ents_seen_streamed:
	li      $t9, 0                       # the streamed one: masset
0:	addiu   $sp, $sp, -48                # o32's argument area (16 bytes: the C
	sw      $ra, 16($sp)                 #  callee may keep its a0-a3 there), then
	sw      $s0, 20($sp)                 #  ra, s0-s5
	sw      $s1, 24($sp)
	sw      $s2, 28($sp)
	sw      $s3, 32($sp)
	sw      $s4, 36($sp)
	sw      $s5, 40($sp)
	sll     $t0, $a2, 1
	addu    $s0, $a0, $t0                # the entry: list + from
	sll     $t0, $a1, 1
	addu    $s1, $a0, $t0                # the end: list + n
	move    $s2, $a0                     # list
	move    $s3, $a3                     # vis
	move    $s4, $t9                     # -1: the items' model
	lui     $t0, %hi(edicts)
	lw      $s5, %lo(edicts)($t0)

1:	beq     $s0, $s1, 9f                 # past the list
	lhu     $t0, 0($s0)                  # (delay) the edict's index
	addiu   $s0, $s0, 2
	sll     $t1, $t0, 8                  # x 384 (sizeof (edict_t)): 256 + 128
	sll     $t0, $t0, 7
	addu    $t0, $t1, $t0
	addu    $a0, $s5, $t0                # e
	lbu     $t0, E_INUSE($a0)
	lh      $t1, E_MASSET($a0)
	lw      $t2, E_ITEM_MODEL($a0)
	beqz    $t0, 1b                      # not in use
	lw      $v0, E_LEAVES($a0)           # (delay) its centre's leaf
	xor     $t3, $t2, $t1                # the model asked for: the items'
	and     $t3, $t3, $s4                #   or the streamed one
	xor     $t3, $t3, $t1
	bltz    $t3, 1b                      # none
	lhu     $t0, L_VISFRAME($v0)         # (delay; no leaf yet: a read at 6, harmless)
	beqz    $v0, 3f                      # its leaf not found yet
	nop
2:	bne     $t0, $s3, 1b                 # the PVS does not see it
	nop
	subu    $v0, $s0, $s2                # this one: (entry + 1 - list) / 2 - 1
	sra     $v0, $v0, 1
	b       8f
	addiu   $v0, $v0, -1

3:	jal     ent_model_leaf               # (ents.c)
	nop
	beqz    $v0, 1b                      # outside the world
	nop
	lhu     $t0, L_VISFRAME($v0)
	b       2b                           # (not loaded in the delay slot: 2's first
	nop                                  #  instruction reads it)

9:	subu    $v0, $s1, $s2                # n
	sra     $v0, $v0, 1
8:	lw      $ra, 16($sp)
	lw      $s0, 20($sp)
	lw      $s1, 24($sp)
	lw      $s2, 28($sp)
	lw      $s3, 32($sp)
	lw      $s4, 36($sp)
	lw      $s5, 40($sp)
	jr      $ra
	addiu   $sp, $sp, 48
	.size   ents_seen_items, . - ents_seen_items
	.size   ents_seen_streamed, . - ents_seen_streamed
