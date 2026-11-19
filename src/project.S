; projectBox: cull and project one voxel.
;
;   uint8_t projectBox(int24_t X, int24_t Y, int24_t Z, int24_t *out)
;
; Culls the world voxel [X-1, X] x [Y-1, Y] x [Z-1, Z] in camera space, projects its corners into
; out[12] and classifies it against the viewport. Returns BOX_* (engine.h) in A. out is written
; only partly (or not at all) for BOX_SKIP. Reads the per-frame camera state that setupCamera()
; in main.c writes (layout in engine.h).
;
; Why assembly: clang turns every multiply, right shift and even AND into a library call. And the
; CE is memory bound: roughly 2 cycles per byte touched, opcode bytes included. So this code
; avoids calls and 3-byte absolute addresses where it can, and keeps every camera value x3 so it
; is directly a byte offset into an int24 table (no index multiplies).
;
; Registers: after the table lookups IX points at .Lvars, the block below that holds the locals
; and the per-frame camera scalars (VAR(V_x) addresses them, 3 bytes per access against 4 or 5 for
; an absolute address). Once a box survives culling, IY points at out.
;
; Projection is lookups only: FOCAL_LENGTH * a / z = expTable[logTable[|a|] - logTable[z] - EXP_TABLE_MIN].
; logTable entries are x3, so with bc = logTable[z] - EXP_BASE, the entry for |a| minus bc is the
; address of the expTable entry. gen_tables.py proves every index stays inside the tables for boxes
; that pass the culling below: change them together.
;
; Signed compares use the sign of the sbc/adc result: every value here is far below 2^22, so the
; subtraction cannot overflow.
;
; ABI: arguments at sp+3, sp+6, sp+9, sp+12; result in A; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

#include "engine.h"

#if TABLE_CULL_X_NUM != 5 || TABLE_CULL_X_DEN != 4
#error "the horizontal culling below compares 4 x with 5 z"
#endif

#define EXP_BASE	_expTable - 3 * EXP_TABLE_MIN
#define OUT(k)		iy + 3 * k
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
#define V_SIZE		65

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

; leave with a (BOX_*_CLIP) unless slot k is a viewport column / row
	.macro	INSIDE_X k
	ld	hl, (OUT(\k))
#if VIEW_X0 != 0
	ld	de, VIEW_X0
	or	a, a
	sbc	hl, de
#endif
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
	add	iy, sp			; iy + 3 = X, iy + 6 = Y, iy + 9 = Z, iy + 12 = out
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
	pop	iy			; the arguments are no longer needed (out is read from the stack later)

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
	ld	ix, .Lvars		; from here on ix = locals

	; ---- cull in camera space (engine.h lists the tests) ----
	ld	de, (VAR(V_CAM_ZLO))
	or	a, a
	sbc	hl, de
	ld	de, (VAR(V_CAM_ZSPAN))
	or	a, a
	sbc	hl, de			; unsigned: carry if every corner is in depth range
	jp	nc, .Lskip

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

	ld	hl, (VAR(V_CAM_KZMAX3))
	lea	de, iy + 0
	add	hl, de
	ld	de, (VAR(V_YT))
	or	a, a
	sbc	hl, de			; kzmax + z00 - yt
	jp	m, .Lskip

	lea	hl, iy + 0
	ld	de, (VAR(V_YT))
	add	hl, de
	ld	de, (VAR(V_CAM_CYL))
	or	a, a
	sbc	hl, de			; yt + z00 - cyl
	jp	m, .Lskip

	; the box is kept: iy = out (sp + 0 is the saved ix, so the arguments start at sp + 6)
	ld	(VAR(V_Z00)), iy
	ld	iy, 0
	add	iy, sp
	ld	iy, (iy + 15)

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

	.extern	_cam_bias_x
	.extern	_cam_bias_y
	.extern	_cam_bias_z
	.extern	_rotX_
	.extern	_rotZ_
	.extern	_camY_
	.extern	_logTable
	.extern	_expTable
