// IMU sampling, filtering, and publishing to UI/telemetry.
#include "imu_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "qmi8658.h"
#include "i2c_bus.h"
#include "lvgl_port.h"
#include "ui.h"
#include "wifi_mgr.h"   // <-- ADD THIS
#include "mqtt_mgr.h"
#include "audio_mgr.h"
#include "leveling.h"

#include <math.h>

static const char *TAG = "imu";
static portMUX_TYPE s_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_update_us = 0;
static bool s_diag_healthy = false;
static bool s_diag_stationary = false;

// Math helpers for angle conversion and clamping.
static float rad2deg(float r) { return r * (180.0f / (float)M_PI); }
static float clampf(float v, float lo, float hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }

#define ROLL_CLAMP_DEG  20.0f
#define PITCH_CLAMP_DEG 23.0f
// Hysteresis deadband: enter-zero threshold and wider exit threshold.
#define ROLL_DB_ENTER_DEG   0.8f
#define ROLL_DB_EXIT_DEG    1.2f
#define PITCH_DB_ENTER_DEG  0.8f
#define PITCH_DB_EXIT_DEG   1.2f

// Stationary detector on accel magnitude (m/s^2), with hysteresis.
#define G_MPS2                 9.80665f
#define STATIONARY_WIN         40
#define STATIONARY_MIN_SAMPLES 20
#define ST_VAR_ENTER           0.0100f   // stddev ~= 0.10
#define ST_VAR_EXIT            0.0324f   // stddev ~= 0.18
#define ST_G_ERR_ENTER         0.20f
#define ST_G_ERR_EXIT          0.45f

static float apply_deadband_hyst(float v, float enter_deg, float exit_deg, bool stationary, bool *latched_zero)
{
    if (!latched_zero) return v;

    float av = fabsf(v);
    if (*latched_zero) {
        if (av >= exit_deg) {
            *latched_zero = false;
        } else {
            return 0.0f;
        }
    }

    // Only snap into "exact zero" when motion is settled.
    if (stationary && av <= enter_deg) {
        *latched_zero = true;
        return 0.0f;
    }

    return v;
}

#define MEDIAN_WIN 5
static float median5(const float *v, int n)
{
    float tmp[MEDIAN_WIN];
    for (int i = 0; i < n; i++) tmp[i] = v[i];
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (tmp[j] < tmp[i]) {
                float t = tmp[i];
                tmp[i] = tmp[j];
                tmp[j] = t;
            }
        }
    }
    return tmp[n / 2];
}

// Simple accel-based tilt (bubble-level), using gravity only.
static void accel_to_roll_pitch_deg(float ax, float ay, float az, float *roll, float *pitch)
{
    // Use -az so gravity is positive at rest (device reports ~-9.8 m/s^2 on Z)
    float az_up = -az;
    *roll  = rad2deg(atan2f(ay, az_up));
    *pitch = rad2deg(atan2f(-ax, sqrtf(ay * ay + az_up * az_up)));
}

