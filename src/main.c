#include <keypadc.h>
#include <graphx.h>

#include <sys/timers.h>
#include <math.h>
#include <time.h>
#include <stdint.h>

#include <debug.h>


#define FOV 0.8
#define LEN(arr) ((int) (sizeof (arr) / sizeof (arr)[0]))

#define NONE 0

#define GFX_HEIGHT_HALF 120
#define GFX_WIDTH_HALF  160

#define PANEL_BOTTOM  0
#define PANEL_LEFT    1
#define PANEL_BACK    2

#define WIREFRAME               0
#define VISIBLE_ONLY_WIREFRAME  1
#define FILLED                  2

#define NO_CLIP      0
#define CLIP         1
#define SKIP_RENDER  2

// all exclusive. rerun powAZx50.py when values are changed
#define RENDER_DIST_X                16   // actual limit is double this value
#define RENDER_DIST_Y                10   // same as above
#define RENDER_DIST_Z_BACK           20   // back from the camera - used for calculations
#define RENDER_DIST_Z_FRONT          30   // in front of the camera
#define VISIBLE_RENDER_DIST_Z_BACK   3    // used solely for optimization
#define VISIBLE_RENDER_DIST_Z_FRONT  30   //

#define GFX_BLACK   0x00
#define GFX_RED     0xE0
#define GFX_ORANGE  0xE3
#define GFX_GREEN   0x03
#define GFX_BLUE    0x10
#define GFX_PURPLE  0x50
#define GFX_YELLOW  0xE7
#define GFX_PINK    0xF0
#define GFX_WHITE   0xFF

// simple rectangular prism. cone-shape would be best in the future
#define RENDER_DISTANCE_ALGORITHM (z > -VISIBLE_RENDER_DIST_Z_BACK) && (z < VISIBLE_RENDER_DIST_Z_FRONT) && (x < RENDER_DIST_X) && (x > -RENDER_DIST_X) && (y < RENDER_DIST_Y) && (y > -RENDER_DIST_Y)


// auto-generated, do not tamper
const static uint8_t powAZx50list[] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 238, 191, 153, 122, 98, 78, 62, 50, 40, 32, 26, 20, 16, 13, 10, 8, 7, 5, 4, 3, 3, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};


static int8_t player_x = 0;
static int8_t player_y = 0;
static int8_t player_z = 0;


static int compareCoordinates(const void *a, const void *b) {

    const int8_t *coord1 = (int8_t *)a;
    const int8_t *coord2 = (int8_t *)b;
    
    
    if ((coord2[2] - coord1[2]) != 0) {
        return (coord2[2] - coord1[2]);
    }
    
    return (abs(coord2[0] - player_x) + abs(coord2[1] - player_y)) - (abs(coord1[0] - player_x) + abs(coord1[1] - player_y));
}

