; fillScene / drawSpans: filled, shaded voxels with hidden surface removal.
;
;   void fillScene(const projected_box_t *boxes, uint24_t n, uint24_t base)
;   void drawSpans(void)
;
; fill_ref.c is the same algorithm in C and draws the same frame, pixel for pixel: verify changes
; here against it (CLAUDE.md, "Verify asm changes against C").
;
; ---- How it works ----
; Boxes are visited NEAR TO FAR (the painter order of chunks.h, boxes[] in order). Each viewport row keeps
; the list of its x intervals that nothing has covered yet. Every face of a box that faces the
; camera and is not covered by a neighbouring voxel (projectScene decides which) is a convex quad
; on screen; per row it covers [xl, xr). Only the parts of that inside open intervals are listed
; ("emitted") as spans, and then removed from the intervals. So a pixel hidden by a nearer box is
; never written, a face whose rows are all closed costs a scan of them, and once every row is
; closed the farther boxes are not looked at. A distant box (BOX_RECT, the level of detail of
; projectRecords) is one rectangle instead of its faces: the same rows, with the same [xl, xr) in each.
;
; The background (sky above the horizon, row CENTRE_Y, ground below) is filled with push, row by
; row, before the spans are drawn, but only in rows that are still open (or lost part of a row to
; an overflow, see below): a row that boxes closed is entirely written by spans.
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
; address), vertices are page offsets (ld l, n indexes them), colours are patched into
; immediates, and the row loop has a path of its own for each case of one open interval.
;
; ---- Per box (.Lbox) ----
; Its coordinates v[] are copied to the start of .Lpage. A face is an entry of a face table: its 4
; vertices in clockwise screen order, twice over (so a side never wraps), each as the page offsets
; of its x and its y. .Lfaces_gen holds them for the GEN layout (fixed); for the AXIS layout they
; depend on the direction faced, so fillScene builds .Lfaces_axis per frame from cam_axis_corner.
;
; ---- Per face (.Lface) ----
; 1. The top vertex t and the bottom one b (first on ties).
; 2. Rows [top y, bottom y), clipped to the viewport and trimmed to rows still open.
; 3. The right side runs clockwise from t, the left side counterclockwise. Each edge of a side
;    becomes one x per row (see Edges), stored straight into the row blocks. A side ends with the
;    first edge that reaches the last row, which b always does.
; 4. Every row: intersect [xl, xr) with each open interval, emit, update the list. A rounding
;    inversion (xl > xr) just means nothing is drawn.
;
; ---- Edges ----
; An edge from (x0, y0) down to (x1, y1) gives x = floor(x + 1/2) in each row, stepped with 16
; fractional bits: the integer part in a 24-bit register, the fraction in an 8-bit one, so a step
; is add a, c / adc hl, de. The slope dx * 65536 / dy is (|dx| recipTable[dy]) >> 8, with
; recipTable[dy] = round(2^24 / dy): mlt instead of a division (3 of them when |dx| < 256, 6
; otherwise), and none for a vertical edge (dx = 0, two of every side face) or a one row edge.
; It drifts by under 0.05 px over the longest possible edge. An edge entered at a later row first
; jumps there by shift and add. x outside 0 .. 255 is clamped, but only an edge with an end outside
; that range (in a box with one) runs the clamping loop: x lies between the ends. Anything from
; VIEW_W up means "right of the viewport", which the intervals clip anyway. Two faces sharing an
; edge compute it from the same two vertices in the same direction, so they meet without gaps.
;
; ---- Per-row block (GAP_BLOCK = 32 bytes, 32-aligned: offsets added to l never carry) ----
;   +0      number of open intervals n (0 .. GAP_MAX)
;   +1, +2  xl, xr of the face being drawn
;   +3      non-zero: an overflow dropped part of this row, so it needs the background anyway
;   +4      the open intervals: n pairs [left, right), sorted left to right, then the sentinel
;           [255, 255]. A face skips the intervals left of it and stops at the first one right
;           of it. A split moves the rest up one place, a closed interval moves them down
;   +29     address of the first viewport column of this row in the frame being drawn
; Overflow: a row holds at most GAP_MAX intervals. A face strictly inside an interval splits it;
; with no room left, the part right of the face is dropped from the list. It then shows the
; background even where a farther box should be. That needs GAP_MAX + 1 separate holes in a row.
;
; ---- Spans ----
; The row loop writes them with push (SP points into the list, interrupts are off), so the list
; grows down from SPAN_LIST_END. drawSpans pops them, lowest first:
;   short (len < LONG_MIN), 6 bytes: address of the first pixel; entry point, colour, unused.
;     drawSpans jumps into a 256-aligned unrolled loop (ld (hl), a / inc hl per pixel) at the entry
;     point, the low byte 256 - 2 len of the jump target (about 10 cycles per pixel).
;   long, 9 bytes: address just past the last pixel; stub, push entry, unused; the colour in all
;     3 bytes. The stub (entry point 4, 8 or 12 of the same page) writes the len % 3 pixels at the
;     end, then points SP at them and jumps into an unrolled run of push bc (3.3 cycles per pixel).
;     .Llong_tab gives stub and push entry by len, with no division.
; Entry point 0 (the first record, popped last) is the exit.
;
; Memory safety: every span lies inside an open interval, intervals only ever shrink from
; [0, VIEW_W), and rows are clipped to the viewport, so no span leaves the field's viewport.
; A face only starts when its worst case fits in the list (.Lrows_setup, SPAN_ROOM).
;
; ABI: arguments at sp+3 ...; IX preserved; AF BC DE HL IY clobbered.
; No apostrophes in comments: this file goes through the C preprocessor.

#include "engine.h"
#include "fill.h"

