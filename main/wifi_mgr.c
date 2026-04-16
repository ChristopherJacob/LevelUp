// Wi-Fi manager: STA/AP, captive portal, status UI, and config persistence.
//
// Features:
//  - First boot (no saved creds): starts SoftAP "Leveler-XXXX" (WPA2 password: "leveler-setup")
//    and serves a setup UI at http://192.168.4.1/
//  - Setup UI includes SSID scan + dropdown, plus manual SSID/password for hidden networks.
//  - Saves creds to NVS and reboots.
//  - Subsequent boots (saved creds): starts STA, connects, and serves a status UI at
//    http://<sta_ip>/ (status) and JSON at /status.json
//  - If STA can't connect after several retries, falls back to AP setup mode.
//  - Captive portal (DNS + redirect) on AP mode for easy onboarding.
//  - Factory reset endpoint: POST /reset clears Wi-Fi creds + leveler offsets and reboots.
//  - wifi_mgr_update_angles(roll,pitch) lets IMU task publish latest readings for status page.
//
// Notes:
//  - Uses chunked HTTP responses to avoid snprintf truncation warnings with -Werror.
//  - Starts HTTP server in AP mode immediately; starts it in STA mode after STA_GOT_IP.
//  - AP uses WIFI_MODE_APSTA so scanning works while AP is up (iPhone friendly).

#include "wifi_mgr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "esp_timer.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "mqtt_mgr.h"
#include "imu_task.h"
#include "dns_server.h"
#include "lvgl_port.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"

/* ---------------- logging ---------------- */
static const char *TAG = "wifi_mgr";

