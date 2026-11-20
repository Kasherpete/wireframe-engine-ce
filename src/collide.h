// Collision with the world: the voxels of the chunk window and the ground. Included by collide.S:
// defines only outside the __ASSEMBLER__ guard.
//
// Solid are every voxel, every cell at y >= 0 (the ground plane, or the floor of a world without
// one: the player never falls below y = 0) and every cell above CAM_Y_MIN (chunks.h: keeps the
// camera's y cell in the range the sort keys allow).
//
// Queries look up a small occupancy grid around the player (8 x 8 columns, 16 cells high, one bit
// per cell), rebuilt from the chunk records whenever a query leaves it or the window changes.
#ifndef COLLIDE_H
#define COLLIDE_H

#define BODY_RADIUS  19661       // the body reaches 0.3 units (in 1/65536) to each side in x and z

#ifndef __ASSEMBLER__
#include <stdint.h>

// collide.S: whether the box [x - BODY_RADIUS, x + BODY_RADIUS) x [feet - height, feet) x
// [z - BODY_RADIUS, z + BODY_RADIUS) overlaps anything solid. Positions in 16.16 fixed point,
// height below 2 units. Near the camera: inside the chunks around the window's centre chunk.
// About 0.4k cycles while the body stays in the same cells as the last free answer, 1.2k
// otherwise, more when the grid has to be rebuilt
uint8_t collideBody(const int32_t *x, const int32_t *z, const int32_t *feet, int24_t height);

// the world changed (records, or the chunks in the window): rebuild the grid on the next query
void collideInvalidate(void);

// set by collideInvalidate: the player checks whether the world now overlaps it, and clears it
extern uint8_t collide_changed;
#endif

#endif
