// The chunk window: the 5 x 5 chunks around the camera, in RAM.
//
// The world file (world.h) is split into chunk columns of CHUNK_SIZE x CHUNK_SIZE cells, of any
// height. The chunks around the camera's chunk are kept as records {x, Y, z, material, exposed
// FACE_*} that projectRecords (project.S) reads directly: x and z are local to the chunk (0 .. 7),
// Y is the world y cell + 1 (the voxel spans [Y-1, Y], y down). When the camera moves into another
// chunk, windowFollow() loads the chunks that came into the window over the ones that left it.
// With the far plane at GRID_RANGE = 16 cells and the camera anywhere in the centre chunk, every
// cell within the far plane is in the window.
//
// ---- painter's order ----
// Boxes are drawn near to far by their Manhattan distance in grid cells from the camera's cell,
// |dx| + |dy| + |dz|. For unit voxels on a grid this is an exact painter's order: if box A hides
// part of box B, the sight line from the camera to B passes through A, and along a line every
// coordinate is monotonic, so on each axis A's cell lies between the camera's cell and B's. A's
// distance is then strictly smaller. Equal distances never occlude each other, so ties can go
// either way. Camera depth (z) is not a valid key: two boxes side by side at the same depth can
// overlap on screen.
//
// Across chunks the same argument works with chunk coordinates: visiting chunks by their Manhattan
// distance in chunks (visit_order), and the boxes of each chunk by their distance in cells, is
// also an exact order. If A hides B, A's chunk distance is at most B's, and if they are equal then
// on every axis A's chunk equals B's, so they are in the same chunk and the sort puts A first.
//
// The order of a chunk only depends on which cell the camera is in. On an axis where the camera is
// outside the chunk's range of cells, moving the camera changes every key of the chunk by the same
// amount, which leaves the order as it is. So a chunk is only re-sorted (sortChunk) when the camera
// moved into another cell on an axis where it is inside the chunk's range, and only when the chunk
// is about to be drawn.
#ifndef CHUNKS_H
#define CHUNKS_H

#include <stdint.h>
#include "engine.h"
#include "world.h"

#define WINDOW       5               // chunks per side of the window
#define WINDOW_HALF  2
#define WINDOW_N     (WINDOW * WINDOW)
#define CHUNK_NONE   0x7fffff        // chunk_t.cx of a slot nothing was loaded into

_Static_assert(WINDOW == 2 * WINDOW_HALF + 1, "the window is centred on the camera's chunk");
_Static_assert(WINDOW_HALF * CHUNK_SIZE >= GRID_RANGE, "the window must reach the far plane from anywhere in its centre chunk");

// The camera's y cell must stay in CAM_Y_MIN .. CAM_Y_MAX: the collision code (collide.c) keeps it
// there. The sort keys are then exact bytes (see initGridDist in chunks.c)
#define CAM_Y_MIN    -32
#define CAM_Y_MAX    0
#define MAX_DXZ      ((WINDOW_HALF + 1) * CHUNK_SIZE - 1)                     // |dx|, |dz| in the window
#define MAX_DY       (WORLD_Y_MAX - CAM_Y_MIN > CAM_Y_MAX - WORLD_Y_MIN ? WORLD_Y_MAX - CAM_Y_MIN : CAM_Y_MAX - WORLD_Y_MIN)
_Static_assert(MAX_DXZ <= 127 && MAX_DY <= 127, "gridDist is exact up to 127 cells");
_Static_assert(2 * MAX_DXZ + MAX_DY <= 255, "sort keys are bytes");

typedef struct {
    int24_t cx, cz;                  // world chunk coordinates (CHUNK_NONE: nothing loaded yet)
    int24_t ox, oz;                  // 8 cx + 1, 8 cz + 1: + cam_bias_x/z is the bias of its records
    uint8_t n;                       // voxels
    int8_t  ymin, ymax;              // range of the records' Y (n > 0). Adjacent: copyVoxels
    uint8_t sorted;                  // the order is the one for sort_rx/ry/rz (sortChunk)
    int24_t sort_rx, sort_ry, sort_rz;
    int8_t  rec[CHUNK_MAX][5];       // {x, Y, z, material, exposed FACE_*}
} chunk_t;

extern chunk_t *win[WINDOW_N];         // the chunk at window position (dz + 2) * 5 + (dx + 2)
extern int24_t  win_cx, win_cz;        // the chunk the window is centred on
extern uint8_t  visit_order[WINDOW_N]; // window positions by distance from the centre, nearest first

// the sort tables, the occupancy grid and an empty window (nothing loaded). After displayInit
void windowInit(void);

// Centres the window on the camera's chunk (setupCamera has run), loading the chunks that are new
// in it: all 25 at startup or after a jump, 5 when crossing a chunk border
void windowFollow(void);

// The chunk (cx, cz) if it is in the window, else NULL
chunk_t *windowChunk(int24_t cx, int24_t cz);

// Sorts the records of ch near to far from the camera cell, unless their order is already right
// for it. ch->n must be at least 1
void sortChunk(chunk_t *ch);

// Recomputes the exposed faces of the chunk at window position w. For runtime world changes: after
// changing its records (clear chunk_t.sorted, update ymin/ymax), call it for w and its 4
// neighbours, then collideInvalidate()
void computeExposure(uint8_t w);

#endif
