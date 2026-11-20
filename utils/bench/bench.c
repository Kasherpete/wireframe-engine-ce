// Benchmark and verification harness (utils/bench/run_bench.sh). Replaces src/main.c in a scratch
// copy of the sources, and is built with SHOW_HUD for the cycle counters.
//
// Renders BENCH_VIEWS camera views from a fixed seed (positions in the BENCH_XMIN .. BENCH_ZMAX
// box of cells, 0.5 .. 2.5 units up, a quarter of them exactly N/E/S/W) and shows the total
// cycles of the window updates (chunk loads and exposure), projectScene (with the chunk sorts in
// it, also shown on their own), fillScene and drawSpans, the worst view, and the most boxes in a
// view. Checked on every view:
//   sort bad   the visible chunks are in order (keys near to far) and hold the same records
//   expo bad   after each window update, computeExposure of the inner 3 x 3 chunks against the
//              faces stored in the world file
//   deint bad  views where an LCD row pair differs after deinterlaceFrame
// The second column breaks projectScene down (cull, sorts, projectRecords) and adds the costs of
// a frame outside the scene: setupCamera and the de-interlace copy.
//   bad px     with -DFILL_REFERENCE each view is also drawn by fill_ref.c into BUFFER_2 and
//              compared byte for byte with BUFFER_1
// -DBENCH_LOD_RECT=a -DBENCH_LOD_PIX=b override the level of detail distances.
// -DBENCH_WALK walks instead: from cell (BENCH_XMIN, BENCH_ZMIN + 0.5) along +x at eye height
// 1.5, looking along +x, a quarter cell per view (as walking at 60 FPS crosses about 1/15 cell
// per frame, the loads and sorts per view are about 4 times a frame's), so "max load k" and the
// per-view maxima show the hitch of a chunk crossing.
#include <keypadc.h>
#include <fontlibc.h>

#include "camera.h"
#include "chunks.h"
#include "colours.h"
#include "display.h"
#include "fill.h"
#include "hud.h"
#include "player.h"
#include "scene.h"
#include "world.h"

#ifndef BENCH_VIEWS
#define BENCH_VIEWS 40
#endif
#ifndef BENCH_XMIN
#define BENCH_XMIN 4
#define BENCH_XMAX 44
#define BENCH_ZMIN 4
#define BENCH_ZMAX 44
#endif

static uint24_t sort_bad = 0, expo_bad = 0, deint_bad = 0;

// after deinterlaceFrame: every LCD row pair of the frame at buf holds the same bytes
static void checkDeinterlace(uint24_t buf) {
    for (uint8_t r = 0; r < VIEW_H; r++) {
        const uint8_t *row = (const uint8_t *)buf + (uint24_t)r * 320;
        for (uint8_t x = 0; x < 160; x++) {
            if (row[x] != row[x + 160]) {
                deint_bad++;
                return;
            }
        }
    }
}

// order-independent sum of the window's records
static uint24_t windowXor(void) {
    uint24_t x = 0;
    for (uint8_t w = 0; w < WINDOW_N; w++) {
        const chunk_t *ch = win[w];
        for (uint8_t i = 0; i < ch->n; i++) {
            uint24_t r = 0;
            for (uint8_t j = 0; j < 5; j++) r = r * 257 + (uint8_t)ch->rec[i][j];
            x += r * r + w;
        }
    }
    return x;
}

static int24_t iabs(int24_t v) { return v < 0 ? -v : v; }

// keys of each visible chunk near to far, from the camera cell
static void checkSort(void) {
    for (uint8_t k = 0; k < chunks_done; k++) {
        const uint8_t w = visit_order[k];
        const chunk_t *ch = win[w];
        if (ch->n == 0 || !chunk_visible[w]) continue;
        int24_t last = 0;
        for (uint8_t i = 0; i < ch->n; i++) {
            const int8_t *r = ch->rec[i];
            const int24_t k = iabs(ch->ox - 1 + r[0] - cam_cell_x) + iabs(r[1] - 1 - cam_cell_y) +
                              iabs(ch->oz - 1 + r[2] - cam_cell_z);
            if (k < last) { sort_bad++; return; }
            last = k;
        }
    }
}

// the engine's computeExposure against the faces make_world.py stored, for the inner 3 x 3 chunks
// (their 4 neighbours are all in the window)
static void checkExposure(void) {
    static uint8_t stored[CHUNK_MAX];
    for (uint8_t w = 0; w < WINDOW_N; w++) {
        const uint8_t wx = w % WINDOW, wz = w / WINDOW;
        if (wx == 0 || wx == WINDOW - 1 || wz == 0 || wz == WINDOW - 1) continue;
        chunk_t *ch = win[w];
        for (uint8_t i = 0; i < ch->n; i++) stored[i] = (uint8_t)ch->rec[i][4];
        computeExposure(w);
        for (uint8_t i = 0; i < ch->n; i++) {
            if (stored[i] != (uint8_t)ch->rec[i][4]) expo_bad++;
        }
    }
}

#ifndef BENCH_WALK
static uint32_t rng = 12345;
static uint24_t rnd24(void) { rng = rng * 1103515245u + 12345u; return (uint24_t)(rng >> 8); }
#endif

static void line(uint8_t x, uint8_t y, const char *s, uint24_t v) {
    fontlib_SetCursorPosition(x, y);
    fontlib_DrawString(s);
    fontlib_DrawUInt(v, 1);
}

