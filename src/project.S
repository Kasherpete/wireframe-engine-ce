; projectRecords: cull and project the scene.
;
;   uint8_t projectRecords(const int8_t (*rec)[5], uint8_t n, projected_box_t *out, uint8_t room,
;                          int24_t bias_x, int24_t bias_z, uint8_t runs)
;
; For each record {X, Y, Z, material, exposed faces} of one chunk, in order: the faces facing the
; camera (face_x/y/z, by grid index) that are also exposed; none, and the box is skipped. Otherwise
; the voxel is culled in camera space, its corners projected and the result classified (BOX_*,
; engine.h). Kept boxes go to out[] in record order, with their faces and material, until room of
; them are kept. Returns how many. Reads the per-frame camera state that setupCamera() in camera.c
; writes (layout in engine.h).
;
; X and Z are chunk local: the grid index of a record is X + bias_x (Z + bias_z, and Y + cam_bias_y),
; where projectScene (scene.c) passes bias_x = cam_bias_x + the x of the first cell of the
; chunk + 1. The voxel spans the cell whose grid index that is (engine.h).
;
; Level of detail: a box whose nearest corner is at least cam_lod_rect deep becomes BOX_RECT, the
; rectangle of its x and y extent projected at its centre depth (4 projections instead of 12). It
; keeps only the visible side (x or z) that points at the camera position most (.Llod_side).
;
; Fog: a box whose centre is at least cam_fog deep gets material + MAT_FOGGED (the fogged colours,
; colours.c). With fog_dist at or beyond the far plane no box reaches cam_fog.
;
; Runs (chunks.h): with runs (the faces this chunk's runs draw, or 0) a voxel with FACE_RUN leaves
; those faces out, and a run record (.Lrun) becomes BOX_RUN: the points between its faces. Only its
; cells that pass the culling of a box are kept (they are contiguous: every test is linear along the
; run), so every point is a corner of a kept box, as the table proof (gen_tables.py) requires. A
; point behind the near plane is moved along the run to it (exact). Runs stop at the level of detail:
; beyond it a voxel is one rectangle anyway, with all its faces, which is cheaper than a run's points
; and edges. Fog is decided per cell as for a box, so a run crossing fog_dist (with the level of
; detail at or beyond it) is split there into two.
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
; Near plane: a box is kept when its FARTHEST corner is in [NEAR_PLANE, far plane), so a wall the
; camera is pressed against (the body is narrower than the near plane is deep) does not vanish.
; Its part nearer than NEAR_PLANE is clipped: facing N/E/S/W the near face is moved to NEAR_PLANE
; (LOG_ZC), which is exact. At other yaws (.Lgen_clip) each corner behind the near plane is moved
; along an edge of the box to a corner in front, to depth NEAR_PLANE: preferring the edge of a
; visible side, so the sides are cut exactly where the near plane cuts them. That is exact for the
; sides unless a corner between two visible sides is behind (then one of the two loses a sliver at
; the screen edge); a top or bottom face may lose a corner triangle. The proof still holds: every
; point projected lies in the box footprint at a depth >= NEAR_PLANE (gen_tables.py).
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
#if FACE_RUN != 0x40 || FACE_RUN_REC != 0x80 || RUN_POINTS != 9
#error "runs: bit tests and the point buffers"
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
#define V_CAM_FOG	83
#define V_ROOM		86
#define V_EXPO		87
#define V_SIZE		88

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

; in: hl = depth z (x3), anywhere from behind the camera up to the far plane. A corner nearer than
; NEAR_PLANE is projected at NEAR_PLANE instead (its x and y kept), so a box straddling the near
; plane stretches over the screen instead of vanishing. out: bc = logTable[z] - EXP_BASE.
; clobbers de, hl
	.macro	LOG_ZC
	ld	de, 3 * NEAR_PLANE
	or	a, a
	sbc	hl, de
	jp	p, 1f
	ld	hl, 0			; z < near: z - near is replaced by 0
1:	add	hl, de			; max(z, near)
	LOG_Z
	.endm

; project a stored log/sign pair and store it
	.macro	PROJECT_TO lv, sv, centre, slot
	ld	hl, (VAR(\lv))
	ld	a, (VAR(\sv))
	PROJECT	\centre
	ld	(OUT(\slot)), hl
	.endm

	; ---- the y logs are shared by every corner ----
	.macro	YLOGS
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
	LOG_ZC
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
	ld	(VAR(V_LEFT)), a
	or	a, a
	jp	z, .Lend
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
	; runs: on for the faces in runs (a run with another face is skipped), and FACE_RUN voxels drop
	; them. And the biases for the run path
	ld	a, (iy + 24)
	ld	(.Lrun_m + 1), a
	or	a, a
	jr	z, .Lruns_off
	ld	a, 0xfa			; the checks as assembled: jp m, .Lrun
	ld	(.Lra), a
	ld	hl, .Lrun
	ld	(.Lra + 1), hl
	ld	a, 0xdd			; bit 6, (ix + V_EXPO)
	ld	(.Lmb), a
	ld	a, 0xcb
	ld	(.Lmb + 1), a
	ld	a, V_EXPO
	ld	(.Lmb + 2), a
	ld	a, V_EXPO		; and a, (ix + V_EXPO)
	ld	(.Lft + 2), a
	ld	a, 0xdd
	jr	.Lruns_on
.Lruns_off:				; jr over them, and the face test on the record
	ld	hl, .Lra_end
	ld	de, .Lra + 2
	or	a, a
	sbc	hl, de
	ld	h, l
	ld	l, 0x18
	ld	(.Lra), hl		; (the byte after it is never executed)
	ld	hl, .Lmb_end
	ld	de, .Lmb + 2
	or	a, a
	sbc	hl, de
	ld	h, l
	ld	l, 0x18
	ld	(.Lmb), hl
	ld	a, 4			; and a, (iy + 4)
	ld	(.Lft + 2), a
	ld	a, 0xfd
.Lruns_on:
	ld	(.Lft), a
	ld	a, (iy + 24)
	cpl
	and	a, FACE_XP | FACE_XN | FACE_ZP | FACE_ZN | FACE_TOP | FACE_BOT
	ld	(.Lmm + 1), a
	ld	(.Lmm0 + 1), a
	ld	a, (iy + 18)
	ld	(.Lmbx + 1), a
	ld	a, (iy + 21)
	ld	(.Lmbz + 1), a
	ld	a, (_lod_rect_dist)
	sub	a, 2
	jr	nc, 2f
	xor	a, a
2:	ld	(.Lmlod + 1), a
	ld	hl, (iy + 18)
	ld	(.Lr_bx), hl
	ld	(.Lrgbx + 1), hl
	ld	hl, (iy + 21)
	ld	(.Lr_bz), hl
	ld	(.Lrgbz + 1), hl
	ld	hl, (_cam_bias_y)
	ld	(.Lrgby + 1), hl

.Lrec:
	ld	iy, (VAR(V_REC))
	ld	a, (iy + 4)
	or	a, a
	jp	z, .Lnext		; enclosed by neighbours: no face can show
	; with the runs off (none among the records), from here to .Lra_end is skipped: .Lra is
	; patched to a jr there
.Lra:	jp	m, .Lrun		; FACE_RUN_REC: a run
	ld	(VAR(V_EXPO)), a
	; FACE_RUN, and surely nearer than the level of detail: its nearest corner is at most
	; |dx| + |dz| + 2 units away (cells from the camera's, gridDist), and a depth is at most a
	; distance. Then the runs draw those faces (else decided by its depth, below)
	bit	6, a
	jr	z, .Lra_end
	ld	hl, _gridDist
	ld	a, (iy + 0)
.Lmbx:	add	a, 0			; + bias_x (patched)
	ld	l, a
	ld	c, (hl)
	ld	a, (iy + 2)
.Lmbz:	add	a, 0			; + bias_z (patched)
	ld	l, a
	ld	a, (hl)
	add	a, c
.Lmlod:	cp	a, 0			; patched: lod_rect_dist - 2 (or 0)
	jr	nc, .Lra_end
	ld	a, (VAR(V_EXPO))
.Lmm0:	and	a, 0			; patched: ~runs (clears FACE_RUN too)
	jp	z, .Lnext
	ld	(VAR(V_EXPO)), a
.Lra_end:

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
	; FACE_RUN and nearer than the level of detail: runs draw those faces (beyond it the box
	; becomes a rectangle with all of them, and runs stop there). With the runs off .Lmb is
	; patched to a jr to .Lmb_end
.Lmb:	bit	6, (VAR(V_EXPO))
	jr	z, .Lmb_end
	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_LODRECT))
	or	a, a
	sbc	hl, de
	jp	p, .Lmb_end		; a rectangle: all its faces
	ld	a, (VAR(V_EXPO))
