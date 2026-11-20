; fillScene / drawSpans: filled, shaded voxels with hidden surface removal.
;
;   void fillScene(const projected_box_t *boxes, uint8_t n, uint24_t base)
;   void drawSpans(void)
;
; fill_ref.c is the same algorithm in C and draws the same frame, pixel for pixel: verify changes
; here against it (CLAUDE.md, "Verify asm changes against C").
;
; ---- How it works ----
; Boxes are visited NEAR TO FAR (the painter order of main.c, backwards). Each viewport row keeps
; the list of its x intervals that nothing has covered yet. Every face of a box that faces the
; camera and is not covered by a neighbouring voxel (projectScene decides which) is a convex quad
; on screen; per row it covers [xl, xr). Only the parts of that inside open intervals are listed
; ("emitted") as spans, and then removed from the intervals. So a pixel hidden by a nearer box is
; never written, a face whose rows are all closed costs a scan of them, and once every row is
; closed the farther boxes are not looked at. The background (sky above the horizon, row
; CENTRE_Y, ground below) is filled row by row with push before the spans are drawn: that costs
; about a third of drawing it as spans.
;
; Rows rather than columns: floors are the bulk of a voxel scene, and a floor tile is several
; times wider than tall on screen. Per-row work beat per-column work by that factor (a column
; version of this file needed about twice the time on a floor).
;
; fillScene only lists spans: it does not touch the frame buffer, so main() runs it before
; waitForBufferScan, while the LCD still shows the last frame. drawSpans, after the wait, writes
; the pixels: that is the only part tied to the LCD refresh.
;
; Speed: the CE is memory bound, about 4 cycles per byte fetched (opcodes included) and 2 per byte
; written. So locals live in .Lpage, addressed as (ix + d) (3 bytes against 4 for an absolute
; address), colours are patched into immediates, and the row loop has a path of its own for the
; usual case of one open interval.
;
; ---- Per face (.Lface) ----
; 1. Its 4 vertices, clockwise on screen (every face facing the camera comes out clockwise with
;    the order in .Lfaces). The top and the bottom vertex split it into the right side (clockwise
;    from the top) and the left side.
; 2. Rows [top y, bottom y), clipped to the viewport and trimmed to rows still open.
; 3. Each side becomes one x per row (see Edges), stored straight into the row blocks.
; 4. Every row: intersect [xl, xr) with each open interval, emit, update the list. A rounding
;    inversion (xl > xr) just means nothing is drawn.
;
; ---- Edges ----
; An edge from (x0, y0) down to (x1, y1) gives x = floor(x + 1/2) in each row, stepped with 16
; fractional bits: the integer part in a 24-bit register, the fraction in an 8-bit one, so a step
; is add a, c / adc hl, de. The slope dx * 65536 / dy is (|dx| recipTable[dy]) >> 8, with
; recipTable[dy] = round(2^24 / dy): mlt instead of a division (3 of them when |dx| < 256, 6
; otherwise). It drifts by under 0.05 px over the longest possible edge. An edge entered at a
; later row first jumps there by shift and add. x outside 0 .. 255 is clamped; anything from VIEW_W
; up means "right of the viewport", which the intervals clip anyway. Two faces sharing an edge
; compute it from the same two vertices in the same direction, so they meet without gaps.
;
; ---- Per-row block (GAP_BLOCK = 32 bytes, 32-aligned: offsets added to l never carry) ----
;   +0      number of open intervals n (0 .. GAP_MAX)
;   +1      scratch: intervals left to look at, in the row loop
;   +2, +3  xl, xr of the face being drawn
;   +4      scratch
;   +7      the open intervals: n pairs [left, right), unordered
;   +29     address of the first viewport column of this row in the frame being drawn
; Overflow: a row holds at most GAP_MAX intervals. A face strictly inside an interval splits it;
; with no room left, the part right of the face is dropped from the list. It then shows the
; background even where a farther box should be. That needs GAP_MAX + 1 separate holes in a row.
;
; ---- Spans ----
; 6 bytes: address of the first pixel, entry point, colour, unused. The row loop writes them with
; push (SP points into the list, interrupts are off), so the list grows down from SPAN_LIST_END.
; drawSpans pops them and jumps into a 256-aligned unrolled loop (ld (hl), a / inc hl per pixel)
; at the entry point, the low byte 256 - 2 len of the jump target; spans longer than SPAN_LEN are
; listed in two parts. Entry point 0 (the first record, popped last) is the exit.
;
; Memory safety: every span lies inside an open interval, intervals only ever shrink from
; [0, VIEW_W), and rows are clipped to the viewport, so no span leaves the field's viewport.
; A face only starts when GAP_MAX + 1 spans per row fit in the list (.Lface, step 2).
;
; ABI: arguments at sp+3 ...; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

#include "engine.h"
#include "fill.h"