#if VIEW_X1 - VIEW_X0 > 255 || VIEW_H > 255
#error "x and y are bytes"
#endif
#if 4 + 2 * GAP_MAX + 2 > 29 || GAP_BLOCK != 32 || GAP_MAX < 2 || VIEW_X1 - VIEW_X0 != 160
#error "per-row block layout"
#endif
#if SPAN_SIZE != 6
#error "short span records are pushed as two 3-byte words"
#endif
#if VIEW_X0 != 0
#error "subtract VIEW_X0 from the vertex x (fill_ref.c does)"
#endif
#if BOX_OUT_LEN != 12 || BOX_AXIS != 1 || BOX_GEN != 2 || BOX_RECT != 3
#error "box layout"
#endif

#define B_N		0
#define B_XL		1
#define B_XR		2
#define B_BG		3
#define B_GAPS		4
#define B_ROWBASE	29

#define SPAN_LEN	120		/* longest short span: 2 len and the 4 stubs fit a page */
#define PUSH_MAX	53		/* push bc per long span: VIEW_W / 3 */
#if LONG_MIN > SPAN_LEN + 1 || LONG_MIN < 3
#error "LONG_MIN: shorter spans must fit the unrolled loop"
#endif
#if 3 * PUSH_MAX + 2 < VIEW_X1 - VIEW_X0
#error "PUSH_MAX"
#endif

; .Lpage (256-aligned), addressed as (ix + offset - 128) with ix = .Lpage + 128
#define P_BOX		0		/* the box coordinates v[] (36 bytes): vertex x and y are offsets here */
#define P_XA		40		/* .Lfaces_axis: offset of the x of corner k (4) */
#define P_YA		44		/* ... of the y of vertex v (8) */
#define P_MAT		64		/* material colours: top, x side, z side, bottom */
#define L_YMIN		128		/* the face: top and bottom y */
#define L_YMAX		131
#define L_T		134		/* 2 t: the top vertex, an offset into the face entry */
#define L_B		135		/* 2 b */
#define L_Y0		136		/* its rows [y0, y1), 24 bits */
#define L_Y1		139
#define L_R0		142		/* the same as bytes: first row, rows */
#define L_NROW		143
#define L_BLK0		144		/* block of the first row */
#define L_RC		147		/* side: the next row (24 bits) */
#define L_COUNT		150		/* edge: rows */
#define L_KK		151		/* k = row - y0 of the first row (24 bits) */
#define L_RP		154		/* -> recipTable[dy] */
#define L_NEG		157
#define L_SFRAC		158
#define L_R		159		/* recipTable[dy] */
#define L_TMP		162		/* 4 bytes, [3] stays 0: a 24-bit value at +0 read from +1 is >> 8 */
#define L_CLAMP		166		/* not 0: the box has a vertex x outside 0 .. 255 */
#define L_CLAMPE	167		/* bit 0: this edge has an end outside 0 .. 255 */
#define L_FACES		168		/* FACE_* bits of the box */
#define L_FTAB		169		/* its face table */
#define L_COL3		172		/* the colour of the face, 3 times (long spans) */
#define L_FACE		175		/* the face entry */
#define L_RECT		178		/* non-zero: a rectangle, columns [L_RXL, L_RXR) in every row */
#define L_RXL		179
#define L_RXR		180
#define L_OUTP		181		/* edge: block of its first row + slot */
#define P_LOD		192		/* lod_slot[64], written by sceneInit (scene.c) */
#define V(o)		ix + o - 128

#define VIEW_W		VIEW_X1 - VIEW_X0	/* no parentheses: see CLAUDE.md. Mind the precedence */
#define BG_REM		(VIEW_X1 - VIEW_X0) % 3	/* only in .rept or after another term */
/* list bytes per row a face may need: GAP_MAX + 1 spans, of which up to VIEW_W / LONG_MIN long */
#define SPAN_ROOM	SPAN_SIZE * GAP_MAX + SPAN_SIZE + 3 * ((VIEW_X1 - VIEW_X0) / LONG_MIN)

	.assume	ADL = 1

	.extern	_ground_colour3

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
	; In two passes, 24 bytes of code per row instead of 33: the row addresses, then the state
	; with 3-byte stores: bytes 0 .. 2 = n 1, xl, xr (anything); 3 .. 5 = no background, the
	; interval [0, VIEW_W); 6 .. 8 = the sentinel and a byte past it
	ld	ix, .Lrows
	ld	de, 320
	ld	b, VIEW_H
1:	ld	(ix + B_ROWBASE), hl
	add	hl, de
	lea	ix, ix + GAP_BLOCK
	djnz	1b
	push	iy
	ld	ix, .Lrows
	ld	bc, 1
	ld	de, 0xffffff
	ld	iy, 65536 * VIEW_X1 - 65536 * VIEW_X0	; VIEW_W << 16 (VIEW_W has no parentheses)
	ld	a, VIEW_H
2:	ld	(ix + B_N), bc
	ld	(ix + B_BG), iy
	ld	(ix + B_GAPS + 2), de
	lea	ix, ix + GAP_BLOCK
	dec	a
	jr	nz, 2b
	pop	iy
	ld	a, VIEW_H
	ld	(.Lopen), a
	ld	(.Lbg_pending), a	; non-zero: drawSpans fills the background
	; the span list: only the end record (entry point 0)
	ld	hl, SPAN_LIST_END - SPAN_SIZE
	ld	(.Lspanp), hl
	xor	a, a
	ld	(SPAN_LIST_END - SPAN_SIZE + 3), a

	; facing N/E/S/W: the face table of the AXIS layout
	ld	a, (_cam_cardinal)
	or	a, a
	push	iy
	call	nz, .Laxis_table
	pop	iy

	; boxes front to back: boxes[] is sorted near to far. .Lnbox counts the boxes left after
	; this one, so the loop runs while decrementing it does not borrow
	ld	hl, (iy + 9)
	ld	de, -1
	add	hl, de
	jr	nc, .Lboxes_done	; n = 0
	ld	(.Lnbox), hl
	ld	hl, (iy + 6)
	ld	(.Lboxp), hl
