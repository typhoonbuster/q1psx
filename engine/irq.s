# q1psx — interrupts without the kernel's dispatch, hand-written MIPS
# assembly (docs/porting-notes.md §81.1).
#
# PSn00bSDK's psxetc reaches its callbacks through the BIOS: the kernel's
# exception handler saves every register into its thread block, runs its
# priority chains, then jumps to the custom exit (HookEntryInt) that calls
# the callbacks, and ReturnFromException restores the thread block: ~2.7 k
# cycles an interrupt on PCSX-Redux (porting-notes §25.7). Here the RAM
# exception vector (0x80000080) jumps to q_exc instead:
#   - an interrupt: the registers a C callback may change saved in q_save,
#     the callbacks run on q_isr_stack with the program's gp, as psxetc's
#     _global_isr runs them (each pending, enabled source acknowledged,
#     then its callback; again until none is pending; the game's sources
#     tested first, by their index);
#   - anything else (a syscall: EnterCriticalSection..., a break): on to
#     the kernel's own handler, through its vector's four words copied in
#     q_bios_vec (k0 and k1 alone touched before).
# The C half (engine/irq.c) replaces psxetc's API: InterruptCallback,
# DMACallback, ResetCallback...; q_dma_isr is psxetc's DMA master handler.
#
# A GTE command the interrupt landed on has run already: the return skips
# it, as the kernel's handler does (psx-spx, "Interrupts vs GTE Commands").

	.set noreorder
	.set noat

.set I_STAT,    0x1070
.set I_MASK,    0x1074
.set DICR,      0x10f4
.set SIO_DATA,  0x1040
.set SIO_CTRL,  0x104a
.set TIMER2,    0x1120

# pad.c's pad_poll_t (§81.4)
.set PP_BUSY,   0
.set PP_I,      1
.set PP_N,      2
.set PP_T,      4
.set PP_SEL,    6
.set PP_RX,     8
.set PP_TX,     17

.ifndef IRQTIME
.set IRQTIME, 0
.endif
.set Q_ISR_STACK, 1024                   # (500 B reached on the benches: irq.c)

	.section .bss.q_irq, "aw", @nobits
	.align  3
	.global q_irq_handlers, q_dma_handlers, q_isr_stack
.if IRQTIME
	.global q_irq_ticks, q_irq_count
.endif
.if IRQTIME
q_irq_ticks:                             # (the bench's IRQTIME: timer 2's ticks in
	.space  4                            #  the handler, its entries)
q_irq_count:
	.space  4
.endif
q_irq_handlers:                          # IRQ 0..10 callbacks
	.space  11 * 4
q_dma_handlers:                          # DMA channel 0..6 callbacks
	.space  7 * 4
q_save:                                  # the interrupted registers, EPC
	.space  (23 + IRQTIME) * 4           # (+88; +92: IRQTIME's start)
	.align  3
q_isr_stack:
	.space  Q_ISR_STACK
q_isr_top:

	.section .data.q_irq, "aw", @progbits
	.align  2
	.global q_bios_vec
q_bios_vec:                              # the kernel's vector, run from here
	.word   0, 0, 0, 0                   # (irq.c copies it; nop until then)

	.section .text.q_exc, "ax", @progbits
	.global q_exc, q_vec_tpl
	.type   q_exc, @function

# the vector's four words (irq.c copies them to 0x80000080): q_exc's first
# instruction, Cause's read, in the jump's delay slot (jr has its target)
q_vec_tpl:
	lui     $k0, %hi(q_exc)
	addiu   $k0, $k0, %lo(q_exc)
	jr      $k0
	mfc0    $k0, $13                     # Cause

q_exc:                                   # (k0 = Cause: the vector's slot)
	lui     $k1, %hi(q_save)
	andi    $k0, $k0, 0x7c               # ExcCode x 4: 0, an interrupt
	beqz    $k0, q_irq
	addiu   $k1, $k1, %lo(q_save)
	j       q_bios_vec                   # else the kernel's handler
	nop

