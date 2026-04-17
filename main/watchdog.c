// Software watchdog: monitors that critical tasks are still producing output.
// Reboots if the IMU goes silent (safety-critical); logs warnings for LVGL / MQTT.
#include "watchdog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

#include "imu_task.h"
#include "lvgl_port.h"
#include "mqtt_mgr.h"

static const char *TAG = "watchdog";

// IMU stale threshold: sensor data older than this triggers a reboot.
// The IMU runs at 50 Hz so 10 s of silence means something has gone badly wrong.
#define WDT_IMU_STALE_MS    10000

// LVGL stale threshold: render loop frozen this long triggers an error log.
// We don't reboot — a display glitch shouldn't interrupt an active leveling session.
#define WDT_LVGL_STALE_MS   10000

// MQTT task stale threshold: publish loop frozen this long triggers a warning.
// Non-critical — losing telemetry is recoverable without a reboot.
#define WDT_MQTT_STALE_MS   30000

// How often the watchdog checks each subsystem.
#define WDT_CHECK_INTERVAL_MS 5000

static void watchdog_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "watchdog started (IMU reboot >%ds, LVGL warn >%ds, MQTT warn >%ds)",
             WDT_IMU_STALE_MS / 1000, WDT_LVGL_STALE_MS / 1000, WDT_MQTT_STALE_MS / 1000);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WDT_CHECK_INTERVAL_MS));

        // --- IMU (safety-critical) ---
        uint32_t imu_age = imu_task_ms_since_update();
        if (imu_age != UINT32_MAX && imu_age > WDT_IMU_STALE_MS) {
            ESP_LOGE(TAG, "IMU silent for %u ms — rebooting", imu_age);
            esp_restart();
        }

        // --- LVGL render loop ---
        uint32_t lvgl_age = lvgl_port_ms_since_frame();
        if (lvgl_age != UINT32_MAX && lvgl_age > WDT_LVGL_STALE_MS) {
            ESP_LOGE(TAG, "LVGL render loop silent for %u ms", lvgl_age);
        }

        // --- MQTT publish task ---
        uint32_t mqtt_age = mqtt_mgr_ms_since_alive();
        if (mqtt_age != UINT32_MAX && mqtt_age > WDT_MQTT_STALE_MS) {
            ESP_LOGW(TAG, "MQTT publish task silent for %u ms", mqtt_age);
        }
    }
}

void watchdog_start(void)
{
    // Low priority — this task only needs to wake every few seconds.
    if (xTaskCreate(watchdog_task, "watchdog", 2048, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create watchdog task");
    }
}
