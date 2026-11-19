; sortRecords: the painter sort of the scene (main.c, sortScene).
;
;   void sortRecords(int8_t (*rec)[4], int8_t (*scratch)[4], uint8_t *keys, uint8_t n,
;                    uint8_t bx, uint8_t by, uint8_t bz)
;
; Sorts the n records {X, Y, Z, colour} far to near by their Manhattan distance in cells from the
; camera, gridDist[X + bx] + gridDist[Y + by] + gridDist[Z + bz] with 8-bit adds (main.c explains
; why the low byte of the grid index is enough). A stable counting sort in three passes over
; rec, then scratch is copied back over rec. keys must hold n bytes. n must be 1 .. 255.
;
; Measured per sort: 5.3k cycles for 7 records, 59k for 120 (the same sort in C: 7.9k and 127k,
; the old qsort: 41k-48k and 1.1M-1.8M). About 470 cycles per record plus a little per key.
;
; Speed tricks:
; - gridDist, the counts and the start pointers each begin on a 256-byte boundary, so setting the
;   low byte of a register pair (ld e, a / ld l, a / ld ixl, a) indexes them with no add.
; - bx, by and bz are written into the add a, n instructions below (self-modifying code: the
;   program runs from RAM), which leaves b free for djnz.
; - The start of each key in scratch is kept as a 24-bit pointer, so placing a record needs no
;   multiply by 4.
; - Pass 1 also tracks the largest key (in a patched cp), so the prefix pass only visits keys
;   max .. 0, and for an empty key only tests the count. It clears each used count as it reads
;   it, so they are all zero again for the next sort without a separate clear.
;
; ABI: arguments at sp+3 .. sp+21; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

#include "engine.h"

#define BUCKETS		3 * GRID_RANGE + 1		/* keys 0 .. 3 GRID_RANGE. No parentheses (see CLAUDE.md), so wrap it when multiplying */

#if 3 * (3 * GRID_RANGE) + 2 > 255
#error "start pointers (3 bytes per key) no longer fit in one 256-byte page"
#endif

; argument offsets from sp after push ix
#define A_REC		6
#define A_SCRATCH	9
#define A_KEYS		12
#define A_N		15

	.assume	ADL = 1

	.section	.text._sortRecords,"ax",@progbits
	.global	_sortRecords
	.type	_sortRecords,@function
_sortRecords:
	ld	iy, 0
	add	iy, sp
	ld	a, (iy + 15)		; patch the camera offsets into pass 1
	ld	(.Lbx + 1), a
	ld	a, (iy + 18)
	ld	(.Lby + 1), a
	ld	a, (iy + 21)
	ld	(.Lbz + 1), a
	xor	a, a
	ld	(.Lmax + 1), a		; largest key so far
	push	ix

	; ---- pass 1: each key into keys[], and how many records have it ----
	; iy = record, ix = keys[i], de = gridDist (e = index), hl = counts (l = key), b = left
	ld	b, (iy + A_N - 3)
	ld	ix, (iy + A_KEYS - 3)
	ld	iy, (iy + A_REC - 3)
	ld	de, _gridDist
	ld	hl, .Lcount
.Lkey:
	ld	a, (iy + 0)
.Lbx:	add	a, 0			; + bx (patched)
	ld	e, a
	ld	a, (de)
	ld	c, a
	ld	a, (iy + 1)
.Lby:	add	a, 0			; + by (patched)
	ld	e, a
	ld	a, (de)
	add	a, c
	ld	c, a
	ld	a, (iy + 2)
.Lbz:	add	a, 0			; + bz (patched)
	ld	e, a
	ld	a, (de)
	add	a, c			; the key
.Lmax:	cp	a, 0			; largest key so far (patched)
	jr	c, .Lnotmax
	ld	(.Lmax + 1), a
.Lnotmax:
	ld	(ix + 0), a
	inc	ix
	ld	l, a
	inc	(hl)
	lea	iy, iy + 4
	djnz	.Lkey

	; ---- pass 2: where each key starts in scratch, farthest key first ----
	; Only keys max .. 0, and an empty key costs just the test: its start pointer is never read.
	; hl = counts (l = key), ix = start pointers (ixl = 3 key), de = next free record in scratch
	ld	iy, 0
	add	iy, sp
	ld	de, (iy + A_SCRATCH)
	ld	ix, .Lstart
	ld	a, (.Lmax + 1)
	ld	l, a
	ld	b, a
	inc	b
.Lprefix:
	ld	a, (hl)
	or	a, a
	jr	nz, .Lused
.Lnext:
	dec	l
	djnz	.Lprefix
	jr	.Lplace_start

.Lused:
	ld	(hl), 0			; clear the count for the next sort
	ld	c, a
	ld	a, l
	add	a, a
	add	a, l			; 3 key, at most 216: no carry
	ld	ixl, a
	ld	(ix + 0), de
	push	hl
	sbc	hl, hl
	ld	l, c
	add	hl, hl
	add	hl, hl
	add	hl, de
	ex	de, hl			; de += 4 count
	pop	hl
	jr	.Lnext

	; ---- pass 3: copy each record to the next free place for its key ----
	; iy = record, de = keys[i], ix = start pointers (ixl = 3 key), hl = destination
.Lplace_start:
	ld	b, (iy + A_N)
	ld	de, (iy + A_KEYS)
	ld	iy, (iy + A_REC)
	ld	ix, .Lstart
.Lplace:
	ld	a, (de)
	inc	de
	ld	c, a
	add	a, a
	add	a, c
	ld	ixl, a
	ld	hl, (ix + 0)
	ld	a, (iy + 0)
	ld	(hl), a
	inc	hl
	ld	a, (iy + 1)
	ld	(hl), a
	inc	hl
	ld	a, (iy + 2)
	ld	(hl), a
	inc	hl
	ld	a, (iy + 3)
	ld	(hl), a
	inc	hl
	ld	(ix + 0), hl
	lea	iy, iy + 4
	djnz	.Lplace

	; ---- copy scratch back over the records ----
	ld	iy, 0
	add	iy, sp
	or	a, a
	sbc	hl, hl
	ld	l, (iy + A_N)
	add	hl, hl
	add	hl, hl
	push	hl
	pop	bc			; 4 n bytes
	ld	de, (iy + A_REC)
	ld	hl, (iy + A_SCRATCH)
	ldir

	pop	ix
	ret


	.section	.bss._sortRecords,"aw",@nobits
	.balign	256
	.global	_gridDist
_gridDist:	.zero	256		; filled by initGridDist() in main.c
.Lcount:	.zero	256		; records per key (only BUCKETS used). Zero between sorts
.Lstart:	.zero	3 * (BUCKETS)	; next free record in scratch per key, 3 bytes each

