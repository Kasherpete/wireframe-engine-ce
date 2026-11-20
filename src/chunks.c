// The chunk window (chunks.h): loading, painter's sort, exposure.
#include <string.h>
#include <fileioc.h>

#include "chunks.h"
#include "camera.h"
#include "collide.h"
#include "display.h"
#include "hud.h"

// chunk (cx, cz) lives in slot (cx mod 7) * 7 + (cz mod 7). The first CHUNKS_BSS slots are in
// bss, the next CHUNKS_VRAM in DBL_BUF2, the rest in the scratch AppVar (windowAlloc)
static chunk_t  chunks_bss[CHUNKS_BSS];
static chunk_t *slot[WINDOW_N];
static uint8_t  scratch_handle;
chunk_t *win[WINDOW_N];
int24_t  win_cx, win_cz;
uint8_t  visit_order[WINDOW_N];


// ---- painter's sort ----

// sort.S. gridDist[n] = |d| for d = n - GRID_RANGE as a signed byte: the distance in cells along
// one axis for the low byte of the grid index n (projectRecords' index: n - GRID_RANGE is the box
// cell minus the camera's). Exact for |d| <= 127, which covers every record of the window (MAX_DXZ,
// MAX_DY in chunks.h), so the key of every record is exact, culled or not: sortChunk relies on it.
// Page-aligned
extern uint8_t gridDist[256];

// sort.S: sorts the n records near to far for camera biases bx, by, bz (the low bytes of the grid
// index biases), through scratch[n] and keys[n]. n must be 1 .. 255. ~480 cycles per record
void sortRecords(int8_t (*rec)[5], int8_t (*scratch)[5], uint8_t *keys, uint8_t n,
                 uint8_t bx, uint8_t by, uint8_t bz);

static int8_t  sort_scratch[CHUNK_MAX][5];
static uint8_t sort_key[CHUNK_MAX];

static void initGridDist(void) {
    uint8_t n = 0;
    do {
        const int8_t d = (int8_t)(uint8_t)(n - GRID_RANGE);
        gridDist[n] = (uint8_t)(d < 0 ? -d : d);
    } while (++n != 0);
}

// Where the camera cell c is relative to the cells lo .. hi on one axis: below, above, or the
// offset inside. The order of a chunk only changes when one of these does (chunks.h)
#define REL_BELOW  -1
#define REL_ABOVE  0x7fff
static int24_t sortRel(int24_t c, int24_t lo, int24_t hi) {
    if (c < lo) {
        return REL_BELOW;
    }
    if (c > hi) {
        return REL_ABOVE;
    }
    return c - lo;
}

// ifs, not a combined 0/1 value: see the compiler bug in CLAUDE.md
void sortChunk(chunk_t *ch) {
    const int24_t rx = sortRel(cam_cell_x, ch->ox - 1, ch->ox + CHUNK_SIZE - 2);
    const int24_t ry = sortRel(cam_cell_y, ch->ymin - 1, ch->ymax - 1);
    const int24_t rz = sortRel(cam_cell_z, ch->oz - 1, ch->oz + CHUNK_SIZE - 2);
    if (ch->sorted) {
        if (rx == ch->sort_rx) {
            if (ry == ch->sort_ry) {
                if (rz == ch->sort_rz) {
                    return;
                }
            }
        }
    }
    PROF(const uint24_t t = CYCLES();)
    sortRecords(ch->rec, sort_scratch, sort_key, ch->n, (uint8_t)(cam_bias_x + ch->ox),
                (uint8_t)cam_bias_y, (uint8_t)(cam_bias_z + ch->oz));
    PROF(prof_sort_cycles += CYCLES() - t; prof_sorts++;)
    ch->sort_rx = rx;
    ch->sort_ry = ry;
    ch->sort_rz = rz;
    ch->sorted = 1;
}


// ---- loading ----

// chunk.S: the n file voxels as records {x, y + 1, z, material, faces}, and the least and greatest
// record Y in yrange[0] and [1]
void copyVoxels(int8_t (*rec)[5], const world_voxel_t *v, uint8_t n, int8_t *yrange);

// Copies chunk (cx, cz) from the world file into ch (an empty chunk outside the world)
static void loadChunk(chunk_t *ch, int24_t cx, int24_t cz) {
    uint8_t n;
    const world_voxel_t *v = worldChunk(cx, cz, &n);
    ch->cx = cx;
    ch->cz = cz;
    ch->ox = cx * CHUNK_SIZE + 1;
    ch->oz = cz * CHUNK_SIZE + 1;
    ch->n = n;
    ch->sorted = 0;
    copyVoxels(ch->rec, v, n, &ch->ymin);
}


