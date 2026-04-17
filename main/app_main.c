// Application entrypoint: init subsystems in the correct order.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"

#include "display.h"
#include "lvgl_port.h"
#include "ui.h"
#include "imu_task.h"
#include "wifi_mgr.h"
#include "mqtt_mgr.h"
#include "audio_mgr.h"
#include "button_mgr.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "Leveler LVGL + IMU boot");

    // Check for OTA rollback before marking this firmware valid.
    // If the previously flashed firmware crashed before calling mark_app_valid,
    // the bootloader reverts to this partition and records the failure.
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    bool rolled_back = (invalid != NULL);
    if (rolled_back) {
        ESP_LOGW(TAG, "OTA rollback detected: '%s' did not boot cleanly, reverted",
                 invalid->label);
    }

    // Display panel HW first so LVGL has a target.
    ESP_ERROR_CHECK(display_init());

    // Start Wi-Fi manager BEFORE LVGL allocates large DMA buffers.
    ESP_ERROR_CHECK(wifi_mgr_init());
    ESP_ERROR_CHECK(mqtt_mgr_init());

    // Now bring up LVGL (buffers allocated here).
    ESP_ERROR_CHECK(lvgl_port_init());
    esp_err_t audio_err = audio_mgr_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed (%s), continuing without beeps", esp_err_to_name(audio_err));
    }

    lvgl_port_lock(-1);
    ui_init();
    if (rolled_back) {
        ui_show_rollback_warning();
    }
    lvgl_port_unlock();

    // All subsystems started successfully — commit this firmware as the new valid image.
    // If this is never called (e.g. a crash below), the bootloader will roll back on next boot.
    esp_err_t mark_err = esp_ota_mark_app_valid_cancel_rollback();
    if (mark_err != ESP_OK) {
        ESP_LOGW(TAG, "mark_app_valid failed: %s", esp_err_to_name(mark_err));
    }

    ESP_ERROR_CHECK(button_mgr_init());
    ESP_ERROR_CHECK(imu_task_start());
}
