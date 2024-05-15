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


#define SCREEN_OFFSET ((LCD_WIDTH - 128 * 2) / 4)  // screen x offset
#define draw_buffer_ctrl lcd_LpBase
#define screen_buffer_ctrl lcd_UpBase
#define gfx_vram (**(uint8_t(**)[240][320])0xD40000)  // gfx_vram define override
#define DBL_BUF1 0xD49600
#define DBL_BUF2 0xD5C200
#define BUFFER_1 0xD40000
#define BUFFER_2 0xd52c00

#define FOV 0.8
#define LEN(arr) ((int) (sizeof (arr) / sizeof (arr)[0]))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#define TRAPEZOID_QUALITY 64

#define NONE 0
#define COORDINATE_LIST_LENGTH 4  // number of inside elements

#define GFX_HEIGHT_HALF 60
#define GFX_WIDTH_HALF  80
#define GFX_LCD_WIDTH 160
#define GFX_LCD_HEIGHT 120
#define VIEWPORT_LENGTH 128

#define PANEL_BOTTOM  0
#define PANEL_LEFT    1
#define PANEL_BACK    2

#define WIREFRAME               0
#define VISIBLE_ONLY_WIREFRAME  1
#define FILLED                  2

#define BASE_FILL 0
#define BASE_PART_FILL 1
#define BASE_RENDER_DIST 2

#define NO_CLIP      0
#define CLIP         1
#define SKIP_RENDER  2

// all exclusive. rerun powAZx50.py when values are changed
#define RENDER_DIST_X                16   // actual limit is double this value
#define RENDER_DIST_Y                10   // same as above
#define RENDER_DIST_Z_BACK           20   // back from the camera - used for calculations
#define RENDER_DIST_Z_FRONT          30   // in front of the camera
#define VISIBLE_RENDER_DIST_Z_BACK   3    // used solely for optimization
#define VISIBLE_RENDER_DIST_Z_FRONT  15   //

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

// simple rectangular prism. cone-shape would be best in the future
#define RENDER_DISTANCE_ALGORITHM (z > -VISIBLE_RENDER_DIST_Z_BACK) && (z < VISIBLE_RENDER_DIST_Z_FRONT) && (x < RENDER_DIST_X) && (x > -RENDER_DIST_X) && (y < RENDER_DIST_Y) && (y > -RENDER_DIST_Y)


// auto-generated, do not tamper
const static uint8_t powAZx50list[] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 238, 191, 153, 122, 98, 78, 62, 50, 40, 32, 26, 20, 16, 13, 10, 8, 7, 5, 4, 3, 3, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};

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

static int8_t player_x = 0;
static int8_t player_y = -2;
static int8_t player_z = 0;


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


static int compareCoordinates(const void *a, const void *b) {

    const int8_t *coord1 = (int8_t *)a;
    const int8_t *coord2 = (int8_t *)b;
    
    
    if ((coord2[2] - coord1[2]) != 0) {
        return (coord2[2] - coord1[2]);
    }
    
    return (abs(coord2[0] - player_x) + abs(coord2[1] - player_y)) - (abs(coord1[0] - player_x) + abs(coord1[1] - player_y));
}


static void sortCoordinateList(int8_t coordinates[][COORDINATE_LIST_LENGTH], size_t numCoordinates) {

    qsort(coordinates, numCoordinates, sizeof(coordinates[0]), compareCoordinates);
}


