# Leveling Guidance — Design Spec

**Date:** 2026-06-16
**Status:** Approved (design) — ready for implementation plan
**Component:** new `main/leveling.*` + changes to `imu_task`, `ui`, `wifi_mgr`, `mqtt_mgr`

## Problem

Today LevelUp shows the *tilt* (roll/pitch in degrees and inches) but not what to **do**
about it. The genuinely useful output for a van is: which wheel(s) to raise and by how
much (blocks), or which way to drive onto ramps until level. This feature turns the
device from a bubble level into a tool that tells the user the answer.

## Goals

- Compute, from the current tilt + vehicle dimensions, an actionable per-corner
  leveling correction.
- Support two physical leveling methods, user-selectable: **leveling blocks** (lift
  individual low wheels) and **drive-up ramps** (one axis at a time).
- Surface the guidance on three surfaces: the on-device AMOLED display, the web
  dashboard, and MQTT / Home Assistant.
- Keep the correction math in a pure, reusable module so a **future air-leveling
  controller** (airbags + WiFi-controlled compressor) can consume the same result to
  close the loop automatically.

## Non-goals (v1)

- Metric / cm units toggle on the device (HA already receives both deg and in).
- MQTT command/select to change Blocks/Ramps mode (MQTT stays read-only in v1).
- Gyro sensor fusion (accel-only stays as-is).
- Auto-switching the on-device view (manual toggle only).
- Air-leveling closed-loop control (only the data seam is left ready for it).

## Architecture (A1: standalone pure module)

New module **`main/leveling.c` / `main/leveling.h`** — no hardware, LVGL, or NVS
dependencies; pure math + structs, host-unit-testable.

```c
typedef enum { LEVEL_MODE_BLOCKS, LEVEL_MODE_RAMPS } leveling_mode_t;
typedef enum { CORNER_FL, CORNER_FR, CORNER_RL, CORNER_RR } corner_t;

// Orientation mapping derived from the wizard "which way is front" answer.
typedef struct {
    bool  front_is_pitch_axis;  // true: van front/back is along the pitch axis
    float front_sign;           // +1/-1: which sign of that axis points to the front
    float left_sign;            // +1/-1: which sign of the other axis points left
} leveling_orient_t;

typedef struct {
    bool     guidance_available; // false if vehicle dims unset/zero
    float    corner_lift_in[4];  // inches to raise each wheel (CORNER_* index); highest corner = 0
    corner_t worst_corner;       // largest lift — act on first
    float    max_lift_in;        // worst-corner magnitude

    // Ramp mode (dominant single axis):
    bool     ramp_lift_left;     // ramp goes under the left wheels (else right)
    bool     ramp_lift_front;    // ramp goes under the front wheels (else rear)
    bool     ramp_axis_is_roll;  // dominant axis is roll (side) vs pitch (end)
    float    ramp_target_in;     // ramp height needed
    float    ramp_remaining_in;  // live distance-to-level as they drive up

    // Shared:
    bool     is_level;           // both axes within LEVEL_GOOD threshold
} leveling_result_t;

leveling_result_t leveling_compute(float roll_rel_deg, float pitch_rel_deg,
                                   float trackwidth_in, float wheelbase_in,
                                   leveling_mode_t mode,
                                   const leveling_orient_t *orient);
```

### Data flow

```
imu_task (50 Hz loop, existing)
  produces filtered roll_rel / pitch_rel  (imu_task.c:312-315 area)
      |
      v
  leveling_compute(roll_rel, pitch_rel, trackwidth_in, wheelbase_in, mode, orient)
      |  -> leveling_result_t
      +--> ui_update_guidance(&res)        (under LVGL lock, like ui_set_inches)
      +--> wifi_mgr_update_guidance(&res)   (caches latest for /status + /status.json)
      +--> mqtt_mgr_update_guidance(&res)   (folds into existing state JSON publish)
```

- imu_task remains the single producer; no new task (reuses the existing 50 Hz cycle).
- `leveling.c` is pure logic; ui/web/mqtt are dumb renderers.
- `mode` and `orient` are loaded from NVS by `wifi_mgr` (which already owns
  trackwidth/wheelbase and the web/settings layer) and passed into imu_task's compute
  call via accessors mirroring `wifi_mgr_get_trackwidth_in()` /
  `wifi_mgr_get_wheelbase_in()`.