#if VIEW_X1 - VIEW_X0 > 255 || VIEW_H > 255
#error "x and y are bytes"
#endif
#if 7 + 2 * GAP_MAX > 29 || GAP_BLOCK != 32 || GAP_MAX < 2
#error "per-row block layout"
#endif
#if SPAN_SIZE != 6
#error "span records are pushed as two 3-byte words"
#endif
#if VIEW_X0 != 0
#error "subtract VIEW_X0 from the vertex x (fill_ref.c does)"
#endif

#define B_N		0
#define B_LEFT		1
#define B_XL		2
#define B_XR		3
#define B_TMP		4
#define B_GAPS		7
#define B_ROWBASE	29

#define SPAN_LEN	126		/* longest span in one record: 2 len + the exit jump fit a page */

; .Lpage (256-aligned), addressed as (ix + offset - 128) with ix = .Lpage + 128.
; Vertex v = corner k at the top (v = k) or the bottom (v = k + 4)
#define P_X		0		/* x of vertex v at P_X + 3 v */
#define P_Y		24		/* y at P_Y + 3 v */
#define P_MAT		64		/* material colours: top, x side, z side, bottom */
#define P_FX		96		/* the face being drawn: x of its vertex i at P_FX + 3 i */
#define P_FY		108		/* y at P_FY + 3 i */
#define L_YMIN		128		/* the face: top and bottom y, their vertices (offsets P_FY + 3 i) */
#define L_YMAX		131
#define L_T		134
#define L_B		135
#define L_Y0		136		/* its rows [y0, y1), 24 bits */
#define L_Y1		139
#define L_R0		142		/* the same as bytes: first row, rows */
#define L_NROW		143
#define L_BLK0		144		/* block of the first row */
#define L_DIR		147		/* side: vertex step (+-3), block slot, next row (24 bits) */
#define L_SLOT		148
#define L_RC		149
#define L_K0		152		/* edge: its vertices (offsets P_FY + 3 i), first row, rows */
#define L_K1		153
#define L_RI		154
#define L_COUNT		155
#define L_KK		156		/* k = row - y0 of the first row (24 bits) */
#define L_RP		159		/* -> recipTable[dy] */
#define L_DY1		162		/* 0 if dy = 1 */
#define L_CLAMP		163		/* the edge needs the clamping loop */
#define L_NEG		164
#define L_SFRAC		165
#define L_R		166		/* recipTable[dy] */
#define L_TMP		169		/* 4 bytes, [3] stays 0: a 24-bit value at +0 read from +1 is >> 8 */
#define L_CLIPPED	173		/* the box is not NOCLIP: edges may need clamping */
#define L_OUTP		174
#define V(o)		ix + o - 128

#define VIEW_W		VIEW_X1 - VIEW_X0	/* no parentheses: see CLAUDE.md. Mind the precedence */
#define BG_REM		(VIEW_X1 - VIEW_X0) % 3	/* only in .rept or after another term */
#define SPAN_ROOM	SPAN_SIZE * GAP_MAX + SPAN_SIZE	/* list bytes per row a face may need */

	.assume	ADL = 1

; ==================================================================================================
	.section	.text._fillScene,"ax",@progbits
	.global	_fillScene
	.type	_fillScene,@function
_fillScene:
	push	ix
	ld	iy, 0
	add	iy, sp			; iy + 6 = boxes, iy + 9 = n, iy + 12 = base
	ld	a, i			; p/v = interrupts enabled
	push	af
	di

	; every row: one open interval [0, VIEW_W), and its address in this frame
	ld	hl, (iy + 12)
	ld	de, VIEW_X0
	add	hl, de
	ld	(.Lfield), hl
	ld	ix, .Lrows
	ld	de, 320
	ld	b, VIEW_H
1:	ld	(ix + B_N), 1
	ld	(ix + B_GAPS), 0
	ld	(ix + B_GAPS + 1), VIEW_W
	ld	(ix + B_ROWBASE), hl
	add	hl, de
	lea	ix, ix + GAP_BLOCK
	djnz	1b
	ld	a, VIEW_H
	ld	(.Lopen), a
	ld	(.Lbg_pending), a	; non-zero: drawSpans fills the background
	; the span list: only the end record (entry point 0)
	ld	hl, SPAN_LIST_END - SPAN_SIZE
	ld	(.Lspanp), hl
	xor	a, a
	ld	(SPAN_LIST_END - SPAN_SIZE + 3), a

	; boxes front to back: from the end of the list
	ld	a, (iy + 9)
	or	a, a
	jr	z, .Lboxes_done
	ld	(.Lnbox), a
	ld	h, a
	ld	l, PB_SIZE
	mlt	hl
	ld	de, (iy + 6)
	add	hl, de
	ld	(.Lboxp), hl
