#pragma once

#include "esp_err.h"

esp_err_t mqtt_mgr_init(void);
void mqtt_mgr_update_angles(float roll_deg, float pitch_deg);
