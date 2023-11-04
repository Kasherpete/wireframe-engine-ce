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
#define GFX_WIDTH_HALF 160

#define PANEL_BOTTOM 0
#define PANEL_LEFT 1
#define PANEL_BACK 2

// all exclusive. rerun powAZx50.py when values are changed
#define RENDER_DIST_X 16       // actual limit is double this value
#define RENDER_DIST_Y 10       // same as above
#define RENDER_DIST_Z_BACK 3   // back from the camera
#define RENDER_DIST_Z_FRONT 10 // in front of the camera

// simple rectangular prism. cone-shape would be best in the future
#define RENDER_DISTANCE_ALGORITHM (z > -RENDER_DIST_Z_BACK) && (z < RENDER_DIST_Z_FRONT) && (x < RENDER_DIST_X) && (x > -RENDER_DIST_X) && (y < RENDER_DIST_Y) && (y > -RENDER_DIST_Y)


// auto-generated, do not tamper
uint8_t powAZx50list[] = {98, 78, 62, 50, 40, 32, 26, 20, 16, 13, 10, 8, 7};


int8_t player_x = 0;
int8_t player_y = 0;
int8_t player_z = 0;


int compareCoordinates(const void *a, const void *b) {
    return (((int8_t *)b)[2] - player_z) - (((int8_t *)a)[2] - player_z);
}

int compareCoordinatesXY(const void *a, const void *b) {
    return ((((int8_t *)b)[0] - player_x) + (((int8_t *)b)[1]) - player_x) - ((((int8_t *)a)[0] - player_y) + (((int8_t *)a)[1]) - player_y);
}

void sortCoordinateList(int8_t coordinates[][3], int numCoordinates) {

    qsort(coordinates, numCoordinates, sizeof(int8_t) * 3, compareCoordinatesXY);
    qsort(coordinates, numCoordinates, sizeof(int8_t) * 3, compareCoordinates);

}

