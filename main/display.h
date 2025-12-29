#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t display_init(void);
esp_err_t display_fill_rgb565(uint16_t color);
