# q1psx — player movement, hand-written MIPS assembly (R4.3).
#
# Port of engine/pmove.c, which is the specification: same algorithm, same
# integer operations, bit-identical results (the bench build links both and
# compares them tick by tick). Quake's player physics over a tick of dt
# 60ths of a second (CMD_DT, 1 .. 6: the game's tick rate):
# categorize (on the ground?), wish direction, then SV_ClientThink's intent
# (SV_WaterJump / SV_WaterMove / friction and acceleration), PlayerPreThink's
# drag, water jump and PlayerJump, SV_CheckWater, gravity, speed cap, then
# SV_FlyMove's slide (slide_move) and SV_WalkMove's step-up over ledges of
# 18 units.
#
#   pmove_step(a0 = pmove_t *, a1 = const pmove_cmd_t *)
#
# The pmove_t is copied to the scratchpad (PM_*, same layout) on entry and
# back on exit; every intermediate vector lives there too (p_defs.inc).
# s7 = scratchpad throughout. clip_trace (engine/clip.s) preserves s0-s7 and
# only touches the scratchpad below 0x040 plus the trace_t it is given.
#
# Internal routines take arguments in a0-a3 and may clobber t0-t9, a0-a2,
# v0, v1; slide_move also uses s2-s6.

	.set noreorder
	.set noat

	.include "gtereg.inc"
	.include "p_defs.inc"

.set PM_HZ,           60
.set PM_MAXSPEED,    320
.set PM_ACCEL,        10
.set PM_FRICTION,      4
.set PM_STOPSPEED,   100
.set PM_GRAVITY,     800
.set PM_JUMPSPEED,   270
.set PM_STEPSIZE,     18
.set PM_MAXVELOCITY, 2000
.set FLOOR_NZ,      2867     # 0.7 x 4096
.set STOP_V,           2
.set NOCLIP_SPEED,     4
.set PM_AXIS,        128     # full stick deflection (pmove.h)
.set PM_MINS_Z,      -24     # the box: SV_CheckWater's feet / waist / eyes
.set PM_MAXS_Z,       32
.set PM_EYE_HEIGHT,   22
.set PM_WATERJUMP_TICKS, 120 # FL_WATERJUMP's 2 s
.set PM_WATERJUMP_UP, 225
.set PM_SINK,         60     # SV_WaterMove's drift to the bottom, units/s

# VCOPY d, s / VZERO d: int32[3] at scratchpad offsets. VCOPY clobbers t0-t2.
.macro VCOPY d, s
	lw      $t0, \s+0($s7)
	lw      $t1, \s+4($s7)
	lw      $t2, \s+8($s7)
	sw      $t0, \d+0($s7)
	sw      $t1, \d+4($s7)
	sw      $t2, \d+8($s7)
.endm

.macro VZERO d
	sw      $zero, \d+0($s7)
	sw      $zero, \d+4($s7)
	sw      $zero, \d+8($s7)
.endm

# DOT_IN rd, va, na: rd = va (int32[3]) . na (int16[3]); DOT_II for two
# int32[3]. rd must not be t0-t3 (clobbered). Ends HI/LO-safe: a mult / div
# may follow directly.
.macro DOT_IN rd, va, na
	lw      $t0, 0(\va)
	lh      $t1, 0(\na)
	lw      $t2, 4(\va)
	mult    $t0, $t1
	lh      $t3, 2(\na)
	mflo    \rd
	lw      $t0, 8(\va)
	lh      $t1, 4(\na)
	mult    $t2, $t3
	mflo    $t2
	addu    \rd, \rd, $t2
	nop
	mult    $t0, $t1
	mflo    $t2
	addu    \rd, \rd, $t2
	nop
.endm

.macro DOT_II rd, va, vb
	lw      $t0, 0(\va)
	lw      $t1, 0(\vb)
	lw      $t2, 4(\va)
	mult    $t0, $t1
	lw      $t3, 4(\vb)
	mflo    \rd
	lw      $t0, 8(\va)
	lw      $t1, 8(\vb)
	mult    $t2, $t3
	mflo    $t2
	addu    \rd, \rd, $t2
	nop
	mult    $t0, $t1
	mflo    $t2
	addu    \rd, \rd, $t2
	nop
.endm

# PMCOPY to, tb, from, fb: the 21 words of a pmove_t from from(fb) to
# to(tb), unrolled. Clobbers t0-t6.
.macro PMCOPY to, tb, from, fb
	lw      $t0,  \from+0(\fb)
	lw      $t1,  \from+4(\fb)
	lw      $t2,  \from+8(\fb)
	lw      $t3, \from+12(\fb)
	lw      $t4, \from+16(\fb)
	lw      $t5, \from+20(\fb)
	lw      $t6, \from+24(\fb)
	sw      $t0,  \to+0(\tb)
	sw      $t1,  \to+4(\tb)
	sw      $t2,  \to+8(\tb)
	sw      $t3, \to+12(\tb)
	sw      $t4, \to+16(\tb)
	sw      $t5, \to+20(\tb)
	sw      $t6, \to+24(\tb)
	lw      $t0, \from+28(\fb)
	lw      $t1, \from+32(\fb)
	lw      $t2, \from+36(\fb)
	lw      $t3, \from+40(\fb)
	lw      $t4, \from+44(\fb)
	lw      $t5, \from+48(\fb)
	lw      $t6, \from+52(\fb)
	sw      $t0, \to+28(\tb)
	sw      $t1, \to+32(\tb)
	sw      $t2, \to+36(\tb)
	sw      $t3, \to+40(\tb)
	sw      $t4, \to+44(\tb)
	sw      $t5, \to+48(\tb)
	sw      $t6, \to+52(\tb)
	lw      $t0, \from+56(\fb)
	lw      $t1, \from+60(\fb)
	lw      $t2, \from+64(\fb)
	lw      $t3, \from+68(\fb)
	lw      $t4, \from+72(\fb)
	lw      $t5, \from+76(\fb)
	lw      $t6, \from+80(\fb)
	sw      $t0, \to+56(\tb)
	sw      $t1, \to+60(\tb)
	sw      $t2, \to+64(\tb)
	sw      $t3, \to+68(\tb)
	sw      $t4, \to+72(\tb)
	sw      $t5, \to+76(\tb)
	sw      $t6, \to+80(\tb)
.endm

# the drag of PlayerPreThink's WaterMove by tick length (rows: dt 0 .. 6) and
# water level: 1 - 0.8 wl dt / 60, 0.16, truncated (engine/pmove.c water_drag)
	.section .rodata.pm_water_drag, "a", @progbits
	.align  2
pm_water_drag:
	.word   65536, 65536, 65536, 65536
	.word   65536, 64662, 63788, 62914
	.word   65536, 63788, 62040, 60293
	.word   65536, 62914, 60293, 57671
	.word   65536, 62040, 58545, 55050
	.word   65536, 61166, 56797, 52428
	.word   65536, 60293, 55050, 49807
# SV_WaterMove's friction over the tick, 1 - 4 dt / 60, 0.16, rounded (dt 0 .. 6;
# pmove.c water_scale)
pm_water_scale:
	.word   65536, 61167, 56798, 52429, 48060, 43691, 39322
# gravity's midpoint over a tick longer than a 60th: g dt^2 / 2 in pos_acc
# units, 800 x 8 x dt^2 / 120; 0 at dt 1 (pmove.c grav_mid)
pm_grav_mid:
	.word   0, 0, 213, 480, 853, 1333, 1920

	.section .text.pmove_step, "ax", @progbits
	.global pmove_step
	.type   pmove_step, @function

