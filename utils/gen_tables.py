#!/usr/bin/env python3
"""Generates src/projection_tables.h and src/projection_tables.c, the projection tables.

Run from anywhere: python3 utils/gen_tables.py

The calculator has no FPU and clang never emits the eZ80's hardware multiply, so every
multiply, divide and right shift is a slow library call. Projection is therefore done with
lookups only:

    FOCAL_LENGTH * a / z  =  expTable[logTable[|a|] - logTable[z] - EXP_TABLE_MIN]

logTable values are stored x3, so the difference of two of them is already a byte offset into
expTable (int24 entries). project.S also keeps camera coordinates x3, so they are byte offsets
into logTable. That removes every index multiply from the per-box code.

The parameters below are the single source of truth. They are written into the header as
#defines, and main.c and project.S check their own assumptions against them.
"""

import math
import os

SUBUNITS = 128          # camera-space coordinates are in 1/SUBUNITS of a world unit
FOCAL_LENGTH = 64       # logical px per unit at depth 1
VIEW_HALF_WIDTH = 80    # logical px from the screen centre to the viewport edge (160 px viewport)
# Horizontal culling keeps |x| <= (CULL_X_NUM / CULL_X_DEN) z, which must be exactly the view:
# VIEW_HALF_WIDTH / FOCAL_LENGTH (80 / 64 = 1.25, a 102 deg FOV). project.S hard-codes 4 x vs 5 z.
CULL_X_NUM, CULL_X_DEN = 5, 4
RENDER_DIST = 24        # largest far plane, in world units (GRID_RANGE). The 7 x 7 chunk window (chunks.h) covers 24.
                        # render_dist (camera.c) can lower it at runtime: the proof below holds for any nearer far plane
# The near plane (1/SUBUNITS units): geometry nearer is clipped (project.S). The body keeps the eye
# BODY_RADIUS = 0.3 from any wall (player.h), and a wall at distance d is at least d / |(1.25, 1)|
# = d / 1.6008 deep where the view's side edge meets it, so 0.3 / 1.6008 = 0.1874 is the most that
# never clips a wall the player stands against (camera.c asserts it): 23 / 128 = 0.1797
NEAR_PLANE = 23
LOG_STEPS = 128         # log table resolution: steps per doubling (0.54% per step; 256 was 7 KB more)
SIN_STEPS = 256         # yaw angles per full turn
LOG_ZERO = -400         # logTable[0]: low enough that a = 0 always projects to offset 0

FAR_PLANE = RENDER_DIST * SUBUNITS


def rnd(v):
    return math.floor(v + 0.5)


