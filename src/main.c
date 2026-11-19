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


#define SCREEN_OFFSET ((GFX_LCD_WIDTH - VIEWPORT_LENGTH) / 2)  // first viewport column (0: full width)
#define draw_buffer_ctrl lcd_LpBase
#define screen_buffer_ctrl lcd_UpBase

#define DBL_BUF1 0xd52c00
#define DBL_BUF2 0xD5C200
#define BUFFER_1 0xD40000
#define BUFFER_2 0xD49600

#define LEN(arr) ((int) (sizeof (arr) / sizeof (arr)[0]))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#define GFX_HEIGHT_HALF 60
#define GFX_WIDTH_HALF  80
#define GFX_LCD_WIDTH 160
#define GFX_LCD_HEIGHT 120
#define VIEWPORT_LENGTH 160

// FOR TESTING ONLY...NOT ACCURATE
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

// the scene: x, y, z of each voxel (it spans [X-1, X] x [Y-1, Y] x [Z-1, Z]) and its colour
static int8_t coordinates[][4] = {
        {5, -2, 5, 0x44},
        {5, -1, 5, 0x44},
        {5, 0, 5, 0x44},
        {4, -3, 5, 0x33},
        {6, -3, 5, 0x33},
        {5, -3, 4, 0x33},
        {5, -3, 6, 0x33},
};

// Player state. Position is 16.16 fixed point world units. Yaw is in 1/65536 of an angle step,
// with 256 steps per turn, so 24 bits are exactly one turn and wrapping is just turning past 360
// degrees: yaw 0 looks along +z, and increasing yaw turns right, towards +x. Velocities ease towards what the keys ask for (handleInput).
static int32_t  player_x   = 0;
static int32_t  player_y   = -(2L << 16);
static int32_t  player_z   = 0;
static uint24_t player_yaw = 0;
static int24_t  walk_vel   = 0;   // 1/256 units per second, along the view direction
static int24_t  turn_vel   = 0;   // 1/256 steps per second
static uint8_t  snapping   = 0;   // easing into the nearest N/E/S/W


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


// Interlaced fields: instead of copying every drawn LCD row to the row below (deInterlace*, about
// 119k cycles per frame), each frame draws only one field of the back buffer. Pointing the draw
// base 160 bytes into a frame makes graphx row y land on LCD row 2y+1 (the odd field) instead of
// 2y, and the frame's last byte is still graphx row 119, x 159, so nothing spills.
// Order: 0 = buffer 1 even, 1 = buffer 2 even, 2 = buffer 1 odd, 3 = buffer 2 odd. Each buffer is
// drawn every other frame, so its other field is 2 frames old: still images are exact, motion
// shows both positions on alternate rows (combing).
//
// interlace_setting switches this at runtime: 1 = interlaced, 0 = progressive (draw the even field
// and de-interlace it every frame, the old way: no motion blur, about 119k cycles more). It may be
// changed at any time; swapBuffer latches it into frame_interlaced, so a frame is never drawn in
// one mode and finished in the other (de-interlacing a frame drawn into the odd field would copy
// past the end of the buffer).
uint8_t interlace_setting = 1;
static uint8_t frame_interlaced = 1;   // mode of the frame being drawn
static uint8_t field_phase = 0;

static uint24_t phaseBuffer(uint8_t phase) {
    return (phase & 1) ? BUFFER_2 : BUFFER_1;
}