.Lbox_loop:
	ld	iy, (.Lboxp)
	lea	hl, iy + PB_SIZE
	ld	(.Lboxp), hl
	call	.Lbox
	ld	a, (.Lopen)
	or	a, a
	jr	z, .Lboxes_done		; every row is closed: the rest is hidden
	ld	hl, (.Lnbox)
	ld	de, -1
	add	hl, de
	ld	(.Lnbox), hl
	jr	c, .Lbox_loop
.Lboxes_done:
	pop	af
	jp	po, 1f
	ei
1:	pop	ix
	ret


; ---- .Lfaces_axis from cam_axis_corner. AXIS layout: nl nr nt nb fl fr ft fb, and edge e
; (near left, near right, far left, far right) is world corner k = cam_axis_corner[e]
.Laxis_table:
	ld	hl, .Lpage
	.macro	AXMAP e
	ld	a, (_cam_axis_corner + \e)
	add	a, P_XA
	ld	l, a
	ld	(hl), 3 * (\e & 1) + 12 * (\e >> 1)	; x: nl nr fl fr
	add	a, P_YA - P_XA
	ld	l, a
	ld	(hl), 6 + 12 * (\e >> 1)		; top: nt ft
	add	a, 4
	ld	l, a
	ld	(hl), 9 + 12 * (\e >> 1)		; bottom: nb fb
	.endm
	AXMAP	0
	AXMAP	1
	AXMAP	2
	AXMAP	3
	ld	de, .Lface_v
	ld	iy, .Lfaces_axis
	ld	b, 6 * 8
1:	ld	a, (de)			; vertex v
	inc	de
	ld	c, a
	and	a, 3
	add	a, P_XA
	ld	l, a
	ld	a, (hl)
	ld	(iy + 0), a
	ld	a, c
	add	a, P_YA
	ld	l, a
	ld	a, (hl)
	ld	(iy + 1), a
	lea	iy, iy + 2
	djnz	1b
	ret


; ==================================================================================================
; a = hl clamped to 0 .. 255 (|hl| is far below 32768, so h tells which side): a column
	.macro	CLAMP8
	ld	a, h
	or	a, a
	ld	a, l
	jr	z, 1f
	ld	a, h
	rla
	sbc	a, a			; negative: ff
	cpl				; negative: 0 (left of the viewport), else 255 (right of it)
1:
	.endm

; one box: iy -> projected_box_t. Clobbers every register
.Lbox:
	ld	ix, .Lpage + 128
	ld	a, (iy + PB_FACES)
	ld	(V(L_FACES)), a
	ld	a, (iy + PB_MAT)	; hl = materials[mat]
	add	a, a
	add	a, a
	or	a, a
	sbc	hl, hl
	ld	l, a
	ld	de, _materials
	add	hl, de
	ld	a, (iy + PB_KIND)
	cp	a, BOX_RECT
	jp	z, .Lrect
	ld	de, (hl)		; top, x side, z side
	ld	(V(P_MAT)), de
	inc	hl
	inc	hl
	inc	hl
	ld	a, (hl)
	ld	(V(P_MAT + 3)), a

	; ---- the coordinates, and the face table of their layout ----
	lea	hl, iy + PB_V
	ld	de, .Lpage + P_BOX
	ld	bc, 3 * BOX_OUT_LEN
	ldir
	ld	a, (iy + PB_KIND)
	; ---- the face table of their layout. Then whether a vertex x is outside 0 .. 255, which
	; makes the edges check their ends: x is inside when its bytes 1 and 2 are 0.
	; GEN: x at 0, 3, 6, 9. AXIS: 0, 3, 12, 15 ----
	ld	b, a
	ld	a, (V(P_BOX + 1))
	or	a, (V(P_BOX + 2))
	or	a, (V(P_BOX + 4))
	or	a, (V(P_BOX + 5))
	ld	hl, .Lfaces_gen
	djnz	1f
	ld	hl, .Lfaces_axis	; BOX_AXIS
	or	a, (V(P_BOX + 13))
	or	a, (V(P_BOX + 14))
	or	a, (V(P_BOX + 16))
	or	a, (V(P_BOX + 17))
	jr	2f
1:	or	a, (V(P_BOX + 7))
	or	a, (V(P_BOX + 8))
	or	a, (V(P_BOX + 10))
	or	a, (V(P_BOX + 11))
2:	ld	(V(L_FTAB)), hl
	ld	(V(L_CLAMP)), a

	; ---- each visible face ----
	.macro	FACE_IF f, slot
	bit	\f, (V(L_FACES))
	jr	z, 1f
	ld	iy, (V(L_FTAB))
	lea	iy, iy + 16 * \f
	ld	a, (V(P_MAT + \slot))
	call	.Lface
1:
	.endm
	FACE_IF	0, MAT_XSIDE
	FACE_IF	1, MAT_XSIDE
	FACE_IF	2, MAT_ZSIDE
	FACE_IF	3, MAT_ZSIDE
	FACE_IF	4, MAT_TOP
	FACE_IF	5, MAT_BOTTOM
	ret


; ---- a distant box drawn as one rectangle: iy -> box (v[] = left, right, top, bottom),
; hl -> materials[mat]. Colour: lod_slot[faces] picks the material colour
.Lrect:
	ld	a, (V(L_FACES))
	ld	de, .Lpage + P_LOD
	add	a, e
	ld	e, a
	ld	a, (de)			; MAT_* slot
	ld	de, 0
	ld	e, a
	add	hl, de
	ld	a, (hl)
	call	.Lcolour
	ld	hl, (iy + PB_V)
	CLAMP8
	ld	(V(L_RXL)), a
	ld	hl, (iy + PB_V + 3)
	CLAMP8
	ld	(V(L_RXR)), a
	ld	hl, (iy + PB_V + 6)
	ld	(V(L_YMIN)), hl
	ld	hl, (iy + PB_V + 9)
	ld	(V(L_YMAX)), hl
	ld	(V(L_RECT)), 1
	jp	.Lrows_setup


