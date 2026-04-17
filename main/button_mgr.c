#include "button_mgr.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"
#include "expander.h"
#include "i2c_bus.h"
#include "lvgl_port.h"
#include "ui.h"

static const char *TAG = "button_mgr";

// Waveshare board notes: BOOT on GPIO0 (active-low), PWR on EXIO4 (active-high).
#define BTN_BOOT_GPIO        GPIO_NUM_0
#define BTN_PWR_EXIO_MASK    IO_EXPANDER_PIN_NUM_4

#define BTN_POLL_MS          25
#define BTN_DEBOUNCE_MS      70
#define BTN_LONG_MS          1000

typedef struct {
    bool raw_last;
    bool stable;
    int64_t last_change_us;
    int64_t press_start_us;
    bool long_fired;
} btn_state_t;

static btn_state_t s_boot = {0};
static btn_state_t s_pwr = {0};
static bool s_started = false;
static int s_pwr_progress_bucket = -1;

static bool read_boot_pressed(void)
{
    return gpio_get_level(BTN_BOOT_GPIO) == 0;
}

static bool read_pwr_pressed(void)
{
    esp_io_expander_handle_t ex = expander_get_handle();
    if (!ex) return false;

    if (!i2c_bus_lock(50)) return false;
    uint32_t level = 0;
    esp_err_t err = esp_io_expander_get_level(ex, BTN_PWR_EXIO_MASK, &level);
    i2c_bus_unlock();
    if (err != ESP_OK) return false;
    return (level & BTN_PWR_EXIO_MASK) != 0;
}

static void handle_press_logic(btn_state_t *st, bool pressed,
                               void (*on_short)(void), void (*on_long)(void))
{
    int64_t now = esp_timer_get_time();

    if (pressed != st->raw_last) {
        st->raw_last = pressed;
        st->last_change_us = now;
    }

    if ((now - st->last_change_us) < (BTN_DEBOUNCE_MS * 1000LL)) {
        return;
    }

    if (st->stable != st->raw_last) {
        st->stable = st->raw_last;
        if (st->stable) {
            st->press_start_us = now;
            st->long_fired = false;
        } else if (!st->long_fired && on_short) {
            on_short();
        }
    }

    if (st->stable && !st->long_fired &&
        (now - st->press_start_us) >= (BTN_LONG_MS * 1000LL)) {
        st->long_fired = true;
        if (on_long) on_long();
    }
}

static void on_boot_short(void)
{
    // BOOT short-press toggles mute/unmute.
    if (lvgl_port_lock(30)) {
        ui_toggle_mute_runtime();
        lvgl_port_unlock();
    }
}

static void on_pwr_short(void)
{
    // PWR short-press toggles display blank/wake.
    lvgl_port_set_screen_on(!lvgl_port_is_screen_on());
}

static void on_pwr_long(void)
{
    // PWR long-press sets current orientation as zero.
    if (lvgl_port_lock(30)) {
        ui_zero_current();
        lvgl_port_unlock();
    }
}

static void process_pwr_button(bool pressed)
{
    int64_t now = esp_timer_get_time();

    if (pressed != s_pwr.raw_last) {
        s_pwr.raw_last = pressed;
        s_pwr.last_change_us = now;
    }

    if ((now - s_pwr.last_change_us) < (BTN_DEBOUNCE_MS * 1000LL)) {
        return;
    }

    if (s_pwr.stable != s_pwr.raw_last) {
        s_pwr.stable = s_pwr.raw_last;
        if (s_pwr.stable) {
            s_pwr.press_start_us = now;
            s_pwr.long_fired = false;
            s_pwr_progress_bucket = 0;
            if (lvgl_port_lock(30)) {
                ui_zero_hold_progress(0.0f);
                lvgl_port_unlock();
            }
        } else {
            if (!s_pwr.long_fired) {
                on_pwr_short();
            }
            if (lvgl_port_lock(30)) {
                ui_zero_hold_cancel();
                lvgl_port_unlock();
            }
            s_pwr_progress_bucket = -1;
        }
    }

    if (s_pwr.stable && !s_pwr.long_fired) {
        int64_t held_us = now - s_pwr.press_start_us;
        float frac = (float)held_us / (float)(BTN_LONG_MS * 1000LL);
        if (frac > 1.0f) frac = 1.0f;

        int bucket = (int)(frac * 10.0f); // 10% steps
        if (bucket != s_pwr_progress_bucket) {
            s_pwr_progress_bucket = bucket;
            if (lvgl_port_lock(30)) {
                ui_zero_hold_progress(frac);
                lvgl_port_unlock();
            }
        }

        if (held_us >= (BTN_LONG_MS * 1000LL)) {
            s_pwr.long_fired = true;
            on_pwr_long();
            if (lvgl_port_lock(30)) {
                ui_zero_hold_complete();
                lvgl_port_unlock();
            }
            s_pwr_progress_bucket = -1;
        }
    }
}

static void button_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "button task started");

    while (1) {
        handle_press_logic(&s_boot, read_boot_pressed(), on_boot_short, NULL);
        process_pwr_button(read_pwr_pressed());
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

esp_err_t button_mgr_init(void)
{
    if (s_started) return ESP_OK;

    // BOOT button GPIO input with pull-up.
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BTN_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // PWR key is routed through the IO expander.
    esp_io_expander_handle_t ex = expander_get_handle();
    if (!ex) {
        ESP_LOGW(TAG, "expander handle unavailable, PWR side key disabled");
    } else if (i2c_bus_lock(50)) {
        esp_err_t err = esp_io_expander_set_dir(ex, BTN_PWR_EXIO_MASK, IO_EXPANDER_INPUT);
        i2c_bus_unlock();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to set EXIO4 input: %s", esp_err_to_name(err));
        }
    }

    // Give this task enough headroom for UI callbacks and logging.
    if (xTaskCreate(button_task, "button_task", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create button_task");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}
