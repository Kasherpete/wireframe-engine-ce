// The scene: which chunks are in view, and projecting their voxels for the rasterizer (fill.h).
#ifndef SCENE_H
#define SCENE_H

#include <stdint.h>
#include "engine.h"
#include "chunks.h"

#define MAX_PROJECTED 400            // boxes projected per frame (the farthest are dropped)

// this frame's projected boxes, near to far: the input of fillScene
extern projected_box_t projected[MAX_PROJECTED];
extern uint24_t num_projected;

// per window position: whether the chunk can be in view (cullChunks). And how many visit_order
// entries projectScene went through (the bench checks those chunks' order)
extern uint8_t chunk_visible[WINDOW_N];
extern uint8_t chunks_done;

// the level of detail colour table (lod_slot, fill.h): once at startup
void sceneInit(void);

// Culls and projects every visible chunk into projected[], without drawing. After setupCamera
// and windowFollow. Touches no frame buffer, so main() runs it before waitForBufferScan
void projectScene(void);

#endif