// Small embedded favicon (32x32 PNG) served at /favicon.png.
static const uint8_t s_favicon_png[] = {
0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x20,
0x00,0x00,0x00,0x20,0x08,0x06,0x00,0x00,0x00,0x73,0x7A,0x7A,0xF4,0x00,0x00,0x0A,0x02,0x49,0x44,0x41,
0x54,0x78,0xDA,0x9D,0x57,0x6B,0x70,0x1B,0xD5,0x19,0x3D,0x77,0x77,0xA5,0xD5,0xD3,0x92,0xAC,0x87,0x23,
0x3B,0x96,0x64,0xF9,0xFD,0x8A,0x9D,0xC4,0x76,0x3C,0xCE,0xC3,0x04,0xC8,0x10,0x12,0xCA,0xA4,0x80,0x78,
0x94,0x4E,0x19,0x68,0x99,0x76,0x80,0xD2,0x76,0x9A,0x99,0xF2,0x68,0x51,0x4D,0xD3,0xF6,0x47,0x67,0x18,
0xDA,0xD2,0x96,0xE9,0x0F,0x86,0xA1,0x85,0xD2,0xB8,0x40,0x9A,0x10,0x48,0x42,0x43,0xE2,0x06,0x02,0x04,
0xC7,0xC1,0x71,0x6C,0x6C,0xD9,0x96,0x2D,0xC9,0x0F,0x3D,0x2C,0x5B,0x92,0xF5,0x5A,0x49,0xBB,0xB7,0x3F,
0xE2,0xB4,0x6E,0x9B,0x64,0x92,0x7C,0xFF,0xF6,0xEE,0xCC,0x9E,0x73,0xBF,0x7B,0xCE,0xFD,0xCE,0x02,0x57,
0xA9,0xB6,0xDB,0xBE,0xFA,0xCC,0xF6,0xBB,0xBF,0x71,0x6C,0xE7,0x03,0x0F,0x3F,0x05,0x80,0xA5,0x94,0x12,
0x00,0xCC,0xCA,0xEB,0xA2,0xF6,0x1D,0xBB,0x5A,0x01,0xA0,0xBB,0xBB,0x9B,0x5B,0x59,0x27,0xB8,0xCE,0xE2,
0x56,0x3F,0xB4,0x6C,0xEA,0x76,0x38,0xEB,0x9B,0x9D,0x42,0x4E,0x68,0x89,0xC5,0x96,0x1E,0x8A,0x85,0x66,
0x5A,0xE6,0x86,0xCF,0x62,0xCB,0x9D,0x0F,0xEC,0xD8,0xB2,0xEB,0xCE,0x13,0x00,0x3E,0xA7,0x94,0xC2,0x6E,
0xB7,0x1B,0x6A,0x3A,0x6E,0x3A,0xAA,0x33,0x97,0x6C,0x60,0x41,0x7E,0xD5,0xF7,0xC1,0xE1,0xA7,0x09,0xB9,
0x88,0xBD,0x42,0x92,0x5E,0x2B,0x01,0xC6,0xE5,0x72,0xB1,0x00,0x50,0xDF,0xDA,0x76,0x6B,0xFD,0xA6,0xAD,
0xDE,0x65,0x21,0x7F,0x5C,0x5F,0x6A,0x7F,0x21,0x9F,0x13,0x64,0x45,0xB1,0x68,0x42,0x56,0xC8,0xE5,0x09,
0xC7,0xD1,0xD8,0x42,0xDC,0x4A,0x08,0x91,0x08,0x21,0x12,0xE4,0x72,0xAB,0x56,0xA7,0x6F,0x3F,0xF5,0x6E,
0x2F,0xE1,0x94,0x45,0x7B,0x9D,0xCD,0x1B,0xBE,0x67,0xB1,0xD9,0x1C,0x94,0x52,0xE6,0x7A,0xC0,0x01,0x80,
0x1D,0x19,0x19,0x21,0x84,0x10,0xAA,0xD2,0x69,0x25,0x99,0xC6,0xE8,0x62,0x95,0x45,0x45,0xBE,0xD1,0xA1,
0x23,0x03,0xC7,0x0F,0x75,0x19,0xCD,0x16,0x53,0x42,0x62,0x36,0x1B,0xCA,0x2A,0xA0,0x90,0xB1,0xAD,0x56,
0x67,0xF5,0xCE,0xF6,0xED,0xB7,0x3F,0x2D,0x57,0xAA,0xDB,0x75,0x46,0x73,0xD5,0xE0,0xE9,0x93,0x50,0x69,
0xF5,0x68,0xDE,0xBC,0x63,0x57,0x75,0xD3,0x86,0x47,0x9D,0x4D,0x6D,0xF7,0x87,0x83,0x33,0x83,0xD9,0xE5,
0x78,0x00,0x6E,0x37,0x83,0xBE,0x3E,0x7A,0x2D,0x47,0x20,0x49,0x92,0x44,0x08,0x21,0x01,0x7B,0x63,0xA7,
0x5C,0xCE,0x91,0x8F,0xBE,0xF8,0xF0,0xE0,0x7D,0x0C,0xC3,0x60,0xD0,0x33,0xDA,0xBF,0xFB,0x9E,0x7B,0xC5,
0xAE,0x06,0x2B,0x19,0x0D,0xC8,0xEB,0x36,0xB5,0xD4,0xD7,0x2D,0x26,0x12,0x18,0xE6,0x95,0xCD,0xBC,0x52,
0x05,0xAD,0xB1,0x24,0x32,0x33,0x31,0xFC,0xB2,0xC6,0x5C,0xFE,0x0C,0xA8,0xA4,0xE2,0x95,0xCA,0x26,0x9A,
0xCB,0xC9,0x00,0x00,0x3D,0x3D,0xD7,0xAC,0x07,0x02,0x00,0x26,0x9B,0xCD,0xDA,0xB4,0xF9,0xE6,0x27,0x00,
0xC8,0x38,0x8E,0x83,0xD1,0x68,0xD4,0xEE,0xDB,0xF7,0xE3,0x33,0xA1,0xA0,0x9F,0x7E,0x74,0xF2,0x50,0xFE,
0xBD,0x43,0x7F,0x15,0x29,0xA5,0xF9,0x69,0xCF,0xE7,0x85,0xAF,0x3D,0xB6,0x37,0xFB,0xED,0x9E,0xDF,0x50,
0xAB,0xB3,0xEE,0x35,0x00,0xC6,0x86,0x6D,0xBB,0x73,0xB5,0x9B,0x77,0x51,0x55,0x71,0xC9,0x23,0x17,0x3F,
0xE9,0x66,0xAE,0x47,0x84,0x14,0x00,0x16,0xFC,0xFE,0xF9,0x05,0xBF,0xFF,0x25,0x00,0xA4,0x50,0x28,0x20,
0x1A,0x8D,0x56,0xEF,0x7D,0xF2,0xD1,0xF6,0xC3,0x47,0x8E,0x8B,0x6F,0xBD,0x73,0x98,0xFD,0xFA,0x83,0xF7,
0x01,0x00,0x1B,0x59,0x8C,0x23,0x23,0x08,0x5C,0x76,0x2E,0x80,0xD4,0x72,0xFC,0x75,0x00,0xD1,0xE4,0x62,
0xE4,0xF7,0x84,0x65,0x76,0xA6,0x17,0x43,0xAF,0x50,0x4A,0x09,0x21,0x44,0xBA,0x76,0x02,0x94,0x12,0x10,
0xC2,0x56,0xB6,0x74,0x3D,0x5C,0x56,0x59,0xF3,0x20,0xC3,0xB2,0x26,0x96,0x01,0xCD,0x25,0xE3,0xF9,0xA7,
0xF6,0xFD,0x56,0x4C,0xE7,0x28,0x13,0xC9,0xB2,0x48,0x65,0xD2,0x38,0xF6,0xC1,0x41,0xBC,0x7B,0xA2,0x1F,
0xD6,0xF2,0x0A,0xF6,0xDC,0x99,0x4F,0x68,0x22,0x32,0x1F,0x58,0x01,0xFC,0x81,0x5A,0x6D,0xF9,0x25,0xDC,
0x6E,0xE6,0x7A,0xC0,0xB1,0xCA,0xB7,0x45,0xA5,0xD5,0xCD,0x8B,0x80,0xC8,0xF2,0xBC,0x02,0x46,0xCB,0x1A,
0x68,0xCD,0x65,0xC8,0xA5,0xE2,0xD8,0xB6,0x6D,0x2B,0xE2,0x89,0x14,0x7C,0xB3,0x41,0x31,0x99,0x88,0xB3,
0x3A,0x9D,0x8E,0x96,0x3B,0x2A,0x48,0xFF,0xC7,0x7D,0xE2,0x67,0x47,0xDF,0xAE,0x27,0x84,0x8C,0xAF,0xA8,
0x5F,0xC2,0x0D,0x14,0x03,0x80,0xAC,0xEB,0xDC,0xBE,0x67,0x63,0xD7,0xD6,0x02,0xCF,0x2B,0x04,0x21,0x93,
0x2E,0x4C,0x7B,0x46,0xC4,0x90,0x7F,0x52,0xAC,0xAE,0xA9,0xA5,0xF3,0xC1,0x30,0xCD,0x0A,0x39,0xAA,0xD3,
0xEB,0xD9,0xA5,0x85,0x30,0x38,0x8E,0x45,0x36,0x93,0x41,0x3A,0x93,0x65,0xE4,0x80,0x8C,0x52,0xFA,0xBF,
0x9B,0xB9,0xEE,0x8B,0x88,0x4D,0x24,0x96,0x76,0x54,0x37,0xB7,0xF1,0xE5,0x95,0x35,0x42,0x30,0xE0,0xE7,
0x2A,0xAA,0x6B,0x0B,0x62,0x5E,0x60,0x72,0x42,0x0E,0xBC,0x52,0x49,0xA3,0x91,0x30,0x7D,0xE7,0x95,0xDF,
0xBD,0x6E,0x6F,0x6C,0xBB,0x9B,0x95,0xF1,0x6A,0x9D,0x5E,0x2F,0x95,0xD9,0x1C,0xCC,0x28,0xA0,0x5C,0x05,
0x7E,0x83,0x1D,0x70,0xBB,0xA5,0xD0,0x4C,0xE0,0xD7,0xE7,0xFB,0x3F,0x8D,0x86,0x43,0x61,0x7E,0xB0,0xFF,
0x13,0x61,0x70,0x70,0x90,0x4B,0x09,0x22,0x13,0x5A,0x58,0x22,0xFE,0xC0,0x2C,0x13,0x99,0x9F,0x29,0x64,
0xE2,0x0B,0xFB,0x8C,0x26,0x93,0x60,0x34,0x9A,0x30,0x37,0x3D,0x21,0x3A,0x1C,0x76,0x68,0x6C,0x35,0x77,
0x01,0x20,0x2E,0x97,0x0B,0x37,0x5A,0x2C,0xFA,0xFA,0x50,0x10,0x32,0x73,0xDE,0x0B,0x67,0xDF,0x48,0xA6,
0x53,0x27,0x84,0x78,0xFC,0x17,0xE1,0x50,0x30,0x32,0xEF,0x9B,0x28,0x2B,0x2A,0x36,0xC9,0xB2,0xE9,0x54,
0x6E,0xDC,0x3B,0xFD,0xEC,0xD2,0xCC,0xE4,0xE9,0xA6,0x0D,0x9D,0x4F,0x97,0x97,0xAF,0xE5,0x58,0x99,0x9C,
0x84,0x33,0x54,0xBC,0x45,0xE1,0xEB,0x5E,0xAF,0xCA,0xCE,0xBD,0x7A,0xBC,0x7F,0xA0,0xBB,0x1B,0xAC,0xCF,
0x77,0xFD,0x5D,0xB8,0x64,0x43,0x06,0xC0,0x8C,0x7F,0x78,0x60,0x66,0x65,0xFD,0xF9,0x20,0xF0,0xE7,0xC0,
0xD9,0x8F,0xC5,0x2C,0x90,0xFB,0x79,0x2B,0xDF,0x34,0xB7,0xB5,0xF6,0xE3,0x41,0x4E,0x25,0x5F,0x48,0x24,
0xA8,0x31,0x1F,0x25,0xF7,0xA6,0x3F,0xA3,0xE5,0xDA,0x08,0xFD,0xAE,0x2F,0xD9,0x01,0xE0,0x8F,0xB5,0x49,
0xB0,0x7D,0x37,0xD4,0x81,0x8B,0x45,0x57,0xE6,0x02,0x33,0x32,0x32,0x42,0xBA,0xBB,0xBB,0x59,0xBF,0xDF,
0x1F,0xA5,0x0C,0x89,0x03,0x24,0x59,0xCE,0x69,0x9F,0xD0,0x38,0x6A,0xEF,0xD4,0x44,0x7D,0x85,0x3B,0x0A,
0x67,0x99,0x86,0xE4,0x00,0x66,0xE7,0x93,0xCC,0x01,0x79,0xA7,0x14,0x2F,0xA9,0xAF,0x9C,0xF1,0x0C,0x79,
0x06,0x82,0x64,0x78,0x65,0x23,0xF4,0x46,0x6C,0x78,0x25,0x87,0x48,0x00,0x94,0xDD,0x37,0xDB,0xFF,0x94,
0x96,0x2A,0xF6,0x74,0xF0,0x3E,0xC8,0xD4,0xA5,0x4C,0x54,0xD7,0x02,0x99,0xD5,0x06,0xB1,0x50,0x90,0xE2,
0x8B,0x0B,0x6C,0x38,0xE0,0x8D,0x9D,0x3E,0x7A,0xD0,0x49,0x29,0x8D,0x91,0x8B,0x63,0x51,0xBA,0xA1,0x71,
0xBC,0x1A,0x9C,0x52,0xD0,0xFB,0xF7,0xF0,0x8E,0xFA,0x46,0xF3,0x81,0x0D,0x6D,0x9A,0x96,0xC4,0x52,0x48,
0x62,0x94,0x2A,0xC6,0xA0,0xCB,0xD0,0x37,0x8F,0x64,0x91,0x5E,0x08,0xC3,0x5C,0x62,0x62,0x2E,0x4C,0x79,
0xF3,0x2D,0x6D,0xED,0xFA,0xC0,0xD4,0x78,0x0F,0x21,0xE4,0x49,0x97,0xCB,0xC5,0xF4,0xF6,0xF6,0x5E,0xD7,
0x3D,0xF0,0x7F,0xE5,0xEE,0x06,0x43,0x08,0xE8,0x54,0xC4,0xF8,0x50,0xC7,0x06,0xBE,0x45,0xC3,0x67,0x85,
0xE6,0x3A,0x42,0xD6,0xE8,0xF2,0x74,0x31,0x30,0x0D,0x9D,0x22,0x05,0xBB,0xB3,0x06,0x32,0x5E,0x81,0x75,
0xEB,0xDB,0x58,0xB5,0x5A,0x23,0x3A,0xEA,0x37,0x3E,0x26,0x93,0xA9,0x5A,0x1A,0xF6,0xEF,0xA7,0xAB,0x8E,
0x76,0x75,0xA7,0x2F,0x1B,0x58,0x2E,0x4B,0x60,0xE4,0xF1,0xFD,0x14,0x00,0xB2,0x6C,0x6B,0xC7,0xDB,0xA7,
0xBB,0x0B,0x03,0x63,0x26,0xF6,0xD4,0x00,0x8F,0xC1,0x49,0x2B,0xFE,0x39,0x76,0x07,0x0A,0xF2,0x36,0x8C,
0x8E,0x9C,0x43,0x36,0x9B,0x47,0x24,0x12,0x26,0x27,0x3F,0x38,0x46,0x21,0x57,0xB1,0xD6,0xDA,0xC6,0x9F,
0xF5,0x10,0x22,0xB9,0xDD,0x6E,0xB2,0x0A,0xEC,0x52,0x40,0x91,0x00,0x50,0xD7,0xFE,0xFD,0xEC,0x6A,0x22,
0xEC,0xE5,0x08,0x98,0xC3,0x61,0xD6,0xE7,0xF3,0x49,0x1C,0x27,0x9A,0x1A,0x37,0xDC,0xBE,0x3B,0x96,0xD9,
0x28,0x4D,0x87,0x6B,0x19,0xEF,0x7C,0x39,0x64,0x2A,0x2B,0x8A,0x0D,0x0A,0xC4,0xE2,0xCB,0xE0,0x38,0x19,
0x0A,0x62,0x01,0x93,0xE3,0x63,0x0C,0x00,0x49,0xA1,0xD1,0xD7,0x49,0xA2,0x28,0xBE,0x7F,0xF0,0xED,0x93,
0x2E,0x97,0x8B,0x1D,0x19,0x19,0xB9,0x24,0x70,0x38,0x9A,0xDB,0x6A,0x63,0xE1,0xB9,0xC2,0x48,0x6F,0x6F,
0x06,0x17,0xD3,0x13,0xB9,0x22,0x01,0x9F,0xCF,0x47,0xDD,0x6E,0x37,0x73,0xE8,0xC0,0x81,0xA1,0xC4,0x72,
0x6C,0x67,0x71,0xB1,0xB2,0xAC,0xBA,0xAE,0xB2,0x10,0x8D,0x04,0x48,0x70,0x76,0x9A,0x49,0x2C,0xA7,0x51,
0x5D,0xDF,0x88,0xA1,0xC1,0x01,0x98,0x4A,0x1D,0x24,0x97,0xCF,0x13,0xAD,0xDE,0x40,0xE2,0x8B,0x0B,0x92,
0xB3,0xB9,0xED,0x16,0x83,0xD1,0x52,0xF4,0xE1,0x7B,0x7F,0x3F,0xE2,0x76,0x9F,0xE0,0x26,0xC2,0x83,0x95,
0x6B,0x2A,0xEA,0x4E,0xE9,0x8C,0x25,0x0F,0xAE,0xEF,0xEA,0xDE,0x5B,0xD5,0xD0,0x52,0x3B,0x71,0xE1,0xDC,
0x29,0x42,0x48,0x16,0x00,0x61,0xAF,0x66,0x51,0x9F,0xCF,0x27,0x28,0x0D,0x96,0xFA,0xB9,0x60,0xA4,0x33,
0x95,0xCE,0xB1,0x9C,0xD6,0x44,0x72,0xE9,0x04,0x5D,0xEB,0xAC,0x21,0xF9,0x7C,0x81,0xF8,0xBD,0xE3,0x24,
0x27,0x11,0x94,0x96,0x95,0x51,0x83,0xD1,0x4C,0x4A,0x6C,0x4E,0xC2,0xCA,0xF8,0x82,0xC6,0x60,0xDA,0x22,
0x97,0xC9,0x94,0xBD,0xAF,0xFF,0xF4,0xD8,0xF2,0x42,0x50,0x2D,0x88,0xF9,0x4F,0xFD,0x43,0x67,0x7E,0xA2,
0x50,0x69,0x4F,0x5B,0x4A,0xCB,0x9E,0x37,0x97,0xD9,0x76,0xFA,0x3C,0x23,0x07,0x29,0xA5,0xE9,0xAB,0x04,
0x87,0x9B,0x00,0x80,0x94,0xD9,0x1C,0xD4,0xD9,0xD0,0x8A,0x8F,0x8E,0x1D,0x78,0x33,0x30,0x3E,0xEC,0x11,
0x25,0x10,0xB5,0x46,0x2D,0x85,0x66,0xFD,0x98,0xF4,0x8C,0x42,0x4C,0x2D,0x81,0x83,0x88,0x60,0xC0,0x4B,
0x23,0x33,0x5E,0xC2,0x32,0x84,0x9B,0xF1,0x4E,0x8A,0x5A,0x63,0xC9,0x8F,0x3A,0x6E,0xDD,0xFD,0xFE,0x5D,
0xDF,0x7C,0xF2,0xCD,0xE6,0xA6,0x66,0x1B,0x80,0xF4,0x85,0x4F,0x4F,0xF6,0x1D,0x78,0xE5,0xA5,0xDB,0x18,
0x5E,0xDD,0x69,0xB6,0x57,0xDD,0x43,0xEE,0x25,0x0C,0x73,0x75,0x7C,0xD0,0xBC,0x90,0x55,0xAA,0xD4,0x1A,
0x30,0x52,0xFE,0x0F,0x67,0x8E,0xFC,0xED,0x81,0x58,0x64,0x6E,0xCC,0xF3,0xE5,0x97,0x64,0x7A,0xCA,0x2B,
0xB1,0x72,0x1E,0x55,0xB5,0x75,0xD0,0x14,0xE9,0xC9,0x4C,0x60,0x86,0xC4,0xE3,0x49,0x8C,0x9F,0xEF,0x27,
0x2A,0x15,0xCF,0xCA,0x78,0x9E,0xB6,0x6F,0xDE,0xBE,0x33,0x9E,0xCA,0x76,0x29,0x8B,0xAD,0xAF,0x56,0x36,
0xB6,0x7E,0xCB,0xED,0x76,0x33,0x00,0xCE,0x86,0x66,0x03,0xF1,0x62,0x4B,0x69,0x17,0x7A,0x21,0x5E,0x91,
0x80,0xA5,0xB1,0x91,0x02,0x80,0x5C,0xAE,0x28,0x15,0xF3,0x82,0x54,0x10,0x69,0xBB,0xA3,0x75,0xCB,0xA9,
0x62,0xA3,0xD9,0xEE,0x70,0x56,0x92,0xC8,0xFC,0x0C,0x61,0x39,0x39,0x7C,0x5E,0x2F,0x92,0xCB,0xCB,0x58,
0xDF,0xD6,0x8E,0xB5,0x36,0x1B,0x00,0x06,0xB4,0x20,0x40,0xAD,0x90,0x81,0x61,0x59,0x51,0xA1,0x50,0xE6,
0x79,0x9D,0x45,0x5C,0x53,0xDD,0xFC,0xC3,0x9E,0x8B,0x39,0x91,0xB3,0x39,0x6B,0x64,0x5A,0x9D,0x41,0x7D,
0x25,0x1B,0x12,0x00,0x64,0xBF,0xCB,0x25,0x01,0x80,0x5A,0x5B,0xE4,0x2C,0x08,0xD9,0x84,0x98,0x4D,0x1F,
0xCB,0xE7,0x32,0x22,0xAF,0x29,0x52,0x84,0x82,0xF3,0x62,0x6C,0x21,0x44,0xB3,0xC9,0x18,0x2E,0x7C,0xD1,
0x8F,0x58,0x22,0x01,0xA3,0xB9,0x04,0xFA,0x22,0x2D,0xB6,0x6E,0xDB,0x8C,0xCE,0xAE,0xCD,0x30,0x9B,0x2D,
0x64,0x39,0x95,0x66,0x59,0xB9,0x82,0xCB,0x17,0x44,0x36,0x27,0x32,0x76,0x00,0xA2,0xA3,0x79,0xD3,0x77,
0x28,0xAF,0x51,0x05,0xFD,0x53,0x87,0xFF,0x8F,0x80,0xCB,0xE5,0x62,0x09,0xC3,0x50,0x42,0xC8,0xA5,0x01,
0x65,0x50,0xEB,0x0C,0xB5,0x39,0x21,0xE3,0xCD,0xE7,0x0B,0x43,0x91,0xA9,0xB1,0x47,0x08,0xC3,0x21,0xE0,
0x1D,0x67,0x39,0x5E,0xC5,0x70,0x1C,0x87,0xE5,0xF8,0x12,0xE6,0x7D,0x93,0x58,0x5C,0x8A,0x21,0x3C,0x3F,
0x8B,0x64,0xB6,0x80,0x29,0xDF,0x2C,0x96,0x92,0x39,0x48,0xAC,0x12,0x93,0x13,0xE3,0x74,0x2E,0x10,0xC0,
0x52,0x34,0x3C,0xAF,0xD4,0x9B,0x1F,0x57,0x59,0x6C,0x2F,0x8E,0x0F,0xF5,0x0F,0xCD,0x78,0xCE,0xBF,0x01,
0xB7,0xFB,0xBF,0x35,0xD0,0xDB,0xDB,0x2B,0x52,0x49,0x62,0x28,0xA5,0x3C,0x21,0x44,0x34,0x98,0xAD,0x1D,
0x6A,0xBD,0x51,0x16,0x99,0xF5,0x1F,0x02,0x28,0x78,0x8E,0xA4,0xCC,0x65,0x76,0x64,0xD2,0xCB,0x03,0x79,
0x21,0xFB,0x3E,0xC7,0xAB,0x0A,0x4A,0xAD,0x9E,0x7A,0x3D,0x5F,0x62,0x6A,0x6C,0x18,0x46,0x5B,0x15,0xC6,
0x46,0x47,0x11,0x8E,0xA7,0x91,0x12,0xF2,0x48,0x0B,0x79,0x6A,0xB6,0x55,0x52,0x6D,0xB1,0x49,0x4A,0x27,
0x62,0xCE,0x35,0xF5,0x6D,0x2F,0x25,0x17,0x43,0x67,0xA6,0xFB,0x4F,0x7E,0x85,0x10,0x92,0x41,0x4F,0x0F,
0x38,0x00,0x04,0x94,0xC2,0x5A,0x5A,0xAA,0xB4,0x54,0x35,0x3D,0xDB,0xD8,0xDC,0xBC,0x47,0xAB,0x51,0xB2,
0x91,0xF9,0xE0,0xD0,0x52,0x2A,0xBB,0x2E,0x91,0x48,0x50,0xCF,0xE8,0xF0,0x26,0x4B,0x63,0x67,0xBF,0x56,
0x21,0x73,0x88,0x92,0x44,0x93,0xCB,0x49,0xAD,0x58,0x10,0x3C,0x42,0x3A,0x95,0x94,0xAB,0xD4,0xBA,0x74,
0x62,0x89,0x9E,0x3D,0x75,0x94,0x18,0x4A,0xCA,0x24,0xAB,0xCD,0x41,0xC7,0xCE,0x7F,0x41,0x59,0x85,0x02,
0x19,0xA1,0xC0,0x65,0xB2,0x69,0x76,0x69,0x76,0x0A,0xE9,0x44,0xD4,0x2B,0xE6,0x85,0x97,0x43,0x63,0x03,
0x2F,0x02,0xC8,0xAF,0xFC,0xC2,0x49,0x64,0xE5,0x32,0x12,0x75,0x25,0xF6,0xED,0xF6,0x86,0x75,0x1F,0x56,
0x54,0x55,0x43,0xA3,0xE4,0x61,0x2E,0x2D,0x87,0xCF,0x3B,0x89,0x68,0x70,0x56,0xD4,0x1B,0xF4,0x6C,0x32,
0x9D,0x85,0x4A,0xA5,0x86,0xA9,0xBC,0x12,0xFF,0x78,0xEB,0x35,0x70,0x32,0x1E,0xAD,0xED,0x9D,0x50,0xA8,
0x34,0x52,0x3A,0xB9,0x0C,0x21,0x27,0x30,0xC9,0x4C,0x1E,0x45,0xA6,0x35,0x10,0x0A,0x12,0xFC,0x9E,0x0B,
0x10,0x32,0x49,0x21,0x97,0x4E,0x9F,0x58,0xF0,0x8F,0xBE,0x20,0xA6,0x12,0xA7,0x00,0x64,0x41,0x08,0xB0,
0x2A,0xC4,0xAE,0x1E,0x0E,0x9C,0x4C,0xA5,0x6A,0x94,0x58,0xA5,0xDD,0x68,0xB6,0x5A,0x39,0x39,0x67,0x91,
0xCB,0xB8,0xEF,0x57,0xD4,0xAD,0x2B,0xD6,0xE9,0x8A,0xA0,0x52,0xA9,0xA0,0xD1,0x9B,0x30,0x31,0xEE,0xC1,
0xB9,0x93,0x87,0xFB,0xAA,0xD6,0xB5,0x77,0x98,0xCB,0x1C,0xCA,0xC8,0x9C,0x1F,0x6B,0x9D,0xB5,0x48,0xC4,
0x63,0x34,0x12,0x0A,0x7A,0xB3,0xD9,0xAC,0x9F,0x4A,0xE2,0x7C,0x2C,0x32,0x3F,0x11,0x19,0x1F,0xFC,0x0B,
0x80,0xD1,0x7F,0x6B,0xDB,0x75,0x0F,0x8B,0xDE,0x5E,0x69,0x75,0x66,0xB8,0x6A,0x92,0x55,0xE9,0xCD,0x2D,
0x2A,0x43,0xF1,0xC6,0xBC,0x20,0x14,0x17,0xAF,0x29,0x95,0x53,0xB0,0xF9,0xA0,0x67,0xE8,0x4C,0x36,0x19,
0xEB,0x53,0xE9,0xCC,0xAD,0x9C,0x4A,0xDB,0x90,0xC9,0x24,0x99,0x22,0x83,0x85,0x46,0xA7,0x2E,0x4C,0x01,
0x38,0x0B,0x40,0xF8,0x8F,0x9F,0x08,0xDC,0xCF,0x3D,0xC7,0xAC,0xD8,0x4F,0xBA,0x5C,0x58,0xF9,0x17,0xA0,
0x88,0xAF,0x30,0xC2,0x28,0x4E,0x72,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82,
};
static const size_t s_favicon_png_len = sizeof(s_favicon_png);

