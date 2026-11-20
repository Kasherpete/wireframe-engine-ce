// The palette and the material colours (colours.h).
#include "colours.h"
#include "world.h"

const uint8_t palette[32] = {
    0x00, 0x00, /*   0: rgb(  0,   0,   0) */
    0xaa, 0x90, /*   1: rgb( 33,  45,  82) */
    0x8a, 0xbc, /*   2: rgb(123,  36,  82) */
    0x0a, 0x82, /*   3: rgb(  0, 134,  82) */
    0x47, 0x55, /*   4: rgb(173,  81,  58) */
    0x4a, 0xb1, /*   5: rgb( 99,  85,  82) */
    0x18, 0x63, /*   6: rgb(197, 194, 197) */
    0xdc, 0x7f, /*   7: rgb(255, 243, 230) */
    0x09, 0x7c, /*   8: rgb(255,   0,  74) */
    0x80, 0x7e, /*   9: rgb(255, 162,   0) */
    0xa5, 0x7f, /*  10: rgb(255, 235,  41) */
    0x87, 0x03, /*  11: rgb(  0, 227,  58) */
    0xbf, 0x96, /*  12: rgb( 41, 174, 255) */
    0xd3, 0xc1, /*  13: rgb(132, 117, 156) */
    0xd4, 0xfd, /*  14: rgb(255, 117, 165) */
    0x35, 0x7f, /*  15: rgb(255, 202, 173) */
};

// Material m is palette colour m lit from above: the top is m, the z sides m dithered with a
// darker shade (the two nibbles of a byte are two LCD pixels), the x sides the darker shade, the
// bottom darker still. The darker shades follow the PICO-8 palette's own ramps (8 red -> 2 dark
// purple -> 1 dark blue, ...)
#define MAT(m, dark, darker) {(m) * 0x11, (dark) * 0x11, (m) * 0x10 + (dark), (darker) * 0x11}
const uint8_t materials[MAT_COUNT][4] = {
    MAT(0, 0, 0),    MAT(1, 0, 0),    MAT(2, 1, 0),    MAT(3, 1, 0),
    MAT(4, 2, 1),    MAT(5, 1, 0),    MAT(6, 13, 5),   MAT(7, 6, 13),
    MAT(8, 2, 1),    MAT(9, 4, 2),    MAT(10, 9, 4),   MAT(11, 3, 1),
    MAT(12, 13, 5),  MAT(13, 5, 1),   MAT(14, 8, 2),   MAT(15, 14, 8),
};

uint24_t ground_colour3;

void coloursInit(void) {
    uint8_t g = GROUND_COLOUR;
    if (world->ground != WORLD_NO_GROUND) {
        g = materials[world->ground][MAT_TOP];
    }
    ground_colour3 = g * (uint24_t)0x010101;
}
