// The title screen (title.h).
#include <keypadc.h>

#include <sys/rtc.h>

#include "display.h"
#include "pico8_font.h"
#include "player.h"
#include "title.h"
#include "title_font.h"

#define BUTTONS    3
#define BUTTON_X   40           // logical pixels (160 x 120)
#define BUTTON_W   80
#define BUTTON_H   14
#define BUTTON_Y0  50
#define BUTTON_DY  20

#define TITLE_COLOUR     0xdd   // greish blue
#define TEXT_COLOUR      0x77   // white
#define SHADOW_COLOUR    0x00   // black
#define BUTTON_COLOUR    0x55   // grey
#define SELECTED_COLOUR  0xcc   // blue
#define NO_TEXT_BG       0xff   // the text background colour, made transparent (never drawn)
#define EDITION_COLOUR   0x11   // dark blue
#define SPLASH_COLOUR    0xaa   // yellow
#define SPLASH_SHADOW    0x44   // brown

#define TITLE_Y      12         // "CALCRAFT", 16 rows at scale 2
#define EDITION_Y    30         // the edition box: 1 px border, 2 px padding, 5 rows of text, shadow
#define EDITION_H    11
#define CORNER_Y     (GFX_LCD_HEIGHT - 7)   // the bottom corner texts: 5 rows, the shadow, 1 px
#define CORNER_X     2

// PICO-8 text: lowercase shows as its capitals (pico8_font.h)
#define EDITION_TEXT  "ti84+ ce edition"
#define VERSION_TEXT  "v1.2.1"
#define SITE_TEXT     "kasher.dev"

// The splash: PICO-8 text rising 3 px per 8 to the right (about 20 degrees), centred here
#define SPLASH_CX         138
#define SPLASH_CY         26
#define SPLASH_MAX_CHARS  10               // longer ones are cut (wider would leave the screen)
#define SPLASH_W          (SPLASH_MAX_CHARS * PICO8_ADVANCE)    // the last glyph is 3 px, then its shadow
#define SPLASH_RISE(u)    (((u) * 3) >> 3)
#define SPLASH_H          (PICO8_HEIGHT + 1 + SPLASH_RISE(SPLASH_W - 1))
#define SPLASHES          10
static const char *const splashes[SPLASHES] = {
    "try basic!", "Opensource", "Made in C!", "Kasher.dev!", "now in 3d!", "no ads!", "Stay safe!", "Creeper?", "HelloWorld", "God cares!"
};
gfx_UninitedSprite(splash, SPLASH_W, SPLASH_H);
static uint8_t splash_ready = 0;

// 2^24 yaw units per turn and 32768 ticks per second: dt << 3 is one turn in 64 s
#define TURN_SHIFT  3

static const char *const labels[BUTTONS] = {"Play", "Settings", "Quit"};
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

// shadowed text, 1 px right and down (legacy Minecraft style); all inside 160 x 120 (unclipped)
static void printShadowed(const char *s, int x, int y, uint8_t colour) {
    gfx_SetTextFGColor(SHADOW_COLOUR);
    gfx_PrintStringXY(s, x + 1, y + 1);
    gfx_SetTextFGColor(colour);
    gfx_PrintStringXY(s, x, y);
}

// the same in PICO-8 (fontlib, transparent background)
static void pico8Shadowed(const char *s, uint24_t x, uint8_t y, uint8_t colour) {
    fontlib_SetForegroundColor(SHADOW_COLOUR);
    fontlib_SetCursorPosition(x + 1, y + 1);
    fontlib_DrawString(s);
    fontlib_SetForegroundColor(colour);
    fontlib_SetCursorPosition(x, y);
    fontlib_DrawString(s);
}

static int centredX(const char *s) {
    return GFX_WIDTH_HALF - (int)gfx_GetStringWidth(s) / 2;
}

// a pixel of the splash; (u, v) in unsheared text, so u < SPLASH_W and v < 6
static void splashPixel(uint8_t w, uint8_t u, uint8_t v, uint8_t colour) {
    const uint8_t y = v + SPLASH_RISE(w - 1) - SPLASH_RISE(u);
    splash->data[y * w + u] = colour;
}

