#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create and initialize the I2C master bus (new driver) and run a small self-test.
 *
 * @param[out] bus_out  Returned bus handle.
 * @return ESP_OK on success, otherwise ESP_ERR_*
 */
esp_err_t i2c_bus_init(i2c_master_bus_handle_t *bus_out);

/**
 * @brief Optional: release the bus (not required for your current app)
 */
esp_err_t i2c_bus_deinit(i2c_master_bus_handle_t bus);

/** @brief Get the singleton bus handle (NULL until init). */
i2c_master_bus_handle_t i2c_bus_get_handle(void);

/** @brief Global I2C mutex (shared by IMU + touch + expander). */
bool i2c_bus_lock(int timeout_ms);
void i2c_bus_unlock(void);

#ifdef __cplusplus
}
#endif
