# Leveling Guidance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn LevelUp from a tilt display into a leveling tool that tells the user which wheels to raise (blocks) or which way to drive (ramps), shown on the AMOLED, web dashboard, and Home Assistant.

**Architecture:** A new pure module `main/leveling.c/.h` computes a `leveling_result_t` (per-corner lift, worst corner, ramp direction, is_level) from roll/pitch + vehicle dimensions + orientation + mode. `imu_task` calls it each cycle and fans the result out to three dumb renderers (`ui`, `wifi_mgr`, `mqtt_mgr`) via new `*_update_guidance()` setters, mirroring the existing `*_update_angles()` pattern. The pure module is host-unit-tested with plain `cc`; the result struct is the seam a future air-leveling controller will consume.

**Tech Stack:** C11, ESP-IDF 5.5.1, FreeRTOS, LVGL 9, ESP HTTP server, MQTT (HA discovery). Host tests: plain `cc` + `assert`, no framework.

**Build/flash reminders (from project memory):**
- Build: `source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh && idf.py build`
- Flash: `bash tools/flash_remote.sh` (NOT `idf.py flash`)
- `wifi_mgr.c` `send_chunkf` has a 512-byte buffer — use `send_chunk` for static HTML, `send_chunkf` only for small substitutions. Wrap DOM lookups for later-chunk elements in `DOMContentLoaded`.

---

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `main/leveling.h` | Public types (`leveling_mode_t`, `corner_t`, `leveling_front_t`, `leveling_orient_t`, `leveling_result_t`) + `leveling_compute()` / `leveling_orient_from_front()` | Create |
| `main/leveling.c` | Pure math: orientation mapping, corner-height plane, block lifts, ramp dominant-axis, is_level, dims guard | Create |
| `test/leveling/test_leveling.c` | Host unit tests (plain `cc`, `assert`) | Create |
| `main/CMakeLists.txt` | Add `leveling.c` to SRCS | Modify |
| `main/wifi_mgr.h` / `.c` | Own `lvl_orient` + `lvl_mode` NVS settings; accessors; cache latest result; `/status.json` + `/status` card; mode endpoint; wizard orientation step | Modify |
| `main/imu_task.c` | Call `leveling_compute()` and fan out to ui/web/mqtt | Modify |
| `main/mqtt_mgr.h` / `.c` | `mqtt_mgr_update_guidance()`; guidance fields in state JSON; corner + `is_level` discovery entities | Modify |
| `main/ui.h` / `.c` | `ui_update_guidance()`; bubble⇄guidance view toggle; guidance screen LVGL objects | Modify |
| `docs/rest-sensors.md`, `docs/mqtt-discovery.md` | Document new fields/entities | Modify |

---

## Task 1: Leveling module — types + orientation mapping (TDD)

**Files:**
- Create: `main/leveling.h`
- Create: `main/leveling.c`
- Create: `test/leveling/test_leveling.c`

- [ ] **Step 1: Write `main/leveling.h`**

```c
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
```

- [ ] **Step 2: Write the failing test `test/leveling/test_leveling.c` (orientation only for now)**

```c
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
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cc -std=c11 -Imain -o /tmp/test_leveling test/leveling/test_leveling.c main/leveling.c -lm && /tmp/test_leveling`
Expected: FAIL — link error (`leveling.c` not yet written / `leveling_orient_from_front` undefined).

- [ ] **Step 4: Write `main/leveling.c` with the orientation table**

```c
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
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cc -std=c11 -Imain -o /tmp/test_leveling test/leveling/test_leveling.c main/leveling.c -lm && /tmp/test_leveling`
Expected: `ALL TESTS PASSED`

- [ ] **Step 6: Commit**

```bash
git add main/leveling.h main/leveling.c test/leveling/test_leveling.c
git commit -m "feat(leveling): add module types + orientation mapping"
```

---

## Task 2: Block-mode corner lifts (TDD)

**Files:**
- Modify: `main/leveling.c`
- Test: `test/leveling/test_leveling.c`

- [ ] **Step 1: Add the failing test (append a function + call it in `main`)**

Add this function above `main()` and add `test_block_lifts();` as the first line of `main()`:

```c
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

    // Perfectly level -> all zero.
    leveling_result_t z = leveling_compute(0.0f, 0.0f, 60.0f, 120.0f,
                                           LEVEL_MODE_BLOCKS, o);
    assert(near(z.corner_lift_in[CORNER_FL], 0.0f));
    assert(near(z.max_lift_in, 0.0f));

    // Unset dimensions -> guidance unavailable.
    leveling_result_t u = leveling_compute(5.0f, 0.0f, 0.0f, 120.0f,
                                           LEVEL_MODE_BLOCKS, o);
    assert(u.guidance_available == false);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cc -std=c11 -Imain -o /tmp/test_leveling test/leveling/test_leveling.c main/leveling.c -lm && /tmp/test_leveling`
Expected: FAIL (assertion in `test_block_lifts` — lifts are all 0 from the stub).

- [ ] **Step 3: Implement corner-height plane + block lifts in `leveling_compute`**

Replace the body of `leveling_compute` after the `guidance_available = true;` line (i.e. replace `return r;`) with:

```c
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
    // CORNER_FL: front + left
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

    return r;
```

- [ ] **Step 4: Run to verify it passes**

Run: `cc -std=c11 -Imain -o /tmp/test_leveling test/leveling/test_leveling.c main/leveling.c -lm && /tmp/test_leveling`
Expected: `ALL TESTS PASSED`

- [ ] **Step 5: Commit**

```bash
git add main/leveling.c test/leveling/test_leveling.c
git commit -m "feat(leveling): block-mode corner lift computation"
```

---

## Task 3: Ramp-mode dominant axis (TDD)

**Files:**
- Modify: `main/leveling.c`
- Test: `test/leveling/test_leveling.c`

- [ ] **Step 1: Add the failing test (append function + call from `main`)**

```c
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
```

- [ ] **Step 2: Run to verify it fails**

