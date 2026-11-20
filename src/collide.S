; collideBody: whether the player body overlaps anything solid (collide.h, collide.c).
;
;   uint8_t collideBody(const int32_t *x, const int32_t *z, const int32_t *feet, int24_t height)
;
; The body is the box [x - BODY_RADIUS, x + BODY_RADIUS) x [feet - height, feet) x [z - BODY_RADIUS,
; z + BODY_RADIUS), positions in 16.16 fixed point. Its cells: xa .. xb, ya .. yb, za .. zb, at most
; 2 along x and z (BODY_RADIUS < 1/2) and 3 along y (height < 2). Solid are y >= 0 (the ground),
; y < CAM_Y_MIN (the ceiling) and every voxel, looked up in the occupancy grid of collide.c:
; grid[dz * 8 + dx] has bit dy set for a voxel at (grid_x0 + dx, grid_y0 + dy, grid_z0 + dz). If
; the cells are not inside the grid, collideBuild (C) rebuilds it around them first.
;
; The last block of cells found free is kept (collide_free): the same block again is free without
; a look at the grid. Moving a little at a time, the body usually stays in the same cells, so most
; calls end there. collideInvalidate clears it (a block with yb = 0 is never free).
;
; Cells are handled in 16 bits (.sis instructions): the world is far smaller than +-32768 cells.
;
; Why assembly: in C a call cost about 3.3k cycles (32-bit maths on the position, a stack frame,
; spills).
;
; ABI: arguments at sp+3 .. sp+12; result in A; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

#include "collide.h"

#define GRID_W		8
#define GRID_H		16
#define CAM_Y_MIN	-32		/* chunks.h (collide.c checks these three) */

	.assume	ADL = 1

; the cells of [v - BODY_RADIUS, v + BODY_RADIUS) for the 16.16 value at the pointer at (iy + off),
; into dst (low) and dst + 2 (high), 16 bits each (the stores write 3 bytes: in order). The low
; cell is cell(v) - 1 when frac(v) < BODY_RADIUS, else cell(v); the high one cell(v) + 1 when
; frac(v) + BODY_RADIUS - 1 carries out of 16 bits
	.macro	SIDES off, dst
	ld	hl, (iy + \off)
	ld	de, (hl)		; frac (16 bits)
	inc	hl
	inc	hl
	ld	bc, (hl)		; cell (16 bits)
	ex	de, hl
	push	hl
	ld	de, BODY_RADIUS
	or	a, a
	sbc.sis	hl, de			; carry: frac < BODY_RADIUS
	push	bc
	pop	hl
	jr	nc, 1f
	dec	hl
1:	ld	(\dst), hl
	pop	hl
	ld	de, BODY_RADIUS - 1
	add.sis	hl, de			; carry: past the cell
	push	bc
	pop	hl
	jr	nc, 2f
	inc	hl
2:	ld	(\dst + 2), hl
	.endm

; a = (int16)(hl - de) if that is in 0 .. n - 1, else jump to fail. clobbers hl, de
	.macro	GRID_OFFSET n, fail
	or	a, a
	sbc.sis	hl, de
	ld	de, 0x10000 - \n
	add.sis	hl, de			; carry: not in 0 .. n - 1
	jp	c, \fail
	ld	a, l
	add	a, \n
	.endm

	.section	.text._collideBody,"ax",@progbits
	.global	_collideBody
	.type	_collideBody,@function
_collideBody:
	ld	iy, 0
	add	iy, sp
	; ---- the cells: .Lcells = xa xb ya yb za zb, 16 bits each, stored in this order (every
	; store writes 3 bytes, the third one into the next value) ----
	SIDES	3, .Lcells
	; y: ya = cell(feet - height) = cf - hi - (ff < lo), height = hi 65536 + lo (hi 0 or 1),
	; and yb = cell(feet - 1) = cf - (ff = 0)
	ld	hl, (iy + 9)
	ld	de, (hl)		; ff
	inc	hl
	inc	hl
	ld	bc, (hl)		; cf
	ex	de, hl
	push	hl
	ld	de, (iy + 12)		; lo
	or	a, a
	sbc.sis	hl, de			; carry: ff < lo
	push	bc
	pop	hl
	jr	nc, 1f
	dec	hl
