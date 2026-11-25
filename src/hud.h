// The on-screen HUD (FPS and frame times), shown while hud_shown is set (the title's settings
// toggle it; on by default in "make hud" builds), and the cycle counters behind it. The per-phase
// profiling counters (PROF()) are only in "make hud" builds (SHOW_HUD); otherwise PROF() is empty
// and compiles away. The text costs about 60k cycles, 1.2 ms, per frame while shown.
#ifndef HUD_H
#define HUD_H

#include <stdint.h>
#include <sys/timers.h>

// Timer 2 counts CPU cycles (48 MHz) and is left free-running (hudInit). Read the low 24 bits
// directly: timer_Get() hangs with TIMER_CPU. A difference of two reads is right across the 24-bit
// wrap as long as the interval is under 2^24 cycles (349 ms)
#define CYCLES()   (*(volatile uint24_t *)TIMER_COUNT_ADDR(2))

// the rows the HUD draws into: deinterlaceFrame copies them while it is shown
#define HUD_ROWS   28

extern uint8_t hud_shown;

// the font and the cycle counter
void hudInit(void);
// FPS, D = this frame's scene work, F = the previous frame's total excluding the wait for the LCD
void hudDraw(uint8_t fps, uint24_t draw_cycles, uint24_t frame_cycles);

#ifdef SHOW_HUD
#define PROF(...)  __VA_ARGS__

// per-phase totals since startup (the bench reads and resets them)
extern uint24_t prof_sorts, prof_loads, prof_chunks, prof_records, prof_occluded;
extern uint32_t prof_sort_cycles, prof_load_cycles, prof_cull_cycles, prof_proj_cycles, prof_fill_cycles, prof_occ_cycles;
#else
#define PROF(...)
#endif

#endif