# =============================================================================
pmove_step:
	addiu   $sp, $sp, -40
	sw      $ra, 0($sp)
	sw      $s0, 4($sp)
	sw      $s1, 8($sp)
	sw      $s2, 12($sp)
	sw      $s3, 16($sp)
	sw      $s4, 20($sp)
	sw      $s5, 24($sp)
	sw      $s6, 28($sp)
	sw      $s7, 32($sp)
	lui     $s7, 0x1F80
	move    $s0, $a0                     # pmove_t
	move    $s1, $a1                     # cmd

	# the tick's length (dt 60ths of a second) and the rates' factors
	lw      $t0, CMD_DT($s1)
	lui     $t3, %hi(pm_water_scale)
	sll     $t1, $t0, 2                  # 4 dt: friction
	sll     $t2, $t0, 3                  # 8 dt: the move (vel x CLIP_SUB)
	sw      $t0, PS_DT($s7)
	sw      $t1, PS_DT4($s7)
	sw      $t2, PS_DT8($s7)
	sll     $t4, $t0, 1
	addu    $t4, $t4, $t2                # 10 dt: acceleration
	sw      $t4, PS_DT10($s7)
	li      $t4, PM_GRAVITY
	mult    $t0, $t4                     # 800 dt: gravity
	addu    $t3, $t3, $t1                # (dt x 4: the scale's entry)
	lw      $t3, %lo(pm_water_scale)($t3)
	lui     $t7, %hi(pm_grav_mid)
	addu    $t7, $t7, $t1
	lw      $t7, %lo(pm_grav_mid)($t7)
	sll     $t5, $t0, 4                  # dt x 16: the drag's row
	lui     $t6, %hi(pm_water_drag)
	addiu   $t6, $t6, %lo(pm_water_drag)
	addu    $t6, $t6, $t5
	sw      $t3, PS_WSCALE($s7)
	sw      $t6, PS_DRAG($s7)
	sw      $t7, PS_GMIDDT($s7)
	mflo    $t4
	sw      $t4, PS_DTG($s7)

	PMCOPY  PM_POS, $s7, 0, $s0            # pmove_t -> scratchpad
	sw      $zero, PM_TOUCHED($s7)       # (nothing run into yet this tick)

	lw      $a0, PM_YAW($s7)
	jal     r_sin
	nop
	sll     $v0, $v0, 16
	sra     $v0, $v0, 16
	sw      $v0, PS_SY($s7)
	lw      $a0, PM_YAW($s7)
	jal     r_cos
	nop
	sll     $v0, $v0, 16
	sra     $v0, $v0, 16
	sw      $v0, PS_CY($s7)

	lw      $t0, PM_NOCLIP($s7)
	nop
	beqz    $t0, ps_walk
	nop

	# ---- noclip: free flight along the view, no collision -------------------
	lw      $a0, PM_PITCH($s7)
	jal     r_sin
	nop
	sll     $s2, $v0, 16
	sra     $s2, $s2, 16                 # sp
	lw      $a0, PM_PITCH($s7)
	jal     r_cos
	nop
	sll     $s3, $v0, 16
	sra     $s3, $s3, 16                 # cp
	lw      $s4, PS_SY($s7)
	lw      $s5, PS_CY($s7)
	lw      $t0, CMD_WALK($s1)
	li      $s6, NOCLIP_SPEED * CLIP_SUB * 4
	beqz    $t0, 1f
	nop
	li      $s6, NOCLIP_SPEED * CLIP_SUB # s = speed (x4 unless walking)
1:	lw      $t0, PS_DT($s7)              # x dt: the tick's distance
	nop
	mult    $s6, $t0
	mflo    $s6
	lw      $t0, CMD_FWD($s1)
	lw      $t1, CMD_SIDE($s1)
	mult    $t0, $s6
	mflo    $t8
	sra     $t8, $t8, 7                  # fs = fwd * s / PM_AXIS
	nop
	mult    $t1, $s6
	mflo    $t9
	sra     $t9, $t9, 7                  # ss = side * s / PM_AXIS
	nop
	# fwd = (cp cy >> 12, cp sy >> 12, -sp), right = (sy, -cy, 0)
	mult    $s3, $s5
	mflo    $t0
	sra     $t0, $t0, 12                 # fwd0
	nop
	mult    $t0, $t8
	mflo    $t0
	nop
	nop
	mult    $s4, $t9                     # right0 * ss
	mflo    $t1
	addu    $t0, $t0, $t1
	sra     $t0, $t0, 12
	lw      $t1, PM_POS+0($s7)
	mult    $s3, $s4
	addu    $t1, $t1, $t0
	sw      $t1, PM_POS+0($s7)
	mflo    $t0
	sra     $t0, $t0, 12                 # fwd1
	nop
	mult    $t0, $t8
	mflo    $t0
	subu    $t2, $zero, $s5              # right1 = -cy
	nop
	mult    $t2, $t9
	mflo    $t1
	addu    $t0, $t0, $t1
	sra     $t0, $t0, 12
	lw      $t1, PM_POS+4($s7)
	subu    $t2, $zero, $s2              # fwd2 = -sp
	mult    $t2, $t8
	addu    $t1, $t1, $t0
	sw      $t1, PM_POS+4($s7)
	mflo    $t0
	sra     $t0, $t0, 12                 # (fwd2 fs + 0 ss) >> 12
	lw      $t1, PM_POS+8($s7)
	lw      $t2, CMD_UP($s1)
	addu    $t1, $t1, $t0
	mult    $t2, $s6
	mflo    $t2                          # up * s
	addu    $t1, $t1, $t2
	sw      $t1, PM_POS+8($s7)
	VZERO   PM_VEL
	b       ps_out
	nop

	# ---- walking ------------------------------------------------------------
ps_walk:
	jal     categorize
	nop

	# wish direction and speed, as Quake (wishspeed = |wishvel|, capped):
	# len = |(f, s)| (f, s = cmd fwd / side, +-PM_AXIS at most);
	# wishdir = (fwd f + right s) / len with fwd = (cy, sy, 0), right =
	# (sy, -cy, 0); wishspeed = maxspeed x min(len, PM_AXIS) / PM_AXIS
	lw      $t0, CMD_FWD($s1)
	lw      $t1, CMD_SIDE($s1)
	sw      $zero, PS_WISH+0($s7)
	mult    $t0, $t0
	sw      $zero, PS_WISH+4($s7)
	sw      $zero, PS_WISH+8($s7)
	mflo    $a0
	move    $s2, $zero                   # wishspeed (none without input)
	nop
	mult    $t1, $t1
	mflo    $t2
	jal     isqrt32                      # v0 = len
	addu    $a0, $a0, $t2                # f^2 + s^2
	beqz    $v0, 2f                      # no input
	lw      $t0, CMD_FWD($s1)
	lw      $t1, CMD_SIDE($s1)
	lw      $t2, PS_CY($s7)
	lw      $t3, PS_SY($s7)
	mult    $t2, $t0
	mflo    $t4
	nop
	nop
	mult    $t3, $t1
	mflo    $t5
	addu    $t4, $t4, $t5                # cy f + sy s
	nop
	div     $zero, $t4, $v0
	mflo    $t4                          # wish0 (waits for the division)
	nop
	nop
	mult    $t3, $t0
	mflo    $t5
	nop
	nop
	mult    $t2, $t1
	mflo    $t6
	subu    $t5, $t5, $t6                # sy f - cy s
	nop
	div     $zero, $t5, $v0
	mflo    $t5                          # wish1
	slti    $t0, $v0, PM_AXIS
	sw      $t4, PS_WISH+0($s7)
	sw      $t5, PS_WISH+4($s7)
	bnez    $t0, 1f
	lw      $t1, CMD_WALK($s1)
	li      $v0, PM_AXIS                 # amount = min(len, PM_AXIS)
