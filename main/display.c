#include "display.h"
#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "driver/i2c_master.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "esp_io_expander_tca9554.h"
#include "esp_lcd_sh8601.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

static i2c_master_bus_handle_t s_i2c_bus = NULL;

#if CONFIG_LV_COLOR_DEPTH == 32
#define LCD_BITS_PER_PIXEL 24
#else
#define LCD_BITS_PER_PIXEL 16
#endif

// Minimal init sequence (lifted from your demo)
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

static esp_err_t i2c_init_and_self_test(void)
{
    ESP_LOGI(TAG, "i2c_new_master_bus on SDA=%d SCL=%d", PIN_I2C_SDA, PIN_I2C_SCL);

    // Create master bus (NEW I2C driver)
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    ESP_LOGI(TAG, "i2c_new_master_bus err=%s handle=%p", esp_err_to_name(err), s_i2c_bus);
    if (err != ESP_OK || s_i2c_bus == NULL) {
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    // --- SELF TEST: try adding a device immediately (proves handle is valid) ---
    // We try the most likely expander addresses. Only one needs to succeed.
    const uint8_t addrs_to_try[] = {
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,   // 0x20
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_001,   // 0x21
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_010,   // 0x22
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_011,   // 0x23
        ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_000,  // 0x38 (some boards use TCA9554A)
    };

    bool any_added = false;
    for (size_t i = 0; i < sizeof(addrs_to_try); i++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs_to_try[i],
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev = NULL;
        err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &dev);
        ESP_LOGI(TAG, "i2c self-test add_device addr=0x%02X err=%s dev=%p",
                 addrs_to_try[i], esp_err_to_name(err), dev);

        if (err == ESP_OK && dev != NULL) {
            any_added = true;
            // Remove again (this was only a sanity check)
            i2c_master_bus_rm_device(dev);
            break;
        }
    }

    if (!any_added) {
        ESP_LOGW(TAG, "i2c self-test could not add any device; bus may still be OK, but wiring/addr could be wrong");
        // Don't hard-fail here; the expander init will provide the real answer.
    }

    return ESP_OK;
}

static esp_err_t expander_power_sequence(void)
{
    esp_io_expander_handle_t ex = NULL;

    // MUST pass the I2C BUS HANDLE (not I2C_PORT). Your previous file was still using I2C_PORT here.
    ESP_RETURN_ON_ERROR(
        esp_io_expander_new_i2c_tca9554(s_i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &ex),
        TAG, "tca9554 new failed");

    // Pins 0/1/2 as outputs, then toggle low -> high
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(ex,
                                IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2,
                                IO_EXPANDER_OUTPUT),
        TAG, "tca9554 set_dir failed");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(ex, IO_EXPANDER_PIN_NUM_0, 0), TAG, "ex pin0 low");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(ex, IO_EXPANDER_PIN_NUM_1, 0), TAG, "ex pin1 low");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(ex, IO_EXPANDER_PIN_NUM_2, 0), TAG, "ex pin2 low");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(ex, IO_EXPANDER_PIN_NUM_0, 1), TAG, "ex pin0 high");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(ex, IO_EXPANDER_PIN_NUM_1, 1), TAG, "ex pin1 high");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(ex, IO_EXPANDER_PIN_NUM_2, 1), TAG, "ex pin2 high");

    return ESP_OK;
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "init i2c");
    ESP_RETURN_ON_ERROR(i2c_init_and_self_test(), TAG, "i2c init failed");

    ESP_LOGI(TAG, "expander power/reset sequence");
    ESP_RETURN_ON_ERROR(expander_power_sequence(), TAG, "expander sequence failed");

    ESP_LOGI(TAG, "init QSPI bus");
    const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_SCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        (LCD_H_RES * LCD_V_RES * LCD_BITS_PER_PIXEL) / 8
    );
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus init failed");

    ESP_LOGI(TAG, "new panel IO (QSPI)");
    const esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(
        PIN_LCD_CS,
        NULL, // no flush callback (no LVGL in this minimal build)
        NULL
    );
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io),
        TAG, "new panel io failed"
    );

    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
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
