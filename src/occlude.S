; chunkSeen: whether a chunk can still show, from the coverage fillBoxes has built so far.
; cullLoop: the per-chunk loop of cullChunks (scene.c).
;
;   uint8_t chunkSeen(uint8_t w, int24_t yt, int24_t yb)
;
; w is the window position of the chunk: cullLoop leaves the camera-space x and z of the first
; corner of its footprint (1/SUB units, unscaled) as row and column tables. yt and yb are the
; camera-space y of the top and bottom of its records. Per frame (cullChunks): occ_dxmin, occ_dxmax, occ_dzmin and occ_dzmax, the least and
; greatest offsets of its 4 corners from the first.
;
; The screen rectangle tested (fillSees) is conservative: every point a box of the chunk projects
; lies in its footprint, between the records' y, at a depth in [NEAR_PLANE, far plane). So it lies
; in the camera-space box [xmin, xmax] x [yt, yb] x [zn, zf] of the corners, widened by OCC_SLACK
; (the rounding of the per-frame tables; zf at most the far plane). Its screen rectangle: each
; edge at the depth that moves it outwards (x / z is least at zn for x < 0, at zf for x >= 0), then
; a pixel more each way than the rounding of the tables needs. (The extremes of the 4 corners,
; which are tighter, caught a few more chunks but cost three times as much.)
; A chunk reaching nearer than NEAR_PLANE + 2 OCC_SLACK is always seen (it nearly spans the view).
;
; Projection as in project.S: FOCAL_LENGTH m / z = expTable[logTable[m] - logTable[z] - EXP_TABLE_MIN].
; Depths are within [NEAR_PLANE, far plane]. m beyond the log table, or more than about 5 z / 4
; (off the screen anyway), saturates to 255 pixels without a lookup, so every index is in range:
; the exp index is at least LOG_ZERO - log(far plane), the table start (gen_tables.py), and at
; most DMAX3 / 3.
;
; ABI: arguments at sp+3 ...; result in A; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

#include "engine.h"

#define WINDOW		7		/* chunks.h (scene.c checks it) */

#define OCC_SLACK	32		/* scene.c checks it is CULL_SLACK */
#define DMAX3		252		/* 3 * 84 log steps: a ratio of about 1.256 */
#define SAT		255
#define EXP_BASE	_expTable - 3 * EXP_TABLE_MIN

#if DMAX3 / 3 >= EXP_TABLE_LEN + EXP_TABLE_MIN
#error "expTable must reach DMAX3"
#endif
#if TABLE_FAR_PLANE >= LOG_TABLE_LEN
#error "logTable must reach the far plane"
#endif
#if VIEW_X1 - VIEW_X0 != 160 || VIEW_H != 120 || CENTRE_Y != 60
#error "the screen centre below"
#endif

; offsets into .Lv
#define O_X	0		/* x of the first corner */
#define O_LN	3		/* logTable entries of the nearest depth (for values < 0: left, up) */
#define O_LF	6		/* ... and of the farthest */
#define O_R	9		/* the column past the rectangle */

	.assume	ADL = 1

; hl = logTable[hl] (an x3 entry). clobbers de
	.macro	LOG3
	push	hl
	pop	de
	add	hl, hl
	add	hl, de
	ld	de, _logTable
	add	hl, de
	ld	hl, (hl)
	.endm

; hl = -hl. clobbers de
	.macro	NEG24
	ex	de, hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de
	.endm

	.section	.text._chunkSeen,"ax",@progbits
	.global	_chunkSeen
	.type	_chunkSeen,@function
_chunkSeen:
	push	ix
	ld	iy, 0
	add	iy, sp			; iy + 6 w, 9 yt, 12 yb
	ld	ix, .Lv
	; the first corner of window position w: (rowx[j] + colx[i], rowz[j] + colz[i]) (cullLoop)
	ld	de, 0
	ld	e, (iy + 6)
	ld	hl, .Lwij
	add	hl, de
	add	hl, de
	ld	e, (hl)			; 3 i
	inc	hl
	ld	a, (hl)			; 3 j
	ld	hl, .Lcolx
	add	hl, de
	ld	bc, (hl)
	ld	hl, .Lcolz
	add	hl, de
	ld	hl, (hl)
	push	hl
	ld	e, a
	ld	hl, .Lrowx
	add	hl, de
	ld	hl, (hl)
	add	hl, bc
	ld	(ix + O_X), hl
	ld	hl, .Lrowz
	add	hl, de
	ld	hl, (hl)
	pop	bc
	add	hl, bc			; z of the first corner
	push	hl
	; the nearest depth: too near, and the chunk is seen
	ld	de, (_occ_dzmin)
	add	hl, de
	ld	de, NEAR_PLANE + 2 * OCC_SLACK
	or	a, a
	sbc	hl, de
	jp	m, .Lseen_pop
	ld	de, NEAR_PLANE + OCC_SLACK
	add	hl, de			; zn = zmin - OCC_SLACK
	LOG3
	ld	(ix + O_LN), hl
	; the farthest, at most the far plane
	pop	hl
	ld	de, (_occ_dzmax)
	add	hl, de
	ld	de, OCC_SLACK
	add	hl, de
	ld	de, (_cam_far)
	or	a, a
	sbc	hl, de
	jp	m, 1f
	ld	hl, 0
