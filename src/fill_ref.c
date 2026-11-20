// C reference of the fill rasterizer (fill.S): the same algorithm, drawing the same frame pixel for
// pixel, to verify the assembly against (see CLAUDE.md, "Verify asm changes against C"). Compiled
// only with -DFILL_REFERENCE; it is slow (library calls for every shift and multiply).
#ifdef FILL_REFERENCE

#include <stdint.h>
#include "engine.h"
#include "fill.h"

#define ROW      320            // bytes per graphx row
#define VIEW_W   (VIEW_X1 - VIEW_X0)

// the reference keeps its own span list, and emits the background as spans too
#define SPAN_MAX 3000
typedef struct {
    uint8_t *addr;
    uint8_t len;
    uint8_t colour;
} span_t;
static span_t span_list[SPAN_MAX];
static span_t *span_end;

// per row: the open x intervals [x0, x1), unordered
static uint8_t gap_n[VIEW_H];
static uint8_t gap_x0[VIEW_H][GAP_MAX];
static uint8_t gap_x1[VIEW_H][GAP_MAX];
static uint8_t open_rows;       // rows with at least one open interval
static uint8_t *row0;           // first viewport column of row 0 in the frame being drawn

// per row: x of the left and right edge of the face being drawn
static uint8_t e_xl[VIEW_H], e_xr[VIEW_H];

void drawSpansRef(void) {
    for (const span_t *s = span_list; s != span_end; s++) {
        uint8_t *p = s->addr;
        for (uint8_t n = s->len; n != 0; n--) {
            *p++ = s->colour;
        }
    }
    span_end = span_list;
}

static void emit(uint8_t y, uint8_t x0, uint8_t x1, uint8_t colour) {
    span_end->addr = row0 + x0 + (uint24_t)y * ROW;
    span_end->len = x1 - x0;
    span_end->colour = colour;
    span_end++;
}

// at most GAP_MAX + 1 spans per row and face
static void roomForRow(void) {
    if (span_end >= span_list + SPAN_MAX - GAP_MAX - 1) {
        waitForBufferScan();
        drawSpansRef();
    }
}

// dx * 65536 / dy (truncated) through recipTable, exactly as fill.S computes it:
// (|dx| recipTable[dy]) >> 8. dy >= 1, |dx| <= 2 SCREEN_COORD_MAX
static int32_t fillSlope(int24_t dx, int24_t dy) {
    if (dy < 2) {
        return 0;  // a one row edge is only evaluated at its start
    }
    const uint32_t a = dx < 0 ? -dx : dx;
    const uint32_t r = recipTable[dy];
    const int32_t s = (int32_t)(a * (r >> 8) + ((a * (r & 0xff)) >> 8));
    return dx < 0 ? -s : s;
}

// x of the chain of vertices from t to b (stepping dir through the 4, y non-decreasing) for the
// rows [y, y1) into out[]: round(x) of the edge, clamped to [0, 255] (from VIEW_W on, every value
// means "right of the viewport")
static void rasterChain(uint8_t *out, const int24_t *vx, const int24_t *vy, uint8_t t, int8_t dir,
                        uint8_t b, int24_t y, int24_t y1) {
    uint8_t i = t;
    while (i != b) {
        const uint8_t j = (uint8_t)(i + dir) & 3;
        const int24_t ya = vy[i], yb = vy[j];
        const int24_t end = (j == b || yb > y1) ? y1 : yb;
        if (end > y) {
            // here ya <= y < end <= yb
            const int24_t xa = vx[i];
            const int32_t s = fillSlope(vx[j] - xa, yb - ya);
            int32_t x = ((int32_t)xa << 16) + 32768 + (int32_t)(y - ya) * s;
            for (; y < end; y++) {
                const int24_t r = (int24_t)(x >> 16);
                out[y] = r < 0 ? 0 : r > 255 ? 255 : (uint8_t)r;
                x += s;
            }
        }
        i = j;
    }
}

// Row y: emits the parts of [xl, xr) still open, front to back, and closes them. The intervals are
// unordered here; fill.S keeps them sorted, which changes which interval is looked at first but
// not which pixels a face gets (they are disjoint, and a split happens inside just one)
static void coverRow(uint8_t y, uint8_t xl, uint8_t xr, uint8_t colour) {
    if (xr > VIEW_W) {
        xr = VIEW_W;
    }
    uint8_t *g0s = gap_x0[y], *g1s = gap_x1[y];
    uint8_t n = gap_n[y];
    uint8_t i = 0;
    while (i < n) {
        const uint8_t g0 = g0s[i], g1 = g1s[i];
        const uint8_t a = xl > g0 ? xl : g0;
        const uint8_t b = xr < g1 ? xr : g1;
        if (a >= b) {
            i++;
            continue;
        }
        emit(y, a, b, colour);
        if (a > g0) {
            g1s[i] = a;             // the left part stays open
            if (b < g1) {           // and the right part: a split. Nothing else overlaps
                if (n < GAP_MAX) {
                    g0s[n] = b;
                    g1s[n] = g1;
                    n++;
                } else {
                    emit(y, b, g1, y < CENTRE_Y ? SKY_COLOUR : GROUND_COLOUR);  // see fill.S
                }
                break;
            }
            i++;
        } else if (b < g1) {
            g0s[i] = b;             // the right part stays open
            i++;
        } else {
            n--;                    // closed: move the last one here and look at it next
            g0s[i] = g0s[n];
            g1s[i] = g1s[n];
        }
    }
    if (n == 0) {
        open_rows--;
    }
    gap_n[y] = n;
}