// once: a random splash, sheared, with its shadow, into the sprite (transparent NO_TEXT_BG)
static void splashRender(void) {
    const char *s = splashes[rtc_Time() % SPLASHES];
    uint8_t n = 0;
    while (s[n] && n < SPLASH_MAX_CHARS) {
        n++;
    }
    const uint8_t w = n * PICO8_ADVANCE;
    splash->width = w;
    splash->height = PICO8_HEIGHT + 1 + SPLASH_RISE(w - 1);
    for (uint24_t i = 0; i < (uint24_t)w * splash->height; i++) {
        splash->data[i] = NO_TEXT_BG;
    }
    for (uint8_t pass = 0; pass < 2; pass++) {       // the shadow, then the text over it
        for (uint8_t i = 0; i < n; i++) {
            uint8_t c = (uint8_t)s[i];
            if (c >= 'A' && c <= 'Z') {
                c += 'a' - 'A';                         // PICO-8 capitals are at the lowercase codes
            }
            if (c < ' ' || c >= 128) {
                c = '?';
            }
            const uint8_t *g = pico8Glyph(c);
            for (uint8_t v = 0; v < PICO8_HEIGHT; v++) {
                for (uint8_t col = 0; col < 3; col++) {
                    if (g[v] & (0x40 >> col)) {
                        const uint8_t u = i * PICO8_ADVANCE + col;
                        if (pass == 0) {
                            splashPixel(w, u + 1, v + 1, SPLASH_SHADOW);
                        } else {
                            splashPixel(w, u, v, SPLASH_COLOUR);
                        }
                    }
                }
            }
        }
    }
    splash_ready = 1;
}

void titleDraw(void) {
    if (!splash_ready) {
        splashRender();
    }
    // unclipped text (graphx clipped text ignores the scale): fixed strings, all inside 160 x 120
    gfx_SetTextBGColor(NO_TEXT_BG);
    gfx_SetTextTransparentColor(NO_TEXT_BG);

    // the default font for the title, PICO-8 (fontlib) for the edition box and corners, the
    // legacy font for the buttons
    gfx_SetFontData(NULL);
    gfx_SetFontSpacing(NULL);
    gfx_SetTextScale(2, 2);
    gfx_SetTextFGColor(SHADOW_COLOUR);
    gfx_PrintStringXY("CALCRAFT", centredX("CALCRAFT"), TITLE_Y + 2);
    gfx_SetTextFGColor(TITLE_COLOUR);
    gfx_PrintStringXY("CALCRAFT", centredX("CALCRAFT"), TITLE_Y);

    gfx_SetTextScale(1, 1);
    fontlib_SetFont(PICO8_FONT, 0);
    fontlib_SetWindow(0, 0, GFX_LCD_WIDTH, GFX_LCD_HEIGHT);
    fontlib_SetTransparency(true);
    const uint24_t ew = fontlib_GetStringWidth(EDITION_TEXT) + 5;   // the advance leaves 1 px right
    const uint24_t ex = GFX_WIDTH_HALF - ew / 2;
    gfx_SetColor(SHADOW_COLOUR);
    gfx_Rectangle(ex, EDITION_Y, ew, EDITION_H);
    gfx_SetColor(EDITION_COLOUR);
    gfx_FillRectangle(ex + 1, EDITION_Y + 1, ew - 2, EDITION_H - 2);
    pico8Shadowed(EDITION_TEXT, ex + 3, EDITION_Y + 3, TEXT_COLOUR);
    // pico8Shadowed(SITE_TEXT, CORNER_X, CORNER_Y, TEXT_COLOUR);
    fontlib_SetForegroundColor(SHADOW_COLOUR);
    fontlib_SetCursorPosition(CORNER_X, CORNER_Y);
    fontlib_DrawString(SITE_TEXT);
    // pico8Shadowed(VERSION_TEXT, GFX_LCD_WIDTH - CORNER_X - 1 - fontlib_GetStringWidth(VERSION_TEXT), CORNER_Y,
    //               TEXT_COLOUR);

    fontlib_SetForegroundColor(SHADOW_COLOUR);
    fontlib_SetCursorPosition(GFX_LCD_WIDTH - CORNER_X - 1 - fontlib_GetStringWidth(VERSION_TEXT), CORNER_Y);
    fontlib_DrawString(VERSION_TEXT);

    gfx_SetTransparentColor(NO_TEXT_BG);
    gfx_TransparentSprite(splash, SPLASH_CX - splash->width / 2, SPLASH_CY - splash->height / 2);

    gfx_SetFontData(legacy_font);
    gfx_SetFontSpacing(legacy_spacing);
    for (uint8_t i = 0; i < BUTTONS; i++) {
        const int y = BUTTON_Y0 + i * BUTTON_DY;
        gfx_SetColor(SHADOW_COLOUR);
        gfx_Rectangle(BUTTON_X, y, BUTTON_W, BUTTON_H);
        gfx_SetColor(i == selected ? SELECTED_COLOUR : BUTTON_COLOUR);
        gfx_FillRectangle(BUTTON_X + 1, y + 1, BUTTON_W - 2, BUTTON_H - 2);
        printShadowed(labels[i], centredX(labels[i]), y + 3, TEXT_COLOUR);
    }
}