.Lmm:	and	a, 0			; patched: ~runs (all faces when the runs are off)
	jp	z, .Lskip
	ld	(VAR(V_EXPO)), a
.Lmb_end:
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

	; ---- the faces towards the camera, by the low byte of the grid index (camera.c), that
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
.Lft:	and	a, (VAR(V_EXPO))	; exposed, less runs (bits 6, 7 never face). Runs off: (iy + 4)
	jp	z, .Lnext
	ld	(VAR(V_FACES)), a

	; the box is kept: iy = its slot in out[]
	ld	iy, (VAR(V_OUT))

	; ---- level of detail, by the depth of the nearest corner (z00 + kzmin3). A ground panel
	; (y cell 0: record Y = 1, a flat patch drawn over the ground plane) never gets it: as a
	; rectangle its whole y extent would show, so it would look thick ----
	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_LODRECT))
	or	a, a
	sbc	hl, de
	jp	m, .Lfull
	ld	hl, (VAR(V_REC))
	inc	hl
	ld	a, (hl)
	dec	a
	jp	z, .Lfull		; ground panel: a full box at any distance
	call	.Llod_side		; a rectangle: keep only the side it shows most
	jp	.Lrect

.Lfull:
	YLOGS

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
	LOG_ZC				; the near face may be nearer than the near plane
	FACE	0, 1, 2, 3		; nl nr nt nb

	ld	a, BOX_AXIS
.Ldone:
	; kept: a = BOX_*, then its faces and material
	ld	(iy + PB_KIND), a
	ld	a, (VAR(V_FACES))
	ld	(iy + PB_FACES), a
	ld	hl, (VAR(V_Z00))	; fog: z00 >= cam_fog
	ld	de, (VAR(V_CAM_FOG))
	or	a, a
	sbc	hl, de
	ld	hl, (VAR(V_REC))	; (ld and inc hl keep the flags)
	inc	hl
	inc	hl
	inc	hl
	ld	a, (hl)
	jp	m, 1f
	add	a, MAT_FOGGED
1:	ld	(iy + PB_MAT), a
	lea	iy, iy + PB_SIZE
	ld	(VAR(V_OUT)), iy
	inc	(VAR(V_KEPT))
	dec	(VAR(V_ROOM))
	jp	z, .Lout_kept		; out[] is full
.Lskip:
.Lnext:
	ld	hl, (VAR(V_REC))
	ld	de, 5
	add	hl, de
	ld	(VAR(V_REC)), hl
	dec	(VAR(V_LEFT))
	jp	nz, .Lrec
.Lend:
	ld	a, (VAR(V_LEFT))
	ld	(_project_left), a
	ld	a, (VAR(V_KEPT))
	pop	ix
	ret
.Lout_kept:				; full after keeping this record: it is done
	dec	(VAR(V_LEFT))
.Lout_full:				; (a run that did not fit is not: it is done again)
	ld	a, 1
	ld	(_project_full), a
	jr	.Lend

	; ==== level of detail: the rectangle of the x and y extent, at the centre depth. Each side
	; projected straight into out[] (no logs kept: each is used once). Neighbouring boxes at
	; the same depth project the same grid lines, so their rectangles meet exactly. A unit is at
	; least 2.5 pixels within the far plane, so r > l and b > t ====