- The `leveling_result_t` struct is the exact seam a future air-leveling controller
  taps instead of the renderers.

## Math model

Roll and pitch define a tilt plane. Each corner's current relative height:

```
half_track = trackwidth_in / 2     // x: left/right, sign per orient.left_sign
half_base  = wheelbase_in  / 2     // y: front/rear, sign per orient.front_sign
h(corner)  = x_side * tan(roll_rad) + y_end * tan(pitch_rad)
```

with `x_side = ±half_track`, `y_end = ±half_base`, and axis-to-(roll|pitch) assignment
plus signs resolved by `leveling_orient_t` (from the wizard step).

**Blocks mode** — the highest corner is the on-ground reference. For each corner:
`corner_lift_in = max(h) - h(corner)`. Highest corner = 0; others positive. `worst_corner`
= largest lift (place the first/tallest block there). This is the correct "raise the low
wheels to meet the high one" solution.

**Ramps mode** — ramps lift one axis at a time, so collapse to the dominant correction:
compare roll-driven lift (`tan(roll) * trackwidth`) vs pitch-driven lift
(`tan(pitch) * wheelbase`); pick the larger. `ramp_axis_is_roll`, `ramp_lift_left`,
`ramp_lift_front` describe where the ramp goes; `ramp_target_in` is its height;
`ramp_remaining_in` is the live distance-to-level (drops toward 0 as they drive up). The
existing audio beeper cadence (accelerates as |error|→0) doubles as the "almost there…
STOP" cue.

**Level threshold** — reuse the UI's `LEVEL_GOOD_DEG` (0.5°): `is_level` true when both
axes are within it. The existing `<0.02 in` snap (imu_task.c:307-308) kills near-zero
jitter.

**Units** — inches in v1, matching the current readout.

**Edge cases**
- Lifts bounded by the existing ±20° roll / ±23° pitch clamps, which cap the max lift.
- If `trackwidth_in` or `wheelbase_in` is unset/zero → `guidance_available = false`;
  renderers show a "set vehicle dimensions" message instead of bogus numbers.
- Consumers tolerate a stale/zeroed result when the IMU is unhealthy (show "—", not level).

## On-device guidance screen (layout C)

- **Toggle:** a touch button on the main screen (styled like the existing sound button,
  ui.c:625) switches **Bubble view** (default) ⇄ **Guidance view**. In-memory state;
  boots to Bubble view. A **⤢** control on the guidance screen returns to the bubble.
- **Blocks view:** a top-view van graphic; each corner shows its `corner_lift_in`,
  color-coded green/amber/red using the existing dot-color thresholds (ui.c:257); the
  `worst_corner` is emphasized (white ring). Bottom status line: "Raise REAR-LEFT first"
  + a stack list ("RL 3.1\" · RR 2.3\" · FR 0.8\"").
- **Ramps view:** same screen via a **Blocks/Ramps** pill; collapses to the dominant
  axis with a drive-direction arrow + a big live `ramp_remaining_in` countdown and a
  one-line instruction ("Drive RIGHT wheels up ramp").
- **FRONT marker** drawn per the wizard orientation so the graphic matches reality.
- **Level state:** status line shows "Level ✓" in green when `is_level`.

**Implementation:** build the guidance LVGL objects once in `ui_init()`, kept hidden; a
`ui_set_view(view)` shows/hides the bubble group vs guidance group; the Blocks/Ramps pill
writes the shared NVS mode. `ui_update_guidance(const leveling_result_t*)` refreshes
labels/arrow/colors, called from imu_task under the LVGL lock. Art: start with a drawn van
outline (rounded rect + wheel marks); a converted LVGL image based on
`images/ha/van_top.png` can come later (not blocking v1).

## Wizard orientation step

Add one step to the existing `/wizard` flow (wifi_mgr.c, endpoints ~3209-3238), after the
level-reference step: **"Which way is the front of the van?"** — four choices relative to
the screen: Top / Bottom / Left / Right.

