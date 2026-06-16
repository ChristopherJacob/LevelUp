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
#include "esp_random.h"
#include "audio_mgr.h"
#include "ui.h"
#include "leveling.h"

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
#define NVS_KEY_LVL_ORIENT  "lvl_orient"
#define NVS_KEY_LVL_MODE    "lvl_mode"
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
static char s_csrf_token[17] = {0};  // 16 hex chars, generated at server start
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

#define UI_VERSION "2026-04-17-1"

/* ---- In-RAM log ring buffer (captured via vprintf hook) ---- */
#define LOG_BUF_SIZE        8192
static char           s_log_buf[LOG_BUF_SIZE];
static char           s_log_stage[256]; // static: NOT stack-allocated in the hook
static uint32_t       s_log_head = 0;
static bool           s_log_full = false;
static portMUX_TYPE   s_log_mux  = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf = NULL;

/* ---------------- vehicle config cache ---------------- */
static char s_wheelbase_in[16] = {0};
static char s_trackwidth_in[16] = {0};
static volatile unsigned char s_lvl_orient = 0; // ORIENT_FRONT_TOP
static volatile unsigned char s_lvl_mode   = 0; // LEVEL_MODE_BLOCKS
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
static leveling_result_t s_guide;

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

// Load leveling orientation/mode from NVS, clamping to valid ranges.
static void nvs_load_leveler(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_LEVELER, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, NVS_KEY_LVL_ORIENT, &v) == ESP_OK) {
        s_lvl_orient = (v > 3) ? 0 : v;   // leveling_front_t is 0..3
    }
    v = 0;
    if (nvs_get_u8(h, NVS_KEY_LVL_MODE, &v) == ESP_OK) {
        s_lvl_mode = (v > 1) ? 0 : v;     // leveling_mode_t is 0..1
    }
    nvs_close(h);
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
        nvs_erase_key(h, NVS_KEY_LVL_ORIENT);
        nvs_erase_key(h, NVS_KEY_LVL_MODE);
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
static esp_err_t http_leveling_mode_post(httpd_req_t *req);
static esp_err_t http_wizard_orient_post(httpd_req_t *req);

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
    send_chunkf(req,
        "<script>var _csrf='%s';"
        "document.addEventListener('DOMContentLoaded',function(){"
        "var t=document.createElement('input');"
        "t.type='hidden';t.name='csrf_token';t.value=_csrf;"
        "document.querySelectorAll('form').forEach(function(f){f.appendChild(t.cloneNode(true));});"
        "});</script>",
        s_csrf_token);
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
        // JSON-escape SSID: handle quotes, backslashes, and control characters.
        char esc[192] = {0};  // worst case: 32 bytes * 6 chars (\uXXXX) + NUL
        const char *in = items[i].ssid;
        char *o = esc;
        size_t left = sizeof(esc) - 1;
        while (*in && left > 0) {
            unsigned char c = (unsigned char)*in;
            if (c == '\"' || c == '\\') {
                if (left < 2) break;
                *o++ = '\\'; *o++ = *in++; left -= 2;
            } else if (c < 0x20) {
                if (left < 6) break;
                snprintf(o, 7, "\\u%04x", c);
                o += 6; left -= 6; in++;
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
/* Validate CSRF token from a URL-encoded POST body. */
static bool csrf_ok(const char *body)
{
    char tok[17] = {0};
    if (!form_get_value(body, "csrf_token", tok, sizeof(tok))) return false;
    return (strcmp(tok, s_csrf_token) == 0);
}

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

    if (!csrf_ok(body)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token invalid");
        return ESP_OK;
    }

    char ssid[33] = {0};
    char pass[65] = {0};

    if (!form_get_value(body, "ssid", ssid, sizeof(ssid))) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_OK;
    }
    (void)form_get_value(body, "pass", pass, sizeof(pass)); // optional for open nets

    free(body);

    if (strcmp(ssid, s_ssid) == 0 && strcmp(pass, s_pass) == 0) {
        ESP_LOGI(TAG, "wifi_save: no change, skipping NVS write and reboot");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "<html><body><h3>No change.</h3><p><a href='/status'>Back to status</a></p></body></html>");
        return ESP_OK;
    }

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

    if (!csrf_ok(body)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token invalid");
        return ESP_OK;
    }

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

    // Skip write if all submitted fields match in-memory state.
    // Password is skipped from comparison when keep_pass is set (blank field = "don't change").
    bool mqtt_changed = (strcmp(uri,   s_mqtt_uri)   != 0) ||
                        (strcmp(user,  s_mqtt_user)  != 0) ||
                        (strcmp(topic, s_mqtt_topic) != 0) ||
                        (strcmp(disc,  s_mqtt_disc)  != 0) ||
                        (enable_flag != s_mqtt_enable)     ||
                        (!keep_pass && pass_ptr && strcmp(pass_ptr, s_mqtt_pass) != 0);

    if (!mqtt_changed) {
        ESP_LOGI(TAG, "mqtt_save: no change, skipping NVS write");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "<html><body><h3>No change.</h3><p><a href='/status'>Back to status</a></p></body></html>");
        return ESP_OK;
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

    if (!csrf_ok(body)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token invalid");
        return ESP_OK;
    }

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

    bool wb_changed = wheelbase_norm[0]  && strcmp(wheelbase_norm,  s_wheelbase_in) != 0;
    bool tw_changed = trackwidth_norm[0] && strcmp(trackwidth_norm, s_trackwidth_in) != 0;

    if (!wb_changed && !tw_changed) {
        ESP_LOGI(TAG, "config_save: no change, skipping NVS write");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "<html><body><h3>No change.</h3><p><a href='/status'>Back to status</a></p></body></html>");
        return ESP_OK;
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

    if (!csrf_ok(body)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token invalid");
        return ESP_OK;
    }

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

    bool net_changed = (strcmp(host, s_hostname) != 0) ||
                       (use_dhcp != s_use_dhcp)         ||
                       (strcmp(ip_norm, s_sta_ip) != 0) ||
                       (strcmp(gw_norm, s_sta_gw) != 0) ||
                       (strcmp(nm_norm, s_sta_nm) != 0);

    if (!net_changed) {
        ESP_LOGI(TAG, "network_save: no change, skipping NVS write and reboot");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "<html><body><h3>No change.</h3><p><a href='/status'>Back to status</a></p></body></html>");
        return ESP_OK;
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

    if (!csrf_ok(body)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token invalid");
        return ESP_OK;
    }

    char timeout_s_str[16] = {0};
    (void)form_get_value(body, "screen_timeout_s", timeout_s_str, sizeof(timeout_s_str));
    free(body);

    uint32_t timeout_s = parse_u32_or_default(timeout_s_str, 60);
    if (timeout_s > 86400U) timeout_s = 86400U;

    if (timeout_s == s_screen_timeout_s) {
        ESP_LOGI(TAG, "display_save: no change, skipping NVS write");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "<html><body><h3>No change.</h3><p><a href='/status'>Back to status</a></p></body></html>");
        return ESP_OK;
    }

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
    leveling_result_t g;
    portENTER_CRITICAL(&s_angle_mux);
    roll = s_last_roll_deg;
    pitch = s_last_pitch_deg;
    ax = s_last_ax;
    ay = s_last_ay;
    az = s_last_az;
    g = s_guide;
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
    send_chunkf(req, "\"screen_on\":%s,", screen_on ? "true" : "false");
    send_chunkf(req, "\"lvl_mode\":\"%s\",", wifi_mgr_get_mode() == 1 ? "ramps" : "blocks");
    send_chunkf(req, "\"guidance_available\":%s,", g.guidance_available ? "true" : "false");
    send_chunkf(req, "\"is_level\":%s,", g.is_level ? "true" : "false");
    send_chunkf(req, "\"lift_fl\":%.1f,\"lift_fr\":%.1f,", g.corner_lift_in[0], g.corner_lift_in[1]);
    send_chunkf(req, "\"lift_rl\":%.1f,\"lift_rr\":%.1f,", g.corner_lift_in[2], g.corner_lift_in[3]);
    send_chunkf(req, "\"worst_corner\":%d,", (int)g.worst_corner);
    send_chunkf(req, "\"ramp_axis_is_roll\":%s,", g.ramp_axis_is_roll ? "true" : "false");
    send_chunkf(req, "\"ramp_lift_left\":%s,", g.ramp_lift_left ? "true" : "false");
    send_chunkf(req, "\"ramp_lift_front\":%s,", g.ramp_lift_front ? "true" : "false");
    send_chunkf(req, "\"ramp_remaining_in\":%.1f", g.ramp_remaining_in);
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
    // CSRF: embed token as JS var; auto-inject hidden field into every form on load.
    send_chunkf(req,
        "<script>var _csrf='%s';"
        "document.addEventListener('DOMContentLoaded',function(){"
        "var t=document.createElement('input');"
        "t.type='hidden';t.name='csrf_token';t.value=_csrf;"
        "document.querySelectorAll('form').forEach(function(f){f.appendChild(t.cloneNode(true));});"
        "});</script>",
        s_csrf_token);
    send_chunk(req,
        "<title>Level Up Dashboard</title>"
        "<style>"
        ":root{--bg:#f5f6fa;--surface:#fff;--border:#e0e1e7;--text:#1a1b1e;"
        "--muted:#6b7280;--accent:#2563eb;--btn:#1d1d1f;--btn-text:#fff;"
        "--shadow:0 1px 4px rgba(0,0,0,.08);"
        "--ok:#16a34a;--ok-bg:#dcfce7;--err:#dc2626;--err-bg:#fee2e2;"
        "--warn:#d97706;--warn-bg:#fef3c7;--neu:#6b7280;--neu-bg:#f3f4f6}"
        "[data-theme=dark]{--bg:#111827;--surface:#1f2937;--border:#374151;--text:#f9fafb;"
        "--muted:#9ca3af;--accent:#60a5fa;--btn:#f3f4f6;--btn-text:#111827;"
        "--shadow:0 1px 4px rgba(0,0,0,.4);"
        "--ok:#4ade80;--ok-bg:#052e16;--err:#f87171;--err-bg:#450a0a;"
        "--warn:#fbbf24;--warn-bg:#451a03;--neu:#6b7280;--neu-bg:#374151}"
        "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:24px;"
        "background:var(--bg);color:var(--text);transition:background .2s,color .2s}"
        ".wrap{max-width:1100px;margin:0 auto}"
        ".title{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px}"
        ".title h2{margin:0;font-size:22px}"
        ".title-right{display:flex;align-items:center;gap:12px}"
        ".grid{display:grid;grid-template-columns:1.2fr 1fr;gap:16px}"
        ".card{background:var(--surface);padding:18px;border:1px solid var(--border);"
        "border-radius:18px;box-shadow:var(--shadow)}"
        "@media(max-width:900px){.grid{grid-template-columns:1fr}}"
        "table{width:100%;border-collapse:collapse;margin-top:10px}"
        "td{padding:9px 8px;border-bottom:1px solid var(--border);vertical-align:middle}"
        "td:first-child{color:var(--muted);font-size:13px;width:44%}"
        "label{display:block;margin:10px 0 6px;font-weight:600}"
        "input[type=text],input[type=password],input:not([type]){"
        "width:100%;font-size:14px;padding:10px;"
        "border:1px solid var(--border);border-radius:10px;"
        "background:var(--surface);color:var(--text);box-sizing:border-box}"
        "input[type=checkbox]{width:auto}"
        ".row{display:flex;gap:10px;flex-wrap:wrap}"
        ".row>div{flex:1;min-width:220px}"
        ".pw-wrap{display:flex;gap:8px;align-items:center}"
        ".pw-wrap input{flex:1}"
        ".pw-btn{width:44px;height:40px;display:inline-flex;align-items:center;justify-content:center;"
        "border:1px solid var(--border);border-radius:10px;background:var(--surface);"
        "color:var(--text);cursor:pointer}"
        ".pw-btn svg{width:20px;height:20px;fill:var(--text)}"
        ".pw-btn.is-on{background:var(--btn)}"
        ".pw-btn.is-on svg{fill:var(--btn-text)}"
        ".muted{color:var(--muted);font-size:13px}"
        "button:not(#themeBtn):not(.pw-btn):not(.btn-sm){margin-top:14px;width:100%;padding:12px;"
        "font-size:15px;font-weight:600;border:0;border-radius:12px;"
        "background:var(--btn);color:var(--btn-text);cursor:pointer}"
        ".btn-sm{padding:4px 12px;font-size:13px;border:1px solid var(--border);"
        "border-radius:8px;background:var(--surface);color:var(--text);cursor:pointer}"
        ".danger{background:#dc2626!important;color:#fff!important}"
        "a{color:var(--accent);text-decoration:none}"
        "details{border:1px solid var(--border);border-radius:12px;padding:8px 12px;"
        "margin-top:10px;background:var(--surface)}"
        "summary{cursor:pointer;font-weight:700;list-style:none;padding:4px 0}"
        "summary::-webkit-details-marker{display:none}"
        "details>summary::after{content:'+';float:right;color:var(--muted)}"
        "details[open]>summary::after{content:'-'}"
        ".badge{display:inline-block;font-size:11px;font-weight:700;padding:2px 8px;"
        "border-radius:99px;letter-spacing:.02em}"
        ".ok{color:var(--ok);background:var(--ok-bg)}"
        ".err{color:var(--err);background:var(--err-bg)}"
        ".warn{color:var(--warn);background:var(--warn-bg)}"
        ".neu{color:var(--neu);background:var(--neu-bg)}"
        ".tip{position:relative;cursor:help;border-bottom:1px dotted var(--muted)}"
        ".tip::after{content:attr(data-tip);position:absolute;left:0;bottom:calc(100% + 6px);"
        "background:#1a1b1e;color:#f9fafb;font-size:12px;font-weight:400;"
        "padding:6px 10px;border-radius:8px;white-space:normal;max-width:220px;"
        "width:max-content;box-shadow:0 2px 8px rgba(0,0,0,.3);"
        "z-index:10;opacity:0;pointer-events:none;transition:opacity .15s}"
        ".tip:hover::after{opacity:1}"
        "#themeBtn{display:inline-flex;align-items:center;justify-content:center;"
        "width:34px;height:34px;border-radius:50%;border:1px solid var(--border);"
        "background:var(--surface);cursor:pointer;padding:0;font-size:17px;line-height:1}"
        "#otaBar{background:var(--border)!important}"
        "#otaFill{background:var(--accent)!important}"
        "</style>"
        "<script>(function(){"
        "var t=localStorage.getItem('theme');"
        "if(t==='dark'||(t==null&&window.matchMedia&&"
        "window.matchMedia('(prefers-color-scheme:dark)').matches))"
        "{document.documentElement.setAttribute('data-theme','dark');}"
        "})();</script>"
        "</head>"
        "<body><div class='wrap'>"
        "<div class='title'>"
        "<h2>Leveler Dashboard</h2>"
        "<div class='title-right'>"
        "<a href='/wizard' style='font-size:13px;padding:6px 12px;border:1px solid var(--border);"
        "border-radius:8px;text-decoration:none;color:var(--text);background:var(--surface)'>"
        "&#x2699;&#xfe0f; Setup Wizard</a>"
        "<span class='muted'>v" UI_VERSION "</span>"
        "<button id='themeBtn' type='button' aria-label='Toggle dark mode' onclick='toggleTheme()'>"
        "<span id='themeIco'></span>"
        "</button>"
        "</div></div>"
    );

    // OTA rollback banner — shown when the previously flashed firmware failed to boot.
    const esp_partition_t *invalid_part = esp_ota_get_last_invalid_partition();
    if (invalid_part) {
        send_chunkf(req,
            "<div style='background:#c0510a;color:#fff;padding:10px 14px;border-radius:8px;"
            "margin-bottom:12px;font-size:.95em'>"
            "&#9888; <strong>OTA rollback:</strong> The firmware flashed to &lsquo;%s&rsquo; "
            "did not boot cleanly and was automatically reverted. "
            "Please flash a corrected build before trying again."
            "</div>",
            invalid_part->label);
    }


    send_chunk(req,
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

    bool connected = wifi_mgr_is_connected();
    send_chunk(req, "<table>");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='AP = setup access point active; STA = joined to your Wi-Fi network'>Mode</span></td>"
        "<td><span class='badge neu'>%s</span></td></tr>",
        s_running_ap ? "AP" : "STA");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Wi-Fi network name saved in device settings'>SSID</span></td>"
        "<td>%s</td></tr>",
        s_have_creds ? s_ssid : "(none)");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Whether device is currently joined to the configured Wi-Fi network'>Connected</span></td>"
        "<td><span class='badge %s'>%s</span></td></tr>",
        connected ? "ok" : "err", connected ? "YES" : "NO");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Device IP address on the local network'>IP</span></td>"
        "<td>%s</td></tr>", ipbuf[0] ? ipbuf : "(none)");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Router IP address (network gateway)'>Gateway</span></td>"
        "<td>%s</td></tr>", gwbuf[0] ? gwbuf : "(none)");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Subnet mask defining the local network range'>Netmask</span></td>"
        "<td>%s</td></tr>", nmbuf[0] ? nmbuf : "(none)");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Wi-Fi signal strength in dBm — e.g. -40 excellent, -70 fair, -85 poor'>RSSI</span></td>"
        "<td>%d dBm</td></tr>", rssi);
    float roll_in = tanf(roll * DEG2RAD) * s_trackwidth_val;
    float pitch_in = tanf(pitch * DEG2RAD) * s_wheelbase_val;
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Left/right tilt in degrees — positive = right side high'>Roll</span></td>"
        "<td>%.3f&deg;</td></tr>", roll);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Front/rear tilt in degrees — positive = nose high'>Pitch</span></td>"
        "<td>%.3f&deg;</td></tr>", pitch);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Lateral level offset at the axle based on trackwidth and roll angle'>Roll (in)</span></td>"
        "<td>%.1f in</td></tr>", roll_in);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Fore/aft level offset based on wheelbase and pitch angle'>Pitch (in)</span></td>"
        "<td>%.1f in</td></tr>", pitch_in);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Raw accelerometer along the X axis in g-force'>Accel X</span></td>"
        "<td>%.4f g</td></tr>", ax);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Raw accelerometer along the Y axis in g-force'>Accel Y</span></td>"
        "<td>%.4f g</td></tr>", ay);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Raw accelerometer along the Z axis — 1.0 g when flat'>Accel Z</span></td>"
        "<td>%.4f g</td></tr>", az);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Whether custom MQTT settings are saved or factory defaults are used'>MQTT config</span></td>"
        "<td>%s</td></tr>", s_have_mqtt ? "Saved" : "Defaults");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Whether MQTT sensor publishing is currently active'>MQTT enabled</span></td>"
        "<td><span class='badge %s'>%s</span></td></tr>",
        s_mqtt_enable ? "ok" : "neu", s_mqtt_enable ? "YES" : "NO");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='mDNS name — device is accessible as hostname.local on your LAN'>Hostname</span></td>"
        "<td>%s</td></tr>", s_hostname);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='DHCP = address assigned by router; Static = fixed address set manually'>IP mode</span></td>"
        "<td><span class='badge neu'>%s</span></td></tr>",
        s_use_dhcp ? "DHCP" : "Static");
    if (!s_use_dhcp) {
        send_chunkf(req,
            "<tr><td><span class='tip' data-tip='Manually configured static IP address'>Static IP</span></td>"
            "<td>%s</td></tr>", s_sta_ip);
        send_chunkf(req,
            "<tr><td><span class='tip' data-tip='Manually configured gateway (router) IP'>Static GW</span></td>"
            "<td>%s</td></tr>", s_sta_gw);
        send_chunkf(req,
            "<tr><td><span class='tip' data-tip='Manually configured subnet mask'>Static NM</span></td>"
            "<td>%s</td></tr>", s_sta_nm);
    }
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Seconds of inactivity before display blanks — 0 disables auto-off'>Screen timeout</span></td>"
        "<td>%u s</td></tr>", (unsigned)s_screen_timeout_s);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Front-to-rear axle distance — used to convert pitch angle to level offset in inches'>Wheelbase</span></td>"
        "<td>%s</td></tr>",
        s_wheelbase_in[0] ? s_wheelbase_in : "133 (default)");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Left-to-right wheel distance — used to convert roll angle to level offset in inches'>Track width</span></td>"
        "<td>%s</td></tr>",
        s_trackwidth_in[0] ? s_trackwidth_in : "65.2 (default)");
    send_chunk(req,
        "<tr><td colspan='2' style='font-weight:700;font-size:11px;letter-spacing:.06em;"
        "text-transform:uppercase;color:var(--muted);padding-top:16px;border-bottom:none'>"
        "Diagnostics</td></tr>");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Time elapsed since the device last restarted'>Uptime</span></td>"
        "<td>%u s</td></tr>", (unsigned)uptime_s);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Available heap RAM remaining on the device'>Free heap</span></td>"
        "<td>%u B</td></tr>", (unsigned)free_heap);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='IMU health — OK = fresh angle data; STALE/ERR = no recent reading'>IMU status</span></td>"
        "<td><span class='badge %s'>%s</span></td></tr>",
        imu_ok ? "ok" : "err", imu_ok ? "OK" : "STALE/ERR");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Milliseconds since the last valid IMU reading was received'>IMU age</span></td>"
        "<td>%u ms</td></tr>", (unsigned)imu_age_ms);
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Whether the IMU detects no motion — affects audio beep cadence'>IMU stationary</span></td>"
        "<td><span class='badge %s'>%s</span></td></tr>",
        imu_stationary ? "ok" : "warn", imu_stationary ? "YES" : "NO");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Whether the device is currently connected to the MQTT broker'>MQTT connected</span></td>"
        "<td><span class='badge %s'>%s</span></td></tr>",
        mqtt_connected ? "ok" : (s_mqtt_enable ? "err" : "neu"),
        mqtt_connected ? "YES" : "NO");
    send_chunkf(req,
        "<tr><td><span class='tip' data-tip='Whether the LVGL display panel is active or blanked'>Screen</span></td>"
        "<td><span class='badge %s'>%s</span></td></tr>",
        screen_on ? "ok" : "neu", screen_on ? "ON" : "OFF");
    send_chunk(req, "</table></details></div>");

    /* Leveling guidance card */
    send_chunk(req,
        "<div class='card' id='guideCard' style='margin:12px 0;padding:14px;"
        "border:1px solid var(--border);border-radius:12px'>"
        "<div style='display:flex;justify-content:space-between;align-items:center'>"
        "<b>Leveling guidance</b>"
        "<span id='gMode' style='font-size:12px;color:var(--muted)'></span></div>"
        "<table style='width:100%;margin-top:8px;border-collapse:collapse;text-align:center'>"
        "<tr><td colspan=2 style='font-size:10px;color:var(--muted)'>FRONT</td></tr>"
        "<tr><td id='gFL'>--</td><td id='gFR'>--</td></tr>"
        "<tr><td id='gRL'>--</td><td id='gRR'>--</td></tr>"
        "<tr><td colspan=2 style='font-size:10px;color:var(--muted)'>REAR</td></tr></table>"
        "<div id='gStatus' style='margin-top:8px;font-weight:600'></div>"
        "<button id='gToggle' style='margin-top:8px' onclick='toggleMode()'>Switch mode</button>"
        "</div>"
    );
    /* Guidance JS — wrapped in DOMContentLoaded since guideCard is in a later chunk */
    send_chunk(req,
        "<script>"
        "function fmt(v){return (v<0.05?'0':v.toFixed(1)+'\"');}"
        "function paint(d){"
        "var r=d.lvl_mode==='ramps';"
        "document.getElementById('gMode').textContent=d.lvl_mode;"
        "if(!d.guidance_available){document.getElementById('gStatus').textContent="
        "'Set vehicle dimensions';return;}"
        "document.getElementById('gFL').textContent=r?'':fmt(d.lift_fl);"
        "document.getElementById('gFR').textContent=r?'':fmt(d.lift_fr);"
        "document.getElementById('gRL').textContent=r?'':fmt(d.lift_rl);"
        "document.getElementById('gRR').textContent=r?'':fmt(d.lift_rr);"
        "var s;if(d.is_level){s='Level \\u2713';}"
        "else if(r){var dir=d.ramp_axis_is_roll?(d.ramp_lift_left?'LEFT':'RIGHT')"
        ":(d.ramp_lift_front?'FRONT':'REAR');"
        "s='Drive '+dir+' wheels up '+d.ramp_remaining_in.toFixed(1)+'\"';}"
        "else{var cn=['FRONT-LEFT','FRONT-RIGHT','REAR-LEFT','REAR-RIGHT'];"
        "s='Raise '+cn[d.worst_corner]+' first';}"
        "document.getElementById('gStatus').textContent=s;}"
        "window.__paintGuide=paint;"
        "function toggleMode(){"
        "fetch('/status.json').then(function(r){return r.json();}).then(function(d){"
        "var nm=d.lvl_mode==='ramps'?'blocks':'ramps';"
        "return fetch('/leveling_mode',{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'csrf_token='+encodeURIComponent(_csrf)+'&mode='+nm});})"
        ".then(function(){if(window._pollGuide)window._pollGuide();})"
        "['catch'](function(){});}"
        "</script>"
    );

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
        "xhr.setRequestHeader('X-CSRF-Token',_csrf);"
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
        "function toggleTheme(){"
        "var isDark=document.documentElement.getAttribute('data-theme')==='dark';"
        "document.documentElement.setAttribute('data-theme',isDark?'light':'dark');"
        "localStorage.setItem('theme',isDark?'light':'dark');"
        "document.getElementById('themeIco').textContent=isDark?'\\u263D':'\\u2600';"
        "}"
        "(function(){"
        "var isDark=document.documentElement.getAttribute('data-theme')==='dark';"
        "document.getElementById('themeIco').textContent=isDark?'\\u2600':'\\u263D';"
        "})();"
        "var _logTimer=null;"
        "function fetchLog(){"
        "fetch('/logs').then(function(r){return r.text();}).then(function(t){"
        "var pre=document.getElementById('logPre');"
        "if(!pre)return;"
        "var atBottom=(pre.scrollHeight-pre.scrollTop)<=pre.clientHeight+40;"
        "pre.textContent=t;"
        "if(atBottom)pre.scrollTop=pre.scrollHeight;"
        "}).catch(function(){});}"
        "function clearLog(){"
        "var pre=document.getElementById('logPre');"
        "if(pre)pre.textContent='';}"
        /* logLive lives in a later HTML chunk — defer lookup until DOM is ready */
        "document.addEventListener('DOMContentLoaded',function(){"
        "var ll=document.getElementById('logLive');"
        "if(!ll)return;"
        "function _logToggle(){"
        "if(ll.checked){fetchLog();_logTimer=setInterval(fetchLog,2000);}"
        "else{clearInterval(_logTimer);_logTimer=null;}}"
        "ll.addEventListener('change',_logToggle);"
        "_logToggle();});"
        "window._pollGuide=function(){"
        "fetch('/status.json').then(function(r){return r.json();})"
        ".then(function(d){if(window.__paintGuide)window.__paintGuide(d);})"
        "['catch'](function(){});};"
        "document.addEventListener('DOMContentLoaded',function(){"
        "window._pollGuide();setInterval(window._pollGuide,2000);});"
        "</script>"
        "</details></div></div>"
    );
    // Log panel — full-width row below the two-column grid.
    send_chunk(req,
        "<div class='card' style='margin-top:16px'>"
        "<details><summary>Device Logs</summary>"
        "<div style='display:flex;gap:10px;align-items:center;margin:10px 0 8px'>"
        "<label style='margin:0;font-weight:normal;font-size:14px'>"
        "<input type='checkbox' id='logLive' checked> Live"
        "</label>"
        "<button class='btn-sm' type='button' onclick='clearLog()'>Clear display</button>"
        "</div>"
        "<pre id='logPre'"
        " style='height:300px;overflow-y:auto;font-size:11.5px;line-height:1.5;"
        "background:var(--bg);border:1px solid var(--border);border-radius:8px;"
        "padding:10px;white-space:pre-wrap;word-break:break-all;margin:0;color:var(--text)'>"
        "</pre>"
        "</details></div>"
        "</div></body></html>"
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
    // Read body to extract and validate CSRF token.
    int total = req->content_len;
    if (total > 0 && total <= 256) {
        char *body = calloc(1, (size_t)total + 1);
        if (body) {
            int received = 0;
            while (received < total) {
                int r = httpd_req_recv(req, body + received, total - received);
                if (r <= 0) { free(body); goto csrf_fail; }
                received += r;
            }
            body[received] = 0;
            bool ok = csrf_ok(body);
            free(body);
            if (!ok) goto csrf_fail;
        }
    } else if (total != 0) {
        goto csrf_fail;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><h3>Resetting...</h3><p>Rebooting...</p></body></html>");
    nvs_factory_reset();

    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;

csrf_fail:
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token invalid");
    return ESP_OK;
}

/* =========================================================
 * HTTP: /ota (POST) — stream firmware binary directly into OTA partition
 * ========================================================= */
#define OTA_BUF_SIZE 4096

static esp_err_t http_ota_post(httpd_req_t *req)
{
    // Validate CSRF token from X-CSRF-Token header (body is raw binary, not form data).
    char csrf_hdr[17] = {0};
    httpd_req_get_hdr_value_str(req, "X-CSRF-Token", csrf_hdr, sizeof(csrf_hdr));
    if (strcmp(csrf_hdr, s_csrf_token) != 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token invalid");
        return ESP_FAIL;
    }

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
 * Log ring buffer — vprintf hook + HTTP endpoint
 * ========================================================= */
// Custom vprintf: tee into ring buffer then forward to original UART output.
// IMPORTANT: uses s_log_stage (BSS) instead of stack-allocated buffers to
// avoid blowing the stack on tasks with small stacks (e.g. app_main, audio).
static int log_vprintf(const char *fmt, va_list args)
{
    // Forward to UART first using a va_copy so args stays at its initial position.
    int ret = 0;
    if (s_orig_vprintf) {
        va_list tmp;
        va_copy(tmp, args);
        ret = s_orig_vprintf(fmt, tmp);
        va_end(tmp);
    }

    // Format into static staging buffer, then strip ANSI and write to ring buffer.
    // portMUX serializes access to both s_log_stage and the ring buffer.
    portENTER_CRITICAL(&s_log_mux);
    int n = vsnprintf(s_log_stage, sizeof(s_log_stage), fmt, args);
    if (n > 0) {
        if (n >= (int)sizeof(s_log_stage)) n = (int)sizeof(s_log_stage) - 1;
        bool esc = false;
        for (int i = 0; i < n; i++) {
            char c = s_log_stage[i];
            if (esc) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) esc = false;
            } else if (c == '\033' && i + 1 < n && s_log_stage[i + 1] == '[') {
                esc = true;
                i++; // skip '['
            } else {
                s_log_buf[s_log_head] = c;
                s_log_head = (s_log_head + 1) % LOG_BUF_SIZE;
                if (s_log_head == 0) s_log_full = true;
            }
        }
    }
    portEXIT_CRITICAL(&s_log_mux);

    return ret > 0 ? ret : (n > 0 ? n : 0);
}