.Lrect:
	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_KZMID3))
	add	hl, de
	LOG_Z				; bc
	ld	hl, (VAR(V_X00))
	ld	de, (VAR(V_CAM_KXMIN3))
	add	hl, de
	LOG_ABS
	PROJECT	CENTRE_X
	ld	(OUT(0)), hl		; l
	ld	hl, (VAR(V_X00))
	ld	de, (VAR(V_CAM_KXMAX3))
	add	hl, de
	LOG_ABS
	PROJECT	CENTRE_X
	ld	(OUT(1)), hl		; r
	ld	hl, (VAR(V_YT))
	LOG_ABS
	PROJECT	CENTRE_Y
	ld	(OUT(2)), hl		; t
	ld	hl, (VAR(V_YT))
	ld	de, 3 * SUB
	add	hl, de
	LOG_ABS
	PROJECT	CENTRE_Y
	ld	(OUT(3)), hl		; b
	ld	a, BOX_RECT
	jp	.Ldone

	; ==== level of detail colour: of the visible x and z sides, keep only the one that points
	; at the camera position most (the axis with the larger distance in cells, z on a tie), so
	; the colour depends on where the camera is, not where it looks. lod_slot (scene.c) then
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
	ld	hl, (VAR(V_Z00))	; nearest corner behind the near plane: clip
	ld	de, (VAR(V_CAM_KZMIN3))
	add	hl, de
	ld	de, 3 * NEAR_PLANE
	or	a, a
	sbc	hl, de
	jp	m, .Lgen_clip
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

	ld	a, BOX_GEN
	jp	.Ldone

	; ==== any other yaw, with corners behind the near plane (see Near plane above) ====
	; The 4 corners {x, z} into .Lcorn, each moved to the near plane if behind it into .Lclp
	; (24 bytes on: (iy + 24)), then projected like the general path. Rare: a few boxes a frame
	; at most, beside or in front of a camera close to them
.Lgen_clip:
	ld	hl, (VAR(V_X00))
	ld	(.Lcorn + 0), hl	; corner 0
	ld	de, (VAR(V_CAM_COS3))
	add	hl, de
	ld	(.Lcorn + 6), hl	; corner 1: + (c, s)
	ld	hl, (VAR(V_Z00))
	ld	(.Lcorn + 3), hl
	ld	de, (VAR(V_CAM_SIN3))
	add	hl, de
	ld	(.Lcorn + 9), hl
	ld	hl, (VAR(V_X00))	; corner 2: + (-s, c)
	ld	de, (VAR(V_CAM_SIN3))
	or	a, a
	sbc	hl, de
	ld	(.Lcorn + 12), hl
	ld	de, (VAR(V_CAM_COS3))	; corner 3: corner 2 + (c, s)
	add	hl, de
	ld	(.Lcorn + 18), hl
	ld	hl, (VAR(V_Z00))
	ld	de, (VAR(V_CAM_COS3))
	add	hl, de
	ld	(.Lcorn + 15), hl
	ld	de, (VAR(V_CAM_SIN3))
	add	hl, de
	ld	(.Lcorn + 21), hl

	; corner k: its neighbour along its z side is k ^ 1, along its x side k ^ 2 (engine.h)
	.macro	CLIP_CORNER k, zface, xface
	ld	iy, .Lcorn + 6 * \k
	ld	hl, .Lcorn + 6 * (\k ^ 1)
	ld	de, .Lcorn + 6 * (\k ^ 2)
	ld	c, \zface
	ld	b, \xface
	call	.Lclip_one
	.endm
	CLIP_CORNER 0, FACE_ZN, FACE_XN
	CLIP_CORNER 1, FACE_ZN, FACE_XP
	CLIP_CORNER 2, FACE_ZP, FACE_XN
	CLIP_CORNER 3, FACE_ZP, FACE_XP

	ld	iy, (VAR(V_OUT))
	ld	hl, .Lclp
	ld	(.Lclip_p), hl
	ld	a, 4
	ld	(.Lclip_n), a
.Lclip_proj:				; corner k into OUT(k), OUT(4 + k), OUT(8 + k): iy moves 3 a corner
	ld	hl, (.Lclip_p)
	ld	de, (hl)
	ld	(VAR(V_CX)), de
	inc	hl
	inc	hl
	inc	hl
	ld	de, (hl)
	ld	(VAR(V_CZ)), de
	inc	hl
	inc	hl
	inc	hl
	ld	(.Lclip_p), hl
	CORNER	0, 4, 8
	lea	iy, iy + 3
	ld	hl, .Lclip_n
	dec	(hl)
	jp	nz, .Lclip_proj
	lea	iy, iy - 12
	ld	a, BOX_GEN
	jp	.Ldone

	; ==== a run of merged faces (chunks.h): iy -> {X, Y, Z, material + 16 (length - 1),
	; FACE_RUN_REC + face}, a = its exposed byte. Rare next to boxes (a few dozen a frame), so
	; plain code with absolute addresses
.Lrun:
.Lrun_m:
	and	a, 0			; patched: runs (0: off)
	jp	z, .Lnext
	ld	(VAR(V_FACES)), a
	; facing the camera: face_x by the low byte of X + bias_x for x faces, face_z for z faces
	ld	c, a
	and	a, FACE_XP | FACE_XN
	jr	z, 1f
	ld	a, (.Lr_bx)
	add	a, (iy + 0)
	ld	hl, _face_x
	jr	2f
1:	ld	a, (.Lr_bz)
	add	a, (iy + 2)
	ld	hl, _face_z
