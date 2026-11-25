// The player: input, movement and collision. The camera sits at the player's eye.
//
// Position is 16.16 fixed point world units (y grows downward, the ground is y = 0). Yaw is in
// 1/65536 of an angle step, with 256 steps per turn, so 24 bits are exactly one turn and wrapping
// is just turning past 360 degrees: yaw 0 looks along +z, and increasing yaw turns right, towards
// +x.
//
// The body is a box BODY_RADIUS (collide.h, 0.3 units) to each side of the eye in x and z, from the
// feet up to HEAD_ROOM above the eye. It collides with the voxels and the ground (collide.h):
// walking slides along walls, and a jump (about 1.3 units high) gets onto a voxel.
#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>

#define STAND_HEIGHT   (3L << 15)     // eye above the feet: 1.5 units, in 1/65536 units
#define CROUCH_HEIGHT  (3L << 14)     // 0.75 units
#define HEAD_ROOM      (1L << 14)     // the body reaches 0.25 units above the eye

extern int32_t  player_x, player_y, player_z;   // the eye

// The cell and the fraction (1/65536 units) of a 16.16 fixed point coordinate held in memory (a
// variable, not an expression): 16-bit loads of its halves. v >> 16 would be a library shift.
// (volatile: otherwise clang turns the load back into the shift when it holds v in registers)
#define CELL_OF(v)  ((int24_t)((const volatile int16_t *)&(v))[1])
#define FRAC_OF(v)  ((uint24_t)((const volatile uint16_t *)&(v))[0])
extern uint24_t player_yaw;

// at the world's spawn, or where "Save & Quit" left the player (world_header_t.saved): the
// middle of that cell, on the ground (or on top of what stands there). At 0, 0 with no world open
void playerSpawn(void);
// "Save & Quit": hands the player's cell and yaw to worldSetPlayer, for worldClose(1)
void playerSave(void);

// Reads the keys and moves the player for dt clock ticks (CLOCKS_PER_SEC per second). Controls:
// arrows walk and turn, [mode]/[del] strafe, [2nd] jump, [alpha] crouch
void playerUpdate(uint24_t dt);

#endif