def main():
    sin_table = [rnd(SUBUNITS * math.sin(2 * math.pi * a / SIN_STEPS)) for a in range(SIN_STEPS)]

    assert VIEW_HALF_WIDTH * CULL_X_DEN == FOCAL_LENGTH * CULL_X_NUM, \
        "horizontal culling must match the viewport (project.S hard-codes 4 x vs 5 z)"

    # Size the tables so project.S needs no range check: cover every |a| and every log difference
    # logTable[|a|] - logTable[z] that survives the culling in project.S.
    #   Every corner has z < FAR_PLANE, and the farthest one z >= NEAR_PLANE. A corner nearer than
    #   NEAR_PLANE is moved along a box edge to depth NEAR_PLANE (project.S, near plane), or at
    #   cardinal yaws the near face is moved to it. Either way the point projected lies in the box's
    #   footprint at depth z = NEAR_PLANE, so the bounds below hold for it with z = NEAR_PLANE.
    #   A box's corners (and so the points of its footprint) spread by at most `spread` in x and z. With R = CULL_X_NUM / CULL_X_DEN, culling keeps a box only if xmin <= R zmax and
    #   xmax >= -R zmax (exact: project.S compares CULL_X_DEN x with CULL_X_NUM z), and yt <= zmax and
    #   yb >= -zmax with yb = yt + SUBUNITS. So any corner has
    #     |x| <= R zmax + spread      and  zmax <= z + spread  ->  |x| <= R (z + spread) + spread
    #     |y| <= zmax + SUBUNITS                               ->  |y| <= z + spread + SUBUNITS
    #   |x| is an integer, so the floor of the bound holds too.
    spread = max(abs(sin_table[a]) + abs(sin_table[(a + 64) % SIN_STEPS]) for a in range(SIN_STEPS))

    # largest |x| and |y| of any corner of a kept box whose farthest corner is at depth zmax
    def x_bound(zmax):
        return CULL_X_NUM * zmax // CULL_X_DEN + spread

    def y_bound(zmax):
        return zmax + SUBUNITS

    # zmax < FAR_PLANE: the log table must reach the largest |a| of all
    log_table_len = max(x_bound(FAR_PLANE - 1), y_bound(FAR_PLANE - 1)) + 1

    log_table = [LOG_ZERO] + [rnd(LOG_STEPS * math.log2(v)) for v in range(1, log_table_len)]
    log_table3 = [3 * v for v in log_table]  # stored x3, see the module docstring

    # per corner at depth z, zmax <= z + spread. That bound can pass the end of the log table
    # near the far plane, where zmax < FAR_PLANE caps |a| instead, hence the min()
    # The level of detail (BOX_RECT in project.S) projects corner values, or values between two
    # corners, at the box's centre depth zc instead. zc lies between the nearest and the farthest
    # corner, so zc >= NEAR_PLANE and zmax <= zc + spread: the same bound, with z = zc, covers it
    d_min = min(log_table[0], 0) - log_table[FAR_PLANE - 1]
    d_max = max(log_table[min(bound(z + spread), log_table_len - 1)] - log_table[z]
                for z in range(NEAR_PLANE, FAR_PLANE)
                for bound in (x_bound, y_bound))
    exp_min, exp_max = d_min, d_max + 1
    # Near-plane clipping (project.S, .Lclip_one) moves a corner behind the near plane along a box
    # edge: |dx| a / b = expTable[log |dx| + log a - log b - log FOCAL_LENGTH], with |dx| <= SUBUNITS
    # (an edge) and a <= b, so the index is at most this. Below exp_min the result is 0 (checked there)
    assert FOCAL_LENGTH & (FOCAL_LENGTH - 1) == 0, "project.S subtracts log2(FOCAL_LENGTH) steps"
    assert log_table[SUBUNITS] - LOG_STEPS * int(math.log2(FOCAL_LENGTH)) <= exp_max, \
        "near-plane clipping must stay inside expTable" 
    exp_table = [rnd(FOCAL_LENGTH * 2 ** (d / LOG_STEPS)) for d in range(exp_min, exp_max + 1)]
    assert rnd(FOCAL_LENGTH * 2 ** ((log_table[0] - log_table[NEAR_PLANE]) / LOG_STEPS)) == 0, \
        "LOG_ZERO must be low enough that a = 0 projects to 0"

    # Every projected coordinate is within SCREEN_COORD_MAX of the screen centre (the largest
    # expTable entry), so a screen-space edge spans at most 2 SCREEN_COORD_MAX px in x and in y.
    # The fill rasterizer (fill.S) turns an edge into a per-column slope S = dy * 256 / dx with
    # recipTable[dx] = round(2^24 / dx) instead of a division (see fill.S). [0] and [1] are unused
    coord_max = exp_table[-1]
    # Only dy < 256: a taller edge (a face close to the camera, a few a frame) is divided in fill.S
    # (.Lrecip_long, ~1k cycles). The whole range, 2 * coord_max + 1 entries, was 7.3 KB
    recip_len = 256
    assert all(rnd(2 ** 24 / d) == ((1 << 24) + d // 2) // d ==
               ((1 << 24) - (d + 1) // 2) // d + 1 for d in range(recip_len, 2 * coord_max + 1)), \
        "fill.S .Lrecip_long must give recipTable's values past its end"
    recip_table = [0, 0] + [rnd(2 ** 24 / dx) for dx in range(2, recip_len)]
    assert max(recip_table) <= 2 ** 23, "recipTable entries must have a top byte <= 0x80 (fill.S)"

    def rows(values, per_line=16):
        return ",\n".join("    " + ", ".join(str(v) for v in values[i:i + per_line])
                          for i in range(0, len(values), per_line))

    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")
    banner = "// auto-generated by utils/gen_tables.py. do not edit; change the script and re-run it\n"

    # Defines only (plus C declarations hidden from the assembler): included by main.c and by
    # project.S. Values must not start with "(": the assembler would read that as a memory operand.
    header = banner + f"""#ifndef PROJECTION_TABLES_H
#define PROJECTION_TABLES_H

#define TABLE_SUBUNITS      {SUBUNITS}
#define TABLE_FOCAL_LENGTH  {FOCAL_LENGTH}
#define TABLE_VIEW_HALF_WIDTH {VIEW_HALF_WIDTH}
#define TABLE_CULL_X_NUM    {CULL_X_NUM}
#define TABLE_CULL_X_DEN    {CULL_X_DEN}
#define TABLE_RENDER_DIST   {RENDER_DIST}
#define TABLE_NEAR_PLANE    {NEAR_PLANE}
#define TABLE_FAR_PLANE     {FAR_PLANE}
#define TABLE_LOG_STEPS     {LOG_STEPS}
#define TABLE_FOCAL_LOG2    {int(math.log2(FOCAL_LENGTH))}
#define LOG_TABLE_LEN       {log_table_len}
#define EXP_TABLE_MIN       {exp_min}
#define EXP_TABLE_LEN       {len(exp_table)}
#define SIN_TABLE_LEN       {SIN_STEPS}
#define SCREEN_COORD_MAX    {coord_max}
#define RECIP_TABLE_LEN     {recip_len}

#ifndef __ASSEMBLER__
#include <stdint.h>
// round(TABLE_SUBUNITS * sin(2 pi a / SIN_TABLE_LEN))
extern const int24_t sinTable[SIN_TABLE_LEN];
// 3 * round({LOG_STEPS} * log2(v)), v in 1/TABLE_SUBUNITS units. x3 so that the difference of two
// entries is a byte offset into expTable. [0] makes a = 0 project to 0
extern const int24_t logTable[LOG_TABLE_LEN];
// round(TABLE_FOCAL_LENGTH * 2^(d / {LOG_STEPS})), d = index + EXP_TABLE_MIN. Covers every d that
// survives the culling in project.S (proved by the asserts in gen_tables.py): no range check needed
extern const int24_t expTable[EXP_TABLE_LEN];
// round(2^24 / dx) for dx = 2 .. 2 SCREEN_COORD_MAX: edge slopes without a division (fill.S)
extern const uint24_t recipTable[RECIP_TABLE_LEN];
#endif

#endif
"""

    tables = banner + f"""#include "projection_tables.h"

const int24_t sinTable[SIN_TABLE_LEN] = {{
{rows(sin_table)}
}};

const int24_t logTable[LOG_TABLE_LEN] = {{
{rows(log_table3)}
}};

const int24_t expTable[EXP_TABLE_LEN] = {{
{rows(exp_table)}
}};

const uint24_t recipTable[RECIP_TABLE_LEN] = {{
{rows(recip_table, 10)}
}};
"""
    for name, text in (("projection_tables.h", header), ("projection_tables.c", tables)):
        with open(os.path.join(src, name), "w") as f:
            f.write(text)
    print(f"wrote src/projection_tables.h and .c: log {log_table_len}, exp {len(exp_table)} "
          f"(d {exp_min}..{exp_max}, max offset {coord_max} px), sin {SIN_STEPS}, recip {recip_len} entries")


if __name__ == "__main__":
    main()
