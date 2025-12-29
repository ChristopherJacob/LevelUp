#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "display.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "Leveler minimal boot");
    ESP_ERROR_CHECK(display_init());

    while (1) {
        ESP_ERROR_CHECK(display_fill_rgb565(0xF800)); // red
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_ERROR_CHECK(display_fill_rgb565(0x07E0)); // green
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_ERROR_CHECK(display_fill_rgb565(0x001F)); // blue
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_ERROR_CHECK(display_fill_rgb565(0xFFFF)); // white
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_ERROR_CHECK(display_fill_rgb565(0x0000)); // black
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
