// Voxel engine demo: a title screen (settings, a world list), then walk around the chosen world
// (world.h: any AppVar starting with WORLD_MAGIC). [clear] opens the pause menu (title.h), which
// resumes or quits, with or without saving the position. With no world at startup the title
// says where to get one, over a black screen.
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

// Not per-box code: built for size (CLAUDE.md, "Memory"). Saves ~15 KB of RAM over -O3 inlining
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)

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
    const uint8_t start_err = worldStart();
    if (start_err != WORLD_OK) {
        titleNoWorld(world_count ? worldError(start_err) : NULL);   // world stays NULL
    }
    coloursInit();
    playerSpawn();              // the title screen first, from the spawn; [play] spawns again
    uint8_t title = 1;
    uint8_t paused = 0;         // the pause menu over the frozen world
    uint8_t save = 0;           // Save & Quit: the date and the position are saved at exit
    uint8_t clear_last = 0;     // [clear] was down last frame (a held one pauses once)

    hudInit();
    displayInit();

    cameraInit();
    sceneInit();
    windowInit();
    if (world) {
        setupCamera();
        windowFollow();
    }
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

        if (title || paused) {
            const uint8_t choice = titleUpdate(dt);
            if (choice == TITLE_QUIT) {
                break;
            }
            if (choice == TITLE_SAVE_QUIT) {
                save = 1;
                playerSave();
                break;
            }
            uint8_t start = choice == TITLE_START;      // after the low memory notice
            if (choice == TITLE_RESUME) {
                paused = 0;
                clear_last = kb_Data[6] & kb_Clear;     // [clear] resumed: not a pause again
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
                    // the game takes most of the RAM: a large archived world may not fit to be
                    // saved. Say so first (from its spawn behind the notice), then play it anyway
                    uint24_t need, avail;
                    playerSpawn();
                    if (worldCanSave(&need, &avail)) {
                        start = 1;
                    } else {
                        titleLowMemory(need, avail);
                    }
                }
            }
            if (start) {
                playerSpawn();
                title = 0;
                clear_last = kb_Data[6] & kb_Clear;
            }
        } else {
            const uint8_t clear = kb_Data[6] & kb_Clear;
            if (clear && !clear_last) {
                paused = 1;
                titlePause(worldCanSave(NULL, NULL));
            } else {
                playerUpdate(dt);
            }
            clear_last = clear;
        }
        const uint8_t menu = title || paused;
        if (world) {
            setupCamera();
            windowFollow();
        }
        if (loading) {
            displayProgress(LOADING_FULL);
        }

        // cull, project and rasterize before the wait (fill.h: unless the span list overflows)
        if (world) {
            projectScene(draw_buffer_ctrl);
        }
        if (menu) {
            titlePoll();
        }

        // idle time, so it is left out of the frame time
        uint24_t wait_cycles = CYCLES();
        // waitForBufferScan();
        wait_cycles = CYCLES() - wait_cycles;
        if (menu) {
            titlePoll();
        }

        // copy the drawn rows to the rows below (all of them under the menu text), and show the frame
        if (!world) {
            // no world (only on the title): black, the even rows, then copied down
            gfx_SetColor(0x00);
            gfx_FillRectangle(0, 0, GFX_LCD_WIDTH, GFX_LCD_HEIGHT);
            titleDraw();
            deinterlaceRows((uint8_t *)draw_buffer_ctrl, GFX_LCD_WIDTH);
        } else if (menu) {
            // every viewport pixel of the field: the visible parts of the boxes and the background
            drawSpans();
            titleDraw();
            deinterlaceFrame(GFX_LCD_HEIGHT);
        } else if (hud_shown) {
            drawSpans();
            hudDraw(fps, num_projected, frame_cycles);
            deinterlaceFrame(HUD_ROWS);
        } else {
            drawSpans();
            deinterlaceFrame(0);
        }
        swapBuffer();
        if (loading) {
            displayLoadingEnd();     // the first frame replaces the loading screen
            loading = 0;
        }

        frame_cycles = CYCLES() - frame_start - wait_cycles;
    }

    // The world's last played date and the player's position last (only on Save & Quit): archiving
    // it may show the OS garbage collection screen, and it needs free RAM, so a scratch AppVar (the
    // bench's, or an older version's) goes first
    kb_SetMode(kb_mode);
    gfx_End();
    windowFree();
    if (!worldClose(save) && save) {
        // not enough free RAM to move the archived world to RAM (worldCanSave)
        displayError("Not saved: not enough", "free RAM for this world.");
    }
    worldArchivePending();      // worlds renamed or created on the title
    return 0;
}

#pragma clang attribute pop