/* ---------------- NVS keys ---------------- */
#define NVS_NS_WIFI   "wifi"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"
#define NVS_KEY_HOST  "hostname"
#define NVS_KEY_DHCP  "dhcp"
#define NVS_KEY_IP    "ip"
#define NVS_KEY_GW    "gw"
#define NVS_KEY_NM    "nm"

/* MQTT config (keep in sync with mqtt_mgr.c) */
#define NVS_NS_MQTT          "mqtt"
#define NVS_KEY_MQTT_URI     "uri"
#define NVS_KEY_MQTT_USER    "user"
#define NVS_KEY_MQTT_PASS    "pass"
#define NVS_KEY_MQTT_TOPIC   "topic"
#define NVS_KEY_MQTT_DISC    "disc"
#define NVS_KEY_MQTT_ENABLE  "enable"

/* Offsets (from your ui.c) for factory reset */
#define NVS_NS_LEVELER   "leveler"
#define NVS_KEY_ROLL0    "roll0_md"
#define NVS_KEY_PITCH0   "pitch0_md"

/* Vehicle config (for UI / status) */
#define NVS_NS_CONFIG       "config"
#define NVS_KEY_WHEELBASE   "wheelbase_in"
#define NVS_KEY_TRACKWIDTH  "trackwidth_in"
#define NVS_KEY_SCREEN_TO   "screen_to_s"

/* ---------------- AP defaults ---------------- */
#define AP_IP_ADDR    "192.168.4.1"
#define AP_NETMASK    "255.255.255.0"
#define AP_GW_ADDR    "192.168.4.1"
#define AP_CHANNEL    6
#define AP_MAX_CONN   4
#define AP_PASSWORD   "leveler-setup"  // WPA2-PSK requires >= 8 chars

/* ---------------- state ---------------- */
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap  = NULL;

static httpd_handle_t s_httpd = NULL;
static bool s_http_started = false;
static dns_server_handle_t s_dns = NULL;

static bool s_have_creds = false;
static bool s_running_ap = false;

static bool s_wifi_inited = false;
static int s_sta_failures = 0;
static bool s_forced_ap = false;
static int64_t s_sta_first_fail_us = 0;

#define STA_RETRY_BEFORE_AP 5
#define STA_MIN_FAIL_TIME_SEC 30

/* ---------------- creds buffer ---------------- */
static char s_ssid[33] = {0};   // 32 + null
static char s_pass[65] = {0};   // 64 + null
static char s_hostname[33] = {0};
static bool s_use_dhcp = true;
static char s_sta_ip[16] = {0};
static char s_sta_gw[16] = {0};
static char s_sta_nm[16] = {0};

/* ---------------- mqtt config cache for /status ---------------- */
static char s_mqtt_uri[128] = {0};
static char s_mqtt_user[64] = {0};
static char s_mqtt_pass[64] = {0};
static char s_mqtt_topic[32] = {0};
static char s_mqtt_disc[32] = {0};
static bool s_mqtt_enable = false;
static bool s_have_mqtt = false;

#define UI_VERSION "2026-01-29-6"

/* ---------------- vehicle config cache ---------------- */
static char s_wheelbase_in[16] = {0};
static char s_trackwidth_in[16] = {0};
static float s_wheelbase_val = 133.0f;
static float s_trackwidth_val = 65.2f;
static uint32_t s_screen_timeout_s = 60;

#define DEG2RAD (0.017453292519943295f)

/* ---------------- angle cache for /status ---------------- */
static portMUX_TYPE s_angle_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_last_roll_deg = 0.0f;
static float s_last_pitch_deg = 0.0f;
static float s_last_ax = 0.0f;
static float s_last_ay = 0.0f;
static float s_last_az = 0.0f;

/* =========================================================
 * Small helpers
 * ========================================================= */
static void safe_strcpy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) { dst[0] = 0; return; }
    strlcpy(dst, src, dst_len);
}

static void ip4_to_str(const esp_ip4_addr_t *a, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (!a) { safe_strcpy(out, out_len, ""); return; }
    char tmp[16];
    snprintf(tmp, sizeof(tmp), IPSTR, IP2STR(a));
    safe_strcpy(out, out_len, tmp);
}

// Defaults used when no MQTT config is saved.
static void mqtt_defaults(void)
{
    safe_strcpy(s_mqtt_uri, sizeof(s_mqtt_uri), CONFIG_LEVELUP_MQTT_BROKER_URI);
    safe_strcpy(s_mqtt_user, sizeof(s_mqtt_user), CONFIG_LEVELUP_MQTT_USERNAME);
    safe_strcpy(s_mqtt_pass, sizeof(s_mqtt_pass), CONFIG_LEVELUP_MQTT_PASSWORD);
    safe_strcpy(s_mqtt_topic, sizeof(s_mqtt_topic), CONFIG_LEVELUP_MQTT_TOPIC_PREFIX);
    safe_strcpy(s_mqtt_disc, sizeof(s_mqtt_disc), CONFIG_LEVELUP_MQTT_DISCOVERY_PREFIX);
    s_mqtt_enable = false;
}

// Defaults used when no vehicle config is saved.
static void config_defaults(void)
{
    safe_strcpy(s_wheelbase_in, sizeof(s_wheelbase_in), "133");
    safe_strcpy(s_trackwidth_in, sizeof(s_trackwidth_in), "65.2");
    s_wheelbase_val = 133.0f;
    s_trackwidth_val = 65.2f;
    s_screen_timeout_s = 60;
}

// Defaults used when no network override is saved.
static void network_defaults(void)
{
    safe_strcpy(s_hostname, sizeof(s_hostname), "levelup");
    s_use_dhcp = true;
    s_sta_ip[0] = 0;
    s_sta_gw[0] = 0;
    s_sta_nm[0] = 0;
}

// Minimal URL decode for form bodies.
static void url_decode_inplace(char *s)
{
    // minimal + and %xx decode, enough for SSID/pass
    if (!s) return;
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *o++ = ' '; continue; }
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
            continue;
        }
        *o++ = *p;
    }
    *o = 0;
}

static void trim_trailing_zeros(char *s)
{
    if (!s || !s[0]) return;
    char *dot = strchr(s, '.');
    if (!dot) return;
    char *end = s + strlen(s) - 1;
    while (end > dot && *end == '0') {
        *end-- = '\0';
    }
    if (end == dot) {
        *end = '\0';
    }
}

static bool parse_positive_number(const char *in, char *out, size_t out_len)
{
    if (!in || !in[0]) return false;
    errno = 0;
    char *end = NULL;
    double v = strtod(in, &end);
    if (end == in) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return false;
    if (errno != 0 || !isfinite(v) || v <= 0.0 || v > 1000.0) return false;
    if (out && out_len > 0) {
        snprintf(out, out_len, "%.3f", v);
        trim_trailing_zeros(out);
    }
    return true;
}

static float parse_or_default(const char *s, float def)
{
    if (!s || !s[0]) return def;
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return def;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return def;
    if (errno != 0 || !isfinite(v) || v <= 0.0) return def;
    return (float)v;
}

static uint32_t parse_u32_or_default(const char *s, uint32_t def)
{
    if (!s || !s[0]) return def;
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) return def;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return def;
    if (errno != 0) return def;
    if (v > 86400UL) v = 86400UL;
    return (uint32_t)v;
}

static bool parse_ipv4_str(const char *in, char *out, size_t out_len)
{
    if (!in || !in[0]) return false;
    esp_ip4_addr_t ip = {0};
    ip.addr = esp_ip4addr_aton(in);
    if (ip.addr == 0 && strcmp(in, "0.0.0.0") != 0) return false;
    ip4_to_str(&ip, out, out_len);
    return (out[0] != 0);
}

