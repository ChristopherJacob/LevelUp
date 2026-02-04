#pragma once

#include "esp_err.h"
#include <stdbool.h>

// Initialize MQTT subsystem and start publish task if enabled.
esp_err_t mqtt_mgr_init(void);
// Update latest filtered angles for publish loop.
void mqtt_mgr_update_angles(float roll_deg, float pitch_deg);
// Update vehicle dimensions used for inches conversion.
void mqtt_mgr_set_vehicle_config(float wheelbase_in, float trackwidth_in);
// Update latest accel values for diagnostics.
void mqtt_mgr_update_accel(float ax, float ay, float az);
esp_err_t mqtt_mgr_restart(void);
// Current MQTT transport state.
bool mqtt_mgr_is_connected(void);
