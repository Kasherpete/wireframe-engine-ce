// Collision with the world (collide.h).
//
// collideBody (collide.S) computes the body's cells and looks them up in the grid here.
//
// The chunk records are in painter's order, not by position, so finding a cell in them is a scan.
// Instead, a query that leaves the occupancy grid rebuilds it around itself: one pass over the
// records of the (at most 4) chunks the grid overlaps. The grid is 8 x 8 columns, so walking
// rebuilds it about every 3 cells, and 16 cells high, so only climbing or falling far does.
#include <string.h>

#include "collide.h"
#include "chunks.h"

#define GRID_W   8                    // columns per side
#define GRID_H   16                   // cells per column: the bits of a uint16_t
#define GRID_NONE -0x4000             // grid_x0, grid_z0 of no grid: far from any cell (16 bits)

_Static_assert(GRID_W == CHUNK_SIZE, "the grid overlaps at most 2 x 2 chunks");
_Static_assert(GRID_W == 8 && GRID_H == 16 && CAM_Y_MIN == -32, "collide.S hard-codes these");

// bit dy of grid[dz * GRID_W + dx]: cell (grid_x0 + dx, grid_y0 + dy, grid_z0 + dz) is a voxel.
// Read and written by collideBody and collideMark (collide.S)
uint16_t grid[GRID_W * GRID_W];
int24_t  grid_x0 = GRID_NONE, grid_y0, grid_z0 = GRID_NONE;
uint8_t  collide_changed = 1;

// collide.S: the last block of cells found free. Its yb (int16 [3]) = 0 means none
extern int16_t collide_free[6];

void collideInvalidate(void) {
    grid_x0 = grid_z0 = GRID_NONE;
    collide_free[3] = 0;
    collide_changed = 1;
}


// collide.S: marks the n records of a chunk that lie in the grid, record {x, Y, z} at
// (x - lx, Y - ly, z - lz). Byte arithmetic: a record's x, z (0 .. 7) and Y (-95 .. 96) minus the
// grid's start wraps to 0 .. GRID_W - 1 (or GRID_H - 1) exactly when the cell is inside, since
// every true difference lies within -255 .. 255
void collideMark(const int8_t (*rec)[5], uint8_t n, uint8_t lx, uint8_t ly, uint8_t lz);

// collide.S: the grid around the block of cells from (xa, ya, za), as much room as possible on
// every side of a block of up to 2 x 3 x 2 cells. The cells come as int16 in the low bytes
void collideBuild(int24_t xa, int24_t ya, int24_t za) {
    const int24_t x0 = (int16_t)xa - (GRID_W - 2) / 2;
    const int24_t y0 = (int16_t)ya - (GRID_H - 3) / 2;
    const int24_t z0 = (int16_t)za - (GRID_W - 2) / 2;
    grid_x0 = x0;
    grid_y0 = y0;
    grid_z0 = z0;
    memset(grid, 0, sizeof grid);
    const int24_t cx0 = x0 >> 3, cz0 = z0 >> 3;
    _Static_assert(CHUNK_SIZE == 8, "chunk of a cell");
    for (int24_t cz = cz0; cz <= cz0 + 1; cz++) {
        for (int24_t cx = cx0; cx <= cx0 + 1; cx++) {
            const chunk_t *ch = windowChunk(cx, cz);
            if (ch == NULL || ch->n == 0) {
                continue;               // outside the window: nothing to collide with
            }
            if (ch->ymax <= y0 || ch->ymin > y0 + GRID_H) {
                continue;               // no record Y in y0 + 1 .. y0 + GRID_H
            }
            collideMark(ch->rec, ch->n, (uint8_t)(x0 - cx * CHUNK_SIZE), (uint8_t)(y0 + 1),
                        (uint8_t)(z0 - cz * CHUNK_SIZE));    // record Y = y cell + 1
        }
    }
}