// Escape user-provided strings before embedding in HTML.
static void html_escape(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (!in) { out[0] = 0; return; }

    size_t used = 0;
    for (const char *p = in; *p && used + 1 < out_len; p++) {
        const char *rep = NULL;
        switch (*p) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default:
                out[used++] = *p;
                continue;
        }
        size_t rep_len = strlen(rep);
        if (used + rep_len >= out_len) break;
        memcpy(out + used, rep, rep_len);
        used += rep_len;
    }
    out[used] = 0;
}

// Tiny form parser for key=value&key2=value2 bodies.
static bool form_get_value(const char *body, const char *key, char *out, size_t out_len)
{
    // parse key=value&key2=value2 (tiny form parser)
    if (!body || !key || !out || out_len == 0) return false;

    const size_t klen = strlen(key);
    const char *p = body;

    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;

        const char *amp = strchr(eq + 1, '&');
        size_t name_len = (size_t)(eq - p);

        if (name_len == klen && strncmp(p, key, klen) == 0) {
            size_t val_len = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
            if (val_len >= out_len) val_len = out_len - 1;
            memcpy(out, eq + 1, val_len);
            out[val_len] = 0;
            url_decode_inplace(out);
            return true;
        }

        p = amp ? amp + 1 : NULL;
    }
    return false;
}

/* =========================================================
 * NVS helpers
 * ========================================================= */
// Load Wi-Fi credentials from NVS.
static bool nvs_load_wifi(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) != ESP_OK) return false;

    size_t ssid_sz = ssid_len;
    size_t pass_sz = pass_len;

    esp_err_t e1 = nvs_get_str(h, NVS_KEY_SSID, ssid_out, &ssid_sz);
    esp_err_t e2 = nvs_get_str(h, NVS_KEY_PASS, pass_out, &pass_sz);

    nvs_close(h);
    return (e1 == ESP_OK && e2 == ESP_OK && ssid_out[0] != '\0');
}

// Load optional STA network overrides (hostname + DHCP/static).
static void nvs_load_network(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) != ESP_OK) return;

    size_t sz = sizeof(s_hostname);
    (void)nvs_get_str(h, NVS_KEY_HOST, s_hostname, &sz);

    uint8_t dhcp = 1;
    if (nvs_get_u8(h, NVS_KEY_DHCP, &dhcp) == ESP_OK) {
        s_use_dhcp = (dhcp != 0);
    }

    sz = sizeof(s_sta_ip);
    (void)nvs_get_str(h, NVS_KEY_IP, s_sta_ip, &sz);
    sz = sizeof(s_sta_gw);
    (void)nvs_get_str(h, NVS_KEY_GW, s_sta_gw, &sz);
    sz = sizeof(s_sta_nm);
    (void)nvs_get_str(h, NVS_KEY_NM, s_sta_nm, &sz);
    nvs_close(h);
}

// Load MQTT config from NVS (if any).
static bool nvs_load_mqtt(char *uri_out, size_t uri_len,
                          char *user_out, size_t user_len,
                          char *pass_out, size_t pass_len,
                          char *topic_out, size_t topic_len,
                          char *disc_out, size_t disc_len,
                          bool *enable_out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_MQTT, NVS_READONLY, &h) != ESP_OK) return false;

    bool any = false;
    size_t sz = 0;

    sz = uri_len;
    if (nvs_get_str(h, NVS_KEY_MQTT_URI, uri_out, &sz) == ESP_OK) any = true;
    sz = user_len;
    if (nvs_get_str(h, NVS_KEY_MQTT_USER, user_out, &sz) == ESP_OK) any = true;
    sz = pass_len;
    if (nvs_get_str(h, NVS_KEY_MQTT_PASS, pass_out, &sz) == ESP_OK) any = true;
    sz = topic_len;
    if (nvs_get_str(h, NVS_KEY_MQTT_TOPIC, topic_out, &sz) == ESP_OK) any = true;
    sz = disc_len;
    if (nvs_get_str(h, NVS_KEY_MQTT_DISC, disc_out, &sz) == ESP_OK) any = true;
    if (enable_out) {
        uint8_t en = 0;
        if (nvs_get_u8(h, NVS_KEY_MQTT_ENABLE, &en) == ESP_OK) {
            *enable_out = (en != 0);
            any = true;
        }
    }

    nvs_close(h);
    return any;
}

// Load vehicle config from NVS (if any).
static bool nvs_load_config(char *wheelbase_out, size_t wheelbase_len,
                            char *trackwidth_out, size_t trackwidth_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CONFIG, NVS_READONLY, &h) != ESP_OK) return false;

    bool any = false;
    size_t sz = 0;

    sz = wheelbase_len;
    if (nvs_get_str(h, NVS_KEY_WHEELBASE, wheelbase_out, &sz) == ESP_OK) any = true;
    sz = trackwidth_len;
    if (nvs_get_str(h, NVS_KEY_TRACKWIDTH, trackwidth_out, &sz) == ESP_OK) any = true;

    nvs_close(h);
    return any;
}

// Load display timeout (seconds) from NVS config namespace.
static bool nvs_load_screen_timeout(uint32_t *timeout_s_out)
{
    if (!timeout_s_out) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CONFIG, NVS_READONLY, &h) != ESP_OK) return false;
    uint32_t v = 0;
    esp_err_t err = nvs_get_u32(h, NVS_KEY_SCREEN_TO, &v);
    nvs_close(h);
    if (err != ESP_OK) return false;
    *timeout_s_out = v;
    return true;
}

// Persist Wi-Fi credentials to NVS.
static esp_err_t nvs_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h), TAG, "nvs_open failed");

    ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_SSID, ssid), TAG, "nvs_set_str ssid failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_PASS, pass ? pass : ""), TAG, "nvs_set_str pass failed");

    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Persist STA network overrides.
static esp_err_t nvs_save_network(const char *hostname,
                                  bool use_dhcp,
                                  const char *ip,
                                  const char *gw,
                                  const char *nm)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h), TAG, "nvs_open wifi failed");

    if (hostname && hostname[0]) {
        ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_HOST, hostname), TAG, "nvs_set_str host failed");
    } else {
        nvs_erase_key(h, NVS_KEY_HOST);
    }

    ESP_RETURN_ON_ERROR(nvs_set_u8(h, NVS_KEY_DHCP, use_dhcp ? 1 : 0), TAG, "nvs_set_u8 dhcp failed");

    if (ip && ip[0]) ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_IP, ip), TAG, "nvs_set_str ip failed");
    else nvs_erase_key(h, NVS_KEY_IP);

    if (gw && gw[0]) ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_GW, gw), TAG, "nvs_set_str gw failed");
    else nvs_erase_key(h, NVS_KEY_GW);

    if (nm && nm[0]) ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_NM, nm), TAG, "nvs_set_str nm failed");
    else nvs_erase_key(h, NVS_KEY_NM);

    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Persist MQTT configuration to NVS.
static esp_err_t nvs_save_mqtt(const char *uri,
                               const char *user,
                               const char *pass,
                               bool keep_pass,
                               const char *topic,
                               const char *disc,
                               bool enable)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS_MQTT, NVS_READWRITE, &h), TAG, "nvs_open mqtt failed");

    if (uri) {
        if (uri[0]) {
            ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_MQTT_URI, uri), TAG, "nvs_set_str uri failed");
        } else {
            nvs_erase_key(h, NVS_KEY_MQTT_URI);
        }
    }

    if (user) {
        if (user[0]) {
            ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_MQTT_USER, user), TAG, "nvs_set_str user failed");
        } else {
            nvs_erase_key(h, NVS_KEY_MQTT_USER);
        }
    }

    if (!keep_pass) {
        if (pass && pass[0]) {
            ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_MQTT_PASS, pass), TAG, "nvs_set_str pass failed");
        } else {
            nvs_erase_key(h, NVS_KEY_MQTT_PASS);
        }
    }

    if (topic) {
        if (topic[0]) {
            ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_MQTT_TOPIC, topic), TAG, "nvs_set_str topic failed");
        } else {
            nvs_erase_key(h, NVS_KEY_MQTT_TOPIC);
        }
    }

    if (disc) {
        if (disc[0]) {
            ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_MQTT_DISC, disc), TAG, "nvs_set_str disc failed");
        } else {
            nvs_erase_key(h, NVS_KEY_MQTT_DISC);
        }
    }

    ESP_RETURN_ON_ERROR(nvs_set_u8(h, NVS_KEY_MQTT_ENABLE, enable ? 1 : 0),
                        TAG, "nvs_set_u8 enable failed");

    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Persist vehicle config to NVS.
static esp_err_t nvs_save_config(const char *wheelbase,
                                 const char *trackwidth)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS_CONFIG, NVS_READWRITE, &h), TAG, "nvs_open config failed");

    if (wheelbase && wheelbase[0]) {
        ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_WHEELBASE, wheelbase), TAG, "nvs_set_str wheelbase failed");
    } else {
        nvs_erase_key(h, NVS_KEY_WHEELBASE);
    }

    if (trackwidth && trackwidth[0]) {
        ESP_RETURN_ON_ERROR(nvs_set_str(h, NVS_KEY_TRACKWIDTH, trackwidth), TAG, "nvs_set_str trackwidth failed");
    } else {
        nvs_erase_key(h, NVS_KEY_TRACKWIDTH);
    }

    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Persist display timeout (seconds).
