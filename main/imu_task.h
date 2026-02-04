#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the IMU task (sensor read + filtering + publish).
esp_err_t imu_task_start(void);
// True if IMU loop is initialized and producing fresh samples.
bool imu_task_is_healthy(void);
// Age of latest successful IMU sample in milliseconds.
uint32_t imu_task_ms_since_update(void);
// Current stationary state from accel-variance detector.
bool imu_task_is_stationary(void);

#ifdef __cplusplus
}
#endif
