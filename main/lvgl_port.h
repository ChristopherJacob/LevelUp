#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lvgl_port_init(void);

/** LVGL is not thread-safe. Use these around any LVGL calls outside the LVGL task. */
bool lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);

/** Configure inactivity timeout in milliseconds (0 disables auto-blank). */
void lvgl_port_set_screen_timeout_ms(uint32_t timeout_ms);
/** Force the display panel power state. */
void lvgl_port_set_screen_on(bool on);
/** Query current display panel power state. */
bool lvgl_port_is_screen_on(void);

#ifdef __cplusplus
}
#endif