static esp_err_t nvs_save_screen_timeout(uint32_t timeout_s)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS_CONFIG, NVS_READWRITE, &h), TAG, "nvs_open config failed");
    ESP_RETURN_ON_ERROR(nvs_set_u32(h, NVS_KEY_SCREEN_TO, timeout_s), TAG, "nvs_set_u32 timeout failed");
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Clear all persisted settings and offsets.
static void nvs_factory_reset(void)
{
    // Clear Wi-Fi creds
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    // Clear offsets (keep consistent with your ui.c keys)
    if (nvs_open(NVS_NS_LEVELER, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_ROLL0);
        nvs_erase_key(h, NVS_KEY_PITCH0);
        nvs_commit(h);
        nvs_close(h);
    }

    if (nvs_open(NVS_NS_MQTT, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    if (nvs_open(NVS_NS_CONFIG, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* =========================================================
 * Wi-Fi scan helpers (AP setup)
 * ========================================================= */
typedef struct {
    char ssid[33];
    int  rssi;
    wifi_auth_mode_t auth;
} ap_item_t;

static int do_scan(ap_item_t *out, int max_items)
{
    if (!out || max_items <= 0) return 0;

    // Scan while AP is up: requires APSTA mode (we use that in start_ap()).
    wifi_scan_config_t sc = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = true
    };

    esp_err_t err = esp_wifi_scan_start(&sc, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t n = 0;
    err = esp_wifi_scan_get_ap_num(&n);
    if (err != ESP_OK || n == 0) return 0;

    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) return 0;

    uint16_t got = n;
    err = esp_wifi_scan_get_ap_records(&got, recs);
    if (err != ESP_OK) {
        free(recs);
        return 0;
    }

    // copy unique SSIDs (simple de-dupe)
    int out_n = 0;
    for (int i = 0; i < got && out_n < max_items; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid || ssid[0] == 0) continue;

        bool dup = false;
        for (int j = 0; j < out_n; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;

        safe_strcpy(out[out_n].ssid, sizeof(out[out_n].ssid), ssid);
        out[out_n].rssi = recs[i].rssi;
        out[out_n].auth = recs[i].authmode;
        out_n++;
    }

    free(recs);
    return out_n;
}

/* =========================================================
 * HTTP server helpers (chunked responses)
 * ========================================================= */
static esp_err_t send_chunk(httpd_req_t *req, const char *s)
{
    if (!s) s = "";
    return httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_chunkf(httpd_req_t *req, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return ESP_FAIL;
    return httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_status_get(httpd_req_t *req);
static esp_err_t start_ap(void);
static esp_err_t http_config_save_post(httpd_req_t *req);
static esp_err_t http_favicon_get(httpd_req_t *req);
static esp_err_t http_network_save_post(httpd_req_t *req);
static esp_err_t http_display_save_post(httpd_req_t *req);

static void start_captive_portal(void)
{
    if (s_dns) return;
    dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&config);
    if (!s_dns) {
        ESP_LOGW(TAG, "DNS captive portal start failed");
    } else {
        ESP_LOGI(TAG, "DNS captive portal started");
    }
}

static void stop_captive_portal(void)
{
    if (s_dns) {
        stop_dns_server(s_dns);
        s_dns = NULL;
        ESP_LOGI(TAG, "DNS captive portal stopped");
    }
}

static esp_err_t http_captive_redirect_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

/* =========================================================
 * HTML: Setup page (with scan dropdown)
 * ========================================================= */
// Setup page (AP mode) or status redirect (STA mode).
static esp_err_t http_root_get(httpd_req_t *req)
{
    if (s_have_creds && !s_running_ap) {
        return http_status_get(req);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    // Simple single page:
    // - button to scan (AJAX) -> populates select
    // - manual SSID/password fields remain
    send_chunk(req,
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    );
    send_chunk(req, "<link rel='icon' type='image/png' href='/favicon.png'/>");
    send_chunk(req,
        "<title>Leveler Setup Portal</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:24px;}"
        ".card{max-width:520px;margin:0 auto;padding:18px;border:1px solid #ddd;border-radius:18px;}"
        "label{display:block;margin:12px 0 6px;font-weight:600;}"
        "input,select{width:100%;font-size:16px;padding:10px;border:1px solid #ccc;border-radius:10px;}"
        "button{margin-top:14px;width:100%;padding:12px;font-size:16px;border:0;border-radius:12px;background:#111;color:#fff;}"
        ".muted{color:#666;font-size:13px}"
        "</style>"
        "</head><body><div class='card'>"
        "<h2>Leveler Setup Portal</h2>"
        "<p class='muted'>Connect your Leveler to Wi-Fi so the status page and MQTT can be used.</p>"
        "<button id='scanBtn' type='button'>Scan for Networks</button>"
        "<label>Networks</label>"
        "<select id='netSelect'><option value=''>Press \"Scan for Networks\" First</option></select>"
        "<form method='POST' action='/save'>"
        "<label>SSID</label><input id='ssid' name='ssid' maxlength='32' required>"
        "<label>Password</label><input name='pass' maxlength='64' type='password'>"
        "<button type='submit'>Save &amp; Reboot</button>"
        "</form>"
        "<p class='muted' style='margin-top:10px'>Already connected? Visit <a href='/status'>Status</a> or <a href='/status.json'>JSON</a>.</p>"
        "<script>"
        "const sel=document.getElementById('netSelect');"
        "const ssid=document.getElementById('ssid');"
        "sel.addEventListener('change',()=>{ if(sel.value) ssid.value=sel.value; });"
        "document.getElementById('scanBtn').addEventListener('click', async ()=>{"
        "  sel.innerHTML='<option>Scanning for networks</option>';"
        "  try{"
        "    const r=await fetch('/scan.json',{cache:'no-store'});"
        "    const j=await r.json();"
        "    const items=j.networks||[];"
        "    sel.innerHTML='<option value=\"\">Select SSID</option>';"
        "    items.forEach(n=>{"
        "      const o=document.createElement('option');"
        "      o.value=n.ssid;"
        "      o.textContent=`${n.ssid}  (${n.rssi} dBm)`;"
        "      sel.appendChild(o);"
        "    });"
        "    if(items.length===0) sel.innerHTML='<option value=\"\">No networks found</option>';"
        "  }catch(e){"
        "    sel.innerHTML='<option value=\"\">Scan failed</option>';"
        "  }"
        "});"
        "</script>"
        "</div></body></html>"
    );

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* =========================================================
 * HTTP: /scan.json
 * ========================================================= */
static esp_err_t http_scan_json_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    ap_item_t items[20];
    int n = do_scan(items, (int)(sizeof(items)/sizeof(items[0])));

    send_chunk(req, "{");
    send_chunkf(req, "\"count\":%d,", n);
    send_chunk(req, "\"networks\":[");
    for (int i = 0; i < n; i++) {
        const char *comma = (i == n - 1) ? "" : ",";
        // Minimal JSON escaping for SSID (handle quotes/backslashes)
        char esc[80] = {0};
        const char *in = items[i].ssid;
        char *o = esc;
        size_t left = sizeof(esc) - 1;
        while (*in && left > 0) {
            if (*in == '\"' || *in == '\\') {
                if (left < 2) break;
                *o++ = '\\'; *o++ = *in++; left -= 2;
            } else {
                *o++ = *in++; left--;
            }
        }
        *o = 0;

        send_chunkf(req, "{\"ssid\":\"%s\",\"rssi\":%d}%s", esc, items[i].rssi, comma);
    }
    send_chunk(req, "]}");

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* =========================================================
 * HTTP: /save (POST)
 * ========================================================= */
// Save Wi-Fi credentials and reboot.
static esp_err_t http_save_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
        return ESP_OK;
    }

    char *body = (char *)calloc(1, (size_t)total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = 0;

    char ssid[33] = {0};
    char pass[65] = {0};

    if (!form_get_value(body, "ssid", ssid, sizeof(ssid))) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_OK;
    }
    (void)form_get_value(body, "pass", pass, sizeof(pass)); // optional for open nets

    free(body);

    ESP_LOGI(TAG, "Saving Wi-Fi SSID='%s' (pass len=%d)", ssid, (int)strlen(pass));
    if (nvs_save_wifi(ssid, pass) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><h3>Saved!</h3><p>Rebooting...</p></body></html>");

    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

/* =========================================================
 * HTTP: /mqtt_save (POST)
 * ========================================================= */
// Save MQTT settings from the status page.
static esp_err_t http_mqtt_save_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
        return ESP_OK;
    }

    char *body = (char *)calloc(1, (size_t)total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = 0;

    char uri[128] = {0};
    char user[64] = {0};
    char pass[64] = {0};
    char topic[32] = {0};
    char disc[32] = {0};
    char clear_pass[8] = {0};

    (void)form_get_value(body, "mqtt_uri", uri, sizeof(uri));
    (void)form_get_value(body, "mqtt_user", user, sizeof(user));
    bool pass_present = form_get_value(body, "mqtt_pass", pass, sizeof(pass));
    (void)form_get_value(body, "mqtt_topic", topic, sizeof(topic));
    (void)form_get_value(body, "mqtt_disc", disc, sizeof(disc));
    bool enable_flag = form_get_value(body, "mqtt_enable", clear_pass, sizeof(clear_pass));
    bool clear_pass_flag = form_get_value(body, "mqtt_clear_pass", clear_pass, sizeof(clear_pass));

    free(body);

    const char *pass_ptr = pass;
    bool keep_pass = false;
    if (clear_pass_flag) {
        pass_ptr = "";
    } else if (!pass_present || pass[0] == '\0') {
        keep_pass = true;
        pass_ptr = NULL;
    }

    ESP_LOGI(TAG, "Saving MQTT config (uri='%s', user='%s', topic='%s', disc='%s', enable=%s, keep_pass=%s)",
             uri, user, topic, disc, enable_flag ? "YES" : "NO", keep_pass ? "YES" : "NO");

    if (nvs_save_mqtt(uri, user, pass_ptr, keep_pass, topic, disc, enable_flag) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_OK;
    }

    mqtt_defaults();
    if (uri[0]) safe_strcpy(s_mqtt_uri, sizeof(s_mqtt_uri), uri);
    if (user[0]) safe_strcpy(s_mqtt_user, sizeof(s_mqtt_user), user);
    if (topic[0]) safe_strcpy(s_mqtt_topic, sizeof(s_mqtt_topic), topic);
    if (disc[0]) safe_strcpy(s_mqtt_disc, sizeof(s_mqtt_disc), disc);
    if (!keep_pass) {
        safe_strcpy(s_mqtt_pass, sizeof(s_mqtt_pass), pass_ptr ? pass_ptr : "");
    }
    s_mqtt_enable = enable_flag;
    s_have_mqtt = true;

    ESP_LOGI(TAG, "Restarting MQTT after config save");
    mqtt_mgr_restart();

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><h3>Saved!</h3><p>Restarting MQTT...</p><p><a href='/status'>Back to status</a></p></body></html>");
    return ESP_OK;
}

/* =========================================================
 * HTTP: /config_save (POST)
 * ========================================================= */
// Save vehicle config from the status page.
static esp_err_t http_config_save_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
        return ESP_OK;
    }

    char *body = (char *)calloc(1, (size_t)total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = 0;

    char wheelbase_in[16] = {0};
    char trackwidth_in[16] = {0};
    char wheelbase_norm[16] = {0};
    char trackwidth_norm[16] = {0};

    bool have_wheelbase = form_get_value(body, "wheelbase_in", wheelbase_in, sizeof(wheelbase_in));
    bool have_trackwidth = form_get_value(body, "trackwidth_in", trackwidth_in, sizeof(trackwidth_in));

    free(body);

    if (have_wheelbase && wheelbase_in[0]) {
        if (!parse_positive_number(wheelbase_in, wheelbase_norm, sizeof(wheelbase_norm))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid wheelbase value");
            return ESP_OK;
        }
    }
    if (have_trackwidth && trackwidth_in[0]) {
        if (!parse_positive_number(trackwidth_in, trackwidth_norm, sizeof(trackwidth_norm))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid track width value");
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "Saving config (wheelbase='%s', trackwidth='%s')", wheelbase_norm, trackwidth_norm);

    if (nvs_save_config(wheelbase_norm, trackwidth_norm) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_OK;
    }

    config_defaults();
    if (wheelbase_norm[0]) safe_strcpy(s_wheelbase_in, sizeof(s_wheelbase_in), wheelbase_norm);
    if (trackwidth_norm[0]) safe_strcpy(s_trackwidth_in, sizeof(s_trackwidth_in), trackwidth_norm);
    s_wheelbase_val = parse_or_default(s_wheelbase_in, 133.0f);
    s_trackwidth_val = parse_or_default(s_trackwidth_in, 65.2f);
    mqtt_mgr_set_vehicle_config(s_wheelbase_val, s_trackwidth_val);

    network_defaults();
    nvs_load_network();
    if (s_hostname[0] == 0) {
        safe_strcpy(s_hostname, sizeof(s_hostname), "levelup");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><h3>Saved!</h3><p><a href='/status'>Back to status</a></p></body></html>");
    return ESP_OK;
}

/* =========================================================
 * HTTP: /network_save (POST)
 * ========================================================= */
// Save hostname + DHCP/static network settings and reboot.
static esp_err_t http_network_save_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
        return ESP_OK;
    }

    char *body = (char *)calloc(1, (size_t)total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = 0;

    char host[33] = {0};
    char ip_in[16] = {0};
    char gw_in[16] = {0};
    char nm_in[16] = {0};
    char ip_norm[16] = {0};
    char gw_norm[16] = {0};
    char nm_norm[16] = {0};

    (void)form_get_value(body, "hostname", host, sizeof(host));
    bool use_dhcp = form_get_value(body, "use_dhcp", ip_in, sizeof(ip_in));
    (void)form_get_value(body, "sta_ip", ip_in, sizeof(ip_in));
    (void)form_get_value(body, "sta_gw", gw_in, sizeof(gw_in));
    (void)form_get_value(body, "sta_nm", nm_in, sizeof(nm_in));
    free(body);

    if (host[0] == 0) {
        safe_strcpy(host, sizeof(host), "levelup");
    }

    if (!use_dhcp) {
        if (!parse_ipv4_str(ip_in, ip_norm, sizeof(ip_norm))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid static IP");
            return ESP_OK;
        }
        if (!parse_ipv4_str(gw_in, gw_norm, sizeof(gw_norm))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid gateway");
            return ESP_OK;
        }
        if (!parse_ipv4_str(nm_in, nm_norm, sizeof(nm_norm))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid netmask");
            return ESP_OK;
        }
    }

    if (nvs_save_network(host, use_dhcp, ip_norm, gw_norm, nm_norm) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_OK;
    }

    safe_strcpy(s_hostname, sizeof(s_hostname), host);
    s_use_dhcp = use_dhcp;
    safe_strcpy(s_sta_ip, sizeof(s_sta_ip), ip_norm);
    safe_strcpy(s_sta_gw, sizeof(s_sta_gw), gw_norm);
    safe_strcpy(s_sta_nm, sizeof(s_sta_nm), nm_norm);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><h3>Saved!</h3><p>Rebooting to apply network settings...</p></body></html>");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

/* =========================================================
 * HTTP: /display_save (POST)
 * ========================================================= */
// Save display timeout (seconds). Value 0 disables auto-blank.
static esp_err_t http_display_save_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
        return ESP_OK;
    }

    char *body = (char *)calloc(1, (size_t)total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        received += r;
    }
    body[received] = 0;

    char timeout_s_str[16] = {0};
    (void)form_get_value(body, "screen_timeout_s", timeout_s_str, sizeof(timeout_s_str));
    free(body);

    uint32_t timeout_s = parse_u32_or_default(timeout_s_str, 60);
    if (timeout_s > 86400U) timeout_s = 86400U;

    if (nvs_save_screen_timeout(timeout_s) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_OK;
    }

    s_screen_timeout_s = timeout_s;
    lvgl_port_set_screen_timeout_ms(s_screen_timeout_s * 1000U);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><h3>Saved!</h3><p><a href='/status'>Back to status</a></p></body></html>");
    return ESP_OK;
}

/* =========================================================
 * HTTP: /status.json
 * ========================================================= */
// Status JSON for local dashboards and diagnostics.
static esp_err_t http_status_json_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    // Angles
    float roll, pitch, ax, ay, az;
    portENTER_CRITICAL(&s_angle_mux);
    roll = s_last_roll_deg;
    pitch = s_last_pitch_deg;
    ax = s_last_ax;
    ay = s_last_ay;
    az = s_last_az;
    portEXIT_CRITICAL(&s_angle_mux);

    // IP info
    char ipbuf[16] = {0}, gwbuf[16] = {0}, nmbuf[16] = {0};
    int rssi = 0;

    if (!s_running_ap && s_netif_sta) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_netif_sta, &ip) == ESP_OK) {
            ip4_to_str(&ip.ip, ipbuf, sizeof(ipbuf));
            ip4_to_str(&ip.gw, gwbuf, sizeof(gwbuf));
            ip4_to_str(&ip.netmask, nmbuf, sizeof(nmbuf));
        }
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    } else if (s_running_ap && s_netif_ap) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_netif_ap, &ip) == ESP_OK) {
            ip4_to_str(&ip.ip, ipbuf, sizeof(ipbuf));
            ip4_to_str(&ip.gw, gwbuf, sizeof(gwbuf));
            ip4_to_str(&ip.netmask, nmbuf, sizeof(nmbuf));
        }
    }

    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    uint32_t free_heap = esp_get_free_heap_size();
    bool imu_ok = imu_task_is_healthy();
    uint32_t imu_age_ms = imu_task_ms_since_update();
    bool imu_stationary = imu_task_is_stationary();
    bool mqtt_connected = mqtt_mgr_is_connected();
    bool screen_on = lvgl_port_is_screen_on();

    send_chunk(req, "{");
    send_chunkf(req, "\"mode\":\"%s\",", s_running_ap ? "AP" : "STA");
    send_chunkf(req, "\"have_creds\":%s,", s_have_creds ? "true" : "false");
    send_chunkf(req, "\"configured_ssid\":\"%s\",", s_have_creds ? s_ssid : "");
    send_chunkf(req, "\"connected\":%s,", wifi_mgr_is_connected() ? "true" : "false");
    send_chunkf(req, "\"ip\":\"%s\",", ipbuf);
    send_chunkf(req, "\"gateway\":\"%s\",", gwbuf);
    send_chunkf(req, "\"netmask\":\"%s\",", nmbuf);
    send_chunkf(req, "\"rssi\":%d,", rssi);
    float roll_in = tanf(roll * DEG2RAD) * s_trackwidth_val;
    float pitch_in = tanf(pitch * DEG2RAD) * s_wheelbase_val;

    send_chunkf(req, "\"roll_deg\":%.3f,", roll);
    send_chunkf(req, "\"pitch_deg\":%.3f,", pitch);
    send_chunkf(req, "\"roll_in\":%.1f,", roll_in);
    send_chunkf(req, "\"pitch_in\":%.1f,", pitch_in);
    send_chunkf(req, "\"accel_x\":%.4f,", ax);
    send_chunkf(req, "\"accel_y\":%.4f,", ay);
    send_chunkf(req, "\"accel_z\":%.4f,", az);
    send_chunkf(req, "\"mqtt_saved\":%s,", s_have_mqtt ? "true" : "false");
    send_chunkf(req, "\"mqtt_enabled\":%s,", s_mqtt_enable ? "true" : "false");
    send_chunkf(req, "\"hostname\":\"%s\",", s_hostname);
    send_chunkf(req, "\"use_dhcp\":%s,", s_use_dhcp ? "true" : "false");
    send_chunkf(req, "\"static_ip\":\"%s\",", s_sta_ip);
    send_chunkf(req, "\"static_gateway\":\"%s\",", s_sta_gw);
    send_chunkf(req, "\"static_netmask\":\"%s\",", s_sta_nm);
    send_chunkf(req, "\"screen_timeout_s\":%u,", (unsigned)s_screen_timeout_s);
    send_chunkf(req, "\"wheelbase_in\":%s,", s_wheelbase_in[0] ? s_wheelbase_in : "null");
    send_chunkf(req, "\"trackwidth_in\":%s,", s_trackwidth_in[0] ? s_trackwidth_in : "null");
    send_chunkf(req, "\"uptime_s\":%u,", (unsigned)uptime_s);
    send_chunkf(req, "\"free_heap\":%u,", (unsigned)free_heap);
    send_chunkf(req, "\"imu_ok\":%s,", imu_ok ? "true" : "false");
    send_chunkf(req, "\"imu_age_ms\":%u,", (unsigned)imu_age_ms);
    send_chunkf(req, "\"imu_stationary\":%s,", imu_stationary ? "true" : "false");
    send_chunkf(req, "\"mqtt_connected\":%s,", mqtt_connected ? "true" : "false");
    send_chunkf(req, "\"screen_on\":%s", screen_on ? "true" : "false");
    send_chunk(req, "}");

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* =========================================================
 * HTTP: /status (HTML)
 * ========================================================= */
