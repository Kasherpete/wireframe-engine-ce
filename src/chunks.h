// The chunk window: the 7 x 7 chunks around the camera, in RAM.
//
// The world file (world.h) is split into chunk columns of CHUNK_SIZE x CHUNK_SIZE cells, of any
// height. The chunks around the camera's chunk are kept as records {x, Y, z, material, exposed
// FACE_*} that projectRecords (project.S) reads directly: x and z are local to the chunk (0 .. 7),
// Y is the world y cell + 1 (the voxel spans [Y-1, Y], y down). When the camera moves into another
// chunk, windowFollow() loads the chunks that came into the window over the ones that left it.
// With the far plane at GRID_RANGE = 24 cells and the camera anywhere in the centre chunk, every
// cell within the far plane is in the window.
//
// Memory: 49 chunks of 1305 bytes do not fit in bss. CHUNKS_BSS of them are in bss, CHUNKS_VRAM in
// the spare VRAM frame DBL_BUF2 (never shown). With FILL_REFERENCE (the bench) fewer fit there, and
// the rest are in a scratch AppVar (CHUNKS_APPVAR) that windowAlloc() creates before the world is
// opened (creating a variable may move the world AppVar if it is in RAM) and windowFree() deletes
// before worldClose. computeExposure's occupancy grid is in the span list frame DBL_BUF1: it is
// free between frames (drawSpans empties the list).
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
// Within a chunk any order works that puts A before B whenever A hides part of B. The sort key of
// a record is a sum of one table entry per axis (sortChunk): a_x(|dx|) + a_y(|dy|) + a_z(|dz|), each
// entry |d| minus its least value in the chunk, so each is strictly increasing in |d| and the
// argument above holds for any such sum.
//
// The order of a chunk only depends on which cell the camera is in. On an axis where the camera is
// outside the chunk's range of cells, moving the camera changes every |d| of the chunk by the same
// amount, which leaves the keys as they are. So a chunk is only re-sorted (sortChunk) when the camera
// moved into another cell on an axis where it is inside the chunk's range, and only when the chunk
// is about to be drawn.
//
// ---- runs: merged faces ----
// Big flat objects are slow drawn as unit faces: fill.S pays per face and per row of each face, so
// a wall of 7 faces side by side costs 7 times the rows of one. The world file therefore merges, per
// chunk, the coplanar exposed side faces of one orientation (x faces, or z faces: make_world.py picks
// the one that saves more) into runs: a record that stands for up to RUN_MAX faces of one material in
// a horizontal row, one cell tall. Its voxels have FACE_RUN, and leave those faces to it.
//
// A run's faces lie at many Manhattan distances, so no place in the plain order is right for it (a
// pillar in front of the far end of a long wall is farther than its near end). The chunk is then
// sorted along its run axis first: for x runs the key is S |dx| + |dy| + |dz| (each relative to its
// least in the chunk, and S one more than the greatest |dy| + |dz|, so this is exactly that order;
// S must stay at most 32 for byte keys, a chunk too tall for that is sorted plainly and its runs are
// off), and a run of x faces in the slab |dx| = k gets the key S k, the start of its slab. That is exact: whatever hides part of the run lies between the camera and the slab, at a
// smaller |dx|, and whatever the run hides lies beyond the slab, at a larger |dx|, or in the slab with
// a larger |dy| + |dz| than the face it lies behind. Boxes on the other side of the camera with the
// same |dx| cannot overlap it. z runs likewise with |dz|. Between chunks nothing changes: a run stays
// inside its chunk.
//
// A chunk without runs drawn (farther than the level of detail, or too tall) keeps its runs after its
// voxels (they come so in the file, and a plain sort moves them there with key 255), and sorts and
// projects only its voxels: then its runs cost nothing.
//
// A run is drawn as one polygon whose top and bottom edges pass through every grid point between its
// faces (fill.S), so its edges are the very edges the faces next to it use, and it meets them
// without cracks. Runs stop at the level of detail distance (project.S): beyond it a voxel is one
// rectangle with all its faces, cheaper than a run there. projectRecords keeps only the cells of a run that would pass the culling of a box,
// so the projection tables cover it as they cover boxes (gen_tables.py).
#ifndef CHUNKS_H
#define CHUNKS_H

