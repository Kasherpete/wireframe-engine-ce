# CLAUDE.md

A voxel/wireframe 3D renderer for the TI-84 Plus CE (eZ80 CPU at 48 MHz, no FPU, no hardware
divide, 24-bit `int`). It's written in C and eZ80 assembly against the CE toolchain (CEdev) and
its `graphx`, `keypadc` and `fontlibc` libraries.

| File | Contents |
|---|---|
| `src/main.c` | display setup, buffers, input and movement, per-frame camera setup, `drawBox` (graphx calls), main loop |
| `src/project.S` | `projectBox`: per-box culling, projection and clip classification, hand-written assembly |
| `src/deinterlace.S` | `deinterlaceRows`: the per-frame de-interlace copy (one `ldir` per row) |
| `src/engine.h` | constants and per-frame camera state shared by `main.c` and `project.S` |
| `src/projection_tables.{h,c}` | **generated** by `utils/gen_tables.py`; don't edit |

## Priorities

- **Performance beats visual quality.** Approximations, lookup tables, coarse resolution and
  small visual artifacts are acceptable if they buy frame rate. Don't trade speed for accuracy
  unless asked.
- Memory safety still matters. A `_NoClip` draw outside the buffer silently corrupts memory:
  other frame buffers or the program itself. Speed never justifies an unchecked `_NoClip` call.

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
- Expected warnings: `GFX_LCD_WIDTH`/`GFX_LCD_HEIGHT` macro redefinitions and the linker's `.note.GNU-stack` / executable-stack warnings. Anything else is new.
- `.S` files in `src/` go through the C preprocessor, then the GNU z80 assembler, so they can
  `#include` headers. Keep `;` comments free of apostrophes, and make sure shared `#define`
  values don't start with `(`: the assembler reads `ld de, (x)` as a memory load.
- After adding or removing a header, `make clean`: stale dependency files otherwise break the build.

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
    loose. `KEYS` can only tap keys, not hold them. The player starts at (0,-2,0) facing +z, so
    take the "idle" shots with no `KEYS`. To test held keys or a specific yaw, call
    `updateMotion()` or set `player_*` directly in a scratch copy.
  - `dbg_printf` output is not captured. To inspect values, draw them on screen with
    `fontlib_DrawInt` in a scratch copy of the source.
  - The headless `cemu-autotester` only compares CRCs of memory ranges. It can't take
    screenshots.
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
- **Fields:** graphx only draws into bytes 0–159 of each graphx row: the even LCD rows. With the
  draw base set 160 bytes into a frame, the same graphx row y lands on LCD row 2y+1 (the odd
  field) instead, and graphx row 119 still ends on the frame's last byte.
- **Interlaced mode (default):** each frame draws only one field of the back buffer, in the order
  buffer 1 even, buffer 2 even, buffer 1 odd, buffer 2 odd (`swapBuffer`). Nothing is copied.
  Each buffer is drawn every other frame, so its other field is 2 frames old: still images are
  exact, and motion shows a slight comb or "motion blur".
- **Progressive mode:** always draw the even field, then `deInterlaceViewportBuffer` copies it
  to the odd rows, so every logical pixel becomes a 2×2 block. About 149k cycles more per frame
  at full width.
  - `interlace_setting` switches modes at runtime ([mode] key). `swapBuffer` latches it into
    `frame_interlaced`, so a frame is never drawn in one mode and finished in the other:
    de-interlacing a frame drawn into the odd field would copy past the end of the buffer.
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
| `DBL_BUF1` | 0xD52C00 – 0xD5C1FF | pre-rendered background (currently sky only)           |
| `DBL_BUF2` | 0xD5C200 – 0xD657FF | blank frame shown during `init_screen` to hide setup   |

- `draw_buffer_ctrl` is `lcd_LpBase`. graphx draws wherever it points (`gfx_vbuffer` reads
  0xE30014). `screen_buffer_ctrl` is `lcd_UpBase`, the buffer the LCD scans out.
  `swapBuffer()` swaps BUFFER_1 and BUFFER_2 between those roles (and picks the field, see
  Display). The draw base may be a buffer + 160, so compare buffers by `field_phase`, not by
  `draw_buffer_ctrl == BUFFER_1`.
- `preRenderScreen()` draws the background once, with the draw base set to
  `DBL_BUF1 - SCREEN_OFFSET`. That makes the first viewport column land at offset 0, so the
  stored rows are `VIEWPORT_LENGTH` bytes of viewport data at a 320-byte stride.
  - It is only a sky fill for now. The old ground grid used the old projection and was
    removed; a ground drawn with rotation would have to be redrawn every frame.
- Each frame, `blitPrerenderScreen(0)` memcpys those 120 × 160 bytes into the draw base at
  `SCREEN_OFFSET`: into whichever field is being drawn. At startup, `main` fills both fields of
  both buffers with it.
- Don't use `gfx_SetDrawBuffer`, `gfx_SwapDraw`, `gfx_Blit*`, `gfx_FillScreen` or
  `gfx_ZeroScreen`. They assume 76,800-byte 8bpp buffers at graphx's own addresses: they would
  write across two of these frames, or into the pre-rendered background.

