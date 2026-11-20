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

// Level of detail: world units from which a box becomes a rectangle, then a pixel (engine.h
// LOD_*). Variables, so a game can change them at runtime (e.g. a quality setting): setupCamera
// reads them every frame. At or beyond the far plane (GRID_RANGE) a level is off
extern uint8_t lod_rect_dist, lod_pixel_dist;

// the face tables project.S reads: once at startup
void cameraInit(void);

// once per frame, after the player moved: the camera at the player's eye (player.h)
void setupCamera(void);

#endif
