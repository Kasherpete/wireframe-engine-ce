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

// all exclusive
#define RENDER_DIST_X 4  // dont draw below -4x or higher than 4x
#define RENDER_DIST_Y 3
#define RENDER_DIST_Z_BACK 3 // back from the camera
#define RENDER_DIST_Z_FRONT 8 // above 0

// simple rectangular prism. cone-shape would be best.
#define RENDER_DISTANCE_ALGORITHM (z > -RENDER_DIST_Z_BACK) && (z < RENDER_DIST_Z_FRONT) && (x < RENDER_DIST_X) && (x > -RENDER_DIST_X) && (y < RENDER_DIST_Y) && (y > -RENDER_DIST_Y)


struct Player
{
    int_fast8_t x;
    int_fast8_t y;
    int_fast8_t z;
};


void drawBox(int x, int y, int z) {

    // render distance calculations. needs improvement
    if (RENDER_DISTANCE_ALGORITHM) {

        // just some optimizations
        const uint_fast8_t powAZx50 = pow(FOV,z) * 50;
        const uint_fast8_t powAZp1x50 = pow(FOV,(z+1)) * 50;

        // calculate values - data type needs to go from -1 to 241 height, 321 width
        // potential optimization here. willing to make viewport 255x255
        int_fast16_t w = (x*powAZx50) + GFX_WIDTH_HALF;
        int_fast16_t v = (y*powAZx50) + GFX_HEIGHT_HALF;
        int_fast16_t u = ((x-1)*powAZx50) + GFX_WIDTH_HALF;
        int_fast16_t t = ((y-1)*powAZx50) + GFX_HEIGHT_HALF;
        int_fast16_t r = (x*powAZp1x50) + GFX_WIDTH_HALF;
        int_fast16_t q = (y*powAZp1x50) + GFX_HEIGHT_HALF;
        int_fast16_t p = ((x-1)*powAZp1x50) + GFX_WIDTH_HALF;
        int_fast16_t o = ((y-1)*powAZp1x50) + GFX_HEIGHT_HALF;

        // front box
        gfx_Rectangle(u, t, w-u+1, w-u+1);
        // back box
        gfx_Rectangle(p,o,r-p+1, r-p+1);
        // lines
        gfx_Line(r, q, w, v);
        gfx_Line(r, o, w, t);
        gfx_Line(p, o, u, t);
        gfx_Line(p, q, u, v);
    }
}


int main(void)
{

    gfx_Begin();
    gfx_ZeroScreen();

    struct Player player;

    player.x = 0;
    player.y = 0;
    player.z = 0;

    gfx_SetColor(gfx_red);
    gfx_SetTextFGColor(gfx_red);

    int frame_count = 0;
    clock_t start_time = clock();
    int fps = 0;

    while (!(kb_Data[6] == kb_Clear)) {

        if (kb_Data[7] == kb_Up) {
            player.z += 1;
        } else if (kb_Data[7] == kb_Down) {
            player.z -= 1;
        } else if (kb_Data[7] == kb_Left) {
            player.x -= 1;
        } else if (kb_Data[7] == kb_Right) {
            player.x += 1;
        } else if (kb_Data[1] == kb_2nd) {
            player.y += 1;
        } else if (kb_Data[2] == kb_Alpha) {
            player.y -= 1;
        }

        gfx_SwapDraw();

        gfx_ZeroScreen();
        gfx_SetTextXY(5,5);
        gfx_PrintInt(fps, 2);
        
        drawBox(2-player.x,2+player.y,1-player.z);
        drawBox(1-player.x,2+player.y,1-player.z);
        drawBox(0-player.x,2+player.y,1-player.z);
        drawBox(0-player.x,2+player.y,2-player.z);
        drawBox(2-player.x,3+player.y,1-player.z);
        drawBox(2-player.x,1+player.y,1-player.z);
        drawBox(2-player.x,0+player.y,1-player.z);


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
