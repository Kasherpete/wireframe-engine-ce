// The title screen (title.h).
#include <keypadc.h>

#include <sys/rtc.h>

#include "camera.h"
#include "display.h"
#include "hud.h"
#include "pico8_font.h"
#include "player.h"
#include "title.h"
#include "title_font.h"
#include "world.h"

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

// the screens
#define SCREEN_MAIN      0
#define SCREEN_SETTINGS  1
#define SCREEN_WORLDS    2

static const char *const labels[BUTTONS] = {"Play", "Settings", "Quit"};
static uint8_t screen = SCREEN_MAIN;
static uint8_t selected = 0;       // the button of the screen shown
static uint8_t keys_prev = 0;      // main waits for [2nd]/[enter] to be released first

#define KEY_UP      0x01
#define KEY_DOWN    0x02
#define KEY_LEFT    0x04
#define KEY_RIGHT   0x08
#define KEY_CHOOSE  0x10
#define KEY_BACK    0x20

// ---- settings ----
#define SET_RENDER   0
#define SET_FOG      1
#define SET_HUD      2
#define SET_DONE     3
#define SETTINGS     4
#define RENDER_MIN   8          // render distance steps, up to GRID_RANGE
#define RENDER_STEP  4
_Static_assert((GRID_RANGE - RENDER_MIN) % RENDER_STEP == 0, "the steps must reach GRID_RANGE");

static uint8_t fog_on = FOG_DIST < GRID_RANGE;

// render_dist and fog_dist from the settings: fog over the last third, so 16 at 24 (FOG_DIST)
static void applySettings(void) {
    fog_dist = fog_on ? render_dist - render_dist / 3 : 255;
}

// dir: +1 or -1 (left/right; wraps only with wrap, for [2nd])
static void changeSetting(uint8_t item, int8_t dir, uint8_t wrap) {
    if (item == SET_RENDER) {
        int24_t d = render_dist + dir * RENDER_STEP;
        if (d > GRID_RANGE) {
            d = wrap ? RENDER_MIN : GRID_RANGE;
        }
        if (d < RENDER_MIN) {
            d = RENDER_MIN;
        }
        render_dist = d;
    } else if (item == SET_FOG) {
        fog_on = !fog_on;
    } else if (item == SET_HUD) {
        hud_shown = !hud_shown;
    }
    applySettings();
}

// ---- the world list ----
#define LIST_ROWS  4            // world buttons shown at once
static uint8_t list_top = 0;    // the first one shown
static const char *list_error = NULL;
static uint8_t chosen_world = 0;

uint8_t titleWorld(void) {
    return chosen_world;
}

void titleError(const char *msg) {
    list_error = msg;
}

static void openScreen(uint8_t s, uint8_t sel) {
    screen = s;
    selected = sel;
    list_top = 0;
    list_error = NULL;
}

