; projectBox: cull and project one voxel.
;
;   uint8_t projectBox(int24_t X, int24_t Y, int24_t Z)
;
; Culls the world voxel [X-1, X] x [Y-1, Y] x [Z-1, Z] in camera space, projects its corners into
; box_out and classifies it against the viewport. Returns BOX_* (engine.h) in A. Reads the
; per-frame camera state that setupCamera() in main.c writes (layout in engine.h).
;
; Why assembly: clang turns every multiply, right shift and even AND into a library call. And the
; CE is memory bound: roughly 2 cycles per byte touched, opcode bytes included. So this code
; avoids calls and 3-byte absolute addresses where it can, and keeps every camera value x3 so it
; is directly a byte offset into an int24 table (no index multiplies).
;
; Projection is lookups only: FOCAL_LENGTH * a / z = expTable[logTable[|a|] - logTable[z] - EXP_TABLE_MIN].
; logTable entries are x3, so with bc = logTable[z] - EXP_BASE, the entry for |a| minus bc is the
; address of the expTable entry. gen_tables.py proves every index stays inside the tables for boxes
; that pass the culling below: change them together.
;
; Signed compares use the sign of the sbc/adc result: every value here is far below 2^22, so the
; subtraction cannot overflow.
;
; ABI: arguments at sp+3, sp+6, sp+9; result in A; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

#include "engine.h"

#if TABLE_CULL_X_NUM != 5 || TABLE_CULL_X_DEN != 4
#error "the horizontal culling below compares 4 x with 5 z"
#endif

#define EXP_BASE	_expTable - 3 * EXP_TABLE_MIN
#define OUT(k)		_box_out + 3 * k

	.assume	ADL = 1

; ---- macros ----

; in: hl = camera value v (x3). out: hl = logTable entry for |v|, a = 1 if v <= 0 else 0
; (v = 0 projects to the centre either way). clobbers de
	.macro	LOG_ABS
	xor	a, a			; a = 0, carry clear
	ex	de, hl
	sbc	hl, hl
	sbc	hl, de			; hl = -v
	jp	p, 1f			; v <= 0: -v is |v|
	ex	de, hl			; v > 0
	jr	2f
1:	inc	a
2:	ld	de, _logTable
	add	hl, de
	ld	hl, (hl)
	.endm

; in: hl = depth z (x3, inside the culling range). out: bc = logTable[z] - EXP_BASE. clobbers de, hl
	.macro	LOG_Z
	ld	de, _logTable
	add	hl, de
	ld	hl, (hl)
	ld	de, EXP_BASE
	or	a, a
	sbc	hl, de
	push	hl
	pop	bc
	.endm

; in: hl = logTable entry for |v|, a = sign from LOG_ABS, bc from LOG_Z.
; out: hl = centre +- FOCAL_LENGTH * |v| / z. bc and a preserved, de clobbered
	.macro	PROJECT centre
	or	a, a			; clear carry
	sbc	hl, bc			; address of the expTable entry
	ld	hl, (hl)
	bit	0, a
	jr	nz, 1f
	ld	de, \centre
	add	hl, de
	jr	2f
1:	ex	de, hl
	ld	hl, \centre
	or	a, a
	sbc	hl, de
2:
	.endm

; project a stored log/sign pair and store it
	.macro	PROJECT_TO lv, sv, centre, slot
	ld	hl, (\lv)
	ld	a, (\sv)
	PROJECT	\centre
	ld	(OUT(\slot)), hl
	.endm

; axis-aligned face at the depth in bc: left right top bottom into slots s0..s3
	.macro	FACE s0, s1, s2, s3
	PROJECT_TO .Llx0, .Lsx0, CENTRE_X, \s0
	PROJECT_TO .Llx1, .Lsx1, CENTRE_X, \s1
	PROJECT_TO .Llt, .Lst, CENTRE_Y, \s2
	PROJECT_TO .Llb, .Lsb, CENTRE_Y, \s3
	.endm

; vertical edge at camera (.Lcx, .Lcz): x, top y, bottom y into slots sx, st, sb
	.macro	CORNER sx, st, sb
	ld	hl, (.Lcz)
	LOG_Z
	ld	hl, (.Lcx)
	LOG_ABS
	PROJECT	CENTRE_X
	ld	(OUT(\sx)), hl
	PROJECT_TO .Llt, .Lst, CENTRE_Y, \st
	PROJECT_TO .Llb, .Lsb, CENTRE_Y, \sb
	.endm

; leave with a (BOX_*_CLIP) unless slot k is a viewport column / row
	.macro	INSIDE_X k
	ld	hl, (OUT(\k))
	ld	de, VIEW_X0
	or	a, a
	sbc	hl, de
	ld	de, VIEW_X1 - VIEW_X0
	or	a, a
	sbc	hl, de			; unsigned: carry if inside
	jp	nc, .Ldone
	.endm
	.macro	INSIDE_Y k
	ld	hl, (OUT(\k))
	ld	de, VIEW_H
	or	a, a
	sbc	hl, de
	jp	nc, .Ldone
	.endm

