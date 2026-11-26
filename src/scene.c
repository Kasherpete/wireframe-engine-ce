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

projected_box_t projected[PROJECT_BATCH];
uint24_t num_projected;
#ifdef FILL_REFERENCE
projected_box_t projected_log[PROJECTED_LOG];
uint24_t num_logged;
#endif
uint8_t chunk_visible[WINDOW_N];
uint8_t chunks_done;
uint8_t occlusion_enabled = 1;

// For chunkSeen (occlude.S), per frame: the least and greatest offsets of a chunk's 4 corners from
// its first (cullLoop leaves the first corners)
int24_t occ_dzmin, occ_dzmax, occ_dxmin, occ_dxmax;
uint8_t chunkSeen(uint8_t w, int24_t yt, int24_t yb);

// occlude.S: the loop of cullChunks over the window, from the first chunk's corner (row_x, row_z),
// in steps u = (cull_ux, cull_uz) along world x and v = (-cull_uz, cull_ux) along world z: writes
// chunk_visible[] (and the corners for chunkSeen). cull_* are the corner extremes with CULL_SLACK
int24_t cull_xmin, cull_xmax, cull_zmin, cull_zmax, cull_ux, cull_uz;
void cullLoop(int24_t row_x, int24_t row_z);


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
_Static_assert(CULL_SLACK == 32 && WINDOW == 7, "OCC_SLACK and WINDOW in occlude.S");

// Per frame, so C, but without library calls in the loop: 5 z is (z << 2) + z (a multiply by 5
// would be a ~380 cycle call, 50 of them), and / SUB a shift (a divide is ~2k cycles)
static void cullChunks(void) {
    const int24_t s = cam_sin, c = cam_cos;
    // camera space of world point (8 (win_cx - 3), 8 (win_cz - 3)): rotate (point - player). The
    // position in 1/SUB units from bytes 1 .. 3 of the 16.16 value (>> 8, then >> 1: no 32-bit
    // shift), rounded down: off by under 1/SUB, which CULL_SLACK covers
    const int24_t px = ((int24_t)((win_cx - WINDOW_HALF) * CHUNK_SIZE) << 7)
                       - (*(const int24_t *)((const uint8_t *)&player_x + 1) >> 1);
    const int24_t pz = ((int24_t)((win_cz - WINDOW_HALF) * CHUNK_SIZE) << 7)
                       - (*(const int24_t *)((const uint8_t *)&player_z + 1) >> 1);
    const int24_t row_x = (px * c - pz * s) >> 7;   // / SUB, rounded down: CULL_SLACK covers it
    const int24_t row_z = (px * s + pz * c) >> 7;
    // a chunk step along world x moves (8c, 8s) in camera space, along world z (-8s, 8c)
    const int24_t ux = c << 3, uz = s << 3, vx = -uz, vz = ux;
    _Static_assert(CHUNK_SIZE == 8 && SUB == 128, "shifts");
    // the corners of a chunk relative to its first: min and max over 0, u, v, u + v (+ slack)
    const int24_t xmin = MIN(0, ux) + MIN(0, vx) - CULL_SLACK, xmax = MAX(0, ux) + MAX(0, vx) + CULL_SLACK;
    const int24_t zmin = MIN(0, uz) + MIN(0, vz) - CULL_SLACK, zmax = MAX(0, uz) + MAX(0, vz) + CULL_SLACK;
    // chunkSeen: the extremes of the corner offsets 0, u, v, u + v (v = (-uz, ux))
    occ_dzmin = MIN(0, uz) + MIN(0, vz);
    occ_dzmax = MAX(0, uz) + MAX(0, vz);
    occ_dxmin = MIN(0, ux) + MIN(0, vx);
    occ_dxmax = MAX(0, ux) + MAX(0, vx);
    cull_xmin = xmin;
    cull_xmax = xmax;
    cull_zmin = zmin;
    cull_zmax = zmax;
    cull_ux = ux;
    cull_uz = uz;
    cullLoop(row_x, row_z);
}


