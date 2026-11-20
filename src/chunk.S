; The per-voxel loops of chunk loading and exposure (main.c: loadChunk, computeExposure).
;
;   void copyVoxels(int8_t (*rec)[5], const world_voxel_t *v, uint8_t n, int8_t *yrange)
;   void occMark(const int8_t (*rec)[5], uint8_t n, uint8_t ybase)
;   void occNeighbour(const int8_t (*rec)[5], uint8_t n, uint8_t axis, uint8_t edge, int24_t at,
;                     uint8_t ybase, uint8_t ylen)
;   void occExpose(int8_t (*rec)[5], uint8_t n, uint8_t ybase)
;
; copyVoxels turns n file voxels {x + 8 z, y, material, faces} (world.h) into records {x, Y = y + 1,
; z, material, faces}, and stores the least and greatest Y in yrange[0] and [1] (127 and -128 for
; n = 0).
;
; The occupancy grid (main.c, OCC) holds one byte per cell of a chunk and a border of one around
; it, 0 or 1: OCC_ROW bytes per row of x, OCC_LAYER per layer of z rows. occ_layer[L] is the
; address of cell (0, L, 0), so cell (x, L, z) is occ_layer[L] + OCC_ROW z + x, and its six
; neighbours are at +-1, +-OCC_ROW and +-OCC_LAYER: all (ix + d) displacements. A record is in
; layer L = Y - ybase, where ybase = the chunk least Y - 1.
;
; occMark sets the cell of each of the n records to 1.
; occNeighbour marks the records of a neighbouring chunk that touch this one: those whose
; coordinate axis (0: x, 2: z) is edge, and whose layer is 1 .. ylen (the chunk own layers), at
; occ_layer[L] + at + their other coordinate (at = -1 or CHUNK_SIZE on the x axis; on the z axis
; that times OCC_ROW).
; occExpose stores in each record [4] the FACE_* bits of the neighbouring cells that are 0.
; All do nothing for n = 0.
;
; In C these were about 500 (copy) and 1.5k (mark and expose) cycles per voxel, and 80 per
; neighbour record: sign extensions, 16-bit lookups, spills, library calls.
;
; ABI: arguments at sp+3, sp+6 ...; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

#include "engine.h"

#if OCC_ROW != 10 || OCC_LAYER != 100
#error "occExpose reads the neighbours as (ix + d): OCC_LAYER must stay under 128"
#endif
#if FACE_XP != 1 || FACE_XN != 2 || FACE_ZP != 4 || FACE_ZN != 8 || FACE_TOP != 16 || FACE_BOT != 32
#error "occExpose builds the FACE_* bits by shifting"
#endif

	.assume	ADL = 1

; hl = occ_layer[a]. clobbers de
	.macro	LAYER
	ld	l, a
	ld	h, 3
	mlt	hl			; 3 L (mlt clears the upper byte)
	ld	de, _occ_layer
	add	hl, de
	ld	hl, (hl)
	.endm

; hl = address of the cell of the record at iy (clobbers a, de). ybase is patched into the sub
	.macro	CELL yb
	ld	a, (iy + 1)
\yb:	sub	a, 0			; - ybase (patched)
	LAYER
	ld	e, (iy + 2)
	ld	d, OCC_ROW
	mlt	de			; OCC_ROW z, upper byte clear
	add	hl, de
	ld	e, (iy + 0)
	ld	d, 0
	add	hl, de
	.endm


	.section	.text._copyVoxels,"ax",@progbits
	.global	_copyVoxels
	.type	_copyVoxels,@function
_copyVoxels:
	ld	iy, 0
	add	iy, sp
	ld	de, (iy + 3)		; rec
	ld	b, (iy + 9)
	ld	iy, (iy + 6)		; v
	ld	c, 0xff			; least Y, biased by 0x80 (unsigned order)
	ld	h, 0			; greatest
	inc	b
	dec	b
	jr	z, .Lcopied
.Lcopy:
	ld	a, (iy + 0)		; x
	and	a, 7
	ld	(de), a
	inc	de
	ld	a, (iy + 1)		; Y = y + 1 (y <= WORLD_Y_MAX: no overflow)
	inc	a
	ld	(de), a
	inc	de
	xor	a, 0x80
	cp	a, c
	jr	nc, 1f
	ld	c, a
1:	cp	a, h
	jr	c, 2f
	ld	h, a
2:	ld	a, (iy + 0)		; z (xz < 64: worldOpen checked)
	rrca
	rrca
	rrca
	and	a, 7
	ld	(de), a
	inc	de
	ld	a, (iy + 2)		; material
	ld	(de), a
	inc	de
	ld	a, (iy + 3)		; exposed faces
	ld	(de), a
	inc	de
	lea	iy, iy + 4
	djnz	.Lcopy
