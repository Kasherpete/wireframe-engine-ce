// Collision test (utils/test/run_collide_test.sh): replaces src/main.c in a scratch copy, where
// player.c reads its keys from test_keys[] instead of the keypad. Runs scripted scenarios, each
// SCENARIO_TICKS long. Shows the current one at the top left, and a table of every finished one:
// final eye x, y, z (1/256 units), the highest eye y reached, the average and the most cycles
// playerUpdate
// took in a frame (both in hundreds of cycles).
#include <keypadc.h>
#include <fontlibc.h>
#include <time.h>

#include "camera.h"
#include "chunks.h"
#include "colours.h"
#include "display.h"
#include "fill.h"
#include "hud.h"
#include "player.h"
#include "scene.h"
#include "world.h"

uint8_t test_keys[8];

#define SCENARIO_TICKS (8 * CLOCKS_PER_SEC)

typedef struct {
    int24_t x, z;          // start, 1/256 units
    uint8_t yaw;           // steps
    uint8_t arrows, group1, group2;   // held keys (kb_Data[7], [1], [2])
    const char *name;
} scenario_t;

static const scenario_t scenarios[] = {
    {24 * 256 + 128, 24 * 256 + 128, 0, 0, 0, 0, "idle"},
    {24 * 256 + 128, 24 * 256 + 128, 64, kb_Up, 0, 0, "pillar +x"},
    {1 * 256 + 128, 13 * 256 + 128, 64, kb_Up, kb_2nd, 0, "stairs"},
    {16 * 256 + 128, 27 * 256 + 128, 0, kb_Up, 0, 0, "door"},
    {16 * 256 + 128, 32 * 256 + 128, 0, 0, kb_2nd, 0, "roof jump"},
    {24 * 256 + 128, 24 * 256 + 128, 40, kb_Up, 0, 0, "diag slide"},
    {16 * 256 + 128, 32 * 256 + 128, 0, 0, 0, kb_Alpha, "crouch"},
    {4 * 256 + 128, 13 * 256 + 128, 64, 0, 0, 0, "spawn in step"},
};
#define N_SCEN (sizeof scenarios / sizeof scenarios[0])

typedef struct {
    int24_t x, y, z, top;
    uint24_t cycles, avg;
    uint8_t done;
} result_t;
static result_t results[8];

static void num(uint8_t x, uint8_t y, const char *s, int24_t v) {
    fontlib_SetCursorPosition(x, y);
    fontlib_DrawString(s);
    if (v < 0) {
        fontlib_DrawGlyph('-');
        v = -v;
    }
    fontlib_DrawUInt((uint24_t)v, 1);
}

int main(void) {
    if (!windowAlloc()) return 0;
    if (worldStart() != WORLD_OK) {
        displayError("No usable world", NULL);
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
    kb_SetMode(MODE_3_CONTINUOUS);

    uint8_t s = 0;
    clock_t last = clock(), start = last;
    int24_t top = 0;
    uint24_t max_cycles = 0;
    uint32_t sum_cycles = 0;
    uint24_t frames = 1;
    uint8_t started = 0;
    while (!(kb_Data[6] & kb_Clear)) {
        const clock_t now = clock();
        uint24_t dt = now - last;
        last = now;
        if (dt > CLOCKS_PER_SEC / 8) dt = CLOCKS_PER_SEC / 8;
        if (!started || now - start >= SCENARIO_TICKS) {
            if (started) {
                result_t *r = &results[s];
                r->x = (int24_t)(player_x >> 8);
                r->y = (int24_t)(player_y >> 8);
                r->z = (int24_t)(player_z >> 8);
                r->top = top;
                r->cycles = max_cycles;
                r->avg = (uint24_t)(sum_cycles / frames);
                r->done = 1;
                s = (uint8_t)((s + 1) % N_SCEN);
            }
            started = 1;
            start = now;
            const scenario_t *c = &scenarios[s];
            playerSpawn();
            player_x = (int32_t)c->x << 8;
            player_z = (int32_t)c->z << 8;
            player_yaw = (uint24_t)c->yaw << 16;
            test_keys[7] = c->arrows;
            test_keys[1] = c->group1;
            test_keys[2] = c->group2;
            top = 0;
            max_cycles = 0;
            sum_cycles = 0;
            frames = 1;
        }
        const uint24_t t0 = CYCLES();
        playerUpdate(dt);
        const uint24_t t1 = CYCLES() - t0;
        if (t1 > max_cycles) max_cycles = t1;
        sum_cycles += t1;
        frames++;
        const int24_t ey = (int24_t)(player_y >> 8);
        if (ey < top) top = ey;
        setupCamera();
        windowFollow();
        projectScene(draw_buffer_ctrl);
        waitForBufferScan();
        drawSpans();
        fontlib_SetCursorPosition(3, 3);
        fontlib_DrawUInt(s, 1);
        fontlib_DrawGlyph(' ');
        fontlib_DrawString(scenarios[s].name);
        num(3, 11, "x ", (int24_t)(player_x >> 8));
        num(3, 19, "y ", ey);
        num(3, 27, "z ", (int24_t)(player_z >> 8));
        num(3, 35, "top ", top);
        num(3, 43, "cyc ", (int24_t)max_cycles);
        for (uint8_t i = 0; i < N_SCEN; i++) {
            const result_t *r = &results[i];
            if (!r->done) continue;
            const uint8_t y = (uint8_t)(56 + 8 * i);
            num(3, y, "", i);
            num(12, y, "", r->x);
            num(38, y, "", r->y);
            num(64, y, "", r->z);
            num(90, y, "", r->top);
            num(116, y, "", (int24_t)(r->avg / 100));
            num(136, y, "", (int24_t)(r->cycles / 100));
        }
        deinterlaceFrame(HUD_ROWS);
        swapBuffer();
    }
    gfx_End();
    windowFree();
    return 0;
}