; ---- a = the colour: into the row loop, and 3 times for long spans
.Lcolour:
	ld	(.Lsm_col + 1), a
	ld	(V(L_COL3)), a
	ld	(V(L_COL3 + 1)), a
	ld	(V(L_COL3 + 2)), a
	ret


; ==================================================================================================
; one face: iy -> its face table entry, a = colour. ix = .Lpage + 128
.Lface:
	call	.Lcolour
	ld	(V(L_FACE)), iy

	; ---- 1. t = argmin y, b = argmax y (the first one on ties): de = ymin, bc = ymax ----
	ld	hl, .Lpage
	ld	l, (iy + 1)
	ld	de, (hl)
	push	de
	pop	bc
	xor	a, a
	ld	(V(L_T)), a
	ld	(V(L_B)), a
	ld	(V(L_RECT)), a
	.macro	MINMAX i
	ld	hl, .Lpage
	ld	l, (iy + 2 * \i + 1)	; the y of vertex i (2 bytes per vertex: x and y offsets)
	ld	hl, (hl)
	or	a, a
	sbc	hl, de			; y - ymin
	jp	p, 1f
	add	hl, de
	ex	de, hl			; a new top
	ld	(V(L_T)), 2 * \i
	jr	2f
1:	add	hl, de
	or	a, a
	sbc	hl, bc			; y - ymax
	jr	z, 2f
	jp	m, 2f
	add	hl, bc			; a new bottom
	push	hl
	pop	bc
	ld	(V(L_B)), 2 * \i
2:
	.endm
	MINMAX	1
	MINMAX	2
	MINMAX	3
	ld	(V(L_YMIN)), de
	ld	(V(L_YMAX)), bc

	; ---- the x extent of the face, [xmin, xmax] of its vertices clamped to 0 .. 255, into
	; L_RXL and L_RXR (used by rectangles only otherwise). Every span of the face lies in it: an
	; edge never steps past its end vertex, and xr <= the largest x ----
	ld	hl, .Lpage
	ld	l, (iy + 0)
	ld	de, (hl)
	push	de
	pop	bc
	.macro	XMINMAX i
	ld	hl, .Lpage
	ld	l, (iy + 2 * \i)	; the x of vertex i
	ld	hl, (hl)
	or	a, a
	sbc	hl, de			; x - xmin
	jp	p, 1f
	add	hl, de
	ex	de, hl			; a new least
	jr	2f
1:	add	hl, de
	or	a, a
	sbc	hl, bc			; x - xmax
	jp	m, 2f
	add	hl, bc
	push	hl
	pop	bc			; a new greatest
2:
	.endm
	XMINMAX	1
	XMINMAX	2
	XMINMAX	3
	ex	de, hl
	CLAMP8
	ld	(V(L_RXL)), a
	push	bc
	pop	hl
	CLAMP8
	ld	(V(L_RXR)), a
	sub	a, (V(L_RXL))
	ret	z			; no column (and x only ever rounds inside the extent)
	ret	c

; ---- 2. rows [max(ymin, 0), min(ymax, VIEW_H)), trimmed to rows where something can show
; (faces and rectangles) ----
.Lrows_setup:
	call	.Lrows_find
	ret	nc			; nothing of it can show
	jp	.Lrows_found

; The first row of [max(L_YMIN, 0), min(L_YMAX, VIEW_H)) with an open interval overlapping
; [L_RXL, L_RXR): the others cannot get a pixel of this face, rectangle or box. Found: carry, a =
; the row, hl = its block, b = the rows from it to the end of the range. None: no carry. Only the
; parts of the range with something to show then get their edges computed: about a third of the
; faces in the test world, and most in a dense one, are entirely hidden by nearer boxes
.Lrows_find:
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
	jp	m, .Lfind_none
	jr	z, .Lfind_none
	ld	b, l			; at most VIEW_H: bytes from here on
	ld	a, e			; first row
	ld	h, a
	ld	l, GAP_BLOCK
	mlt	hl
	ld	de, .Lrows
	add	hl, de
	ld	de, GAP_BLOCK

	; carry: an open interval of the row at hl overlaps [L_RXL, L_RXR). The intervals are sorted
	; and end with the sentinel [255, 255]: the first one with g1 > xl decides, overlapping when
	; its g0 < xr (never the sentinel: xr <= 255). a kept, c clobbered
	.macro	ROW_SEES
	ld	c, a
	ld	a, (hl)			; n
	or	a, a			; (no carry)
	jr	z, 9f			; closed
	push	hl
	ld	a, l
	add	a, B_GAPS		; blocks are 32-aligned: no carry into h
	ld	l, a			; g0 of the first interval
	ld	a, (V(L_RXL))
8:	inc	hl
	cp	a, (hl)			; xl - g1
	inc	hl
	jr	nc, 8b			; g1 <= xl: the next one
	dec	hl
	dec	hl
	ld	a, (hl)			; its g0
	pop	hl
	cp	a, (V(L_RXR))		; g0 - xr: borrow if they overlap
9:	ld	a, c
	.endm

1:	ROW_SEES
	ret	c
	add	hl, de
	inc	a
	djnz	1b
.Lfind_none:
	or	a, a
	ret

.Lrows_found:
	ld	(V(L_R0)), a
	ld	(V(L_BLK0)), hl
	ld	c, b
	dec	c
	ld	d, c
	ld	e, GAP_BLOCK
	mlt	de
	add	hl, de			; block of the last row
	ld	de, -GAP_BLOCK