; grid line (argument at iy + arg) - 1 -> table index n * mult bytes, n checked against
; [0, 2 * GRID_RANGE) so n + 1 is in the table too. clobbers de
	.macro	GRID_INDEX arg, bias
	ld	hl, (iy + \arg)
	ld	de, (\bias)
	add	hl, de
	ld	de, 2 * GRID_RANGE
	or	a, a
	sbc	hl, de
	jp	nc, .Lskip
	add	hl, de
	push	hl
	pop	de
	add	hl, hl
	add	hl, de			; 3 n
	.endm


	.section	.text._projectBox,"ax",@progbits
	.global	_projectBox
	.type	_projectBox,@function
_projectBox:
	ld	iy, 0
	add	iy, sp			; iy + 3 = X, iy + 6 = Y, iy + 9 = Z
	push	ix

	; ---- per-frame tables for the box: camera y of its top, and its rotated x and z lines ----
	GRID_INDEX 6, _cam_bias_y
	ld	de, _camY_
	add	hl, de
	ld	hl, (hl)
	ld	(.Lyt), hl

	GRID_INDEX 9, _cam_bias_z
	add	hl, hl			; 6 n: {ZS, ZC} pairs
	ld	de, _rotZ_
	add	hl, de
	push	hl
	pop	ix

	GRID_INDEX 3, _cam_bias_x
	add	hl, hl			; {XC, XS} pairs
	ld	de, _rotX_
	add	hl, de
	push	hl
	pop	iy			; the arguments are no longer needed

	; camera space of corner (X-1, Z-1): x00 = XC - ZS in bc, z00 = XS + ZC in iy
	ld	hl, (iy + 0)
	ld	de, (ix + 0)
	or	a, a
	sbc	hl, de
	push	hl
	pop	bc
	ld	hl, (iy + 3)
	ld	de, (ix + 3)
	add	hl, de
	push	hl
	pop	iy

	; ---- cull in camera space (engine.h lists the tests) ----
	ld	de, (_cam_zlo)
	or	a, a
	sbc	hl, de
	ld	de, (_cam_zspan)
	or	a, a
	sbc	hl, de			; unsigned: carry if every corner is in depth range
	jp	nc, .Lskip

	; the view is |x| <= 1.25 z (CULL_X_NUM / CULL_X_DEN), so compare 4 x with 5 z
	ld	(.Lx00), bc
	lea	hl, iy + 0
	add	hl, hl
	add	hl, hl
	lea	de, iy + 0
	add	hl, de
	ex	de, hl			; de = 5 z00
	push	bc
	pop	hl
	add	hl, hl
	add	hl, hl
	push	hl
	pop	bc			; bc = 4 x00

	ld	hl, (_cam_cxr)
	add	hl, de
	or	a, a
	sbc	hl, bc			; cxr + 5 z00 - 4 x00
	jp	m, .Lskip

	push	bc
	pop	hl
	add	hl, de
	ld	de, (_cam_cxl)
	or	a, a
	sbc	hl, de			; 4 x00 + 5 z00 - cxl
	jp	m, .Lskip

	ld	hl, (_cam_kzmax3)
	lea	de, iy + 0
	add	hl, de
	ld	de, (.Lyt)
	or	a, a
	sbc	hl, de			; kzmax + z00 - yt
	jp	m, .Lskip

	lea	hl, iy + 0
	ld	de, (.Lyt)
	add	hl, de
	ld	de, (_cam_cyl)
	or	a, a
	sbc	hl, de			; yt + z00 - cyl
	jp	m, .Lskip

	ld	(.Lz00), iy

	; ---- the y logs are shared by every corner ----
	ld	hl, (.Lyt)
	LOG_ABS
	ld	(.Llt), hl
	ld	(.Lst), a
	ld	hl, (.Lyt)
	ld	de, 3 * SUB
	add	hl, de			; box bottom
	LOG_ABS
	ld	(.Llb), hl
	ld	(.Lsb), a

	ld	a, (_cam_cardinal)
	or	a, a
	jp	z, .Lgeneral

	; ==== facing N/E/S/W: the box is axis aligned in camera space ====
	ld	hl, (.Lx00)
	ld	de, (_cam_kxmin3)
	add	hl, de
	LOG_ABS
	ld	(.Llx0), hl
	ld	(.Lsx0), a
	ld	hl, (.Lx00)
	ld	de, (_cam_kxmax3)
	add	hl, de
	LOG_ABS
	ld	(.Llx1), hl
	ld	(.Lsx1), a

	; Each far vertex lies between the screen centre and its near vertex, and the centre is
	; inside the viewport. So the far face alone decides whether anything is on screen, and the
	; near face alone whether everything is.
	ld	hl, (.Lz00)
	ld	de, (_cam_kzmax3)
	add	hl, de
	LOG_Z
	FACE	4, 5, 6, 7		; fl fr ft fb

	ld	hl, (OUT(4))
	ld	de, VIEW_X1
	or	a, a
	sbc	hl, de
	jp	p, .Lskip		; fl >= X1
	ld	hl, (OUT(5))
	ld	de, VIEW_X0
	or	a, a
	sbc	hl, de
	jp	m, .Lskip		; fr < X0
	ld	hl, (OUT(6))
	ld	de, VIEW_H
	or	a, a
	sbc	hl, de
	jp	p, .Lskip		; ft >= H
	ld	hl, (OUT(7))
	add	hl, hl
	jp	c, .Lskip		; fb < 0

	ld	hl, (.Lz00)
	ld	de, (_cam_kzmin3)
	add	hl, de
	LOG_Z
	FACE	0, 1, 2, 3		; nl nr nt nb

	ld	a, BOX_AXIS_CLIP
	INSIDE_X 0
	INSIDE_X 1
	INSIDE_Y 2
	INSIDE_Y 3
	ld	a, BOX_AXIS_NOCLIP