1:	li      $s2, PM_MAXSPEED
	beqz    $t1, 1f
	nop
	li      $s2, PM_MAXSPEED / 2
1:	mult    $s2, $v0
	mflo    $s2
	sra     $s2, $s2, 7                  # / PM_AXIS

2:	# ---- SV_ClientThink: the intent on the velocity (the water level is
	# the last tick's, as Quake's think runs before its physics) ----------
	lw      $t0, PM_WATERJUMP($s7)
	lw      $t1, PM_WATERLEVEL($s7)
	bnez    $t0, 3f                      # FL_WATERJUMP: SV_WaterJump
	slti    $t1, $t1, 2
	beqz    $t1, 4f                      # waterlevel >= 2: SV_WaterMove
	lw      $t0, PM_ONGROUND($s7)        # friction on the ground, then accel
	nop
	beqz    $t0, 5f
	move    $a0, $s2                     # cap = wishspeed
	jal     friction
	nop
	b       6f
	move    $a0, $s2
5:	slti    $t1, $s2, 31                 # air: cap = min(wishspeed, 30)
	bnez    $t1, 6f
	nop
	li      $a0, 30
6:	jal     accelerate
	move    $a1, $s2                     # rate = wishspeed
	b       7f
	nop
3:	jal     water_jump
	nop
	b       7f
	nop
4:	jal     water_move
	nop

7:	# ---- PlayerPreThink (client.qc): WaterMove's drag, CheckWaterJump's
	# push (the game found the ledge), PlayerJump ------------------------
	lw      $t0, PM_WATERLEVEL($s7)
	lw      $t1, PM_WATERJUMP($s7)
	beqz    $t0, 8f
	sll     $t0, $t0, 2
	bnez    $t1, 8f
	lw      $t2, PS_DRAG($s7)            # the tick's row
	nop
	addu    $t2, $t2, $t0
	lw      $t2, 0($t2)                  # the drag of this level, 0.16
	lw      $t0, PM_VEL+0($s7)
	lw      $t1, PM_VEL+4($s7)
	mult    $t0, $t2
	lw      $t3, PM_VEL+8($s7)
	li      $t4, 65536
	mflo    $t0
	nop
	nop
	div     $zero, $t0, $t4              # vel x drag / 65536 (truncating, as the C's /)
	mflo    $t0
	nop
	nop
	mult    $t1, $t2
	sw      $t0, PM_VEL+0($s7)
	mflo    $t1
	nop
	nop
	div     $zero, $t1, $t4
	mflo    $t1
	nop
	nop
	mult    $t3, $t2
	sw      $t1, PM_VEL+4($s7)
	mflo    $t3
	nop
	nop
	div     $zero, $t3, $t4
	mflo    $t3
	sw      $t3, PM_VEL+8($s7)
8:	lw      $t0, CMD_WATERJUMP($s1)      # CheckWaterJump found the ledge
	nop
	beqz    $t0, 9f
	li      $t1, PM_WATERJUMP_TICKS
	li      $t2, PM_WATERJUMP_UP
	sw      $t1, PM_WATERJUMP($s7)
	sw      $t2, PM_VEL+8($s7)
	sw      $zero, PM_JUMPREL($s7)
9:	lw      $t0, CMD_JUMP($s1)           # PlayerJump (held)
	lw      $t1, PM_WATERJUMP($s7)
	beqz    $t0, 13f
	lw      $t2, PM_WATERLEVEL($s7)
	bnez    $t1, 14f                     # FL_WATERJUMP: nothing
	slti    $t2, $t2, 2
	bnez    $t2, 11f                     # under the water: a kick up by its type
	lw      $t3, PM_WATERTYPE($s7)
	li      $t4, QW_CONTENTS_WATER
	li      $t5, 100
	beq     $t3, $t4, 10f
	li      $t4, QW_CONTENTS_SLIME
	li      $t5, 80
	beq     $t3, $t4, 10f
	nop
	li      $t5, 50
10:	b       14f
	sw      $t5, PM_VEL+8($s7)
11:	lw      $t0, PM_ONGROUND($s7)        # on the ground, the button released since
	lw      $t1, PM_JUMPREL($s7)
	beqz    $t0, 14f
	nop
	beqz    $t1, 14f
	lw      $t2, PM_VEL+8($s7)
	sw      $zero, PM_JUMPREL($s7)
	sw      $zero, PM_ONGROUND($s7)      # don't stairwalk
	addiu   $t2, $t2, PM_JUMPSPEED
	b       14f
	sw      $t2, PM_VEL+8($s7)
13:	li      $t0, 1
	sw      $t0, PM_JUMPREL($s7)
14:
	# ---- SV_Physics_Client: the water, gravity unless swimming, the move
	jal     check_water
	nop
	lw      $t0, PM_ONGROUND($s7)        # gravity: not on the ground, not
	lw      $t6, PM_WATERJUMP($s7)       # swimming, not jumping out of the water
	bnez    $t0, 6f
	lw      $t1, PM_GRAVACC($s7)
	bnez    $v0, 6f
	lw      $t7, PS_DTG($s7)             # (gravity x dt)
	bnez    $t6, 6f
	li      $t2, PM_HZ
	addu    $t1, $t1, $t7
	div     $zero, $t1, $t2
	lw      $t3, PM_VEL+8($s7)
	mflo    $t4                          # grav_acc / 60
	mfhi    $t5                          # grav_acc % 60
	subu    $t3, $t3, $t4
	sw      $t3, PM_VEL+8($s7)
	lw      $t6, PS_GMIDDT($s7)          # (the move takes gravity's midpoint)
	sw      $t5, PM_GRAVACC($s7)
	b       7f
	sw      $t6, PS_GMID($s7)
6:	sw      $zero, PM_GRAVACC($s7)
	sw      $zero, PS_GMID($s7)

7:	li      $t3, PM_MAXVELOCITY          # cap each component (sv_maxvelocity)
	li      $t4, -PM_MAXVELOCITY
	addiu   $t5, $s7, PM_VEL
	li      $t6, 3
8:	lw      $t0, 0($t5)
	nop
	slt     $t1, $t3, $t0
	beqz    $t1, 9f
	slt     $t2, $t0, $t4
	move    $t0, $t3
9:	beqz    $t2, 10f
	nop
	move    $t0, $t4
