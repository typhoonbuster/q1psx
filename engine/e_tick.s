# q1psx — SV_Physics's walk over the edicts, hand-written MIPS assembly
# (docs/porting-notes.md §72). C ABI:
#
#   void ents_tick_loop(uint32_t *active, const uint32_t *pushers, uint32_t half, int wdt);
#
# ents_tick's loop: the edicts of the active bitmap this tick runs (half:
# the world's half's bits, | pushers: the movers each tick), in index order,
# each one's physics by its kind (engine/ents.c ents_tick_loop_c is its C,
# the C engine's; the benches' frames are the C's, hash for hash):
# - a forced retouch first (ent_force_retouch: not a mover's);
# - a mover: ents_physics_pusher with game_dt 1 (then the world's, wdt);
# - a monster or what it throws (mdef, or a streamed model): a body past its
#   end removed; a monster whose model is not in, paused; a step mover's
#   physics (ai_physics_step), not called when it has nothing to do (on the
#   ground, flying or swimming, no think due, still where its contents were
#   taken); the toss, fly and bounce movers' (ai_physics_toss); else its
#   think;
# - another edict: its think, then a toss mover's fall (items_physics);
# - then, asleep (out of use; or no think, no move, not a monster's or a
#   thrown model's fall, not a body), its bit cleared in active (read from
#   memory: the calls wake edicts). The word's bits are the snapshot taken
#   when the word is reached, as the C's.
# Each edict's fields are read again after any call (a call can remove it,
# change its kind, move it). The lowest set bit by a de Bruijn product (the
# multiply's latency spent clearing it and finding the table).

	.set noreorder
	.set noat

	.include "p_defs.inc"

.set MT_STEP,   4                        # MOVETYPE_*
.set MT_FLY,    5
.set MT_TOSS,   6
.set MT_PUSH,   7
.set MT_NOCLIP, 8
.set FL_GROUND_FLY_SWIM, 515             # FL_ONGROUND | FL_FLY | FL_SWIM
.set FL_ONGROUND, 512

	.section .text.ents_tick_loop, "ax", @progbits
	.global ents_tick_loop
	.type   ents_tick_loop, @function
ents_tick_loop:                          # a0 = active, a1 = pushers, a2 = half, a3 = wdt
	addiu   $sp, $sp, -56                # o32's argument area (16 bytes: a callee
	sw      $ra, 16($sp)                 #  may keep its a0-a3 there; the thinks
	sw      $s0, 20($sp)                 #  ents_run_think jumps to as its tail
	sw      $s1, 24($sp)                 #  call run on this frame: a varargs one
	sw      $s2, 28($sp)                 #  wrote a1-a3 over the s registers kept
	sw      $s3, 32($sp)                 #  at 4-12), then ra, s0-s8
	sw      $s4, 36($sp)
	sw      $s5, 40($sp)
	sw      $s6, 44($sp)
	sw      $s7, 48($sp)
	sw      $s8, 52($sp)
	move    $s0, $a0                     # active[w]'s address
	move    $s1, $a1                     # pushers[w]'s
	move    $s2, $a2                     # half
	move    $s3, $a3                     # wdt
	move    $s5, $zero                   # w x 32
	lui     $t0, %hi(edicts)
	lw      $s6, %lo(edicts)($t0)

1:	lw      $t0, 0($s0)                  # a word
	lw      $t1, 0($s1)
	nop
	or      $t1, $t1, $s2
	and     $s4, $t0, $t1                # its bits this tick
	beqz    $s4, 90f
	nop

2:	negu    $t0, $s4                     # an edict: the lowest bit
	and     $s8, $s4, $t0
	lui     $t1, 0x077c
	ori     $t1, $t1, 0xb531
	multu   $s8, $t1                     # (de Bruijn)
	addiu   $t2, $s4, -1
	and     $s4, $s4, $t2                # (cleared from the snapshot)
	lui     $t3, %hi(ents_ctz_tab)
	addiu   $t3, $t3, %lo(ents_ctz_tab)
	mflo    $t2
	srl     $t2, $t2, 27
	addu    $t2, $t3, $t2
	lbu     $t2, 0($t2)                  # its bit's number
	lui     $t1, %hi(ent_force_retouch)
	addu    $t2, $s5, $t2                # its index
	sll     $t3, $t2, 8                  # x 384 (sizeof (edict_t)): 256 + 128
	sll     $t2, $t2, 7
	addu    $t2, $t3, $t2
	addu    $s7, $s6, $t2                # e
	lbu     $t0, E_INUSE($s7)
	lw      $t1, %lo(ent_force_retouch)($t1)
	beqz    $t0, 80f                     # out of use: asleep
	lbu     $t2, E_MOVETYPE($s7)         # (delay)
	beqz    $t1, 3f                      # no retouch
	li      $t3, MT_PUSH                 # (delay)
	beq     $t2, $t3, 3f                 # (not a mover's)
	nop
	jal     ents_touch_links             # force retouch even for stationary
	move    $a0, $s7
	lbu     $t0, E_INUSE($s7)
	lbu     $t2, E_MOVETYPE($s7)
	beqz    $t0, 80f
	li      $t3, MT_PUSH                 # (delay)

3:	bne     $t2, $t3, 4f
	lw      $t4, E_MDEF($s7)             # (delay)
	lui     $t5, %hi(game_dt)            # a mover: game_dt 1
	li      $t6, 1
	sw      $t6, %lo(game_dt)($t5)
	jal     ents_physics_pusher
	move    $a0, $s7                     # (delay)
	lui     $t5, %hi(game_dt)
	b       70f
	sw      $s3, %lo(game_dt)($t5)       # (delay) the world's again

