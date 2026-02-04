// Audio beeper: ES8311 output + adaptive cadence from level error.
#include "audio_mgr.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"

#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"

#include "i2c_bus.h"

static const char *TAG = "audio_mgr";

// Pinout from Waveshare ESP32-S3-Touch-AMOLED-1.8 demo.
#define AUDIO_I2S_PORT       I2S_NUM_0
#define AUDIO_I2S_MCLK       GPIO_NUM_16
#define AUDIO_I2S_BCLK       GPIO_NUM_9
#define AUDIO_I2S_WS         GPIO_NUM_45
#define AUDIO_I2S_DOUT       GPIO_NUM_8
#define AUDIO_I2S_DIN        GPIO_NUM_10
#define AUDIO_PA_CTRL        GPIO_NUM_46

#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_MCLK_MULT      384
#define AUDIO_BEEP_HZ        1200
#define AUDIO_BEEP_MS        30
#define AUDIO_BEEP_AMP       4000

// Cadence: close to level => faster beep, farther => slower beep.
#define BEEP_FAST_MS         120
#define BEEP_SLOW_MS         1200
#define BEEP_MAX_ERR_DEG     8.0f
#define BEEP_MOVING_MIN_ERR_DEG 2.0f

static i2s_chan_handle_t s_tx = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
static bool s_init_done = false;
static bool s_muted = false;
static bool s_enabled = true;
static int s_volume_pct = 45;

static portMUX_TYPE s_angle_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_roll = 0.0f;
static float s_pitch = 0.0f;
static bool s_stationary = true;

static int16_t s_beep_buf[(AUDIO_SAMPLE_RATE * AUDIO_BEEP_MS / 1000) * 2];
static size_t s_beep_bytes = 0;

static uint32_t audio_beep_interval_ms(float roll_deg, float pitch_deg, bool stationary)
{
    float e = fmaxf(fabsf(roll_deg), fabsf(pitch_deg));
    // While moving, avoid the ultra-fast "centered" cadence.
    if (!stationary && e < BEEP_MOVING_MIN_ERR_DEG) {
        e = BEEP_MOVING_MIN_ERR_DEG;
    }
    float norm = e / BEEP_MAX_ERR_DEG;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    return (uint32_t)(BEEP_FAST_MS + norm * (float)(BEEP_SLOW_MS - BEEP_FAST_MS));
}

static void audio_build_beep_buffer(void)
{
    const int frames = (AUDIO_SAMPLE_RATE * AUDIO_BEEP_MS) / 1000;
    const int period = AUDIO_SAMPLE_RATE / AUDIO_BEEP_HZ;
    for (int i = 0; i < frames; i++) {
        int16_t s = ((i % period) < (period / 2)) ? AUDIO_BEEP_AMP : -AUDIO_BEEP_AMP;
        s_beep_buf[i * 2 + 0] = s;
        s_beep_buf[i * 2 + 1] = s;
    }
    s_beep_bytes = (size_t)frames * 2U * sizeof(int16_t);
}

static esp_err_t audio_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK,
            .bclk = AUDIO_I2S_BCLK,
            .ws = AUDIO_I2S_WS,
            .dout = AUDIO_I2S_DOUT,
            .din = AUDIO_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable failed");
    return ESP_OK;
}

static esp_err_t audio_codec_init(void)
{
    i2c_master_bus_handle_t i2c_bus = i2c_bus_get_handle();
    ESP_RETURN_ON_FALSE(i2c_bus, ESP_ERR_INVALID_STATE, TAG, "i2c bus not initialized");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "audio i2c ctrl create failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = NULL,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "audio i2s data create failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "audio gpio if create failed");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = AUDIO_PA_CTRL,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
        .mclk_div = AUDIO_MCLK_MULT,
    };
    const audio_codec_if_t *es_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(es_if, ESP_FAIL, TAG, "es8311 if create failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "codec dev create failed");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = AUDIO_SAMPLE_RATE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "codec open failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_codec, s_volume_pct) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "codec volume set failed");
    return ESP_OK;
}

static void audio_beep_task(void *arg)
{
    (void)arg;
    int64_t next_beep_us = esp_timer_get_time();
    while (1) {
        float roll, pitch;
        bool muted, enabled, stationary;
        portENTER_CRITICAL(&s_angle_mux);
        roll = s_roll;
        pitch = s_pitch;
        muted = s_muted;
        enabled = s_enabled;
        stationary = s_stationary;
        portEXIT_CRITICAL(&s_angle_mux);

        if (enabled && !muted && s_tx && s_beep_bytes > 0) {
            int64_t now = esp_timer_get_time();
            if (now >= next_beep_us) {
                size_t written = 0;
                (void)i2s_channel_write(s_tx, s_beep_buf, s_beep_bytes, &written, 50);
                uint32_t interval_ms = audio_beep_interval_ms(roll, pitch, stationary);
                next_beep_us = now + (int64_t)interval_ms * 1000LL;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t audio_mgr_init(void)
{
    if (s_init_done) return ESP_OK;
    audio_build_beep_buffer();
    ESP_RETURN_ON_ERROR(audio_i2s_init(), TAG, "audio i2s init failed");
    ESP_RETURN_ON_ERROR(audio_codec_init(), TAG, "audio codec init failed");
    xTaskCreate(audio_beep_task, "audio_beep", 4096, NULL, 4, NULL);
    s_init_done = true;
    ESP_LOGI(TAG, "audio init OK");
    return ESP_OK;
}

void audio_mgr_update_angles(float roll_deg, float pitch_deg)
{
    portENTER_CRITICAL(&s_angle_mux);
    s_roll = roll_deg;
    s_pitch = pitch_deg;
    portEXIT_CRITICAL(&s_angle_mux);
}

void audio_mgr_set_stationary(bool stationary)
{
    portENTER_CRITICAL(&s_angle_mux);
    s_stationary = stationary;
    portEXIT_CRITICAL(&s_angle_mux);
}

void audio_mgr_set_muted(bool muted)
{
    portENTER_CRITICAL(&s_angle_mux);
    s_muted = muted;
    portEXIT_CRITICAL(&s_angle_mux);
}

bool audio_mgr_is_muted(void)
{
    bool muted;
    portENTER_CRITICAL(&s_angle_mux);
    muted = s_muted;
    portEXIT_CRITICAL(&s_angle_mux);
    return muted;
}

void audio_mgr_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_angle_mux);
    s_enabled = enabled;
    portEXIT_CRITICAL(&s_angle_mux);
}

bool audio_mgr_is_enabled(void)
{
    bool enabled;
    portENTER_CRITICAL(&s_angle_mux);
    enabled = s_enabled;
    portEXIT_CRITICAL(&s_angle_mux);
    return enabled;
}

void audio_mgr_set_volume(int volume_pct)
{
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;
    portENTER_CRITICAL(&s_angle_mux);
    s_volume_pct = volume_pct;
    portEXIT_CRITICAL(&s_angle_mux);
    if (s_codec) {
        (void)esp_codec_dev_set_out_vol(s_codec, volume_pct);
    }
}

int audio_mgr_get_volume(void)
{
    int volume_pct;
    portENTER_CRITICAL(&s_angle_mux);
    volume_pct = s_volume_pct;
    portEXIT_CRITICAL(&s_angle_mux);
    return volume_pct;
}