2:	ld	l, a
	ld	a, c
	and	a, (hl)
	jp	z, .Lnext		; facing away
	ld	a, (iy + 3)
	and	a, 15
	ld	(.Lr_mat), a
	ld	a, (iy + 3)
	rrca
	rrca
	rrca
	rrca
	and	a, 7
	inc	a
	ld	(.Lr_len), a		; cells, 1 .. RUN_MAX (worldOpen checked)
	GRID_INDEX 1, .Lrgby		; the y of its cells
	ld	de, _camY_
	add	hl, de
	ld	hl, (hl)
	ld	(VAR(V_YT)), hl
	ld	a, (VAR(V_FACES))
	and	a, FACE_XP | FACE_XN
	jp	z, .Lrun_zf
	; x faces of the cells (X, Y, Z + i): the x line is fixed, the z line steps. Along the run a
	; corner moves by (ax, az) = (-sin, cos) in camera space, the face line is X - 1 (-x) or X (+x)
	GRID_INDEX 0, .Lrgbx
	add	hl, hl
	ld	de, _rotX_
	add	hl, de
	ld	(.Lr_xp), hl		; {XC, XS} of the low x line
	ld	hl, (VAR(V_CAM_SIN3))
	ex	de, hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de
	ld	(.Lr_ax), hl
	ld	hl, (VAR(V_CAM_COS3))
	ld	(.Lr_az), hl
	ld	hl, (VAR(V_CAM_SIN3))		; +x face: corner 1, + (cos, sin)
	ld	(.Lr_fz2), hl
	ld	a, (VAR(V_FACES))
	cp	a, FACE_XP
	ld	hl, (VAR(V_CAM_COS3))
	jr	z, 1f
	or	a, a
	sbc	hl, hl
	ld	(.Lr_fz2), hl		; -x face: the low corner
1:	ld	(.Lr_fx), hl
	ld	hl, (.Lr_bz)
	ld	de, 0
	ld	e, (iy + 2)
	add	hl, de
	ld	(.Lr_n), hl		; grid index of cell 0 along the run (maybe outside the tables)
	ld	hl, _rotZ_
	ld	(.Lr_zp), hl		; stepping: rotZ_ + 6 n
	jp	.Lrun_cells
.Lrun_zf:
	; z faces of the cells (X + i, Y, Z): (ax, az) = (cos, sin), the face line Z - 1 or Z
	GRID_INDEX 2, .Lrgbz
	add	hl, hl
	ld	de, _rotZ_
	add	hl, de
	ld	(.Lr_xp), hl		; {ZS, ZC} of the low z line (the fixed one)
	ld	hl, (VAR(V_CAM_COS3))
	ld	(.Lr_ax), hl
	ld	(.Lr_fz2), hl		; +z face: corner 2, + (-sin, cos)
	ld	hl, (VAR(V_CAM_SIN3))
	ld	(.Lr_az), hl
	ex	de, hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de
	ld	a, (VAR(V_FACES))
	cp	a, FACE_ZP
	jr	z, 1f
	or	a, a
	sbc	hl, hl
	ld	(.Lr_fz2), hl		; -z face: the low corner
1:	ld	(.Lr_fx), hl
	ld	hl, (.Lr_bx)
	ld	de, 0
	ld	e, (iy + 0)
	add	hl, de
	ld	(.Lr_n), hl
	ld	hl, _rotX_
	ld	(.Lr_zp), hl

	; ---- the cells lo .. hi to draw: inside the tables, then (the tests are linear along the run,
	; so each keeps an interval of cells) passing the culling of a box and nearer than the level of
	; detail. The camera space of cell lo comes from the tables, the rest by adding (ax, az) ----
.Lrun_cells:
	xor	a, a
	ld	(.Lr_lo), a
	ld	a, (.Lr_len)
	dec	a
	ld	(.Lr_hi), a
	ld	de, (.Lr_n)
	ld	hl, 2 * GRID_RANGE - 1
	or	a, a
	sbc	hl, de			; the last cell in the tables, from cell 0
	jp	m, .Lnext
	ld	bc, 0
	ld	c, a
	or	a, a
	sbc	hl, bc
	jr	nc, 1f
	add	hl, bc
	ld	a, l
	ld	(.Lr_hi), a
1:	ld	hl, (.Lr_n)
	ld	bc, 0
	or	a, a
	sbc	hl, bc
	jp	p, 2f
	ex	de, hl			; cell 0 is before the tables: lo = -n
	or	a, a
	sbc	hl, hl
	sbc	hl, de
	ld	a, (.Lr_hi)
	ld	c, a
	or	a, a
	sbc	hl, bc
	jr	z, 21f
	jp	nc, .Lnext		; -n > hi: none in the tables
21:	add	hl, bc
	ld	a, l
	ld	(.Lr_lo), a
	; the table entries of cell lo: n = n0 + lo. The tests below start from it (cell b)
2:	ld	a, (.Lr_lo)
	ld	(.Lr_b), a
	ld	a, (.Lr_hi)
	ld	(.Lr_bh), a
	ld	hl, (.Lr_n)
	ld	de, 0
	ld	a, (.Lr_lo)
	ld	e, a
	add	hl, de
	push	hl
	pop	de
	add	hl, hl
	add	hl, de
	add	hl, hl			; 6 n
	ld	de, (.Lr_zp)
	add	hl, de			; the stepping entry
	ld	a, (VAR(V_FACES))
	and	a, FACE_XP | FACE_XN
	jr	nz, 3f
	ex	de, hl			; z run: it is the {XC, XS} one
	ld	hl, (.Lr_xp)
	ex	de, hl
	ld	(.Lr_xp), hl
	ex	de, hl
3:	push	hl
	pop	iy			; -> {ZS, ZC}
	ld	de, (iy + 0)		; ZS
	ld	bc, (iy + 3)		; ZC
	ld	iy, (.Lr_xp)		; -> {XC, XS}
	ld	hl, (iy + 3)		; XS
	add	hl, bc
	ld	(VAR(V_Z00)), hl	; z00 of cell lo
	ld	hl, (iy + 0)		; XC
	or	a, a
	sbc	hl, de
	ld	(VAR(V_X00)), hl	; x00 of cell lo

	; ---- the four box tests, as functions of the cell that must be >= 0 (linear along the run):
	;   v0 = z00 + m, m the least of -zlo (the farthest corner not nearer than the near plane),
	;        kzmax - yt (not below the view) and yt - cyl (not above it)
	;   v1 = c - z00, c the least of zlo + zspan - 1 (the farthest corner before the far plane)
	;        and lod_rect - 1 (nearer than the level of detail)
	;   v2 = cxr + 5 z00 - 4 x00 (not right of the view), v3 = 4 x00 + 5 z00 - cxl (not left)
	; at cells b and bh (.Lr_vals). Kept at both ends: every cell passes it; failed at both: none
	; does; else (a run crossing an edge of the view, rare) the cells are stepped (.Lr_test) ----
	ld	hl, (VAR(V_CAM_ZLO))
	ex	de, hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de			; -zlo
	push	hl
	pop	bc
	ld	hl, (VAR(V_CAM_KZMAX3))
	ld	de, (VAR(V_YT))
	or	a, a
	sbc	hl, de
	call	.Lr_min
	ld	hl, (VAR(V_YT))
	ld	de, (VAR(V_CAM_CYL))
	or	a, a
	sbc	hl, de
	call	.Lr_min
	ld	(.Lr_m), bc
	ld	hl, (VAR(V_CAM_ZLO))
	ld	de, (VAR(V_CAM_ZSPAN))
	add	hl, de
	dec	hl
	push	hl
	pop	bc
	ld	hl, (VAR(V_CAM_LODRECT))
	dec	hl
	call	.Lr_min
	ld	(.Lr_c3), bc
	; cell bh: cell b + (bh - b) (ax, az)
	ld	hl, (VAR(V_X00))
	ld	de, (VAR(V_Z00))
	ld	a, (.Lr_b)
	ld	c, a
	ld	a, (.Lr_bh)
	sub	a, c
	jr	z, 2f
	ld	b, a
