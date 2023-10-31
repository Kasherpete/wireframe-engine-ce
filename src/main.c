#include <keypadc.h>
#include <graphx.h>

#include <sys/timers.h>
#include <math.h>
#include <time.h>
#include <stdint.h>


#define FOV 0.8
#define LEN(arr) ((int) (sizeof (arr) / sizeof (arr)[0]))

#define GFX_HEIGHT_HALF 120
#define GFX_WIDTH_HALF 160

#define CACHE_MAX_LENGTH 1
#define CACHE_IS_FULL (cache_size == CACHE_MAX_LENGTH - 1)

// all exclusive
#define RENDER_DIST_X 8  // dont draw below -4x or higher than 4x
#define RENDER_DIST_Y 8
#define RENDER_DIST_Z_BACK 3 // back from the camera
#define RENDER_DIST_Z_FRONT 8 // above 0

// simple rectangular prism. cone-shape would be best in the future
#define RENDER_DISTANCE_ALGORITHM (z > -RENDER_DIST_Z_BACK) && (z < RENDER_DIST_Z_FRONT) && (x < RENDER_DIST_X) && (x > -RENDER_DIST_X) && (y < RENDER_DIST_Y) && (y > -RENDER_DIST_Y)


// auto-generated, do not tamper
const uint8_t powAZx50list[] = {98, 78, 62, 50, 40, 32, 26, 20, 16, 13, 10};

// 1-3 - coor; 4-11 - vertices; 12 - clip

// auto-generated, do not tamper 20-11
int24_t cache[CACHE_MAX_LENGTH][12] = {};
uint8_t cache_size = 0;

// int retrieveCacheEntry(int8_t x, int8_t y, int8_t z) {
//     for (uint8_t i = 0; i <= cache_size; i++) {
//         if ((x == cache[i][0]) && (y == cache[i][1]) && (z == cache[i][2])) {
//             return i;
//         }
//     }
//     return -1;
// }