3:	ROW_SEES
	jr	c, 4f
	add	hl, de
	djnz	3b			; ends at the latest at the first row, which shows
4:	ld	(V(L_NROW)), b

	; room in the span list for the worst case
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

	; ---- 3. xl and xr of every row ----
	ld	b, (V(L_NROW))
	ld	a, (V(L_RECT))
	or	a, a
	jr	z, .Lsides
	; a rectangle: the same in every row
	ld	iy, (V(L_BLK0))
	ld	de, (V(L_RXL))		; e = xl, d = xr
1:	ld	(iy + B_XL), e
	ld	(iy + B_XR), d
	lea	iy, iy + GAP_BLOCK
	djnz	1b
	jr	.Lrun

.Lsides:
	; the right side, clockwise from t (entries t .. t + 3), and the left side backwards from
	; the copy of t four entries on (t + 4 .. t + 1)
	ld	hl, (V(L_Y0))
	ld	(V(L_RC)), hl
	ld	iy, (V(L_FACE))
	ld	de, 0
	ld	e, (V(L_T))
	add	iy, de
	call	.Lchain_r
	ld	hl, (V(L_Y0))
	ld	(V(L_RC)), hl
	ld	iy, (V(L_FACE))
	ld	a, (V(L_T))
	add	a, 8
	ld	de, 0
	ld	e, a
	add	iy, de
	call	.Lchain_l

	; ---- 4. the rows ----
.Lrun:
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


; ---- the list is short of room: draw it now (needs the back buffer). Clobbers iy
.Lflush:
	call	_waitForBufferScan	; C: preserves ix
	jp	_drawSpans		; empties the list


; ==================================================================================================
; ---- the row loop. SP = span list pointer (no calls, no stack), ix = block of the first row,
; b = rows, c = colour, d and the upper byte of de = 0. Ends with jp .Lrows_done

; a span: e = first x, a = len (1 .. VIEW_W). Clobbers a, e, hl
	.macro	EMIT
	cp	a, LONG_MIN
	jr	nc, 7f
	add	a, a
	neg				; 256 - 2 len: the entry point
	ld	l, a
	ld	h, c			; the colour
	push	hl
	ld	hl, (ix + B_ROWBASE)
	add	hl, de
	push	hl			; the address
	jr	8f
7:	ld	hl, (.Lpage + L_COL3)
	push	hl			; the colour, 3 times
	ld	hl, .Llong_tab
	ld	l, a
	add	a, e
	ld	e, a			; the end
	ld	a, (hl)			; the stub for len % 3
	inc	h
	ld	h, (hl)			; the push entry for len / 3
	ld	l, a
	push	hl
	ld	hl, (ix + B_ROWBASE)
	add	hl, de
	push	hl			; the address past the end
8:
	.endm

.Lrow_next:
	lea	ix, ix + GAP_BLOCK
	dec	b
	jp	z, .Lrows_done
.Lrow:
	ld	a, (ix + B_N)
	dec	a
	jp	nz, .Lrow_multi
	; ---- the usual case: one open interval [g0, g1) ----
	ld	hl, (ix + B_XL)		; l = xl, h = xr
	ld	a, (ix + B_GAPS)
	cp	a, l
	jr	c, .Lr_left		; g0 < xl: [g0, xl) stays open
	ld	e, a			; xl <= g0: the span starts at g0
	ld	a, h
	cp	a, (ix + B_GAPS + 1)
	jr	nc, .Lr_all		; xr >= g1: the whole interval
	sub	a, e			; [g0, xr), and [xr, g1) stays open
	jr	z, .Lrow_next
	jr	c, .Lrow_next		; xr <= g0: no overlap
	ld	(ix + B_GAPS), h
	EMIT
	jr	.Lrow_next
.Lr_all:
	ld	(ix + B_N), 0		; closed
	ld	hl, .Lopen
	dec	(hl)
	ld	a, (ix + B_GAPS + 1)
	sub	a, e
	EMIT
	jp	.Lrow_next
.Lr_left:
	ld	e, l			; the span starts at xl
	ld	a, l
	cp	a, (ix + B_GAPS + 1)
	jp	nc, .Lrow_next		; xl >= g1: no overlap
	ld	a, h
	cp	a, (ix + B_GAPS + 1)
	jr	nc, .Lr_right		; xr >= g1: [xl, g1)
	sub	a, e			; [xl, xr) inside: a split
	jp	z, .Lrow_next
	jp	c, .Lrow_next		; rounding inversion
	ld	(ix + B_N), 2
	ld	l, (ix + B_GAPS + 1)
	ld	(ix + B_GAPS + 1), e	; [g0, xl)
	ld	(ix + B_GAPS + 2), h	; and [xr, g1)
	ld	(ix + B_GAPS + 3), l
	ld	(ix + B_GAPS + 4), 255	; then the sentinel
	ld	(ix + B_GAPS + 5), 255
	EMIT
	jp	.Lrow_next
.Lr_right:
	ld	a, (ix + B_GAPS + 1)
	ld	(ix + B_GAPS + 1), e	; [g0, xl) stays open
	sub	a, e
	EMIT
	jp	.Lrow_next

	; ---- no open interval, or several (sorted, then the sentinel) ----
.Lrow_multi:
	inc	a
	jp	z, .Lrow_next		; closed
	ld	hl, (ix + B_XL)		; l = xl, h = xr
	ld	a, l
	cp	a, h
	jp	nc, .Lrow_next		; xl >= xr: nothing (rounding inversion)
	; skip the intervals left of xl. The sentinel (g1 = 255 > xl) ends it
	lea	iy, ix + B_GAPS
1:	cp	a, (iy + 1)
	jr	c, .Lgap
	lea	iy, iy + 2
	jr	1b