.Lbox_loop:
	ld	hl, (.Lboxp)
	ld	de, -PB_SIZE
	add	hl, de
	ld	(.Lboxp), hl
	push	hl
	pop	iy
	call	.Lbox
	ld	a, (.Lopen)
	or	a, a
	jr	z, .Lall_closed		; every row is closed: the rest is hidden
	ld	hl, .Lnbox
	dec	(hl)
	jr	nz, .Lbox_loop
	jr	.Lboxes_done
.Lall_closed:
	ld	(.Lbg_pending), a	; and no background is visible either
.Lboxes_done:
	pop	af
	jp	po, 1f
	ei
1:	pop	ix
	ret


; ==================================================================================================
; one box: iy -> projected_box_t. Clobbers every register
.Lbox:
	ld	ix, .Lpage + 128
	; ---- the 8 vertices: x at P_X + 3 v, y at P_Y + 3 v ----
	ld	a, (iy + PB_KIND)
	cp	a, BOX_GEN_NOCLIP
	jr	c, .Laxis
	; GEN: sx[4] st[4] sb[4]
	.macro	GEN_CORNER k
	ld	de, (iy + PB_V + 3 * \k)
	ld	(V(P_X + 3 * \k)), de
	ld	(V(P_X + 3 * \k + 12)), de
	ld	de, (iy + PB_V + 12 + 3 * \k)
	ld	(V(P_Y + 3 * \k)), de
	ld	de, (iy + PB_V + 24 + 3 * \k)
	ld	(V(P_Y + 3 * \k + 12)), de
	.endm
	GEN_CORNER 0
	GEN_CORNER 1
	GEN_CORNER 2
	GEN_CORNER 3
	jp	.Lcorners_done

	; nl nr nt nb fl fr ft fb: edge e is world corner k = cam_axis_corner[e]
	.macro	AXIS_EDGE e, vx, vt, vb
	ld	a, (_cam_axis_corner + \e)
	ld	e, a
	add	a, a
	add	a, e			; 3 k
	ld	l, a
	ld	de, (iy + PB_V + 3 * \vx)
	ld	(hl), de		; x, top
	add	a, 12
	ld	l, a
	ld	(hl), de		; x, bottom
	add	a, P_Y - 12
	ld	l, a
	ld	de, (iy + PB_V + 3 * \vt)
	ld	(hl), de		; y, top
	add	a, 12
	ld	l, a
	ld	de, (iy + PB_V + 3 * \vb)
	ld	(hl), de		; y, bottom
	.endm
.Laxis:
	ld	hl, .Lpage
	AXIS_EDGE 0, 0, 2, 3
	AXIS_EDGE 1, 1, 2, 3
	AXIS_EDGE 2, 4, 6, 7
	AXIS_EDGE 3, 5, 6, 7
.Lcorners_done:
	; NOCLIP: every vertex is inside the viewport, so no edge needs clamping
	ld	a, (iy + PB_KIND)
	cp	a, BOX_AXIS_NOCLIP
	jr	z, 1f
	cp	a, BOX_GEN_NOCLIP
1:	ld	a, 0
	jr	z, 2f
	inc	a
2:	ld	(V(L_CLIPPED)), a

	; ---- material colours to P_MAT ----
	ld	a, (iy + PB_MAT)
	add	a, a
	add	a, a
	or	a, a
	sbc	hl, hl
	ld	l, a
	ld	de, _materials
	add	hl, de
	ld	de, (hl)		; top, x side, z side
	ld	(V(P_MAT)), de
	inc	hl
	inc	hl
	inc	hl
	ld	a, (hl)
	ld	(V(P_MAT + 3)), a

	; ---- each visible face ----
	ld	a, (iy + PB_FACES)
	ld	hl, .Lfaces
1:	srl	a
	jr	nc, 2f
	push	af
	push	hl
	call	.Lface
	pop	hl
	pop	af
2:	ld	de, 8
	add	hl, de
	or	a, a
	jr	nz, 1b
	ret


; ==================================================================================================
; one face: hl -> its .Lfaces entry (4 vertex offsets 3 v, colour offset). ix = .Lpage + 128
.Lface:
	; ---- 1. its vertices into P_FX / P_FY; the top one (t) and the bottom one (b) ----
	ld	de, .Lpage
	.macro	FACE_VERTEX i
	ld	e, (hl)
	inc	hl
	ex	de, hl
	ld	bc, (hl)		; x
	ld	(V(P_FX + 3 * \i)), bc
	ld	a, l
	add	a, P_Y
	ld	l, a
	ld	bc, (hl)		; y
	ld	(V(P_FY + 3 * \i)), bc
	ex	de, hl
	.endm
	FACE_VERTEX 0
	FACE_VERTEX 1
	FACE_VERTEX 2
	FACE_VERTEX 3
	ld	e, (hl)
	ld	a, (de)
	ld	(.Lsm_col + 1), a	; the colour, for the row loop

	; t = argmin y, b = argmax y (the first one on ties), as offsets P_FY + 3 i
	ld	hl, (V(P_FY))
	ld	(V(L_YMIN)), hl
	ld	(V(L_YMAX)), hl
	ld	a, P_FY
	ld	(V(L_T)), a
	ld	(V(L_B)), a
	.macro	MINMAX i
	ld	hl, (V(P_FY + 3 * \i))
	ld	de, (V(L_YMIN))
	or	a, a
	sbc	hl, de
	add	hl, de			; (keeps the sign flag)
	jp	p, 1f
	ld	(V(L_YMIN)), hl
	ld	(V(L_T)), P_FY + 3 * \i