void drawBox(int8_t x, int8_t y, int8_t z, uint8_t type, uint8_t outline_color) {

    // render distance calculations. needs improvement
    if (RENDER_DISTANCE_ALGORITHM) {

        // get pre-calculated values
        const uint8_t powAZx50 = powAZx50list[z+3];
        const uint8_t powAZp1x50 = powAZx50list[z+2];

        // calculate values - data type needs to go from -1 to 241 height, 321 width
        // potential optimization here. willing to make viewport 255x255

        int24_t w = (x*powAZx50) + GFX_WIDTH_HALF;
        int24_t v = (y*powAZx50) + GFX_HEIGHT_HALF;
        int24_t u = ((x-1)*powAZx50) + GFX_WIDTH_HALF;
        int24_t t = ((y-1)*powAZx50) + GFX_HEIGHT_HALF;
        int24_t r = (x*powAZp1x50) + GFX_WIDTH_HALF;
        int24_t q = (y*powAZp1x50) + GFX_HEIGHT_HALF;
        int24_t p = ((x-1)*powAZp1x50) + GFX_WIDTH_HALF;
        int24_t o = ((y-1)*powAZp1x50) + GFX_HEIGHT_HALF;

        // tiny optimization. not sure if this has any benefit
        uint8_t wup1 = w-u+1;
        uint8_t rpp1 = r-p+1;

        // 0 = on screen
        // 1 = partially on screen
        // 2 = off screen
        uint8_t clip = 1;

        // determines route of action for clipping
        if ((r < 320) && (p >= 0) && (o >= 0) && (q < 240)) {
            clip = 0;
        } else if ((w > 320) || (u < 0) || (v > 240) || (t < 0)) {
            clip = 2;
        }

        
        switch (type) {
        case 0:
            switch (clip) {

            case 0:  // if fully on screen
                gfx_Rectangle_NoClip(u, t, wup1, wup1);
                gfx_Rectangle_NoClip(p,o,rpp1, rpp1);
                gfx_Line_NoClip(r, q, w, v);
                gfx_Line_NoClip(r, o, w, t);
                gfx_Line_NoClip(p, o, u, t);
                gfx_Line_NoClip(p, q, u, v);
                break;

            case 1:  // if partly off screen
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
        case 1:

            switch (clip) {

            case 0:  // if fully on screen

                // fill area
                gfx_FillRectangle_NoClip(u, t, wup1, wup1);
                gfx_FillRectangle_NoClip(p, o, rpp1, rpp1);
                gfx_FillTriangle_NoClip(r, q, w, v, r, v);
                gfx_FillTriangle_NoClip(r, o, w, t, w, o);
                gfx_FillTriangle_NoClip(p, o, u, t, u, o);
                gfx_FillTriangle_NoClip(p, q, u, v, p, v);
                
                // render outline
                gfx_SetColor(outline_color);
                gfx_Rectangle_NoClip(u, t, wup1, wup1);
                gfx_Rectangle_NoClip(p, o, rpp1, rpp1);
                gfx_Line_NoClip(r, q, w, v);
                gfx_Line_NoClip(r, o, w, t);
                gfx_Line_NoClip(p, o, u, t);
                gfx_Line_NoClip(p, q, u, v);
                break;

            case 1:  // if partly off screen

                // fill area
                gfx_FillRectangle(u, t, wup1, wup1);
                gfx_FillRectangle(p, o, rpp1, rpp1);
                gfx_FillTriangle(r, q, w, v, r, v);
                gfx_FillTriangle(r, o, w, t, w, o);
                gfx_FillTriangle(p, o, u, t, u, o);
                gfx_FillTriangle(p, q, u, v, p, v);

                // render outline
                gfx_SetColor(outline_color);
                gfx_Rectangle(u, t, wup1, wup1);
                gfx_Rectangle(p, o, rpp1, rpp1);
                gfx_Line(r, q, w, v);
                gfx_Line(r, o, w, t);
                gfx_Line(p, o, u, t);
                gfx_Line(p, q, u, v);
                break;
            
            default:  // if off screen
                break;
            break;
            }
        }
    }
}

void drawPanel(int8_t x, int8_t y, int8_t z, uint8_t position) {

    if (RENDER_DISTANCE_ALGORITHM) {

        // get pre-calculated values
        const uint8_t powAZx50 = powAZx50list[z+3];
        const uint8_t powAZp1x50 = powAZx50list[z+2];

        int24_t u = ((x-1)*powAZx50) + GFX_WIDTH_HALF;
        int24_t t = ((y-1)*powAZx50) + GFX_HEIGHT_HALF;

        switch (position) {
        case PANEL_BACK:
        {
            int24_t w = (x*powAZx50) + GFX_WIDTH_HALF;

            gfx_Rectangle(u, t, w-u+1, w-u+1);
            break;
        }

        case PANEL_BOTTOM:
        {

            int24_t w = (x*powAZx50) + GFX_WIDTH_HALF;

            int24_t r = (x*powAZp1x50) + GFX_WIDTH_HALF;
            int24_t p = ((x-1)*powAZp1x50) + GFX_WIDTH_HALF;
            int24_t o = ((y-1)*powAZp1x50) + GFX_HEIGHT_HALF;

            gfx_HorizLine(p, o, r-p+1);
            gfx_HorizLine(u, t, w-u+1);
            gfx_Line(r, o, w, t);
            gfx_Line(p, o, u, t);

            break;
        }

        case PANEL_LEFT:
        {

            int24_t p = ((x-1)*powAZp1x50) + GFX_WIDTH_HALF;
            int24_t o = ((y-1)*powAZp1x50) + GFX_HEIGHT_HALF;
            int24_t v = (y*powAZx50) + GFX_HEIGHT_HALF;
            int24_t q = (y*powAZp1x50) + GFX_HEIGHT_HALF;

            gfx_VertLine(u, t, v-t+1);
            gfx_VertLine(p, o, q-o+1);
            gfx_Line(p, o, u, t);
            gfx_Line(p, q, u, v);

            break;
        }

            
        }
    }
}


// takes about 0.00025 seconds per item to sort
 int8_t coordinates[][3] = {
        {1, 2, 5},
        {2, 3, 3},
        {3, 4, 5},
        {4, 5, 4},
        {0, 2, 5},
        {2, 2, 5},
        {7, 2, 5},
        {8, 5, 5},
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

    gfx_SetColor(gfx_red);
    gfx_SetTextFGColor(gfx_red);

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
        
        // main drawing
        drawBox(coordinates[0][0]-player_x,coordinates[0][1]+player_y,coordinates[0][2]-player_z, 1, gfx_white);


        gfx_SwapDraw();

    }

    gfx_End();
    return 0;
}