// HTTP GET /logs — returns ring buffer contents as plain text.
static esp_err_t http_logs_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char *tmp = malloc(LOG_BUF_SIZE);
    if (!tmp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_log_mux);
    uint32_t head = s_log_head;
    bool full = s_log_full;
    size_t total;
    if (!full) {
        total = head;
        memcpy(tmp, s_log_buf, total);
    } else {
        size_t tail_len = LOG_BUF_SIZE - head;
        memcpy(tmp, s_log_buf + head, tail_len);
        memcpy(tmp + tail_len, s_log_buf, head);
        total = LOG_BUF_SIZE;
    }
    portEXIT_CRITICAL(&s_log_mux);

    httpd_resp_send_chunk(req, tmp, (ssize_t)total);
    httpd_resp_send_chunk(req, NULL, 0);
    free(tmp);
    return ESP_OK;
}

/* =========================================================
 * Calibration Wizard: embedded vehicle image (base64 JPEG, 240 px wide)
 * ========================================================= */
static const char s_vehicle_b64[] =
    "/9j/4AAQSkZJRgABAQAASABIAAD/4QBMRXhpZgAATU0AKgAAAAgAAYdpAAQAAAABAAAAGgAAAAAA"
    "A6ABAAMAAAABAAEAAKACAAQAAAABAAAA8KADAAQAAAABAAABaAAAAAD/wAARCAFoAPADASIAAhEB"
    "AxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9"
    "AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6"
    "Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ip"
    "qrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEB"
    "AQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJB"
    "UQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RV"
    "VldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6"
    "wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9sAQwADAwMDAwMFAwMFBwUFBQcJ"
    "BwcHBwkMCQkJCQkMDgwMDAwMDA4ODg4ODg4OERERERERFBQUFBQWFhYWFhYWFhYW/9sAQwEDBAQG"
    "BQYKBQUKFxANEBcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcXFxcX"
    "FxcX/90ABAAP/9oADAMBAAIRAxEAPwD9UqKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigA"
    "ooooAKKKKACiiigAooooAKKKKACiiigAooooA//Q/VKiiigAooooAKKKKACiiigAqOWeGHb5zqm9"
    "gi7iBuY9AM9SfSvkj48ftS2Pwz1KTwf4Sto9T12JV+0SSljbWjSAFEZU+eWZgQRGpGARlgSAfhL4"
    "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"
    "3rGb8FzQBzoQUuyt0+H9dHP2Gf8A74NB0DXAP+PCf/vg0DPLPGPw78F/EC2htfF+mx36WzFoizMj"
    "oWxna6MrAHAyM4OBTvB3w88G+ALWaz8IadHYR3Lh5SpZ2cqMDczszEDJwM4GT616d/wj2uHrYz/9"
    "8Gl/4R7XMf8AHjP/AN8GgDEVKdsGK2x4f1z/AJ8Z/wDvg0v/AAj+uf8APjP/AN8GgDECVMiAGtlP"
    "DuvMwC2E3PqMfzrp7bwRqhH76OFf96Rv/ZaAOEVAa63w1o0+o3sblT5MTBnY9OOcD1Jr0Ox8KaPb"
    "RL51ujy4+Yksy59gxro4444kEcShFHQKMAUCH0UUUAFFFFABRRRQAUUUUAf/0v1SooooAKKKKACi"
    "iigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigD/9P9UqKK"
    "KACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAoooo"
    "A//U/VKiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigA"
    "ooooAKKKKAP/1f1SooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACi"
    "iigAooooAKKKKACiiigD/9k=";
