#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Perform the board's power/reset enable sequence via TCA9554.
 *
 * @param[in] bus  I2C bus handle from i2c_new_master_bus()
 */
esp_err_t expander_power_sequence(i2c_master_bus_handle_t bus);

/**
 * @brief Get the initialized expander handle (NULL if unavailable).
 */
esp_io_expander_handle_t expander_get_handle(void);

#ifdef __cplusplus
}
#endif
