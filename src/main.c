#include <keypadc.h>
#include <graphx.h>

#include <sys/timers.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <debug.h>
#include <fontlibc.h>

#include <sys/lcd.h>
#include <string.h>

#include "engine.h"
#include "fill.h"


#define SCREEN_OFFSET ((GFX_LCD_WIDTH - VIEWPORT_LENGTH) / 2)  // first viewport column (0: full width)
#define draw_buffer_ctrl lcd_LpBase
#define screen_buffer_ctrl lcd_UpBase

#define DBL_BUF1 0xd52c00   // the fill rasterizer's span list (fill.h)
#define DBL_BUF2 0xD5C200
#define BUFFER_1 0xD40000
#define BUFFER_2 0xD49600

#define LEN(arr) ((int) (sizeof (arr) / sizeof (arr)[0]))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#define GFX_HEIGHT_HALF 60
#define GFX_WIDTH_HALF  80
// graphx defines these as 320x240; with the 4bpp trick the logical screen is 160x120.
#undef GFX_LCD_WIDTH
#undef GFX_LCD_HEIGHT
#define GFX_LCD_WIDTH 160
#define GFX_LCD_HEIGHT 120
#define VIEWPORT_LENGTH 160

// FOR TESTING ONLY...NOT ACCURATE (scene colours are materials[] now)
#define GFX_BLACK   0x00
#define GFX_RED     0x11
#define GFX_ORANGE  0x22
#define GFX_GREEN   0x33
#define GFX_BLUE    0xcc
#define GFX_PURPLE  0x55
#define GFX_YELLOW  0x66
#define GFX_PINK    0x77
#define GFX_WHITE   0x88


