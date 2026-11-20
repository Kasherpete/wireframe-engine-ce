// The LCD (display.h): 4bpp setup, double buffering and the de-interlace copy.
#include <string.h>
#include <ti/screen.h>
#include <ti/getcsc.h>

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


// The loading screen: white, with a progress bar in the middle, in BUFFER_1 from displayLoading
// until the first frame. Palette entry 7 (PICO-8's warm white) is pure white meanwhile.
#define LOADING_BG    0x88      // white
#define LOADING_FG    0x77      // black
#define LOADING_WHITE 0xffff    // gfx_palette entry 7 while loading (1555: bit 15 is the low green bit)

// the bar, in logical pixels (each 2x2 LCD pixels): a 1 px outline, a 1 px gap, then the fill
#define BAR_W      100
#define BAR_H      8
#define BAR_X      (GFX_WIDTH_HALF - BAR_W / 2)
#define BAR_Y      (GFX_HEIGHT_HALF - BAR_H / 2)
#define BAR_FILL_X     (BAR_X + 2)
#define BAR_FILL_Y     (BAR_Y + 2)
#define BAR_FILL_W     (BAR_W - 4)
#define BAR_FILL_H     (BAR_H - 4)
_Static_assert(BAR_FILL_W * LOADING_FULL / 256 == 3 * LOADING_FULL / 8, "displayProgress scales by 3/8");
_Static_assert(BAR_X >= 0 && BAR_X + BAR_W <= GFX_LCD_WIDTH && BAR_Y >= 0 && BAR_Y + BAR_H <= GFX_LCD_HEIGHT,
               "the bar must lie on the screen");

static uint8_t started = 0;     // graphx is running (displayLoading), so errors must end it first
static uint8_t loading = 0;     // the loading screen is up (BUFFER_1 shown, the white palette)
static uint8_t bar_px = 0;      // fill drawn so far, in pixels

// fills logical pixels [x, x + w) x [y, y + h) of the frame at BUFFER_1, both LCD rows of each (so
// no de-interlace). Only with constant, on-screen rectangles (the asserts above)
static void loadingRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t colour) {
    uint8_t *row = (uint8_t *)BUFFER_1 + (uint24_t)y * (2 * GFX_LCD_WIDTH) + x;
    for (; h != 0; h--, row += 2 * GFX_LCD_WIDTH) {
        memset(row, colour, w);
        memset(row + GFX_LCD_WIDTH, colour, w);
    }
}


void displayLoading(void) {
    gfx_Begin();
    gfx_SetPalette(palette, sizeof palette, 0);
    gfx_palette[LOADING_FG & 0x0f] = LOADING_WHITE;
    gfx_SetClipRegion(SCREEN_OFFSET, 0, SCREEN_OFFSET + VIEWPORT_LENGTH, GFX_LCD_HEIGHT);

    // the background and the empty bar, then shown
    memset((void *)BUFFER_1, LOADING_BG, BUFFER_2 - BUFFER_1);
    loadingRect(BAR_X, BAR_Y, BAR_W, BAR_H, LOADING_FG);
    loadingRect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, LOADING_BG);
    setScreenBuffer(BUFFER_1);
    setDrawBuffer(BUFFER_1);
    lcd_Control = LCD_CONTROL_4BPP;
    started = loading = 1;
    bar_px = 0;
}


void displayProgress(uint24_t done) {
    if (!loading) {
        return;
    }
    if (done > LOADING_FULL) {
        done = LOADING_FULL;
    }
    const uint8_t px = (uint8_t)((done * 3) >> 3);     // BAR_FILL_W / LOADING_FULL
    if (px > bar_px) {
        loadingRect(BAR_FILL_X + bar_px, BAR_FILL_Y, px - bar_px, BAR_FILL_H, LOADING_FG);
        bar_px = px;
    }
}


void displayLoadingEnd(void) {
    if (!loading) {
        return;
    }
    // once the LCD has taken the first frame's base (swapBuffer), at the start of a refresh: the
    // whole refresh then has the game palette, and the loading screen never shows it
    waitForBufferScan();
    gfx_SetPalette(palette, sizeof palette, 0);
    loading = 0;
}


void displayInit(void) {
    if (!started) {
        displayLoading();
    }
    displayProgress(LOADING_INIT);

    // the first frame is drawn into BUFFER_2, so the loading screen stays up until it is done.
    // Every frame rewrites every viewport pixel (fill.S): the side bars are all there is to prepare
    memset((void *)BUFFER_2, SKY_COLOUR, BUFFER_2 - BUFFER_1);
    setDrawBuffer(BUFFER_2);
    fillSideBars();
    deInterlaceFullBuffer(BUFFER_2);
    drawing_buffer_2 = 1;
}


void displayError(const char *line1, const char *line2) {
    if (started) {
        gfx_End();
        started = loading = 0;
    }
    os_ClrHome();
    os_PutStrFull(line1);
    if (line2) {
        os_NewLine();
        os_PutStrFull(line2);
    }
    while (!os_GetCSC()) {
    }
}
