; sortRecords: the painter sort of one chunk (main.c, sortChunk).
;
;   void sortRecords(int8_t (*rec)[5], int8_t (*scratch)[5], uint8_t *keys, uint8_t n,
;                    uint8_t bx, uint8_t by, uint8_t bz)
;
; Sorts the n records {X, Y, Z, material, exposed faces} NEAR TO FAR by their Manhattan distance in
; cells from the camera, gridDist[X + bx] + gridDist[Y + by] + gridDist[Z + bz] with 8-bit adds
; (main.c explains why the low byte of the grid index is exact). A stable counting sort in three
; passes over rec, then scratch is copied back over rec. keys must hold n bytes. n must be 1 .. 255.
;
; Measured per sort: 74k cycles for a 153-record scene (the earlier version with 3-byte start
; pointers: 78k; with 4-byte records it took 5.3k for 7 records and 59k for 120, the same sort in
; C 7.9k and 127k, the old qsort 41k-48k and 1.1M-1.8M). About 480 cycles per record; 43k on
; average for the chunks of the test world.
;
; Speed tricks:
; - gridDist, the counts and the start indices each begin on a 256-byte boundary, so setting the
;   low byte of a register pair (ld e, a / ld l, a) indexes them with no add.
; - bx, by and bz are written into the add a, n instructions below (self-modifying code: the
;   program runs from RAM), which leaves b free for djnz.
; - The start of each key in scratch is kept as a record index (a byte), so the starts of all 256
;   possible keys fit one page; placing a record multiplies it by REC_SIZE with mlt.
; - Pass 1 also tracks the largest key (in a patched cp), so the prefix pass only visits keys
;   0 .. max, and for an empty key only tests the count. It clears each used count as it reads
;   it, so they are all zero again for the next sort without a separate clear.
;
; ABI: arguments at sp+3 .. sp+21; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

#include "engine.h"

#define REC_SIZE	5				/* bytes per scene record */
/* Keys are bytes: main.c asserts that no key of a loaded chunk exceeds 255 */

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
	lea	iy, iy + REC_SIZE
	djnz	.Lkey

	; ---- pass 2: the index in scratch where each key starts, nearest key first ----
	; Only keys 0 .. max, and an empty key costs just the test: its start is never read.
	; hl = counts (l = key), de = starts (e = key), c = next free record index
	ld	iy, 0
	add	iy, sp
	ld	de, .Lstart
	ld	c, 0
	ld	l, c
	ld	a, (.Lmax + 1)
	ld	b, a
	inc	b
.Lprefix:
	ld	a, (hl)
	or	a, a
	jr	nz, .Lused
.Lnext:
	inc	l
	djnz	.Lprefix
	jr	.Lplace_start

.Lused:
	ld	(hl), 0			; clear the count for the next sort
	ld	e, l
	ex	de, hl
	ld	(hl), c			; where this key starts
	ex	de, hl
	add	a, c
	ld	c, a			; and the next one (n <= 255: no carry)
	jr	.Lnext

	; ---- pass 3: copy each record to the next free place for its key ----
	; iy = record, ix = keys[i], hl = starts (l = key), de = scratch
.Lplace_start:
	ld	b, (iy + A_N)
	ld	ix, (iy + A_KEYS)
	ld	de, (iy + A_SCRATCH)
	ld	iy, (iy + A_REC)
	ld	hl, .Lstart
.Lplace:
	ld	l, (ix + 0)
	inc	ix
	ld	a, (hl)
	inc	(hl)
	push	hl
	ld	h, a
	ld	l, REC_SIZE
	mlt	hl
	add	hl, de			; scratch + REC_SIZE index
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
	ld	a, (iy + 4)
	ld	(hl), a
	pop	hl
	lea	iy, iy + REC_SIZE
	djnz	.Lplace

	; ---- copy scratch back over the records ----
	ld	iy, 0
	add	iy, sp
	ld	h, (iy + A_N)
	ld	l, REC_SIZE
	mlt	hl
	push	hl
	pop	bc			; REC_SIZE n bytes
	ld	de, (iy + A_REC)
	ld	hl, (iy + A_SCRATCH)
	ldir

	pop	ix
	ret


	.section	.bss._sortRecords,"aw",@nobits
	.balign	256
	.global	_gridDist
_gridDist:	.zero	256		; filled by initGridDist() in main.c
.Lcount:	.zero	256		; records per key. Zero between sorts
.Lstart:	.zero	256		; next free record index in scratch per key