.Lcopied:
	ld	iy, 0
	add	iy, sp
	ld	iy, (iy + 12)		; yrange
	ld	a, c
	xor	a, 0x80
	ld	(iy + 0), a
	ld	a, h
	xor	a, 0x80
	ld	(iy + 1), a
	ret


	.section	.text._occMark,"ax",@progbits
	.global	_occMark
	.type	_occMark,@function
_occMark:
	ld	iy, 0
	add	iy, sp
	ld	a, (iy + 9)
	ld	(.Lmyb + 1), a		; self-modifying: the program runs from RAM
	ld	b, (iy + 6)
	ld	iy, (iy + 3)
	inc	b
	dec	b
	ret	z
.Lmark:
	CELL	.Lmyb
	ld	(hl), 1
	lea	iy, iy + 5
	djnz	.Lmark
	ret


	.section	.text._occNeighbour,"ax",@progbits
	.global	_occNeighbour
	.type	_occNeighbour,@function
_occNeighbour:
	ld	iy, 0
	add	iy, sp
	ld	a, (iy + 12)		; edge
	ld	(.Lnxe + 1), a		; self-modifying: the program runs from RAM
	ld	(.Lnze + 1), a
	ld	a, (iy + 18)		; ybase, biased by 0x80 so that Y < ybase borrows
	xor	a, 0x80
	ld	(.Lnxb + 1), a
	ld	(.Lnzb + 1), a
	ld	a, (iy + 21)		; ylen
	ld	(.Lnxl + 1), a
	ld	(.Lnzl + 1), a
	ld	hl, (iy + 15)		; at
	ld	(.Lnxa + 1), hl
	ld	(.Lnza + 1), hl
	ld	a, (iy + 9)		; axis
	ld	b, (iy + 6)
	ld	iy, (iy + 3)
	inc	b
	dec	b
	ret	z
	or	a, a
	jr	nz, .Lnz

	; x axis: the cell is occ_layer[L] + at + OCC_ROW z
.Lnx:
	ld	a, (iy + 0)
.Lnxe:	cp	a, 0			; edge (patched)
	jr	nz, 1f
	ld	a, (iy + 1)
	xor	a, 0x80
.Lnxb:	sub	a, 0			; L = Y - ybase (patched), unsigned: borrow if Y < ybase
	jr	c, 1f
	dec	a
.Lnxl:	cp	a, 0			; L - 1 < ylen (patched)
	jr	nc, 1f
	inc	a
	LAYER
	ld	e, (iy + 2)
	ld	d, OCC_ROW
	mlt	de
	add	hl, de
.Lnxa:	ld	de, 0			; at (patched)
	add	hl, de
	ld	(hl), 1
1:	lea	iy, iy + 5
	djnz	.Lnx
	ret

	; z axis: the cell is occ_layer[L] + at + x
.Lnz:
	ld	a, (iy + 2)
.Lnze:	cp	a, 0
	jr	nz, 1f
	ld	a, (iy + 1)
	xor	a, 0x80
.Lnzb:	sub	a, 0
	jr	c, 1f
	dec	a
.Lnzl:	cp	a, 0
	jr	nc, 1f
	inc	a
	LAYER
	ld	e, (iy + 0)
	ld	d, 1
	mlt	de			; x, upper byte clear
	add	hl, de
.Lnza:	ld	de, 0
	add	hl, de
	ld	(hl), 1
1:	lea	iy, iy + 5
	djnz	.Lnz
	ret


	.section	.text._occExpose,"ax",@progbits
	.global	_occExpose
	.type	_occExpose,@function
_occExpose:
	ld	iy, 0
	add	iy, sp
	ld	a, (iy + 9)
	ld	(.Leyb + 1), a
	ld	b, (iy + 6)
	ld	iy, (iy + 3)
	inc	b
	dec	b
	ret	z
	push	ix
.Lexpose:
	CELL	.Leyb
	push	hl
	pop	ix
	; the occupied neighbours as FACE_* bits, highest first, then the others are the exposed faces
	ld	a, (ix + OCC_LAYER)	; Y + 1, below: FACE_BOT
	add	a, a
	or	a, (ix - OCC_LAYER)	; Y - 1, above (y grows downward): FACE_TOP
	add	a, a
	or	a, (ix - OCC_ROW)	; FACE_ZN
	add	a, a
	or	a, (ix + OCC_ROW)	; FACE_ZP
	add	a, a
	or	a, (ix - 1)		; FACE_XN
	add	a, a
	or	a, (ix + 1)		; FACE_XP
	cpl
	and	a, FACE_XP | FACE_XN | FACE_ZP | FACE_ZN | FACE_TOP | FACE_BOT
	ld	(iy + 4), a
	lea	iy, iy + 5
	djnz	.Lexpose
	pop	ix
	ret

	.extern	_occ_layer
