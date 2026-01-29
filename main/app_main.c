#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "display.h"
#include "lvgl_port.h"
#include "ui.h"
#include "imu_task.h"
#include "wifi_mgr.h"
#include "mqtt_mgr.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "Leveler LVGL + IMU boot");

    // Display panel HW first (OK)
    ESP_ERROR_CHECK(display_init());

    // Start Wi-Fi manager BEFORE LVGL allocates large DMA buffers
    ESP_ERROR_CHECK(wifi_mgr_init());
    ESP_ERROR_CHECK(mqtt_mgr_init());

    // Now bring up LVGL (buffers allocated here)
    ESP_ERROR_CHECK(lvgl_port_init());

    lvgl_port_lock(-1);
    ui_init();
    lvgl_port_unlock();

    ESP_ERROR_CHECK(imu_task_start());
}