static void drawBox(int8_t x, int8_t y, int8_t z, uint8_t type, uint8_t outline_color) {

    // render distance calculations. needs improvement
    if (RENDER_DISTANCE_ALGORITHM) {

        // get pre-calculated values
        const uint8_t powAZx50 = powAZx50list[z+RENDER_DIST_Z_BACK];
        const uint8_t powAZp1x50 = powAZx50list[z+RENDER_DIST_Z_BACK-1];

        // calculate values - data type needs to go from -1 to 241 height, 321 width
        // potential optimization here. willing to make viewport 255x255

        const int24_t w = (x*powAZx50) + GFX_WIDTH_HALF;
        const int24_t v = (y*powAZx50) + GFX_HEIGHT_HALF;
        const int24_t u = ((x-1)*powAZx50) + GFX_WIDTH_HALF;
        const int24_t t = ((y-1)*powAZx50) + GFX_HEIGHT_HALF;
        const int24_t r = (x*powAZp1x50) + GFX_WIDTH_HALF;
        const int24_t q = (y*powAZp1x50) + GFX_HEIGHT_HALF;
        const int24_t p = ((x-1)*powAZp1x50) + GFX_WIDTH_HALF;
        const int24_t o = ((y-1)*powAZp1x50) + GFX_HEIGHT_HALF;

        // tiny optimization. not sure if this has any benefit
        const uint8_t wup1 = w-u+1;
        const uint8_t rpp1 = r-p+1;

        // 0 = on screen
        // 1 = partially on screen
        // 2 = off screen
        uint8_t clip = CLIP;

        // determines route of action for clipping
        if ((r < SCREEN_OFFSET+VIEWPORT_LENGTH) && (p >= SCREEN_OFFSET) && (o >= 0) && (q < GFX_LCD_HEIGHT)) {
            clip = NO_CLIP;
        } else if ((u > SCREEN_OFFSET+VIEWPORT_LENGTH) || (w < SCREEN_OFFSET) || (t > GFX_LCD_HEIGHT) || (v < 0)) {
            clip = SKIP_RENDER;
        }

        
        switch (type) {
        case WIREFRAME:
            switch (clip) {

            case NO_CLIP:  // if fully on screen
                gfx_Rectangle_NoClip(u, t, wup1, wup1);
                gfx_Rectangle_NoClip(p,o,rpp1, rpp1);
                gfx_Line_NoClip(r, q, w, v);
                gfx_Line_NoClip(r, o, w, t);
                gfx_Line_NoClip(p, o, u, t);
                gfx_Line_NoClip(p, q, u, v);
                break;

            case CLIP:  // if partly off screen
                gfx_Rectangle(u, t, wup1, wup1);
                gfx_Rectangle(p,o,rpp1, rpp1);
                gfx_Line(r, q, w, v);
                gfx_Line(r, o, w, t);
                gfx_Line(p, o, u, t);
                gfx_Line(p, q, u, v);
                break;
            
            default:  // if off screen
                break;
            }
            break;
        case VISIBLE_ONLY_WIREFRAME:
            switch (clip) {

            case NO_CLIP:  // if fully on screen
                if (u < GFX_WIDTH_HALF && v > GFX_HEIGHT_HALF) {
                    gfx_Line_NoClip(r, q, w, v);
                    gfx_Line_NoClip(r, o, w, t);
                    gfx_Line_NoClip(p, o, u, t);
                    gfx_VertLine_NoClip(w, t, wup1);
                    gfx_HorizLine_NoClip(u, t, wup1);
                } else if (u >= GFX_WIDTH_HALF && v > GFX_HEIGHT_HALF) {
                    gfx_Line_NoClip(r, o, w, t);
                    gfx_Line_NoClip(p, o, u, t);
                    gfx_Line_NoClip(p, q, u, v);
                    gfx_VertLine_NoClip(u, t, wup1);
                    gfx_HorizLine_NoClip(u, t, wup1);
                } else if (u >= GFX_WIDTH_HALF && v <= GFX_HEIGHT_HALF) {
                    gfx_Line_NoClip(r, q, w, v);
                    gfx_Line_NoClip(p, q, u, v);
                    gfx_Line_NoClip(p, o, u, t);
                    gfx_HorizLine_NoClip(u, v, wup1);
                    gfx_VertLine_NoClip(u, t, wup1);
                } else {
                    gfx_Line_NoClip(r, o, w, t);
                    gfx_Line_NoClip(p, q, u, v);
                    gfx_Line_NoClip(r, q, w, v);
                    gfx_VertLine_NoClip(w, t, wup1);
                    gfx_HorizLine_NoClip(u, v, wup1);
                }
                
                // render outline
                gfx_Rectangle(p, o, rpp1, rpp1);

                break;

            case CLIP:  // if partly off screen
                if (u < GFX_WIDTH_HALF && v > GFX_HEIGHT_HALF) {  // bottom left
                    gfx_Line(r, q, w, v);
                    gfx_Line(r, o, w, t);
                    gfx_Line(p, o, u, t);
                    gfx_VertLine(w, t, wup1);
                    gfx_HorizLine(u, t, wup1);
                } else if (u >= GFX_WIDTH_HALF && v > GFX_HEIGHT_HALF) {  // bottom right
                    gfx_Line(r, o, w, t);
                    gfx_Line(p, o, u, t);
                    gfx_Line(p, q, u, v);
                    gfx_VertLine(u, t, wup1);
                    gfx_HorizLine(u, t, wup1);
                } else if (u >= GFX_WIDTH_HALF && v <= GFX_HEIGHT_HALF) {  // top right
                    gfx_Line(r, q, w, v);
                    gfx_Line(p, q, u, v);
                    gfx_Line(p, o, u, t);
                    gfx_HorizLine(u, v, wup1);
                    gfx_VertLine(u, t, wup1);
                } else {  // top left
                    gfx_Line(r, o, w, t);
                    gfx_Line(p, q, u, v);
                    gfx_Line(r, q, w, v);
                    gfx_VertLine(w, t, wup1);
                    gfx_HorizLine(u, v, wup1);
                }

                // render outline
                gfx_Rectangle(p, o, rpp1, rpp1);
                
                break;
            
            default:  // if off screen
                break;
            }
            break;
        case FILLED:

            switch (clip) {

            case 0:  // if fully on screen

                // fill area
                if (u < GFX_WIDTH_HALF && v > GFX_HEIGHT_HALF) {
                    gfx_FillTriangle_NoClip(w, v, w, t, u, t);
                    gfx_FillTriangle_NoClip(p, q, r, q, p, o);
                    gfx_FillTriangle_NoClip(u, t, w, v, p, o);
                    gfx_FillTriangle_NoClip(p, o, r, q, w, v);
                    gfx_SetColor(outline_color);
                    gfx_Line_NoClip(r, q, w, v);
                    gfx_Line_NoClip(r, o, w, t);
                    gfx_Line_NoClip(p, o, u, t);
                    gfx_VertLine_NoClip(w, t, wup1);
                    gfx_HorizLine_NoClip(u, t, wup1);
                } else if (u >= GFX_WIDTH_HALF && v > GFX_HEIGHT_HALF) {
                    gfx_FillTriangle_NoClip(w, t, u, t, u, v);
                    gfx_FillTriangle_NoClip(p, q, r, o, r, q);
                    gfx_FillTriangle_NoClip(u, v, w, t, r, o);
                    gfx_FillTriangle_NoClip(p, q, r, o, u, v);
                    gfx_SetColor(outline_color);
                    gfx_Line_NoClip(r, o, w, t);
                    gfx_Line_NoClip(p, o, u, t);
                    gfx_Line_NoClip(p, q, u, v);
                    gfx_VertLine_NoClip(u, t, wup1);
                    gfx_HorizLine_NoClip(u, t, wup1);
                } else if (u >= GFX_WIDTH_HALF && v <= GFX_HEIGHT_HALF) {
                    gfx_FillTriangle_NoClip(u, t, u, v, w, v);
                    gfx_FillTriangle_NoClip(r, q, r, o, p, o);
                    gfx_FillTriangle_NoClip(r, q, p, o, w, v);
                    gfx_FillTriangle_NoClip(w, v, u, t, p, o);
                    gfx_SetColor(outline_color);
                    gfx_Line_NoClip(r, q, w, v);
                    gfx_Line_NoClip(p, q, u, v);
                    gfx_Line_NoClip(p, o, u, t);
                    gfx_HorizLine_NoClip(u, v, wup1);
                    gfx_VertLine_NoClip(u, t, wup1);
                } else {
                    gfx_FillTriangle_NoClip(w, t, w, v, u, v);
                    gfx_FillTriangle_NoClip(r, o, p, o, p, q);
                    gfx_FillTriangle_NoClip(u, v, w, t, p, q);
                    gfx_FillTriangle_NoClip(p, q, r, o, w, t);
                    gfx_SetColor(outline_color);
                    gfx_Line_NoClip(r, o, w, t);
                    gfx_Line_NoClip(p, q, u, v);
                    gfx_Line_NoClip(r, q, w, v);
                    gfx_VertLine_NoClip(w, t, wup1);
                    gfx_HorizLine_NoClip(u, v, wup1);
                }
                
                // render outline
                gfx_Rectangle(p, o, rpp1, rpp1);

                break;

            case 1:  // if partly off screen

                // fill area
                if (u < GFX_WIDTH_HALF && v > GFX_HEIGHT_HALF) {  // bottom left
                    gfx_FillTriangle(w, v, w, t, u, t);
                    gfx_FillTriangle(p, q, r, q, p, o);
                    gfx_FillTriangle(u, t, w, v, p, o);
                    gfx_FillTriangle(p, o, r, q, w, v);
                    gfx_SetColor(outline_color);
                    gfx_Line(r, q, w, v);
                    gfx_Line(r, o, w, t);
                    gfx_Line(p, o, u, t);
                    gfx_VertLine(w, t, wup1);
                    gfx_HorizLine(u, t, wup1);
                } else if (u >= GFX_WIDTH_HALF && v > GFX_HEIGHT_HALF) {  // bottom right
                    gfx_FillTriangle(w, t, u, t, u, v);
                    gfx_FillTriangle(p, q, r, o, r, q);
                    gfx_FillTriangle(u, v, w, t, r, o);
                    gfx_FillTriangle(p, q, r, o, u, v);
                    gfx_SetColor(outline_color);
                    gfx_Line(r, o, w, t);
                    gfx_Line(p, o, u, t);
                    gfx_Line(p, q, u, v);
                    gfx_VertLine(u, t, wup1);
                    gfx_HorizLine(u, t, wup1);
                } else if (u >= GFX_WIDTH_HALF && v <= GFX_HEIGHT_HALF) {  // top right
                    gfx_FillTriangle(u, t, u, v, w, v);
                    gfx_FillTriangle(r, q, r, o, p, o);
                    gfx_FillTriangle(r, q, p, o, w, v);
                    gfx_FillTriangle(w, v, u, t, p, o);
                    gfx_SetColor(outline_color);
                    gfx_Line(r, q, w, v);
                    gfx_Line(p, q, u, v);
                    gfx_Line(p, o, u, t);
                    gfx_HorizLine(u, v, wup1);
                    gfx_VertLine(u, t, wup1);
                } else {  // top left
                    gfx_FillTriangle(w, t, w, v, u, v);
                    gfx_FillTriangle(r, o, p, o, p, q);
                    gfx_FillTriangle(u, v, w, t, p, q);
                    gfx_FillTriangle(p, q, r, o, w, t);
                    gfx_SetColor(outline_color);
                    gfx_Line(r, o, w, t);
                    gfx_Line(p, q, u, v);
                    gfx_Line(r, q, w, v);
                    gfx_VertLine(w, t, wup1);
                    gfx_HorizLine(u, v, wup1);
                }

                // render outline
                gfx_Rectangle(p, o, rpp1, rpp1);
                
                break;
            
            default:  // if off screen
                break;
            break;
            }
        }
    }
}


