# CLAUDE.md

A voxel 3D renderer (filled, shaded faces with hidden surface removal) for the TI-84 Plus CE (eZ80 CPU at 48 MHz, no FPU, no hardware
divide, 24-bit `int`). It's written in C and eZ80 assembly against the CE toolchain (CEdev) and
its `graphx`, `keypadc` and `fontlibc` libraries.

| File | Contents |
|---|---|
| `src/main.c` | display setup, buffers, input and movement, per-frame camera setup, the scene, materials, `computeExposure`, `projectScene`, main loop |
| `src/project.S` | `projectBox`: per-box culling, projection and clip classification, hand-written assembly |
| `src/fill.S` | `fillScene` / `drawSpans`: the filled rasterizer with hidden surface removal (see Filled rendering) |
| `src/fill.h` | its interface and span-list constants |
| `src/fill_ref.c` | the same rasterizer in plain C, compiled only with `-DFILL_REFERENCE`, to verify `fill.S` against |
| `src/deinterlace.S` | `deinterlaceRows`: the per-frame de-interlace copy (one `ldir` per row) |
| `src/sort.S` | `sortRecords`: the painter's sort of the scene (see Painter's order), and `gridDist` |
| `src/engine.h` | constants and per-frame camera state shared by `main.c`, `project.S` and `fill.S`; the `projected_box_t` layout |
| `src/projection_tables.{h,c}` | **generated** by `utils/gen_tables.py`; don't edit |

## Priorities

- **Performance beats visual quality.** Approximations, lookup tables, coarse resolution and
  small visual artifacts are acceptable if they buy frame rate. Don't trade speed for accuracy
  unless asked.
- Memory safety still matters. A write outside the buffer silently corrupts memory: other frame
  buffers or the program itself. Speed never justifies an unchecked `_NoClip` call, or a span in
  `fill.S` that isn't inside an open interval (see Filled rendering).

## Build

```sh
make          # release build -> bin/DEMOPRGM.8xp (defines NDEBUG; dbg_printf compiles away). No HUD
make hud      # release build plus the on-screen HUD (FPS, D/F timings, buffer number)
make debug    # -O0, dbg_printf enabled (combine: make debug hud)
make clean
```

- The HUD is compiled in only when `SHOW_HUD` is defined (`make hud`). Without it, `CYCLES()` is
  0 and `initHud`/`drawHud` are empty, so the profiling and fontlib code disappear entirely.
- Objects don't depend on compiler flags, so the makefile writes the variant (`release`, `hud`,
  `debug`, `debug hud`) to `obj/variant` and makes everything depend on it: switching variants
  rebuilds the C files. `.S` files don't depend on it, so don't use `SHOW_HUD`/`DEBUG` in them.

- CEdev v15 is installed at `~/CEdev`. The README says ez80-clang v22+ (nightly) is required.
- `obj/` and `bin/` are build output (gitignored).
- The build should produce no warnings. The `GFX_LCD_WIDTH`/`GFX_LCD_HEIGHT` redefinitions are
  `#undef`ed first, and the makefile passes `--no-warn-execstack` to the linker (the eZ80 has
  no executable-stack concept). Any warning is new.
- `.S` files in `src/` go through the C preprocessor, then the GNU z80 assembler, so they can
  `#include` headers. Keep `;` comments free of apostrophes, and make sure shared `#define`
  values don't start with `(`: the assembler reads `ld de, (x)` as a memory load.
- After adding or removing a header, `make clean`: stale dependency files otherwise break the build.
- `fill_ref.c` is empty unless `FILL_REFERENCE` is defined. To verify `fill.S`, build a scratch
  copy with `CFLAGS += -DFILL_REFERENCE` (see Filled rendering, Verification).

## Running in CEmu (you can do this yourself)

```sh
utils/run_cemu.sh                                     # bin/DEMOPRGM.8xp -> /tmp/cemu_run/t01..t12.png
SHOTS=6 KEYS='"key|right"' utils/run_cemu.sh bin/DEMOPRGM.8xp /tmp/out
```

- The script boots a clean emulated calculator, sends `clibs.8xg` and the program, and takes a
  screenshot every 2 s. Read the PNGs to check rendering, and (in a `make hud` build) the FPS
  counter in the top left.
