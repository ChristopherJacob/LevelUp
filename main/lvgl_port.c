#include "lvgl_port.h"
#include "display.h"
#include "board.h"
#include "i2c_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "lvgl.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i2c.h"          // <-- for esp_lcd_new_panel_io_i2c_v2()
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

static const char *TAG = "lvgl_port";

#define LVGL_TICK_PERIOD_MS        2
#define LVGL_TASK_MAX_DELAY_MS   500
#define LVGL_TASK_MIN_DELAY_MS     1
#define LVGL_TASK_STACK_SIZE    (6 * 1024)
#define LVGL_TASK_PRIORITY         5
// Default auto-blank timeout for the AMOLED after inactivity.
#define SCREEN_TIMEOUT_DEFAULT_MS      (60 * 1000)

static SemaphoreHandle_t s_lvgl_mux = NULL;
static lv_display_t *s_disp = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

// Touch
static esp_lcd_touch_handle_t s_touch = NULL;
static lv_indev_t *s_indev = NULL;

// LVGL draw buffer objects (static, no heap allocation)
static lv_draw_buf_t s_draw_buf1;
static lv_draw_buf_t s_draw_buf2;

// Pointers used by LVGL API
static lv_draw_buf_t *s_buf1 = NULL;
static lv_draw_buf_t *s_buf2 = NULL;

// Backing memory for the draw buffers (DMA-capable)
static void *s_buf1_mem = NULL;
static void *s_buf2_mem = NULL;
static bool s_screen_on = true;
static int64_t s_last_touch_us = 0;
static uint32_t s_screen_timeout_ms = SCREEN_TIMEOUT_DEFAULT_MS;

/* ----------------------------------------------------------------
 * BEGIN COLOR-FIX SUPPORT
 *
 * Waveshare demo (05_LVGL_WITH_RAM) swaps RGB565 bytes in the flush
 * callback before pushing pixels to the SH8601.
 *
 * This fixes the “Blue/Red swapped” / “purple/yellowish white” issues
 * without forcing BGR in panel config.
 *
 * NOTE:
 *  - We do NOT swap back afterward, matching the demo approach.
 *  - LVGL re-renders into the buffer each frame, so this is fine.
 * ---------------------------------------------------------------- */
#if (LV_COLOR_DEPTH == 16)
/* LVGL provides this in its SW draw helpers. We declare it to avoid
 * include-path/version headaches across LVGL/IDF setups. */
void lv_draw_sw_rgb565_swap(void * buf, uint32_t px_cnt);
#endif
/* ----------------------------------------------------------------
 * END COLOR-FIX SUPPORT
 * ---------------------------------------------------------------- */

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

// Toggle panel power state (used by inactivity blanking and wake-on-touch).
static void lvgl_set_screen_power(bool on)
{
    if (!s_panel || s_screen_on == on) return;
    if (esp_lcd_panel_disp_on_off(s_panel, on) == ESP_OK) {
        s_screen_on = on;
        ESP_LOGI(TAG, "screen %s", on ? "ON" : "OFF");
    }
}