// Status HTML page with config forms.
static esp_err_t http_status_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    float roll, pitch, ax, ay, az;
    portENTER_CRITICAL(&s_angle_mux);
    roll = s_last_roll_deg;
    pitch = s_last_pitch_deg;
    ax = s_last_ax;
    ay = s_last_ay;
    az = s_last_az;
    portEXIT_CRITICAL(&s_angle_mux);

    char ipbuf[16] = {0}, gwbuf[16] = {0}, nmbuf[16] = {0};
    int rssi = 0;

    if (!s_running_ap && s_netif_sta) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_netif_sta, &ip) == ESP_OK) {
            ip4_to_str(&ip.ip, ipbuf, sizeof(ipbuf));
            ip4_to_str(&ip.gw, gwbuf, sizeof(gwbuf));
            ip4_to_str(&ip.netmask, nmbuf, sizeof(nmbuf));
        }
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    } else if (s_running_ap && s_netif_ap) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_get_ip_info(s_netif_ap, &ip) == ESP_OK) {
            ip4_to_str(&ip.ip, ipbuf, sizeof(ipbuf));
            ip4_to_str(&ip.gw, gwbuf, sizeof(gwbuf));
            ip4_to_str(&ip.netmask, nmbuf, sizeof(nmbuf));
        }
    }

    char mqtt_uri_esc[192] = {0};
    char mqtt_user_esc[96] = {0};
    char mqtt_pass_esc[96] = {0};
    char mqtt_topic_esc[64] = {0};
    char mqtt_disc_esc[64] = {0};
    html_escape(s_mqtt_uri, mqtt_uri_esc, sizeof(mqtt_uri_esc));
    html_escape(s_mqtt_user, mqtt_user_esc, sizeof(mqtt_user_esc));
    html_escape(s_mqtt_pass, mqtt_pass_esc, sizeof(mqtt_pass_esc));
    html_escape(s_mqtt_topic, mqtt_topic_esc, sizeof(mqtt_topic_esc));
    html_escape(s_mqtt_disc, mqtt_disc_esc, sizeof(mqtt_disc_esc));

    char wheelbase_esc[32] = {0};
    char trackwidth_esc[32] = {0};
    html_escape(s_wheelbase_in, wheelbase_esc, sizeof(wheelbase_esc));
    html_escape(s_trackwidth_in, trackwidth_esc, sizeof(trackwidth_esc));
    char hostname_esc[48] = {0};
    char ip_esc[24] = {0};
    char gw_esc[24] = {0};
    char nm_esc[24] = {0};
    char screen_timeout_esc[16] = {0};
    html_escape(s_hostname, hostname_esc, sizeof(hostname_esc));
    html_escape(s_sta_ip, ip_esc, sizeof(ip_esc));
    html_escape(s_sta_gw, gw_esc, sizeof(gw_esc));
    html_escape(s_sta_nm, nm_esc, sizeof(nm_esc));
    snprintf(screen_timeout_esc, sizeof(screen_timeout_esc), "%u", (unsigned)s_screen_timeout_s);

    send_chunk(req,
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    );
    send_chunk(req, "<link rel='icon' type='image/png' href='/favicon.png'/>");
    send_chunk(req,
        "<title>Leveler Dashboard</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:24px;background:#f7f7f8;color:#111;}"
        ".wrap{max-width:1100px;margin:0 auto;}"
        ".title{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px;}"
        ".title h2{margin:0;font-size:22px;}"
        ".grid{display:grid;grid-template-columns:1.2fr 1fr;gap:16px;}"
        ".card{background:#fff;padding:18px;border:1px solid #e6e6e9;border-radius:18px;box-shadow:0 1px 2px rgba(0,0,0,.04);}"
        "@media(max-width:900px){.grid{grid-template-columns:1fr;}}"
        "table{width:100%;border-collapse:collapse;margin-top:10px}"
        "td{padding:10px 8px;border-bottom:1px solid #eee;vertical-align:top}"
        "label{display:block;margin:10px 0 6px;font-weight:600;}"
        "input{width:100%;font-size:14px;padding:10px;border:1px solid #ccc;border-radius:10px;}"
        ".row{display:flex;gap:10px;flex-wrap:wrap}"
        ".row>div{flex:1;min-width:220px}"
        ".pw-wrap{display:flex;gap:8px;align-items:center}"
        ".pw-wrap input{flex:1}"
        ".pw-btn{width:44px;height:40px;display:inline-flex;align-items:center;justify-content:center;"
        "border:1px solid #ccc;border-radius:10px;background:#f5f5f5;color:#111}"
        ".pw-btn svg{width:20px;height:20px;fill:#333}"
        ".pw-btn.is-on{background:#111}"
        ".pw-btn.is-on svg{fill:#fff}"
        ".muted{color:#666;font-size:13px}"
        "button{margin-top:14px;width:100%;padding:12px;font-size:16px;border:0;border-radius:12px;background:#111;color:#fff;}"
        ".danger{background:#b00020}"
        "a{color:#005bd1;text-decoration:none}"
        "details{border:1px solid #ececf0;border-radius:12px;padding:8px 12px;margin-top:10px;background:#fcfcfd}"
        "summary{cursor:pointer;font-weight:700;list-style:none;}"
        "summary::-webkit-details-marker{display:none}"
        "details>summary::after{content:'+';float:right;color:#666}"
        "details[open]>summary::after{content:'-'}"
        "</style></head><body><div class='wrap'>"
        "<div class='title'><h2>Leveler Dashboard</h2>"
        "<span class='muted'>UI version: " UI_VERSION "</span></div>"
        "<div class='grid'>"
        "<div class='card'>"
        "<details open><summary>Status</summary>"
    );

    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    uint32_t free_heap = esp_get_free_heap_size();
    bool imu_ok = imu_task_is_healthy();
    uint32_t imu_age_ms = imu_task_ms_since_update();
    bool imu_stationary = imu_task_is_stationary();
    bool mqtt_connected = mqtt_mgr_is_connected();
    bool screen_on = lvgl_port_is_screen_on();

    send_chunk(req, "<table>");
    send_chunkf(req, "<tr><td><b>Mode</b></td><td>%s</td></tr>", s_running_ap ? "AP (setup)" : "STA");
    send_chunkf(req, "<tr><td><b>Configured SSID</b></td><td>%s</td></tr>", s_have_creds ? s_ssid : "(none)");
    send_chunkf(req, "<tr><td><b>Connected</b></td><td>%s</td></tr>", wifi_mgr_is_connected() ? "YES" : "NO");
    send_chunkf(req, "<tr><td><b>IP</b></td><td>%s</td></tr>", ipbuf[0] ? ipbuf : "(none)");
    send_chunkf(req, "<tr><td><b>Gateway</b></td><td>%s</td></tr>", gwbuf[0] ? gwbuf : "(none)");
    send_chunkf(req, "<tr><td><b>Netmask</b></td><td>%s</td></tr>", nmbuf[0] ? nmbuf : "(none)");
    send_chunkf(req, "<tr><td><b>RSSI</b></td><td>%d</td></tr>", rssi);
    float roll_in = tanf(roll * DEG2RAD) * s_trackwidth_val;
    float pitch_in = tanf(pitch * DEG2RAD) * s_wheelbase_val;

    send_chunkf(req, "<tr><td><b>Roll</b></td><td>%.3f&deg;</td></tr>", roll);
    send_chunkf(req, "<tr><td><b>Pitch</b></td><td>%.3f&deg;</td></tr>", pitch);
    send_chunkf(req, "<tr><td><b>Roll (in)</b></td><td>%.1f in</td></tr>", roll_in);
    send_chunkf(req, "<tr><td><b>Pitch (in)</b></td><td>%.1f in</td></tr>", pitch_in);
    send_chunkf(req, "<tr><td><b>Accel X</b></td><td>%.4f</td></tr>", ax);
    send_chunkf(req, "<tr><td><b>Accel Y</b></td><td>%.4f</td></tr>", ay);
    send_chunkf(req, "<tr><td><b>Accel Z</b></td><td>%.4f</td></tr>", az);
    send_chunkf(req, "<tr><td><b>MQTT config</b></td><td>%s</td></tr>",
                s_have_mqtt ? "Saved" : "Defaults");
    send_chunkf(req, "<tr><td><b>MQTT enabled</b></td><td>%s</td></tr>",
                s_mqtt_enable ? "YES" : "NO");
    send_chunkf(req, "<tr><td><b>Hostname</b></td><td>%s</td></tr>", s_hostname);
    send_chunkf(req, "<tr><td><b>IP mode</b></td><td>%s</td></tr>", s_use_dhcp ? "DHCP" : "Static");
    if (!s_use_dhcp) {
        send_chunkf(req, "<tr><td><b>Static IP</b></td><td>%s</td></tr>", s_sta_ip);
        send_chunkf(req, "<tr><td><b>Static Gateway</b></td><td>%s</td></tr>", s_sta_gw);
        send_chunkf(req, "<tr><td><b>Static Netmask</b></td><td>%s</td></tr>", s_sta_nm);
    }
    send_chunkf(req, "<tr><td><b>Screen timeout</b></td><td>%u s</td></tr>", (unsigned)s_screen_timeout_s);
    send_chunkf(req, "<tr><td><b>Wheelbase (in)</b></td><td>%s</td></tr>",
                s_wheelbase_in[0] ? s_wheelbase_in : "Default (133)");
    send_chunkf(req, "<tr><td><b>Track width (in)</b></td><td>%s</td></tr>",
                s_trackwidth_in[0] ? s_trackwidth_in : "Default (65.2)");
    send_chunk(req, "<tr><td colspan='2'><b>Diagnostics</b></td></tr>");
    send_chunkf(req, "<tr><td><b>Uptime</b></td><td>%u s</td></tr>", (unsigned)uptime_s);
    send_chunkf(req, "<tr><td><b>Free heap</b></td><td>%u bytes</td></tr>", (unsigned)free_heap);
    send_chunkf(req, "<tr><td><b>IMU status</b></td><td>%s</td></tr>", imu_ok ? "OK" : "STALE/ERROR");
    send_chunkf(req, "<tr><td><b>IMU age</b></td><td>%u ms</td></tr>", (unsigned)imu_age_ms);
    send_chunkf(req, "<tr><td><b>IMU stationary</b></td><td>%s</td></tr>", imu_stationary ? "YES" : "NO");
    send_chunkf(req, "<tr><td><b>MQTT connected</b></td><td>%s</td></tr>", mqtt_connected ? "YES" : "NO");
    send_chunkf(req, "<tr><td><b>Screen</b></td><td>%s</td></tr>", screen_on ? "ON" : "OFF");
    send_chunk(req, "</table></details></div>");

    send_chunk(req,
        "<div class='card'>"
        "<details open><summary>Settings</summary>"
        "<details open><summary>Networking</summary>"
        "<form method='POST' action='/network_save'>"
        "<label>Hostname</label>"
        "<input name='hostname' maxlength='32' value='"
    );
    if (hostname_esc[0]) {
        send_chunk(req, hostname_esc);
    } else {
        send_chunk(req, "levelup");
    }
    send_chunk(req,
        "'>"
        "<label><input id='dhcpToggle' type='checkbox' name='use_dhcp' "
    );
    if (s_use_dhcp) {
        send_chunk(req, "checked ");
    }
    send_chunk(req,
        "> Use DHCP</label>"
        "<div id='staticFields'>"
        "<label>Static IP</label>"
        "<input name='sta_ip' maxlength='15' placeholder='192.168.1.50' value='"
    );
    if (ip_esc[0]) send_chunk(req, ip_esc);
    send_chunk(req,
        "'>"
        "<label>Gateway</label>"
        "<input name='sta_gw' maxlength='15' placeholder='192.168.1.1' value='"
    );
    if (gw_esc[0]) send_chunk(req, gw_esc);
    send_chunk(req,
        "'>"
        "<label>Netmask</label>"
        "<input name='sta_nm' maxlength='15' placeholder='255.255.255.0' value='"
    );
    if (nm_esc[0]) send_chunk(req, nm_esc);
    send_chunk(req,
        "'>"
        "</div>"
        "<p class='muted'>Saving networking settings reboots the device.</p>"
        "<button type='submit'>Save Networking</button>"
        "</form>"
        "</details>"
        "<details><summary>Display</summary>"
        "<form method='POST' action='/display_save'>"
        "<label>Screen timeout (seconds)</label>"
        "<input name='screen_timeout_s' maxlength='5' inputmode='numeric' value='"
    );
    send_chunk(req, screen_timeout_esc);
    send_chunk(req,
        "'>"
        "<p class='muted'>Set to 0 to disable auto-blank.</p>"
        "<button type='submit'>Save Display</button>"
        "</form>"
        "</details>"
    );
    send_chunk(req,
        "<details><summary>Vehicle</summary>"
        "<form method='POST' action='/config_save'>"
        "<label>Wheelbase (inches)</label>"
        "<input name='wheelbase_in' maxlength='15' inputmode='decimal' value='"
    );
    if (wheelbase_esc[0]) {
        send_chunk(req, wheelbase_esc);
    } else {
        send_chunk(req, "133");
    }
    send_chunk(req,
        "'>"
        "<label>Track width (inches)</label>"
        "<input name='trackwidth_in' maxlength='15' inputmode='decimal' value='"
    );
    if (trackwidth_esc[0]) {
        send_chunk(req, trackwidth_esc);
    } else {
        send_chunk(req, "65.2");
    }
    send_chunk(req,
        "'>"
        "<p class='muted'>Leave blank to revert to defaults.</p>"
        "<button type='submit'>Save Vehicle Info</button>"
        "</form>"
        "</details>"
        "<details><summary>MQTT</summary>"
        "<form method='POST' action='/mqtt_save'>"
        "<label><input type='checkbox' name='mqtt_enable' "
    );
    if (s_mqtt_enable) {
        send_chunk(req, "checked ");
    }
    send_chunk(req,
        "> Enable MQTT</label>"
        "<label>Broker URI</label>"
        "<input name='mqtt_uri' maxlength='127' value='"
    );
    if (mqtt_uri_esc[0]) {
        send_chunk(req, mqtt_uri_esc);
    }
    send_chunk(req,
        "'>"
        "<label>Username</label>"
        "<input name='mqtt_user' maxlength='63' type='text' placeholder='username' value='"
    );
    if (mqtt_user_esc[0]) {
        send_chunk(req, mqtt_user_esc);
    }
    send_chunk(req,
        "'>"
        "<label>Password</label>"
        "<div class='pw-wrap'>"
        "<input id='mqttPass' name='mqtt_pass' maxlength='63' type='password' data-saved='"
    );
    if (mqtt_pass_esc[0]) {
        send_chunk(req, mqtt_pass_esc);
    }
    send_chunk(req,
        "' placeholder='"
    );
    send_chunk(req, s_mqtt_pass[0] ? "(stored)" : "(none)");
    send_chunk(req,
        "'>"
        "<button id='mqttPassBtn' class='pw-btn' type='button' aria-label='Show password'>"
        "<svg viewBox='0 0 24 24' aria-hidden='true'>"
        "<path d='M12 5c-5 0-9.3 3.1-11 7 1.7 3.9 6 7 11 7s9.3-3.1 11-7c-1.7-3.9-6-7-11-7zm0 11a4 4 0 1 1 0-8 4 4 0 0 1 0 8zm0-6a2 2 0 1 0 0 4 2 2 0 0 0 0-4z'/>"
        "</svg>"
        "</button>"
        "</div>"
        "<label><input type='checkbox' name='mqtt_clear_pass'> Clear stored password</label>"
        "<div class='row'>"
        "<div><label>Topic prefix</label>"
        "<input name='mqtt_topic' maxlength='31' value='"
    );
    if (mqtt_topic_esc[0]) {
        send_chunk(req, mqtt_topic_esc);
    }
    send_chunk(req,
        "'></div>"
        "<div><label>Discovery prefix</label>"
        "<input name='mqtt_disc' maxlength='31' value='"
    );
    if (mqtt_disc_esc[0]) {
        send_chunk(req, mqtt_disc_esc);
    }
    send_chunk(req,
        "'></div>"
        "</div>"
        "<p class='muted'>Leave fields blank to use defaults. Leave password blank to keep current.</p>"
        "<button type='submit'>Save MQTT info &amp; restart service</button>"
        "</form>"
        "</details>"
        "<details><summary>Advanced</summary>"
        "<p class='muted' style='margin-top:10px'>"
        "JSON endpoint: <a href='/status.json'>/status.json</a>"
        "</p>"
    );

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);
    send_chunkf(req,
        "<p class='muted'>Firmware: v%s &nbsp;|&nbsp; Built: %s %s</p>"
        "<p class='muted'>Running: %s &nbsp;&rarr;&nbsp; OTA target: %s</p>",
        app_desc->version, app_desc->date, app_desc->time,
        running ? running->label : "?",
        next    ? next->label    : "none"
    );
    send_chunk(req,
        "<details><summary>Firmware Update (OTA)</summary>"
        "<p class='muted' style='margin-top:8px'>Select the compiled <code>.bin</code> and press Upload.</p>"
        "<label>Firmware binary (.bin)</label>"
        "<input type='file' id='otaFile' accept='.bin'>"
        "<div id='otaBar' style='display:none;height:8px;background:#eee;border-radius:4px;margin:8px 0'>"
        "<div id='otaFill' style='height:100%;width:0;background:#111;border-radius:4px;transition:width .3s'></div></div>"
        "<p id='otaStatus' class='muted'></p>"
        "<button type='button' onclick='doOta()'>Upload &amp; Flash</button>"
        "</details>"
        "<form method='POST' action='/reset' "
        "onsubmit=\"return confirm('Factory reset? This clears Wi-Fi + offsets and reboots.');\">"
        "<button class='danger' type='submit'>Factory Reset</button>"
        "</form>"
        "<p class='muted' style='margin-top:10px'>Setup portal: <a href='/'>/</a></p>"
        "</details>"
        "<script>"
        "function doOta(){"
        "var f=document.getElementById('otaFile').files[0];"
        "if(!f){alert('Select a .bin file first');return;}"
        "var bar=document.getElementById('otaBar');"
        "var fill=document.getElementById('otaFill');"
        "var st=document.getElementById('otaStatus');"
        "bar.style.display='block';st.textContent='Uploading...';"
        "var xhr=new XMLHttpRequest();"
        "xhr.open('POST','/ota');"
        "xhr.setRequestHeader('Content-Type','application/octet-stream');"
        "xhr.upload.onprogress=function(e){"
        "if(e.lengthComputable){"
        "var p=Math.round(e.loaded/e.total*100);"
        "fill.style.width=p+'%';"
        "st.textContent='Uploading: '+p+'%';"
        "}};"
        "xhr.onload=function(){"
        "if(xhr.status===200){"
        "fill.style.width='100%';"
        "st.textContent='Done! Device rebooting...';"
        "}else{st.textContent='Error: '+xhr.responseText;bar.style.display='none';}"
        "};"
        "xhr.onerror=function(){st.textContent='Upload failed (network error)';bar.style.display='none';};"
        "xhr.send(f);}"
        "const dhcp=document.getElementById('dhcpToggle');"
        "const sf=document.getElementById('staticFields');"
        "const refreshStatic=()=>{ if(sf) sf.style.display=(dhcp&&dhcp.checked)?'none':'block'; };"
        "if(dhcp){ dhcp.addEventListener('change',refreshStatic); refreshStatic(); }"
        "const pass=document.getElementById('mqttPass');"
        "const btn=document.getElementById('mqttPassBtn');"
        "if(pass&&btn){"
        "btn.addEventListener('click',()=>{"
        "const saved=pass.getAttribute('data-saved')||'';"
        "if(pass.type==='password'){"
        "if(saved&&pass.value==='') pass.value=saved;"
        "pass.type='text';"
        "btn.classList.add('is-on');"
        "btn.setAttribute('aria-label','Hide password');"
        "}else{"
        "pass.type='password';"
        "btn.classList.remove('is-on');"
        "btn.setAttribute('aria-label','Show password');"
        "}"
        "});"
        "}"
        "</script>"
        "</details></div></div></div></body></html>"
    );

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* =========================================================
 * HTTP: /favicon.png
 * ========================================================= */
