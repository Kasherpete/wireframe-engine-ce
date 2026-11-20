// The camera: per-frame setup of the state project.S reads (engine.h).
//
// Camera space: x right, y down, z forward, in 1/SUB world units. setupCamera() builds the
// per-frame tables and scalars once per frame (C is fine there). The per-box work is in project.S,
// because clang turns every multiply, right shift and even AND into a library call.
#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>
#include "engine.h"

// the camera's cell, and the sin and cos of its yaw (x SUB). Written by setupCamera
extern int24_t cam_cell_x, cam_cell_y, cam_cell_z;
extern int24_t cam_sin, cam_cos;

// Variables, so a game can change them at runtime (e.g. a quality setting): setupCamera reads them
// every frame.
// Level of detail: world units from which a box becomes a rectangle (engine.h LOD_RECT_DIST). At
// or beyond the far plane it is off.
extern uint8_t lod_rect_dist;
// The far plane in world units, 1 .. GRID_RANGE (default GRID_RANGE, the most the tables and the
// chunk window allow). Lower is faster.
extern uint8_t render_dist;
// Distance fog (make fog, FOG_BUILD): boxes whose centre is at least fog_dist deep are drawn half
// dithered with the sky colour, and so is the ground between fog_dist and the far plane; beyond
// the far plane the ground is sky colour. At or beyond the far plane: no fog. Without FOG_BUILD it
// is ignored (no fog)
extern uint8_t fog_dist;

// the face tables project.S reads: once at startup
void cameraInit(void);

// once per frame, after the player moved: the camera at the player's eye (player.h)
void setupCamera(void);

#endif
