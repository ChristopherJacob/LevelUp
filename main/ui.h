#pragma once
#include <stdint.h>
#include "leveling.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build the LVGL scene graph and initialize display values.
void ui_init(void);
// Update the bubble position and status colors from raw IMU angles.
void ui_set_angles(float roll_deg, float pitch_deg);
// Show splash screen for a fixed duration.
void ui_show_splash(uint32_t duration_ms);
// Apply stored zero offsets to raw angles.
void ui_apply_offsets(float raw_roll_deg, float raw_pitch_deg, float *adj_roll_deg, float *adj_pitch_deg);
// Update numeric inches readout.
void ui_set_inches(float roll_in, float pitch_in);
// Set current raw orientation as the new zero reference.
void ui_zero_current(void);
// Show hold-to-zero progress (0.0-1.0) using the bottom readout bar.
void ui_zero_hold_progress(float frac);
// Hide hold-to-zero progress if a hold is canceled.
void ui_zero_hold_cancel(void);
// Show completed hold-to-zero feedback briefly in the center.
void ui_zero_hold_complete(void);
// Toggle mute and refresh icon/state feedback.
void ui_toggle_mute(void);
// Toggle mute without writing to NVS (for fast hardware button path).
void ui_toggle_mute_runtime(void);
// Show a persistent rollback warning banner (OTA new firmware failed, reverted).
void ui_show_rollback_warning(void);
// Persist current mute/volume to NVS (called by HTTP wizard handler after updating state).
void ui_save_audio_prefs(void);
// Update the guidance screen from the latest leveling result.
void ui_update_guidance(const leveling_result_t *g);

#ifdef __cplusplus
}
#endif
