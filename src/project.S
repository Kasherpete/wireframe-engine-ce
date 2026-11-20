; projectRecords: cull and project the scene.
;
;   uint8_t projectRecords(const int8_t (*rec)[5], uint8_t n, projected_box_t *out, uint8_t room,
;                          int24_t bias_x, int24_t bias_z)
;
; For each record {X, Y, Z, material, exposed faces} of one chunk, in order: the faces facing the
; camera (face_x/y/z, by grid index) that are also exposed; none, and the box is skipped. Otherwise
; the voxel is culled in camera space, its corners projected and the result classified (BOX_*,
; engine.h). Kept boxes go to out[] in record order, with their faces and material, until room of
; them are kept. Returns how many. Reads the per-frame camera state that setupCamera() in main.c
; writes (layout in engine.h).
;
; X and Z are chunk local: the grid index of a record is X + bias_x (Z + bias_z, and Y + cam_bias_y),
; where main.c passes bias_x = cam_bias_x + the x of the first cell of the chunk + 1. The voxel
; spans the cell whose grid index that is (engine.h).
;
; Level of detail: a box whose nearest corner is at least cam_lod_rect deep becomes BOX_RECT, the
; rectangle of its x and y extent projected at its centre depth (4 projections instead of 12).
; From cam_lod_pix on it is a 2 x 2 block of logical pixels at its projected centre (2 projections).
; Both keep only the visible side (x or z) that points at the camera position most (.Llod_side).
;
; Why assembly: clang turns every multiply, right shift and even AND into a library call. And the
; CE is memory bound: roughly 2 cycles per byte touched, opcode bytes included. So this code
; avoids calls and 3-byte absolute addresses where it can, and keeps every camera value x3 so it
; is directly a byte offset into an int24 table (no index multiplies).
;
; Registers: IX points at .Lvars, the block below that holds the locals and the per-frame camera
; scalars (VAR(V_x) addresses them, 3 bytes per access against 4 or 5 for an absolute address).
; IY points at the record, then at its rotation table entries, and once a box survives culling at
; its slot in out[].
;
; Projection is lookups only: FOCAL_LENGTH * a / z = expTable[logTable[|a|] - logTable[z] - EXP_TABLE_MIN].
; logTable entries are x3, so with bc = logTable[z] - EXP_BASE, the entry for |a| minus bc is the
; address of the expTable entry. gen_tables.py proves every index stays inside the tables for boxes
; that pass the culling below: change them together. The level of detail projects corner values
; (or values between them) at a depth between the nearest and the farthest corner, which the same
; proof covers.
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
#define OUT(k)		iy + PB_V + 3 * k
#define VAR(o)		ix + o

; byte offsets into .Lvars (checked against its labels at the end of the file)
#define V_YT	0
#define V_X00	3
#define V_Z00	6
#define V_CX	9
#define V_CZ	12
#define V_LT	15
#define V_LB	18
#define V_LX0	21
#define V_LX1	24
#define V_ST	27
#define V_SB	28
#define V_SX0	29
#define V_SX1	30
#define V_CAM_COS3	31
#define V_CAM_SIN3	34
#define V_CAM_KXMIN3	37
#define V_CAM_KXMAX3	40
#define V_CAM_KZMIN3	43
#define V_CAM_KZMAX3	46
#define V_CAM_ZLO	49
#define V_CAM_ZSPAN	52
#define V_CAM_CXL	55
#define V_CAM_CXR	58
#define V_CAM_CYL	61
#define V_CAM_CARDINAL	64
#define V_REC		65
#define V_OUT		68
#define V_LEFT		71
#define V_KEPT		72
#define V_FACES		73
#define V_CAM_KXMID3	74
#define V_CAM_KZMID3	77
#define V_CAM_LODRECT	80
#define V_CAM_LODPIX	83
#define V_ROOM		86
#define V_SIZE		87

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
	ld	hl, (VAR(\lv))
	ld	a, (VAR(\sv))
	PROJECT	\centre
	ld	(OUT(\slot)), hl
	.endm

; axis-aligned face at the depth in bc: left right top bottom into slots s0..s3
	.macro	FACE s0, s1, s2, s3
	PROJECT_TO V_LX0, V_SX0, CENTRE_X, \s0
	PROJECT_TO V_LX1, V_SX1, CENTRE_X, \s1
	PROJECT_TO V_LT, V_ST, CENTRE_Y, \s2
	PROJECT_TO V_LB, V_SB, CENTRE_Y, \s3
	.endm