Run: `cc -std=c11 -Imain -o /tmp/test_leveling test/leveling/test_leveling.c main/leveling.c -lm && /tmp/test_leveling`
Expected: FAIL (ramp fields are 0 from Task 2).

- [ ] **Step 3: Implement ramp fields. Insert before `return r;` in `leveling_compute`**

```c
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
```

(Place this block immediately before the final `return r;` you added in Task 2.)

- [ ] **Step 4: Run to verify it passes**

Run: `cc -std=c11 -Imain -o /tmp/test_leveling test/leveling/test_leveling.c main/leveling.c -lm && /tmp/test_leveling`
Expected: `ALL TESTS PASSED`

- [ ] **Step 5: Commit**

```bash
git add main/leveling.c test/leveling/test_leveling.c
git commit -m "feat(leveling): ramp-mode dominant-axis guidance"
```

---

## Task 4: is_level threshold + orientation-swap coverage (TDD)

**Files:**
- Test: `test/leveling/test_leveling.c`

This task adds coverage only (the logic already exists) to lock behavior.

- [ ] **Step 1: Add the test (append function + call from `main`)**

```c
static void test_level_and_orient_swap(void)
{
    leveling_orient_t top = leveling_orient_from_front(ORIENT_FRONT_TOP);
    // Within 0.5 deg both axes -> level.
    leveling_result_t l = leveling_compute(0.3f, -0.4f, 60.0f, 120.0f,
                                           LEVEL_MODE_BLOCKS, top);
    assert(l.is_level == true);
    leveling_result_t nl = leveling_compute(0.3f, 1.0f, 60.0f, 120.0f,
                                            LEVEL_MODE_BLOCKS, top);
    assert(nl.is_level == false);

    // FRONT_LEFT swaps which device axis is front/back. A device "roll" now drives
    // the front/back tilt. roll=+5 with front_sign=+1 -> front high -> rear lifted.
    leveling_orient_t left = leveling_orient_from_front(ORIENT_FRONT_LEFT);
    leveling_result_t s = leveling_compute(5.0f, 0.0f, 60.0f, 120.0f,
                                           LEVEL_MODE_BLOCKS, left);
    assert(near(s.corner_lift_in[CORNER_FL], 0.0f));   // front high -> front is ref
    assert(near(s.corner_lift_in[CORNER_RL], s.max_lift_in)); // rear needs lift
}
```

- [ ] **Step 2: Run to verify it fails first, then passes**

Run: `cc -std=c11 -Imain -o /tmp/test_leveling test/leveling/test_leveling.c main/leveling.c -lm && /tmp/test_leveling`
Expected: PASS (logic already present). If any assert fails, the orientation table or plane math has a sign bug — fix in `leveling.c` until green. (This is the intended guard.)

- [ ] **Step 3: Commit**

```bash
git add test/leveling/test_leveling.c
git commit -m "test(leveling): cover is_level threshold and orientation swap"
```

---

## Task 5: Add `leveling.c` to the firmware build

**Files:**
- Modify: `main/CMakeLists.txt:7` (after `"imu_task.c"`)

- [ ] **Step 1: Add the source file**

In `main/CMakeLists.txt`, add `"leveling.c"` to the `SRCS` list, after the `"imu_task.c"` line:

```cmake
    "imu_task.c"
    "leveling.c"
```

- [ ] **Step 2: Build the firmware**

Run: `source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh && idf.py build`
Expected: build succeeds (leveling.c compiles; nothing calls it yet).

- [ ] **Step 3: Commit**

```bash
git add main/CMakeLists.txt
git commit -m "build: compile leveling module into firmware"
```

---

## Task 6: wifi_mgr — orientation + mode settings (NVS) and accessors

**Files:**
- Modify: `main/wifi_mgr.h` (add accessors)
- Modify: `main/wifi_mgr.c` (NVS keys, state, load/save, accessors)

`wifi_mgr` already owns the `leveler` NVS namespace usage indirectly and the vehicle dims; it becomes the owner of `lvl_orient` (u8) and `lvl_mode` (u8).

- [ ] **Step 1: Add public accessors to `main/wifi_mgr.h`** (after line 16, the trackwidth accessor)

```c
// Leveling orientation/mode settings (see leveling.h enums).
unsigned char wifi_mgr_get_orient(void);   // leveling_front_t value
unsigned char wifi_mgr_get_mode(void);     // leveling_mode_t value
void wifi_mgr_set_mode(unsigned char mode); // update + persist Blocks/Ramps
```

- [ ] **Step 2: Add NVS keys + state in `main/wifi_mgr.c`** (near line 223-224, beside `NVS_KEY_WHEELBASE`)

```c
#define NVS_KEY_LVL_ORIENT  "lvl_orient"
#define NVS_KEY_LVL_MODE    "lvl_mode"
```

And near the `s_wheelbase_in`/`s_trackwidth_in` declarations (line ~288):

```c
static unsigned char s_lvl_orient = 0; // ORIENT_FRONT_TOP
static unsigned char s_lvl_mode   = 0; // LEVEL_MODE_BLOCKS
```

- [ ] **Step 3: Load these in init.** In the init path near line 3557 (where `nvs_load_config` is called), add after the existing dimension load + parse:

```c
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_LEVELER, NVS_READONLY, &h) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(h, NVS_KEY_LVL_ORIENT, &v) == ESP_OK) s_lvl_orient = v;
            v = 0;
            if (nvs_get_u8(h, NVS_KEY_LVL_MODE, &v) == ESP_OK) s_lvl_mode = v;
            nvs_close(h);
        }
    }
```

- [ ] **Step 4: Add accessors + setter near the existing dimension accessors (line ~3585)**

```c
unsigned char wifi_mgr_get_orient(void) { return s_lvl_orient; }
unsigned char wifi_mgr_get_mode(void)   { return s_lvl_mode; }

void wifi_mgr_set_mode(unsigned char mode)
{
    if (mode > 1) mode = 0;
    s_lvl_mode = mode;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_LEVELER, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_LVL_MODE, mode);
        nvs_commit(h);
        nvs_close(h);
    }
}
```

- [ ] **Step 5: Build**

