// The camera (camera.h): the per-frame state project.S reads.
#include <string.h>

#include "camera.h"
#include "player.h"

_Static_assert(SIN_TABLE_LEN == 256, "yaw is looked up as a byte");

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

int24_t cam_cell_x, cam_cell_y, cam_cell_z;
int24_t cam_sin, cam_cos;

uint8_t lod_rect_dist = LOD_RECT_DIST;
uint8_t lod_pixel_dist = LOD_PIXEL_DIST;

// shared with project.S, see engine.h (the cam_ scalars are defined in project.S)
int24_t cam_bias_x, cam_bias_y, cam_bias_z;
int24_t rotX_[2 * GRID_LEN], rotZ_[2 * GRID_LEN], camY_[GRID_LEN];
uint8_t cam_axis_corner[4];

// cam_axis_corner per cardinal direction (yaw >> 22): the world corner (engine.h) of the near-left,
// near-right, far-left and far-right edge. E.g. facing +x (yaw 64 steps) the near face is x = X - 1
// (corners 0 and 2), and +z is to the left
static const uint8_t axis_corners[4][4] = {
    {0, 1, 2, 3},   // facing +z
    {2, 0, 3, 1},   // +x
    {3, 2, 1, 0},   // -z
    {1, 3, 0, 2},   // -x
};


// The faces of a box that face the camera (FACE_*), from d = box cell - camera cell on each axis:
// the +x face (at X) faces the camera exactly when the camera is in a cell right of the box's, and
// so on; d = 0 means the camera is level with the box on that axis, so neither face is visible.
// With n the grid index projectRecords uses, d is n - GRID_RANGE, so the bits are tables indexed
// by the low byte of n: three 8-bit adds and lookups per box. n wraps for boxes beyond GRID_RANGE,
// which projectRecords culls before it looks. face_x, face_y and face_z are in project.S, on
// consecutive pages. n >= 2 GRID_RANGE: a box projectRecords culls, so no faces (ifs: see the
// compiler bug in CLAUDE.md)
void cameraInit(void) {
    uint8_t n = 0;
    do {
        if (n < GRID_RANGE) {               // box cell < camera cell
            face_x[n] = FACE_XP;
            face_z[n] = FACE_ZP;
            face_y[n] = FACE_BOT;           // y grows downward: the box is above the camera
        } else if (n > GRID_RANGE && n < 2 * GRID_RANGE) {
            face_x[n] = FACE_XN;
            face_z[n] = FACE_ZN;
            face_y[n] = FACE_TOP;
        } else {
            face_x[n] = face_y[n] = face_z[n] = 0;
        }
    } while (++n != 0);
}


// camera.S: t[n * stride] = 3 * (n - GRID_RANGE - frac/256) * k for n = 0 .. GRID_LEN - 1, by
// repeated addition: every entry an exact multiple of 3 (project.S uses them as byte offsets).
// The C version, which the assembly matches exactly:
//     const int24_t v = -GRID_RANGE * k - ((frac * k) >> 8);
//     for (uint8_t n = 0; n < GRID_LEN; n++) t[n * stride] = 3 * v + 3 * k * n;
void fillRotTable(int24_t *t, uint8_t stride, int24_t k, int24_t frac);

// v / 2 rounded toward zero, as C's division does, without the ~2k cycle library divide
static int24_t halfTowardZero(int24_t v) {
    if (v < 0) {
        return -((-v) >> 1);
    }
    return v >> 1;
}


