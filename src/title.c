// The title screen (title.h).
#include <keypadc.h>

#include "display.h"
#include "player.h"
#include "title.h"

#define BUTTONS    3
#define BUTTON_X   40           // logical pixels (160 x 120)
#define BUTTON_W   80
#define BUTTON_H   14
#define BUTTON_Y0  50
#define BUTTON_DY  20

#define TEXT_COLOUR      0x77   // white
#define SHADOW_COLOUR    0x00   // black
#define BUTTON_COLOUR    0x55   // grey
#define SELECTED_COLOUR  0xcc   // blue
#define NO_TEXT_BG       0xff   // the text background colour, made transparent (never drawn)

// 2^24 yaw units per turn and 32768 ticks per second: dt << 3 is one turn in 64 s
#define TURN_SHIFT  3

static const char *const labels[BUTTONS] = {"play", "settings", "quit"};
static uint8_t selected = 0;
static uint8_t keys_prev = 0;      // main waits for [2nd]/[enter] to be released first

#define KEY_UP      0x01
#define KEY_DOWN    0x02
#define KEY_CHOOSE  0x04

uint8_t titleUpdate(uint24_t dt) {
    player_yaw += dt << TURN_SHIFT;

    const uint8_t keys = ((kb_Data[7] & kb_Up) ? KEY_UP : 0) | ((kb_Data[7] & kb_Down) ? KEY_DOWN : 0)
        | (((kb_Data[1] & kb_2nd) || (kb_Data[6] & kb_Enter)) ? KEY_CHOOSE : 0);
    const uint8_t pressed = keys & ~keys_prev;
    const uint8_t released = keys_prev & ~keys;
    keys_prev = keys;

    if ((pressed & KEY_UP) && selected > 0) {
        selected--;
    }
    if ((pressed & KEY_DOWN) && selected < BUTTONS - 1) {
        selected++;
    }
    if (released & KEY_CHOOSE) {
        if (selected == 0) {
            return TITLE_PLAY;
        }
        if (selected == 2) {
            return TITLE_QUIT;
        }
        // settings: nothing yet
    }
    return TITLE_STAY;
}

static void printCentred(const char *s, int y) {
    gfx_PrintStringXY(s, GFX_WIDTH_HALF - (int)gfx_GetStringWidth(s) / 2, y);
}

void titleDraw(void) {
    // unclipped text (graphx clipped text ignores the scale): fixed strings, all inside 160 x 120
    gfx_SetTextBGColor(NO_TEXT_BG);
    gfx_SetTextTransparentColor(NO_TEXT_BG);

    gfx_SetTextScale(2, 2);
    gfx_SetTextFGColor(SHADOW_COLOUR);
    printCentred("ti-craft", 13);
    gfx_SetTextFGColor(TEXT_COLOUR);
    printCentred("ti-craft", 12);

    gfx_SetTextScale(1, 1);
    for (uint8_t i = 0; i < BUTTONS; i++) {
        const int y = BUTTON_Y0 + i * BUTTON_DY;
        gfx_SetColor(SHADOW_COLOUR);
        gfx_Rectangle(BUTTON_X, y, BUTTON_W, BUTTON_H);
        gfx_SetColor(i == selected ? SELECTED_COLOUR : BUTTON_COLOUR);
        gfx_FillRectangle(BUTTON_X + 1, y + 1, BUTTON_W - 2, BUTTON_H - 2);
        printCentred(labels[i], y + (BUTTON_H - 8) / 2);
    }
}
