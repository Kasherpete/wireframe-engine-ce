// Shared between main.c and project.S. Defines only outside the __ASSEMBLER__ guard, and no
// value may start with "(": the assembler would read that as a memory operand.
#ifndef ENGINE_H
#define ENGINE_H

#include "projection_tables.h"

// logical screen (see CLAUDE.md: 160x120 logical pixels, the 3D viewport is all of it)
#define VIEW_X0      0     // first viewport column (SCREEN_OFFSET)
#define VIEW_X1      160   // one past the last viewport column
#define VIEW_H       120   // viewport height
#define CENTRE_X     80
#define CENTRE_Y     60

// Camera space is x right, y down, z forward, in 1/SUB world units. Everything shared with
// project.S is stored x3: an int24 table entry is 3 bytes, so a x3 value is directly a byte
// offset into logTable (whose entries are x3 too, so log differences index expTable).
#define SUB          TABLE_SUBUNITS
#define NEAR_PLANE   TABLE_NEAR_PLANE    // boxes with a corner nearer than this are culled
#define FAR_PLANE    TABLE_FAR_PLANE     // ... or with a corner at or beyond this
#define GRID_RANGE   TABLE_RENDER_DIST   // per-frame tables cover grid lines +-GRID_RANGE from the camera cell

// projectBox() results. box_out holds the screen coordinates:
//   AXIS: nl nr nt nb fl fr ft fb  (near and far face rectangles: left right top bottom)
//   GEN:  sx[4] st[4] sb[4]        (the 4 vertical edges: x, top y, bottom y). corner k is
//                                  0 = (X-1, Z-1), 1 = (X, Z-1), 2 = (X-1, Z), 3 = (X, Z)
#define BOX_SKIP          0   // nothing on screen
#define BOX_AXIS_NOCLIP   1   // facing N/E/S/W, all 8 vertices inside the viewport
#define BOX_AXIS_CLIP     2   // facing N/E/S/W, partly inside
#define BOX_GEN_NOCLIP    3   // any other yaw, all 8 vertices inside
#define BOX_GEN_CLIP      4   // any other yaw, may be partly or (rarely) not at all inside

#ifndef __ASSEMBLER__
#include <stdint.h>

#define GRID_LEN     (2 * GRID_RANGE + 1)

// ---- per-frame camera state: written by setupCamera() in main.c, read by project.S ----
// n = X + cam_bias_x is the table index of grid line X-1 (likewise y and z)
extern int24_t cam_bias_x, cam_bias_y, cam_bias_z;
// rotX_[2n] = XC, rotX_[2n+1] = XS and rotZ_[2n] = ZS, rotZ_[2n+1] = ZC, where XC/XS is
// (x - player x) * cos/sin for grid line x (and the same for z). camY_[n] = y - player y.
// Camera space of grid point (x, z): x' = XC - ZS, z' = XS + ZC. All x3.
extern int24_t rotX_[2 * GRID_LEN], rotZ_[2 * GRID_LEN], camY_[GRID_LEN];
// x3 yaw, and a box's corner offsets from its (X-1, Z-1) corner: (+c, +s), (-s, +c), (c-s, s+c)
extern int24_t cam_cos3, cam_sin3;
extern int24_t cam_kxmin3, cam_kxmax3, cam_kzmin3, cam_kzmax3;
// culling, all x3. With x00/z00 the camera space of corner (X-1, Z-1), a box is kept when
//   (uint)(z00 - zlo) < zspan    every corner in [NEAR_PLANE, FAR_PLANE)
//   4 x00 + 5 z00 >= cxl         not entirely left of the view  (xmax >= -1.25 zmax)
//   4 x00 - 5 z00 <= cxr         not entirely right of the view (xmin <= 1.25 zmax)
// (the view is |x| <= 1.25 z: 80 px half width at TABLE_FOCAL_LENGTH 64)
//   yt - z00 <= kzmax3           not entirely below the view    (yt <= zmax)
//   yt + z00 >= cyl              not entirely above the view    (yb >= -zmax)
extern int24_t cam_zlo, cam_zspan, cam_cxl, cam_cxr, cam_cyl;
extern uint8_t cam_cardinal;                          // exactly facing N/E/S/W: boxes are axis aligned on screen

extern int24_t box_out[12];

// Culls and projects the world voxel [X-1, X] x [Y-1, Y] x [Z-1, Z] into box_out. Returns BOX_*.
uint8_t projectBox(int24_t X, int24_t Y, int24_t Z);
#endif

#endif