// Per frame, in C, but with no library calls but fillRotTable's: multiplies by constants are adds
// and shifts, the cells and fractions of the position are bytes of it (CELL_OF, player.h), and the
// box centre offset halves instead of dividing by 6
void setupCamera(void) {
    const uint8_t *yaw = (const uint8_t *)&player_yaw;
    const uint8_t angle = yaw[2];
    const int24_t s = sinTable[angle];
    const int24_t c = sinTable[(uint8_t)(angle + 64)];
    // a multiple of 64 steps, fraction included: (player_yaw & 0x3fffff) == 0. Ifs, not a
    // combined test: see the compiler bug in CLAUDE.md
    uint8_t cardinal = 0;
    if ((yaw[2] & 0x3f) == 0) {
        if (yaw[1] == 0) {
            if (yaw[0] == 0) {
                cardinal = 1;
            }
        }
    }
    cam_cardinal = cardinal;
    memcpy(cam_axis_corner, axis_corners[angle >> 6], 4);

    // grid cell, and the position within it in 1/256 units
    const int24_t cell_x = CELL_OF(player_x);
    const int24_t cell_y = CELL_OF(player_y);
    const int24_t cell_z = CELL_OF(player_z);
    cam_bias_x = GRID_RANGE - 1 - cell_x;
    cam_bias_y = GRID_RANGE - 1 - cell_y;
    cam_bias_z = GRID_RANGE - 1 - cell_z;
    cam_cell_x = cell_x;
    cam_cell_y = cell_y;
    cam_cell_z = cell_z;
    cam_sin = s;
    cam_cos = c;
    const int24_t fx = ((const uint8_t *)&player_x)[1];     // (player_x >> 8) & 0xff
    const int24_t fy = ((const uint8_t *)&player_y)[1];
    const int24_t fz = ((const uint8_t *)&player_z)[1];

    // x' = dx cos - dz sin, z' = dx sin + dz cos
    fillRotTable(rotX_,     2, c, fx);    // XC
    fillRotTable(rotX_ + 1, 2, s, fx);    // XS
    fillRotTable(rotZ_,     2, s, fz);    // ZS
    fillRotTable(rotZ_ + 1, 2, c, fz);    // ZC
    fillRotTable(camY_,     1, SUB, fy);  // no rotation in y, just the offset

    // a box's corner offsets from its (X-1, Z-1) corner: (c, s), (-s, c), (c - s, s + c). Their
    // least and greatest, unscaled, then everything x3
    const int24_t kxmin = MIN(0, c) + MIN(0, -s), kxmax = MAX(0, c) + MAX(0, -s);
    const int24_t kzmin = MIN(0, s) + MIN(0, c), kzmax = MAX(0, s) + MAX(0, c);
    const int24_t kxmin3 = kxmin + kxmin + kxmin, kxmax3 = kxmax + kxmax + kxmax;
    const int24_t kzmin3 = kzmin + kzmin + kzmin, kzmax3 = kzmax + kzmax + kzmax;
    cam_cos3 = c + c + c;
    cam_sin3 = s + s + s;
    cam_kxmin3 = kxmin3;
    cam_kxmax3 = kxmax3;
    cam_kzmin3 = kzmin3;
    cam_kzmax3 = kzmax3;

    // box centre offsets: 3 trunc((min + max) / 2), multiples of 3 between the min and max offsets
    // (the same as 3 ((min3 + max3) / 6) with C's division)
    const int24_t mx = halfTowardZero(kxmin + kxmax), mz = halfTowardZero(kzmin + kzmax);
    cam_kxmid3 = mx + mx + mx;
    cam_kzmid3 = mz + mz + mz;

    // level of detail: by the depth of the nearest corner, z00 + kzmin3. 3 SUB = 384 = 256 + 128
    _Static_assert(3 * SUB == 384, "3 SUB d = (d << 8) + (d << 7)");
    const int24_t lr = lod_rect_dist, lp = lod_pixel_dist;
    cam_lod_rect = (lr << 8) + (lr << 7) - kzmin3;
    cam_lod_pix  = (lp << 8) + (lp << 7) - kzmin3;

    // culling constants, see engine.h. The horizontal view is |x| <= 1.25 z: 4 x against 5 z
    const int24_t kzmax15 = (kzmax3 << 2) + kzmax3;
    cam_zlo   = 3 * NEAR_PLANE - kzmin3;
    cam_zspan = (3 * FAR_PLANE - kzmax3) - cam_zlo;
    cam_cxl   = -((kxmax3 << 2) + kzmax15);
    cam_cxr   = kzmax15 - (kxmin3 << 2);
    cam_cyl   = -(3 * SUB + kzmax3);
}