// the faces a chunk's runs draw, by chunk_t.mode (WORLD_RUNS_*)
static const uint8_t run_faces[3] = {0, FACE_XP | FACE_XN, FACE_ZP | FACE_ZN};

// Chunks nearest first (visit_order), the boxes of each near to far (sortChunk), so the boxes reach
// fillBoxes near to far (chunks.h, painter's order). A box with no face both towards the camera and
// not covered by a neighbour is skipped before projecting. The per-box loop is in project.S.
// A chunk is projected into projected[] and filled at once, a batch at a time: so the coverage of
// the nearer chunks is known before farther ones are projected (once every row is covered, nothing
// more is), and projected[] only holds one batch
void projectScene(uint24_t base) {
    PROF(const uint24_t tc = CYCLES();)
    cullChunks();
    PROF(prof_cull_cycles += CYCLES() - tc;)
    fillBegin(base);
    num_projected = 0;
    // boxes against MAX_PROJECTED: those kept, and the rectangles project.S skipped as hidden, so
    // the cap stops a frame where it did before they were skipped (else a dense view goes on to
    // farther chunks and costs more)
    uint24_t used = 0;
    // the eye height in 1/SUB units, rounded down (the 16.16 value >> 9 from its bytes)
    const int24_t eye = (int24_t)(*(const int24_t *)((const uint8_t *)&player_y + 1) >> 1);
#ifdef FILL_REFERENCE
    num_logged = 0;
#endif
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
        if (used >= MAX_PROJECTED) {
            k--;                        // the cap, reached by skipped rectangles: this chunk is
            break;                      // not done (chunks_done)
        }
        if (occlusion_enabled) {
            // its records' cells ymin - 1 .. ymax - 1 span y from ymin - 2 to ymax - 1 (y down),
            // widened by the rounding of eye
            const int24_t yt = ((int24_t)(ch->ymin - 2) << 7) - eye - 1;
            const int24_t yb = ((int24_t)(ch->ymax - 1) << 7) - eye + 1;
            PROF(const uint24_t to = CYCLES();)
            const uint8_t seen = chunkSeen(w, yt, yb);
            PROF(prof_occ_cycles += CYCLES() - to;)
            if (!seen) {
                PROF(prof_occluded++;)
                chunk_visible[w] = 0;
                continue;               // hidden behind what is filled so far
            }
        }
        sortChunk(ch);
        // without runs drawn only the voxels: the runs are after them (chunks.h)
        uint8_t n = ch->n;
        if (ch->mode == WORLD_RUNS_NONE) {
            n -= ch->nruns;
        }
        const int8_t (*rec)[5] = ch->rec;
        PROF(prof_records += n; prof_chunks++;)
        for (;;) {
            PROF(const uint24_t tp = CYCLES();)
            uint8_t room = PROJECT_BATCH;
            if (MAX_PROJECTED - used < PROJECT_BATCH) {
                room = (uint8_t)(MAX_PROJECTED - used);
            }
            project_full = 0;
            project_hidden = 0;
            const uint8_t kept = projectRecords(rec, n, projected, room,
                                                cam_bias_x + ch->ox, cam_bias_z + ch->oz,
                                                run_faces[ch->mode]);
            PROF(const uint24_t tf = CYCLES(); prof_proj_cycles += tf - tp;)
            num_projected += kept;
            used += kept + project_hidden;
#ifdef FILL_REFERENCE
            for (uint8_t i = 0; i < kept && num_logged < PROJECTED_LOG; i++) {
                projected_log[num_logged++] = projected[i];
            }
#endif
            const uint8_t open = fillBoxes(projected, kept);
            PROF(prof_fill_cycles += CYCLES() - tf;)
            if (!open) {
                goto done;              // every row is covered: the rest is hidden
            }
            if (!project_full) {
                break;
            }
            if (used >= MAX_PROJECTED || !kept) {
                goto done;              // the most boxes per frame (or a run would pass it)
            }
            // projected[] was full: the rest of the chunk
            rec += n - project_left;
            n = project_left;
            if (!n) {
                break;
            }
        }
    }
done:
    chunks_done = k;
}
