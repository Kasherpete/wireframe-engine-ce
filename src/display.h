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
#define DBL_BUF1  0xD52C00    // fill.S span list (fill.h); nothing else may use it
#define DBL_BUF2  0xD5C200    // blank frame during displayInit, then scratch (chunks.c, fill_ref.c)

#define draw_buffer_ctrl    lcd_LpBase   // where graphx draws
#define screen_buffer_ctrl  lcd_UpBase   // what the LCD scans out

// graphx, the palette, 4bpp mode, both frames cleared to the sky colour. Before the window loads:
// chunks.c uses DBL_BUF2 as scratch afterwards
void displayInit(void);

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