# An interrupt. The pad's port alone (SIO0, 9 a frame, §81.4) with one of
# the poll's middle bytes takes the short way: five registers kept, the
# answer stored and the next byte sent here, as pad.c's poll_isr does. The
# id byte, the last one and every other source take the dispatch.
q_irq:
	sw      $at,  0($k1)
	sw      $v0,  4($k1)
	sw      $v1,  8($k1)
	sw      $a0, 12($k1)
	sw      $a1, 16($k1)
	lui     $at, 0x1f80                  # the I/O registers
.if IRQTIME
	sw      $a2, 20($k1)                 # (the short way's start: a2; q_full
	lhu     $a2, TIMER2($at)             #  keeps a2 again, unchanged till there)
	nop
	sw      $a2, 92($k1)
	lw      $a2, 20($k1)
.endif
	lhu     $v0, I_STAT($at)
	lhu     $v1, I_MASK($at)
	lui     $a0, %hi(pad_poll)
	and     $v0, $v0, $v1
	xori    $v0, $v0, 0x0080             # 0: SIO0 alone
	bnez    $v0, q_full
	addiu   $a0, $a0, %lo(pad_poll)
	lbu     $v0, PP_BUSY($a0)
	lbu     $v1, PP_I($a0)
	beqz    $v0, q_full                  # no poll in flight: poll_isr's
	lbu     $v0, PP_N($a0)
	addiu   $v1, $v1, 1                  # i + 1
	slt     $v0, $v1, $v0
	beqz    $v0, q_full                  # the last answer: poll_end's
	xori    $v0, $v1, 2
	beqz    $v0, q_full                  # the id byte: the poll's length
	addu    $v0, $a0, $v1                # (delay: the state + i + 1; the
	lbu     $a1, SIO_DATA($at)           #  answer read only on this way)
	sb      $v1, PP_I($a0)               # i + 1
	sb      $a1, PP_RX - 1($v0)          # rx[i]
	li      $a1, 0xff7f
	sh      $a1, I_STAT($at)             # acknowledged (the dispatch's)
	lbu     $a1, PP_N($a0)
	lbu     $v0, PP_TX($v0)              # tx[i + 1]
	addiu   $a1, $a1, -1
	xor     $v1, $v1, $a1                # 0: the last byte goes: its answer by
	lhu     $a1, PP_SEL($a0)             # the receive interrupt (no /ACK
	bnez    $v1, 1f                      # follows it), the others by /ACK;
	ori     $a1, $a1, 0x1013             # the port's select kept (0x2000:
	xori    $a1, $a1, 0x1800             # port 2, §120): 0x1013 -> 0x0813
1:	sh      $a1, SIO_CTRL($at)           # (the port's flag cleared)
	sb      $v0, SIO_DATA($at)           # the next byte
.if IRQTIME
	lhu     $a1, TIMER2($at)
	lw      $v0, 92($k1)
	lui     $v1, %hi(q_irq_ticks)
	subu    $a1, $a1, $v0
	andi    $a1, $a1, 0xffff
	lw      $v0, %lo(q_irq_ticks)($v1)
	nop
	addu    $v0, $v0, $a1
	sw      $v0, %lo(q_irq_ticks)($v1)
	lw      $v0, %lo(q_irq_count)($v1)
	nop
	addiu   $v0, $v0, 1
	sw      $v0, %lo(q_irq_count)($v1)
.endif
	mfc0    $k0, $14                     # EPC
	lhu     $v0, TIMER2($at)             # (the poll's last byte: timer 2)
	lw      $v1, 0($k0)                  # the instruction there
	sh      $v0, PP_T($a0)
	srl     $v1, $v1, 25
	xori    $v1, $v1, 0x25               # 0: a GTE command, run already
	bnez    $v1, 2f
	lw      $at,  0($k1)
	addiu   $k0, $k0, 4                  # (skipped)
