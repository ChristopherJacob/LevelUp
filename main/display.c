#include "display.h"
#include "board.h"
#include "i2c_bus.h"
#include "expander.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "esp_lcd_sh8601.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

#if CONFIG_LV_COLOR_DEPTH == 32
#define LCD_BITS_PER_PIXEL 24
#else
#define LCD_BITS_PER_PIXEL 16
#endif

/* NOTE:
 * Do NOT force COLMOD(0x3A) / MADCTL(0x36) here.
 * The SH8601 driver already uses these internally and logs:
 *  "The 3Ah command has been used and will be overwritten..."
 * That’s why injecting them caused purple/yellowish artifacts.
 *
 * We keep only the basic wake/area/TE/brightness sequence.
 */
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},                  // Sleep out
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},               // TE scanline
    {0x35, (uint8_t[]){0x00}, 1, 0},                     // TE on
    {0x53, (uint8_t[]){0x20}, 1, 10},                    // Write CTRL display
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},   // Column addr 0..367
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},   // Row addr 0..447
    {0x51, (uint8_t[]){0x00}, 1, 10},                    // Brightness 0
    {0x29, (uint8_t[]){0x00}, 0, 10},                    // Display on
    {0x51, (uint8_t[]){0xFF}, 1, 0},                     // Brightness max
};

esp_err_t display_init(void)
{
    // ---- I2C bus (shared) ----
    ESP_LOGI(TAG, "init i2c bus");
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_bus_init(&bus), TAG, "i2c bus init failed");

    // ---- Expander power/reset ----
    ESP_LOGI(TAG, "expander power/reset sequence");
    ESP_RETURN_ON_ERROR(expander_power_sequence(bus), TAG, "expander sequence failed");

    // ---- QSPI bus ----
    ESP_LOGI(TAG, "init QSPI bus");
    const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_SCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        (LCD_H_RES * LCD_V_RES * LCD_BITS_PER_PIXEL) / 8
    );
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus init failed");

    // ---- Panel IO ----
    ESP_LOGI(TAG, "new panel IO (QSPI)");
    const esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(
        PIN_LCD_CS,
        NULL, // flush cb set by LVGL port
        NULL
    );
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io),
        TAG, "new panel io failed"
    );

    // ---- Panel driver ----
    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,

        // IMPORTANT: Keep this RGB (matches Waveshare demo).
        // Do NOT set BGR here. Our LVGL flush now does the RGB565 byte swap.
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,

        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_cfg,
    };

    ESP_LOGI(TAG, "new SH8601 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel), TAG, "new panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on failed");

    ESP_LOGI(TAG, "display init OK");
    return ESP_OK;
}

esp_err_t display_fill_rgb565(uint16_t color)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;

    static uint16_t *line = NULL;
    if (!line) {
        line = heap_caps_malloc(LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!line) return ESP_ERR_NO_MEM;
    }

    for (int x = 0; x < LCD_H_RES; x++) line[x] = color;

    for (int y = 0; y < LCD_V_RES; y++) {
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + 1, line),
            TAG, "draw line failed"
        );
    }
    return ESP_OK;
}

esp_lcd_panel_handle_t display_get_panel(void)
{
    return s_panel;
}