- **The ROM runs TI-OS 5.8, which blocks native ASM programs.** Launching directly gives
  "ERROR: INVALID". The script also sends arTIfiCE (a TI-BASIC jailbreak, sent as `prgmA`),
  runs it, and launches the program from the arTIfiCE shell.
- Inputs it uses: ROM `~/Downloads/ROM_IMAGE_2.rom`, `~/Downloads/clibs.8xg` and
  `~/Downloads/arTIfiCE_v2.1.8xp`. Override them with `CEMU_ROM`, `CEMU_CLIBS` and `CEMU_ARTIFICE`.
- It always runs CEmu with `--no-settings -n`. Never run CEmu in a way that saves state or
  settings: the user's own config and image live in `~/.config/cemu-dev/CEmu/`.
  - Their config has `save_on_close=true`.
  - Their debugger config has `open_on_reset=true`, which pauses emulation and gives black
    screenshots.
- Kill CEmu by PID (`pgrep -x cemu`). `pkill -f` patterns can match your own shell.
- Limitations:
  - Key presses run faster than wall-clock time, so their timing relative to screenshots is
    loose. `KEYS` can only tap keys, not hold them. The player starts at (0,-1.5,0) facing +z, so
    take the "idle" shots with no `KEYS`. To test held keys or a specific yaw, call
    `updateMotion()` or set `player_*` directly in a scratch copy.
  - `dbg_printf` output is not captured. To inspect values, draw them on screen with
    `fontlib_DrawInt` in a scratch copy of the source.
  - The headless `cemu-autotester` only compares CRCs of memory ranges. It can't take
    screenshots.
- **CEmu runs faster than real time here (about 8×):** the 2 s between screenshots are about 16
  emulated seconds. To show several camera views, switch them on `clock()` every ~14 s and print
  the view number on the HUD, rather than counting on screenshot timing.
- **The FPS counter is quantized to 60/n (60, 30, 20, 15…)** because `waitForBufferScan`
  syncs every frame to the LCD refresh. A real speedup only shows when it crosses one of these
  steps, so compare readings across several screenshots and scenes.
- **Measure in CPU cycles instead.** Timer 2 runs at the CPU clock (`main` enables it). In a
  scratch copy, read it with `*(volatile uint24_t *)TIMER_COUNT_ADDR(2)` around the code and
  draw the difference with `fontlib_DrawUInt`. Don't use `timer_Get()`: with `TIMER_CPU` it
  re-reads until two reads match, which never happens, so it hangs.
- **Verify asm changes against C.** Keep a plain-C reference of the same maths in a scratch
  copy, run both over many boxes and camera states (sub-unit positions, cardinal and arbitrary
  yaws, grid-range edges), and draw the mismatch count. That is how `project.S` was checked.

## Display: the 4bpp half-resolution trick

The LCD is put in **4 bits per pixel** mode (`lcd_Control = 0x13925`: the BPP field is 2 = 4bpp,
with TFT, BGR and power on). graphx doesn't know this and keeps drawing as if the screen were
8bpp, 320×240. That mismatch is the trick:

- A 4bpp LCD row of 320 pixels is 160 bytes. One graphx row of 320 bytes is therefore
  **two** LCD rows.
- A frame is 320×240×½ = 38,400 bytes, which is only 120 graphx rows. graphx rows ≥ 120 belong
  to the next buffer in memory.
- Within a graphx row, bytes 0–159 are the top LCD row and bytes 160–319 are the row below it.
  Each byte holds 2 horizontal LCD pixels, one per nibble.
- So graphx works on a **logical 160×120 screen**. `GFX_LCD_WIDTH`/`GFX_LCD_HEIGHT` are
  redefined to 160/120 on purpose, and `GFX_WIDTH_HALF`/`GFX_HEIGHT_HALF` (80, 60) are the
  centre.
- **Colours are doubled nibbles.** `0x33` makes both pixels palette entry 3; a mixed byte like
  `0x3c` gives two different pixels (possible dithering).
  - Only 16 colours exist: `mypalette` in `main.c`, a PICO-8-style palette.
  - The `GFX_*` colour names are approximate. Use the palette comments for the real colour of
    each index.
