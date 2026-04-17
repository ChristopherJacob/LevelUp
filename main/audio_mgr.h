#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize ES8311 + I2S and start beep scheduler task.
esp_err_t audio_mgr_init(void);

// Update latest filtered relative angles used for beep cadence.
void audio_mgr_update_angles(float roll_deg, float pitch_deg);
// Hint whether device is stationary; moving state avoids "perfect-level" fast beeps.
void audio_mgr_set_stationary(bool stationary);

// Mute control used by UI toggle.
void audio_mgr_set_muted(bool muted);
bool audio_mgr_is_muted(void);

// Runtime enable (used to defer beeps until splash is gone).
void audio_mgr_set_enabled(bool enabled);
bool audio_mgr_is_enabled(void);

// Output volume percent (0-100).
void audio_mgr_set_volume(int volume_pct);
int audio_mgr_get_volume(void);

// Request an immediate one-shot beep (e.g. from calibration wizard test).
// Fires on the next beep-task tick regardless of cadence timer; respects mute.
void audio_mgr_request_beep(void);

#ifdef __cplusplus
}
#endif