1:	ld	de, (V(L_YMAX))
	ex	de, hl
	or	a, a
	sbc	hl, de			; ymax - y
	jp	p, 2f
	ld	(V(L_YMAX)), de
	ld	(V(L_B)), P_FY + 3 * \i
2:
	.endm
	MINMAX 1
	MINMAX 2
	MINMAX 3

	; ---- 2. rows [max(ymin, 0), min(ymax, VIEW_H)), trimmed to open ones ----
	ld	hl, (V(L_YMIN))
	ld	de, 0
	or	a, a
	sbc	hl, de
	jp	p, 1f
	sbc	hl, hl			; above the viewport: 0 (no borrow above)
1:	ld	(V(L_Y0)), hl		; may be past the end
	ld	hl, (V(L_YMAX))
	ld	de, VIEW_H
	or	a, a
	sbc	hl, de
	add	hl, de			; (keeps the sign flag)
	jp	m, 2f
	ex	de, hl			; below the viewport: VIEW_H
2:	ld	de, (V(L_Y0))
	or	a, a
	sbc	hl, de			; rows
	ret	m
	ret	z
	ld	b, l			; at most VIEW_H: bytes from here on
	ld	a, e			; first row
	ld	h, a
	ld	l, GAP_BLOCK
	mlt	hl
	ld	de, .Lrows
	add	hl, de
	ld	de, GAP_BLOCK
1:	inc	(hl)
	dec	(hl)
	jr	nz, 2f
	add	hl, de
	inc	a
	djnz	1b
	ret				; every row is closed
2:	ld	(V(L_R0)), a
	ld	(V(L_BLK0)), hl
	ld	c, b
	dec	c
	ld	d, c
	ld	e, GAP_BLOCK
	mlt	de
	add	hl, de			; block of the last row
	ld	de, -GAP_BLOCK
3:	inc	(hl)
	dec	(hl)
	jr	nz, 4f
	add	hl, de
	djnz	3b			; ends at the latest at the first row, which is open
4:	ld	(V(L_NROW)), b

	; room in the span list for the worst case, GAP_MAX + 1 spans per row
	ld	h, b
	ld	l, SPAN_ROOM
	mlt	hl
	ex	de, hl
	ld	hl, (.Lspanp)
	or	a, a
	sbc	hl, de
	ld	de, SPAN_LIST
	or	a, a
	sbc	hl, de
	call	c, .Lflush

	; rows as 24-bit numbers, for the edge arithmetic
	or	a, a
	sbc	hl, hl
	ld	l, (V(L_R0))
	ld	(V(L_Y0)), hl
	ld	bc, 0
	ld	c, (V(L_NROW))
	add	hl, bc
	ld	(V(L_Y1)), hl

	; ---- 3. the right side (clockwise from t) into xr, the left side into xl ----
	ld	(V(L_DIR)), 3		; + 3: the next vertex
	ld	a, B_XR
	call	.Lside
	ld	(V(L_DIR)), -3 & 255
	ld	a, B_XL
	call	.Lside

	; ---- 4. the rows ----
	ld	b, (V(L_NROW))
	ld	ix, (V(L_BLK0))
	ld	de, 0			; d and the upper byte stay 0 in the row loop
.Lsm_col:
	ld	c, 0			; the colour (patched)
	ld	(.Lrow_sp), sp
	ld	sp, (.Lspanp)
	jp	.Lrow
.Lrows_done:
	ld	(.Lspanp), sp
	ld	sp, (.Lrow_sp)
	ld	ix, .Lpage + 128
	ret


; ---- the list is short of room: draw it now (needs the back buffer) ----
.Lflush:
	call	_waitForBufferScan	; C: preserves ix
	jp	_drawSpans		; empties the list


; ==================================================================================================
; ---- the row loop. SP = span list pointer (no calls, no stack), ix = block of the first row,
; b = rows, c = colour, d and the upper byte of de = 0. Ends with jp .Lrows_done

; one span record: e = first x, a = len (1 .. SPAN_LEN). Clobbers a, hl
	.macro	EMIT1
	add	a, a
	neg				; 256 - 2 len: the entry point
	ld	l, a
	ld	h, c			; the colour
	push	hl
	ld	hl, (ix + B_ROWBASE)
	add	hl, de
	push	hl			; the address
	.endm
