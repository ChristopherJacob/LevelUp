#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lvgl_port_init(void);

/** LVGL is not thread-safe. Use these around any LVGL calls outside the LVGL task. */
bool lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);

#ifdef __cplusplus
}
#endif