1:	add	hl, de			; zf
	LOG3
	ld	(ix + O_LF), hl

	; ---- columns [l, r) = [79 + offset(xmin - slack), 82 + offset(xmax + slack)) ----
	ld	hl, (ix + O_X)
	ld	de, (_occ_dxmax)
	add	hl, de
	ld	de, OCC_SLACK
	add	hl, de
	call	.Lhi
	ld	de, 82
	add	hl, de
	ld	de, 160
	or	a, a
	sbc	hl, de
	jp	m, 2f
	ld	hl, 0			; at most the right edge
2:	add	hl, de
	ld	(ix + O_R), l
	ld	de, 1
	or	a, a
	sbc	hl, de
	jp	m, .Lhidden		; r <= 0
	ld	hl, (ix + O_X)
	ld	de, (_occ_dxmin)
	add	hl, de
	ld	de, -OCC_SLACK
	add	hl, de
	call	.Llo
	ld	de, 79
	add	hl, de
	ld	de, 0
	or	a, a
	sbc	hl, de
	jp	p, 3f
	ld	hl, 0			; at least the left edge
3:	ld	a, l			; l >= 0; with r <= 160, l < r also means l < 160
	ld	e, (ix + O_R)
	or	a, a
	sbc	hl, de
	jp	p, .Lhidden		; l >= r
	ld	(ix + O_X), a		; (x is not needed any more)

	; ---- rows [t, b) = [59 + offset(yt), 62 + offset(yb)) ----
	ld	hl, (iy + 9)
	call	.Llo
	ld	de, 59
	add	hl, de
	ld	de, 0
	or	a, a
	sbc	hl, de
	jp	p, 4f
	ld	hl, 0			; at least the first row
4:	push	hl			; t
	ld	hl, (iy + 12)
	call	.Lhi
	ld	de, 62
	add	hl, de
	ld	de, 120
	or	a, a
	sbc	hl, de
	jp	m, 5f
	ld	hl, 0			; at most the last
5:	add	hl, de
	pop	de			; t
	or	a, a
	sbc	hl, de			; rows b - t
	jp	m, .Lhidden
	jr	z, .Lhidden		; (so t < 120 as well)
	; fillSees(l, r, t, rows)
	push	hl
	push	de
	ld	e, (ix + O_R)
	push	de
	ld	e, (ix + O_X)
	push	de
	call	_fillSees
	pop	de
	pop	de
	pop	de
	pop	de
	pop	ix
	ret
.Lseen_pop:
	pop	hl
	ld	a, 1
	pop	ix
	ret
.Lhidden:
	xor	a, a
	pop	ix
	ret


; ---- hl = the screen offset of value hl: .Llo at the nearest depth when it is < 0, else the
; farthest (the outward edge on the left or top); .Lhi at the nearest when > 0, else the
; farthest. clobbers bc, de
.Llo:
	ld	de, 0
	or	a, a
	sbc	hl, de
	jp	p, 1f
	NEG24
	ld	bc, (ix + O_LN)
	call	.Loff
	NEG24
	ret
1:	ld	bc, (ix + O_LF)
	jr	.Loff

.Lhi:
	ld	de, 0
	or	a, a
	sbc	hl, de
	jp	m, 1f
	jr	z, 1f
	ld	bc, (ix + O_LN)
	jr	.Loff
1:	NEG24
	ld	bc, (ix + O_LF)
	call	.Loff
	NEG24
	ret

; hl = FOCAL_LENGTH m / z for m = hl >= 0, bc = logTable[z]; SAT when m or the ratio is too
; large (see the top). clobbers de
.Loff:
	push	hl
	ld	de, -LOG_TABLE_LEN
	add	hl, de			; carry: m >= LOG_TABLE_LEN
	pop	hl
	jr	c, .Lsat
	LOG3				; (no carry out of the address)
	sbc	hl, bc			; d = log m - log z, x3
	ld	de, DMAX3
	or	a, a
	sbc	hl, de
	jp	p, .Lsat
	add	hl, de
	ld	de, EXP_BASE
	add	hl, de
	ld	hl, (hl)
	ret
