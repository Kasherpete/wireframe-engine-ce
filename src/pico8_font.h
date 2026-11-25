// The PICO-8 font (font/myfont.inc, a fontlibc font), shared by the HUD and the title screen.
// PICO-8 draws its normal capitals for the lowercase codes (its uppercase codes are a smaller
// set), so text for it is written in lowercase.
#ifndef PICO8_FONT_H
#define PICO8_FONT_H

#include <fontlibc.h>
#include <stdint.h>

#define PICO8_HEIGHT   5
#define PICO8_ADVANCE  4    // every glyph: 3 px, then a blank column

extern const uint8_t pico8_font_data[];
#define PICO8_FONT  ((const fontlib_font_t *)pico8_font_data)

// the 5 row bytes of glyph c (32-127), pixels in bits 6-4 (bit 7 is the blank left column)
const uint8_t *pico8Glyph(uint8_t c);

#endif
