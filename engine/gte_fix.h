/*
 * q1psx — work around a PSn00bSDK inline-GTE bug.
 *
 * `inline_c.h`'s gte_SetRotMatrix / gte_SetTransMatrix load the MATRIX through
 * $t0/$t1/$t2 but only declare $t2 in the asm clobber list. At -O0 this is
 * harmless (GCC reloads everything from the stack); at -O2 GCC keeps live
 * values in $t0/$t1 across the call and they get silently trashed — e.g. the
 * `&m` pointer, so the *second* of two consecutive calls loads the matrix from
 * garbage and the whole view transform breaks (black screen).
 *
 * Re-define the two macros with the asm unchanged but every scratch register
 * declared. Include this AFTER <inline_c.h>.
 */

#pragma once

#include <inline_c.h>

#undef gte_SetRotMatrix
#undef gte_SetTransMatrix

#define gte_SetRotMatrix(r0) __asm__ volatile ( \
	"lw    $t0, 0( %0 );"  \
	"lw    $t1, 4( %0 );"  \
	"ctc2  $t0, $0;"       \
	"ctc2  $t1, $1;"       \
	"lw    $t0, 8( %0 );"  \
	"lw    $t1, 12( %0 );" \
	"lhu   $t2, 16( %0 );" \
	"ctc2  $t0, $2;"       \
	"ctc2  $t1, $3;"       \
	"ctc2  $t2, $4;"       \
	:                      \
	: "r"( r0 )            \
	: "$t0", "$t1", "$t2" )

#define gte_SetTransMatrix(r0) __asm__ volatile ( \
	"lw    $t0, 20( %0 );" \
	"lw    $t1, 24( %0 );" \
	"ctc2  $t0, $5;"       \
	"lw    $t2, 28( %0 );" \
	"ctc2  $t1, $6;"       \
	"ctc2  $t2, $7;"       \
	:                      \
	: "r"( r0 )            \
	: "$t0", "$t1", "$t2" )