- Each choice maps to a `leveling_orient_t` (axis assignment + signs). One 4-way answer
  fully determines all signs (device assumed mounted screen-up, which the level-reference
  step implies).
- New POST handler `/wizard/orient` mirroring `/wizard/zero`; saved to NVS key
  `lvl_orient` (u8 enum) in the `leveler` namespace. Default = "Top is front".
- This step also sets the **default Blocks/Ramps mode** (NVS key `lvl_mode`, u8); the mode
  is also toggleable live on the guidance screen and web dashboard.
- HTML uses `send_chunk` for static blocks; `send_chunkf` only for small substitutions
  (512-byte buffer limit).

## Web dashboard

Extend `/status` and `/status.json` (wifi_mgr.c handlers ~3108/3124).

- **`/status.json`** gains a `guidance` object: `mode`, `guidance_available`, `is_level`,
  `corner_lift_in` (FL/FR/RL/RR), `worst_corner`, `ramp_lift_left`, `ramp_lift_front`,
  `ramp_axis_is_roll`, `ramp_target_in`, `ramp_remaining_in`.
- **`/status` HTML** gains a guidance card: a small CSS/SVG van top-view with the four
  corner numbers (same color coding) mirroring the device Blocks view, plus a one-line
  instruction and a Blocks/Ramps control that writes the shared NVS mode. Page already
  polls `status.json`; render from the new fields. Use `DOMContentLoaded` for elements in
  a later chunk.
- `wifi_mgr` owns the cached latest `leveling_result_t` (set by `wifi_mgr_update_guidance()`)
  plus the `mode` / `orient` NVS settings and accessors.

## MQTT / Home Assistant

Extend `mqtt_mgr.c`, reusing the existing per-device discovery + retained-state pattern.

- **State JSON** gains: `lift_fl`, `lift_fr`, `lift_rl`, `lift_rr` (in), `worst_corner`,
  `is_level`, `ramp_target`, `ramp_remaining`, `mode` — one publish, no new cadence.
- **New discovery entities:**
  - 4 × `sensor` — corner lifts FL/FR/RL/RR (inches, icon `mdi:format-vertical-align-up`).
  - 1 × `binary_sensor` — `is_level` (headline automation hook).
  - Optional, low cost: 1 × text `sensor` for worst corner / readable instruction line.
- **Device `configuration_url`** added to discovery → links the HA device page to the web
  dashboard.
- MQTT is read-only in v1 (Blocks/Ramps select via command topic is a deliberate cut; the
  publisher-only shape leaves room to add a command topic later for air leveling).

## Settings / NVS

`leveler` namespace:
- `lvl_orient` (u8 enum) — default "Top is front".
- `lvl_mode` (u8 Blocks/Ramps) — default Blocks.
- `trackwidth_in`, `wheelbase_in` — already exist.

## Error handling

- Missing/zero vehicle dimensions → `guidance_available = false`; device shows "Set
  vehicle dimensions in /wizard"; web/HA show unavailable, not bogus numbers.
- Lifts clamped via existing ±20°/±23° angle clamps; near-zero snap already in place.
- All consumers tolerate a stale/zeroed result when the IMU is unhealthy.

## Testing

- **Host unit tests for `leveling.c`** (pure, no ESP deps):
  - Known tilts → expected corner lifts and `worst_corner`.
  - Orientation matrix: all 4 front directions produce correct sign mapping.
  - Ramp dominant-axis selection (roll-dominant, pitch-dominant, tie).
  - `is_level` threshold behavior.
  - Zero / unset dimensions → `guidance_available = false`.
- **On-device manual verification:** build, flash via `tools/flash_remote.sh`, tilt the
  device, confirm device screen ⇄ web ⇄ HA agree, and that the wizard orientation
  correctly flips the FRONT marker and corner assignment.

## Future hooks (not implemented)

- **Air leveling:** a controller consumes `leveling_result_t` directly and drives a
  WiFi-controlled compressor/valves to close the loop (drive on, auto-level). Requires an
  MQTT command topic and an actuator module — both layer cleanly onto this design without
  reworking the math or discovery.
- Metric units toggle; gyro sensor fusion; auto-switch view.