// palette data
unsigned char mypalette[32] =
{
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

// Face colours per material: {top, x side, z side, bottom}. Material m is palette colour m lit
// from above: the top is m, the z sides m dithered with a darker shade (the two nibbles of a
// byte are two LCD pixels), the x sides the darker shade, the bottom darker still. The darker
// shades follow the PICO-8 palette's own ramps (8 red -> 2 dark purple -> 1 dark blue, ...).
#define MAT(m, dark, darker) {(m) * 0x11, (dark) * 0x11, (m) * 0x10 + (dark), (darker) * 0x11}
const uint8_t materials[MAT_COUNT][4] = {
    MAT(0, 0, 0),    MAT(1, 0, 0),    MAT(2, 1, 0),    MAT(3, 1, 0),
    MAT(4, 2, 1),    MAT(5, 1, 0),    MAT(6, 13, 5),   MAT(7, 6, 13),
    MAT(8, 2, 1),    MAT(9, 4, 2),    MAT(10, 9, 4),   MAT(11, 3, 1),
    MAT(12, 13, 5),  MAT(13, 5, 1),   MAT(14, 8, 2),   MAT(15, 14, 8),
};

// the scene: x, y, z of each voxel (it spans [X-1, X] x [Y-1, Y] x [Z-1, Z]) and its material
// (index into materials[], 0 .. MAT_COUNT - 1). The fifth byte is filled in by computeExposure().
// If the scene changes at runtime, call computeExposure() again
// static int8_t coordinates[][5] = {
//         {5, -2, 5, 4},
//         {5, -1, 5, 4},
//         {5, 0, 5, 4},
//         {4, -3, 5, 3},
//         {6, -3, 5, 3},
//         {5, -3, 4, 3},
//         {5, -3, 6, 3},
// };

static int8_t coordinates[][5] = {
    {-5, 1, -3, 3},
    {-5, 1, -2, 11},
    {-5, 1, -1, 3},
    {-5, 1, 0, 11},
    {-5, 1, 1, 3},
    {-5, 1, 2, 11},
    {-5, 1, 3, 3},
    {-5, 1, 4, 11},
    {-5, 1, 5, 3},
    {-5, 1, 6, 11},
    {-5, 1, 7, 3},
    {-4, 1, -3, 11},
    {-4, 1, -2, 3},
    {-4, 1, -1, 11},
    {-4, 1, 0, 3},
    {-4, 1, 1, 11},
    {-4, 1, 2, 3},
    {-4, 1, 3, 11},
    {-4, 1, 4, 3},
    {-4, 1, 5, 11},
    {-4, 1, 6, 3},
    {-4, 1, 7, 11},
    {-3, 1, -3, 3},
    {-3, 1, -2, 11},
    {-3, 1, -1, 3},
    {-3, 1, 0, 11},
    {-3, 1, 1, 3},
    {-3, 1, 2, 11},
    {-3, 1, 3, 3},
    {-3, 1, 4, 11},
    {-3, 1, 5, 3},
    {-3, 1, 6, 11},
    {-3, 1, 7, 3},
    {-2, 1, -3, 11},
    {-2, 1, -2, 3},
    {-2, 1, -1, 11},
    {-2, 1, 0, 3},
    {-2, 1, 1, 11},
    {-2, 1, 2, 3},
    {-2, 1, 3, 11},
    {-2, 1, 4, 3},
    {-2, 1, 5, 11},
    {-2, 1, 6, 3},
    {-2, 1, 7, 11},
    {-1, 1, -3, 3},
    {-1, 1, -2, 11},
    {-1, 1, -1, 3},
    {-1, 1, 0, 11},
    {-1, 1, 1, 3},
    {-1, 1, 2, 11},
    {-1, 1, 3, 3},
    {-1, 1, 4, 11},
    {-1, 1, 5, 3},
    {-1, 1, 6, 11},
    {-1, 1, 7, 3},
    {0, 1, -3, 11},
    {0, 1, -2, 3},
    {0, 1, -1, 11},
    {0, 1, 0, 3},
    {0, 1, 1, 11},
    {0, 1, 2, 3},
    {0, 1, 3, 11},
    {0, 1, 4, 3},
    {0, 1, 5, 11},
    {0, 1, 6, 3},
    {0, 1, 7, 11},
    {1, 1, -3, 3},
    {1, 1, -2, 11},
    {1, 1, -1, 3},
    {1, 1, 0, 11},
    {1, 1, 1, 3},
    {1, 1, 2, 11},
    {1, 1, 3, 3},
    {1, 1, 4, 11},
    {1, 1, 5, 3},
    {1, 1, 6, 11},
    {1, 1, 7, 3},
    {2, 1, -3, 11},
    {2, 1, -2, 3},
    {2, 1, -1, 11},
    {2, 1, 0, 3},
    {2, 1, 1, 11},
    {2, 1, 2, 3},
    {2, 1, 3, 11},
    {2, 1, 4, 3},
    {2, 1, 5, 11},
    {2, 1, 6, 3},
    {2, 1, 7, 11},
    {3, 1, -3, 3},
    {3, 1, -2, 11},
    {3, 1, -1, 3},
    {3, 1, 0, 11},
    {3, 1, 1, 3},
    {3, 1, 2, 11},
    {3, 1, 3, 3},
    {3, 1, 4, 11},
    {3, 1, 5, 3},
    {3, 1, 6, 11},
    {3, 1, 7, 3},
    {4, 1, -3, 11},
    {4, 1, -2, 3},
    {4, 1, -1, 11},
    {4, 1, 0, 3},
    {4, 1, 1, 11},
    {4, 1, 2, 3},
    {4, 1, 3, 11},
    {4, 1, 4, 3},
    {4, 1, 5, 11},
    {4, 1, 6, 3},
    {4, 1, 7, 11},
    {5, 1, -3, 3},
    {5, 1, -2, 11},
    {5, 1, -1, 3},
    {5, 1, 0, 11},
    {5, 1, 1, 3},
    {5, 1, 2, 11},
    {5, 1, 3, 3},
    {5, 1, 4, 11},
    {5, 1, 5, 3},
    {5, 1, 6, 11},
    {5, 1, 7, 3},
    {-3, 0, 2, 6},
    {-3, -1, 2, 6},
    {-3, 0, 3, 6},
    {-3, -1, 3, 6},
    {-3, 0, 4, 6},
    {-3, -1, 4, 6},
    {-3, 0, 5, 6},
    {-3, -1, 5, 6},
    {-3, 0, 6, 6},
    {-3, -1, 6, 6},
    {-3, 0, 7, 6},
    {-3, -1, 7, 6},
    {3, 0, 0, 8},
    {3, -1, 0, 8},
    {3, -2, 0, 8},
    {3, -3, 0, 8},
    {0, -4, 4, 12},
    {1, -4, 4, 12},
    {2, -4, 4, 12},
    {1, -4, 5, 12},
    {5, -2, 5, 4},
    {5, -1, 5, 4},
    {5, 0, 5, 4},
    {4, -3, 5, 11},
    {6, -3, 5, 11},
    {5, -3, 4, 11},
    {5, -3, 6, 11},
    {-8, 0, 14, 9},
    {-4, 0, 14, 9},
    {0, 0, 14, 9},
    {4, 0, 14, 9},
    {8, 0, 14, 9}
};



// Player state. Position is 16.16 fixed point world units. Yaw is in 1/65536 of an angle step,
// with 256 steps per turn, so 24 bits are exactly one turn and wrapping is just turning past 360
// degrees: yaw 0 looks along +z, and increasing yaw turns right, towards +x. Velocities ease towards what the keys ask for (handleInput).
// The eye is eye_height + jump_height above the ground plane y = 0 (y grows downward), so
// player_y = -(eye_height + jump_height). There is no collision: the ground is always at y = 0.
#define STAND_HEIGHT  (3L << 15)      // 1.5 units, in 1/65536 units
#define CROUCH_HEIGHT (3L << 14)      // 0.75 units
static int32_t  player_x   = 0;
static int32_t  player_y   = -STAND_HEIGHT;
static int32_t  player_z   = 0;
static uint24_t player_yaw = 0;
static int24_t  walk_vel   = 0;   // 1/256 units per second, along the view direction
static int24_t  strafe_vel = 0;   // 1/256 units per second, to the right of the view direction
static int24_t  turn_vel   = 0;   // 1/256 steps per second
static uint8_t  snapping   = 0;   // easing into the nearest N/E/S/W
static int24_t  eye_height = STAND_HEIGHT;  // 1/65536 units: eases between STAND_ and CROUCH_HEIGHT
static int32_t  jump_height = 0;  // 1/65536 units above standing on the ground, >= 0
static int24_t  jump_vel   = 0;   // 1/256 units per second, upwards


// PL111 "next base update" interrupt flag: set when the LCD loads lcd_UpBase at the start of a
// frame. swapBuffer clears it, so once it is set again the new frame is on screen and the old one
// is free to draw into (the same flag gfx_SwapDraw/gfx_Wait use). Polling lcd_UpBaseCurr for "stopped
// changing" returned early whenever a frame took less than the LCD idle period (about 5 ms), giving
// two swaps per refresh and drawing into the buffer still on screen.
#define LCD_INT_LNBU (1 << 2)

void waitForBufferScan(void) {
    while (!(lcd_IntStatus & LCD_INT_LNBU)) {
    }
}


// deinterlace.S: for each of the 120 graphx rows from row0, copy bytes 0..width-1 to the LCD row
// below (+160). width must be 1..160
void deinterlaceRows(uint8_t *row0, uint24_t width);

// the whole 160 byte width of the frame at buffer
static void deInterlaceFullBuffer(uint24_t buffer) {
    deinterlaceRows((uint8_t *)buffer, LCD_WIDTH / 2);
}


// only the viewport columns of the frame at buffer: side bars (if SCREEN_OFFSET > 0) are never redrawn
static void deInterlaceViewportBuffer(uint24_t buffer) {
    deinterlaceRows((uint8_t *)buffer + SCREEN_OFFSET, VIEWPORT_LENGTH);
}


void setDrawBuffer(uint24_t buffer) {
    draw_buffer_ctrl = buffer;
}


void setScreenBuffer(uint24_t buffer) {
    screen_buffer_ctrl = buffer;
}


// Double buffering: each frame is drawn into the even LCD rows of the back buffer, and
// deInterlaceViewportBuffer copies them to the odd rows, so every logical pixel is a 2x2 block.
static uint8_t drawing_buffer_2 = 0;   // the frame being drawn: 0 = BUFFER_1, 1 = BUFFER_2

void swapBuffer() {

    // show the frame just drawn, then set up the next frame in the other buffer. Base first, then
    // the acknowledge: the other way round, a refresh in between would set the flag for the old base
    // and waitForBufferScan would return while that frame is still on screen. This way round the
    // worst case is waiting one refresh too many
    setScreenBuffer(drawing_buffer_2 ? BUFFER_2 : BUFFER_1);
    lcd_IntAcknowledge = LCD_INT_LNBU;
    drawing_buffer_2 = !drawing_buffer_2;
    setDrawBuffer(drawing_buffer_2 ? BUFFER_2 : BUFFER_1);

}


// ---- painter's order ----
// Boxes are drawn far to near by their Manhattan distance in grid cells from the camera's cell,
// |dx| + |dy| + |dz|. For unit voxels on a grid this is an exact painter's order: if box A hides
// part of box B, the sight line from the camera to B passes through A, and along a line every
// coordinate is monotonic, so on each axis A's cell lies between the camera's cell and B's. A's
// distance is then strictly smaller. Equal distances never occlude each other, so ties can go
// either way. Camera depth (z) is not a valid key: two boxes side by side at the same depth can
// overlap on screen.
//
// fill.S walks the list backwards, NEAR TO FAR, and keeps track of what is already covered: the
// same order read the other way round, so a box is only drawn where nothing nearer is.
//
// The key only depends on which cell the camera is in, not on its yaw or position inside the cell,
// so the order only changes when the camera crosses into another cell. main() then calls
// sortScene(), which reorders coordinates[] itself. projectScene() keeps boxes in list order, so
// the boxes it keeps come out already sorted and a frame without a re-sort pays nothing for it.
// Walking at full speed crosses about 4 cells per second (up to 6 diagonally); standing still or
// turning never re-sorts.
//
// sortScene() calls sortRecords() in sort.S: a counting sort (stable, no comparisons) over keys
// 0 .. 3 GRID_RANGE, through a scratch copy. 74k cycles for the 153-box test scene.

// gridDist[n] = |n - GRID_RANGE|: the distance in cells along one axis, for the grid index
// n = X + cam_bias_x (the one projectRecords() uses). The box spans cell X - 1 and the camera is in
// cell (player x >> 16), so n - GRID_RANGE is their difference (likewise y and z).
// It is indexed by the low byte of n only, so all 256 entries exist. For every box
// projectRecords() keeps, n is in [0, 2 GRID_RANGE) and the low byte is n itself. For a culled box the low byte
// may wrap to any entry, which is harmless: where a culled box sorts does not matter. So the key
// is three 8-bit adds and lookups, with no sign extension or range check. Defined in sort.S,
// page-aligned
extern uint8_t gridDist[256];

static void initGridDist(void) {
    uint8_t n = 0;
    do {
        gridDist[n] = (n >= 2 * GRID_RANGE) ? GRID_RANGE : (n < GRID_RANGE) ? GRID_RANGE - n : n - GRID_RANGE;
    } while (++n != 0);
}

_Static_assert(3 * GRID_RANGE < 256, "distance keys are bytes");
_Static_assert(LEN(coordinates) <= 255, "sortScene counts and indexes the scene in bytes");

static int8_t  sort_scratch[LEN(coordinates)][5];
static uint8_t sort_key[LEN(coordinates)];
static int24_t sorted_bias_x, sorted_bias_y, sorted_bias_z;  // the camera cell coordinates[] is sorted for

// sort.S: sorts the n records far to near for camera biases bx, by, bz (the low bytes of
// cam_bias_*), through scratch[n] and keys[n]. n must be 1 .. 255
void sortRecords(int8_t (*rec)[5], int8_t (*scratch)[5], uint8_t *keys, uint8_t n,
                 uint8_t bx, uint8_t by, uint8_t bz);

_Static_assert(LEN(coordinates) >= 1, "sortRecords needs at least one record");

// Sorts coordinates[] far to near from the camera cell setupCamera() last set up
static void sortScene(void) {

    sortRecords(coordinates, sort_scratch, sort_key, LEN(coordinates),
                (uint8_t)cam_bias_x, (uint8_t)cam_bias_y, (uint8_t)cam_bias_z);
    sorted_bias_x = cam_bias_x;
    sorted_bias_y = cam_bias_y;
    sorted_bias_z = cam_bias_z;
}


// whether the camera is in another cell than coordinates[] was sorted for (the biases change
// exactly when the cell does). Ifs, not a combined 0/1 value: see the compiler bug in CLAUDE.md
static uint8_t sceneOrderStale(void) {
    if (cam_bias_x != sorted_bias_x) {
        return 1;
    }
    if (cam_bias_y != sorted_bias_y) {
        return 1;
    }
    if (cam_bias_z != sorted_bias_z) {
        return 1;
    }
    return 0;
}


// ---- camera ----
// Camera space: x right, y down, z forward, in 1/SUB world units. setupCamera() builds the
// per-frame state in engine.h once per frame (C is fine there). The per-box work is in
// project.S, because clang turns every multiply, right shift and even AND into a library call.

#define MAX_DT  (CLOCKS_PER_SEC / 8)  // clamp frame time so a stall does not teleport the player

_Static_assert(SPAN_LIST == DBL_BUF1 && SPAN_LIST_END == DBL_BUF2, "fill.h span list is not DBL_BUF1");
_Static_assert(TABLE_VIEW_HALF_WIDTH * 2 == VIEWPORT_LENGTH, "gen_tables.py culls for a different viewport width");
_Static_assert(SIN_TABLE_LEN == 256, "yaw is looked up as a byte");
_Static_assert(SCREEN_OFFSET == VIEW_X0 && SCREEN_OFFSET + VIEWPORT_LENGTH == VIEW_X1, "engine.h viewport out of sync");
_Static_assert(GFX_LCD_HEIGHT == VIEW_H && GFX_WIDTH_HALF == CENTRE_X && GFX_HEIGHT_HALF == CENTRE_Y, "engine.h screen out of sync");

// shared with project.S, see engine.h (the cam_ scalars are defined in project.S)
int24_t cam_bias_x, cam_bias_y, cam_bias_z;
int24_t rotX_[2 * GRID_LEN], rotZ_[2 * GRID_LEN], camY_[GRID_LEN];
uint8_t cam_axis_corner[4];

// cam_axis_corner per cardinal direction (yaw >> 22): the world corner (engine.h) of the near-left,
// near-right, far-left and far-right edge. E.g. facing +x (yaw 64 steps) the near face is x = X - 1
// (corners 0 and 2), and +z is to the left
static const uint8_t axis_corners[4][4] = {
    {0, 1, 2, 3},   // facing +z
    {2, 0, 3, 1},   // +x
    {3, 2, 1, 0},   // -z
    {1, 3, 0, 2},   // -x
};


// Level of detail (engine.h): world units from which a box becomes a rectangle, then a pixel.
// Variables, so a game can change them at runtime (e.g. a quality setting): setupCamera reads
// them every frame. At or beyond the far plane (GRID_RANGE) a level is off
uint8_t lod_rect_dist = LOD_RECT_DIST;
uint8_t lod_pixel_dist = LOD_PIXEL_DIST;

// The colour of a BOX_RECT box by its FACE_* bits (fill.h, lod_slot): far away a side shows far
// more than a top (that shrinks with height / depth), so a side if visible, else the top or bottom.
// project.S has already kept only the side that points at the camera position most (.Llod_side),
// so the order here doesn't matter and the table is fixed: built once at startup
static void initLodSlots(void) {
    for (uint8_t m = 0; m < 64; m++) {
        uint8_t slot = (m & FACE_TOP) ? MAT_TOP : MAT_BOTTOM;
        if (m & (FACE_ZP | FACE_ZN)) {
            slot = MAT_ZSIDE;
        } else if (m & (FACE_XP | FACE_XN)) {
            slot = MAT_XSIDE;
        }
        lod_slot[m] = slot;
    }
}


// t[n * stride] = 3 * (n - GRID_RANGE - frac/256) * k, by repeated addition. Built unscaled and
// then tripled so every entry is an exact multiple of 3 (project.S uses them as byte offsets)
static void fillRotTable(int24_t *t, uint8_t stride, int24_t k, int24_t frac) {

    const int24_t v = -GRID_RANGE * k - ((frac * k) >> 8);
    int24_t v3 = v + v + v;
    const int24_t k3 = k + k + k;
    for (uint8_t n = 0; n < GRID_LEN; n++) {
        *t = v3;
        t += stride;
        v3 += k3;
    }
}


// once per frame, after the player moves. Per-frame code: plain C is fine here
static void setupCamera(void) {

    const uint8_t angle = (uint8_t)(player_yaw >> 16);
    const int24_t s = sinTable[angle];
    const int24_t c = sinTable[(uint8_t)(angle + 64)];
    cam_cardinal = (player_yaw & 0x3fffff) == 0;  // a multiple of 64 steps, fraction included
    memcpy(cam_axis_corner, axis_corners[angle >> 6], 4);

    // grid cell, and the position within it in 1/256 units
    const int24_t cell_x = (int24_t)(player_x >> 16);
    const int24_t cell_y = (int24_t)(player_y >> 16);
    const int24_t cell_z = (int24_t)(player_z >> 16);
    cam_bias_x = GRID_RANGE - 1 - cell_x;
    cam_bias_y = GRID_RANGE - 1 - cell_y;
    cam_bias_z = GRID_RANGE - 1 - cell_z;
    const int24_t fx = (int24_t)(player_x >> 8) & 0xff;
    const int24_t fy = (int24_t)(player_y >> 8) & 0xff;
    const int24_t fz = (int24_t)(player_z >> 8) & 0xff;

    // x' = dx cos - dz sin, z' = dx sin + dz cos
    fillRotTable(rotX_,     2, c, fx);    // XC
    fillRotTable(rotX_ + 1, 2, s, fx);    // XS
    fillRotTable(rotZ_,     2, s, fz);    // ZS
    fillRotTable(rotZ_ + 1, 2, c, fz);    // ZC
    fillRotTable(camY_,     1, SUB, fy);  // no rotation in y, just the offset

    const int24_t c3 = 3 * c, s3 = 3 * s;
    cam_cos3 = c3;
    cam_sin3 = s3;
    cam_kxmin3 = MIN(0, c3) + MIN(0, -s3);
    cam_kxmax3 = MAX(0, c3) + MAX(0, -s3);
    cam_kzmin3 = MIN(0, s3) + MIN(0, c3);
    cam_kzmax3 = MAX(0, s3) + MAX(0, c3);

    // box centre offsets: multiples of 3 between the min and max offsets
    cam_kxmid3 = 3 * ((cam_kxmin3 + cam_kxmax3) / 6);
    cam_kzmid3 = 3 * ((cam_kzmin3 + cam_kzmax3) / 6);

    // level of detail: by the depth of the nearest corner, z00 + kzmin3
    cam_lod_rect = 3 * SUB * (int24_t)lod_rect_dist - cam_kzmin3;
    cam_lod_pix  = 3 * SUB * (int24_t)lod_pixel_dist - cam_kzmin3;

    // culling constants, see engine.h
    cam_zlo   = 3 * NEAR_PLANE - cam_kzmin3;
    cam_zspan = (3 * FAR_PLANE - cam_kzmax3) - cam_zlo;
    cam_cxl   = -(4 * cam_kxmax3 + 5 * cam_kzmax3);   // horizontal view is |x| <= 1.25 z (engine.h)
    cam_cxr   = 5 * cam_kzmax3 - 4 * cam_kxmin3;
    cam_cyl   = -(3 * SUB + cam_kzmax3);
}


// the layout fill.S reads
// IntelliSense sizes int24_t differently from ez80-clang and would flag these; the real build checks them
#ifndef __INTELLISENSE__
_Static_assert(sizeof(projected_box_t) == PB_SIZE, "engine.h PB_SIZE out of sync");
_Static_assert(__builtin_offsetof(projected_box_t, kind) == PB_KIND, "engine.h PB_KIND out of sync");
_Static_assert(__builtin_offsetof(projected_box_t, faces) == PB_FACES, "engine.h PB_FACES out of sync");
_Static_assert(__builtin_offsetof(projected_box_t, mat) == PB_MAT, "engine.h PB_MAT out of sync");
_Static_assert(__builtin_offsetof(projected_box_t, v) == PB_V, "engine.h PB_V out of sync");
#endif

static projected_box_t projected[LEN(coordinates)];
static uint8_t num_projected;


// Faces that no other voxel covers: a face shared with a neighbouring voxel can never be seen,
// so projectScene() never hands it to the rasterizer. A floor then costs one face per voxel
// instead of three. Stored in coordinates[i][4] as FACE_* bits. Every pair once: n^2 / 2 tests,
// about 40 ms at startup for 255 voxels
static void computeExposure(void) {
    for (uint8_t i = 0; i < LEN(coordinates); i++) {
        coordinates[i][4] = FACE_XP | FACE_XN | FACE_ZP | FACE_ZN | FACE_TOP | FACE_BOT;
    }
    for (uint8_t i = 0; i < LEN(coordinates); i++) {
        int8_t *a = coordinates[i];
        for (uint8_t j = i + 1; j < LEN(coordinates); j++) {
            int8_t *b = coordinates[j];
            const int8_t dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
            // ifs, not a combined test: see the compiler bug in CLAUDE.md
            if (dy == 0 && dz == 0) {
                if (dx == 1) {
                    a[4] &= ~FACE_XP;
                    b[4] &= ~FACE_XN;
                } else if (dx == -1) {
                    a[4] &= ~FACE_XN;
                    b[4] &= ~FACE_XP;
                }
            } else if (dx == 0 && dy == 0) {
                if (dz == 1) {
                    a[4] &= ~FACE_ZP;
                    b[4] &= ~FACE_ZN;
                } else if (dz == -1) {
                    a[4] &= ~FACE_ZN;
                    b[4] &= ~FACE_ZP;
                }
            } else if (dx == 0 && dz == 0) {
                if (dy == -1) {             // b is above a: a's top face is covered
                    a[4] &= ~FACE_TOP;
                    b[4] &= ~FACE_BOT;
                } else if (dy == 1) {
                    a[4] &= ~FACE_BOT;
                    b[4] &= ~FACE_TOP;
                }
            }
        }
    }
}


// The faces of a box that face the camera (FACE_*), from d = box cell - camera cell on each axis:
// the +x face (at X) faces the camera exactly when the camera is in a cell right of the box's, and
// so on; d = 0 means the camera is level with the box on that axis, so neither face is visible.
// With n = X + cam_bias_x (the grid index projectRecords uses) d is n - GRID_RANGE, so the bits are
// tables indexed by the low byte of n: three 8-bit adds and lookups per box. n wraps for boxes
// beyond GRID_RANGE, which projectRecords culls before it looks (as in sortRecords). C with int24
// compares cost about 1k cycles per box. face_x, face_y and face_z are in project.S, on
// consecutive pages

// n >= 2 GRID_RANGE: a box projectRecords culls, so no faces (ifs: see the compiler bug in CLAUDE.md)
static void initFaceTables(void) {
    uint8_t n = 0;
    do {
        if (n < GRID_RANGE) {               // box cell < camera cell
            face_x[n] = FACE_XP;
            face_z[n] = FACE_ZP;
            face_y[n] = FACE_BOT;           // y grows downward: the box is above the camera
        } else if (n > GRID_RANGE && n < 2 * GRID_RANGE) {
            face_x[n] = FACE_XN;
            face_z[n] = FACE_ZN;
            face_y[n] = FACE_TOP;
        } else {
            face_x[n] = face_y[n] = face_z[n] = 0;
        }
    } while (++n != 0);
}


// Culls and projects every box into projected[], without drawing. Boxes stay in the order of
// coordinates[], which sortScene() keeps far to near. A box with no face both towards the camera
// and not covered by a neighbour is skipped before projecting (its silhouette is covered by
// nearer neighbours). The loop, the face test and the projection are all in project.S: in C the
// loop and the call cost about 650 cycles per record. main() calls this before waitForBufferScan(),
// so it runs in time that would otherwise be spent waiting for the LCD
static void projectScene(void) {
    num_projected = projectRecords(coordinates, LEN(coordinates), projected);
}


// ---- input ----
// Speeds are per second and scaled by the frame time, so they do not change with FPS.
// Per-frame code, so the 32-bit maths here does not matter.

#define WALK_SPEED   (4 * 256)        // units/s, in 1/256 units (forwards/backwards and strafing)
#define WALK_ACCEL   (32 * 256)       // units/s^2: full speed (and a full stop) in 1/8 s
#define TURN_SPEED   (96 * 256)       // steps/s (96 = 135 deg/s), in 1/256 steps
#define TURN_ACCEL   (768L * 256)     // steps/s^2: full speed in 1/8 s
#define SNAP_RANGE   (8L << 16)       // after a turn, ease into the nearest N/E/S/W if within 8 steps (11 deg). 0 disables
#define SNAP_START   (16 * 256)       // ... once the turn has slowed below 16 steps/s
#define SNAP_GAIN    10               // snap speed = 10 * remaining angle per second (time constant 0.1 s)
#define SNAP_DONE    (1L << 13)       // finish exactly once within 1/8 step
#define JUMP_SPEED   (7 * 256)        // units/s upwards at take-off, in 1/256 units
#define GRAVITY      (24L * 256)      // units/s^2: a jump peaks about 1 unit up after 0.29 s
#define CROUCH_SPEED (6L * 256)       // units/s: crouching or standing up takes 1/8 s

_Static_assert((int32_t)MAX_DT * TURN_SPEED < 0x7fffffff / 2, "per-frame turn step overflows");

// what updateMotion is asked to do (handleInput reads the keys)
#define MOVE_FORWARD     0x01   // [up]
#define MOVE_BACK        0x02   // [down]
#define MOVE_LEFT        0x04   // [left]: strafe
#define MOVE_RIGHT       0x08   // [right]: strafe
#define MOVE_TURN_LEFT   0x10   // [mode]
#define MOVE_TURN_RIGHT  0x20   // [del]
#define MOVE_JUMP        0x40   // [2nd]: held, it jumps again on landing
#define MOVE_CROUCH      0x80   // [alpha], while held


// moves v towards target by at most step
static int24_t approach(int24_t v, int24_t target, int24_t step) {
    if (v < target) {
        return (target - v < step) ? target : v + step;
    }
    return (v - target < step) ? target : v - step;
}


// moves the player at vel (1/256 units per second) along the direction angle steps from +z
static void movePlayer(int24_t vel, uint8_t angle, uint24_t dt) {
    const int32_t dist = ((int32_t)vel * dt) >> 7;  // 1/65536 units: vel/256 * dt/32768 * 65536
    player_x += (dist * sinTable[angle]) / SUB;
    player_z += (dist * sinTable[(uint8_t)(angle + 64)]) / SUB;
}


// keys is a set of MOVE_* flags. dt is in clock ticks
static void updateMotion(uint8_t keys, uint24_t dt) {

    // ---- walking and strafing ----
    const int24_t walk_step = (int24_t)(((int32_t)WALK_ACCEL * dt) / CLOCKS_PER_SEC);

    int24_t walk_target = 0;
    if (keys & MOVE_FORWARD) {
        walk_target = WALK_SPEED;
    } else if (keys & MOVE_BACK) {
        walk_target = -WALK_SPEED;
    }
    walk_vel = approach(walk_vel, walk_target, walk_step);

    int24_t strafe_target = 0;
    if (keys & MOVE_RIGHT) {
        strafe_target = WALK_SPEED;
    } else if (keys & MOVE_LEFT) {
        strafe_target = -WALK_SPEED;
    }
    strafe_vel = approach(strafe_vel, strafe_target, walk_step);

    const uint8_t angle = (uint8_t)(player_yaw >> 16);
    if (walk_vel != 0) {
        movePlayer(walk_vel, angle, dt);
    }
    if (strafe_vel != 0) {
        movePlayer(strafe_vel, (uint8_t)(angle + 64), dt);  // 90 degrees right of the view
    }

    // ---- jumping and crouching ----
    if (jump_height == 0 && jump_vel == 0 && (keys & MOVE_JUMP)) {
        jump_vel = JUMP_SPEED;
    }
    if (jump_height != 0 || jump_vel != 0) {
        jump_height += ((int32_t)jump_vel * dt) >> 7;
        jump_vel -= (int24_t)((GRAVITY * dt) / CLOCKS_PER_SEC);
        if (jump_height <= 0) {
            jump_height = 0;  // landed
            jump_vel = 0;
        }
    }
    eye_height = approach(eye_height, (keys & MOVE_CROUCH) ? CROUCH_HEIGHT : STAND_HEIGHT,
                          (int24_t)((CROUCH_SPEED * dt) >> 7));
    player_y = -(eye_height + jump_height);

    // ---- turning ----
    const int24_t turn_step = (int24_t)((TURN_ACCEL * dt) / CLOCKS_PER_SEC);
    if (keys & (MOVE_TURN_LEFT | MOVE_TURN_RIGHT)) {
        snapping = 0;
        turn_vel = approach(turn_vel, (keys & MOVE_TURN_LEFT) ? -TURN_SPEED : TURN_SPEED, turn_step);
    } else {
        // signed distance to the nearest multiple of 64 steps, in 1/65536 steps
        const int32_t off = (int32_t)((player_yaw + (32L << 16)) & ((64L << 16) - 1)) - (32L << 16);
        if (!snapping && off != 0 && off <= SNAP_RANGE && off >= -SNAP_RANGE &&
            turn_vel < SNAP_START && turn_vel > -SNAP_START) {
            snapping = 1;
        }
        if (snapping) {
            if (off < SNAP_DONE && off > -SNAP_DONE) {
                player_yaw -= off;  // exactly N/E/S/W: projectRecords takes the fast path
                turn_vel = 0;
                snapping = 0;
            } else {
                turn_vel = (int24_t)(-(off >> 8) * SNAP_GAIN);
            }
        } else {
            turn_vel = approach(turn_vel, 0, turn_step);
        }
    }
    player_yaw += ((int32_t)turn_vel * dt) >> 7;  // 1/65536 steps
}


// no kb_Scan(): main() puts the keypad in continuous scan mode, so kb_Data is always current.
// kb_Scan() waits for a whole scan, about 62k cycles (1.3 ms) per frame
static void handleInput(uint24_t dt) {
    const uint8_t arrows = kb_Data[7];
    const uint8_t group1 = kb_Data[1];
    uint8_t keys = 0;
    if (arrows & kb_Up)          keys |= MOVE_FORWARD;
    if (arrows & kb_Down)        keys |= MOVE_BACK;
    if (arrows & kb_Left)        keys |= MOVE_LEFT;
    if (arrows & kb_Right)       keys |= MOVE_RIGHT;
    if (group1 & kb_Mode)        keys |= MOVE_TURN_LEFT;
    if (group1 & kb_Del)         keys |= MOVE_TURN_RIGHT;
    if (group1 & kb_2nd)         keys |= MOVE_JUMP;
    if (kb_Data[2] & kb_Alpha)   keys |= MOVE_CROUCH;
    updateMotion(keys, dt);
}


// ---- HUD and profiling ----
// Only in "make hud" builds (SHOW_HUD). Otherwise CYCLES() is 0 and initHud/drawHud are empty, so
// the compiler removes all of it: the text alone costs about 60k cycles (1.2 ms) per frame.
#ifdef SHOW_HUD

static uint8_t my_font_data[] = {
    #include "../font/myfont.inc"
};

// Timer 2 counts CPU cycles (48 MHz) and is left free-running. Read the low 24 bits directly:
// timer_Get() hangs with TIMER_CPU (see CLAUDE.md). Differences of two reads are correct across
// the 24-bit wrap as long as the interval is under 2^24 cycles (349 ms)
#define CYCLES() (*(volatile uint24_t *)TIMER_COUNT_ADDR(2))
#define CYCLES_PER_TENTH_MS 4800


static void initHud(void) {

    // load font
    fontlib_font_t* my_font = (fontlib_font_t *)my_font_data;
    fontlib_SetFont(my_font, 0);

    fontlib_SetTransparency(true);
    fontlib_SetWindowFullScreen();
    fontlib_SetColors(0x00,0xff);

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


// D: this frame's scene (projection and rasterization + drawing the spans). F: the previous frame's
// total work (everything but the wait)
static void drawHud(uint8_t fps, uint24_t draw_cycles, uint24_t frame_cycles) {

    fontlib_SetCursorPosition(3,3);
    fontlib_DrawUInt(fps, 2);

    drawMs(11, "d ", draw_cycles);
    drawMs(19, "f ", frame_cycles);

    // // buffer number, top left (in the side bar when there is one)
    // fontlib_SetTransparency(false);
    // fontlib_SetColors(GFX_BLUE, GFX_BLACK);
    // fontlib_SetCursorPosition(3,3);
    // fontlib_DrawUInt(drawing_buffer_2 + 1, 1);
}

#else

#define CYCLES() 0

static void initHud(void) {}

static void drawHud(uint8_t fps, uint24_t draw_cycles, uint24_t frame_cycles) {
    (void)fps;
    (void)draw_cycles;
    (void)frame_cycles;
}

#endif


// the bars beside the viewport, in the current colour. None while the viewport is full width:
// a 0 wide _NoClip fill at x = 160 would not be safe
static void fillSideBars(void) {
#if SCREEN_OFFSET > 0
    gfx_FillRectangle_NoClip(0, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
    gfx_FillRectangle_NoClip(SCREEN_OFFSET + VIEWPORT_LENGTH, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
#endif
}


void init_screen() {

    // initialize graphx
    gfx_Begin();
    
    // set palette
    gfx_SetPalette(mypalette, sizeof mypalette, 0);
    gfx_SetClipRegion(SCREEN_OFFSET, 0, SCREEN_OFFSET + VIEWPORT_LENGTH, GFX_LCD_HEIGHT);
    lcd_Control = 0x13925; // 4 bpp mode

    // set screen display to blank double buffer
    setScreenBuffer(DBL_BUF2);

    // Both buffers start as background (every frame rewrites every viewport pixel, fill.S)
    memset((void *)BUFFER_1, SKY_COLOUR, 2 * (BUFFER_2 - BUFFER_1));

    // initialize second buffer
    setDrawBuffer(BUFFER_2);
    fillSideBars();
    deInterlaceFullBuffer(BUFFER_2);

    // initialize first buffer
    setDrawBuffer(BUFFER_1);
    fillSideBars();
    deInterlaceFullBuffer(BUFFER_1);

    // set screen display back normal
    setScreenBuffer(BUFFER_1);
    setDrawBuffer(BUFFER_1);

}


int main(void)
{

    // wait until user releases keys
    while ((kb_Data[1] & kb_2nd) || (kb_Data[6] & kb_Enter)) {
        msleep(100);
        kb_Scan();
    }

    // initialization
    initHud();
    init_screen();
    
    // prep 3d environment
    computeExposure();
    initFaceTables();
    initGridDist();
    initLodSlots();
    setupCamera();
    sortScene();

    // FPS calculation
    uint8_t frame_count = 0;
    clock_t start_time = clock();
    clock_t last_time = start_time;
    uint8_t fps = 0;

    // profiling: the previous frame's work time, excluding the wait for the LCD (shown one frame late)
    uint24_t frame_cycles = 0;

    // the keypad rescans by itself, so kb_Data stays current without kb_Scan(). Restored on exit
    const uint8_t kb_mode = kb_GetMode();
    kb_SetMode(MODE_3_CONTINUOUS);

    // while clear not pressed
    while (!(kb_Data[6] & kb_Clear)) {

        const uint24_t frame_start = CYCLES();
        frame_count++;

        // frame time for smooth movement
        const clock_t now = clock();
        uint24_t dt = now - last_time;
        last_time = now;
        if (dt > MAX_DT) {
            dt = MAX_DT;
        }

        // do this every second
        if (now - start_time >= CLOCKS_PER_SEC) {

            // FPS calculation
            fps = frame_count;
            frame_count = 0;
            start_time = now;
        }

        handleInput(dt);
        setupCamera();

        // painter's order: only changes when the camera enters another cell
        if (sceneOrderStale()) {
            sortScene();
        }

        // Cull, project and rasterize before the wait: none of it touches a frame buffer (unless
        // the span list overflows, see fill.h), so it runs while the LCD still shows the last
        // frame, and only drawSpans below is tied to the refresh
        const uint24_t project_start = CYCLES();
        projectScene();
        fillScene(projected, num_projected, draw_buffer_ctrl);
        const uint24_t project_cycles = CYCLES() - project_start;

        // this eliminates flickering. Idle time, so it is left out of the frame time
        uint24_t wait_cycles = CYCLES();
        waitForBufferScan();
        wait_cycles = CYCLES() - wait_cycles;

        // every viewport pixel of the field: the visible parts of the boxes and the background
        const uint24_t draw_start = CYCLES();
        drawSpans();

        // scene time: projection and rasterization (before the wait) + drawing the spans
        const uint24_t draw_cycles = CYCLES() - draw_start + project_cycles;

        // FPS and timings ("make hud" builds only)
        drawHud(fps, draw_cycles, frame_cycles);

        // copy the drawn rows to the rows below
        deInterlaceViewportBuffer(draw_buffer_ctrl);

        swapBuffer();

        frame_cycles = CYCLES() - frame_start - wait_cycles;
    }

    // program end
    kb_SetMode(kb_mode);
    gfx_End();
    return 0;
}