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


#define SCREEN_OFFSET ((LCD_WIDTH - 128 * 2) / 4)  // screen x offset
#define draw_buffer_ctrl lcd_LpBase
#define screen_buffer_ctrl lcd_UpBase

#define DBL_BUF1 0xd52c00
#define DBL_BUF2 0xD5C200
#define BUFFER_1 0xD40000
#define BUFFER_2 0xD49600

#define LEN(arr) ((int) (sizeof (arr) / sizeof (arr)[0]))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

// debug timing. compiled out entirely in release builds (NDEBUG), since clock() is not free
#ifndef NDEBUG
#define DBG_TIMER_START() unsigned long dbg_timer = clock()
#define DBG_LAP(label) do { dbg_printf(label ": %lu\n", clock() - dbg_timer); dbg_timer = clock(); } while (0)
#else
#define DBG_TIMER_START() do {} while (0)
#define DBG_LAP(label) do {} while (0)
#endif

#define COORDINATE_LIST_LENGTH 4  // number of inside elements

#define GFX_HEIGHT_HALF 60
#define GFX_WIDTH_HALF  80
#define GFX_LCD_WIDTH 160
#define GFX_LCD_HEIGHT 120
#define VIEWPORT_LENGTH 128

#define NO_CLIP      0
#define CLIP         1
#define SKIP_RENDER  2

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


// font data
static uint8_t my_font_data[] = {
    #include "../font/myfont.inc"
};

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

// takes about 0.00025 seconds per item to sort
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
// with 256 steps per turn (only the low 24 bits matter): yaw 0 looks along +z, and increasing yaw
// turns right, towards +x. Velocities ease towards what the keys ask for (handleInput).
static int32_t  player_x   = 0;
static int32_t  player_y   = -(2L << 16);
static int32_t  player_z   = 0;
static uint32_t player_yaw = 0;
static int24_t  walk_vel   = 0;   // 1/256 units per second, along the view direction
static int24_t  turn_vel   = 0;   // 1/256 steps per second
static uint8_t  snapping   = 0;   // easing into the nearest N/E/S/W
static int24_t  cam_cell_x;       // grid cell the camera is in, set by setupCamera() (for the sort)


void waitForBufferScan(void) {

    volatile uint24_t n = lcd_UpBaseCurr;

    usleep(1);

    while (n != lcd_UpBaseCurr) {

        n = lcd_UpBaseCurr;
        usleep(1);
    }
}


void deInterlaceFullBuffer(uint24_t buffer) {
    for(uint8_t y = 0; y < LCD_HEIGHT / 2; y++) {
        memcpy(&gfx_vbuffer[y][LCD_WIDTH / 2], &gfx_vbuffer[y][0], LCD_WIDTH / 2);}
}


void deInterlaceViewportBuffer(uint24_t buffer) {
    for(uint8_t y = 0; y < LCD_HEIGHT / 2; y++) {
        memcpy(&gfx_vbuffer[y][LCD_WIDTH / 2 + SCREEN_OFFSET], &gfx_vbuffer[y][SCREEN_OFFSET], LCD_WIDTH / 2 - SCREEN_OFFSET * 2);}
}


void setDrawBuffer(uint24_t buffer) {
    draw_buffer_ctrl = buffer;
}


void setScreenBuffer(uint24_t buffer) {
    screen_buffer_ctrl = buffer;
}


void swapBuffer() {

    // swap roles of the buffers
    if (draw_buffer_ctrl == BUFFER_1) {
        setDrawBuffer(BUFFER_2);
        setScreenBuffer(BUFFER_1);
    } else {
        setDrawBuffer(BUFFER_1);
        setScreenBuffer(BUFFER_2);
    }

}


static int24_t absInt(int24_t v) {
    return v < 0 ? -v : v;
}


