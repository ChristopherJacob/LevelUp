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
    return r;
}
