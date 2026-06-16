// Pure leveling-guidance math. See leveling.h.
#include "leveling.h"
#include <math.h>

#define DEG2RAD 0.017453292519943295f

// NOTE: signs below assume the device mounted screen-up with the default IMU
// axis convention. Physical sign correctness is confirmed on-device in the
// final verification task; flip a row here if the FRONT marker points wrong.
leveling_orient_t leveling_orient_from_front(leveling_front_t front)
{
    switch (front) {
        case ORIENT_FRONT_TOP:    return (leveling_orient_t){ true,   1.0f,  1.0f };
        case ORIENT_FRONT_BOTTOM: return (leveling_orient_t){ true,  -1.0f, -1.0f };
        case ORIENT_FRONT_LEFT:   return (leveling_orient_t){ false,  1.0f, -1.0f };
        case ORIENT_FRONT_RIGHT:  return (leveling_orient_t){ false, -1.0f,  1.0f };
        default:                  return (leveling_orient_t){ true,   1.0f,  1.0f };
    }
}

leveling_result_t leveling_compute(float roll_deg, float pitch_deg,
                                   float trackwidth_in, float wheelbase_in,
                                   leveling_mode_t mode,
                                   leveling_orient_t orient)
{
    leveling_result_t r = (leveling_result_t){0};
    (void)mode;

    if (trackwidth_in <= 0.0f || wheelbase_in <= 0.0f) {
        r.guidance_available = false;
        return r;
    }
    r.guidance_available = true;

    // Map device angles into van-frame tilts (degrees).
    float front_high_deg, left_high_deg;
    if (orient.front_is_pitch_axis) {
        front_high_deg = orient.front_sign * pitch_deg;
        left_high_deg  = orient.left_sign  * roll_deg;
    } else {
        front_high_deg = orient.front_sign * roll_deg;
        left_high_deg  = orient.left_sign  * pitch_deg;
    }

    r.is_level = (fabsf(front_high_deg) <= LEVELING_LEVEL_DEG) &&
                 (fabsf(left_high_deg)  <= LEVELING_LEVEL_DEG);

    const float half_base = wheelbase_in  * 0.5f; // along front/rear
    const float half_wt   = trackwidth_in * 0.5f; // along left/right
    const float tan_front = tanf(front_high_deg * DEG2RAD);
    const float tan_left  = tanf(left_high_deg  * DEG2RAD);

    // Relative corner heights (inches). +front and +left are "high".
    float h[4];
    h[CORNER_FL] =  half_base * tan_front + half_wt * tan_left;
    h[CORNER_FR] =  half_base * tan_front - half_wt * tan_left;
    h[CORNER_RL] = -half_base * tan_front + half_wt * tan_left;
    h[CORNER_RR] = -half_base * tan_front - half_wt * tan_left;

    float hmax = h[0];
    for (int i = 1; i < 4; i++) if (h[i] > hmax) hmax = h[i];

    r.max_lift_in = 0.0f;
    r.worst_corner = CORNER_FL;
    for (int i = 0; i < 4; i++) {
        float lift = hmax - h[i];
        if (lift < 0.0f) lift = 0.0f;
        r.corner_lift_in[i] = lift;
        if (lift > r.max_lift_in) {
            r.max_lift_in = lift;
            r.worst_corner = (corner_t)i;
        }
    }

    // Ramp mode collapses to the single dominant axis.
    float roll_lift  = fabsf(trackwidth_in * tan_left);   // lift to cancel side tilt
    float pitch_lift = fabsf(wheelbase_in  * tan_front);  // lift to cancel end tilt
    r.ramp_axis_is_roll = (roll_lift >= pitch_lift);
    if (r.ramp_axis_is_roll) {
        r.ramp_target_in = roll_lift;
        // Lift the LOW side. left_high>0 means left is high -> lift right (left=false).
        r.ramp_lift_left = (left_high_deg < 0.0f);
    } else {
        r.ramp_target_in = pitch_lift;
        r.ramp_lift_front = (front_high_deg < 0.0f);
    }
    r.ramp_remaining_in = r.ramp_target_in;

    return r;
}