10:	sw      $t0, 0($t5)
	addiu   $t6, $t6, -1
	bnez    $t6, 8b
	addiu   $t5, $t5, 4

	# this tick's displacement with sub-unit carry: pos_acc += vel x 8 dt,
	# move = pos_acc / 60, pos_acc = its remainder
	lw      $t9, PS_DT8($s7)
	li      $t3, PM_HZ
	lw      $t0, PM_VEL+0($s7)
	lw      $t1, PM_POSACC+0($s7)
	mult    $t9, $t0
	mflo    $t0
	addu    $t1, $t1, $t0
	nop                                  # (two apart from the div: HI/LO)
	div     $zero, $t1, $t3
	mflo    $t0
	mfhi    $t1
	sw      $t0, PS_MOVE+0($s7)
	sw      $t1, PM_POSACC+0($s7)
	lw      $t0, PM_VEL+4($s7)
	lw      $t1, PM_POSACC+4($s7)
	mult    $t9, $t0
	mflo    $t0
	addu    $t1, $t1, $t0
	nop
	div     $zero, $t1, $t3
	mflo    $t0
	mfhi    $t1
	sw      $t0, PS_MOVE+4($s7)
	sw      $t1, PM_POSACC+4($s7)
	lw      $t0, PM_VEL+8($s7)
	lw      $t1, PM_POSACC+8($s7)
	lw      $t2, PS_GMID($s7)
	mult    $t9, $t0
	addu    $t1, $t1, $t2                # (+ gravity's midpoint, a longer tick)
	mflo    $t0
	addu    $t1, $t1, $t0
	nop
	div     $zero, $t1, $t3
	mflo    $t0
	mfhi    $t1
	sw      $t0, PS_MOVE+8($s7)
	sw      $t1, PM_POSACC+8($s7)

	# plain slide
	VCOPY   PS_FLATPOS, PM_POS
	VCOPY   PS_FLATVEL, PM_VEL
	jal     slide_move
	nop
	move    $v1, $v0                     # blocked
	VCOPY   PS_AFTER, PM_POS
	VCOPY   PS_FOUTVEL, PM_VEL

	# step-up: a wall stopped us on the ground (or in the water, never while
	# jumping out of it: SV_WalkMove) -> retry over an 18-unit lip
	andi    $t0, $v1, 2
	beqz    $t0, ps_cat
	nop
	lw      $t1, PM_ONGROUND($s7)
	lw      $t2, PM_WATERLEVEL($s7)
	lw      $t3, PM_WATERJUMP($s7)
	or      $t1, $t1, $t2
	beqz    $t1, ps_cat
	nop
	bnez    $t3, ps_cat
	nop
	VCOPY   PM_POS, PS_FLATPOS
	VCOPY   PM_VEL, PS_FLATVEL
	lw      $t2, PM_POS+8($s7)           # up
	lw      $t0, PM_POS+0($s7)
	lw      $t1, PM_POS+4($s7)
	addiu   $t2, $t2, PM_STEPSIZE * CLIP_SUB
	sw      $t0, PS_TO+0($s7)
	sw      $t1, PS_TO+4($s7)
	sw      $t2, PS_TO+8($s7)
	li      $a0, 1
	addiu   $a1, $s7, PM_POS
	addiu   $a2, $s7, PS_TO
	jal     clip_trace
	addiu   $a3, $s7, PS_TRB
	VCOPY   PM_POS, PS_TRB+TR_ENDPOS
	jal     slide_move                   # across
	nop
	lw      $t2, PM_POS+8($s7)           # down
	lw      $t0, PM_POS+0($s7)
	lw      $t1, PM_POS+4($s7)
	addiu   $t2, $t2, -(PM_STEPSIZE + 2) * CLIP_SUB
	sw      $t0, PS_TO+0($s7)
	sw      $t1, PS_TO+4($s7)
	sw      $t2, PS_TO+8($s7)
	li      $a0, 1
	addiu   $a1, $s7, PM_POS
	addiu   $a2, $s7, PS_TO
	jal     clip_trace
	addiu   $a3, $s7, PS_TRB
	lw      $t0, PS_TRB+TR_FRACTION($s7) # stepped_ok: landed on a floor
	lh      $t1, PS_TRB+TR_PLANE_N+4($s7)
	slti    $t0, $t0, Q_FIX_ONE
	slti    $t1, $t1, FLOOR_NZ + 1
	xori    $t1, $t1, 1
	and     $s5, $t0, $t1
	beqz    $s5, 12f
	nop
	VCOPY   PM_POS, PS_TRB+TR_ENDPOS
	# keep the step only if it went further than the flat slide
	lw      $t0, PM_POS+0($s7)
	lw      $t1, PS_FLATPOS+0($s7)
	lw      $t2, PM_POS+4($s7)
	lw      $t3, PS_FLATPOS+4($s7)
	subu    $t0, $t0, $t1
	subu    $t2, $t2, $t3
	mult    $t0, $t0
	mflo    $t4
	nop
	nop
	mult    $t2, $t2
	mflo    $t5
	addu    $t4, $t4, $t5                # stepped distance^2
	lw      $t0, PS_AFTER+0($s7)
	lw      $t2, PS_AFTER+4($s7)
	subu    $t0, $t0, $t1
	mult    $t0, $t0
	subu    $t2, $t2, $t3
	mflo    $t6
	nop
	nop
	mult    $t2, $t2
	mflo    $t5
	addu    $t6, $t6, $t5                # flat distance^2
	slt     $t7, $t6, $t4
	beqz    $t7, 12f
	nop
	lw      $t0, PS_FOUTVEL+8($s7)       # stepped: vertical speed of the
	b       ps_cat                       # flat attempt
	sw      $t0, PM_VEL+8($s7)
12:	VCOPY   PM_POS, PS_AFTER             # the step did not help
	VCOPY   PM_VEL, PS_FOUTVEL

ps_cat:
	jal     categorize
	nop

ps_out:
	PMCOPY  0, $s0, PM_POS, $s7            # scratchpad -> pmove_t
	lw      $ra, 0($sp)
	lw      $s0, 4($sp)
	lw      $s1, 8($sp)
	lw      $s2, 12($sp)
	lw      $s3, 16($sp)
	lw      $s4, 20($sp)
	lw      $s5, 24($sp)
	lw      $s6, 28($sp)
	lw      $s7, 32($sp)
	jr      $ra
	addiu   $sp, $sp, 40

# =============================================================================
# check_water: SV_CheckWater. The contents at the feet (mins_z + 1), then the
# waist, then the eyes (hull 0); water, slime, lava and the sky count
# (cont <= CONTENTS_WATER). PM_WATERLEVEL 0..3, PM_WATERTYPE the feet's;
# v0 = Quake's return, waterlevel > 1: 1 when the waist is under (gravity
# stops), 0 with the feet alone in the water.
check_water:
	addiu   $sp, $sp, -8
	sw      $ra, 0($sp)
	lw      $t0, PM_POS+0($s7)
	lw      $t1, PM_POS+4($s7)
	lw      $t2, PM_POS+8($s7)
	sw      $t0, PS_PT+0($s7)
	sw      $t1, PS_PT+4($s7)
	addiu   $t2, $t2, (PM_MINS_Z + 1) * CLIP_SUB
	sw      $t2, PS_PT+8($s7)
	sw      $zero, PM_WATERLEVEL($s7)
	li      $t3, QW_CONTENTS_EMPTY
	sw      $t3, PM_WATERTYPE($s7)
	move    $a0, $zero
	jal     clip_contents
	addiu   $a1, $s7, PS_PT
	slti    $t0, $v0, QW_CONTENTS_WATER + 1   # cont <= CONTENTS_WATER?
	beqz    $t0, 9f
	li      $t1, 1
	sw      $v0, PM_WATERTYPE($s7)
	sw      $t1, PM_WATERLEVEL($s7)
	lw      $t2, PM_POS+8($s7)
	move    $a0, $zero
	addiu   $t2, $t2, (PM_MINS_Z + PM_MAXS_Z) / 2 * CLIP_SUB
	jal     clip_contents
	sw      $t2, PS_PT+8($s7)
	slti    $t0, $v0, QW_CONTENTS_WATER + 1
	beqz    $t0, 9f                      # the waist out: level 1, v0 = 0
	lw      $t2, PM_POS+8($s7)
	move    $a0, $zero
	addiu   $t2, $t2, PM_EYE_HEIGHT * CLIP_SUB
	jal     clip_contents
	sw      $t2, PS_PT+8($s7)
	slti    $t0, $v0, QW_CONTENTS_WATER + 1   # the eyes under: level 3, else 2
	lw      $ra, 0($sp)
	addiu   $t0, $t0, 2
	sw      $t0, PM_WATERLEVEL($s7)
	li      $v0, 1
	jr      $ra
	addiu   $sp, $sp, 8
