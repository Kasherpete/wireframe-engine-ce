# CLAUDE.md

A voxel/wireframe 3D renderer for the TI-84 Plus CE (eZ80 CPU at 48 MHz, no FPU, no hardware
divide, 24-bit `int`). It's written in C against the CE toolchain (CEdev) and its `graphx`,
`keypadc` and `fontlibc` libraries. Everything lives in `src/main.c`.

## Priorities

- **Performance beats visual quality.** Approximations, lookup tables, coarse resolution and
  small visual artifacts are acceptable if they buy frame rate. Don't trade speed for accuracy
  unless asked.
- Memory safety still matters. A `_NoClip` draw outside the buffer silently corrupts memory:
  other frame buffers or the program itself. Speed never justifies an unchecked `_NoClip` call.

## Build

```sh
make          # release build -> bin/DEMOPRGM.8xp (defines NDEBUG; dbg_printf compiles away)
make debug    # -O0, dbg_printf enabled
make clean
```

- CEdev v15 is installed at `~/CEdev`. The README says ez80-clang v22+ (nightly) is required.
- `obj/` and `bin/` are build output (gitignored).
- Expected warnings: `GFX_LCD_WIDTH`/`GFX_LCD_HEIGHT`/`gfx_vram` macro redefinitions, unused
  `buffer` parameters, and unused `drawPanel`/`drawPlane` helpers. Anything else is new.
- Things only compile-checked: nothing in `main()` calls `drawPlane`, `drawPanel` or
  `drawBasePlane`. Changes there need a temporary call added in `main()` to be exercised.

## Running in CEmu (you can do this yourself)

```sh
utils/run_cemu.sh                                     # bin/DEMOPRGM.8xp -> /tmp/cemu_run/t01..t12.png
SHOTS=6 KEYS='"key|right"' utils/run_cemu.sh bin/DEMOPRGM.8xp /tmp/out
```

- The script boots a clean emulated calculator, sends `clibs.8xg` and the program, and takes a
  screenshot every 2 s. Read the PNGs to check rendering, and the FPS counter in the top left.
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
    loose. The player starts at (0,-2,0), so take the "idle" shots with no `KEYS`.
  - `dbg_printf` output is not captured. To inspect values, draw them on screen with
    `fontlib_DrawInt` in a scratch copy of the source.
  - The headless `cemu-autotester` only compares CRCs of memory ranges. It can't take
    screenshots.
- **The FPS counter is quantized to 60/n (60, 30, 20, 15…)** because `waitForBufferScan`
  syncs every frame to the LCD refresh. A real speedup only shows when it crosses one of these
  steps, so compare readings across several screenshots and scenes.

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
- **De-interlacing:** graphx only draws into bytes 0–159 of each row (the top LCD row). After
  drawing, `deInterlaceViewportBuffer` copies bytes 16–143 of every row to bytes 176–303. That
  duplicates each LCD row, so every logical pixel becomes a 2×2 block.
  - `deInterlaceFullBuffer` does the same for the whole 160-byte width.
  - Both ignore their `buffer` argument and act on `gfx_vbuffer`, the current draw buffer
    (`lcd_LpBase`). Set the draw buffer before calling them.
- **Viewport:** the 3D view is logical x 16–143 (`SCREEN_OFFSET` = 16, `VIEWPORT_LENGTH` =
  128) by y 0–119, which is LCD pixels 32–287. The clip region is set to exactly that. The
  side bars are black and hold HUD text: fontlib isn't limited by the clip region.
- **Coordinate spaces:** screen x includes `SCREEN_OFFSET`. Compare x against
  `SCREEN_OFFSET` and `SCREEN_OFFSET + VIEWPORT_LENGTH`, never against `0` and
  `VIEWPORT_LENGTH`.

## VRAM layout (custom; do not use graphx's own buffer helpers)

VRAM is 0xD40000–0xD657FF (153,600 bytes), split into four 38,400-byte (0x9600) 4bpp frames:

| Name       | Range               | Use                                                    |
|------------|---------------------|--------------------------------------------------------|
| `BUFFER_1` | 0xD40000 – 0xD495FF | frame A (draw/display, swapped each frame)             |
| `BUFFER_2` | 0xD49600 – 0xD52BFF | frame B                                                |
| `DBL_BUF1` | 0xD52C00 – 0xD5C1FF | pre-rendered background (sky + ground grid)            |
| `DBL_BUF2` | 0xD5C200 – 0xD657FF | blank frame shown during `init_screen` to hide setup   |

- `draw_buffer_ctrl` is `lcd_LpBase`. graphx draws wherever it points (`gfx_vbuffer` reads
  0xE30014). `screen_buffer_ctrl` is `lcd_UpBase`, the buffer the LCD scans out.
  `swapBuffer()` swaps BUFFER_1 and BUFFER_2 between those roles.
- `preRenderScreen()` draws the background once, with the draw base set to `DBL_BUF1 - 16`.
  That makes logical x = 16 land at offset 0, so the stored rows are 128 bytes of viewport
  data at a 320-byte stride.
  - It depends on `player_y`, but x/z movement doesn't change it: the grid repeats every
    unit. It must be re-run if the ground or `player_y` changes.
- Each frame, `blitPrerenderScreen(0)` memcpys those 120 × 128 bytes into the draw buffer at
  x = 16.