// ---- exposure ----
// Faces that no other voxel covers: a face shared with a neighbouring voxel can never be seen, so
// projectRecords never hands it to the rasterizer. With a ground plane, everything at y >= 0
// counts as solid: a voxel standing on the ground has no bottom face, a path tile only a top.
//
// The world file stores every voxel's faces (make_world.py computes them with the whole world at
// hand), so loading a chunk is a copy. computeExposure is for runtime changes and must give exactly
// what make_world.py gives (the bench checks that).
//
// computeExposure(w) marks the chunk, and the voxels of its 4 neighbours that touch it, in an
// occupancy grid: the chunk's cells plus a border of one, from one layer above its highest voxel
// to one below its lowest (and every cell at y >= 0 with a ground plane). Then each voxel looks at
// its 6 neighbouring cells. A neighbour chunk outside the window counts as empty: the faces
// towards it point away from the camera (the camera is in the centre chunk), so they are never
// drawn anyway. The grid is in DBL_BUF2, the VRAM frame shown only during displayInit.
#define OCC          ((uint8_t *)DBL_BUF2)
#define OCC_LAYERS   (WORLD_Y_MAX - WORLD_Y_MIN + 3)          // y: a chunk's range, plus one each side
_Static_assert(OCC_ROW == CHUNK_SIZE + 2 && OCC_LAYER == OCC_ROW * OCC_ROW, "x and z: -1 .. 8");
// (DBL_BUF2 from DBL_BUF2_FREE on holds chunks, or in the bench fill_ref.c's span list)
_Static_assert(OCC_ROW + 1 + OCC_LAYERS * OCC_LAYER <= DBL_BUF2_FREE, "the occupancy grid must fit the start of DBL_BUF2");

// occ_layer[L]: the address of cell (x 0, layer L, z 0), so cell (x, L, z) is at
// occ_layer[L] + OCC_ROW z + x for x and z from -1 to CHUNK_SIZE. Layer L is Y - ymin + 1.
// Read by chunk.S
uint8_t *occ_layer[OCC_LAYERS];

static void initOccupancy(void) {
    uint8_t *p = OCC + OCC_ROW + 1;
    for (uint8_t i = 0; i < OCC_LAYERS; i++) {
        occ_layer[i] = p;
        p += OCC_LAYER;
    }
}

// chunk.S, layer L = Y - ybase (n may be 0):
// occMark sets the cell of each record to 1.
// occNeighbour marks the records of a neighbour chunk whose coordinate axis (0: x, 2: z) is edge
// and whose layer is 1 .. ylen, at offset `at` (x = -1 or CHUNK_SIZE; for z, that times OCC_ROW).
// occExpose stores the FACE_* bits of each record's empty neighbour cells in rec[i][4].
void occMark(const int8_t (*rec)[5], uint8_t n, uint8_t ybase);
void occNeighbour(const int8_t (*rec)[5], uint8_t n, uint8_t axis, uint8_t edge, int24_t at,
                  uint8_t ybase, uint8_t ylen);
void occExpose(int8_t (*rec)[5], uint8_t n, uint8_t ybase);

void computeExposure(uint8_t w) {
    chunk_t *ch = win[w];
    if (ch->n == 0) {
        return;
    }
    const int8_t ymin = ch->ymin, ymax = ch->ymax;
    const uint8_t ybase = (uint8_t)(ymin - 1);
    // layers 0 .. ymax - ymin + 2 (at most OCC_LAYERS - 1), from cell (-1, 0, -1). With a ground
    // plane, the layers of Y >= 1 (y cell >= 0) are all solid
    const uint8_t last = (uint8_t)(ymax - ymin + 2);
    uint8_t *const start = occ_layer[0] - OCC_ROW - 1;
    uint8_t *const end = occ_layer[last] + OCC_LAYER - OCC_ROW - 1;
    uint8_t *solid = end;                   // the first solid layer
    if (world->ground != WORLD_NO_GROUND) {
        const int24_t l = 2 - (int24_t)ymin;    // the layer of Y = 1
        if (l <= 0) {
            solid = start;
        } else if (l <= last) {
            solid = occ_layer[l] - OCC_ROW - 1;
        }
    }
    memset(start, 0, (size_t)(solid - start));
    memset(solid, 1, (size_t)(end - solid));

    const uint8_t ylen = (uint8_t)(ymax - ymin + 1);

    occMark(ch->rec, ch->n, ybase);
    const uint8_t wx = w % WINDOW;
    if (wx > 0) {
        const chunk_t *nb = win[w - 1];
        occNeighbour(nb->rec, nb->n, 0, CHUNK_SIZE - 1, -1, ybase, ylen);
    }
    if (wx < WINDOW - 1) {
        const chunk_t *nb = win[w + 1];
        occNeighbour(nb->rec, nb->n, 0, 0, CHUNK_SIZE, ybase, ylen);
    }
    if (w >= WINDOW) {
        const chunk_t *nb = win[w - WINDOW];
        occNeighbour(nb->rec, nb->n, 2, CHUNK_SIZE - 1, -OCC_ROW, ybase, ylen);
    }
    if (w < WINDOW_N - WINDOW) {
        const chunk_t *nb = win[w + WINDOW];
        occNeighbour(nb->rec, nb->n, 2, 0, CHUNK_SIZE * OCC_ROW, ybase, ylen);
    }
    occExpose(ch->rec, ch->n, ybase);
}