9:	lw      $ra, 0($sp)
	move    $v0, $zero
	jr      $ra
	addiu   $sp, $sp, 8

# =============================================================================
# water_move: SV_WaterMove. The wish along the view, its pitch too (the
# forward (cp cy, cp sy, -sp) and right (sy, -cy, 0) vectors x4096), the
# stick's full deflection sv_maxspeed: wishvel x64 = ((fwd3 f + right s) x
# 320) >> 13; sinking at 60 units/s when idle; |wishvel| capped at 320 x 64
# then x 0.7; the friction 1 - 4/60 on the whole velocity (truncating);
# then the speed gained toward the wish, 10 x wishspeed / 60 a tick, never
# past wishspeed less the speed, along the wish normalized x4096.
# s2 = len, s3 = wishspeed, s4 = accel, s5 = cursor, s6 = the pitch's sine.
water_move:
	addiu   $sp, $sp, -8
	sw      $ra, 0($sp)
	lw      $a0, PM_PITCH($s7)
	jal     r_sin
	nop
	sll     $s6, $v0, 16
	sra     $s6, $s6, 16                 # sp
	lw      $a0, PM_PITCH($s7)
	jal     r_cos
	nop
	sll     $v0, $v0, 16
	sra     $v0, $v0, 16                 # cp
	lw      $t2, PS_CY($s7)
	lw      $t3, PS_SY($s7)
	mult    $v0, $t2
	mflo    $t0
	sra     $t0, $t0, 12                 # fwd3 x = cp cy >> 12
	nop
	mult    $v0, $t3
	sw      $t0, PS_FWD3+0($s7)
	mflo    $t1
	sra     $t1, $t1, 12                 # fwd3 y = cp sy >> 12
	subu    $t4, $zero, $s6              # fwd3 z = -sp
	sw      $t1, PS_FWD3+4($s7)
	sw      $t4, PS_FWD3+8($s7)
	# wishvel x64: ((fwd3 f + right s) x 320) >> 13 per axis
	lw      $t5, CMD_FWD($s1)
	lw      $t6, CMD_SIDE($s1)
	li      $t7, PM_MAXSPEED
	mult    $t0, $t5                     # x: fwd3x f + sy s
	mflo    $t0
	nop
	nop
	mult    $t3, $t6
	mflo    $t1
	addu    $t0, $t0, $t1
	nop
	mult    $t0, $t7
	mflo    $t0
	sra     $t0, $t0, 13
	sw      $t0, PS_WV+0($s7)
	lw      $t0, PS_FWD3+4($s7)
	subu    $t1, $zero, $t2              # right y = -cy
	mult    $t0, $t5                     # y: fwd3y f - cy s
	mflo    $t0
	nop
	nop
	mult    $t1, $t6
	mflo    $t1
	addu    $t0, $t0, $t1
	nop
	mult    $t0, $t7
	mflo    $t0
	sra     $t0, $t0, 13
	sw      $t0, PS_WV+4($s7)
	mult    $t4, $t5                     # z: fwd3z f (right z = 0)
	mflo    $t0
	nop
	nop
	mult    $t0, $t7
	lw      $t1, CMD_UP($s1)
	mflo    $t0
	sra     $t0, $t0, 13
	or      $t3, $t5, $t6
	or      $t3, $t3, $t1
	bnez    $t3, 1f                      # idle: sink
	sll     $t1, $t1, 6                  # (upmove x64)
	li      $t1, -PM_SINK * 64
1:	addu    $t0, $t0, $t1
	sw      $t0, PS_WV+8($s7)
	addiu   $t9, $s7, PS_WV
	DOT_II  $a0, $t9, $t9
	jal     isqrt32
	nop
	move    $s2, $v0                     # len = |wishvel| x64
	li      $t0, PM_MAXSPEED * 64
	slt     $t1, $t0, $v0
	beqz    $t1, 2f
	move    $s3, $v0
	move    $s3, $t0                     # capped at sv_maxspeed
2:	li      $t0, 7
	mult    $s3, $t0
	li      $t1, 10
	mflo    $s3
	nop
	nop
	div     $zero, $s3, $t1
	mflo    $s3                          # wishspeed = x 0.7
	# the water friction on the whole velocity: vel x the tick's scale / 65536,
	# truncating
	lw      $t2, PS_WSCALE($s7)
	li      $t4, 65536
	lw      $t0, PM_VEL+0($s7)
	lw      $t1, PM_VEL+4($s7)
	mult    $t0, $t2
	lw      $t3, PM_VEL+8($s7)
	mflo    $t0
	nop
	nop
	div     $zero, $t0, $t4
	mflo    $t0
	nop
	nop
	mult    $t1, $t2
	sw      $t0, PM_VEL+0($s7)
	mflo    $t1
	nop
	nop
	div     $zero, $t1, $t4
	mflo    $t1
	nop
	nop
	mult    $t3, $t2
	sw      $t1, PM_VEL+4($s7)
	mflo    $t3
	nop
	nop
	div     $zero, $t3, $t4
	mflo    $t3
	sw      $t3, PM_VEL+8($s7)
	addiu   $t9, $s7, PM_VEL
	DOT_II  $a0, $t9, $t9
	jal     isqrt32
	nop
	beqz    $s3, 9f                      # no wish
	sll     $v0, $v0, 6                  # newspeed x64
	subu    $t0, $s3, $v0                # addspeed = wishspeed - newspeed
	blez    $t0, 9f
	lw      $t1, PS_DT10($s7)
	nop
	mult    $s3, $t1
	li      $t2, PM_HZ
	mflo    $s4
	nop
	nop
	div     $zero, $s4, $t2
	mflo    $s4                          # accel = 10 wishspeed dt / 60
	slt     $t1, $t0, $s4
	beqz    $t1, 3f
	nop
	move    $s4, $t0                     # min(accel, addspeed)
3:	addiu   $s5, $s7, PS_WV              # vel += ((wv << 12) / len x accel) >> 18
	li      $s6, 3
4:	lw      $t0, 0($s5)
	nop
	sll     $t0, $t0, 12
	div     $zero, $t0, $s2
	mflo    $t0                          # the wish normalized x4096
	nop
	nop
	mult    $t0, $s4
	lw      $t1, PM_VEL-PS_WV($s5)
	mflo    $t0
	sra     $t0, $t0, 18
	addu    $t1, $t1, $t0
	sw      $t1, PM_VEL-PS_WV($s5)
	addiu   $s6, $s6, -1
	bnez    $s6, 4b
	addiu   $s5, $s5, 4
9:	lw      $ra, 0($sp)
	nop
	jr      $ra
	addiu   $sp, $sp, 8

# =============================================================================
# water_jump: SV_WaterJump. CheckWaterJump's push drives the horizontal
# velocity while FL_WATERJUMP holds: up to 2 s, or until out of the water.
water_jump:
	lw      $t0, PM_WATERJUMP($s7)
	lw      $t3, PS_DT($s7)
	lw      $t1, PM_WATERLEVEL($s7)
	slt     $t2, $t3, $t0                # dt < left: on
	beqz    $t2, 1f                      # (<= dt: over)
	subu    $t0, $t0, $t3
	bnez    $t1, 2f
	nop