- Don't use `gfx_SetDrawBuffer`, `gfx_SwapDraw`, `gfx_Blit*`, `gfx_FillScreen` or
  `gfx_ZeroScreen`. They assume 76,800-byte 8bpp buffers at graphx's own addresses: they would
  write across two of these frames, or into the pre-rendered background.
- The `gfx_vram` macro at the top of `main.c` is unused and wrong: it dereferences pixel data
  as a pointer. Don't use it.

## Frame pipeline (`main`)

1. Every second: update FPS, re-sort objects (`sortCoordinateList`, painter's order: far z
   first, then Manhattan distance in x/y).
2. `kb_Scan()` and arrow keys move the player one unit per frame. There's no time delta, and
   it's `else if`, so only one axis moves at a time.
3. `waitForBufferScan()` busy-waits until `lcd_UpBaseCurr` stops changing, so the previous swap
   has taken effect. This removes flicker and is why FPS is quantized.
4. Blit the background, then draw every object back to front with `drawBox(..., FILLED, ...)`.
5. Draw the HUD (FPS and buffer number) with fontlib.
6. Run `deInterlaceViewportBuffer`, then `swapBuffer`.

## Projection and lookup tables

- No camera rotation. The camera looks along +z, positions are integers, and screen y grows
  downward.
- A box at (x, y, z), relative to the player, occupies [x-1, x]×[y-1, y]×[z-1, z]. Its z-1 face
  is nearer and larger.
- Perspective is exponential, not 1/z: `screen = coord * powAZx50[z + RENDER_DIST_Z_BACK] +
  centre`.
  - `powAZx50[i] = round(FOV^(i - RENDER_DIST_Z_BACK) * 50)` with `FOV = 0.8`, stored as
    `uint8_t`.
  - It saturates at 255 near the camera and reaches 0 far away.
- `vertexCacheList[z + 20][x + 16]` (`int24_t`) precomputes `x * pow + GFX_WIDTH_HALF`.
  - `drawBox` reuses it for y by subtracting `Y_TABLE_SHIFT` (80 − 60).
  - It is signed and unclamped on purpose. Don't go back to clamped `uint8_t`: off-screen
    vertices then wrap or clamp, which breaks the clip decisions and line slopes.
- `RENDER_DISTANCE_ALGORITHM` (visible z −2…14, |x| < 16, |y| < 10) gates every table lookup.
  `_Static_assert`s check that the render-distance constants keep the indices in range.
- **Regenerating tables** after changing `FOV`, `RENDER_DIST_Z_*` or `RENDER_DIST_X`:
  1. `cd utils && echo 8 | python3 powAZx50.py`. This **rewrites the `powAZx50list` line in
     `src/main.c` in place**. Answer 8, since main.c uses `uint8_t`.
  2. `echo 24 | python3 vertice_gen.py`. This reads `powAZx50list` from main.c and writes
     `utils/vertexCacheList.inc`. Answer 24, since the table is `int24_t`.
  3. Keep `#define NAME value` and `const static uint8_t powAZx50list[] = {...};` on one line:
     both scripts parse main.c by text matching.

## Clipping conventions

- Each primitive computes `clip` = `NO_CLIP` (entirely inside the viewport), `CLIP` (partly
  inside) or `SKIP_RENDER` (entirely outside).
  - `NO_CLIP` must be proven against **all** vertices before any `_NoClip` call.
  - `SKIP_RENDER` should test the whole bounding box: x max < 16, x min ≥ 144, y max < 0 or
    y min ≥ 120.
- `_NoClip` graphx functions take `uint8_t` y and `uint24_t` lengths: negatives wrap to huge
  values. Clipped versions take `int`.
- Trapezoid fillers (`drawTrapezoid`, `drawRotateTrapezoid` and the `_NoClip` versions) return
  early on zero height/width, which would otherwise be a division by zero. They skip spans of
  length ≤ 0.

## eZ80 performance notes

- `int` is 24 bits and native. `int24_t`/`uint24_t` are the cheapest types for arithmetic and
  table loads; 8-bit types save memory but need extension.
- There's no hardware multiply beyond 8×8 and no divide, so division is a library call.
  - Keep division out of per-pixel and per-scanline loops.
  - Signed `/ 64` isn't a plain shift. Use a shift when rounding toward −∞ is acceptable.
- There's no FPU. Don't introduce `float`/`double` at runtime; precompute in the Python scripts.
- Don't leave `clock()`, `dbg_printf` argument evaluation or other timing calls in hot paths.
  `clock()` isn't removed in release builds even when its result is unused. The `debug` branch
  has profiling calls like this.
- Prefer lookup tables and precomputation. Release builds use `-O3`.

## Repository notes

- Branches:
  - `main`: current code.
  - `debug`: `main` plus per-object `clock()` / `dbg_printf` timing calls.
- `gfx/` holds convimg output for the palette. Only `src/` is compiled; `mypalette` in
  `main.c` is the one that's used.
- `readme.md` has the TODO list and changelog. Its demo GIF links are Discord CDN URLs that
  expire.
- The project is due for a rewrite. Known problems:
  - massive duplication (`drawBox` quadrant/clip copies, four trapezoid variants)
  - dead code
  - `int8_t` player coordinates that wrap
  - coarse `uint8_t` perspective table
  - coarse movement and no camera rotation
  - odd vertice projection and perspective/FOV