.Ldone:
	pop	ix
	ret

.Lskip:
	pop	ix
	xor	a, a			; BOX_SKIP
	ret

	; ==== any other yaw: project the 4 vertical edges ====
.Lgeneral:
	ld	hl, (.Lx00)
	ld	(.Lcx), hl
	ld	hl, (.Lz00)
	ld	(.Lcz), hl
	CORNER	0, 4, 8

	ld	hl, (.Lx00)		; corner 1: + (c, s)
	ld	de, (_cam_cos3)
	add	hl, de
	ld	(.Lcx), hl
	ld	hl, (.Lz00)
	ld	de, (_cam_sin3)
	add	hl, de
	ld	(.Lcz), hl
	CORNER	1, 5, 9

	ld	hl, (.Lx00)		; corner 2: + (-s, c)
	ld	de, (_cam_sin3)
	or	a, a
	sbc	hl, de
	ld	(.Lcx), hl
	ld	hl, (.Lz00)
	ld	de, (_cam_cos3)
	add	hl, de
	ld	(.Lcz), hl
	CORNER	2, 6, 10

	ld	hl, (.Lcx)		; corner 3: corner 2 + (c, s)
	ld	de, (_cam_cos3)
	add	hl, de
	ld	(.Lcx), hl
	ld	hl, (.Lcz)
	ld	de, (_cam_sin3)
	add	hl, de
	ld	(.Lcz), hl
	CORNER	3, 7, 11

	; Culling already removed nearly everything off screen, so only test for fully inside.
	; Tops are above bottoms, so (uint)top < H and (uint)bottom < H cover all four y bounds.
	ld	a, BOX_GEN_CLIP
	INSIDE_X 0
	INSIDE_X 1
	INSIDE_X 2
	INSIDE_X 3
	INSIDE_Y 4
	INSIDE_Y 5
	INSIDE_Y 6
	INSIDE_Y 7
	INSIDE_Y 8
	INSIDE_Y 9
	INSIDE_Y 10
	INSIDE_Y 11
	ld	a, BOX_GEN_NOCLIP
	jp	.Ldone


	.section	.bss._projectBox,"aw",@nobits
.Lyt:	.zero	3			; camera y of the box top (x3)
.Lx00:	.zero	3			; camera space of corner (X-1, Z-1) (x3)
.Lz00:	.zero	3
.Lcx:	.zero	3			; current corner (general path)
.Lcz:	.zero	3
.Llt:	.zero	3			; log entries of |top|, |bottom|, |xmin|, |xmax|
.Llb:	.zero	3
.Llx0:	.zero	3
.Llx1:	.zero	3
.Lst:	.zero	1			; and their signs
.Lsb:	.zero	1
.Lsx0:	.zero	1
.Lsx1:	.zero	1

	.extern	_cam_bias_x
	.extern	_cam_bias_y
	.extern	_cam_bias_z
	.extern	_rotX_
	.extern	_rotZ_
	.extern	_camY_
	.extern	_cam_cos3
	.extern	_cam_sin3
	.extern	_cam_kxmin3
	.extern	_cam_kxmax3
	.extern	_cam_kzmin3
	.extern	_cam_kzmax3
	.extern	_cam_zlo
	.extern	_cam_zspan
	.extern	_cam_cxl
	.extern	_cam_cxr
	.extern	_cam_cyl
	.extern	_cam_cardinal
	.extern	_logTable
	.extern	_expTable
	.extern	_box_out