; a span: e = first x, a = len (1 .. VIEW_W). Clobbers a, e, hl
	.macro	EMIT
	cp	a, SPAN_LEN + 1
	jr	c, 1f
	sub	a, SPAN_LEN
	ld	(ix + B_TMP), a		; the rest
	ld	a, SPAN_LEN
	EMIT1
	ld	a, e
	add	a, SPAN_LEN
	ld	e, a
	ld	a, (ix + B_TMP)
1:	EMIT1
	.endm

.Lrow:
	ld	a, (ix + B_N)
	dec	a
	jp	nz, .Lrow_multi
	; ---- the usual case: one open interval [g0, g1) ----
	ld	a, (ix + B_XL)
	ld	l, (ix + B_GAPS)
	cp	a, l
	jr	nc, 1f
	ld	a, l
1:	ld	e, a			; A = max(xl, g0)
	ld	a, (ix + B_XR)
	ld	h, (ix + B_GAPS + 1)
	cp	a, h
	jr	c, 2f
	ld	a, h			; B = min(xr, g1)
2:	sub	a, e
	jp	z, .Lrow_next
	jp	c, .Lrow_next		; B <= A: no overlap
	EMIT
	; the interval loses [xl, xr)
	ld	a, (ix + B_XL)
	cp	a, (ix + B_GAPS)
	jr	z, 3f
	jr	c, 3f			; xl <= g0: nothing stays open left
	ld	h, (ix + B_GAPS + 1)
	ld	(ix + B_GAPS + 1), a	; [g0, xl) stays open
	ld	a, (ix + B_XR)
	cp	a, h
	jr	nc, .Lrow_next
	ld	(ix + B_GAPS + 2), a	; and [xr, old g1)
	ld	(ix + B_GAPS + 3), h
	ld	(ix + B_N), 2
	jr	.Lrow_next
3:	ld	a, (ix + B_XR)
	cp	a, (ix + B_GAPS + 1)
	jr	nc, 4f
	ld	(ix + B_GAPS), a	; [xr, g1) stays open
	jr	.Lrow_next
4:	ld	(ix + B_N), 0		; closed
	ld	hl, .Lopen
	dec	(hl)
.Lrow_next:
	lea	ix, ix + GAP_BLOCK
	dec	b
	jp	nz, .Lrow
	jp	.Lrows_done

	; ---- no open interval, or several ----
.Lrow_multi:
	inc	a
	jp	z, .Lrow_next
	ld	(ix + B_LEFT), a
	lea	iy, ix + B_GAPS
.Lgap:
	ld	a, (ix + B_XL)
	cp	a, (iy + 0)
	jr	nc, 1f
	ld	a, (iy + 0)
1:	ld	e, a			; A
	ld	a, (ix + B_XR)
	cp	a, (iy + 1)
	jr	c, 2f
	ld	a, (iy + 1)
2:	sub	a, e
	jp	z, .Lgap_next
	jp	c, .Lgap_next
	EMIT
	ld	a, (ix + B_XL)
	cp	a, (iy + 0)
	jr	z, .Lno_left
	jr	c, .Lno_left
	ld	l, (iy + 1)		; old g1
	ld	(iy + 1), a		; [g0, xl) stays open
	ld	a, (ix + B_XR)
	cp	a, l
	jr	nc, .Lgap_next
	; [xr, old g1) too: a split. The face was inside this interval, so no other one overlaps it
	ld	h, a
	ld	a, (ix + B_N)
	cp	a, GAP_MAX
	jr	z, .Lrow_next		; no room: it keeps the background (see the header)
	inc	(ix + B_N)
	add	a, a
	add	a, B_GAPS
	ld	e, a
	lea	iy, ix + 0
	add	iy, de
	ld	(iy + 0), h
	ld	(iy + 1), l
	jp	.Lrow_next
.Lno_left:
	ld	a, (ix + B_XR)
	cp	a, (iy + 1)
	jr	nc, .Lremove		; xr >= g1: closed
	ld	(iy + 0), a		; [xr, g1) stays open
	jr	.Lgap_next
.Lremove:
	; move the last interval here, and look at this place again
	dec	(ix + B_N)
	jr	z, .Lrow_closed
	ld	a, (ix + B_N)
	add	a, a
	add	a, B_GAPS
	lea	hl, ix + 0
	add	a, l
	ld	l, a
	ld	a, (hl)
	ld	(iy + 0), a
	inc	hl
	ld	a, (hl)
	ld	(iy + 1), a
	dec	(ix + B_LEFT)
	jp	nz, .Lgap
	jp	.Lrow_next
.Lrow_closed:
	ld	hl, .Lopen
	dec	(hl)
	jp	.Lrow_next
