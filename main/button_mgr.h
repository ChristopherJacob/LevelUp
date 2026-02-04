#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize side-button handling task.
esp_err_t button_mgr_init(void);

#ifdef __cplusplus
}
#endif
