// Application entrypoint: init subsystems in the correct order.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

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
    lvgl_port_unlock();

    ESP_ERROR_CHECK(button_mgr_init());
    ESP_ERROR_CHECK(imu_task_start());
}
