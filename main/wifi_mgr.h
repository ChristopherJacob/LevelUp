#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t wifi_mgr_init(void);
bool wifi_mgr_is_connected(void);

/* Push latest roll/pitch for /status */
void wifi_mgr_update_angles(float roll_deg, float pitch_deg);