; vertical edge at camera (.Lcx, .Lcz): x, top y, bottom y into slots sx, st, sb
	.macro	CORNER sx, st, sb
	ld	hl, (VAR(V_CZ))
	LOG_Z
	ld	hl, (VAR(V_CX))
	LOG_ABS
	PROJECT	CENTRE_X
	ld	(OUT(\sx)), hl
	PROJECT_TO V_LT, V_ST, CENTRE_Y, \st
	PROJECT_TO V_LB, V_SB, CENTRE_Y, \sb
	.endm

; grid line (record byte at iy + off) - 1 -> table index 3 n in hl, n checked against
; [0, 2 * GRID_RANGE) so n + 1 is in the table too. n = X + bias in 16 bits: X is a byte and the
; camera cell and the chunk origin (so the bias) are int16, so n is far from wrapping 16 bits and
; its low 16 bits are in range exactly when n is. The bias is patched into the ld de, nn at label
; smc. clobbers a, de
	.macro	GRID_INDEX off, smc
	ld	a, (iy + \off)
	ld	l, a
	rla
	sbc	a, a
	ld	h, a			; X, sign-extended to 16 bits
\smc:	ld	de, 0			; + the bias (patched)
	add	hl, de
	ld	a, h
	or	a, a
	jp	nz, .Lskip
	ld	a, l
	cp	a, 2 * GRID_RANGE
	jp	nc, .Lskip
	ld	h, 3
	mlt	hl			; 3 n (mlt clears the upper byte)
	.endm


	.section	.text._projectRecords,"ax",@progbits
	.global	_projectRecords
	.type	_projectRecords,@function
_projectRecords:
	push	ix
	ld	iy, 0
	add	iy, sp			; iy + 6 rec, 9 n, 12 out, 15 room, 18 bias_x, 21 bias_z
	ld	ix, .Lvars
	ld	hl, (iy + 6)
	ld	(VAR(V_REC)), hl
	ld	hl, (iy + 12)
	ld	(VAR(V_OUT)), hl
	xor	a, a
	ld	(VAR(V_KEPT)), a
	ld	a, (iy + 9)
	or	a, a
	jp	z, .Lend
	ld	(VAR(V_LEFT)), a
	ld	a, (iy + 15)
	or	a, a
	jp	z, .Lend
	ld	(VAR(V_ROOM)), a
	; the biases into the face test and the level of detail side (self-modifying: the program
	; runs from RAM)
	ld	a, (iy + 18)
	ld	(.Lfbx + 1), a
	ld	(.Llbx + 1), a
	ld	a, (_cam_bias_y)
	ld	(.Lfby + 1), a
	ld	a, (iy + 21)
	ld	(.Lfbz + 1), a
	ld	(.Llbz + 1), a
	ld	hl, (iy + 18)		; and into the grid index (16 bits matter)
	ld	(.Lgbx + 1), hl
	ld	hl, (_cam_bias_y)
	ld	(.Lgby + 1), hl
	ld	hl, (iy + 21)
	ld	(.Lgbz + 1), hl