// A face, corners in clockwise screen order (front faces all come out that way)
static void fillFace(const int24_t *vx, const int24_t *vy, uint8_t colour) {
    uint8_t t = 0, b = 0;
    for (uint8_t k = 1; k < 4; k++) {
        if (vy[k] < vy[t]) {
            t = k;
        }
        if (vy[k] > vy[b]) {
            b = k;
        }
    }
    int24_t y0 = vy[t] > 0 ? vy[t] : 0;
    int24_t y1 = vy[b] < VIEW_H ? vy[b] : VIEW_H;
    while (y0 < y1 && gap_n[y0] == 0) {
        y0++;
    }
    while (y1 > y0 && gap_n[y1 - 1] == 0) {
        y1--;
    }
    if (y0 >= y1) {
        return;
    }
    rasterChain(e_xr, vx, vy, t, 1, b, y0, y1);     // clockwise from the top: the right side
    rasterChain(e_xl, vx, vy, t, -1, b, y0, y1);
    for (int24_t y = y0; y < y1; y++) {
        roomForRow();
        if (gap_n[y] != 0 && e_xl[y] < e_xr[y]) {
            coverRow((uint8_t)y, e_xl[y], e_xr[y], colour);
        }
    }
}

// corners of each face in clockwise screen order: corner k (engine.h) at the top (+0) or bottom
// (+4) of its vertical edge. Faces in FACE_* bit order: +x -x +z -z top bottom
static const uint8_t face_corners[6][4] = {
    {1, 3, 7, 5},       // +x: corners 1 and 3
    {2, 0, 4, 6},       // -x: 0 and 2
    {3, 2, 6, 7},       // +z: 2 and 3
    {0, 1, 5, 4},       // -z: 0 and 1
    {2, 3, 1, 0},       // top
    {4, 5, 7, 6},       // bottom
};
static const uint8_t face_mat[6] = {MAT_XSIDE, MAT_XSIDE, MAT_ZSIDE, MAT_ZSIDE, MAT_TOP, MAT_BOTTOM};

static uint8_t clamp8(int24_t x) {
    return x < 0 ? 0 : x > 255 ? 255 : (uint8_t)x;
}

// level of detail: one rectangle, columns [l, r) and rows [t, b), in the colour lod_slot picks
static void fillRect(const projected_box_t *p) {
    const int24_t *v = p->v;
    const uint8_t colour = materials[p->mat][lod_slot[p->faces]];
    const uint8_t xl = clamp8(v[0] - VIEW_X0), xr = clamp8(v[1] - VIEW_X0);
    const int24_t y0 = v[2] > 0 ? v[2] : 0;
    const int24_t y1 = v[3] < VIEW_H ? v[3] : VIEW_H;
    for (int24_t y = y0; y < y1; y++) {
        roomForRow();
        if (gap_n[y] != 0 && xl < xr) {
            coverRow((uint8_t)y, xl, xr, colour);
        }
    }
}

static void fillBox(const projected_box_t *p) {
    int24_t sx[8], sy[8];       // corner k: (sx[k], sy[k]) top, (sx[k + 4], sy[k + 4]) bottom
    const int24_t *v = p->v;
    if (p->kind == BOX_RECT) {
        fillRect(p);
        return;
    }
    if (p->kind == BOX_GEN) {
        for (uint8_t k = 0; k < 4; k++) {
            sx[k] = sx[k + 4] = v[k];
            sy[k] = v[4 + k];
            sy[k + 4] = v[8 + k];
        }
    } else {
        // nl nr nt nb fl fr ft fb
        for (uint8_t e = 0; e < 4; e++) {
            const uint8_t k = cam_axis_corner[e];
            const uint8_t far = (e & 2) << 1;   // 0 near, 4 far
            sx[k] = sx[k + 4] = v[far + (e & 1)];
            sy[k] = v[far + 2];
            sy[k + 4] = v[far + 3];
        }
    }
    for (uint8_t k = 0; k < 8; k++) {
        sx[k] -= VIEW_X0;
    }

    const uint8_t *m = materials[p->mat];
    for (uint8_t f = 0; f < 6; f++) {
        if (p->faces & (1 << f)) {
            int24_t fx[4], fy[4];
            for (uint8_t i = 0; i < 4; i++) {
                fx[i] = sx[face_corners[f][i]];
                fy[i] = sy[face_corners[f][i]];
            }
            fillFace(fx, fy, m[face_mat[f]]);
        }
    }
}

void fillSceneRef(const projected_box_t *boxes, uint8_t n, uint24_t base) {
    row0 = (uint8_t *)base + VIEW_X0;
    span_end = span_list;
    for (uint8_t y = 0; y < VIEW_H; y++) {
        gap_n[y] = 1;
        gap_x0[y][0] = 0;
        gap_x1[y][0] = VIEW_W;
    }
    open_rows = VIEW_H;

    // front to back: boxes[] is sorted far to near
    for (const projected_box_t *p = boxes + n; p != boxes && open_rows != 0; ) {
        fillBox(--p);
    }

    for (uint8_t y = 0; y < VIEW_H; y++) {
        roomForRow();
        for (uint8_t i = 0; i < gap_n[y]; i++) {
            emit(y, gap_x0[y][i], gap_x1[y][i], y < CENTRE_Y ? SKY_COLOUR : GROUND_COLOUR);
        }
    }
}

#endif
