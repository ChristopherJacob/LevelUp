#include "imu_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"

#include "qmi8658.h"
#include "i2c_bus.h"
#include "lvgl_port.h"
#include "ui.h"
#include "wifi_mgr.h"   // <-- ADD THIS
#include "mqtt_mgr.h"

#include <math.h>

static const char *TAG = "imu";

static float rad2deg(float r) { return r * (180.0f / (float)M_PI); }

// Simple accel-based tilt (bubble-level)
static void accel_to_roll_pitch_deg(float ax, float ay, float az, float *roll, float *pitch)
{
    *roll  = rad2deg(atan2f(ay, az));
    *pitch = rad2deg(atan2f(-ax, sqrtf(ay * ay + az * az)));
}

static esp_err_t imu_init_on_bus(i2c_master_bus_handle_t bus, qmi8658_dev_t *dev_out)
{
    if (!bus || !dev_out) return ESP_ERR_INVALID_ARG;

    qmi8658_dev_t dev = {0};
    esp_err_t err;

    // Try both possible I2C addresses
    if (!i2c_bus_lock(200)) return ESP_ERR_TIMEOUT;
    err = qmi8658_init(&dev, bus, QMI8658_ADDRESS_HIGH);
    i2c_bus_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Init @ ADDRESS_HIGH failed (%s), trying ADDRESS_LOW", esp_err_to_name(err));

        if (!i2c_bus_lock(200)) return ESP_ERR_TIMEOUT;
        err = qmi8658_init(&dev, bus, QMI8658_ADDRESS_LOW);
        i2c_bus_unlock();
    }
    if (err != ESP_OK) return err;

    // Configure sensor (lock around each I2C transaction)
    if (!i2c_bus_lock(200)) return ESP_ERR_TIMEOUT;
    err = qmi8658_set_accel_range(&dev, QMI8658_ACCEL_RANGE_8G);
    i2c_bus_unlock();
    if (err != ESP_OK) return err;

    if (!i2c_bus_lock(200)) return ESP_ERR_TIMEOUT;
    err = qmi8658_set_accel_odr(&dev, QMI8658_ACCEL_ODR_250HZ);
    i2c_bus_unlock();
    if (err != ESP_OK) return err;

    if (!i2c_bus_lock(200)) return ESP_ERR_TIMEOUT;
    err = qmi8658_set_gyro_range(&dev, QMI8658_GYRO_RANGE_512DPS);
    i2c_bus_unlock();
    if (err != ESP_OK) return err;

    if (!i2c_bus_lock(200)) return ESP_ERR_TIMEOUT;
    err = qmi8658_set_gyro_odr(&dev, QMI8658_GYRO_ODR_250HZ);
    i2c_bus_unlock();
    if (err != ESP_OK) return err;

    // These are void in your qmi8658.h
    qmi8658_set_accel_unit_mps2(&dev, true);
    qmi8658_set_gyro_unit_rads(&dev, true);

    *dev_out = dev;
    return ESP_OK;
}

static void imu_task(void *arg)
{
    (void)arg;

    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus handle is NULL");
        vTaskDelete(NULL);
    }

    qmi8658_dev_t dev = {0};
    qmi8658_data_t data = {0};

    ESP_ERROR_CHECK(imu_init_on_bus(bus, &dev));
    ESP_LOGI(TAG, "IMU init OK");

    // Simple smoothing so the bubble is stable
    const float alpha = 0.20f; // 0..1 (higher = less smoothing)
    float roll_f = 0.0f, pitch_f = 0.0f;

    int consecutive_errs = 0;

    while (1) {
        bool ready = false;
        esp_err_t err;

        // --- is_data_ready() (locked) ---
        if (!i2c_bus_lock(50)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        err = qmi8658_is_data_ready(&dev, &ready);
        i2c_bus_unlock();

        if (err != ESP_OK) {
            consecutive_errs++;
            ESP_LOGW(TAG, "is_data_ready failed (%s) [%d]", esp_err_to_name(err), consecutive_errs);

        } else if (ready) {
            // --- read_sensor_data() (locked) ---
            if (i2c_bus_lock(50)) {
                err = qmi8658_read_sensor_data(&dev, &data);
                i2c_bus_unlock();
            } else {
                err = ESP_ERR_TIMEOUT;
            }

            if (err == ESP_OK) {
                consecutive_errs = 0;

                float roll, pitch;
                accel_to_roll_pitch_deg(data.accelX, data.accelY, data.accelZ, &roll, &pitch);

                // EMA filter
                roll_f  = (1.0f - alpha) * roll_f  + alpha * roll;
                pitch_f = (1.0f - alpha) * pitch_f + alpha * pitch;

                // Update UI (LVGL lock required)
                if (lvgl_port_lock(10)) {
                    ui_set_angles(roll_f, pitch_f);

                    // Publish values to web status page (match UI smoothness)
                    wifi_mgr_update_angles(roll_f, pitch_f);
                    mqtt_mgr_update_angles(roll_f, pitch_f);

                    lvgl_port_unlock();
                }

            } else {
                consecutive_errs++;
                ESP_LOGW(TAG, "read_sensor_data failed (%s) [%d]", esp_err_to_name(err), consecutive_errs);
            }

        } else {
            // Not ready: don't count as an error
            consecutive_errs = 0;
        }

        // If the bus gets wedged, re-init the IMU
        if (consecutive_errs >= 10) {
            ESP_LOGW(TAG, "Too many IMU I2C errors; reinitializing IMU");
            consecutive_errs = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            ESP_ERROR_CHECK(imu_init_on_bus(bus, &dev));
            ESP_LOGI(TAG, "IMU re-init OK");
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // 50 Hz UI update
    }
}

esp_err_t imu_task_start(void)
{
    xTaskCreate(imu_task, "imu", 4096, NULL, 5, NULL);
    return ESP_OK;
}