void swapBuffer() {

    // show the frame just drawn, then set up the next frame in the other buffer. Base first, then
    // the acknowledge: the other way round, a refresh in between would set the flag for the old base
    // and waitForBufferScan would return while that frame is still on screen. This way round the
    // worst case is waiting one refresh too many
    setScreenBuffer(phaseBuffer(field_phase));
    lcd_IntAcknowledge = LCD_INT_LNBU;
    field_phase = (field_phase + 1) & 3;
    frame_interlaced = interlace_setting;
    setDrawBuffer(phaseBuffer(field_phase) + ((frame_interlaced && (field_phase & 2)) ? LCD_WIDTH / 2 : 0));

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
// The key only depends on which cell the camera is in, not on its yaw or position inside the cell,
// so the order only changes when the camera crosses into another cell. main() then calls
// sortScene(), which reorders coordinates[] itself. projectScene() keeps boxes in list order, so
// the boxes it keeps come out already sorted and a frame without a re-sort pays nothing for it.
// Walking at full speed crosses about 4 cells per second (up to 6 diagonally); standing still or
// turning never re-sorts.
//
// sortScene() calls sortRecords() in sort.S: a counting sort (stable, no comparisons) over keys
// 0 .. 3 GRID_RANGE, through a scratch copy. 5.3k cycles for the 7-box scene, 59k for 120 boxes.

// gridDist[n] = |n - GRID_RANGE|: the distance in cells along one axis, for the grid index
// n = X + cam_bias_x (the one projectBox() uses). The box spans cell X - 1 and the camera is in
// cell (player x >> 16), so n - GRID_RANGE is their difference (likewise y and z).
// It is indexed by the low byte of n only, so all 256 entries exist. For every box projectBox()
// keeps, n is in [0, 2 GRID_RANGE) and the low byte is n itself. For a culled box the low byte
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

static int8_t  sort_scratch[LEN(coordinates)][4];
static uint8_t sort_key[LEN(coordinates)];
static int24_t sorted_bias_x, sorted_bias_y, sorted_bias_z;  // the camera cell coordinates[] is sorted for

// sort.S: sorts the n records far to near for camera biases bx, by, bz (the low bytes of
// cam_bias_*), through scratch[n] and keys[n]. n must be 1 .. 255
void sortRecords(int8_t (*rec)[4], int8_t (*scratch)[4], uint8_t *keys, uint8_t n,
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

_Static_assert(TABLE_VIEW_HALF_WIDTH * 2 == VIEWPORT_LENGTH, "gen_tables.py culls for a different viewport width");
_Static_assert(SIN_TABLE_LEN == 256, "yaw is looked up as a byte");
_Static_assert(SCREEN_OFFSET == VIEW_X0 && SCREEN_OFFSET + VIEWPORT_LENGTH == VIEW_X1, "engine.h viewport out of sync");
_Static_assert(GFX_LCD_HEIGHT == VIEW_H && GFX_WIDTH_HALF == CENTRE_X && GFX_HEIGHT_HALF == CENTRE_Y, "engine.h screen out of sync");

// shared with project.S, see engine.h (the cam_ scalars are defined in project.S)
int24_t cam_bias_x, cam_bias_y, cam_bias_z;
int24_t rotX_[2 * GRID_LEN], rotZ_[2 * GRID_LEN], camY_[GRID_LEN];


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

    // culling constants, see engine.h
    cam_zlo   = 3 * NEAR_PLANE - cam_kzmin3;
    cam_zspan = (3 * FAR_PLANE - cam_kzmax3) - cam_zlo;
    cam_cxl   = -(4 * cam_kxmax3 + 5 * cam_kzmax3);   // horizontal view is |x| <= 1.25 z (engine.h)
    cam_cxr   = 5 * cam_kzmax3 - 4 * cam_kxmin3;
    cam_cyl   = -(3 * SUB + cam_kzmax3);
}


// A box that passed culling, as projectScene() left it for drawScene()
typedef struct {
    uint8_t kind;               // BOX_* result of projectBox(), never BOX_SKIP
    uint8_t colour;
    int24_t v[BOX_OUT_LEN];     // screen coordinates, layout in engine.h
} projected_box_t;

static projected_box_t projected[LEN(coordinates)];
static uint8_t num_projected;


// Culls and projects every box into projected[], without drawing. Boxes stay in the order of
// coordinates[], which sortScene() keeps far to near. main() calls this before
// waitForBufferScan(), so it runs in time that would otherwise be spent waiting for the LCD
static void projectScene(void) {

    projected_box_t *p = projected;
    uint8_t n = 0;
    for (uint8_t i = 0; i < LEN(coordinates); i++) {
        const int8_t *c = coordinates[i];
        const uint8_t kind = projectBox(c[0], c[1], c[2], p->v);
        if (kind != BOX_SKIP) {
            p->kind = kind;
            p->colour = (uint8_t)c[3];
            p++;
            n++;
        }
    }
    num_projected = n;
}


// Wireframe of one projected box, in the current gfx colour. projectBox() already culled,
// projected and classified it; this only issues the graphx calls.
static void drawBox(const projected_box_t *p) {

    const int24_t *b = p->v;

    switch (p->kind) {

    // facing N/E/S/W: near and far faces are screen rectangles. 2 rectangles (fast spans) + 4 lines
    case BOX_AXIS_NOCLIP: {
        const int24_t nl = b[0], nr = b[1], nt = b[2], nb = b[3];
        const int24_t fl = b[4], fr = b[5], ft = b[6], fb = b[7];
        gfx_Rectangle_NoClip(nl, nt, nr - nl + 1, nb - nt + 1);
        gfx_Rectangle_NoClip(fl, ft, fr - fl + 1, fb - ft + 1);
        gfx_Line_NoClip(nl, nt, fl, ft);
        gfx_Line_NoClip(nr, nt, fr, ft);
        gfx_Line_NoClip(nl, nb, fl, fb);
        gfx_Line_NoClip(nr, nb, fr, fb);
        break;
    }

    case BOX_AXIS_CLIP: {
        const int24_t nl = b[0], nr = b[1], nt = b[2], nb = b[3];
        const int24_t fl = b[4], fr = b[5], ft = b[6], fb = b[7];
        // the far face is often still fully inside
        if (fl >= SCREEN_OFFSET && fr < SCREEN_OFFSET + VIEWPORT_LENGTH && ft >= 0 && fb < GFX_LCD_HEIGHT) {
            gfx_Rectangle_NoClip(fl, ft, fr - fl + 1, fb - ft + 1);
        } else {
            gfx_Rectangle(fl, ft, fr - fl + 1, fb - ft + 1);
        }
        gfx_Rectangle(nl, nt, nr - nl + 1, nb - nt + 1);
        gfx_Line(nl, nt, fl, ft);
        gfx_Line(nr, nt, fr, ft);
        gfx_Line(nl, nb, fl, fb);
        gfx_Line(nr, nb, fr, fb);
        break;
    }

    // any other yaw: 4 vertical edges (vertical on screen too, since there is only yaw) + 8 lines
    case BOX_GEN_NOCLIP: {
        const int24_t *sx = b, *st = b + 4, *sb = b + 8;
        for (uint8_t k = 0; k < 4; k++) {
            gfx_VertLine_NoClip(sx[k], st[k], sb[k] - st[k] + 1);
        }
        gfx_Line_NoClip(sx[0], st[0], sx[1], st[1]);
        gfx_Line_NoClip(sx[2], st[2], sx[3], st[3]);
        gfx_Line_NoClip(sx[0], st[0], sx[2], st[2]);
        gfx_Line_NoClip(sx[1], st[1], sx[3], st[3]);
        gfx_Line_NoClip(sx[0], sb[0], sx[1], sb[1]);
        gfx_Line_NoClip(sx[2], sb[2], sx[3], sb[3]);
        gfx_Line_NoClip(sx[0], sb[0], sx[2], sb[2]);
        gfx_Line_NoClip(sx[1], sb[1], sx[3], sb[3]);
        break;
    }

    case BOX_GEN_CLIP: {
        const int24_t *sx = b, *st = b + 4, *sb = b + 8;
        for (uint8_t k = 0; k < 4; k++) {
            gfx_VertLine(sx[k], st[k], sb[k] - st[k] + 1);
        }
        gfx_Line(sx[0], st[0], sx[1], st[1]);
        gfx_Line(sx[2], st[2], sx[3], st[3]);
        gfx_Line(sx[0], st[0], sx[2], st[2]);
        gfx_Line(sx[1], st[1], sx[3], st[3]);
        gfx_Line(sx[0], sb[0], sx[1], sb[1]);
        gfx_Line(sx[2], sb[2], sx[3], sb[3]);
        gfx_Line(sx[0], sb[0], sx[2], sb[2]);
        gfx_Line(sx[1], sb[1], sx[3], sb[3]);
        break;
    }

    default:  // BOX_SKIP: never stored by projectScene()
        break;
    }
}


// draws what projectScene() kept, far to near
static void drawScene(void) {

    const projected_box_t *p = projected;
    for (uint8_t n = num_projected; n != 0; n--, p++) {
        gfx_SetColor(p->colour);
        drawBox(p);
    }
}


// ---- input ----
// Speeds are per second and scaled by the frame time, so they do not change with FPS.
// Per-frame code, so the 32-bit maths here does not matter.

#define WALK_SPEED   (4 * 256)        // units/s, in 1/256 units
#define WALK_ACCEL   (32 * 256)       // units/s^2: full speed (and a full stop) in 1/8 s
#define TURN_SPEED   (96 * 256)       // steps/s (96 = 135 deg/s), in 1/256 steps
#define TURN_ACCEL   (768L * 256)     // steps/s^2: full speed in 1/8 s
#define SNAP_RANGE   (8L << 16)       // after a turn, ease into the nearest N/E/S/W if within 8 steps (11 deg). 0 disables
#define SNAP_START   (16 * 256)       // ... once the turn has slowed below 16 steps/s
#define SNAP_GAIN    10               // snap speed = 10 * remaining angle per second (time constant 0.1 s)
#define SNAP_DONE    (1L << 13)       // finish exactly once within 1/8 step

_Static_assert((int32_t)MAX_DT * TURN_SPEED < 0x7fffffff / 2, "per-frame turn step overflows");


// moves v towards target by at most step
static int24_t approach(int24_t v, int24_t target, int24_t step) {
    if (v < target) {
        return (target - v < step) ? target : v + step;
    }
    return (v - target < step) ? target : v - step;
}


// arrows is kb_Data[7]: left/right turn, up/down walk along the view direction. dt is in clock ticks
static void updateMotion(uint8_t arrows, uint24_t dt) {

    // ---- walking ----
    int24_t walk_target = 0;
    if (arrows & kb_Up) {
        walk_target = WALK_SPEED;
    } else if (arrows & kb_Down) {
        walk_target = -WALK_SPEED;
    }
    walk_vel = approach(walk_vel, walk_target, (int24_t)(((int32_t)WALK_ACCEL * dt) / CLOCKS_PER_SEC));

    if (walk_vel != 0) {
        const uint8_t angle = (uint8_t)(player_yaw >> 16);
        const int32_t dist = ((int32_t)walk_vel * dt) >> 7;  // 1/65536 units: vel/256 * dt/32768 * 65536
        player_x += (dist * sinTable[angle]) / SUB;
        player_z += (dist * sinTable[(uint8_t)(angle + 64)]) / SUB;
    }

    // ---- turning ----
    const int24_t turn_step = (int24_t)((TURN_ACCEL * dt) / CLOCKS_PER_SEC);
    if (arrows & (kb_Left | kb_Right)) {
        snapping = 0;
        turn_vel = approach(turn_vel, (arrows & kb_Left) ? -TURN_SPEED : TURN_SPEED, turn_step);
    } else {
        // signed distance to the nearest multiple of 64 steps, in 1/65536 steps
        const int32_t off = (int32_t)((player_yaw + (32L << 16)) & ((64L << 16) - 1)) - (32L << 16);
        if (!snapping && off != 0 && off <= SNAP_RANGE && off >= -SNAP_RANGE &&
            turn_vel < SNAP_START && turn_vel > -SNAP_START) {
            snapping = 1;
        }
        if (snapping) {
            if (off < SNAP_DONE && off > -SNAP_DONE) {
                player_yaw -= off;  // exactly N/E/S/W: drawBox takes the fast path
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
    updateMotion(kb_Data[7], dt);

    // [mode] toggles interlacing, once per press
    static uint8_t mode_held = 0;
    const uint8_t mode_down = kb_Data[1] & kb_Mode;
    if (mode_down && !mode_held) {
        interlace_setting = !interlace_setting;
    }
    mode_held = mode_down;
}


void preRenderScreen() {
    
    uint24_t current_buffer = draw_buffer_ctrl;
    setDrawBuffer(DBL_BUF1 - SCREEN_OFFSET);

    // sky. the ground grid will come back once the new projection exists
    gfx_SetColor(GFX_BLUE);
    gfx_FillRectangle_NoClip(SCREEN_OFFSET, 0, VIEWPORT_LENGTH, GFX_LCD_HEIGHT);

    setDrawBuffer(current_buffer);

}


// Performance: this costs about 159k cycles (3.3 ms) per frame at full width (127k at the old 128
// columns), more than the whole scene draw. memcpy is an ldir, about 8 cycles per byte for these
// 19,200 bytes, and that is already the
// cheapest way to copy on the CE. A copy only pays when the background holds an actual picture:
// while it is a solid colour (or one colour per row), an asm fill with push measured 58k cycles at
// 128 columns
// (about 3.8 per byte, since it only writes). A background drawn every frame is cheaper still if
// it replaces this clear instead of being drawn on top of it.
void blitPrerenderScreen(uint24_t buffer) {

    const uint8_t *src = (const uint8_t *)((buffer > 1) ? DBL_BUF2 + (buffer - 2) * (LCD_WIDTH / 2)
                                                        : DBL_BUF1 + buffer * (LCD_WIDTH / 2));
    uint8_t *dst = (uint8_t *)draw_buffer_ctrl + SCREEN_OFFSET;

    // pointer steps instead of y * LCD_WIDTH, which was a library multiply per row
    for (uint8_t y = 0; y < LCD_HEIGHT / 2; y++) {
        memcpy(dst, src, VIEWPORT_LENGTH);
        dst += LCD_WIDTH;
        src += LCD_WIDTH;
    }
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


// D: this frame's scene (projection + graphx). F: the previous frame's total work (everything but the wait)
static void drawHud(uint8_t fps, uint24_t draw_cycles, uint24_t frame_cycles) {

    fontlib_SetCursorPosition(3,3);
    fontlib_DrawUInt(fps, 2);

    drawMs(11, "d ", draw_cycles);
    drawMs(19, "f ", frame_cycles);

    // // buffer number, top left (in the side bar when there is one)
    // fontlib_SetTransparency(false);
    // fontlib_SetColors(GFX_BLUE, GFX_BLACK);
    // fontlib_SetCursorPosition(3,3);
    // fontlib_DrawUInt((field_phase & 1) + 1, 1);
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
    initGridDist();
    setupCamera();
    sortScene();
    preRenderScreen();

    // both fields of both buffers start as background: the first frames only draw one field each
    setDrawBuffer(BUFFER_2);
    blitPrerenderScreen(0);
    deInterlaceViewportBuffer(BUFFER_2);
    setDrawBuffer(BUFFER_1);
    blitPrerenderScreen(0);
    deInterlaceViewportBuffer(BUFFER_1);

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

        // cull and project before the wait: it only reads the camera, not the buffer on screen
        const uint24_t project_start = CYCLES();
        projectScene();
        const uint24_t project_cycles = CYCLES() - project_start;

        // this eliminates flickering. Idle time, so it is left out of the frame time
        uint24_t wait_cycles = CYCLES();
        waitForBufferScan();
        wait_cycles = CYCLES() - wait_cycles;

        // blit prerendered display
        blitPrerenderScreen(0);

        const uint24_t draw_start = CYCLES();

        drawScene();

        // scene time: projectScene (before the wait) + graphx
        const uint24_t draw_cycles = CYCLES() - draw_start + project_cycles;

        // FPS and timings ("make hud" builds only)
        drawHud(fps, draw_cycles, frame_cycles);

        // progressive frames copy the drawn rows to the rows below. Interlaced ones skip it (see swapBuffer)
        if (!frame_interlaced) {
            deInterlaceViewportBuffer(draw_buffer_ctrl);
        }

        swapBuffer();

        frame_cycles = CYCLES() - frame_start - wait_cycles;
    }

    // program end
    kb_SetMode(kb_mode);
    gfx_End();
    return 0;
}