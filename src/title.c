// The title screen (title.h).
#include <keypadc.h>
#include <string.h>

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
#define MARKED_COLOUR    0xdd   // the world the Rename button acts on, while it has the focus
#define TEXT_BG_COLOUR   0x00   // black, the box behind PICO-8 text (reads over the world)
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
// the text 1 px in from every side of a sheared background band (the last glyph is 3 px, then its
// shadow column, then 1 px of band); the band is PICO8_HEIGHT + 3 rows: 1 px, text, shadow, 1 px
#define SPLASH_W          (SPLASH_MAX_CHARS * PICO8_ADVANCE + 2)
#define SPLASH_BAND       (PICO8_HEIGHT + 3)
#define SPLASH_RISE(u)    (((u) * 3) >> 3)
#define SPLASH_H          (SPLASH_BAND + SPLASH_RISE(SPLASH_W - 1))
#define SPLASHES          9
static const char *const splashes[SPLASHES] = {
    "try basic!", "Opensource", "Made in C!", "Kasher.dev!", "now in 3d!", "no ads!", "Stay safe!", "Creeper?", "HelloWorld"
};
gfx_UninitedSprite(splash, SPLASH_W, SPLASH_H);
static uint8_t splash_ready = 0;

// 2^24 yaw units per turn and 32768 ticks per second: dt << 3 is one turn in 64 s
#define TURN_SHIFT  3

// the screens
#define SCREEN_MAIN      0
#define SCREEN_SETTINGS  1
#define SCREEN_WORLDS    2
#define SCREEN_NAME      3      // typing a world name (rename, or a new world)
#define SCREEN_NOWORLD   4      // no world could be opened: where to get one, Continue

static const char *const labels[BUTTONS] = {"Play", "Settings", "Quit"};
static uint8_t screen = SCREEN_MAIN;
static uint8_t selected = 0;       // the button of the screen shown
static uint8_t armed = 0;          // [2nd]/[enter] went down in the menu: its release chooses
static const char *noworld_reason = NULL;

// the pause menu: SCREEN_MAIN shows these instead (Settings at the same place, 1)
#define PAUSE_BUTTONS  4
#define PAUSE_RESUME   0
#define PAUSE_SETTINGS 1
#define PAUSE_SAVE     2
#define PAUSE_QUIT     3
static const char *const pause_labels[PAUSE_BUTTONS] = {
    "Back to Game", "Settings", "Save & Quit", "Quit Without Saving"
};
static uint8_t paused = 0;
// kb_Data[1 .. 7] at the last titlePoll, and the keys pressed and released since titleUpdate took
// them (main waits for [2nd]/[enter] to be released first)
#define KEY_GROUPS  7
static uint8_t key_last[KEY_GROUPS], key_down[KEY_GROUPS], key_up[KEY_GROUPS];

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
// selected: a world (< world_count), or a button of the bottom row, world_count + BOTTOM_*
#define LIST_ROWS  4            // world buttons shown at once
#define BOTTOM_NEW     0
#define BOTTOM_RENAME  1
#define BOTTOM_BACK    2
#define BOTTOMS        3
static uint8_t list_top = 0;    // the first one shown
static uint8_t list_sel = 0;    // the world last selected (Rename renames it, the info shows it)
static uint8_t bottom = BOTTOM_NEW;    // the bottom button last selected
static const char *list_error = NULL;
static uint8_t chosen_world = 0;

// ---- the name screen ----
// Keys type like the OS with alpha lock: [math] A, [apps] B ... [0] space, [.] ':', [(-)] '?'.
// [alpha] switches capitals / small letters / digits, [del] erases, [enter] keeps it, [clear] cancels
#define MODE_UPPER  0
#define MODE_LOWER  1
#define MODE_DIGIT  2
static char name_buf[WORLD_NAME_LEN];
static uint8_t name_len;
static uint8_t name_mode;
static uint8_t naming_new;      // a new world, not a rename of list_sel

