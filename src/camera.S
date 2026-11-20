; fillRotTable: one per-frame rotation table of setupCamera (camera.c).
;
;   void fillRotTable(int24_t *t, uint8_t stride, int24_t k, int24_t frac)
;
; t[n * stride] = 3 v + 3 k n for n = 0 .. GRID_LEN - 1, where v = -GRID_RANGE k - floor(frac k / 256):
; 3 (n - GRID_RANGE - frac / 256) k, rounded the same way for every n, by repeated addition. Every
; entry is an exact multiple of 3 (project.S uses them as byte offsets). frac is 0 .. 255 and
; |k| <= 128 (sinTable, SUB), so frac |k| is one mlt. stride is 1 or 2.
;
; Why assembly: the C loop cost about 140 cycles per entry (a pointer step by a variable stride, a
; library multiply and shift for v): 23k cycles per frame for the 5 tables of 33 entries. This is
; about 45.
;
; ABI: arguments at sp+3 .. sp+12; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

#include "engine.h"

#if GRID_RANGE != 24 || SUB != 128
#error "v = -24 k - ... is 8 k + 16 k, and |k| <= 128 fits mlt"
#endif

	.assume	ADL = 1

	.section	.text._fillRotTable,"ax",@progbits
	.global	_fillRotTable
	.type	_fillRotTable,@function
_fillRotTable:
	ld	iy, 0
	add	iy, sp
	; ---- q = floor(frac k / 256) ----
	ld	hl, (iy + 9)		; k
	ld	e, h			; bit 7: the sign of k (|k| <= 128)
	ld	a, l
	bit	7, e
	jr	z, 1f
	neg				; |k|
1:	ld	h, (iy + 12)		; frac
	ld	l, a
	mlt	hl			; p = frac |k|, below 2^15 (mlt clears the upper byte)
	bit	7, e
	jr	z, 2f
	ld	bc, 255			; k < 0: q = -ceil(p / 256) = -((p + 255) >> 8)
	add	hl, bc
	ld	c, h
	or	a, a
	sbc	hl, hl
	ld	b, 0
	sbc	hl, bc			; (bc: c, and b and the upper byte 0 from ld bc, 255)
	jr	3f
2:	ld	c, h			; k >= 0: q = p >> 8
	ld	hl, 0
	ld	l, c
3:	; ---- v = -24 k - q, then 3 v and 3 k ----
	push	hl
	pop	bc			; q
	ld	hl, (iy + 9)
	add	hl, hl
	add	hl, hl
	add	hl, hl			; 8 k
	push	hl
	pop	de
	add	hl, hl			; 16 k
	add	hl, de			; 24 k
	add	hl, bc			; 24 k + q
	ex	de, hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de			; v
	push	hl
	pop	de
	add	hl, hl
	add	hl, de			; 3 v
	ex	de, hl
	ld	hl, (iy + 9)
	push	hl
	pop	bc
	add	hl, hl
	add	hl, bc
	push	hl
	pop	bc			; 3 k
	ex	de, hl			; hl = 3 v
	; ---- the entries, 3 stride bytes apart ----
	ld	a, (iy + 6)
	ld	de, 0
	ld	e, a
	add	a, a
	add	a, e
	ld	e, a			; 3 stride
	ld	iy, (iy + 3)
	ld	a, 2 * GRID_RANGE + 1		; GRID_LEN
.Lentry:
	ld	(iy + 0), hl
	add	iy, de
	add	hl, bc
	dec	a
	jr	nz, .Lentry
	ret