.Lsat:
	ld	hl, SAT
	ret


	.section	.bss._chunkSeen,"aw",@nobits
.Lv:	.zero	10

	.extern	_logTable
	.extern	_expTable
	.extern	_fillSees
	.extern	_cam_far
	.extern	_occ_dzmin
	.extern	_occ_dzmax
	.extern	_occ_dxmin
	.extern	_occ_dxmax


; ==================================================================================================
; void cullLoop(int24_t row_x, int24_t row_z): the per-chunk tests of cullChunks (scene.c, where
; they are described). Chunk (i, j) of the window (i along world x) has its first corner at
; (row_x, row_z) + i u + j v, u = (cull_ux, cull_uz), v = (-cull_uz, cull_ux); cull_xmin ...
; cull_zmax are the extremes of its corners relative to that one, with the slack. Each test is a
; linear function of (i, j) that must not be negative:
;   A = z1 - NEAR_PLANE          (z1 = z + zmax: not behind the camera)
;   B = cam_far - 1 - z0         (z0 = z + zmin: nearer than the far plane)
;   C = 4 x1 + 5 z1              (x1 = x + xmax: not left of the view)
;   D = 5 z1 - 4 x0              (x0 = x + xmin: not right of it)
; so they are stepped by addition, and a chunk is kept when the OR of their top bytes has no sign.
; Writes chunk_visible[7 j + i], and the corner tables chunkSeen reads: colx[i] = i ux,
; colz[i] = i uz, rowx[j] = row_x - j uz, rowz[j] = row_z + j ux
#define T_A	0		/* running values, then their steps along i and along j, then row starts */
#define T_B	3
#define T_C	6
#define T_D	9
#define T_IA	12
#define T_IB	15
#define T_IC	18
#define T_ID	21
#define T_JA	24
#define T_JB	27
#define T_JC	30
#define T_JD	33
#define T_RA	36
#define T_RB	39
#define T_RC	42
#define T_RD	45
#define T_X1	48		/* row_x + xmax, row_x + xmin, row_z + zmax */
#define T_X0	51
#define T_Z1	54

	.section	.text._cullLoop,"ax",@progbits
	.global	_cullLoop
	.type	_cullLoop,@function
_cullLoop:
	push	ix
	ld	iy, 0
	add	iy, sp			; iy + 6 row_x, 9 row_z
	ld	ix, .Lt
	; the corner tables
	ld	hl, (iy + 6)
	ld	(.Lrowx), hl
	ld	hl, (iy + 9)
	ld	(.Lrowz), hl
	or	a, a
	sbc	hl, hl
	ld	(.Lcolx), hl
	ld	(.Lcolz), hl
	ld	iy, .Lcolx
	ld	b, WINDOW - 1