// by kb_Data group 2 .. 6 and bit (keypadc.h)
static const char key_letters[5][8] = {
    {0,   'X', 'S', 'N', 'I', 'D', 'A', 0},
    {' ', 'Y', 'T', 'O', 'J', 'E', 'B', 0},
    {':', 'Z', 'U', 'P', 'K', 'F', 'C', 0},
    {'?', 0,   'V', 'Q', 'L', 'G', 0,   0},
    {0,   0,   'W', 'R', 'M', 'H', 0,   0},
};
static const char key_digits[5][8] = {
    {0,   0,   0,   0,   0,   0,   0,   0},
    {'0', '1', '4', '7', ',', 0,   0,   0},
    {'.', '2', '5', '8', '(', 0,   0,   0},
    {'-', '3', '6', '9', ')', 0,   0,   0},
    {0,   '+', '-', '*', '/', '^', 0,   0},
};

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

// the world list with world i (if listed) selected
static void openWorlds(uint8_t i) {
    openScreen(SCREEN_WORLDS, 0);
    list_sel = i < world_count ? i : 0;
    selected = list_sel;
}

// the name screen, starting from name (any bytes: kept printable)
static void openName(uint8_t is_new, const char *name) {
    screen = SCREEN_NAME;
    naming_new = is_new;
    name_mode = MODE_UPPER;
    name_len = 0;
    while (name[name_len] && name_len < WORLD_NAME_LEN - 1) {
        const uint8_t c = (uint8_t)name[name_len];
        name_buf[name_len++] = c >= ' ' && c <= '~' ? (char)c : '?';
    }
    name_buf[name_len] = '\0';
}

void titleNoWorld(const char *reason) {
    screen = SCREEN_NOWORLD;
    selected = 0;
    noworld_reason = reason;
}

void titlePause(void) {
    paused = 1;
    screen = SCREEN_MAIN;
    selected = PAUSE_RESUME;
    armed = 0;
    for (uint8_t g = 0; g < KEY_GROUPS; g++) {
        key_last[g] = kb_Data[g + 1];
        key_down[g] = key_up[g] = 0;
    }
}

void titlePoll(void) {
    for (uint8_t g = 0; g < KEY_GROUPS; g++) {
        const uint8_t now = kb_Data[g + 1];
        key_down[g] |= now & ~key_last[g];
        key_up[g] |= key_last[g] & ~now;
        key_last[g] = now;
    }
}

// the name screen's keys (down, up: by kb_Data group - 1); [clear] is handled by titleUpdate
static void nameUpdate(const uint8_t *down, const uint8_t *up) {
    for (uint8_t g = 0; g < 6; g++) {
        const uint8_t pressed = down[g];
        const uint8_t released = up[g];
        if (g == 0) {
            if ((pressed & kb_Del) && name_len > 0) {
                name_buf[--name_len] = '\0';
            }
            continue;
        }
        if (g == 1 && (pressed & kb_Alpha)) {
            name_mode = name_mode == MODE_DIGIT ? MODE_UPPER : name_mode + 1;
        }
        if (g == 5 && (released & kb_Enter) && name_len > 0) {
            // on release, so the world list does not take it as a choice
            char appvar[9];
            uint8_t err;
            if (naming_new) {
                err = worldCreate(name_buf, appvar);
            } else {
                strcpy(appvar, world_list[list_sel].appvar);
                err = worldRename(list_sel, name_buf);
            }
            openWorlds(worldFind(appvar));
            if (err != WORLD_OK) {
                list_error = worldError(err);
            }
            return;
        }
        for (uint8_t b = 0; b < 8; b++) {
            if (!(pressed & (1 << b))) {
                continue;
            }
            char c = name_mode == MODE_DIGIT ? key_digits[g - 1][b] : key_letters[g - 1][b];
            if (c >= 'A' && c <= 'Z' && name_mode == MODE_LOWER) {
                c += 'a' - 'A';
            }
            if (c && name_len < WORLD_NAME_LEN - 1) {
                name_buf[name_len++] = c;
                name_buf[name_len] = '\0';
            }
        }
    }
}