- **De-interlacing:** graphx only draws into bytes 0–159 of each graphx row: the even LCD rows.
  Every frame `deInterlaceViewportBuffer` copies them to the odd rows, so every logical pixel
  becomes a 2×2 block. About 149k cycles per frame at full width.
  - An interlaced mode (drawing only one field per frame, alternating, with no copy) existed
    until commit 8f3d1e3. It was faster but combed visibly while moving, so it was removed.
  - `deInterlaceFullBuffer` copies the whole 160-byte width. Both functions call
    `deinterlaceRows` in `src/deinterlace.S`, one `ldir` per row: `ldir` (about 7.7 cycles per
    byte) is the cheapest copy on the CE.
- **Viewport:** the 3D view is the whole logical screen, x 0–159 (`SCREEN_OFFSET` = 0,
  `VIEWPORT_LENGTH` = 160) by y 0–119. The clip region is set to exactly that. With a narrower
  `VIEWPORT_LENGTH`, `fillSideBars` draws black bars beside it, and `gen_tables.py`'s
  `VIEW_HALF_WIDTH` (and with it the culling) must change too.
- **Coordinate spaces:** screen x includes `SCREEN_OFFSET`. Compare x against
  `SCREEN_OFFSET` and `SCREEN_OFFSET + VIEWPORT_LENGTH`, never against `0` and
  `VIEWPORT_LENGTH`, even while `SCREEN_OFFSET` is 0.

## VRAM layout (custom; do not use graphx's own buffer helpers)

VRAM is 0xD40000–0xD657FF (153,600 bytes), split into four 38,400-byte (0x9600) 4bpp frames:

| Name       | Range               | Use                                                    |
|------------|---------------------|--------------------------------------------------------|
| `BUFFER_1` | 0xD40000 – 0xD495FF | frame A (draw/display, swapped each frame)             |
| `BUFFER_2` | 0xD49600 – 0xD52BFF | frame B                                                |
| `DBL_BUF1` | 0xD52C00 – 0xD5C1FF | `fill.S`'s span list (`SPAN_LIST` in `fill.h`)         |
| `DBL_BUF2` | 0xD5C200 – 0xD657FF | blank frame shown during `init_screen` to hide setup   |

- `draw_buffer_ctrl` is `lcd_LpBase`. graphx draws wherever it points (`gfx_vbuffer` reads
  0xE30014). `screen_buffer_ctrl` is `lcd_UpBase`, the buffer the LCD scans out.
  `swapBuffer()` swaps BUFFER_1 and BUFFER_2 between those roles (`drawing_buffer_2` says
  which one is being drawn).
- There is no pre-rendered background any more: `drawSpans` fills it every frame (sky above the
  horizon, ground below; `SKY_COLOUR` / `GROUND_COLOUR` in `engine.h`), and every viewport pixel
  is rewritten each frame. `init_screen` fills both buffers with the sky colour once.
- The span list (`DBL_BUF1`, 38,400 bytes) is VRAM nobody displays; reading and writing it costs
  the same as other RAM. Nothing else may use `DBL_BUF1`.
- Don't use `gfx_SetDrawBuffer`, `gfx_SwapDraw`, `gfx_Blit*`, `gfx_FillScreen` or
  `gfx_ZeroScreen`. They assume 76,800-byte 8bpp buffers at graphx's own addresses: they would
  write across two of these frames, or into the span list.

## Frame pipeline (`main`)

1. Measure the frame time `dt` with `clock()` (32768 ticks/s, clamped to 1/8 s). Every second:
   update FPS.
2. `handleInput(dt)` → `updateMotion()`: arrow keys set target speeds, see Movement. `main` puts
   the keypad in continuous scan mode (`MODE_3_CONTINUOUS`, restored on exit), so `kb_Data` is
   always current and the loop never calls `kb_Scan()`: that waits for a full scan, about 62k
   cycles (1.3 ms).
