// The scene: which chunks are in view, and projecting their voxels for the rasterizer (fill.h).
#ifndef SCENE_H
#define SCENE_H

#include <stdint.h>
#include "engine.h"
#include "chunks.h"

#define PROJECT_BATCH 64             // boxes projected before they are filled (6 .. 255: a run fits)
#define MAX_PROJECTED 400            // boxes projected per frame (the farthest are dropped)

// the batch of projected boxes projectScene passes to fillBoxes, and how many boxes it projected
// this frame in all
extern projected_box_t projected[PROJECT_BATCH];
extern uint24_t num_projected;
#ifdef FILL_REFERENCE
// the bench: every box of the frame, for fillSceneRef
#define PROJECTED_LOG MAX_PROJECTED
extern projected_box_t projected_log[PROJECTED_LOG];
extern uint24_t num_logged;
#endif

// per window position: whether the chunk can be in view (cullChunks; projectScene clears it for a
// chunk it found hidden behind nearer ones). And how many visit_order entries projectScene went
// through (the bench checks the order of those chunks still marked visible)
extern uint8_t chunk_visible[WINDOW_N];
extern uint8_t chunks_done;
// non-zero (default): skip chunks whose screen bounds are covered by nearer ones (the bench
// compares with 0)
extern uint8_t occlusion_enabled;

// the level of detail colour table (lod_slot, fill.h): once at startup
void sceneInit(void);

// Culls and projects every visible chunk and lists the spans of the frame drawn at base
// (fillBegin, fillBoxes), without drawing. After setupCamera and windowFollow. Touches no frame
// buffer (unless the span list fills up, fill.h), so main() runs it before waitForBufferScan
void projectScene(uint24_t base);

#endif
