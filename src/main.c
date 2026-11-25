// Voxel engine demo: a title screen (settings, a world list), then walk around the chosen world
// (world.h: any AppVar starting with WORLD_MAGIC). [clear] quits.
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
#include "title.h"
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
    playerSpawn();              // the title screen first, from the spawn; [play] spawns again
    uint8_t title = 1;
    uint8_t played = 0;         // a world was played: its last played date is saved at exit

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

    for (;;) {

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

        if (title) {
            const uint8_t choice = titleUpdate(dt);
            if (choice == TITLE_QUIT) {
                break;
            }
            if (choice == TITLE_PLAY) {
                // another world than the one behind the title: the loading screen while it is
                // checked (on an error the open one stays, and the title shows why)
                uint8_t err = WORLD_OK;
                const uint8_t w = titleWorld();
                if (w != world_current) {
                    displayReload();
                    loading = 1;
                    err = worldSelect(w);
                    if (err == WORLD_OK) {
                        coloursInit();
                        windowInit();       // windowFollow below loads it all
                    } else {
                        titleError(worldError(err));
                    }
                }
                if (err == WORLD_OK) {
                    playerSpawn();
                    title = 0;
                    played = 1;
                }
            }
        } else {
            if (kb_Data[6] & kb_Clear) {
                break;
            }
            playerUpdate(dt);
        }
        setupCamera();
        windowFollow();
        if (loading) {
            displayProgress(LOADING_FULL);
        }

        // cull, project and rasterize before the wait (fill.h: unless the span list overflows)
        projectScene(draw_buffer_ctrl);
        if (title) {
            titlePoll();
        }

        // idle time, so it is left out of the frame time
        uint24_t wait_cycles = CYCLES();
        waitForBufferScan();
        wait_cycles = CYCLES() - wait_cycles;
        if (title) {
            titlePoll();
        }

        // every viewport pixel of the field: the visible parts of the boxes and the background
        drawSpans();

        // copy the drawn rows to the rows below (all of them under the menu text), and show the frame
        if (title) {
            titleDraw();
            deinterlaceFrame(GFX_LCD_HEIGHT);
        } else if (hud_shown) {
            hudDraw(fps, num_projected, frame_cycles);
            deinterlaceFrame(HUD_ROWS);
        } else {
            deinterlaceFrame(0);
        }
        swapBuffer();
        if (loading) {
            displayLoadingEnd();     // the first frame replaces the loading screen
            loading = 0;
        }

        frame_cycles = CYCLES() - frame_start - wait_cycles;
    }

    // The world's last played date last (only if it was played, not just seen behind the title): archiving it may show the OS garbage collection screen,
    // and it needs free RAM, so a scratch AppVar (the bench's, or an older version's) goes first
    kb_SetMode(kb_mode);
    gfx_End();
    windowFree();
    worldClose(played);
    worldArchivePending();      // worlds renamed or created on the title
    return 0;
}
