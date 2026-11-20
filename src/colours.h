// The palette and the face colours of each material.
#ifndef COLOURS_H
#define COLOURS_H

#include <stdint.h>
#include "engine.h"

// 16 colours (PICO-8 style), 2 bytes each in the LCD's 1555 format: gfx_SetPalette loads it
extern const uint8_t palette[32];

// {top, x side, z side, bottom} colour bytes per material, then the same fogged: declared in
// engine.h (fill.S reads it)
// extern const uint8_t materials[2 * MAT_COUNT][4];

// The colour byte of the ground below the horizon, 3 times (fill.S reads it): the top colour of
// the world's ground material, or GROUND_COLOUR without a ground plane. And the same half fogged
// (make fog). Set by coloursInit. The background rows: fill.h
extern uint24_t ground_colour3, bg_dither3;

// after worldOpen: the ground colour of the open world
void coloursInit(void);

#endif
