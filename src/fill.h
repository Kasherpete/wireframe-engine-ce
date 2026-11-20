// The fill rasterizer: fill.S (and its C reference, fill_ref.c). Defines only outside the
// __ASSEMBLER__ guard: fill.S includes this file.
#ifndef FILL_H
#define FILL_H

#include "engine.h"

#define GAP_MAX         11          // open x intervals per row (more: see fill.S, "Overflow")
#define GAP_BLOCK       32          // bytes of per-row state in fill.S

// The span list lives in the spare VRAM frame DBL_BUF1 (display.c checks they agree): 6-byte
// records pushed downwards from the end, 6399 of them. A face only starts when its worst case fits
// (GAP_MAX + 1 spans per row); otherwise the list is drawn early (see fillScene)
#define SPAN_LIST       0xD52C00
#define SPAN_LIST_END   0xD5C200
#define SPAN_SIZE       6
// spans of at least LONG_MIN pixels are drawn with push (3.3 cycles per pixel instead of 10.4,
// but about 200 more per span): 9-byte records (see fill.S, "Spans")
#define LONG_MIN        28

#ifndef __ASSEMBLER__
#include <stdint.h>

// Before the wait: front to back over boxes[] (sorted near to far), lists a span for every
// visible part of every box, for the frame drawn at base (draw_buffer_ctrl). Touches no frame
// buffer, unless the span list fills up: then it waits for the back buffer (waitForBufferScan)
// and draws the batch so far. Runs with interrupts disabled.
void fillScene(const projected_box_t *boxes, uint24_t n, uint24_t base);

// After waitForBufferScan: fills the background (sky above the horizon, ground below) unless
// boxes cover every column, then draws the spans fillScene listed, and empties the list.
// Runs with interrupts disabled.
void drawSpans(void);

// After drawSpans: copies each drawn row to the LCD row below it (the 2 x 2 pixels), skipping the
// rows nothing was drawn in (drawSpans filled both of their LCD rows). The first force rows are
// copied anyway: for text drawn after drawSpans (the HUD)
void deinterlaceFrame(uint8_t force);

// The colour of a BOX_RECT box: lod_slot[faces] is the MAT_* entry of its material to use, by
// the FACE_* bits projectScene kept (for a BOX_RECT, at most one side). sceneInit (scene.c) fills it
extern uint8_t lod_slot[64];

// display.c
void waitForBufferScan(void);
// colours.c: the colour byte of the ground below the horizon, 3 times (the world's ground plane),
// and the ground half fogged (make fog)
extern uint24_t ground_colour3, bg_dither3;
// The background rows, top to bottom: sky colour (the sky, then the ground beyond the far plane
// when fogged), bg_dither3, ground_colour3. They add up to VIEW_H; the first is at least CENTRE_Y.
// Without fog {CENTRE_Y, 0, VIEW_H - CENTRE_Y}; with it setupCamera (camera.c) sets them per frame
extern uint8_t bg_rows[3];

#ifdef FILL_REFERENCE
// fill_ref.c: the C reference. Draws the same frame as fillScene + drawSpans
void fillSceneRef(const projected_box_t *boxes, uint24_t n, uint24_t base);
void drawSpansRef(void);
#endif
#endif

#endif