#include <stdint.h>
#include "engine.h"
#include "world.h"

#define WINDOW       7               // chunks per side of the window
#define WINDOW_HALF  3
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
    uint8_t nruns;                   // runs among the n records
    uint8_t sorted;                  // the order is the one for sort_rx/ry/rz (sortChunk)
    int24_t sort_rx, sort_ry, sort_rz;
    uint8_t runs;                    // WORLD_RUNS_*: the orientation of its runs (world.h)
    uint8_t want;                    // runs when it was sorted (within the level of detail distance)
    uint8_t mixed;                   // the runs are not all after the voxels (sorted with runs)
    uint8_t mode;                    // the order it is sorted in: runs, or WORLD_RUNS_NONE (plain,
                                     // runs off: farther, too tall, or runs_enabled is 0)
    int8_t  rec[CHUNK_MAX][5];       // {x, Y, z, material, exposed FACE_*}, or runs (above)
} chunk_t;

#define CHUNKS_BSS     20                       // chunk slots in bss
#ifdef FILL_REFERENCE
#define CHUNKS_VRAM    19                       // the bench's fill_ref.c keeps its lists after them
#else
#define CHUNKS_VRAM    29                       // chunk slots in DBL_BUF2: all of the window's others
#endif
#define CHUNKS_APPVAR  "VXSCRTCH"               // the scratch AppVar for the rest, if any (deleted on exit)

extern chunk_t *win[WINDOW_N];         // the chunk at window position (dz + 3) * 7 + (dx + 3)
extern int24_t  win_cx, win_cz;        // the chunk the window is centred on
extern uint8_t  visit_order[WINDOW_N]; // window positions by distance from the centre, nearest first
// 0 draws every chunk without its runs (sorted plainly, voxels drawing all their faces). Clear every
// chunk_t.sorted after changing it
extern uint8_t  runs_enabled;
// chunks fewer than this many cells from the camera's draw their runs (RUN_DIST). Clear every
// chunk_t.sorted after changing it
extern uint8_t  run_dist;

// Sets the chunk slot pointers, and creates the scratch AppVar for the slots that are in neither bss
// nor VRAM (only with FILL_REFERENCE). Before worldStart (so the world does not move after it is
// opened) and before graphx starts: on failure (not enough free RAM) it says so on the home screen,
// waits for a key and returns 0
uint8_t windowAlloc(void);

// Deletes the scratch AppVar (if any). After gfx_End, before worldClose (which may need the RAM)
void windowFree(void);

// the sort tables, the occupancy grid and an empty window (nothing loaded). After displayInit and
// windowAlloc
void windowInit(void);

// Centres the window on the camera's chunk (setupCamera has run), loading the chunks that are new
// in it: all 49 at startup or after a jump, 7 when crossing a chunk border
void windowFollow(void);

// The chunk (cx, cz) if it is in the window, else NULL
chunk_t *windowChunk(int24_t cx, int24_t cz);

// Sorts the records of ch near to far from the camera cell, unless their order is already right
// for it. ch->n must be at least 1
void sortChunk(chunk_t *ch);

// Recomputes the exposed faces of the chunk at window position w, and turns its runs off (they would
// not match a changed chunk; its voxels draw all their faces again). For runtime world changes: after
// changing its records (clear chunk_t.sorted, update ymin/ymax), call it for w and its 4
// neighbours, then collideInvalidate(). Never while spans are listed (between projectScene and
// drawSpans): its occupancy grid is in the span list's frame, DBL_BUF1
void computeExposure(uint8_t w);

#endif