static void drawTrapezoid(int x1, int x2, int x3, int x4, int y1, int y2) {


    int n1 = (((x2-x1)*TRAPEZOID_QUALITY/(y1-y2)));
    int n2 = (((x3-x4)*TRAPEZOID_QUALITY/(y1-y2)));

    int j = x1 * TRAPEZOID_QUALITY;
    int k = x4 * TRAPEZOID_QUALITY;
    int s = 0;

    if (y1 < y2) {
        for (int i = y1; i < y2; i++) {
            
            j -= n1;
            k -= n2;
            
            if (i < 0 || i > GFX_LCD_HEIGHT)
            continue;

            s = j / TRAPEZOID_QUALITY;
            gfx_HorizLine(s, i, k / TRAPEZOID_QUALITY-s);
        }
    } else {
        for (int i = y1; i > y2; i--) {

            j += n1;
            k += n2;
            
            if (i < 0 || i > GFX_LCD_HEIGHT)
            continue;

            s = j / TRAPEZOID_QUALITY;
            gfx_HorizLine(s, i, k / TRAPEZOID_QUALITY-s);
        }
    }
}


static void drawTrapezoid_NoClip(int x1, int x2, int x3, int x4, int y1, int y2) {

    int n1 = (((x2-x1)*TRAPEZOID_QUALITY/(y1-y2)));
    int n2 = (((x3-x4)*TRAPEZOID_QUALITY/(y1-y2)));

    int j = x1 * TRAPEZOID_QUALITY;
    int k = x4 * TRAPEZOID_QUALITY;
    int s = 0;

    if (y1 < y2) {
        for (int i = y1; i < y2; i++) {
            
            j -= n1;
            k -= n2;
            
            if (i < 0 || i > GFX_LCD_HEIGHT)
            continue;

            s = j / TRAPEZOID_QUALITY;
            gfx_HorizLine_NoClip(s, i, k / TRAPEZOID_QUALITY-s);
        }
    } else {
        for (int i = y1; i > y2; i--) {

            j += n1;
            k += n2;
            
            if (i < 0 || i > GFX_LCD_HEIGHT)
            continue;

            s = j / TRAPEZOID_QUALITY;
            gfx_HorizLine_NoClip(s, i, k / TRAPEZOID_QUALITY-s);
        }
    }
}


