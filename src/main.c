// #include <ti/screen.h>
#include <keypadc.h>
#include <stdlib.h>
#include <graphx.h>
#include <sys/timers.h>
#include <math.h>
#include <time.h>
#include <stdio.h>


#define FOV 0.9
#define LEN(arr) ((int) (sizeof (arr) / sizeof (arr)[0]))

const int GFX_HEIGHT_HALF = GFX_LCD_HEIGHT / 2;
const int GFX_WIDTH_HALF = GFX_LCD_WIDTH / 2;


struct Player
{
    char x;
    char y;
    char z;
};


void drawBox(int x, int y, int z) {

    // just some optimizations
    const int powAZx50 = pow(FOV,z) * 50;
    const int powAZp1x50 = pow(FOV,(z+1)) * 50;

    // calculate values
    int w = (x*powAZx50) + GFX_WIDTH_HALF;
    int v = (y*powAZx50) + GFX_HEIGHT_HALF;
    int u = ((x-1)*powAZx50) + GFX_WIDTH_HALF;
    int t = ((y-1)*powAZx50) + GFX_HEIGHT_HALF;
    int r = (x*powAZp1x50) + GFX_WIDTH_HALF;
    int q = (y*powAZp1x50) + GFX_HEIGHT_HALF;
    int p = ((x-1)*powAZp1x50) + GFX_WIDTH_HALF;
    int o = ((y-1)*powAZp1x50) + GFX_HEIGHT_HALF;

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
