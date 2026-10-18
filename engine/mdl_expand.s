# q1psx — mdl_expand: a streamed model's frame (formats/qstream.h: one word
# a vertex, x | z << 8 | y << 16 | normal << 24) into the renderer's qw_vert
# pool (engine/mdl.h). C ABI:
#
#   void mdl_expand(qw_vert *dst, const uint32_t *src, int n);
#   void mdl_expand_shift(qw_vert *dst, const uint32_t *src, int n, int shift);
#
# Word w gives x | y << 16 = w & 0x00ff00ff and z | normal << 16 =
# (w >> 8) & 0x00ff00ff: two ANDs and a shift, two stores. Two vertices a
# turn (n odd: the last one apart). The shift's variant (a big model: its
# steps doubled or more, its matrix within the GTE's range) shifts x, y and
# z left, the normal kept. Clobbers t0-t7.

	.set noreorder
	.set noat

	.section .text.mdl_expand, "ax", @progbits
	.global mdl_expand
	.type mdl_expand, @function
mdl_expand:
	lui     $t0, 0x00ff
	addiu   $at, $a2, -2
	bltz    $at, 2f                      # fewer than two
	ori     $t0, $t0, 0x00ff             # (0x00ff00ff)
1:	lw      $t1, 0($a1)
	lw      $t3, 4($a1)
	srl     $t2, $t1, 8
	and     $t1, $t1, $t0                # x | y << 16
	and     $t2, $t2, $t0                # z | normal << 16
	srl     $t4, $t3, 8
	and     $t3, $t3, $t0
	and     $t4, $t4, $t0
	sw      $t1, 0($a0)
	sw      $t2, 4($a0)
	sw      $t3, 8($a0)
	sw      $t4, 12($a0)
	addiu   $a2, $a2, -2
	addiu   $at, $a2, -2
	addiu   $a1, $a1, 8
	bgez    $at, 1b
	addiu   $a0, $a0, 16
2:	blez    $a2, 3f                      # the last one, n odd
	nop
	lw      $t1, 0($a1)
	nop
	srl     $t2, $t1, 8
	and     $t1, $t1, $t0
	and     $t2, $t2, $t0
	sw      $t1, 0($a0)
	sw      $t2, 4($a0)
3:	jr      $ra
	nop

	.section .text.mdl_expand_shift, "ax", @progbits
	.global mdl_expand_shift
	.type mdl_expand_shift, @function
mdl_expand_shift:
	lui     $t0, 0x00ff
	ori     $t0, $t0, 0x00ff             # x | y << 16's mask
	blez    $a2, 2f
	lui     $t5, 0x00ff                  # the normal's (0x00ff0000)
1:	lw      $t1, 0($a1)
	addiu   $a1, $a1, 4
	srl     $t2, $t1, 8
	and     $t1, $t1, $t0
	sllv    $t1, $t1, $a3                # x, y shifted (each in its half)
	andi    $t3, $t2, 0xff
	sllv    $t3, $t3, $a3                # z shifted
	and     $t2, $t2, $t5                # normal << 16
	or      $t2, $t2, $t3
	sw      $t1, 0($a0)
	sw      $t2, 4($a0)
	addiu   $a2, $a2, -1
	bgtz    $a2, 1b
	addiu   $a0, $a0, 8
2:	jr      $ra
	nop
