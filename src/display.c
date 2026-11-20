// The LCD (display.h): 4bpp setup, double buffering and the de-interlace copy.
#include <string.h>

#include "display.h"
#include "colours.h"
#include "engine.h"
#include "fill.h"

_Static_assert(SPAN_LIST == DBL_BUF1 && SPAN_LIST_END == DBL_BUF2, "fill.h span list is not DBL_BUF1");
_Static_assert(SCREEN_OFFSET == VIEW_X0 && SCREEN_OFFSET + VIEWPORT_LENGTH == VIEW_X1, "engine.h viewport out of sync");
_Static_assert(GFX_LCD_HEIGHT == VIEW_H && GFX_WIDTH_HALF == CENTRE_X && GFX_HEIGHT_HALF == CENTRE_Y, "engine.h screen out of sync");
_Static_assert(TABLE_VIEW_HALF_WIDTH * 2 == VIEWPORT_LENGTH, "gen_tables.py culls for a different viewport width");

#define LCD_CONTROL_4BPP  0x13925

// PL111 "next base update" interrupt flag: set when the LCD loads lcd_UpBase at the start of a
// frame. swapBuffer clears it, so once it is set again the new frame is on screen and the old one
// is free to draw into (the flag gfx_SwapDraw/gfx_Wait use). Polling lcd_UpBaseCurr for "stopped
// changing" instead returned early whenever a frame took less than the LCD idle period (about
// 5 ms), giving two swaps per refresh and drawing into the frame on screen.
#define LCD_INT_LNBU      (1 << 2)

static uint8_t drawing_buffer_2 = 0;   // the frame being drawn: 0 = BUFFER_1, 1 = BUFFER_2


void waitForBufferScan(void) {
    while (!(lcd_IntStatus & LCD_INT_LNBU)) {
    }
}


void setDrawBuffer(uint24_t buffer) {
    draw_buffer_ctrl = buffer;
}


void setScreenBuffer(uint24_t buffer) {
    screen_buffer_ctrl = buffer;
}


// Base first, then the acknowledge: the other way round, a refresh in between would set the flag
// for the old base and waitForBufferScan would return while that frame is still on screen. This
// way round the worst case is waiting one refresh too many
void swapBuffer(void) {
    setScreenBuffer(drawing_buffer_2 ? BUFFER_2 : BUFFER_1);
    lcd_IntAcknowledge = LCD_INT_LNBU;
    drawing_buffer_2 = !drawing_buffer_2;
    setDrawBuffer(drawing_buffer_2 ? BUFFER_2 : BUFFER_1);
}


void deInterlaceFullBuffer(uint24_t buffer) {
    deinterlaceRows((uint8_t *)buffer, GFX_LCD_WIDTH);
}


// the bars beside the viewport, in the current colour. None while the viewport is full width: a
// 0 wide _NoClip fill at x = 160 would not be safe
static void fillSideBars(void) {
#if SCREEN_OFFSET > 0
    gfx_FillRectangle_NoClip(0, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
    gfx_FillRectangle_NoClip(SCREEN_OFFSET + VIEWPORT_LENGTH, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
#endif
}


void displayInit(void) {
    gfx_Begin();
    gfx_SetPalette(palette, sizeof palette, 0);
    gfx_SetClipRegion(SCREEN_OFFSET, 0, SCREEN_OFFSET + VIEWPORT_LENGTH, GFX_LCD_HEIGHT);
    lcd_Control = LCD_CONTROL_4BPP;

    // show a blank frame while both buffers are prepared
    setScreenBuffer(DBL_BUF2);

    // both buffers start as background (every frame rewrites every viewport pixel, fill.S)
    memset((void *)BUFFER_1, SKY_COLOUR, 2 * (BUFFER_2 - BUFFER_1));
    setDrawBuffer(BUFFER_2);
    fillSideBars();
    deInterlaceFullBuffer(BUFFER_2);
    setDrawBuffer(BUFFER_1);
    fillSideBars();
    deInterlaceFullBuffer(BUFFER_1);

    setScreenBuffer(BUFFER_1);
    setDrawBuffer(BUFFER_1);
}