.Lgap:
	; iy -> [g0, g1) with g1 > xl. Overlap unless g0 >= xr (always for the sentinel)
	ld	a, (ix + B_XR)
	cp	a, (iy + 0)
	jp	z, .Lrow_next
	jp	c, .Lrow_next
	ld	a, (ix + B_XL)
	cp	a, (iy + 0)
	jp	z, .Lg_from_g0
	jp	c, .Lg_from_g0
	; xl > g0: [g0, xl) stays open
	ld	e, a
	ld	a, (ix + B_XR)
	cp	a, (iy + 1)
	jr	nc, .Lg_to_g1
	; [xl, xr) inside the interval: a split into [g0, xl) and [xr, g1). Nothing else overlaps
	sub	a, e
	EMIT
	ld	a, (ix + B_N)
	cp	a, GAP_MAX
	jr	z, .Lgap_full
	inc	(ix + B_N)
	; make room after this interval: move the rest and the sentinel up one place
	push	bc
	push	de
	add	a, a
	add	a, B_GAPS + 1
	ld	l, a
	ld	a, ixl
	add	a, l
	lea	hl, ix + 0
	ld	l, a			; hl -> the last byte of the sentinel (blocks are 32-aligned)
	sub	a, iyl
	dec	a			; bytes from iy + 2 up to it
	ld	bc, 0
	ld	c, a
	push	hl
	pop	de
	inc	de
	inc	de
	lddr
	pop	de
	pop	bc
	ld	a, (iy + 1)
	ld	(iy + 3), a		; [xr, g1)
	ld	a, (ix + B_XR)
	ld	(iy + 2), a
	ld	a, (ix + B_XL)
	ld	(iy + 1), a		; [g0, xl)
	jp	.Lrow_next
.Lgap_full:
	ld	a, (ix + B_XL)		; no room: [xr, g1) keeps the background (see the header)
	ld	(iy + 1), a
	ld	(ix + B_BG), 1
	jp	.Lrow_next
.Lg_to_g1:
	; [xl, g1): the interval becomes [g0, xl), and the next one may overlap too
	ld	a, (iy + 1)
	ld	(iy + 1), e
	sub	a, e
	EMIT
	lea	iy, iy + 2
	jp	.Lgap
.Lg_from_g0:
	; xl <= g0: the span starts at g0
	ld	e, (iy + 0)
	ld	a, (ix + B_XR)
	cp	a, (iy + 1)
	jr	nc, .Lg_all
	ld	(iy + 0), a		; [xr, g1) stays open, and nothing right of it can overlap
	sub	a, e
	EMIT
	jp	.Lrow_next
.Lg_all:
	; the whole interval: remove it (move the rest and the sentinel down one place)
	ld	a, (iy + 1)
	sub	a, e
	EMIT
	push	bc
	push	de
	ld	a, (ix + B_N)
	add	a, a
	add	a, B_GAPS + 2
	ld	l, a
	ld	a, ixl
	add	a, l			; past the sentinel
	sub	a, iyl
	sub	a, 2			; bytes from iy + 2 up to there (at least the sentinel)
	ld	bc, 0
	ld	c, a
	lea	hl, iy + 2
	lea	de, iy + 0
	ldir
	pop	de
	pop	bc
	dec	(ix + B_N)
	jp	nz, .Lgap		; the next interval is now at iy
	ld	hl, .Lopen
	dec	(hl)			; closed
	jp	.Lrow_next


; ==================================================================================================
; ---- one side of the face: iy -> its first vertex in the face entry (2 bytes: x and y offsets),
; stepping to the next one (step 2) or the previous one (step -2). The x of each row from L_RC up
; to L_Y1 goes into block slot `slot`. The side ends with the first vertex at or below L_Y1: the
; bottom vertex b always is, so it ends within 3 edges. ix = .Lpage + 128
	.macro	CHAIN step, slot
1:	ld	hl, .Lpage
	ld	l, (iy + \step + 1)
	ld	hl, (hl)		; y of the next vertex
	ld	de, (V(L_Y1))
	or	a, a
	sbc	hl, de
	jp	p, 4f			; at or below y1: the last edge
	add	hl, de
	ld	de, (V(L_RC))
	or	a, a
	sbc	hl, de			; rows of this edge
	jr	z, 3f
	jp	m, 3f			; none (only after rounding made the face slightly concave)
	ld	(V(L_COUNT)), l
	add	hl, de
	ld	(V(L_RC)), hl		; where the next one starts
	call	5f
3:	lea	iy, iy + \step
	jr	1b
4:	ld	hl, (V(L_Y1))
	ld	de, (V(L_RC))
	or	a, a
	sbc	hl, de			; rows up to y1
	ret	z
	ld	(V(L_COUNT)), l
5:	; the edge: de = its first row (24 bits)
	ld	hl, .Lpage
	ld	l, (iy + 1)
	ld	bc, (hl)		; ya
	ex	de, hl
	ld	a, l			; the first row as a byte
	or	a, a
	sbc	hl, bc			; k = row - ya >= 0
	ld	(V(L_KK)), hl
	ld	h, a
	ld	l, GAP_BLOCK
	mlt	hl
	ld	de, .Lrows + \slot
	add	hl, de
	ld	(V(L_OUTP)), hl		; block of the first row + slot
	ld	hl, .Lpage
	ld	l, (iy + \step + 1)
	ld	hl, (hl)
	or	a, a
	sbc	hl, bc			; dy >= 1
	push	hl
	ld	hl, .Lpage
	ld	l, (iy + 0)
	ld	de, (hl)		; xa
	ld	l, (iy + \step)
	ld	hl, (hl)		; xb
	pop	bc
	jp	.Ledge
	.endm

.Lchain_r:
	CHAIN	2, B_XR
.Lchain_l:
	CHAIN	-2, B_XL


