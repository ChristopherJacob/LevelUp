#include "leveling.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void test_orient_from_front(void)
{
    leveling_orient_t o = leveling_orient_from_front(ORIENT_FRONT_TOP);
    assert(o.front_is_pitch_axis == true);
    assert(o.front_sign == 1.0f);
    assert(o.left_sign == 1.0f);

    o = leveling_orient_from_front(ORIENT_FRONT_BOTTOM);
    assert(o.front_is_pitch_axis == true);
    assert(o.front_sign == -1.0f);
    assert(o.left_sign == -1.0f);

    o = leveling_orient_from_front(ORIENT_FRONT_LEFT);
    assert(o.front_is_pitch_axis == false);
    assert(o.front_sign == 1.0f);
    assert(o.left_sign == -1.0f);

    o = leveling_orient_from_front(ORIENT_FRONT_RIGHT);
    assert(o.front_is_pitch_axis == false);
    assert(o.front_sign == -1.0f);
    assert(o.left_sign == 1.0f);
}

static int near(float a, float b) { return fabsf(a - b) < 0.05f; }

static void test_block_lifts(void)
{
    // Front-top orientation. Pure roll: left side high by 5 deg.
    // trackwidth 60in -> half 30in -> left corners higher by 30*tan(5)+30*tan(5).
    // Left corners are the reference (highest, lift 0); right corners need lifting.
    leveling_orient_t o = leveling_orient_from_front(ORIENT_FRONT_TOP);
    leveling_result_t r = leveling_compute(5.0f, 0.0f, 60.0f, 120.0f,
                                           LEVEL_MODE_BLOCKS, o);
    assert(r.guidance_available);
    // left high -> FL, RL are reference (0); FR, RR lifted by 60*tan(5deg)=5.25in
    assert(near(r.corner_lift_in[CORNER_FL], 0.0f));
    assert(near(r.corner_lift_in[CORNER_RL], 0.0f));
    assert(near(r.corner_lift_in[CORNER_FR], 5.25f));
    assert(near(r.corner_lift_in[CORNER_RR], 5.25f));
    assert(near(r.max_lift_in, 5.25f));
    // worst corner is one of the two low (right) corners
    assert(r.worst_corner == CORNER_FR || r.worst_corner == CORNER_RR);
    assert(r.is_level == false);

    // Perfectly level -> all zero.
    leveling_result_t z = leveling_compute(0.0f, 0.0f, 60.0f, 120.0f,
                                           LEVEL_MODE_BLOCKS, o);
    assert(near(z.corner_lift_in[CORNER_FL], 0.0f));
    assert(near(z.corner_lift_in[CORNER_FR], 0.0f));
    assert(near(z.corner_lift_in[CORNER_RL], 0.0f));
    assert(near(z.corner_lift_in[CORNER_RR], 0.0f));
    assert(near(z.max_lift_in, 0.0f));
    assert(z.is_level == true);

    // Axis-swap: FRONT_LEFT makes device roll drive front/back tilt.
    leveling_orient_t left = leveling_orient_from_front(ORIENT_FRONT_LEFT);
    leveling_result_t s = leveling_compute(5.0f, 0.0f, 60.0f, 120.0f,
                                           LEVEL_MODE_BLOCKS, left);
    assert(s.guidance_available);
    assert(near(s.corner_lift_in[CORNER_FL], 0.0f));
    assert(near(s.corner_lift_in[CORNER_FR], 0.0f));
    assert(near(s.corner_lift_in[CORNER_RL], 10.49f));
    assert(near(s.corner_lift_in[CORNER_RR], 10.49f));
    assert(near(s.max_lift_in, 10.49f));

    // Unset dimensions -> guidance unavailable.
    leveling_result_t u = leveling_compute(5.0f, 0.0f, 0.0f, 120.0f,
                                           LEVEL_MODE_BLOCKS, o);
    assert(u.guidance_available == false);
}

static void test_ramp_dominant(void)
{
    leveling_orient_t o = leveling_orient_from_front(ORIENT_FRONT_TOP);

    // Roll-dominant: big side tilt, tiny end tilt.
    // left high 5deg, trackwidth 60 -> roll lift 60*tan5=5.25; pitch ~0.
    leveling_result_t r = leveling_compute(5.0f, 0.2f, 60.0f, 120.0f,
                                           LEVEL_MODE_RAMPS, o);
    assert(r.ramp_axis_is_roll == true);
    assert(r.ramp_lift_left == false);     // left is high -> ramp under RIGHT wheels
    assert(near(r.ramp_target_in, 5.25f));
    assert(near(r.ramp_remaining_in, 5.25f));

    // Pitch-dominant: nose up 4deg, wheelbase 120 -> 120*tan4=8.39; roll tiny.
    leveling_result_t p = leveling_compute(0.2f, 4.0f, 60.0f, 120.0f,
                                           LEVEL_MODE_RAMPS, o);
    assert(p.ramp_axis_is_roll == false);
    assert(p.ramp_lift_front == false);    // front is high -> ramp under REAR wheels
    assert(near(p.ramp_target_in, 8.39f));
}

int main(void)
{
    test_orient_from_front();
    test_block_lifts();
    test_ramp_dominant();
    printf("ALL TESTS PASSED\n");
    return 0;
}
