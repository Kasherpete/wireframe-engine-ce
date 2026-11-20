// The player (player.h). Per-frame code in C; the collision lookups are in collide.S. Per-frame
// still costs: a 32-bit multiply or shift is a ~1k cycle library call, so the rates below are
// shifts and adds of dt.
#include <keypadc.h>
#include <time.h>

#include "player.h"
#include "collide.h"
#include "engine.h"
#include "world.h"

int32_t  player_x = 0;
int32_t  player_y = -STAND_HEIGHT;
int32_t  player_z = 0;
uint24_t player_yaw = 0;

// Speeds are per second and scaled by the frame time, so they do not change with FPS. They ease
// towards what the keys ask for
static int24_t walk_vel   = 0;   // 1/256 units per second, along the view direction
static int24_t strafe_vel = 0;   // 1/256 units per second, to the right of the view direction
static int24_t turn_vel   = 0;   // 1/256 steps per second
static uint8_t snapping   = 0;   // easing into the nearest N/E/S/W
static int32_t feet_y     = 0;   // 1/65536 units: the bottom of the body
static int24_t fall_vel   = 0;   // 1/256 units per second, downwards
static uint8_t on_ground  = 0;
static int24_t eye_height = STAND_HEIGHT;  // 1/65536 units: eases between STAND_ and CROUCH_HEIGHT

#define WALK_SPEED   (4 * 256)        // units/s, in 1/256 units (forwards/backwards and strafing)
#define WALK_ACCEL   (32 * 256)       // units/s^2: full speed (and a full stop) in 1/8 s
#define TURN_SPEED   (96 * 256)       // steps/s (96 = 135 deg/s), in 1/256 steps
#define TURN_ACCEL   (768L * 256)     // steps/s^2: full speed in 1/8 s
#define SNAP_RANGE   (8L << 16)       // after a turn, ease into the nearest N/E/S/W if within 8 steps (11 deg). 0 disables
#define SNAP_START   (16 * 256)       // ... once the turn has slowed below 16 steps/s
#define SNAP_GAIN    10               // snap speed = 10 * remaining angle per second (time constant 0.1 s)
#define SNAP_DONE    (1L << 13)       // finish exactly once within 1/8 step
#define JUMP_SPEED   (8 * 256)        // units/s upwards at take-off: a jump peaks 1.33 units up after 1/3 s
#define GRAVITY      (24L * 256)      // units/s^2
#define MAX_FALL     (24 * 256)       // units/s
#define CROUCH_SPEED (6L * 256)       // units/s: crouching or standing up takes 1/8 s
#define MAX_DT       (CLOCKS_PER_SEC / 8)   // main() clamps the frame time to this
#define STEP_MAX     (1L << 15)       // collision steps of at most half a unit: no cell is skipped
#define ONE          (1L << 16)
#define CELL_FLOOR(v) ((v) & ~(ONE - 1))   // the start of the cell v is in

// Per-frame steps are rate * dt / CLOCKS_PER_SEC. With these rates that is a few shifts and adds
// (a 32-bit multiply and shift would be library calls, about 1k cycles each)
_Static_assert(CLOCKS_PER_SEC == 1L << 15, "per second: >> 15");
_Static_assert(WALK_ACCEL == 1L << 13, "walk_step = dt / 4");
_Static_assert(CROUCH_SPEED == 12L << 7, "crouch step = 12 dt");
_Static_assert(GRAVITY == 3L << 11, "gravity step = 3 dt / 16");
_Static_assert(TURN_ACCEL == 6L << 15, "turn step = 6 dt");
_Static_assert(WALK_SPEED <= 1 << 10 && MAX_DT <= 1 << 12 && SUB == 128, "walking maths fits 24 bits");
_Static_assert((int32_t)MAX_DT * TURN_SPEED < 0x7fffffff / 2, "per-frame turn step overflows");
_Static_assert(STEP_MAX < ONE, "a collision step enters at most one new cell per axis");
_Static_assert(BODY_RADIUS < ONE / 2 && STAND_HEIGHT + HEAD_ROOM < 2 * ONE, "collideBody: at most 2 x 3 x 2 cells");

// what updateMotion is asked to do (playerUpdate reads the keys)
#define MOVE_FORWARD     0x01   // [up]
#define MOVE_BACK        0x02   // [down]
#define MOVE_TURN_LEFT   0x04   // [left]
#define MOVE_TURN_RIGHT  0x08   // [right]
#define MOVE_LEFT        0x10   // [mode]: strafe
#define MOVE_RIGHT       0x20   // [del]: strafe
#define MOVE_JUMP        0x40   // [2nd]: held, it jumps again on landing
#define MOVE_CROUCH      0x80   // [alpha], while held