1:	push	bc
	ld	bc, (.Lr_ax)
	add	hl, bc
	ex	de, hl
	ld	bc, (.Lr_az)
	add	hl, bc
	ex	de, hl
	pop	bc
	djnz	1b
2:	ld	(.Lr_ze), de
	ld	iy, .Lr_ve
	call	.Lr_vals
	ld	hl, (VAR(V_X00))
	ld	de, (VAR(V_Z00))
	ld	iy, .Lr_vb
	call	.Lr_vals

	; test k: hl = v at cell b, de = its step
	.macro	RTEST k
	ld	a, (.Lr_vb + 3 * \k + 2)
	ld	c, a
	ld	a, (.Lr_ve + 3 * \k + 2)
	and	a, c
	jp	m, .Lnext		; failed at both ends
	ld	a, (.Lr_ve + 3 * \k + 2)
	or	a, c
	jp	p, 9f			; kept at both
	call	.Lr_df\k
	ld	hl, (.Lr_vb + 3 * \k)
	call	.Lr_test
	jp	c, .Lnext
9:
	.endm
	RTEST	0
	RTEST	1
	RTEST	2
	RTEST	3

	; camera space of cell lo, from cell b (only when a test moved lo)
	ld	a, (.Lr_lo)
	ld	hl, .Lr_b
	sub	a, (hl)
	jr	z, 8f
	ld	b, a
	ld	hl, (VAR(V_X00))
	ld	de, (VAR(V_Z00))
9:	push	bc
	ld	bc, (.Lr_ax)
	add	hl, bc
	ex	de, hl
	ld	bc, (.Lr_az)
	add	hl, bc
	ex	de, hl
	pop	bc
	djnz	9b
	ld	(VAR(V_X00)), hl
	ld	(VAR(V_Z00)), de
8:
	; fog of cell lo (z00 >= cam_fog, as for a box). Cells b and bh alike (the usual case: fog
	; is beyond the level of detail): all of them. Else k, the first cell of the other fog
	; (0xff: none), stepping from lo
	ld	a, 0xff
	ld	(.Lr_k), a
	ld	de, (VAR(V_CAM_FOG))
	ld	hl, (VAR(V_Z00))
	or	a, a
	sbc	hl, de			; f(lo)
	ld	b, 0
	jp	m, 4f
	inc	b
4:	ld	a, b
	ld	(.Lr_fog), a
	push	hl
	ld	hl, (.Lr_ze)
	or	a, a
	sbc	hl, de			; f(bh)
	ld	a, 0
	jp	m, 41f
	inc	a
41:	pop	hl
	cp	a, b
	jr	z, 7f			; the same: so are lo .. hi
	ld	de, (.Lr_az)
	ld	a, (.Lr_lo)
	ld	c, a
5:	ld	a, (.Lr_hi)
	cp	a, c
	jr	z, 7f
	inc	c
	or	a, a
	adc	hl, de			; the fog test of cell c
	ld	a, 0
	jp	m, 6f
	inc	a
6:	ld	b, a
	ld	a, (.Lr_fog)
	cp	a, b
	jr	z, 5b
	ld	a, c
	ld	(.Lr_k), a
7:
	YLOGS

	; ---- the points lo .. hi + 1 on the face line into .Lpts as {x, z}: the face corner of
	; cell lo, then + (ax, az) each ----
	ld	a, (.Lr_hi)
	ld	hl, .Lr_lo
	sub	a, (hl)
	add	a, 2
	ld	(.Lr_np), a
	ld	b, a
	ld	hl, (VAR(V_X00))
	ld	de, (.Lr_fx)
	add	hl, de
	push	hl
	ld	hl, (VAR(V_Z00))
	ld	de, (.Lr_fz2)
	add	hl, de
	ex	de, hl			; de = z
	pop	hl			; hl = x
	ld	iy, .Lpts
.Lrp:
	ld	(iy + 0), hl
	ld	(iy + 3), de
	lea	iy, iy + 6
	push	de
	ld	de, (.Lr_ax)
	add	hl, de
	ex	(sp), hl
	ld	de, (.Lr_az)
	add	hl, de
	ex	de, hl
	pop	hl
	djnz	.Lrp

	; ---- one piece, or two split at cell k by fog ----
	xor	a, a
	ld	(.Lr_s), a
	ld	a, (.Lr_fog)
	ld	(.Lr_pf), a
	ld	a, (.Lr_k)
	inc	a
	jr	nz, .Lrun_split
	ld	a, (.Lr_np)
	dec	a
	ld	(.Lr_e), a
	call	.Lrun_piece
	jp	c, .Lout_full
	jp	.Lnext
.Lrun_split:
	dec	a
	ld	hl, .Lr_lo
	sub	a, (hl)
	ld	(.Lr_e), a		; points 0 .. k - i0
	ld	(.Lr_ks), a
	call	.Lrun_piece
	jp	c, .Lout_full
	ld	a, (.Lr_ks)
	ld	(.Lr_s), a		; then k - i0 .. np - 1, the other fog
	ld	a, (.Lr_np)
	dec	a
	ld	(.Lr_e), a
	ld	a, (.Lr_pf)
	xor	a, 1
	ld	(.Lr_pf), a
	call	.Lrun_piece
	jp	c, .Lout_full
	jp	.Lnext

