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
#                r_state. Then on to the marking (r_world.s mark_pvs),
#                which returns to mark_done. Clobbers what mark_pvs may
#                (t0-t7, t9, a1-a3, v0, v1, at, hi / lo); keeps a0, ra, the
#                s registers, fp, t8, gp. The same set: 17 instructions and
#                the multiply's wait.
#   zone_patch   C ABI: a chunk's patch words into the world nodes' face
#                blocks (formats/qworld.h QZP_*): a0 = the first node, a1 =
#                the words, a2 = their count, a3 = the chunk's records
#   gamma_light_records
#                C ABI (engine/gamma.h): the Brightness's light on records,
#                a chunk's as its sectors come in (engine/zone.c relight),
#                the world's at its load, the brush models' pieces
#
# The C engine (CMake Q1PSX_ENGINE_C: ZONE_C = 1) takes zone_patch and
# gamma_light_records in C (engine/zone.c, engine/gamma.c); zone_mark is the
# renderer's (r_world.s has no C).

	.set noreorder
	.set noat

	.include "r_defs.inc"

.ifndef ZONE_C
.set ZONE_C, 0
.endif

.set ZR_LEAVES,  0       # zone.h zone_rt_t
.set ZR_LEAFSET, 4
.set ZR_SET,     8

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
	sll     $t1, $t1, 1                  #  waits for the product), a halfword a leaf
	addu    $t2, $t2, $t1
	lhu     $t2, 0($t2)                  # the leaf's set
	nop
	bne     $t2, $t4, zm_enter
	nop
	j       mark_pvs                     # the same set: on to the marking
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
	lw      $t1, RS_VERTPOOL($s0)        # (another chunk current: its pool)
	lui     $t8, 0x1f80                  # (the scratchpad's base, the walk's t8)
	addiu   $sp, $sp, 32
	j       mark_pvs
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

# gamma_light_records: a0 = the first record, a1 = the end, a2 = the most
# records, a3 = the curve (256 bytes); v0 = where it stopped. Each whole
# record before a1 (until one of size 0): its light bytes through the
# curve, a word at a time over their run padded to 4 (the padding's zeros
# stay: the curve keeps 0), summed; its average light, (sum + 2n) / 4n by
# the high word of a product with 2^32 / 4n rounded up (gamma.c
# light_recip: exact for every sum of n <= QW_MAX_VERTS bytes), 63 at
# most, into its flags' bits 2-7 (QWF_SKY and QWF_LIQUID kept). A word:
# 27 instructions; a record's own: 36 and the product's wait.
	.section .text.gamma_light_records, "ax", @progbits
	.global gamma_light_records
	.type   gamma_light_records, @function
gamma_light_records:
	lui     $t9, %hi(light_recip)
	addiu   $t9, $t9, %lo(light_recip)
glr_rec:
	beqz    $a2, glr_done
	addiu   $t0, $a0, 20                 # (the header's end)
	sltu    $t0, $a1, $t0
	bnez    $t0, glr_done                # no whole header before the end
	lhu     $t1, R_SIZE($a0)
	lbu     $t2, R_NVERTS($a0)
	beqz    $t1, glr_done                # size 0: the stream's end
	addu    $t3, $a0, $t1                # the next record
	sltu    $t0, $a1, $t3
	bnez    $t0, glr_done                # not whole before the end
	lhu     $t4, R_POLYS($a0)
	addiu   $t5, $t2, 3
	srl     $t5, $t5, 2                  # the light bytes' words
	sll     $t6, $t5, 2
	addu    $t4, $t4, $a0
	subu    $t4, $t4, $t6                # the light bytes
	beqz    $t5, glr_avg
	move    $v1, $zero                   # the sum
glr_word:
	lw      $t6, 0($t4)
	addiu   $t5, $t5, -1
	andi    $t7, $t6, 0xff
	srl     $t8, $t6, 8
	andi    $t8, $t8, 0xff
	addu    $t7, $t7, $a3
	addu    $t8, $t8, $a3
	lbu     $t7, 0($t7)
	lbu     $t8, 0($t8)
	srl     $t0, $t6, 16
	andi    $t0, $t0, 0xff
	srl     $t6, $t6, 24
	addu    $t0, $t0, $a3
	addu    $t6, $t6, $a3
	lbu     $t0, 0($t0)
	lbu     $t6, 0($t6)
	addu    $v1, $v1, $t7
	addu    $v1, $v1, $t8
	addu    $v1, $v1, $t0
	addu    $v1, $v1, $t6
	sll     $t8, $t8, 8
	sll     $t0, $t0, 16
	sll     $t6, $t6, 24
	or      $t7, $t7, $t8
	or      $t0, $t0, $t6
	or      $t7, $t7, $t0
	sw      $t7, 0($t4)
	bnez    $t5, glr_word
	addiu   $t4, $t4, 4
glr_avg:
	sll     $t5, $t2, 2
	addu    $t5, $t5, $t9
	lw      $t5, 0($t5)                  # 2^32 / 4n rounded up (n = 0: 0)
	sll     $t6, $t2, 1
	addu    $t6, $t6, $v1                # sum + 2n
	multu   $t6, $t5
	lbu     $t7, R_FLAGS($a0)
	addiu   $a2, $a2, -1
	andi    $t7, $t7, 3                  # QWF_SKY | QWF_LIQUID
	mfhi    $t6                          # the average (waits for the product)
	sltiu   $t0, $t6, 64
	bnez    $t0, 1f
	sll     $t6, $t6, 2
	li      $t6, 63 << 2
1:	or      $t7, $t7, $t6
	sb      $t7, R_FLAGS($a0)
	b       glr_rec
	move    $a0, $t3
glr_done:
	jr      $ra
	move    $v0, $a0
	.size   gamma_light_records, . - gamma_light_records
.endif