.Lgap_next:
	lea	iy, iy + 2
	dec	(ix + B_LEFT)
	jp	nz, .Lgap
	jp	.Lrow_next


; ==================================================================================================
; ---- one side of the face: from vertex t to vertex b, stepping L_DIR (+-3, wrapping in the 4
; vertices), x of each row [y0, y1) into block slot a. ix = .Lpage + 128
.Lside:
	ld	(V(L_SLOT)), a
	ld	hl, (V(L_Y0))
	ld	(V(L_RC)), hl
	ld	a, (V(L_T))
	ld	(V(L_K0)), a
.Ledge_loop:
	; the next vertex
	add	a, (V(L_DIR))
	cp	a, P_FY
	jr	nc, 1f
	add	a, 12			; wrapped below the first vertex
1:	cp	a, P_FY + 12
	jr	c, 2f
	sub	a, 12			; past the last
2:	ld	(V(L_K1)), a
	; end = y1 for the last edge or when y(k1) > y1, else y(k1)
	ld	hl, .Lpage
	ld	l, a
	ld	de, (hl)		; yb
	ld	hl, (V(L_Y1))
	cp	a, (V(L_B))
	jr	z, 3f
	or	a, a
	sbc	hl, de			; y1 - yb
	add	hl, de			; (keeps the sign flag)
	jp	m, 3f
	ex	de, hl
3:	ld	de, (V(L_RC))
	or	a, a
	sbc	hl, de			; rows of this edge
	jp	m, .Ledge_next
	jp	z, .Ledge_next		; none: the next edge starts at the same row
	ld	(V(L_COUNT)), l
	add	hl, de
	ld	(V(L_RC)), hl		; where the next one starts
	ld	(V(L_RI)), e		; first row
	call	.Ledge
.Ledge_next:
	ld	a, (V(L_K1))
	ld	(V(L_K0)), a
	cp	a, (V(L_B))
	jp	nz, .Ledge_loop
	ret

; x of the current edge (vertices k0 -> k1, offsets of y; x is 12 lower) for L_COUNT rows from
; row L_RI; de = that row (24 bits)
.Ledge:
	; k = row - ya, dy = yb - ya (here ya <= row < yb)
	ld	hl, .Lpage
	ld	l, (V(L_K0))
	ld	hl, (hl)		; ya
	ex	de, hl
	or	a, a
	sbc	hl, de
	ld	(V(L_KK)), hl
	ld	hl, .Lpage
	ld	l, (V(L_K1))
	ld	hl, (hl)
	or	a, a
	sbc	hl, de			; dy >= 1
	ld	a, l
	dec	a
	or	a, h
	ld	(V(L_DY1)), a		; 0: dy = 1, the slope is never used
	push	hl
	pop	de
	add	hl, hl
	add	hl, de
	ld	de, _recipTable
	add	hl, de
	ld	(V(L_RP)), hl
	; output: block of the first row + slot
	ld	h, (V(L_RI))
	ld	l, GAP_BLOCK
	mlt	hl
	ld	de, .Lrows
	add	hl, de
	ld	a, (V(L_SLOT))
	add	a, l
	ld	l, a
	ld	(V(L_OUTP)), hl
	; xa, xb
	ld	hl, .Lpage
	ld	a, (V(L_K0))
	sub	a, P_FY - P_FX
	ld	l, a
	ld	de, (hl)		; xa
	ld	a, (V(L_K1))
	sub	a, P_FY - P_FX
	ld	l, a
	ld	hl, (hl)		; xb
	; the loop without clamping when the box is NOCLIP, or both ends are in 1 .. 254
	ld	c, 0
	ld	a, (V(L_CLIPPED))
	or	a, a
	jr	z, 3f
	push	de
	push	hl
	dec	hl
	ld	de, 254
	or	a, a
	sbc	hl, de
	jr	c, 1f
	ld	c, 1
1:	pop	hl
	pop	de
	push	hl
	push	de
	ex	de, hl
	dec	hl
	ld	de, 254
	or	a, a
	sbc	hl, de
	jr	c, 2f
	ld	c, 1
2:	pop	de
	pop	hl
3:	ld	(V(L_CLAMP)), c
	push	de			; xa
	or	a, a
	sbc	hl, de			; dx
	call	.Lslope			; de = integer part, c = fraction
	; X = xa + 1/2 = xa * 256 + 128 (xa is within +-SCREEN_COORD_MAX + 80)
	pop	hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	add	hl, hl
	ld	l, 128
	ld	a, (V(L_KK))
	or	a, (V(L_KK + 1))
	ld	a, 0			; the fraction
	call	nz, .Ljump		; X += k S

	ld	iy, (V(L_OUTP))
	ld	b, (V(L_COUNT))
	bit	0, (V(L_CLAMP))
	jr	nz, .Lclamp