// Initialize IMU over I2C with the desired ranges/ODR.
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
        return;
    }

    qmi8658_dev_t dev = {0};
    qmi8658_data_t data = {0};

    ESP_ERROR_CHECK(imu_init_on_bus(bus, &dev));
    ESP_LOGI(TAG, "IMU init OK");
    portENTER_CRITICAL(&s_diag_mux);
    s_diag_healthy = true;
    s_last_update_us = esp_timer_get_time();
    s_diag_stationary = false;
    portEXIT_CRITICAL(&s_diag_mux);

    // Smoothing pipeline: median(5) to remove spikes, then time-based EMA.
    const float tau_sec = 0.5f; // larger = more smoothing
    float roll_f = 0.0f, pitch_f = 0.0f;
    int64_t last_us = esp_timer_get_time();

    // Median filter buffers (small window to kill single-sample spikes).
    float roll_buf[MEDIAN_WIN] = {0};
    float pitch_buf[MEDIAN_WIN] = {0};
    int buf_idx = 0;
    int buf_count = 0;
    bool roll_zero_latched = false;
    bool pitch_zero_latched = false;

    // Running window stats for stationary detection.
    float amag_buf[STATIONARY_WIN] = {0};
    int amag_idx = 0;
    int amag_count = 0;
    float amag_sum = 0.0f;
    float amag_sumsq = 0.0f;
    bool is_stationary = false;

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
                consecutive_errs++;
                ESP_LOGW(TAG, "i2c lock timeout reading sensor [%d]", consecutive_errs);
            }

            if (err == ESP_OK) {
                consecutive_errs = 0;

                float roll, pitch;
                accel_to_roll_pitch_deg(data.accelX, data.accelY, data.accelZ, &roll, &pitch);

                // Update stationary detector from accel magnitude variance.
                float amag = sqrtf(data.accelX * data.accelX +
                                   data.accelY * data.accelY +
                                   data.accelZ * data.accelZ);
                if (amag_count == STATIONARY_WIN) {
                    float old = amag_buf[amag_idx];
                    amag_sum -= old;
                    amag_sumsq -= old * old;
                } else {
                    amag_count++;
                }
                amag_buf[amag_idx] = amag;
                amag_sum += amag;
                amag_sumsq += amag * amag;
                amag_idx = (amag_idx + 1) % STATIONARY_WIN;

                if (amag_count >= STATIONARY_MIN_SAMPLES) {
                    float mean = amag_sum / (float)amag_count;
                    float var = (amag_sumsq / (float)amag_count) - (mean * mean);
                    if (var < 0.0f) var = 0.0f;
                    float g_err = fabsf(mean - G_MPS2);

                    bool enter_ok = (var <= ST_VAR_ENTER) && (g_err <= ST_G_ERR_ENTER);
                    bool exit_bad = (var >= ST_VAR_EXIT) || (g_err >= ST_G_ERR_EXIT);

                    bool prev_stationary = is_stationary;
                    if (!is_stationary && enter_ok) {
                        is_stationary = true;
                    } else if (is_stationary && exit_bad) {
                        is_stationary = false;
                    }
                    if (prev_stationary != is_stationary) {
                        ESP_LOGI(TAG, "stationary=%s var=%.4f g_err=%.3f",
                                 is_stationary ? "true" : "false", var, g_err);
                    }
                }
                portENTER_CRITICAL(&s_diag_mux);
                s_last_update_us = esp_timer_get_time();
                s_diag_stationary = is_stationary;
                s_diag_healthy = true;
                portEXIT_CRITICAL(&s_diag_mux);

                // Median filter to knock out spikes.
                roll_buf[buf_idx] = roll;
                pitch_buf[buf_idx] = pitch;
                buf_idx = (buf_idx + 1) % MEDIAN_WIN;
                if (buf_count < MEDIAN_WIN) buf_count++;

                float roll_med = median5(roll_buf, buf_count);
                float pitch_med = median5(pitch_buf, buf_count);

                // EMA filter (time-based for consistent feel).
                int64_t now_us = esp_timer_get_time();
                float dt = (float)(now_us - last_us) / 1000000.0f;
                if (dt < 0.001f) dt = 0.001f;
                if (dt > 0.2f) dt = 0.2f;
                last_us = now_us;
                float alpha = 1.0f - expf(-dt / tau_sec);
                roll_f  = (1.0f - alpha) * roll_f  + alpha * roll_med;
                pitch_f = (1.0f - alpha) * pitch_f + alpha * pitch_med;

                // Update UI (LVGL lock required).
                if (lvgl_port_lock(10)) {
                    ui_set_angles(roll_f, pitch_f);

                    // Apply zero offsets, then clamp and deadband for stable UX.
                    float roll_rel = roll_f;
                    float pitch_rel = pitch_f;
                    ui_apply_offsets(roll_f, pitch_f, &roll_rel, &pitch_rel);

                    roll_rel = clampf(roll_rel, -ROLL_CLAMP_DEG, ROLL_CLAMP_DEG);
                    pitch_rel = clampf(pitch_rel, -PITCH_CLAMP_DEG, PITCH_CLAMP_DEG);
                    roll_rel = apply_deadband_hyst(roll_rel, ROLL_DB_ENTER_DEG, ROLL_DB_EXIT_DEG,
                                                   is_stationary, &roll_zero_latched);
                    pitch_rel = apply_deadband_hyst(pitch_rel, PITCH_DB_ENTER_DEG, PITCH_DB_EXIT_DEG,
                                                    is_stationary, &pitch_zero_latched);

                    // Convert to inches using vehicle dimensions.
                    float wheelbase_in = wifi_mgr_get_wheelbase_in();
                    float trackwidth_in = wifi_mgr_get_trackwidth_in();
                    float roll_in = tanf(roll_rel * (float)M_PI / 180.0f) * trackwidth_in;
                    float pitch_in = tanf(pitch_rel * (float)M_PI / 180.0f) * wheelbase_in;
                    if (fabsf(roll_in) < 0.02f) roll_in = 0.0f;
                    if (fabsf(pitch_in) < 0.02f) pitch_in = 0.0f;
                    ui_set_inches(roll_in, pitch_in);

                    // Shared leveling guidance (block/ramp) for all surfaces.
                    leveling_orient_t orient =
                        leveling_orient_from_front((leveling_front_t)wifi_mgr_get_orient());
                    leveling_result_t guide =
                        leveling_compute(roll_rel, pitch_rel,
                                         trackwidth_in, wheelbase_in,
                                         (leveling_mode_t)wifi_mgr_get_mode(),
                                         orient);
                    ui_update_guidance(&guide);
                    wifi_mgr_update_guidance(&guide);
                    mqtt_mgr_update_guidance(&guide);

                    // Publish values to web status + MQTT (same filtered data).
                    wifi_mgr_update_angles(roll_rel, pitch_rel);
                    wifi_mgr_update_accel(data.accelX, data.accelY, data.accelZ);
                    mqtt_mgr_update_angles(roll_rel, pitch_rel);
                    mqtt_mgr_update_accel(data.accelX, data.accelY, data.accelZ);
                    audio_mgr_update_angles(roll_rel, pitch_rel);
                    audio_mgr_set_stationary(is_stationary);

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
            portENTER_CRITICAL(&s_diag_mux);
            s_diag_healthy = false;
            portEXIT_CRITICAL(&s_diag_mux);
            consecutive_errs = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            ESP_ERROR_CHECK(imu_init_on_bus(bus, &dev));
            ESP_LOGI(TAG, "IMU re-init OK");
            portENTER_CRITICAL(&s_diag_mux);
            s_diag_healthy = true;
            portEXIT_CRITICAL(&s_diag_mux);
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // 50 Hz UI update
    }
}

esp_err_t imu_task_start(void)
{
    xTaskCreate(imu_task, "imu", 4096, NULL, 5, NULL);
    return ESP_OK;
}

bool imu_task_is_healthy(void)
{
    bool healthy;
    portENTER_CRITICAL(&s_diag_mux);
    healthy = s_diag_healthy;
    portEXIT_CRITICAL(&s_diag_mux);
    return healthy;
}

uint32_t imu_task_ms_since_update(void)
{
    int64_t last_us;
    portENTER_CRITICAL(&s_diag_mux);
    last_us = s_last_update_us;
    portEXIT_CRITICAL(&s_diag_mux);
    if (last_us <= 0) return UINT32_MAX;
    int64_t age_us = esp_timer_get_time() - last_us;
    if (age_us < 0) age_us = 0;
    uint64_t age_ms = (uint64_t)(age_us / 1000LL);
    if (age_ms > UINT32_MAX) return UINT32_MAX;
    return (uint32_t)age_ms;
}

bool imu_task_is_stationary(void)
{
    bool stationary;
    portENTER_CRITICAL(&s_diag_mux);
    stationary = s_diag_stationary;
    portEXIT_CRITICAL(&s_diag_mux);
    return stationary;
}