bool lvgl_port_lock(int timeout_ms)
{
    if (!s_lvgl_mux) return false;
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mux, ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    if (s_lvgl_mux) xSemaphoreGive(s_lvgl_mux);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LVGL task started");

    uint32_t delay_ms = LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        // Blank the screen after timeout, but keep touch active so tap can wake it.
        if (s_screen_timeout_ms > 0 && s_screen_on && s_last_touch_us > 0) {
            int64_t idle_us = esp_timer_get_time() - s_last_touch_us;
            if (idle_us > ((int64_t)s_screen_timeout_ms * 1000LL)) {
                lvgl_set_screen_power(false);
            }
        }

        if (lvgl_port_lock(100)) {
            delay_ms = lv_timer_handler();
            lvgl_port_unlock();
        } else {
            ESP_LOGW(TAG, "lvgl lock timeout, skipping frame");
        }

        if (delay_ms > LVGL_TASK_MAX_DELAY_MS) delay_ms = LVGL_TASK_MAX_DELAY_MS;
        if (delay_ms < LVGL_TASK_MIN_DELAY_MS) delay_ms = LVGL_TASK_MIN_DELAY_MS;

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * LVGL v9 flush callback signature.
 * `px_map` points to the pixel buffer for the region.
 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    // LVGL uses inclusive coords; esp_lcd draw uses end-exclusive
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2 + 1;
    int y2 = area->y2 + 1;

    /* ----------------------------------------------------------------
     * BEGIN COLOR FIX (KEEP)
     * Swap RGB565 bytes to match what the SH8601 pipeline expects.
     * This matches Waveshare's demo approach.
     * ---------------------------------------------------------------- */
#if (LV_COLOR_DEPTH == 16)
    uint32_t w = (uint32_t)(x2 - x1);
    uint32_t h = (uint32_t)(y2 - y1);
    uint32_t px_cnt = w * h;
    lv_draw_sw_rgb565_swap((void *)px_map, px_cnt);
#endif
    /* ----------------------------------------------------------------
     * END COLOR FIX
     * ---------------------------------------------------------------- */

    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, (lv_color_t *)px_map);

    lv_display_flush_ready(disp);
}

// Optional: if your touch panel is mounted rotated/swapped you can map here.
// For now: raw p.x/p.y direct.
static inline void touch_map_xy(uint16_t *x, uint16_t *y)
{
    (void)x;
    (void)y;
}

/**
 * LVGL input read callback for FT5x06 touch controller.
 */
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    data->state = LV_INDEV_STATE_RELEASED;

    if (!s_touch) return;

    // If INT pin exists, use it to avoid polling (FT5x06 INT is typically active-low)
    if (PIN_TOUCH_INT >= 0) {
        int int_level = gpio_get_level(PIN_TOUCH_INT);
        if (int_level == 1) {
            // No touch signaled
            return;
        }
    }

    if (!i2c_bus_lock(20)) {
        return;
    }

    esp_err_t err = esp_lcd_touch_read_data(s_touch);
    if (err != ESP_OK) {
        i2c_bus_unlock();
        return;
    }

    esp_lcd_touch_point_data_t p = {0};
    uint8_t cnt = 0;

    err = esp_lcd_touch_get_data(s_touch, &p, &cnt, 1);

    i2c_bus_unlock();

    if (err == ESP_OK && cnt > 0) {
        uint16_t x = p.x;
        uint16_t y = p.y;

        touch_map_xy(&x, &y);

        if (x >= LCD_H_RES) x = LCD_H_RES - 1;
        if (y >= LCD_V_RES) y = LCD_V_RES - 1;

        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;

        s_last_touch_us = esp_timer_get_time();
        if (!s_screen_on) {
            // Wake immediately on first touch.
            lvgl_set_screen_power(true);
        }
    }
}