3. `setupCamera()` builds the per-frame camera state (below). If the camera is in another grid
   cell than the scene was sorted for, `sortScene()` re-sorts `coordinates[]` (see Painter's order).
4. `projectScene()` works out each box's faces to draw (facing the camera and exposed, see Filled
   rendering), skips boxes with none, and culls and projects the rest (`projectBox`) into
   `projected[]`, keeping only the boxes that aren't `BOX_SKIP`, in scene order (far to near).
   Then `fillScene()` rasterizes them front to back into the span list. Neither touches a frame
   buffer, so both run before the wait: while the work fits, it overlaps the LCD showing the last
   frame, and only phase 6 is tied to the refresh. Anything else that doesn't touch the back
   buffer belongs here too.
5. `waitForBufferScan()` busy-waits for the LCD's "next base update" flag (`lcd_IntStatus` bit
   2, which `swapBuffer` clears right after writing the new base), so the previous swap has taken effect and the back buffer is
   no longer on screen. This removes flicker and is why FPS is quantized.
   - Don't go back to polling `lcd_UpBaseCurr` until it stops changing. That also stops while
     the LCD is idle between frames, so any frame under about 5 ms swapped twice per refresh and
     drew into the buffer still on screen.
   - `swapBuffer` must write `lcd_UpBase` before acknowledging the flag. The other way round, a
     refresh in between sets the flag for the old base and the next frame draws into the
     buffer still on screen.
6. `drawSpans()`: the background (about 70k cycles, `push` fills row by row; skipped when boxes
   cover every row), then the spans.