// the world list's keys
static uint8_t worldsUpdate(uint8_t pressed, uint8_t released) {
    if (list_sel >= world_count) {
        list_sel = 0;
    }
    if (selected < world_count) {
        if ((pressed & KEY_UP) && selected > 0) {
            selected--;
        } else if (pressed & KEY_DOWN) {
            selected = selected + 1 < world_count ? selected + 1 : world_count + bottom;
        }
        if (selected < world_count && selected != list_sel) {
            list_sel = selected;
            list_error = NULL;
        }
    } else {
        if ((pressed & KEY_LEFT) && bottom > 0) {
            bottom--;
        }
        if ((pressed & KEY_RIGHT) && bottom < BOTTOMS - 1) {
            bottom++;
        }
        selected = world_count + bottom;
        if ((pressed & KEY_UP) && world_count > 0) {
            selected = list_sel;
        }
    }

    if (pressed & KEY_BACK) {
        openScreen(SCREEN_MAIN, 0);
    } else if (released & KEY_CHOOSE) {
        if (selected < world_count) {
            chosen_world = selected;
            return TITLE_PLAY;
        }
        if (bottom == BOTTOM_BACK) {
            openScreen(SCREEN_MAIN, 0);
        } else if (bottom == BOTTOM_NEW) {
            if (world_count >= WORLD_LIST_MAX) {
                list_error = "Too many worlds (16 at most)";
            } else {
                openName(1, "New World");
            }
        } else if (world_count > 0) {
            const world_entry_t *e = &world_list[list_sel];
            openName(0, e->name[0] ? e->name : e->appvar);
        }
    }
    return TITLE_STAY;
}