static esp_err_t touch_init(i2c_master_bus_handle_t bus)
{
    if (!bus) return ESP_ERR_INVALID_STATE;

    const uint8_t addrs[] = { 0x38, 0x3C };

    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        esp_lcd_panel_io_handle_t tp_io = NULL;

        esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
            .dev_addr = addrs[i],
            .scl_speed_hz = 400000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .disable_control_phase = 1,
            },
        };

        ESP_LOGI(TAG, "touch: create I2C panel IO (NG v2) addr=0x%02X", tp_io_cfg.dev_addr);

        esp_err_t err = esp_lcd_new_panel_io_i2c_v2(bus, &tp_io_cfg, &tp_io);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch: panel_io_i2c create failed @0x%02X: %s",
                     tp_io_cfg.dev_addr, esp_err_to_name(err));
            continue;
        }

        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = LCD_H_RES,
            .y_max = LCD_V_RES,
            .rst_gpio_num = -1,
            .int_gpio_num = PIN_TOUCH_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };

        ESP_LOGI(TAG, "touch: init FT5x06");
        err = esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch: FT5x06 init failed @0x%02X: %s",
                     tp_io_cfg.dev_addr, esp_err_to_name(err));
            s_touch = NULL;
            continue;
        }

        ESP_LOGI(TAG, "touch: FT5x06 init OK @0x%02X", tp_io_cfg.dev_addr);
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t lvgl_port_init(void)
{
    esp_lcd_panel_handle_t panel = display_get_panel();
    if (!panel) {
        ESP_LOGE(TAG, "display panel is NULL; call display_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "lv_init()");
    lv_init();
    s_panel = panel;
    s_last_touch_us = esp_timer_get_time();

    s_lvgl_mux = xSemaphoreCreateMutex();
    if (!s_lvgl_mux) return ESP_ERR_NO_MEM;

    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (!s_disp) return ESP_ERR_NO_MEM;

    lv_display_set_user_data(s_disp, panel);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);

    //const int heights_to_try[] = { 112, 80, 60, 40, 30, 20 };
    const int heights_to_try[] = { 40, 30, 20, 16, 12, 8 };
    int chosen_h = 0;

    for (size_t i = 0; i < sizeof(heights_to_try) / sizeof(heights_to_try[0]); i++) {
        int h = heights_to_try[i];
        size_t buf_px = (size_t)LCD_H_RES * (size_t)h;
        size_t buf_bytes = buf_px * sizeof(lv_color_t);

        void *m1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
        void *m2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);

        ESP_LOGI(TAG, "Try LVGL DMA buffers: %dx%d -> %u bytes each (m1=%p m2=%p)",
                 LCD_H_RES, h, (unsigned)buf_bytes, m1, m2);

        if (m1 && m2) {
            s_buf1_mem = m1;
            s_buf2_mem = m2;
            chosen_h = h;
            break;
        }

        if (m1) heap_caps_free(m1);
        if (m2) heap_caps_free(m2);
    }

    if (chosen_h == 0) {
        ESP_LOGE(TAG, "LVGL draw buffer alloc failed for all sizes (DMA heap too small)");
        return ESP_ERR_NO_MEM;
    }

    size_t chosen_bytes = (size_t)LCD_H_RES * (size_t)chosen_h * sizeof(lv_color_t);

    s_buf1 = &s_draw_buf1;
    s_buf2 = &s_draw_buf2;

    lv_draw_buf_init(s_buf1, LCD_H_RES, chosen_h, LV_COLOR_FORMAT_RGB565, 0, s_buf1_mem, chosen_bytes);
    lv_draw_buf_init(s_buf2, LCD_H_RES, chosen_h, LV_COLOR_FORMAT_RGB565, 0, s_buf2_mem, chosen_bytes);

    lv_display_set_draw_buffers(s_disp, s_buf1, s_buf2);

    ESP_LOGI(TAG, "LVGL buffers OK: height=%d bytes_each=%u", chosen_h, (unsigned)chosen_bytes);

    const esp_timer_create_args_t tick_args = {
        .callback = &lvgl_tick_cb,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG, "esp_timer_create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000), TAG, "start tick failed");

    if (xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create lvgl task");
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "Touch: I2C bus handle is NULL; UI will be display-only");
    } else {
        esp_err_t terr = touch_init(bus);
        if (terr != ESP_OK) {
            ESP_LOGW(TAG, "Touch init failed (%s) - UI will be display-only", esp_err_to_name(terr));
        } else {
            s_indev = lv_indev_create();
            lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(s_indev, lvgl_touch_read_cb);
            ESP_LOGI(TAG, "Touch: LVGL indev registered");
        }
    }

    ESP_LOGI(TAG, "LVGL port init OK");
    return ESP_OK;
}

// Update screen timeout at runtime from settings UI.
void lvgl_port_set_screen_timeout_ms(uint32_t timeout_ms)
{
    s_screen_timeout_ms = timeout_ms;
    if (timeout_ms == 0) {
        // If timeout is disabled while blanked, wake immediately.
        lvgl_set_screen_power(true);
    }
}

void lvgl_port_set_screen_on(bool on)
{
    lvgl_set_screen_power(on);
    if (on) {
        // Treat a manual wake like activity so we don't immediately blank again.
        s_last_touch_us = esp_timer_get_time();
    }
}

bool lvgl_port_is_screen_on(void)
{
    return s_screen_on;
}