.Lrec:
	ld	iy, (VAR(V_REC))
	ld	a, (iy + 4)
	or	a, a
	jp	z, .Lnext		; enclosed by neighbours: no face can show

	; ---- the rotated x and z grid lines of the box ----
	GRID_INDEX 2, .Lgbz
	add	hl, hl			; 6 n: {ZS, ZC} pairs
	ld	de, _rotZ_
	add	hl, de
	ld	(VAR(V_CX)), hl		; (scratch until the general path)
	GRID_INDEX 0, .Lgbx
	add	hl, hl			; {XC, XS} pairs
	ld	de, _rotX_
	add	hl, de

	; ---- cull in camera space (engine.h lists the tests), cheapest rejections first ----
	; depth of corner (X-1, Z-1): z00 = XS + ZC
	ld	iy, (VAR(V_CX))
	ld	de, (iy + 3)		; ZC
	ld	bc, (iy + 0)		; ZS
	push	hl
	pop	iy			; {XC, XS}
	ld	hl, (iy + 3)		; XS
	add	hl, de
	ld	(VAR(V_Z00)), hl
	ld	de, (VAR(V_CAM_ZLO))
	or	a, a
	sbc	hl, de
	ld	de, (VAR(V_CAM_ZSPAN))
	or	a, a
	sbc	hl, de			; unsigned: carry if every corner is in depth range
	jp	nc, .Lskip
	ld	hl, (iy + 0)		; XC
	or	a, a
	sbc	hl, bc
	push	hl
	pop	bc			; x00 = XC - ZS
	ld	iy, (VAR(V_Z00))

	; the view is |x| <= 1.25 z (CULL_X_NUM / CULL_X_DEN), so compare 4 x with 5 z
	ld	(VAR(V_X00)), bc
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

	ld	hl, (VAR(V_CAM_CXR))
	add	hl, de
	or	a, a
	sbc	hl, bc			; cxr + 5 z00 - 4 x00
	jp	m, .Lskip

	push	bc
	pop	hl
	add	hl, de
	ld	de, (VAR(V_CAM_CXL))
	or	a, a
	sbc	hl, de			; 4 x00 + 5 z00 - cxl
	jp	m, .Lskip

	; camera y of the box top
	ld	iy, (VAR(V_REC))
	GRID_INDEX 1, .Lgby
	ld	de, _camY_
	add	hl, de
	ld	hl, (hl)
	ld	(VAR(V_YT)), hl
	ex	de, hl			; de = yt
	ld	bc, (VAR(V_Z00))
	ld	hl, (VAR(V_CAM_KZMAX3))
	add	hl, bc
	or	a, a
	sbc	hl, de			; kzmax + z00 - yt
	jp	m, .Lskip
	ex	de, hl
	add	hl, bc
	ld	de, (VAR(V_CAM_CYL))
	or	a, a
	sbc	hl, de			; yt + z00 - cyl
	jp	m, .Lskip

	; ---- the faces towards the camera, by the low byte of the grid index (see main.c), that
	; are also exposed. None: nothing of this box can show ----
	ld	hl, _face_x
	ld	a, (iy + 0)
.Lfbx:	add	a, 0			; + bias_x (patched)
	ld	l, a
	ld	c, (hl)
	inc	h			; face_y (the tables are 1024-aligned: no wrap)
	ld	a, (iy + 1)
.Lfby:	add	a, 0
	ld	l, a
	ld	a, (hl)
	or	a, c
	ld	c, a
	inc	h			; face_z
	ld	a, (iy + 2)
.Lfbz:	add	a, 0
	ld	l, a
	ld	a, (hl)
	or	a, c
	and	a, (iy + 4)
	jp	z, .Lnext
	ld	(VAR(V_FACES)), a

	; the box is kept: iy = its slot in out[]
	ld	iy, (VAR(V_OUT))

	; ---- level of detail, by the depth of the nearest corner (z00 + kzmin3) ----
	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_LODRECT))
	or	a, a
	sbc	hl, de
	call	p, .Llod_side		; rectangle or pixel: keep only the side it shows most
	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_LODPIX))
	or	a, a
	sbc	hl, de
	jp	p, .Lpixel

	; ---- the y logs are shared by every corner ----
	ld	hl, (VAR(V_YT))
	LOG_ABS
	ld	(VAR(V_LT)), hl
	ld	(VAR(V_ST)), a
	ld	hl, (VAR(V_YT))
	ld	de, 3 * SUB
	add	hl, de			; box bottom
	LOG_ABS
	ld	(VAR(V_LB)), hl
	ld	(VAR(V_SB)), a

	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_LODRECT))
	or	a, a
	sbc	hl, de
	jp	p, .Lrect

	ld	a, (VAR(V_CAM_CARDINAL))
	or	a, a
	jp	z, .Lgeneral

	; ==== facing N/E/S/W: the box is axis aligned in camera space ====
	ld	hl, (VAR(V_X00))
	ld	de, (VAR(V_CAM_KXMIN3))
	add	hl, de
	LOG_ABS
	ld	(VAR(V_LX0)), hl
	ld	(VAR(V_SX0)), a
	ld	hl, (VAR(V_X00))
	ld	de, (VAR(V_CAM_KXMAX3))
	add	hl, de
	LOG_ABS
	ld	(VAR(V_LX1)), hl
	ld	(VAR(V_SX1)), a

	; Each far vertex lies between the screen centre and its near vertex, and the centre is
	; inside the viewport. So the far face alone decides whether anything is on screen, and the
	; near face alone whether everything is.
	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_KZMAX3))
	add	hl, de
	LOG_Z
	FACE	4, 5, 6, 7		; fl fr ft fb

	ld	hl, (OUT(4))
	ld	de, VIEW_X1
	or	a, a
	sbc	hl, de
	jp	p, .Lskip		; fl >= X1
	ld	hl, (OUT(5))
#if VIEW_X0 == 0
	add	hl, hl
	jp	c, .Lskip		; fr < X0
