# CLAUDE.md

Voxel 3D renderer (filled, shaded faces, hidden surface removal) for the TI-84 Plus CE (eZ80 at 48 MHz, no FPU, no hardware divide, 24-bit `int`). C and eZ80 assembly against CEdev (`graphx`, `keypadc`, `fontlibc`).

| File | Contents |
|---|---|
| `src/main.c` | `main`: startup and the frame loop, nothing else |
| `src/title.{h,c}` | title screen: menus drawn with graphx text over the world seen from the spawn, camera turning slowly right (`titleUpdate`, `titleDraw`): main (play, settings, quit), world list (`world_list`, `titleWorld`, `titleError`), settings (render distance 8-24, fog, FPS info, GitHub star note; applied at once, not saved). [clear] goes back, quits from the main menu. `main` runs it in the frame loop, all rows de-interlaced while it shows |
| `src/title_font.h` | **generated** by `utils/gen_title_font.py` (Pillow) from `font/minecraft-legacy-edition.otf`: the graphx font of the title buttons; don't edit |
| `src/pico8_font.{h,c}` | the PICO-8 font (`font/myfont.inc`, fontlibc) for the HUD and the title (edition box, corner texts with fontlib; the splash reads its glyphs with `pico8Glyph`); lowercase shows as its capitals |
| `src/display.{h,c}` | 4bpp setup, loading screen with progress bar (`displayLoading`, `displayProgress`, `displayLoadingEnd`; `displayReload` shows it again when switching worlds), startup errors (`displayError`), `displayInit`, VRAM frame addresses, `swapBuffer`, `waitForBufferScan`, full-frame de-interlace (startup) |
| `src/colours.{h,c}` | `palette`, `materials[]` (plain, then fogged), `ground_colour3`, `bg_rows` (`coloursInit`) |
| `src/player.{h,c}` | player state, keys, movement, jump/crouch, collision response (`playerSpawn`, `playerUpdate`) |
| `src/collide.{h,c}`, `collide.S` | collision: occupancy bit grid around the player (`collideBuild`, C), `collideBody` and `collideMark` (asm) |
| `src/camera.{h,c}`, `camera.S` | `setupCamera` (per-frame state for `project.S`, fog bands), face tables (`cameraInit`), `lod_rect_dist`, `render_dist`, `fog_dist`; `fillRotTable` (asm) |
| `src/chunks.{h,c}` | chunk window (`windowAlloc`/`windowFree`: chunk memory, `windowInit`, `windowFollow`, `windowChunk`), `sortChunk`, `computeExposure`, painter's-order notes |
| `src/scene.{h,c}` | `projected[]` (one batch), `cullChunks`, `projectScene` (projects and fills chunk by chunk, chunk occlusion), LOD colour table (`sceneInit`) |
| `src/occlude.S` | `chunkSeen` (is a chunk's screen rectangle still open?), `cullLoop` (the per-chunk loop of `cullChunks`) |
| `src/hud.{h,c}` | HUD (FPS, D/F timings) while `hud_shown` (settings; default on in `make hud`), `CYCLES()`; `PROF()` and the profiling counters `make hud` only |
| `src/world.{h,c}` | world file (AppVar): format structs, `worldScan` (every AppVar whose data starts with `VXWD`, most recently played first), `worldOpen` (CRC, range checks; keeps the open world on error), `worldSelect`, `worldStart` (open the newest that passes, or report), chunk lookup, `worldClose(save)` |
| `src/chunk.S` | per-voxel loops of chunk loading and exposure |
| `src/project.S` | `projectRecords`: per-record culling, facing test, projection, near-plane clipping, level of detail, fog |
| `src/fill.S`, `fill.h` | filled rasterizer with hidden surface removal; its header comment is the detailed description |
| `src/fill_ref.c` | same rasterizer in C, only with `-DFILL_REFERENCE`, to verify `fill.S` |
| `src/deinterlace.S` | `deinterlaceRows`: plain row copy (startup only; per frame it is `deinterlaceFrame` in `fill.S`) |
| `src/sort.S` | `sortRecords` (painter's sort of a chunk), `gridDist` |
| `src/engine.h` | constants and per-frame camera state shared by C and asm; `projected_box_t` layout |
| `src/projection_tables.{h,c}` | **generated** by `utils/gen_tables.py`; don't edit |
| `utils/make_world.py` | builds `bin/WORLD.8xv`; its docstring is the file format |
| `utils/bench/` | benchmark and verification harness (`run_bench.sh`; `bench.c` replaces `main.c`) |
| `utils/test/` | scripted collision test (`run_collide_test.sh`; `collide_test.c` replaces `main.c`, keys scripted) |

## Priorities

- **Performance beats visual quality.** Approximations, lookup tables and small artifacts are fine if they buy frame rate.
- Memory safety still matters: a stray write corrupts other buffers or the program. Never use an unchecked `_NoClip` call, or a span in `fill.S` outside an open interval.
- No `float`/`double` at runtime; precompute in Python. No `clock()`/`dbg_printf` argument evaluation in hot paths (`clock()` survives release builds); one `clock()` per frame for `dt` is fine.

## Build

```sh
make          # release -> bin/DEMOPRGM.8xp, also builds bin/WORLD.8xv (needed at runtime)
make hud      # HUD (FPS, D/F timings) on by default, plus the profiling counters
make debug    # -O0, dbg_printf enabled (combine: make debug hud)
```

- The build must have no warnings. `GFX_LCD_WIDTH/HEIGHT` are `#undef`ed before redefinition; the makefile passes `--no-warn-execstack`.
- The variant is written to `obj/variant`, so switching rebuilds C files. `.S` files don't depend on it: don't use `SHOW_HUD`/`DEBUG` there.
- `.S` objects don't track the headers they include either: after changing `engine.h`, `projection_tables.h` (`gen_tables.py`), `chunks.h` or `fill.h`, `make clean`, or a stale object links (an old `camera.S` built for range 16 silently produced wrong rotation tables).
- `.S` files go through the C preprocessor, then the GNU z80 assembler. Keep `;` comments free of apostrophes, and shared `#define` values must not start with `(` (read as a memory load).
- After adding or removing a header, `make clean`.
- CEdev v15 at `~/CEdev`; needs ez80-clang v22+.

## Running in CEmu

```sh
utils/run_cemu.sh                                     # -> /tmp/cemu_run/t01..t12.png
SHOTS=6 KEYS='"key|right"' utils/run_cemu.sh bin/DEMOPRGM.8xp /tmp/out
```

- Boots a clean calculator, sends `clibs.8xg`, the world, arTIfiCE (TI-OS 5.8 blocks native ASM otherwise) and the program, and screenshots every 2 s. Inputs: `~/Downloads/ROM_IMAGE_2.rom`, `clibs.8xg`, `arTIfiCE_v2.1.8xp` (override with `CEMU_ROM`, `CEMU_CLIBS`, `CEMU_ARTIFICE`; `FILES=` sends no world).
- Always run CEmu with `--no-settings -n`: the user's config in `~/.config/cemu-dev/CEmu/` has `save_on_close=true` and `open_on_reset=true` (black screenshots). Kill it by PID (`pgrep -x cemu`), not `pkill -f`. Only one run at a time: `run_cemu.sh` (and so the bench and collision scripts) kills every running `cemu` first, so a run in the background and another in the foreground spoil each other (blank or home-screen shots).
- CEmu runs ~8x faster than real time (2 s between shots is ~16 emulated seconds). `KEYS` can only tap keys. The player starts at the world's spawn (test world: cell (24, 24), facing +z), so take idle shots without `KEYS`. For held keys or a given yaw, set `player_*` in a scratch copy, or add a scenario to `utils/test/collide_test.c` (it feeds `player.c` scripted keys). For several views, switch on `clock()` every ~14 s and print the view number on the HUD.
- `dbg_printf` isn't captured: draw values with `fontlib_DrawInt` in a scratch copy.
- The world file arrives archived. To test `worldOpen`/`worldClose`, replace `main` in a scratch copy and print with `os_PutStrFull` before graphx starts. To test a corrupt file, flip a byte of `World.pack()` output and write it with `to_8xv` (keeps the TI checksum valid).
- **FPS is quantized to 60/n** (`waitForBufferScan` syncs to the LCD). **Measure in CPU cycles instead:** timer 2 runs at the CPU clock, read `*(volatile uint24_t *)TIMER_COUNT_ADDR(2)` around the code in a scratch copy. Don't use `timer_Get()` (hangs with `TIMER_CPU`).
- **Verify asm changes against a plain-C reference** over many boxes and camera states (sub-unit positions, cardinal and arbitrary yaws, grid-range edges), drawing the mismatch count.

## Display: the 4bpp half-resolution trick

The LCD is in 4bpp mode (`lcd_Control = 0x13925`), but graphx draws as if 8bpp 320x240:

- One graphx row (320 bytes) is two LCD rows; a frame is 120 graphx rows. Bytes 0-159 of a row are the top LCD row, 160-319 the row below. Each byte is 2 horizontal pixels (nibbles).
- So graphx sees a **logical 160x120 screen**; `GFX_LCD_WIDTH`/`HEIGHT` are redefined to 160/120 on purpose, `GFX_WIDTH_HALF`/`HEIGHT_HALF` are 80/60.
- **Colours are doubled nibbles** (`0x33` = palette entry 3 twice; mixed bytes dither). 16 colours: `palette` in `colours.c` (PICO-8 style).
- Background: sky above the horizon (row `CENTRE_Y`), ground below, as three bands of rows `bg_rows` (`fill.h`): sky colour, `bg_dither3`, `ground_colour3`. With fog off (`fog_dist` at or beyond the far plane) `{60, 0, 60}`; otherwise the ground beyond the far plane moves into the first band and the ground beyond `fog_dist` is dithered (`setupCamera`, `fill_ref.c` matches).
- graphx only draws the even LCD rows; each frame `deinterlaceFrame` (`fill.S`) copies them to the odd rows, one `ldir` per row (~1.3k cycles), except rows nothing was drawn in: `drawSpans` fills both LCD rows of those with background by `push` (320 bytes, ~0.5k more than 160). About 40% of rows in a walk; the first `HUD_ROWS` rows are always copied (HUD text is drawn after `drawSpans`). A drawing-one-field interlaced mode was removed (commit 8f3d1e3) because it combed while moving.
- **Viewport** is the whole logical screen (`SCREEN_OFFSET` 0, `VIEWPORT_LENGTH` 160). A narrower one needs `fillSideBars` and a changed `VIEW_HALF_WIDTH` in `gen_tables.py`. Always compare screen x against `SCREEN_OFFSET` and `SCREEN_OFFSET + VIEWPORT_LENGTH`, never `0`/`VIEWPORT_LENGTH`.

## VRAM layout (custom; don't use graphx's buffer helpers)

VRAM 0xD40000-0xD657FF is four 38,400-byte (0x9600) 4bpp frames: `BUFFER_1`, `BUFFER_2` (swapped each frame), `DBL_BUF1` (`fill.S` span list; between frames, when no spans are listed, `computeExposure`'s occupancy grid in its first 0x4C00 bytes; nothing else may use it), `DBL_BUF2` (29 chunk slots, or with `-DFILL_REFERENCE` 19 and the bench's `fill_ref.c` lists after them; never shown).

- `draw_buffer_ctrl` is `lcd_LpBase` (where graphx draws); `screen_buffer_ctrl` is `lcd_UpBase` (scanned out). `swapBuffer()` swaps them.
- No pre-rendered background: `drawSpans` fills sky/ground every frame (`SKY_COLOUR`, `GROUND_COLOUR`).
- **Never use** `gfx_SetDrawBuffer`, `gfx_SwapDraw`, `gfx_Blit*`, `gfx_FillScreen`, `gfx_ZeroScreen`: they assume 76,800-byte 8bpp buffers at graphx's addresses and would overwrite two frames or the span list.

## Frame pipeline (`main`)

1. `dt` from `clock()` (32768 ticks/s, clamped to 1/8 s); FPS every second.
2. `playerUpdate(dt)`: keys -> `updateMotion()` (movement and collision). The keypad is in continuous scan mode, so `kb_Data` is always current; never call `kb_Scan()` (~62k cycles).
3. `setupCamera()` (also the fog background bands, two divisions), then `windowFollow()` (`updateWindow` loads chunks that entered the window).
4. `projectScene(draw_buffer_ctrl)`: `cullChunks`, `fillBegin`, then per visible chunk nearest first: `chunkSeen` (skip it if its screen rectangle is covered already), `sortChunk` if stale, `projectRecords` into the `projected[]` batch (64 boxes; a chunk with more is done in pieces) and `fillBoxes` (spans front to back). Stops when every row is covered or after `MAX_PROJECTED` (400) boxes. Nothing touches a frame buffer, so it all runs before the wait and overlaps the LCD showing the last frame. Anything not touching the back buffer belongs here.
5. `waitForBufferScan()` waits for `lcd_IntStatus` bit 2 ("next base update"), which `swapBuffer` clears right after writing the new base.
   - Don't poll `lcd_UpBaseCurr` instead: it stops changing while the LCD is idle, so frames under ~5 ms swapped twice per refresh and drew into the displayed buffer.
   - `swapBuffer` must write `lcd_UpBase` *before* acknowledging the flag, or a refresh in between draws into the displayed buffer.
6. `drawSpans()`: background in still-open rows, then the spans.
7. If `hud_shown` (settings, or `make hud`): draw the HUD (`D` = this frame's scene work, `F` = previous frame's total excluding the wait). [clear] in game quits; on the title, `titleUpdate` handles it.
8. `deinterlaceFrame(HUD_ROWS)`, `swapBuffer`.

## Movement

- Position is 16.16 fixed point (`int32_t player_x/y/z`). Yaw is `uint24_t` in 1/65536 of a step, 256 steps per turn (2^24 = one turn, so wrapping is free). Yaw 0 looks along +z, increasing turns toward +x.
- Controls: [up]/[down] walk, [left]/[right] turn, [mode]/[del] strafe, [2nd] jump (held: again on landing), [alpha] crouch. Speeds ease and scale by `dt`. Diagonals are sqrt(2) faster. Per-frame rates are written as shifts and adds of `dt` (static-asserted): a 32-bit multiply and shift would be ~1k cycles each.
- `player_x/z` and the eye `player_y = feet_y - eye_height` (`player.c`); the body is `BODY_RADIUS` (0.3) to each side and reaches `HEAD_ROOM` (0.25) above the eye: 1.75 standing, 1.0 crouched. A jump (8 u/s, gravity 24 u/s^2) peaks at 1.33, enough to get onto one voxel; falls integrate with the mean of the old and new speed, so jump height doesn't depend on frame rate.
- **Collision** (`collide.*`): solid are voxels, every cell at y >= 0 (always, with or without a ground plane: the player never falls below 0) and every cell with y < `CAM_Y_MIN` (-32, so the camera stays in the range the sort keys allow).
  - `collideBody` (asm) turns the body box into cells (16-bit halves of the 16.16 values, never a 32-bit shift) and tests them in a 8x8 column x 16 cell bit grid (`grid[]`, `collide.c`), rebuilt around the body by `collideBuild` (one pass over the records of at most 4 chunks, `collideMark` in asm) when a query leaves it or the window moved (`collideInvalidate`). It remembers the last free block of cells (`collide_free`): same cells again is free without a lookup, so most frames cost ~0.4k per query.
  - Response (`player.c`): a horizontal move in the open is one query for both axes; if blocked, each axis on its own in steps < 1 cell, stopping against the cell face (so the player slides). Vertical likewise, landing sets `on_ground`. Standing still on the ground makes no query. `collide_changed` (set by `playerSpawn` and `collideInvalidate`) lifts a player stuck inside a voxel (spawn on a voxel, runtime changes).
  - Cost (CEmu, test world): ~5k cycles per frame standing, ~20-25k walking, up to ~65k on a frame that rebuilds the grid (~170k in the dense world).
- **Snapping:** a released turn within `SNAP_RANGE` (8 steps) of N/E/S/W eases onto it exactly. Cardinal is the fast path (~30% faster per box). `setupCamera` samples the rotation at the step angle (`player_yaw` bits 16-23), so any yaw whose step is a multiple of 64 renders cardinally (`cam_cardinal`), fraction or not: the last frames of an easing are fast too. 0 disables it.
- 32-bit maths is acceptable once per frame, but each 32-bit multiply or shift is a ~1k cycle library call: read the halves of a 16.16 value with `CELL_OF`/`FRAC_OF` (`player.h`) or its bytes instead of shifting.

## Camera and projection

- Camera space: x right, y down, z forward. Yaw only, no pitch. A box at world (X, Y, Z) occupies [X-1, X]x[Y-1, Y]x[Z-1, Z].
- **Render distance 24** (`RENDER_DIST` in `gen_tables.py` = `GRID_RANGE`, the farthest far plane the tables allow): the 7x7 chunk window reaches 24 cells to each side from anywhere in the centre chunk (static-asserted in `chunks.h`). The far plane is a depth: the view reaches 1.25 x 24 = 30 cells to each side there, so the far corners of the view can lack the outer chunk ring (the 5x5 window at 16 had the same gap: 20 cells).
  - `render_dist` (`camera.c`, default `GRID_RANGE`) lowers it at runtime: `setupCamera` turns it into `cam_zspan` and `cam_far` (`cullChunks`). The table proof holds for any nearer far plane.
- **True 1/z perspective**: `screen = centre + 64 * a / z` (horizontal |x| <= 1.25 z, vertical |y| <= 0.94 z).
- **Units:** camera space is in 1/`SUB` (128) world units, and every value shared with `project.S` is stored **x3** (an `int24_t` table entry is 3 bytes, so a x3 value is a byte offset). Per-frame values must stay exact multiples of 3 (`fillRotTable` in `camera.S` builds them unscaled, then triples; it matches the old C version exactly over all 131k inputs).
- **Rotation, per frame** (`setupCamera`): per grid line within `GRID_RANGE` of the camera cell, tables of `(line - player) * cos/sin`, built by repeated addition, interleaved as `rotX_ = {XC, XS}`, `rotZ_ = {ZS, ZC}`. A grid point is `x' = XC - ZS`, `z' = XS + ZC`; the other three box corners are fixed per-frame offsets (`cam_k*3`, `cam_k*mid3`). Full list of per-frame state in `engine.h`.
- **Projection** (`projectRecords`): `64 * a / z = expTable[logTable[|a|] - logTable[z] - EXP_TABLE_MIN]` (logTable entries are x3 too, so the difference is a byte offset).
  - Per record, cheapest rejection first: exposed byte, x/z grid lookups and depth test, side tests, y lookup and above/below tests, then facing test. Most records are culled, so the order matters.
  - Grid index is checked in 16 bits (`GRID_INDEX`): record byte sign-extended plus the camera bias patched into an `ld de, nn`.
  - IX points at `.Lvars` (locals, loop state, per-frame camera scalars `cam_cos3` ... `cam_fog` written by `setupCamera`), addressed as `(ix + V_*)`. The `V_*` offsets are `#define`s (the assembler won't take a later label difference as a displacement); `.if` checks at the end of the file fail the assembly if they stop matching or the block exceeds 128 bytes. IY points at the record, then the rotation tables, then the box's `projected[]` slot.
  - **Self-modifying code:** camera biases are patched into the face test's `add a, n` and the grid index's `ld de, nn` once per call.
  - `face_x/y/z` are three consecutive 1024-aligned pages in `project.S`'s bss (`inc h` steps between them).
- **Culling** is in camera space before projecting: the farthest corner nearer than `NEAR_PLANE` or at/beyond the far plane, the box left/right of `|x| <= 1.25 z` (tested as 4x vs 5z), or above/below `|y| <= z`.
- **Near plane** `NEAR_PLANE` = 23/128 (0.18). The eye stays `BODY_RADIUS` (0.3) from walls, and a wall at distance d is at least d / 1.6 deep where the side of the view meets it, so a nearer plane never cuts a wall the player touches (static-asserted in `camera.c`). With the old 0.5 (and the box culled by its *nearest* corner) a wall the camera was pressed against vanished.
  - The part of a box nearer than `NEAR_PLANE` is clipped. Facing N/E/S/W the near face is moved to `NEAR_PLANE` (`LOG_ZC`): exact. At other yaws (`.Lgen_clip`, a few boxes per frame at most) each corner behind it is moved along a box edge to a corner in front, to depth `NEAR_PLANE`, preferring the edge of a visible side: `|dx| a / b` through the log/exp tables. Exact for the sides unless a corner between two visible sides is behind (one of them loses a sliver at the screen edge); a top or bottom may lose a corner triangle. Checked against a C model (exact division) over 300 random camera states, a third pressed against a cell face: 0 mismatches, at most 2 px.
- **The culling bounds every table index.** `gen_tables.py` asserts that any box passing these exact tests has all logTable/expTable indices in range (clipped points included: they lie in the box footprint at `NEAR_PLANE`), so `project.S` has no range checks, and projected coordinates stay within `SCREEN_COORD_MAX` (+-1221: 64 x the widest footprint over `NEAR_PLANE`). **If you change the culling, change the generator's proof with it.**
  - `CULL_X_NUM / CULL_X_DEN` must equal `VIEW_HALF_WIDTH / FOCAL_LENGTH`; `project.S` hard-codes 5/4 and `#error`s on change.
- **Level of detail.** A box whose nearest corner is at least `lod_rect_dist` deep (`LOD_RECT_DIST` = 12, at least 1) becomes `BOX_RECT`: the rectangle of its x/y extent at its centre depth (4 projections, one rectangle in `fill.S`). At or beyond the far plane it is off. A variable in `camera.c`, read every frame. (A 2x2 pixel level from 24 units was removed.)
  - The colour is a visible side if any: `.Llod_side` keeps only the x or z side pointing most at the camera *position* (larger of |dx|, |dz| by `gridDist`; z on a tie), so it doesn't change while turning. `lod_slot` is a fixed table (`sceneInit`). Voxels at y cell 0 are never LOD boxes.
- **Fog** (every build): a box whose centre is at least `fog_dist` deep (`FOG_DIST` = 16, a variable in `camera.c`; at or beyond `render_dist` turns it off; the `cam_fog` test in `project.S` is ~15 bytes, ~60 cycles, per kept box) gets material + `MAT_FOGGED` (16): `materials[16..31]` are each colour byte with one nibble replaced by the sky colour (a 50% dither; the z side takes the x side's shade). The ground between `fog_dist` and the far plane is dithered the same way and the ground beyond it is sky colour (`bg_rows`), so boxes crossing the far plane fade out. Depth, not distance, as the far plane is. Cost: two divisions per frame (~5k cycles), nothing measurable in the scene.
- **Regenerating tables:** edit the constants atop `utils/gen_tables.py` (`SUBUNITS`, `FOCAL_LENGTH`, `VIEW_HALF_WIDTH`, `RENDER_DIST`, `NEAR_PLANE`, `LOG_STEPS`...), run `python3 utils/gen_tables.py`, then `make clean`. `RENDER_DIST` must be at most `WINDOW_HALF * CHUNK_SIZE` (24); more needs a 9x9 window (81 chunks). `camera.S` hard-codes `GRID_RANGE` 24. The log table grows ~480 bytes per unit; a nearer `NEAR_PLANE` grows `recipTable` and `expTable` (wider projected coordinates) and needs a lower `LOG_ZERO`.

## Clipping conventions

- `projected_box_t.kind`: `BOX_AXIS` (step angle N/E/S/W: near and far faces are rectangles, 8 coordinates), `BOX_GEN` (other yaws: 4 vertical edges, 12 coordinates), `BOX_RECT` (LOD: `l r t b`, at least 1 px each way). Layouts in `engine.h`. `fill.S` turns both box layouts into the same 8 vertices via a face table per layout (`.Lfaces_axis` rebuilt per frame from `cam_axis_corner`).
- `fill.S` clips by construction (spans only inside open intervals, rows clipped to the viewport), so `project.S` does no inside/outside classification. A box may be partly or wholly off screen.
- If anything is ever drawn with graphx `_NoClip` calls: they take `uint8_t` y and `uint24_t` lengths, so negatives wrap, and the result must be proven against **all** 8 vertices first. Clipped versions take `int`.

## World and chunks

- **World file:** any AppVar whose data starts with `VXWD` (`worldScan`, up to 16; `make_world.py --appvar NAME --name "World name"`), read in place (flash if archived). Format version 3 (`world.h`, `make_world.py` docstring): 48-byte header (magic "VXWD", CRC-32, spawn, ground material...), chunk index (offset, count, run orientation), then 4-byte records {x + 8 z, y cell, material, exposed faces}: a chunk's voxels, then its runs. Up to ~16k voxels.
- **Ground plane:** with `ground` set, every cell at y >= 0 is solid ground, drawn as the background below the horizon (`ground_colour3`, read by `drawSpans`) for free. The file holds no ground-material voxels at y >= 0 and none below 0; other materials at y = 0 are drawn over it (top face only).
- **Exposed faces are in the file** (computed by `make_world.py`: ground solid, outside the world empty), so loading is a copy. `computeExposure` is for runtime changes and must agree with the generator (the bench checks the inner 3x3 chunks after each load).
- `worldOpen` (behind the loading screen; the CRC is ~2 s for 36 KB, in 512-byte blocks that advance the progress bar) checks header, CRC, every chunk offset and every voxel's range, so nothing later does. Bounds that matter: x + 8 z < 64, y in -96...95, material < 16, faces < 64. On error `worldStart` reports it through `displayError` (ends graphx, prints on the home screen, waits for a key) and `main` exits.
- **Switching worlds** (title, Play): `worldSelect` behind `displayReload`'s loading screen, then `coloursInit`, `windowInit`, `playerSpawn`; on error the open world stays and the list shows why. Only one world is open at a time.
- `worldClose(save)` (after `gfx_End`; `save` only if a world was played, not just shown behind the title) writes the RTC date as last played and updates the CRC; an archived world is unarchived and re-archived (needs free RAM the size of the world; may show the OS garbage collection screen).
- **Chunks:** columns of 8x8 cells, any height, at most 255 records (voxels and runs). RAM holds a 7x7 window: 49 slots of 1305 bytes (slot `(cx mod 7) * 7 + (cz mod 7)`, pointers in `slot[]`), `win[49]` by window position. Slots 0-19 are in bss, 20-48 in `DBL_BUF2` (VRAM). (Only the `-DFILL_REFERENCE` bench build has fewer in VRAM and the rest in a scratch AppVar `VXSCRTCH`.) Records are 5 bytes {x, Y, z, material, exposed}, x/z chunk local, Y = y cell + 1. A chunk's grid index bias is `cam_bias_x + 8 cx + 1`, passed to `projectRecords` and `sortRecords`.
- `windowAlloc` (before `worldStart`) sets the slot pointers, and in the reference build creates the AppVar (creating a variable may move the world AppVar if it is in RAM; on failure `displayError` prints "Not enough free RAM" and `main` exits); `windowFree` deletes any `VXSCRTCH` (also one left by an older version) after `gfx_End`, before `worldClose`. Harness `main`s (bench, collision test) call both.
- `updateWindow` copies the chunks new to the window (`copyVoxels`): all 49 at startup or a jump (~1.2M cycles in the dense world), 7 when crossing a chunk border (~90k in the test world).
- **Runtime world changes:** change the records, clear `chunk_t.sorted`, update `ymin`/`ymax`, call `computeExposure` for that window position and its 4 neighbours, then `collideInvalidate()` (the collision grid is built from the records). Not written back to the file. `computeExposure` uses an occupancy grid (chunk plus 1-cell border; layers at y >= 0 solid with a ground plane, cleared on every call) in `DBL_BUF1`, the span list's frame: **never call it between `projectScene` and `drawSpans`** (spans listed). A neighbour outside the window counts as empty (its faces point away from the camera).
- **Chunk culling** (`cullChunks`, C setup, per-chunk loop `cullLoop` in `occlude.S`): camera-space bounding box of each chunk's 4 corners against the same near/far/side tests as a box, with quarter-unit slack for table rounding. The four tests are linear in the window position (i, j), so `cullLoop` steps four running sums and keeps a chunk when the OR of their top bytes has no sign bit; it also leaves per-row and per-column corner tables, from which `chunkSeen` rebuilds a chunk's first corner. The setup reads the position as bytes (no 32-bit shifts). ~32k cycles per frame (a C loop was ~50k: clang unrolled it and turned the running sums into ~25 multiply calls).
- **Chunk occlusion** (`chunkSeen`, `occlude.S`): before a chunk is sorted and projected, the screen rectangle of its camera-space box (corner x/z extremes, records' y range, depths from its nearest corner to its farthest, clamped to the far plane, slack and a pixel each way) is tested against the rows still open (`fillSees`, the `ROW_SEES` test of the visibility trim). Covered: the chunk is skipped (and `chunk_visible` cleared). Conservative, so the output is unchanged (`-DBENCH_OCCLUSION_CHECK`: 0 px over 100 views); `occlusion_enabled = 0` turns it off. Projections through the log/exp tables, saturated beyond 5z/4 so every index is in range. ~3k cycles per chunk. Tried and dropped: the extremes of the 4 projected corners (catches ~50% more in random views, the same in the walk, but ~6k per test: net loss), a quick test of the top-centre pixel or the centre column first (no better). A hidden chunk costs ~70-100k (sort + projection + fill of hidden boxes); in the test world ~5% of the drawn chunks are caught, in the dense preset far more.
- **Memory:** bss and the malloc heap share 0xD052C6-0xD13FD8 (~60.4 KB); bss is at 39.7 KB (20 chunks 26.1, `projected[PROJECT_BATCH = 64]` 2.5, `fill.S` 4.2, rest ~7), ~21 KB left. The program runs from user RAM (23.3 KB code, 36.4 KB read-only data, mostly tables: log 12 KB, exp 14.6 KB, recip 7.3 KB).
  - **User RAM** (CEmu, clean calculator, HUD build running): 46 KB free (the release build creates no variable; the old 14.3 KB scratch AppVar is gone). A scratch AppVar's pointer (reference build) is invalid after any variable is created, deleted, resized or (un)archived (nothing does at runtime).
  - The dense preset (no ground plane, chunks at 255) reaches the 400-box cap in a few views (6 of 100), dropping the farthest; before chunk occlusion it was 83 of 100.

## Runs (merged faces)

- `make_world.py` merges, per chunk, the coplanar exposed side faces of one orientation (x or z faces, whichever saves more) into **runs**: records of 3 (`RUN_MIN`) to 8 (`RUN_MAX`, a chunk row) faces of one material in a horizontal row, one cell tall, not at y = 0. Record: `exposed = FACE_RUN_REC (0x80) + face`, `material + 16 (length - 1)`. A voxel with a face in a run gets `FACE_RUN` (0x40) and runs of one for its other exposed faces of that orientation; it leaves those faces to the runs. Full description: `chunks.h`, "runs".
- **Order:** a chunk with runs drawn is sorted along the run axis first: key `S |dx| + |dy| + |dz|` (each minus its least in the chunk, `S` = one more than the largest `|dy| + |dz|`, at most 32 for byte keys), and a run gets `S |dx|`, the start of its slab. Exact (argument in `chunks.h`). Key tables per sort in `sortTab` (`sortAxisKeys`/`sortScaleKeys` in `sort.S`).
- **When:** only chunks fewer than `run_dist` (`RUN_DIST` = 8) cells from the camera's, and runs stop at the level of detail distance (beyond it a voxel is a rectangle with all its faces). Other chunks sort plainly (gridDist keys, runs last with key 255, `nruns`/`mixed` in `chunk_t`) and project only their voxels, and `projectRecords` patches its run checks out (`.Lra`, `.Lmb`, `.Lft`). `runs_enabled = 0` turns all runs off (clear `sorted`).
- **Projection** (`project.S`, `.Lrun`): the four culling tests of a box are linear along the run, so they are evaluated at both end cells (`.Lr_vals`) and only stepped (`.Lr_test`) when a run crosses an edge of the view. Points by addition from the first (exact: the rotation tables are additive). A point behind the near plane is moved along the run to it (in place); at cardinal yaws an all-behind run is drawn clamped like a box's near face. Fog split per cell (two pieces). Output `BOX_RUN`: faces = face + 16 P, P points {x, top, bottom}, 1-3 slots (`RUN_SLOT_POINTS`, `RUN_SLOT2_POINTS`).
- **Drawing** (`fill.S` `.Lrun_box`/`.Lface_run`, `fill_ref.c` `fillRun`): one polygon of 2 P vertices whose top and bottom edges pass through every grid point, so its edges are the ones the neighbouring faces use: no T-junction cracks.
- **Checks:** `-DBENCH_RUNS_CHECK` draws each view with and without runs and counts differing pixels (0 over 100 random views, LOD on and off, and the close-up scenes); `-DBENCH_SCENES` is a list of close-up views (walls, house, hill); `-DBENCH_RUN_DIST=d`. `computeExposure` turns a chunk's runs off (runtime changes).
- **Gains** (k-cycles, frame = scene; before -> after): close-up scenes 92,466 -> 88,796 (-4%), walk 236,265 -> 223,099 (-5.6%), 100 random views 251,914 -> 249,836 (-0.8%); fill -10..-19%, projection +6..+12%. Small-object worlds lose a little: random boxes 274,806 -> 283,591 (+3%), dense 641,344 -> 649,229 (+1%). Runs cost about 3k cycles to project, a 3-face run saves about that much in `fill.S`.

## Painter's order

- Boxes are drawn **near to far** by **Manhattan distance in grid cells** from the camera's cell. For unit voxels this is exact: if A hides part of B, every coordinate is monotonic along the sight line, so A's cell lies between the camera's and B's on each axis and A's distance is strictly smaller. Camera depth (z) is *not* a valid key.
- **Across chunks** the same argument holds: `projectScene` visits chunks by Manhattan chunk distance (`visit_order`) and each chunk's boxes by cell distance; the concatenation is exact. `fillBoxes` gets the batches in that order and walks each forwards. A chunk whose output does not fit the batch is resumed where `projectRecords` stopped (`project_left`; a run that did not fit is done again, harmlessly: a piece drawn twice is hidden by itself). Near to far means the 400-box cap drops the farthest boxes.
- **A chunk's order changes only when the camera's cell changes on an axis where the camera is inside the chunk's range** (x, z: its 8 cells; y: its records' range). `sortChunk` stores that relation per axis (`sortRel`) and re-sorts only when it changes, only for chunks about to be projected.
  - This needs every key exact, culled boxes included: `gridDist` is |d| of the index's low byte as a signed byte (exact to +-127), and `chunks.h` asserts the window (|dx|, |dz| <= 31) and y range (camera y cell in `CAM_Y_MIN` (-32) ... 0, kept by the collision ceiling) keep keys exact and under 256.
- `sortRecords` (`sort.S`): stable counting sort on byte keys through a scratch copy: key and count, prefix, place, `ldir` back. ~480 cycles per record.
  - `gridDist`, the counts and the start indices each begin on a 256-byte boundary (`.balign 256`) so `ld e,a` / `ld l,a` index them without an add.
  - **Self-modifying code:** the three bias bytes and the running maximum key are patched into `add a,n` / `cp a,n`.
  - The prefix pass visits only keys 0...max and zeroes each used count as it reads it: counts must be zero on entry, and are on exit.
  - Key per axis is `gridDist[(uint8_t)(X + bias)]`; range-checking the 24-bit index instead was ~35% slower.

## Filled rendering (`fill.S`)

- **Front to back with per-row coverage.** Each viewport row keeps a list of uncovered x intervals (at most `GAP_MAX` = 11, a 32-byte block per row), **sorted left to right and ended by sentinel** `[255, 255]`. A face (convex quad) covers `[xl, xr)` per row; only the parts inside open intervals become spans and are removed from the intervals, so hidden pixels are never written and once every row is closed farther boxes aren't looked at.
  - One-interval rows have their own path (four cases, no loop). With several, a face skips intervals left of it and stops at the first one right of it; a split moves the rest up (`lddr`), a closed interval moves them down (`ldir`).
  - **Visibility trim** (`.Lrows_find`, macro `ROW_SEES`): before any edge is computed, a face's rows are trimmed at both ends to rows with an open interval overlapping its x extent `[min x, max x]` of its vertices (every span lies inside it: an edge never steps past its end vertex). None: the face is skipped. `BOX_RECT`s use it too. Output is unchanged; in the test world about a third of the faces that reached edge setup were entirely hidden (75% in the dense preset). The same test on a whole box first (its 8-vertex extent) was tried and dropped: ~900 cycles per box, 6-8% slower `fillScene` in the test world, because hidden boxes are already cheap with the per-face test.
- **Rows, not columns:** floor tiles are much wider than tall on screen and cost is per line of a face. Tall narrow faces (tree trunk) are the bad case.
- **Faces to draw** = facing the camera (sign of box cell minus camera cell per axis, via `face_x/y/z`) **and exposed** (5th record byte). An enclosed voxel isn't even projected.
- **Face tables:** a box's coordinates are copied into `.Lpage` (`ldir`, 36 bytes); a face is a 16-byte entry of its 4 vertices clockwise, twice over (so a side never wraps), as page offsets of x and y. The right side runs forwards from the top vertex, the left backwards from its copy four entries on; a side stops at the first vertex at or below the last row.
- A `BOX_RECT` writes the same `[xl, xr)` into all its rows through the same row loop: no edges, no face setup.
- **Shading:** `materials[16]` in `colours.c` gives each material a top, x-side, z-side (dithered) and bottom colour byte. Records hold a material index.
- **Edges** step x per row with 16 fractional bits; slopes come from `recipTable` (`round(2^24 / dy)`) and `mlt`, none for vertical or one-row edges. Shared edges are computed from the same two vertices in the same direction, so faces meet without cracks. Only an edge with an end outside x 0..255, in a box with one, runs the clamping loop.
- **Near faces at cardinal yaws** (`.Lface_sel`): the near face of a `BOX_AXIS` box is an exact rectangle (x `[nl, nr)`, y `[nt, nb)`), so it takes the rectangle row path, skipping the vertex scans and edge setup (walk: `fillScene` -4.5%). It must stay in its place in the face order: with an overflowing row (`GAP_MAX`) the order changes which part is dropped (moving it first gave 7 px against `fill_ref.c`).
- **Two phases.** `fillBegin`/`fillBoxes` (before the wait) only writes span records with `push` while SP points into the span list. `drawSpans` (after the wait) fills the background with `push` row by row in rows still open (a closed row is entirely covered), then pops the records. A short span (< `LONG_MIN` = 28 px, 6 bytes) jumps into a 256-aligned unrolled `ld (hl),a / inc hl` loop; a long one (9 bytes, colour x3) goes through a stub writing `len % 3` pixels, then an unrolled `push bc` run (`.Llong_tab` gives stub and entry by length). Both phases run with interrupts disabled and restore the previous state.
- **Self-modifying code:** the face colour is patched into the row loop's `ld c, n`.
- **Overflow:** a row with more than `GAP_MAX` holes drops the rightmost part of the split interval and is marked (block byte +3) so it still gets the background once closed. A full span list is drawn early (`waitForBufferScan` + `drawSpans`): the phase split is lost for that frame, nothing breaks.
- **Memory safety:** spans only lie inside open intervals, intervals only shrink from `[0, VIEW_W)`, rows are clipped to `[0, VIEW_H)`. Keep it so: it is what makes arbitrary projected coordinates safe. A split needs `n < GAP_MAX`, so the sentinel never passes byte 27 (the row address is at 29).
- **Verification:** `fill_ref.c` is the same algorithm in C and must draw exactly the same frame. `utils/bench/run_bench.sh` builds `bench.c` against a copy of `src/` and runs it in CEmu with a world from `make_world.py` (`WORLD=dense` for the no-ground preset; `-DBENCH_WALK` for a straight walk). It prints cycle totals per phase and checks chunk draw order, `computeExposure` against the file, that every LCD row pair matches after `deinterlaceFrame` (`deint bad`), and with `-DFILL_REFERENCE` both renderers into two VRAM frames with differing bytes counted (results on the last screenshot; the screen is 160 px wide, so print averages, not totals, in the right column). **Rerun it after changing `fill.S`, `fill_ref.c` or `project.S`, and change `fill.S` and `fill_ref.c` together.** Sort errors hit both renderers alike, hence the separate sort check.
- **Costs** (cycles, approximate): ~1.1k setup per edge plus 46 per row, ~1.4k per face, ~500 per face-row, ~50 per short span, 10.4 per pixel in `drawSpans`.
- **If it needs to be faster:** share projected corners between neighbouring boxes (per-frame cache by grid point); step both sides inside the row loop instead of storing x per row; compute edges shared between faces of a box once; write short spans to both LCD rows too (only background rows skip the de-interlace copy now).

## eZ80 performance notes

- **The CE is memory bound: ~4 cycles per byte fetched or read (opcode and immediate bytes included), ~2 per byte written.** Routines cost about "bytes x 4". Shorter instructions win: `ld a,(nn)` costs a third more than `ld a,(ix+d)`; `call`/`ret` moves 8 bytes. Estimate a routine by its bytes before writing it.
- **clang almost never emits `mlt`.** Library-call costs (cycles): 8x8 multiply ~180, 16-bit ~240, 24-bit ~380 (even by constants), 32-bit ~680, divide 1,700-2,000, right shift ~250 (even `>> 8`), `& 0xff` a library call, `abs()` / `x < 0 ? -x : x` a `__icmpzero` + `__ineg` call.
- Cheap: additions, table lookups, compares (signed ones compile inline), left shifts (`add hl,hl`), powers of 2. `ldir` (~7.7-8 cycles/byte) is the cheapest copy; an unrolled `push` fill (interrupts off) is ~3.8 per byte.
- `mlt rr` clears the upper byte of the 24-bit register (`ld h, a / ld l, n / mlt hl / add hl, de` relies on it).
- **Compiler bug (ez80-clang 22.1.8):** a chain of compares turned into a 0/1 value (`a && b && c` stored, not branched on) can emit `sbc a, l` instead of `sbc a, a`. Write such tests as `if`s, and before trusting a safety check grep `obj/lto.s` for `sbc\ta, [bcdehl]$` (should find nothing).
- **Numeric local labels** (`1:` / `jp 2f`) resolve to the nearest one in that direction, across macros and routines. When renaming or removing one, grep for every `Nf`/`Nb` that used it: a leftover `jp p, 2f` once jumped into unrelated code and froze the game after a few blocks of walking.
- **Hot per-object code belongs in assembly** (`src/*.S`); per-frame code can stay in C. ABI: arguments at `sp+3`, `sp+6`, `sp+9` (3 bytes each, even for `char`); 8-bit result in `A`, 24-bit in `HL`. Preserve `IX`; `AF BC DE HL IY` may be clobbered. Inline `__asm__` works (`"+l"` = HL), but `.S` files are easier to reason about.
- **Typical frame cost** (test world with ground plane, 128-view bench walk, ~129 boxes, render distance 24): ~1.98M cycles, of which `fillScene` 45%, `projectScene` 39% (sorts and `cullChunks` included), `drawSpans` 10%, de-interlace 5%; `setupCamera` is 23k, the player and collision 5-25k (not in the bench), the HUD ~60k. At render distance 16 it was ~1.42M (~64 boxes).
- **Batching and chunk occlusion pass** (November 2026, scene k-cycles, HEAD before -> after): 100 random views 249.8M -> 243.4M (-2.6%), 128-view walk 223.1M -> 216.7M (-2.9%), close-up scenes 221.4M -> 202.5M (-8.5%), dense 649.2M -> 601.1M (-7.4%, and the 400-box cap now rarely hit). Memory: bss 58.2 -> 39.7 KB, no 14.3 KB scratch AppVar. From: projecting and filling chunk by chunk (64-box batches), `chunkSeen`, `cullLoop` in asm, near faces as rectangles, the occupancy grid moved to `DBL_BUF1` (29 chunk slots in `DBL_BUF2`). Bench: `occluded`/`occ k` lines (chunks skipped, cycles of the tests), `-DBENCH_OCCLUSION_CHECK`, `-DBENCH_NO_OCCLUSION`; `full` now counts views that stopped early (all covered or the cap).
- **Bench history** (k-cycles; frame = scene + `setupCamera` + de-interlace), before/after the November 2026 optimization pass: 100 random views 207.2M -> 198.8M (-4%), 128-view walk 186.9M -> 182.2M (-2.5%), dense 100 views ~782M -> ~695M (-11%, `fillScene` -25%). Gains: face visibility trim (`fill.S`), background rows written to both LCD rows, `setupCamera`/`cullChunks` without library calls, `fillRotTable` in asm, shorter row reset in `fillScene`.
- **Render distance 16 -> 24** (with the near-plane fix and the 7x7 window), scene only: 100 random views 185.7M -> 251.9M (+36%; boxes per view 82 -> 154, worst view 3.28M -> 3.90M), 128-view walk frame 182.2M -> 252.8M (+39%), dense ~695M -> 659M (the 400-box cap). By `render_dist` (100 views, old 0.5 near plane): 16: 205.7M, 20: 233.4M, 24: 252.9M. Even at 16 the new code costs +11% over the old: boxes pressed against or under the camera are now drawn, and the outer chunk ring fills the sides of the view the 5x5 window lacked. Fog: same scene cost.

## Open issues

- Near-plane clipping at non-cardinal yaws moves whole corners (one point per vertical edge): a side next to a clipped corner between two visible sides loses a sliver at the screen edge, and a top or bottom face may lose a corner triangle.
- The ground is a flat colour below the horizon, not geometry.
- Big near objects made of many 1x1 faces (hill, house) are the slow views. Runs merge one side orientation per chunk, one cell tall; tops (y faces), the other orientation and vertical merging are not merged (the sort order allows only one axis per chunk).
- Worlds of small objects (`--preset random`, dense) are 1-3% slower than without runs: the per-record checks and sort table setup cost more than their few short runs save.
- The read-only tables are ~34 KB of the program.
- Runtime world changes are not written back to the file.

`readme.md` has the TODO list and changelog. `docs/` holds the README's demo recordings (APNG). `gfx/` holds convimg palette output; only `src/` is compiled, and `palette` in `colours.c` is the one used. Branches: `main` is current; `debug` has per-object timing calls; `rewrite` is the pre-filled-rendering rewrite.
