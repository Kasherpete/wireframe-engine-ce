// The LCD: 4bpp half-resolution mode, the VRAM frames and double buffering.
//
// The LCD runs in 4bpp mode, but graphx draws as if it were 8bpp 320x240. One graphx row (320
// bytes) is then two LCD rows of 160 bytes, each byte two horizontal pixels: graphx sees a logical
// 160x120 screen whose pixels are 2x2 LCD pixels once each drawn row is copied to the row below it
// (deinterlaceFrame in fill.h, every frame). Colours are doubled nibbles (0x33 = palette entry 3
// twice).
//
// VRAM (0xD40000 .. 0xD657FF) holds four 38,400-byte frames. Never use graphx's buffer helpers
// (gfx_SetDrawBuffer, gfx_SwapDraw, gfx_Blit*, gfx_FillScreen, gfx_ZeroScreen): they assume
// 76,800-byte 8bpp buffers and would overwrite two frames or the span list.
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <graphx.h>
#include <sys/lcd.h>

// the logical screen (graphx defines these as 320x240)
#undef GFX_LCD_WIDTH
#undef GFX_LCD_HEIGHT
#define GFX_LCD_WIDTH    160
#define GFX_LCD_HEIGHT   120
#define GFX_WIDTH_HALF   80
#define GFX_HEIGHT_HALF  60

// the 3D viewport: the whole logical screen. A narrower one needs fillSideBars (display.c) and a
// changed VIEW_HALF_WIDTH in utils/gen_tables.py
#define VIEWPORT_LENGTH  160
#define SCREEN_OFFSET    ((GFX_LCD_WIDTH - VIEWPORT_LENGTH) / 2)   // first viewport column

// the four VRAM frames
#define BUFFER_1  0xD40000    // drawn and shown in turn
#define BUFFER_2  0xD49600
#define DBL_BUF1  0xD52C00    // fill.S span list (fill.h); between frames computeExposure's
                              // occupancy grid (chunks.c). Nothing else may use it
#define DBL_BUF2  0xD5C200    // chunk slots (chunks.c), and in the bench fill_ref.c's lists
#define DBL_BUF2_END 0xD65800 // the end of VRAM

#define draw_buffer_ctrl    lcd_LpBase   // where graphx draws
#define screen_buffer_ctrl  lcd_UpBase   // what the LCD scans out

// graphx, the palette, 4bpp mode, and the loading screen in BUFFER_1: white (palette entry 7
// pure white until displayLoadingEnd), an empty progress bar in the middle. First thing in main;
// errors before displayInit go through displayError. With a viewport narrower than the screen,
// BUFFER_1 would keep white side bars
void displayLoading(void);

// while running (between frames, after swapBuffer): the loading screen again, from an empty bar,
// shown until displayLoadingEnd after the next swapBuffer (switching worlds)
void displayReload(void);

// the progress bar, out of LOADING_FULL: filled to done (never back). Nothing unless the loading
// screen is up. The steps, by measured time (the CRC is ~80% of startup in the test world, ~95% in
// the dense one): up to LOADING_CRC during the CRC, then the values below
#define LOADING_FULL   256
#define LOADING_CRC    216     // worldOpen, block by block
#define LOADING_CHECK  232     // worldOpen done (range checks)
#define LOADING_INIT   240     // displayInit
void displayProgress(uint24_t done);

// after the first swapBuffer: waits until the LCD shows the first frame, then the game palette
void displayLoadingEnd(void);

// BUFFER_2 becomes the draw buffer (displayLoading first if it has not run)
void displayInit(void);

// ends graphx if it is running, prints line1 (and line2 unless NULL) on the home screen and waits
// for a key
void displayError(const char *line1, const char *line2);

// Waits until the LCD has started showing the frame swapBuffer last handed it, so the other one
// is free to draw into
void waitForBufferScan(void);

// shows the frame just drawn and makes the other one the draw buffer
void swapBuffer(void);

void setDrawBuffer(uint24_t buffer);
void setScreenBuffer(uint24_t buffer);

// copies each row of the frame at buffer to the LCD row below it, the whole width (at startup; per
// frame, deinterlaceFrame in fill.h skips the rows that need no copy)
void deInterlaceFullBuffer(uint24_t buffer);

// deinterlace.S: for each of the 120 graphx rows from row0, copies bytes 0 .. width-1 to the LCD
// row below (+160). width must be 1 .. 160
void deinterlaceRows(uint8_t *row0, uint24_t width);

#endif