/* =========================================================
 * HTTP: /leveling_mode (POST) — switch blocks/ramps mode
 * ========================================================= */
// POST /leveling_mode  (form: csrf_token + mode=blocks|ramps)
static esp_err_t http_leveling_mode_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 256) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad len"); return ESP_OK; }
    char *body = (char *)calloc(1, (size_t)total + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem"); return ESP_OK; }
    int rcvd = 0;
    while (rcvd < total) {
        int r = httpd_req_recv(req, body + rcvd, total - rcvd);
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); return ESP_OK; }
        rcvd += r;
    }
    body[rcvd] = '\0';
    if (!csrf_ok(body)) { free(body); httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF"); return ESP_OK; }
    char mode[12] = {0};
    (void)form_get_value(body, "mode", mode, sizeof(mode));
    free(body);
    wifi_mgr_set_mode(strcmp(mode, "ramps") == 0 ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* =========================================================
 * HTTP: /wizard (GET) — multi-step calibration wizard
 * ========================================================= */
static esp_err_t http_wizard_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    int  vol   = audio_mgr_get_volume();
    bool muted = audio_mgr_is_muted();

    /* head */
    send_chunk(req,
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' type='image/png' href='/favicon.png'>"
        "<title>Setup Wizard \xe2\x80\x94 Leveler</title>"
    );
    /* early theme to prevent flash */
    send_chunk(req,
        "<script>(function(){"
        "var t=localStorage.getItem('theme');"
        "if(t==='dark'||(t==null&&window.matchMedia&&"
        "window.matchMedia('(prefers-color-scheme:dark)').matches))"
        "{document.documentElement.setAttribute('data-theme','dark');}"
        "})();</script>"
    );
    send_chunk(req,
        "<style>"
        ":root{--bg:#f5f6fa;--surface:#fff;--border:#e0e1e7;--text:#1a1b1e;"
        "--muted:#6b7280;--accent:#2563eb;--btn:#1d1d1f;--btn-text:#fff;"
        "--ok:#16a34a;--ok-bg:#dcfce7;--err:#dc2626;--err-bg:#fee2e2;"
        "--warn:#d97706;--warn-bg:#fef3c7}"
        "[data-theme=dark]{--bg:#111827;--surface:#1f2937;--border:#374151;"
        "--text:#f9fafb;--muted:#9ca3af;--accent:#60a5fa;--btn:#f3f4f6;"
        "--btn-text:#111827;--ok:#4ade80;--ok-bg:#052e16;"
        "--err:#f87171;--err-bg:#450a0a;--warn:#fbbf24;--warn-bg:#451a03}"
        "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:0;"
        "background:var(--bg);color:var(--text);min-height:100vh;"
        "display:flex;align-items:center;justify-content:center}"
        ".wizard{max-width:460px;width:100%;margin:24px;background:var(--surface);"
        "border:1px solid var(--border);border-radius:20px;overflow:hidden;"
        "box-shadow:0 4px 24px rgba(0,0,0,.12)}"
        ".wiz-head{padding:20px 24px 14px;border-bottom:1px solid var(--border)}"
        ".wiz-head h2{margin:0 0 4px;font-size:18px;font-weight:700}"
        ".wiz-sub{font-size:13px;color:var(--muted);margin:0}"
        ".dots{display:flex;gap:6px;margin-top:12px}"
        ".dot{flex:1;height:4px;border-radius:99px;background:var(--border);transition:background .3s}"
        ".dot.done{background:var(--ok)}"
        ".dot.active{background:var(--accent)}"
        ".wiz-body{padding:22px 24px}"
        ".step{display:none}"
        ".step.active{display:block}"
        ".desc{color:var(--muted);font-size:14px;margin:0 0 18px;line-height:1.5}"
        ".vehicle-wrap{position:relative;margin:0 auto 18px;width:180px}"
        ".vehicle-wrap img{width:100%;border-radius:12px;display:block;"
        "box-shadow:0 2px 8px rgba(0,0,0,.15)}"
        ".ax{position:absolute;font-size:10px;font-weight:800;letter-spacing:.06em;"
        "background:var(--surface);color:var(--text);padding:2px 5px;border-radius:4px;"
        "border:1px solid var(--border)}"
        ".ax.top{top:-12px;left:50%;transform:translateX(-50%)}"
        ".ax.bot{bottom:-12px;left:50%;transform:translateX(-50%)}"
        ".ax.lft{left:-26px;top:50%;transform:translateY(-50%)}"
        ".ax.rgt{right:-26px;top:50%;transform:translateY(-50%)}"
        ".angle-row{display:flex;gap:10px;margin-bottom:18px}"
        ".abox{flex:1;padding:12px 10px;border:1px solid var(--border);"
        "border-radius:12px;text-align:center;transition:border-color .3s,background .3s}"
        ".abox .albl{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}"
        ".abox .aval{font-size:26px;font-weight:700;margin:4px 0 0;"
        "font-variant-numeric:tabular-nums;letter-spacing:-.02em}"
        ".abox.ok{border-color:var(--ok);background:var(--ok-bg)}"
        ".abox.warn{border-color:var(--warn);background:var(--warn-bg)}"
        ".chk-row{display:flex;align-items:center;gap:10px;margin-bottom:14px;"
        "font-size:15px;cursor:pointer;user-select:none}"
        ".chk-row input{width:18px;height:18px;cursor:pointer}"
        ".srow{margin-bottom:16px}"
        ".srow-lbl{display:flex;justify-content:space-between;font-size:14px;"
        "margin-bottom:8px;font-weight:500}"
        ".srow input[type=range]{width:100%;height:6px;-webkit-appearance:none;"
        "appearance:none;background:var(--border);border-radius:99px;outline:none}"
        ".srow input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;"
        "width:22px;height:22px;border-radius:50%;background:var(--accent);cursor:pointer}"
        ".wbtn{width:100%;padding:13px;font-size:15px;font-weight:600;border:0;"
        "border-radius:12px;background:var(--btn);color:var(--btn-text);"
        "cursor:pointer;margin-top:8px;transition:opacity .15s}"
        ".wbtn:disabled{opacity:.35;cursor:not-allowed}"
        ".wbtn.sec{background:var(--border);color:var(--text)}"
        ".msg{font-size:14px;padding:10px 14px;border-radius:10px;"
        "margin-top:12px;display:none;line-height:1.4}"
        ".msg.ok{color:var(--ok);background:var(--ok-bg);display:block}"
        ".msg.err{color:var(--err);background:var(--err-bg);display:block}"
        ".done-icon{text-align:center;font-size:60px;margin:4px 0 14px}"
        ".done-title{text-align:center;font-size:20px;font-weight:700;margin-bottom:6px}"
        ".done-sub{text-align:center;color:var(--muted);font-size:14px;margin-bottom:22px;line-height:1.5}"
        "a.go-link{display:block;text-align:center;padding:13px;border-radius:12px;"
        "background:var(--btn);color:var(--btn-text);font-size:15px;font-weight:600;"
        "text-decoration:none;margin-top:8px}"
        "a.back-link{display:block;text-align:center;margin-top:12px;"
        "color:var(--accent);text-decoration:none;font-size:13px}"
        "</style></head>"
    );

    /* wizard container + header */
    send_chunk(req,
        "<body><div class='wizard'>"
        "<div class='wiz-head'>"
        "<h2>Leveler Setup Wizard</h2>"
        "<p class='wiz-sub' id='stepSub'>Step 1 of 4: Level Reference</p>"
        "<div class='dots'>"
        "<div class='dot active' id='dot1'></div>"
        "<div class='dot' id='dot2'></div>"
        "<div class='dot' id='dot3'></div>"
        "<div class='dot' id='dot4'></div>"
        "</div></div>"
        "<div class='wiz-body'>"
    );

    /* Step 1: Level Reference */
    send_chunk(req,
        "<div class='step active' id='step1'>"
        "<p class='desc'>Park on the surface you consider &#x2018;level,&#x2019; "
        "then tap <b>Set Zero Reference</b>. "
        "This stores your calibration baseline.</p>"
        "<div class='vehicle-wrap'>"
        "<span class='ax top'>FRONT</span>"
        "<span class='ax bot'>REAR</span>"
        "<span class='ax lft'>L</span>"
        "<span class='ax rgt'>R</span>"
    );
    send_chunk(req, "<img src='data:image/jpeg;base64,");
    httpd_resp_send_chunk(req, s_vehicle_b64, (ssize_t)(sizeof(s_vehicle_b64) - 1));
    send_chunk(req,
        "' alt='Vehicle top-down view'></div>"
        "<div class='angle-row'>"
        "<div class='abox' id='rBox'>"
        "<div class='albl'>Roll &#x2194;</div>"
        "<div class='aval' id='rVal'>&#x2014;</div>"
        "</div>"
        "<div class='abox' id='pBox'>"
        "<div class='albl'>Pitch &#x2195;</div>"
        "<div class='aval' id='pVal'>&#x2014;</div>"
        "</div></div>"
        "<button class='wbtn' onclick='doZero()'>Set Zero Reference</button>"
        "<div class='msg' id='zMsg'></div>"
        "</div>"   /* end step1 */
    );

    /* Step 2o: Orientation */
    send_chunk(req,
        "<div class='step' id='step2o'>"
        "<p class='desc'>Which screen edge points to the <b>front</b> of the van? "
        "This lets the guidance name the correct wheels.</p>"
        "<div style='display:grid;grid-template-columns:1fr 1fr;gap:10px'>"
        "<button class='wbtn sec' onclick='setOrient(0)'>Top</button>"
        "<button class='wbtn sec' onclick='setOrient(1)'>Bottom</button>"
        "<button class='wbtn sec' onclick='setOrient(2)'>Left</button>"
        "<button class='wbtn sec' onclick='setOrient(3)'>Right</button>"
        "</div><div class='msg' id='oMsg'></div></div>"
    );

    /* Step 3 (was 2): Audio — split across plain send_chunk calls to stay under the
       512-byte send_chunkf buffer; only the mute/volume values use chunkf. */
    send_chunk(req,
        "<div class='step' id='step2'>"
        "<p class='desc'>Adjust beeper volume and test it. "
        "The tone speeds up as your vehicle approaches level.</p>"
        "<label class='chk-row'>"
        "<input type='checkbox' id='muteChk'"
    );
    send_chunk(req, muted ? " checked" : "");
    send_chunk(req, "><span>Mute beeper</span></label>"
        "<div class='srow'>"
        "<div class='srow-lbl'><span>Volume</span>"
        "<span id='volPct'>"
    );
    send_chunkf(req, "%d%%", vol);
    send_chunk(req, "</span></div>"
        "<input type='range' id='volSlider' min='0' max='100' value='"
    );
    send_chunkf(req, "%d", vol);
    send_chunk(req,
        "' oninput='document.getElementById(\"volPct\").textContent=this.value+\"%\"'>"
        "</div>"
        "<button class='wbtn sec' onclick='doBeep()'>&#9654; Test Beep</button>"
        "<button class='wbtn' onclick='doSaveAudio()'>Save &amp; Continue &#x2192;</button>"
        "<div class='msg' id='aMsg'></div>"
        "</div>"   /* end step2 */
    );

    /* Step 3: Done */
    send_chunk(req,
        "<div class='step' id='step3'>"
        "<div class='done-icon'>&#x2705;</div>"
        "<div class='done-title'>Setup Complete</div>"
        "<div class='done-sub'>Your Leveler is calibrated and ready to use.<br>"
        "Head to the dashboard to monitor angles.</div>"
        "<a href='/status' class='go-link'>Go to Dashboard &#x2192;</a>"
        "<a href='/' class='back-link'>Back to Settings</a>"
        "</div>"   /* end step3 */
    );

    send_chunk(req, "</div></div>");   /* wiz-body / wizard */

    /* JavaScript — inject CSRF token via send_chunkf (fits in 512 B),
       then send the static function body as plain chunks. */
    send_chunkf(req, "<script>var _csrf='%s';", s_csrf_token);
    send_chunk(req,
        "var pollT=null;"
        "var SUBS=['Step 1 of 4: Level Reference',"
        "'Step 2 of 4: Orientation',"
        "'Step 3 of 4: Audio Settings',"
        "'Step 4 of 4: Setup Complete'];"
        "var STEP_IDS=['step1','step2o','step2','step3'];"
        "function goStep(n){document.querySelectorAll('.step').forEach(function(s){"
        "s.classList.remove('active');});"
        "document.getElementById(STEP_IDS[n-1]).classList.add('active');"
        "[1,2,3,4].forEach(function(i){var d=document.getElementById('dot'+i);"
        "d.className='dot'+(i<n?' done':i===n?' active':'');});"
        "document.getElementById('stepSub').textContent=SUBS[n-1];"
        "if(n===1)startPoll();else stopPoll();}"
    );
    send_chunk(req,
        "function setOrient(v){var m=document.getElementById('oMsg');m.className='msg';"
        "post('/wizard/orient','&orient='+v).then(function(r){return r.json();})"
        ".then(function(d){if(d.ok){m.textContent='\\u2713 Saved';m.className='msg ok';"
        "setTimeout(function(){goStep(3);},700);}"
        "else{m.textContent='\\u2717 '+(d.error||'failed');m.className='msg err';}})"
        "['catch'](function(){m.textContent='\\u2717 Failed';m.className='msg err';});}"
        "function startPoll(){"
        "if(pollT)return;"
        "fetchA();pollT=setInterval(fetchA,600);}"
        "function stopPoll(){clearInterval(pollT);pollT=null;}"
        "function fetchA(){"
        "fetch('/status.json').then(function(r){return r.json();})"
        ".then(function(d){setA('rBox','rVal',d.roll_deg);"
        "setA('pBox','pVal',d.pitch_deg);})['catch'](function(){});}"
        "function setA(bid,vid,v){"
        "var box=document.getElementById(bid);"
        "var val=document.getElementById(vid);"
        "val.textContent=(v>=0?'+':'')+v.toFixed(2)+'\u00b0';"
        "box.className='abox'+(Math.abs(v)<0.5?' ok':Math.abs(v)<2?' warn':'');}"
        "function post(url,extra){"
        "return fetch(url,{method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'csrf_token='+encodeURIComponent(_csrf)+(extra||'')});}"
        "function doZero(){"
        "var m=document.getElementById('zMsg');m.className='msg';"
        "post('/wizard/zero').then(function(r){return r.json();})"
        ".then(function(d){"
        "if(d.ok){m.textContent='\\u2713 Zero set!';m.className='msg ok';"
        "setTimeout(function(){goStep(2);},1000);}"
        "else{m.textContent='\\u2717 '+d.error;m.className='msg err';}})"
        "['catch'](function(){m.textContent='\\u2717 Request failed';m.className='msg err';});}"
        "function doBeep(){"
        "var b=document.querySelector('[onclick=\"doBeep()\"]');"
        "if(b){b.disabled=true;b.textContent='\\u266a Beeping\\u2026';}"
        "post('/wizard/beep_test')['catch'](function(){})"
        ".finally(function(){"
        "setTimeout(function(){if(b){b.disabled=false;"
        "b.textContent='\\u25b6 Test Beep';}},900);});}"
        "function doSaveAudio(){"
        "var m=document.getElementById('aMsg');m.className='msg';"
        "var v=document.getElementById('volSlider').value;"
        "var mu=document.getElementById('muteChk').checked?'1':'0';"
        "post('/wizard/audio_save','&volume='+v+'&muted='+mu)"
        ".then(function(r){return r.json();})"
        ".then(function(d){"
        "if(d.ok){m.textContent='\\u2713 Saved!';m.className='msg ok';"
        "setTimeout(function(){goStep(4);},800);}"
        "else{m.textContent='\\u2717 '+d.error;m.className='msg err';}})"
        "['catch'](function(){m.textContent='\\u2717 Request failed';m.className='msg err';});}"
        "startPoll();"
        "</script>"
    );

    send_chunk(req, "</body></html>");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* =========================================================
 * HTTP: /wizard/zero (POST) — store current orientation as zero
 * ========================================================= */
static esp_err_t http_wizard_zero_post(httpd_req_t *req)
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
    int rcvd = 0;
    while (rcvd < total) {
        int r = httpd_req_recv(req, body + rcvd, total - rcvd);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        rcvd += r;
    }
    body[rcvd] = '\0';
    if (!csrf_ok(body)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token invalid");
        return ESP_OK;
    }
    free(body);

    lvgl_port_lock(-1);
    ui_zero_current();
    lvgl_port_unlock();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* =========================================================
 * HTTP: /wizard/orient (POST) — save screen orientation
 * ========================================================= */
// POST /wizard/orient  (form: csrf_token + orient=0..3)
static esp_err_t http_wizard_orient_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 256) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad len"); return ESP_OK; }
    char *body = (char *)calloc(1, (size_t)total + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem"); return ESP_OK; }
    int rcvd = 0;
    while (rcvd < total) {
        int r = httpd_req_recv(req, body + rcvd, total - rcvd);
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); return ESP_OK; }
        rcvd += r;
    }
    body[rcvd] = '\0';
    if (!csrf_ok(body)) { free(body); httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF"); return ESP_OK; }
    char ov[8] = {0};
    (void)form_get_value(body, "orient", ov, sizeof(ov));
    free(body);
    int o = atoi(ov);
    if (o < 0 || o > 3) o = 0;
    wifi_mgr_set_orient((unsigned char)o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* =========================================================
 * HTTP: /wizard/beep_test (POST) — fire a one-shot test beep
 * ========================================================= */
static esp_err_t http_wizard_beep_test_post(httpd_req_t *req)
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
    int rcvd = 0;
    while (rcvd < total) {
        int r = httpd_req_recv(req, body + rcvd, total - rcvd);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        rcvd += r;
    }
    body[rcvd] = '\0';
    if (!csrf_ok(body)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token invalid");
        return ESP_OK;
    }
    free(body);

    audio_mgr_request_beep();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* =========================================================
 * HTTP: /wizard/audio_save (POST) — persist mute + volume
 * ========================================================= */
static esp_err_t http_wizard_audio_save_post(httpd_req_t *req)
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
    int rcvd = 0;
    while (rcvd < total) {
        int r = httpd_req_recv(req, body + rcvd, total - rcvd);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        rcvd += r;
    }
    body[rcvd] = '\0';
    if (!csrf_ok(body)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "CSRF token invalid");
        return ESP_OK;
    }

    char vol_s[8]   = {0};
    char muted_s[4] = {0};
    (void)form_get_value(body, "volume", vol_s,   sizeof(vol_s));
    (void)form_get_value(body, "muted",  muted_s, sizeof(muted_s));
    free(body);

    int vol = atoi(vol_s);
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    bool muted_new = (muted_s[0] == '1');

    audio_mgr_set_volume(vol);
    audio_mgr_set_muted(muted_new);
    ui_save_audio_prefs();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* =========================================================
 * HTTP server start / ensure
 * ========================================================= */
// HTTP server: setup UI, status UI, and endpoints.
static httpd_handle_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 28;  /* 26 handlers + 2 headroom */
    cfg.stack_size = 8192;  // extra headroom for OTA flash writes
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    // Generate a fresh CSRF token for this server lifetime.
    uint8_t rand_bytes[8];
    esp_fill_random(rand_bytes, sizeof(rand_bytes));
    snprintf(s_csrf_token, sizeof(s_csrf_token),
             "%02x%02x%02x%02x%02x%02x%02x%02x",
             rand_bytes[0], rand_bytes[1], rand_bytes[2], rand_bytes[3],
             rand_bytes[4], rand_bytes[5], rand_bytes[6], rand_bytes[7]);

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

    httpd_uri_t leveling_mode = {
        .uri = "/leveling_mode",
        .method = HTTP_POST,
        .handler = http_leveling_mode_post,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &leveling_mode);

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

    // Device log viewer
    httpd_uri_t logs = {
        .uri = "/logs",
        .method = HTTP_GET,
        .handler = http_logs_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &logs);

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


    // Calibration wizard
    httpd_uri_t wizard = {
        .uri = "/wizard",
        .method = HTTP_GET,
        .handler = http_wizard_get,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wizard);

    httpd_uri_t wizard_zero = {
        .uri = "/wizard/zero",
        .method = HTTP_POST,
        .handler = http_wizard_zero_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wizard_zero);

    httpd_uri_t wizard_beep = {
        .uri = "/wizard/beep_test",
        .method = HTTP_POST,
        .handler = http_wizard_beep_test_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wizard_beep);

    httpd_uri_t wizard_audio = {
        .uri = "/wizard/audio_save",
        .method = HTTP_POST,
        .handler = http_wizard_audio_save_post,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wizard_audio);

    httpd_uri_t wizard_orient = {
        .uri = "/wizard/orient",
        .method = HTTP_POST,
        .handler = http_wizard_orient_post,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &wizard_orient);

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
    // Install log capture hook as early as possible.
    if (!s_orig_vprintf) {
        s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
    }

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

    network_defaults();
    nvs_load_network();

    config_defaults();
    (void)nvs_load_config(s_wheelbase_in, sizeof(s_wheelbase_in),
                          s_trackwidth_in, sizeof(s_trackwidth_in));
    (void)nvs_load_screen_timeout(&s_screen_timeout_s);
    s_wheelbase_val = parse_or_default(s_wheelbase_in, 133.0f);
    s_trackwidth_val = parse_or_default(s_trackwidth_in, 65.2f);
    nvs_load_leveler();
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

unsigned char wifi_mgr_get_orient(void) { return s_lvl_orient; }
unsigned char wifi_mgr_get_mode(void)   { return s_lvl_mode; }

void wifi_mgr_set_mode(unsigned char mode)
{
    if (mode > 1) {
        ESP_LOGW(TAG, "set_mode: invalid mode %u, clamping to BLOCKS", mode);
        mode = 0;
    }
    s_lvl_mode = mode;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_LEVELER, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_mode: nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, NVS_KEY_LVL_MODE, mode);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_mode: nvs_set_u8 failed: %s", esp_err_to_name(err));
    err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_mode: nvs_commit failed: %s", esp_err_to_name(err));
    nvs_close(h);
}

void wifi_mgr_set_orient(unsigned char orient)
{
    if (orient > 3) {
        ESP_LOGW(TAG, "set_orient: invalid orient %u, clamping to TOP", orient);
        orient = 0;
    }
    s_lvl_orient = orient;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_LEVELER, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_orient: nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, NVS_KEY_LVL_ORIENT, orient);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_orient: nvs_set_u8 failed: %s", esp_err_to_name(err));
    err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_orient: nvs_commit failed: %s", esp_err_to_name(err));
    nvs_close(h);
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

// Push latest leveling guidance for /status.
void wifi_mgr_update_guidance(const leveling_result_t *g)
{
    if (!g) return;
    portENTER_CRITICAL(&s_angle_mux);
    s_guide = *g;
    portEXIT_CRITICAL(&s_angle_mux);
}