; bc = min(bc, hl), signed. Clobbers hl
.Lr_min:
	or	a, a
	sbc	hl, bc
	ret	p
	add	hl, bc
	push	hl
	pop	bc
	ret

; de = -de. Clobbers nothing else but flags
.Lr_neg_de:
	push	hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de
	ex	de, hl
	pop	hl
	ret

; hl = 5 hl - 4 de, or (.Lr_5p4) 5 hl + 4 de. Clobbers de
.Lr_5m4:
	call	.Lr_neg_de
.Lr_5p4:
	push	hl
	ex	de, hl
	add	hl, hl
	add	hl, hl
	ex	de, hl			; 4 de
	add	hl, hl
	add	hl, hl
	ex	(sp), hl
	pop	bc			; bc = 4 hl
	add	hl, bc			; 5 hl
	add	hl, de
	ret

; the four test values (above) of the cell with x00 = hl, z00 = de into iy + 0, 3, 6, 9.
; Clobbers bc, de, hl
.Lr_vals:
	push	hl			; x00
	ld	hl, (.Lr_m)
	add	hl, de
	ld	(iy + 0), hl		; v0 = z00 + m
	ld	hl, (.Lr_c3)
	or	a, a
	sbc	hl, de
	ld	(iy + 3), hl		; v1 = c - z00
	push	de
	pop	hl
	add	hl, hl
	add	hl, hl
	add	hl, de
	ex	de, hl			; de = 5 z00
	pop	hl
	add	hl, hl
	add	hl, hl			; hl = 4 x00
	push	hl
	ex	de, hl
	push	hl
	ld	bc, (VAR(V_CAM_CXR))
	add	hl, bc
	or	a, a
	sbc	hl, de
	ld	(iy + 6), hl		; v2 = cxr + 5 z00 - 4 x00
	pop	hl
	add	hl, de
	ld	de, (VAR(V_CAM_CXL))
	or	a, a
	sbc	hl, de
	ld	(iy + 9), hl		; v3 = 4 x00 + 5 z00 - cxl
	pop	hl
	ret

; the step of test k along the run into de. Clobbers bc, hl
.Lr_df0:
	ld	de, (.Lr_az)
	ret
.Lr_df1:
	ld	de, (.Lr_az)
	jp	.Lr_neg_de
.Lr_df2:
	ld	hl, (.Lr_az)
	ld	de, (.Lr_ax)
	call	.Lr_5m4
	ex	de, hl
	ret
.Lr_df3:
	ld	hl, (.Lr_az)
	ld	de, (.Lr_ax)
	call	.Lr_5p4
	ex	de, hl
	ret

; Narrows the cells .Lr_lo .. .Lr_hi to those where a linear test holds: f(b) = hl for cell b =
; .Lr_b, f(i + 1) = f(i) + de, kept where f >= 0 (stepping through b .. .Lr_bh). Carry: no cell is
; left. Clobbers a, bc, hl
.Lr_test:
	ld	bc, 0
	or	a, a
	adc	hl, bc			; the sign of f(b)
	jp	m, .Lrt_neg
	ex	de, hl
	or	a, a
	adc	hl, bc			; f(b) >= 0: if de >= 0 every cell passes
	ex	de, hl
	ret	p			; (no carry)
	ld	a, (.Lr_b)		; else b .. c, c the last one with f >= 0
	ld	c, a
1:	ld	a, (.Lr_bh)
	cp	a, c
	ret	z			; (no carry)
	or	a, a
	adc	hl, de
	jp	m, 2f
	inc	c
	jr	1b
2:	ld	a, (.Lr_hi)		; hi = min(hi, c)
	cp	a, c
	jr	c, .Lrt_check
	ld	a, c
	ld	(.Lr_hi), a
	jr	.Lrt_check
.Lrt_neg:
	ex	de, hl
	or	a, a
	adc	hl, bc
	ex	de, hl
	jp	m, 4f			; f(b) < 0 and falling: none
	jr	z, 4f
	ld	a, (.Lr_b)		; rising: c .. bh, c the first one with f >= 0
	ld	c, a
3:	ld	a, (.Lr_bh)
	cp	a, c
	jr	z, 4f
	inc	c
	or	a, a
	adc	hl, de
	jp	m, 3b
	ld	a, (.Lr_lo)		; lo = max(lo, c)
	cp	a, c
	jr	nc, .Lrt_check
	ld	a, c
	ld	(.Lr_lo), a
.Lrt_check:
	ld	a, (.Lr_hi)		; carry if lo > hi
	ld	c, a
	ld	a, (.Lr_lo)
	cp	a, c
	jr	z, 5f
	ccf
	ret	nc			; lo < hi
	; lo > hi: carry is set
4:	scf
5:	ret
; One piece: points .Lr_s .. .Lr_e of .Lpts (at least 2), fog .Lr_pf, into the next slot(s) of
; out[] as BOX_RUN. Points behind the near plane are dropped, the last of them moved along the run
; to it (.Lclip_x; in place, so a point the pieces share stays on the run). Carry: out[] is full
; (project_full is then set by the caller)
.Lrun_piece:
	; first point behind: scan for the first one in front, move the one before it there (in
	; place: a point moved along the run stays on it, so the other piece can share it)
	ld	a, (.Lr_s)
	ld	h, a
	ld	l, 6
	mlt	hl
	ld	de, .Lpts
	add	hl, de
	push	hl
	call	.Lclip_front
	pop	iy
	jp	p, .Lrp_last
	ld	a, (.Lr_e)
	ld	hl, .Lr_s
	sub	a, (hl)
	ld	b, a			; points after the first
1:	lea	hl, iy + 6
	push	hl
	call	.Lclip_front
	pop	hl
	jp	p, 2f
	lea	iy, iy + 6
	djnz	1b
	; all behind: at other yaws nothing of the face shows. Facing N/E/S/W a box drawn so has
	; its near face moved to the near plane (LOG_ZC), so the run too, as it is (this face is
	; then a near face: the camera is pressed against it, nearer than the body allows)
	ld	a, (VAR(V_CAM_CARDINAL))
	or	a, a
	jp	nz, .Lrp_proj
	ret				; (no carry)
