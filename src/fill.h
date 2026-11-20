// The fill rasterizer: fill.S (and its C reference, fill_ref.c). Defines only outside the
// __ASSEMBLER__ guard: fill.S includes this file.
#ifndef FILL_H
#define FILL_H

#include "engine.h"

#define GAP_MAX         11          // open x intervals per row (more: see fill.S, "Overflow")
#define GAP_BLOCK       32          // bytes of per-row state in fill.S

// The span list lives in the spare VRAM frame DBL_BUF1 (main.c checks they agree): 6-byte records
// pushed downwards from the end, 6399 of them. A face only starts when its worst case fits
// (GAP_MAX + 1 spans per row); otherwise the list is drawn early (see fillScene)
#define SPAN_LIST       0xD52C00
#define SPAN_LIST_END   0xD5C200
#define SPAN_SIZE       6

#ifndef __ASSEMBLER__
#include <stdint.h>

// Before the wait: front to back over boxes[] (sorted far to near), lists a span for every
// visible part of every box, for the frame drawn at base (draw_buffer_ctrl). Touches no frame
// buffer, unless the span list fills up: then it waits for the back buffer (waitForBufferScan)
// and draws the batch so far. Runs with interrupts disabled.
void fillScene(const projected_box_t *boxes, uint8_t n, uint24_t base);

// After waitForBufferScan: fills the background (sky above the horizon, ground below) unless
// boxes cover every column, then draws the spans fillScene listed, and empties the list.
// Runs with interrupts disabled.
void drawSpans(void);

// main.c
void waitForBufferScan(void);

#ifdef FILL_REFERENCE
// fill_ref.c: the C reference. Draws the same frame as fillScene + drawSpans
void fillSceneRef(const projected_box_t *boxes, uint8_t n, uint24_t base);
void drawSpansRef(void);
#endif
#endif

#endif