2:	lw      $v0,  4($k1)
	lw      $v1,  8($k1)
	lw      $a0, 12($k1)
	lw      $a1, 16($k1)
	jr      $k0
	rfe

q_full:                                  # (at, v0, v1, a0, a1 kept already)
	mfc0    $v0, $14                     # EPC: kept, as a callback's syscall
	sw      $a2, 20($k1)                 # (EnterCriticalSection...) would
	sw      $v0, 88($k1)                 # take it over, and k0 / k1
	sw      $a3, 24($k1)
	sw      $t0, 28($k1)
	sw      $t1, 32($k1)
	sw      $t2, 36($k1)
	sw      $t3, 40($k1)
	sw      $t4, 44($k1)
	sw      $t5, 48($k1)
	sw      $t6, 52($k1)
	sw      $t7, 56($k1)
	sw      $t8, 60($k1)
	sw      $t9, 64($k1)
	mfhi    $t0                          # (waits for a multiply in flight)
	mflo    $t1
	sw      $gp, 68($k1)
	sw      $sp, 72($k1)
	sw      $ra, 76($k1)
	sw      $t0, 80($k1)                 # hi
	sw      $t1, 84($k1)                 # lo
	lui     $sp, %hi(q_isr_top - 16)     # (o32: the callee's argument area)
	addiu   $sp, $sp, %lo(q_isr_top - 16)
	lui     $gp, %hi(_gp)
	addiu   $gp, $gp, %lo(_gp)

# One source a pass, the status read again after each callback (nothing
# kept over a call: the program's s-registers are the callbacks' to keep).
# The sources the game enables first, by their index (0 the vblank, 2 the
# CD, 3 the DMA, 6 the profile timer, 7 the pad's port: each a test), then
# any other one, the lowest first.
q_pass:
	lui     $t2, 0x1f80                  # the I/O registers
q_pass_1:                                # (a callback's return: t2 set in its slot)
	lhu     $t0, I_STAT($t2)
	lhu     $t1, I_MASK($t2)
	lui     $t9, %hi(q_irq_handlers)     # (q_src's)
	and     $t3, $t0, $t1                # pending and enabled
	beqz    $t3, q_ret
	andi    $t0, $t3, 0x0001
	bnez    $t0, q_src                   # the vblank
	li      $t1, 0 * 4
	andi    $t0, $t3, 0x0004
	bnez    $t0, q_src                   # the CD
	li      $t1, 2 * 4
	andi    $t0, $t3, 0x0008
	bnez    $t0, q_src                   # the DMA
	li      $t1, 3 * 4
	andi    $t0, $t3, 0x0040
	bnez    $t0, q_src                   # timer 2
	li      $t1, 6 * 4
	andi    $t0, $t3, 0x0080
	bnez    $t0, q_src                   # SIO0, the pad
	li      $t1, 7 * 4
	li      $t0, 1                       # another: the lowest
	move    $t1, $zero
1:	and     $t4, $t3, $t0
	bnez    $t4, q_src
	nop
	sll     $t0, $t0, 1
	b       1b
	addiu   $t1, $t1, 4
q_src:                                   # t0 = its bit, t1 = 4 x its index
	addu    $t9, $t9, $t1
	lw      $t9, %lo(q_irq_handlers)($t9)
	nor     $t0, $zero, $t0
	beqz    $t9, q_pass                  # (no callback: acknowledged alone)
	sh      $t0, I_STAT($t2)             # acknowledged, then its callback
	jalr    $t9
	nop
	b       q_pass_1
	lui     $t2, 0x1f80                  # (delay: q_pass's first)

q_ret:
	lui     $k1, %hi(q_save)             # (k0 / k1 again: see q_full)
	addiu   $k1, $k1, %lo(q_save)
