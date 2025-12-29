#pragma once
#include "driver/gpio.h"

// ---- I2C bus (Touch + IO expander) ----
#define I2C_PORT            I2C_NUM_0
#define PIN_I2C_SCL         GPIO_NUM_14
#define PIN_I2C_SDA         GPIO_NUM_15
#define I2C_CLK_HZ          (200 * 1000)

// ---- AMOLED (SH8601) QSPI ----
#define PIN_LCD_CS          GPIO_NUM_12
#define PIN_LCD_SCLK        GPIO_NUM_11
#define PIN_LCD_D0          GPIO_NUM_4
#define PIN_LCD_D1          GPIO_NUM_5
#define PIN_LCD_D2          GPIO_NUM_6
#define PIN_LCD_D3          GPIO_NUM_7

#define LCD_H_RES           368
#define LCD_V_RES           448

// No direct reset/backlight GPIO on this board (handled via expander / internal)
#define PIN_LCD_RST         (-1)
#define PIN_LCD_BK_LIGHT    (-1)

// ---- IO Expander (TCA9554) ----
// Waveshare demo uses ADDRESS_000; keep that first.
// If it fails, we can add a quick scan later.
