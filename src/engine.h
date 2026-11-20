// Shared between main.c, project.S and fill.S. Defines only outside the __ASSEMBLER__ guard, and no
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

// projectRecords() results (projected_box_t kind). Its v[BOX_OUT_LEN] holds the screen coordinates:
//   AXIS: nl nr nt nb fl fr ft fb  (near and far face rectangles: left right top bottom)
//   GEN:  sx[4] st[4] sb[4]        (the 4 vertical edges: x, top y, bottom y). corner k is
//                                  0 = (X-1, Z-1), 1 = (X, Z-1), 2 = (X-1, Z), 3 = (X, Z)
//   RECT: l r t b                  (columns [l, r) and rows [t, b), at least 1 each way)
// None is classified as inside or partly inside the viewport: fill.S clips by construction, and
// checks the 4 x values itself to decide whether edges need clamping (cheaper than testing all
// 12 coordinates here). A box may be partly or (rarely) not at all inside.
#define BOX_SKIP          0   // nothing on screen
#define BOX_AXIS          1   // facing N/E/S/W: the near and far faces are rectangles
#define BOX_GEN           2   // any other yaw
#define BOX_RECT          3   // level of detail: far enough to be one rectangle, out = l r t b
#define BOX_OUT_LEN       12

// ---- filled rendering (fill.S) ----
// Faces of a box (corner k as in the GEN layout above). projectScene keeps those that face the
// camera, from the sign of (box cell - camera cell) on each axis, and that no neighbouring voxel
// covers (computeExposure in main.c)
#define FACE_XP      0x01  // +x face, corners 1 and 3: camera cell x > box cell x
#define FACE_XN      0x02  // -x face, corners 0 and 2: camera cell x < box cell x
#define FACE_ZP      0x04  // +z face, corners 2 and 3
#define FACE_ZN      0x08  // -z face, corners 0 and 1
#define FACE_TOP     0x10  // the box is below the camera (y grows downward)
#define FACE_BOT     0x20  // the box is above the camera

// A projected box, as projectScene() stores it for the fill rasterizer: kind (BOX_*, never
// BOX_SKIP), faces to draw (FACE_*), material (index into materials[]), then out[BOX_OUT_LEN]
#define PB_KIND      0
#define PB_FACES     1
#define PB_MAT       2
#define PB_V         3
#define PB_SIZE      39    // PB_V + 3 BOX_OUT_LEN

// colour bytes of a material, 4 per entry in materials[]
#define MAT_TOP      0
#define MAT_XSIDE    1
#define MAT_ZSIDE    2
#define MAT_BOTTOM   3
#define MAT_COUNT    16

// the background behind every box: sky above the horizon (row CENTRE_Y: there is no pitch), ground below
#define SKY_COLOUR     0xcc
#define GROUND_COLOUR  0x55

// ---- level of detail (the defaults of lod_rect_dist and lod_pixel_dist in main.c) ----
// A box whose nearest corner is at least LOD_RECT_DIST world units deep is drawn as one rectangle
// (its projected extent, in the colour of the side it shows most), and from LOD_PIXEL_DIST on as a
// single pixel: 2 projections and one 1-pixel span instead of 12 projections and up to 3 faces.
// A box spans about 64 / depth pixels, so at 10 units it is a 6 pixel rectangle. Lower values are
// faster and blockier; values at or beyond the far plane (TABLE_RENDER_DIST) turn a level off.
#define LOD_RECT_DIST   12
#define LOD_PIXEL_DIST  36

#ifndef __ASSEMBLER__
#include <stdint.h>

#define GRID_LEN     (2 * GRID_RANGE + 1)

// ---- per-frame camera state: written by setupCamera() in main.c, read by project.S ----
// The tables and biases are defined in main.c. The scalars from cam_cos3 on are defined in
// project.S, next to its locals, so it can address them all through IX.
// n = X + cam_bias_x is the table index of grid line X-1 (likewise y and z)
extern int24_t cam_bias_x, cam_bias_y, cam_bias_z;
// rotX_[2n] = XC, rotX_[2n+1] = XS and rotZ_[2n] = ZS, rotZ_[2n+1] = ZC, where XC/XS is
// (x - player x) * cos/sin for grid line x (and the same for z). camY_[n] = y - player y.
// Camera space of grid point (x, z): x' = XC - ZS, z' = XS + ZC. All x3.
extern int24_t rotX_[2 * GRID_LEN], rotZ_[2 * GRID_LEN], camY_[GRID_LEN];
// x3 yaw, and a box's corner offsets from its (X-1, Z-1) corner: (+c, +s), (-s, +c), (c-s, s+c)
extern int24_t cam_cos3, cam_sin3;
extern int24_t cam_kxmin3, cam_kxmax3, cam_kzmin3, cam_kzmax3;
// the middle of those offsets (x3): a box centre is (x00 + kxmid3, z00 + kzmid3)
extern int24_t cam_kxmid3, cam_kzmid3;
// level of detail (x3): BOX_RECT when z00 >= cam_lod_rect, one pixel when z00 >= cam_lod_pix
extern int24_t cam_lod_rect, cam_lod_pix;
// culling, all x3. With x00/z00 the camera space of corner (X-1, Z-1), a box is kept when
//   (uint)(z00 - zlo) < zspan    every corner in [NEAR_PLANE, FAR_PLANE)
//   4 x00 + 5 z00 >= cxl         not entirely left of the view  (xmax >= -1.25 zmax)
//   4 x00 - 5 z00 <= cxr         not entirely right of the view (xmin <= 1.25 zmax)
// (the view is |x| <= 1.25 z: 80 px half width at TABLE_FOCAL_LENGTH 64)
//   yt - z00 <= kzmax3           not entirely below the view    (yt <= zmax)
//   yt + z00 >= cyl              not entirely above the view    (yb >= -zmax)
extern int24_t cam_zlo, cam_zspan, cam_cxl, cam_cxr, cam_cyl;
extern uint8_t cam_cardinal;                          // exactly facing N/E/S/W: boxes are axis aligned on screen

// ---- filled rendering ----
typedef struct {
    uint8_t kind;               // BOX_*, never BOX_SKIP
    uint8_t faces;              // FACE_* bits: facing the camera and not covered by a neighbour
    uint8_t mat;                // index into materials[]
    int24_t v[BOX_OUT_LEN];     // screen coordinates, layout above
} projected_box_t;

// project.S: for each of the n scene records {X, Y, Z, material, exposed FACE_*}: the faces
// facing the camera and exposed; if any, culls the world voxel [X-1, X] x [Y-1, Y] x [Z-1, Z] and
// projects it into the next slot of out[]. Returns how many boxes it kept, in record order
extern uint8_t face_x[256], face_y[256], face_z[256];  // FACE_* towards the camera, by grid index
uint8_t projectRecords(const int8_t (*rec)[5], uint8_t n, projected_box_t *out);

// {top, x side, z side, bottom} colour bytes per material (main.c)
extern const uint8_t materials[MAT_COUNT][4];
// AXIS layout -> GEN corners: the world corner k (0..3) of the near-left, near-right, far-left and
// far-right vertical edge while facing N/E/S/W. Written by setupCamera()
extern uint8_t cam_axis_corner[4];
#endif

#endif