1:	ld	hl, (iy + 0)		; colx
	ld	de, (_cull_ux)
	add	hl, de
	ld	(iy + 3), hl
	ld	hl, (iy + 3 * WINDOW)	; colz
	ld	de, (_cull_uz)
	add	hl, de
	ld	(iy + 3 * WINDOW + 3), hl
	ld	hl, (iy + 6 * WINDOW)	; rowx
	or	a, a
	sbc	hl, de
	ld	(iy + 6 * WINDOW + 3), hl
	ld	hl, (iy + 9 * WINDOW)	; rowz
	ld	de, (_cull_ux)
	add	hl, de
	ld	(iy + 9 * WINDOW + 3), hl
	lea	iy, iy + 3
	djnz	1b

	; the starts: x1, x0, z1 of chunk (0, 0)
	ld	hl, (.Lrowx)
	ld	de, (_cull_xmax)
	add	hl, de
	ld	(ix + T_X1), hl
	ld	hl, (.Lrowx)
	ld	de, (_cull_xmin)
	add	hl, de
	ld	(ix + T_X0), hl
	ld	hl, (.Lrowz)
	ld	de, (_cull_zmax)
	add	hl, de
	ld	(ix + T_Z1), hl
	; A = z1 - NEAR_PLANE
	ld	de, -NEAR_PLANE
	add	hl, de
	ld	(ix + T_RA), hl
	; B = cam_far - 1 - z0
	ld	hl, (.Lrowz)
	ld	de, (_cull_zmin)
	add	hl, de
	ex	de, hl
	ld	hl, (_cam_far)
	dec	hl
	or	a, a
	sbc	hl, de
	ld	(ix + T_RB), hl
	; C = 4 x1 + 5 z1, D = 5 z1 - 4 x0
	ld	hl, (ix + T_Z1)
	push	hl
	pop	de
	add	hl, hl
	add	hl, hl
	add	hl, de			; 5 z1
	push	hl
	ld	de, (ix + T_X1)
	ex	de, hl
	add	hl, hl
	add	hl, hl
	add	hl, de
	ld	(ix + T_RC), hl
	pop	hl
	ld	de, (ix + T_X0)
	ex	de, hl
	add	hl, hl
	add	hl, hl
	ex	de, hl
	or	a, a
	sbc	hl, de
	ld	(ix + T_RD), hl
	; steps along i: u = (ux, uz). A: uz, B: -uz, C: 4 ux + 5 uz, D: 5 uz - 4 ux
	ld	hl, (_cull_uz)
	ld	(ix + T_IA), hl
	ex	de, hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de
	ld	(ix + T_IB), hl
	ld	hl, (_cull_ux)
	ld	de, (_cull_uz)
	call	.Lcd_steps
	ld	(ix + T_IC), hl
	ld	(ix + T_ID), de
	; along j: v = (-uz, ux). A: ux, B: -ux, C: 5 ux - 4 uz, D: 5 ux + 4 uz
	ld	hl, (_cull_ux)
	ld	(ix + T_JA), hl
	ex	de, hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de
	ld	(ix + T_JB), hl
	ld	de, (_cull_uz)
	or	a, a
	sbc	hl, hl
	sbc	hl, de			; -uz
	ld	de, (_cull_ux)
	call	.Lcd_steps
	ld	(ix + T_JC), hl
	ld	(ix + T_JD), de

	; ---- the window ----
	ld	iy, _chunk_visible
	ld	c, WINDOW
.Lrow:
	ld	hl, (ix + T_RA)
	ld	(ix + T_A), hl
	ld	hl, (ix + T_RB)
	ld	(ix + T_B), hl
	ld	hl, (ix + T_RC)
	ld	(ix + T_C), hl
	ld	hl, (ix + T_RD)
	ld	(ix + T_D), hl
	ld	b, WINDOW
.Lcol:
	ld	a, (ix + T_A + 2)
	or	a, (ix + T_B + 2)
	or	a, (ix + T_C + 2)
	or	a, (ix + T_D + 2)
	rlca
	and	a, 1
	xor	a, 1			; 1: no test negative
	ld	(iy), a
	inc	iy
	.irp	t, T_A, T_B, T_C, T_D
	ld	hl, (ix + \t)
	ld	de, (ix + \t + T_IA)
	add	hl, de
	ld	(ix + \t), hl
	.endr
	djnz	.Lcol
	.irp	t, T_RA, T_RB, T_RC, T_RD
	ld	hl, (ix + \t)
	ld	de, (ix + \t - T_RA + T_JA)
	add	hl, de
	ld	(ix + \t), hl
	.endr
	dec	c
	jp	nz, .Lrow
	pop	ix
	ret

; for a step (dx, dz) = (hl, de): hl = 4 dx + 5 dz (C), de = 5 dz - 4 dx (D)
.Lcd_steps:
	add	hl, hl
	add	hl, hl			; 4 dx
	push	hl
	push	de
	pop	hl
	add	hl, hl
	add	hl, hl
	add	hl, de			; 5 dz
	pop	de
	push	hl
	add	hl, de			; C
	ex	(sp), hl
	or	a, a
	sbc	hl, de			; D
	ex	de, hl
	pop	hl
	ret

; window position w = 7 j + i: 3 i, 3 j (offsets into the corner tables)
	.section	.rodata._cullLoop,"a",@progbits
.Lwij:
	.irp	j, 0, 1, 2, 3, 4, 5, 6
	.irp	i, 0, 1, 2, 3, 4, 5, 6
	.byte	3 * \i, 3 * \j
	.endr
	.endr

	.section	.bss._cullLoop,"aw",@nobits
.Lt:	.zero	57
.Lcolx:	.zero	3 * WINDOW		; (the four tables are consecutive: cullLoop steps through them)
.Lcolz:	.zero	3 * WINDOW
.Lrowx:	.zero	3 * WINDOW
.Lrowz:	.zero	3 * WINDOW

	.extern	_chunk_visible
	.extern	_cull_xmin
	.extern	_cull_xmax
	.extern	_cull_zmin
	.extern	_cull_zmax
	.extern	_cull_ux
	.extern	_cull_uz
