#include "expander.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

#include "esp_io_expander_tca9554.h"

static const char *TAG = "expander";

// Keep one handle for lifetime (so we don't leak and so we can reuse later)
static esp_io_expander_handle_t s_ex = NULL;

static esp_err_t expander_try_init(i2c_master_bus_handle_t bus)
{
    // Try TCA9554 addresses 0x20..0x27
    for (uint32_t addr = ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000;
         addr <= ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_111;
         addr++) {

        esp_io_expander_handle_t ex = NULL;
        esp_err_t err = esp_io_expander_new_i2c_tca9554(bus, addr, &ex);
        if (err == ESP_OK && ex) {
            ESP_LOGI(TAG, "Found TCA9554 at 0x%02X", (unsigned)addr);
            s_ex = ex;
            return ESP_OK;
        }
    }

    // Try TCA9554A addresses 0x38..0x3F
    for (uint32_t addr = ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_000;
         addr <= ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_111;
         addr++) {

        esp_io_expander_handle_t ex = NULL;
        esp_err_t err = esp_io_expander_new_i2c_tca9554(bus, addr, &ex);
        if (err == ESP_OK && ex) {
            ESP_LOGI(TAG, "Found TCA9554A at 0x%02X", (unsigned)addr);
            s_ex = ex;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t expander_power_sequence(i2c_master_bus_handle_t bus)
{
    if (!bus) return ESP_ERR_INVALID_ARG;

    // If already initialized, reuse it
    if (!s_ex) {
        ESP_LOGI(TAG, "init IO expander");
        esp_err_t err = expander_try_init(bus);
        ESP_RETURN_ON_ERROR(err, TAG, "no TCA9554/TCA9554A found on I2C bus");
    }

    // Pins 0/1/2 as outputs, then toggle low -> high (matches demo power/reset sequence)
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(s_ex,
                                IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2,
                                IO_EXPANDER_OUTPUT),
        TAG, "tca9554 set_dir failed"
    );

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_ex, IO_EXPANDER_PIN_NUM_0, 0), TAG, "ex pin0 low");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_ex, IO_EXPANDER_PIN_NUM_1, 0), TAG, "ex pin1 low");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_ex, IO_EXPANDER_PIN_NUM_2, 0), TAG, "ex pin2 low");

    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_ex, IO_EXPANDER_PIN_NUM_0, 1), TAG, "ex pin0 high");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_ex, IO_EXPANDER_PIN_NUM_1, 1), TAG, "ex pin1 high");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_ex, IO_EXPANDER_PIN_NUM_2, 1), TAG, "ex pin2 high");

    return ESP_OK;
}

esp_io_expander_handle_t expander_get_handle(void)
{
    return s_ex;
}
