# CLAUDE.md

Voxel 3D renderer (filled, shaded faces, hidden surface removal) for the TI-84 Plus CE (eZ80 at 48 MHz, no FPU, no hardware divide, 24-bit `int`). C and eZ80 assembly against CEdev (`graphx`, `keypadc`, `fontlibc`).

| File | Contents |
|---|---|
| `src/main.c` | display setup, input and movement, chunk window, per-frame camera setup, chunk culling, materials, `projectScene`, main loop |
| `src/world.{h,c}` | world file (AppVar): format structs, `worldOpen` (CRC, range checks), chunk lookup, `worldClose` |
| `src/chunk.S` | per-voxel loops of chunk loading and exposure |
| `src/project.S` | `projectRecords`: per-record culling, facing test, projection, level of detail |
| `src/fill.S`, `fill.h` | filled rasterizer with hidden surface removal; its header comment is the detailed description |
| `src/fill_ref.c` | same rasterizer in C, only with `-DFILL_REFERENCE`, to verify `fill.S` |
| `src/deinterlace.S` | per-frame de-interlace copy |
| `src/sort.S` | `sortRecords` (painter's sort of a chunk), `gridDist` |
| `src/engine.h` | constants and per-frame camera state shared by C and asm; `projected_box_t` layout |
| `src/projection_tables.{h,c}` | **generated** by `utils/gen_tables.py`; don't edit |
| `utils/make_world.py` | builds `bin/WORLD.8xv`; its docstring is the file format |
| `utils/bench/` | benchmark and verification harness (`run_bench.sh`) |

## Priorities

- **Performance beats visual quality.** Approximations, lookup tables and small artifacts are fine if they buy frame rate.
- Memory safety still matters: a stray write corrupts other buffers or the program. Never use an unchecked `_NoClip` call, or a span in `fill.S` outside an open interval.
- No `float`/`double` at runtime; precompute in Python. No `clock()`/`dbg_printf` argument evaluation in hot paths (`clock()` survives release builds); one `clock()` per frame for `dt` is fine.

## Build

```sh
make          # release -> bin/DEMOPRGM.8xp, also builds bin/WORLD.8xv (needed at runtime)
make hud      # plus on-screen HUD (FPS, D/F timings)
make debug    # -O0, dbg_printf enabled (combine: make debug hud)
```

- The build must have no warnings. `GFX_LCD_WIDTH/HEIGHT` are `#undef`ed before redefinition; the makefile passes `--no-warn-execstack`.
- The variant is written to `obj/variant`, so switching rebuilds C files. `.S` files don't depend on it: don't use `SHOW_HUD`/`DEBUG` there.
- `.S` files go through the C preprocessor, then the GNU z80 assembler. Keep `;` comments free of apostrophes, and shared `#define` values must not start with `(` (read as a memory load).
- After adding or removing a header, `make clean`.
- CEdev v15 at `~/CEdev`; needs ez80-clang v22+.

## Running in CEmu

```sh
utils/run_cemu.sh                                     # -> /tmp/cemu_run/t01..t12.png
SHOTS=6 KEYS='"key|right"' utils/run_cemu.sh bin/DEMOPRGM.8xp /tmp/out
```

- Boots a clean calculator, sends `clibs.8xg`, the world, arTIfiCE (TI-OS 5.8 blocks native ASM otherwise) and the program, and screenshots every 2 s. Inputs: `~/Downloads/ROM_IMAGE_2.rom`, `clibs.8xg`, `arTIfiCE_v2.1.8xp` (override with `CEMU_ROM`, `CEMU_CLIBS`, `CEMU_ARTIFICE`; `FILES=` sends no world).
- Always run CEmu with `--no-settings -n`: the user's config in `~/.config/cemu-dev/CEmu/` has `save_on_close=true` and `open_on_reset=true` (black screenshots). Kill it by PID (`pgrep -x cemu`), not `pkill -f`.
- CEmu runs ~8x faster than real time (2 s between shots is ~16 emulated seconds). `KEYS` can only tap keys. The player starts at the world's spawn (test world: cell (24, 24), facing +z), so take idle shots without `KEYS`. For held keys or a given yaw, set `player_*` in a scratch copy. For several views, switch on `clock()` every ~14 s and print the view number on the HUD.
- `dbg_printf` isn't captured: draw values with `fontlib_DrawInt` in a scratch copy.
- The world file arrives archived. To test `worldOpen`/`worldClose`, replace `main` in a scratch copy and print with `os_PutStrFull` before graphx starts. To test a corrupt file, flip a byte of `World.pack()` output and write it with `to_8xv` (keeps the TI checksum valid).
- **FPS is quantized to 60/n** (`waitForBufferScan` syncs to the LCD). **Measure in CPU cycles instead:** timer 2 runs at the CPU clock, read `*(volatile uint24_t *)TIMER_COUNT_ADDR(2)` around the code in a scratch copy. Don't use `timer_Get()` (hangs with `TIMER_CPU`).
- **Verify asm changes against a plain-C reference** over many boxes and camera states (sub-unit positions, cardinal and arbitrary yaws, grid-range edges), drawing the mismatch count.

## Display: the 4bpp half-resolution trick

The LCD is in 4bpp mode (`lcd_Control = 0x13925`), but graphx draws as if 8bpp 320x240:

- One graphx row (320 bytes) is two LCD rows; a frame is 120 graphx rows. Bytes 0-159 of a row are the top LCD row, 160-319 the row below. Each byte is 2 horizontal pixels (nibbles).
- So graphx sees a **logical 160x120 screen**; `GFX_LCD_WIDTH`/`HEIGHT` are redefined to 160/120 on purpose, `GFX_WIDTH_HALF`/`HEIGHT_HALF` are 80/60.
- **Colours are doubled nibbles** (`0x33` = palette entry 3 twice; mixed bytes dither). 16 colours: `mypalette` in `main.c` (PICO-8 style); the `GFX_*` names are approximate.
- graphx only draws the even LCD rows; each frame `deInterlaceViewportBuffer` copies them to the odd rows (`deinterlaceRows`, one `ldir` per row, ~149k cycles). A drawing-one-field interlaced mode was removed (commit 8f3d1e3) because it combed while moving.
- **Viewport** is the whole logical screen (`SCREEN_OFFSET` 0, `VIEWPORT_LENGTH` 160). A narrower one needs `fillSideBars` and a changed `VIEW_HALF_WIDTH` in `gen_tables.py`. Always compare screen x against `SCREEN_OFFSET` and `SCREEN_OFFSET + VIEWPORT_LENGTH`, never `0`/`VIEWPORT_LENGTH`.

## VRAM layout (custom; don't use graphx's buffer helpers)

VRAM 0xD40000-0xD657FF is four 38,400-byte (0x9600) 4bpp frames: `BUFFER_1`, `BUFFER_2` (swapped each frame), `DBL_BUF1` (`fill.S` span list, nothing else may use it), `DBL_BUF2` (blank frame during `init_screen`, then scratch: bytes 0-0x4BFF hold `computeExposure`'s occupancy grid, so chunks load only after `init_screen`; from 0x4C00 the bench's `fill_ref.c` span list).

- `draw_buffer_ctrl` is `lcd_LpBase` (where graphx draws); `screen_buffer_ctrl` is `lcd_UpBase` (scanned out). `swapBuffer()` swaps them.
- No pre-rendered background: `drawSpans` fills sky/ground every frame (`SKY_COLOUR`, `GROUND_COLOUR`).
- **Never use** `gfx_SetDrawBuffer`, `gfx_SwapDraw`, `gfx_Blit*`, `gfx_FillScreen`, `gfx_ZeroScreen`: they assume 76,800-byte 8bpp buffers at graphx's addresses and would overwrite two frames or the span list.

## Frame pipeline (`main`)

1. `dt` from `clock()` (32768 ticks/s, clamped to 1/8 s); FPS every second.
2. `handleInput(dt)` -> `updateMotion()`. The keypad is in continuous scan mode, so `kb_Data` is always current; never call `kb_Scan()` (~62k cycles).
3. `setupCamera()`, then `followCamera()` (`updateWindow` loads chunks that entered the window).
4. `projectScene()`: `cullChunks`, then per visible chunk nearest first: `sortChunk` if stale, `projectRecords`. Then `fillScene()` writes spans front to back. Neither touches a frame buffer, so both run before the wait and overlap the LCD showing the last frame. Anything not touching the back buffer belongs here.
5. `waitForBufferScan()` waits for `lcd_IntStatus` bit 2 ("next base update"), which `swapBuffer` clears right after writing the new base.
   - Don't poll `lcd_UpBaseCurr` instead: it stops changing while the LCD is idle, so frames under ~5 ms swapped twice per refresh and drew into the displayed buffer.
   - `swapBuffer` must write `lcd_UpBase` *before* acknowledging the flag, or a refresh in between draws into the displayed buffer.
6. `drawSpans()`: background in still-open rows, then the spans.
7. `make hud` only: draw the HUD (`D` = this frame's scene work, `F` = previous frame's total excluding the wait).
8. `deInterlaceViewportBuffer`, `swapBuffer`.

## Movement

- Position is 16.16 fixed point (`int32_t player_x/y/z`). Yaw is `uint24_t` in 1/65536 of a step, 256 steps per turn (2^24 = one turn, so wrapping is free). Yaw 0 looks along +z, increasing turns toward +x.
- Controls: arrows walk/strafe, [mode]/[del] turn, [2nd] jump, [alpha] crouch. Speeds ease and scale by `dt`. Diagonals are sqrt(2) faster.
- `player_y = -(eye_height + jump_height)`; ground plane is y = 0, no collision with the scene.
- **Snapping:** a released turn within `SNAP_RANGE` (8 steps) of N/E/S/W eases onto it exactly. Exactly cardinal is the fast path (~30% faster per box), so landing exactly matters. 0 disables it.
- 32-bit maths is fine here (once per frame).

## Camera and projection

- Camera space: x right, y down, z forward. Yaw only, no pitch. A box at world (X, Y, Z) occupies [X-1, X]x[Y-1, Y]x[Z-1, Z].
- **Render distance 16** (`RENDER_DIST` in `gen_tables.py` = `GRID_RANGE`, the far plane): the 5x5 chunk window covers it from anywhere in the centre chunk (static-asserted in `main.c`).
- **True 1/z perspective**: `screen = centre + 64 * a / z` (horizontal |x| <= 1.25 z, vertical |y| <= 0.94 z).
- **Units:** camera space is in 1/`SUB` (128) world units, and every value shared with `project.S` is stored **x3** (an `int24_t` table entry is 3 bytes, so a x3 value is a byte offset). Per-frame values must stay exact multiples of 3 (`fillRotTable` builds them unscaled, then triples).
- **Rotation, per frame** (`setupCamera`): per grid line within `GRID_RANGE` of the camera cell, tables of `(line - player) * cos/sin`, built by repeated addition, interleaved as `rotX_ = {XC, XS}`, `rotZ_ = {ZS, ZC}`. A grid point is `x' = XC - ZS`, `z' = XS + ZC`; the other three box corners are fixed per-frame offsets (`cam_k*3`, `cam_k*mid3`). Full list of per-frame state in `engine.h`.
- **Projection** (`projectRecords`): `64 * a / z = expTable[logTable[|a|] - logTable[z] - EXP_TABLE_MIN]` (logTable entries are x3 too, so the difference is a byte offset).
  - Per record, cheapest rejection first: exposed byte, x/z grid lookups and depth test, side tests, y lookup and above/below tests, then facing test. Most records are culled, so the order matters.
  - Grid index is checked in 16 bits (`GRID_INDEX`): record byte sign-extended plus the camera bias patched into an `ld de, nn`.
  - IX points at `.Lvars` (locals, loop state, per-frame camera scalars `cam_cos3` ... `cam_lod_pix` written by `setupCamera`), addressed as `(ix + V_*)`. The `V_*` offsets are `#define`s (the assembler won't take a later label difference as a displacement); `.if` checks at the end of the file fail the assembly if they stop matching or the block exceeds 128 bytes. IY points at the record, then the rotation tables, then the box's `projected[]` slot.
  - **Self-modifying code:** camera biases are patched into the face test's `add a, n` and the grid index's `ld de, nn` once per call.
  - `face_x/y/z` are three consecutive 1024-aligned pages in `project.S`'s bss (`inc h` steps between them).
- **Culling** is in camera space before projecting: the farthest corner nearer than `NEAR_PLANE` (0.5) or at/beyond `FAR_PLANE`, the box left/right of `|x| <= 1.25 z` (tested as 4x vs 5z), or above/below `|y| <= z`. A box straddling the near plane is not clipped: each corner nearer than `NEAR_PLANE` is projected at `NEAR_PLANE` (`LOG_ZC`), so it stretches over the screen instead of vanishing.
- **The culling bounds every table index.** `gen_tables.py` asserts that any box passing these exact tests has all logTable/expTable indices in range, so `project.S` has no range checks, and projected coordinates stay within ~+-570. **If you change the culling, change the generator's proof with it.**
  - `CULL_X_NUM / CULL_X_DEN` must equal `VIEW_HALF_WIDTH / FOCAL_LENGTH`; `project.S` hard-codes 5/4 and `#error`s on change.
- **Level of detail.** A box whose nearest corner is at least `lod_rect_dist` deep (`LOD_RECT_DIST` = 8) becomes `BOX_RECT`: the rectangle of its x/y extent at its centre depth (4 projections, one rectangle in `fill.S`). From `lod_pixel_dist` (36) it is a 2x2 pixel block at its centre (2 projections). A value at or beyond the far plane turns a level off (the pixel level is off at far plane 16). Both are variables in `main.c`, read every frame.
  - The colour is a visible side if any: `.Llod_side` keeps only the x or z side pointing most at the camera *position* (larger of |dx|, |dz| by `gridDist`; z on a tie), so it doesn't change while turning. `lod_slot` is a fixed table (`initLodSlots`). Voxels at y cell 0 are never LOD boxes.
- **Regenerating tables:** edit the constants atop `utils/gen_tables.py` (`SUBUNITS`, `FOCAL_LENGTH`, `VIEW_HALF_WIDTH`, `RENDER_DIST`, `NEAR_PLANE`, `LOG_STEPS`...) and run `python3 utils/gen_tables.py`. `RENDER_DIST` must be at most `WINDOW_HALF * CHUNK_SIZE` (16); more needs a bigger window, which needs more bss than exists. The log table grows ~480 bytes per unit.

## Clipping conventions

- `projected_box_t.kind`: `BOX_AXIS` (exactly N/E/S/W: near and far faces are rectangles, 8 coordinates), `BOX_GEN` (other yaws: 4 vertical edges, 12 coordinates), `BOX_RECT` (LOD: `l r t b`, at least 1 px each way). Layouts in `engine.h`. `fill.S` turns both box layouts into the same 8 vertices via a face table per layout (`.Lfaces_axis` rebuilt per frame from `cam_axis_corner`).
- `fill.S` clips by construction (spans only inside open intervals, rows clipped to the viewport), so `project.S` does no inside/outside classification. A box may be partly or wholly off screen.
- If anything is ever drawn with graphx `_NoClip` calls: they take `uint8_t` y and `uint24_t` lengths, so negatives wrap, and the result must be proven against **all** 8 vertices first. Clipped versions take `int`.

## World and chunks

- **World file:** AppVar `WORLD`, read in place (flash if archived). Format version 2 (`world.h`, `make_world.py` docstring): 48-byte header (magic "VXWD", CRC-32, spawn, ground material...), chunk index, then 4-byte voxels {x + 8 z, y cell, material, exposed faces}. Up to ~16k voxels.
- **Ground plane:** with `ground` set, every cell at y >= 0 is solid ground, drawn as the background below the horizon (`ground_colour3`, read by `drawSpans`) for free. The file holds no ground-material voxels at y >= 0 and none below 0; other materials at y = 0 are drawn over it (top face only).
- **Exposed faces are in the file** (computed by `make_world.py`: ground solid, outside the world empty), so loading is a copy. `computeExposure` is for runtime changes and must agree with the generator (the bench checks the inner 3x3 chunks after each load).
- `worldOpen` (before graphx starts) checks header, CRC, every chunk offset and every voxel's range, so nothing later does. Bounds that matter: x + 8 z < 64, y in -96...95, material < 16, faces < 64. On error `main` prints it, waits for a key and exits.
- `worldClose` (after `gfx_End`) writes the RTC date as last played and updates the CRC; an archived world is unarchived and re-archived (needs free RAM the size of the world; may show the OS garbage collection screen).
- **Chunks:** columns of 8x8 cells, any height, at most 255 voxels. RAM holds a 5x5 window: `chunks[25]` (slot `(cx mod 5) * 5 + (cz mod 5)`), `win[25]` by window position. Records are 5 bytes {x, Y, z, material, exposed}, x/z chunk local, Y = y cell + 1. A chunk's grid index bias is `cam_bias_x + 8 cx + 1`, passed to `projectRecords` and `sortRecords`.
- `updateWindow` copies the chunks new to the window (`copyVoxels`): all 25 at startup or a jump, 5 when crossing a chunk border.
- **Runtime world changes:** change the records, clear `chunk_t.sorted`, update `ymin`/`ymax`, call `computeExposure` for that window position and its 4 neighbours. Not written back to the file. `computeExposure` uses an occupancy grid (chunk plus 1-cell border; layers at y >= 0 solid with a ground plane) in `DBL_BUF2`; a neighbour outside the window counts as empty (its faces point away from the camera).
- **Chunk culling** (`cullChunks`, C): camera-space bounding box of each chunk's 4 corners against the same near/far/side tests as a box, with quarter-unit slack for table rounding.
- **Memory:** bss and the malloc heap share 0xD052C6-0xD13FD8 (~60.4 KB); bss is at 57.9 KB (chunks 32.5, `projected[MAX_PROJECTED = 400]` 15.6, `fill.S` 4.2, rest ~5.6). The program runs from user RAM (17.5 KB code, 26.1 KB tables).
  - **More memory:** ~90 KB of user RAM is free with the program loaded. A scratch AppVar (`ti_Open("...", "w")`, `ti_Resize`, `ti_GetDataPtr`) gives a block (40,000 bytes worked); its pointer is invalid after any variable is created, deleted, resized or (un)archived, so delete it before `worldClose`.
  - The dense preset (no ground plane, chunks at 255) fills the 400 projected boxes in most views, dropping the farthest.

## Painter's order

- Boxes are drawn **near to far** by **Manhattan distance in grid cells** from the camera's cell. For unit voxels this is exact: if A hides part of B, every coordinate is monotonic along the sight line, so A's cell lies between the camera's and B's on each axis and A's distance is strictly smaller. Camera depth (z) is *not* a valid key.
- **Across chunks** the same argument holds: `projectScene` visits chunks by Manhattan chunk distance (`visit_order`) and each chunk's boxes by cell distance; the concatenation is exact. `fill.S` walks `projected[]` forwards. Near to far means a full `projected[]` drops the farthest boxes (`projectRecords` stops at `room`).
- **A chunk's order changes only when the camera's cell changes on an axis where the camera is inside the chunk's range** (x, z: its 8 cells; y: its records' range). `sortChunk` stores that relation per axis (`sortRel`) and re-sorts only when it changes, only for chunks about to be projected.
  - This needs every key exact, culled boxes included: `gridDist` is |d| of the index's low byte as a signed byte (exact to +-127), and `main.c` asserts the window (|dx|, |dz| <= 23) and y range (camera y cell in -4...0: no flying) keep keys under 256.
- `sortRecords` (`sort.S`): stable counting sort on byte keys through a scratch copy: key and count, prefix, place, `ldir` back. ~480 cycles per record.
  - `gridDist`, the counts and the start indices each begin on a 256-byte boundary (`.balign 256`) so `ld e,a` / `ld l,a` index them without an add.
  - **Self-modifying code:** the three bias bytes and the running maximum key are patched into `add a,n` / `cp a,n`.
  - The prefix pass visits only keys 0...max and zeroes each used count as it reads it: counts must be zero on entry, and are on exit.
  - Key per axis is `gridDist[(uint8_t)(X + bias)]`; range-checking the 24-bit index instead was ~35% slower.

## Filled rendering (`fill.S`)

- **Front to back with per-row coverage.** Each viewport row keeps a list of uncovered x intervals (at most `GAP_MAX` = 11, a 32-byte block per row), **sorted left to right and ended by sentinel** `[255, 255]`. A face (convex quad) covers `[xl, xr)` per row; only the parts inside open intervals become spans and are removed from the intervals, so hidden pixels are never written and once every row is closed farther boxes aren't looked at.
  - One-interval rows have their own path (four cases, no loop). With several, a face skips intervals left of it and stops at the first one right of it; a split moves the rest up (`lddr`), a closed interval moves them down (`ldir`).
- **Rows, not columns:** floor tiles are much wider than tall on screen and cost is per line of a face. Tall narrow faces (tree trunk) are the bad case.
- **Faces to draw** = facing the camera (sign of box cell minus camera cell per axis, via `face_x/y/z`) **and exposed** (5th record byte). An enclosed voxel isn't even projected.
- **Face tables:** a box's coordinates are copied into `.Lpage` (`ldir`, 36 bytes); a face is a 16-byte entry of its 4 vertices clockwise, twice over (so a side never wraps), as page offsets of x and y. The right side runs forwards from the top vertex, the left backwards from its copy four entries on; a side stops at the first vertex at or below the last row.
- A `BOX_RECT` writes the same `[xl, xr)` into all its rows through the same row loop: no edges, no face setup.
- **Shading:** `materials[16]` in `main.c` gives each material a top, x-side, z-side (dithered) and bottom colour byte. Records hold a material index.
- **Edges** step x per row with 16 fractional bits; slopes come from `recipTable` (`round(2^24 / dy)`) and `mlt`, none for vertical or one-row edges. Shared edges are computed from the same two vertices in the same direction, so faces meet without cracks. Only an edge with an end outside x 0..255, in a box with one, runs the clamping loop.
- **Two phases.** `fillScene` (before the wait) only writes span records with `push` while SP points into the span list. `drawSpans` (after the wait) fills the background with `push` row by row in rows still open (a closed row is entirely covered), then pops the records. A short span (< `LONG_MIN` = 28 px, 6 bytes) jumps into a 256-aligned unrolled `ld (hl),a / inc hl` loop; a long one (9 bytes, colour x3) goes through a stub writing `len % 3` pixels, then an unrolled `push bc` run (`.Llong_tab` gives stub and entry by length). Both phases run with interrupts disabled and restore the previous state.
- **Self-modifying code:** the face colour is patched into the row loop's `ld c, n`.
- **Overflow:** a row with more than `GAP_MAX` holes drops the rightmost part of the split interval and is marked (block byte +3) so it still gets the background once closed. A full span list is drawn early (`waitForBufferScan` + `drawSpans`): the phase split is lost for that frame, nothing breaks.
- **Memory safety:** spans only lie inside open intervals, intervals only shrink from `[0, VIEW_W)`, rows are clipped to `[0, VIEW_H)`. Keep it so: it is what makes arbitrary projected coordinates safe. A split needs `n < GAP_MAX`, so the sentinel never passes byte 27 (the row address is at 29).
- **Verification:** `fill_ref.c` is the same algorithm in C and must draw exactly the same frame. `utils/bench/run_bench.sh` builds `bench.c` against a copy of `src/` and runs it in CEmu with a world from `make_world.py` (`WORLD=dense` for the no-ground preset; `-DBENCH_WALK` for a straight walk). It prints cycle totals per phase and checks chunk draw order, `computeExposure` against the file, and with `-DFILL_REFERENCE` both renderers into two VRAM frames with differing bytes counted (results on the last screenshot; the screen is 160 px wide, so print averages, not totals, in the right column). **Rerun it after changing `fill.S`, `fill_ref.c` or `project.S`, and change `fill.S` and `fill_ref.c` together.** Sort errors hit both renderers alike, hence the separate sort check.
- **Costs** (cycles, approximate): ~1.1k setup per edge plus 46 per row, ~1.4k per face, ~500 per face-row, ~50 per short span, 10.4 per pixel in `drawSpans`.
- **If it needs to be faster:** share projected corners between neighbouring boxes (per-frame cache by grid point); skip a box whose screen bounding box has no open pixel (~3-4%); step both sides inside the row loop instead of storing x per row; compute edges shared between faces of a box once.

## eZ80 performance notes

- **The CE is memory bound: ~4 cycles per byte fetched or read (opcode and immediate bytes included), ~2 per byte written.** Routines cost about "bytes x 4". Shorter instructions win: `ld a,(nn)` costs a third more than `ld a,(ix+d)`; `call`/`ret` moves 8 bytes. Estimate a routine by its bytes before writing it.
- **clang almost never emits `mlt`.** Library-call costs (cycles): 8x8 multiply ~180, 16-bit ~240, 24-bit ~380 (even by constants), 32-bit ~680, divide 1,700-2,000, right shift ~250 (even `>> 8`), `& 0xff` a library call, `abs()` / `x < 0 ? -x : x` a `__icmpzero` + `__ineg` call.
- Cheap: additions, table lookups, compares (signed ones compile inline), left shifts (`add hl,hl`), powers of 2. `ldir` (~7.7-8 cycles/byte) is the cheapest copy; an unrolled `push` fill (interrupts off) is ~3.8 per byte.
- `mlt rr` clears the upper byte of the 24-bit register (`ld h, a / ld l, n / mlt hl / add hl, de` relies on it).
- **Compiler bug (ez80-clang 22.1.8):** a chain of compares turned into a 0/1 value (`a && b && c` stored, not branched on) can emit `sbc a, l` instead of `sbc a, a`. Write such tests as `if`s, and before trusting a safety check grep `obj/lto.s` for `sbc\ta, [bcdehl]$` (should find nothing).
- **Hot per-object code belongs in assembly** (`src/*.S`); per-frame code can stay in C. ABI: arguments at `sp+3`, `sp+6`, `sp+9` (3 bytes each, even for `char`); 8-bit result in `A`, 24-bit in `HL`. Preserve `IX`; `AF BC DE HL IY` may be clobbered. Inline `__asm__` works (`"+l"` = HL), but `.S` files are easier to reason about.
- **Typical frame cost** (test world with ground plane, ~80 boxes, 128-view walk): ~1.8M cycles, of which `fillScene` 48%, `projectRecords` 26%, `drawSpans` 10%, de-interlace 8%; outside the scene `setupCamera` is 23k and the HUD ~60k. Heavy views (hill, house up close) run 17-19 FPS, light ones 44.

## Open issues

- A box straddling the near plane is not clipped: a corner nearer than `NEAR_PLANE` is projected at `NEAR_PLANE` (stretched, not cut).
- The ground is a flat colour below the horizon, not geometry.
- Big near objects made of many 1x1 faces (hill, house) are the slow views; merging coplanar wall and step faces would help (only the floor is merged, into the ground plane).
- bss is nearly full; more memory means a scratch AppVar (see World and chunks).
- Runtime world changes are not written back to the file.

`readme.md` has the TODO list and changelog. `gfx/` holds convimg palette output; only `src/` is compiled, and `mypalette` in `main.c` is the one used. Branches: `main` is current; `debug` has per-object timing calls; `rewrite` is the pre-filled-rendering rewrite.