1:	move    $t0, $zero
2:	sw      $t0, PM_WATERJUMP($s7)
	lw      $t0, PM_MOVEDIR+0($s7)
	lw      $t1, PM_MOVEDIR+4($s7)
	sw      $t0, PM_VEL+0($s7)
	jr      $ra
	sw      $t1, PM_VEL+4($s7)

# =============================================================================
# categorize: trace 2 units down; on a walkable floor set onground, settle
# onto it and kill downward speed.
categorize:
	addiu   $sp, $sp, -8
	sw      $ra, 0($sp)
	lw      $t0, PM_VEL+8($s7)
	nop
	slti    $t1, $t0, 181                # rising fast (jumped): airborne
	bnez    $t1, 1f
	nop
	b       9f
	sw      $zero, PM_ONGROUND($s7)
1:	lw      $t0, PM_POS+0($s7)
	lw      $t1, PM_POS+4($s7)
	lw      $t2, PM_POS+8($s7)
	sw      $t0, PS_TO+0($s7)
	sw      $t1, PS_TO+4($s7)
	addiu   $t2, $t2, -2 * CLIP_SUB
	sw      $t2, PS_TO+8($s7)
	li      $a0, 1
	addiu   $a1, $s7, PM_POS
	addiu   $a2, $s7, PS_TO
	jal     clip_trace
	addiu   $a3, $s7, PS_TRB
	lw      $t0, PS_TRB+TR_FRACTION($s7)
	lh      $t1, PS_TRB+TR_PLANE_N+4($s7)
	slti    $t0, $t0, Q_FIX_ONE
	beqz    $t0, 2f                      # nothing below
	slti    $t1, $t1, FLOOR_NZ
	beqz    $t1, 3f
	nop
2:	b       9f                           # too steep
	sw      $zero, PM_ONGROUND($s7)
3:	lh      $t0, PS_TRB+TR_ENT($s7)      # on the ground: 1 + what (the trace's ent)
	nop
	addiu   $t0, $t0, 1
	sw      $t0, PM_ONGROUND($s7)
	lw      $t0, PS_TRB+TR_STARTSOLID($s7)
	lw      $t1, PS_TRB+TR_ALLSOLID($s7)
	nop
	or      $t0, $t0, $t1
	bnez    $t0, 4f
	nop
	VCOPY   PM_POS, PS_TRB+TR_ENDPOS
4:	lw      $t0, PM_VEL+8($s7)
	nop
	bgez    $t0, 9f
	nop
	sw      $zero, PM_VEL+8($s7)
9:	lw      $ra, 0($sp)
	nop
	jr      $ra
	addiu   $sp, $sp, 8

# =============================================================================
# friction: ground friction, speed -= max(speed, stopspeed) * 4 dt / 60,
# applied as one 0.16 scale factor.
friction:
	addiu   $sp, $sp, -8
	sw      $ra, 0($sp)
	addiu   $t9, $s7, PM_VEL
	DOT_II  $a0, $t9, $t9
	jal     isqrt32
	nop
	blez    $v0, 9f                      # speed < 1
	slti    $t0, $v0, PM_STOPSPEED
	move    $t1, $v0                     # control = max(speed, stopspeed)
	beqz    $t0, 1f
	nop
	li      $t1, PM_STOPSPEED
1:	lw      $t2, PS_DT4($s7)             # PM_FRICTION x dt
	nop
	mult    $t1, $t2
	li      $t2, PM_HZ
	mflo    $t1
	nop
	nop
	div     $zero, $t1, $t2
	mflo    $t1                          # drop
	subu    $t1, $v0, $t1                # newspeed
	bgez    $t1, 2f
	nop
	move    $t1, $zero
2:	sll     $t1, $t1, 16
	divu    $zero, $t1, $v0
	li      $t5, 0x8000
	mflo    $t1                          # scale = newspeed / speed, 0.16
	lw      $t2, PM_VEL+0($s7)
	lw      $t3, PM_VEL+4($s7)
	mult    $t2, $t1
	lw      $t4, PM_VEL+8($s7)
	mflo    $t2
	addu    $t2, $t2, $t5
	sra     $t2, $t2, 16
	mult    $t3, $t1
	sw      $t2, PM_VEL+0($s7)
	mflo    $t3
	addu    $t3, $t3, $t5
	sra     $t3, $t3, 16
	mult    $t4, $t1
	sw      $t3, PM_VEL+4($s7)
	mflo    $t4
	addu    $t4, $t4, $t5
	sra     $t4, $t4, 16
	sw      $t4, PM_VEL+8($s7)
9:	lw      $ra, 0($sp)
	nop
	jr      $ra
	addiu   $sp, $sp, 8

# =============================================================================
# accelerate(a0 = cap, a1 = rate): speed along wishdir up to `cap`, gaining
# at most 10 * rate * dt / 60 this tick (Quake's SV_Accelerate / AirAccelerate).
accelerate:
	addiu   $t9, $s7, PM_VEL
	addiu   $t8, $s7, PS_WISH
	DOT_II  $v0, $t9, $t8
	sra     $v0, $v0, 12                 # current speed along wishdir
	subu    $v0, $a0, $v0                # add = cap - cur
	blez    $v0, 9f
	lw      $t0, PS_DT10($s7)            # PM_ACCEL x dt
	nop
	mult    $a1, $t0
	li      $t1, PM_HZ
	mflo    $t0
	nop
	nop
	div     $zero, $t0, $t1
	mflo    $t0                          # accel
	slt     $t1, $v0, $t0
	beqz    $t1, 1f
	nop
	move    $t0, $v0                     # min(accel, add)
1:	lw      $t1, PS_WISH+0($s7)
	lw      $t2, PS_WISH+4($s7)
	mult    $t1, $t0
	lw      $t3, PM_VEL+0($s7)
	lw      $t4, PM_VEL+4($s7)
	mflo    $t1
	sra     $t1, $t1, 12
	addu    $t3, $t3, $t1
	mult    $t2, $t0
	sw      $t3, PM_VEL+0($s7)
	mflo    $t2
	sra     $t2, $t2, 12
	addu    $t4, $t4, $t2
	sw      $t4, PM_VEL+4($s7)           # wishdir z = 0: vel z unchanged
9:	jr      $ra
	nop

# =============================================================================
# slide_move: SV_FlyMove. Slide PM_POS by PS_MOVE, clipping PM_VEL off every
# surface hit; sets PM_ONGROUND on a floor. v0 = blocked (1 floor, 2 wall,
# 4 stuck). s2 bump, s3 planes hit, s4 blocked, s5 time left, s6 scratch.
slide_move:
	addiu   $sp, $sp, -8
	sw      $ra, 0($sp)
	VCOPY   PS_SVEL, PM_VEL
	VCOPY   PS_PRIMAL, PM_VEL
	VCOPY   PS_CUR, PM_POS
	VCOPY   PS_REM, PS_MOVE
	move    $s2, $zero
	move    $s3, $zero
	move    $s4, $zero
	li      $s5, Q_FIX_ONE

