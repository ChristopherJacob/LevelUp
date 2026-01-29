#pragma once
#include "esp_err.h"
#include <stdint.h>
#include "esp_lcd_panel_ops.h"   // for esp_lcd_panel_handle_t

esp_err_t display_init(void);
esp_err_t display_fill_rgb565(uint16_t color);

/** Return panel handle after display_init() */
esp_lcd_panel_handle_t display_get_panel(void);

