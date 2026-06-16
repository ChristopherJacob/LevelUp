// Pure leveling-guidance math. No hardware / LVGL / NVS dependencies.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LEVEL_MODE_BLOCKS = 0, LEVEL_MODE_RAMPS = 1 } leveling_mode_t;

// Corner index order used throughout the codebase.
typedef enum { CORNER_FL = 0, CORNER_FR = 1, CORNER_RL = 2, CORNER_RR = 3 } corner_t;

// Which screen edge points to the front of the van (wizard answer).
typedef enum {
    ORIENT_FRONT_TOP = 0,
    ORIENT_FRONT_BOTTOM = 1,
    ORIENT_FRONT_LEFT = 2,
    ORIENT_FRONT_RIGHT = 3,
} leveling_front_t;

// Mapping from device roll/pitch into van-frame "front-high" / "left-high" tilts.
typedef struct {
    bool  front_is_pitch_axis; // true: van front/back lies along the pitch axis
    float front_sign;          // +1/-1 so (axis*front_sign) > 0 means FRONT is high
    float left_sign;           // +1/-1 so (axis*left_sign)  > 0 means LEFT side is high
} leveling_orient_t;

typedef struct {
    bool     guidance_available; // false when vehicle dimensions are unset/zero
    float    corner_lift_in[4];  // indexed by corner_t; highest corner == 0.0
    corner_t worst_corner;       // largest lift — act on first
    float    max_lift_in;        // magnitude of the worst-corner lift

    // Ramp mode (dominant single axis):
    bool     ramp_axis_is_roll;  // dominant correction is roll (side) vs pitch (end)
    bool     ramp_lift_left;     // ramp goes under LEFT wheels (else right)  [roll axis]
    bool     ramp_lift_front;    // ramp goes under FRONT wheels (else rear)  [pitch axis]
    float    ramp_target_in;     // ramp height needed
    float    ramp_remaining_in;  // live distance-to-level (== ramp_target_in here)

    bool     is_level;           // both van-frame tilts within LEVELING_LEVEL_DEG
} leveling_result_t;

#define LEVELING_LEVEL_DEG 0.5f

// Resolve a wizard front-direction choice into an axis/sign mapping.
leveling_orient_t leveling_orient_from_front(leveling_front_t front);

// Compute guidance. roll/pitch are the device (already zero-offset) angles in degrees.
leveling_result_t leveling_compute(float roll_deg, float pitch_deg,
                                   float trackwidth_in, float wheelbase_in,
                                   leveling_mode_t mode,
                                   leveling_orient_t orient);

#ifdef __cplusplus
}
#endif