static void sortCoordinateList(int8_t coordinates[][3], size_t numCoordinates) {

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
        const uint8_t wup1 = w-u;
        const uint8_t rpp1 = r-p;

        // 0 = on screen
        // 1 = partially on screen
        // 2 = off screen
        uint8_t clip = CLIP;

        // determines route of action for clipping
        if ((r < 320) && (p >= 0) && (o >= 0) && (q < 240)) {
            clip = NO_CLIP;
        } else if ((u > 320) || (w < 0) || (t > 240) || (v < 0)) {
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
                if (u < 160 && v > 120) {
                    gfx_Line_NoClip(r, q, w, v);
                    gfx_Line_NoClip(r, o, w, t);
                    gfx_Line_NoClip(p, o, u, t);
                    gfx_VertLine_NoClip(w, t, wup1);
                    gfx_HorizLine_NoClip(u, t, wup1);
                } else if (u >= 160 && v > 120) {
                    gfx_Line_NoClip(r, o, w, t);
                    gfx_Line_NoClip(p, o, u, t);
                    gfx_Line_NoClip(p, q, u, v);
                    gfx_VertLine_NoClip(u, t, wup1);
                    gfx_HorizLine_NoClip(u, t, wup1);
                } else if (u >= 160 && v <= 120) {
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
                if (u < 160 && v > 120) {  // bottom left
                    gfx_Line(r, q, w, v);
                    gfx_Line(r, o, w, t);
                    gfx_Line(p, o, u, t);
                    gfx_VertLine(w, t, wup1);
                    gfx_HorizLine(u, t, wup1);
                } else if (u >= 160 && v > 120) {  // bottom right
                    gfx_Line(r, o, w, t);
                    gfx_Line(p, o, u, t);
                    gfx_Line(p, q, u, v);
                    gfx_VertLine(u, t, wup1);
                    gfx_HorizLine(u, t, wup1);
                } else if (u >= 160 && v <= 120) {  // top right
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
                if (u < 160 && v > 120) {
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
                } else if (u >= 160 && v > 120) {
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
                } else if (u >= 160 && v <= 120) {
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
                if (u < 160 && v > 120) {  // bottom left
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
                } else if (u >= 160 && v > 120) {  // bottom right
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
                } else if (u >= 160 && v <= 120) {  // top right
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


    int n1 = (((x2-x1)*100/(y1-y2)));
    int n2 = (((x3-x4)*100/(y1-y2)));

    int j = x1 * 100;
    int k = x4 * 100;
    int s = 0;

    if (y1 < y2) {
        for (int i = y1; i < y2; i++) {
            
            j -= n1;
            k -= n2;
            s = j / 100;
            gfx_HorizLine(s, i, k / 100-s);
        }
    } else {
        for (int i = y1; i > y2; i--) {
            j += n1;
            k += n2;
            s = j / 100;
            gfx_HorizLine(s, i, k / 100-s);
        }
    }
}

static void drawTrapezoid_NoClip(int x1, int x2, int x3, int x4, int y1, int y2) {

    int n1 = (((x2-x1)*100/(y1-y2)));
    int n2 = (((x3-x4)*100/(y1-y2)));

    int j = x1 * 100;
    int k = x4 * 100;
    int s = 0;

    if (y1 < y2) {
        for (int i = y1; i < y2; i++) {
            
            j -= n1;
            k -= n2;
            s = j / 100;
            gfx_HorizLine_NoClip(s, i, k / 100-s);
        }
    } else {
        for (int i = y1; i > y2; i--) {
            j += n1;
            k += n2;
            s = j / 100;
            gfx_HorizLine_NoClip(s, i, k / 100-s);
        }
    }
}

static void drawRotateTrapezoid_NoClip(int y1, int y2, int y3, int y4, int x2, int x1) {

    int n1 = (((y2-y1)*100/(x1-x2)));
    int n2 = (((y3-y4)*100/(x1-x2)));

    int j = y1 * 100;
    int k = y4 * 100;
    int s = 0;

    if (x1 < x2) {
        for (int i = x1; i < x2; i++) {
            
            j -= n1;
            k -= n2;
            s = j / 100;
            gfx_VertLine_NoClip(i, s, k / 100-s);
        }
    } else {
        for (int i = x1; i > x2; i--) {
            j += n1;
            k += n2;
            s = j / 100;
            gfx_VertLine_NoClip(i, s, k / 100-s);
        }
    }
}

static void drawRotateTrapezoid(int y1, int y2, int y3, int y4, int x2, int x1) {

    int n1 = (((y2-y1)*100/(x1-x2)));
    int n2 = (((y3-y4)*100/(x1-x2)));

    int j = y1 * 100;
    int k = y4 * 100;
    int s = 0;

    if (x1 < x2) {
        for (int i = x1; i < x2; i++) {
            
            j -= n1;
            k -= n2;
            s = j / 100;
            gfx_VertLine(i, s, k / 100-s);
        }
    } else {
        for (int i = x1; i > x2; i--) {
            j += n1;
            k += n2;
            s = j / 100;
            gfx_VertLine(i, s, k / 100-s);
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
                gfx_FillRectangle(u, t, w-u, w-u);
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

            gfx_HorizLine(p, o, r-p);
            gfx_HorizLine(u, t, w-u);
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

            gfx_VertLine(u, t, v-t);
            gfx_VertLine(p, o, q-o);
            gfx_Line(p, o, u, t);
            gfx_Line(p, q, u, v);

            break;
        }

            
        }
    }
}


// TODO: Do front and side

static void drawPlane(int8_t x, int8_t y, int8_t z, uint8_t length, uint8_t width, uint8_t rotation, uint8_t fill, uint8_t outline_color) {

    x -= 1;
    z -= 1;
    
    
    if (((z + width) < VISIBLE_RENDER_DIST_Z_FRONT) && (((z + width) > -VISIBLE_RENDER_DIST_Z_BACK) && ((z) > -RENDER_DIST_Z_BACK))) {
        
        int24_t r;
        const uint8_t powAZx50 = powAZx50list[z+RENDER_DIST_Z_BACK];
        const uint8_t powAZp1x50 = powAZx50list[z+RENDER_DIST_Z_BACK+length];
        const int24_t a = (y)*powAZp1x50;
        const int24_t b = (y)*powAZx50;
        
        uint8_t clip = CLIP;

        gfx_SetColor(GFX_WHITE); // (x*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_WIDTH_HALF

        if (((a) + GFX_HEIGHT_HALF <= 0) || ((a) + GFX_HEIGHT_HALF >= 240) || (((x*powAZp1x50) + GFX_WIDTH_HALF >= 320) || (((x+width)*powAZp1x50) + GFX_WIDTH_HALF <= 0))) {
            clip = SKIP_RENDER;
            gfx_SetColor(GFX_BLUE);
        } else if (((b) + GFX_HEIGHT_HALF >= 0) && ((b) + GFX_HEIGHT_HALF < 240) && ((((x)*powAZx50)) + GFX_WIDTH_HALF >= 0) && ((((x+width)*powAZx50)) + GFX_WIDTH_HALF < 320)) {
            clip = NO_CLIP;
            gfx_SetColor(GFX_GREEN);
        }

        if (fill == FILLED) {
            r = (a) + GFX_HEIGHT_HALF;
            const int24_t w = ((x+width)*powAZp1x50) + GFX_WIDTH_HALF;
            const int24_t v = (x*powAZx50) + GFX_WIDTH_HALF;
            const int24_t u = (b) + GFX_HEIGHT_HALF;

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
                    gfx_HorizLine_NoClip(r, ((y)*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_HEIGHT_HALF, ((x+width)*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_WIDTH_HALF-r);
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
                    gfx_HorizLine(r, ((y)*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_HEIGHT_HALF, ((x+width)*powAZx50list[z+RENDER_DIST_Z_BACK+i]) + GFX_WIDTH_HALF-r);
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
    } else {
        gfx_Rectangle(50, 50, 20, 20);
    }
}


// takes about 0.00025 seconds per item to sort
static int8_t coordinates[][3] = {
        {1, 2, 5},
        {-1, 2, 5},
        {-3, 2, 5},
};

int main(void)
{

    // wait until user releases keys
    while ((kb_Data[1] & kb_2nd) || (kb_Data[6] & kb_Enter)) {
        msleep(100);  // 100 ms
    }

    gfx_Begin();
    gfx_ZeroScreen();
    sortCoordinateList(coordinates, LEN(coordinates));

    gfx_SetColor(GFX_RED);
    gfx_SetTextFGColor(GFX_RED);

    uint8_t frame_count = 0;
    clock_t start_time = clock();
    uint8_t fps = 0;

    while (!(kb_Data[6] & kb_Clear)) {
        
        // FPS calculation
        frame_count++;

        if (clock() - start_time >= CLOCKS_PER_SEC) {
            fps = frame_count;
            frame_count = 0;
            start_time = clock();
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
        } else if (kb_Data[1] & kb_2nd) {
            player_y += 1;
        } else if (kb_Data[2] & kb_Alpha) {
            player_y -= 1;
        }

        

        // FPS counter
        gfx_ZeroScreen();
        gfx_SetTextXY(5,5);
        gfx_PrintString("FPS:");
        gfx_SetTextXY(35,5);
        gfx_PrintUInt(fps, 2);
        gfx_SetTextXY(55,5);
        gfx_PrintString("X:");
        gfx_SetTextXY(70,5);
        gfx_PrintInt(player_x, 2);
        gfx_SetTextXY(90,5);
        gfx_PrintString("Y:");
        gfx_SetTextXY(105,5);
        gfx_PrintInt(player_y, 2);
        gfx_SetTextXY(125,5);
        gfx_PrintString("Z:");
        gfx_SetTextXY(140,5);
        gfx_PrintInt(player_z, 2);
        gfx_SetTextXY(275,5);
        gfx_PrintString("v0.2.5");
        
        
        gfx_SetColor(GFX_BLUE);
        // drawPlane(0-player_x, 0-player_y, 0-player_z, 5, 12, PANEL_BOTTOM, FILLED, GFX_GREEN);
        // drawBox(0-player_x, 0-player_y, 0-player_z, WIREFRAME, NONE);
        drawPanel(0-player_x, 0-player_y, 0-player_z, PANEL_LEFT, FILLED, GFX_RED);


        gfx_SwapDraw();

    }

    gfx_End();
    return 0;
}