7. `make hud` builds only (about 60k cycles): draw the HUD with fontlib: FPS, `D` (this frame's
   scene: projection and rasterization before the wait plus `drawSpans` after it) and `F` (the previous
   frame's total work, excluding `waitForBufferScan`) in ms. Timer 2 is free-running; `CYCLES()` reads it and differences are valid up to 349 ms.
8. `deInterlaceViewportBuffer`, then `swapBuffer`.

## Movement

- Position is 16.16 fixed point world units (`int32_t player_x/y/z`). Yaw is `uint24_t` in
  1/65536 of an angle step, with 256 steps per turn, so 2^24 is exactly one turn and wrapping is
  just turning past 360°. Yaw 0 looks along +z; increasing yaw turns right, towards +x.
- Controls: Up/Down walk along the view direction, Left/Right strafe, [mode]/[del] turn left/right,
  [2nd] jumps (held, it jumps again on landing), [alpha] crouches while held. `handleInput` turns
  the keys into `MOVE_*` flags for `updateMotion(keys, dt)`. Walking, strafing and turning speeds
  ease towards the target (full speed or a stop in 1/8 s) and are scaled by `dt`, so they don't
  change with FPS. Walking diagonally is √2 faster (no normalisation).
- Height: `player_y = -(eye_height + jump_height)`. The ground plane is always y = 0 (there is no
  collision with the scene). `eye_height` eases between `STAND_HEIGHT` (1.5) and
  `CROUCH_HEIGHT` (0.75) in 1/8 s; a jump starts at `JUMP_SPEED` (7 units/s) under `GRAVITY`
  (24 units/s²), peaking about 1 unit up.
- **Snapping:** when a turn is released within `SNAP_RANGE` (8 steps, 11°) of N/E/S/W and has
  slowed below `SNAP_START`, the yaw eases into it (time constant 0.1 s) and lands on it
  exactly. Exactly cardinal is the fast path (about 30% faster per box), so landing exactly
  matters. `SNAP_RANGE` 0 disables it.
- 32-bit maths is fine here: this runs once per frame.

## Camera and projection

- Camera space: x right, y down (screen y grows downward), z forward. Yaw only, no pitch, so
  vertical world edges stay vertical on screen.
- A box at world (X, Y, Z) occupies [X-1, X]×[Y-1, Y]×[Z-1, Z].
- **Perspective is true 1/z**: `screen = centre + 64 * a / z`. With the 160 px viewport the
  horizontal view is |x| ≤ 1.25 z (about 102°) and the vertical one |y| ≤ 0.94 z.
- **Units:** camera space is in 1/`SUB` (128) world units, and every value shared with
  `project.S` is stored **×3**. An `int24_t` table entry is 3 bytes, so a ×3 value is directly a
  byte offset. Per-frame values must stay exact multiples of 3 (`fillRotTable` builds them
  unscaled, then triples them).
- **Rotation, per frame** (`setupCamera`, C): for each grid line within `GRID_RANGE` (24) of the
  camera cell, tables of `(line − player) × cos/sin`, built by repeated addition, interleaved as
  `rotX_ = {XC, XS}` and `rotZ_ = {ZS, ZC}`. A grid point's camera space is
  `x' = XC − ZS`, `z' = XS + ZC`, so a box corner costs two lookups and an add, and the other
  three corners are fixed per-frame offsets (`cam_k*3`). Also per frame: `camY_`, the index
  biases and the culling constants. The full list is in `engine.h`.
- **Projection, per box** (`projectBox` in `project.S`): table lookups only.
  `64 * a / z = expTable[logTable[|a|] − logTable[z] − EXP_TABLE_MIN]`. logTable entries are
  ×3 too, so the difference of two entries is a byte offset into expTable.
  - Registers: after the table lookups IX points at `.Lvars`. Only once a box survives culling
    is `out` read from the stack into IY, so culled boxes don't pay for it. `.Lvars` holds
    the locals and the per-frame camera scalars (`cam_cos3` … `cam_cardinal`, defined in
    `project.S`, written by `setupCamera`), addressed as `(ix + V_*)`: 3 bytes per access
    against 4–5 for an absolute address. The `V_*` offsets are `#define`s, because the
    assembler won't take a later label difference as a displacement. `.if` checks at the end of
    the file make the assembly fail if they stop matching the labels, or the block grows past
    128 bytes. The grid biases and tables stay in `main.c`, read before IX is set.
- **Culling** happens in camera space before projecting: any corner nearer than `NEAR_PLANE`
  (0.5 units) or at/beyond `FAR_PLANE` (24 units), or the whole box left/right of
  `|x| ≤ 1.25 z` (tested as 4x against 5z, so no division), or above/below `|y| ≤ z` (a little
  wider than the view, which is fine). A box straddling the near plane is skipped rather than clipped, so boxes
  vanish when you walk into them.
- **The culling bounds every table index.** `gen_tables.py` proves (with asserts) that for any
  box passing these exact tests, every logTable/expTable index is in range, so `project.S` has
  no range checks. **If you change the culling, change the generator's proof with it.** The
  same bound keeps projected coordinates within about ±570, which graphx's clipped routines
  handle.
  - The horizontal ratio is `CULL_X_NUM / CULL_X_DEN` in the generator, and must equal
    `VIEW_HALF_WIDTH / FOCAL_LENGTH`. `project.S` hard-codes 5/4 and `#error`s if they change.
- **Regenerating tables:** edit the constants at the top of `utils/gen_tables.py`
  (`SUBUNITS`, `FOCAL_LENGTH`, `VIEW_HALF_WIDTH`, `RENDER_DIST`, `NEAR_PLANE`, `LOG_STEPS`…)
  and run `python3 utils/gen_tables.py`. It writes `src/projection_tables.{h,c}` (about 25 KB
  of tables). `main.c` static-asserts the assumptions it makes, e.g. `VIEW_HALF_WIDTH` is half
  `VIEWPORT_LENGTH`.

## Clipping conventions

- `projectBox` returns `BOX_SKIP`, `BOX_AXIS_NOCLIP/CLIP` (exactly facing N/E/S/W: near and far
  faces are rectangles, 8 coordinates) or `BOX_GEN_NOCLIP/CLIP` (any other yaw: the 4 vertical
  edges, 12 coordinates). Its `out` argument (`BOX_OUT_LEN` entries, a `projected[]` slot)
  receives the coordinates (layout in `engine.h`). `fill.S` turns both layouts into the same 8
  vertices (`cam_axis_corner` maps the AXIS edges to world corners).
- `fill.S` clips by construction (spans only inside open intervals, rows clipped to the
  viewport), so it needs no `_NoClip` guarantee; it only uses `*_NOCLIP` to skip the clamping
  of edge x values. The rules below still hold for anything drawn with graphx.
- A `*_NOCLIP` result must be proven against **all** 8 vertices before any `_NoClip` call.
  - Axis case: each far vertex lies between the screen centre and its near vertex, and the
    centre is inside the viewport. So the near face being inside proves the whole box is, and
    the far face alone decides whether it is entirely off screen.
  - General case: all 12 coordinates are tested. There's no screen-space skip test (camera-space
    culling already removed nearly everything), so `BOX_GEN_CLIP` may occasionally be entirely
    off screen. The clipped calls handle that.