Run: `source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh && idf.py build`
Expected: build succeeds.

- [ ] **Step 6: Commit**

```bash
git add main/wifi_mgr.h main/wifi_mgr.c
git commit -m "feat(wifi_mgr): persist leveling orientation + mode settings"
```

---

## Task 7: imu_task — compute guidance and fan out

**Files:**
- Modify: `main/imu_task.c` (includes + inside the LVGL-locked block at lines ~302-317)

- [ ] **Step 1: Add the include** (near line 17, with the other mgr includes)

```c
#include "leveling.h"
```

- [ ] **Step 2: Compute + dispatch.** Inside the `if (lvgl_port_lock(10)) {` block in `imu_task` (currently lines ~287-320), after the existing `ui_set_inches(roll_in, pitch_in);` call and before the `wifi_mgr_update_angles(...)` line, insert:

```c
                    // Shared leveling guidance (block/ramp) for all surfaces.
                    leveling_orient_t orient =
                        leveling_orient_from_front((leveling_front_t)wifi_mgr_get_orient());
                    leveling_result_t guide =
                        leveling_compute(roll_rel, pitch_rel,
                                         trackwidth_in, wheelbase_in,
                                         (leveling_mode_t)wifi_mgr_get_mode(),
                                         orient);
                    ui_update_guidance(&guide);
                    wifi_mgr_update_guidance(&guide);
                    mqtt_mgr_update_guidance(&guide);
```

(Note: `roll_rel`, `pitch_rel`, `trackwidth_in`, `wheelbase_in` are already in scope here — see imu_task.c:293-306.)

- [ ] **Step 3: Build — expect failures for the not-yet-defined setters**

Run: `source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh && idf.py build`
Expected: FAIL — `ui_update_guidance`, `wifi_mgr_update_guidance`, `mqtt_mgr_update_guidance` undeclared. (These are added in Tasks 8-10.) This confirms the call site compiles structurally.

- [ ] **Step 4: Commit (WIP — resolved by the next tasks)**

```bash
git add main/imu_task.c
git commit -m "feat(imu_task): compute leveling guidance and dispatch to surfaces"
```

---

## Task 8: mqtt_mgr — guidance setter, state JSON, discovery entities

**Files:**
- Modify: `main/mqtt_mgr.h` (declare setter)
- Modify: `main/mqtt_mgr.c` (include, cached struct, state JSON, discovery)

- [ ] **Step 1: Declare the setter in `main/mqtt_mgr.h`** (after line 13)

```c
#include "leveling.h"
// Update latest leveling guidance for the publish loop.
void mqtt_mgr_update_guidance(const leveling_result_t *g);
```

- [ ] **Step 2: Add include + cached state in `main/mqtt_mgr.c`.** Add near the top includes (line ~24):

```c
#include "leveling.h"
```

Add to the angle-muxed state (near line 41):

```c
static leveling_result_t s_guide;
```

Add discovery topic storage (near line 53, with the other `s_discovery_*` strings):

```c
static char s_discovery_lift_fl_topic[128];
static char s_discovery_lift_fr_topic[128];
static char s_discovery_lift_rl_topic[128];
static char s_discovery_lift_rr_topic[128];
static char s_discovery_is_level_topic[128];
```

- [ ] **Step 3: Build the new discovery topics.** In `mqtt_mgr_build_topics()` (after line 137), add:

```c
    snprintf(s_discovery_lift_fl_topic, sizeof(s_discovery_lift_fl_topic),
             "%s/sensor/%s/lift_fl/config", s_mqtt_disc, s_device_id);
    snprintf(s_discovery_lift_fr_topic, sizeof(s_discovery_lift_fr_topic),
             "%s/sensor/%s/lift_fr/config", s_mqtt_disc, s_device_id);
    snprintf(s_discovery_lift_rl_topic, sizeof(s_discovery_lift_rl_topic),
             "%s/sensor/%s/lift_rl/config", s_mqtt_disc, s_device_id);
    snprintf(s_discovery_lift_rr_topic, sizeof(s_discovery_lift_rr_topic),
             "%s/sensor/%s/lift_rr/config", s_mqtt_disc, s_device_id);
    snprintf(s_discovery_is_level_topic, sizeof(s_discovery_is_level_topic),
             "%s/binary_sensor/%s/is_level/config", s_mqtt_disc, s_device_id);
```

- [ ] **Step 4: Add a binary_sensor discovery helper.** After `mqtt_pub_disc_entity()` (line ~179), add:

```c
// Publish a binary_sensor discovery entry (ON/OFF payloads).
static void mqtt_pub_disc_binary(const char *config_topic, const char *name,
                                 const char *uniq_suffix, const char *value_key,
                                 const char *opt, const char *dev_block)
{
    char payload[640];
    int len = snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"uniq_id\":\"%s_%s\","
        "\"state_topic\":\"%s\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"value_template\":\"{{ value_json.%s }}\","
        "\"payload_on\":\"true\",\"payload_off\":\"false\""
        "%s,%s}",
        name, s_device_id, uniq_suffix, s_state_topic, s_availability_topic,
        value_key, opt, dev_block);
    if (len > 0 && len < (int)sizeof(payload)) {
        esp_mqtt_client_publish(s_client, config_topic, payload, 0, 1, 1);
    } else {
        ESP_LOGW(TAG, "binary discovery overflow for '%s' (%d)", name, len);
    }
}
```

- [ ] **Step 5: Publish the new entities.** In `mqtt_mgr_publish_discovery()`, before the final `ESP_LOGI(...)` (line ~268), add:

```c
    mqtt_pub_disc_entity(s_discovery_lift_fl_topic, "LevelUp Lift Front-Left", "lift_fl",
                         "lift_fl", "in",
                         ",\"icon\":\"mdi:format-vertical-align-up\","
                         "\"suggested_display_precision\":1", dev);
    mqtt_pub_disc_entity(s_discovery_lift_fr_topic, "LevelUp Lift Front-Right", "lift_fr",
                         "lift_fr", "in",
                         ",\"icon\":\"mdi:format-vertical-align-up\","
                         "\"suggested_display_precision\":1", dev);
    mqtt_pub_disc_entity(s_discovery_lift_rl_topic, "LevelUp Lift Rear-Left", "lift_rl",
                         "lift_rl", "in",
                         ",\"icon\":\"mdi:format-vertical-align-up\","
                         "\"suggested_display_precision\":1", dev);
    mqtt_pub_disc_entity(s_discovery_lift_rr_topic, "LevelUp Lift Rear-Right", "lift_rr",
                         "lift_rr", "in",
                         ",\"icon\":\"mdi:format-vertical-align-up\","
                         "\"suggested_display_precision\":1", dev);
    mqtt_pub_disc_binary(s_discovery_is_level_topic, "LevelUp Is Level", "is_level",
                         "is_level", ",\"icon\":\"mdi:car-lifted-pickup\"", dev);
```

- [ ] **Step 6: Add the setter + fold guidance into the state payload.** Add the setter near `mqtt_mgr_update_angles` (line ~493):

```c
void mqtt_mgr_update_guidance(const leveling_result_t *g)
{
    if (!g) return;
    portENTER_CRITICAL(&s_angle_mux);
    s_guide = *g;
    portEXIT_CRITICAL(&s_angle_mux);
}
```

In `mqtt_mgr_publish_state()`, read the cached guidance under the existing critical section (add to the block at lines ~285-293):

```c
    leveling_result_t g = s_guide;
```

Then extend the state JSON. Replace the closing of the `payload` snprintf (the `"\"mode\":\"STA\""` line and its args, lines ~325-327) so the JSON also carries guidance — change the format string tail from:

```c
                       "\"mode\":\"STA\""
                       "}",
                       roll, pitch, roll_in, pitch_in, ax, ay, az, rssi, ipbuf);
```

to:

```c
                       "\"mode\":\"STA\","
                       "\"lift_fl\":%.1f,\"lift_fr\":%.1f,"
                       "\"lift_rl\":%.1f,\"lift_rr\":%.1f,"
                       "\"is_level\":%s,"
                       "\"lvl_mode\":\"%s\","
                       "\"ramp_target\":%.1f,\"ramp_remaining\":%.1f"
                       "}",
                       roll, pitch, roll_in, pitch_in, ax, ay, az, rssi, ipbuf,
                       g.corner_lift_in[0], g.corner_lift_in[1],
                       g.corner_lift_in[2], g.corner_lift_in[3],
                       g.is_level ? "true" : "false",
                       g.ramp_axis_is_roll ? "ramp_roll" : "ramp_pitch",
                       g.ramp_target_in, g.ramp_remaining_in);
```

Also bump the `payload` buffer from `char payload[256];` (line ~313) to `char payload[384];`.

- [ ] **Step 7: Build**