.if IRQTIME
	lui     $t0, 0x1f80
	lhu     $t0, TIMER2($t0)
	lw      $t1, 92($k1)
	lui     $t2, %hi(q_irq_ticks)
	subu    $t0, $t0, $t1
	andi    $t0, $t0, 0xffff
	lw      $t1, %lo(q_irq_ticks)($t2)
	lw      $t3, %lo(q_irq_count)($t2)
	addu    $t1, $t1, $t0
	addiu   $t3, $t3, 1
	sw      $t1, %lo(q_irq_ticks)($t2)
	sw      $t3, %lo(q_irq_count)($t2)
.endif
	lw      $k0, 88($k1)                 # EPC
	lw      $t0, 80($k1)
	lw      $t2, 0($k0)                  # the instruction there
	lw      $t1, 84($k1)
	srl     $t2, $t2, 25
	mthi    $t0
	mtlo    $t1
	xori    $t2, $t2, 0x25               # 0: a GTE command, run already
	bnez    $t2, 1f
	lw      $at,  0($k1)
	addiu   $k0, $k0, 4                  # (skipped)
1:	lw      $v0,  4($k1)
	lw      $v1,  8($k1)
	lw      $a0, 12($k1)
	lw      $a1, 16($k1)
	lw      $a2, 20($k1)
	lw      $a3, 24($k1)
	lw      $t0, 28($k1)
	lw      $t1, 32($k1)
	lw      $t2, 36($k1)
	lw      $t3, 40($k1)
	lw      $t4, 44($k1)
	lw      $t5, 48($k1)
	lw      $t6, 52($k1)
	lw      $t7, 56($k1)
	lw      $t8, 60($k1)
	lw      $t9, 64($k1)
	lw      $gp, 68($k1)
	lw      $sp, 72($k1)
	lw      $ra, 76($k1)
	jr      $k0
	rfe
	.size   q_exc, . - q_exc

# -----------------------------------------------------------------------------
# q_dma_isr: IRQ_DMA's callback while a DMA callback is set (psxetc's
# _global_dma_handler): each channel whose flag DICR raised, acknowledged
# (its flag written back, the enables kept), then its callback; again until
# none is raised. A C function: keeps s0-s3 on its frame.
	.section .text.q_dma_isr, "ax", @progbits
	.global q_dma_isr
	.type   q_dma_isr, @function
q_dma_isr:
	addiu   $sp, $sp, -40
	sw      $ra, 32($sp)
	sw      $s0, 28($sp)
	sw      $s1, 24($sp)
	sw      $s2, 20($sp)
	sw      $s3, 16($sp)
	lui     $s0, 0x1f80
	lw      $t0, DICR($s0)
1:	lui     $s2, %hi(q_dma_handlers)
	srl     $s1, $t0, 24
	andi    $s1, $s1, 0x7f               # the channels' flags
	beqz    $s1, 4f
	addiu   $s2, $s2, %lo(q_dma_handlers)
	lui     $s3, 0x0100                  # channel 0's flag
2:	andi    $t2, $s1, 1
	beqz    $t2, 3f
	srl     $s1, $s1, 1
	lw      $t0, DICR($s0)
	lw      $t9, 0($s2)
	sll     $t0, $t0, 8                  # the enables (bits 0-23) kept
	srl     $t0, $t0, 8
	or      $t0, $t0, $s3                # and this flag written back: cleared
	beqz    $t9, 3f
	sw      $t0, DICR($s0)
	jalr    $t9
	nop
3:	sll     $s3, $s3, 1
	bnez    $s1, 2b
	addiu   $s2, $s2, 4
	b       1b
	lw      $t0, DICR($s0)               # what came meanwhile
4:	lw      $ra, 32($sp)
	lw      $s0, 28($sp)
	lw      $s1, 24($sp)
	lw      $s2, 20($sp)
	lw      $s3, 16($sp)
	jr      $ra
	addiu   $sp, $sp, 40
	.size   q_dma_isr, . - q_dma_isr