#else
	ld	de, VIEW_X0
	or	a, a
	sbc	hl, de
	jp	m, .Lskip		; fr < X0
#endif
	ld	hl, (OUT(6))
	ld	de, VIEW_H
	or	a, a
	sbc	hl, de
	jp	p, .Lskip		; ft >= H
	ld	hl, (OUT(7))
	add	hl, hl
	jp	c, .Lskip		; fb < 0

	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_KZMIN3))
	add	hl, de
	LOG_Z
	FACE	0, 1, 2, 3		; nl nr nt nb

	ld	a, BOX_AXIS
.Ldone:
	; kept: a = BOX_*, then its faces and material
	ld	(iy + PB_KIND), a
	ld	a, (VAR(V_FACES))
	ld	(iy + PB_FACES), a
	ld	hl, (VAR(V_REC))
	inc	hl
	inc	hl
	inc	hl
	ld	a, (hl)
	ld	(iy + PB_MAT), a
	lea	iy, iy + PB_SIZE
	ld	(VAR(V_OUT)), iy
	inc	(VAR(V_KEPT))
	dec	(VAR(V_ROOM))
	jp	z, .Lend		; out[] is full
.Lskip:
.Lnext:
	ld	hl, (VAR(V_REC))
	ld	de, 5
	add	hl, de
	ld	(VAR(V_REC)), hl
	dec	(VAR(V_LEFT))
	jp	nz, .Lrec
.Lend:
	ld	a, (VAR(V_KEPT))
	pop	ix
	ret

	; ==== level of detail: the rectangle of the x and y extent, at the centre depth ====
.Lrect:
	ld	hl, (VAR(V_X00))
	ld	de, (VAR(V_CAM_KXMIN3))
	add	hl, de
	LOG_ABS
	ld	(VAR(V_LX0)), hl
	ld	(VAR(V_SX0)), a
	ld	hl, (VAR(V_X00))
	ld	de, (VAR(V_CAM_KXMAX3))
	add	hl, de
	LOG_ABS
	ld	(VAR(V_LX1)), hl
	ld	(VAR(V_SX1)), a
	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_KZMID3))
	add	hl, de
	LOG_Z
	FACE	0, 1, 2, 3		; l r t b
	; at least a pixel each way (rounding never gives r < l or b < t)
	ld	hl, (OUT(0))
	ld	de, (OUT(1))
	or	a, a
	sbc	hl, de
	jr	nz, 1f
	inc	de
	ld	(OUT(1)), de
1:	ld	hl, (OUT(2))
	ld	de, (OUT(3))
	or	a, a
	sbc	hl, de
	jr	nz, 2f
	inc	de
	ld	(OUT(3)), de
2:	ld	a, BOX_RECT
	jp	.Ldone

	; ==== level of detail: one pixel at the projected centre ====
.Lpixel:
	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_KZMID3))
	add	hl, de
	LOG_Z
	ld	hl, (VAR(V_X00))
	ld	de, (VAR(V_CAM_KXMID3))
	add	hl, de
	LOG_ABS
	PROJECT	CENTRE_X
	dec	hl			; 2 x 2 logical pixels around the centre
	ld	(OUT(0)), hl
	inc	hl
	inc	hl
	ld	(OUT(1)), hl
	ld	hl, (VAR(V_YT))
	ld	de, 3 * SUB / 2
	add	hl, de
	LOG_ABS
	PROJECT	CENTRE_Y
	dec	hl
	ld	(OUT(2)), hl
	inc	hl
	inc	hl
	ld	(OUT(3)), hl
	ld	a, BOX_RECT
	jp	.Ldone

	; ==== level of detail colour: of the visible x and z sides, keep only the one that points
	; at the camera position most (the axis with the larger distance in cells, z on a tie), so
	; the colour depends on where the camera is, not where it looks. lod_slot (main.c) then
	; picks that side. gridDist is exact for a kept box (its grid index is in 0 .. 2 GRID_RANGE)
.Llod_side:
	ld	hl, (VAR(V_REC))
	ld	de, _gridDist		; 256-aligned: e is the index
	ld	a, (hl)
.Llbx:	add	a, 0			; + bias_x (patched)
	ld	e, a
	ld	a, (de)
	ld	c, a			; |dx|
	inc	hl
	inc	hl
	ld	a, (hl)