static void drawRotateTrapezoid_NoClip(int y1, int y2, int y3, int y4, int x2, int x1) {

    int n1 = (((y2-y1)*TRAPEZOID_QUALITY/(x1-x2)));
    int n2 = (((y3-y4)*TRAPEZOID_QUALITY/(x1-x2)));

    int j = y1 * TRAPEZOID_QUALITY;
    int k = y4 * TRAPEZOID_QUALITY;
    int s = 0;

    if (x1 < x2) {
        for (int i = x1; i < x2; i++) {
            
            j -= n1;
            k -= n2;
            
            if (i < 0)
            continue;

            s = j / TRAPEZOID_QUALITY;
            gfx_VertLine_NoClip(i, s, k / TRAPEZOID_QUALITY-s);
        }
    } else {
        for (int i = x1; i > x2; i--) {

            j += n1;
            k += n2;

            if (i > VIEWPORT_LENGTH)
            continue;

            s = j / TRAPEZOID_QUALITY;
            gfx_VertLine_NoClip(i, s, k / TRAPEZOID_QUALITY-s);
        }
    }
}


static void drawRotateTrapezoid(int y1, int y2, int y3, int y4, int x2, int x1) {

    int n1 = (((y2-y1)*TRAPEZOID_QUALITY/(x1-x2)));
    int n2 = (((y3-y4)*TRAPEZOID_QUALITY/(x1-x2)));

    int j = y1 * TRAPEZOID_QUALITY;
    int k = y4 * TRAPEZOID_QUALITY;
    int s = 0;

    if (x1 < x2) {
        for (int i = x1; i < x2; i++) {

            j -= n1;
            k -= n2;
            
            if (i < 0)
            continue;

            
            s = j / TRAPEZOID_QUALITY;
            gfx_VertLine(i, s, k / TRAPEZOID_QUALITY-s);
        }
    } else {
        for (int i = x1; i > x2; i--) {

            j += n1;
            k += n2;

            if (i > VIEWPORT_LENGTH)
            continue;

            
            s = j / TRAPEZOID_QUALITY;
            gfx_VertLine(i, s, k / TRAPEZOID_QUALITY-s);
        }
    }
}

/*     x2  x3
    y2 /----\
    y1/______\
     x1       x4
*/