static esp_err_t http_favicon_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char *)s_favicon_png, (ssize_t)s_favicon_png_len);
}

/* =========================================================
 * HTTP: /reset (POST)
 * ========================================================= */
static esp_err_t http_reset_post(httpd_req_t *req)
{
    (void)req;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><h3>Resetting…</h3><p>Rebooting…</p></body></html>");

    nvs_factory_reset();

    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

/* =========================================================
 * HTTP: /ota (POST) — stream firmware binary directly into OTA partition
 * ========================================================= */
#define OTA_BUF_SIZE 4096

static esp_err_t http_ota_post(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 5 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition found");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: writing %d bytes to partition '%s'", content_len, update_part->label);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < content_len) {
        int to_read = content_len - received;
        if (to_read > OTA_BUF_SIZE) to_read = OTA_BUF_SIZE;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }
        received += r;
    }
    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA verify failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA complete — rebooting into new firmware");
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* =========================================================
 * HTTP server start / ensure
 * ========================================================= */
// HTTP server: setup UI, status UI, and endpoints.
static httpd_handle_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 22;
    cfg.stack_size = 8192;  // extra headroom for OTA flash writes
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) return NULL;

    // Root (setup UI)
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = http_root_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root);

    // Scan
    httpd_uri_t scan = {
        .uri = "/scan.json",
        .method = HTTP_GET,
        .handler = http_scan_json_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &scan);

    // Save
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = http_save_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &save);

    httpd_uri_t mqtt_save = {
        .uri = "/mqtt_save",
        .method = HTTP_POST,
        .handler = http_mqtt_save_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &mqtt_save);

    httpd_uri_t config_save = {
        .uri = "/config_save",
        .method = HTTP_POST,
        .handler = http_config_save_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &config_save);

    httpd_uri_t network_save = {
        .uri = "/network_save",
        .method = HTTP_POST,
        .handler = http_network_save_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &network_save);

    httpd_uri_t display_save = {
        .uri = "/display_save",
        .method = HTTP_POST,
        .handler = http_display_save_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &display_save);

    // Status
    httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = http_status_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &status);

    httpd_uri_t favicon = {
        .uri = "/favicon.png",
        .method = HTTP_GET,
        .handler = http_favicon_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &favicon);

    httpd_uri_t status_json = {
        .uri = "/status.json",
        .method = HTTP_GET,
        .handler = http_status_json_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &status_json);

    // Reset
    httpd_uri_t reset = {
        .uri = "/reset",
        .method = HTTP_POST,
        .handler = http_reset_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &reset);

    httpd_uri_t ota = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = http_ota_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota);

    // Captive portal detection endpoints -> redirect to "/"
    httpd_uri_t cp_generate = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = http_captive_redirect_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &cp_generate);

    httpd_uri_t cp_hotspot = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = http_captive_redirect_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &cp_hotspot);

    httpd_uri_t cp_ncsi = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = http_captive_redirect_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &cp_ncsi);

    httpd_uri_t cp_connect = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = http_captive_redirect_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &cp_connect);

    httpd_uri_t cp_success = {
        .uri = "/success.txt",
        .method = HTTP_GET,
        .handler = http_captive_redirect_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &cp_success);

    httpd_uri_t cp_fwlink = {
        .uri = "/fwlink",
        .method = HTTP_GET,
        .handler = http_captive_redirect_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &cp_fwlink);

    // Captive portal-ish wildcard: serve setup UI for unknown GETs
    httpd_uri_t wildcard = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = http_root_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wildcard);

    return server;
}