sm_bump:
	lw      $t0, PS_REM+0($s7)
	lw      $t1, PS_REM+4($s7)
	lw      $t2, PS_REM+8($s7)
	or      $t3, $t0, $t1
	or      $t3, $t3, $t2
	beqz    $t3, sm_done                 # nothing left to move
	nop
	mult    $t0, $s5                     # disp = (rem * time_left + 2048) >> 12
	mflo    $t3
	addiu   $t3, $t3, Q_FIX_ONE / 2
	sra     $t3, $t3, 12
	mult    $t1, $s5
	sw      $t3, PS_DISP+0($s7)
	mflo    $t3
	addiu   $t3, $t3, Q_FIX_ONE / 2
	sra     $t3, $t3, 12
	mult    $t2, $s5
	sw      $t3, PS_DISP+4($s7)
	mflo    $t3
	addiu   $t3, $t3, Q_FIX_ONE / 2
	sra     $t3, $t3, 12
	sw      $t3, PS_DISP+8($s7)

	li      $s6, 2                       # keep disp off every plane hit since
1:	move    $a3, $zero                   # the last advance (two passes)
2:	beq     $a3, $s3, 3f
	sll     $t0, $a3, 3
	addiu   $a1, $s7, PS_PLANES
	addu    $a1, $a1, $t0
	jal     push_out
	addiu   $a0, $s7, PS_DISP
	b       2b
	addiu   $a3, $a3, 1
3:	addiu   $s6, $s6, -1
	bnez    $s6, 1b
	nop

	lw      $t0, PS_CUR+0($s7)           # end = cur + disp
	lw      $t1, PS_DISP+0($s7)
	lw      $t2, PS_CUR+4($s7)
	lw      $t3, PS_DISP+4($s7)
	addu    $t0, $t0, $t1
	lw      $t4, PS_CUR+8($s7)
	addu    $t2, $t2, $t3
	lw      $t5, PS_DISP+8($s7)
	sw      $t0, PS_END+0($s7)
	sw      $t2, PS_END+4($s7)
	addu    $t4, $t4, $t5
	sw      $t4, PS_END+8($s7)
	li      $a0, 1
	addiu   $a1, $s7, PS_CUR
	addiu   $a2, $s7, PS_END
	jal     clip_trace
	addiu   $a3, $s7, PS_TRB

	lw      $t0, PS_TRB+TR_ALLSOLID($s7)
	nop
	beqz    $t0, 4f
	nop
	VZERO   PS_SVEL                      # stuck in solid
	b       sm_done
	ori     $s4, $s4, 4
4:	lw      $t0, PS_TRB+TR_ENDPOS+0($s7) # advanced? (by position: a short
	lw      $t1, PS_TRB+TR_ENDPOS+4($s7) # impact can round back onto the
	lw      $t2, PS_TRB+TR_ENDPOS+8($s7) # start)
	lw      $t3, PS_CUR+0($s7)
	lw      $t4, PS_CUR+4($s7)
	lw      $t5, PS_CUR+8($s7)
	xor     $t3, $t3, $t0
	xor     $t4, $t4, $t1
	xor     $t5, $t5, $t2
	or      $t3, $t3, $t4
	or      $t3, $t3, $t5
	beqz    $t3, 5f
	nop
	sw      $t0, PS_CUR+0($s7)
	sw      $t1, PS_CUR+4($s7)
	sw      $t2, PS_CUR+8($s7)
	move    $s3, $zero
5:	lw      $t0, PS_TRB+TR_FRACTION($s7)
	li      $t1, Q_FIX_ONE
	slt     $t2, $t0, $t1
	beqz    $t2, sm_done                 # moved the whole way
	lh      $t1, PS_TRB+TR_ENT($s7)      # (a brush model run into: SV_Impact's)
	nop
	beqz    $t1, 8f
	nop
	sw      $t1, PM_TOUCHED($s7)
8:	lh      $t0, PS_TRB+TR_PLANE_N+4($s7)
	lh      $t1, PS_TRB+TR_ENT($s7)
	slti    $t2, $t0, FLOOR_NZ + 1
	bnez    $t2, 6f
	addiu   $t1, $t1, 1                  # (on the ground: 1 + the trace's ent)
	ori     $s4, $s4, 1                  # floor
	b       7f
	sw      $t1, PM_ONGROUND($s7)
6:	ori     $s4, $s4, 2                  # wall
7:	lw      $t0, PS_TRB+TR_FRACTION($s7)
	nop
	mult    $s5, $t0
	mflo    $t1
	sra     $t1, $t1, 12
	subu    $s5, $s5, $t1                # time_left -= time_left * fraction
	slti    $t0, $s3, 5
	beqz    $t0, sm_stop
	sll     $t0, $s3, 3
	addu    $t0, $s7, $t0                # planes[nplanes++] = plane hit
	lw      $t1, PS_TRB+TR_PLANE_N+0($s7)
	lh      $t2, PS_TRB+TR_PLANE_N+4($s7)
	sw      $t1, PS_PLANES+0($t0)
	sh      $t2, PS_PLANES+4($t0)
	addiu   $s3, $s3, 1

	# a velocity parallel to every plane hit since the last advance
	move    $a3, $zero                   # i
sm_try:
	beq     $a3, $s3, sm_crease
	addiu   $a0, $s7, PS_PRIMAL
	sll     $t0, $a3, 3
	addiu   $a1, $s7, PS_PLANES
	addu    $a1, $a1, $t0
	jal     clip_velocity
	addiu   $a2, $s7, PS_NEWVEL
	move    $t8, $zero                   # j
8:	beq     $t8, $s3, sm_found
	nop
	beq     $t8, $a3, 9f
	sll     $t0, $t8, 3
	addiu   $t9, $s7, PS_PLANES
	addu    $t9, $t9, $t0
	addiu   $t7, $s7, PS_NEWVEL
	DOT_IN  $v0, $t7, $t9
	bltz    $v0, 10f                     # into plane j: try the next i
	nop
9:	b       8b
	addiu   $t8, $t8, 1
10:	b       sm_try
	addiu   $a3, $a3, 1
sm_found:
	VCOPY   PS_SVEL, PS_NEWVEL
	b       sm_check
	nop

sm_crease:                               # two planes: slide along their crease
	li      $t0, 2
	bne     $s3, $t0, sm_stop
	nop
	lh      $t0, PS_PLANES+0($s7)        # p0
	lh      $t1, PS_PLANES+2($s7)
	lh      $t2, PS_PLANES+4($s7)
	lh      $t3, PS_PLANES+8($s7)        # p1
	lh      $t4, PS_PLANES+10($s7)
	lh      $t5, PS_PLANES+12($s7)
	nop
	mult    $t1, $t5
	mflo    $t6
	nop
	nop
	mult    $t2, $t4
	mflo    $t7
	subu    $t6, $t6, $t7
	sra     $t6, $t6, 12
	mult    $t2, $t3
	sw      $t6, PS_DIR+0($s7)           # dir = p0 x p1 >> 12
	mflo    $t6
	nop
	nop
	mult    $t0, $t5
	mflo    $t7
	subu    $t6, $t6, $t7
	sra     $t6, $t6, 12
	mult    $t0, $t4
	sw      $t6, PS_DIR+4($s7)
	mflo    $t6
	nop
	nop
	mult    $t1, $t3
	mflo    $t7
	subu    $t6, $t6, $t7
	sra     $t6, $t6, 12
	sw      $t6, PS_DIR+8($s7)
	addiu   $t9, $s7, PS_DIR
	DOT_II  $a0, $t9, $t9
	jal     isqrt32                      # len = |dir|
	nop
	beqz    $v0, sm_stop                 # parallel planes: wedged
	move    $s6, $v0
	addiu   $t9, $s7, PS_DIR
	addiu   $t8, $s7, PS_SVEL
	DOT_II  $v1, $t9, $t8
	div     $zero, $v1, $s6
	mflo    $v1                          # speed along the crease
	lw      $t0, PS_DIR+0($s7)
	lw      $t1, PS_DIR+4($s7)
	mult    $t0, $v1                     # vel = dir * sp / len
	lw      $t2, PS_DIR+8($s7)
	mflo    $t0
	nop
	nop
	div     $zero, $t0, $s6
	mflo    $t0
	nop
	nop
	mult    $t1, $v1
	sw      $t0, PS_SVEL+0($s7)
	mflo    $t1
	nop
	nop
	div     $zero, $t1, $s6
	mflo    $t1
	nop
	nop
	mult    $t2, $v1
	sw      $t1, PS_SVEL+4($s7)
	mflo    $t2
	nop
	nop
	div     $zero, $t2, $s6
	mflo    $t2
	sw      $t2, PS_SVEL+8($s7)