1:	ld	a, (iy + 14)		; hi
	or	a, a
	jr	z, 2f
	dec	hl
2:	ld	(.Lcells + 4), hl
	pop	hl
	ld	a, h
	or	a, l
	push	bc
	pop	hl
	jr	nz, 3f
	dec	hl
3:	ld	(.Lcells + 6), hl
	SIDES	6, .Lcells + 8

	; ---- the ground: yb >= 0 ----
	ld	a, (.Lcells + 7)
	rla
	ld	a, 1
	ret	nc

	; ---- the block found free last time (12 bytes, 3 at a time; equal leaves no borrow) ----
	ld	hl, (.Lcells)
	ld	de, (_collide_free)
	or	a, a
	sbc	hl, de
	jr	nz, .Llook
	ld	hl, (.Lcells + 3)
	ld	de, (_collide_free + 3)
	sbc	hl, de
	jr	nz, .Llook
	ld	hl, (.Lcells + 6)
	ld	de, (_collide_free + 6)
	sbc	hl, de
	jr	nz, .Llook
	ld	hl, (.Lcells + 9)
	ld	de, (_collide_free + 9)
	sbc	hl, de
	jr	nz, .Llook
	xor	a, a
	ret

.Llook:
	; ---- the ceiling: ya < CAM_Y_MIN ----
	ld	hl, (.Lcells + 4)
	ld	de, CAM_Y_MIN
	or	a, a
	sbc.sis	hl, de
	ld	a, 1
	ret	m

.Lretry:
	; ---- offsets in the grid: x and z 0 .. 7, y 0 .. 15, or rebuild ----
	ld	hl, (.Lcells)
	ld	de, (_grid_x0)
	GRID_OFFSET	GRID_W, .Lbuild
	ld	b, a			; dx0
	ld	hl, (.Lcells + 2)
	ld	de, (_grid_x0)
	GRID_OFFSET	GRID_W, .Lbuild
	sub	a, b
	ld	c, a			; dx1 - dx0: 0 or 1
	ld	hl, (.Lcells + 10)
	ld	de, (_grid_z0)
	GRID_OFFSET	GRID_W, .Lbuild
	ld	(.Ldz1), a
	ld	hl, (.Lcells + 8)
	ld	de, (_grid_z0)
	GRID_OFFSET	GRID_W, .Lbuild
	ld	(.Ldz0), a
	ld	hl, (.Lcells + 6)
	ld	de, (_grid_y0)
	GRID_OFFSET	GRID_H, .Lbuild
	ld	(.Ldy1), a
	ld	hl, (.Lcells + 4)
	ld	de, (_grid_y0)
	GRID_OFFSET	GRID_H, .Lbuild

	; ---- the mask of bits dy0 .. dy1: below[dy1 + 1] and not below[dy0] ----
	ld	hl, .Lbelow
	add	a, a
	ld	de, 0
	ld	e, a
	add	hl, de
	ld	de, (hl)		; below[dy0] (e, d)
	ld	a, (.Ldy1)
	inc	a
	add	a, a
	ld	hl, .Lbelow
	push	bc
	ld	bc, 0
	ld	c, a
	add	hl, bc
	pop	bc
	ld	a, e
	cpl
	and	a, (hl)
	ld	e, a
	inc	hl
	ld	a, d
	cpl
	and	a, (hl)
	ld	d, a			; de = the mask

	; ---- the columns: iy = grid + 2 (8 dz0 + dx0), row by row ----
	ld	a, (.Ldz0)
	add	a, a
	add	a, a
	add	a, a
	add	a, b			; 8 dz0 + dx0 (at most 63)
	add	a, a
	ld	iy, _grid
	ld	hl, 0
	ld	l, a
	ex	de, hl
	add	iy, de
	ex	de, hl
	ld	a, (.Ldz1)
	ld	hl, .Ldz0
	sub	a, (hl)
	ld	b, a
	inc	b			; rows: 1 or 2
.Lrow:
	ld	a, (iy + 0)
	and	a, e
	jr	nz, .Lsolid
	ld	a, (iy + 1)
	and	a, d
	jr	nz, .Lsolid
	bit	0, c			; a second column
	jr	z, .Lnext_row
	ld	a, (iy + 2)
	and	a, e
	jr	nz, .Lsolid
	ld	a, (iy + 3)
	and	a, d
	jr	nz, .Lsolid