- `_NoClip` graphx functions take `uint8_t` y and `uint24_t` lengths: negatives wrap to huge
  values. Clipped versions take `int`.

## Painter's order

- `coordinates[]` is kept far to near by **Manhattan distance in grid cells** from the camera's cell,
  `|dx| + |dy| + |dz|`. For unit voxels this is an exact painter's order: if A hides part of B,
  the sight line to B passes through A, every coordinate is monotonic along it, so on each axis
  A's cell lies between the camera's and B's, and A's distance is strictly smaller. Equal keys
  never occlude each other. Camera depth (z) is *not* a valid key: two boxes side by side at
  the same depth can overlap on screen. `fill.S` walks the list backwards, **near to far**: the
  same relation read the other way round.
- **The key depends only on the camera's cell**, not on yaw or the position inside the cell. So
  `main` re-sorts only when the cell changes (`sceneOrderStale()`: the `cam_bias_*` differ from
  the ones `sortScene` last used), never while standing still or turning, and the order is never
  stale. Walking crosses about 4 cells per second (up to 6 diagonally).
- `sortScene()` reorders `coordinates[]` itself, so `projectScene` keeps boxes already sorted and
  frames without a re-sort pay nothing. It calls `sortRecords` (`sort.S`): a stable counting sort
  (keys 0 … 3·`GRID_RANGE`) through a scratch copy, in three passes (key and count, prefix,
  place) plus an `ldir` back. Records are 5 bytes: x, y, z, material, exposed faces
  (`REC_SIZE` in `sort.S`).
  - `gridDist`, the counts and the per-key start pointers each begin on a 256-byte boundary
    (`.balign 256` in `sort.S`'s bss), so `ld e,a` / `ld l,a` / `ld ixl,a` index them with no add.
    The tables take 731 bytes of bss.
  - **Self-modifying code:** the three bias bytes and the running maximum key are patched into
    `add a,n` / `cp a,n` immediates, which frees registers for `djnz`. This works because
    programs run from RAM (graphx does the same).
  - The prefix pass only visits keys max … 0, touches only the count of an empty key, and zeroes
    each used count as it reads it: the counts must be zero on entry, and are on exit.
  - Key per axis: `gridDist[(uint8_t)(X + cam_bias_x)]`, a 256-entry table. For every box
    `projectBox` keeps, the grid index is in [0, 48), so its low byte is exact; a culled box may
    get a wrapped key, which doesn't matter. That makes the key three 8-bit adds and lookups.
    Range-checking the 24-bit index instead made the sort ~35% slower.
- Cost per sort (measured with 4-byte records; 5-byte ones copy one byte more per box): 5.3k
  cycles for the 7-box scene, 59k for 120 boxes (about 470 per box). The
  same sort in C took 7.9k and 127k; the old once-a-second `qsort` (world-space comparator with
  `abs` calls) took 41k–48k and 1.1M–1.8M.
- An earlier version sorted the kept boxes every frame (key computed in `projectBox`, bucket
  lists walked in asm): 19k cycles per frame for 36 kept boxes, about 1.1M per second at 60 FPS.
  Sorting on cell change costs nothing standing still and about 0.24–0.35M per second walking
  through the 120-box scene, as one ~1.2 ms spike on each crossing frame.

## Filled rendering (`fill.S`)

The file's header comment is the detailed description; this is the map.

- **Front to back with per-row coverage.** Each viewport row keeps a list of x intervals not yet
  covered (at most `GAP_MAX` = 11, in a 32-byte block per row). Boxes are visited near to far.
  Each face to draw is a convex quad on screen: per row it covers `[xl, xr)`. Only the parts
  inside open intervals become spans, and those parts are removed from the intervals. So a
  hidden pixel is never written, and once every row is closed, farther boxes aren't looked at.
- **Rows, not columns.** A column version was written first and was about twice as slow on
  floors: a floor tile is several times wider than tall on screen, and the cost is per line of a
  face. Tall narrow faces (the tree trunk) are the bad case for rows.
- **Faces to draw** = facing the camera (sign of box cell − camera cell per axis, three
  byte-indexed tables in `projectScene`) **and exposed**: not shared with a neighbouring voxel
  (`computeExposure`, once at startup, stored as the 5th byte of each scene record). A floor then
  costs one face per voxel instead of three, and an enclosed voxel isn't even projected.
  Without exposure culling the hidden side faces of floor tiles dominated the cost.
- **Shading:** `materials[16]` in `main.c` gives each palette colour a top, x-side, z-side and
  bottom colour byte (the z sides are dithered: the two nibbles of a byte are two LCD pixels).
  Scene records hold a material index, not a colour byte.
- **Edges** step x per row with 16 fractional bits; slopes come from `recipTable` (generated by
  `gen_tables.py`, `round(2^24 / dy)`) and `mlt`, with no division. Shared edges are computed from
  the same two vertices in the same direction, so faces meet without cracks.
- **Two phases.** `fillScene` (before the wait) only writes 6-byte span records, with `push` while
  SP points into the list in `DBL_BUF1`. `drawSpans` (after the wait) fills the background with
  `push` row by row, then pops the records and jumps into a 256-aligned unrolled `ld (hl),a /
  inc hl` loop. Both run with interrupts disabled and restore the previous state.
- **Self-modifying code:** the face colour is patched into the row loop's `ld c, n`, as `sort.S`
  does with its biases.
- **Overflow:** a row with more than `GAP_MAX` separate holes drops the rightmost part (it shows
  the background). A full span list is drawn early (`waitForBufferScan` + `drawSpans`), so the
  phase split is lost for that frame but nothing breaks.
- **Memory safety:** spans only ever lie inside open intervals, intervals only shrink from
  `[0, VIEW_W)`, rows are clipped to `[0, VIEW_H)`. Keep it that way: it is what makes arbitrary
  projected coordinates safe.
- **Verification:** `fill_ref.c` is the same algorithm in C and must draw exactly the same frame.
  A scratch harness built with `-DFILL_REFERENCE` (replace `main()`: for a few hundred
  pseudo-random camera positions and yaws, a quarter of them exactly cardinal, render the
  153-box test scene with both into two VRAM frames and count differing bytes) found 0
  differences in 300 views. A deliberately broken reference was caught (28 of 40 views). Rerun
  it after changing `fill.S` or `fill_ref.c`, and change both together.
- **Costs** (cycles, measured): about 2k per face plus about 2k per edge of setup, about 500 per
  face-row (430 for the usual one-interval row, plus the two edge steps), about 50 per span and
  10.4 per pixel in `drawSpans`.
- **If it needs to be faster:**
  - a `push` path in `drawSpans` for long spans (~3.5 instead of 10.4 cycles per pixel)
  - edges shared between faces computed once
  - the per-box face test moved into `projectBox`
  - long spans checked against the row's interval directly instead of the general loop

## eZ80 performance notes

Measured in CEmu with the CPU-cycle timer:

- **The CE is memory bound: about 4 cycles per byte fetched or read** (opcode and immediate
  bytes included) **and about 2 per byte written.** Measured on `fill.S`: an unrolled
  `ld (hl),a / add hl,de` pixel loop costs ~10.4 cycles per pixel, a `push` fill ~3.7 per byte, and
  whole routines match "count the bytes, times 4" within ~10%. Shorter instructions win: a 4-byte
  absolute access (`ld a,(nn)`) costs a third more than `ld a,(ix+d)`, and `call`/`ret` moves 8
  bytes. Estimate a routine by its bytes before writing it.
- **clang almost never emits the hardware `mlt` (8×8→16) instruction** (seen once, for an 8-bit
  divide by 3 as a multiply by 0xAB). Library-call costs, in cycles:

  | Operation | Cycles |
  |---|---|
  | 8×8 multiply | ~180 |
  | 16-bit multiply | ~240 |
  | 24-bit multiply (including by constants like 96 or 195) | ~380 |
  | 32-bit multiply | ~680 |
  | divide | 1,700–2,000 |
  | right shift (even `>> 8`, even via byte access) | ~250 |
  | `& 0xff` | library call |

- What's cheap: additions, table lookups, compares, left shifts (`add hl,hl`), multiplying by a
  power of 2.
- `x < 0 ? -x : x` and `abs()` become `__icmpzero` + `__ineg` calls.
- Signed compares compile inline (bias by 0x800000, then an unsigned compare).
- **Compiler bug (ez80-clang 22.1.8):** when a chain of compares is turned into a 0/1 value
  (`a && b && c` stored in a variable rather than branched on), the backend can emit
  `sbc a, l` where it means `sbc a, a`, giving a garbage result that depends on whatever `A`
  held. Seen in a test harness, not in the engine. Write such tests as `if`s, and before
  trusting a safety check (e.g. one guarding a `_NoClip` call), grep `obj/lto.s` for
  `sbc\ta, [bcdehl]$`, which should find nothing.
- **Hot per-object code belongs in assembly** (`src/*.S`). ABI: arguments at `sp+3`, `sp+6`,
  `sp+9` (3 bytes each, even for `char`); 8-bit result in `A`, 24-bit in `HL`. Preserve `IX`;
  `AF BC DE HL IY` may be clobbered. `ld rr,(hl)` and `lea` are available.
  - Inline `__asm__` also works (`"+l"` puts a value in HL), but separate `.S` files are easier
    to reason about.
- Per-frame code can stay in C: a few library calls per frame don't matter.
- Where the time goes with filled rendering (cycles, `make hud`):

  | Scene | projection | `fillScene` | `drawSpans` | `D` |
  |---|---|---|---|---|
  | the 7-box tree, start view | 34k | 300k | 106k | 9.2 ms (60 FPS) |
  | 153-box test scene (11×11 floor, wall, pillar, slab, tree), dense views | 400k–580k | 800k–1.5M | 140k–250k | 28–48 ms |
  | same, nothing in view | 300k | 12k | 74k | 8 ms |

  On the same 153-box scene and views the old wireframe renderer took `D` 11–38 ms against
  14–48 ms now, so filling costs about 1.0–1.8× the wireframe. Projection is a large share:
  `projectBox` + call overhead is about 3.5k cycles per kept box, and the C face test in
  `projectScene` about 300 per box.
- Whole frame, 7-box idle scene, `make hud`, before interlacing and the full-width viewport
  (`F` 9.7 ms): de-interlace 119k, background blit 127k, scene 123k, HUD about 60k,
  `setupCamera` 23k, everything else about 6k cycles. Now, at full width: `F` 8.6 ms
  with the (since removed) interlaced mode (no de-interlace, blit about 159k, and more of the scene
  in view); 14.6 ms with the de-interlace copy every frame. Copying
  memory with `ldir` costs about 8 cycles per byte; a write-only fill with unrolled `push` (SP
  pointed into the buffer, interrupts off) about 3.8.
- There's no FPU. Don't introduce `float`/`double` at runtime; precompute in Python.
- Don't leave `clock()`, `dbg_printf` argument evaluation or other timing calls in hot paths.
  `clock()` isn't removed in release builds even when its result is unused. The `debug` branch
  has profiling calls like this. One `clock()` per frame for `dt` is fine.

## Repository notes

- Branches:
  - `main`: the old engine (exponential projection, integer movement, no rotation).
  - `debug`: `main` plus per-object `clock()` / `dbg_printf` timing calls.
  - `rewrite`: this rewrite, before filled rendering.
- `gfx/` holds convimg output for the palette. Only `src/` is compiled; `mypalette` in
  `main.c` is the one that's used.
- `readme.md` has the TODO list and changelog. Its demo GIF links are Discord CDN URLs that
  expire.
- Open issues:
  - boxes vanish instead of clipping at the near plane: with filled faces this shows as a hole
    in the floor near the bottom of the screen when looking down at it
  - the ground is a flat colour below the horizon, not geometry
  - scene coordinates are `int8_t`; exposure is computed once at startup (`computeExposure`), so
    a scene that changes at runtime must call it again