/* x1  x2
    |\   y1
    | \  y2
    | |
    | /  y3
    |/   y4
*/


static void drawPanel(int8_t x, int8_t y, int8_t z, uint8_t position, uint8_t fill, uint8_t color) {

    if (RENDER_DISTANCE_ALGORITHM) {

        // get pre-calculated values
        const uint8_t powAZx50 = powAZx50list[z+RENDER_DIST_Z_BACK];
        const uint8_t powAZp1x50 = powAZx50list[z+RENDER_DIST_Z_BACK-1];

        const int24_t u = ((x-1)*powAZx50) + GFX_WIDTH_HALF;
        const int24_t t = ((y-1)*powAZx50) + GFX_HEIGHT_HALF;

        switch (position) {
        case PANEL_BACK:
        {
            const int24_t w = (x*powAZx50) + GFX_WIDTH_HALF;
            
            if (fill == FILLED) {
                gfx_FillRectangle(u, t, w-u+1, w-u+1);
                gfx_SetColor(color);
            }
            gfx_Rectangle(u, t, w-u, w-u);
            break;
        }

        case PANEL_BOTTOM:
        {

            const int24_t w = (x*powAZx50) + GFX_WIDTH_HALF;

            const int24_t r = (x*powAZp1x50) + GFX_WIDTH_HALF;
            const int24_t p = ((x-1)*powAZp1x50) + GFX_WIDTH_HALF;
            const int24_t o = ((y-1)*powAZp1x50) + GFX_HEIGHT_HALF;

            if (fill == FILLED) {
                drawTrapezoid(u, p, r, w, t, o);
                gfx_SetColor(color);
            }

            gfx_HorizLine(p, o, r-p+1);
            gfx_HorizLine(u, t, w-u+1);
            gfx_Line(r, o, w, t);
            gfx_Line(p, o, u, t);

            break;
        }

        case PANEL_LEFT:
        {

            const int24_t p = ((x-1)*powAZp1x50) + GFX_WIDTH_HALF;
            const int24_t o = ((y-1)*powAZp1x50) + GFX_HEIGHT_HALF;
            const int24_t v = (y*powAZx50) + GFX_HEIGHT_HALF;
            const int24_t q = (y*powAZp1x50) + GFX_HEIGHT_HALF;

            if (fill == FILLED) {
                drawRotateTrapezoid(t, o, q, v, p, u);
                gfx_SetColor(color);
            }

            gfx_VertLine(u, t, v-t+1);
            gfx_VertLine(p, o, q-o+1);
            gfx_Line(p, o, u, t);
            gfx_Line(p, q, u, v);

            break;
        }

            
        }
    }
}