int main(void) {
#ifdef BENCH_LOD_RECT
    lod_rect_dist = BENCH_LOD_RECT;
    lod_pixel_dist = BENCH_LOD_PIX;
#endif
    if (!worldStart()) return 0;
    coloursInit();
    playerSpawn();
    hudInit();
    displayInit();
    cameraInit();
    sceneInit();
    windowInit();
    setupCamera();
    windowFollow();

    uint32_t t_load = 0, t_proj = 0, t_fill = 0, t_draw = 0, t_max = 0, nboxes = 0, t_setup = 0, t_deint = 0;
    uint24_t max_boxes = 0, full = 0, max_load = 0;
#ifdef FILL_REFERENCE
    uint24_t mismatch = 0, badviews = 0;
#endif
    prof_sorts = 0;
    prof_sort_cycles = 0;
    prof_loads = 0;
    prof_cull_cycles = prof_proj_cycles = 0;
    prof_chunks = prof_records = 0;
    for (uint24_t v = 0; v < BENCH_VIEWS; v++) {
#ifdef BENCH_WALK
        player_x = ((int32_t)BENCH_XMIN << 16) + ((int32_t)v << 14);
        player_z = ((int32_t)BENCH_ZMIN << 16) + (1L << 15);
        player_y = -STAND_HEIGHT;
        player_yaw = 64L << 16;
#else
        const int24_t rx = (int24_t)(rnd24() % (uint24_t)((BENCH_XMAX - BENCH_XMIN) * 256));
        const int24_t rz = (int24_t)(rnd24() % (uint24_t)((BENCH_ZMAX - BENCH_ZMIN) * 256));
        player_x = ((int32_t)rx << 8) + ((int32_t)BENCH_XMIN << 16);
        player_z = ((int32_t)rz << 8) + ((int32_t)BENCH_ZMIN << 16);
        player_y = -((int32_t)(rnd24() % 512u) << 8) - (1L << 15);   // 0.5 .. 2.5 up
        player_yaw = rnd24();
        if ((v & 3) == 0) {
            player_yaw &= 0xc00000;
        }
#endif
        uint24_t ts = CYCLES();
        setupCamera();
        t_setup += CYCLES() - ts;
        const uint24_t loads = prof_loads;
        uint24_t t0 = CYCLES();
        windowFollow();
        t0 = CYCLES() - t0;
        t_load += t0;
        if (t0 > max_load) max_load = t0;
        if (prof_loads != loads) {
            checkExposure();
        }
        const uint24_t before = windowXor();
        setDrawBuffer(BUFFER_1);
        t0 = CYCLES();
        projectScene();
        uint24_t t1 = CYCLES();
        fillScene(projected, num_projected, BUFFER_1);
        uint24_t t2 = CYCLES();
        drawSpans();
        uint24_t t3 = CYCLES();
        deinterlaceFrame(0);
        t_deint += CYCLES() - t3;
        checkDeinterlace(BUFFER_1);
        t_proj += t1 - t0;
        t_fill += t2 - t1;
        t_draw += t3 - t2;
        if (t3 - t0 > t_max) t_max = t3 - t0;
        nboxes += num_projected;
        if (num_projected > max_boxes) max_boxes = num_projected;
        if (num_projected == MAX_PROJECTED) full++;
        if (windowXor() != before) sort_bad++;
        checkSort();
#ifdef FILL_REFERENCE
        fillSceneRef(projected, num_projected, BUFFER_2);
        drawSpansRef();
        uint24_t bad = 0;
        for (uint24_t r = 0; r < 120; r++) {
            const uint8_t *a = (const uint8_t *)BUFFER_1 + r * 320, *b = (const uint8_t *)BUFFER_2 + r * 320;
            for (uint8_t x = 0; x < 160; x++) {
                if (a[x] != b[x]) bad++;
            }
        }
        mismatch += bad;
        if (bad) badviews++;
#endif
    }

    setDrawBuffer(BUFFER_1);
    setScreenBuffer(BUFFER_1);
    fontlib_SetColors(0x00, 0x77);
    fontlib_SetTransparency(false);
    line(3, 3,  "views ", BENCH_VIEWS);
    line(3, 11, "boxes ", (uint24_t)nboxes);
    line(3, 19, "proj k ", (uint24_t)(t_proj / 1000));
    line(3, 27, "fill k ", (uint24_t)(t_fill / 1000));
    line(3, 35, "draw k ", (uint24_t)(t_draw / 1000));
    line(3, 43, "tot k ", (uint24_t)((t_proj + t_fill + t_draw) / 1000));
    line(3, 51, "max k ", (uint24_t)(t_max / 1000));
    line(3, 59, "most ", max_boxes);
    line(3, 67, "full ", full);
#ifdef FILL_REFERENCE
    line(3, 75, "bad px ", mismatch);
    line(3, 83, "bad views ", badviews);
#endif
    line(84, 3,  "sorts ", prof_sorts);
    line(84, 11, "sort k ", (uint24_t)(prof_sort_cycles / 1000));
    line(84, 19, "loads ", prof_loads);
    line(84, 27, "load k ", (uint24_t)(t_load / 1000));
    line(84, 35, "sort bad ", sort_bad);
    line(84, 43, "expo bad ", expo_bad);
    line(84, 51, "max load k ", max_load / 1000);
    line(84, 59, "cull k ", (uint24_t)(prof_cull_cycles / 1000));
    line(84, 67, "prec k ", (uint24_t)(prof_proj_cycles / 1000));
    line(84, 75, "chunks ", prof_chunks);
    line(84, 83, "rec/v ", prof_records / BENCH_VIEWS);
    line(84, 91, "setup k ", (uint24_t)(t_setup / 1000));
    line(84, 99, "deint k ", (uint24_t)(t_deint / 1000));
    line(84, 83 + 8 * 3, "deint bad ", deint_bad);
    deInterlaceFullBuffer(BUFFER_1);
    while (!(kb_Data[6] & kb_Clear)) {
        kb_Scan();
    }
    gfx_End();
    return 0;
}