uint8_t titleUpdate(uint24_t dt) {
    if (!paused) {
        player_yaw += dt << TURN_SHIFT;     // the pause menu keeps the world as it was
    }

    // the keys pressed and released since the last call (a tap shorter than a frame included)
    titlePoll();
    uint8_t down[KEY_GROUPS], up[KEY_GROUPS];
    for (uint8_t g = 0; g < KEY_GROUPS; g++) {
        down[g] = key_down[g];
        up[g] = key_up[g];
        key_down[g] = key_up[g] = 0;
    }
    uint8_t pressed = 0, released = 0;
    for (uint8_t pass = 0; pass < 2; pass++) {
        const uint8_t *k = pass ? up : down;
        uint8_t keys = 0;
        if (k[6] & kb_Up) {
            keys |= KEY_UP;
        }
        if (k[6] & kb_Down) {
            keys |= KEY_DOWN;
        }
        if (k[6] & kb_Left) {
            keys |= KEY_LEFT;
        }
        if (k[6] & kb_Right) {
            keys |= KEY_RIGHT;
        }
        if ((k[0] & kb_2nd) || (k[5] & kb_Enter)) {
            keys |= KEY_CHOOSE;
        }
        if (k[5] & kb_Clear) {
            keys |= KEY_BACK;
        }
        if (pass) {
            released = keys;
        } else {
            pressed = keys;
        }
    }
    // a release chooses only after a press seen here (not a key held when the menu opened, as
    // [2nd] for a jump just before the pause)
    if (pressed & KEY_CHOOSE) {
        armed = 1;
    }
    if (released & KEY_CHOOSE) {
        if (!armed) {
            released &= ~KEY_CHOOSE;
        }
        armed = 0;
    }

    if (screen == SCREEN_NOWORLD) {
        if ((released & KEY_CHOOSE) || (pressed & KEY_BACK)) {
            openScreen(SCREEN_MAIN, 0);
        }
        return TITLE_STAY;
    }

    if (screen == SCREEN_NAME) {
        if (pressed & KEY_BACK) {
            openWorlds(list_sel);
        } else {
            nameUpdate(down, up);
        }
        return TITLE_STAY;
    }
    if (screen == SCREEN_WORLDS) {
        return worldsUpdate(pressed, released);
    }

    // the buttons of this screen
    const uint8_t n = screen == SCREEN_SETTINGS ? SETTINGS : paused ? PAUSE_BUTTONS : BUTTONS;
    if ((pressed & KEY_UP) && selected > 0) {
        selected--;
    }
    if ((pressed & KEY_DOWN) && selected < n - 1) {
        selected++;
    }

    if (screen == SCREEN_MAIN && paused) {
        uint8_t choice = TITLE_STAY;
        if (pressed & KEY_BACK) {
            choice = TITLE_RESUME;
        } else if (released & KEY_CHOOSE) {
            if (selected == PAUSE_SETTINGS) {
                openScreen(SCREEN_SETTINGS, 0);
            } else {
                choice = selected == PAUSE_RESUME ? TITLE_RESUME :
                         selected == PAUSE_SAVE ? TITLE_SAVE_QUIT : TITLE_QUIT;
            }
        }
        if (choice == TITLE_RESUME) {
            paused = 0;
        }
        return choice;
    } else if (screen == SCREEN_MAIN) {
        if (pressed & KEY_BACK) {
            return TITLE_QUIT;
        }
        if (released & KEY_CHOOSE) {
            if (selected == 0) {
                openWorlds(world_current);          // the open world first selected
            } else if (selected == 1) {
                openScreen(SCREEN_SETTINGS, 0);
            } else {
                return TITLE_QUIT;
            }
        }
    } else {
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

// PICO-8 text (fontlib) on a box 1 px wider each way (the advance leaves the right column), so it
// reads over the world; LINE_DY apart, lines' boxes join
static void pico8Boxed(const char *s, uint24_t x, uint8_t y, uint8_t colour) {
    gfx_SetColor(TEXT_BG_COLOUR);
    gfx_FillRectangle(x, y - 1, fontlib_GetStringWidth(s) + 1, PICO8_HEIGHT + 2);
    fontlib_SetForegroundColor(colour);
    fontlib_SetCursorPosition(x, y);
    fontlib_DrawString(s);
}

static int centredX(const char *s) {
    return GFX_WIDTH_HALF - (int)gfx_GetStringWidth(s) / 2;
}

// a pixel of the splash; (u, v) in the unsheared band, so u < SPLASH_W and v < SPLASH_BAND
static void splashPixel(uint8_t w, uint8_t u, uint8_t v, uint8_t colour) {
    const uint8_t y = v + SPLASH_RISE(w - 1) - SPLASH_RISE(u);
    splash->data[y * w + u] = colour;
}

// once: a random splash, sheared, with its shadow on its band, into the sprite (transparent
// NO_TEXT_BG around the band)
static void splashRender(void) {
    const char *s = splashes[rtc_Time() % SPLASHES];
    uint8_t n = 0;
    while (s[n] && n < SPLASH_MAX_CHARS) {
        n++;
    }
    const uint8_t w = n * PICO8_ADVANCE + 2;
    splash->width = w;
    splash->height = SPLASH_BAND + SPLASH_RISE(w - 1);
    for (uint24_t i = 0; i < (uint24_t)w * splash->height; i++) {
        splash->data[i] = NO_TEXT_BG;
    }
    // for (uint8_t u = 0; u < w; u++) {
    //     for (uint8_t v = 0; v < SPLASH_BAND; v++) {
    //         splashPixel(w, u, v, TEXT_BG_COLOUR);
    //     }
    // }
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
                        const uint8_t u = i * PICO8_ADVANCE + col + 1;
                        if (pass == 0) {
                            splashPixel(w, u + 1, v + 2, SPLASH_SHADOW);
                        } else {
                            splashPixel(w, u, v + 1, SPLASH_COLOUR);
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
#define BACK_Y        99        // the bottom row: New, Rename, Back
#define BOTTOM_X      6
#define BOTTOM_W      46
#define BOTTOM_DX     50
#define NAME_Y        30        // the name being typed
#define NAME_HELP_Y   56        // its PICO-8 help lines
#define LABEL_MAX     24        // characters of a button label, NUL included
#define TEXT_MAX      32        // characters of a PICO-8 line, NUL included (128 px)

// a button with its label centred in it (fitted: see fitLabel); colour BUTTON_COLOUR,
// SELECTED_COLOUR or MARKED_COLOUR
static void drawButton(int x, int y, int w, const char *label, uint8_t colour) {
    gfx_SetColor(SHADOW_COLOUR);
    gfx_Rectangle(x, y, w, BUTTON_H);
    gfx_SetColor(colour);
    gfx_FillRectangle(x + 1, y + 1, w - 2, BUTTON_H - 2);
    printShadowed(label, x + w / 2 - (int)gfx_GetStringWidth(label) / 2, y + 3, TEXT_COLOUR);
}

static uint8_t buttonColour(uint8_t sel) {
    return sel ? SELECTED_COLOUR : BUTTON_COLOUR;
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
    pico8Boxed(s, GFX_WIDTH_HALF - fontlib_GetStringWidth(s) / 2, y, colour);
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

// v in exactly digits decimal digits (leading zeros), NUL terminated; returns the end
static char *putPadded(char *p, uint24_t v, uint8_t digits) {
    p += digits;
    *p = '\0';
    for (char *q = p; q-- != p - digits; v /= 10) {
        *q = (char)('0' + v % 10);
    }
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
        drawButton(SETTINGS_X, MENU_Y0 + i * MENU_DY, SETTINGS_W, label, buttonColour(i == selected));
    }
    // PICO-8 text is lowercase (its capitals)
    pico8Centred("leave a star on github!", STAR_Y, TEXT_COLOUR);
    pico8Centred("github.com/kasherpete/", STAR_Y + LINE_DY, SPLASH_COLOUR);
    pico8Centred("wireframe-engine-ce", STAR_Y + 2 * LINE_DY, SPLASH_COLOUR);
}

static void drawWorlds(void) {
    drawHeading("Select World");
    // the last selected world in view
    if (list_sel < list_top) {
        list_top = list_sel;
    }
    if (list_sel >= list_top + LIST_ROWS) {
        list_top = list_sel - (LIST_ROWS - 1);
    }
    char label[LABEL_MAX];
    for (uint8_t r = 0; r < LIST_ROWS && list_top + r < world_count; r++) {
        const uint8_t i = list_top + r;
        const world_entry_t *e = &world_list[i];
        fitLabel(label, e->name[0] ? e->name : e->appvar, LIST_W - 4);
        uint8_t colour = buttonColour(i == selected);
        if (i == list_sel && selected >= world_count && bottom == BOTTOM_RENAME) {
            colour = MARKED_COLOUR;
        }
        drawButton(LIST_X, MENU_Y0 + r * MENU_DY, LIST_W, label, colour);
    }
    // more above or below
    if (list_top > 0) {
        pico8Boxed("^", LIST_X + LIST_W + 3, MENU_Y0 + 4, TEXT_COLOUR);
    }
    if (list_top + LIST_ROWS < world_count) {
        pico8Boxed("v", LIST_X + LIST_W + 3, MENU_Y0 + (LIST_ROWS - 1) * MENU_DY + 4, TEXT_COLOUR);
    }

    // the last selected world: its AppVar, its place in the list, when it was last played
    char text[TEXT_MAX];
    if (list_sel < world_count) {
        const world_entry_t *e = &world_list[list_sel];
        char *p = putStr(text, "appvar ");
        pico8Text(p, e->appvar);
        pico8Boxed(text, LIST_X, INFO_Y, TEXT_COLOUR);
        p = putUInt(text, list_sel + 1);
        p = putStr(p, "/");
        putUInt(p, world_count);
        pico8Boxed(text, LIST_X + LIST_W - fontlib_GetStringWidth(text), INFO_Y, TEXT_COLOUR);

        if (!list_error) {
            if (e->last_played.year == 0) {
                putStr(text, "never played");
            } else {
                p = putStr(text, "last played ");
                p = putPadded(p, e->last_played.year % 10000, 4);
                p = putStr(p, "-");
                p = putPadded(p, e->last_played.month % 100, 2);
                p = putStr(p, "-");
                putPadded(p, e->last_played.day % 100, 2);
            }
            pico8Boxed(text, LIST_X, INFO_Y + LINE_DY, TEXT_COLOUR);
        }
    }
    if (list_error) {
        pico8Text(text, list_error);
        pico8Boxed(text, LIST_X, INFO_Y + LINE_DY, SPLASH_COLOUR);
    }

    static const char *const bottom_labels[BOTTOMS] = {"New", "Rename", "Back"};
    for (uint8_t b = 0; b < BOTTOMS; b++) {
        drawButton(BOTTOM_X + b * BOTTOM_DX, BACK_Y, BOTTOM_W, bottom_labels[b],
                   buttonColour(selected == world_count + b));
    }
}

static void drawName(void) {
    drawHeading(naming_new ? "New World" : "Rename World");
    char label[LABEL_MAX];
    char *p = putStr(label, name_buf);      // printable, at most 15 characters: fits
    putStr(p, "_");
    drawButton(LIST_X, NAME_Y, LIST_W, label, SELECTED_COLOUR);

    static const char *const modes[3] = {"abc (capitals)", "abc (small)", "123"};
    char text[TEXT_MAX];
    p = putStr(text, "alpha: ");
    putStr(p, modes[name_mode]);
    pico8Boxed(text, LIST_X, NAME_HELP_Y, TEXT_COLOUR);
    pico8Boxed("del: erase", LIST_X, NAME_HELP_Y + LINE_DY, TEXT_COLOUR);
    pico8Boxed("enter: save", LIST_X, NAME_HELP_Y + 2 * LINE_DY, TEXT_COLOUR);
    pico8Boxed("clear: cancel", LIST_X, NAME_HELP_Y + 3 * LINE_DY, TEXT_COLOUR);
}

// the pause menu: a heading and the buttons, nothing else over the frozen world
#define PAUSE_HEAD_Y  20
#define PAUSE_X       20
#define PAUSE_W       120
#define PAUSE_Y0      38
#define PAUSE_DY      18

static void drawPause(void) {
    printShadowed("Game Paused", centredX("Game Paused"), PAUSE_HEAD_Y, TITLE_COLOUR);
    for (uint8_t i = 0; i < PAUSE_BUTTONS; i++) {
        drawButton(PAUSE_X, PAUSE_Y0 + i * PAUSE_DY, PAUSE_W, pause_labels[i],
                   buttonColour(i == selected));
    }
}

// no world: where to get one (PICO-8 text, lowercase shows as capitals)
#define NOWORLD_Y   34
#define CONTINUE_Y  76

static void drawNoWorld(void) {
    drawHeading("No World Found");
    uint8_t y = NOWORLD_Y;
    if (noworld_reason) {
        char text[TEXT_MAX];
        pico8Text(text, noworld_reason);
        pico8Centred(text, y, SPLASH_COLOUR);
        y += LINE_DY + 3;
    }
    pico8Centred("download world.8xv from", y, TEXT_COLOUR);
    pico8Centred("kasher.dev/calcraft", y + LINE_DY, SPLASH_COLOUR);
    pico8Centred("and send it to your calculator,", y + 2 * LINE_DY, TEXT_COLOUR);
    pico8Centred("or make one with play > new", y + 3 * LINE_DY, TEXT_COLOUR);
    drawButton(BUTTON_X, CONTINUE_Y, BUTTON_W, "Continue", SELECTED_COLOUR);
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
    gfx_PrintStringXY("CALCRAFT", centredX("CALCRAFT") -1, TITLE_Y + 2);
    gfx_SetTextFGColor(TITLE_COLOUR);
    gfx_PrintStringXY("CALCRAFT", centredX("CALCRAFT"), TITLE_Y);

    gfx_SetTextScale(1, 1);
    const uint24_t ew = fontlib_GetStringWidth(EDITION_TEXT) + 5;   // the advance leaves 1 px right
    const uint24_t ex = GFX_WIDTH_HALF - ew / 2;
    gfx_SetColor(SHADOW_COLOUR);
    gfx_Rectangle(ex, EDITION_Y, ew, EDITION_H);
    gfx_SetColor(EDITION_COLOUR);
    gfx_FillRectangle(ex + 1, EDITION_Y + 1, ew - 2, EDITION_H - 2);
    // the edition box is its own background: shadowed text
    fontlib_SetForegroundColor(SHADOW_COLOUR);
    fontlib_SetCursorPosition(ex + 4, EDITION_Y + 4);
    fontlib_DrawString(EDITION_TEXT);
    fontlib_SetForegroundColor(TEXT_COLOUR);
    fontlib_SetCursorPosition(ex + 3, EDITION_Y + 3);
    fontlib_DrawString(EDITION_TEXT);
    
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
        drawButton(BUTTON_X, BUTTON_Y0 + i * BUTTON_DY, BUTTON_W, labels[i], buttonColour(i == selected));
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

    if (screen == SCREEN_MAIN && !paused) {
        drawMain();
        return;
    }
    gfx_SetTextScale(1, 1);
    gfx_SetFontData(legacy_font);
    gfx_SetFontSpacing(legacy_spacing);
    if (screen == SCREEN_MAIN) {
        drawPause();
    } else if (screen == SCREEN_NOWORLD) {
        drawNoWorld();
    } else if (screen == SCREEN_SETTINGS) {
        drawSettings();
    } else if (screen == SCREEN_WORLDS) {
        drawWorlds();
    } else {
        drawName();
    }
}