sm_check:                                # reverses the original push: stop
	addiu   $t9, $s7, PS_SVEL
	addiu   $t8, $s7, PS_PRIMAL
	DOT_II  $v0, $t9, $t8
	blez    $v0, sm_stop
	li      $t4, PM_HZ
	lw      $t9, PS_DT8($s7)
	lw      $t0, PS_SVEL+0($s7)          # rem = (vel x 8 dt +- 30) / 60
	nop
	mult    $t9, $t0
	mflo    $t0
	sra     $t1, $t0, 31
	xori    $t2, $t1, PM_HZ / 2
	subu    $t2, $t2, $t1
	addu    $t0, $t0, $t2
	div     $zero, $t0, $t4
	mflo    $t0
	sw      $t0, PS_REM+0($s7)
	lw      $t0, PS_SVEL+4($s7)
	nop
	mult    $t9, $t0
	mflo    $t0
	sra     $t1, $t0, 31
	xori    $t2, $t1, PM_HZ / 2
	subu    $t2, $t2, $t1
	addu    $t0, $t0, $t2
	div     $zero, $t0, $t4
	mflo    $t0
	sw      $t0, PS_REM+4($s7)
	lw      $t0, PS_SVEL+8($s7)
	nop
	mult    $t9, $t0
	mflo    $t0
	sra     $t1, $t0, 31
	xori    $t2, $t1, PM_HZ / 2
	subu    $t2, $t2, $t1
	addu    $t0, $t0, $t2
	div     $zero, $t0, $t4
	mflo    $t0
	sw      $t0, PS_REM+8($s7)
	addiu   $s2, $s2, 1
	slti    $t0, $s2, 4
	bnez    $t0, sm_bump
	nop
	b       sm_done
	nop

sm_stop:
	VZERO   PS_SVEL
sm_done:
	VCOPY   PM_POS, PS_CUR
	VCOPY   PM_VEL, PS_SVEL
	move    $v0, $s4
	lw      $ra, 0($sp)
	nop
	jr      $ra
	addiu   $sp, $sp, 8

# =============================================================================
# clip_velocity(a0 = v int32[3], a1 = n int16[3], a2 = out int32[3]):
# out = v - n (v.n >> 12) >> 12, components under STOP_V zeroed, then pushed
# off the plane (push_out).
clip_velocity:
	addiu   $sp, $sp, -8
	sw      $ra, 0($sp)
	DOT_IN  $v0, $a0, $a1
	sra     $v0, $v0, 12                 # backoff
	lh      $t0, 0($a1)
	lw      $t1, 0($a0)
	mult    $t0, $v0
	lh      $t2, 2($a1)
	lw      $t3, 4($a0)
	mflo    $t4
	sra     $t4, $t4, 12
	subu    $t4, $t1, $t4
	mult    $t2, $v0
	addiu   $t5, $t4, STOP_V - 1         # |out| < STOP_V: out + 1 < 3 unsigned
	sltiu   $t5, $t5, 2 * STOP_V - 1
	beqz    $t5, 1f
	nop
	move    $t4, $zero
1:	sw      $t4, 0($a2)
	lh      $t0, 4($a1)
	lw      $t1, 8($a0)
	mflo    $t4
	sra     $t4, $t4, 12
	subu    $t4, $t3, $t4
	mult    $t0, $v0
	addiu   $t5, $t4, STOP_V - 1
	sltiu   $t5, $t5, 2 * STOP_V - 1
	beqz    $t5, 2f
	nop
	move    $t4, $zero
2:	sw      $t4, 4($a2)
	mflo    $t4
	sra     $t4, $t4, 12
	subu    $t4, $t1, $t4
	addiu   $t5, $t4, STOP_V - 1
	sltiu   $t5, $t5, 2 * STOP_V - 1
	beqz    $t5, 3f
	nop
	move    $t4, $zero
3:	sw      $t4, 8($a2)
	jal     push_out
	move    $a0, $a2
	lw      $ra, 0($sp)
	nop
	jr      $ra
	addiu   $sp, $sp, 8

# =============================================================================
# push_out(a0 = v int32[3], a1 = n int16[3]): make v.n >= 0 with the smallest
# change, on n's dominant axis (the first of the largest |n|).
# Clobbers t0-t5, v0, v1, a2.
push_out:
	DOT_IN  $v0, $a0, $a1
	bgez    $v0, 9f
	nop
	lh      $t0, 0($a1)
	lh      $t1, 2($a1)
	lh      $t2, 4($a1)
	move    $t4, $zero                   # m * 4
	sra     $t5, $t0, 31
	xor     $t3, $t0, $t5
	subu    $t3, $t3, $t5                # |n[m]|
	move    $v1, $t0                     # n[m]
	sra     $t5, $t1, 31
	xor     $a2, $t1, $t5
	subu    $a2, $a2, $t5
	slt     $t5, $t3, $a2
	beqz    $t5, 1f
	nop
	move    $t3, $a2
	move    $v1, $t1
	li      $t4, 4
1:	sra     $t5, $t2, 31
	xor     $a2, $t2, $t5
	subu    $a2, $a2, $t5
	slt     $t5, $t3, $a2
	beqz    $t5, 2f
	nop
	move    $t3, $a2
	move    $v1, $t2
	li      $t4, 8
2:	subu    $t0, $t3, $v0                # s = (-d + a - 1) / a
	addiu   $t0, $t0, -1
	divu    $zero, $t0, $t3
	addu    $t4, $a0, $t4
	lw      $t1, 0($t4)
	mflo    $t0
	bgez    $v1, 3f
	addu    $t2, $t1, $t0
	subu    $t2, $t1, $t0
3:	sw      $t2, 0($t4)
9:	jr      $ra
	nop

# =============================================================================
# isqrt32: v0 = floor(sqrt(a0)), a0 < 2^31; bit by bit, starting from the
# highest even bit <= a0 (the GTE's leading-zero counter finds it).
# Clobbers a0, t0-t2.
isqrt32:
	mtc2    $a0, C2_LZCS
	move    $v0, $zero
	beqz    $a0, 4f
	mfc2    $t1, C2_LZCR                 # leading zeros (1..31 here)
	li      $t0, 31
	subu    $t1, $t0, $t1                # index of the top bit
	srl     $t1, $t1, 1
	sll     $t1, $t1, 1                  # rounded down to even
	li      $t0, 1
	sllv    $t0, $t0, $t1                # first bit <= a0
2:	addu    $t1, $v0, $t0
	sltu    $t2, $a0, $t1
	bnez    $t2, 3f
	srl     $v0, $v0, 1
	subu    $a0, $a0, $t1
	addu    $v0, $v0, $t0
3:	srl     $t0, $t0, 2
	bnez    $t0, 2b
	nop
4:	jr      $ra
	nop

	.size   pmove_step, . - pmove_step