.Llbz:	add	a, 0			; + bias_z (patched)
	ld	e, a
	ld	a, (de)			; |dz|
	cp	a, c
	ld	a, (VAR(V_FACES))
	ld	c, a
	jr	c, 1f
	and	a, FACE_ZP | FACE_ZN	; z dominates: drop the x sides if a z side shows
	ret	z
	ld	a, c
	and	a, ~(FACE_XP | FACE_XN) & 0xff
	ld	(VAR(V_FACES)), a
	ret
1:	and	a, FACE_XP | FACE_XN	; x dominates: drop the z sides if an x side shows
	ret	z
	ld	a, c
	and	a, ~(FACE_ZP | FACE_ZN) & 0xff
	ld	(VAR(V_FACES)), a
	ret

	; ==== any other yaw: project the 4 vertical edges ====
.Lgeneral:
	ld	hl, (VAR(V_X00))
	ld	(VAR(V_CX)), hl
	ld	hl, (VAR(V_Z00))
	ld	(VAR(V_CZ)), hl
	CORNER	0, 4, 8

	ld	hl, (VAR(V_X00))		; corner 1: + (c, s)
	ld	de, (VAR(V_CAM_COS3))
	add	hl, de
	ld	(VAR(V_CX)), hl
	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_SIN3))
	add	hl, de
	ld	(VAR(V_CZ)), hl
	CORNER	1, 5, 9

	ld	hl, (VAR(V_X00))		; corner 2: + (-s, c)
	ld	de, (VAR(V_CAM_SIN3))
	or	a, a
	sbc	hl, de
	ld	(VAR(V_CX)), hl
	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_COS3))
	add	hl, de
	ld	(VAR(V_CZ)), hl
	CORNER	2, 6, 10

	ld	hl, (VAR(V_CX))		; corner 3: corner 2 + (c, s)
	ld	de, (VAR(V_CAM_COS3))
	add	hl, de
	ld	(VAR(V_CX)), hl
	ld	hl, (VAR(V_CZ))
	ld	de, (VAR(V_CAM_SIN3))
	add	hl, de
	ld	(VAR(V_CZ)), hl
	CORNER	3, 7, 11

	; Culling already removed nearly everything off screen, so only test for fully inside.
	; Tops are above bottoms, so (uint)top < H and (uint)bottom < H cover all four y bounds.
	ld	a, BOX_GEN
	jp	.Ldone


	.section	.bss._projectRecords,"aw",@nobits
; Everything here is addressed through ix (VAR), so the block must stay under 128 bytes.
; The _cam_ values are written by setupCamera() in main.c (declared in engine.h).
.Lvars:
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
	.global	_cam_cos3, _cam_sin3
	.global	_cam_kxmin3, _cam_kxmax3, _cam_kzmin3, _cam_kzmax3
	.global	_cam_zlo, _cam_zspan, _cam_cxl, _cam_cxr, _cam_cyl, _cam_cardinal
_cam_cos3:	.zero	3
_cam_sin3:	.zero	3
_cam_kxmin3:	.zero	3
_cam_kxmax3:	.zero	3
_cam_kzmin3:	.zero	3
_cam_kzmax3:	.zero	3
_cam_zlo:	.zero	3
_cam_zspan:	.zero	3
_cam_cxl:	.zero	3
_cam_cxr:	.zero	3
_cam_cyl:	.zero	3
_cam_cardinal:	.zero	1
.Lrecp:		.zero	3			; the next record
.Loutp:		.zero	3			; the next slot in out[]
.Lleft:		.zero	1			; records left
.Lkept:		.zero	1
.Lfaces:	.zero	1			; FACE_* of the current box
	.global	_cam_kxmid3, _cam_kzmid3, _cam_lod_rect, _cam_lod_pix