.Lnext_row:
	lea	iy, iy + 2 * GRID_W
	djnz	.Lrow
	; free: remember the block
	ld	hl, .Lcells
	ld	de, _collide_free
	ld	bc, 12
	ldir
	xor	a, a
	ret
.Lsolid:
	ld	a, 1
	ret

.Lbuild:
	; rebuild the grid around (xa, ya, za), then look again (collideBuild takes int24_t, the
	; cells are 16 bits: sign-extended there)
	ld	hl, (.Lcells + 8)
	push	hl
	ld	hl, (.Lcells + 4)
	push	hl
	ld	hl, (.Lcells)
	push	hl
	call	_collideBuild
	pop	hl
	pop	hl
	pop	hl
	jp	.Lretry


	.section	.rodata._collideBody,"a",@progbits
; below[n] = (1 << n) - 1: the bits below bit n
.Lbelow:
	.set	n, 0
	.rept	GRID_H + 1
	.short	(1 << n) - 1
	.set	n, n + 1
	.endr

	.section	.bss._collideBody,"aw",@nobits
.Lcells:	.zero	13		; xa xb ya yb za zb, and the byte the last 3-byte store spills into
	.global	_collide_free
_collide_free:	.zero	13		; the block found free last (yb = 0: none)
.Ldz0:	.zero	1
.Ldz1:	.zero	1
.Ldy1:	.zero	1

	.extern	_grid
	.extern	_grid_x0
	.extern	_grid_y0
	.extern	_grid_z0
	.extern	_collideBuild


; ==================================================================================================
;   void collideMark(const int8_t (*rec)[5], uint8_t n, uint8_t lx, uint8_t ly, uint8_t lz)
;
; Marks the records of a chunk that lie in the occupancy grid (collide.c, collideBuild): a record
; {x, Y, z, ...} is at dx = x - lx, dy = Y - ly, dz = z - lz in the grid when dx and dz are 0 .. 7
; and dy 0 .. 15 (byte arithmetic: collide.c explains why it is exact). Sets bit dy of
; grid[8 dz + dx]. n may be 0. lx, ly and lz are patched into the sub instructions (self-modifying
; code: the program runs from RAM). About 60 cycles per record outside the grid, 200 inside; in C
; it was about 300.
	.section	.text._collideMark,"ax",@progbits
	.global	_collideMark
	.type	_collideMark,@function
_collideMark:
	ld	iy, 0
	add	iy, sp
	ld	a, (iy + 9)
	ld	(.Lmx + 1), a
	ld	a, (iy + 12)
	ld	(.Lmy + 1), a
	ld	a, (iy + 15)
	ld	(.Lmz + 1), a
	ld	b, (iy + 6)
	ld	iy, (iy + 3)
	inc	b
	dec	b
	ret	z
.Lmark:
	ld	a, (iy + 0)
.Lmx:	sub	a, 0			; dx (patched)
	cp	a, GRID_W
	jr	nc, .Lmark_next
	ld	c, a
	ld	a, (iy + 2)
.Lmz:	sub	a, 0			; dz (patched)
	cp	a, GRID_W
	jr	nc, .Lmark_next
	add	a, a
	add	a, a
	add	a, a
	add	a, c
	add	a, a
	ld	c, a			; 2 (8 dz + dx): the word
	ld	a, (iy + 1)
.Lmy:	sub	a, 0			; dy (patched)
	cp	a, GRID_H
	jr	nc, .Lmark_next
	cp	a, 8
	jr	c, 1f
	inc	c			; bits 8 .. 15: the high byte
	sub	a, 8
1:	ld	hl, .Lbit
	ld	de, 0
	ld	e, a
	add	hl, de
	ld	a, (hl)			; 1 << (dy mod 8)
	ld	hl, _grid
	ld	e, c
	add	hl, de
	or	a, (hl)
	ld	(hl), a
.Lmark_next:
	lea	iy, iy + 5
	djnz	.Lmark
	ret

	.section	.rodata._collideMark,"a",@progbits
.Lbit:	.byte	1, 2, 4, 8, 16, 32, 64, 128
