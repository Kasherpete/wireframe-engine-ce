// Benchmark and verification harness (utils/bench/run_bench.sh). Included at the end of a scratch
// copy of main.c, whose own main() is renamed, so it sees every static function and variable.
//
// Renders BENCH_VIEWS camera views from a fixed seed (positions in the BENCH_XMIN .. BENCH_ZMAX
// box, 0.5 .. 2.5 units up, a quarter of them exactly N/E/S/W) and shows the total cycles of
// projectScene, fillScene and drawSpans, the worst view, and every re-sort checked (keys far to
// near, same records) with its average cost. With -DFILL_REFERENCE each view is also drawn by
// fill_ref.c into BUFFER_2 and compared byte for byte with BUFFER_1: "bad px" must be 0.
// -DBENCH_LOD_RECT=a -DBENCH_LOD_PIX=b override the level of detail distances.
#ifndef BENCH_VIEWS
#define BENCH_VIEWS 40
#endif
#ifndef BENCH_XMIN
#define BENCH_XMIN -6
#define BENCH_XMAX 6
#define BENCH_ZMIN -5
#define BENCH_ZMAX 8
#endif

// the scene order: keys non-increasing (far to near), and the same records as before
static uint24_t sort_bad = 0, sorts = 0;
static uint32_t sort_cycles = 0;
static uint24_t sceneSum(void) {
    uint24_t sum = 0;
    for (uint8_t i = 0; i < LEN(coordinates); i++) {
        for (uint8_t j = 0; j < 5; j++) sum = sum * 31 + (uint8_t)coordinates[i][j] * (j + 1);
    }
    return sum;
}
static uint24_t sceneXor(void) {
    uint24_t x = 0;
    for (uint8_t i = 0; i < LEN(coordinates); i++) {
        uint24_t r = 0;
        for (uint8_t j = 0; j < 5; j++) r = r * 257 + (uint8_t)coordinates[i][j];
        x += r * r;
    }
    return x;
}
static void checkSort(void) {
    const uint24_t before = sceneXor();
    const uint24_t ts = CYCLES();
    sortScene();
    sort_cycles += CYCLES() - ts;
    sorts++;
    if (sceneXor() != before) { sort_bad++; return; }
    uint8_t last = 255;
    for (uint8_t i = 0; i < LEN(coordinates); i++) {
        const int24_t dx = coordinates[i][0] + cam_bias_x - GRID_RANGE;
        const int24_t dy = coordinates[i][1] + cam_bias_y - GRID_RANGE;
        const int24_t dz = coordinates[i][2] + cam_bias_z - GRID_RANGE;
        if (dx < -GRID_RANGE || dx >= GRID_RANGE || dy < -GRID_RANGE || dy >= GRID_RANGE ||
            dz < -GRID_RANGE || dz >= GRID_RANGE) continue;   // keys of culled boxes may wrap
        const uint8_t k = (uint8_t)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + (dz < 0 ? -dz : dz));
        if (k > last) { sort_bad++; return; }
        last = k;
    }
}
static uint32_t rng = 12345;
static uint24_t rnd24(void) { rng = rng * 1103515245u + 12345u; return (uint24_t)(rng >> 8); }

static void line(uint8_t y, const char *s, uint24_t v) {
    fontlib_SetCursorPosition(3, y);
    fontlib_DrawString(s);
    fontlib_DrawUInt(v, 1);
}

int main(void) {
#ifdef BENCH_LOD_RECT
    lod_rect_dist = BENCH_LOD_RECT;
    lod_pixel_dist = BENCH_LOD_PIX;
#endif
    initHud();
    init_screen();
    computeExposure();
    initFaceTables();
    initGridDist();
    initLodSlots();
    setupCamera();
    sortScene();

    uint32_t t_proj = 0, t_fill = 0, t_draw = 0, t_max = 0, nboxes = 0;
    uint24_t mismatch = 0, badviews = 0;
    for (uint24_t v = 0; v < BENCH_VIEWS; v++) {
        const int24_t rx = (int24_t)(rnd24() % (uint24_t)((BENCH_XMAX - BENCH_XMIN) * 256));
        const int24_t rz = (int24_t)(rnd24() % (uint24_t)((BENCH_ZMAX - BENCH_ZMIN) * 256));
        player_x = ((int32_t)rx << 8) + ((int32_t)BENCH_XMIN << 16);
        player_z = ((int32_t)rz << 8) + ((int32_t)BENCH_ZMIN << 16);
        player_y = -((int32_t)(rnd24() % 512u) << 8) - (1L << 15);   // 0.5 .. 2.5 up
        player_yaw = rnd24();
        if ((v & 3) == 0) {
            player_yaw &= 0xc00000;
        }
        setupCamera();
        if (sceneOrderStale()) {
            checkSort();
        }
        setDrawBuffer(BUFFER_1);
        uint24_t t0 = CYCLES();
        projectScene();
        uint24_t t1 = CYCLES();
        fillScene(projected, num_projected, BUFFER_1);
        uint24_t t2 = CYCLES();
        drawSpans();
        uint24_t t3 = CYCLES();
        t_proj += t1 - t0;
        t_fill += t2 - t1;
        t_draw += t3 - t2;
        if (t3 - t0 > t_max) t_max = t3 - t0;
        nboxes += num_projected;
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
    line(3,  "views ", BENCH_VIEWS);
    line(11, "boxes ", (uint24_t)nboxes);
    line(19, "proj k ", (uint24_t)(t_proj / 1000));
    line(27, "fill k ", (uint24_t)(t_fill / 1000));
    line(35, "draw k ", (uint24_t)(t_draw / 1000));
    line(43, "tot k ", (uint24_t)((t_proj + t_fill + t_draw) / 1000));
    line(51, "max k ", (uint24_t)(t_max / 1000));
#ifdef FILL_REFERENCE
    line(59, "bad px ", mismatch);
    line(67, "bad views ", badviews);
#endif
    fontlib_SetCursorPosition(110, 3); fontlib_DrawString("sort ");
    fontlib_DrawUInt(sort_bad, 1); fontlib_DrawString("/"); fontlib_DrawUInt(sorts, 1);
    fontlib_SetCursorPosition(110, 11); fontlib_DrawString("avg ");
    fontlib_DrawUInt((uint24_t)(sort_cycles / (sorts ? sorts : 1)), 1);
    deInterlaceFullBuffer(BUFFER_1);
    while (!(kb_Data[6] & kb_Clear)) {
        kb_Scan();
    }
    gfx_End();
    return 0;
}