; ---- x of an edge for L_COUNT rows from L_OUTP: de = xa, hl = xb, bc = dy, L_KK = rows to skip
.Ledge:
	; clamp only if an end is outside 0 .. 255 (then bytes 1 or 2 are not 0)
	ld	a, (V(L_CLAMP))	; 0, or any other value
	or	a, a
	jr	z, 1f
	ld	(V(L_TMP)), hl
	ld	a, (V(L_TMP + 1))
	or	a, (V(L_TMP + 2))
	ld	(V(L_TMP)), de
	or	a, (V(L_TMP + 1))
	or	a, (V(L_TMP + 2))
	jr	z, 1f
	ld	a, 1
1:	ld	(V(L_CLAMPE)), a

	push	de			; xa
	or	a, a
	sbc	hl, de			; dx
	jr	z, .Lflat		; vertical: no slope
	ld	a, c
	dec	a
	or	a, b			; dy < 2^16: a = 0 if dy = 1
	jr	z, .Lflat		; one row: only evaluated at its start
	push	hl
	push	bc
	pop	hl
	add	hl, hl
	add	hl, bc
	ld	de, _recipTable
	add	hl, de
	ld	(V(L_RP)), hl
	pop	hl
	call	.Lslope			; de = integer part, c = fraction
	jr	2f
.Lflat:
	ld	de, 0
	ld	c, e
2:	; X = xa + 1/2 = xa * 256 + 128 (xa is within +-SCREEN_COORD_MAX + 80)
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

	ld	b, (V(L_COUNT))
	bit	0, (V(L_CLAMPE))
	ld	ix, (V(L_OUTP))
	jr	nz, .Lclamp
.Lfast:
	ld	(ix + 0), h
	lea	ix, ix + GAP_BLOCK
	add	a, c
	adc	hl, de
	djnz	.Lfast
	ld	ix, .Lpage + 128
	ret

.Lclamp:
	push	af
	ld	(.Lpage + L_TMP), hl
	ld	a, (.Lpage + L_TMP + 2)	; upper byte of the integer part
	or	a, a
	jr	z, 1f			; 0 .. 255: x
	rlca
	sbc	a, a			; negative: ff, else 0
	cpl				; negative: 0 (left of the viewport), else 255 (right of it)
	jr	2f
1:	ld	a, h
2:	ld	(ix + 0), a
	lea	ix, ix + GAP_BLOCK
	pop	af
	add	a, c
	adc	hl, de
	djnz	.Lclamp
	ld	ix, .Lpage + 128
	ret


; ---- slope: hl = dx (|dx| <= 2 SCREEN_COORD_MAX, not 0), L_RP -> recipTable[dy], dy >= 2.
; out: de = integer part, c = fraction (8 bits) of dx * 65536 / dy = (|dx| recipTable[dy]) >> 8
.Lslope:
	xor	a, a
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
	; a1 = h (1 .. 4): hi(a0 r0) + a1 r0 + a0 r1 + ((a1 r1 + a0 r2) << 8) + (a1 r2 << 16)
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
	mlt	de			; a1 r2, at most 512
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


; ---- X += k S, k = L_KK (1 .. 65535, consumed): hl = integer part, a = fraction; de, c = S (kept)
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

	; ---- the background, in rows still open: push, SP walking each row backwards ----
	ld	a, (.Lbg_pending)
	or	a, a
	jp	z, .Lspans
	xor	a, a
	ld	(.Lbg_pending), a
	ld	hl, (.Lfield)
	ld	ix, .Lrows
	ld	de, SKY_COLOUR * 0x010101
	ld	iyl, CENTRE_Y
	; A row nothing was drawn in (one open interval, [0, VIEW_W)) gets both of its LCD rows here,
	; 320 bytes by push (about 3.3 cycles per byte), and deinterlaceFrame skips it (ldir would cost
	; about 7.7 per byte). Other open rows get the background in their drawn LCD row only
	.macro	BG_ROWS
1:	ld	a, (ix + B_N)
	or	a, (ix + B_BG)
	jp	z, 2f			; spans cover the whole row
	dec	a			; (B_BG is only ever set with several intervals)
	jp	nz, 4f
	ld	a, (ix + B_GAPS + 1)
	sub	a, VIEW_W
	or	a, (ix + B_GAPS)
	jp	nz, 4f
	.rept	2			; untouched: 320 = 2 + 3 * 106 bytes
	ld	(hl), e
	inc	hl
	.endr
	ld	bc, 318
	add	hl, bc
	ld	sp, hl
	.rept	106
	push	de
	.endr
	jr	5f			; hl: the next row
4:	.rept	BG_REM
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
	jr	3f
2:	ld	bc, 320
3:	add	hl, bc
5:	lea	ix, ix + GAP_BLOCK
	dec	iyl
	jp	nz, 1b
	.endm
	BG_ROWS
	ld	de, (_ground_colour3)	; the world ground plane (colours.c)
	ld	iyl, VIEW_H - CENTRE_Y
	BG_ROWS

	; ---- the spans ----
.Lspans:
	ld	sp, (.Lspanp)
	ld	ix, .Ldraw_page
	ld	iy, .Lpush_page
	jp	.Ldraw_next

	.balign	256
.Ldraw_page:
	jp	.Ldraw_exit		; entry point 0: the end of the list
	jp	.Llong0			; 4, 8, 12: a long span with len % 3 = 0, 1, 2
	jp	.Llong1
	jp	.Llong2
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
	pop	de			; e = entry point, d = colour (long: push entry)
	ld	a, d
	ld	ixl, e
	jp	(ix)
	.if	.Ldraw_next - .Ldraw_page != 256
	.error	"the unrolled loop must end exactly on the next page"
	.endif

; long spans: hl = the address past the end, a = push entry, the colour x3 next in the list
.Llong2:
	pop	bc
	dec	hl
	ld	(hl), c
	dec	hl
	ld	(hl), c
	ld	(.Lpush_sp), sp
	ld	sp, hl
	ld	iyl, a
	jp	(iy)
