#include "i2c_bus.h"
#include "board.h"

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_io_expander_tca9554.h" // address constants

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t s_bus = NULL;
static SemaphoreHandle_t s_bus_mux = NULL;

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return s_bus;
}

bool i2c_bus_lock(int timeout_ms)
{
    if (!s_bus_mux) return false;
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_bus_mux, ticks) == pdTRUE;
}

void i2c_bus_unlock(void)
{
    if (s_bus_mux) xSemaphoreGive(s_bus_mux);
}

esp_err_t i2c_bus_init(i2c_master_bus_handle_t *bus_out)
{
    if (!bus_out) return ESP_ERR_INVALID_ARG;

    if (s_bus) { // already init
        *bus_out = s_bus;
        return ESP_OK;
    }

    if (!s_bus_mux) {
        s_bus_mux = xSemaphoreCreateMutex();
        if (!s_bus_mux) return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "i2c_new_master_bus on SDA=%d SCL=%d", PIN_I2C_SDA, PIN_I2C_SCL);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    ESP_LOGI(TAG, "i2c_new_master_bus err=%s handle=%p", esp_err_to_name(err), s_bus);
    if (err != ESP_OK || s_bus == NULL) {
        s_bus = NULL;
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    // ---- Optional self-test: add/remove a device ----
    const uint8_t addrs_to_try[] = {
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,  // 0x20
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_001,  // 0x21
        ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_000, // 0x38
    };

    bool any_added = false;
    for (size_t i = 0; i < sizeof(addrs_to_try) / sizeof(addrs_to_try[0]); i++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = addrs_to_try[i],
            .scl_speed_hz    = 100000,
        };
        i2c_master_dev_handle_t dev = NULL;

        if (!i2c_bus_lock(200)) {
            err = ESP_ERR_TIMEOUT;
            break;
        }

        err = i2c_master_bus_add_device(s_bus, &dev_cfg, &dev);
        if (err == ESP_OK && dev != NULL) {
            // Remove while still holding the lock (keep the test atomic)
            i2c_master_bus_rm_device(dev);
            any_added = true;
        }

        i2c_bus_unlock();

        ESP_LOGI(TAG, "self-test add_device addr=0x%02X err=%s dev=%p",
                 addrs_to_try[i], esp_err_to_name(err), dev);

        if (any_added) break;
    }

    if (!any_added) {
        ESP_LOGW(TAG, "self-test could not add any device; wiring/addr may differ");
        // not fatal
    }

    *bus_out = s_bus;
    return ESP_OK;
}

esp_err_t i2c_bus_deinit(i2c_master_bus_handle_t bus)
{
    if (!bus || bus != s_bus) return ESP_ERR_INVALID_ARG;

    // Ideally ensure nobody holds the bus lock here; for now just delete bus.
    esp_err_t err = i2c_del_master_bus(s_bus);
    if (err == ESP_OK) s_bus = NULL;
    return err;
}
