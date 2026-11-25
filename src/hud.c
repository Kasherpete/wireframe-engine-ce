// The HUD (hud.h).
#include <fontlibc.h>

#include "hud.h"
#include "pico8_font.h"

#ifdef SHOW_HUD
uint8_t hud_shown = 1;
uint24_t prof_sorts, prof_loads, prof_chunks, prof_records, prof_occluded;
uint32_t prof_sort_cycles, prof_load_cycles, prof_cull_cycles, prof_proj_cycles, prof_fill_cycles, prof_occ_cycles;
#else
uint8_t hud_shown = 0;
#endif


#define CYCLES_PER_TENTH_MS 4800

void hudInit(void) {
    fontlib_SetFont(PICO8_FONT, 0);
    fontlib_SetTransparency(true);
    fontlib_SetWindowFullScreen();
    fontlib_SetColors(0x00, 0xff);

    timer_Set(2, 0);
    timer_Enable(2, TIMER_CPU, TIMER_NOINT, TIMER_UP);
}


// "<label>12.3ms" at logical (3, y). One divide, so ~2k cycles
static void drawMs(uint8_t y, const char *label, uint24_t cycles) {
    const uint24_t tenths = cycles / CYCLES_PER_TENTH_MS;
    fontlib_SetCursorPosition(3, y);
    fontlib_DrawString(label);
    fontlib_DrawUInt(tenths / 10, 1);
    fontlib_DrawGlyph('.');
    fontlib_DrawUInt(tenths % 10, 1);
    fontlib_DrawString("ms");
}


void hudDraw(uint8_t fps, uint24_t draw_cycles, uint24_t frame_cycles) {
    fontlib_SetColors(0x00, 0xff);      // the title screen draws in other colours
    fontlib_SetCursorPosition(3, 3);
    fontlib_DrawUInt(fps, 2);
    drawMs(11, "d ", draw_cycles);
    drawMs(19, "f ", frame_cycles);
}