void playerSpawn(void) {
    player_x = ((int32_t)world->spawn_x << 16) + (ONE >> 1);
    player_z = ((int32_t)world->spawn_z << 16) + (ONE >> 1);
    player_yaw = (uint24_t)world->spawn_yaw << 16;
    feet_y = 0;                 // playerUpdate lifts the player out of anything standing there
    walk_vel = strafe_vel = turn_vel = fall_vel = 0;
    snapping = on_ground = 0;
    eye_height = STAND_HEIGHT;
    player_y = feet_y - eye_height;
    collide_changed = 1;        // look for anything overlapping the player
}


// ---- collision ----

// whether the body at the current position overlaps anything solid
static uint8_t blocked(void) {
    return collideBody(&player_x, &player_z, &feet_y, eye_height + HEAD_ROOM);
}

// Moves the player by d along x or z (*p is player_x or player_z). On hitting something it stops
// against the face of the cell it ran into, and returns 1
static uint8_t moveAxis(int32_t *p, int32_t d) {
    while (d != 0) {
        const int32_t step = d > STEP_MAX ? STEP_MAX : d < -STEP_MAX ? -STEP_MAX : d;
        const int32_t old = *p;
        d -= step;
        *p += step;
        if (blocked()) {
            if (step > 0) {
                *p = CELL_FLOOR(*p + BODY_RADIUS - 1) - BODY_RADIUS;
            } else {
                *p = CELL_FLOOR(*p - BODY_RADIUS) + ONE + BODY_RADIUS;
            }
            if (blocked()) {
                *p = old;       // (only if it was stuck already)
            }
            return 1;
        }
    }
    return 0;
}

// Moves the feet down by d (up for d < 0). Landing on something sets on_ground; hitting the
// ground or a ceiling stops the fall or the jump
static void moveVertical(int32_t d) {
    on_ground = 0;
    while (d != 0) {
        const int32_t step = d > STEP_MAX ? STEP_MAX : d < -STEP_MAX ? -STEP_MAX : d;
        const int32_t old = feet_y;
        d -= step;
        feet_y += step;
        if (blocked()) {
            if (step > 0) {
                feet_y = CELL_FLOOR(feet_y - 1);    // standing on the cell below
                on_ground = 1;
            } else {
                const int32_t height = eye_height + HEAD_ROOM;
                feet_y = CELL_FLOOR(feet_y - height) + ONE + height;   // head against the cell above
            }
            if (blocked()) {
                feet_y = old;
            }
            fall_vel = 0;
            return;
        }
    }
}


// ---- motion ----

// moves v towards target by at most step
static int24_t approach(int24_t v, int24_t target, int24_t step) {
    if (v < target) {
        return (target - v < step) ? target : v + step;
    }
    return (v - target < step) ? target : v - step;
}

