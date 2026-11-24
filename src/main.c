// Voxel engine demo: walk around the world in the WORLD AppVar. [clear] quits.
//
// A frame: move the player, set up the camera, load chunks that came into view, project and
// rasterize the scene (before the wait: none of it touches a frame buffer, so it overlaps the LCD
// showing the last frame), wait for the back buffer, draw the spans, de-interlace and swap.
#include <keypadc.h>
#include <time.h>
#include <sys/timers.h>

#include "camera.h"
#include "chunks.h"
#include "colours.h"
#include "display.h"
#include "fill.h"
#include "hud.h"
#include "player.h"
#include "scene.h"
#include "world.h"

#define MAX_DT  (CLOCKS_PER_SEC / 8)  // clamp the frame time so a stall does not teleport the player

int main(void) {

    // the loading screen, first: the world's CRC check takes a while (~2 s for 36 KB)
    displayLoading();
    uint8_t loading = 1;

    // wait until the keys that started the program are released
    while ((kb_Data[1] & kb_2nd) || (kb_Data[6] & kb_Enter)) {
        msleep(100);
        kb_Scan();
    }

    // the chunk memory (bss and VRAM; the bench adds a scratch AppVar, created before the world is
    // opened, as creating a variable may move it), then the world file. On an error displayError
    // ends graphx and reports it
    if (!windowAlloc()) {
        return 0;
    }
    if (!worldStart()) {
        windowFree();
        return 0;
    }
    coloursInit();
    playerSpawn();

    hudInit();
    displayInit();

    cameraInit();
    sceneInit();
    windowInit();
    setupCamera();
    windowFollow();
    displayProgress(LOADING_FULL);

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

        // FPS, once a second
        if (now - start_time >= CLOCKS_PER_SEC) {
            fps = frame_count;
            frame_count = 0;
            start_time = now;
        }

        playerUpdate(dt);
        setupCamera();
        windowFollow();

        // cull, project and rasterize before the wait (fill.h: unless the span list overflows)
        const uint24_t project_start = CYCLES();
        projectScene(draw_buffer_ctrl);
        const uint24_t project_cycles = CYCLES() - project_start;

        // idle time, so it is left out of the frame time
        uint24_t wait_cycles = CYCLES();
        waitForBufferScan();
        wait_cycles = CYCLES() - wait_cycles;

        // every viewport pixel of the field: the visible parts of the boxes and the background
        const uint24_t draw_start = CYCLES();
        drawSpans();
        const uint24_t draw_cycles = CYCLES() - draw_start + project_cycles;

        hudDraw(fps, draw_cycles, frame_cycles);

        // copy the drawn rows to the rows below, and show the frame
        deinterlaceFrame(HUD_ROWS);
        swapBuffer();
        if (loading) {
            displayLoadingEnd();     // the first frame replaces the loading screen
            loading = 0;
        }

        frame_cycles = CYCLES() - frame_start - wait_cycles;
    }

    // The world's last played date last: archiving it may show the OS garbage collection screen,
    // and it needs free RAM, so a scratch AppVar (the bench's, or an older version's) goes first
    kb_SetMode(kb_mode);
    gfx_End();
    windowFree();
    worldClose();
    return 0;
}
