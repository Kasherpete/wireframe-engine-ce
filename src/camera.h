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
// The render distance in world units, 1 .. GRID_RANGE (default GRID_RANGE, the most the tables and
// the chunk window allow): a circle around the camera's cell (radTab, engine.h), and the far plane
// (the circle lies inside it). Lower is faster.
extern uint8_t render_dist;
// Distance fog: boxes whose cell is at least fog_dist from the camera's are drawn half dithered with
// the sky colour (radTab, engine.h: a circle, the same at every yaw), and so is the ground between
// fog_dist and the far plane (by depth: bands of rows); beyond the far plane the ground is sky
// colour. At or beyond render_dist: no fog
extern uint8_t fog_dist;

// the face tables project.S reads: once at startup
void cameraInit(void);

// once per frame, after the player moved: the camera at the player's eye (player.h)
void setupCamera(void);

#endif
