#pragma once
#include "esp_err.h"
#include <stdbool.h>

// Initialize Wi-Fi, HTTP server, and load persisted settings.
esp_err_t wifi_mgr_init(void);
// Return current STA connection state.
bool wifi_mgr_is_connected(void);

/* Push latest roll/pitch for /status */
void wifi_mgr_update_angles(float roll_deg, float pitch_deg);
// Push latest accel values for diagnostics.
void wifi_mgr_update_accel(float ax, float ay, float az);
// Accessors for vehicle dimensions used in inches conversion.
float wifi_mgr_get_wheelbase_in(void);
float wifi_mgr_get_trackwidth_in(void);

// Leveling orientation/mode settings (values map to leveling.h enums).
unsigned char wifi_mgr_get_orient(void);    // leveling_front_t value (0-3)
unsigned char wifi_mgr_get_mode(void);      // leveling_mode_t value (0=blocks,1=ramps)
void wifi_mgr_set_mode(unsigned char mode); // update + persist Blocks/Ramps