// ---- the window ----

#define SCRATCH_SIZE  ((WINDOW_N - CHUNKS_BSS - CHUNKS_VRAM) * sizeof(chunk_t))
_Static_assert(SCRATCH_SIZE < 65000, "an AppVar holds at most 65505 bytes");
_Static_assert(DBL_BUF2_FREE + CHUNKS_VRAM * sizeof(chunk_t) <= DBL_BUF2_END - DBL_BUF2, "the chunks must fit DBL_BUF2");
#ifndef FILL_REFERENCE
_Static_assert(SCRATCH_SIZE <= 15 * 1024, "the message below");
#endif

// The AppVar's data pointer stays valid as long as no variable is created, deleted, resized or
// (un)archived: nothing does that between here and windowFree. The VRAM slots are only written
// from windowInit on, after displayInit has shown DBL_BUF2 for the last time
uint8_t windowAlloc(void) {
    scratch_handle = ti_Open(CHUNKS_APPVAR, "w");
    if (scratch_handle && ti_Resize(SCRATCH_SIZE, scratch_handle) == (int)SCRATCH_SIZE) {
        chunk_t *p = ti_GetDataPtr(scratch_handle);
        chunk_t *vram = (chunk_t *)(DBL_BUF2 + DBL_BUF2_FREE);
        for (uint8_t i = 0; i < WINDOW_N; i++) {
            if (i < CHUNKS_BSS) {
                slot[i] = &chunks_bss[i];
            } else if (i < CHUNKS_BSS + CHUNKS_VRAM) {
                slot[i] = vram++;
            } else {
                slot[i] = p++;
            }
        }
        return 1;
    }
    windowFree();
    displayError("Not enough free RAM: the", "chunks need 15 KB");
    return 0;
}

void windowFree(void) {
    if (scratch_handle) {
        ti_Close(scratch_handle);
        scratch_handle = 0;
    }
    ti_Delete(CHUNKS_APPVAR);
}

void windowInit(void) {
    initGridDist();
    initOccupancy();

    // visit_order: the window positions by Manhattan distance from the centre
    uint8_t k = 0;
    for (uint8_t d = 0; d <= 2 * WINDOW_HALF; d++) {
        for (uint8_t w = 0; w < WINDOW_N; w++) {
            const int8_t dx = (int8_t)(w % WINDOW) - WINDOW_HALF, dz = (int8_t)(w / WINDOW) - WINDOW_HALF;
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) == d) {
                visit_order[k++] = w;
            }
        }
    }
    for (uint8_t i = 0; i < WINDOW_N; i++) {
        slot[i]->cx = CHUNK_NONE;
    }
    win_cx = CHUNK_NONE;
}

// v mod WINDOW in 0 .. WINDOW - 1 (a division: once per axis and window update)
static uint8_t modWindow(int24_t v) {
    const int24_t m = v % WINDOW;
    return (uint8_t)(m < 0 ? m + WINDOW : m);
}

static void updateWindow(int24_t cx, int24_t cz) {
    PROF(const uint24_t t = CYCLES();)
    uint8_t w = 0;
    const uint8_t mx0 = modWindow(cx - WINDOW_HALF);
    uint8_t mz = modWindow(cz - WINDOW_HALF);
    for (int24_t dz = -WINDOW_HALF; dz <= WINDOW_HALF; dz++) {
        uint8_t mx = mx0;
        for (int24_t dx = -WINDOW_HALF; dx <= WINDOW_HALF; dx++, w++) {
            chunk_t *ch = slot[(mx << 3) - mx + mz];  // mx * WINDOW
            if (++mx == WINDOW) {
                mx = 0;
            }
            win[w] = ch;
            if (ch->cx != cx + dx || ch->cz != cz + dz) {
                loadChunk(ch, cx + dx, cz + dz);
            }
        }
        if (++mz == WINDOW) {
            mz = 0;
        }
    }
    win_cx = cx;
    win_cz = cz;
    collideInvalidate();
    PROF(prof_load_cycles += CYCLES() - t; prof_loads++;)
}

void windowFollow(void) {
    _Static_assert(CHUNK_SIZE == 8, "chunk of a cell");
    const int24_t cx = cam_cell_x >> 3, cz = cam_cell_z >> 3;
    if (cx != win_cx || cz != win_cz) {
        updateWindow(cx, cz);
    }
}

chunk_t *windowChunk(int24_t cx, int24_t cz) {
    const int24_t dx = cx - win_cx + WINDOW_HALF, dz = cz - win_cz + WINDOW_HALF;
    if (dx < 0 || dx >= WINDOW) {
        return NULL;
    }
    if (dz < 0 || dz >= WINDOW) {
        return NULL;
    }
    _Static_assert(WINDOW == 7, "dz * WINDOW below");
    return win[(dz << 3) - dz + dx];       // dz * WINDOW without a library multiply
}