// painter's order in world space. only matters once boxes are filled, and with rotation it
// should eventually sort by camera depth instead
static int compareCoordinates(const void *a, const void *b) {

    const int8_t *coord1 = (int8_t *)a;
    const int8_t *coord2 = (int8_t *)b;

    if ((coord2[2] - coord1[2]) != 0) {
        return (coord2[2] - coord1[2]);
    }

    return (absInt(coord2[0] - cam_cell_x) + absInt(coord2[1])) - (absInt(coord1[0] - cam_cell_x) + absInt(coord1[1]));
}


static void sortCoordinateList(int8_t coordinates[][COORDINATE_LIST_LENGTH], size_t numCoordinates) {

    qsort(coordinates, numCoordinates, sizeof(coordinates[0]), compareCoordinates);
}


// ---- camera ----
// Camera space: x right, y down, z forward, in 1/SUB world units. setupCamera() builds the
// per-frame state in engine.h once per frame (C is fine there). The per-box work is in
// project.S, because clang turns every multiply, right shift and even AND into a library call.

#define MAX_DT  (CLOCKS_PER_SEC / 8)  // clamp frame time so a stall does not teleport the player

_Static_assert(TABLE_FOCAL_LENGTH * 2 == VIEWPORT_LENGTH, "culling assumes |x| <= z is the horizontal view (90 deg FOV)");
_Static_assert(SIN_TABLE_LEN == 256, "yaw is looked up as a byte");
_Static_assert(SCREEN_OFFSET == VIEW_X0 && SCREEN_OFFSET + VIEWPORT_LENGTH == VIEW_X1, "engine.h viewport out of sync");
_Static_assert(GFX_LCD_HEIGHT == VIEW_H && GFX_WIDTH_HALF == CENTRE_X && GFX_HEIGHT_HALF == CENTRE_Y, "engine.h screen out of sync");

// shared with project.S, see engine.h
int24_t cam_bias_x, cam_bias_y, cam_bias_z;
int24_t rotX_[2 * GRID_LEN], rotZ_[2 * GRID_LEN], camY_[GRID_LEN];
int24_t cam_cos3, cam_sin3;
int24_t cam_kxmin3, cam_kxmax3, cam_kzmin3, cam_kzmax3;
int24_t cam_zlo, cam_zspan, cam_cxl, cam_cxr, cam_cyl;
uint8_t cam_cardinal;
int24_t box_out[12];


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
    cam_cell_x = (int24_t)(player_x >> 16);
    const int24_t cell_y = (int24_t)(player_y >> 16);
    const int24_t cell_z = (int24_t)(player_z >> 16);
    cam_bias_x = GRID_RANGE - 1 - cam_cell_x;
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
    cam_cxl   = -(cam_kxmax3 + cam_kzmax3);
    cam_cxr   = cam_kzmax3 - cam_kxmin3;
    cam_cyl   = -(3 * SUB + cam_kzmax3);
}