Run: `source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh && idf.py build`
Expected: succeeds (mqtt setter now resolves one of imu_task's three calls).

- [ ] **Step 8: Commit**

```bash
git add main/mqtt_mgr.h main/mqtt_mgr.c
git commit -m "feat(mqtt): publish corner lifts + is_level binary_sensor"
```

---

## Task 9: ui — guidance setter + bubble⇄guidance view toggle + guidance screen

**Files:**
- Modify: `main/ui.h` (declare setter + view API)
- Modify: `main/ui.c` (include, objects, toggle, render)

- [ ] **Step 1: Declare the API in `main/ui.h`** (after line 17)

```c
#include "leveling.h"
// Update the guidance screen from the latest leveling result.
void ui_update_guidance(const leveling_result_t *g);
```

- [ ] **Step 2: Add include + objects in `main/ui.c`.** Add include near line 15:

```c
#include "leveling.h"
#include "wifi_mgr.h"
```

Add static objects near the other LVGL object statics (line ~52):

```c
// Guidance view
static lv_obj_t *s_guide_btn   = NULL;  // toggle on the bubble view
static lv_obj_t *s_guide_screen = NULL; // container for guidance UI
static lv_obj_t *s_guide_mode_btn = NULL;
static lv_obj_t *s_guide_status = NULL;
static lv_obj_t *s_guide_corner[4] = {NULL,NULL,NULL,NULL};
static lv_obj_t *s_guide_back_btn = NULL;
static bool s_guide_visible = false;
static leveling_result_t s_guide_last;
```

- [ ] **Step 3: Build the guidance screen in `ui_init()`.** Just before the final `ui_show_splash(SPLASH_DURATION_MS);` line (line ~704), add:

```c
    // ---- Guidance toggle button (top-left), shown on the bubble view ----
    s_guide_btn = lv_button_create(scr);
    lv_obj_set_size(s_guide_btn, 60, 52);
    lv_obj_align(s_guide_btn, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_radius(s_guide_btn, 12, 0);
    lv_obj_set_style_bg_color(s_guide_btn, lv_color_hex(0x12151A), 0);
    lv_obj_set_style_border_color(s_guide_btn, lv_color_hex(0x2E3440), 0);
    lv_obj_set_style_border_width(s_guide_btn, 1, 0);
    lv_obj_add_event_cb(s_guide_btn, guide_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gicon = lv_label_create(s_guide_btn);
    lv_label_set_text(gicon, LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(gicon, lv_color_hex(0xE9EDF2), 0);
    lv_obj_center(gicon);

    // ---- Guidance screen container (hidden by default) ----
    s_guide_screen = lv_obj_create(scr);
    lv_obj_remove_style_all(s_guide_screen);
    lv_obj_set_size(s_guide_screen, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_guide_screen, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_guide_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_guide_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_guide_screen, LV_OBJ_FLAG_HIDDEN);

    // Back-to-bubble button (top-left).
    s_guide_back_btn = lv_button_create(s_guide_screen);
    lv_obj_set_size(s_guide_back_btn, 56, 46);
    lv_obj_align(s_guide_back_btn, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_radius(s_guide_back_btn, 10, 0);
    lv_obj_set_style_bg_color(s_guide_back_btn, lv_color_hex(0x12151A), 0);
    lv_obj_add_event_cb(s_guide_back_btn, guide_back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bicon = lv_label_create(s_guide_back_btn);
    lv_label_set_text(bicon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bicon, lv_color_hex(0xE9EDF2), 0);
    lv_obj_center(bicon);

    // Blocks/Ramps mode toggle (top-right).
    s_guide_mode_btn = lv_button_create(s_guide_screen);
    lv_obj_set_size(s_guide_mode_btn, 96, 46);
    lv_obj_align(s_guide_mode_btn, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_radius(s_guide_mode_btn, 10, 0);
    lv_obj_set_style_bg_color(s_guide_mode_btn, lv_color_hex(0x12151A), 0);
    lv_obj_add_event_cb(s_guide_mode_btn, guide_mode_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mlbl = lv_label_create(s_guide_mode_btn);
    lv_obj_set_user_data(s_guide_mode_btn, mlbl);
    lv_label_set_text(mlbl, wifi_mgr_get_mode() == 0 ? "BLOCKS" : "RAMPS");
    lv_obj_set_style_text_color(mlbl, lv_color_hex(0xE9EDF2), 0);
    lv_obj_center(mlbl);

    // Van rectangle.
    lv_obj_t *van = lv_obj_create(s_guide_screen);
    lv_obj_set_size(van, 180, 200);
    lv_obj_align(van, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_radius(van, 16, 0);
    lv_obj_set_style_bg_color(van, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_color(van, lv_color_hex(0x2E3440), 0);
    lv_obj_set_style_border_width(van, 2, 0);
    lv_obj_clear_flag(van, LV_OBJ_FLAG_SCROLLABLE);

    // Four corner labels, parented to the van.
    const lv_align_t al[4] = { LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT,
                               LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT };
    const int ox[4] = { 6, -6, 6, -6 };
    const int oy[4] = { 6, 6, -6, -6 };
    for (int i = 0; i < 4; i++) {
        s_guide_corner[i] = lv_label_create(van);
        lv_obj_set_style_text_font(s_guide_corner[i], FONT_BIG, 0);
        lv_obj_set_style_text_color(s_guide_corner[i], lv_color_hex(0x39D98A), 0);
        lv_label_set_text(s_guide_corner[i], "--");
        lv_obj_align(s_guide_corner[i], al[i], ox[i], oy[i]);
    }

    // Bottom status line.
    s_guide_status = lv_label_create(s_guide_screen);
    lv_obj_set_style_text_font(s_guide_status, FONT_MED, 0);
    lv_obj_set_style_text_color(s_guide_status, lv_color_hex(0xE9EDF2), 0);
    lv_label_set_text(s_guide_status, "");
    lv_obj_align(s_guide_status, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_obj_move_foreground(s_guide_btn);
```

- [ ] **Step 4: Add the event callbacks + helpers ABOVE `ui_init()`** (e.g. near line 540, after `ui_show_rollback_warning`)

```c
static void ui_guide_set_visible(bool visible)
{
    s_guide_visible = visible;
    if (s_guide_screen) {
        if (visible) {
            lv_obj_clear_flag(s_guide_screen, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_guide_screen);
        } else {
            lv_obj_add_flag(s_guide_screen, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void guide_btn_event_cb(lv_event_t *e)   { (void)e; ui_guide_set_visible(true); }
static void guide_back_event_cb(lv_event_t *e)  { (void)e; ui_guide_set_visible(false); }

static void guide_mode_event_cb(lv_event_t *e)
{
    (void)e;
    unsigned char m = wifi_mgr_get_mode() == 0 ? 1 : 0;
    wifi_mgr_set_mode(m);
    if (s_guide_mode_btn) {
        lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(s_guide_mode_btn);
        if (lbl) lv_label_set_text(lbl, m == 0 ? "BLOCKS" : "RAMPS");
    }
}
```

(Add forward declarations for the three `*_event_cb` near the top static-function area, line ~128, so `ui_init` can reference them:)

```c
static void guide_btn_event_cb(lv_event_t *e);
static void guide_back_event_cb(lv_event_t *e);
static void guide_mode_event_cb(lv_event_t *e);
```

- [ ] **Step 5: Implement `ui_update_guidance()` at the end of `main/ui.c`**

```c
// Render the guidance screen from the latest leveling result.
void ui_update_guidance(const leveling_result_t *g)
{
    if (!g) return;
    s_guide_last = *g;
    if (!s_guide_screen) return;

    bool ramps = (wifi_mgr_get_mode() == 1);

    if (!g->guidance_available) {
        for (int i = 0; i < 4; i++) if (s_guide_corner[i]) lv_label_set_text(s_guide_corner[i], "--");
        if (s_guide_status) lv_label_set_text(s_guide_status, "Set vehicle dimensions in /wizard");
        return;
    }

    char buf[16];
    for (int i = 0; i < 4; i++) {
        if (!s_guide_corner[i]) continue;
        float lift = g->corner_lift_in[i];
        if (ramps || lift < 0.05f) {
            lv_label_set_text(s_guide_corner[i], ramps ? "" : "0");
        } else {
            snprintf(buf, sizeof(buf), "%.1f\"", lift);
            lv_label_set_text(s_guide_corner[i], buf);
        }
        lv_color_t c = lift <= 0.1f ? lv_color_hex(0x39D98A)
                      : lift <= 1.0f ? lv_color_hex(0xF2C94C)
                      : lv_color_hex(0xEB5757);
        lv_obj_set_style_text_color(s_guide_corner[i], c, 0);
    }

    if (!s_guide_status) return;
    if (g->is_level) {
        lv_obj_set_style_text_color(s_guide_status, lv_color_hex(0x39D98A), 0);
        lv_label_set_text(s_guide_status, "Level " LV_SYMBOL_OK);
    } else if (ramps) {
        char rb[48];
        const char *dir = g->ramp_axis_is_roll
            ? (g->ramp_lift_left ? "LEFT" : "RIGHT")
            : (g->ramp_lift_front ? "FRONT" : "REAR");
        snprintf(rb, sizeof(rb), "Drive %s wheels up %.1f\"", dir, g->ramp_remaining_in);
        lv_obj_set_style_text_color(s_guide_status, lv_color_hex(0xE9EDF2), 0);
        lv_label_set_text(s_guide_status, rb);
    } else {
        const char *cn[4] = { "FRONT-LEFT", "FRONT-RIGHT", "REAR-LEFT", "REAR-RIGHT" };
        char bb[48];
        snprintf(bb, sizeof(bb), "Raise %s first", cn[g->worst_corner]);
        lv_obj_set_style_text_color(s_guide_status, lv_color_hex(0xE9EDF2), 0);
        lv_label_set_text(s_guide_status, bb);
    }
}
```

- [ ] **Step 6: Build**

Run: `source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh && idf.py build`
Expected: succeeds (the `ui_update_guidance` call in imu_task now resolves). `wifi_mgr_update_guidance` still missing → that's Task 10.

- [ ] **Step 7: Commit**

```bash
git add main/ui.h main/ui.c
git commit -m "feat(ui): guidance screen with bubble toggle and blocks/ramps"
```

---

## Task 10: wifi_mgr — guidance cache, /status.json fields, status card, mode endpoint, wizard orientation step

**Files:**
- Modify: `main/wifi_mgr.h` (declare guidance setter)
- Modify: `main/wifi_mgr.c` (include, cache, json, html, endpoints)

- [ ] **Step 1: Declare the setter in `main/wifi_mgr.h`** (after the accessors from Task 6)

```c
#include "leveling.h"
// Push latest leveling guidance for /status.
void wifi_mgr_update_guidance(const leveling_result_t *g);
```

- [ ] **Step 2: Add include, cache, and setter in `main/wifi_mgr.c`.** Add include near the top includes:

```c
#include "leveling.h"
```

Add cached state near `s_lvl_orient` (Task 6):

```c
static leveling_result_t s_guide;
```

Add the setter near `wifi_mgr_update_angles` (search for its definition):

```c
void wifi_mgr_update_guidance(const leveling_result_t *g)
{
    if (!g) return;
    portENTER_CRITICAL(&s_angle_mux);
    s_guide = *g;
    portEXIT_CRITICAL(&s_angle_mux);
}
```

- [ ] **Step 3: Add guidance fields to `/status.json`.** In `http_status_json_get` (line ~1468), read the cache with the existing critical section (add inside the block at lines ~1475-1481):

```c
    leveling_result_t g = s_guide;
```

Then, just before the final `send_chunk(req, "}");` (line ~1549), change the preceding line's trailing format so JSON stays valid. Replace:

```c
    send_chunkf(req, "\"screen_on\":%s", screen_on ? "true" : "false");
    send_chunk(req, "}");
```

with:

```c
    send_chunkf(req, "\"screen_on\":%s,", screen_on ? "true" : "false");
    send_chunkf(req, "\"lvl_mode\":\"%s\",", wifi_mgr_get_mode() == 1 ? "ramps" : "blocks");
    send_chunkf(req, "\"guidance_available\":%s,", g.guidance_available ? "true" : "false");
    send_chunkf(req, "\"is_level\":%s,", g.is_level ? "true" : "false");
    send_chunkf(req, "\"lift_fl\":%.1f,\"lift_fr\":%.1f,", g.corner_lift_in[0], g.corner_lift_in[1]);
    send_chunkf(req, "\"lift_rl\":%.1f,\"lift_rr\":%.1f,", g.corner_lift_in[2], g.corner_lift_in[3]);
    send_chunkf(req, "\"worst_corner\":%d,", (int)g.worst_corner);
    send_chunkf(req, "\"ramp_axis_is_roll\":%s,", g.ramp_axis_is_roll ? "true" : "false");
    send_chunkf(req, "\"ramp_lift_left\":%s,", g.ramp_lift_left ? "true" : "false");
    send_chunkf(req, "\"ramp_lift_front\":%s,", g.ramp_lift_front ? "true" : "false");
    send_chunkf(req, "\"ramp_remaining_in\":%.1f", g.ramp_remaining_in);
    send_chunk(req, "}");
```

- [ ] **Step 4: Add a `/leveling_mode` POST endpoint.** Add the handler near `http_display_save_post` (line ~1405):

```c
// POST /leveling_mode  (form: csrf_token + mode=blocks|ramps)
static esp_err_t http_leveling_mode_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 256) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad len"); return ESP_OK; }
    char *body = (char *)calloc(1, (size_t)total + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem"); return ESP_OK; }
    int rcvd = 0;
    while (rcvd < total) {
        int r = httpd_req_recv(req, body + rcvd, total - rcvd);
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); return ESP_OK; }
        rcvd += r;
    }
    body[rcvd] = '\0';
    if (!csrf_ok(body)) { free(body); httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF"); return ESP_OK; }
    char mode[12] = {0};
    (void)form_get_value(body, "mode", mode, sizeof(mode));
    free(body);
    wifi_mgr_set_mode(strcmp(mode, "ramps") == 0 ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
```

Register it in the handler-registration block (near line 3104, beside `display_save`):

```c
    httpd_uri_t leveling_mode = {
        .uri = "/leveling_mode",
        .method = HTTP_POST,
        .handler = http_leveling_mode_post,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &leveling_mode);
```

Add its forward declaration with the other `static esp_err_t http_*` prototypes (near line 870):

```c
static esp_err_t http_leveling_mode_post(httpd_req_t *req);
```

- [ ] **Step 5: Add a guidance card to the `/status` HTML.** In `http_status_get`, after the angle/level section is emitted (find a sensible spot near the top of the page body), add a static block + JS that reads the new `status.json` fields. Insert this static HTML where other cards are sent:

```c
    send_chunk(req,
        "<div class='card' id='guideCard' style='margin:12px 0;padding:14px;"
        "border:1px solid var(--border);border-radius:12px'>"
        "<div style='display:flex;justify-content:space-between;align-items:center'>"
        "<b>Leveling guidance</b>"
        "<span id='gMode' style='font-size:12px;color:var(--muted)'></span></div>"
        "<table style='width:100%;margin-top:8px;border-collapse:collapse;text-align:center'>"
        "<tr><td id='gFL'>--</td><td id='gFR'>--</td></tr>"
        "<tr><td colspan=2 style='font-size:10px;color:var(--muted)'>FRONT</td></tr>"
        "<tr><td colspan=2 style='font-size:10px;color:var(--muted)'>REAR</td></tr>"
        "<tr><td id='gRL'>--</td><td id='gRR'>--</td></tr></table>"
        "<div id='gStatus' style='margin-top:8px;font-weight:600'></div>"
        "<button id='gToggle' style='margin-top:8px' onclick='toggleMode()'>Switch mode</button>"
        "</div>"
    );
```

Add the JS to the page's existing polling script (it already fetches `/status.json`). Append this inside that script — and because `guideCard` is sent in a later chunk than the early `<script>`, wrap the wiring in `DOMContentLoaded`:

```c
    send_chunk(req,
        "<script>document.addEventListener('DOMContentLoaded',function(){"
        "function fmt(v){return (v<0.05?'0':v.toFixed(1)+'\"');}"
        "function paint(d){"
        "var r=d.lvl_mode==='ramps';"
        "document.getElementById('gMode').textContent=d.lvl_mode;"
        "if(!d.guidance_available){document.getElementById('gStatus').textContent="
        "'Set vehicle dimensions';return;}"
        "document.getElementById('gFL').textContent=r?'':fmt(d.lift_fl);"
        "document.getElementById('gFR').textContent=r?'':fmt(d.lift_fr);"
        "document.getElementById('gRL').textContent=r?'':fmt(d.lift_rl);"
        "document.getElementById('gRR').textContent=r?'':fmt(d.lift_rr);"
        "var s;if(d.is_level){s='Level \\u2713';}"
        "else if(r){var dir=d.ramp_axis_is_roll?(d.ramp_lift_left?'LEFT':'RIGHT')"
        ":(d.ramp_lift_front?'FRONT':'REAR');"
        "s='Drive '+dir+' wheels up '+d.ramp_remaining_in.toFixed(1)+'\"';}"
        "else{var cn=['FRONT-LEFT','FRONT-RIGHT','REAR-LEFT','REAR-RIGHT'];"
        "s='Raise '+cn[d.worst_corner]+' first';}"
        "document.getElementById('gStatus').textContent=s;}"
        "window.__paintGuide=paint;"
        "});"
        "function toggleMode(){"
        "fetch('/status.json').then(function(r){return r.json();}).then(function(d){"
        "var nm=d.lvl_mode==='ramps'?'blocks':'ramps';"
        "fetch('/leveling_mode',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'csrf_token='+encodeURIComponent(_csrf)+'&mode='+nm});});}"
        "</script>"
    );
```

Then, in the page's existing `status.json` poll success handler, add a call `if(window.__paintGuide)window.__paintGuide(d);` (locate the `.then(function(d){...})` that already updates angle fields and add this line inside it).

- [ ] **Step 6: Add the wizard orientation step.** This converts the wizard from 3 to 4 steps. In `http_wizard_get` (line ~2632):
  - Change `"Step 1 of 3..."` subtitle and the 3 dots to 4 dots (add `<div class='dot' id='dot4'></div>` and update `id='stepSub'` initial text to `"Step 1 of 4: Level Reference"`).
  - Update the JS `SUBS` array to 4 entries and `[1,2,3]`→`[1,2,3,4]` and `goStep` bounds, and make step 1 advance to the new orientation step instead of audio.

Insert a new orientation step block (place it after step 1's closing `</div>` at line ~2774, before step 2):

```c
    /* Step 2: Orientation */
    send_chunk(req,
        "<div class='step' id='step2o'>"
        "<p class='desc'>Which screen edge points to the <b>front</b> of the van? "
        "This lets the guidance name the correct wheels.</p>"
        "<div style='display:grid;grid-template-columns:1fr 1fr;gap:10px'>"
        "<button class='wbtn sec' onclick='setOrient(0)'>Top</button>"
        "<button class='wbtn sec' onclick='setOrient(1)'>Bottom</button>"
        "<button class='wbtn sec' onclick='setOrient(2)'>Left</button>"
        "<button class='wbtn sec' onclick='setOrient(3)'>Right</button>"
        "</div><div class='msg' id='oMsg'></div></div>"
    );
```

Renumber the existing audio step to step 3 and done to step 4 (update their `id`s used by `goStep`, e.g. keep DOM ids but ensure `goStep(n)` shows the right element; simplest: give the steps ids `step1, step2o, step3, step4` and update `goStep` to map index→id via an array). Add to the JS:

```c
    send_chunk(req,
        "var STEP_IDS=['step1','step2o','step3','step4'];"
        "function goStep(n){document.querySelectorAll('.step').forEach(function(s){"
        "s.classList.remove('active');});"
        "document.getElementById(STEP_IDS[n-1]).classList.add('active');"
        "[1,2,3,4].forEach(function(i){var d=document.getElementById('dot'+i);"
        "d.className='dot'+(i<n?' done':i===n?' active':'');});"
        "document.getElementById('stepSub').textContent=SUBS[n-1];"
        "if(n===1)startPoll();else stopPoll();}"
        "function setOrient(v){var m=document.getElementById('oMsg');m.className='msg';"
        "post('/wizard/orient','&orient='+v).then(function(r){return r.json();})"
        ".then(function(d){if(d.ok){m.textContent='\\u2713 Saved';m.className='msg ok';"
        "setTimeout(function(){goStep(3);},700);}"
        "else{m.textContent='\\u2717 '+d.error;m.className='msg err';}})"
        "['catch'](function(){m.textContent='\\u2717 Failed';m.className='msg err';});}"
    );
```

(Remove the old `function goStep` and `SUBS` from the original script so there is exactly one definition; set `SUBS` to the 4-entry array: `['Step 1 of 4: Level Reference','Step 2 of 4: Orientation','Step 3 of 4: Audio Settings','Step 4 of 4: Setup Complete']`. Update step 1's `doZero` success to call `goStep(2)` (orientation) — it already calls `goStep(2)`, which now lands on orientation; update audio `doSaveAudio` success to `goStep(4)`.)

- [ ] **Step 7: Add the `/wizard/orient` handler.** Add near `http_wizard_zero_post` (line ~2891):

```c
// POST /wizard/orient  (form: csrf_token + orient=0..3)
static esp_err_t http_wizard_orient_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 256) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad len"); return ESP_OK; }
    char *body = (char *)calloc(1, (size_t)total + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem"); return ESP_OK; }
    int rcvd = 0;
    while (rcvd < total) {
        int r = httpd_req_recv(req, body + rcvd, total - rcvd);
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); return ESP_OK; }
        rcvd += r;
    }
    body[rcvd] = '\0';
    if (!csrf_ok(body)) { free(body); httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF"); return ESP_OK; }
    char ov[8] = {0};
    (void)form_get_value(body, "orient", ov, sizeof(ov));
    free(body);
    int o = atoi(ov);
    if (o < 0 || o > 3) o = 0;
    s_lvl_orient = (unsigned char)o;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_LEVELER, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_LVL_ORIENT, (uint8_t)o);
        nvs_commit(h); nvs_close(h);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
```

Register it (near line 3222, beside `wizard_zero`):

```c
    httpd_uri_t wizard_orient = {
        .uri = "/wizard/orient",
        .method = HTTP_POST,
        .handler = http_wizard_orient_post,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &wizard_orient);
```

Add its forward declaration near the other prototypes (line ~870):

```c
static esp_err_t http_wizard_orient_post(httpd_req_t *req);
```

- [ ] **Step 8: Build**

Run: `source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh && idf.py build`
Expected: succeeds — all three of imu_task's setters now resolve; full firmware links.

- [ ] **Step 9: Commit**

```bash
git add main/wifi_mgr.h main/wifi_mgr.c
git commit -m "feat(wifi_mgr): status guidance card, mode endpoint, wizard orientation step"
```

---

## Task 11: Docs + on-device verification

**Files:**
- Modify: `docs/rest-sensors.md`, `docs/mqtt-discovery.md`

- [ ] **Step 1: Document the new `/status.json` fields** in `docs/rest-sensors.md`: `lvl_mode`, `guidance_available`, `is_level`, `lift_fl/fr/rl/rr`, `worst_corner`, `ramp_axis_is_roll`, `ramp_lift_left`, `ramp_lift_front`, `ramp_remaining_in`. Add a short paragraph and a sample JSON snippet.

- [ ] **Step 2: Document the new HA entities** in `docs/mqtt-discovery.md`: 4 corner-lift sensors (`lift_fl/fr/rl/rr`, unit `in`) and the `is_level` binary_sensor. Note the state JSON keys they read.

- [ ] **Step 3: Commit docs**

```bash
git add docs/rest-sensors.md docs/mqtt-discovery.md
git commit -m "docs: document leveling-guidance REST fields and HA entities"
```

- [ ] **Step 4: Re-run host unit tests (regression)**

Run: `cc -std=c11 -Imain -o /tmp/test_leveling test/leveling/test_leveling.c main/leveling.c -lm && /tmp/test_leveling`
Expected: `ALL TESTS PASSED`

- [ ] **Step 5: Flash and verify on-device**

Run: `source /Users/cjacob/esp/v5.5.1/esp-idf/export.sh && idf.py build && bash tools/flash_remote.sh`
Then verify:
- Wizard `/wizard` now has 4 steps; the orientation step saves and advances.
- On the device, the new top-left button opens the guidance screen; Blocks/Ramps toggle works; back button returns to the bubble.
- Tilt the device: low corners show inches (Blocks); the `▲ FRONT` direction matches the wizard choice. **If FRONT points the wrong way or left/right are swapped, flip the offending row's signs in `leveling_orient_from_front()` (leveling.c) and re-run the host tests + rebuild.**
- Web `/status` guidance card mirrors the device; the mode toggle button switches both.
- In Home Assistant, the four `LevelUp Lift *` sensors and `LevelUp Is Level` binary_sensor appear and update.

- [ ] **Step 6: Final commit (if sign fixes were needed)**

```bash
git add main/leveling.c
git commit -m "fix(leveling): correct orientation signs verified on-device"
```

---

## Self-Review

**Spec coverage:**
- Pure `leveling` module (A1) — Tasks 1-5. ✓
- Block-mode "raise low wheels to highest corner" — Task 2. ✓
- Ramp-mode dominant-axis collapse — Task 3. ✓
- `is_level` threshold + zero-dimension guard — Tasks 1,2,4. ✓
- Orientation mapping from 4-way wizard answer — Tasks 1,10. ✓
- imu_task single producer fan-out — Task 7. ✓
- On-device guidance screen + toggle + Blocks/Ramps — Task 9. ✓
- Web `/status.json` + status card + mode control — Task 10. ✓
- MQTT corner sensors + `is_level` binary_sensor + `configuration_url` (already present in code) — Task 8. ✓
- NVS `lvl_orient` + `lvl_mode` — Task 6. ✓
- Host unit tests — Tasks 1-4, regression in Task 11. ✓
- Docs — Task 11. ✓
- Future air-leveling seam — preserved (result struct + read-only MQTT). ✓

**Placeholder scan:** No TBD/TODO; every code step has concrete code. The on-device sign-flip note (Task 11 Step 5) is a real calibration step, not a placeholder.

**Type consistency:** `leveling_result_t`, `corner_t`, `leveling_orient_t`, `leveling_front_t`, `leveling_mode_t` used identically across module, imu_task, ui, wifi_mgr, mqtt_mgr. Setters named `*_update_guidance(const leveling_result_t *)` everywhere. `wifi_mgr_get_mode/set_mode/get_orient` consistent between header and call sites.

**Note on build ordering:** Task 7 intentionally leaves the firmware non-linking (calls setters defined in Tasks 8-10). If strict per-task green builds are required, implement Tasks 8, 9, 10 immediately after 7 before expecting a successful `idf.py build`; the host unit tests (Tasks 1-4) are independently green at each of their commits.
