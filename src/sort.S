; sortRecords: the painter sort of one chunk (chunks.c, sortChunk).
;
;   void sortRecords(int8_t (*rec)[5], int8_t (*scratch)[5], uint8_t *keys, uint8_t n,
;                    const uint8_t *tab, uint8_t bx, uint8_t by, uint8_t bz, uint8_t run)
;
; Sorts the n records {X, Y, Z, material, exposed faces} NEAR TO FAR by the key
; tab[X + bx] + tab[Y + by] + tab[Z + bz] with 8-bit adds (tab 256-aligned). Plainly sortChunk
; (chunks.c) passes gridDist and the grid index biases: the Manhattan distance in cells (chunks.c
; explains why the low byte of the grid index is exact), and runs get 255: they end up after the
; voxels (chunks.h), not drawn. run = 3: there are no runs among the n records (no check at all). With runs, sortTab with the key tables of the three axes, and run = 1 (x runs) or 2 (z
; runs): a run (FACE_RUN_REC) gets tab[X + bx], or tab[Z + bz], only: the start of its slab
; (chunks.h, runs).
; A stable counting sort in three passes over rec, then scratch is copied back over rec. keys must
; hold n bytes. n must be 1 .. 255.
;
; Measured per sort: 74k cycles for a 153-record scene (the earlier version with 3-byte start
; pointers: 78k; with 4-byte records it took 5.3k for 7 records and 59k for 120, the same sort in
; C 7.9k and 127k, the old qsort 41k-48k and 1.1M-1.8M). About 480 cycles per record; 43k on
; average for the chunks of the test world.
;
; Speed tricks:
; - sortTab, the counts and the start indices each begin on a 256-byte boundary, so setting the
;   low byte of a register pair (ld e, a / ld l, a) indexes them with no add.
; - by and the run axis are written into the instructions below (self-modifying code: the program
;   runs from RAM), which leaves b free for djnz.
; - The start of each key in scratch is kept as a record index (a byte), so the starts of all 256
;   possible keys fit one page; placing a record multiplies it by REC_SIZE with mlt.
; - Pass 1 also tracks the largest key (in a patched cp), so the prefix pass only visits keys
;   0 .. max, and for an empty key only tests the count. It clears each used count as it reads
;   it, so they are all zero again for the next sort without a separate clear.
;
; ABI: arguments at sp+3 .. sp+21; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

#include "engine.h"

#if FACE_RUN_REC != 0x80
#error "pass 1 tests the sign of the exposed byte"
#endif

#define REC_SIZE	5				/* bytes per scene record */
/* Keys are bytes: chunks.h asserts that no key of a loaded chunk exceeds 255 */

; argument offsets from sp after push ix
#define A_REC		6
#define A_SCRATCH	9
#define A_KEYS		12
#define A_N		15
#define A_TAB		18

	.assume	ADL = 1

	.section	.text._sortRecords,"ax",@progbits
	.global	_sortRecords
	.type	_sortRecords,@function
_sortRecords:
	ld	iy, 0
	add	iy, sp
	ld	a, (iy + 18)		; patch the biases and the run axis into pass 1
	ld	(.Lbx + 1), a
	ld	a, (iy + 21)
	ld	(.Lby + 1), a
	ld	a, (iy + 24)
	ld	(.Lbz + 1), a
	ld	a, (iy + 27)		; run: 0, 1 (x), 2 (z) or 3 (no runs among them)
	ld	bc, 0x7efd		; ld a, (iy + 4): the run check as assembled
	cp	a, 3
	jr	nz, 4f
	ld	bc, 0x0618		; jr over it (8 bytes)
4:	ld	(.Lkey), bc		; (writes 3 bytes: the displacement again)
	ld	c, a
	ld	a, 4
	ld	(.Lkey + 2), a
	ld	a, c
	ld	hl, .Lrunlast		; 0: runs last
	or	a, a
	jr	z, 2f
	cp	a, 3
	jr	z, 2f
	ld	hl, .Lrunkey
	dec	a
	ld	(.Lrk + 2), a		; (iy + 0) or (iy + 1): fd 7e d
	jr	z, 1f
	inc	a			; (iy + 2)
	ld	(.Lrk + 2), a
	ld	a, (iy + 24)		; + bz
	jr	3f