## Frame pipeline (`main`)

1. Measure the frame time `dt` with `clock()` (32768 ticks/s, clamped to 1/8 s). Every second:
   update FPS and re-sort objects (`sortCoordinateList`, painter's order in world space: far z
   first. That's wrong once the camera turns, but wireframe doesn't care; fix it for filling).
2. `handleInput(dt)` → `updateMotion()`: arrow keys set target speeds, see Movement. `main` puts
   the keypad in continuous scan mode (`MODE_3_CONTINUOUS`, restored on exit), so `kb_Data` is
   always current and the loop never calls `kb_Scan()`: that waits for a full scan, about 62k
   cycles (1.3 ms).
3. `setupCamera()` builds the per-frame camera state (below).
4. `waitForBufferScan()` busy-waits for the LCD's "next base update" flag (`lcd_IntStatus` bit
   2, which `swapBuffer` clears), so the previous swap has taken effect and the back buffer is
   no longer on screen. This removes flicker and is why FPS is quantized.
   - Don't go back to polling `lcd_UpBaseCurr` until it stops changing. That also stops while
     the LCD is idle between frames, so any frame under about 5 ms swapped twice per refresh and
     drew into the buffer still on screen.
5. Blit the background (about 159k cycles at full width, see the comment on
   `blitPrerenderScreen`), then `drawBox(X, Y, Z)` every object in the current gfx colour.
6. `make hud` builds only (about 60k cycles): draw the HUD with fontlib: FPS, `D` (this frame's scene draw, i.e. `projectBox` + graphx) and
   `F` (the previous frame's total work, excluding `waitForBufferScan`) in ms, and the buffer
   number. Timer 2 is free-running; `CYCLES()` reads it and differences are valid up to 349 ms.
7. Progressive frames only: `deInterlaceViewportBuffer`. Then `swapBuffer`.

## Movement

- Position is 16.16 fixed point world units (`int32_t player_x/y/z`). Yaw is `uint24_t` in
  1/65536 of an angle step, with 256 steps per turn, so 2^24 is exactly one turn and wrapping is
  just turning past 360°. Yaw 0 looks along +z; increasing yaw turns right, towards +x.
- Up/Down walk along the view direction, Left/Right turn. Walking and turning speeds ease
  towards the target (full speed or a stop in 1/8 s) and are scaled by `dt`, so they don't
  change with FPS.
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
  faces are rectangles, drawn as 2 `gfx_Rectangle` + 4 lines) or `BOX_GEN_NOCLIP/CLIP` (any other
  yaw: 4 `gfx_VertLine` + 8 lines). `box_out` holds the coordinates (layout in `engine.h`).
- A `*_NOCLIP` result must be proven against **all** 8 vertices before any `_NoClip` call.
  - Axis case: each far vertex lies between the screen centre and its near vertex, and the
    centre is inside the viewport. So the near face being inside proves the whole box is, and
    the far face alone decides whether it is entirely off screen.
  - General case: all 12 coordinates are tested. There's no screen-space skip test (camera-space
    culling already removed nearly everything), so `BOX_GEN_CLIP` may occasionally be entirely
    off screen. The clipped calls handle that.
- `_NoClip` graphx functions take `uint8_t` y and `uint24_t` lengths: negatives wrap to huge
  values. Clipped versions take `int`.

## eZ80 performance notes

Measured in CEmu with the CPU-cycle timer:

- **The CE is memory bound: roughly 2 cycles per byte touched**, opcode and immediate bytes
  included. Shorter instructions win. A 3-byte absolute address or immediate
  (`ld hl, (nn)` = 7 bytes moved) costs about twice a `push`/`pop` or `(ix + d)` access.
  `call`/`ret` moves 8 bytes.
- **clang never emits the hardware `mlt` (8×8→16) instruction.** Library-call costs, in cycles:

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
- Where the time goes now (60-box test scene, facing north): `projectBox` + call overhead is
  about 237k cycles, and graphx drawing about 430k. Drawing dominates. Turned off-axis, the
  figures are about 300k and 640k.
- Whole frame, 7-box idle scene, `make hud`, before interlacing and the full-width viewport
  (`F` 9.7 ms): de-interlace 119k, background blit 127k, scene 123k, HUD about 60k,
  `setupCamera` 23k, everything else about 6k cycles. Now, at full width: `F` 8.6 ms
  interlaced (no de-interlace, blit about 159k, and more of the scene in view). Copying
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
  - `rewrite`: this rewrite. Wireframe only so far; filling comes later.
- `gfx/` holds convimg output for the palette. Only `src/` is compiled; `mypalette` in
  `main.c` is the one that's used.
- `readme.md` has the TODO list and changelog. Its demo GIF links are Discord CDN URLs that
  expire.
- Open issues on `rewrite`:
  - no filled boxes yet
  - painter's sort is in world space
  - boxes vanish instead of clipping at the near plane
  - no ground
  - scene coordinates are `int8_t`