// Wireframe of the world voxel [X-1, X] x [Y-1, Y] x [Z-1, Z], in the current gfx colour.
// projectBox() culls, projects and classifies it; this only issues the graphx calls.
static void drawBox(int24_t X, int24_t Y, int24_t Z) {

    const int24_t *b = box_out;

    switch (projectBox(X, Y, Z)) {

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

    default:  // BOX_SKIP
        break;
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


static void handleInput(uint24_t dt) {
    kb_Scan();
    updateMotion(kb_Data[7], dt);
}


void preRenderScreen() {
    
    uint24_t current_buffer = draw_buffer_ctrl;
    setDrawBuffer(DBL_BUF1-16);

    gfx_SetClipRegion(16, 0, 128+16, GFX_LCD_HEIGHT);

    // sky. the ground grid will come back once the new projection exists
    gfx_SetColor(GFX_BLUE);
    gfx_FillRectangle_NoClip(SCREEN_OFFSET, 0, VIEWPORT_LENGTH, GFX_LCD_HEIGHT);

    setDrawBuffer(current_buffer);
    gfx_SetClipRegion(SCREEN_OFFSET, 0, SCREEN_OFFSET + 128, GFX_LCD_HEIGHT);

}


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


void init_fontlib() {

    // load font
    fontlib_font_t* my_font = (fontlib_font_t *)my_font_data;
    fontlib_SetFont(my_font, 0);
    
    fontlib_SetTransparency(true);
    fontlib_SetWindowFullScreen();
    fontlib_SetColors(0x00,0xff);

}


void init_screen() {

    // initialize graphx
    gfx_Begin();
    
    // set palette
    gfx_SetPalette(mypalette, sizeof mypalette, 0);
    gfx_SetClipRegion(SCREEN_OFFSET, 0, SCREEN_OFFSET + 128, GFX_LCD_HEIGHT);
    lcd_Control = 0x13925; // 4 bpp mode

    // set screen display to blank double buffer
    setScreenBuffer(DBL_BUF2);

    // initialize second buffer
    setDrawBuffer(BUFFER_2);
    gfx_FillRectangle_NoClip(0, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
    gfx_FillRectangle_NoClip(VIEWPORT_LENGTH+SCREEN_OFFSET, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
    deInterlaceFullBuffer(BUFFER_2);

    // initialize first buffer
    setDrawBuffer(BUFFER_1);
    gfx_FillRectangle_NoClip(0, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
    gfx_FillRectangle_NoClip(VIEWPORT_LENGTH+SCREEN_OFFSET, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
    deInterlaceFullBuffer(BUFFER_2);

    // // initialize first double buffer
    // setDrawBuffer(DBL_BUF1);
    // gfx_FillRectangle_NoClip(0, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
    // gfx_FillRectangle_NoClip(VIEWPORT_LENGTH+SCREEN_OFFSET, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
    // // de-interlacing is not needed, as this is a final rendering stage step

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
    init_fontlib();
    init_screen();
    
    // prep 3d environment
    setupCamera();
    sortCoordinateList(coordinates, LEN(coordinates));
    preRenderScreen();

    // FPS calculation
    uint8_t frame_count = 0;
    clock_t start_time = clock();
    clock_t last_time = start_time;
    uint8_t fps = 0;

    
    timer_Set(2, 0);
    timer_Enable(2, TIMER_CPU, TIMER_NOINT, TIMER_UP);

    // while clear not pressed
    while (!(kb_Data[6] & kb_Clear)) {
        
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

            // sort objects
            sortCoordinateList(coordinates, LEN(coordinates));
        }

        handleInput(dt);
        setupCamera();

        // this eliminates flickering
        waitForBufferScan();

        // blit prerendered display
        blitPrerenderScreen(0);

        timer_Set(2, 0);

        DBG_TIMER_START();

        // draw scene
        for (uint8_t i = 0; i < LEN(coordinates); i++) {
            gfx_SetColor(coordinates[i][3]);

            drawBox(coordinates[i][0], coordinates[i][1], coordinates[i][2]);
        }

        DBG_LAP("scene draw");
        dbg_printf("\n");

        // draw FPS counter
        fontlib_SetTransparency(true);
        fontlib_SetForegroundColor(GFX_BLACK);
        fontlib_SetCursorPosition(20,4);
        fontlib_DrawUInt(fps, 2);

        // gfx_SetColor(GFX_BLACK);
        // gfx_FillRectangle_NoClip(0,0,8,16);
        // fontlib_SetForegroundColor

        fontlib_SetTransparency(false);
        fontlib_SetColors(GFX_BLUE, GFX_BLACK);
        fontlib_SetCursorPosition(3,3);

        if (draw_buffer_ctrl == BUFFER_1) {
            fontlib_DrawUInt(1,1);
        } else {
            fontlib_DrawUInt(2,1);
        }

        // de-interlace viewport
        deInterlaceViewportBuffer(draw_buffer_ctrl);

        // swap roles of the buffers
        swapBuffer();

    }

    // program end
    gfx_End();
    return 0;
}