// keys is a set of MOVE_* flags. dt is in clock ticks
static void updateMotion(uint8_t keys, uint24_t dt) {

    // The world changed (or the player was placed): if the body is inside something, lift it out
    // of it, and let it fall if the ground went away. Otherwise standing still needs no query
    const uint8_t changed = collide_changed;
    if (changed) {
        collide_changed = 0;
        for (uint8_t i = 0; i < 2 * WORLD_Y_MAX && blocked(); i++) {
            feet_y -= ONE;
            fall_vel = 0;
        }
    }

    // ---- walking and strafing ----
    const int24_t walk_step = (int24_t)(dt >> 2);

    int24_t walk_target = 0;
    if (keys & MOVE_FORWARD) {
        walk_target = WALK_SPEED;
    } else if (keys & MOVE_BACK) {
        walk_target = -WALK_SPEED;
    }
    walk_vel = approach(walk_vel, walk_target, walk_step);

    int24_t strafe_target = 0;
    if (keys & MOVE_RIGHT) {
        strafe_target = WALK_SPEED;
    } else if (keys & MOVE_LEFT) {
        strafe_target = -WALK_SPEED;
    }
    strafe_vel = approach(strafe_vel, strafe_target, walk_step);

    uint8_t moved = 0;
    if (walk_vel != 0 || strafe_vel != 0) {
        // 1/65536 units: vel/256 * dt/32768 * 65536. Strafing is 90 degrees right of the view
        // in 24 bits: |vel| dt <= 2^10 2^12, and |walk s| + |strafe c| <= 2^15 181 (|s| + |c| <= 181)
        const uint8_t angle = ((const uint8_t *)&player_yaw)[2];
        const int24_t walk = (walk_vel * (int24_t)dt) >> 7;
        const int24_t strafe = (strafe_vel * (int24_t)dt) >> 7;
        const int24_t s = sinTable[angle], c = sinTable[(uint8_t)(angle + 64)];
        // x along (sin, cos) for walking, (cos, -sin) for strafing; sinTable is x SUB = 128
        const int24_t dx = (walk * s + strafe * c) >> 7, dz = (walk * c - strafe * s) >> 7;
        // In the open (the usual case) one query for both axes. Against something, each axis on
        // its own, so the player slides along it
        uint8_t done = 0;
        if (dx <= STEP_MAX && dx >= -STEP_MAX && dz <= STEP_MAX && dz >= -STEP_MAX) {
            player_x += dx;
            player_z += dz;
            if (blocked()) {
                player_x -= dx;
                player_z -= dz;
            } else {
                done = 1;
            }
        }
        if (!done) {
            moveAxis(&player_x, dx);
            moveAxis(&player_z, dz);
        }
        moved = 1;
    }

    // ---- crouching: the body shrinks from the top, and only grows back where there is room ----
    const int24_t old_eye = eye_height;
    eye_height = approach(eye_height, (keys & MOVE_CROUCH) ? CROUCH_HEIGHT : STAND_HEIGHT,
                          (int24_t)((dt << 3) + (dt << 2)));
    if (eye_height > old_eye && blocked()) {
        eye_height = old_eye;
    }

    // ---- jumping and falling. Standing still on the ground, nothing can change ----
    if (on_ground && (keys & MOVE_JUMP)) {
        fall_vel = -JUMP_SPEED;
        on_ground = 0;
    }
    if (!on_ground || moved || changed) {
        // the mean of the speeds before and after: exact under constant gravity, so the height of
        // a jump does not depend on the frame rate
        const int24_t v0 = fall_vel;
        fall_vel += (int24_t)((dt + dt + dt) >> 4);
        if (fall_vel > MAX_FALL) {
            fall_vel = MAX_FALL;
        }
        int32_t fall = ((int32_t)(v0 + fall_vel) * dt) >> 8;
        if (fall == 0) {
            fall = 1;           // still probe the ground: on_ground depends on it
        }
        moveVertical(fall);
    }
    player_y = feet_y - eye_height;

    // ---- turning. Not turning and exactly N/E/S/W (the bytes of yaw & 0x3fffff), nothing to do ----
    const int24_t turn_step = (int24_t)((dt << 2) + (dt << 1));
    const uint8_t *yaw = (const uint8_t *)&player_yaw;
    if (!(keys & (MOVE_TURN_LEFT | MOVE_TURN_RIGHT)) && turn_vel == 0 && !snapping &&
        (yaw[2] & 0x3f) == 0 && yaw[1] == 0 && yaw[0] == 0) {
        return;
    }
    if (keys & (MOVE_TURN_LEFT | MOVE_TURN_RIGHT)) {
        snapping = 0;
        turn_vel = approach(turn_vel, (keys & MOVE_TURN_LEFT) ? -TURN_SPEED : TURN_SPEED, turn_step);
    } else {
        // signed distance to the nearest multiple of 64 steps, in 1/65536 steps
        const int32_t off = (int32_t)((player_yaw + (32L << 16)) & ((64L << 16) - 1)) - (32L << 16);
        if (!snapping && off != 0 && off <= SNAP_RANGE && off >= -SNAP_RANGE &&
            turn_vel < SNAP_START && turn_vel > -SNAP_START) {
            snapping = 1;
        }
        if (snapping) {
            if (off < SNAP_DONE && off > -SNAP_DONE) {
                player_yaw -= off;  // exactly N/E/S/W: projectRecords takes the fast path
                turn_vel = 0;
                snapping = 0;
            } else {
                turn_vel = (int24_t)(-(off >> 8) * SNAP_GAIN);
            }
        } else {
            turn_vel = approach(turn_vel, 0, turn_step);
        }
    }
    if (turn_vel != 0) {
        player_yaw += ((int32_t)turn_vel * dt) >> 7;  // 1/65536 steps
    }
}


// No kb_Scan(): main() puts the keypad in continuous scan mode, so kb_Data is always current.
// kb_Scan() waits for a whole scan, about 62k cycles (1.3 ms) per frame
void playerUpdate(uint24_t dt) {
    const uint8_t arrows = kb_Data[7];
    const uint8_t group1 = kb_Data[1];
    uint8_t keys = 0;
    if (arrows & kb_Up)          keys |= MOVE_FORWARD;
    if (arrows & kb_Down)        keys |= MOVE_BACK;
    if (arrows & kb_Left)        keys |= MOVE_TURN_LEFT;
    if (arrows & kb_Right)       keys |= MOVE_TURN_RIGHT;
    if (group1 & kb_Mode)        keys |= MOVE_LEFT;
    if (group1 & kb_Del)         keys |= MOVE_RIGHT;
    if (group1 & kb_2nd)         keys |= MOVE_JUMP;
    if (kb_Data[2] & kb_Alpha)   keys |= MOVE_CROUCH;
    updateMotion(keys, dt);
}