4:	lh      $t5, E_MASSET($s7)
	bnez    $t4, 5f                      # a monster
	nop
	bltz    $t5, 60f                     # neither monster nor thrown model
	nop
	b       7f                           # a thrown model: no body, no pause
	nop
5:	lh      $t6, E_HEALTH($s7)
	nop
	bgtz    $t6, 6f                      # alive
	lui     $t7, %hi(ent_time)           # (delay)
	lw      $t7, %lo(ent_time)($t7)      # a body: its end?
	lw      $t8, E_PAIN_FINISHED($s7)
	nop
	subu    $t7, $t7, $t8
	bltz    $t7, 6f
	nop
	jal     ent_remove                   # a body's end
	move    $a0, $s7
	b       70f
	nop
6:	bltz    $t5, 7f                      # no model of its own: not paused
	nop
	jal     stream_model
	move    $a0, $t5                     # (delay)
	beqz    $v0, 70f                     # its model not in: paused
	nop
	lbu     $t2, E_MOVETYPE($s7)         # (the call took t2)
7:	li      $t3, MT_STEP
	bne     $t2, $t3, 20f
	lw      $t0, E_FLAGS($s7)            # (delay) a step mover:
	lui     $t1, %hi(ent_time)
	lw      $t1, %lo(ent_time)($t1)
	andi    $t0, $t0, FL_GROUND_FLY_SWIM
	beqz    $t0, 10f                     # falling: the physics
	lw      $t3, E_NEXTTHINK($s7)        # (delay)
	nop
	blez    $t3, 8f                      # no think
	slt     $t4, $t1, $t3                # (delay) ent_time < nextthink: not due
	beqz    $t4, 10f                     # due
	nop
8:	lb      $t5, E_WATERTYPE($s7)
	lw      $t6, E_ORIGIN+0($s7)
	beqz    $t5, 10f                     # no contents taken yet
	lw      $t7, E_WATER_AT+0($s7)       # (delay)
	lw      $t8, E_ORIGIN+4($s7)
	bne     $t6, $t7, 10f                # moved
	lw      $t9, E_WATER_AT+4($s7)       # (delay)
	lw      $t6, E_ORIGIN+8($s7)
	bne     $t8, $t9, 10f
	lw      $t7, E_WATER_AT+8($s7)       # (delay)
	nop
	beq     $t6, $t7, 70f                # nothing to do
	nop
10:	jal     ai_physics_step
	move    $a0, $s7
	b       70f
	nop
20:	sltiu   $t3, $t2, MT_FLY             # toss, fly, bounce, a missile: their physics
	bnez    $t3, 30f                     # (none, walk: the think)
	li      $t3, MT_PUSH                 # (delay)
	beq     $t2, $t3, 30f
	li      $t3, MT_NOCLIP               # (delay)
	beq     $t2, $t3, 30f
	nop
	jal     ai_physics_toss
	move    $a0, $s7
	b       70f
	nop
30:	jal     ents_run_think
	move    $a0, $s7
	b       70f
	nop

60:	jal     ents_run_think               # another edict: the think, then the move
	move    $a0, $s7
	lbu     $t0, E_INUSE($s7)
	lbu     $t2, E_MOVETYPE($s7)
	beqz    $t0, 80f
	li      $t3, MT_TOSS                 # (delay)
	bne     $t2, $t3, 70f
	nop
	jal     items_physics
	move    $a0, $s7

70:	lbu     $t0, E_INUSE($s7)            # asleep?
	lw      $t1, E_NEXTTHINK($s7)
	beqz    $t0, 80f                     # out of use
	lw      $t2, E_MOVE_TICKS($s7)       # (delay)
	bgtz    $t1, 85f                     # a think
	lw      $t3, E_MDEF($s7)             # (delay)
	bnez    $t2, 85f                     # a move
	lh      $t4, E_MASSET($s7)           # (delay)
	bnez    $t3, 72f                     # a monster's,
	nop
	bltz    $t4, 74f                     # (neither)
	nop
72:	lbu     $t5, E_MOVETYPE($s7)         # or a thrown model's: falling?
	lw      $t6, E_FLAGS($s7)
	sltiu   $t7, $t5, MT_STEP
	bnez    $t7, 74f
	li      $t7, MT_PUSH                 # (delay)
	beq     $t5, $t7, 74f
	andi    $t6, $t6, FL_ONGROUND        # (delay)
	beqz    $t6, 85f                     # falling: on
	nop
74:	beqz    $t3, 80f                     # not a monster's: asleep
	lh      $t5, E_HEALTH($s7)           # (delay)
	nop
	blez    $t5, 85f                     # a body: till its end
	nop
80:	lw      $t0, 0($s0)                  # asleep: its bit cleared
	nor     $t1, $s8, $zero
	and     $t0, $t0, $t1
	sw      $t0, 0($s0)
85:	bnez    $s4, 2b                      # the word's next
	nop

90:	addiu   $s5, $s5, 32                 # the next word
	addiu   $s0, $s0, 4
	li      $t0, 1024                    # (ENT_WORDS x 32)
	bne     $s5, $t0, 1b
	addiu   $s1, $s1, 4                  # (delay)

	lw      $ra, 16($sp)
	lw      $s0, 20($sp)
	lw      $s1, 24($sp)
	lw      $s2, 28($sp)
	lw      $s3, 32($sp)
	lw      $s4, 36($sp)
	lw      $s5, 40($sp)
	lw      $s6, 44($sp)
	lw      $s7, 48($sp)
	lw      $s8, 52($sp)
	jr      $ra
	addiu   $sp, $sp, 56
	.size   ents_tick_loop, . - ents_tick_loop
