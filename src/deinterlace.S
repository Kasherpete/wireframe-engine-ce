; deinterlaceRows: duplicate each drawn LCD row into the LCD row below it.
;
;   void deinterlaceRows(uint8_t *row0, uint24_t width)
;
; For each of the 120 graphx rows (320 byte stride) starting at row0, copies bytes 0..width-1 to
; bytes 160..160+width-1, the LCD row below in 4bpp mode (see CLAUDE.md). width must be 1..160:
; ldir with bc = 0 would copy 16 MB. row0 plus 119 rows plus 160 + width must stay inside the frame.
;
; Used at startup (displayInit) and for any full-frame copy. Each frame, deinterlaceFrame in fill.S
; does the same per row but skips the rows drawSpans already filled on both LCD rows.
;
; ldir is the cheapest copy on the CE (about 7.7 cycles per byte in CEmu): about 149k cycles for
; the whole 160 byte width.
;
; ABI: arguments at sp+3, sp+6; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

	.assume	ADL = 1

	.section	.text._deinterlaceRows,"ax",@progbits
	.global	_deinterlaceRows
_deinterlaceRows:
	ld	iy, 0
	add	iy, sp
	ld	hl, (iy + 3)		; hl = row start
	ld	a, 120
1:	push	hl
	ld	de, 160
	ex	de, hl
	add	hl, de
	ex	de, hl			; de = row start + 160, hl = row start
	ld	bc, (iy + 6)		; width
	ldir
	pop	hl
	ld	de, 320
	add	hl, de			; next row
	dec	a
	jr	nz, 1b
	ret