.Llong1:
	pop	bc
	dec	hl
	ld	(hl), c
	ld	(.Lpush_sp), sp
	ld	sp, hl
	ld	iyl, a
	jp	(iy)
.Llong0:
	pop	bc
	ld	(.Lpush_sp), sp
	ld	sp, hl
	ld	iyl, a
	jp	(iy)

.Ldraw_exit:
	ld	sp, (.Lsave_sp)
	ld	hl, SPAN_LIST_END - SPAN_SIZE
	ld	(.Lspanp), hl		; only the end record is left
	pop	af
	jp	po, 1f
	ei
1:	pop	ix
	ret

	.balign	256
.Lpush_page:
	.rept	PUSH_MAX
	push	bc
	.endr
.Lpush_end:
	ld	sp, (.Lpush_sp)
	pop	hl
	pop	de
	ld	a, d
	ld	ixl, e
	jp	(ix)


; ==================================================================================================
;   void deinterlaceFrame(uint8_t force)
; After drawSpans: copies each viewport row of the frame fillScene drew to the LCD row below it
; (the 2 x 2 pixels, see CLAUDE.md), except the rows nothing was drawn in: drawSpans already filled
; both of their LCD rows with the background. The first force rows are copied anyway (the HUD
; draws text into them after drawSpans). About 1.3k cycles per row copied, 0.1k per row skipped
	.section	.text._deinterlaceFrame,"ax",@progbits
	.global	_deinterlaceFrame
	.type	_deinterlaceFrame,@function
_deinterlaceFrame:
	ld	iy, 0
	add	iy, sp
	ld	a, (iy + 3)
	push	ix
	ld	ix, .Lrows
	ld	iyl, VIEW_H
	ld	iyh, a			; rows still to copy regardless
1:	ld	a, iyh
	or	a, a
	jr	z, 2f
	dec	iyh
	jr	3f
2:	ld	a, (ix + B_N)
	dec	a
	jr	nz, 3f
	ld	a, (ix + B_GAPS + 1)
	sub	a, VIEW_W
	or	a, (ix + B_GAPS)
	jr	z, 4f			; untouched: done by drawSpans
3:	ld	hl, (ix + B_ROWBASE)
	ld	de, 160			; the LCD row below
	ex	de, hl
	add	hl, de
	ex	de, hl
	ld	bc, VIEW_W
	ldir
4:	lea	ix, ix + GAP_BLOCK
	dec	iyl
	jr	nz, 1b
	pop	ix
	ret


; ==================================================================================================
	.section	.rodata._fillScene,"a",@progbits
; per face in FACE_* bit order (+x -x +z -z top bottom): its 4 vertices in clockwise screen order
; (v = corner k at the top, k + 4 at the bottom), twice over
	.macro	FACE_V a, b, c, d
	.byte	\a, \b, \c, \d, \a, \b, \c, \d
	.endm
.Lface_v:
	FACE_V	1, 3, 7, 5		; +x: corners 1, 3
	FACE_V	2, 0, 4, 6		; -x: 0, 2
	FACE_V	3, 2, 6, 7		; +z: 2, 3
	FACE_V	0, 1, 5, 4		; -z: 0, 1
	FACE_V	2, 3, 1, 0		; top
	FACE_V	4, 5, 7, 6		; bottom

; the same as page offsets of x and y in the GEN layout: sx[4] st[4] sb[4]
	.macro	GV v
	.byte	3 * (\v & 3), 12 + 3 * \v
	.endm
	.macro	FACE_GEN a, b, c, d
	GV	\a
	GV	\b
	GV	\c
	GV	\d
	GV	\a
	GV	\b
	GV	\c
	GV	\d
	.endm
	.balign	16
.Lfaces_gen:
	FACE_GEN 1, 3, 7, 5
	FACE_GEN 2, 0, 4, 6
	FACE_GEN 3, 2, 6, 7
	FACE_GEN 0, 1, 5, 4
	FACE_GEN 2, 3, 1, 0
	FACE_GEN 4, 5, 7, 6

; a long span of len pixels: the stub for len % 3 (draw page entry 4, 8, 12), and on the next
; page the push entry for len / 3
	.balign	512			; so inc h never wraps
.Llong_tab:
	.set	i, 0
	.rept	256
	.byte	4 + 4 * (i - 3 * (i / 3))
	.set	i, i + 1
	.endr
	.set	i, 0
	.rept	256
	.if	i / 3 <= PUSH_MAX
	.byte	PUSH_MAX - i / 3
	.else
	.byte	0
	.endif
	.set	i, i + 1
	.endr


; ==================================================================================================
	.section	.bss._fillScene,"aw",@nobits
	.balign	256
.Lpage:		.zero	P_LOD		; the box, colours, the face, locals (P_* and L_*)
	.global	_lod_slot
_lod_slot:	.zero	64		; MAT_* slot of a rectangle by FACE_* bits (scene.c)
	.balign	GAP_BLOCK
.Lrows:		.zero	GAP_BLOCK * VIEW_H
	.balign	16
.Lfaces_axis:	.zero	6 * 16		; .Lfaces_gen for the AXIS layout (.Laxis_table)
.Lfield:	.zero	3		; first viewport column of row 0 in the frame being drawn
.Lspanp:	.zero	3		; the span list: lowest record (the list grows down)
.Lsave_sp:	.zero	3
.Lrow_sp:	.zero	3
.Lpush_sp:	.zero	3
.Lbg_pending:	.zero	1		; drawSpans still has to fill the background
.Lboxp:		.zero	3
.Lnbox:		.zero	3
.Lopen:		.zero	1		; rows with an open interval

	.extern	_materials
	.extern	_cam_axis_corner
	.extern	_cam_cardinal
	.extern	_recipTable
	.extern	_waitForBufferScan
