// The PICO-8 font (pico8_font.h).
#include "pico8_font.h"

const uint8_t pico8_font_data[] = {
    #include "../font/myfont.inc"
};

const uint8_t *pico8Glyph(uint8_t c) {
    const fontlib_font_t *f = PICO8_FONT;
    const uint8_t *offsets = pico8_font_data + f->bitmaps + 2 * (c - f->first_glyph);
    // the glyph's rows start 2 bytes after the offset stored for it in this file (checked: the
    // splash drawn from these matches fontlib's own drawing of the same characters)
    return pico8_font_data + (offsets[0] | offsets[1] << 8) + 2;
}