.Lfast:
	ld	(iy + 0), h
	lea	iy, iy + GAP_BLOCK
	add	a, c
	adc	hl, de
	djnz	.Lfast
	ret

.Lclamp:
	push	af
	ld	(V(L_TMP)), hl
	ld	a, (V(L_TMP + 2))	; upper byte of the integer part
	or	a, a
	jr	z, 1f			; 0 .. 255: x
	rlca
	sbc	a, a			; negative: ff, else 0
	cpl				; negative: 0 (left of the viewport), else 255 (right of it)
	jr	2f
1:	ld	a, h
2:	ld	(iy + 0), a
	lea	iy, iy + GAP_BLOCK
	pop	af
	add	a, c
	adc	hl, de
	djnz	.Lclamp
	ret


; ---- slope: hl = dx (|dx| <= 2 SCREEN_COORD_MAX), L_RP -> recipTable[dy], L_DY1 = 0 if dy = 1.
; out: de = integer part, c = fraction (8 bits) of dx * 65536 / dy = (|dx| recipTable[dy]) >> 8
.Lslope:
	ld	a, (V(L_DY1))
	or	a, a
	jr	nz, 1f
	ld	de, 0			; dy = 1: a one row edge is only evaluated at its start
	ld	c, e
	ret
1:	xor	a, a
	ex	de, hl
	sbc	hl, hl
	sbc	hl, de			; -dx
	jp	p, 2f			; dx <= 0
	ex	de, hl
	jr	3f
2:	inc	a
3:	ld	(V(L_NEG)), a
	; hl = |dx|: l = a0, h = a1
	ld	b, l
	ld	a, h
	or	a, a
	jr	nz, .Lslope_wide
	; |dx| < 256: (a0 R) >> 8 = hi(a0 r0) + a0 r1 + (a0 r2 << 8), all in 16 bits
	ld	hl, (V(L_RP))
	ld	a, (hl)
	ld	d, b
	ld	e, a
	mlt	de
	ld	a, d			; hi(a0 r0)
	inc	hl
	ld	e, (hl)
	ld	d, b
	mlt	de			; a0 r1
	add	a, e
	ld	c, a			; the fraction
	ld	a, d
	adc	a, 0
	inc	hl
	ld	e, (hl)
	ld	d, b
	mlt	de			; a0 r2, upper byte 0
	add	a, e
	ld	e, a
	jr	nc, .Lslope_sign
	inc	d
	jr	.Lslope_sign
.Lslope_wide:
	; a1 = h (1 .. 3): hi(a0 r0) + a1 r0 + a0 r1 + ((a1 r1 + a0 r2) << 8) + (a1 r2 << 16)
	ld	c, l			; c = a0, b = a1
	ld	b, h
	ld	hl, (V(L_RP))
	ld	hl, (hl)
	ld	(V(L_R)), hl
	ld	a, (V(L_R))
	ld	d, c
	ld	e, a
	mlt	de			; a0 r0
	or	a, a
	sbc	hl, hl
	ld	l, d
	ld	d, b
	ld	e, a
	mlt	de
	add	hl, de			; + a1 r0
	ld	a, (V(L_R + 1))
	ld	d, c
	ld	e, a
	mlt	de
	add	hl, de			; + a0 r1: L, below 2^17
	ld	(V(L_TMP)), hl
	ld	a, l
	ld	(V(L_SFRAC)), a		; the fraction is the low byte of L
	ld	hl, (V(L_TMP + 1))	; L >> 8
	ld	a, (V(L_R + 1))
	ld	d, b
	ld	e, a
	mlt	de
	add	hl, de			; + a1 r1
	ld	a, (V(L_R + 2))
	ld	d, c
	ld	e, a
	mlt	de
	add	hl, de			; + a0 r2
	ld	d, b
	ld	e, a
	mlt	de			; a1 r2, at most 384
	ld	(V(L_TMP + 1)), de	; [3] = its upper byte, 0
	ld	(V(L_TMP)), 0
	ld	de, (V(L_TMP))		; a1 r2 << 8
	add	hl, de
	ex	de, hl
	ld	c, (V(L_SFRAC))
.Lslope_sign:
	ld	a, (V(L_NEG))
	or	a, a
	ret	z
	xor	a, a
	sub	a, c			; -fraction, borrow if it was not 0
	ld	c, a
	ld	hl, 0
	sbc	hl, de			; -integer - borrow
	ex	de, hl
	ret


; ---- X += k S, k = L_KK (1 .. 979, consumed): hl = integer part, a = fraction; de, c = S (kept)
.Ljump:
	push	de
	push	bc
	ld	b, a
1:	srl	(V(L_KK + 1))
	rr	(V(L_KK))
	jr	nc, 2f
	ld	a, b
	add	a, c
	ld	b, a
	adc	hl, de
