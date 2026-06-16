#include "leveling.h"
#include <assert.h>
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

int main(void)
{
    test_orient_from_front();
    printf("ALL TESTS PASSED\n");
    return 0;
}