void drawBox(int8_t x, int8_t y, int8_t z, uint8_t fill, uint8_t fill_color) {

    // render distance calculations. needs improvement
    if (RENDER_DISTANCE_ALGORITHM) {

        // just some optimizations
        const uint8_t powAZx50 = powAZx50list[z+3];
        const uint8_t powAZp1x50 = powAZx50list[z+2];

        // calculate values - data type needs to go from -1 to 241 height, 321 width
        // potential optimization here. willing to make viewport 255x255

        int24_t w;
        int24_t v;
        int24_t u;
        int24_t t;
        int24_t r;
        int24_t q;
        int24_t p;
        int24_t o;
        uint8_t clip;

        int8_t entry;

        for (uint8_t i = 0; i < cache_size; i++) {
            if ((x == cache[i][0]) && (y == cache[i][1]) && (z == cache[i][2])) {
                entry = i;
                break;
            }
        }

        if (entry) {
            w = cache[entry][3];
            v = cache[entry][4];
            u = cache[entry][5];
            t = cache[entry][6];
            r = cache[entry][7];
            q = cache[entry][8];
            p = cache[entry][9];
            o = cache[entry][10];
            clip = cache[entry][11];
        } else {
            w = (x*powAZx50) + GFX_WIDTH_HALF;
            v = (y*powAZx50) + GFX_HEIGHT_HALF;
            u = ((x-1)*powAZx50) + GFX_WIDTH_HALF;
            t = ((y-1)*powAZx50) + GFX_HEIGHT_HALF;
            r = (x*powAZp1x50) + GFX_WIDTH_HALF;
            q = (y*powAZp1x50) + GFX_HEIGHT_HALF;
            p = ((x-1)*powAZp1x50) + GFX_WIDTH_HALF;
            o = ((y-1)*powAZp1x50) + GFX_HEIGHT_HALF;

            clip = 1;

            if ((r < 320) && (p >= 0) && (o >= 0) && (q < 240)) {
                clip = 0;
            }
            if ((w > 320) || (u < 0) || (v > 240) || (t < 0)) {
                clip = 2;
            }

            if (!CACHE_IS_FULL) {
                cache[cache_size][0] = x;
                cache[cache_size][1] = y;
                cache[cache_size][2] = z;
                cache[cache_size][3] = w;
                cache[cache_size][4] = v;
                cache[cache_size][5] = u;
                cache[cache_size][6] = t;
                cache[cache_size][7] = r;
                cache[cache_size][8] = q;
                cache[cache_size][9] = p;
                cache[cache_size][10] = o;
                cache[cache_size][11] = clip;

                cache_size++;
            }
        }

        
        if (!fill) {
            switch (clip) {

            case 0:  // if fully on screen
                gfx_Rectangle_NoClip(u, t, w-u+1, w-u+1);
                gfx_Rectangle_NoClip(p,o,r-p+1, r-p+1);
                gfx_Line_NoClip(r, q, w, v);
                gfx_Line_NoClip(r, o, w, t);
                gfx_Line_NoClip(p, o, u, t);
                gfx_Line_NoClip(p, q, u, v);
                break;

            case 1:  // if partly off screen
                gfx_Rectangle(u, t, w-u+1, w-u+1);
                gfx_Rectangle(p,o,r-p+1, r-p+1);
                gfx_Line(r, q, w, v);
                gfx_Line(r, o, w, t);
                gfx_Line(p, o, u, t);
                gfx_Line(p, q, u, v);
                break;
            
            default:  // if off screen
                break;
            }
        } else {

            uint8_t outline_color = gfx_SetColor(fill_color);

            switch (clip) {

            case 0:  // if fully on screen
                gfx_FillRectangle_NoClip(u, t, w-u+1, w-u+1);
                gfx_FillRectangle_NoClip(p,o,r-p+1, r-p+1);
                gfx_FillTriangle_NoClip(r, q, w, v, r, v);  // bottom right
                gfx_FillTriangle_NoClip(r, o, w, t, w, o);  // top right
                gfx_FillTriangle_NoClip(p, o, u, t, u, o);  // top left
                gfx_FillTriangle_NoClip(p, q, u, v, p, v);  // bottom left

                gfx_SetColor(outline_color);
                gfx_Rectangle_NoClip(u, t, w-u+1, w-u+1);
                gfx_Rectangle_NoClip(p,o,r-p+1, r-p+1);
                gfx_Line_NoClip(r, q, w, v);
                gfx_Line_NoClip(r, o, w, t);
                gfx_Line_NoClip(p, o, u, t);
                gfx_Line_NoClip(p, q, u, v);
                break;

            case 1:  // if partly off screen
                gfx_FillRectangle(u, t, w-u+1, w-u+1);
                gfx_FillRectangle(p,o,r-p+1, r-p+1);
                gfx_FillTriangle(r, q, w, v, r, v);  // bottom right
                gfx_FillTriangle(r, o, w, t, w, o);  // top right
                gfx_FillTriangle(p, o, u, t, u, o);  // top left
                gfx_FillTriangle(p, q, u, v, p, v);  // bottom left

                gfx_SetColor(outline_color);
                gfx_Rectangle(u, t, w-u+1, w-u+1);
                gfx_Rectangle(p,o,r-p+1, r-p+1);
                gfx_Line(r, q, w, v);
                gfx_Line(r, o, w, t);
                gfx_Line(p, o, u, t);
                gfx_Line(p, q, u, v);
                break;
            
            default:  // if off screen
                break;
            }
        }
        
        
        
    }
}


int main(void)
{

    while ((kb_Data[1] & kb_2nd) || (kb_Data[6] & kb_Enter)) {
        sleep(.1);
    }

    gfx_Begin();
    gfx_ZeroScreen();

    
    int8_t player_x = 0;
    int8_t player_y = 0;
    int8_t player_z = 0;


    gfx_SetColor(gfx_red);
    gfx_SetTextFGColor(gfx_red);

    uint8_t frame_count = 0;
    clock_t start_time = clock();
    uint8_t fps = 0;

    while (!(kb_Data[6] & kb_Clear)) {

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

        gfx_SwapDraw();

        gfx_ZeroScreen();
        gfx_SetTextXY(5,5);
        gfx_PrintString("FPS:");
        gfx_SetTextXY(35,5);
        gfx_PrintUInt(fps, 2);
        gfx_SetTextXY(55,5);
        gfx_PrintString("Cache:");
        gfx_SetTextXY(100,5);
        gfx_PrintUInt(cache_size, 2);
        
        for (int i = 0; i < 30; i++)
            drawBox(2-player_x,2+player_y,1-player_z,NULL,NULL);


        frame_count++;

        if (clock() - start_time >= CLOCKS_PER_SEC) {
            fps = frame_count;
            frame_count = 0;
            start_time = clock();
        }
    }


    gfx_End();
    return 0;
}