uint8_t titleUpdate(uint24_t dt) {
    player_yaw += dt << TURN_SHIFT;

    uint8_t keys = 0;
    if (kb_Data[7] & kb_Up) {
        keys |= KEY_UP;
    }
    if (kb_Data[7] & kb_Down) {
        keys |= KEY_DOWN;
    }
    if (kb_Data[7] & kb_Left) {
        keys |= KEY_LEFT;
    }
    if (kb_Data[7] & kb_Right) {
        keys |= KEY_RIGHT;
    }
    if ((kb_Data[1] & kb_2nd) || (kb_Data[6] & kb_Enter)) {
        keys |= KEY_CHOOSE;
    }
    if (kb_Data[6] & kb_Clear) {
        keys |= KEY_BACK;
    }
    const uint8_t pressed = keys & ~keys_prev;
    const uint8_t released = keys_prev & ~keys;
    keys_prev = keys;

    // the buttons of this screen
    uint8_t n = BUTTONS;
    if (screen == SCREEN_SETTINGS) {
        n = SETTINGS;
    } else if (screen == SCREEN_WORLDS) {
        n = world_count + 1;                    // the worlds, then back
    }
    const uint8_t old = selected;
    if ((pressed & KEY_UP) && selected > 0) {
        selected--;
    }
    if ((pressed & KEY_DOWN) && selected < n - 1) {
        selected++;
    }

    if (screen == SCREEN_MAIN) {
        if (pressed & KEY_BACK) {
            return TITLE_QUIT;
        }
        if (released & KEY_CHOOSE) {
            if (selected == 0) {
                // the open world first selected
                openScreen(SCREEN_WORLDS, world_current < world_count ? world_current : 0);
            } else if (selected == 1) {
                openScreen(SCREEN_SETTINGS, 0);
            } else {
                return TITLE_QUIT;
            }
        }
    } else if (screen == SCREEN_SETTINGS) {
        if (pressed & KEY_LEFT) {
            changeSetting(selected, -1, 0);
        }
        if (pressed & KEY_RIGHT) {
            changeSetting(selected, 1, 0);
        }
        if (released & KEY_CHOOSE) {
            if (selected == SET_DONE) {
                openScreen(SCREEN_MAIN, 1);
            } else {
                changeSetting(selected, 1, 1);
            }
        }
        if (pressed & KEY_BACK) {
            openScreen(SCREEN_MAIN, 1);
        }
    } else {
        if (selected != old) {
            list_error = NULL;
        }
        if (pressed & KEY_BACK) {
            openScreen(SCREEN_MAIN, 0);
        } else if (released & KEY_CHOOSE) {
            if (selected == world_count) {
                openScreen(SCREEN_MAIN, 0);
            } else {
                chosen_world = selected;
                return TITLE_PLAY;
            }
        }
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

// ---- the menus ----
#define MENU_HEAD_Y   4         // the heading (legacy font)
#define MENU_Y0       16        // the first button
#define MENU_DY       16
#define SETTINGS_X    30
#define SETTINGS_W    100
#define STAR_Y        84        // the star text: 4 PICO-8 lines
#define LINE_DY       7         // a PICO-8 line: 5 rows, the shadow, a blank row
#define LIST_X        20
#define LIST_W        120
#define INFO_Y        83        // 2 PICO-8 lines under the world list
#define BACK_Y        99
#define LABEL_MAX     24        // characters of a button label, NUL included
#define TEXT_MAX      32        // characters of a PICO-8 line, NUL included (128 px)

// a button with its label centred (fitted: see fitLabel)
static void drawButton(int x, int y, int w, const char *label, uint8_t sel) {
    gfx_SetColor(SHADOW_COLOUR);
    gfx_Rectangle(x, y, w, BUTTON_H);
    gfx_SetColor(sel ? SELECTED_COLOUR : BUTTON_COLOUR);
    gfx_FillRectangle(x + 1, y + 1, w - 2, BUTTON_H - 2);
    printShadowed(label, centredX(label), y + 3, TEXT_COLOUR);
}

// src (any bytes: a world name from a file) into dst for the legacy font: characters it lacks
// become '?', and it is cut to at most max_w pixels and LABEL_MAX - 1 characters, so centred it
// lies on the screen (the text is drawn unclipped)
static void fitLabel(char *dst, const char *src, uint8_t max_w) {
    uint8_t w = 0, n = 0;
    while (*src && n < LABEL_MAX - 1) {
        uint8_t c = (uint8_t)*src++;
        if (c < ' ' || c > '~') {
            c = '?';
        }
        w += legacy_spacing[c];
        if (w > max_w) {
            break;
        }
        dst[n++] = (char)c;
    }
    dst[n] = '\0';
}

// src into dst for the PICO-8 font: lowercase (its capitals), '?' for what it lacks, at most
// TEXT_MAX - 1 characters
static void pico8Text(char *dst, const char *src) {
    uint8_t n = 0;
    while (*src && n < TEXT_MAX - 1) {
        uint8_t c = (uint8_t)*src++;
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if (c < ' ' || c > '~') {
            c = '?';
        }
        dst[n++] = (char)c;
    }
    dst[n] = '\0';
}

static void pico8Centred(const char *s, uint8_t y, uint8_t colour) {
    pico8Shadowed(s, GFX_WIDTH_HALF - fontlib_GetStringWidth(s) / 2, y, colour);
}

// v (< 1000) in decimal at p, NUL terminated; returns the end
static char *putUInt(char *p, uint24_t v) {
    if (v >= 100) {
        *p++ = (char)('0' + v / 100);
    }
    if (v >= 10) {
        *p++ = (char)('0' + v / 10 % 10);
    }
    *p++ = (char)('0' + v % 10);
    *p = '\0';
    return p;
}

static char *putStr(char *p, const char *s) {
    while (*s) {
        *p++ = *s++;
    }
    *p = '\0';
    return p;
}

static void drawHeading(const char *s) {
    printShadowed(s, centredX(s), MENU_HEAD_Y, TITLE_COLOUR);
}

static void drawSettings(void) {
    drawHeading("Settings");
    char label[LABEL_MAX];
    for (uint8_t i = 0; i < SETTINGS; i++) {
        char *p = label;
        if (i == SET_RENDER) {
            p = putStr(p, "Render: ");
            putUInt(p, render_dist);
        } else if (i == SET_FOG) {
            p = putStr(p, "Fog: ");
            putStr(p, fog_on ? "On" : "Off");
        } else if (i == SET_HUD) {
            p = putStr(p, "FPS info: ");
            putStr(p, hud_shown ? "On" : "Off");
        } else {
            putStr(p, "Done");
        }
        drawButton(SETTINGS_X, MENU_Y0 + i * MENU_DY, SETTINGS_W, label, i == selected);
    }
    // PICO-8 text is lowercase (its capitals)
    pico8Centred("like it? no need to donate -", STAR_Y, TEXT_COLOUR);
    pico8Centred("just star it on github:", STAR_Y + LINE_DY, TEXT_COLOUR);
    pico8Centred("github.com/kasherpete/", STAR_Y + 2 * LINE_DY, SPLASH_COLOUR);
    pico8Centred("wireframe-engine-ce", STAR_Y + 3 * LINE_DY, SPLASH_COLOUR);
}

static void drawWorlds(void) {
    drawHeading("Select World");
    // the selected world in view
    if (selected < world_count) {
        if (selected < list_top) {
            list_top = selected;
        }
        if (selected >= list_top + LIST_ROWS) {
            list_top = selected - (LIST_ROWS - 1);
        }
    }
    char label[LABEL_MAX];
    for (uint8_t r = 0; r < LIST_ROWS && list_top + r < world_count; r++) {
        const world_entry_t *e = &world_list[list_top + r];
        fitLabel(label, e->name[0] ? e->name : e->appvar, LIST_W - 4);
        drawButton(LIST_X, MENU_Y0 + r * MENU_DY, LIST_W, label, list_top + r == selected);
    }
    // more above or below
    if (list_top > 0) {
        pico8Shadowed("^", LIST_X + LIST_W + 3, MENU_Y0 + 4, TEXT_COLOUR);
    }
    if (list_top + LIST_ROWS < world_count) {
        pico8Shadowed("v", LIST_X + LIST_W + 3, MENU_Y0 + (LIST_ROWS - 1) * MENU_DY + 4, TEXT_COLOUR);
    }

    // the selected world: its AppVar, its place in the list, when it was last played
    if (selected < world_count) {
        const world_entry_t *e = &world_list[selected];
        char text[TEXT_MAX];
        char *p = putStr(text, "appvar ");
        pico8Text(p, e->appvar);
        pico8Shadowed(text, LIST_X, INFO_Y, TEXT_COLOUR);
        p = putUInt(text, selected + 1);
        p = putStr(p, "/");
        putUInt(p, world_count);
        pico8Shadowed(text, LIST_X + LIST_W - fontlib_GetStringWidth(text), INFO_Y, TEXT_COLOUR);

        if (list_error) {
            pico8Text(text, list_error);
            pico8Shadowed(text, LIST_X, INFO_Y + LINE_DY, SPLASH_COLOUR);
        } else if (e->last_played.year == 0) {
            pico8Shadowed("never played", LIST_X, INFO_Y + LINE_DY, TEXT_COLOUR);
        } else {
            fontlib_SetForegroundColor(SHADOW_COLOUR);
            for (uint8_t pass = 0; pass < 2; pass++) {
                fontlib_SetCursorPosition(LIST_X + 1 - pass, INFO_Y + LINE_DY + 1 - pass);
                fontlib_DrawString("last played ");
                fontlib_DrawUInt(e->last_played.year, 4);
                fontlib_DrawGlyph('-');
                fontlib_DrawUInt(e->last_played.month, 2);
                fontlib_DrawGlyph('-');
                fontlib_DrawUInt(e->last_played.day, 2);
                fontlib_SetForegroundColor(TEXT_COLOUR);
            }
        }
    }
    drawButton(BUTTON_X, BACK_Y, BUTTON_W, "Back", selected == world_count);
}

static void drawMain(void) {
    if (!splash_ready) {
        splashRender();
    }
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
        drawButton(BUTTON_X, BUTTON_Y0 + i * BUTTON_DY, BUTTON_W, labels[i], i == selected);
    }
}

void titleDraw(void) {
    // unclipped text (graphx clipped text ignores the scale): fixed strings, or fitted (fitLabel),
    // all inside 160 x 120
    gfx_SetTextBGColor(NO_TEXT_BG);
    gfx_SetTextTransparentColor(NO_TEXT_BG);
    fontlib_SetFont(PICO8_FONT, 0);
    fontlib_SetWindow(0, 0, GFX_LCD_WIDTH, GFX_LCD_HEIGHT);
    fontlib_SetTransparency(true);

    if (screen == SCREEN_MAIN) {
        drawMain();
        return;
    }
    gfx_SetTextScale(1, 1);
    gfx_SetFontData(legacy_font);
    gfx_SetFontSpacing(legacy_spacing);
    if (screen == SCREEN_SETTINGS) {
        drawSettings();
    } else {
        drawWorlds();
    }
}