// Guard to avoid duplicate server creation.
static void ensure_http_server_started(void)
{
    if (s_http_started) return;

    s_httpd = start_http_server();
    if (s_httpd) {
        s_http_started = true;
        ESP_LOGI(TAG, "HTTP server started (mode=%s)", s_running_ap ? "AP" : "STA");
    } else {
        ESP_LOGW(TAG, "HTTP server failed to start");
    }
}

// Fallback to AP setup when STA fails repeatedly.
static void fallback_to_ap(void)
{
    if (s_forced_ap || s_running_ap) return;
    if (s_sta_first_fail_us == 0) {
        s_sta_first_fail_us = esp_timer_get_time();
    }
    int64_t elapsed_us = esp_timer_get_time() - s_sta_first_fail_us;
    if (elapsed_us < (int64_t)STA_MIN_FAIL_TIME_SEC * 1000000LL) {
        ESP_LOGW(TAG, "STA failures=%d but waiting %ds before AP fallback", s_sta_failures, STA_MIN_FAIL_TIME_SEC);
        return;
    }
    s_forced_ap = true;
    s_sta_failures = 0;
    s_sta_first_fail_us = 0;
    ESP_LOGW(TAG, "STA failed too many times, falling back to AP setup");
    esp_wifi_stop();
    start_ap();
}

/* =========================================================
 * Wi-Fi event handler
 * ========================================================= */
// Wi-Fi event handler for STA/AP lifecycle.
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                if (!s_running_ap) {
                    ESP_LOGI(TAG, "STA start -> connect");
                    s_sta_failures = 0;
                    s_sta_first_fail_us = 0;
                    esp_wifi_connect();
                } else {
                    ESP_LOGI(TAG, "STA start ignored (AP mode)");
                }
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "STA disconnected -> retry");
                xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
                if (!s_running_ap) {
                    s_sta_failures++;
                    if (s_sta_first_fail_us == 0) {
                        s_sta_first_fail_us = esp_timer_get_time();
                    }
                    if (s_have_creds && s_sta_failures >= STA_RETRY_BEFORE_AP) {
                        fallback_to_ap();
                        break;
                    }
                    esp_wifi_connect();
                }
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
                ESP_LOGI(TAG, "AP client connected: " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
                ESP_LOGI(TAG, "AP client disconnected: " MACSTR " AID=%d", MAC2STR(e->mac), e->aid);
                break;
            }

            default:
                break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
            ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&e->ip_info.ip));
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
            s_sta_failures = 0;
            s_forced_ap = false;
            s_sta_first_fail_us = 0;

            // <<< KEY FIX: start HTTP server in STA mode after we have an IP >>>
            ensure_http_server_started();
        }
#ifdef IP_EVENT_AP_STAIPASSIGNED
        else if (id == IP_EVENT_AP_STAIPASSIGNED) {
            ip_event_ap_staipassigned_t *e = (ip_event_ap_staipassigned_t *)data;
            ESP_LOGI(TAG, "AP assigned client IP: " IPSTR, IP2STR(&e->ip));
        }
#endif
    }
}

/* =========================================================
 * Wi-Fi init once
 * ========================================================= */
static esp_err_t wifi_init_once(void)
{
    if (s_wifi_inited) return ESP_OK;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    // Register handlers once
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG, "wifi event reg failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG, "ip event reg failed");

    s_wifi_inited = true;
    return ESP_OK;
}

/* =========================================================
 * STA mode
 * ========================================================= */
// Start station mode with given credentials.
static esp_err_t start_sta(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "Starting STA: ssid='%s'", ssid);

    s_running_ap = false;
    stop_captive_portal();

    if (!s_netif_sta) {
        s_netif_sta = esp_netif_create_default_wifi_sta();
        if (!s_netif_sta) return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi_init_once failed");

    wifi_config_t cfg = {0};
    safe_strcpy((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), ssid);
    safe_strcpy((char *)cfg.sta.password, sizeof(cfg.sta.password), pass ? pass : "");
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_netif_sta, s_hostname), TAG, "set hostname failed");

    if (s_use_dhcp) {
        // DHCP mode: ensure DHCP client is running and clear static info cache.
        (void)esp_netif_dhcpc_stop(s_netif_sta);
        ESP_RETURN_ON_ERROR(esp_netif_dhcpc_start(s_netif_sta), TAG, "dhcp start failed");
    } else {
        // Static mode: set IP/gateway/netmask before starting STA.
        esp_ip4_addr_t ip = {0}, gw = {0}, nm = {0};
        ip.addr = esp_ip4addr_aton(s_sta_ip);
        gw.addr = esp_ip4addr_aton(s_sta_gw);
        nm.addr = esp_ip4addr_aton(s_sta_nm);
        if ((ip.addr == 0 && strcmp(s_sta_ip, "0.0.0.0") != 0) ||
            (gw.addr == 0 && strcmp(s_sta_gw, "0.0.0.0") != 0) ||
            (nm.addr == 0 && strcmp(s_sta_nm, "0.0.0.0") != 0)) {
            ESP_LOGW(TAG, "Invalid static IP config; falling back to DHCP");
            s_use_dhcp = true;
            (void)esp_netif_dhcpc_stop(s_netif_sta);
            ESP_RETURN_ON_ERROR(esp_netif_dhcpc_start(s_netif_sta), TAG, "dhcp start failed");
        } else {
            esp_netif_ip_info_t info = {0};
            info.ip = ip;
            info.gw = gw;
            info.netmask = nm;
            (void)esp_netif_dhcpc_stop(s_netif_sta);
            ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_netif_sta, &info), TAG, "set static ip failed");
        }
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode sta failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    // iPhone + some routers behave better with PS disabled during onboarding/testing
    esp_wifi_set_ps(WIFI_PS_NONE);

    // HTTP server will start on STA_GOT_IP (see handler)
    return ESP_OK;
}

/* =========================================================
 * AP mode (setup)
 * ========================================================= */
// Start AP mode for first-time setup.
static esp_err_t start_ap(void)
{
    s_running_ap = true;

    if (!s_netif_ap) {
        s_netif_ap = esp_netif_create_default_wifi_ap();
        if (!s_netif_ap) return ESP_FAIL;
    }

    // Set AP IP to 192.168.4.1
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = esp_ip4addr_aton(AP_IP_ADDR);
    ip.gw.addr = esp_ip4addr_aton(AP_GW_ADDR);
    ip.netmask.addr = esp_ip4addr_aton(AP_NETMASK);

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_netif_ap));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif_ap, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_netif_ap));

    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi_init_once failed");

    // Generate SSID like "Leveler-1A2B"
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[33];
    snprintf(ap_ssid, sizeof(ap_ssid), "Leveler-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_cfg = {0};
    safe_strcpy((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), ap_ssid);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.max_connection = AP_MAX_CONN;

    // WPA2 makes iPhone happier vs OPEN in some environments.
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    safe_strcpy((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), AP_PASSWORD);

    // IMPORTANT: APSTA so scan works while AP is up
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set mode apsta failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set ap cfg failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "AP started: SSID=%s  PASS=%s  (browse http://%s/)",
             ap_ssid, AP_PASSWORD, AP_IP_ADDR);

    // Start HTTP server immediately in AP mode
    ensure_http_server_started();
    start_captive_portal();

    return ESP_OK;
}

/* =========================================================
 * Public API
 * ========================================================= */
// Initialize Wi-Fi, HTTP server, and load persisted settings.
esp_err_t wifi_mgr_init(void)
{
    // NVS init (required for Wi-Fi + saved creds)
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (nvs_err != ESP_OK) {
        ESP_ERROR_CHECK(nvs_err);
    }

    // These may already be initialized elsewhere; treat invalid-state as OK.
    esp_err_t e;

    e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(e, TAG, "esp_netif_init failed");
    }

    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(e, TAG, "event loop create failed");
    }

    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        if (!s_wifi_events) return ESP_ERR_NO_MEM;
    }

    // reset per-boot server guard
    s_http_started = false;
    s_forced_ap = false;
    s_sta_failures = 0;
    s_sta_first_fail_us = 0;

    mqtt_defaults();
    s_have_mqtt = nvs_load_mqtt(s_mqtt_uri, sizeof(s_mqtt_uri),
                                s_mqtt_user, sizeof(s_mqtt_user),
                                s_mqtt_pass, sizeof(s_mqtt_pass),
                                s_mqtt_topic, sizeof(s_mqtt_topic),
                                s_mqtt_disc, sizeof(s_mqtt_disc),
                                &s_mqtt_enable);

    config_defaults();
    (void)nvs_load_config(s_wheelbase_in, sizeof(s_wheelbase_in),
                          s_trackwidth_in, sizeof(s_trackwidth_in));
    (void)nvs_load_screen_timeout(&s_screen_timeout_s);
    s_wheelbase_val = parse_or_default(s_wheelbase_in, 133.0f);
    s_trackwidth_val = parse_or_default(s_trackwidth_in, 65.2f);
    mqtt_mgr_set_vehicle_config(s_wheelbase_val, s_trackwidth_val);
    lvgl_port_set_screen_timeout_ms(s_screen_timeout_s * 1000U);

    // Load creds
    memset(s_ssid, 0, sizeof(s_ssid));
    memset(s_pass, 0, sizeof(s_pass));
    s_have_creds = nvs_load_wifi(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass));
    ESP_LOGI(TAG, "Have saved creds? %s", s_have_creds ? "YES" : "NO");

    if (s_have_creds) {
        return start_sta(s_ssid, s_pass);
    } else {
        return start_ap();
    }
}

bool wifi_mgr_is_connected(void)
{
    if (!s_wifi_events) return false;
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

// Accessors used by IMU task for inches conversion.
float wifi_mgr_get_wheelbase_in(void)
{
    return s_wheelbase_val;
}

float wifi_mgr_get_trackwidth_in(void)
{
    return s_trackwidth_val;
}

// Update latest filtered angles for status endpoints.
void wifi_mgr_update_angles(float roll_deg, float pitch_deg)
{
    portENTER_CRITICAL(&s_angle_mux);
    s_last_roll_deg = roll_deg;
    s_last_pitch_deg = pitch_deg;
    portEXIT_CRITICAL(&s_angle_mux);
}

// Update latest accel values for diagnostics.
void wifi_mgr_update_accel(float ax, float ay, float az)
{
    portENTER_CRITICAL(&s_angle_mux);
    s_last_ax = ax;
    s_last_ay = ay;
    s_last_az = az;
    portEXIT_CRITICAL(&s_angle_mux);
}