2:	sla	c
	ex	de, hl
	adc	hl, hl
	ex	de, hl
	ld	a, (V(L_KK))
	or	a, (V(L_KK + 1))
	jr	nz, 1b
	ld	a, b
	pop	bc
	pop	de
	ret


; ==================================================================================================
	.section	.text._drawSpans,"ax",@progbits
	.global	_drawSpans
	.type	_drawSpans,@function
_drawSpans:
	push	ix
	ld	a, i
	push	af
	di
	ld	(.Lsave_sp), sp

	; ---- the background, row by row with push (SP walks each row backwards) ----
	ld	a, (.Lbg_pending)
	or	a, a
	jp	z, .Lspans
	xor	a, a
	ld	(.Lbg_pending), a
	ld	hl, (.Lfield)
	ld	de, SKY_COLOUR * 0x010101
	ld	a, CENTRE_Y
	.macro	BG_ROWS
1:
	.rept	BG_REM
	ld	(hl), e
	inc	hl
	.endr
	ld	bc, VIEW_W - (BG_REM)
	add	hl, bc
	ld	sp, hl
	.rept	(VIEW_W - (BG_REM)) / 3
	push	de
	.endr
	ld	bc, 320 - VIEW_X1 + VIEW_X0
	add	hl, bc
	dec	a
	jp	nz, 1b
	.endm
	BG_ROWS
	ld	de, GROUND_COLOUR * 0x010101
	ld	a, VIEW_H - CENTRE_Y
	BG_ROWS

	; ---- the spans ----
.Lspans:
	ld	sp, (.Lspanp)
	ld	ix, .Ldraw_page
	jp	.Ldraw_next

	.balign	256
.Ldraw_page:
	jp	.Ldraw_exit		; entry point 0: the end of the list
.Ldraw_stub_end:
	.if	256 - 2 * SPAN_LEN - (.Ldraw_stub_end - .Ldraw_page)
	.skip	256 - 2 * SPAN_LEN - (.Ldraw_stub_end - .Ldraw_page)
	.endif
	.rept	SPAN_LEN
	ld	(hl), a
	inc	hl
	.endr
.Ldraw_next:
	pop	hl			; address
	pop	de			; e = entry point, d = colour
	ld	a, d
	ld	ixl, e
	jp	(ix)
	.if	.Ldraw_next - .Ldraw_page != 256
	.error	"the unrolled loop must end exactly on the next page"
	.endif

.Ldraw_exit:
	ld	sp, (.Lsave_sp)
	ld	hl, SPAN_LIST_END - SPAN_SIZE
	ld	(.Lspanp), hl		; only the end record is left
	pop	af
	jp	po, 1f
	ei
1:	pop	ix
	ret


; ==================================================================================================
	.section	.rodata._fillScene,"a",@progbits
; per face in FACE_* bit order (+x -x +z -z top bottom), 8 bytes each: the page offsets
; P_X + 3 v of its 4 vertices (v = corner k at the top, k + 4 at the bottom) in clockwise screen
; order, and the page offset of its colour
.Lfaces:
	.byte	3 * 1, 3 * 3, 3 * 7, 3 * 5, P_MAT + MAT_XSIDE, 0, 0, 0	; +x: corners 1, 3
	.byte	3 * 2, 3 * 0, 3 * 4, 3 * 6, P_MAT + MAT_XSIDE, 0, 0, 0	; -x: 0, 2
	.byte	3 * 3, 3 * 2, 3 * 6, 3 * 7, P_MAT + MAT_ZSIDE, 0, 0, 0	; +z: 2, 3
	.byte	3 * 0, 3 * 1, 3 * 5, 3 * 4, P_MAT + MAT_ZSIDE, 0, 0, 0	; -z: 0, 1
	.byte	3 * 2, 3 * 3, 3 * 1, 3 * 0, P_MAT + MAT_TOP, 0, 0, 0	; top
	.byte	3 * 4, 3 * 5, 3 * 7, 3 * 6, P_MAT + MAT_BOTTOM, 0, 0, 0	; bottom


; ==================================================================================================
	.section	.bss._fillScene,"aw",@nobits
	.balign	256
.Lpage:		.zero	256		; vertices, colours, the face, locals (P_* and L_*)
	.balign	GAP_BLOCK
.Lrows:		.zero	GAP_BLOCK * VIEW_H
.Lfield:	.zero	3		; first viewport column of row 0 in the frame being drawn
.Lspanp:	.zero	3		; the span list: lowest record (the list grows down)
.Lsave_sp:	.zero	3
.Lrow_sp:	.zero	3
.Lbg_pending:	.zero	1		; drawSpans still has to fill the background
.Lboxp:		.zero	3
.Lnbox:		.zero	1
.Lopen:		.zero	1		; rows with an open interval

	.extern	_materials
	.extern	_cam_axis_corner
	.extern	_recipTable
	.extern	_waitForBufferScan