static void drawPlane(int8_t x, int8_t y, int8_t z, uint8_t length, uint8_t width, uint8_t rotation, uint8_t fill, uint8_t outline_color) {

    switch (rotation) {
    case PANEL_BOTTOM:
        x -= 1;
        z -= 1;
        
        if (((z + length) < VISIBLE_RENDER_DIST_Z_FRONT) && (((z + length) > -VISIBLE_RENDER_DIST_Z_BACK) && ((z) > -RENDER_DIST_Z_BACK))) {
            
            int24_t r;
            const uint8_t powAZx50 = powAZx50list[z+RENDER_DIST_Z_BACK];
            const uint8_t powAZp1x50 = powAZx50list[z+RENDER_DIST_Z_BACK+length];
            const int24_t a = (y)*powAZp1x50;
            const int24_t b = (y)*powAZx50;
            
            uint8_t clip = CLIP;


            if ((a + GFX_HEIGHT_HALF <= 0) || (a + GFX_HEIGHT_HALF >= GFX_LCD_HEIGHT) || (((x*powAZp1x50) + GFX_WIDTH_HALF >= SCREEN_OFFSET+VIEWPORT_LENGTH) || (((x+width)*powAZp1x50) + GFX_WIDTH_HALF <= SCREEN_OFFSET))) {
                clip = SKIP_RENDER;
            } else if ((b + GFX_HEIGHT_HALF >= 0) && (b + GFX_HEIGHT_HALF < GFX_LCD_HEIGHT) && (((x*powAZx50)) + GFX_WIDTH_HALF >= SCREEN_OFFSET) && ((((x+width)*powAZx50)) + GFX_WIDTH_HALF < SCREEN_OFFSET+VIEWPORT_LENGTH)) {
                clip = NO_CLIP;
            }

            if (fill == FILLED) {
                r = a + GFX_HEIGHT_HALF;
                const int24_t w = (x+width)*powAZp1x50 + GFX_WIDTH_HALF;
                const int24_t v = x*powAZx50 + GFX_WIDTH_HALF;
                const int24_t u = b + GFX_HEIGHT_HALF;

                switch (clip) {
                case CLIP:
                    // gfx_FillTriangle((x*powAZp1x50) + GFX_WIDTH_HALF, r, w, r, v, u);
                    // gfx_FillTriangle(v, u, w, r, ((x+width)*powAZx50) + GFX_WIDTH_HALF, u);
                    drawTrapezoid(v, (x*powAZp1x50) + GFX_WIDTH_HALF, w, ((x+width)*powAZx50) + GFX_WIDTH_HALF+1, u, r);
                    break;
                case NO_CLIP:
                    // gfx_FillTriangle_NoClip((x*powAZp1x50) + GFX_WIDTH_HALF, r, w, r, v, u);
                    // gfx_FillTriangle_NoClip(v, u, w, r, ((x+width)*powAZx50) + GFX_WIDTH_HALF, u);
                    drawTrapezoid(v, (x*powAZp1x50) + GFX_WIDTH_HALF, w, ((x+width)*powAZx50) + GFX_WIDTH_HALF, u, r);
                    break;
                default:
                    break;
                }

                gfx_SetColor(outline_color);
            }

            switch (clip) {
                case NO_CLIP:
                    for (uint8_t i = 0; i <= length; i++) {
                        r = (x*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_WIDTH_HALF;
                        gfx_HorizLine_NoClip(r, ((y)*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_HEIGHT_HALF, ((x+width)*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_WIDTH_HALF-r+1);
                    }

                    for (uint8_t i = 0; i <= width; i++) {

                        gfx_Line_NoClip(
                        ((x+i)*powAZp1x50) + GFX_WIDTH_HALF,
                        (a) + GFX_HEIGHT_HALF,
                        ((x+i)*powAZx50) + GFX_WIDTH_HALF,
                        (b) + GFX_HEIGHT_HALF
                        );
                    }
                    break;
                case CLIP:
                    for (uint8_t i = 0; i <= length; i++) {
                        r = (x*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_WIDTH_HALF;
                        gfx_HorizLine(r, ((y)*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_HEIGHT_HALF, ((x+width)*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_WIDTH_HALF-r+1);
                    }

                    for (uint8_t i = 0; i <= width; i++) {

                        gfx_Line(
                        ((x+i)*powAZp1x50) + GFX_WIDTH_HALF,
                        (a) + GFX_HEIGHT_HALF,
                        ((x+i)*powAZx50) + GFX_WIDTH_HALF,
                        (b) + GFX_HEIGHT_HALF
                        );
                    }
                    break;
                default:
                    break;
            }
        }
        break;
    case PANEL_LEFT:
        if (((z + length) < VISIBLE_RENDER_DIST_Z_FRONT) && (((z + length) > -VISIBLE_RENDER_DIST_Z_BACK) && ((z) > -RENDER_DIST_Z_BACK))) {

            // get pre-calculated values
            const uint8_t powAZx50 = powAZx50list[z+RENDER_DIST_Z_BACK-1];
            const uint8_t powAZp1x50 = powAZx50list[z+RENDER_DIST_Z_BACK+length-1];

            const int24_t u = ((x-1)*powAZx50) + GFX_WIDTH_HALF;
            const int24_t t = ((y-width)*powAZx50) + GFX_HEIGHT_HALF;

            
            const int24_t p = ((x-1)*powAZp1x50) + GFX_WIDTH_HALF;
            const int24_t o = ((y-width)*powAZp1x50) + GFX_HEIGHT_HALF;
            const int24_t v = (y*powAZx50) + GFX_HEIGHT_HALF;
            const int24_t q = (y*powAZp1x50) + GFX_HEIGHT_HALF;

            uint8_t clip = CLIP;


            if (p > SCREEN_OFFSET+VIEWPORT_LENGTH && p < SCREEN_OFFSET && q < 0 && o > GFX_LCD_HEIGHT) {
                clip = SKIP_RENDER;
            } else if (u > SCREEN_OFFSET && u < SCREEN_OFFSET+VIEWPORT_LENGTH && t > 0 && v < GFX_LCD_HEIGHT) {
                clip = NO_CLIP;
            }

            if (fill == FILLED) {
                switch (clip) {
                case CLIP:
                    drawRotateTrapezoid(t, o, q, v, p, u);
                    break;
                case NO_CLIP:
                    drawRotateTrapezoid_NoClip(t, o, q, v, p, u);
                    break;
                default:
                    break;
                }
                gfx_SetColor(outline_color);
            }

            for (uint8_t i = 0; i <= width; i++) {

                gfx_Line(p, ((y-i)*powAZp1x50) + GFX_HEIGHT_HALF, u, ((y-i)*powAZx50) + GFX_HEIGHT_HALF);
                gfx_Line(p, q, u, v);
            }

            for (uint8_t i = 0; i <= length; i++) {

                const uint8_t powAZp1x50 = powAZx50list[z+RENDER_DIST_Z_BACK+i-1];
                const int24_t o = ((y-width)*powAZp1x50) + GFX_HEIGHT_HALF;

                gfx_VertLine(u, t, v-t);
                gfx_VertLine(((x-1)*powAZp1x50) + GFX_WIDTH_HALF, o, (y*powAZp1x50) + GFX_HEIGHT_HALF-o);
            }
        }
        break;
    case PANEL_BACK:
        
        if (true) {  // removing this causes bugs, idfk know why
            const uint8_t powAZx50 = powAZx50list[z+RENDER_DIST_Z_BACK];

            const int24_t u = ((x-1)*powAZx50) + GFX_WIDTH_HALF;
            const int24_t t = ((y-1)*powAZx50) + GFX_HEIGHT_HALF;

            const int24_t w = ((x-1+width)*powAZx50) + GFX_WIDTH_HALF;
            const int24_t v = ((y-1+length)*powAZx50) + GFX_HEIGHT_HALF;
            uint8_t clip = CLIP;

            if (w < SCREEN_OFFSET && u > SCREEN_OFFSET+VIEWPORT_LENGTH && v > GFX_LCD_HEIGHT && t < 0) {
                clip = SKIP_RENDER;
            } else if (w < SCREEN_OFFSET+VIEWPORT_LENGTH && SCREEN_OFFSET > 0 && t > 0 && v < GFX_LCD_HEIGHT) {
                clip = NO_CLIP;
            }

            if (fill == FILLED) {
                switch (clip) {
                case CLIP:
                    gfx_FillRectangle(u, t, w-u, v-t);
                    break;
                case NO_CLIP:
                    gfx_FillRectangle_NoClip(u, t, w-u, v-t);
                    break;
                default:
                    break;
                }
                
                gfx_SetColor(outline_color);
            }
            
            switch (clip) {
            case CLIP:
                for (uint8_t i = 0; i <= length; i++) {
                    gfx_HorizLine(u, ((y-1+i)*powAZx50) + GFX_HEIGHT_HALF, w-u);
                }
                for (uint8_t i = 0; i <= width; i++) {
                    gfx_VertLine(((x-1+i)*powAZx50) + GFX_WIDTH_HALF, t, v-t);
                }
                break;
            case NO_CLIP:
                for (uint8_t i = 0; i <= length; i++) {
                    gfx_HorizLine_NoClip(u, ((y-1+i)*powAZx50) + GFX_HEIGHT_HALF, w-u);
                }
                for (uint8_t i = 0; i <= width; i++) {
                    gfx_VertLine_NoClip(((x-1+i)*powAZx50) + GFX_WIDTH_HALF, t, v-t);
                }
                break;
            default:
                break;
                    
            }
        }
        break;
    }
}


static void drawBasePlane(int8_t y, uint8_t outline_color, uint8_t type) {

    if (type == BASE_FILL || type == BASE_PART_FILL) {
        gfx_FillRectangle_NoClip(0, GFX_HEIGHT_HALF, GFX_LCD_WIDTH, GFX_HEIGHT_HALF);
    } else if (type == BASE_RENDER_DIST) {
        drawPlane(-RENDER_DIST_X, y, -VISIBLE_RENDER_DIST_Z_BACK, VISIBLE_RENDER_DIST_Z_FRONT, RENDER_DIST_X*2, PANEL_BOTTOM, FILLED, outline_color);
        return;
    }

    if (type == BASE_PART_FILL) {
        drawPlane(-RENDER_DIST_X, y, -VISIBLE_RENDER_DIST_Z_BACK, VISIBLE_RENDER_DIST_Z_FRONT, RENDER_DIST_X*2, PANEL_BOTTOM, WIREFRAME, outline_color);
        return;
    } else {
        // do entire fill
    }

}


void preRenderScreen() {
    
    uint24_t current_buffer = draw_buffer_ctrl;
    setDrawBuffer(DBL_BUF1);

    // sky
    gfx_SetColor(GFX_BLUE);
    gfx_FillRectangle_NoClip(SCREEN_OFFSET, 0, VIEWPORT_LENGTH, GFX_LCD_HEIGHT);
    
    gfx_SetColor(GFX_GREEN);

    int8_t x = -RENDER_DIST_X-1;
    int8_t y=-player_y;
    int8_t z=-VISIBLE_RENDER_DIST_Z_BACK-1;
    uint8_t length=VISIBLE_RENDER_DIST_Z_FRONT;
    uint8_t width=RENDER_DIST_X*2;

    
    int24_t r;
    const uint8_t powAZx50 = powAZx50list[z+RENDER_DIST_Z_BACK];
    const uint8_t powAZp1x50 = powAZx50list[z+RENDER_DIST_Z_BACK+length];
    const int24_t a = (y)*powAZp1x50;
    const int24_t b = (y)*powAZx50;
   

    r = a + GFX_HEIGHT_HALF;
    const int24_t w = (x+width)*powAZp1x50 + GFX_WIDTH_HALF;
    const int24_t v = x*powAZx50 + GFX_WIDTH_HALF;
    const int24_t u = b + GFX_HEIGHT_HALF;

    int x1 = v;
    int x2=(x*powAZp1x50) + GFX_WIDTH_HALF;
    int x3=w;
    int x4=((x+width)*powAZx50) + GFX_WIDTH_HALF;
    int y1=u;
    int y2=r;


    int n1 = (((x2-x1)*TRAPEZOID_QUALITY/(y1-y2)));
    int n2 = (((x3-x4)*TRAPEZOID_QUALITY/(y1-y2)));

    int j = x1 * TRAPEZOID_QUALITY;
    int k = x4 * TRAPEZOID_QUALITY;
    int s = 0;

    if (y1 < y2) {
        for (int i = y1; i < y2; i++) {
            
            j -= n1;
            k -= n2;
            
            if (i < 0 || i > GFX_LCD_HEIGHT)
            continue;

            s = j / TRAPEZOID_QUALITY;
            gfx_HorizLine(s, i, k / TRAPEZOID_QUALITY-s);
        }
    } else {
        for (int i = y1; i > y2; i--) {

            j += n1;
            k += n2;
            
            if (i < 0 || i > GFX_LCD_HEIGHT)
            continue;

            s = j / TRAPEZOID_QUALITY;
            gfx_HorizLine(s, i, k / TRAPEZOID_QUALITY-s);
        }
    }

    drawTrapezoid(v, (x*powAZp1x50) + GFX_WIDTH_HALF, w, ((x+width)*powAZx50) + GFX_WIDTH_HALF, u, r);
        

    gfx_SetColor(GFX_BLACK);

    for (uint8_t i = 0; i <= length; i++) {
        r = (x*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_WIDTH_HALF;
        gfx_HorizLine(r, ((y)*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_HEIGHT_HALF, ((x+width)*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_WIDTH_HALF-r+1);
    }

    for (uint8_t i = 0; i <= width; i++) {

        gfx_Line(
        ((x+i)*powAZp1x50) + GFX_WIDTH_HALF,
        (a) + GFX_HEIGHT_HALF,
        ((x+i)*powAZx50) + GFX_WIDTH_HALF,
        (b) + GFX_HEIGHT_HALF
        );
    }
    
    setDrawBuffer(current_buffer);

}


void blitPrerenderScreen(uint24_t buffer) {

    if (buffer > 1) {
        buffer = ((buffer-2) * LCD_WIDTH/2) + DBL_BUF2;
    } else {
        buffer = (buffer * LCD_WIDTH/2) + DBL_BUF1;
    }
    
    for (uint8_t y = 0; y < LCD_HEIGHT / 2; y++) {
        memcpy(draw_buffer_ctrl+y*LCD_WIDTH, buffer+y*LCD_WIDTH, LCD_WIDTH / 2);
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

    // initialize first double buffer
    setDrawBuffer(DBL_BUF1);
    gfx_FillRectangle_NoClip(0, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
    gfx_FillRectangle_NoClip(VIEWPORT_LENGTH+SCREEN_OFFSET, 0, SCREEN_OFFSET, GFX_LCD_HEIGHT);
    // de-interlacing is not needed, as this is a final rendering stage step

    // set screen display back normal
    setScreenBuffer(BUFFER_1);
    setDrawBuffer(BUFFER_1);

}


int main(void)
{

    // wait until user releases keys
    while ((kb_Data[1] & kb_2nd) || (kb_Data[6] & kb_Enter)) {
        msleep(100);
    }

    // initialization
    init_fontlib();
    init_screen();
    
    // prep 3d environment
    sortCoordinateList(coordinates, LEN(coordinates));
    preRenderScreen();

    // FPS calculation
    uint8_t frame_count = 0;
    clock_t start_time = clock();
    uint8_t fps = 0;

    // while clear not pressed
    while (!(kb_Data[6] & kb_Clear)) {
        
        frame_count++;

        // do this every second
        if (clock() - start_time >= CLOCKS_PER_SEC) {

            // FPS calculation
            fps = frame_count;
            frame_count = 0;
            start_time = clock();

            // sort objects
            sortCoordinateList(coordinates, LEN(coordinates));
        }

        // handle input
        if (kb_Data[7] & kb_Up) {
            player_z += 1;
        } else if (kb_Data[7] & kb_Down) {
            player_z -= 1;
        } else if (kb_Data[7] & kb_Left) {
            player_x -= 1;
        } else if (kb_Data[7] & kb_Right) {
            player_x += 1;
        } // else if (kb_Data[1] & kb_2nd) {
        //     player_y += 1;
        // } else if (kb_Data[2] & kb_Alpha) {
        //     player_y -= 1;
        // }

        
        // blit prerendered display
        blitPrerenderScreen(0);

        // draw scene
        for (uint8_t i = 0; i < LEN(coordinates); i++) {
            gfx_SetColor(coordinates[i][3]);
            drawBox(coordinates[i][0]-player_x, coordinates[i][1]-player_y, coordinates[i][2]-player_z, FILLED, GFX_BLACK);
        }

        // draw FPS counter
        fontlib_SetCursorPosition(20,4);
        fontlib_DrawUInt(fps, 2);

        // de-interlace viewport
        deInterlaceViewportBuffer(draw_buffer_ctrl);

        // swap roles of the buffers
        swapBuffer();

    }

    // program end
    gfx_End();
    return 0;
}