// The scene (scene.h): chunk culling and projection.
#include "scene.h"
#include "camera.h"
#include "fill.h"
#include "hud.h"
#include "player.h"

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

// the layout fill.S reads. IntelliSense sizes int24_t differently from ez80-clang and would flag
// these; the real build checks them
#ifndef __INTELLISENSE__
_Static_assert(sizeof(projected_box_t) == PB_SIZE, "engine.h PB_SIZE out of sync");
_Static_assert(__builtin_offsetof(projected_box_t, kind) == PB_KIND, "engine.h PB_KIND out of sync");
_Static_assert(__builtin_offsetof(projected_box_t, faces) == PB_FACES, "engine.h PB_FACES out of sync");
_Static_assert(__builtin_offsetof(projected_box_t, mat) == PB_MAT, "engine.h PB_MAT out of sync");
_Static_assert(__builtin_offsetof(projected_box_t, v) == PB_V, "engine.h PB_V out of sync");
#endif

projected_box_t projected[MAX_PROJECTED];
uint24_t num_projected;
uint8_t chunk_visible[WINDOW_N];
uint8_t chunks_done;


// The colour of a BOX_RECT box by its FACE_* bits (fill.h, lod_slot): far away a side shows far
// more than a top (that shrinks with height / depth), so a side if visible, else the top or bottom.
// project.S has already kept only the side that points at the camera position most (.Llod_side),
// so the order here doesn't matter and the table is fixed
void sceneInit(void) {
    for (uint8_t m = 0; m < 64; m++) {
        uint8_t slot = (m & FACE_TOP) ? MAT_TOP : MAT_BOTTOM;
        if (m & (FACE_ZP | FACE_ZN)) {
            slot = MAT_ZSIDE;
        } else if (m & (FACE_XP | FACE_XN)) {
            slot = MAT_XSIDE;
        }
        lod_slot[m] = slot;
    }
}


// ---- chunk culling ----
// A chunk column is skipped when no voxel in it could pass projectRecords' culling: its corners'
// camera-space bounding box (x and z, columns have no vertical bound) all nearer than the near
// plane, all at or beyond the far plane, or all left or right of the view (the same 4 x vs 5 z
// tests as for a box, engine.h). A box's corners lie inside its chunk's, so its own bounds are
// inside those. CULL_SLACK covers the rounding of the per-frame tables. Per frame, in 1/SUB units:
// the window's first corner, then steps of a chunk along x and z, by addition
#define CULL_SLACK   (SUB / 4)

// Per frame, so C, but without library calls in the loop: 5 z is (z << 2) + z (a multiply by 5
// would be a ~380 cycle call, 50 of them), and / SUB a shift (a divide is ~2k cycles)
static void cullChunks(void) {
    const int24_t s = cam_sin, c = cam_cos;
    // camera space of world point (8 (win_cx - 2), 8 (win_cz - 2)): rotate (point - player)
    const int24_t px = (int24_t)((((int32_t)((win_cx - WINDOW_HALF) * CHUNK_SIZE) << 16) - player_x) >> 9);
    const int24_t pz = (int24_t)((((int32_t)((win_cz - WINDOW_HALF) * CHUNK_SIZE) << 16) - player_z) >> 9);
    int24_t row_x = (px * c - pz * s) >> 7;     // / SUB, rounded down: CULL_SLACK covers it
    int24_t row_z = (px * s + pz * c) >> 7;
    // a chunk step along world x moves (8c, 8s) in camera space, along world z (-8s, 8c)
    const int24_t ux = c << 3, uz = s << 3, vx = -uz, vz = ux;
    _Static_assert(CHUNK_SIZE == 8 && SUB == 128, "shifts");
    // the corners of a chunk relative to its first: min and max over 0, u, v, u + v (+ slack)
    const int24_t xmin = MIN(0, ux) + MIN(0, vx) - CULL_SLACK, xmax = MAX(0, ux) + MAX(0, vx) + CULL_SLACK;
    const int24_t zmin = MIN(0, uz) + MIN(0, vz) - CULL_SLACK, zmax = MAX(0, uz) + MAX(0, vz) + CULL_SLACK;
    uint8_t *vis = chunk_visible;
    for (uint8_t j = 0; j < WINDOW; j++) {
        int24_t x = row_x, z = row_z;
        for (uint8_t i = 0; i < WINDOW; i++) {
            const int24_t x0 = x + xmin, x1 = x + xmax, z0 = z + zmin, z1 = z + zmax;
            const int24_t z5 = (z1 << 2) + z1;
            uint8_t v = 1;
            if (z1 < NEAR_PLANE) {
                v = 0;                      // behind the camera
            } else if (z0 >= FAR_PLANE) {
                v = 0;
            } else if ((x1 << 2) + z5 < 0) {
                v = 0;                      // left of the view: 4 x1 < -5 z1
            } else if ((x0 << 2) - z5 > 0) {
                v = 0;                      // right of the view: 4 x0 > 5 z1
            }
            *vis++ = v;
            x += ux;
            z += uz;
        }
        row_x += vx;
        row_z += vz;
    }
}


// Chunks nearest first (visit_order), the boxes of each near to far (sortChunk), so projected[] is
// near to far (chunks.h, painter's order). A box with no face both towards the camera and not
// covered by a neighbour is skipped before projecting. The per-box loop is in project.S
void projectScene(void) {
    PROF(const uint24_t tc = CYCLES();)
    cullChunks();
    PROF(prof_cull_cycles += CYCLES() - tc;)
    projected_box_t *out = projected;
    uint24_t room = MAX_PROJECTED;
    uint8_t k = 0;
    while (k < WINDOW_N) {
        const uint8_t w = visit_order[k++];
        chunk_t *ch = win[w];
        if (ch->n == 0) {
            continue;
        }
        if (!chunk_visible[w]) {
            continue;
        }
        sortChunk(ch);
        PROF(const uint24_t tp = CYCLES();)
        const uint8_t kept = projectRecords(ch->rec, ch->n, out, room > 255 ? 255 : (uint8_t)room,
                                            cam_bias_x + ch->ox, cam_bias_z + ch->oz);
        PROF(prof_proj_cycles += CYCLES() - tp; prof_records += ch->n; prof_chunks++;)
        out += kept;
        room -= kept;
        if (room == 0) {
            break;
        }
    }
    chunks_done = k;
    num_projected = MAX_PROJECTED - room;
}