2:	call	.Lclip_x		; iy -> the last one behind, hl -> the first in front
	ld	(iy + 0), hl
	ld	hl, 3 * NEAR_PLANE
	ld	(iy + 3), hl
	lea	hl, iy + 0
	ld	de, .Lpts
	or	a, a
	sbc	hl, de
	ld	a, l			; 6 s: s = a / 6, at most 7 steps
	ld	b, 0
3:	or	a, a
	jr	z, 4f
	sub	a, 6
	inc	b
	jr	3b
4:	ld	a, b
	ld	(.Lr_s), a
.Lrp_last:
	; last point behind: scan back for the last one in front
	ld	a, (.Lr_e)
	ld	h, a
	ld	l, 6
	mlt	hl
	ld	de, .Lpts
	add	hl, de
	push	hl
	call	.Lclip_front
	pop	iy
	jp	p, .Lrp_proj
	ld	a, (.Lr_e)
	ld	b, a
1:	lea	hl, iy - 6
	push	hl
	call	.Lclip_front
	pop	hl
	jp	p, 2f
	lea	iy, iy - 6
	dec	b
	jr	1b			; ends: the first point is in front now
2:	call	.Lclip_x		; iy -> the first one behind from the end, hl -> the last in front
	ld	(iy + 0), hl
	ld	hl, 3 * NEAR_PLANE
	ld	(iy + 3), hl
	ld	a, b
	ld	(.Lr_e), a

.Lrp_proj:
	; P points, 1 or 2 slots
	ld	a, (.Lr_e)
	ld	hl, .Lr_s
	sub	a, (hl)
	inc	a
	ld	(.Lr_c), a
	ld	c, 1
	cp	a, RUN_SLOT_POINTS + 1
	jr	c, 1f
	inc	c
	cp	a, RUN_SLOT2_POINTS + 1
	jr	c, 1f
	inc	c
1:	ld	a, (VAR(V_ROOM))
	cp	a, c
	ret	c			; no room
	sub	a, c
	ld	(VAR(V_ROOM)), a
	ld	a, (VAR(V_KEPT))
	add	a, c
	ld	(VAR(V_KEPT)), a
	ld	iy, (VAR(V_OUT))
	ld	a, (.Lr_c)
	add	a, a
	add	a, a
	add	a, a
	add	a, a
	or	a, (VAR(V_FACES))
	ld	(iy + PB_FACES), a
	ld	(iy + PB_KIND), BOX_RUN
	ld	a, (.Lr_pf)
	or	a, a
	ld	a, (.Lr_mat)
	jr	z, 2f
	add	a, MAT_FOGGED
2:	ld	(iy + PB_MAT), a
	push	bc			; c = slots
	ld	a, (.Lr_s)
	ld	h, a
	ld	l, 6
	mlt	hl
	ld	de, .Lpts
	add	hl, de
	ld	(.Lr_pp), hl
	ld	a, (.Lr_c)
3:	push	af
	ld	hl, (.Lr_pp)
	ld	de, (hl)
	ld	(VAR(V_CX)), de
	inc	hl
	inc	hl
	inc	hl
	ld	de, (hl)
	ld	(VAR(V_CZ)), de
	inc	hl
	inc	hl
	inc	hl
	ld	(.Lr_pp), hl
	CORNER	0, 1, 2			; x, top, bottom: v[3 k], v[3 k + 1], v[3 k + 2]
	lea	iy, iy + 9
	pop	af
	dec	a
	jp	nz, 3b
	pop	bc
	ld	iy, (VAR(V_OUT))
	ld	de, PB_SIZE
4:	add	iy, de
	dec	c
	jr	nz, 4b
	ld	(VAR(V_OUT)), iy
	ld	a, (VAR(V_ROOM))
	or	a, a
	ret	nz			; (no carry)
	scf				; out[] is full
	ret


; One corner: iy -> {x, z} of corner k, its result goes to iy + 24. hl -> its neighbour along its
; z side, de -> along its x side, c and b = the FACE_* bits of those sides. A corner in front of the
; near plane is copied. One behind it is moved to depth NEAR_PLANE along the edge to a neighbour in
; front: the z one if both are in front and its side is visible or the x side is not, and the
; opposite corner (hl + de - iy, which is then in front: the farthest corner is) if neither is.
.Lclip_one:
	ld	(.Lnb_z), hl
	ld	(.Lnb_x), de
	add	hl, de
	lea	de, iy + 0
	or	a, a
	sbc	hl, de
	ld	(.Lnb_d), hl
	ld	hl, (iy + 0)
	ld	(iy + 24), hl
	ld	hl, (iy + 3)
	ld	(iy + 27), hl
	ld	de, 3 * NEAR_PLANE
	or	a, a
	sbc	hl, de
	ret	p			; in front: as it is
	ld	hl, (.Lnb_z)
	call	.Lclip_front
	jp	m, .Lclip_zb
	ld	hl, (.Lnb_x)		; the z neighbour is in front
	call	.Lclip_front
	ld	hl, (.Lnb_z)
	jp	m, .Lclip_to		; only it
	ld	a, (VAR(V_FACES))	; both: the z side unless only the x side is visible
	and	a, c
	jr	nz, .Lclip_to
	ld	a, (VAR(V_FACES))
	and	a, b
	jr	z, .Lclip_to
	ld	hl, (.Lnb_x)
	jr	.Lclip_to
.Lclip_zb:
	ld	hl, (.Lnb_x)
	call	.Lclip_front
	ld	hl, (.Lnb_x)
	jp	p, .Lclip_to
	ld	hl, (.Lnb_d)
.Lclip_to:
	call	.Lclip_x
	ld	(iy + 24), hl
	ld	hl, 3 * NEAR_PLANE
	ld	(iy + 27), hl
	ret
	; hl -> corner A in front, iy -> D behind: hl = x = xD + sign(dx) |dx| a / b of the point at
	; z = NEAR_PLANE, with dx = xA - xD, a = NEAR_PLANE - zD, b = zA - zD (0 < a < b <= SUB: one
	; edge). |dx| a / b = expTable[log |dx| + log a - log b - log FOCAL_LENGTH], logs x3 as usual