1:	ld	a, (iy + 18)		; + bx
3:	ld	(.Lrkb + 1), a
2:	ld	(.Lrj + 1), hl
	xor	a, a
	ld	(.Lmax + 1), a		; largest key so far
	push	ix

	; ---- pass 1: each key into keys[], and how many records have it ----
	; iy = record, ix = keys[i], de = sortTab (e = index), hl = counts (l = key), b = left
	ld	b, (iy + A_N - 3)
	ld	ix, (iy + A_KEYS - 3)
	ld	de, (iy + A_TAB - 3)
	ld	iy, (iy + A_REC - 3)
	ld	hl, .Lcount
.Lkey:
	ld	a, (iy + 4)		; (patched to jr .Lkey_v when there are no runs)
	or	a, a
.Lrj:	jp	m, .Lrunkey		; FACE_RUN_REC (patched: .Lrunkey or .Lrunlast)
.Lkey_v:
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
.Lkeyed:
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
	jr	.Lprefix_start

	; a run: the key of the start of its slab
.Lrunkey:
.Lrk:	ld	a, (iy + 0)		; (iy + 0) or (iy + 2) (patched)
.Lrkb:	add	a, 0			; + bx or bz (patched)
	ld	e, a
	ld	a, (de)
	jr	.Lkeyed
.Lrunlast:
	ld	a, 255
	jr	.Lkeyed

	; ---- pass 2: the index in scratch where each key starts, nearest key first ----
	; Only keys 0 .. max, and an empty key costs just the test: its start is never read.
	; hl = counts (l = key), de = starts (e = key), c = next free record index
.Lprefix_start:
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


; uint8_t sortAxisKeys(uint8_t *t, int8_t d, uint8_t len): the keys of one axis of a chunk with runs
; (sortChunk): t[i] = |d + i| minus its least over i < len (d + i is a cell minus the camera cell,
; |d + i| <= 127). Returns the greatest
	.section	.text._sortAxisKeys,"ax",@progbits
	.global	_sortAxisKeys
	.type	_sortAxisKeys,@function
_sortAxisKeys:
	ld	iy, 0
	add	iy, sp
	ld	hl, (iy + 3)
	ld	c, (iy + 6)		; d
	ld	b, (iy + 9)		; len (at least 1)
	ld	e, c			; least: d when d > 0
	ld	a, c
	or	a, a
	jr	z, 1f
	jp	p, 2f
1:	add	a, b
	dec	a			; the last: d + len - 1
	ld	e, 0
	jp	p, 2f			; reaches 0: least 0
	neg
	ld	e, a			; all below: -(the last)
2:	ld	d, 0			; the greatest so far
3:	ld	a, c
	or	a, a
	jp	p, 4f
	neg
4:	sub	a, e
	ld	(hl), a
	inc	hl
	cp	a, d
	jr	c, 5f
	ld	d, a
5:	inc	c
	djnz	3b
	ld	a, d
	ret

; void sortScaleKeys(uint8_t *t, uint8_t s): t[i] *= s for the CHUNK_SIZE keys of the run axis
	.section	.text._sortScaleKeys,"ax",@progbits
	.global	_sortScaleKeys
	.type	_sortScaleKeys,@function
_sortScaleKeys:
	ld	iy, 0
	add	iy, sp
	ld	de, (iy + 3)
	ld	c, (iy + 6)
	ld	b, 8
1:	ld	a, (de)
	ld	h, a
	ld	l, c
	mlt	hl
	ld	a, l
	ld	(de), a
	inc	de
	djnz	1b
	ret


	.section	.bss._sortRecords,"aw",@nobits
	.balign	256
	.global	_gridDist
_gridDist:	.zero	256		; filled by initGridDist() in chunks.c
	.global	_sortTab
_sortTab:	.zero	256		; the key tables of the chunk being sorted (sortChunk)
.Lcount:	.zero	256		; records per key. Zero between sorts
.Lstart:	.zero	256		; next free record index in scratch per key


	.section	.text._sortRecords,"ax",@progbits
	.if	.Lkey_v - .Lkey != 8
	.error	"the jr patched over the run check of pass 1 skips 8 bytes"
	.endif
