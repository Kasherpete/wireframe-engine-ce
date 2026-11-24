// The on-screen HUD and the cycle counters behind it: only in "make hud" builds (SHOW_HUD).
// Otherwise CYCLES() is 0, PROF() is empty and the HUD functions are empty, so all of it compiles
// away (the text alone costs about 60k cycles, 1.2 ms, per frame).
#ifndef HUD_H
#define HUD_H

#include <stdint.h>

#ifdef SHOW_HUD
#include <sys/timers.h>

// Timer 2 counts CPU cycles (48 MHz) and is left free-running. Read the low 24 bits directly:
// timer_Get() hangs with TIMER_CPU. A difference of two reads is right across the 24-bit wrap
// as long as the interval is under 2^24 cycles (349 ms)
#define CYCLES()   (*(volatile uint24_t *)TIMER_COUNT_ADDR(2))
#define PROF(...)  __VA_ARGS__

// per-phase totals since startup (the bench reads and resets them)
extern uint24_t prof_sorts, prof_loads, prof_chunks, prof_records, prof_occluded;
extern uint32_t prof_sort_cycles, prof_load_cycles, prof_cull_cycles, prof_proj_cycles, prof_fill_cycles, prof_occ_cycles;

// the rows the HUD draws into: deinterlaceFrame always copies them
#define HUD_ROWS   28

// the font and the cycle counter
void hudInit(void);
// FPS, D = this frame's scene work, F = the previous frame's total excluding the wait for the LCD
void hudDraw(uint8_t fps, uint24_t draw_cycles, uint24_t frame_cycles);

#else

#define CYCLES()   0
#define PROF(...)
#define HUD_ROWS   0

static inline void hudInit(void) {}
static inline void hudDraw(uint8_t fps, uint24_t draw_cycles, uint24_t frame_cycles) {
    (void)fps;
    (void)draw_cycles;
    (void)frame_cycles;
}

#endif

#endif