_cam_kxmid3:	.zero	3
_cam_kzmid3:	.zero	3
_cam_lod_rect:	.zero	3
_cam_lod_pix:	.zero	3
.Lroom:		.zero	1			; boxes out[] still has room for
.Lvars_end:
	.if	.Lvars_end - .Lvars != V_SIZE || V_SIZE > 128
	.error	".Lvars does not match V_SIZE, or is too large for (ix + d)"
	.endif
	.if	.Lyt - .Lvars != V_YT
	.error	"V_YT does not match .Lvars"
	.endif
	.if	.Lx00 - .Lvars != V_X00
	.error	"V_X00 does not match .Lvars"
	.endif
	.if	.Lz00 - .Lvars != V_Z00
	.error	"V_Z00 does not match .Lvars"
	.endif
	.if	.Lcx - .Lvars != V_CX
	.error	"V_CX does not match .Lvars"
	.endif
	.if	.Lcz - .Lvars != V_CZ
	.error	"V_CZ does not match .Lvars"
	.endif
	.if	.Llt - .Lvars != V_LT
	.error	"V_LT does not match .Lvars"
	.endif
	.if	.Llb - .Lvars != V_LB
	.error	"V_LB does not match .Lvars"
	.endif
	.if	.Llx0 - .Lvars != V_LX0
	.error	"V_LX0 does not match .Lvars"
	.endif
	.if	.Llx1 - .Lvars != V_LX1
	.error	"V_LX1 does not match .Lvars"
	.endif
	.if	.Lst - .Lvars != V_ST
	.error	"V_ST does not match .Lvars"
	.endif
	.if	.Lsb - .Lvars != V_SB
	.error	"V_SB does not match .Lvars"
	.endif
	.if	.Lsx0 - .Lvars != V_SX0
	.error	"V_SX0 does not match .Lvars"
	.endif
	.if	.Lsx1 - .Lvars != V_SX1
	.error	"V_SX1 does not match .Lvars"
	.endif
	.if	_cam_cos3 - .Lvars != V_CAM_COS3
	.error	"V_CAM_COS3 does not match .Lvars"
	.endif
	.if	_cam_sin3 - .Lvars != V_CAM_SIN3
	.error	"V_CAM_SIN3 does not match .Lvars"
	.endif
	.if	_cam_kxmin3 - .Lvars != V_CAM_KXMIN3
	.error	"V_CAM_KXMIN3 does not match .Lvars"
	.endif
	.if	_cam_kxmax3 - .Lvars != V_CAM_KXMAX3
	.error	"V_CAM_KXMAX3 does not match .Lvars"
	.endif
	.if	_cam_kzmin3 - .Lvars != V_CAM_KZMIN3
	.error	"V_CAM_KZMIN3 does not match .Lvars"
	.endif
	.if	_cam_kzmax3 - .Lvars != V_CAM_KZMAX3
	.error	"V_CAM_KZMAX3 does not match .Lvars"
	.endif
	.if	_cam_zlo - .Lvars != V_CAM_ZLO
	.error	"V_CAM_ZLO does not match .Lvars"
	.endif
	.if	_cam_zspan - .Lvars != V_CAM_ZSPAN
	.error	"V_CAM_ZSPAN does not match .Lvars"
	.endif
	.if	_cam_cxl - .Lvars != V_CAM_CXL
	.error	"V_CAM_CXL does not match .Lvars"
	.endif
	.if	_cam_cxr - .Lvars != V_CAM_CXR
	.error	"V_CAM_CXR does not match .Lvars"
	.endif
	.if	_cam_cyl - .Lvars != V_CAM_CYL
	.error	"V_CAM_CYL does not match .Lvars"
	.endif
	.if	_cam_cardinal - .Lvars != V_CAM_CARDINAL
	.error	"V_CAM_CARDINAL does not match .Lvars"
	.endif
	.if	.Lrecp - .Lvars != V_REC || .Loutp - .Lvars != V_OUT || .Lleft - .Lvars != V_LEFT
	.error	"V_REC, V_OUT or V_LEFT does not match .Lvars"
	.endif
	.if	.Lkept - .Lvars != V_KEPT || .Lfaces - .Lvars != V_FACES
	.error	"V_KEPT or V_FACES does not match .Lvars"
	.endif
	.if	_cam_kxmid3 - .Lvars != V_CAM_KXMID3 || _cam_kzmid3 - .Lvars != V_CAM_KZMID3
	.error	"V_CAM_KXMID3 or V_CAM_KZMID3 does not match .Lvars"
	.endif
	.if	_cam_lod_rect - .Lvars != V_CAM_LODRECT || _cam_lod_pix - .Lvars != V_CAM_LODPIX
	.error	"V_CAM_LODRECT or V_CAM_LODPIX does not match .Lvars"
	.endif
	.if	.Lroom - .Lvars != V_ROOM
	.error	"V_ROOM does not match .Lvars"
	.endif

; Faces of a box towards the camera by the grid index of each axis (main.c, initFaceTables):
; three consecutive pages, so inc h steps from one to the next
	.balign	1024
	.global	_face_x, _face_y, _face_z
_face_x:	.zero	256
_face_y:	.zero	256
_face_z:	.zero	256

	.extern	_cam_bias_y
	.extern	_rotX_
	.extern	_rotZ_
	.extern	_camY_
	.extern	_logTable
	.extern	_expTable