.Lclip_x:
	push	hl
	inc	hl
	inc	hl
	inc	hl
	ld	hl, (hl)		; zA
	ld	de, (iy + 3)
	or	a, a
	sbc	hl, de			; b
	ld	de, _logTable
	add	hl, de
	ld	hl, (hl)
	ld	(.Lclip_lb), hl
	ld	hl, 3 * NEAR_PLANE
	ld	de, (iy + 3)
	or	a, a
	sbc	hl, de			; a
	ld	de, _logTable
	add	hl, de
	ld	hl, (hl)
	ld	de, (.Lclip_lb)
	or	a, a
	sbc	hl, de
	ld	(.Lclip_lb), hl		; log a - log b
	pop	hl
	ld	hl, (hl)		; xA
	ld	de, (iy + 0)
	or	a, a
	sbc	hl, de			; dx
	LOG_ABS				; a = 1: dx <= 0
	ld	de, (.Lclip_lb)
	add	hl, de
	ld	de, 3 * (TABLE_FOCAL_LOG2 * TABLE_LOG_STEPS + EXP_TABLE_MIN)
	or	a, a
	sbc	hl, de			; offset into expTable
	jp	p, 1f
	ld	hl, 0			; below the table: rounds to 0
	jr	2f
1:	ld	de, _expTable
	add	hl, de
	ld	hl, (hl)		; |dx| a / b
2:	push	hl
	pop	de
	add	hl, hl
	add	hl, de			; x3
	bit	0, a
	jr	z, 3f
	ex	de, hl
	or	a, a
	sbc	hl, hl
	sbc	hl, de			; dx <= 0: minus
3:	ld	de, (iy + 0)
	add	hl, de
	ret

; sign flag: negative if the corner at hl is behind the near plane. Clobbers de, hl
.Lclip_front:
	inc	hl
	inc	hl
	inc	hl
	ld	hl, (hl)
	ld	de, 3 * NEAR_PLANE
	or	a, a
	sbc	hl, de
	ret


	.section	.bss._projectRecords,"aw",@nobits
; Everything here is addressed through ix (VAR), so the block must stay under 128 bytes.
; The _cam_ values are written by setupCamera() in camera.c (declared in engine.h).
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
	.global	_cam_kxmid3, _cam_kzmid3, _cam_lod_rect, _cam_fog
_cam_kxmid3:	.zero	3
_cam_kzmid3:	.zero	3
_cam_lod_rect:	.zero	3
_cam_fog:	.zero	3
.Lroom:		.zero	1			; slots out[] still has room for
.Lexpo:		.zero	1			; exposed faces of the record, less those in runs
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
	.if	_cam_lod_rect - .Lvars != V_CAM_LODRECT || _cam_fog - .Lvars != V_CAM_FOG
	.error	"V_CAM_LODRECT or V_CAM_FOG does not match .Lvars"
	.endif
	.if	.Lroom - .Lvars != V_ROOM || .Lexpo - .Lvars != V_EXPO
	.error	"V_ROOM or V_EXPO does not match .Lvars"
	.endif

; near-plane clipping (.Lgen_clip): the corners {x, z}, then the clipped ones (24 bytes on)
.Lcorn:		.zero	24
.Lclp:		.zero	24
.Lnb_z:		.zero	3			; .Lclip_one: the neighbours of the corner
.Lnb_x:		.zero	3
.Lnb_d:		.zero	3
.Lclip_lb:	.zero	3
.Lclip_p:	.zero	3			; .Lgen_clip: the next clipped corner, corners left
.Lclip_n:	.zero	1

; runs (.Lrun)
	.global	_project_full
_project_full:	.zero	1			; out[] is full (engine.h)
	.global	_project_left
_project_left:	.zero	1			; records not done (engine.h)
.Lr_bx:		.zero	3			; bias_x, bias_z
.Lr_bz:		.zero	3
.Lr_xp:		.zero	3			; -> the {XC, XS} entry of the fixed line, then of cell lo
.Lr_zp:		.zero	3			; the stepping table (rotZ_ or rotX_)
.Lr_ax:		.zero	3			; camera space step along the run (x3)
.Lr_az:		.zero	3
.Lr_fx:		.zero	3			; the face corner of a cell from its low corner
.Lr_fz2:	.zero	3
.Lr_n:		.zero	3			; grid index of cell 0 along the run
.Lr_pp:		.zero	3			; the current point
.Lr_mat:	.zero	1
.Lr_len:	.zero	1			; cells
.Lr_lo:		.zero	1			; the first and last cell drawn
.Lr_hi:		.zero	1
.Lr_b:		.zero	1			; the first and last cell in the tables
.Lr_bh:		.zero	1
.Lr_k:		.zero	1			; the first of them of the other fog (0xff: none)
.Lr_fog:	.zero	1			; the fog of cell lo
.Lr_np:		.zero	1			; points in .Lpts
.Lr_s:		.zero	1			; the piece: first and last point, fog, points
.Lr_e:		.zero	1
.Lr_pf:		.zero	1
.Lr_c:		.zero	1
.Lr_ks:		.zero	1			; the point the pieces share
.Lpts:		.zero	6 * RUN_POINTS		; {x, z} of the points of the kept cells
.Lr_m:		.zero	3			; the constants of tests 0 and 1
.Lr_c3:		.zero	3
.Lr_ze:		.zero	3			; z00 of cell bh
.Lr_vb:		.zero	12			; the test values of cells b and bh
.Lr_ve:		.zero	12

; Faces of a box towards the camera by the grid index of each axis (camera.c, cameraInit):
; three consecutive pages, so inc h steps from one to the next
	.balign	1024
	.global	_face_x, _face_y, _face_z
_face_x:	.zero	256
_face_y:	.zero	256
_face_z:	.zero	256

	.extern	_cam_bias_y
	.extern	_lod_rect_dist
	.extern	_gridDist
	.extern	_rotX_
	.extern	_rotZ_
	.extern	_camY_
	.extern	_logTable
	.extern	_expTable
